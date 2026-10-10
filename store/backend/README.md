# iotstore backend

The API of the iotdash app store: developers upload app packages (`.idbz`, made by `iotdash pack`),
boards list and install them. One AWS Lambda (Python 3.13) behind a function URL or an HTTP API;
data in DynamoDB, files in S3. One entry point dispatches the routes, every answer is
`{"success", "data"}`, accounts use JWT, deploys are a zip and the AWS CLI.
The API itself is in [../docs/API.md](../docs/API.md).

## Layout

```
IOTStore/
  entry_points/IOTStore.py   main(event, context): CORS, auth gate, dispatch, error wrapping
  libs/
    tables.py                DynamoDB tables (names, key schemas), ids, timestamps
    storage.py               S3 presigned POST/GET, or local files + signed URLs (run_local.sh)
    auth.py                  bcrypt passwords, JWT access/refresh tokens
    idbz.py                  the package format (vendored from iotdash/tools/idbz.py)
    images.py                the board's IMG1 icon -> PNG
    http_response.py, exceptions.py, config.py, util.py
  services/
    auth/                    register, login, refresh, me
    projects/                projects and versions (upload, complete, publish, delete)
    store/                   the public catalog boards read, install links
local_server.py              Flask runner (run_local.sh)
tests/                       pytest (moto: in-memory DynamoDB and S3)
layers/iotstore.txt          the Lambda layer (PyJWT, bcrypt)
```

Deploying is `iotdash store deploy` (`../scripts/deploy.sh`, see `../README.md`): tables, bucket, role, layer, function.

## Tables (DynamoDB, on-demand)

Every table: partition key `pk` (S), sort key `sk` (S). Items also keep their ids (`user_id`,
`project_id`, `version_id`) as plain attributes; the answers and the indexes use those.

| table | `pk` | `sk` | indexes (projection ALL) |
|---|---|---|---|
| `<prefix>users` | `USER#<user_id>` | `PROFILE` | `email-index`: `email` (S) |
| `<prefix>projects` | `PROJECT#<project_id>` | `META` | `app_id-index`: `app_id` (S); `owner-index`: `owner_id` (S) + `created_date` (S) |
| `<prefix>versions` | `PROJECT#<project_id>` | `VERSION#<version_id>` | — |

`TABLE_PREFIX` is `<STORE_NAME>-` when deployed by `scripts/deploy.sh` (default `iotstore-`). Files:
bucket `S3_BUCKET_NAME`, keys `projects/<app_id>/<version_id>/package.idbz`, `icon.img`, `icon.png`.

## Settings (environment)

See `.env.example` and `IOTStore/libs/config.py`. On the Lambda (`scripts/deploy.sh` sets them from
`deploy.env`): `JWT_SECRET_KEY`, `STORAGE=s3`, `S3_BUCKET_NAME`, `TABLE_PREFIX`, `CORS_ORIGIN` (the web
app's origins, comma-separated), `STORE_NAME` (shown by the web app), `SIGNUP` (`open` or `closed`:
only the first account, the admin), `ENV`, `BUILD_COMMIT`. The function's role needs DynamoDB
Get/Put/Update/Delete/Query/Scan on the three tables (+ indexes) and S3 Get/Put/Delete on `projects/*`.

## Run it here

```sh
cp .env.example .env.local   # then edit; run_local.sh refuses to start without it
./run_local.sh               # http://<LAN address>:9310
```

The runner keeps DynamoDB in-process (moto) and saves it to `.data/dynamo.json` between runs
(`DYNAMODB_ENDPOINT_URL`, e.g. DynamoDB Local, replaces that). Files:

- `STORAGE=s3` + `AWS_PROFILE`: the real bucket, exactly as on the Lambda. Uploads are presigned
  POSTs the browser (or `iotdash publish`) sends straight to S3; an install hands the board a
  presigned GET and the board downloads the package from S3 itself. Only S3 calls leave the
  machine (moto lets them through with the real credentials). The bucket needs CORS for the web
  app's origin (below).
- `STORAGE=local`: no AWS at all. Files in `.data/storage`, served by the runner with signed URLs
  of the same shape; `PUBLIC_BASE_URL=auto` puts this machine's LAN address in them for boards.

Walkthrough (zsh):

```sh
B=http://127.0.0.1:9310
TOK=$(curl -s -X POST $B/api/auth/register -H 'Content-Type: application/json' \
  -d '{"email":"me@example.com","password":"password123","name":"Me"}' | jq -r .data.access_token)
PID=$(curl -s -X POST $B/api/projects -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  -d '{"app_id":"calculator","name":"Calculator"}' | jq -r .data.id)
F=~/my-apps/calculator/build/calculator-1.0.idbz   # made by `iotdash pack` in that app's folder
UP=$(curl -s -X POST $B/api/projects/$PID/versions/upload -H "Authorization: Bearer $TOK" \
  -H 'Content-Type: application/json' -d "{\"size\":$(stat -f%z $F)}")
args=(); for k in $(echo $UP | jq -r '.data.upload.fields | keys[]'); do
  args+=(-F "$k=$(echo $UP | jq -r .data.upload.fields.$k)"); done
curl -s "${args[@]}" -F file=@$F $(echo $UP | jq -r .data.upload.url)
curl -s -X POST $B/api/projects/$PID/versions/$(echo $UP | jq -r .data.version_id)/complete \
  -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' -d '{"description":"first"}'
curl -s "$B/api/store/apps?os=0.1.0"
curl -s -X POST $B/api/store/apps/calculator/install -H 'Content-Type: application/json' -d '{"os":"0.1.0"}'
```

`iotdash publish --store URL` (in an app project) does the upload in one command.

## Tests

```sh
iotdash store test          # or: .venv/bin/python -m pytest -q tests
```

They drive `main()` with API Gateway v2 events against moto's DynamoDB and S3: accounts,
project ownership, the full upload with a real package (the SDK's calculator example, fixture), refused packages
(other app, duplicate version, broken checksum, too large, not uploaded), publishing, the catalog's
OS filter (newest version that runs on the board's OS), install counts and links, PNG icons, S3 mode.

## Deploy

`iotdash store deploy` (first install and every release; bump `VERSION` in `IOTStore/libs/config.py`
so answers show which code runs). See `../README.md`.
