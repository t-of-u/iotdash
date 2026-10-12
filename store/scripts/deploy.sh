#!/bin/bash
# Deploy an iotstore API into your AWS account, or update it: every resource is made when missing
# and brought up to date when present, so the same command does the first install and each release.
#
#   cp deploy.env.example deploy.env      # then edit (STORE_NAME, AWS_PROFILE, WEB_ORIGIN, ...)
#   scripts/deploy.sh                     # tables, bucket, role, dependency layer, function, URL
#   scripts/deploy.sh --dry-run           # only print the changes it would make (reads AWS)
#   IOTSTORE_CONFIG=staging.env scripts/deploy.sh
#
# Makes (names from STORE_NAME unless deploy.env sets them):
#   DynamoDB  <TABLE_PREFIX>users, -projects, -versions (default prefix <name>-; on-demand, keys pk + sk only)
#   S3        S3_BUCKET_NAME (default <name>-packages-<account>; private; CORS for WEB_ORIGIN)
#   Lambda    FUNCTION_NAME (default <name>-api; Python 3.13) + layer <function>-deps + a public URL
#   IAM       <function>-role (logs, those tables, that bucket)
# The URL is printed and kept in .deploy/<name>.env (API_URL) for scripts/deploy-web.sh.
set -euo pipefail
# shellcheck source=_common.sh
. "$(dirname "$0")/_common.sh"
DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1
backend="$root/backend"

# read-only calls (a) always run; changes only without --dry-run
change() {
  if [ $DRY = 1 ]; then echo "  would: aws $*" | cut -c1-160 >&2; else a "$@"; fi
}
step() { echo "== $*"; }

ACCOUNT=$(a sts get-caller-identity --query Account --output text)
BUCKET=${S3_BUCKET_NAME:-${STORE_NAME}-packages-${ACCOUNT}}
echo "store $STORE_NAME in account $ACCOUNT, $REGION$([ $DRY = 1 ] && echo ' (dry run)')"
echo "  function $FUNCTION, tables ${P}users/projects/versions, bucket $BUCKET, role $ROLE, layer $LAYER"

# ---------------------------------------------------------------- 1. tables
# only pk + sk (strings), no secondary indexes: lookups use pointer items (backend/IOTStore/libs/tables.py)
step "DynamoDB tables (${P}users, ${P}projects, ${P}versions)"
make_table() {
  local name=$1 keys
  if keys=$(a dynamodb describe-table --table-name "$name" --query 'Table.KeySchema[].[AttributeName,KeyType]' \
      --output text 2>/dev/null); then
    keys=$(echo "$keys" | tr '\t\n' ': ')
    case "$keys" in
      "pk:HASH sk:RANGE "*) echo "  $name: exists" ;;
      *) echo "  $name exists with keys ${keys}but the store needs pk (HASH) + sk (RANGE), both strings" >&2; exit 1 ;;
    esac
    return
  fi
  change dynamodb create-table --table-name "$name" --billing-mode PAY_PER_REQUEST \
    --key-schema AttributeName=pk,KeyType=HASH AttributeName=sk,KeyType=RANGE \
    --attribute-definitions AttributeName=pk,AttributeType=S AttributeName=sk,AttributeType=S > /dev/null
  [ $DRY = 1 ] || { a dynamodb wait table-exists --table-name "$name"; echo "  $name: created"; }
}
make_table "${P}users"
make_table "${P}projects"
make_table "${P}versions"

# ---------------------------------------------------------------- 2. bucket
step "S3 bucket $BUCKET"
if a s3api head-bucket --bucket "$BUCKET" > /dev/null 2>&1; then
  echo "  exists"
else
  if [ "$REGION" = us-east-1 ]; then
    change s3api create-bucket --bucket "$BUCKET" > /dev/null
  else
    change s3api create-bucket --bucket "$BUCKET" --create-bucket-configuration LocationConstraint="$REGION" > /dev/null
  fi
fi
change s3api put-public-access-block --bucket "$BUCKET" --public-access-block-configuration \
  BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true
cors=$(web_origins | paste -sd, - | sed 's/,/, /g')  # WEB_ORIGIN + the S3 website when WEB_BUCKET hosts the app
[ -n "$cors" ] || cors="*"
origins=$(echo "$cors" | tr ',' '\n' | sed 's/^ *//' | sed 's/.*/"&"/' | paste -sd, -)
cat > "$work/cors.json" <<EOF
{"CORSRules": [{"AllowedOrigins": [$origins], "AllowedMethods": ["GET", "HEAD", "POST"],
  "AllowedHeaders": ["*"], "ExposeHeaders": ["ETag", "Content-Length"], "MaxAgeSeconds": 3600}]}
EOF
change s3api put-bucket-cors --bucket "$BUCKET" --cors-configuration "file://$work/cors.json"
echo "  private, CORS for: $origins"

# ---------------------------------------------------------------- 3. role
step "IAM role $ROLE"
new_role=0
if a iam get-role --role-name "$ROLE" > /dev/null 2>&1; then
  echo "  exists"
else
  change iam create-role --role-name "$ROLE" --assume-role-policy-document \
    '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"lambda.amazonaws.com"},"Action":"sts:AssumeRole"}]}' > /dev/null
  change iam attach-role-policy --role-name "$ROLE" --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
  new_role=1
fi
T="arn:aws:dynamodb:${REGION}:${ACCOUNT}:table/${P}"
cat > "$work/policy.json" <<EOF
{"Version": "2012-10-17", "Statement": [
  {"Effect": "Allow", "Action": ["dynamodb:GetItem", "dynamodb:BatchGetItem", "dynamodb:PutItem", "dynamodb:UpdateItem",
     "dynamodb:DeleteItem", "dynamodb:ConditionCheckItem", "dynamodb:Query", "dynamodb:Scan", "dynamodb:BatchWriteItem"],
   "Resource": ["${T}users", "${T}projects", "${T}versions"]},
  {"Effect": "Allow", "Action": ["s3:GetObject", "s3:PutObject", "s3:DeleteObject"],
   "Resource": "arn:aws:s3:::${BUCKET}/projects/*"},
  {"Effect": "Allow", "Action": "s3:ListBucket", "Resource": "arn:aws:s3:::${BUCKET}"}]}
EOF
change iam put-role-policy --role-name "$ROLE" --policy-name "${STORE_NAME}-data" --policy-document "file://$work/policy.json"
if [ $new_role = 1 ] && [ $DRY = 0 ]; then
  echo "  waiting for the new role to reach Lambda"
  sleep 12
fi

# ---------------------------------------------------------------- 4. dependency layer
step "Lambda layer $LAYER (backend/layers/iotstore.txt)"
want=$(shasum -a 256 "$backend/layers/iotstore.txt" | cut -c1-16)
have=$(a lambda list-layer-versions --layer-name "$LAYER" --query 'LayerVersions[0].[LayerVersionArn,Description]' \
  --output text 2>/dev/null || true)
LAYER_ARN=$(echo "$have" | awk '{print $1}')
if [ -n "$have" ] && [ "$have" != None ] && [ "$(echo "$have" | awk '{print $2}')" = "deps-$want" ]; then
  echo "  up to date"
else
  py=$(command -v python3.13 || true)
  [ -n "$py" ] || { echo "python3.13 is needed to build the layer (the Lambda runtime's version)" >&2; exit 1; }
  rm -rf "$work/layer" "$work/layer.zip"
  "$py" -m pip install -q -r "$backend/layers/iotstore.txt" --platform manylinux2014_x86_64 --implementation cp \
    --python-version 3.13 --only-binary=:all: --upgrade --target "$work/layer/python"
  (cd "$work/layer" && zip -qr9 ../layer.zip python)
  if [ $DRY = 1 ]; then
    echo "  would: publish $LAYER ($(du -h "$work/layer.zip" | cut -f1))"
    LAYER_ARN="arn:aws:lambda:${REGION}:${ACCOUNT}:layer:${LAYER}:new"
  else
    LAYER_ARN=$(a lambda publish-layer-version --layer-name "$LAYER" --description "deps-$want" \
      --zip-file "fileb://$work/layer.zip" --compatible-runtimes python3.13 --compatible-architectures x86_64 \
      --query LayerVersionArn --output text)
    echo "  published $LAYER_ARN"
  fi
fi

# ---------------------------------------------------------------- 5. function
step "Lambda function $FUNCTION"
rm -f "$work/function.zip"
(cd "$backend" && zip -qr9 "$work/function.zip" IOTStore -x "*__pycache__*" "*.pyc" "*.DS_Store")
exists=0
a lambda get-function --function-name "$FUNCTION" > /dev/null 2>&1 && exists=1
secret=${JWT_SECRET_KEY:-}
if [ -z "$secret" ] && [ $exists = 1 ]; then  # keep the one it has: a new key signs everyone out
  secret=$(a lambda get-function-configuration --function-name "$FUNCTION" \
    --query 'Environment.Variables.JWT_SECRET_KEY' --output text 2>/dev/null || true)
  [ "$secret" = None ] && secret=""
fi
[ -n "$secret" ] || secret=$(openssl rand -hex 32)
commit=$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)
python3 "$root/scripts/_env_json.py" "$work/env.json" \
  JWT_SECRET_KEY="$secret" STORAGE=s3 S3_BUCKET_NAME="$BUCKET" TABLE_PREFIX="$P" CORS_ORIGIN="$cors" \
  STORE_NAME="${STORE_TITLE:-$STORE_NAME}" SIGNUP="${SIGNUP:-open}" ENV=PROD BUILD_COMMIT="$commit"
if [ $exists = 0 ]; then
  change lambda create-function --function-name "$FUNCTION" --runtime python3.13 --architectures x86_64 \
    --handler IOTStore.entry_points.IOTStore.main --zip-file "fileb://$work/function.zip" \
    --role "arn:aws:iam::${ACCOUNT}:role/${ROLE}" --layers "$LAYER_ARN" --timeout 30 --memory-size 512 \
    --environment "file://$work/env.json" > /dev/null
  [ $DRY = 1 ] || { a lambda wait function-active-v2 --function-name "$FUNCTION"; echo "  created"; }
else
  # a function made elsewhere (e.g. in the console) gets the role with the store's permissions too
  role_arn="arn:aws:iam::${ACCOUNT}:role/${ROLE}"
  had=$(a lambda get-function-configuration --function-name "$FUNCTION" --query Role --output text)
  [ "$had" = "$role_arn" ] || echo "  role: ${had##*/} -> $ROLE"
  change lambda update-function-code --function-name "$FUNCTION" --zip-file "fileb://$work/function.zip" > /dev/null
  [ $DRY = 1 ] || a lambda wait function-updated-v2 --function-name "$FUNCTION"
  # memory and timeout too: a console-made function has 128 MB and 3 s, too little for bcrypt
  change lambda update-function-configuration --function-name "$FUNCTION" --runtime python3.13 --role "$role_arn" \
    --timeout 30 --memory-size 512 --handler IOTStore.entry_points.IOTStore.main --layers "$LAYER_ARN" --environment "file://$work/env.json" > /dev/null
  [ $DRY = 1 ] || { a lambda wait function-updated-v2 --function-name "$FUNCTION"; echo "  updated ($commit)"; }
fi
rm -f "$work/env.json"

# ---------------------------------------------------------------- 6. public URL
step "function URL"
URL=$(a lambda get-function-url-config --function-name "$FUNCTION" --query FunctionUrl --output text 2>/dev/null || true)
if [ -z "$URL" ] || [ "$URL" = None ]; then
  if [ $DRY = 1 ]; then
    echo "  would: create a public function URL (auth NONE; CORS answered by the function)"
    URL="https://<new>.lambda-url.${REGION}.on.aws/"
  else
    URL=$(a lambda create-function-url-config --function-name "$FUNCTION" --auth-type NONE --query FunctionUrl --output text)
    a lambda add-permission --function-name "$FUNCTION" --statement-id public-url --action lambda:InvokeFunctionUrl \
      --principal '*' --function-url-auth-type NONE > /dev/null
    a lambda add-permission --function-name "$FUNCTION" --statement-id public-url-invoke --action lambda:InvokeFunction \
      --principal '*' --invoked-via-function-url > /dev/null
  fi
fi
URL=${URL%/}
[ $DRY = 1 ] || printf 'API_URL=%s\nS3_BUCKET_NAME=%s\n' "$URL" "$BUCKET" > "$work/${STORE_NAME}.env"
echo
echo "API: $URL"
if [ $DRY = 0 ]; then
  curl -s "$URL/api/health" && echo || true
fi
echo "next: iotdash store deploy-web (the web app), then on boards: Apps -> Store address -> $URL"
