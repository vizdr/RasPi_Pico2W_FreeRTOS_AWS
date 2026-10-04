#include "time_task.h"
#include "wifi_task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/sntp.h"

#include <sys/time.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#define TIME_HEARTBEAT_INTERVAL_MS 60000

#define TIME_SYNCED_BIT (1 << 0)

static EventGroupHandle_t s_time_event_group;

void time_task_init(void) {
    s_time_event_group = xEventGroupCreate();
    if (s_time_event_group == NULL) {
        panic("failed to create time event group");
    }
}

bool time_is_synced(void) {
    return (xEventGroupGetBits(s_time_event_group) & TIME_SYNCED_BIT) != 0;
}

void time_wait_synced(void) {
    xEventGroupWaitBits(s_time_event_group, TIME_SYNCED_BIT,
                         pdFALSE /* don't clear on exit */, pdTRUE, portMAX_DELAY);
}

// Invoked from lwIP's SNTP client (via the SNTP_SET_SYSTEM_TIME macro in lwipopts.h)
// whenever it successfully syncs - both the first time and on every periodic resync
// afterward (SNTP_OPMODE_POLL keeps resyncing on its own; we don't need to drive that).
void time_task_sntp_set_system_time(uint32_t sec) {
    struct timeval tv = {.tv_sec = (time_t)sec, .tv_usec = 0};
    settimeofday(&tv, NULL);

    time_t now = tv.tv_sec;
    printf("time_task: synced via SNTP, UTC now = %s", ctime(&now)); // ctime() ends in '\n'

    xEventGroupSetBits(s_time_event_group, TIME_SYNCED_BIT);
}

void time_task(__unused void *params) {
    wifi_wait_connected();

    // sntp_init() and friends are raw lwIP API calls (not going through a socket), so they
    // need the CYW43/lwIP lock held - see the LWIP_ASSERT_CORE_LOCKED() calls in sntp.c.
    cyw43_arch_lwip_begin();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    cyw43_arch_lwip_end();

    printf("time_task: SNTP started, waiting for first sync...\n");
    time_wait_synced();

    // Nothing left to drive manually - lwIP's own SNTP timer keeps resyncing periodically.
    // Stick around just to print a heartbeat, useful for confirming the clock stays sane.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TIME_HEARTBEAT_INTERVAL_MS));
        time_t now = time(NULL);
        printf("time_task: UTC now = %s", ctime(&now));
    }
}
