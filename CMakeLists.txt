# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Location of the FreeRTOS kernel managed by the Pico VS Code extension
if (NOT DEFINED FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    set(FREERTOS_KERNEL_PATH ${USERHOME}/.pico-sdk/FreeRTOS-Kernel)
endif()

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(blink_freertos C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Pull in FreeRTOS-Kernel (must be after pico_sdk_init)
include(FreeRTOS_Kernel_import.cmake)

# Add executable. Default name is the project name, version 0.1

add_executable(blink_freertos
    main.c
    aws_credentials.c
    aws_iot_task.c
    led.c
    sensor_task.c
    temp_task.c
    time_task.c
    wifi_task.c
    dht.c
    humiture_task.c
    )

# Generate PIO header for dht.pio (must come after add_executable: it calls
# add_dependencies()/get_target_property() on the target, which must already exist)
pico_generate_pio_header(blink_freertos ${CMAKE_CURRENT_LIST_DIR}/dht.pio)

target_include_directories(blink_freertos PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    )

# pull in common dependencies
target_link_libraries(blink_freertos
    pico_stdlib
    FreeRTOS-Kernel-Heap4
    hardware_i2c
    hardware_adc
    pico_lwip_mqtt
    pico_lwip_mbedtls
    pico_mbedtls
    pico_lwip_sntp
    hardware_pio
    hardware_clocks
    )

if (PICO_CYW43_SUPPORTED)
    target_link_libraries(blink_freertos pico_cyw43_arch_lwip_sys_freertos)
    target_compile_definitions(blink_freertos PRIVATE
        # Actually enforce TLS server certificate verification against our CA chain -
        # without this, altcp_tls_mbedtls would accept any server, defeating the point
        # of shipping the Amazon Root CA at all.
        ALTCP_MBEDTLS_AUTHMODE=MBEDTLS_SSL_VERIFY_REQUIRED
        # Default (1024 words) is tight once mbedtls/TLS work runs in the CYW43 driver's
        # own worker task; the official picow_freertos_http_client_sys example needed
        # this same bump for the same library combination.
        CYW43_TASK_STACK_SIZE=2048
        )
endif()

# USB serial for debug output (same cable used to flash) alongside the default UART
# TEMPORARY DIAGNOSTIC: USB CDC stdio disabled - a TinyUSB control-transfer assert
# (usbd_control_xfer_cb:157) was firing during early enumeration on the native USB
# port, and every task's printf() appeared to hang afterward (likely blocking on the
# wedged USB side of the combined USB+UART stdio output). UART (via the Debug Probe
# bridge) is what we've relied on throughout this project anyway - isolating whether
# USB CDC is the actual culprit before deciding whether it's worth fixing vs. just
# leaving disabled, since this project doesn't functionally need it.
pico_enable_stdio_usb(blink_freertos 0)
pico_enable_stdio_uart(blink_freertos 1)

# create map/bin/hex file etc.
pico_add_extra_outputs(blink_freertos)
