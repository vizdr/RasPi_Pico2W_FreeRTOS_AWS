import time
from decimal import Decimal

import boto3

TABLE_NAME = "Pico2wTelemetry"
NUMERIC_FIELDS = ("temperature_c", "ambient_temp_c", "humidity_pct", "dht_fail_count")
STRING_FIELDS = ("dht_err",)

_table = boto3.resource("dynamodb").Table(TABLE_NAME)


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

    _table.put_item(Item=item)
    return {"status": "ok", "device_id": device_id, "reading_ts": item["reading_ts"]}
