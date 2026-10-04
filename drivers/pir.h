/**
 * @file pir.h
 * @brief Makeblock Me PIR Motion Sensor v1.1 (BISS0001) driver for RP2350.
 *
 * Hardware layer only: pin setup, trigger-mode control, level read and the
 * edge interrupt. No FreeRTOS dependency - what to do with an edge is up to
 * the caller's callback (see pir_task.c for the FreeRTOS service on top).
 *
 * Sensor pins, as the Makeblock library maps them (MePIRMotionSensor.cpp):
 *   RJ25 S2 -> BISS0001 Vo : output, high while motion is detected
 *   RJ25 S1 -> BISS0001 A  : trigger mode input, 1 = retriggerable
 */
#ifndef PIR_H
#define PIR_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Pass as mode_pin when S1 is not wired; the module's own default then applies. */
#define PIR_NO_PIN  (-1)

/** The pyroelectric element gives false triggers for a while after power-up. */
#ifndef PIR_WARMUP_MS
#define PIR_WARMUP_MS  30000u
#endif

/**
 * Edge callback. Runs in INTERRUPT context, on the core that called pir_init():
 * keep it short, and use only ISR-safe APIs (e.g. xQueueSendFromISR).
 *
 * @param level    pin level sampled in the ISR, after the edge (true = motion).
 *                 If a rise and a fall were both latched, this says which came last.
 * @param time_ms  ms since boot, sampled in the ISR: time_us_64() / 1000, 64-bit so it
 *                 never wraps (the SDK's to_ms_since_boot() is 32-bit and wraps after
 *                 49.7 days)
 */
typedef void (*pir_edge_cb_t)(bool level, uint64_t time_ms);

typedef struct {
    uint          out_pin;     /**< RJ25 S2, sensor output */
    int           mode_pin;    /**< RJ25 S1, trigger mode, or PIR_NO_PIN */
    pir_edge_cb_t on_edge;     /**< NULL while edges are disabled */
} pir_t;

/**
 * Configure both pins and register the edge interrupt handler (edges stay
 * disabled until pir_irq_enable()). Call once, before the scheduler starts or
 * from a single init task, on the core that should service the interrupt.
 *
 * Only one sensor is supported: SDK raw GPIO handlers take no context
 * argument, so the driver keeps the device in a file-level static.
 *
 * @param out_pin        GPIO wired to S2. Use a non-ADC pin: the output swings
 *                       to ~3.8 V, fine on RP2350 fault-tolerant pins only.
 * @param mode_pin       GPIO wired to S1, or PIR_NO_PIN
 * @param retriggerable  initial trigger mode (ignored without a mode pin)
 * @return               false if a sensor is already initialised
 */
bool pir_init(pir_t *dev, uint out_pin, int mode_pin, bool retriggerable);

/** Current output level: true = motion detected. */
bool pir_read(const pir_t *dev);

/**
 * Select the trigger mode: true = retriggerable (output stays high while motion
 * continues), false = single trigger. No-op without a mode pin. Safe to call
 * from any task: it is a single atomic GPIO write.
 */
void pir_set_retrigger(const pir_t *dev, bool retriggerable);

/** Drop edges latched so far, then deliver new ones to cb (both edges). */
void pir_irq_enable(pir_t *dev, pir_edge_cb_t cb);

/** Stop delivering edges. */
void pir_irq_disable(pir_t *dev);

#ifdef __cplusplus
}
#endif
#endif /* PIR_H */
