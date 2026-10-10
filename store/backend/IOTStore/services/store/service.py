"""
The store as boards see it (public, no account): public projects with a published, ready version
that runs on the board's OS, and the download link of a package.
"""

from boto3.dynamodb.conditions import Attr, Key

from ...libs import storage, tables
from ...libs.exceptions import NotFound
from ...libs.util import text, version_le


def _alive(item):
    return item and not item.get("deleted_date")


class StoreService:
    def __init__(self):
        self.projects = tables.table("projects")
        self.versions = tables.table("versions")

    def _published(self, project_id: str):
        items = tables.query_all(self.versions, KeyConditionExpression=tables.versions_of(project_id),
                                 ScanIndexForward=False)
        return [v for v in items if _alive(v) and v.get("status") == "ready" and v.get("published")]

    @staticmethod
    def _fits(v, os_version):
        return not os_version or version_le(v.get("os_min") or "0", os_version)

    def _project(self, app_id: str) -> dict:
        items = tables.query_all(self.projects, IndexName="app_id-index", KeyConditionExpression=Key("app_id").eq(app_id))
        p = next((p for p in items if _alive(p) and p.get("visibility", "public") == "public"), None)
        if not p:
            raise NotFound(f"no app '{app_id}' in the store", "NO_APP")
        return p

    @staticmethod
    def _version_out(v: dict) -> dict:
        return {"version_id": v["version_id"], "version": v.get("version"), "title": v.get("title"),
                "description": v.get("description", ""), "size": v.get("size"), "sha256": v.get("sha256"),
                "os_min": v.get("os_min"), "os_built": v.get("os_built"), "installs": v.get("installs", 0),
                "created_date": v.get("created_date")}

    def _app_out(self, p: dict, latest: dict) -> dict:
        return {"app_id": p["app_id"], "name": p.get("name"), "description": p.get("description", ""),
                "owner": p.get("owner_name"), "installs": p.get("installs", 0),
                "icon_img_url": storage.presign_get(latest["icon_img_key"]) if latest.get("icon_img_key") else None,
                "icon_png_url": storage.presign_get(latest["icon_png_key"]) if latest.get("icon_png_key") else None,
                "latest": self._version_out(latest)}

    def catalog(self, query: dict) -> dict:
        os_version = (query or {}).get("os", "")
        projects = tables.scan_all(self.projects, FilterExpression=Attr("deleted_date").not_exists())
        items = []
        for p in projects:
            if p.get("visibility", "public") != "public":
                continue
            latest = next((v for v in self._published(p["project_id"]) if self._fits(v, os_version)), None)
            if latest:
                items.append(self._app_out(p, latest))
        items.sort(key=lambda a: (a["name"] or a["app_id"]).lower())
        return {"os": os_version, "items": items}

    def app(self, app_id: str, query: dict) -> dict:
        os_version = (query or {}).get("os", "")
        p = self._project(app_id)
        versions = self._published(p["project_id"])
        latest = next((v for v in versions if self._fits(v, os_version)), None)
        if not latest:
            raise NotFound(f"no version of '{app_id}' runs on OS {os_version}", "NO_COMPATIBLE_VERSION")
        out = self._app_out(p, latest)
        out["versions"] = [{**self._version_out(v), "compatible": self._fits(v, os_version)} for v in versions]
        return out

    def install(self, app_id: str, body: dict) -> dict:
        p = self._project(app_id)
        os_version = text(body, "os", default="")
        version_id = text(body, "version_id", default="")
        versions = self._published(p["project_id"])
        if version_id:
            v = next((v for v in versions if v["version_id"] == version_id), None)
        else:
            v = next((v for v in versions if self._fits(v, os_version)), None)
        if not v:
            raise NotFound("no such version to install", "NO_VERSION")
        add = {"UpdateExpression": "ADD installs :one", "ExpressionAttributeValues": {":one": 1}}
        self.versions.update_item(Key=tables.version_key(v["project_id"], v["version_id"]), **add)
        self.projects.update_item(Key=tables.project_key(p["project_id"]), **add)
        return {"app_id": app_id, "version_id": v["version_id"], "version": v.get("version"), "title": v.get("title"),
                "size": v.get("size"), "sha256": v.get("sha256"), "url": storage.presign_get(v["package_key"]),
                "expires_in": storage.GET_EXPIRES}
