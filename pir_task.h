#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

// FreeRTOS service for the Makeblock Me PIR Motion Sensor v1.1 - the hardware
// driver is pir.c/h. See README-PIR.md.

typedef struct {
    bool     motion;            // output currently high
    uint32_t motion_count;      // number of motion-start events since boot
    uint64_t last_change_ms;    // ms since boot of the last start/stop
} pir_status_t;

// Initialises the driver (pins, edge ISR, retriggerable mode), then creates the task.
// Call once from main(), before vTaskStartScheduler().
bool pir_task_start(UBaseType_t priority);

// Returns false while the sensor is still in its power-up warm-up period.
bool pir_get_status(pir_status_t *out);

// Trigger mode via RJ25 S1 (true = retriggerable). No-op if S1 is not wired
// (PIR_MODE_GPIO == PIR_NO_PIN) or before pir_task_start().
void pir_task_set_retrigger(bool retriggerable);

// Weak hooks, called from the pir task (not the ISR) - override to act on motion.
// Keep them short and non-blocking: post to a queue and return.
// time_ms is when the edge happened (ms since boot, sampled in the ISR; 64-bit, never
// wraps); duration_ms is the length of the motion that just ended. When a hook runs,
// pir_get_status() already reflects the new state, and its motion_count is shared by a
// start and its stop.
void pir_on_motion_start(uint64_t time_ms);
void pir_on_motion_stop(uint64_t time_ms, uint32_t duration_ms);
