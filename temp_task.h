#pragma once

// Periodically samples the RP2350's internal ADC-based temperature sensor and prints the
// reading in Celsius. See temp_task.c for the conversion formula.
void temp_task(void *params);

// Most recent reading (0.0 before the first sample). A torn read of this float is possible
// in principle but harmless here - at worst one telemetry publish carries a stale or mixed
// value, not worth a mutex for a slow-changing status metric like temperature.
float temp_task_get_last_celsius(void);
