#pragma once

// Connects to AWS IoT Core over MQTT-over-TLS (mutual TLS: our device cert/key, AWS's
// identity verified against the Amazon Root CA - see aws_credentials.h) and periodically
// publishes sensor telemetry. Reconnects automatically if the connection drops.
void aws_iot_task(void *params);
