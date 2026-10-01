/**
 * @file pir_task.c
 * @brief FreeRTOS service for the Makeblock Me PIR Motion Sensor on Pico 2 W.
 *
 * Owns the sensor exclusively (driver: pir.c/h). The driver's edge callback
 * runs in the ISR and only pushes the sampled level and timestamp to a queue;
 * this task does the rest:
 *
 *   level goes high -> motion start
 *   level goes low  -> motion stop
 *
 * That is the usual FreeRTOS deferred-interrupt pattern: nothing that can
 * block or take long runs in interrupt context.
 */

#include "pir_task.h"
#include "pir.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "pico/time.h"

#include <stdio.h>

/* ---- configuration ------------------------------------------------------ */

#ifndef PIR_GPIO
#define PIR_GPIO                 14u     /* physical pin 19; RJ25 S2; not an ADC pin */
#endif

#ifndef PIR_MODE_GPIO
#define PIR_MODE_GPIO            13      /* physical pin 17; RJ25 S1; PIR_NO_PIN = not wired */
#endif

/* Safety net: if an edge is ever lost (queue full, glitch shorter than the
 * ISR latency), the task re-reads the pin this often and resyncs. */
#define PIR_RESYNC_MS            1000u

#define PIR_QUEUE_LEN            8u

/* ---- shared state ------------------------------------------------------- */

typedef struct {
    uint32_t time_ms;
    bool     level;
} pir_event_t;

/* mode_pin preset so pir_task_set_retrigger() is a no-op before start. */
static pir_t             s_dev = { .mode_pin = PIR_NO_PIN };
static QueueHandle_t     s_queue;
static pir_status_t      s_status;
static volatile bool     s_ready;
static volatile uint32_t s_queue_overflows;

__attribute__((weak))
void pir_on_motion_start(void)
{
}

__attribute__((weak))
void pir_on_motion_stop(uint32_t duration_ms)
{
    (void)duration_ms;
}

bool pir_get_status(pir_status_t *out)
{
    if (!s_ready) {
        return false;
    }
    taskENTER_CRITICAL();
    *out = s_status;
    taskEXIT_CRITICAL();
    return true;
}

void pir_task_set_retrigger(bool retriggerable)
{
    pir_set_retrigger(&s_dev, retriggerable);
}

/* ---- ISR side ----------------------------------------------------------- */

/* Driver edge callback, interrupt context. IO_IRQ_BANK0 runs at
 * PICO_DEFAULT_IRQ_PRIORITY (0x80), numerically above
 * configMAX_SYSCALL_INTERRUPT_PRIORITY (16), so the FromISR API is allowed. */
static void pir_on_edge_isr(bool level, uint32_t time_ms)
{
    pir_event_t ev = {
        .time_ms = time_ms,
        .level   = level,
    };

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &ev, &woken) != pdTRUE) {
        s_queue_overflows++;
    }
    portYIELD_FROM_ISR(woken);
}

/* ---- task --------------------------------------------------------------- */

static void pir_apply(bool level, uint32_t time_ms)
{
    if (level == s_status.motion) {
        return;     /* sub-latency glitch, or already resynced by polling */
    }

    uint32_t duration_ms = time_ms - s_status.last_change_ms;

    taskENTER_CRITICAL();
    s_status.motion         = level;
    s_status.last_change_ms = time_ms;
    if (level) {
        s_status.motion_count++;
    }
    taskEXIT_CRITICAL();

    if (level) {
        printf("[pir] motion start (#%lu)\n", (unsigned long)s_status.motion_count);
        pir_on_motion_start();
    } else {
        printf("[pir] motion stop after %lu ms\n", (unsigned long)duration_ms);
        pir_on_motion_stop(duration_ms);
    }
}

static void pir_task(void *arg)
{
    (void)arg;

    printf("[pir] warming up for %u s\n", (unsigned)(PIR_WARMUP_MS / 1000u));
    vTaskDelay(pdMS_TO_TICKS(PIR_WARMUP_MS));

    /* Enabling drops the edges latched during warm-up; the starting level is
     * then taken directly. */
    pir_irq_enable(&s_dev, pir_on_edge_isr);

    s_status.last_change_ms = to_ms_since_boot(get_absolute_time());
    s_ready = true;
    printf("[pir] ready\n");
    pir_apply(pir_read(&s_dev), s_status.last_change_ms);

    uint32_t reported_overflows = 0;

    for (;;) {
        pir_event_t ev;
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(PIR_RESYNC_MS)) == pdTRUE) {
            pir_apply(ev.level, ev.time_ms);
        } else {
            pir_apply(pir_read(&s_dev), to_ms_since_boot(get_absolute_time()));
        }

        if (s_queue_overflows != reported_overflows) {
            reported_overflows = s_queue_overflows;
            printf("[pir] event queue overflowed %lu times - noisy input?\n",
                   (unsigned long)reported_overflows);
        }
    }
}

bool pir_task_start(UBaseType_t priority)
{
    s_queue = xQueueCreate(PIR_QUEUE_LEN, sizeof(pir_event_t));
    if (s_queue == NULL) {
        return false;
    }

    /* Retriggerable: the output stays high while motion continues, so one
     * start/stop pair is one period of activity. */
    if (!pir_init(&s_dev, PIR_GPIO, PIR_MODE_GPIO, true)) {
        return false;
    }

    return xTaskCreate(pir_task, "pir", 512, NULL, priority, NULL) == pdPASS;
}
