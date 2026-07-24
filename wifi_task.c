#include "wifi_task.h"
#include "wifi_credentials.h"
#include "led.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"  // for netif_ip4_addr()

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#define WIFI_CONNECT_TIMEOUT_MS     30000
#define WIFI_RETRY_DELAY_MS         5000
#define WIFI_LINK_CHECK_INTERVAL_MS 2000

#define WIFI_CONNECTED_BIT (1 << 0)

static EventGroupHandle_t s_wifi_event_group;

void wifi_task_init(void) {
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        panic("failed to create wifi event group");
    }
}

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
    if (led_init() != PICO_OK) {
        panic("led/cyw43 init failed");
    }
    printf("wifi_task: cyw43_arch_init() returned OK\n");

    cyw43_arch_enable_sta_mode();  // Start the CYW43 driver in STA mode (Station mode (client), no AP, no P2P).

    // Disable power-saving: the default (CYW43_DEFAULT_PM) has been observed to cause
    // rapid connect/disconnect flapping with some routers shortly after association.
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);

    for (;;) {
        printf("wifi_task: connecting to SSID '%s'...\n", WIFI_SSID);
        int err = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                                       CYW43_AUTH_WPA2_AES_PSK,
                                                       WIFI_CONNECT_TIMEOUT_MS);
        if (err) {
            printf("wifi_task: connect failed (err=%d), retrying in %d ms\n",
                   err, WIFI_RETRY_DELAY_MS);
            led_set(false);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
            continue;
        }

        printf("wifi_task: connected, IP = %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
        led_set(true);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Stay here monitoring the link for as long as it holds up. Deliberately
        // cyw43_tcpip_link_status(), not cyw43_wifi_link_status(): the latter is a raw
        // radio-association status whose own documented value set never includes
        // CYW43_LINK_UP (that's specifically "has an IP", a TCP/IP-level concept) - using
        // it here made this loop exit immediately on every single iteration. This matches
        // what cyw43_arch_wifi_connect_timeout_ms() itself polls internally.
        while (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP) {
            vTaskDelay(pdMS_TO_TICKS(WIFI_LINK_CHECK_INTERVAL_MS));
        }

        printf("wifi_task: link down, reconnecting...\n");
        led_set(false);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
