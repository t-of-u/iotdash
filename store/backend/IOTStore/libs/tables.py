"""
The store's DynamoDB tables: names, key schemas (scripts/infra/create_tables.sh creates the same),
the keys of each item, one cached boto3 resource, ids and timestamps.

Every table is keyed by pk (partition, S) + sk (sort, S). Items keep their ids as plain attributes
too (user_id, project_id, version_id): the answers and the indexes use those.

    iotstore-users      pk USER#<user_id>        sk PROFILE                GSI email-index (email)
    iotstore-projects   pk PROJECT#<project_id>  sk META                   GSI app_id-index (app_id),
                                                                           GSI owner-index (owner_id, created_date)
    iotstore-versions   pk PROJECT#<project_id>  sk VERSION#<version_id>   version_id is time-ordered: newest last

All on-demand (PAY_PER_REQUEST). Names: TABLE_PREFIX ("iotstore-") + kind, or TABLE_USERS /
TABLE_PROJECTS / TABLE_VERSIONS for one table. DYNAMODB_ENDPOINT_URL points at a local endpoint.
"""

import os
import secrets
import time
from datetime import datetime, timezone

import boto3
from boto3.dynamodb.conditions import Key

KINDS = ("users", "projects", "versions")


def name(kind: str) -> str:
    return os.environ.get(f"TABLE_{kind.upper()}") or os.environ.get("TABLE_PREFIX", "iotstore-") + kind


def _s(attr):
    return {"AttributeName": attr, "AttributeType": "S"}


def _key(hash_key, range_key=None):
    k = [{"AttributeName": hash_key, "KeyType": "HASH"}]
    if range_key:
        k.append({"AttributeName": range_key, "KeyType": "RANGE"})
    return k


def _gsi(index, hash_key, range_key=None):
    return {"IndexName": index, "KeySchema": _key(hash_key, range_key), "Projection": {"ProjectionType": "ALL"}}


SCHEMAS = {
    "users": {
        "KeySchema": _key("pk", "sk"),
        "AttributeDefinitions": [_s("pk"), _s("sk"), _s("email")],
        "GlobalSecondaryIndexes": [_gsi("email-index", "email")],
    },
    "projects": {
        "KeySchema": _key("pk", "sk"),
        "AttributeDefinitions": [_s("pk"), _s("sk"), _s("app_id"), _s("owner_id"), _s("created_date")],
        "GlobalSecondaryIndexes": [_gsi("app_id-index", "app_id"), _gsi("owner-index", "owner_id", "created_date")],
    },
    "versions": {
        "KeySchema": _key("pk", "sk"),
        "AttributeDefinitions": [_s("pk"), _s("sk")],
    },
}


# ---- the keys of an item
def user_key(user_id: str) -> dict:
    return {"pk": f"USER#{user_id}", "sk": "PROFILE"}


def project_key(project_id: str) -> dict:
    return {"pk": f"PROJECT#{project_id}", "sk": "META"}


def version_key(project_id: str, version_id: str) -> dict:
    return {"pk": f"PROJECT#{project_id}", "sk": f"VERSION#{version_id}"}


def versions_of(project_id: str):
    """Query condition: every version of a project (sk order = upload order)."""
    return Key("pk").eq(f"PROJECT#{project_id}") & Key("sk").begins_with("VERSION#")


def keyed(kind: str, item: dict) -> dict:
    """The item with its pk/sk, from its ids (also brings items saved without keys up to date)."""
    if kind == "users":
        key = user_key(item["user_id"])
    elif kind == "projects":
        key = project_key(item["project_id"])
    else:
        key = version_key(item["project_id"], item["version_id"])
    return {**item, **key}

_resource = None


def resource():
    global _resource
    if _resource is None:
        kw = {"region_name": os.environ.get("AWS_REGION", "ap-southeast-1")}
        if os.environ.get("DYNAMODB_ENDPOINT_URL"):
            kw["endpoint_url"] = os.environ["DYNAMODB_ENDPOINT_URL"]
        _resource = boto3.resource("dynamodb", **kw)
    return _resource


def table(kind: str):
    return resource().Table(name(kind))


def reset():
    """Forget the cached resource (tests swap the AWS mock between cases)."""
    global _resource
    _resource = None


def create_all():
    """Create the three tables (local runner, tests). The real ones come from create_tables.sh."""
    for kind in KINDS:
        resource().create_table(TableName=name(kind), BillingMode="PAY_PER_REQUEST", **SCHEMAS[kind])


def new_id() -> str:
    """Time-ordered: 13 digits of milliseconds then 10 hex digits, so string order is age order."""
    return f"{int(time.time() * 1000):013d}{secrets.token_hex(5)}"


def now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def query_all(tbl, **kw):
    """Every item of a Query (follows LastEvaluatedKey)."""
    items = []
    while True:
        r = tbl.query(**kw)
        items += r.get("Items", [])
        if "LastEvaluatedKey" not in r:
            return items
        kw["ExclusiveStartKey"] = r["LastEvaluatedKey"]


def scan_all(tbl, **kw):
    items = []
    while True:
        r = tbl.scan(**kw)
        items += r.get("Items", [])
        if "LastEvaluatedKey" not in r:
            return items
        kw["ExclusiveStartKey"] = r["LastEvaluatedKey"]
