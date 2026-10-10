#!/bin/bash
# Build the web app for your store (after scripts/deploy.sh) and, when WEB_BUCKET is set in
# deploy.env, copy it there. The app is static files (frontend/dist): any host serves it, as long
# as unknown paths fall back to index.html (S3 website: error document index.html; CloudFront:
# 403/404 -> /index.html) and .wasm is served as application/wasm.
#
#   scripts/deploy-web.sh                 build (and upload when WEB_BUCKET is set)
#   IOTSTORE_CONFIG=staging.env scripts/deploy-web.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
CONF=${IOTSTORE_CONFIG:-$root/deploy.env}
[ -f "$CONF" ] || { echo "no $CONF: cp deploy.env.example deploy.env, then edit it" >&2; exit 1; }
load_env() {  # KEY=VALUE lines; values may have spaces, quotes optional; # lines and blanks skipped
  local line key val
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in '' | \#*) continue ;; esac
    key=$(printf '%s' "${line%%=*}" | tr -d ' ')
    val=$(printf '%s' "${line#*=}" | sed -e 's/^ *//' -e 's/ *$//' -e 's/^"\(.*\)"$/\1/' -e "s/^'\(.*\)'$/\1/")
    export "$key=$val"
  done < "$1"
}
load_env "$CONF"
[ -n "${AWS_PROFILE:-}" ] || unset AWS_PROFILE  # empty = the default credentials (an empty value would make the CLI fail)
STORE_NAME=${STORE_NAME:-iotstore}
[ -f "$root/.deploy/${STORE_NAME}.env" ] && load_env "$root/.deploy/${STORE_NAME}.env"
REGION=${AWS_REGION:-ap-southeast-1}
[ -n "${API_URL:-}" ] || { echo "no API_URL: run scripts/deploy.sh first (or set API_URL in $CONF)" >&2; exit 1; }
a() { aws --region "$REGION" ${AWS_PROFILE:+--profile "$AWS_PROFILE"} "$@"; }

cd "$root/frontend"
[ -d node_modules ] || npm ci
VITE_API_URL="$API_URL" VITE_STORE_NAME="${STORE_TITLE:-$STORE_NAME}" npm run build
echo "web app for $API_URL: $root/frontend/dist ($(du -sh dist | cut -f1))"
if [ -n "${WEB_BUCKET:-}" ]; then
  a s3 sync dist "s3://$WEB_BUCKET" --delete --exclude "*.wasm" --only-show-errors
  a s3 cp dist "s3://$WEB_BUCKET" --recursive --exclude "*" --include "*.wasm" --content-type application/wasm --only-show-errors
  echo "copied to s3://$WEB_BUCKET (its origin must be in WEB_ORIGIN; run scripts/deploy.sh after changing that)"
fi
