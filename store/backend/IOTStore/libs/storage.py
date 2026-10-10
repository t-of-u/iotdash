"""
Package and icon files: S3 in production, a local directory when developing.

    STORAGE=s3     bucket S3_BUCKET_NAME (required), region AWS_REGION; uploads are
                   presigned POSTs (the browser sends the file straight to S3), downloads presigned GETs
    STORAGE=local  files under LOCAL_STORAGE_DIR, URLs on the local runner (PUBLIC_BASE_URL/_storage/...)
                   signed with an HMAC of JWT_SECRET_KEY; the upload form has the same shape as S3's

Keys: projects/<app_id>/<version_id>/package.idbz, .../icon.img, .../icon.png
"""

import hashlib
import hmac
import os
import time
from pathlib import Path
from urllib.parse import quote, urlencode

MAX_PACKAGE = 8 * 1024 * 1024
UPLOAD_EXPIRES = 900
GET_EXPIRES = 3600


class StorageMissing(Exception):
    pass


def backend() -> str:
    return os.environ.get("STORAGE", "s3")


def bucket() -> str:
    name = os.environ.get("S3_BUCKET_NAME", "")
    if not name:
        raise RuntimeError("S3_BUCKET_NAME is not set (STORAGE=s3)")
    return name


def package_key(app_id, version_id):
    return f"projects/{app_id}/{version_id}/package.idbz"


def sibling(key, name):
    return key.rsplit("/", 1)[0] + "/" + name


# ---------------------------------------------------------------- S3

_s3 = None


def _client():
    global _s3
    if _s3 is None:
        import boto3
        from botocore.config import Config

        # presigned URLs on the bucket's regional host (https://<bucket>.s3.<region>.amazonaws.com/...):
        # the global host answers a new bucket outside us-east-1 with a 307 redirect, which a
        # browser's form POST (CORS) and the board's download do not follow well
        region = os.environ.get("AWS_REGION", "ap-southeast-1")
        kw = {"region_name": region, "config": Config(signature_version="s3v4", s3={"addressing_style": "virtual"}),
              "endpoint_url": os.environ.get("S3_ENDPOINT_URL") or f"https://s3.{region}.amazonaws.com"}
        _s3 = boto3.client("s3", **kw)
    return _s3


def reset():
    global _s3
    _s3 = None


# ---------------------------------------------------------------- local

def _root() -> Path:
    return Path(os.environ.get("LOCAL_STORAGE_DIR", ".data/storage"))


def _secret() -> bytes:
    return os.environ.get("JWT_SECRET_KEY", "dev-secret").encode()


def _sign(*parts) -> str:
    return hmac.new(_secret(), "\n".join(str(p) for p in parts).encode(), hashlib.sha256).hexdigest()


def _local_path(key: str) -> Path:
    p = (_root() / key).resolve()
    if not str(p).startswith(str(_root().resolve()) + os.sep):
        raise ValueError("bad key")
    return p


def _base() -> str:
    return os.environ.get("PUBLIC_BASE_URL", "http://localhost:9310").rstrip("/")


def local_accept_upload(fields: dict, data: bytes) -> str:
    """The local runner's POST /_storage/upload: checks the signed form, stores the file."""
    key, expires, max_size = fields.get("key", ""), fields.get("expires", "0"), fields.get("max", "0")
    if not hmac.compare_digest(_sign("put", key, expires, max_size), fields.get("signature", "")):
        raise PermissionError("bad signature")
    if int(expires) < time.time():
        raise PermissionError("upload form expired")
    if not 0 < len(data) <= int(max_size):
        raise ValueError(f"size {len(data)} not in 1..{max_size}")
    p = _local_path(key)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    return key


def local_open_download(key: str, expires: str, signature: str) -> Path:
    """The local runner's GET /_storage/get/<key>: the file of a valid signed URL."""
    if not hmac.compare_digest(_sign("get", key, expires), signature or ""):
        raise PermissionError("bad signature")
    if int(expires or 0) < time.time():
        raise PermissionError("link expired")
    p = _local_path(key)
    if not p.exists():
        raise FileNotFoundError(key)
    return p


# ---------------------------------------------------------------- the operations services use

def presign_upload(key: str, max_size: int = MAX_PACKAGE) -> dict:
    """{url, method, fields}: the browser posts every field, then `file` last."""
    if backend() == "local":
        expires = str(int(time.time()) + UPLOAD_EXPIRES)
        return {"url": _base() + "/_storage/upload", "method": "POST",
                "fields": {"key": key, "expires": expires, "max": str(max_size),
                           "signature": _sign("put", key, expires, max_size)}}
    ct = "application/octet-stream"
    post = _client().generate_presigned_post(
        Bucket=bucket(), Key=key, Fields={"Content-Type": ct},
        Conditions=[{"Content-Type": ct}, ["content-length-range", 1, max_size]], ExpiresIn=UPLOAD_EXPIRES)
    return {"url": post["url"], "method": "POST", "fields": post["fields"]}


def presign_get(key: str, expires_in: int = GET_EXPIRES) -> str:
    if backend() == "local":
        expires = str(int(time.time()) + expires_in)
        return f"{_base()}/_storage/get/{quote(key)}?" + urlencode({"expires": expires, "signature": _sign("get", key, expires)})
    return _client().generate_presigned_url("get_object", Params={"Bucket": bucket(), "Key": key}, ExpiresIn=expires_in)


def get(key: str) -> bytes:
    if backend() == "local":
        p = _local_path(key)
        if not p.exists():
            raise StorageMissing(key)
        return p.read_bytes()
    from botocore.exceptions import ClientError

    try:
        return _client().get_object(Bucket=bucket(), Key=key)["Body"].read()
    except ClientError as e:
        if e.response.get("Error", {}).get("Code") in ("NoSuchKey", "404"):
            raise StorageMissing(key)
        raise


def put(key: str, data: bytes, content_type: str = "application/octet-stream"):
    if backend() == "local":
        p = _local_path(key)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return
    _client().put_object(Bucket=bucket(), Key=key, Body=data, ContentType=content_type)


def delete(key: str):
    if not key:
        return
    if backend() == "local":
        p = _local_path(key)
        if p.exists():
            p.unlink()
        return
    _client().delete_object(Bucket=bucket(), Key=key)
