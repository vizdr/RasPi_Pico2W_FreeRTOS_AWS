#pragma once
#include "FreeRTOS.h"
#include "task.h"
#include "dht.h"
bool humiture_task_start(UBaseType_t priority);
bool humiture_get_latest(dht_reading_t *out, uint32_t *age_ms);
void humiture_on_sample(const dht_reading_t *r); // weak hook, override to act on new samples

// Diagnostics: the outcome of the most recent read attempt (regardless of staleness)
// and how many read attempts have failed since boot. Lets callers report *why* no
// fresh sample is available, not just that one isn't.
void humiture_get_diag(dht_status_t *last_status, uint32_t *error_count);
