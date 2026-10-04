import json
import os
from decimal import Decimal

import boto3
from boto3.dynamodb.conditions import Key

TABLE_NAME = "Pico2wTelemetry"
THRESHOLDS_TABLE_NAME = "Pico2wAlarmThresholds"
DEFAULT_DEVICE_ID = "pico2w-VZ-210726-freertos"
DEFAULT_LIMIT = 20
MAX_LIMIT = 500

# Must match store_telemetry_lambda.py's defaults, used only before a device's first
# POST /telemetry/thresholds.
DEFAULT_THRESHOLDS = {
    "temperature_threshold": Decimal("45.0"),
    "ambient_temp_threshold": Decimal("35.0"),
    "humidity_threshold": Decimal("70.0"),
}

# Set as a Lambda environment variable (never hardcoded here) so this file stays
# secret-free and committable. Must match the custom header CloudFront's /telemetry*
# behavior attaches to its origin requests - see Phase 10 in AWS-Telemetry-WebUI.md.
ORIGIN_VERIFY_HEADER = "x-origin-verify"
ORIGIN_VERIFY_SECRET = os.environ.get("ORIGIN_VERIFY_SECRET")

_table = boto3.resource("dynamodb").Table(TABLE_NAME)
_thresholds_table = boto3.resource("dynamodb").Table(THRESHOLDS_TABLE_NAME)


def _to_jsonable(item):
    out = dict(item)
    out["reading_ts"] = int(out["reading_ts"])
    for field in ("temperature_c", "ambient_temp_c", "humidity_pct"):
        if isinstance(out.get(field), Decimal):
            out[field] = float(out[field])
    if isinstance(out.get("dht_fail_count"), Decimal):
        out["dht_fail_count"] = int(out["dht_fail_count"])
    return out


def _get_thresholds(device_id):
    response = _thresholds_table.get_item(Key={"device_id": device_id})
    item = response.get("Item") or {}
    out = {}
    for key, default in DEFAULT_THRESHOLDS.items():
        out[key] = float(item.get(key, default))
    out["updated_ts"] = int(item["updated_ts"]) if "updated_ts" in item else None
    return out


def lambda_handler(event, context):
    headers = event.get("headers") or {}
    if not ORIGIN_VERIFY_SECRET or headers.get(ORIGIN_VERIFY_HEADER) != ORIGIN_VERIFY_SECRET:
        return {"statusCode": 403, "body": json.dumps({"error": "forbidden"})}

    params = event.get("queryStringParameters") or {}
    device_id = params.get("device_id", DEFAULT_DEVICE_ID)

    try:
        limit = int(params.get("limit", DEFAULT_LIMIT))
    except ValueError:
        limit = DEFAULT_LIMIT
    limit = max(1, min(limit, MAX_LIMIT))

    result = _table.query(
        KeyConditionExpression=Key("device_id").eq(device_id),
        ScanIndexForward=False,
        Limit=limit,
    )
    readings = [_to_jsonable(item) for item in result.get("Items", [])]
    thresholds = _get_thresholds(device_id)

    body = {
        "device_id": device_id,
        "count": len(readings),
        "readings": readings,
        "thresholds": thresholds,
    }
    return {
        "statusCode": 200,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(body),
    }
