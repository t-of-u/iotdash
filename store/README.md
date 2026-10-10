# iotstore (iotdash/store)

The iotdash SDK's app store, which anyone can host: developers upload their apps' packages and
keep every version, try each version on a simulated board in the browser, and boards list, install,
update and remove apps from their **Apps** app. The store is a framework; the apps in it belong to
whoever publishes them.

```
store/
  backend/            the API: AWS Lambda (Python 3.13) + DynamoDB + S3           backend/README.md
  frontend/           the web app: React + Vite + TypeScript (static files)       frontend/README.md
  docs/API.md         the HTTP API (web app, boards, the iotdash CLI)
  scripts/deploy.sh   your store on AWS: makes or updates every resource from deploy.env
  scripts/deploy-web.sh   builds the web app for your store (and copies it to S3)
  deploy.env.example  a store's settings
```

How the pieces meet:

- **Uploads**: the API hands the browser (or `iotdash publish`) a presigned S3 POST; the file goes
  straight to S3, then the API checks the package (checksums, app id, new version) and records it.
- **Installs**: a board asks the API for an app, gets a presigned S3 GET and downloads the package
  from S3 itself, writing it to its card as it arrives (iotdash `lib/rt/os_store.cpp`).
- **Simulator**: the web app runs any version on the iotdash OS compiled to WebAssembly (from the
  iotdash SDK's `web/`), with a simulated Wi-Fi network, Bluetooth devices and a weighing scale.

## Host your own store

Needs an AWS account and the AWS CLI v2, Python 3.13 (to build the Lambda's dependency layer),
Node 20+, and this SDK's browser build (`web/build.sh`, for the simulator's engine).

```sh
cp store/deploy.env.example store/deploy.env   # STORE_NAME, AWS_PROFILE, AWS_REGION, WEB_ORIGIN, SIGNUP, ...
iotdash store deploy --dry-run        # what it would make (reads your account, changes nothing)
iotdash store deploy                  # the API; prints its URL          (= store/scripts/deploy.sh)
iotdash store deploy-web              # the web app in frontend/dist (uploaded when WEB_BUCKET is set)
```

`scripts/deploy.sh` is safe to run again: it makes what is missing and updates the rest (new code,
settings, CORS), keeping the token signing key. What it makes, all named after `STORE_NAME`:

| | |
|---|---|
| DynamoDB | `<name>-users`, `<name>-projects`, `<name>-versions` (on-demand; keys `pk` + `sk`, see `docs/API.md`) |
| S3 | `S3_BUCKET_NAME` (default `<name>-packages-<account id>`): private, CORS for `WEB_ORIGIN` |
| IAM | `<name>-api-role`: logs, those three tables, that bucket |
| Lambda | `<name>-api` + layer `<name>-deps`, a public function URL (the API) |

Then:

1. Serve `frontend/dist` from any static host. Unknown paths must fall back to `index.html` (S3
   website: error document `index.html`; CloudFront: 403/404 → `/index.html`) and `.wasm` must be
   `application/wasm`. Put its origin in `WEB_ORIGIN` and run `scripts/deploy.sh` again.
2. Open the web app and create the first account: it becomes the store's admin. With `SIGNUP=closed`
   nobody else can sign up.
3. Boards: **Apps** → *Store address* → the API's URL.

Several stores in one account (staging and production, say): one config file each, with
different `STORE_NAME`s: `iotdash store deploy --config staging.env`.

## Work on it locally

```sh
cp store/backend/.env.example store/backend/.env.local   # once: JWT_SECRET_KEY, STORAGE, ...
iotdash store dev          # the API on :9310 (DynamoDB in-process) + the web app on :5310 (--api / --web: one)
iotdash store test         # the API's tests
```

Files go to a real S3 bucket (`STORAGE=s3` + `AWS_PROFILE`, as on Lambda) or stay on this machine
(`STORAGE=local`, no AWS at all). A board on the same network: **Apps** → *Store address* →
`http://<this computer's IP>:9310`.

## Publish an app

```sh
cd my-app                     # an iotdash app project (iotdash create my-app)
iotdash pack                  # build/my-app-1.0.idbz: upload it on the project page, or
IOTSTORE_URL=https://<your API> IOTSTORE_EMAIL=me@example.com iotdash publish --notes "What changed"
```

Boards are offered the newest published version whose `os.min` (from the app's `app.json`) is not
newer than their OS.
