#!/bin/bash
# Build the web app for your store (after scripts/deploy.sh) and, when WEB_BUCKET is set in
# deploy.env, publish it there:
#
#   WEB_HOSTING=website (default)  the bucket becomes a public S3 website: made when missing, website
#                                  hosting on (unknown paths -> index.html), ACLs off, public ACLs still
#                                  blocked, a policy that lets anyone read its files; prints the URL
#   WEB_HOSTING=private            only the files (the bucket stays private, e.g. behind CloudFront)
#
# Without WEB_BUCKET it only builds frontend/dist (any static host serves it: unknown paths must fall
# back to index.html and .wasm be application/wasm).
#
#   scripts/deploy-web.sh              (iotdash store deploy-web)
#   IOTSTORE_CONFIG=staging.env scripts/deploy-web.sh
set -euo pipefail
# shellcheck source=_common.sh
. "$(dirname "$0")/_common.sh"
[ -f "$work/${STORE_NAME}.env" ] && load_env "$work/${STORE_NAME}.env"   # API_URL from scripts/deploy.sh
[ -n "${API_URL:-}" ] || { echo "no API_URL: run scripts/deploy.sh first (or set API_URL in $CONF)" >&2; exit 1; }

echo "== web app for $API_URL"
cd "$root/frontend"
[ -d node_modules ] || npm ci
VITE_API_URL="$API_URL" VITE_STORE_NAME="${STORE_TITLE:-$STORE_NAME}" npm run build
echo "  frontend/dist: $(du -sh dist | cut -f1)"
[ -n "${WEB_BUCKET:-}" ] || { echo "no WEB_BUCKET: serve frontend/dist from your host"; exit 0; }

echo "== S3 bucket $WEB_BUCKET ($WEB_HOSTING)"
if ! a s3api head-bucket --bucket "$WEB_BUCKET" > /dev/null 2>&1; then
  if [ "$REGION" = us-east-1 ]; then
    a s3api create-bucket --bucket "$WEB_BUCKET" > /dev/null
  else
    a s3api create-bucket --bucket "$WEB_BUCKET" --create-bucket-configuration LocationConstraint="$REGION" > /dev/null
  fi
  echo "  created"
fi
if [ "$WEB_HOSTING" = website ]; then
  a s3api put-bucket-ownership-controls --bucket "$WEB_BUCKET" \
    --ownership-controls 'Rules=[{ObjectOwnership=BucketOwnerEnforced}]'
  # a public policy is allowed, public ACLs are not (the policy is the only way in)
  a s3api put-public-access-block --bucket "$WEB_BUCKET" --public-access-block-configuration \
    BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=false,RestrictPublicBuckets=false
  a s3api put-bucket-website --bucket "$WEB_BUCKET" \
    --website-configuration '{"IndexDocument":{"Suffix":"index.html"},"ErrorDocument":{"Key":"index.html"}}'
  policy="{\"Version\":\"2012-10-17\",\"Statement\":[{\"Sid\":\"PublicReadWebApp\",\"Effect\":\"Allow\",\"Principal\":\"*\",\"Action\":\"s3:GetObject\",\"Resource\":\"arn:aws:s3:::${WEB_BUCKET}/*\"}]}"
  if ! a s3api put-bucket-policy --bucket "$WEB_BUCKET" --policy "$policy"; then
    echo "  the public-read policy was refused: is Block Public Access on for the whole account" >&2
    echo "  (S3 console -> Block Public Access settings for this account)? Or use WEB_HOSTING=private" >&2
    exit 1
  fi
  echo "  website hosting on, public read (policy), ACLs off"
fi

echo "== upload"
# caching: assets/ have content hashes in their names (keep a year); index.html and the engine
# (iotdash/, loaded with ?v=<build>) are checked on every load (no-cache: cheap with ETags)
a s3 sync dist "s3://$WEB_BUCKET" --delete --exclude "index.html" --exclude "iotdash/*" --exclude "assets/*" --only-show-errors
a s3 sync dist/assets "s3://$WEB_BUCKET/assets" --delete --cache-control "public, max-age=31536000, immutable" --only-show-errors
a s3 sync dist/iotdash "s3://$WEB_BUCKET/iotdash" --delete --exclude "*.wasm" --cache-control "no-cache" --only-show-errors
a s3 cp dist/iotdash "s3://$WEB_BUCKET/iotdash" --recursive --exclude "*" --include "*.wasm" \
  --content-type application/wasm --cache-control "no-cache" --only-show-errors
a s3 cp dist/index.html "s3://$WEB_BUCKET/index.html" --cache-control "no-cache" --only-show-errors
echo "  $(find dist -type f | wc -l | tr -d ' ') files"

if [ "$WEB_HOSTING" = website ]; then
  site=$(web_site_url)
  echo
  echo "web app: $site"
  # the API must allow this origin (scripts/deploy.sh adds it when WEB_BUCKET is set)
  allowed=$(a lambda get-function-configuration --function-name "$FUNCTION" \
    --query 'Environment.Variables.CORS_ORIGIN' --output text 2>/dev/null || true)
  case ", $allowed, " in
    *", $site, "* | *", *, "*) ;;
    *) echo "the API does not allow this origin yet: run iotdash store deploy (it adds it)" ;;
  esac
else
  echo "files in s3://$WEB_BUCKET; serve them (e.g. CloudFront) and add that origin to WEB_ORIGIN"
fi
