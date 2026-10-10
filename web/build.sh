#!/bin/sh
# Build iotdash for web browsers (WebAssembly) and stage what a page needs in web/dist/:
#
#   iotdash.js, iotdash.wasm   the engine + OS (an ES module factory: createIotdash)
#   iotdash-web.js             the driver: canvas, touch/keyboard, simulated network and scale
#   system/                    the OS image (boot.idb, os.idb, os.json, assets/)
#
# Needs the Emscripten SDK (em++ on PATH, or installed in $EMSDK, default ~/emsdk) and the OS
# built (tools/iotdash.py os build; done here when missing).
#
#   web/build.sh            build (incremental)
#   web/build.sh clean      start over
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
obj="$here/build"
out="$here/dist"
if [ "$1" = "clean" ]; then
  rm -rf "$obj" "$out"
  exit 0
fi
if ! command -v em++ >/dev/null 2>&1; then
  sdk="${EMSDK:-$HOME/emsdk}"
  [ -f "$sdk/emsdk_env.sh" ] || { echo "no Emscripten SDK: install it (https://emscripten.org) or set EMSDK" >&2; exit 1; }
  export EMSDK_QUIET=1
  [ -z "$EMSDK_PYTHON" ] && [ -x /opt/homebrew/opt/python@3.13/bin/python3.13 ] && export EMSDK_PYTHON=/opt/homebrew/opt/python@3.13/bin/python3.13
  . "$sdk/emsdk_env.sh" >/dev/null
fi
mkdir -p "$obj" "$out/system"

OPT="-O2 -DNDEBUG"
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# objects: QuickJS (C) and the engine (C++), rebuilt when the source or a header is newer
newest_header=$(ls -t "$root"/lib/rt/*.h "$root"/lib/quickjs/*.h "$root"/sim/fake_radio.h | head -1)
list="$obj/sources.txt"
: > "$list"
for f in "$root"/lib/quickjs/quickjs.c "$root"/lib/quickjs/libregexp.c "$root"/lib/quickjs/libunicode.c \
         "$root"/lib/quickjs/dtoa.c "$root"/lib/rt/*.cpp "$here/web_main.cpp"; do
  o="$obj/$(basename "$f").o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$newest_header" -nt "$o" ]; then echo "$f" >> "$list"; fi
done
if [ -s "$list" ]; then
  echo "compiling $(wc -l < "$list" | tr -d ' ') files"
  cat > "$obj/cc.sh" <<EOF
#!/bin/sh
f="\$1"; o="$obj/\$(basename "\$f").o"
case "\$f" in
  *.c) exec emcc $OPT -w -D_GNU_SOURCE -std=gnu11 -I"$root/lib/quickjs" -c "\$f" -o "\$o" ;;
  *)   exec em++ $OPT -std=c++17 -Wno-unused-function -I"$root/lib/rt" -I"$root/lib/quickjs" -c "\$f" -o "\$o" ;;
esac
EOF
  xargs -P "$jobs" -n 1 sh "$obj/cc.sh" < "$list"
fi

exports="_malloc,_free,_iot_boot,_iot_update,_iot_frame,_iot_presents,_iot_pointer,_iot_text,_iot_key,_iot_launch,\
_iot_home,_iot_apps_changed,_iot_fetch_done,_iot_download_data,_iot_download_done,_iot_internet,_iot_wifi_reach,_iot_scale,_iot_scale_power,_iot_eval"
em++ $OPT "$obj"/*.o -o "$out/iotdash.js" \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createIotdash -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=2MB -sFORCE_FILESYSTEM=1 \
  -sEXPORTED_FUNCTIONS="$exports" \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,HEAPU8,HEAPU16,UTF8ToString,stringToUTF8,lengthBytesUTF8

# the OS image
if [ ! -f "$root/os/build/boot.idb" ] || [ ! -f "$root/os/build/card/system/os.idb" ]; then
  (cd "$root" && tools/.venv/bin/python tools/iotdash.py os build)
fi
cp "$root/os/build/boot.idb" "$out/system/boot.idb"
cp -R "$root/os/build/card/system/." "$out/system/"
cp "$here/iotdash-web.js" "$out/iotdash-web.js"
cp "$here/demo.html" "$out/demo.html"
echo "web/dist: $(du -sh "$out" | cut -f1) (engine $(du -h "$out/iotdash.wasm" | cut -f1))"
