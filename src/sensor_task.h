#pragma once

// Periodically reads an MPU6050 accelerometer over I2C (SDA=GPIO4, SCL=GPIO5 on pico2_w)
// and prints the readings. The hardware layer is the driver in mpu6050.c/h; this task
// only owns the polling period and the retry-while-absent policy.
void sensor_task(void *params);
