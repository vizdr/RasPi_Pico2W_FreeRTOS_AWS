/**
 * @file sensor_task.c
 * @brief FreeRTOS service for the MPU6050 accelerometer (driver: mpu6050.c/h).
 *
 * Owns the sensor exclusively, so the driver needs no mutex. Keeps retrying while the
 * sensor is absent, which is the normal state here: the MPU6050 is an example and is
 * not currently wired to real hardware.
 */

#include "sensor_task.h"
#include "mpu6050.h"

#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

/* ---- configuration ------------------------------------------------------ */

#ifndef SENSOR_I2C
#define SENSOR_I2C          i2c_default
#endif

#ifndef SENSOR_SDA_GPIO
#define SENSOR_SDA_GPIO     PICO_DEFAULT_I2C_SDA_PIN
#endif

#ifndef SENSOR_SCL_GPIO
#define SENSOR_SCL_GPIO     PICO_DEFAULT_I2C_SCL_PIN
#endif

#define SENSOR_BAUDRATE     (400 * 1000)
#define SENSOR_POLL_MS      200u

/* ---- task --------------------------------------------------------------- */

static mpu6050_t s_dev;

void sensor_task(__unused void *params)
{
    mpu6050_init(&s_dev, SENSOR_I2C, SENSOR_SDA_GPIO, SENSOR_SCL_GPIO,
                 SENSOR_BAUDRATE, MPU6050_ADDR_DEFAULT);

    bool present = (mpu6050_wake(&s_dev) == MPU6050_OK);
    if (!present) {
        printf("sensor_task: no MPU6050 ACK on I2C (SDA=GPIO%d, SCL=GPIO%d) - "
               "check wiring, will keep retrying\n", SENSOR_SDA_GPIO, SENSOR_SCL_GPIO);
    }

    for (;;) {
        if (!present) {
            present = (mpu6050_wake(&s_dev) == MPU6050_OK);
        } else {
            mpu6050_accel_t accel;
            mpu6050_status_t st = mpu6050_read_accel(&s_dev, &accel);
            if (st == MPU6050_OK) {
                printf("accel: x=%d y=%d z=%d\n", accel.x, accel.y, accel.z);
            } else {
                printf("sensor_task: read failed: %s, will retry\n", mpu6050_status_str(st));
                present = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_MS));
    }
}
