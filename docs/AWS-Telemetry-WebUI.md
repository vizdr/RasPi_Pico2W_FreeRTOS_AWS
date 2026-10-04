# Telemetry Web UI: DynamoDB + Lambda + API Gateway + S3

This document covers an extension to the AWS IoT Core integration described in
[AWS-RasPi_PicoW2.md](AWS-RasPi_PicoW2.md): a second, parallel telemetry path that stores every
reading in a database and serves it to a password-protected, HTTPS browser dashboard with
Chart.js line graphs, adapting the pattern from a separate Raspberry Pi 4B / Python course project
(pseudo-sensor → MQTT → IoT Core → Rule → Lambda → RDS → API Gateway → Lambda → HTML UI) to this
project's existing FreeRTOS firmware.

**No firmware changes were needed for the core pipeline.** The Pico 2 W already publishes
telemetry JSON (`temperature_c`, `ambient_temp_c`, `humidity_pct`) to AWS IoT Core over
mutual-TLS MQTT (see `aws_iot_task.c`) and an existing IoT Rule already forwards it to SQS.
Everything in Phases 1–6 is a second rule + a small serverless backend added alongside that, on
the AWS side only. The original SQS path is untouched. Phases 7–11 added a line-chart dashboard,
HTTPS, and password protection (AWS-side + a small, optional firmware diagnostics addition — see
§5.2). Phases 13–17 added configurable alarm thresholds with visual indication, adapted from
[vizdr/ES-Design-WS-Gui](https://github.com/vizdr/ES-Design-WS-Gui) — see §4.9.

---

## 1. Architecture

```
Browser --HTTPS--> CloudFront (single distribution, default *.cloudfront.net domain)
                      |
                      +-- CloudFront Function (viewer-request, every path): HTTP Basic Auth gate
                      |
                      +-- behavior "/*"          --> S3 origin (private, via Origin Access Control)
                      |                               |
                      |                               v
                      |                         web_ui/index.html (Chart.js dashboard)
                      |
                      +-- behavior "/telemetry*" --> API Gateway origin (+ shared-secret header)
                                                          |
                                      GET /telemetry       |       POST /telemetry/thresholds
                                                v          |          v
                                 Lambda: pico2w-get-telemetry    Lambda: pico2w-set-thresholds
                                      (both verify the secret)         |
                                                |      \               v
                                                |       \--> DynamoDB: Pico2wAlarmThresholds
                                                v                      ^
                                                DynamoDB: Pico2wTelemetry  | GetItem (at ingest,
                                                          ^                |  to stamp alarm flags)
                                                          | PutItem        |
                                                Lambda: pico2w-store-telemetry
                                                          ^
                                                          | (new rule)
Pico 2 W (unchanged) --MQTT/TLS--> AWS IoT Core --+
                                                   | (existing rule, untouched)
                                                   v
                                             SQS queue (kept)
```

Region: `eu-central-1` for all telemetry resources; `us-east-1` for CloudFront Functions (a
CloudFront-API-wide requirement — the distribution itself is global). Account: `596633517506`.

---

## 2. Step-by-step plan

### 2.1 Plan

Six phases, in dependency order (mirrors §1's architecture, each step additive — nothing here
touches firmware or the existing SQS rule):

1. **DynamoDB table** — `Pico2wTelemetry`, on-demand billing, `device_id` partition key +
   `reading_ts` sort key.
2. **Write path** — `pico2w-store-telemetry` Lambda + its IAM role, a second IoT Rule
   (`topic(1) AS device_id`) targeting it, and a resource-based permission letting IoT Core invoke
   it. Verified with a manual `iot-data publish` test before trusting real hardware.
3. **Read path** — `pico2w-get-telemetry` Lambda + its IAM role, an API Gateway HTTP API with a
   `GET /telemetry` route and CORS enabled, and a resource-based permission letting API Gateway
   invoke it.
4. **Web UI** — a self-contained `index.html` (no build step) hosted on an S3 bucket configured
   for static website hosting with a public-read (objects only) bucket policy.
5. **Verification** — real device powered on, readings cross-checked between DynamoDB and the API
   response, CloudWatch Logs checked for errors, CORS confirmed against the actual site origin, the
   pre-existing SQS path confirmed still unaffected, and the dashboard opened in a real browser.
6. **Documentation** — this file, plus the [README.md](../README.md) updates.

Phases 7–12 (added afterward, same dependency-order principle): HTTPS and password protection for
the dashboard, requested once the core pipeline was already working end-to-end.

7. **Chart.js line graphs** — a line chart with points added to `web_ui/index.html` (Chart.js via
   CDN, no build step), alongside the existing table.
8. **CloudFront + HTTPS** — an Origin Access Control (OAC) and a CloudFront distribution put in
   front of the S3 bucket; the bucket flips from public-read to private (CloudFront-only).
9. **Password gate** — a CloudFront Function implementing HTTP Basic Auth, tested against sample
   events before going live, then associated with the distribution.
10. **API routed through CloudFront too** — API Gateway added as a second CloudFront origin/behavior
    (same password gate), plus a shared-secret header that `pico2w-get-telemetry` validates so the
    raw API Gateway URL can't bypass the login.
11. **Verification** — the full HTTPS/auth/bypass-closure matrix re-checked end-to-end, plus (an
    unplanned but real) investigation into a DHT11 sensor dropout discovered during this pass —
    see §5.2.
12. **Documentation** — this update.

Phases 13–17 added configurable alarm thresholds with visual indication in the dashboard,
adapting the pattern from [vizdr/ES-Design-WS-Gui](https://github.com/vizdr/ES-Design-WS-Gui)
(a Tornado/WebSocket project) to this serverless, polling-based architecture — see §3.

13. **Threshold storage** — a small `Pico2wAlarmThresholds` DynamoDB table, seeded with defaults.
14. **Set-thresholds endpoint** — `pico2w-set-thresholds` Lambda + its IAM role, and a
    `POST /telemetry/thresholds` route on the existing API (no new CloudFront behavior needed —
    it falls under the existing `/telemetry*` path pattern).
15. **Alarm flags** — `pico2w-store-telemetry` stamps `temperature_alarm`/`ambient_temp_alarm`/
    `humidity_alarm` onto every reading at ingest; `pico2w-get-telemetry` passes them through and
    adds the active `thresholds` to its response.
16. **Web UI** — "Current values" and "Alarm values" stat tiles, threshold inputs + a
    "Set alarm values" button, and red `.in-alarm` indication on tiles and table cells.
17. **Verification + documentation** — see §5.4; this update.

### 2.2 AWS CLI commands applied

Every command that created or configured an AWS resource during implementation, in the order it
was run. (`aws sts get-caller-identity` / `aws configure get region` were run first, off-screen
here, just to confirm the CLI was already pointed at account `596633517506` / `eu-central-1`.)

**Phase 1 — DynamoDB table**

```bash
# Create the table: on-demand billing (no capacity to provision), device_id + reading_ts key.
aws dynamodb create-table \
  --table-name Pico2wTelemetry \
  --attribute-definitions \
      AttributeName=device_id,AttributeType=S \
      AttributeName=reading_ts,AttributeType=S \
  --key-schema \
      AttributeName=device_id,KeyType=HASH \
      AttributeName=reading_ts,KeyType=RANGE \
  --billing-mode PAY_PER_REQUEST \
  --region eu-central-1

# Block until the table is ACTIVE before anything tries to write to it.
aws dynamodb wait table-exists --table-name Pico2wTelemetry --region eu-central-1
```

**Phase 2 — write path (`pico2w-store-telemetry` Lambda + IoT Rule)**

```bash
# Execution role, trusted by the Lambda service.
aws iam create-role --role-name Pico2wStoreTelemetryLambdaRole \
  --assume-role-policy-document file://cloud/aws_backend/iam/store_telemetry_trust_policy.json

# Standard managed policy: CloudWatch Logs write access.
aws iam attach-role-policy --role-name Pico2wStoreTelemetryLambdaRole \
  --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole

# Inline policy: dynamodb:PutItem, scoped to just this one table.
aws iam put-role-policy --role-name Pico2wStoreTelemetryLambdaRole \
  --policy-name DynamoDBPutTelemetry \
  --policy-document file://cloud/aws_backend/iam/store_telemetry_permissions_policy.json

# Package the Lambda source into a deployable zip.
cd cloud/aws_backend && zip -q store_telemetry_lambda.zip store_telemetry_lambda.py

# Deploy the write-path Lambda. (Retried a few times a few seconds apart on first run —
# a just-created IAM role isn't always immediately assumable by Lambda.)
aws lambda create-function \
  --function-name pico2w-store-telemetry \
  --runtime python3.13 \
  --role arn:aws:iam::596633517506:role/Pico2wStoreTelemetryLambdaRole \
  --handler store_telemetry_lambda.lambda_handler \
  --zip-file fileb://store_telemetry_lambda.zip \
  --timeout 10 --memory-size 128 --region eu-central-1

# New IoT Rule (leaves the existing SQS rule untouched): SQL adds device_id from the topic.
aws iot create-topic-rule \
  --rule-name Pico2wStoreTelemetryRule \
  --topic-rule-payload file://cloud/aws_backend/iot_store_telemetry_rule.json \
  --region eu-central-1

# Resource-based permission: only this specific rule may invoke this Lambda.
aws lambda add-permission \
  --function-name pico2w-store-telemetry \
  --statement-id IoTRuleInvokeStoreTelemetry \
  --action lambda:InvokeFunction \
  --principal iot.amazonaws.com \
  --source-arn arn:aws:iot:eu-central-1:596633517506:rule/Pico2wStoreTelemetryRule \
  --region eu-central-1

# Manual test publish, to confirm the rule -> Lambda -> DynamoDB path before trusting hardware.
aws iot-data publish \
  --topic 'pico2w-VZ-210726-freertos/telemetry' \
  --cli-binary-format raw-in-base64-out \
  --payload '{"temperature_c":36.5,"ambient_temp_c":25.1,"humidity_pct":40.2}' \
  --region eu-central-1
```

**Phase 3 — read path (`pico2w-get-telemetry` Lambda + API Gateway)**

```bash
# Execution role (reuses the same generic Lambda trust policy from Phase 2).
aws iam create-role --role-name Pico2wGetTelemetryLambdaRole \
  --assume-role-policy-document file://cloud/aws_backend/iam/store_telemetry_trust_policy.json

# CloudWatch Logs + an inline policy for dynamodb:Query, scoped to just this table.
aws iam attach-role-policy --role-name Pico2wGetTelemetryLambdaRole \
  --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
aws iam put-role-policy --role-name Pico2wGetTelemetryLambdaRole \
  --policy-name DynamoDBQueryTelemetry \
  --policy-document file://cloud/aws_backend/iam/get_telemetry_permissions_policy.json

# Package and deploy the read-path Lambda.
zip -q get_telemetry_lambda.zip get_telemetry_lambda.py
aws lambda create-function \
  --function-name pico2w-get-telemetry \
  --runtime python3.13 \
  --role arn:aws:iam::596633517506:role/Pico2wGetTelemetryLambdaRole \
  --handler get_telemetry_lambda.lambda_handler \
  --zip-file fileb://get_telemetry_lambda.zip \
  --timeout 10 --memory-size 128 --region eu-central-1

# HTTP API with CORS declared up front (avoids a separate CORS config step per route).
aws apigatewayv2 create-api \
  --name Pico2wTelemetryApi --protocol-type HTTP \
  --cors-configuration AllowOrigins="*",AllowMethods="GET",AllowHeaders="content-type" \
  --region eu-central-1
# -> returned ApiId o4apfjc495, used in every command below.

# Proxy integration: API Gateway invokes the Lambda directly and passes its response through.
aws apigatewayv2 create-integration \
  --api-id o4apfjc495 --integration-type AWS_PROXY \
  --integration-uri arn:aws:lambda:eu-central-1:596633517506:function:pico2w-get-telemetry \
  --payload-format-version 2.0 --region eu-central-1
# -> returned IntegrationId w2kwsq8

# Route: GET /telemetry -> that integration.
aws apigatewayv2 create-route \
  --api-id o4apfjc495 --route-key "GET /telemetry" \
  --target "integrations/w2kwsq8" --region eu-central-1

# HTTP APIs need an explicit, auto-deploying stage (unless quick-created via --target).
aws apigatewayv2 create-stage \
  --api-id o4apfjc495 --stage-name '$default' --auto-deploy --region eu-central-1

# Resource-based permission: only this API's /telemetry route may invoke this Lambda.
aws lambda add-permission \
  --function-name pico2w-get-telemetry \
  --statement-id ApiGatewayInvokeGetTelemetry \
  --action lambda:InvokeFunction \
  --principal apigateway.amazonaws.com \
  --source-arn "arn:aws:execute-api:eu-central-1:596633517506:o4apfjc495/*/*/telemetry" \
  --region eu-central-1
```

**Phase 4 — web UI on S3**

```bash
# Hosting bucket (account-ID suffix guarantees the globally-unique name requirement is met).
aws s3api create-bucket \
  --bucket pico2w-telemetry-ui-596633517506 \
  --region eu-central-1 \
  --create-bucket-configuration LocationConstraint=eu-central-1

# S3 blocks public access by default; this specifically allows the bucket policy below to apply.
aws s3api put-public-access-block \
  --bucket pico2w-telemetry-ui-596633517506 \
  --public-access-block-configuration BlockPublicAcls=false,IgnorePublicAcls=false,BlockPublicPolicy=false,RestrictPublicBuckets=false \
  --region eu-central-1

# Public s3:GetObject on bucket objects only (no ListBucket, no write access).
aws s3api put-bucket-policy \
  --bucket pico2w-telemetry-ui-596633517506 \
  --policy file://cloud/aws_backend/iam/telemetry_ui_bucket_policy.json \
  --region eu-central-1

# Turns the bucket into a static website endpoint, index.html as the default document.
aws s3api put-bucket-website \
  --bucket pico2w-telemetry-ui-596633517506 \
  --website-configuration '{"IndexDocument":{"Suffix":"index.html"}}' \
  --region eu-central-1

# Publish the dashboard page itself.
aws s3 cp cloud/web_ui/index.html s3://pico2w-telemetry-ui-596633517506/index.html \
  --content-type text/html --region eu-central-1
```

**Phase 5 — verification** (read/no-op commands only — nothing here changes AWS state; see §5 for
what each check confirmed):

```bash
aws dynamodb query --table-name Pico2wTelemetry \
  --key-condition-expression "device_id = :d" \
  --expression-attribute-values '{":d":{"S":"pico2w-VZ-210726-freertos"}}' \
  --no-scan-index-forward --limit 1 --region eu-central-1

aws logs filter-log-events --log-group-name /aws/lambda/pico2w-store-telemetry \
  --start-time $(( $(date +%s) - 300 ))000 --filter-pattern "ERROR" --region eu-central-1
aws logs filter-log-events --log-group-name /aws/lambda/pico2w-get-telemetry \
  --start-time $(( $(date +%s) - 300 ))000 --filter-pattern "ERROR" --region eu-central-1

curl -s -i "https://o4apfjc495.execute-api.eu-central-1.amazonaws.com/telemetry?limit=1" \
  -H "Origin: http://pico2w-telemetry-ui-596633517506.s3-website.eu-central-1.amazonaws.com"

aws sqs get-queue-url --queue-name RaspiPiPico2w-telemetry-queue --region eu-central-1
aws sqs get-queue-attributes --queue-url <url> \
  --attribute-names ApproximateNumberOfMessages ApproximateNumberOfMessagesNotVisible \
  --region eu-central-1
```

**Phase 8 — CloudFront + HTTPS, S3 goes private**

```bash
# Origin Access Control: lets CloudFront read the S3 bucket without the bucket being public.
aws cloudfront create-origin-access-control --origin-access-control-config '{
  "Name": "pico2w-telemetry-ui-oac", "SigningProtocol": "sigv4",
  "SigningBehavior": "always", "OriginAccessControlOriginType": "s3"
}'
# -> returned OAC Id E1Y8E6E71UI0M8

# Distribution: S3 (via the OAC) as the default origin, CachingDisabled while iterating,
# HTTPS via CloudFront's own default certificate (no custom domain needed).
aws cloudfront create-distribution \
  --distribution-config file://cloud/aws_backend/cloudfront_distribution_config.json
# -> returned distribution Id E1910G7OJTPGYC, domain d3nk6zxm1fgda3.cloudfront.net

# Flip the bucket private: only this exact distribution ARN may read it (condition in the
# policy), Block Public Access re-enabled, and static website hosting removed (CloudFront
# now serves the page from the bucket's plain REST endpoint instead).
aws s3api put-bucket-policy --bucket pico2w-telemetry-ui-596633517506 \
  --policy file://cloud/aws_backend/iam/telemetry_ui_bucket_policy.json --region eu-central-1
aws s3api put-public-access-block --bucket pico2w-telemetry-ui-596633517506 \
  --public-access-block-configuration BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true \
  --region eu-central-1
aws s3api delete-bucket-website --bucket pico2w-telemetry-ui-596633517506 --region eu-central-1

aws cloudfront wait distribution-deployed --id E1910G7OJTPGYC
```

**Phase 9 — password gate (CloudFront Function, HTTP Basic Auth)**

```bash
# Create the function (credential baked into the source - see §3 and §7 for why, and
# aws_backend/cloudfront_basic_auth_function.js.example for the gitignored real file's shape).
aws cloudfront create-function \
  --name pico2w-telemetry-basic-auth \
  --function-config '{"Comment":"HTTP Basic Auth gate","Runtime":"cloudfront-js-2.0"}' \
  --function-code fileb://cloud/aws_backend/cloudfront_basic_auth_function.js \
  --region us-east-1

# Test against sample viewer-request events BEFORE going live: one with no Authorization
# header (expect a 401 response object), one with the correct header (expect the request
# passed through unchanged). Caught nothing wrong here, but worth doing before publishing.
aws cloudfront test-function --name pico2w-telemetry-basic-auth --if-match <ETag> \
  --event-object fileb://<test-event>.json --stage DEVELOPMENT --region us-east-1

# Publish DEVELOPMENT -> LIVE.
aws cloudfront publish-function --name pico2w-telemetry-basic-auth --if-match <ETag> \
  --region us-east-1

# Associate it with the distribution's default (S3) behavior, viewer-request event:
# get the current config, add the FunctionAssociations entry, update-distribution with
# --if-match set to the config's current ETag.
aws cloudfront get-distribution-config --id E1910G7OJTPGYC --region us-east-1
aws cloudfront update-distribution --id E1910G7OJTPGYC \
  --distribution-config file://<modified-config>.json --if-match <ETag> --region us-east-1
aws cloudfront wait distribution-deployed --id E1910G7OJTPGYC --region us-east-1
```

**Phase 10 — route the API through CloudFront too, and close the direct-bypass gap**

```bash
# A random shared secret that only CloudFront and the Lambda will know.
openssl rand -hex 24

# get_telemetry_lambda.py updated to 403 unless this exact header is present - see §4.3.
# The secret itself is set as a Lambda environment variable, never hardcoded in the source.
cd cloud/aws_backend && zip -q get_telemetry_lambda.zip get_telemetry_lambda.py
aws lambda update-function-code --function-name pico2w-get-telemetry \
  --zip-file fileb://get_telemetry_lambda.zip --region eu-central-1
aws lambda update-function-configuration --function-name pico2w-get-telemetry \
  --environment "Variables={ORIGIN_VERIFY_SECRET=<the-secret>}" --region eu-central-1

# Add API Gateway as a second CloudFront origin (custom header carries the secret) and a
# /telemetry* behavior targeting it, same Basic Auth function attached, CachingDisabled
# (it's live data), Managed-AllViewerExceptHostHeader origin request policy (forwards query
# strings/headers to the custom origin without conflicting with its own Host header).
# Same get/modify/update-distribution --if-match cycle as Phase 9.
aws cloudfront get-distribution-config --id E1910G7OJTPGYC --region us-east-1
aws cloudfront update-distribution --id E1910G7OJTPGYC \
  --distribution-config file://<modified-config>.json --if-match <ETag> --region us-east-1
aws cloudfront wait distribution-deployed --id E1910G7OJTPGYC --region us-east-1

# web_ui/index.html's default API endpoint updated to the CloudFront URL (same-origin now).
aws s3 cp cloud/web_ui/index.html s3://pico2w-telemetry-ui-596633517506/index.html \
  --content-type text/html --region eu-central-1
```

**Phase 13 — alarm threshold storage**

```bash
# One item per device; no sort key, since there's exactly one threshold set per device.
aws dynamodb create-table \
  --table-name Pico2wAlarmThresholds \
  --attribute-definitions AttributeName=device_id,AttributeType=S \
  --key-schema AttributeName=device_id,KeyType=HASH \
  --billing-mode PAY_PER_REQUEST --region eu-central-1
aws dynamodb wait table-exists --table-name Pico2wAlarmThresholds --region eu-central-1

# Seed the defaults (the Lambdas also fall back to these in code if the item is missing).
aws dynamodb put-item --table-name Pico2wAlarmThresholds --item '{
  "device_id": {"S": "pico2w-VZ-210726-freertos"},
  "temperature_threshold": {"N": "45.0"},
  "ambient_temp_threshold": {"N": "35.0"},
  "humidity_threshold": {"N": "70.0"},
  "updated_ts": {"S": "<epoch-ms>"}
}' --region eu-central-1
```

**Phase 14 — set-thresholds Lambda + route**

```bash
aws iam create-role --role-name Pico2wSetThresholdsLambdaRole \
  --assume-role-policy-document file://cloud/aws_backend/iam/store_telemetry_trust_policy.json
aws iam attach-role-policy --role-name Pico2wSetThresholdsLambdaRole \
  --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
aws iam put-role-policy --role-name Pico2wSetThresholdsLambdaRole \
  --policy-name DynamoDBPutThresholds \
  --policy-document file://cloud/aws_backend/iam/set_thresholds_permissions_policy.json

cd cloud/aws_backend && zip -q set_thresholds_lambda.zip set_thresholds_lambda.py
aws lambda create-function \
  --function-name pico2w-set-thresholds --runtime python3.13 \
  --role arn:aws:iam::596633517506:role/Pico2wSetThresholdsLambdaRole \
  --handler set_thresholds_lambda.lambda_handler \
  --zip-file fileb://set_thresholds_lambda.zip \
  --timeout 10 --memory-size 128 --region eu-central-1
aws lambda update-function-configuration --function-name pico2w-set-thresholds \
  --environment "Variables={ORIGIN_VERIFY_SECRET=<the-same-secret>}" --region eu-central-1

# New route on the existing HTTP API. No CloudFront change was needed for routing -
# /telemetry/thresholds already matches the existing /telemetry* behavior.
aws apigatewayv2 create-integration --api-id o4apfjc495 --integration-type AWS_PROXY \
  --integration-uri arn:aws:lambda:eu-central-1:596633517506:function:pico2w-set-thresholds \
  --payload-format-version 2.0 --region eu-central-1
aws apigatewayv2 create-route --api-id o4apfjc495 \
  --route-key "POST /telemetry/thresholds" --target "integrations/<IntegrationId>" \
  --region eu-central-1
aws lambda add-permission --function-name pico2w-set-thresholds \
  --statement-id ApiGatewayInvokeSetThresholds --action lambda:InvokeFunction \
  --principal apigateway.amazonaws.com \
  --source-arn "arn:aws:execute-api:eu-central-1:596633517506:o4apfjc495/*/*/telemetry/thresholds" \
  --region eu-central-1

# ...but CloudFront DID need one: the /telemetry* behavior was GET/HEAD-only from Phase 10,
# so POST was rejected with a CloudFront 403 before the request ever reached the auth
# function. CloudFront only accepts specific AllowedMethods sets, so allowing POST means
# taking the full 7-method set (CachedMethods stays GET/HEAD - POST is never cached).
aws cloudfront get-distribution-config --id E1910G7OJTPGYC --region us-east-1
aws cloudfront update-distribution --id E1910G7OJTPGYC \
  --distribution-config file://<modified-config>.json --if-match <ETag> --region us-east-1
```

**Phase 15 — alarm flags at ingest**

```bash
# Both the write and read paths now read the thresholds table.
aws iam put-role-policy --role-name Pico2wStoreTelemetryLambdaRole \
  --policy-name DynamoDBGetAlarmThresholds \
  --policy-document file://cloud/aws_backend/iam/alarm_thresholds_read_policy.json
aws iam put-role-policy --role-name Pico2wGetTelemetryLambdaRole \
  --policy-name DynamoDBGetAlarmThresholds \
  --policy-document file://cloud/aws_backend/iam/alarm_thresholds_read_policy.json

cd cloud/aws_backend
zip -q store_telemetry_lambda.zip store_telemetry_lambda.py
aws lambda update-function-code --function-name pico2w-store-telemetry \
  --zip-file fileb://store_telemetry_lambda.zip --region eu-central-1
zip -q get_telemetry_lambda.zip get_telemetry_lambda.py
aws lambda update-function-code --function-name pico2w-get-telemetry \
  --zip-file fileb://get_telemetry_lambda.zip --region eu-central-1
```

---

## 3. Design decisions

**DynamoDB instead of RDS PostgreSQL** (the source course project's choice). RDS needs a VPC,
subnets, security groups, and Lambda-in-VPC networking (NAT/VPC endpoints for the Lambda to reach
other AWS services) — a lot of moving parts for a single-table telemetry log. DynamoDB is
serverless, needs zero networking setup, and its natural key design (partition key + sort key)
maps directly onto "give me the latest N readings for device X."

**`device_id` sourced from the MQTT topic, not the payload.** The device's JSON payload has no
`device_id` field, and adding one would mean a firmware change. Instead the IoT Rule's SQL adds it
at evaluation time via the `topic(1)` function, which extracts the first slash-separated segment
of the topic the message arrived on:

```sql
SELECT *, topic(1) AS device_id, timestamp() AS ingest_ts
FROM 'pico2w-VZ-210726-freertos/telemetry'
```

This also means a second device publishing to `<other-thing>/telemetry` would get its own
partition automatically, with no Lambda redeploy.

**API Gateway HTTP API (v2), not REST API (v1).** Cheaper, and CORS is a first-class
declarative config on the API itself rather than something to wire up per-route/per-method.

**S3 behind CloudFront + Origin Access Control, not plain S3 static website hosting.** The
original Phase 4 implementation used the S3 website endpoint directly (simplest option, but
`http://` only). Once HTTPS and password protection were requested (Phase 8), CloudFront became
necessary anyway — and CloudFront's OAC mechanism is the standard way to front S3 *and* make the
bucket private at the same time, so there's no plain-HTTP, no-login backdoor left sitting at the
old S3 website URL.

**HTTP Basic Auth via a CloudFront Function, not Cognito.** A real login system (Cognito user
pool + CloudFront authorization) would be the "proper" answer for multiple users, password reset,
or session management. This is a single-person hobby dashboard, so a shared username/password
checked by a tiny edge function is proportionate: near-zero cost, minutes of setup, browser-native
login UI. Traded away: the credential lives in the function's own source (see §7), not a secrets
manager.

**A shared-secret header, not an API Gateway resource policy, to lock the API to CloudFront-only.**
HTTP APIs (API Gateway v2) don't support resource policies the way REST APIs (v1) do, so "only
CloudFront may call this" can't be expressed declaratively on the API itself. CloudFront attaches
a fixed custom header to its origin requests; `pico2w-get-telemetry` checks it and returns `403`
otherwise. Simple, no extra infrastructure (no Lambda authorizer needed), but it is a static
shared secret, not a rotated credential — acceptable here, not a pattern to copy for anything more
sensitive.

**Alarms computed server-side at ingest, not client-side at render.** Adapted from
[vizdr/ES-Design-WS-Gui](https://github.com/vizdr/ES-Design-WS-Gui), whose Tornado server checks
each reading against the thresholds and pushes `temperature_alarm`/`humidity_alarm` booleans to
the browser, which only renders them. Same split here: `pico2w-store-telemetry` stamps the flags
onto the DynamoDB item as the reading arrives, so an alarm becomes a *persisted fact about that
reading* rather than a function of whatever the thresholds happen to be when someone later opens
the page. The alternative (compare in JavaScript at render time) would mean the same historical
reading shows as alarming or not depending on the current threshold — fine for a live-only view,
wrong for a table of past readings.

**Polling, not WebSocket push.** The reference pushes alarm state over a WebSocket because it has
a long-lived Tornado process to push from. This project is serverless, and a WebSocket equivalent
would mean an API Gateway WebSocket API, a connections table, and rethinking auth (the CloudFront
Basic Auth gate doesn't cover a WebSocket upgrade). The dashboard already polls every 10s, which
is exactly the device's publish interval — an alarm cannot surface faster than the data that
causes it, so the added infrastructure would buy nothing here.

**Three independent thresholds, including the die temperature.** `temperature_c` (RP2350 die) and
`ambient_temp_c` (DHT11 room) are different physical quantities that happen to share a unit — the
die idles around 34–37 °C, so a single shared threshold would either false-alarm constantly or be
useless for room temperature. Each gets its own threshold and its own flag
(`temperature_alarm` / `ambient_temp_alarm`), plus `humidity_alarm`.

**Thresholds in their own DynamoDB table, not the telemetry table.** `Pico2wAlarmThresholds` is a
single item per device, keyed by `device_id` alone. Keeping it separate means each Lambda's IAM
policy stays narrow and obvious (`PutItem` on thresholds only, for the setter; `GetItem` on
thresholds plus `Query` on telemetry, for the reader) instead of one broad grant over a table
holding two unrelated kinds of row.

**`>=` comparison, matching the reference.** A reading exactly equal to its threshold counts as
in-alarm. Verified explicitly (§5.4) rather than assumed.

**One chart, dual Y-axis, category (not time-scale) X-axis.** Temperature and humidity share one
`<canvas>` with independent left/right axes rather than two separate charts, since they're read
together. A category axis (formatted time-of-day labels) was used instead of Chart.js's time
scale to avoid pulling in a date-adapter library for what is, in practice, evenly-10s-spaced
samples — a time scale would only matter if gaps in the data needed to be visually proportional.

---

## 4. What was built

### 4.1 DynamoDB table

```
Table:          Pico2wTelemetry
Partition key:  device_id (S)
Sort key:       reading_ts (S)   -- epoch-milliseconds as a string, e.g. "1787310581194"
Billing mode:   PAY_PER_REQUEST  -- no capacity to provision or tune
```

`reading_ts` as a zero-padded-by-construction numeric string sorts correctly both lexicographically
(what DynamoDB's `S` comparison does) and numerically, since epoch-ms values stay 13 digits until
the year 2286 — no zero-padding logic needed.

### 4.2 Write path: `pico2w-store-telemetry`

[aws_backend/store_telemetry_lambda.py](../cloud/aws_backend/store_telemetry_lambda.py) — reads
`device_id`/`ingest_ts` (attached by the rule's `SELECT`) plus the three telemetry fields off the
event, converts numerics to `Decimal` (DynamoDB's `boto3` binding rejects native `float`), and
`put_item`s one row.

- IAM role `Pico2wStoreTelemetryLambdaRole`: `AWSLambdaBasicExecutionRole` (CloudWatch Logs) +
  inline `DynamoDBPutTelemetry` — `dynamodb:PutItem` scoped to just this table
  ([aws_backend/iam/store_telemetry_permissions_policy.json](../cloud/aws_backend/iam/store_telemetry_permissions_policy.json)).
- IoT Rule `Pico2wStoreTelemetryRule`
  ([aws_backend/iot_store_telemetry_rule.json](../cloud/aws_backend/iot_store_telemetry_rule.json)): the
  SQL above, action = invoke `pico2w-store-telemetry`.
- Resource-based Lambda permission (`IoTRuleInvokeStoreTelemetry`) grants `iot.amazonaws.com`
  invoke rights, scoped by `SourceArn` to this specific rule.

### 4.3 Read path: `pico2w-get-telemetry`

[aws_backend/get_telemetry_lambda.py](../cloud/aws_backend/get_telemetry_lambda.py) — `Query`s DynamoDB for
a given `device_id` (query param, defaults to `pico2w-VZ-210726-freertos`), newest-first
(`ScanIndexForward=False`), up to `limit` (query param, default 20, capped at 500). Converts
`Decimal` back to `float`/`int` for JSON serialization and returns
`{device_id, count, readings: [...]}`.

- IAM role `Pico2wGetTelemetryLambdaRole`: `AWSLambdaBasicExecutionRole` + inline
  `DynamoDBQueryTelemetry` — `dynamodb:Query` scoped to just this table
  ([aws_backend/iam/get_telemetry_permissions_policy.json](../cloud/aws_backend/iam/get_telemetry_permissions_policy.json)).
- API Gateway HTTP API `Pico2wTelemetryApi` (`ApiId o4apfjc495`):
  - CORS: `AllowOrigins=*`, `AllowMethods=GET`, `AllowHeaders=content-type` (now moot for the
    CloudFront path, which is same-origin — see §4.7 — but left in place; harmless).
  - Route `GET /telemetry` → `AWS_PROXY` integration (payload format 2.0) → `pico2w-get-telemetry`.
  - Stage `$default`, auto-deploy.
  - Resource-based Lambda permission (`ApiGatewayInvokeGetTelemetry`) grants
    `apigateway.amazonaws.com` invoke rights, scoped by `SourceArn` to `<api-id>/*/*/telemetry`.

Direct endpoint (bypasses the password gate, but returns `403` since Phase 10 — see §4.8):
`https://o4apfjc495.execute-api.eu-central-1.amazonaws.com/telemetry`

### 4.4 Web UI

[web_ui/index.html](../cloud/web_ui/index.html) — self-contained HTML/JS (no build step, no dependencies):
editable API endpoint + row-limit fields, a Load button, a 10-second auto-refresh checkbox, a
Chart.js line chart (§4.5), and a table rendering
`device_id`/`temperature_c`/`ambient_temp_c`/`humidity_pct`/`reading_ts` per row (with the DHT
error reason shown in place of a blank cell when a reading is missing — see §5.2).

Originally hosted directly on an S3 static website endpoint (Phase 4); as of Phase 8 the bucket is
private and only reachable through CloudFront (§4.6).

### 4.5 Chart.js line graphs

Chart.js 4.5.1 loaded via CDN (`cdnjs.cloudflare.com`), one line chart with points
(`pointRadius: 3`) above the existing table: Temperature °C and Ambient °C on the left Y-axis,
Humidity % on an independent right Y-axis, X-axis labeled with reading time. `load()` reverses the
API's newest-first order into chronological order before updating the chart, so it reads
left-to-right as time passing.

### 4.6 CloudFront + HTTPS (S3 origin, private bucket)

- Origin Access Control `pico2w-telemetry-ui-oac` (`E1Y8E6E71UI0M8`) — lets CloudFront read the S3
  bucket without the bucket being public.
- CloudFront distribution `E1910G7OJTPGYC` (domain `d3nk6zxm1fgda3.cloudfront.net`)
  ([aws_backend/cloudfront_distribution_config.json](../cloud/aws_backend/cloudfront_distribution_config.json)
  — the base config used to create it; the live config now also carries the Phase 9/10 additions
  applied via `update-distribution`, not reflected back into this file):
  - Default behavior (`/*`): S3 origin via the OAC, `CachingDisabled`, `redirect-to-https`.
  - HTTPS via CloudFront's own default certificate — no custom domain or ACM cert needed.
- S3 bucket flipped private: Block Public Access re-enabled, static website hosting removed, and
  the bucket policy
  ([aws_backend/iam/telemetry_ui_bucket_policy.json](../cloud/aws_backend/iam/telemetry_ui_bucket_policy.json))
  now grants `s3:GetObject` only to `cloudfront.amazonaws.com`, conditioned on `AWS:SourceArn`
  matching this exact distribution — no other distribution, account, or the public can read it.

### 4.7 Password gate (CloudFront Function, HTTP Basic Auth)

CloudFront Function `pico2w-telemetry-basic-auth`
([aws_backend/cloudfront_basic_auth_function.js.example](../cloud/aws_backend/cloudfront_basic_auth_function.js.example)
— the real, credential-bearing file is gitignored, matching this project's existing
`wifi_credentials.h`/`.h.example` pattern): on `viewer-request`, compares the `Authorization`
header against a precomputed `"Basic " + base64(user:pass)` string; missing/wrong → `401` +
`WWW-Authenticate: Basic`, which triggers the browser's native login popup. Tested via
`aws cloudfront test-function` against both an unauthenticated and an authenticated sample event
before publishing. Associated with **both** CloudFront behaviors (`/*` and `/telemetry*`), so one
login covers the whole site.

### 4.8 API routed through CloudFront, direct bypass closed

- API Gateway added as a second CloudFront origin, with custom header `x-origin-verify: <secret>`
  attached to its origin requests (the secret is a random 24-byte value, `openssl rand -hex 24`).
- Behavior `/telemetry*`: that origin, `CachingDisabled` (live data), `Managed-AllViewerExceptHostHeader`
  origin request policy (forwards query strings/headers without conflicting with the origin's own
  Host header), same Basic Auth function as §4.7.
- `get_telemetry_lambda.py` rejects (`403`) any request whose `x-origin-verify` header doesn't
  match `ORIGIN_VERIFY_SECRET` — set as a Lambda environment variable, never hardcoded in the
  committed source. This is what actually closes the bypass: without it, the raw API Gateway URL
  would still serve data to anyone who found it, regardless of CloudFront's password gate.
- `web_ui/index.html`'s default API endpoint updated to the CloudFront URL (`/telemetry`) — now
  same-origin with the page, so CORS is no longer load-bearing and the browser's cached Basic Auth
  credentials cover the chart/table's `fetch()` calls automatically.

**Live dashboard (HTTPS, password-protected):** https://d3nk6zxm1fgda3.cloudfront.net/
Login: see `aws_backend/cloudfront_basic_auth_function.js` (gitignored) or ask whoever set it.

### 4.9 Alarm thresholds and indication

Storage — `Pico2wAlarmThresholds`, one item per device:

```
Partition key:  device_id (S)          -- no sort key: one threshold set per device
Attributes:     temperature_threshold (N)    -- die temp  (temperature_c)
                ambient_temp_threshold (N)   -- room temp (ambient_temp_c)
                humidity_threshold (N)
                updated_ts (S)               -- epoch-ms, when they were last changed
Billing mode:   PAY_PER_REQUEST
```

Write path — [aws_backend/set_thresholds_lambda.py](../cloud/aws_backend/set_thresholds_lambda.py)
(`pico2w-set-thresholds`): validates a JSON body with numeric `temperature`, `ambient_temp` and
`humidity`, writes the item, returns what it stored. Same `x-origin-verify` check as the read
path (§4.8). IAM role `Pico2wSetThresholdsLambdaRole` — `dynamodb:PutItem` on this table only.
Exposed as `POST /telemetry/thresholds`, which needed no new CloudFront behavior (it matches the
existing `/telemetry*` pattern) but did require widening that behavior's `AllowedMethods` — see
§5.4.

Ingest — [aws_backend/store_telemetry_lambda.py](../cloud/aws_backend/store_telemetry_lambda.py) reads the
thresholds (falling back to `45.0`/`35.0`/`70.0` in code if the item doesn't exist yet) and stamps
up to three booleans onto each reading: `temperature_alarm`, `ambient_temp_alarm`,
`humidity_alarm`. A flag is only written when its source field is present, so a DHT11 dropout
(§5.2) yields *no* flag for ambient/humidity rather than a false `false` — verified in §5.4.

Read — [aws_backend/get_telemetry_lambda.py](../cloud/aws_backend/get_telemetry_lambda.py) passes the
booleans through and adds a top-level `thresholds` object, so one poll returns readings, alarm
state and the active thresholds together:

```json
{
  "device_id": "pico2w-VZ-210726-freertos",
  "count": 2,
  "readings": [
    {"reading_ts": 1791106518901, "temperature_c": 34.16, "temperature_alarm": false,
     "ambient_temp_c": 24.0, "ambient_temp_alarm": false,
     "humidity_pct": 61.0, "humidity_alarm": false, "device_id": "..."}
  ],
  "thresholds": {"temperature_threshold": 45.0, "ambient_temp_threshold": 35.0,
                 "humidity_threshold": 70.0, "updated_ts": 1791106560469}
}
```

UI — [web_ui/index.html](../cloud/web_ui/index.html) gained a "Current values" tile row (temperature,
ambient, humidity, last update) and an "Alarm values" tile row (the three active thresholds plus
when they last changed), three threshold inputs and a "Set alarm values" button. Each current-value
tile turns red via `.in-alarm` driven by *its own* server-supplied flag; the thresholds' timestamp
tile turns red when the newest reading has tripped any threshold (the reference's "any alarm"
indication); and table cells are highlighted per-reading, so alarm history is visible too. The
threshold inputs are seeded from the server once and then left alone — otherwise the 10s
auto-refresh would overwrite whatever is being typed.

---

## 5. Verification performed

### 5.1 Core pipeline (Phases 1–6)

- Simulated publish via `aws iot-data publish` to the telemetry topic → confirmed the item landed
  in DynamoDB with `device_id` correctly derived from the topic.
- Powered on the real device → confirmed real readings accumulating every ~10s
  (`AWS_IOT_PUBLISH_INTERVAL_MS`), temperature climbing plausibly as the RP2350 die warmed up
  post-boot.
- Cross-checked the API's latest reading against DynamoDB's latest item directly — matched.
- Confirmed zero errors in both Lambdas' CloudWatch Logs across active windows.
- Confirmed CORS headers (`access-control-allow-origin: *`) present against the S3 website's
  origin specifically, not just `curl` with no `Origin` header.
- Confirmed the existing SQS queue (`RaspiPiPico2w-telemetry-queue`) is still accumulating
  messages, unaffected by the new rule.
- Opened the dashboard in an actual browser and confirmed it renders and updates correctly.

### 5.2 DHT11 intermittent outage — investigation (Phases 7–11)

While verifying Phases 7–11, the dashboard's Humidity/Ambient columns went blank — `temperature_c`
kept arriving, but `ambient_temp_c`/`humidity_pct` were absent from the JSON entirely (confirmed
directly in DynamoDB, not a rendering bug). Investigation, in order:

1. **Code review for a software cause**: checked whether the recently-added PIR motion sensor
   (`pir.c`/`pir_task.c`, added just before this outage) could be conflicting with the DHT11
   driver. It doesn't — PIR uses GPIO14/13 via plain GPIO interrupts; DHT11 uses GPIO15 via
   `pio1`/state-machine 0. No shared pin, no shared PIO block. The DHT driver's own error-recovery
   logic (`dht_arm()` explicitly re-jumps the PIO program to its `entry` label on every read,
   bounded timeouts, no mutex held across a failed read) also checked out — no bug found.
2. **Ruled out this conversation's own changes as the cause**: the humidity data later recovered
   on the device's *existing, unmodified* firmware — before any diagnostic code (see below) was
   even written, and with no device connected to the machine this conversation runs on
   (confirmed: no `/dev/ttyACM*` present throughout). A deterministic software bug doesn't
   self-heal; a dead sensor doesn't either. That left an intermittent physical cause (most likely
   a disturbed/marginal connector from the recent PIR wiring work, or transient power-rail noise)
   as the working theory.
3. **Physical fix**: the user reseated/fixed the wiring; humidity data resumed immediately and
   stayed stable across repeated checks afterward.
4. **Diagnostics added regardless** (`humiture_task.c`/`.h`, `aws_iot_task.c`): when a reading is
   stale, the MQTT payload now includes `dht_err` (`"timeout"`/`"checksum"`/`"range"`/`"busy"`,
   from the DHT driver's own status enum) and `dht_fail_count` instead of silently omitting the
   fields. Both Lambdas updated to persist/pass these through, and the dashboard shows
   `err: <reason>` in the Humidity cell instead of a blank `-` when it happens again. Compiled
   successfully (`cmake --build build`) but **not yet flashed** — this conversation has no
   debug-probe/USB access to the board. The distinction matters for next time: `timeout` repeatedly
   means the sensor isn't responding at all (power/connector); `checksum` mixed with good reads
   means a flaky/noisy line (loose contact, which matches what actually happened here); `range`
   means a decode-level issue.

### 5.3 HTTPS / password-protection matrix (Phase 11)

| Check | Result |
|---|---|
| Dashboard page, no login | `401` |
| Dashboard page, correct login | `200` |
| `/telemetry` via CloudFront, no login | `401` |
| `/telemetry` via CloudFront, correct login | `200` + fresh JSON |
| Raw S3 bucket (direct) | `403` — private, OAC-only |
| Raw API Gateway (direct, no secret header) | `403` — bypass closed |
| Plain `http://` on the CloudFront domain | `301` → HTTPS |
| Both Lambdas' CloudWatch Logs | 0 errors |
| Chart.js CDN | `200` |
| Existing SQS queue | still accumulating, unaffected |
| Dashboard opened in a real browser | renders and updates correctly |

### 5.4 Alarm thresholds (Phase 17)

Run against the live device and the live dashboard URL, not a test stub:

| Check | Result |
|---|---|
| Thresholds survive in DynamoDB, returned to any fresh client | pass — same values from a new session |
| Alarm trips when readings exceed thresholds (set 30/20/50 against 34.16/24.0/61.0) | all three flags `true` on the next reading |
| Alarm clears when thresholds restored (45/35/70) | all three flags `false` |
| `>=` boundary: threshold set to *exactly* the current reading (35.1 vs 35.1) | `temperature_alarm: true` — matches the reference's `>=` |
| DHT11 dropout (synthetic publish, `temperature_c` + `dht_err` only, ambient/humidity thresholds at 1.0 so any value would trip) | `temperature_alarm: true`; `ambient_temp_alarm`/`humidity_alarm` **absent**, not false alarms |
| Invalid body: non-numeric / missing field / malformed JSON / `Infinity` | `400` with a usable error message (see the bug below) |
| All three Lambdas' CloudWatch Logs after the fix | 0 errors |

Two real bugs were caught by this pass rather than by inspection:

1. **`POST` was rejected by CloudFront with a `403`** before reaching the auth function or the
   API. The `/telemetry*` behavior had been created in Phase 10 with `AllowedMethods` =
   `GET,HEAD`, correct at the time since the API was read-only. CloudFront only accepts specific
   method sets, so enabling `POST` means taking the full seven (`CachedMethods` stays `GET,HEAD`).
2. **Non-numeric threshold values returned `500`, not `400`.** `Decimal("hot")` raises
   `decimal.InvalidOperation`, which subclasses `ArithmeticError` — not `ValueError` — so it
   escaped the handler's `except (ValueError, KeyError, TypeError)` entirely. CloudWatch Logs
   confirmed the exact cause (`[ERROR] InvalidOperation: [<class 'decimal.ConversionSyntax'>]`).
   Fixed by catching `InvalidOperation` and additionally rejecting non-finite values, since
   `Decimal("Infinity")` parses happily and would then fail at DynamoDB write time instead.

Not verified here: the rendered page. This environment has no browser or JS runtime, so the tiles,
the red `.in-alarm` styling and the Set button were checked at the data/API level and by
cross-checking every element id the script looks up against the markup — not by rendering or
clicking.

Useful commands for future debugging:

```bash
# Latest N readings straight from DynamoDB
aws dynamodb query --table-name Pico2wTelemetry \
  --key-condition-expression "device_id = :d" \
  --expression-attribute-values '{":d":{"S":"pico2w-VZ-210726-freertos"}}' \
  --no-scan-index-forward --limit 5 --region eu-central-1

# Recent errors from either Lambda
aws logs filter-log-events --log-group-name /aws/lambda/pico2w-store-telemetry \
  --start-time $(( $(date +%s) - 300 ))000 --filter-pattern "ERROR" --region eu-central-1
aws logs filter-log-events --log-group-name /aws/lambda/pico2w-get-telemetry \
  --start-time $(( $(date +%s) - 300 ))000 --filter-pattern "ERROR" --region eu-central-1

# Hit the live dashboard's API through CloudFront (needs the login)
curl -s -u "<user>:<pass>" "https://d3nk6zxm1fgda3.cloudfront.net/telemetry?limit=5" | python3 -m json.tool

# Confirm the direct API Gateway URL is still closed (expect 403, no secret header sent)
curl -s -o /dev/null -w "%{http_code}\n" "https://o4apfjc495.execute-api.eu-central-1.amazonaws.com/telemetry"
```

---

## 6. Redeploying the Lambdas after a code change

```bash
cd cloud/aws_backend
zip -q store_telemetry_lambda.zip store_telemetry_lambda.py
aws lambda update-function-code --function-name pico2w-store-telemetry \
  --zip-file fileb://store_telemetry_lambda.zip --region eu-central-1

zip -q get_telemetry_lambda.zip get_telemetry_lambda.py
aws lambda update-function-code --function-name pico2w-get-telemetry \
  --zip-file fileb://get_telemetry_lambda.zip --region eu-central-1

zip -q set_thresholds_lambda.zip set_thresholds_lambda.py
aws lambda update-function-code --function-name pico2w-set-thresholds \
  --zip-file fileb://set_thresholds_lambda.zip --region eu-central-1
```

Changing alarm thresholds without the dashboard:

```bash
curl -s -u "<user>:<pass>" -X POST "https://d3nk6zxm1fgda3.cloudfront.net/telemetry/thresholds" \
  -H "Content-Type: application/json" \
  -d '{"temperature":45,"ambient_temp":35,"humidity":70}'
```

Updating `web_ui/index.html` (now behind CloudFront, not the old S3 website endpoint):

```bash
aws s3 cp cloud/web_ui/index.html s3://pico2w-telemetry-ui-596633517506/index.html \
  --content-type text/html --region eu-central-1
```

Changing the dashboard password: edit `EXPECTED_AUTH` in the gitignored
`aws_backend/cloudfront_basic_auth_function.js` (regenerate the base64 with
`printf '<user>:<pass>' | base64`), then:

```bash
aws cloudfront update-function --name pico2w-telemetry-basic-auth \
  --function-code fileb://cloud/aws_backend/cloudfront_basic_auth_function.js \
  --if-match <ETag-from-describe-function> --region us-east-1
aws cloudfront publish-function --name pico2w-telemetry-basic-auth \
  --if-match <ETag-from-the-update-above> --region us-east-1
```

Rotating the CloudFront↔API-Gateway shared secret: `openssl rand -hex 24`, then
`aws lambda update-function-configuration ... --environment "Variables={ORIGIN_VERIFY_SECRET=<new>}"`
and update the `CustomHeaders` value on the API Gateway origin in the distribution config
(get-distribution-config → edit → update-distribution, as in §2.2 Phase 10).

Flashing the DHT11 diagnostics firmware change (§5.2, compiled but not yet deployed to the
board): `cmake --build build`, then flash via the Pico VS Code extension or `picotool` as usual
(see [README.md](../README.md)'s *Building and flashing* section) — needs the device connected over
USB/debug-probe, which this conversation doesn't have.

---

## 7. Known tradeoffs / future work

- **The Basic Auth credential lives in the CloudFront Function's own source**, readable by anyone
  with CloudFront console/CLI access to this AWS account — not a secrets-manager-grade credential,
  fine for keeping casual visitors out of a hobby dashboard. See §3 for why this was chosen over
  Cognito.
- **The CloudFront↔API-Gateway header is a static shared secret**, not a rotated credential or a
  signed request. Sufficient to close the "find the raw URL" bypass; not a substitute for a real
  authorizer if this pattern is ever reused for something sensitive.
- **Single shared login, no per-user accounts, sessions, or audit trail** — proportionate for one
  person's hobby dashboard; would need Cognito (or similar) to go further.
- **API Gateway's CORS is still wide open (`AllowOrigins: *`)** from Phase 3, now effectively
  unused (the CloudFront path is same-origin) but left in place since it's harmless and the raw
  API Gateway URL is already blocked at the Lambda layer (§4.8) regardless of CORS.
- **CloudFront's own default domain (`*.cloudfront.net`), no custom domain** — chosen for zero
  extra setup; a branded domain would need a Route 53 hosted zone (or equivalent DNS) and an ACM
  certificate.
- **Single-device assumption in the UI's default state** — `GetTelemetry` defaults to
  `pico2w-VZ-210726-freertos` but accepts a `device_id` query param override, so multi-device
  support already works at the API level; the HTML UI itself has no device picker yet.
- **No TTL / retention policy on DynamoDB** — the table will grow unbounded at ~1 item/10s
  (~260k items/month). Cheap at this scale (on-demand pricing, small items), but a DynamoDB TTL
  attribute would be a simple way to auto-expire old readings if this runs for a long time.
- **DHT11 diagnostic fields (`dht_err`/`dht_fail_count`) are implemented end-to-end but not yet
  flashed to the device** (§5.2) — the backend/UI will display them as soon as the firmware is
  reflashed; until then, a stale reading still just shows a blank cell with no reason.
- **Alarms are indication-only** — a tripped threshold turns the dashboard red, and nothing else.
  No notification, no alarm log, no acknowledgement. Since the flags are already persisted per
  reading, an IoT Rule or DynamoDB Stream → SNS/email would be the natural next step, as would a
  "currently in alarm" query rather than reading it off the newest row.
- **Thresholds are per-device but the UI assumes one device** — the schema and both Lambdas key
  thresholds by `device_id`, so a second board gets its own set automatically; the page just has
  no device picker (same gap as the readings view).
- **No range validation on thresholds** — any finite number is accepted, so a humidity threshold
  of `500` (never reachable) or `-5` (always tripped) is allowed. Deliberate: the plausible range
  depends on the sensor, and a wrong-but-finite threshold is immediately visible on the dashboard
  and trivially corrected.

---

## 8. Resource reference

| Resource | Name / ID |
|---|---|
| DynamoDB table | `Pico2wTelemetry` |
| DynamoDB table (alarm thresholds) | `Pico2wAlarmThresholds` |
| Write Lambda | `pico2w-store-telemetry` |
| Write Lambda role | `Pico2wStoreTelemetryLambdaRole` |
| IoT Rule (new) | `Pico2wStoreTelemetryRule` |
| Read Lambda | `pico2w-get-telemetry` |
| Read Lambda role | `Pico2wGetTelemetryLambdaRole` |
| Set-thresholds Lambda | `pico2w-set-thresholds` |
| Set-thresholds Lambda role | `Pico2wSetThresholdsLambdaRole` |
| API Gateway | `Pico2wTelemetryApi` (`o4apfjc495`) |
| API Gateway direct URL (blocked, §4.8) | `https://o4apfjc495.execute-api.eu-central-1.amazonaws.com/telemetry` |
| S3 bucket (private, CloudFront-only) | `pico2w-telemetry-ui-596633517506` |
| Origin Access Control | `pico2w-telemetry-ui-oac` (`E1Y8E6E71UI0M8`) |
| CloudFront distribution | `E1910G7OJTPGYC` (`d3nk6zxm1fgda3.cloudfront.net`) |
| CloudFront Function (Basic Auth) | `pico2w-telemetry-basic-auth` |
| **Live dashboard (HTTPS, password-protected)** | `https://d3nk6zxm1fgda3.cloudfront.net/` |
| Existing SQS queue (unchanged) | `RaspiPiPico2w-telemetry-queue` |

| File | Role |
|---|---|
| `aws_backend/store_telemetry_lambda.py` | Write-path Lambda source |
| `aws_backend/get_telemetry_lambda.py` | Read-path Lambda source (origin-verify check, §4.8) |
| `aws_backend/set_thresholds_lambda.py` | Alarm-threshold write Lambda source (§4.9) |
| `aws_backend/iot_store_telemetry_rule.json` | IoT Rule definition (SQL + Lambda action) |
| `aws_backend/iam/*.json` | IAM trust/permissions policies + S3 bucket policy (CloudFront-only, §4.6) |
| `aws_backend/cloudfront_distribution_config.json` | Base CloudFront distribution config (Phase 8) |
| `aws_backend/cloudfront_basic_auth_function.js.example` | Basic Auth function template (committed) |
| `aws_backend/cloudfront_basic_auth_function.js` | Real function source with credential (gitignored) |
| `web_ui/index.html` | Dashboard (table + Chart.js), deployed to S3, served via CloudFront |

---

## Appendix A: AWS terminology

A glossary of the AWS services and concepts used in this document, each tied back to how it's
concretely used in this project.

**IAM (Identity and Access Management)** — AWS's account-wide permissions system. Everything that
acts inside the account (a person, a script, a Lambda function, a service like IoT Core) does so
*as* an IAM identity, and can only do what that identity's attached policies explicitly allow
(deny-by-default).

- **IAM User** — an identity for a human or a long-lived script, authenticated with a password
  and/or access keys. Used here as `arn:aws:iam::596633517506:user/VladiZ`, the identity behind
  every `aws` CLI command run throughout this project (confirmed via `aws sts get-caller-identity`).
- **IAM Role** — an identity with no permanent credentials of its own, *assumed* temporarily by
  something else (an AWS service, another account, a person via SSO). Used here for
  `Pico2wStoreTelemetryLambdaRole` and `Pico2wGetTelemetryLambdaRole` — each Lambda function
  assumes its role on every invocation to get temporary credentials for calling DynamoDB/writing
  logs, rather than the function shipping a long-lived access key.
- **Trust policy** (a role's `AssumeRolePolicyDocument`) — *who/what* is allowed to assume a role.
  `aws_backend/iam/store_telemetry_trust_policy.json` says "the Lambda service
  (`lambda.amazonaws.com`) may assume this role" — nothing else can.
- **Permissions policy** — *what* an identity (user or role) can actually do once assumed. Used
  here as small, single-purpose inline policies: `DynamoDBPutTelemetry` (only `dynamodb:PutItem`,
  only on the `Pico2wTelemetry` table) and `DynamoDBQueryTelemetry` (only `dynamodb:Query`, same
  table) — each Lambda gets exactly the one permission it needs, nothing broader.
- **Managed policy** — a reusable, AWS-authored (or account-authored) policy that can be attached
  to multiple identities. Used here as `AWSLambdaBasicExecutionRole`, attached to both Lambda
  roles to grant the standard CloudWatch Logs write permissions every Lambda needs.
- **ARN (Amazon Resource Name)** — the globally unique identifier for any AWS resource, e.g.
  `arn:aws:dynamodb:eu-central-1:596633517506:table/Pico2wTelemetry`. Policies grant permissions
  *on* specific ARNs (that's what "scoped to just this table" means mechanically) rather than on
  resource names alone.

**Lambda** — AWS's serverless compute service: upload code (here, a single `.py` file zipped up),
and AWS runs it on demand in response to an event, with no server to provision or manage. Billed
per invocation/duration, effectively free at this project's request volume. Used twice here:
`pico2w-store-telemetry` (triggered by the IoT Rule, writes to DynamoDB) and
`pico2w-get-telemetry` (triggered by API Gateway, reads from DynamoDB). Each function needs an
**execution role** (an IAM Role, see above) and, separately, a **resource-based policy**
(`aws lambda add-permission`) authorizing *which specific other resource* (this IoT Rule; this API
route) is allowed to invoke it — two different permission directions: the role controls what the
function can call *out* to, the resource policy controls who can call *in* to it.

**DynamoDB** — AWS's managed NoSQL key-value/document database. No servers to size, no schema
migrations for new attributes, scales automatically. Core concepts used here:

- **Table** — `Pico2wTelemetry`, the single table this project uses.
- **Partition key** — the primary lookup key; DynamoDB uses it to decide which internal storage
  partition an item lives on. Used here as `device_id`, so all readings for one device are grouped
  together.
- **Sort key** — an optional second key that orders items *within* a partition key. Used here as
  `reading_ts` (epoch-milliseconds), which is exactly what makes "give me the latest N readings
  for this device" (`Query` with `ScanIndexForward=False`) a single efficient operation instead of
  a table scan.
- **On-demand (`PAY_PER_REQUEST`) billing** — pay per read/write request instead of provisioning
  fixed read/write capacity units up front. Chosen here since telemetry traffic is small and
  bursty-by-device-count rather than steady at a known rate.

**API Gateway** — a managed front door for HTTP APIs: handles routing, CORS, throttling, and
integrates directly with Lambda (or other backends) without you running a web server. Used here as
`Pico2wTelemetryApi`, an **HTTP API** (the newer, cheaper API Gateway product; the older **REST
API** product has more features — request validation, usage plans, API keys — that weren't needed
here).

- **Route** — a method + path pattern, e.g. `GET /telemetry`, mapped to a backend.
- **Integration** — the backend a route calls; here an `AWS_PROXY` integration, meaning API
  Gateway invokes the Lambda directly and passes its JSON response straight through to the caller.
- **Stage** — a named, independently-deployable snapshot of the API's routes (commonly `dev`,
  `prod`, or the HTTP API default `$default`). Used here with `--auto-deploy`, so route changes go
  live immediately without a separate manual deploy step.
- **CORS (Cross-Origin Resource Sharing)** — a browser security mechanism: by default, JavaScript
  running on one origin (here, the S3 website's URL) can't `fetch()` a different origin (the API
  Gateway URL) unless that API explicitly says it's allowed to, via
  `Access-Control-Allow-Origin` response headers. API Gateway's built-in CORS config
  (`AllowOrigins=*`) generates those headers automatically so `web_ui/index.html`'s `fetch()` call
  isn't silently blocked by the browser.

**S3 (Simple Storage Service)** — AWS's object storage: store arbitrary files ("objects") durably,
addressed by key, no filesystem/server involved.

- **Bucket** — a top-level, globally-uniquely-named container for objects. Used here as
  `pico2w-telemetry-ui-596633517506` (the AWS account ID suffix is what guarantees the
  global-uniqueness requirement is met).
- **Static website hosting** — an S3 feature that serves a bucket's objects over plain HTTP as a
  website, with a configurable index document. Used here to serve `web_ui/index.html` without
  running any actual web server.
- **Bucket policy** — a resource-based IAM policy attached directly to the bucket (as opposed to
  to a user/role). Used here to grant `s3:GetObject` to everyone (`Principal: "*"`) but *only* on
  objects (`.../*`, not the bucket itself) — public read of the page, no ability to list the
  bucket's contents or write to it.
- **Block Public Access** — an account/bucket-level safety switch that overrides bucket policies
  to prevent accidental public exposure; it's on by default and had to be explicitly disabled here
  for the public-read bucket policy above to actually take effect.

**IoT Core** — AWS's managed MQTT broker plus a rules engine for routing incoming device messages.
The Pico 2 W connects to it as an MQTT client (see `AWS-RasPi_PicoW2.md` for the mutual-TLS
connection details) and publishes telemetry to a topic; IoT Core doesn't store or process that
message itself — a **Thing** (`pico2w-VZ-210726-freertos`, the IoT Core identity representing this
specific device) is authorized to connect/publish/subscribe via an attached **policy**, and one or
more **Rules** decide what happens to messages on a given topic.

- **IoT Rule** — a standing subscription with a SQL-like filter (`SELECT ... FROM 'topic'`) and one
  or more **actions** to take for every matching message. Two rules exist on this project's
  telemetry topic, run independently and in parallel: the pre-existing one forwards to SQS, and
  `Pico2wStoreTelemetryRule` (added in this document) forwards to the `pico2w-store-telemetry`
  Lambda, with SQL functions `topic(1)` (extracts a topic segment) and `timestamp()` (rule
  evaluation time) enriching the message with fields the device itself never sends.

**SQS (Simple Queue Service)** — AWS's managed message queue: producers push messages in, one or
more consumers pull them out, with per-message visibility timeouts standing in for
acknowledgment. Used here as the project's *original*, still-untouched telemetry path: the
pre-existing IoT Rule forwards every reading into the `RaspiPiPico2w-telemetry-queue` queue for
manual/CLI consumption — a durable buffer, not a database (nothing queries "the last 20 readings"
out of SQS; consuming a message removes it once acknowledged).

**CloudWatch Logs** — where Lambda (and most AWS services) write their `print()`/log output,
organized into one **log group** per Lambda function (e.g. `/aws/lambda/pico2w-store-telemetry`).
Used throughout this project's verification steps (`aws logs filter-log-events`,
`aws logs tail`) to confirm each Lambda ran without errors.

**CloudFront** — AWS's content delivery network (CDN): a global network of edge locations that
cache/proxy requests to an **origin** (S3, API Gateway, or an arbitrary HTTP server), with HTTPS,
custom domains, and edge compute built in. Used here as a single distribution (`E1910G7OJTPGYC`)
in front of both the S3-hosted dashboard and the API Gateway endpoint, so both get one HTTPS
domain and one password gate instead of being separately exposed.

- **Distribution** — the CloudFront resource itself: a set of **origins** and **behaviors**
  (routing rules) under one domain. Propagating a config change to all of CloudFront's edge
  locations takes several minutes (`aws cloudfront wait distribution-deployed`), unlike most AWS
  API calls which take effect immediately.
- **Origin** — a backend CloudFront fetches from on a cache miss. Used here twice: the S3 bucket
  (via OAC) and API Gateway (as a "custom origin," since it's an arbitrary HTTPS endpoint, not S3).
- **Behavior** — a path-pattern-to-origin mapping, plus its own cache policy, origin request
  policy, and function associations. Used here as the default (`/*` → S3) and `/telemetry*` →
  API Gateway) behaviors, each independently configured but sharing the same Basic Auth function.
- **Origin Access Control (OAC)** — the mechanism that lets CloudFront fetch from a *private* S3
  bucket: CloudFront signs its requests to S3 (SigV4), and the bucket's policy trusts only this
  specific distribution's ARN. This is what makes it possible to have S3 be both the dashboard's
  storage *and* fully private at the same time — no public bucket, no public website endpoint.
- **CloudFront Function** — a tiny JavaScript function (a restricted subset of the language, no
  network/filesystem access) that runs at the edge on every request, before CloudFront even checks
  its cache. Used here (`pico2w-telemetry-basic-auth`) to implement the password gate: it inspects
  the `Authorization` header and can short-circuit the request with a `401` before it ever reaches
  S3 or API Gateway. Far lighter-weight than **Lambda@Edge** (CloudFront's other edge-compute
  option, which runs full Lambda functions and supports more but costs more and is slower to
  propagate) — a CloudFront Function was sufficient for a stateless header check like this.
- **Origin request policy** — controls which parts of the viewer's original request (headers,
  query strings, cookies) CloudFront forwards on to the origin. Used here
  (`Managed-AllViewerExceptHostHeader`) on the `/telemetry*` behavior so `limit`/`device_id` query
  strings reach the Lambda, without forwarding the viewer's original `Host` header, which would
  conflict with API Gateway's own expectations for that header.

**HTTP Basic Auth** — the oldest, simplest HTTP authentication scheme: the client sends
`Authorization: Basic <base64(username:password)>` on every request, and the server returns `401`
with a `WWW-Authenticate: Basic` header to prompt for credentials (browsers show this as a native
login popup, not a styled page). No sessions, no cookies, no tokens — the browser just resends the
same header on every subsequent request to that origin once entered, which is also why the
dashboard's `fetch()` calls to `/telemetry` don't need a separate login (§4.8). Weaker than a real
login system (the "password" is base64, not encrypted, so this is only meaningfully secure over
HTTPS — which CloudFront provides), but proportionate for gating a single-person hobby dashboard.
