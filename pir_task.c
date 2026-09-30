/**
 * @file pir_task.c
 * @brief Makeblock Me PIR Motion Sensor v1.1 on Pico 2 W, interrupt driven.
 *
 * The sensor's digital output is high while motion is being detected (for at
 * least the hold time set by its potentiometer). Both edges raise a GPIO IRQ:
 *
 *   rising edge  -> motion start
 *   falling edge -> motion stop
 *
 * The ISR does the minimum - acknowledge, sample the pin level, timestamp,
 * push to a queue - and the pir task does the rest (state, logging, hooks).
 * That is the usual FreeRTOS deferred-interrupt pattern: nothing that can
 * block or take long runs in interrupt context.
 */

#include "pir_task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/time.h"

#include <stdio.h>

/* ---- configuration ------------------------------------------------------ */

#ifndef PIR_GPIO
#define PIR_GPIO                 14u     /* physical pin 19; not an ADC pin */
#endif

/* PIR modules report false triggers while the pyroelectric element settles
 * after power-up. Edges are not enabled until this has elapsed. */
#ifndef PIR_WARMUP_MS
#define PIR_WARMUP_MS            30000u
#endif

/* Safety net: if an edge is ever lost (queue full, glitch shorter than the
 * ISR latency), the task re-reads the pin this often and resyncs. */
#define PIR_RESYNC_MS            1000u

#define PIR_QUEUE_LEN            8u
#define PIR_EDGES                (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL)

/* ---- shared state ------------------------------------------------------- */

typedef struct {
    uint32_t time_ms;
    bool     level;
} pir_event_t;

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

/* ---- ISR ---------------------------------------------------------------- */

/* Raw handler on the shared IO_IRQ_BANK0 vector: the CYW43 driver has its own
 * raw handler there for WL_HOST_WAKE, so this must only look at, and only
 * acknowledge, its own pin. IO_IRQ_BANK0 runs at PICO_DEFAULT_IRQ_PRIORITY
 * (0x80), numerically above configMAX_SYSCALL_INTERRUPT_PRIORITY (16), so the
 * FromISR API is allowed here. */
static void pir_gpio_isr(void)
{
    uint32_t events = gpio_get_irq_event_mask(PIR_GPIO) & PIR_EDGES;
    if (events == 0) {
        return;
    }
    gpio_acknowledge_irq(PIR_GPIO, events);

    /* The current level, not the edge type, is what the task acts on: if a
     * rise and a fall were both latched before we got here, the level says
     * which one came last. */
    pir_event_t ev = {
        .time_ms = to_ms_since_boot(get_absolute_time()),
        .level   = gpio_get(PIR_GPIO),
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

    /* Edges are latched in INTR even while disabled - drop the ones from the
     * warm-up period before enabling, then take the starting level directly. */
    gpio_acknowledge_irq(PIR_GPIO, PIR_EDGES);
    gpio_set_irq_enabled(PIR_GPIO, PIR_EDGES, true);

    s_status.last_change_ms = to_ms_since_boot(get_absolute_time());
    s_ready = true;
    printf("[pir] ready\n");
    pir_apply(gpio_get(PIR_GPIO), s_status.last_change_ms);

    uint32_t reported_overflows = 0;

    for (;;) {
        pir_event_t ev;
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(PIR_RESYNC_MS)) == pdTRUE) {
            pir_apply(ev.level, ev.time_ms);
        } else {
            pir_apply(gpio_get(PIR_GPIO), to_ms_since_boot(get_absolute_time()));
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

    /* The sensor drives the line actively both ways, so no internal pulls are
     * needed (and the RP2350 pull-down is unreliable anyway, erratum E9).
     * Its ~3.8 V high level is fine: GPIO14 is a fault-tolerant pin, VIH up
     * to 5.5 V while IOVDD is powered - see README-PIR.md. */
    gpio_init(PIR_GPIO);
    gpio_set_dir(PIR_GPIO, GPIO_IN);
    gpio_disable_pulls(PIR_GPIO);

    /* Registered now (core 0, before the scheduler), but the pin's edge
     * events stay disabled until the task finishes warm-up. */
    gpio_add_raw_irq_handler(PIR_GPIO, pir_gpio_isr);
    irq_set_enabled(IO_IRQ_BANK0, true);

    return xTaskCreate(pir_task, "pir", 512, NULL, priority, NULL) == pdPASS;
}
