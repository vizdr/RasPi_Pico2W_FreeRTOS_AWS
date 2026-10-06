/**
 * @file mpu6050.c
 * @brief InvenSense MPU6050 accelerometer driver (I2C) for RP2350.
 */

#include "mpu6050.h"

#include "hardware/gpio.h"

#define REG_PWR_MGMT_1    0x6Bu
#define REG_ACCEL_XOUT_H  0x3Bu

/* ------------------------------------------------------------------------- */

void mpu6050_init(mpu6050_t *dev, i2c_inst_t *i2c, uint sda_pin, uint scl_pin,
                  uint baudrate, uint8_t addr)
{
    dev->i2c  = i2c;
    dev->addr = addr;

    i2c_init(i2c, baudrate);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);
}

mpu6050_status_t mpu6050_wake(const mpu6050_t *dev)
{
    /* The device powers up asleep; clearing PWR_MGMT_1 starts conversions. A missing
     * sensor never ACKs its address, which the SDK reports as PICO_ERROR_GENERIC
     * rather than a short count - so anything but the full 2 bytes means "not there". */
    uint8_t buf[2] = { REG_PWR_MGMT_1, 0x00u };
    return i2c_write_blocking(dev->i2c, dev->addr, buf, sizeof(buf), false) == (int)sizeof(buf)
           ? MPU6050_OK
           : MPU6050_ERR_NO_DEVICE;
}

mpu6050_status_t mpu6050_read_accel(const mpu6050_t *dev, mpu6050_accel_t *out)
{
    uint8_t reg = REG_ACCEL_XOUT_H;
    uint8_t data[6];

    /* Register-address write with no stop condition, then a repeated-start read -
     * the standard "select register, then read N bytes" pattern for I2C sensors. */
    if (i2c_write_blocking(dev->i2c, dev->addr, &reg, 1, true) != 1) {
        return MPU6050_ERR_NO_DEVICE;
    }
    if (i2c_read_blocking(dev->i2c, dev->addr, data, sizeof(data), false) != (int)sizeof(data)) {
        return MPU6050_ERR_IO;
    }

    /* Big-endian pairs, high byte first. */
    out->x = (int16_t)((uint16_t)data[0] << 8 | data[1]);
    out->y = (int16_t)((uint16_t)data[2] << 8 | data[3]);
    out->z = (int16_t)((uint16_t)data[4] << 8 | data[5]);
    return MPU6050_OK;
}

const char *mpu6050_status_str(mpu6050_status_t s)
{
    switch (s) {
        case MPU6050_OK:            return "ok";
        case MPU6050_ERR_NO_DEVICE: return "no device";
        case MPU6050_ERR_IO:        return "io";
        default:                    return "?";
    }
}
