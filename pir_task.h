#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

// Makeblock Me PIR Motion Sensor v1.1 - see pir_task.c and README-PIR.md.

typedef struct {
    bool     motion;            // output currently high
    uint32_t motion_count;      // number of motion-start events since boot
    uint32_t last_change_ms;    // ms since boot of the last start/stop
} pir_status_t;

// Configures the GPIO and registers the edge ISR, then creates the task.
// Call once from main(), before vTaskStartScheduler().
bool pir_task_start(UBaseType_t priority);

// Returns false while the sensor is still in its power-up warm-up period.
bool pir_get_status(pir_status_t *out);

// Weak hooks, called from the pir task (not the ISR) - override to act on motion.
// Keep them short and non-blocking: post to a queue and return.
void pir_on_motion_start(void);
void pir_on_motion_stop(uint32_t duration_ms);
