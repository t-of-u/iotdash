"""
API Gateway (HTTP API, payload v2) responses: JSON with the v3 wrapping, CORS on every answer.
"""

import json
import os
from datetime import date, datetime
from decimal import Decimal

from .config import VERSION, BUILD_COMMIT


class DateTimeEncoder(json.JSONEncoder):
    def default(self, o):
        if isinstance(o, datetime):
            return o.isoformat().replace("+00:00", "Z")
        if isinstance(o, date):
            return o.isoformat()
        if isinstance(o, Decimal):  # DynamoDB numbers
            return int(o) if o == o.to_integral_value() else float(o)
        if isinstance(o, (set, frozenset)):
            return sorted(o)
        return super().default(o)


_origin = ""  # the Origin header of the request being answered (set by the entry point)


def set_request_origin(origin) -> None:
    global _origin
    _origin = origin or ""


def allowed_origin() -> str:
    """CORS_ORIGIN: "*" or origins separated by commas; a listed request origin is echoed back."""
    allowed = [o.strip().rstrip("/") for o in os.environ.get("CORS_ORIGIN", "*").split(",") if o.strip()]
    if not allowed or "*" in allowed:
        return "*"
    return _origin if _origin.rstrip("/") in allowed else allowed[0]


def cors_headers() -> dict:
    origin = allowed_origin()
    return {
        "Access-Control-Allow-Origin": origin,
        **({} if origin == "*" else {"Vary": "Origin"}),
        "Access-Control-Allow-Methods": "GET, POST, PUT, PATCH, DELETE, OPTIONS",
        "Access-Control-Allow-Headers": "Content-Type, Authorization",
        "Access-Control-Expose-Headers": "X-App-Version, X-App-Build-Commit",
        "X-App-Version": VERSION,
        "X-App-Build-Commit": BUILD_COMMIT,
    }


def respond(status: int, payload) -> dict:
    headers = {"Content-Type": "application/json", **cors_headers()}
    return {"statusCode": status, "headers": headers,
            "body": json.dumps(payload, cls=DateTimeEncoder, ensure_ascii=False)}


def ok(data) -> dict:
    return respond(200, {"success": True, "data": data})


def error(status: int, message: str, error_code=None, errors=None, extra=None) -> dict:
    body = {"success": False, "message": message, "error_code": error_code, "errors": errors}
    if extra:
        body.update(extra)
    return respond(status, body)
