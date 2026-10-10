"""Small helpers the services share: version order, request fields."""

import re

from .exceptions import BadRequest


def version_key(v):
    """'0.10.2' > '0.9': dot (or -/+) separated parts, numbers compared as numbers."""
    out = []
    for p in re.split(r"[.\-+]", str(v or "0")):
        out.append((0, int(p), "") if p.isdigit() else (1, 0, p))
    return out


def version_le(a, b) -> bool:
    ka, kb = version_key(a), version_key(b)
    n = max(len(ka), len(kb))
    pad = (0, 0, "")
    return ka + [pad] * (n - len(ka)) <= kb + [pad] * (n - len(kb))


def text(body: dict, key: str, required=False, max_len=None, default=None):
    v = body.get(key, default) if isinstance(body, dict) else default
    if v is None:
        if required:
            raise BadRequest(f"{key} is required", "MISSING_FIELD", {key: "required"})
        return default
    if not isinstance(v, str):
        raise BadRequest(f"{key} must be text", "BAD_FIELD", {key: "text"})
    v = v.strip()
    if required and not v:
        raise BadRequest(f"{key} is required", "MISSING_FIELD", {key: "required"})
    if max_len and len(v) > max_len:
        raise BadRequest(f"{key} is longer than {max_len} characters", "BAD_FIELD", {key: f"max {max_len}"})
    return v
