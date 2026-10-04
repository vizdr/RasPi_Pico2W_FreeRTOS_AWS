/**
 * @file watchdog_task.c
 * @brief Watchdog supervisor (PIR-MQTT-VMS-Pico.md §5, decision D14).
 *
 * The RP2350 watchdog counts at most ~16.7 s, too short to express "Wi-Fi down for
 * 4 minutes" itself. So this task makes the decisions and the hardware is only the backstop:
 *
 *   - Wi-Fi down for WIFI_DOWN_MS in a row -> reset. Found on the Pi side: the Pico sat
 *     35 minutes without network and never rebooted (§4.5, finding 2). A reset re-runs
 *     cyw43_arch_init(), which power-cycles the radio through WL_REG_ON.
 *   - a monitored task overdue past its limit -> reset (a hung task; VMS FoundAndFixed #51
 *     is the Pi-side reminder that "still running" is not "still working").
 *   - this task itself not running -> the hardware watchdog expires after HW_TIMEOUT_MS.
 *     That also covers a HardFault, panic() or failed configASSERT: without a debugger they
 *     used to stop the Pico for good; now it reboots after HW_TIMEOUT_MS. Under the
 *     debugger the watchdog pauses while the core is halted, so they can still be inspected.
 *
 * Not monitored: aws_iot_task, which legitimately blocks for as long as there is no
 * internet (it waits for SNTP), so no fixed limit fits it.
 */

#include "watchdog_task.h"
#include "wifi_task.h"

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include <stdio.h>

/* ---- configuration ------------------------------------------------------ */

#ifndef WATCHDOG_ENABLED
#define WATCHDOG_ENABLED     1
#endif

#define HW_TIMEOUT_MS        10000u                /* RP2350 maximum is ~16.7 s */
#define FEED_INTERVAL_MS     1000u
#define WIFI_DOWN_MS         (4u * 60u * 1000u)    /* D14: N = 4 min */

/* Scratch 0-3 are free - the SDK's watchdog_reboot() uses 4-7 - and keep their value
 * through a watchdog reset, though not through a power-on. */
#define SCRATCH_REASON       0
#define SCRATCH_DETAIL       1
#define REASON_MAGIC         0x57440000u           /* "WD" in the top half */
#define REASON_MAGIC_MASK    0xffff0000u

typedef enum {
    REASON_WIFI_DOWN = 1,    /* detail: seconds without Wi-Fi */
    REASON_TASK_HUNG = 2,    /* detail: watchdog_client_t */
} reset_reason_t;

typedef struct {
    const char *name;        /* the FreeRTOS task name */
    uint32_t    limit_ms;    /* longest legitimate gap between two check-ins */
} client_info_t;

static const client_info_t s_clients[WATCHDOG_CLIENT_COUNT] = {
    /* A join attempt blocks up to 30 s (WIFI_CONNECT_TIMEOUT_MS), then 5 s retry delay. */
    [WATCHDOG_CLIENT_WIFI]     = { "WiFi",    90000u },
    /* Wakes at least every PIR_RESYNC_MS (1 s) once its 30 s warm-up is over. */
    [WATCHDOG_CLIENT_PIR]      = { "pir",     30000u },
    /* Wakes at least every 1 s; a connect attempt waits up to 10 s, a retained publish
     * retries for up to 1 s. */
    [WATCHDOG_CLIENT_LAN_MQTT] = { "LanMqtt", 60000u },
};

static volatile TickType_t s_last_checkin[WATCHDOG_CLIENT_COUNT];
static volatile bool       s_monitored[WATCHDOG_CLIENT_COUNT];

void watchdog_checkin(watchdog_client_t client)
{
    if (client < WATCHDOG_CLIENT_COUNT) {
        s_last_checkin[client] = xTaskGetTickCount();
        s_monitored[client]    = true;
    }
}

/* ---- reset and its report ------------------------------------------------ */

static void report_previous_reset(void)
{
    uint32_t reason = watchdog_hw->scratch[SCRATCH_REASON];
    uint32_t detail = watchdog_hw->scratch[SCRATCH_DETAIL];
    watchdog_hw->scratch[SCRATCH_REASON] = 0;

    if (!watchdog_caused_reboot()) {
        return;   /* power-on, or a reset by the debugger */
    }
    if ((reason & REASON_MAGIC_MASK) == REASON_MAGIC) {
        switch ((reset_reason_t)(reason & ~REASON_MAGIC_MASK)) {
        case REASON_WIFI_DOWN:
            printf("[watchdog] previous reset: Wi-Fi down for %lu s\n", (unsigned long)detail);
            return;
        case REASON_TASK_HUNG:
            printf("[watchdog] previous reset: task '%s' stopped checking in\n",
                   detail < WATCHDOG_CLIENT_COUNT ? s_clients[detail].name : "?");
            return;
        }
    }
    if (watchdog_enable_caused_reboot()) {
        /* Also the path out of a HardFault, panic() or failed configASSERT without a
         * debugger: they halt the CPU, and the hardware watchdog expires. */
        printf("[watchdog] previous reset: hardware watchdog expired - the supervisor "
               "stopped running (a crash, panic or assert, or a task starving it)\n");
    }
}

/* Never returns when resets are enabled. */
static void supervisor_reset(reset_reason_t reason, uint32_t detail)
{
#if WATCHDOG_ENABLED
    printf("[watchdog] resetting\n");
    watchdog_hw->scratch[SCRATCH_DETAIL] = detail;
    watchdog_hw->scratch[SCRATCH_REASON] = REASON_MAGIC | (uint32_t)reason;
    /* 100 ms lets the UART drain the messages. No more feeding from here on:
     * watchdog_update() would reload the 100 ms and keep postponing the reset. */
    watchdog_reboot(0, 0, 100);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
#else
    (void)reason;
    (void)detail;
    printf("[watchdog] WATCHDOG_ENABLED=0: not resetting\n");
#endif
}

/* ---- task ----------------------------------------------------------------- */

static void watchdog_task(__unused void *params)
{
    report_previous_reset();

#if WATCHDOG_ENABLED
    /* pause_on_debug: a core halted at a breakpoint doesn't get reset under the debugger. */
    watchdog_enable(HW_TIMEOUT_MS, true);
#endif

    /* Boot counts as "down" until the first join, so a Pico that never gets onto Wi-Fi
     * resets too. */
    TickType_t wifi_last_up = xTaskGetTickCount();

    for (;;) {
        TickType_t now = xTaskGetTickCount();

        if (wifi_is_connected()) {
            wifi_last_up = now;
        } else if ((int32_t)(now - wifi_last_up) >= (int32_t)pdMS_TO_TICKS(WIFI_DOWN_MS)) {
            uint32_t down_s = (uint32_t)((now - wifi_last_up) / configTICK_RATE_HZ);
            printf("[watchdog] Wi-Fi down for %lu s\n", (unsigned long)down_s);
            supervisor_reset(REASON_WIFI_DOWN, down_s);
            wifi_last_up = now;   /* reached only with resets disabled: report once per period */
        }

        for (uint32_t i = 0; i < WATCHDOG_CLIENT_COUNT; i++) {
            if (!s_monitored[i]) {
                continue;
            }
            /* Signed: a check-in that landed after `now` was read is not overdue. */
            int32_t silent = (int32_t)(now - s_last_checkin[i]);
            if (silent > (int32_t)pdMS_TO_TICKS(s_clients[i].limit_ms)) {
                printf("[watchdog] task '%s' has not checked in for %lu s\n",
                       s_clients[i].name, (unsigned long)((uint32_t)silent / configTICK_RATE_HZ));
                supervisor_reset(REASON_TASK_HUNG, i);
                s_last_checkin[i] = now;
            }
        }

#if WATCHDOG_ENABLED
        watchdog_update();
#endif
        vTaskDelay(pdMS_TO_TICKS(FEED_INTERVAL_MS));
    }
}

bool watchdog_task_start(UBaseType_t priority)
{
    return xTaskCreate(watchdog_task, "Watchdog", 512, NULL, priority, NULL) == pdPASS;
}
