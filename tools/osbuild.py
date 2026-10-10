"""iotdash OS: compile os/core and app projects into images and put them on an SD card.

  iotdash os build                       boot image (built into the firmware) + system image
  iotdash os deploy  [--to TARGET]       write /system to the card
  iotdash os format  [--to TARGET]       erase the card (keeps nothing)
  iotdash os sim     [--script S] [--shot out.png]   boot the OS in the simulator

App projects (a folder anywhere: app.json + main.html, see tools/project.py) are compiled by
build_app() into <project>/build/app/ and packed by pack_app() into <project>/build/<id>-<version>.idbz.

TARGET: "sim" (default: os/build/sdcard, the simulator's card), "device" (the board over USB)
or the path of a mounted SD card.

Images are IDB1 bundles like a plain app's: pages, CSS/JS, converted images and the font
atlases its pages use (measured with Chromium). An app image leaves out atlases the system
image already has, so apps share the system's fonts.
"""

import hashlib
import http.server
import json
import mimetypes
import re
import shutil
import socketserver
import struct
import subprocess
import threading
import time
from pathlib import Path

import iotdash as I

OS_DIR = I.ROOT / "os"
CORE = OS_DIR / "core"
EXAMPLES = I.ROOT / "examples"   # sample apps that come with the SDK
APP_ID = re.compile(r"^[a-z0-9][a-z0-9-]{1,39}$")
# files of a project that are not part of the app (top level)
PROJECT_ONLY = {"icon.png", "icon.svg", "README.md", "LICENSE", "CHANGELOG.md", "app.json"}
BUILD = OS_DIR / "build"
CARD = BUILD / "card"        # what a card should hold (staging)
SIM_CARD = BUILD / "sdcard"  # the simulator's card
ICON = 92                    # icon size on the home screen (px)
SKIP_DIRS = {"build", ".git", "node_modules", "__pycache__"}


def os_version():
    m = re.search(r'kVersion = "([^"]+)"', (I.ROOT / "lib/rt/os.h").read_text())
    return m.group(1) if m else "0"


# ---------------------------------------------------------------- local server for Chromium

class Server:
    """Serves /system/ (os/core), /app/ (the app being built) and local files (/__file/...) so
    pages see the same absolute paths as on the device."""

    def __init__(self, routes):
        self.routes = routes

    def __enter__(self):
        routes = self.routes

        class H(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                path = self.path.split("?")[0].split("#")[0]
                from urllib.parse import unquote
                path = unquote(path)
                target = None
                if path.startswith("/__file/"):
                    target = Path("/" + path[len("/__file/"):])
                else:
                    for prefix, d in routes.items():
                        if path.startswith(prefix):
                            target = Path(d) / path[len(prefix):]
                            break
                if not target or not target.is_file():
                    self.send_response(404)
                    self.end_headers()
                    return
                data = target.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", mimetypes.guess_type(str(target))[0] or "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *a):
                pass

        self.httpd = socketserver.ThreadingTCPServer(("127.0.0.1", 0), H)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        return f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def __exit__(self, *a):
        self.httpd.shutdown()
        self.httpd.server_close()


# Chromium has no OS: a stand-in API so pages render as on the device while being measured.
OS_STUB = r"""
(() => {
  const G = window;
  const noop = () => {};
  G.navigate = () => true;
  G.setBackgroundProcess = (name, scope, fn) => { try { fn({name, runs: 1}); } catch (e) {} return 1; };
  G.rmBackgroundProcess = () => true;
  G.clearBackgroundProcess = () => 0;
  class F { read() { return ''; } readLine() { return null; } readLines() { return []; } write() { return 0; }
    writeLine() {} seek() { return 0; } tell() { return 0; } get size() { return 0; } flush() {} close() {} }
  G.open = () => new F();
  const none = () => { throw new Error('no such file'); };
  G.fs = {open: G.open, readFile: none, readJSON: none, writeFile: noop, appendFile: noop, writeJSON: noop,
    exists: () => false, stat: () => null, list: () => [], mkdir: noop, remove: noop, rename: noop};
  G.registry = {get: (k, d) => (d === undefined ? null : d), keys: () => ['darkMode'], all: () => ({darkMode: false}),
    set: noop, remove: noop, clear: noop, onChange: () => noop};
  const icon = '/system/assets/default-icon.svg';
  const apps = [
    {id: 'settings', title: 'Settings', icon, system: true, version: '1.0'},
    {id: 'calculator', title: 'Calculator', icon, system: false, version: '1.0'},
    {id: 'farm', title: 'ฟาร์มเต้าหู้', icon, system: false, version: '1.0'},
  ];
  G.os = {version: '0', app: 'home', page: () => ({id: 'home', title: 'Home', page: 'main.html', depth: 1}),
    apps: () => apps, launch: noop, home: noop, back: noop, confirm: () => Promise.resolve(false),
    alert: () => Promise.resolve(), on: () => noop, emit: noop, processes: () => [], privileged: true,
    theme: () => 'light', uninstall: noop,
    sd: {info: () => ({mounted: true, total: 31914983424, free: 30064771072, type: 'SDHC', name: 'SD32G'}), format: noop}};
})();
"""


def analyze_pages(urls, fam_defs, base, thai_runs=(), ref_dir=None):
    from playwright.sync_api import sync_playwright
    inject = I.ref_inject_css(fam_defs).replace("file:///", base + "/__file/") + "\n" + I.ua_css()
    result = {"fonts": {}, "imgs": {}, "words": []}
    with sync_playwright() as p:
        browser = p.chromium.launch()
        ctx = browser.new_context(viewport={"width": 800, "height": 480}, device_scale_factor=1)
        page = ctx.new_page()
        page.add_init_script(OS_STUB)
        page.add_init_script(
            "document.addEventListener('DOMContentLoaded',()=>{const s=document.createElement('style');"
            "s.textContent=" + json.dumps(inject + "*{font-synthesis:none}") + ";document.head.prepend(s);});")
        for url in urls:
            page.goto(url)
            page.wait_for_timeout(300)
            r = page.evaluate(I.ANALYZE_JS)
            for k, v in r["fonts"].items():
                result["fonts"][k] = result["fonts"].get(k, "") + v
            for k, v in r["imgs"].items():
                result["imgs"][k] = max(result["imgs"].get(k, 0), v)
            if ref_dir:
                ref_dir.mkdir(parents=True, exist_ok=True)
                page.screenshot(path=str(ref_dir / (url.split("/", 3)[-1].replace("/", "_") + ".png")))
        if thai_runs:
            result["words"] = page.evaluate(I.SEGMENT_JS, list(thai_runs))
        browser.close()
    return result


def ref_shot(page, out, js=None, app=None, dark=False):
    """Chromium's rendering of an OS page (e.g. "system/apps/settings/main.html", or
    "app/main.html" with app=NAME): the reference the device's rendering should match.
    js runs after load (e.g. "showTab('system')")."""
    from playwright.sync_api import sync_playwright
    routes = {"/system/": CORE}
    if app:
        routes["/app/"] = Path(app)
    fam_defs = dict(I.FAMILIES)
    with Server(routes) as base:
        inject = I.ref_inject_css(fam_defs).replace("file:///", base + "/__file/") + "\n" + I.ua_css()
        with sync_playwright() as p:
            browser = p.chromium.launch()
            ctx = browser.new_context(viewport={"width": 800, "height": 480}, device_scale_factor=1,
                                      color_scheme="dark" if dark else "light")
            pg = ctx.new_page()
            pg.add_init_script(OS_STUB)
            pg.add_init_script(
                "document.addEventListener('DOMContentLoaded',()=>{const s=document.createElement('style');"
                "s.textContent=" + json.dumps(inject + "*{font-synthesis:none}") + ";document.head.prepend(s);});")
            pg.goto(base + "/" + page.lstrip("/"))
            pg.wait_for_timeout(300)
            if js:
                pg.evaluate(js)
                pg.wait_for_timeout(300)
            pg.screenshot(path=str(out))
            browser.close()
    return out


# ---------------------------------------------------------------- compiling an image

def render_icon(path, size=ICON):
    """App icon (SVG or bitmap) as a size x size image."""
    from PIL import Image
    if path.suffix.lower() == ".svg":
        from playwright.sync_api import sync_playwright
        with sync_playwright() as p:
            b = p.chromium.launch()
            pg = b.new_page(viewport={"width": size, "height": size})
            import base64
            uri = "data:image/svg+xml;base64," + base64.b64encode(path.read_bytes()).decode()
            pg.set_content(f"<html><body style='margin:0;background:transparent'><img src='{uri}'"
                           f" style='width:{size}px;height:{size}px;display:block'></body></html>")
            pg.wait_for_timeout(50)
            png = pg.screenshot(omit_background=True)
            b.close()
        import io
        return Image.open(io.BytesIO(png))
    im = Image.open(path).convert("RGBA")
    return im.resize((size, size), Image.LANCZOS)


def collect(src, skip=()):
    files, texts, pages, images = {}, "", [], []
    for p in sorted(src.rglob("*")):
        rel_parts = p.relative_to(src).parts
        if p.is_dir() or any(part in SKIP_DIRS or part.startswith(".") for part in rel_parts):
            continue
        if rel_parts[0] in skip:
            continue
        rel = p.relative_to(src).as_posix()
        ext = p.suffix.lower()
        if ext in (".html", ".htm", ".css", ".js", ".mjs", ".json", ".txt", ".svg"):
            data = p.read_bytes()
            texts += data.decode("utf-8", "replace")
            if ext in (".html", ".htm"):
                pages.append(rel)
            if ext == ".svg":
                images.append(rel)
                continue
            files[rel] = data
        elif ext in (".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"):
            images.append(rel)
        elif ext in (".ttf", ".otf"):
            pass
        else:
            files[rel] = p.read_bytes()
    return files, texts, pages, images


def make_fonts(info, texts, fam_defs, extra_sizes=(), system=None):
    """Atlases for every (family, weight, size) the pages use. With `system` (key -> chars of
    the system image's atlas), sizes the system already covers are left out."""
    all_chars = set(texts) | I.BASE_CHARS | I.THAI_CHARS | set(I.SYMBOLS)
    all_chars = {c for c in all_chars if ord(c) >= 0x20 and not (0xD800 <= ord(c) < 0xE000)}
    wanted = {}
    for key, text in info["fonts"].items():
        fam_css, w, size, style = key.split("|")
        fam = I.resolve_family(fam_css, fam_defs)
        try:
            w = int(float(w))
            size = round(float(size.replace("px", "")) * 4) / 4
        except ValueError:
            continue
        if 6 <= size <= 160:
            wanted.setdefault((fam, w, size), set()).update(text)
    for k in extra_sizes:
        wanted.setdefault(k, set())
    files, entries, charsets = {}, [], {}
    I.CACHE.mkdir(exist_ok=True)
    internal = [(x[1], x[2]) for x in I.INTERNAL_SIZES]
    for (fam, w, size), used in sorted(wanted.items()):
        if size <= 34:
            chars = set(all_chars)
        else:  # large text: digits/latin + what the pages show at this size
            chars = I.BASE_CHARS | set("°%฿€$−–—…") | (used & all_chars)
            if any("฀" <= c <= "๿" for c in used):
                chars |= I.THAI_CHARS
        if fam == "Inter" and (w, size) in internal:
            chars = chars | I.THAI_CHARS | set(I.SYMBOLS)
        skey = f"{fam}|{w}|{size}"
        if system is not None and skey in system and (used & all_chars) <= set(system[skey]):
            continue  # the system image has this atlas with every character used here
        d = fam_defs[fam]
        key = hashlib.sha1(json.dumps([fam, d, w, size, sorted(chars), I.TEXT_GAMMA, 5]).encode()).hexdigest()[:16]
        cache = I.CACHE / f"{key}.fnt"
        if cache.exists():
            data = cache.read_bytes()
        else:
            data = I.build_atlas(fam, w, size, chars, fam_defs)
            cache.write_bytes(data)
        name = f"fonts/{fam.replace(' ', '')}-{w}-{str(size).replace('.', '_')}.fnt"
        files[name] = data
        entries.append({"file": name, "family": fam, "weight": w, "size": size})
        charsets[skey] = "".join(sorted(chars))
    return files, entries, charsets


def compile_image(src, mount, routes, extra_files=None, extra_sizes=(), system=None, skip=(), log=print):
    """Pack directory `src` (served at `mount` while measured) into an IDB1 image."""
    from PIL import Image
    t0 = time.time()
    files, texts, pages, images = collect(src, skip)
    for rel, path in (extra_files or {}).items():
        data = Path(path).read_bytes()
        files[rel] = data
        texts += data.decode("utf-8", "replace")
    fam_defs = dict(I.FAMILIES)
    runs = I.collect_thai_runs(texts)
    with Server(routes) as base:
        info = analyze_pages([base + mount + p for p in pages], fam_defs, base, runs) if pages else \
            {"fonts": {}, "imgs": {}, "words": []}
    words = sorted(set(info["words"]))
    if words:
        files["thai.dict"] = "\n".join(words).encode()
    font_files, font_entries, charsets = make_fonts(info, texts, fam_defs, extra_sizes, system)
    files.update(font_files)
    for rel in images:
        p = src / rel
        try:
            im = I.render_svg(p) if rel.lower().endswith(".svg") else Image.open(p)
            im.load()
        except Exception as e:  # noqa
            log(f"  ! image {rel}: {e}")
            continue
        maxdim = max(info["imgs"].get(mount + rel, 0), info["imgs"].get(rel, 0))
        if maxdim and max(im.size) > maxdim * 2:
            sc = maxdim * 2 / max(im.size)
            im = im.resize((max(1, int(im.width * sc)), max(1, int(im.height * sc))), Image.LANCZOS)
        files[rel] = I.to_img1(im, rel.lower().endswith(".svg") and I.is_monochrome(im))
    files["manifest.json"] = json.dumps({
        "entry": "main.html", "fonts": font_entries,
        "aliases": {**I.ALIASES, **{f.lower(): f for f in fam_defs}}, "built": int(time.time()),
    }, ensure_ascii=False).encode()
    blob = I.pack_bundle(files)
    fonts_kb = sum(len(font_files[f["file"]]) for f in font_entries) / 1024
    log(f"  {shown(src)}: {len(blob) / 1024:.0f} KB ({len(files)} files, {len(font_entries)} atlases "
        f"{fonts_kb:.0f} KB, {len(words)} Thai words) in {time.time() - t0:.1f}s")
    return blob, charsets


def shown(path):
    """A path as the user would write it: relative to the current folder or the SDK when inside them."""
    path = Path(path).resolve()
    for base in (Path.cwd(), I.ROOT):
        try:
            rel = path.relative_to(base)
            return rel.as_posix() or "."
        except ValueError:
            pass
    return str(path)


def digest(blob):
    return hashlib.sha1(blob).hexdigest()[:16]


# ---------------------------------------------------------------- os / app builds

def build_os(log=print):
    log("system image")
    routes = {"/system/": CORE}
    sys_dir = CARD / "system"
    sys_dir.mkdir(parents=True, exist_ok=True)
    default_icon = I.to_img1(render_icon(CORE / "assets/default-icon.svg"))
    extra = {}
    tmp = BUILD / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    (tmp / "default-icon.img").write_bytes(default_icon)
    extra["assets/default-icon.img"] = tmp / "default-icon.img"
    for app in sorted((CORE / "apps").iterdir()):  # core apps' icons at home-screen size
        for name in ("icon.svg", "icon.png"):
            if (app / name).exists():
                out = tmp / f"{app.name}-icon.img"
                out.write_bytes(I.to_img1(render_icon(app / name)))
                extra[f"apps/{app.name}/icon.img"] = out
                break
    blob, charsets = compile_image(CORE, "/system/", routes, extra_files=extra,
                                   extra_sizes=I.INTERNAL_SIZES + I.DEFAULT_SIZES, skip={"boot"}, log=log)
    (sys_dir / "os.idb").write_bytes(blob)
    (sys_dir / "assets").mkdir(exist_ok=True)
    (sys_dir / "assets/default-icon.img").write_bytes(default_icon)
    meta = {"version": os_version(), "hash": digest(blob), "size": len(blob), "built": int(time.time())}
    (sys_dir / "os.json").write_text(json.dumps(meta))
    (BUILD / "system-fonts.json").write_text(json.dumps(charsets, ensure_ascii=False))
    log("boot image")
    boot, _ = compile_image(CORE / "boot", "/boot/", {"/boot/": CORE / "boot", "/system/": CORE},
                            extra_files={"os.js": CORE / "os.js"}, log=log)
    (BUILD / "boot.idb").write_bytes(boot)
    log(f"os {meta['version']} ({meta['hash']}): {BUILD.relative_to(I.ROOT)}/card/system, boot.idb")
    return meta


def system_fonts():
    p = BUILD / "system-fonts.json"
    if not p.exists():
        build_os()
    return json.loads(p.read_text())


def app_meta(src):
    """app.json of a project with its defaults: id (the folder's name), title, version."""
    src = Path(src)
    meta = {"title": src.name, "version": "1.0"}
    if (src / "app.json").exists():
        try:
            meta.update(json.loads((src / "app.json").read_text(encoding="utf-8")))
        except ValueError as e:
            raise SystemExit(f"{shown(src / 'app.json')}: {e}")
    meta.setdefault("id", src.name)
    if not APP_ID.match(str(meta["id"])):
        raise SystemExit(f'{shown(src / "app.json")}: id "{meta["id"]}" must be 2-40 of a-z, 0-9 and "-" '
                         '(it names the app on the card and in the store)')
    return meta


def build_app(src, out=None, log=print):
    """Compile an app project into OUT (default <project>/build/app/): app.idb, app.json, icon.img."""
    src = Path(src).resolve()
    if not (src / "main.html").exists():
        raise SystemExit(f"{shown(src)} has no main.html")
    meta = app_meta(src)
    log(f"app {meta['id']} {meta['version']} ({shown(src)})")
    # the icon becomes icon.img next to the image (home screen); the pages do not use it
    blob, _ = compile_image(src, "/app/", {"/app/": src, "/system/": CORE}, system=system_fonts(),
                            skip=PROJECT_ONLY, log=log)
    out = Path(out) if out else src / "build" / "app"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    (out / "app.idb").write_bytes(blob)
    meta.update({"hash": digest(blob), "size": len(blob), "built": int(time.time())})
    (out / "app.json").write_text(json.dumps(meta, ensure_ascii=False))
    icon = next((src / n for n in ("icon.svg", "icon.png") if (src / n).exists()), CORE / "assets/default-icon.svg")
    (out / "icon.img").write_bytes(I.to_img1(render_icon(icon)))
    return out


# ---------------------------------------------------------------- cards

class DirCard:
    """A card as a local directory: the simulator's, or a mounted SD card."""

    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def put(self, path, data):
        p = self.root / path.lstrip("/")
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def mkdir(self, path):
        (self.root / path.lstrip("/")).mkdir(parents=True, exist_ok=True)

    def remove(self, path):
        p = self.root / path.lstrip("/")
        if p.is_dir():
            shutil.rmtree(p)
        elif p.exists():
            p.unlink()

    def list(self, path):
        p = self.root / path.lstrip("/")
        return sorted(x.name for x in p.iterdir()) if p.is_dir() else []

    def read(self, path):
        p = self.root / path.lstrip("/")
        return p.read_bytes() if p.exists() else None

    def format(self):
        for x in self.root.iterdir():
            shutil.rmtree(x) if x.is_dir() else x.unlink()

    def done(self, app=None):
        # macOS writes "._name" AppleDouble files next to everything it copies to a FAT card
        for top in ("system", "applications"):
            for f in (self.root / top).rglob("._*"):
                if f.is_file():
                    f.unlink()
        for f in self.root.glob("._*"):
            if f.is_file():
                f.unlink()


class DeviceCard:
    """The board's card over the dev link (see src/main.cpp: FS_* messages)."""

    CHUNK = 4 * 1024  # the board's serial RX buffer is 6 KB and it reads between frames

    def __init__(self):
        self.link = I.Link(echo=False)
        if not self.link.wait_ready():
            raise SystemExit("board did not answer")

    def _req(self, typ, payload, what, timeout=20):
        r = self.link.request(typ, payload, timeout=timeout)
        if not r:
            raise SystemExit(f"{what}: no answer from the board")
        if r[1] != 1:
            raise SystemExit(f"{what}: {r[2:].decode('utf-8', 'replace')}")
        return r[2:].decode("utf-8", "replace")

    def put(self, path, data):
        p = path.encode()
        total = len(data)
        off = 0
        t0 = time.time()
        while True:
            chunk = data[off:off + self.CHUNK]
            flags = (1 if off == 0 else 0) | (2 if off + len(chunk) >= total else 0)
            payload = struct.pack("<BH", flags, len(p)) + p + struct.pack("<I", off) + chunk
            self._req(I.T_FS_WRITE, payload, f"write {path}")
            off += len(chunk)
            if off >= total:
                break
        if total > 64 * 1024:
            print(f"    {path}: {total / 1024:.0f} KB in {time.time() - t0:.1f}s")

    def mkdir(self, path):
        self._req(I.T_FS_MKDIR, path.encode(), f"mkdir {path}")

    def remove(self, path):
        self._req(I.T_FS_DELETE, path.encode(), f"remove {path}")

    def list(self, path):
        return json.loads(self._req(I.T_FS_LIST, path.encode(), f"list {path}") or "[]")

    def read(self, path):
        return None  # not needed by the commands

    def format(self):
        self._req(I.T_FS_FORMAT, b"", "format", timeout=120)

    def done(self, app=None):
        """Tell the running OS: an app changed (home lists it again), else the system (reboot it)."""
        if app:
            self._req(I.T_APPS_CHANGED, app.encode(), "refresh")
        else:
            self._req(I.T_OS_RELOAD, b"", "reload")


def card(target):
    if target in (None, "sim"):
        return DirCard(SIM_CARD)
    if target == "device":
        return DeviceCard()
    return DirCard(target)


def deploy_os(target, log=print):
    if not (CARD / "system/os.idb").exists():
        build_os(log)
    c = card(target)
    for f in sorted((CARD / "system").rglob("*")):
        if f.is_file():
            c.put("/system/" + f.relative_to(CARD / "system").as_posix(), f.read_bytes())
    c.mkdir("/system/data")
    c.mkdir("/applications")
    c.done()
    log(f"system deployed to {target or 'sim'}")


def _install_files(app_id, files, source, version, target, log, c=None):
    c = c or card(target)
    # app.idb, icon.img and app.json; the app's memory.json and data/ stay
    for f in ("app.idb", "icon.img", "app.json"):
        c.put(f"/applications/{app_id}/{f}", files[f])
    # where it came from: the store app ("Apps") tells these from its own installs
    c.put(f"/applications/{app_id}/install.json",
          json.dumps({"source": source, "version": version, "installed": int(time.time())}).encode())
    c.mkdir(f"/applications/{app_id}/data")
    c.done(app_id)
    log(f"{app_id} {version} installed on {target or 'sim'}")
    return c


def install_app(src, target, log=print, c=None):
    """Build a project and copy it to /applications/<id> on TARGET; returns (app id, the card)."""
    out = build_app(src, log=log)
    files = {f: (out / f).read_bytes() for f in ("app.idb", "icon.img", "app.json")}
    meta = json.loads(files["app.json"])
    return meta["id"], _install_files(meta["id"], files, "usb", meta.get("version", ""), target, log, c)


def pack_app(src, out=None, log=print):
    """Build a project and package it (tools/idbz.py): <project>/build/<id>-<version>.idbz."""
    import idbz
    src = Path(src).resolve()
    built = build_app(src, log=log)
    files = {f: (built / f).read_bytes() for f in ("app.json", "icon.img", "app.idb")}
    app = json.loads(files["app.json"])
    manifest = {
        "id": app["id"], "title": app.get("title", app["id"]), "version": app.get("version", "1.0"),
        "description": app.get("description", ""),
        # the OS the image was built for, and the oldest one the app says it runs on
        "os": {"built": os_version(), "min": (app.get("os") or {}).get("min", os_version())},
        "built": app.get("built", int(time.time())),
    }
    blob = idbz.pack(manifest, files)
    path = Path(out) if out else src / "build" / f"{manifest['id']}-{manifest['version']}.idbz"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)
    log(f"  {shown(path)}: {len(blob) / 1024:.0f} KB (IDBZ, {manifest['title']} {manifest['version']}, OS {manifest['os']['built']})")
    return path


def install_pack(path, target, log=print):
    """Install a package file (.idbz) as it is: what the store would install, over USB or to the sim."""
    import idbz
    manifest, files = idbz.unpack(Path(path).read_bytes())
    return manifest["id"], _install_files(manifest["id"], files, "usb", manifest["version"], target, log)


def uninstall_app(name, target, log=print):
    c = card(target)
    if name not in c.list("/applications"):
        raise SystemExit(f"{name} is not installed on {target or 'sim'}")
    c.remove(f"/applications/{name}")
    c.done(name)
    log(f"{name} removed from {target or 'sim'}")


def format_card(target, log=print):
    c = card(target)
    c.format()
    c.mkdir("/system/data")
    c.mkdir("/applications")
    log(f"card {target or 'sim'} formatted")


def run_sim(script=None, shot=None):
    if not (BUILD / "boot.idb").exists():
        build_os()
    if not (SIM_CARD / "system/os.idb").exists():
        deploy_os("sim")
    I.build_sim()
    cmd = [str(I.SIM_BIN), "--os", str(SIM_CARD), "--boot", str(BUILD / "boot.idb")]
    if script:
        cmd += ["--script", script]
    if shot:
        cmd += ["--shot", shot]
    subprocess.run(cmd)


def main_os(args):
    if args.what == "build":
        build_os()
    elif args.what == "deploy":
        if args.build:
            build_os()
        deploy_os(args.to)
    elif args.what == "format":
        format_card(args.to)
    elif args.what == "sim":
        run_sim(args.script, args.shot)
    elif args.what == "ref":
        print(ref_shot(args.page, args.out or "ref.png", args.js, args.app, args.dark))

