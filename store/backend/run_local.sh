#!/bin/sh
# Run the iotstore Lambda on this machine:
#   Flask on 0.0.0.0:${PORT:-9310}, DynamoDB in-process (moto, kept in .data/dynamo.json),
#   package files in .data/storage served by the runner itself.
#
#   ./run_local.sh             uses .env.local (copy .env.example)
#   PORT=9200 ./run_local.sh
set -e
cd "$(dirname "$0")"
py=/opt/homebrew/opt/python@3.13/bin/python3.13
if [ ! -x .venv/bin/python ]; then
  [ -x "$py" ] || py=python3.13
  "$py" -m venv .venv
fi
.venv/bin/python -c 'import sys; sys.exit(sys.version_info[:2] != (3, 13))' || { echo "needs Python 3.13 (the Lambda runtime)" >&2; exit 1; }
.venv/bin/python -m pip install -q -r requirements.local.txt  # (python -m: the venv survives being moved)
if [ -f .env.local ]; then set -a; . ./.env.local; set +a; else echo "no .env.local: copy .env.example" >&2; exit 1; fi
# PUBLIC_BASE_URL=auto: this machine's LAN address (boards on the LAN download packages from it)
if [ "$PUBLIC_BASE_URL" = auto ] || [ -z "$PUBLIC_BASE_URL" ]; then
  ip=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || hostname -I 2>/dev/null | cut -d' ' -f1)
  export PUBLIC_BASE_URL="http://${ip:-127.0.0.1}:${PORT:-9310}"
fi
export PYTHONPATH="$PWD${PYTHONPATH:+:$PYTHONPATH}"
exec .venv/bin/python local_server.py
