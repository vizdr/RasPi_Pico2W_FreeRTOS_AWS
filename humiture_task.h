#pragma once
#include "FreeRTOS.h"
#include "task.h"
#include "dht.h"
bool humiture_task_start(UBaseType_t priority);
bool humiture_get_latest(dht_reading_t *out, uint32_t *age_ms);
