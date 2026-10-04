#include "sensor_task.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "FreeRTOS.h"
#include "task.h"

#define MPU6050_ADDR              0x68
#define MPU6050_REG_PWR_MGMT_1    0x6B
#define MPU6050_REG_ACCEL_XOUT_H  0x3B

#define SENSOR_POLL_MS 200

static bool mpu6050_init(void) {
    i2c_init(i2c_default, 400 * 1000); // 400kHz
    gpio_set_function(PICO_DEFAULT_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICO_DEFAULT_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICO_DEFAULT_I2C_SDA_PIN);
    gpio_pull_up(PICO_DEFAULT_I2C_SCL_PIN);

    // Wake the sensor up (it starts in sleep mode) and confirm it's actually present:
    // i2c_write_blocking returns PICO_ERROR_GENERIC if the address is never ACKed,
    // which is the normal outcome here if no MPU6050 is wired to GPIO4/GPIO5 yet.
    uint8_t buf[2] = {MPU6050_REG_PWR_MGMT_1, 0x00};
    int ret = i2c_write_blocking(i2c_default, MPU6050_ADDR, buf, 2, false);
    return ret == 2;
}

static bool mpu6050_read_accel(int16_t *ax, int16_t *ay, int16_t *az) {
    uint8_t reg = MPU6050_REG_ACCEL_XOUT_H;
    uint8_t data[6];

    // Register-address write with no stop condition, then a repeated-start read —
    // the standard pattern for "select register, then read N bytes" on I2C sensors.
    if (i2c_write_blocking(i2c_default, MPU6050_ADDR, &reg, 1, true) != 1) {
        return false;
    }
    if (i2c_read_blocking(i2c_default, MPU6050_ADDR, data, 6, false) != 6) {
        return false;
    }

    *ax = (int16_t)(data[0] << 8 | data[1]);
    *ay = (int16_t)(data[2] << 8 | data[3]);
    *az = (int16_t)(data[4] << 8 | data[5]);
    return true;
}

void sensor_task(__unused void *params) {
    bool sensor_present = mpu6050_init();
    if (!sensor_present) {
        printf("sensor_task: no MPU6050 ACK on I2C0 (SDA=GPIO%d, SCL=GPIO%d) - "
               "check wiring, will keep retrying\n",
               PICO_DEFAULT_I2C_SDA_PIN, PICO_DEFAULT_I2C_SCL_PIN);
    }

    for (;;) {
        if (!sensor_present) {
            sensor_present = mpu6050_init();
        } else {
            int16_t ax, ay, az;
            if (mpu6050_read_accel(&ax, &ay, &az)) {
                printf("accel: x=%d y=%d z=%d\n", ax, ay, az);
            } else {
                printf("sensor_task: I2C read failed, will retry\n");
                sensor_present = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_MS));
    }
}
