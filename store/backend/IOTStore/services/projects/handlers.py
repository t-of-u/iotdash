"""/api/projects/*"""

from ...libs.exceptions import NotFound
from .service import ProjectsService


def handle_project_routes(parts, method, body, common):
    svc = ProjectsService(common["current_user"])
    n = len(parts)
    if n == 0:
        if method == "GET":
            return svc.list()
        if method == "POST":
            return svc.create(body)
    elif n == 1:
        pid = parts[0]
        if method == "GET":
            return svc.get(pid)
        if method == "PATCH":
            return svc.update(pid, body)
        if method == "DELETE":
            return svc.delete(pid)
    elif parts[1] == "versions":
        pid = parts[0]
        if n == 2 and method == "GET":
            return svc.list_versions(pid)
        if n == 3 and parts[2] == "upload" and method == "POST":
            return svc.start_upload(pid, body)
        if n == 3:
            vid = parts[2]
            if method == "GET":
                return svc.get_version(pid, vid)
            if method == "PATCH":
                return svc.update_version(pid, vid, body)
            if method == "DELETE":
                return svc.delete_version(pid, vid)
        if n == 4 and parts[3] == "complete" and method == "POST":
            return svc.complete(pid, parts[2], body)
    raise NotFound(f"no route {method} /api/projects/{'/'.join(parts)}")
