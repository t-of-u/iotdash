# Shared by scripts/deploy.sh and scripts/deploy-web.sh (sourced, bash): a store's settings and names.

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
REGION=${AWS_REGION:-ap-southeast-1}
P="${TABLE_PREFIX:-${STORE_NAME}-}"               # tables <P>users, <P>projects, <P>versions
FUNCTION="${FUNCTION_NAME:-${STORE_NAME}-api}"   # the API's Lambda function
ROLE="${FUNCTION}-role"
LAYER="${FUNCTION}-deps"
WEB_HOSTING=${WEB_HOSTING:-website}             # website: WEB_BUCKET is a public S3 website; private: files only
work="$root/.deploy"
mkdir -p "$work"

a() { aws --region "$REGION" ${AWS_PROFILE:+--profile "$AWS_PROFILE"} "$@"; }

# the S3 website address of WEB_BUCKET (older regions write "s3-website-<region>", newer "s3-website.<region>")
web_site_url() {
  case "$REGION" in
    us-east-1 | us-west-1 | us-west-2 | eu-west-1 | ap-southeast-1 | ap-southeast-2 | ap-northeast-1 | sa-east-1 | us-gov-west-1)
      echo "http://${WEB_BUCKET}.s3-website-${REGION}.amazonaws.com" ;;
    *) echo "http://${WEB_BUCKET}.s3-website.${REGION}.amazonaws.com" ;;
  esac
}

# the web app's origins, one per line: WEB_ORIGIN (comma-separated) and the S3 website when the store hosts there
web_origins() {
  {
    echo "${WEB_ORIGIN:-}" | tr ',' '\n'
    if [ -n "${WEB_BUCKET:-}" ] && [ "$WEB_HOSTING" = website ]; then web_site_url; fi
  } | sed -e 's/^ *//' -e 's/ *$//' -e 's#/$##' | grep -v '^$' | awk '!seen[$0]++'
}
