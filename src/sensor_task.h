#pragma once

// Periodically reads an MPU6050 accelerometer over I2C0 (SDA=GPIO4, SCL=GPIO5 on pico2_w)
// and prints the readings. See sensor_task.c for wiring/protocol details.
void sensor_task(void *params);
