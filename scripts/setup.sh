#!/bin/sh
# Set up the iotdash SDK on this machine (macOS or Linux), once:
#   Python tools + Chromium (tools/.venv), the OS image, the desktop simulator, a check.
#
#   scripts/setup.sh              build and simulate apps
#   scripts/setup.sh --web        also the browser build, for `iotdash run --web` (Emscripten SDK in ~/emsdk)
#   scripts/setup.sh --firmware   also the board's firmware (PlatformIO)
#   scripts/setup.sh --no-path    leave the shell's startup file alone
#
# It also puts bin/ on PATH: one line in ~/.zshrc, ~/.bash_profile (macOS) / ~/.bashrc,
# ~/.config/fish/config.fish or ~/.profile, for the shell in $SHELL (never twice).
set -e
sdk=$(cd "$(dirname "$0")/.." && pwd)
iot="$sdk/bin/iotdash"
web=0
fw=0
path=1
for a in "$@"; do
  case "$a" in
    --web) web=1 ;;
    --firmware) fw=1 ;;
    --no-path) path=0 ;;
    *) echo "usage: $0 [--web] [--firmware] [--no-path]" >&2; exit 2 ;;
  esac
done

# bin/ on PATH through the shell's startup file (new terminals see it)
add_to_path() {
  shell=$(basename "${SHELL:-sh}")
  case "$shell" in
    zsh) rc="$HOME/.zshrc" ;;
    bash) if [ "$(uname)" = Darwin ]; then rc="$HOME/.bash_profile"; else rc="$HOME/.bashrc"; fi ;;
    fish) rc="$HOME/.config/fish/config.fish" ;;
    *) rc="$HOME/.profile" ;;
  esac
  if [ -f "$rc" ] && grep -qF "$sdk/bin" "$rc"; then
    echo "PATH: $rc already has $sdk/bin"
    return
  fi
  if [ "$shell" = fish ]; then
    line="fish_add_path --append $sdk/bin"
  else
    line="export PATH=\"\$PATH:$sdk/bin\""
  fi
  mkdir -p "$(dirname "$rc")"
  printf '\n# iotdash SDK (command line: iotdash)\n%s\n' "$line" >> "$rc"
  echo "PATH: added $sdk/bin to $rc"
}

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
echo
case ":$PATH:" in
  *":$sdk/bin:"*) echo "PATH: iotdash is ready ($sdk/bin)" ;;
  *) if [ $path = 1 ]; then
       add_to_path
       echo "      open a new terminal (or source that file), then: iotdash doctor"
     else
       echo "PATH: not changed (--no-path); add $sdk/bin yourself"
     fi ;;
esac
