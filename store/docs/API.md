# iotstore API (v1)

One AWS Lambda behind an HTTP API (API Gateway payload v2, or a Lambda function URL).
Everything is under `/api`. Bodies are JSON. Every answer is wrapped:

```json
{"success": true, "data": { ... }}
{"success": false, "message": "...", "error_code": "...", "errors": { ... }}
```

Auth: `Authorization: Bearer <access_token>` (JWT HS256, `JWT_SECRET_KEY`). Token types: `auth` (1 day),
`refresh` (7 days). The `/api/store/*` routes are public: boards read the catalog without an account.

## Objects

**user**: `{id, email, name, role: "developer"|"admin", created_date}`

**project** (one app of the store):
`{id, app_id, name, description, visibility: "public"|"private", owner: {id, name},
  icon_png_url, latest_version: version|null, versions_count, installs, created_date, updated_date}`

- `app_id` is the app's id on the board (`/applications/<app_id>`, the `id` in the package manifest):
  `^[a-z0-9][a-z0-9-]{1,39}$`, unique, fixed after creation.

**version** (one uploaded package):
`{id, project_id, version, title, description, status: "uploading"|"ready"|"failed", published,
  os_built, os_min, size, sha256, manifest, error, installs, uploaded_by: {id, name},
  created_date, published_date}`

- `version` comes from the package (`manifest.version`), unique per project.
- `os_built`: the iotdash OS version the tools that built it had; `os_min`: oldest OS it runs on.
- `created_date` is when it was uploaded (ISO 8601, UTC).

## Health

- `GET /api/health` → `{status: "ok", version, commit, name, signup: "open"|"closed"}` (the store's name and
  whether anyone may create an account; the web app reads it before sign-in)

## Auth (developers, web)

- `POST /api/auth/register` `{email, password, name}` → `{user, access_token, refresh_token}`
  (the first account becomes `admin`; with `SIGNUP=closed` every later one gets 403 `SIGNUP_CLOSED`)
- `POST /api/auth/login` `{email, password}` → `{user, access_token, refresh_token}`
- `POST /api/auth/refresh` `{refresh_token}` → `{access_token, refresh_token}`
- `GET /api/auth/me` → `{user}`

## Projects (signed in; a developer sees their own, an admin sees all)

- `GET /api/projects` → `{items: [project]}`
- `POST /api/projects` `{app_id, name, description?, visibility?}` → `project`
- `GET /api/projects/{project_id}` → `project` + `versions: [version]` (newest first)
- `PATCH /api/projects/{project_id}` `{name?, description?, visibility?}` → `project`
- `DELETE /api/projects/{project_id}` → `{deleted: true}` (soft delete: leaves the store)

## Versions

Upload is two steps, the file goes straight to storage (S3 presigned POST, or the local runner):

1. `POST /api/projects/{project_id}/versions/upload` `{filename, size}`
   → `{version_id, upload: {url, method: "POST", fields: {...}}, expires_in}`
   The browser posts a multipart form: every `fields` entry, then `file` (the .idbz) last.
2. `POST /api/projects/{project_id}/versions/{version_id}/complete` `{description?, published?}`
   → `version` with `status: "ready"`. The server reads the package (tools/idbz.py format), checks the
   checksums, that `manifest.id == app_id` and that the version is new, stores its icon
   (`icon.img` for boards, `icon.png` for the web) and fills `version`, `title`, `os_built`, `os_min`.
   Refusals (the version becomes `failed` with `error`, the uploaded object is deleted):
   422 `BAD_PACKAGE` (not an IDBZ, a checksum mismatch), 422 `WRONG_APP` (`manifest.id` is another
   app), 409 `VERSION_EXISTS` (that version is already ready in the project), 400 `NOT_UPLOADED`.

- `GET /api/projects/{project_id}/versions` → `{items: [version]}`
- `GET /api/projects/{project_id}/versions/{version_id}` → `version` + `package_url` (presigned GET, for
  the web simulator) + `icon_png_url`
- `PATCH /api/projects/{project_id}/versions/{version_id}` `{description?, published?}` → `version`
- `DELETE /api/projects/{project_id}/versions/{version_id}` → `{deleted: true}`

## Store (boards; public)

- `GET /api/store/apps?os=0.1.0` → `{os, items: [store_app]}`
  `store_app = {app_id, name, description, icon_img_url, icon_png_url, installs,
                latest: {version_id, version, description, size, sha256, os_min, os_built, created_date}}`
  `latest` = the newest published, ready version whose `os_min <= os` (versions compared numerically).
  Apps with no such version are left out. Icon URLs are presigned (1 hour).
- `GET /api/store/apps/{app_id}?os=0.1.0` → `store_app` + `versions: [...]` (published, newest first)
- `POST /api/store/apps/{app_id}/install` `{version_id?, os?, device?}`
  → `{app_id, version_id, version, title, size, sha256, url, expires_in}`; `url` is a presigned GET of
  the package. Counts an install. Default version: `latest` for `os`.

## Tables (DynamoDB, on-demand)

Every table has only partition key `pk` (S) and sort key `sk` (S): no secondary indexes. A lookup
by another value reads a pointer item, written in the same transaction as its record (with
`attribute_not_exists`, so emails and live app ids stay unique). Records also keep their ids
(`user_id`, `project_id`, `version_id`) as plain attributes for the answers.

| table | `pk` | `sk` | item |
|---|---|---|---|
| `<prefix>users` | `USER#<user_id>` | `PROFILE` | the account |
| | `EMAIL#<email>` | `USER` | → `user_id` (sign-in) |
| `<prefix>projects` | `PROJECT#<project_id>` | `META` | the project |
| | `APP#<app_id>` | `PROJECT` | → `project_id` (boards; removed when the project is deleted) |
| | `OWNER#<user_id>` | `PROJECT#<created>#<project_id>` | → `project_id` (a developer's projects, by age) |
| `<prefix>versions` | `PROJECT#<project_id>` | `VERSION#<version_id>` | one uploaded package |

Ids are time-ordered strings (13 digits of milliseconds + 10 hex), so `sk` order of a project's versions
is upload order. `TABLE_PREFIX` (default `iotstore-`; `<STORE_NAME>-` from `scripts/deploy.sh`) or `TABLE_USERS` / `TABLE_PROJECTS` / `TABLE_VERSIONS`
rename them. Deletes are soft (`deleted_date`); install counters are atomic `ADD installs 1`.

## Storage

- `STORAGE=s3`: bucket `S3_BUCKET_NAME` (required; region `AWS_REGION`), keys
  `projects/<app_id>/<version_id>/package.idbz`, `.../icon.img`, `.../icon.png`.
  The bucket needs CORS for the web app (POST from the frontend origin, GET for the simulator).
  Presigned URLs use the bucket's regional host, `https://<bucket>.s3.<region>.amazonaws.com/` (the
  global host answers new buckets outside us-east-1 with a redirect). Upload: a presigned POST
  (15 min, `Content-Type: application/octet-stream`, 1 B–8 MB). Download: a presigned GET (1 hour);
  boards fetch it over HTTPS themselves (the API never streams packages).
- `STORAGE=local` (run_local.sh): files under `LOCAL_STORAGE_DIR`, URLs on the local runner
  (`/_storage/...`, HMAC-signed, same form fields as S3), `PUBLIC_BASE_URL` is the address boards use.
