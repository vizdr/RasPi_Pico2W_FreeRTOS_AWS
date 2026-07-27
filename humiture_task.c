/**
 * @file humiture_task.c
 * @brief FreeRTOS sensor task for the Makeblock Me Humiture (DHT11) on Pico 2 W.
 *
 * Owns the sensor exclusively, so dht_read() needs no mutex. Other tasks read
 * the last good sample through humiture_get_latest(), which is protected by a
 * mutex, or subscribe by polling the sample counter.
 */

#include "humiture_task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "pico/time.h"

#include <stdio.h>
#include <string.h>

/* ---- configuration ------------------------------------------------------ */

#ifndef HUMITURE_GPIO
#define HUMITURE_GPIO            15u     /* RJ25 S2 (white wire) */
#endif

#ifndef HUMITURE_PERIOD_MS
#define HUMITURE_PERIOD_MS       5000u   /* >= DHT_MIN_INTERVAL_MS (2000) */
#endif

#define HUMITURE_MAX_RETRIES     3u
#define HUMITURE_RETRY_GAP_MS    2200u   /* respects the sensor's 1 s minimum */
#define HUMITURE_STALE_AFTER_MS  60000u

/* ---- shared state ------------------------------------------------------- */

typedef struct {
    dht_reading_t reading;
    uint32_t      timestamp_ms;
    uint32_t      sample_count;
    uint32_t      error_count;
    dht_status_t  last_status;
    bool          valid;
} humiture_state_t;

static humiture_state_t  s_state;
static SemaphoreHandle_t s_lock;
static dht_t             s_dev;

/**
 * Weak hook: override this in your MQTT layer to forward each good sample,
 * e.g. queue a Device Shadow update via the coreMQTT-Agent. Runs in the sensor
 * task context, so it must not block for long -- post to a queue and return.
 */
__attribute__((weak))
void humiture_on_sample(const dht_reading_t *r)
{
    (void)r;
}

bool humiture_get_latest(dht_reading_t *out, uint32_t *age_ms)
{
    bool ok = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_state.valid) {
            *out = s_state.reading;
            uint32_t now = to_ms_since_boot(get_absolute_time());
            uint32_t age = now - s_state.timestamp_ms;
            if (age_ms) {
                *age_ms = age;
            }
            ok = (age < HUMITURE_STALE_AFTER_MS);
        }
        xSemaphoreGive(s_lock);
    }
    return ok;
}

/* ---- task --------------------------------------------------------------- */

static void humiture_task(void *arg)
{
    (void)arg;

    /* DHT11 needs ~1 s after power-up before its first conversion is valid. */
    vTaskDelay(pdMS_TO_TICKS(1500));

    TickType_t next = xTaskGetTickCount();

    for (;;) {
        dht_reading_t r;
        dht_status_t  st = DHT_ERR_TIMEOUT;

        for (uint32_t attempt = 0; attempt < HUMITURE_MAX_RETRIES; attempt++) {
            st = dht_read(&s_dev, &r);
            if (st == DHT_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(HUMITURE_RETRY_GAP_MS));
        }

        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_state.last_status = st;
            if (st == DHT_OK) {
                s_state.reading      = r;
                s_state.timestamp_ms = to_ms_since_boot(get_absolute_time());
                s_state.sample_count++;
                s_state.valid = true;
            } else {
                s_state.error_count++;
            }
            xSemaphoreGive(s_lock);
        }

        if (st == DHT_OK) {
            printf("[humiture] %d.%d C  %d.%d %%RH\n",
                   r.temperature_x10 / 10, r.temperature_x10 % 10,
                   r.humidity_x10    / 10, r.humidity_x10    % 10);
            humiture_on_sample(&r);
        } else {
            printf("[humiture] read failed: %s (raw %02x %02x %02x %02x %02x)\n",
                   dht_status_str(st),
                   r.raw[0], r.raw[1], r.raw[2], r.raw[3], r.raw[4]);
        }

        vTaskDelayUntil(&next, pdMS_TO_TICKS(HUMITURE_PERIOD_MS));
    }
}

bool humiture_task_start(UBaseType_t priority)
{
    memset(&s_state, 0, sizeof(s_state));

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return false;
    }

    /* pio0 is often taken by CYW43 on the Pico W family -- pio1 or pio2 is
     * the safer choice on the Pico 2 W. */
    if (!dht_init(&s_dev, pio1, 0, HUMITURE_GPIO, DHT_TYPE_DHT11)) {
        return false;
    }

    return xTaskCreate(humiture_task, "humiture", 768, NULL,
                       priority, NULL) == pdPASS;
}
