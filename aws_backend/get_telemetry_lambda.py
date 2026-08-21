import json
from decimal import Decimal

import boto3
from boto3.dynamodb.conditions import Key

TABLE_NAME = "Pico2wTelemetry"
DEFAULT_DEVICE_ID = "pico2w-VZ-210726-freertos"
DEFAULT_LIMIT = 20
MAX_LIMIT = 500

_table = boto3.resource("dynamodb").Table(TABLE_NAME)


def _to_jsonable(item):
    out = dict(item)
    out["reading_ts"] = int(out["reading_ts"])
    for field in ("temperature_c", "ambient_temp_c", "humidity_pct"):
        if isinstance(out.get(field), Decimal):
            out[field] = float(out[field])
    return out


def lambda_handler(event, context):
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

    body = {"device_id": device_id, "count": len(readings), "readings": readings}
    return {
        "statusCode": 200,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(body),
    }
