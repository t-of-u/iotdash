"""
The store's DynamoDB tables: names, key schemas (scripts/deploy.sh creates the same), the keys of
each item, writes in one transaction, one cached boto3 resource, ids and timestamps.

Every table has only pk (partition, S) + sk (sort, S): no secondary indexes. Lookups by another
value go through pointer items written in the same transaction as the record they point at (their
condition "attribute_not_exists" also makes the value unique). Records keep their ids as plain
attributes (user_id, project_id, version_id) for the answers.

    users     USER#<user_id>        PROFILE                         the account
              EMAIL#<email>         USER                            -> user_id (sign-in; one account per email)
    projects  PROJECT#<project_id>  META                            the project
              APP#<app_id>          PROJECT                         -> project_id (boards; one live project per app id)
              OWNER#<owner_id>      PROJECT#<created>#<project_id>  -> project_id (a developer's projects, by age)
    versions  PROJECT#<project_id>  VERSION#<version_id>            version_id is time-ordered: newest last

All on-demand (PAY_PER_REQUEST). Names: TABLE_PREFIX ("iotstore-") + kind, or TABLE_USERS /
TABLE_PROJECTS / TABLE_VERSIONS for one table. DYNAMODB_ENDPOINT_URL points at a local endpoint.
"""

import os
import secrets
import time
from datetime import datetime, timezone

import boto3
from boto3.dynamodb.conditions import Attr, Key

KINDS = ("users", "projects", "versions")
RECORD_SK = {"users": "PROFILE", "projects": "META"}  # sk of the records (the rest are pointers or versions)


def name(kind: str) -> str:
    return os.environ.get(f"TABLE_{kind.upper()}") or os.environ.get("TABLE_PREFIX", "iotstore-") + kind


SCHEMA = {
    "KeySchema": [{"AttributeName": "pk", "KeyType": "HASH"}, {"AttributeName": "sk", "KeyType": "RANGE"}],
    "AttributeDefinitions": [{"AttributeName": "pk", "AttributeType": "S"}, {"AttributeName": "sk", "AttributeType": "S"}],
}


# ---- the keys of an item
def user_key(user_id: str) -> dict:
    return {"pk": f"USER#{user_id}", "sk": "PROFILE"}


def email_key(email: str) -> dict:
    return {"pk": f"EMAIL#{email}", "sk": "USER"}


def project_key(project_id: str) -> dict:
    return {"pk": f"PROJECT#{project_id}", "sk": "META"}


def app_key(app_id: str) -> dict:
    return {"pk": f"APP#{app_id}", "sk": "PROJECT"}


def owner_key(owner_id: str, created_date: str, project_id: str) -> dict:
    return {"pk": f"OWNER#{owner_id}", "sk": f"PROJECT#{created_date}#{project_id}"}


def version_key(project_id: str, version_id: str) -> dict:
    return {"pk": f"PROJECT#{project_id}", "sk": f"VERSION#{version_id}"}


def versions_of(project_id: str):
    """Query condition: every version of a project (sk order = upload order)."""
    return Key("pk").eq(f"PROJECT#{project_id}") & Key("sk").begins_with("VERSION#")


def projects_of(owner_id: str):
    """Query condition: the pointers to a developer's projects (sk order = age)."""
    return Key("pk").eq(f"OWNER#{owner_id}") & Key("sk").begins_with("PROJECT#")


def records(kind: str):
    """Scan filter: the records of a table, not its pointers."""
    return Attr("sk").eq(RECORD_SK[kind])


def keyed(kind: str, item: dict) -> dict:
    """The item with its pk/sk, from its ids (items that have keys, pointers included, keep them)."""
    if "pk" in item and "sk" in item:
        return item
    if kind == "users":
        key = user_key(item["user_id"])
    elif kind == "projects":
        key = project_key(item["project_id"])
    else:
        key = version_key(item["project_id"], item["version_id"])
    return {**item, **key}


def pointers(kind: str, record: dict) -> list:
    """The pointer items a record has (local_server.py brings older data up to date with these)."""
    if kind == "users":
        return [{**email_key(record["email"]), "user_id": record["user_id"]}]
    if kind == "projects":
        out = [{**owner_key(record["owner_id"], record["created_date"], record["project_id"]),
                "project_id": record["project_id"]}]
        if not record.get("deleted_date"):
            out.append({**app_key(record["app_id"]), "project_id": record["project_id"]})
        return out
    return []


# ---- writes in one transaction
class Taken(Exception):
    """A transaction was refused because an item it must create exists (a taken email, app id, ...)."""

    def __init__(self, failed: list):
        super().__init__(f"conditions failed: {failed}")
        self.failed = failed  # indexes into the operations, in order


def put(kind: str, item: dict, new: bool = False) -> dict:
    op = {"TableName": name(kind), "Item": item}
    if new:
        op["ConditionExpression"] = "attribute_not_exists(pk)"
    return {"Put": op}


def delete(kind: str, key: dict, only_if: dict = None) -> dict:
    """Delete an item; only_if={"attr": value} deletes it only while it still says so."""
    op = {"TableName": name(kind), "Key": key}
    if only_if:
        (attr, value), = only_if.items()
        op.update(ConditionExpression="attribute_not_exists(pk) OR #a = :v",
                  ExpressionAttributeNames={"#a": attr}, ExpressionAttributeValues={":v": value})
    return {"Delete": op}


def update(kind: str, key: dict, values: dict) -> dict:
    names = {f"#k{i}": k for i, k in enumerate(values)}
    vals = {f":v{i}": v for i, v in enumerate(values.values())}
    return {"Update": {"TableName": name(kind), "Key": key, "UpdateExpression": "SET " + ", ".join(f"#k{i} = :v{i}" for i in range(len(values))),
                       "ExpressionAttributeNames": names, "ExpressionAttributeValues": vals}}


def transact(*ops):
    """All of them or none (DynamoDB TransactWriteItems); raises Taken when a condition fails."""
    client = resource().meta.client
    try:
        client.transact_write_items(TransactItems=list(ops))
    except client.exceptions.TransactionCanceledException as e:
        reasons = e.response.get("CancellationReasons") or []
        failed = [i for i, r in enumerate(reasons) if r.get("Code") == "ConditionalCheckFailed"]
        if failed:
            raise Taken(failed) from None
        raise


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
    """Create the three tables (local runner, tests). Deployed ones come from scripts/deploy.sh."""
    for kind in KINDS:
        resource().create_table(TableName=name(kind), BillingMode="PAY_PER_REQUEST", **SCHEMA)


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


def get_many(kind: str, keys: list) -> list:
    """Items by key, in the order of keys (missing ones left out); BatchGetItem, 100 at a time."""
    found = {}
    tname = name(kind)
    for i in range(0, len(keys), 100):
        req = {tname: {"Keys": keys[i:i + 100]}}
        while req:
            r = resource().batch_get_item(RequestItems=req)
            for it in r.get("Responses", {}).get(tname, []):
                found[(it["pk"], it["sk"])] = it
            req = r.get("UnprocessedKeys") or None
    return [found[(k["pk"], k["sk"])] for k in keys if (k["pk"], k["sk"]) in found]
