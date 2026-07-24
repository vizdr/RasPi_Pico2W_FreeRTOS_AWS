#pragma once

#include <stdbool.h>
#include <stdint.h>

// Must be called once from main(), before the scheduler starts.
void time_task_init(void);

// Waits for WiFi, then starts SNTP (pool.ntp.org) and keeps the system clock synced for
// as long as the device runs. Required before any TLS handshake: mbedtls's X.509 validity
// checks use the standard time() function, which reads nothing meaningful until this has
// run at least once.
void time_task(void *params);

// Blocks the calling task until the system clock has been set at least once via SNTP.
void time_wait_synced(void);

bool time_is_synced(void);

// Called from the SNTP_SET_SYSTEM_TIME macro in lwipopts.h - not meant to be called
// directly elsewhere.
void time_task_sntp_set_system_time(uint32_t sec);
