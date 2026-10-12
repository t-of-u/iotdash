"""
Projects (one app of the store) and their versions (one uploaded .idbz package each).

    iotstore-projects  PROJECT#<id>/META records; APP#<app_id> pointers (unique app ids), OWNER#<user> pointers
                       (a developer's projects); see libs/tables.py
    iotstore-versions  pk PROJECT#<id>, sk VERSION#<version_id> (time-ordered: a reverse Query lists newest first)

A developer sees and changes their own projects, an admin every project. Deletes are soft
(deleted_date): a deleted project leaves the store, its app id can be taken again.
"""

import hashlib
import re

from boto3.dynamodb.conditions import Attr

from ...libs import idbz, images, storage, tables
from ...libs.exceptions import BadRequest, Conflict, NotFound, Validation
from ...libs.util import text

APP_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{1,39}$")
VISIBILITIES = ("public", "private")


def _alive(item):
    return item and not item.get("deleted_date")


def version_view(v: dict, with_urls=False) -> dict:
    out = {
        "id": v["version_id"], "project_id": v["project_id"], "version": v.get("version"), "title": v.get("title"),
        "description": v.get("description", ""), "status": v.get("status"), "published": bool(v.get("published")),
        "os_built": v.get("os_built"), "os_min": v.get("os_min"), "size": v.get("size"), "sha256": v.get("sha256"),
        "manifest": v.get("manifest"), "error": v.get("error"), "installs": v.get("installs", 0),
        "filename": v.get("filename"), "uploaded_by": v.get("uploaded_by"), "created_date": v.get("created_date"),
        "published_date": v.get("published_date"),
    }
    if with_urls and v.get("status") == "ready":
        out["package_url"] = storage.presign_get(v["package_key"])
        out["icon_png_url"] = storage.presign_get(v["icon_png_key"]) if v.get("icon_png_key") else None
    return out


class ProjectsService:
    def __init__(self, user: dict):
        self.user = user
        self.projects = tables.table("projects")
        self.versions = tables.table("versions")

    # ---------------------------------------------------------------- access

    def _is_admin(self):
        return self.user.get("role") == "admin"

    def _load(self, project_id: str) -> dict:
        p = self.projects.get_item(Key=tables.project_key(project_id)).get("Item")
        if not _alive(p) or (not self._is_admin() and p.get("owner_id") != self.user["user_id"]):
            raise NotFound("no such project", "NO_PROJECT")
        return p

    def _version(self, project_id: str, version_id: str) -> dict:
        v = self.versions.get_item(Key=tables.version_key(project_id, version_id)).get("Item")
        if not _alive(v):
            raise NotFound("no such version", "NO_VERSION")
        return v

    def _all_versions(self, project_id: str):
        items = tables.query_all(self.versions, KeyConditionExpression=tables.versions_of(project_id),
                                 ScanIndexForward=False)
        return [v for v in items if _alive(v)]

    def _view(self, p: dict, versions=None) -> dict:
        versions = self._all_versions(p["project_id"]) if versions is None else versions
        ready = [v for v in versions if v.get("status") == "ready"]
        latest = next((v for v in ready if v.get("published")), None)
        icon_from = latest or (ready[0] if ready else None)
        return {
            "id": p["project_id"], "app_id": p["app_id"], "name": p.get("name"), "description": p.get("description", ""),
            "visibility": p.get("visibility", "public"), "owner": {"id": p.get("owner_id"), "name": p.get("owner_name")},
            "icon_png_url": storage.presign_get(icon_from["icon_png_key"]) if icon_from and icon_from.get("icon_png_key") else None,
            "latest_version": version_view(latest) if latest else None, "versions_count": len(ready),
            "installs": p.get("installs", 0), "created_date": p.get("created_date"), "updated_date": p.get("updated_date"),
        }

    # ---------------------------------------------------------------- projects

    def list(self) -> dict:
        if self._is_admin():
            items = tables.scan_all(self.projects, FilterExpression=tables.records("projects") & Attr("deleted_date").not_exists())
        else:
            ptrs = tables.query_all(self.projects, KeyConditionExpression=tables.projects_of(self.user["user_id"]))
            items = [p for p in tables.get_many("projects", [tables.project_key(x["project_id"]) for x in ptrs]) if _alive(p)]
        items.sort(key=lambda p: p.get("created_date", ""), reverse=True)
        return {"items": [self._view(p) for p in items]}

    def create(self, body: dict) -> dict:
        app_id = text(body, "app_id", required=True).lower()
        if not APP_ID_RE.match(app_id):
            raise BadRequest("app_id: 2-40 of a-z, 0-9 and '-', starting with a letter or digit", "BAD_FIELD",
                             {"app_id": "pattern"})
        name = text(body, "name", required=True, max_len=80)
        visibility = text(body, "visibility", default="public")
        if visibility not in VISIBILITIES:
            raise BadRequest("visibility is public or private", "BAD_FIELD", {"visibility": "public|private"})
        now = tables.now()
        p = {"project_id": tables.new_id(), "app_id": app_id, "name": name,
             "description": text(body, "description", max_len=4000, default=""), "visibility": visibility,
             "owner_id": self.user["user_id"], "owner_name": self.user.get("name", ""), "installs": 0,
             "created_date": now, "updated_date": now}
        pid, owner = p["project_id"], p["owner_id"]
        try:  # the project, its app id (one live project each) and its place in the owner's list, together
            tables.transact(tables.put("projects", tables.keyed("projects", p), new=True),
                            tables.put("projects", {**tables.app_key(app_id), "project_id": pid}, new=True),
                            tables.put("projects", {**tables.owner_key(owner, now, pid), "project_id": pid}))
        except tables.Taken:
            raise Conflict(f"the app id '{app_id}' is taken", "APP_ID_TAKEN") from None
        return self._view(p, [])

    def get(self, project_id: str) -> dict:
        p = self._load(project_id)
        versions = self._all_versions(project_id)
        return {**self._view(p, versions), "versions": [version_view(v) for v in versions]}

    def update(self, project_id: str, body: dict) -> dict:
        p = self._load(project_id)
        sets = {}
        if "name" in body:
            sets["name"] = text(body, "name", required=True, max_len=80)
        if "description" in body:
            sets["description"] = text(body, "description", max_len=4000, default="")
        if "visibility" in body:
            v = text(body, "visibility", required=True)
            if v not in VISIBILITIES:
                raise BadRequest("visibility is public or private", "BAD_FIELD", {"visibility": "public|private"})
            sets["visibility"] = v
        if sets:
            sets["updated_date"] = tables.now()
            self._set(self.projects, tables.project_key(project_id), sets)
            p.update(sets)
        return self._view(p)

    def delete(self, project_id: str) -> dict:
        p = self._load(project_id)
        # soft delete; the app id is free again and the project leaves its owner's list
        tables.transact(tables.update("projects", tables.project_key(project_id), {"deleted_date": tables.now()}),
                        tables.delete("projects", tables.app_key(p["app_id"]), only_if={"project_id": project_id}),
                        tables.delete("projects", tables.owner_key(p["owner_id"], p["created_date"], project_id)))
        return {"deleted": True}

    # ---------------------------------------------------------------- versions

    def list_versions(self, project_id: str) -> dict:
        self._load(project_id)
        return {"items": [version_view(v) for v in self._all_versions(project_id)]}

    def get_version(self, project_id: str, version_id: str) -> dict:
        self._load(project_id)
        return version_view(self._version(project_id, version_id), with_urls=True)

    def start_upload(self, project_id: str, body: dict) -> dict:
        p = self._load(project_id)
        size = body.get("size")
        if not isinstance(size, int) or size <= 0:
            raise BadRequest("size (bytes) is required", "BAD_FIELD", {"size": "positive integer"})
        if size > storage.MAX_PACKAGE:
            raise BadRequest(f"a package is at most {storage.MAX_PACKAGE // (1024 * 1024)} MB", "TOO_LARGE")
        filename = text(body, "filename", max_len=200, default="package.idbz")
        version_id = tables.new_id()
        key = storage.package_key(p["app_id"], version_id)
        self.versions.put_item(Item=tables.keyed("versions", {
            "project_id": project_id, "version_id": version_id, "status": "uploading", "published": False,
            "filename": filename, "upload_size": size, "package_key": key, "installs": 0,
            "uploaded_by": {"id": self.user["user_id"], "name": self.user.get("name", "")},
            "created_date": tables.now()}))
        return {"version_id": version_id, "upload": storage.presign_upload(key), "expires_in": storage.UPLOAD_EXPIRES}

    def complete(self, project_id: str, version_id: str, body: dict) -> dict:
        p = self._load(project_id)
        v = self._version(project_id, version_id)
        if v.get("status") != "uploading":
            raise Conflict(f"this version is already {v.get('status')}", "NOT_UPLOADING")
        try:
            blob = storage.get(v["package_key"])
        except storage.StorageMissing:
            raise BadRequest("the file has not been uploaded yet", "NOT_UPLOADED")
        try:
            manifest = idbz.validate(blob)
            _, files = idbz.unpack(blob, verify=False)
        except idbz.PackError as e:
            self._fail(v, str(e))
            raise Validation(f"not a valid app package: {e}", "BAD_PACKAGE")
        if manifest["id"] != p["app_id"]:
            msg = f"the package is the app '{manifest['id']}', this project is '{p['app_id']}'"
            self._fail(v, msg)
            raise Validation(msg, "WRONG_APP")
        if any(o.get("status") == "ready" and o.get("version") == manifest["version"]
               for o in self._all_versions(project_id) if o["version_id"] != version_id):
            msg = f"version {manifest['version']} is already in this project: raise the version in app.json"
            self._fail(v, msg)
            raise Conflict(msg, "VERSION_EXISTS")
        icon_img = storage.sibling(v["package_key"], "icon.img")
        storage.put(icon_img, files["icon.img"])
        icon_png = None
        try:
            icon_png = storage.sibling(v["package_key"], "icon.png")
            storage.put(icon_png, images.img1_to_png(files["icon.img"]), "image/png")
        except images.ImageError:
            icon_png = None
        published = body.get("published", True) is not False
        now = tables.now()
        os_info = manifest.get("os") or {}
        sets = {"status": "ready", "version": manifest["version"], "title": manifest.get("title", p["name"]),
                "description": text(body, "description", max_len=4000, default="") or manifest.get("description", ""),
                "os_built": os_info.get("built", ""), "os_min": os_info.get("min", os_info.get("built", "")),
                "size": len(blob), "sha256": hashlib.sha256(blob).hexdigest(), "manifest": manifest,
                "icon_img_key": icon_img, "icon_png_key": icon_png, "published": published,
                "published_date": now if published else None, "completed_date": now}
        self._set(self.versions, tables.version_key(project_id, version_id), sets, remove=["error"])
        self._set(self.projects, tables.project_key(project_id), {"updated_date": now})
        v.update(sets)
        return version_view(v, with_urls=True)

    def update_version(self, project_id: str, version_id: str, body: dict) -> dict:
        self._load(project_id)
        v = self._version(project_id, version_id)
        sets = {}
        if "description" in body:
            sets["description"] = text(body, "description", max_len=4000, default="")
        if "published" in body:
            if v.get("status") != "ready" and body["published"]:
                raise Conflict("only a ready version can be published", "NOT_READY")
            sets["published"] = bool(body["published"])
            if sets["published"] and not v.get("published"):
                sets["published_date"] = tables.now()
        if sets:
            self._set(self.versions, tables.version_key(project_id, version_id), sets)
            v.update(sets)
        return version_view(v, with_urls=True)

    def delete_version(self, project_id: str, version_id: str) -> dict:
        self._load(project_id)
        self._version(project_id, version_id)
        self._set(self.versions, tables.version_key(project_id, version_id), {"deleted_date": tables.now()})
        return {"deleted": True}

    # ---------------------------------------------------------------- helpers

    def _fail(self, v: dict, message: str):
        self._set(self.versions, tables.version_key(v["project_id"], v["version_id"]),
                  {"status": "failed", "error": message})
        storage.delete(v["package_key"])

    @staticmethod
    def _set(tbl, key: dict, values: dict, remove=()):
        names, vals, parts = {}, {}, []
        for i, (k, val) in enumerate(values.items()):
            names[f"#k{i}"] = k
            vals[f":v{i}"] = val
            parts.append(f"#k{i} = :v{i}")
        expr = "SET " + ", ".join(parts)
        if remove:
            for j, k in enumerate(remove):
                names[f"#r{j}"] = k
            expr += " REMOVE " + ", ".join(f"#r{j}" for j in range(len(remove)))
        tbl.update_item(Key=key, UpdateExpression=expr, ExpressionAttributeNames=names, ExpressionAttributeValues=vals)
