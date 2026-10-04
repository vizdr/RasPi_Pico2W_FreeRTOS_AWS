import time
from decimal import Decimal

import boto3

TABLE_NAME = "Pico2wTelemetry"
THRESHOLDS_TABLE_NAME = "Pico2wAlarmThresholds"
NUMERIC_FIELDS = ("temperature_c", "ambient_temp_c", "humidity_pct", "dht_fail_count")
STRING_FIELDS = ("dht_err",)

# Used only if a device has never had thresholds set via POST /telemetry/thresholds.
DEFAULT_THRESHOLDS = {
    "temperature_threshold": Decimal("45.0"),    # die temp (temperature_c)
    "ambient_temp_threshold": Decimal("35.0"),   # room temp (ambient_temp_c)
    "humidity_threshold": Decimal("70.0"),
}

_table = boto3.resource("dynamodb").Table(TABLE_NAME)
_thresholds_table = boto3.resource("dynamodb").Table(THRESHOLDS_TABLE_NAME)


def _get_thresholds(device_id):
    response = _thresholds_table.get_item(Key={"device_id": device_id})
    item = response.get("Item") or {}
    return {key: item.get(key, default) for key, default in DEFAULT_THRESHOLDS.items()}


def lambda_handler(event, context):
    # device_id and ingest_ts are attached by the IoT Rule's SELECT clause
    # (topic(1) AS device_id, timestamp() AS ingest_ts), not published by the device itself.
    device_id = event.get("device_id")
    if not device_id:
        raise ValueError("event missing device_id (expected from IoT Rule's topic(1))")

    reading_ts = event.get("ingest_ts", int(time.time() * 1000))

    item = {
        "device_id": device_id,
        "reading_ts": str(reading_ts),
    }
    for field in NUMERIC_FIELDS:
        if event.get(field) is not None:
            item[field] = Decimal(str(event[field]))
    for field in STRING_FIELDS:
        if event.get(field) is not None:
            item[field] = str(event[field])

    # Alarm flags are computed once, at ingest, against the thresholds in effect right
    # now - so they stay a persisted fact about this specific reading even if the
    # thresholds are changed later. A missing field (e.g. a stale DHT11, see
    # humiture_get_diag()) just means no flag is set for it, not a false alarm.
    thresholds = _get_thresholds(device_id)
    if "temperature_c" in item:
        item["temperature_alarm"] = item["temperature_c"] >= thresholds["temperature_threshold"]
    if "ambient_temp_c" in item:
        item["ambient_temp_alarm"] = item["ambient_temp_c"] >= thresholds["ambient_temp_threshold"]
    if "humidity_pct" in item:
        item["humidity_alarm"] = item["humidity_pct"] >= thresholds["humidity_threshold"]

    _table.put_item(Item=item)
    return {"status": "ok", "device_id": device_id, "reading_ts": item["reading_ts"]}
