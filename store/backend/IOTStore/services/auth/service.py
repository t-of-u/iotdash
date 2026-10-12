"""Developer accounts in iotstore-users (USER# records, EMAIL# pointers for sign-in)."""

import re

from ...libs import auth, tables
from ...libs.config import signup_open
from ...libs.exceptions import BadRequest, Conflict, Forbidden, Unauthorized
from ...libs.util import text

EMAIL_RE = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")


def public_user(u: dict) -> dict:
    return {"id": u["user_id"], "email": u["email"], "name": u.get("name", ""), "role": u.get("role", "developer"),
            "created_date": u.get("created_date")}


class AuthService:
    def __init__(self):
        self.users = tables.table("users")

    def by_email(self, email: str):
        ptr = self.users.get_item(Key=tables.email_key(email)).get("Item")
        return self.get(ptr["user_id"]) if ptr else None

    def get(self, user_id: str):
        return self.users.get_item(Key=tables.user_key(user_id)).get("Item")

    def register(self, body: dict) -> dict:
        email = text(body, "email", required=True, max_len=200).lower()
        password = body.get("password") or ""
        name = text(body, "name", max_len=80, default="") or email.split("@")[0]
        if not EMAIL_RE.match(email):
            raise BadRequest("email is not valid", "BAD_FIELD", {"email": "invalid"})
        if not isinstance(password, str) or len(password) < 8:
            raise BadRequest("password needs at least 8 characters", "BAD_FIELD", {"password": "min 8"})
        first = not self.users.scan(Limit=1, ProjectionExpression="pk").get("Items")
        if not first and not signup_open():
            raise Forbidden("sign-up is closed on this store: ask its admin", "SIGNUP_CLOSED")
        user = {"user_id": tables.new_id(), "email": email, "name": name, "role": "admin" if first else "developer",
                "password_hash": auth.hash_password(password), "created_date": tables.now(), "is_active": True}
        try:  # the account and its email pointer together: an email has one account
            tables.transact(tables.put("users", tables.keyed("users", user), new=True),
                            tables.put("users", {**tables.email_key(email), "user_id": user["user_id"]}, new=True))
        except tables.Taken:
            raise Conflict("an account with this email exists", "EMAIL_TAKEN") from None
        return {"user": public_user(user), **auth.tokens(user)}

    def login(self, body: dict) -> dict:
        email = text(body, "email", required=True).lower()
        user = self.by_email(email)
        if not user or not user.get("is_active", True) or not auth.check_password(body.get("password") or "", user["password_hash"]):
            raise Unauthorized("wrong email or password", "BAD_CREDENTIALS")
        return {"user": public_user(user), **auth.tokens(user)}

    def refresh(self, body: dict) -> dict:
        claims = auth.decode(text(body, "refresh_token", required=True), "refresh")
        user = self.get(claims["user_id"])
        if not user or not user.get("is_active", True):
            raise Unauthorized("account not found", "NO_ACCOUNT")
        return auth.tokens(user)

    def current(self, headers: dict) -> dict:
        """The signed-in user of a request (entry point), from its bearer token."""
        claims = auth.decode(auth.bearer(headers), "auth")
        user = self.get(claims["user_id"])
        if not user or not user.get("is_active", True):
            raise Unauthorized("account not found", "NO_ACCOUNT")
        return user
