/**
 * @file dht.h
 * @brief PIO-based DHT11 / DHT22 driver for RP2350 (Raspberry Pi Pico 2 W) under FreeRTOS.
 *
 * Replaces Makeblock's MeHumitureSensor (Arduino, bit-banged, interrupt-hostile).
 * The timing-critical work runs on a PIO state machine; the calling task only
 * sleeps and drains a FIFO, so no critical sections and no interrupt masking.
 *
 * All values are fixed point, scaled by 10 (231 == 23.1). No floats in the
 * driver; helper inlines are provided if you want them at the application edge.
 */
#ifndef DHT_H
#define DHT_H

#include <stdbool.h>
#include <stdint.h>
#include "hardware/pio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DHT_TYPE_DHT11 = 0,   /**< Makeblock Me Humiture Sensor, integer resolution   */
    DHT_TYPE_DHT22 = 1,   /**< AM2302 / DHT22, 0.1 resolution, signed temperature */
} dht_type_t;

typedef enum {
    DHT_OK = 0,
    DHT_ERR_TIMEOUT,      /**< sensor never answered (wiring, power, dead device) */
    DHT_ERR_CHECKSUM,     /**< frame received but corrupt (noise, long cable)     */
    DHT_ERR_RANGE,        /**< checksum ok but the values are not physical        */
    DHT_ERR_BUSY,         /**< called again inside the minimum sampling interval  */
} dht_status_t;

typedef struct {
    int16_t  temperature_x10;   /**< deci-degrees Celsius, e.g. 231 == 23.1 C */
    int16_t  humidity_x10;      /**< deci-percent RH,      e.g. 455 == 45.5 % */
    uint8_t  raw[5];            /**< the 5 bytes as received, for diagnostics */
} dht_reading_t;

typedef struct {
    PIO         pio;
    uint        sm;
    uint        offset;
    uint        pin;
    dht_type_t  type;
    uint32_t    last_read_ms;   /**< enforces the minimum sampling interval */
    bool        primed;
} dht_t;

/** Minimum interval between reads. DHT11 datasheet: 1 s; 2 s is the safe value. */
#define DHT_MIN_INTERVAL_MS  2000u

/**
 * Claim a PIO state machine, load the program and configure the GPIO.
 * Call once, before the scheduler starts or from a single init task.
 *
 * @param pio   pio0 / pio1 / pio2 (RP2350 has three)
 * @param pin   GPIO wired to the sensor data line (RJ25 S2, white wire)
 * @return      true on success, false if the program did not fit
 */
bool dht_init(dht_t *dev, PIO pio, uint sm, uint pin, dht_type_t type);

/**
 * Perform one complete measurement.
 *
 * MUST be called from a FreeRTOS task (it sleeps ~25 ms via vTaskDelay).
 * Not reentrant per device; guard with a mutex if more than one task calls it.
 */
dht_status_t dht_read(dht_t *dev, dht_reading_t *out);

/** Human-readable status, for logs. */
const char *dht_status_str(dht_status_t s);

static inline float dht_temperature_c(const dht_reading_t *r) {
    return (float)r->temperature_x10 / 10.0f;
}
static inline float dht_humidity_pct(const dht_reading_t *r) {
    return (float)r->humidity_x10 / 10.0f;
}

#ifdef __cplusplus
}
#endif
#endif /* DHT_H */
