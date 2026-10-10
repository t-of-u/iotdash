"""
Developer accounts: bcrypt passwords and JWT (HS256, JWT_SECRET_KEY).

    access   {user_id, email, role, type: "auth"}     1 day
    refresh  {user_id, type: "refresh"}              7 days
"""

import os
import time

import bcrypt
import jwt

from .exceptions import Unauthorized

ACCESS_TTL = 24 * 3600
REFRESH_TTL = 7 * 24 * 3600


def _secret() -> str:
    s = os.environ.get("JWT_SECRET_KEY")
    if not s:
        raise RuntimeError("JWT_SECRET_KEY is not set")
    return s


def hash_password(password: str) -> str:
    return bcrypt.hashpw(password.encode(), bcrypt.gensalt()).decode()


def check_password(password: str, hashed: str) -> bool:
    try:
        return bcrypt.checkpw(password.encode(), hashed.encode())
    except ValueError:
        return False


def tokens(user: dict) -> dict:
    now = int(time.time())
    access = jwt.encode({"user_id": user["user_id"], "email": user["email"], "role": user.get("role", "developer"),
                         "type": "auth", "iat": now, "exp": now + ACCESS_TTL}, _secret(), algorithm="HS256")
    refresh = jwt.encode({"user_id": user["user_id"], "type": "refresh", "iat": now, "exp": now + REFRESH_TTL},
                         _secret(), algorithm="HS256")
    return {"access_token": access, "refresh_token": refresh}


def decode(token: str, kind: str = "auth") -> dict:
    try:
        claims = jwt.decode(token, _secret(), algorithms=["HS256"])
    except jwt.ExpiredSignatureError:
        raise Unauthorized("Token expired", "TOKEN_EXPIRED")
    except jwt.InvalidTokenError:
        raise Unauthorized("Invalid token", "INVALID_TOKEN")
    if claims.get("type") != kind:
        raise Unauthorized("Wrong token type", "INVALID_TOKEN")
    return claims


def bearer(headers: dict) -> str:
    h = {k.lower(): v for k, v in (headers or {}).items()}
    value = h.get("authorization", "")
    if not value.lower().startswith("bearer "):
        raise Unauthorized("Missing bearer token", "NO_TOKEN")
    return value[7:].strip()
