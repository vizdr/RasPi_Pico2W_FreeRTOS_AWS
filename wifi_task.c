#include "wifi_task.h"
#include "wifi_credentials.h"
#include "led.h"
#include "watchdog_task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"  // for netif_ip4_addr()

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#include <string.h>

#define WIFI_CONNECT_TIMEOUT_MS     30000
#define WIFI_RETRY_DELAY_MS         5000
#define WIFI_LINK_CHECK_INTERVAL_MS 2000
#define WIFI_SCAN_TIMEOUT_MS        10000

// Regulatory domain for the radio. cyw43_arch_init() alone means "worldwide", which leaves
// out 2.4 GHz channels 12 and 13 - legal in Germany, and a FRITZ!Box on automatic channel
// selection may move there, after which the Pico can't join at all. Override in
// wifi_credentials.h (e.g. CYW43_COUNTRY_UK, CYW43_COUNTRY_USA; see cyw43_country.h).
#ifndef WIFI_COUNTRY
#define WIFI_COUNTRY                CYW43_COUNTRY_GERMANY
#endif

#define WIFI_CONNECTED_BIT (1 << 0)

static EventGroupHandle_t s_wifi_event_group;

void wifi_task_init(void) {
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        panic("failed to create wifi event group");
    }
}

/* ---- radio diagnosis ------------------------------------------------------ */

// cyw43_arch_wifi_connect_timeout_ms() reports "joining" until it times out both when the
// access point can't be found and when it doesn't answer: on CYW43_LINK_NONET it quietly
// re-issues the join. So scan and print what the radio actually hears: once at boot as a
// baseline, and after every failed join. Another device next to the Pico that hears the
// same networks much more strongly points at the Pico's antenna or power, not the router.

#define WIFI_SCAN_MAX_NETS  8

typedef struct {
    int      results;                      // results heard (one per access point and channel)
    int      nets;                         // distinct SSIDs in `net`
    struct {
        char     ssid[33];
        int16_t  rssi;                     // strongest result for this SSID
        uint16_t channel;
    } net[WIFI_SCAN_MAX_NETS];
} wifi_scan_summary_t;

// Static, not on the stack: a scan that outlives WIFI_SCAN_TIMEOUT_MS still calls back.
static wifi_scan_summary_t s_scan;

static int wifi_scan_result(void *env, const cyw43_ev_scan_result_t *r) {
    wifi_scan_summary_t *sum = env;
    if (r == NULL) {
        return 0;
    }
    sum->results++;

    char ssid[33];
    size_t len = r->ssid_len < 32 ? r->ssid_len : 32;
    memcpy(ssid, r->ssid, len);
    ssid[len] = '\0';

    for (int i = 0; i < sum->nets; i++) {
        if (strcmp(sum->net[i].ssid, ssid) == 0) {
            if (r->rssi > sum->net[i].rssi) {
                sum->net[i].rssi    = r->rssi;
                sum->net[i].channel = r->channel;
            }
            return 0;
        }
    }
    if (sum->nets < WIFI_SCAN_MAX_NETS) {
        strcpy(sum->net[sum->nets].ssid, ssid);
        sum->net[sum->nets].rssi    = r->rssi;
        sum->net[sum->nets].channel = r->channel;
        sum->nets++;
    }
    return 0;
}

static void wifi_scan_report(const char *when) {
    memset(&s_scan, 0, sizeof s_scan);
    cyw43_wifi_scan_options_t opts = {0};
    if (cyw43_wifi_scan(&cyw43_state, &opts, &s_scan, wifi_scan_result) != 0) {
        printf("wifi_task: scan (%s): could not start\n", when);
        return;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WIFI_SCAN_TIMEOUT_MS);
    while (cyw43_wifi_scan_active(&cyw43_state) && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    bool complete = !cyw43_wifi_scan_active(&cyw43_state);

    printf("wifi_task: scan (%s)%s: %d result(s), %d network(s)\n", when,
           complete ? "" : ", cut off after 10 s", s_scan.results, s_scan.nets);
    bool found = false;
    for (int i = 0; i < s_scan.nets; i++) {
        bool ours = strcmp(s_scan.net[i].ssid, WIFI_SSID) == 0;
        found |= ours;
        printf("wifi_task:   %c %-32s RSSI %4d dBm  channel %u\n", ours ? '*' : ' ',
               s_scan.net[i].ssid, s_scan.net[i].rssi, s_scan.net[i].channel);
    }
    if (!found) {
        printf("wifi_task: scan: '%s' not heard%s\n", WIFI_SSID,
               s_scan.results == 0 ? " - nothing heard at all: check the Pico's power and "
                                     "antenna (keep wires, modules and metal away from the "
                                     "antenna end of the board)" : "");
    }
}

static void wifi_diagnose_join_failure(int err) {
    const char *what = err == PICO_ERROR_TIMEOUT        ? "timed out: access point not found, or not answering"
                     : err == PICO_ERROR_BADAUTH        ? "refused: wrong password (WIFI_PASSWORD)"
                     : err == PICO_ERROR_CONNECT_FAILED ? "failed: association refused or not answered"
                     :                                    "failed";
    printf("wifi_task: join %s\n", what);

    // A scan can't run beside the pending join; give the radio a moment after leaving.
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_scan_report("after failed join");
}

/* ---- API and task --------------------------------------------------------- */

bool wifi_is_connected(void) {
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

void wifi_wait_connected(void) {
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                         pdFALSE /* don't clear on exit */, pdTRUE, portMAX_DELAY);
}

void wifi_task(__unused void *params) {
    // Must happen here, inside a running task, rather than in main() before the scheduler
    // starts: cyw43_arch_init() (the full lwip_sys_freertos variant) spins up its own
    // FreeRTOS worker task that takes an internal lock during setup, and
    // async_context_freertos_lock_check() compares that lock's holder against
    // xTaskGetCurrentTaskHandle() - which is meaningless/mismatched before any task has
    // been created (pxCurrentTCB isn't set yet), causing a hang (Release) or a failed
    // assert (Debug: "xSemaphoreGetMutexHolder(...) == xTaskGetCurrentTaskHandle()").
    // led_init() would be cyw43_arch_init() here (the LED hangs off the CYW43), which can't
    // set the country; this brings up the radio and the LED together.
    if (cyw43_arch_init_with_country(WIFI_COUNTRY) != PICO_OK) {
        panic("cyw43 init failed");
    }
    printf("wifi_task: cyw43_arch_init() returned OK, country %c%c\n",
           (int)(WIFI_COUNTRY & 0xff), (int)((WIFI_COUNTRY >> 8) & 0xff));

    cyw43_arch_enable_sta_mode();  // Start the CYW43 driver in STA mode (Station mode (client), no AP, no P2P).

    // Disable power-saving: the default (CYW43_DEFAULT_PM) has been observed to cause
    // rapid connect/disconnect flapping with some routers shortly after association.
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);

    wifi_scan_report("at boot");

    for (;;) {
        watchdog_checkin(WATCHDOG_CLIENT_WIFI);
        printf("wifi_task: connecting to SSID '%s'...\n", WIFI_SSID);
        int err = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                                       CYW43_AUTH_WPA2_AES_PSK,
                                                       WIFI_CONNECT_TIMEOUT_MS);
        if (err) {
            led_set(false);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            wifi_diagnose_join_failure(err);
            printf("wifi_task: connect failed (err=%d), retrying in %d ms\n",
                   err, WIFI_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
            continue;
        }

        int32_t rssi = 0;
        cyw43_wifi_get_rssi(&cyw43_state, &rssi);
        printf("wifi_task: connected, IP = %s, RSSI %ld dBm\n",
               ip4addr_ntoa(netif_ip4_addr(netif_default)), (long)rssi);
        led_set(true);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Stay here monitoring the link for as long as it holds up. Deliberately
        // cyw43_tcpip_link_status(), not cyw43_wifi_link_status(): the latter is a raw
        // radio-association status whose own documented value set never includes
        // CYW43_LINK_UP (that's specifically "has an IP", a TCP/IP-level concept) - using
        // it here made this loop exit immediately on every single iteration. This matches
        // what cyw43_arch_wifi_connect_timeout_ms() itself polls internally.
        while (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP) {
            watchdog_checkin(WATCHDOG_CLIENT_WIFI);
            vTaskDelay(pdMS_TO_TICKS(WIFI_LINK_CHECK_INTERVAL_MS));
        }

        printf("wifi_task: link down, reconnecting...\n");
        led_set(false);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
