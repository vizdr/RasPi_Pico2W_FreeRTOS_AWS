#pragma once

#include <stdio.h>

// RP2040/RP2350 SMP port requires this, even when running on a single core
#define configNUMBER_OF_CORES                   1

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1
#define configUSE_TICKLESS_IDLE                  0

#define configCPU_CLOCK_HZ                      133000000
#define configTICK_RATE_HZ                      1000

#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                512
#define configMAX_TASK_NAME_LEN                 16
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configIDLE_SHOULD_YIELD                 1

#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    0

#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (128 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP         0

#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW           2

#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH             1024

#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

// Required by the RP2350_ARM_NTZ FreeRTOS port (see its README.md "FreeRTOS configuration
// for Armv8-M"). configRUN_FREERTOS_SECURE_ONLY selects which portINITIAL_EXC_RETURN constant
// the port bakes into every new task's initial stack frame; leaving it unset picked the
// TrustZone-partitioned variant, which is wrong for this non-partitioned NTZ port and corrupted
// EXC_RETURN on the very first context switch (SVC in vStartFirstTask) -> INVPC UsageFault -> HardFault.
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
// Must be 1: the build uses hardware FPU instructions (-march=armv8-m.main+fp -mfloat-abi=softfp).
#define configENABLE_FPU                        1
// Only value tested by this port per its README.
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16

// Needed by the SMP port's cross-core event group signalling
#define INCLUDE_xTimerPendFunctionCall           1

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState            1
#define INCLUDE_xTaskGetCurrentTaskHandle          1
// Needed by pico_async_context_freertos (used by pico_cyw43_arch_lwip_sys_freertos)
#define INCLUDE_xSemaphoreGetMutexHolder           1

// Uses portDISABLE_INTERRUPTS() rather than taskDISABLE_INTERRUPTS(): the RP2350 port's
// portmacro.h invokes configASSERT() before task.h (which defines taskDISABLE_INTERRUPTS) is included.
// Prints the failing location before halting rather than hanging silently with no diagnostics.
#define configASSERT(x) if((x)==0) { printf("configASSERT failed: %s:%d\n", __FILE__, __LINE__); \
                                      for (volatile int _i = 0; _i < 1000000; _i++) {} \
                                      portDISABLE_INTERRUPTS(); for(;;); }
