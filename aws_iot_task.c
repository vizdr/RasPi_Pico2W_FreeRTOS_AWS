#include "aws_iot_task.h"
#include "aws_credentials.h"
#include "wifi_task.h"
#include "time_task.h"
#include "temp_task.h"
#include "humiture_task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/apps/mqtt.h"
#include "lwip/altcp_tls.h"

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#include <stdio.h>

// Your account's iot:Data-ATS endpoint (IoT Core console -> Settings, or Manage ->
// Domain configurations).
#define AWS_IOT_ENDPOINT "a3dp4umq4qv6ul-ats.iot.eu-central-1.amazonaws.com"
#define AWS_IOT_PORT 8883

// Matches the Thing name, per AWS's convention (and to avoid two clients with different
// IDs both claiming the same Thing, which triggers AWS's forced-disconnect-on-duplicate
// behavior). NOTE: the policy attached to this device's certificate must permit this
// client ID and the topic below - the policy AWS's wizard generated in Phase 1 only
// allows the SDK connectivity-test's own client IDs/topics ("sdk/test/*" etc.) and will
// reject this until updated.
#define AWS_IOT_CLIENT_ID "pico2w-VZ-210726-freertos"
#define AWS_IOT_TELEMETRY_TOPIC "pico2w-VZ-210726-freertos/telemetry"

#define AWS_IOT_PUBLISH_INTERVAL_MS 10000
#define AWS_IOT_RECONNECT_DELAY_MS  5000
#define AWS_IOT_CONNECT_WAIT_MS     15000
#define DNS_RESOLVE_WAIT_MS         10000

#define MQTT_CONNECTED_BIT (1 << 0)
#define DNS_RESOLVED_BIT   (1 << 0)

static EventGroupHandle_t s_mqtt_event_group;
static EventGroupHandle_t s_dns_event_group;
static volatile bool s_dns_ok;
static ip_addr_t s_resolved_ip;

static void dns_found_cb(__unused const char *name, const ip_addr_t *ipaddr, __unused void *arg) {
    if (ipaddr) {
        s_resolved_ip = *ipaddr;
        s_dns_ok = true;
    } else {
        s_dns_ok = false;
    }
    xEventGroupSetBits(s_dns_event_group, DNS_RESOLVED_BIT);
}

// Re-resolved on every (re)connect attempt rather than cached once, in case AWS ever
// changes the IP behind the endpoint hostname.
static bool resolve_endpoint(ip_addr_t *out) {
    xEventGroupClearBits(s_dns_event_group, DNS_RESOLVED_BIT);

    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(AWS_IOT_ENDPOINT, out, dns_found_cb, NULL);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        return true; // already cached; *out was filled in directly
    }
    if (err != ERR_INPROGRESS) {
        printf("aws_iot_task: dns_gethostbyname failed immediately, err=%d\n", err);
        return false;
    }

    xEventGroupWaitBits(s_dns_event_group, DNS_RESOLVED_BIT, pdTRUE, pdTRUE,
                         pdMS_TO_TICKS(DNS_RESOLVE_WAIT_MS));
    if (s_dns_ok) {
        *out = s_resolved_ip;
        return true;
    }
    printf("aws_iot_task: DNS resolution failed/timed out\n");
    return false;
}

static void mqtt_connection_cb(__unused mqtt_client_t *client, __unused void *arg,
                                mqtt_connection_status_t status) {
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("aws_iot_task: MQTT connected\n");
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
    } else {
        printf("aws_iot_task: MQTT connection status=%d\n", (int)status);
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
    }
}

static void mqtt_pub_request_cb(__unused void *arg, err_t result) {
    if (result != ERR_OK) {
        printf("aws_iot_task: publish failed, err=%d\n", result);
    }
}

void aws_iot_task(__unused void *params) {
    wifi_wait_connected();
    time_wait_synced(); // must come before any TLS handshake - see time_task.c

    s_mqtt_event_group = xEventGroupCreate();
    s_dns_event_group = xEventGroupCreate();
    if (s_mqtt_event_group == NULL || s_dns_event_group == NULL) {
        panic("aws_iot_task: failed to create event groups");
    }

    struct altcp_tls_config *tls_config = altcp_tls_create_config_client_2wayauth(
        aws_root_ca_pem, aws_root_ca_pem_len,
        aws_private_key_pem, aws_private_key_pem_len,
        NULL, 0,
        aws_device_cert_pem, aws_device_cert_pem_len);
    if (tls_config == NULL) {
        panic("aws_iot_task: failed to create TLS config");
    }

    mqtt_client_t *client = mqtt_client_new();
    if (client == NULL) {
        panic("aws_iot_task: failed to create MQTT client");
    }

    struct mqtt_connect_client_info_t client_info = {
        .client_id = AWS_IOT_CLIENT_ID,
        .client_user = NULL,
        .client_pass = NULL,
        .keep_alive = 60,
        .will_topic = NULL,
        .will_msg = NULL,
        .will_qos = 0,
        .will_retain = 0,
        .tls_config = tls_config,
    };

    for (;;) {
        ip_addr_t ip;
        if (!resolve_endpoint(&ip)) {
            vTaskDelay(pdMS_TO_TICKS(AWS_IOT_RECONNECT_DELAY_MS));
            continue;
        }

        printf("aws_iot_task: connecting to %s (%s)...\n", AWS_IOT_ENDPOINT, ipaddr_ntoa(&ip));
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);

        cyw43_arch_lwip_begin();
        err_t err = mqtt_client_connect(client, &ip, AWS_IOT_PORT, mqtt_connection_cb, NULL, &client_info);
        cyw43_arch_lwip_end();
        if (err != ERR_OK) {
            printf("aws_iot_task: mqtt_client_connect call failed, err=%d\n", err);
            vTaskDelay(pdMS_TO_TICKS(AWS_IOT_RECONNECT_DELAY_MS));
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(s_mqtt_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdTRUE,
                                                pdMS_TO_TICKS(AWS_IOT_CONNECT_WAIT_MS));
        if (!(bits & MQTT_CONNECTED_BIT)) {
            printf("aws_iot_task: MQTT connect timed out\n");
            vTaskDelay(pdMS_TO_TICKS(AWS_IOT_RECONNECT_DELAY_MS));
            continue;
        }

        // Publish periodically for as long as the connection holds.
        while (mqtt_client_is_connected(client)) {
            char payload[96];
            int len;

            // temperature_c stays the RP2350's own die reading (temp_task) for backward
            // compatibility; ambient_temp_c/humidity_pct are the DHT11's actual room
            // readings, added only when humiture_task has a non-stale sample.
            dht_reading_t dht;
            if (humiture_get_latest(&dht, NULL)) {
                len = snprintf(payload, sizeof(payload),
                                "{\"temperature_c\":%.2f,\"ambient_temp_c\":%.1f,\"humidity_pct\":%.1f}",
                                (double)temp_task_get_last_celsius(),
                                (double)dht_temperature_c(&dht),
                                (double)dht_humidity_pct(&dht));
            } else {
                len = snprintf(payload, sizeof(payload), "{\"temperature_c\":%.2f}",
                                (double)temp_task_get_last_celsius());
            }

            cyw43_arch_lwip_begin();
            err_t perr = mqtt_publish(client, AWS_IOT_TELEMETRY_TOPIC, payload, (u16_t)len,
                                       1 /* QoS 1 */, 0 /* retain */, mqtt_pub_request_cb, NULL);
            cyw43_arch_lwip_end();

            if (perr != ERR_OK) {
                printf("aws_iot_task: mqtt_publish call failed, err=%d\n", perr);
            } else {
                printf("aws_iot_task: published %s to %s\n", payload, AWS_IOT_TELEMETRY_TOPIC);
            }
            vTaskDelay(pdMS_TO_TICKS(AWS_IOT_PUBLISH_INTERVAL_MS));
        }

        printf("aws_iot_task: MQTT disconnected, reconnecting...\n");
    }
}
