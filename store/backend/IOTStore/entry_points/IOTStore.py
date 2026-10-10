"""
The iotstore Lambda: IOTStore.entry_points.IOTStore.main

API Gateway HTTP API (payload v2) or a Lambda function URL. Routes (docs/API.md):

    /api/health
    /api/auth/*      developer accounts         (register/login/refresh public, me signed in)
    /api/projects/*  projects and versions      (signed in)
    /api/store/*     the catalog boards read    (public)

Answers are {"success": true, "data": ...} or {"success": false, "message", "error_code", "errors"}.
"""

import base64
import json
import logging
import os
import traceback

from ..libs import http_response
from ..libs.config import store_info, version_payload
from ..libs.exceptions import APIException
from ..services.auth import handle_auth_routes
from ..services.auth.service import AuthService
from ..services.projects import handle_project_routes
from ..services.store import handle_store_routes

logger = logging.getLogger(__name__)

PUBLIC = {("POST", "/api/auth/register"), ("POST", "/api/auth/login"), ("POST", "/api/auth/refresh"),
          ("GET", "/api/health")}


def _path(event) -> str:
    path = event.get("rawPath") or event.get("path") or "/"
    i = path.find("/api/")
    if i > 0:  # "/{stage}/api/..."
        path = path[i:]
    return path.rstrip("/") or "/"


def _body(event):
    raw = event.get("body")
    if not raw:
        return {}
    if event.get("isBase64Encoded"):
        raw = base64.b64decode(raw).decode("utf-8")
    return json.loads(raw)


def main(event, context):
    method = ((event.get("requestContext") or {}).get("http") or {}).get("method") or event.get("httpMethod", "GET")
    method = method.upper()
    http_response.set_request_origin(next((v for k, v in (event.get("headers") or {}).items() if k.lower() == "origin"), ""))
    if method == "OPTIONS":
        return {"statusCode": 200, "headers": http_response.cors_headers(), "body": ""}
    path = _path(event)
    headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
    try:
        try:
            body = _body(event)
        except (ValueError, UnicodeDecodeError):
            return http_response.error(400, "Invalid JSON in request body", "BAD_JSON")
        common = {"method": method, "query_params": event.get("queryStringParameters") or {},
                  "path_params": event.get("pathParameters") or {}, "headers": headers, "current_user": None}
        if not ((method, path) in PUBLIC or path.startswith("/api/store/")):
            common["current_user"] = AuthService().current(headers)
        parts = [p for p in path.split("/") if p][1:]  # after "api"
        if not parts or path[:5] != "/api/":
            raise _not_found(method, path)
        area, rest = parts[0], parts[1:]
        if area == "health" and not rest:
            result = {"status": "ok", **version_payload(), **store_info()}
        elif area == "auth":
            result = handle_auth_routes(rest, method, body, common)
        elif area == "projects":
            result = handle_project_routes(rest, method, body, common)
        elif area == "store":
            result = handle_store_routes(rest, method, body, common)
        else:
            raise _not_found(method, path)
        if isinstance(result, dict) and "statusCode" in result and "body" in result:
            result.setdefault("headers", {}).update(http_response.cors_headers())
            return result
        return http_response.ok(result)
    except APIException as e:
        return http_response.error(e.status_code, e.message, e.error_code, e.errors)
    except Exception as e:  # noqa: BLE001 - every failure becomes a JSON 500
        logger.exception("unhandled error on %s %s", method, path)
        extra = None if os.environ.get("ENV") == "PROD" else {"stack_trace": traceback.format_exc()}
        return http_response.error(500, f"Internal server error: {e}", "INTERNAL_ERROR", extra=extra)


def _not_found(method, path):
    from ..libs.exceptions import NotFound

    return NotFound(f"no route {method} {path}")
