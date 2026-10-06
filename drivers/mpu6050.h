/**
 * @file mpu6050.h
 * @brief InvenSense MPU6050 accelerometer/gyroscope driver (I2C) for RP2350.
 *
 * Hardware layer only: bus/pin setup, waking the device and reading the
 * acceleration registers. No FreeRTOS dependency - retry policy, polling period and
 * logging belong to the caller (see sensor_task.c for the FreeRTOS service on top).
 *
 * Values are the sensor's raw signed 16-bit counts, not physical units: the scale
 * depends on the configured full-scale range, which this driver leaves at the
 * power-on default of +/-2 g (16384 counts per g).
 */
#ifndef MPU6050_H
#define MPU6050_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Address with AD0 tied low. 0x69 if AD0 is high. */
#define MPU6050_ADDR_DEFAULT  0x68u

typedef enum {
    MPU6050_OK = 0,
    MPU6050_ERR_NO_DEVICE,   /**< address never ACKed: wiring, power, wrong address  */
    MPU6050_ERR_IO,          /**< device ACKed but a transfer came up short          */
} mpu6050_status_t;

typedef struct {
    int16_t x;               /**< raw counts; 16384 per g at the default +/-2 g range */
    int16_t y;
    int16_t z;
} mpu6050_accel_t;

typedef struct {
    i2c_inst_t *i2c;         /**< the bus instance this device sits on */
    uint8_t     addr;
} mpu6050_t;

/**
 * Configure the I2C bus and its pins. Does not talk to the device, so it cannot
 * fail on a missing sensor - call mpu6050_wake() for that. Call once.
 *
 * @param i2c       i2c0 / i2c1
 * @param sda_pin   GPIO for SDA (pulled up internally; an external pull-up is still
 *                  preferable on anything but a short bus)
 * @param scl_pin   GPIO for SCL
 * @param baudrate  bus speed in Hz, e.g. 400000
 * @param addr      MPU6050_ADDR_DEFAULT, or 0x69 with AD0 high
 */
void mpu6050_init(mpu6050_t *dev, i2c_inst_t *i2c, uint sda_pin, uint scl_pin,
                  uint baudrate, uint8_t addr);

/**
 * Clear the sleep bit the device powers up with, which doubles as a presence check:
 * MPU6050_ERR_NO_DEVICE means the address was never ACKed. Safe to call repeatedly,
 * which is how a caller polls for a sensor that is not plugged in yet.
 */
mpu6050_status_t mpu6050_wake(const mpu6050_t *dev);

/** One acceleration sample. Not reentrant per device; guard it if two tasks call it. */
mpu6050_status_t mpu6050_read_accel(const mpu6050_t *dev, mpu6050_accel_t *out);

/** Human-readable status, for logs. */
const char *mpu6050_status_str(mpu6050_status_t s);

#ifdef __cplusplus
}
#endif
#endif /* MPU6050_H */
