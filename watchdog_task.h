#pragma once

#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"

// Supervisor for the RP2350 hardware watchdog (PIR-MQTT-VMS-Pico.md §5, decision D14).
//
// Feeds the hardware watchdog once a second for as long as
//   - Wi-Fi has not been down for 4 minutes in a row, and
//   - every monitored task has checked in within its limit.
// Otherwise it stores the reason in a watchdog scratch register and resets the chip; the
// reason is printed after the reboot. If the supervisor itself stops running, the hardware
// watchdog resets the chip on its own.
//
// Build with -DWATCHDOG_ENABLED=0 to keep the checks and their messages but never reset
// (e.g. while debugging).

typedef enum {
    WATCHDOG_CLIENT_WIFI,
    WATCHDOG_CLIENT_PIR,
    WATCHDOG_CLIENT_LAN_MQTT,
    WATCHDOG_CLIENT_COUNT
} watchdog_client_t;

// Call from the monitored task's main loop. A task is monitored from its first check-in on,
// so a long start-up phase (the PIR warm-up) needs no special case.
void watchdog_checkin(watchdog_client_t client);

// Creates the supervisor task. Call once from main(), before vTaskStartScheduler().
bool watchdog_task_start(UBaseType_t priority);
