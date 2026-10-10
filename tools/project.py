"""iotdash app projects: an app is a folder anywhere, built and run with this SDK (like a Flutter app
and the flutter tool).

    my-app/
      app.json      {"id": "my-app", "title": "My App", "version": "1.0", "description": "", "os": {"min": "0.1.0"}}
      icon.svg      the home-screen icon (or icon.png)
      main.html     the first page; then any pages, CSS, JS, images, fonts it uses
      build/        what the commands make (app/, <id>-<version>.idbz, screenshots): not source

The commands take the project as APP: a folder (default: the current one or the nearest parent
with an app.json), or the name of one of the SDK's examples (examples/NAME).
"""

import functools
import http.server
import json
import os
import re
import shutil
import sys
import threading
import time
import webbrowser
from pathlib import Path

import iotdash as I
import osbuild as O

TEMPLATE = I.ROOT / "templates" / "app"


# ---------------------------------------------------------------- finding the project

def find(app=None):
    """The project folder for APP (see the module docstring)."""
    if app:
        p = Path(app).expanduser()
        if p.is_dir():
            p = p.resolve()
            if (p / "main.html").exists():
                return p
            raise SystemExit(f"{O.shown(p)} is not an iotdash app (no main.html)")
        if "/" not in app and (O.EXAMPLES / app / "main.html").exists():
            return O.EXAMPLES / app
        raise SystemExit(f"no app '{app}': give a project folder, or an example: {', '.join(examples())}")
    here = Path.cwd()
    for d in (here, *here.parents):
        if (d / "app.json").exists() and (d / "main.html").exists():
            return d
    if (here / "main.html").exists():
        return here
    raise SystemExit("no iotdash app here: run this in an app project (app.json + main.html) or give its folder.\n"
                     "  iotdash create my-app      makes a new one")


def examples():
    return sorted(p.name for p in O.EXAMPLES.iterdir() if (p / "main.html").exists()) if O.EXAMPLES.exists() else []


# ---------------------------------------------------------------- create

def create(dest, app_id=None, title=None, log=print):
    dest = Path(dest).expanduser().resolve()
    if dest.exists() and any(dest.iterdir()):
        raise SystemExit(f"{dest} exists and is not empty")
    app_id = app_id or re.sub(r"[^a-z0-9-]+", "-", dest.name.lower()).strip("-")
    if not O.APP_ID.match(app_id):
        raise SystemExit(f'"{app_id}" cannot be an app id: 2-40 of a-z, 0-9 and "-" (use --id)')
    title = title or " ".join(w.capitalize() for w in app_id.split("-"))
    shutil.copytree(TEMPLATE, dest, dirs_exist_ok=True)
    for f in dest.rglob("*"):
        if f.is_file() and f.suffix in (".json", ".html", ".md", ".js", ".css", ".svg"):
            t = f.read_text(encoding="utf-8")
            t2 = t.replace("{{id}}", app_id).replace("{{title}}", title).replace("{{os}}", O.os_version())
            if t2 != t:
                f.write_text(t2, encoding="utf-8")
    (dest / "gitignore").rename(dest / ".gitignore")
    log(f"created {title} ({app_id}) in {O.shown(dest)}\n\n"
        f"  cd {O.shown(dest)}\n"
        f"  iotdash run             the simulator (--device: the board over USB, --web: the browser)\n"
        f"  iotdash pack            a package for the store (build/{app_id}-1.0.idbz)")
    return dest


# ---------------------------------------------------------------- run

def run_sim(src, shot=None):
    app_id, _ = O.install_app(src, "sim")
    script = f"wait 2500; launch {app_id}; wait 1200"
    if not shot:
        print("simulator: close its window to stop (iotdash run again after changes)")
    O.run_sim(script, shot)


def run_device(src):
    """Install over USB and open the app, in one link session (opening the port resets the board)."""
    c = O.DeviceCard()
    app_id, _ = O.install_app(src, "device", c=c)
    link = c.link
    t0 = time.time()
    while time.time() - t0 < 90:  # the OS may still be starting (it copies a new system image first)
        r = link.request(I.T_EVAL, b"os.page().id", timeout=5)
        page = r[2:].decode("utf-8", "replace").strip('"') if r else ""
        if page and page != "boot":
            break
        time.sleep(1)
    r = link.request(I.T_EVAL, f"os.launch({json.dumps(app_id)}); 'ok'".encode(), timeout=10)
    out = r[2:].decode("utf-8", "replace") if r else "no answer"
    print(f"{app_id} open on the board" if "ok" in out else f"installed; opening it said: {out}")


def run_web(src, port=0, open_browser=True):
    """The app in the browser: the engine and OS in WebAssembly (web/build.sh), served from here."""
    dist = I.ROOT / "web" / "dist"
    if not (dist / "iotdash.wasm").exists():
        raise SystemExit(f"no browser build in {O.shown(dist)}: run web/build.sh once (needs the Emscripten SDK)")
    pack = O.pack_app(src)
    blob = pack.read_bytes()

    class Handler(http.server.SimpleHTTPRequestHandler):
        def do_GET(self):
            if self.path.split("?")[0] == "/__app.idbz":
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(blob)))
                self.end_headers()
                self.wfile.write(blob)
                return
            super().do_GET()

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(Handler, directory=str(dist)))
    url = f"http://127.0.0.1:{srv.server_address[1]}/demo.html?pkg=/__app.idbz"
    print(f"{url}\n(Ctrl+C to stop)", flush=True)
    if open_browser:
        threading.Thread(target=lambda: (time.sleep(0.4), webbrowser.open(url)), daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()


# ---------------------------------------------------------------- shot: simulator next to Chromium

def shot(src, page="main.html", dark=False):
    from PIL import Image
    out = src / "build"
    out.mkdir(parents=True, exist_ok=True)
    sim_png, ref_png = out / "shot-sim.png", out / "shot-chromium.png"
    app_id, _ = O.install_app(src, "sim")
    # the theme is a setting of the simulator's OS (kept on its card), read when the app starts
    script = f"regset darkMode {'true' if dark else 'false'}; wait 2500; launch {app_id}; wait 1200"
    if page != "main.html":
        script += f"; eval navigate({json.dumps(page)}); wait 1200"
    O.run_sim(script, str(sim_png))
    O.ref_shot("app/" + page, ref_png, app=src, dark=dark)
    a, b = Image.open(sim_png).convert("RGB"), Image.open(ref_png).convert("RGB")
    canvas = Image.new("RGB", (a.width + b.width + 10, max(a.height, b.height)), (255, 0, 255))
    canvas.paste(a, (0, 0))
    canvas.paste(b, (a.width + 10, 0))
    cmp_path = out / "compare.png"
    canvas.save(cmp_path)
    print(f"simulator (left) / Chromium (right): {O.shown(cmp_path)}")


# ---------------------------------------------------------------- doctor

def doctor():
    ok_all = True

    def line(ok, what, hint=""):
        nonlocal ok_all
        ok_all &= ok is not False
        mark = {True: "✓", False: "✗", None: "-"}[ok]
        print(f"  {mark} {what}" + (f"\n      {hint}" if hint and ok is not True else ""))

    print(f"iotdash SDK {O.os_version()} at {I.ROOT}")
    venv = I.ROOT / "tools" / ".venv"
    line(Path(sys.executable).resolve().is_relative_to(venv.resolve()) or venv.exists(), "Python tools (tools/.venv)",
         "python3 -m venv tools/.venv && tools/.venv/bin/pip install -r tools/requirements.txt")
    missing = [m for m in ("PIL", "fontTools", "freetype", "uharfbuzz", "playwright", "serial", "numpy")
               if not _importable(m)]
    line(not missing, "Python packages" + (f" (missing: {', '.join(missing)})" if missing else ""),
         "tools/.venv/bin/pip install -r tools/requirements.txt")
    try:
        from playwright.sync_api import sync_playwright
        with sync_playwright() as p:
            chrome = Path(p.chromium.executable_path).exists()
    except Exception:  # noqa
        chrome = False
    line(chrome, "Chromium for measuring pages (Playwright)", "tools/.venv/bin/playwright install chromium")
    line((O.BUILD / "boot.idb").exists() and (O.CARD / "system/os.idb").exists(), "OS image (os/build)",
         "iotdash os build")
    cmake = shutil.which("cmake")
    line(bool(cmake), "desktop simulator: cmake + a C++ compiler", "brew install cmake (Xcode command line tools)")
    sdl = (I.SIM_DIR / "build" / "CMakeCache.txt").exists() and \
        "SDL2_DIR:PATH=" in (I.SIM_DIR / "build" / "CMakeCache.txt").read_text(errors="replace") and \
        "SDL2_DIR-NOTFOUND" not in (I.SIM_DIR / "build" / "CMakeCache.txt").read_text(errors="replace")
    line(True if sdl else None, "simulator window (SDL2)" + ("" if sdl else ": not found, screenshots only"),
         "brew install sdl2, then delete sim/build")
    emsdk = shutil.which("em++") or (Path(os.environ.get("EMSDK", Path.home() / "emsdk")) / "emsdk_env.sh").exists()
    web = (I.ROOT / "web" / "dist" / "iotdash.wasm").exists()
    line(True if web else (None if not emsdk else False), "browser build (web/dist) for `iotdash run --web`",
         "web/build.sh" + ("" if emsdk else " (install the Emscripten SDK in ~/emsdk first)"))
    line(True if Path(I.PIO).exists() else None, "PlatformIO for the firmware", "https://platformio.org/install/cli")
    line(True if Path(I.PORT).exists() else None, f"board on USB ({I.PORT})", "plug it in (port: tools/iotdash.py PORT)")
    print("  the app store (store/, optional):")
    line(True if shutil.which("python3.13") or Path("/opt/homebrew/opt/python@3.13/bin/python3.13").exists() else None,
         "Python 3.13 for the store's API", "brew install python@3.13")
    line(True if shutil.which("npm") else None, "Node.js for the store's web app", "https://nodejs.org")
    line(True if shutil.which("aws") else None, "AWS CLI v2 for `iotdash store deploy`", "https://aws.amazon.com/cli/")
    print("ready" if ok_all else "fix the ✗ lines above")


def _importable(mod):
    import importlib.util
    return importlib.util.find_spec(mod) is not None


# ---------------------------------------------------------------- the commands

def main(args):
    cmd = args.cmd
    if cmd == "create":
        return create(args.dir, args.id, args.title)
    if cmd == "doctor":
        return doctor()
    if cmd == "apps":
        for name in O.card(args.to).list("/applications"):
            if not name.startswith("."):
                print(name)
        return
    if cmd == "uninstall":
        target = args.app
        if not target or Path(target).is_dir():
            target = O.app_meta(find(target))["id"]
        return O.uninstall_app(target, args.to)
    if cmd == "install" and args.pack:
        return O.install_pack(args.pack, args.to)
    if cmd == "publish" and args.pack:
        return _publish(args.pack, args)
    src = find(args.app)
    if cmd == "build":
        O.build_app(src)
    elif cmd == "install":
        O.install_app(src, args.to)
    elif cmd == "pack":
        O.pack_app(src, args.out)
    elif cmd == "publish":
        _publish(O.pack_app(src, args.out), args)
    elif cmd == "run":
        if args.device:
            run_device(src)
        elif args.web:
            run_web(src, args.port or 0, not args.no_open)
        else:
            run_sim(src, args.shot)
    elif cmd == "shot":
        shot(src, args.page, args.dark)


def _publish(path, args):
    import storeclient
    try:
        storeclient.publish(path, store=args.store, email=args.email, notes=args.notes, draft=args.draft)
    except storeclient.StoreError as e:
        raise SystemExit(f"publish: {e}")


def add_commands(sub):
    """The project commands of tools/iotdash.py."""
    app_help = "the app's folder (default: here) or an SDK example name"
    c = sub.add_parser("create", help="make a new app project")
    c.add_argument("dir")
    c.add_argument("--id", help="app id (default: from the folder name)")
    c.add_argument("--title", help="name on the home screen")
    for name, h in (("build", "compile into build/app/"), ("pack", "package for the store: build/<id>-<version>.idbz")):
        p = sub.add_parser(name, help=h)
        p.add_argument("app", nargs="?", help=app_help)
        p.add_argument("-o", "--out", help="output (pack: the .idbz path)")
    r = sub.add_parser("run", help="build, install and open it: simulator (default), --device or --web")
    r.add_argument("app", nargs="?", help=app_help)
    g = r.add_mutually_exclusive_group()
    g.add_argument("--device", action="store_true", help="the board over USB")
    g.add_argument("--web", action="store_true", help="the browser (WebAssembly build in web/dist)")
    r.add_argument("--shot", help="simulator: save a screenshot here instead of opening a window")
    r.add_argument("--port", type=int, help="--web: HTTP port (default: any free one)")
    r.add_argument("--no-open", action="store_true", help="--web: only print the address")
    i = sub.add_parser("install", help="build and copy to a card: sim (default), device or a mounted card")
    i.add_argument("app", nargs="?", help=app_help)
    i.add_argument("--to", help="sim (default), device, or the path of a mounted SD card")
    i.add_argument("--pack", help="install this .idbz as it is (what the store does)")
    u = sub.add_parser("uninstall", help="remove an app (and its data) from a card")
    u.add_argument("app", nargs="?", help="app id or folder (default: here)")
    u.add_argument("--to", help="sim (default), device, or the path of a mounted SD card")
    a = sub.add_parser("apps", help="the apps on a card")
    a.add_argument("--to", help="sim (default), device, or the path of a mounted SD card")
    p = sub.add_parser("publish", help="pack and upload to an iotstore")
    p.add_argument("app", nargs="?", help=app_help)
    p.add_argument("--pack", help="upload this .idbz instead of building")
    p.add_argument("-o", "--out", help="where the .idbz goes")
    p.add_argument("--store", help="the store's address (or IOTSTORE_URL)")
    p.add_argument("--email", help="the store account (or IOTSTORE_EMAIL; password: IOTSTORE_PASSWORD)")
    p.add_argument("--notes", default="", help="what changed in this version")
    p.add_argument("--draft", action="store_true", help="upload without publishing")
    s = sub.add_parser("shot", help="screenshots: the simulator next to Chromium (build/compare.png)")
    s.add_argument("app", nargs="?", help=app_help)
    s.add_argument("--page", default="main.html", help="page of the app (default main.html)")
    s.add_argument("--dark", action="store_true", help="dark mode")
    sub.add_parser("doctor", help="check the SDK's tools")
    return {"create", "build", "pack", "run", "install", "uninstall", "apps", "publish", "shot", "doctor"}
