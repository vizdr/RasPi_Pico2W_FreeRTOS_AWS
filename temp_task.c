#include "temp_task.h"

#include "pico/stdlib.h"
#include "hardware/adc.h"

#include "FreeRTOS.h"
#include "task.h"

#define TEMP_POLL_MS 1000
#define ADC_VOLTAGE_REF 3.3f
#define ADC_MAX_VALUE   4096.0f // 12-bit ADC: 2^12 distinct readings

// Conversion formula and reference voltage from the RP2350 datasheet (section 12.4.6) /
// hardware/adc.h: T = 27 - (ADC_Voltage - 0.706) / 0.001721
static float read_onboard_temperature_celsius(void) {
    uint16_t raw = adc_read();
    float voltage = raw * ADC_VOLTAGE_REF / ADC_MAX_VALUE;
    return 27.0f - (voltage - 0.706f) / 0.001721f;
}

static volatile float s_last_celsius;

float temp_task_get_last_celsius(void) {
    return s_last_celsius;
}

void temp_task(__unused void *params) {
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);

    for (;;) {
        float celsius = read_onboard_temperature_celsius();
        s_last_celsius = celsius;
        printf("temp_task: onboard temperature = %.2f C\n", (double)celsius);
        vTaskDelay(pdMS_TO_TICKS(TEMP_POLL_MS));
    }
}
