"""
Non-secret app-wide configuration.

Bump ``VERSION`` before each deploy: every response carries it as the
``X-App-Version`` header, so a stale Lambda is easy to spot.

Nothing here is secret. Secrets and per-environment settings are read from
``os.environ`` at the place they are used:

    JWT_SECRET_KEY        libs/auth (tokens), libs/storage (local URL signatures)
    AWS_REGION            libs/tables, libs/storage; default ap-southeast-1
    TABLE_PREFIX          libs/tables: table names, default "iotstore-"
    TABLE_USERS / TABLE_PROJECTS / TABLE_VERSIONS   libs/tables: one table's name
    DYNAMODB_ENDPOINT_URL libs/tables: a local / other DynamoDB endpoint
    STORAGE               libs/storage: "s3" (default) or "local"
    S3_BUCKET_NAME        libs/storage (STORAGE=s3): required (scripts/deploy.sh sets it)
    S3_ENDPOINT_URL       libs/storage (STORAGE=s3): a local / other S3 endpoint
    LOCAL_STORAGE_DIR     libs/storage (STORAGE=local)
    PUBLIC_BASE_URL       libs/storage (STORAGE=local): the address boards and browsers use
    CORS_ORIGIN           libs/http_response: "*" (default) or the web app's origins, comma-separated
    STORE_NAME            the store's name (web app title, /api/health), default "iotstore"
    SIGNUP                libs/auth service: "open" (default) or "closed" (only the first account,
                          the admin, can sign up)
    ENV                   entry point: stack traces in 500s unless ENV=PROD
"""

import os

VERSION: str = "1.0.0"

# Usually injected at deploy time; the default makes local runs identifiable.
BUILD_COMMIT: str = os.environ.get("BUILD_COMMIT", "local")


def version_payload() -> dict:
    return {"version": VERSION, "commit": BUILD_COMMIT}


def signup_open() -> bool:
    return os.environ.get("SIGNUP", "open").strip().lower() != "closed"


def store_info() -> dict:
    """What the web app shows before anyone signs in."""
    return {"name": os.environ.get("STORE_NAME", "iotstore"), "signup": "open" if signup_open() else "closed"}
