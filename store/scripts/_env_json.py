"""scripts/deploy.sh: the Lambda's environment as JSON (--environment file://...), from KEY=VALUE args."""
import json
import sys

out, pairs = sys.argv[1], sys.argv[2:]
env = dict(p.split("=", 1) for p in pairs)
with open(out, "w") as f:
    json.dump({"Variables": env}, f)
