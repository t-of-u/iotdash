"""
The iotstore Lambda on this machine (run_local.sh): Flask on 0.0.0.0:$PORT (default 9310) turns
requests into API Gateway v2 events for IOTStore.entry_points.IOTStore.main, like the v3 runner.

    DynamoDB  in-process (moto), kept in .data/dynamo.json between runs; or a real / DynamoDB Local
              endpoint when DYNAMODB_ENDPOINT_URL is set (tables must exist there)
    files     STORAGE=s3: the real bucket S3_BUCKET_NAME, as on the Lambda: uploads are presigned POSTs
              straight to S3, installs presigned GETs the board downloads from S3. Needs real AWS
              credentials (AWS_PROFILE); only DynamoDB is simulated, S3 calls go out to AWS.
              STORAGE=local: LOCAL_STORAGE_DIR, served by /_storage/upload and /_storage/get/<key>
              (the signed URLs the API hands out point here, at PUBLIC_BASE_URL)
"""

import atexit
import base64
import json
import os
import threading
from decimal import Decimal
from pathlib import Path

from flask import Flask, Response, request, send_file

from IOTStore.entry_points.IOTStore import main
from IOTStore.libs import http_response, storage, tables

app = Flask(__name__)
DUMP = Path(os.environ.get("LOCAL_DATA_DIR", ".data")) / "dynamo.json"
_lock = threading.Lock()
_mock = None


def _start_dynamo():
    global _mock
    if os.environ.get("DYNAMODB_ENDPOINT_URL"):
        return
    from moto import mock_aws

    if storage.backend() == "s3":
        # DynamoDB in-process, S3 for real: moto lets S3 calls through and keeps the real credentials
        # (presigned URLs signed with moto's fake keys would be refused by S3)
        _mock = mock_aws(config={"core": {"mock_credentials": False, "passthrough": {"services": ["s3"]}}})
    else:
        _mock = mock_aws()
    _mock.start()
    tables.reset()
    tables.create_all()
    if DUMP.exists():
        data = json.loads(DUMP.read_text(), parse_float=Decimal)
        for kind, items in data.items():
            items = [tables.keyed(kind, it) for it in items]  # items saved before pk/sk get keys
            have = {(it["pk"], it["sk"]) for it in items}
            for it in list(items):  # and records saved before pointers get theirs
                if it["sk"] == tables.RECORD_SK.get(kind):
                    items += [ptr for ptr in tables.pointers(kind, it) if (ptr["pk"], ptr["sk"]) not in have]
            with tables.table(kind).batch_writer(overwrite_by_pkeys=["pk", "sk"]) as w:
                for it in items:
                    w.put_item(Item=it)
        print(f"loaded {sum(len(v) for v in data.values())} items from {DUMP}")
    atexit.register(_save)


def _save():
    if _mock is None:
        return
    with _lock:
        data = {k: tables.scan_all(tables.table(k)) for k in tables.KINDS}
        DUMP.parent.mkdir(parents=True, exist_ok=True)
        tmp = DUMP.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, cls=http_response.DateTimeEncoder, ensure_ascii=False))
        tmp.replace(DUMP)


def _cors(resp: Response) -> Response:
    for k, v in http_response.cors_headers().items():
        resp.headers[k] = v
    return resp


@app.route("/api", defaults={"rest": ""}, methods=["GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"])
@app.route("/api/<path:rest>", methods=["GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"])
def api(rest):
    body = request.get_data()
    event = {
        "version": "2.0", "rawPath": "/api/" + rest, "rawQueryString": request.query_string.decode(),
        "requestContext": {"http": {"method": request.method, "sourceIp": request.remote_addr}},
        "headers": {k.lower(): v for k, v in request.headers.items()},
        "queryStringParameters": request.args.to_dict() or None,
        "body": base64.b64encode(body).decode() if body else None, "isBase64Encoded": bool(body),
    }
    r = main(event, None)
    resp = Response(r.get("body", ""), status=r["statusCode"], headers=r.get("headers") or {})
    if request.method in ("POST", "PUT", "PATCH", "DELETE"):
        _save()
    return resp


@app.route("/_storage/upload", methods=["POST", "OPTIONS"])
def storage_upload():
    if request.method == "OPTIONS":
        return _cors(Response(status=200))
    f = request.files.get("file")
    if not f:
        return _cors(Response("no file", status=400))
    try:
        storage.local_accept_upload(request.form.to_dict(), f.read())
    except PermissionError as e:
        return _cors(Response(str(e), status=403))
    except ValueError as e:
        return _cors(Response(str(e), status=400))
    return _cors(Response(status=204))  # like S3


@app.route("/_storage/get/<path:key>", methods=["GET", "HEAD", "OPTIONS"])
def storage_get(key):
    if request.method == "OPTIONS":
        return _cors(Response(status=200))
    try:
        path = storage.local_open_download(key, request.args.get("expires"), request.args.get("signature"))
    except PermissionError as e:
        return _cors(Response(str(e), status=403))
    except (FileNotFoundError, ValueError):
        return _cors(Response("not found", status=404))
    mime = "image/png" if key.endswith(".png") else "application/octet-stream"
    return _cors(send_file(path, mimetype=mime, conditional=False))


@app.route("/")
def index():
    return {"service": "iotstore", "api": "/api/health"}


if __name__ == "__main__":
    os.environ.setdefault("STORAGE", "local")
    _start_dynamo()
    port = int(os.environ.get("PORT", "9310"))
    where = f"s3://{storage.bucket()} ({os.environ.get('AWS_PROFILE') or 'default credentials'})" if storage.backend() == "s3" \
        else f"local, public URL {os.environ.get('PUBLIC_BASE_URL')}"
    print(f"iotstore on http://0.0.0.0:{port}, storage {where}")
    app.run(host="0.0.0.0", port=port, threaded=True)
