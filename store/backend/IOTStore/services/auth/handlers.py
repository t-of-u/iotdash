"""/api/auth/*"""

from ...libs.exceptions import NotFound
from .service import AuthService, public_user


def handle_auth_routes(parts, method, body, common):
    svc = AuthService()
    if method == "POST" and parts == ["register"]:
        return svc.register(body)
    if method == "POST" and parts == ["login"]:
        return svc.login(body)
    if method == "POST" and parts == ["refresh"]:
        return svc.refresh(body)
    if method == "GET" and parts == ["me"]:
        return {"user": public_user(common["current_user"])}
    raise NotFound(f"no route {method} /api/auth/{'/'.join(parts)}")
