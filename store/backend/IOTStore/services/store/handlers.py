"""/api/store/* (public: boards read it without an account)"""

from ...libs.exceptions import NotFound
from .service import StoreService


def handle_store_routes(parts, method, body, common):
    svc = StoreService()
    if parts == ["apps"] and method == "GET":
        return svc.catalog(common["query_params"])
    if len(parts) == 2 and parts[0] == "apps" and method == "GET":
        return svc.app(parts[1], common["query_params"])
    if len(parts) == 3 and parts[0] == "apps" and parts[2] == "install" and method == "POST":
        return svc.install(parts[1], body)
    raise NotFound(f"no route {method} /api/store/{'/'.join(parts)}")
