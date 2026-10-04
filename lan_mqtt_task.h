#pragma once

#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"

// Publishes the PIR sensor's events, state and online status to the Mosquitto broker on
// the Raspberry Pi 4B over plain MQTT on the LAN, and accepts the retrigger-mode command.
// The interface contract the Pi relies on is PIR-MQTT-VMS-Pico.md §3; the broker address
// and credentials are in lan_mqtt_config.h.
//
// Independent of SNTP and of the AWS path: it waits for WiFi only, so motion events keep
// reaching the Pi while the internet is down. Owns the strong overrides of the
// pir_on_motion_*() hooks.

// Creates the event queue and the task. Call once from main(), before
// vTaskStartScheduler().
bool lan_mqtt_task_start(UBaseType_t priority);
