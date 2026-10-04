#pragma once

#include <stdbool.h>

// Must be called once from main(), before the scheduler starts and before any
// task that might call wifi_wait_connected()/wifi_is_connected() runs.
void wifi_task_init(void);

// Connects to WIFI_SSID/WIFI_PASSWORD (see wifi_credentials.h) and stays running,
// monitoring the link and reconnecting automatically if it drops.
void wifi_task(void *params);

// Blocks the calling task until WiFi is connected (associated with an IP address).
// Safe to call from multiple tasks - e.g. the SNTP and MQTT tasks each waiting for
// connectivity before doing their own work.
void wifi_wait_connected(void);

bool wifi_is_connected(void);
