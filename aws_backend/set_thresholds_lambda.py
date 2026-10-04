import json
import os
import time
from decimal import Decimal, InvalidOperation

import boto3

TABLE_NAME = "Pico2wAlarmThresholds"
DEFAULT_DEVICE_ID = "pico2w-VZ-210726-freertos"

# Same shared secret as get_telemetry_lambda.py - see Phase 10 in AWS-Telemetry-WebUI.md.
ORIGIN_VERIFY_HEADER = "x-origin-verify"
ORIGIN_VERIFY_SECRET = os.environ.get("ORIGIN_VERIFY_SECRET")

_table = boto3.resource("dynamodb").Table(TABLE_NAME)


def lambda_handler(event, context):
    headers = event.get("headers") or {}
    if not ORIGIN_VERIFY_SECRET or headers.get(ORIGIN_VERIFY_HEADER) != ORIGIN_VERIFY_SECRET:
        return {"statusCode": 403, "body": json.dumps({"error": "forbidden"})}

    try:
        body = json.loads(event.get("body") or "{}")
        temperature = Decimal(str(body["temperature"]))
        ambient_temp = Decimal(str(body["ambient_temp"]))
        humidity = Decimal(str(body["humidity"]))
        # Decimal accepts "Infinity"/"NaN", which DynamoDB then rejects at write time -
        # reject them here so a bad body is a 400, not a 500.
        if not all(v.is_finite() for v in (temperature, ambient_temp, humidity)):
            raise ValueError("thresholds must be finite numbers")
    except (ValueError, KeyError, TypeError, InvalidOperation):
        return {
            "statusCode": 400,
            "headers": {"Content-Type": "application/json"},
            "body": json.dumps({
                "error": "expected a JSON body with numeric 'temperature', 'ambient_temp', 'humidity'"
            }),
        }

    device_id = body.get("device_id", DEFAULT_DEVICE_ID)
    updated_ts = str(int(time.time() * 1000))

    item = {
        "device_id": device_id,
        "temperature_threshold": temperature,
        "ambient_temp_threshold": ambient_temp,
        "humidity_threshold": humidity,
        "updated_ts": updated_ts,
    }
    _table.put_item(Item=item)

    return {
        "statusCode": 200,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps({
            "device_id": device_id,
            "temperature_threshold": float(temperature),
            "ambient_temp_threshold": float(ambient_temp),
            "humidity_threshold": float(humidity),
            "updated_ts": int(updated_ts),
        }),
    }
