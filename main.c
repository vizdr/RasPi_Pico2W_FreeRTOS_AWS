/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "aws_iot_task.h"
#include "sensor_task.h"
#include "temp_task.h"
#include "time_task.h"
#include "wifi_task.h"
#include "humiture_task.h"

void vApplicationMallocFailedHook(void) {
    panic("malloc failed");
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    panic("stack overflow: %s", pcTaskName);
}

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer,
                                    uint32_t *pulIdleTaskStackSize) {
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer,
                                     uint32_t *pulTimerTaskStackSize) {
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

int main(void) {
    stdio_init_all();

    // led_init()/cyw43_arch_init() is called from within wifi_task, not here - see the
    // comment in wifi_task.c for why it must run after the scheduler has started.
    wifi_task_init();
    time_task_init();

    // Larger stack than configMINIMAL_STACK_SIZE: printf() is stack-hungry (vsnprintf internals).
    xTaskCreate(sensor_task, "Sensor", 512, NULL, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(temp_task, "Temp", 512, NULL, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(wifi_task, "WiFi", 1024, NULL, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(time_task, "Time", 1024, NULL, tskIDLE_PRIORITY + 1, NULL);
    // TLS handshake work is stack-hungry (mbedtls), hence the larger allowance here.
    xTaskCreate(aws_iot_task, "AwsIot", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    humiture_task_start(tskIDLE_PRIORITY + 1);   /* low priority; it only sleeps */


    vTaskStartScheduler();

    // Should never reach here
    for (;;) {
    }
}
