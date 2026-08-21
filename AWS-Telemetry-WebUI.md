# Telemetry Web UI: DynamoDB + Lambda + API Gateway + S3

This document covers an extension to the AWS IoT Core integration described in
[AWS-RasPi_PicoW2.md](AWS-RasPi_PicoW2.md): a second, parallel telemetry path that stores every
reading in a database and serves it to a browser dashboard, adapting the pattern from a separate
Raspberry Pi 4B / Python course project (pseudo-sensor → MQTT → IoT Core → Rule → Lambda → RDS →
API Gateway → Lambda → HTML UI) to this project's existing FreeRTOS firmware.

**No firmware changes were needed.** The Pico 2 W already publishes telemetry JSON
(`temperature_c`, `ambient_temp_c`, `humidity_pct`) to AWS IoT Core over mutual-TLS MQTT (see
`aws_iot_task.c`) and an existing IoT Rule already forwards it to SQS. Everything below is a
second rule + a small serverless backend added alongside that, on the AWS side only. The original
SQS path is untouched.

---

## 1. Architecture

```
Pico 2 W (unchanged) --MQTT/TLS--> AWS IoT Core
                                        |
                    +-------------------+------------------------+
                    | (existing rule, untouched)      (new rule) |
                    v                                             v
             SQS queue (kept)                         Lambda: pico2w-store-telemetry
                                                                    |
                                                                    v
                                                    DynamoDB table: Pico2wTelemetry
                                                                    ^
                                                                    | Query
                                                          Lambda: pico2w-get-telemetry
                                                                    ^
                                                                    | HTTP API (CORS enabled)
                                                          API Gateway: GET /telemetry
                                                                    ^
                                                                    | fetch()
                                                  S3 static website (web_ui/index.html)
```

Region: `eu-central-1`. Account: `596633517506`.

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
6. **Documentation** — this file, plus the [README.md](README.md) updates.

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
  --assume-role-policy-document file://aws_backend/iam/store_telemetry_trust_policy.json

# Standard managed policy: CloudWatch Logs write access.
aws iam attach-role-policy --role-name Pico2wStoreTelemetryLambdaRole \
  --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole

# Inline policy: dynamodb:PutItem, scoped to just this one table.
aws iam put-role-policy --role-name Pico2wStoreTelemetryLambdaRole \
  --policy-name DynamoDBPutTelemetry \
  --policy-document file://aws_backend/iam/store_telemetry_permissions_policy.json

# Package the Lambda source into a deployable zip.
cd aws_backend && zip -q store_telemetry_lambda.zip store_telemetry_lambda.py

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
  --topic-rule-payload file://aws_backend/iot_store_telemetry_rule.json \
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
  --assume-role-policy-document file://aws_backend/iam/store_telemetry_trust_policy.json

# CloudWatch Logs + an inline policy for dynamodb:Query, scoped to just this table.
aws iam attach-role-policy --role-name Pico2wGetTelemetryLambdaRole \
  --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
aws iam put-role-policy --role-name Pico2wGetTelemetryLambdaRole \
  --policy-name DynamoDBQueryTelemetry \
  --policy-document file://aws_backend/iam/get_telemetry_permissions_policy.json

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
  --policy file://aws_backend/iam/telemetry_ui_bucket_policy.json \
  --region eu-central-1

# Turns the bucket into a static website endpoint, index.html as the default document.
aws s3api put-bucket-website \
  --bucket pico2w-telemetry-ui-596633517506 \
  --website-configuration '{"IndexDocument":{"Suffix":"index.html"}}' \
  --region eu-central-1

# Publish the dashboard page itself.
aws s3 cp web_ui/index.html s3://pico2w-telemetry-ui-596633517506/index.html \
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

**S3 static website hosting, not CloudFront.** Simplest option for a single self-contained HTML
file with no build step. Traded away: the site is served over plain `http://`, not `https://` —
acceptable for a no-secrets read-only dashboard, but worth revisiting (front it with CloudFront)
if this pattern is ever reused for something more sensitive.

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

[aws_backend/store_telemetry_lambda.py](aws_backend/store_telemetry_lambda.py) — reads
`device_id`/`ingest_ts` (attached by the rule's `SELECT`) plus the three telemetry fields off the
event, converts numerics to `Decimal` (DynamoDB's `boto3` binding rejects native `float`), and
`put_item`s one row.

- IAM role `Pico2wStoreTelemetryLambdaRole`: `AWSLambdaBasicExecutionRole` (CloudWatch Logs) +
  inline `DynamoDBPutTelemetry` — `dynamodb:PutItem` scoped to just this table
  ([aws_backend/iam/store_telemetry_permissions_policy.json](aws_backend/iam/store_telemetry_permissions_policy.json)).
- IoT Rule `Pico2wStoreTelemetryRule`
  ([aws_backend/iot_store_telemetry_rule.json](aws_backend/iot_store_telemetry_rule.json)): the
  SQL above, action = invoke `pico2w-store-telemetry`.
- Resource-based Lambda permission (`IoTRuleInvokeStoreTelemetry`) grants `iot.amazonaws.com`
  invoke rights, scoped by `SourceArn` to this specific rule.

### 4.3 Read path: `pico2w-get-telemetry`

[aws_backend/get_telemetry_lambda.py](aws_backend/get_telemetry_lambda.py) — `Query`s DynamoDB for
a given `device_id` (query param, defaults to `pico2w-VZ-210726-freertos`), newest-first
(`ScanIndexForward=False`), up to `limit` (query param, default 20, capped at 500). Converts
`Decimal` back to `float`/`int` for JSON serialization and returns
`{device_id, count, readings: [...]}`.

- IAM role `Pico2wGetTelemetryLambdaRole`: `AWSLambdaBasicExecutionRole` + inline
  `DynamoDBQueryTelemetry` — `dynamodb:Query` scoped to just this table
  ([aws_backend/iam/get_telemetry_permissions_policy.json](aws_backend/iam/get_telemetry_permissions_policy.json)).
- API Gateway HTTP API `Pico2wTelemetryApi` (`ApiId o4apfjc495`):
  - CORS: `AllowOrigins=*`, `AllowMethods=GET`, `AllowHeaders=content-type`.
  - Route `GET /telemetry` → `AWS_PROXY` integration (payload format 2.0) → `pico2w-get-telemetry`.
  - Stage `$default`, auto-deploy.
  - Resource-based Lambda permission (`ApiGatewayInvokeGetTelemetry`) grants
    `apigateway.amazonaws.com` invoke rights, scoped by `SourceArn` to `<api-id>/*/*/telemetry`.

**Live endpoint:** `https://o4apfjc495.execute-api.eu-central-1.amazonaws.com/telemetry`

### 4.4 Web UI

[web_ui/index.html](web_ui/index.html) — self-contained HTML/JS (no build step, no dependencies):
editable API endpoint + row-limit fields, a Load button, a 10-second auto-refresh checkbox, and a
table rendering `device_id`/`temperature_c`/`ambient_temp_c`/`humidity_pct`/`reading_ts` per row.

Hosted on S3 bucket `pico2w-telemetry-ui-596633517506` (`eu-central-1`):
- Static website hosting enabled, `index.html` as the index document.
- Public access block disabled and a bucket policy
  ([aws_backend/iam/telemetry_ui_bucket_policy.json](aws_backend/iam/telemetry_ui_bucket_policy.json))
  grants `s3:GetObject` on bucket objects only (no `ListBucket`, no write access) — public-read of
  a page containing no secrets, just a public API URL.

**Live dashboard:** http://pico2w-telemetry-ui-596633517506.s3-website.eu-central-1.amazonaws.com

---

## 5. Verification performed

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

# Hit the API directly
curl -s "https://o4apfjc495.execute-api.eu-central-1.amazonaws.com/telemetry?limit=5" | python3 -m json.tool
```

---

## 6. Redeploying the Lambdas after a code change

```bash
cd aws_backend
zip -q store_telemetry_lambda.zip store_telemetry_lambda.py
aws lambda update-function-code --function-name pico2w-store-telemetry \
  --zip-file fileb://store_telemetry_lambda.zip --region eu-central-1

zip -q get_telemetry_lambda.zip get_telemetry_lambda.py
aws lambda update-function-code --function-name pico2w-get-telemetry \
  --zip-file fileb://get_telemetry_lambda.zip --region eu-central-1
```

Updating `web_ui/index.html`:

```bash
aws s3 cp web_ui/index.html s3://pico2w-telemetry-ui-596633517506/index.html \
  --content-type text/html --region eu-central-1
```

---

## 7. Known tradeoffs / future work

- **API Gateway endpoint is public with no auth or throttling** — matches the source project's
  approach; acceptable since the data isn't sensitive, but a usage-plan throttle or API key would
  be the natural next hardening step if this is ever exposed more broadly.
- **S3 website is plain `http://`, not `https://`** — no CloudFront/ACM certificate in front of it.
  Fine for a no-secrets dashboard; revisit if reused for anything sensitive.
- **CORS is wide open (`AllowOrigins: *`)** — acceptable since the API is read-only and unauth'd
  anyway; scoping it to the exact S3 website origin would be a minor tightening.
- **Single-device assumption in the UI's default state** — `GetTelemetry` defaults to
  `pico2w-VZ-210726-freertos` but accepts a `device_id` query param override, so multi-device
  support already works at the API level; the HTML UI itself has no device picker yet.
- **No TTL / retention policy on DynamoDB** — the table will grow unbounded at ~1 item/10s
  (~260k items/month). Cheap at this scale (on-demand pricing, small items), but a DynamoDB TTL
  attribute would be a simple way to auto-expire old readings if this runs for a long time.

---

## 8. Resource reference

| Resource | Name / ID |
|---|---|
| DynamoDB table | `Pico2wTelemetry` |
| Write Lambda | `pico2w-store-telemetry` |
| Write Lambda role | `Pico2wStoreTelemetryLambdaRole` |
| IoT Rule (new) | `Pico2wStoreTelemetryRule` |
| Read Lambda | `pico2w-get-telemetry` |
| Read Lambda role | `Pico2wGetTelemetryLambdaRole` |
| API Gateway | `Pico2wTelemetryApi` (`o4apfjc495`) |
| API endpoint | `https://o4apfjc495.execute-api.eu-central-1.amazonaws.com/telemetry` |
| S3 bucket | `pico2w-telemetry-ui-596633517506` |
| Dashboard URL | `http://pico2w-telemetry-ui-596633517506.s3-website.eu-central-1.amazonaws.com` |
| Existing SQS queue (unchanged) | `RaspiPiPico2w-telemetry-queue` |

| File | Role |
|---|---|
| `aws_backend/store_telemetry_lambda.py` | Write-path Lambda source |
| `aws_backend/get_telemetry_lambda.py` | Read-path Lambda source |
| `aws_backend/iot_store_telemetry_rule.json` | IoT Rule definition (SQL + Lambda action) |
| `aws_backend/iam/*.json` | IAM trust/permissions policies + S3 bucket policy |
| `web_ui/index.html` | Static dashboard, deployed to S3 |

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
