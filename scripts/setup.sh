#!/bin/sh
# Set up the iotdash SDK on this machine (macOS or Linux), once:
#   Python tools + Chromium (tools/.venv), the OS image, the desktop simulator, a check.
#
#   scripts/setup.sh              build and simulate apps
#   scripts/setup.sh --web        also the browser build, for `iotdash run --web` (Emscripten SDK in ~/emsdk)
#   scripts/setup.sh --firmware   also the board's firmware (PlatformIO)
#
# Then put bin/ on PATH (the script prints the line for your shell).
set -e
sdk=$(cd "$(dirname "$0")/.." && pwd)
iot="$sdk/bin/iotdash"
web=0
fw=0
for a in "$@"; do
  case "$a" in
    --web) web=1 ;;
    --firmware) fw=1 ;;
    *) echo "usage: $0 [--web] [--firmware]" >&2; exit 2 ;;
  esac
done

echo "== Python tools"
"$iot" --version                      # creates tools/.venv and installs Chromium on the first run
echo "== OS image"
"$iot" os build
"$iot" os deploy --to sim             # the simulator's card
echo "== desktop simulator"
"$sdk/tools/.venv/bin/python" -c "import sys; sys.path.insert(0, '$sdk/tools'); import iotdash; iotdash.build_sim()"
if [ $web = 1 ]; then
  echo "== browser build"
  "$sdk/web/build.sh"
fi
if [ $fw = 1 ]; then
  echo "== firmware"
  "$iot" firmware build
fi
echo
"$iot" doctor || true
case ":$PATH:" in
  *":$sdk/bin:"*) ;;
  *) echo
     echo "Add the SDK to PATH (then open a new terminal):"
     echo "  echo 'export PATH=\"\$PATH:$sdk/bin\"' >> ~/.$(basename "${SHELL:-sh}")rc" ;;
esac
