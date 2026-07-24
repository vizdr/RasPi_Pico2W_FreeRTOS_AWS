#pragma once

#include <stdbool.h>

// LED here doubles as a WiFi connection indicator: off while disconnected/connecting,
// on once wifi_task has an IP. See wifi_task.c.

// Must be called once from main(), before the scheduler starts.
int led_init(void); // returns PICO_OK on success

void led_set(bool on);
