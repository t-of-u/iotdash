"""Publish an app package (.idbz) to an iotstore (store/docs/API.md), as the web site does:
sign in, find or create the app's project, upload the file to the form the store hands out,
then let the store check it.

  iotdash app publish NAME [--pack FILE] [--store URL] [--email E] [--notes TEXT] [--draft]

The store and account come from the flags or IOTSTORE_URL / IOTSTORE_EMAIL / IOTSTORE_PASSWORD
(asked for when missing). Nothing is saved.
"""

import getpass
import json
import os
import urllib.error
import urllib.request
import uuid
from pathlib import Path


class StoreError(Exception):
    pass


class Store:
    def __init__(self, base):
        self.base = base.rstrip("/")
        self.token = None

    def call(self, method, path, body=None):
        req = urllib.request.Request(self.base + path, method=method)
        req.add_header("Accept", "application/json")
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            req.add_header("Content-Type", "application/json")
        if self.token:
            req.add_header("Authorization", "Bearer " + self.token)
        try:
            with urllib.request.urlopen(req, data, timeout=60) as r:
                out = json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            try:
                out = json.loads(e.read() or b"{}")
            except ValueError:
                out = {}
            raise StoreError(f"{method} {path}: {out.get('message') or e.reason} ({e.code})") from None
        except urllib.error.URLError as e:
            raise StoreError(f"cannot reach {self.base}: {e.reason}") from None
        if not out.get("success"):
            raise StoreError(f"{method} {path}: {out.get('message', 'failed')}")
        return out.get("data")

    def login(self, email, password):
        self.token = self.call("POST", "/api/auth/login", {"email": email, "password": password})["access_token"]

    @staticmethod
    def post_form(url, fields, filename, blob):
        """multipart/form-data: the fields, then the file (S3 presigned POST wants it last)."""
        b = "----iotdash" + uuid.uuid4().hex
        parts = []
        for k, v in fields.items():
            parts.append(f'--{b}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode())
        parts.append(f'--{b}\r\nContent-Disposition: form-data; name="file"; filename="{filename}"\r\n'
                     f"Content-Type: application/octet-stream\r\n\r\n".encode() + blob + b"\r\n")
        parts.append(f"--{b}--\r\n".encode())
        req = urllib.request.Request(url, data=b"".join(parts), method="POST")
        req.add_header("Content-Type", "multipart/form-data; boundary=" + b)
        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                return r.status
        except urllib.error.HTTPError as e:
            raise StoreError(f"upload refused ({e.code}): {e.read()[:300].decode(errors='replace')}") from None


def publish(pack_path, store=None, email=None, notes="", draft=False, log=print):
    import idbz

    blob = Path(pack_path).read_bytes()
    manifest, _ = idbz.unpack(blob)
    app_id, version = manifest["id"], manifest["version"]
    store = store or os.environ.get("IOTSTORE_URL")
    if not store:
        raise StoreError("which store? --store URL or IOTSTORE_URL")
    email = email or os.environ.get("IOTSTORE_EMAIL") or input("store e-mail: ")
    password = os.environ.get("IOTSTORE_PASSWORD") or getpass.getpass(f"password for {email}: ")

    s = Store(store)
    s.login(email, password)
    project = next((p for p in s.call("GET", "/api/projects")["items"] if p["app_id"] == app_id), None)
    if not project:
        project = s.call("POST", "/api/projects", {"app_id": app_id, "name": manifest.get("title") or app_id,
                                                   "description": manifest.get("description", "")})
        log(f"  new project {project['name']} ({app_id})")
    pid = project["id"]
    start = s.call("POST", f"/api/projects/{pid}/versions/upload",
                   {"filename": Path(pack_path).name, "size": len(blob)})
    s.post_form(start["upload"]["url"], start["upload"]["fields"], Path(pack_path).name, blob)
    v = s.call("POST", f"/api/projects/{pid}/versions/{start['version_id']}/complete",
               {"description": notes or "", "published": not draft})
    log(f"  {app_id} {version} {'(draft) ' if draft else ''}published to {s.base}: "
        f"{v['size'] / 1024:.0f} KB, OS {v.get('os_min')} or newer, version id {v['id']}")
    return v
