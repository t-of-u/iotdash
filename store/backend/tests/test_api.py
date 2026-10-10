import hashlib
import json
import struct
from urllib.parse import parse_qs, unquote, urlparse

from conftest import register, upload


def test_health_and_cors(api):
    assert api.ok("GET", "/api/health")["status"] == "ok"
    status, out, headers = api("OPTIONS", "/api/projects")
    assert status == 200 and headers["Access-Control-Allow-Origin"] == "*"
    status, out, headers = api("GET", "/api/health")
    assert "X-App-Version" in headers and "X-App-Version" in headers["Access-Control-Expose-Headers"]
    # a stage prefix is ignored
    assert api.ok("GET", "/dev/api/health")["status"] == "ok"


def test_self_hosting_settings(api, monkeypatch):
    # several web origins: the request's is echoed when it is listed, else the first
    monkeypatch.setenv("CORS_ORIGIN", "https://store.example.com, http://localhost:5310")
    _, _, h = api("OPTIONS", "/api/projects", origin="http://localhost:5310")
    assert h["Access-Control-Allow-Origin"] == "http://localhost:5310" and h["Vary"] == "Origin"
    _, _, h = api("GET", "/api/health", origin="https://evil.example")
    assert h["Access-Control-Allow-Origin"] == "https://store.example.com"
    # the store's name and whether anyone may sign up
    monkeypatch.setenv("STORE_NAME", "Tofu Apps")
    monkeypatch.setenv("SIGNUP", "closed")
    assert api.ok("GET", "/api/health")["name"] == "Tofu Apps" and api.ok("GET", "/api/health")["signup"] == "closed"
    _, admin = register(api, "owner@x.io")  # the first account still can: it becomes the admin
    assert admin["role"] == "admin"
    status, out, _ = api("POST", "/api/auth/register", {"email": "b@x.io", "password": "password123"})
    assert status == 403 and out["error_code"] == "SIGNUP_CLOSED"


def test_errors_are_wrapped(api):
    status, out, _ = api("GET", "/api/projects")
    assert status == 401 and out["success"] is False and out["error_code"] == "NO_TOKEN"
    tok, _ = register(api, "x@example.com")
    status, out, _ = api("GET", "/api/nothing", token=tok)
    assert status == 404 and out["success"] is False
    status, out, _ = api("POST", "/api/auth/login", raw_body="{nope")
    assert status == 400 and out["error_code"] == "BAD_JSON"


def test_accounts(api):
    tok, user = register(api, "Ann@Example.com", "Ann")
    assert user["role"] == "admin" and user["email"] == "ann@example.com"
    _, bob = register(api, "bob@example.com", "Bob")
    assert bob["role"] == "developer"
    status, out, _ = api("POST", "/api/auth/register", {"email": "bob@example.com", "password": "password123"})
    assert status == 409
    status, out, _ = api("POST", "/api/auth/register", {"email": "c@example.com", "password": "short"})
    assert status == 400
    status, out, _ = api("POST", "/api/auth/login", {"email": "bob@example.com", "password": "wrong-password"})
    assert status == 401 and out["error_code"] == "BAD_CREDENTIALS"
    login = api.ok("POST", "/api/auth/login", {"email": "bob@example.com", "password": "password123"})
    assert api.ok("GET", "/api/auth/me", token=login["access_token"])["user"]["name"] == "Bob"
    fresh = api.ok("POST", "/api/auth/refresh", {"refresh_token": login["refresh_token"]})
    assert api.ok("GET", "/api/auth/me", token=fresh["access_token"])["user"]["email"] == "bob@example.com"
    status, out, _ = api("GET", "/api/auth/me", token=login["refresh_token"])
    assert status == 401  # a refresh token is not an access token


def test_projects_and_ownership(api):
    admin, _ = register(api, "admin@x.io")
    dev_a, _ = register(api, "a@x.io", "A")
    dev_b, _ = register(api, "b@x.io", "B")
    p = api.ok("POST", "/api/projects", {"app_id": "calculator", "name": "Calculator"}, token=dev_a)
    assert p["app_id"] == "calculator" and p["visibility"] == "public" and p["latest_version"] is None
    status, out, _ = api("POST", "/api/projects", {"app_id": "Bad_ID", "name": "x"}, token=dev_a)
    assert status == 400
    status, out, _ = api("POST", "/api/projects", {"app_id": "calculator", "name": "again"}, token=dev_b)
    assert status == 409 and out["error_code"] == "APP_ID_TAKEN"
    assert [x["id"] for x in api.ok("GET", "/api/projects", token=dev_a)["items"]] == [p["id"]]
    assert api.ok("GET", "/api/projects", token=dev_b)["items"] == []
    assert len(api.ok("GET", "/api/projects", token=admin)["items"]) == 1
    status, _, _ = api("GET", f"/api/projects/{p['id']}", token=dev_b)
    assert status == 404
    assert api.ok("GET", f"/api/projects/{p['id']}", token=admin)["name"] == "Calculator"
    up = api.ok("PATCH", f"/api/projects/{p['id']}", {"name": "Calculator 2", "description": "sums",
                                                        "visibility": "private"}, token=dev_a)
    assert up["name"] == "Calculator 2" and up["visibility"] == "private"
    assert api.ok("DELETE", f"/api/projects/{p['id']}", token=dev_a) == {"deleted": True}
    assert api.ok("GET", "/api/projects", token=dev_a)["items"] == []
    # the app id is free again
    api.ok("POST", "/api/projects", {"app_id": "calculator", "name": "again"}, token=dev_b)


def test_upload_publish_install(api, package):
    tok, _ = register(api, "dev@x.io")
    p = api.ok("POST", "/api/projects", {"app_id": "calculator", "name": "Calculator"}, token=tok)
    vid, (status, out, _) = upload(api, p["id"], package, token=tok, description="first release")
    assert status == 200, out
    v = out["data"]
    assert v["status"] == "ready" and v["version"] == "1.0" and v["title"] == "Calculator" and v["published"]
    assert v["os_built"] == "0.1.0" and v["os_min"] == "0.1.0" and v["size"] == len(package)
    assert v["sha256"] == hashlib.sha256(package).hexdigest() and v["description"] == "first release"
    assert v["package_url"].startswith("http://store.test/_storage/get/") and v["icon_png_url"]

    # the PNG icon made from icon.img
    from IOTStore.libs import storage

    q = parse_qs(urlparse(v["icon_png_url"]).query)
    key = unquote(urlparse(v["icon_png_url"]).path.split("/_storage/get/", 1)[1])
    png = storage.local_open_download(key, q["expires"][0], q["signature"][0]).read_bytes()
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    w, h = struct.unpack(">II", png[16:24])
    assert w > 0 and h > 0

    proj = api.ok("GET", f"/api/projects/{p['id']}", token=tok)
    assert proj["latest_version"]["id"] == vid and proj["versions_count"] == 1 and proj["icon_png_url"]

    # the store as a board sees it
    cat = api.ok("GET", "/api/store/apps", query={"os": "0.1.0"}, token="")
    assert [a["app_id"] for a in cat["items"]] == ["calculator"]
    app = cat["items"][0]
    assert app["latest"]["version"] == "1.0" and app["icon_img_url"] and app["icon_png_url"]
    assert api.ok("GET", "/api/store/apps", query={"os": "0.0.9"}, token="")["items"] == []
    detail = api.ok("GET", "/api/store/apps/calculator", query={"os": "0.1.0"}, token="")
    assert detail["versions"][0]["compatible"] is True

    inst = api.ok("POST", "/api/store/apps/calculator/install", {"os": "0.1.0", "device": "sim"}, token="")
    assert inst["version"] == "1.0" and inst["sha256"] == v["sha256"]
    u = urlparse(inst["url"])
    q = parse_qs(u.query)
    got = storage.local_open_download(unquote(u.path.split("/_storage/get/", 1)[1]), q["expires"][0], q["signature"][0])
    assert hashlib.sha256(got.read_bytes()).hexdigest() == v["sha256"]
    assert api.ok("GET", f"/api/projects/{p['id']}", token=tok)["installs"] == 1
    assert api.ok("GET", f"/api/projects/{p['id']}/versions/{vid}", token=tok)["installs"] == 1

    # unpublish: out of the store; private projects never show
    api.ok("PATCH", f"/api/projects/{p['id']}/versions/{vid}", {"published": False}, token=tok)
    assert api.ok("GET", "/api/store/apps", token="")["items"] == []
    api.ok("PATCH", f"/api/projects/{p['id']}/versions/{vid}", {"published": True, "description": "edited"}, token=tok)
    assert api.ok("GET", "/api/store/apps", token="")["items"][0]["latest"]["description"] == "edited"
    api.ok("PATCH", f"/api/projects/{p['id']}", {"visibility": "private"}, token=tok)
    assert api.ok("GET", "/api/store/apps", token="")["items"] == []
    status, _, _ = api("POST", "/api/store/apps/calculator/install", {}, token="")
    assert status == 404


def test_bad_packages(api, package):
    tok, _ = register(api, "dev@x.io")
    other = api.ok("POST", "/api/projects", {"app_id": "other-app", "name": "Other"}, token=tok)
    vid, (status, out, _) = upload(api, other["id"], package, token=tok)
    assert status == 422 and out["error_code"] == "WRONG_APP"
    failed = api.ok("GET", f"/api/projects/{other['id']}/versions/{vid}", token=tok)
    assert failed["status"] == "failed" and "calculator" in failed["error"]

    p = api.ok("POST", "/api/projects", {"app_id": "calculator", "name": "Calculator"}, token=tok)
    broken = bytearray(package)
    broken[-100] ^= 0xFF  # inside app.idb: its checksum no longer matches
    _, (status, out, _) = upload(api, p["id"], bytes(broken), token=tok)
    assert status == 422 and out["error_code"] == "BAD_PACKAGE" and "checksum" in out["message"]
    _, (status, out, _) = upload(api, p["id"], b"not a package at all", token=tok)
    assert status == 422
    _, (status, out, _) = upload(api, p["id"], package, token=tok)
    assert status == 200
    _, (status, out, _) = upload(api, p["id"], package, token=tok)
    assert status == 409 and out["error_code"] == "VERSION_EXISTS"

    status, out, _ = api("POST", f"/api/projects/{p['id']}/versions/upload", {"size": 50 * 1024 * 1024}, token=tok)
    assert status == 400 and out["error_code"] == "TOO_LARGE"
    start = api.ok("POST", f"/api/projects/{p['id']}/versions/upload", {"size": 100}, token=tok)
    status, out, _ = api("POST", f"/api/projects/{p['id']}/versions/{start['version_id']}/complete", {}, token=tok)
    assert status == 400 and out["error_code"] == "NOT_UPLOADED"
    # failed / pending versions are not in the store and not counted
    proj = api.ok("GET", f"/api/projects/{p['id']}", token=tok)
    assert proj["versions_count"] == 1
    assert {v["status"] for v in proj["versions"]} == {"ready", "failed", "uploading"}


def test_os_compatibility_picks_newest_that_fits(api, package, tmp_path):
    """Two versions: the newer needs OS 0.2.0, a 0.1.0 board gets the older one."""
    import sys

    sys.path.insert(0, str(tmp_path))
    from IOTStore.libs import idbz

    tok, _ = register(api, "dev@x.io")
    p = api.ok("POST", "/api/projects", {"app_id": "calculator", "name": "Calculator"}, token=tok)
    upload(api, p["id"], package, token=tok)
    manifest, files = idbz.unpack(package)
    app = json.loads(files["app.json"])
    app["version"] = manifest["version"] = "1.1"
    manifest["os"] = {"built": "0.2.0", "min": "0.2.0"}
    files["app.json"] = json.dumps(app).encode()
    newer = idbz.pack({k: v for k, v in manifest.items() if k not in ("format", "files")}, files)
    _, (status, out, _) = upload(api, p["id"], newer, token=tok)
    assert status == 200 and out["data"]["os_min"] == "0.2.0"
    assert api.ok("GET", "/api/store/apps", query={"os": "0.1.0"}, token="")["items"][0]["latest"]["version"] == "1.0"
    assert api.ok("GET", "/api/store/apps", query={"os": "0.2.0"}, token="")["items"][0]["latest"]["version"] == "1.1"
    assert api.ok("GET", "/api/store/apps", query={"os": "0.10.0"}, token="")["items"][0]["latest"]["version"] == "1.1"
    inst = api.ok("POST", "/api/store/apps/calculator/install", {"os": "0.1.0"}, token="")
    assert inst["version"] == "1.0"


def test_s3_storage(api, package, monkeypatch):
    """STORAGE=s3 against moto's S3: presigned POST form, the object, presigned GET."""
    import boto3
    from IOTStore.libs import storage

    monkeypatch.setenv("STORAGE", "s3")
    monkeypatch.setenv("S3_BUCKET_NAME", "iotstore-packages")
    storage.reset()
    boto3.client("s3", region_name="ap-southeast-1").create_bucket(
        Bucket="iotstore-packages", CreateBucketConfiguration={"LocationConstraint": "ap-southeast-1"})
    tok, _ = register(api, "dev@x.io")
    p = api.ok("POST", "/api/projects", {"app_id": "calculator", "name": "Calculator"}, token=tok)
    start = api.ok("POST", f"/api/projects/{p['id']}/versions/upload", {"size": len(package)}, token=tok)
    fields = start["upload"]["fields"]
    assert "policy" in {k.lower() for k in fields} and fields["key"].endswith("/package.idbz")
    storage.put(fields["key"], package)  # what the browser's POST does
    v = api.ok("POST", f"/api/projects/{p['id']}/versions/{start['version_id']}/complete", {}, token=tok)
    assert v["status"] == "ready" and "iotstore-packages" in v["package_url"] and "X-Amz-Signature" in v["package_url"]
    s3 = boto3.client("s3", region_name="ap-southeast-1")
    keys = {o["Key"] for o in s3.list_objects_v2(Bucket="iotstore-packages")["Contents"]}
    assert {k.rsplit("/", 1)[1] for k in keys} == {"package.idbz", "icon.img", "icon.png"}


def test_items_are_keyed_pk_sk(api, package):
    from IOTStore.libs import tables

    tok, user = register(api, "keys@x.io")
    p = api.ok("POST", "/api/projects", {"app_id": "calculator", "name": "Calculator"}, token=tok)
    vid, _ = upload(api, p["id"], package, token=tok)
    for kind in tables.KINDS:
        schema = tables.resource().meta.client.describe_table(TableName=tables.name(kind))["Table"]["KeySchema"]
        assert [(k["AttributeName"], k["KeyType"]) for k in schema] == [("pk", "HASH"), ("sk", "RANGE")]
    assert tables.table("users").get_item(Key={"pk": f"USER#{user['id']}", "sk": "PROFILE"})["Item"]["email"] == "keys@x.io"
    assert tables.table("projects").get_item(Key={"pk": f"PROJECT#{p['id']}", "sk": "META"})["Item"]["app_id"] == "calculator"
    v = tables.table("versions").get_item(Key={"pk": f"PROJECT#{p['id']}", "sk": f"VERSION#{vid}"})["Item"]
    assert v["status"] == "ready" and v["version_id"] == vid
    # items saved before pk/sk get their keys from their ids
    assert tables.keyed("versions", {"project_id": "a", "version_id": "b"})["sk"] == "VERSION#b"
