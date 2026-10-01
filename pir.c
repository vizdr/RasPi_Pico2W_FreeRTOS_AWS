/**
 * @file pir.c
 * @brief Makeblock Me PIR Motion Sensor v1.1 (BISS0001) driver for RP2350.
 */

#include "pir.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/time.h"

#include <stddef.h>

#define PIR_EDGES  (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL)

/* The raw GPIO handler has no context argument - see pir_init(). */
static pir_t *s_dev;

/* ------------------------------------------------------------------------- */

/* Raw handler on the shared IO_IRQ_BANK0 vector: the CYW43 driver has its own
 * raw handler there for WL_HOST_WAKE, so this must only look at, and only
 * acknowledge, its own pin. It is also entered for the other handlers' IRQs,
 * hence the early return. */
static void pir_gpio_isr(void)
{
    pir_t *dev = s_dev;

    uint32_t events = gpio_get_irq_event_mask(dev->out_pin) & PIR_EDGES;
    if (events == 0) {
        return;
    }
    gpio_acknowledge_irq(dev->out_pin, events);

    pir_edge_cb_t cb = dev->on_edge;
    if (cb != NULL) {
        cb(gpio_get(dev->out_pin), to_ms_since_boot(get_absolute_time()));
    }
}

/* ------------------------------------------------------------------------- */

bool pir_init(pir_t *dev, uint out_pin, int mode_pin, bool retriggerable)
{
    if (s_dev != NULL) {
        return false;
    }

    dev->out_pin  = out_pin;
    dev->mode_pin = mode_pin;
    dev->on_edge  = NULL;

    /* The sensor drives the line actively both ways, so no internal pulls are
     * needed (and the RP2350 pull-down is unreliable anyway, erratum E9).
     * Its ~3.8 V high level is fine on a fault-tolerant pin: VIH up to 5.5 V
     * while IOVDD is powered - see README-PIR.md. */
    gpio_init(out_pin);
    gpio_set_dir(out_pin, GPIO_IN);
    gpio_disable_pulls(out_pin);

    if (mode_pin != PIR_NO_PIN) {
        uint pin = (uint)mode_pin;
        gpio_init(pin);                          // latch = 0, still an input
        gpio_disable_pulls(pin);                 // no pull-down current
        pir_set_retrigger(dev, retriggerable);   // set the level *before* enabling the driver
        gpio_set_dir(pin, GPIO_OUT);             // driver on -> S1 / BISS0001 A
    }

    /* Registered now, but the pin's edge events stay disabled until
     * pir_irq_enable(). */
    s_dev = dev;
    gpio_add_raw_irq_handler(out_pin, pir_gpio_isr);
    irq_set_enabled(IO_IRQ_BANK0, true);
    return true;
}

bool pir_read(const pir_t *dev)
{
    return gpio_get(dev->out_pin);
}

void pir_set_retrigger(const pir_t *dev, bool retriggerable)
{
    if (dev->mode_pin != PIR_NO_PIN) {
        gpio_put((uint)dev->mode_pin, retriggerable);   // BISS0001 A: 1 = retriggerable
    }
}

void pir_irq_enable(pir_t *dev, pir_edge_cb_t cb)
{
    /* Edges are latched in INTR even while disabled - drop the stale ones,
     * or they would be delivered the moment the interrupt is enabled. */
    dev->on_edge = cb;
    gpio_acknowledge_irq(dev->out_pin, PIR_EDGES);
    gpio_set_irq_enabled(dev->out_pin, PIR_EDGES, true);
}

void pir_irq_disable(pir_t *dev)
{
    gpio_set_irq_enabled(dev->out_pin, PIR_EDGES, false);
    dev->on_edge = NULL;
}
