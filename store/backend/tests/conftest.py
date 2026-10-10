import json
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

FIXTURES = ROOT / "tests" / "fixtures"


@pytest.fixture
def env(tmp_path, monkeypatch):
    """Fresh in-memory AWS (moto): the three tables, local file storage in a temp dir."""
    from moto import mock_aws

    for k, v in {"AWS_ACCESS_KEY_ID": "testing", "AWS_SECRET_ACCESS_KEY": "testing", "AWS_SESSION_TOKEN": "testing",
                 "AWS_REGION": "ap-southeast-1", "AWS_DEFAULT_REGION": "ap-southeast-1",
                 "JWT_SECRET_KEY": "test-secret-0123456789abcdef0123456789", "STORAGE": "local", "LOCAL_STORAGE_DIR": str(tmp_path / "files"),
                 "PUBLIC_BASE_URL": "http://store.test", "ENV": "TEST"}.items():
        monkeypatch.setenv(k, v)
    for k in ("DYNAMODB_ENDPOINT_URL", "S3_ENDPOINT_URL", "TABLE_PREFIX"):
        monkeypatch.delenv(k, raising=False)
    from IOTStore.libs import storage, tables

    with mock_aws():
        tables.reset()
        storage.reset()
        tables.create_all()
        yield tmp_path
        tables.reset()
        storage.reset()


class Api:
    def __init__(self):
        from IOTStore.entry_points.IOTStore import main

        self.main = main
        self.token = None

    def __call__(self, method, path, body=None, token=None, query=None, raw_body=None, origin=None):
        headers = {"content-type": "application/json"}
        if origin:
            headers["origin"] = origin
        tok = token if token is not None else self.token
        if tok:
            headers["authorization"] = "Bearer " + tok
        event = {"version": "2.0", "rawPath": path, "requestContext": {"http": {"method": method}},
                 "headers": headers, "queryStringParameters": query,
                 "body": raw_body if raw_body is not None else (json.dumps(body) if body is not None else None)}
        r = self.main(event, None)
        out = json.loads(r["body"]) if r.get("body") else None
        return r["statusCode"], out, r.get("headers", {})

    def ok(self, method, path, body=None, **kw):
        status, out, _ = self(method, path, body, **kw)
        assert status == 200, (status, out)
        assert out["success"] is True
        return out["data"]


@pytest.fixture
def api(env):
    return Api()


@pytest.fixture
def package():
    return (FIXTURES / "calculator-1.0.idbz").read_bytes()


def register(api, email, name="Dev", password="password123"):
    data = api.ok("POST", "/api/auth/register", {"email": email, "password": password, "name": name})
    return data["access_token"], data["user"]


def upload(api, project_id, blob, token=None, description="first", published=None):
    """The browser's two steps, with the file posted to the local storage form."""
    from IOTStore.libs import storage

    start = api.ok("POST", f"/api/projects/{project_id}/versions/upload", {"filename": "p.idbz", "size": len(blob)},
                   token=token)
    storage.local_accept_upload(start["upload"]["fields"], blob)
    body = {"description": description}
    if published is not None:
        body["published"] = published
    return start["version_id"], api(
        "POST", f"/api/projects/{project_id}/versions/{start['version_id']}/complete", body, token=token)
