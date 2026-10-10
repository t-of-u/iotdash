#!/usr/bin/env python3
"""iotdash SDK: apps for iotdash boards (ESP32-S3 7" panel), the OS, the simulators, the board.

Apps are projects anywhere (app.json + main.html, see tools/project.py); run these inside one,
or give its folder (or the name of an SDK example) as APP:

  iotdash create DIR                 a new app project
  iotdash run [APP]                  build, install and open it in the simulator
              [--device | --web]     ... on the board over USB, or in the browser
  iotdash build | pack [APP]         compile (build/app/) | package for the store (build/<id>-<ver>.idbz)
  iotdash install [APP] [--to T]     onto a card: sim (default), device, or a mounted SD card
  iotdash uninstall [APP|ID] | apps  remove one | list them
  iotdash publish [APP] --store URL  pack and upload to an iotstore
  iotdash shot [APP] [--page P]      the simulator's rendering next to Chromium's
  iotdash doctor                     check the SDK's tools

The SDK itself:

  iotdash os build|deploy|format|sim the OS image (tools/osbuild.py)
  iotdash store dev|test|deploy|deploy-web   the app store (store/): run it here, deploy it to AWS
  iotdash firmware [build|upload|monitor]   the board's firmware (PlatformIO)
  iotdash device shot|stats|eval|tap|log|reset|...   the board over USB
  iotdash legacy build|sim|shot|flash|dev APP   single-app bundles from before the OS

The device never parses fonts or images: the compiler rasterizes exactly the font sizes and
weights the pages use (measured with Chromium) and converts images to RGB565.
"""

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
FONT_DIR = TOOLS / "fonts"
CACHE = ROOT / ".build-cache"

# ---------------------------------------------------------------- fonts

FAMILIES = {
    # name: primary file, fallback for missing glyphs (Thai), axes
    "Inter": {"file": "Inter.ttf", "fallback": "NotoSansThai.ttf"},
    "Noto Sans Thai": {"file": "NotoSansThai.ttf", "fallback": "Inter.ttf"},
    "JetBrains Mono": {"file": "JetBrainsMono.ttf", "fallback": "NotoSansThai.ttf"},
}
ALIASES = {
    "sans-serif": "Inter", "system-ui": "Inter", "-apple-system": "Inter", "blinkmacsystemfont": "Inter",
    "segoe ui": "Inter", "roboto": "Inter", "helvetica": "Inter", "helvetica neue": "Inter", "arial": "Inter",
    "ui-sans-serif": "Inter", "serif": "Inter", "georgia": "Inter", "times": "Inter", "times new roman": "Inter",
    "cursive": "Inter", "fantasy": "Inter", "emoji": "Inter", "apple color emoji": "Inter",
    "noto sans": "Inter", "open sans": "Inter", "lato": "Inter", "montserrat": "Inter",
    "monospace": "JetBrains Mono", "ui-monospace": "JetBrains Mono", "menlo": "JetBrains Mono",
    "monaco": "JetBrains Mono", "consolas": "JetBrains Mono", "courier": "JetBrains Mono",
    "courier new": "JetBrains Mono", "sfmono-regular": "JetBrains Mono", "sf mono": "JetBrains Mono",
    "sarabun": "Noto Sans Thai", "prompt": "Noto Sans Thai", "kanit": "Noto Sans Thai", "thonburi": "Noto Sans Thai",
    "noto sans thai looped": "Noto Sans Thai", "ibm plex sans thai": "Noto Sans Thai", "anuphan": "Noto Sans Thai",
    "tahoma": "Noto Sans Thai", "leelawadee": "Noto Sans Thai", "leelawadee ui": "Noto Sans Thai",
}

SYMBOLS = (" °±×÷·•–—‘’“”…€£¥©®™฿←↑→↓↵⌫⇧⌄‹›▲▼◀▶◌✓✔✕✗★☆●○▪◦−µ²³½¼¾¢§¶«»"
           "¿¡ⓘ⚙⚠✎✉☰⋯⋮↻↺⏻⏸⏵⏹♥♡☀☁☂❄☾​�")
BASE_CHARS = set(chr(c) for c in range(0x20, 0x7F))
THAI_CHARS = set(chr(c) for c in list(range(0x0E01, 0x0E3B)) + list(range(0x0E3F, 0x0E5C)))
THAI_BASES = [chr(c) for c in range(0x0E01, 0x0E2F)] + ["ฯ"]  # consonants (+ ฯ)
UPPER = [0x0E31, 0x0E34, 0x0E35, 0x0E36, 0x0E37, 0x0E47, 0x0E4D, 0x0E4E]
TONES = [0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C]
LOWER = [0x0E38, 0x0E39, 0x0E3A]
TK_AFTER_UPPER, TK_AFTER_AM, TK_BASE_VARIANT = 0x8000, 0x4000, 0xFFFF


def resolve_family(css_family_list, extra_families):
    """First family of a CSS font-family list that we can provide."""
    for raw in css_family_list.split(","):
        name = raw.strip().strip("\"'")
        low = name.lower()
        for fam in list(FAMILIES) + list(extra_families):
            if fam.lower() == low:
                return fam
        if low in ALIASES:
            return ALIASES[low]
    return "Inter"


class FontSource:
    """A (variable) font file opened with FreeType and HarfBuzz."""

    def __init__(self, path):
        import freetype
        import uharfbuzz as hb
        self.path = str(path)
        self.ft = freetype.Face(self.path)
        blob = hb.Blob.from_file_path(self.path)
        self.hbface = hb.Face(blob)
        self.hb = hb.Font(self.hbface)
        self.upem = self.hbface.upem
        from fontTools.ttLib import TTFont
        tt = TTFont(self.path, lazy=True)
        self.cmap = tt.getBestCmap()
        self.axes = [(a.axisTag, a.minValue, a.defaultValue, a.maxValue) for a in tt["fvar"].axes] if "fvar" in tt else []
        hhea = tt["hhea"]
        os2 = tt["OS/2"]
        self.ascender, self.descender, self.linegap = hhea.ascent, hhea.descent, hhea.lineGap
        self.xheight = getattr(os2, "sxHeight", 0) or int(self.upem * 0.5)
        self.capheight = getattr(os2, "sCapHeight", 0) or int(self.upem * 0.7)
        post = tt["post"]
        self.ul_pos, self.ul_thick = post.underlinePosition, post.underlineThickness

    def set_instance(self, weight, size):
        coords, hbvars = [], {}
        for tag, mn, df, mx in self.axes:
            if tag == "wght":
                v = max(mn, min(mx, weight))
            elif tag == "opsz":
                v = max(mn, min(mx, size))
            else:
                v = df
            coords.append(v)
            hbvars[tag] = v
        if coords:
            self.ft.set_var_design_coords(coords)
            self.hb.set_variations(hbvars)
        self.ft.set_char_size(int(round(size * 64)))
        self.size = size

    def has(self, ch):
        return ord(ch) in self.cmap

    def gid(self, ch):
        g = self.hb.get_nominal_glyph(ord(ch))
        return g if g is not None else 0

    def render(self, gid):
        import freetype
        self.ft.load_glyph(gid, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_NO_HINTING)
        g = self.ft.glyph
        bm = g.bitmap
        w, h, pitch = bm.width, bm.rows, bm.pitch
        buf = bytes(bm.buffer)
        rows = b"".join(buf[r * pitch:r * pitch + w] for r in range(h)).translate(GAMMA)
        adv = g.linearHoriAdvance / 65536.0
        return adv, g.bitmap_left, g.bitmap_top, w, h, rows

    def shape(self, text):
        import uharfbuzz as hb
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        hb.shape(self.hb, buf, {"kern": True, "liga": False})
        return [(i.codepoint, p.x_advance, p.x_offset, p.y_offset) for i, p in zip(buf.glyph_infos, buf.glyph_positions)]


_sources = {}

# Coverage boost: the panel shows straight sRGB blending, which makes thin stems look lighter
# than macOS/Chromium text; a mild gamma on the glyph alpha brings the weight back.
TEXT_GAMMA = 0.72
GAMMA = bytes(min(255, int(round(255 * (i / 255) ** TEXT_GAMMA))) for i in range(256))


def source(fname):
    if fname not in _sources:
        p = Path(fname)
        if not p.is_absolute():
            p = FONT_DIR / fname
        _sources[fname] = FontSource(p)
    return _sources[fname]


def build_atlas(family, weight, size, chars, fam_defs):
    """Rasterize one FNT1 atlas. Returns bytes."""
    d = fam_defs[family]
    prim = source(d["file"])
    fb = source(d["fallback"]) if d.get("fallback") else None
    prim.set_instance(weight, size)
    if fb:
        fb.set_instance(weight, size)
    scale_p = size / prim.upem
    glyphs = {}  # cp -> (adv, bx, by, w, h, bitmap, flags)
    variants = {}  # (srcIndex, gid) -> cp

    def rast(src, gid, fallback):
        adv, left, top, w, h, bm = src.render(gid)
        return (adv, left, top, w, h, bm, 1 if fallback else 0)

    for ch in sorted(chars):
        cp = ord(ch)
        if cp == 0x200B:
            continue
        if prim.has(ch):
            glyphs[cp] = rast(prim, prim.gid(ch), False)
        elif fb and fb.has(ch):
            glyphs[cp] = rast(fb, fb.gid(ch), True)
    # Thai placement table (HarfBuzz) from whichever face provides Thai
    thai_src = prim if prim.has("ก") else (fb if fb and fb.has("ก") else None)
    thai_rows = []
    if thai_src and any(ord(c) in range(0x0E01, 0x0E5C) for c in chars):
        is_fb = thai_src is fb
        sc = size / thai_src.upem
        nominal = {}

        def variant_cp(gid):
            key = (id(thai_src), gid)
            if key not in variants:
                vcp = 0x110000 + len(variants)
                variants[key] = vcp
                glyphs[vcp] = rast(thai_src, gid, is_fb)
            return variants[key]

        def entry(base, key, text, idx):
            sh = thai_src.shape(text)
            if len(sh) <= idx:
                return None
            gid, xadv, xoff, yoff = sh[idx]
            mark_cp = key & 0x0FFF | 0x0E00 if key != TK_BASE_VARIANT else None
            glyph = 0
            if key == TK_BASE_VARIANT:
                if gid == thai_src.gid(base):
                    return None
                glyph = variant_cp(gid)
                return (ord(base), key, glyph, 0, 0)
            nominal_gid = thai_src.gid(chr(mark_cp))
            if gid != nominal_gid:
                glyph = variant_cp(gid)
            # offsets are relative to the pen after the base (marks have no advance)
            return (ord(base), key, glyph, int(round(xoff * sc * 64)), int(round(yoff * sc * 64)))

        for base in THAI_BASES:
            for m in UPPER:
                e = entry(base, m, base + chr(m), 1)
                if e: thai_rows.append(e)
            for t in TONES:
                e = entry(base, t, base + chr(t), 1)
                if e: thai_rows.append(e)
                e = entry(base, t | TK_AFTER_UPPER, base + "ิ" + chr(t), 2)
                if e: thai_rows.append(e)
                # base + tone + SARA AM: HarfBuzz emits base, nikhahit, tone, aa
                e = entry(base, t | TK_AFTER_AM, base + chr(t) + "ำ", 2)
                if e: thai_rows.append(e)
            for l in LOWER:
                e = entry(base, l, base + chr(l), 1)
                if e: thai_rows.append(e)
            e = entry(base, TK_BASE_VARIANT, base + "ุ", 0)
            if e: thai_rows.append(e)
        # keep ก entries; drop other bases' entries identical to ก for the same key
        ref = {(k): (g, dx, dy) for (b, k, g, dx, dy) in thai_rows if b == 0x0E01}
        thai_rows = [r for r in thai_rows if r[0] == 0x0E01 or ref.get(r[1]) != (r[2], r[3], r[4])]
        thai_rows.sort(key=lambda r: (r[0] << 16) | r[1])
    # kerning (Latin pairs) from HarfBuzz
    kerns = []
    kchars = [c for c in sorted(chars) if 0x20 < ord(c) < 0x7F and prim.has(c)]
    adv_unit = {}
    for c in kchars:
        sh = prim.shape(c)
        adv_unit[c] = sh[0][1] if sh else 0
    letters = [c for c in kchars if c.isalnum() or c in ".,:;'\"-()/"]
    for a in letters:
        for b in letters:
            sh = prim.shape(a + b)
            if len(sh) != 2:
                continue
            k = sh[0][1] - adv_unit[a]
            if k != 0:
                v = int(round(k * scale_p * 64))
                if v != 0:
                    kerns.append(((ord(a) << 16) | ord(b), v))
    kerns.sort()
    # metrics of the instanced face (variable fonts may vary them with MVAR, e.g. Inter's opsz)
    asc = prim.ft.ascender * scale_p
    desc = -prim.ft.descender * scale_p
    gap = max(0, prim.ft.height - prim.ft.ascender + prim.ft.descender) * scale_p
    fba = (fb.ft.ascender * size / fb.upem) if fb else asc
    fbd = (-fb.ft.descender * size / fb.upem) if fb else desc
    # pack
    cps = sorted(glyphs)
    hdr_fmt = "<4sHH" + "f" * 10 + "IIIIIIIHBB32s"
    hdr_size = struct.calcsize(hdr_fmt)
    gfmt = "<IhhhHHHI"
    gsize = struct.calcsize(gfmt)
    glyph_off = hdr_size
    kern_off = glyph_off + gsize * len(cps)
    thai_off = kern_off + 8 * len(kerns)
    bmp_off = thai_off + 12 * len(thai_rows)
    bitmaps = bytearray()
    gbytes = bytearray()
    for cp in cps:
        adv, left, top, w, h, bm, fl = glyphs[cp]
        flags = fl | (2 if 0x0E31 <= cp <= 0x0E4E or cp >= 0x110000 else 0)
        gbytes += struct.pack(gfmt, cp, int(round(adv * 64)), left, top, w, h, flags, len(bitmaps))
        bitmaps += bm
    kbytes = b"".join(struct.pack("<IhH", p, v, 0) for p, v in kerns)
    tbytes = b"".join(struct.pack("<HHIhh", b, k, g, dx, dy) for (b, k, g, dx, dy) in thai_rows)
    fam = family.encode()[:31]
    hdr = struct.pack(hdr_fmt, b"FNT1", 1, 0, float(size), asc, desc, gap, prim.xheight * scale_p,
                      prim.capheight * scale_p, -prim.ul_pos * scale_p, prim.ul_thick * scale_p, fba, fbd,
                      len(cps), len(kerns), len(thai_rows), glyph_off, kern_off, thai_off, bmp_off,
                      int(weight), 0, 0, fam)
    return bytes(hdr + gbytes + kbytes + tbytes + bitmaps)


# ---------------------------------------------------------------- images

def to_img1(im, mask=False):
    """PIL image -> IMG1 (RGB565 little endian + optional A8; or A8 mask)."""
    import numpy as np
    im = im.convert("RGBA")
    a = np.asarray(im, dtype=np.float32)
    h, w = a.shape[:2]
    alpha = a[:, :, 3].astype(np.uint8)
    if mask:
        return struct.pack("<4sHHHH", b"IMG1", w, h, 2, 0) + alpha.tobytes()
    # ordered dither (4x4 Bayer) before truncating to 565
    bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], dtype=np.float32) / 16.0 - 0.5
    t = np.tile(bayer, (h // 4 + 1, w // 4 + 1))[:h, :w]
    r = np.clip(a[:, :, 0] + t * 8, 0, 255).astype(np.uint16)
    g = np.clip(a[:, :, 1] + t * 4, 0, 255).astype(np.uint16)
    b = np.clip(a[:, :, 2] + t * 8, 0, 255).astype(np.uint16)
    px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    has_alpha = bool((alpha < 255).any())
    out = struct.pack("<4sHHHH", b"IMG1", w, h, 1 if has_alpha else 0, 0) + px.astype("<u2").tobytes()
    if has_alpha:
        out += alpha.tobytes()
    return out


def is_monochrome(im):
    import numpy as np
    a = np.asarray(im.convert("RGBA"))
    vis = a[:, :, 3] > 16
    if not vis.any():
        return False
    rgb = a[:, :, :3][vis].astype(int)
    return int((rgb.max(axis=0) - rgb.min(axis=0)).max()) < 24


# ---------------------------------------------------------------- Chromium analysis

ANALYZE_JS = r"""
() => {
  const out = {};
  const add = (fam, w, size, style, text) => {
    const k = fam + '|' + w + '|' + size + '|' + style;
    out[k] = (out[k] || '') + text;
  };
  for (const el of document.querySelectorAll('*')) {
    const cs = getComputedStyle(el);
    let t = '';
    for (const n of el.childNodes) if (n.nodeType === 3) t += n.data;
    if (el.placeholder) t += el.placeholder;
    if (el.value && typeof el.value === 'string') t += el.value;
    if (el.tagName === 'OPTION' || el.tagName === 'SELECT') t += el.textContent;
    for (const pe of ['::before', '::after']) {
      const p = getComputedStyle(el, pe);
      if (p.content && p.content !== 'none' && p.content !== 'normal') {
        add(p.fontFamily, p.fontWeight, p.fontSize, p.fontStyle, p.content.replace(/^"|"$/g, ''));
      }
    }
    add(cs.fontFamily, cs.fontWeight, cs.fontSize, cs.fontStyle, t);
  }
  const imgs = {};
  for (const im of document.querySelectorAll('img')) {
    const r = im.getBoundingClientRect();
    const src = im.getAttribute('src') || '';
    imgs[src] = Math.max(imgs[src] || 0, r.width, r.height);
  }
  return {fonts: out, imgs};
}
"""

SEGMENT_JS = r"""
(runs) => {
  const seg = new Intl.Segmenter('th', {granularity: 'word'});
  const words = new Set();
  for (const r of runs) for (const s of seg.segment(r)) if (s.isWordLike) words.add(s.segment);
  return [...words];
}
"""


def ua_css():
    src = (ROOT / "lib/rt/style.cpp").read_text()
    m = re.search(r'kUserAgentCss = R"CSS\((.*?)\)CSS"', src, re.S)
    return m.group(1) if m else ""


THAI_RANGE = "U+0E00-0E7F, U+25CC"


def _face_rules(name, d):
    """@font-face rules making `name` render like the device atlas: primary font plus the
    fallback face for Thai (unicode-range), so Chromium never falls back to a system font."""
    def url(f):
        p = Path(f)
        return (p if p.is_absolute() else FONT_DIR / p).as_uri()
    rules = [f"@font-face {{ font-family: '{name}'; src: url('{url(d['file'])}'); font-weight: 100 900; }}"]
    fb = d.get("fallback")
    if fb and not source(d["file"]).has("\u0E01"):
        rules.append(f"@font-face {{ font-family: '{name}'; src: url('{url(fb)}'); font-weight: 100 900;"
                     f" unicode-range: {THAI_RANGE}; }}")
    return rules


def font_face_css(fam_defs):
    css = []
    for fam, d in fam_defs.items():
        css += _face_rules(fam, d)
    return "\n".join(css)


def ref_inject_css(fam_defs):
    # Same fonts as the device: bundled families, and every alias (incl. generic names via the
    # html default) mapped onto them, each with the Thai fallback face.
    css = font_face_css(fam_defs).split("\n")
    for alias, fam in ALIASES.items():
        if alias in ("sans-serif", "serif", "monospace", "system-ui", "cursive", "fantasy", "emoji", "ui-monospace",
                     "ui-sans-serif"):
            continue
        if fam in fam_defs:
            css += _face_rules(alias, fam_defs[fam])
    # generic families cannot be redefined: make the root default match the device
    css.append("html { font-family: Inter; }")
    return "\n".join(css)


def analyze(app, pages, fam_defs, ref_dir=None, thai_runs=()):
    from playwright.sync_api import sync_playwright
    result = {"fonts": {}, "imgs": {}, "words": []}
    inject = ref_inject_css(fam_defs) + "\n" + ua_css()
    # map generic families onto the bundled fonts inside Chromium
    generic = ("*{font-synthesis:none}")
    with sync_playwright() as p:
        browser = p.chromium.launch()
        ctx = browser.new_context(viewport={"width": 800, "height": 480}, device_scale_factor=1)
        page = ctx.new_page()
        page.add_init_script(
            "document.addEventListener('DOMContentLoaded',()=>{const s=document.createElement('style');"
            "s.textContent=" + json.dumps(inject + generic) + ";document.head.prepend(s);});")
        for html in pages:
            page.goto((app / html).resolve().as_uri())
            page.wait_for_timeout(250)
            r = page.evaluate(ANALYZE_JS)
            for k, v in r["fonts"].items():
                result["fonts"][k] = result["fonts"].get(k, "") + v
            for k, v in r["imgs"].items():
                result["imgs"][k] = max(result["imgs"].get(k, 0), v)
            if ref_dir:
                ref_dir.mkdir(parents=True, exist_ok=True)
                page.screenshot(path=str(ref_dir / (html.replace("/", "_") + ".png")))
        if thai_runs:
            result["words"] = page.evaluate(SEGMENT_JS, list(thai_runs))
        browser.close()
    return result


# ---------------------------------------------------------------- bundle

def collect_thai_runs(text):
    return set(re.findall(r"[฀-๿]{2,}", text))


def pack_bundle(files):
    """files: dict path -> bytes. Returns IDB1 bytes with 4-byte aligned data."""
    names = sorted(files)
    header = bytearray(b"IDB1" + struct.pack("<I", len(names)))
    dir_size = sum(2 + len(n.encode()) + 8 for n in names)
    off = len(header) + dir_size
    off = (off + 3) & ~3
    entries, data = [], bytearray()
    for n in names:
        b = files[n]
        while (off + len(data)) % 4:
            data += b"\0"
        entries.append((n, off + len(data), len(b)))
        data += b
    for n, o, s in entries:
        nb = n.encode()
        header += struct.pack("<H", len(nb)) + nb + struct.pack("<II", o, s)
    while len(header) % 4:
        header += b"\0"
    assert len(header) <= entries[0][1] if entries else True
    pad = entries[0][1] - len(header) if entries else 0
    return bytes(header + b"\0" * pad + data)


INTERNAL_SIZES = [  # (family, weight, size) used by the runtime's own UI (keyboard, pickers)
    ("Inter", 400, 21), ("Inter", 400, 15), ("Inter", 400, 16), ("Inter", 400, 24), ("Inter", 400, 18),
    ("Inter", 600, 18), ("Inter", 600, 16), ("Inter", 600, 13), ("Inter", 600, 44), ("Inter", 400, 40),
    ("Inter", 400, 13), ("Inter", 700, 16),
]
DEFAULT_SIZES = [("Inter", 400, 14), ("Inter", 400, 16), ("Inter", 700, 16), ("Inter", 400, 20), ("Inter", 600, 24)]


def build(app, out=None, ref=False, quiet=False):
    t0 = time.time()
    app = Path(app).resolve()
    out = Path(out) if out else app / "build" / "bundle.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    log = (lambda *a: None) if quiet else print
    files = {}
    texts = ""
    pages = []
    images = []
    fam_defs = dict(FAMILIES)
    skip_dirs = {"build", ".git", "node_modules", "__pycache__"}
    for p in sorted(app.rglob("*")):
        if p.is_dir() or any(part in skip_dirs for part in p.relative_to(app).parts):
            continue
        rel = p.relative_to(app).as_posix()
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
            pass  # handled through @font-face
        else:
            files[rel] = p.read_bytes()
    # @font-face rules in app CSS
    for m in re.finditer(r"@font-face\s*{([^}]*)}", texts):
        body = m.group(1)
        fam = re.search(r"font-family\s*:\s*['\"]?([^;'\"]+)", body)
        src = re.search(r"url\(\s*['\"]?([^'\")]+)", body)
        if fam and src:
            path = app / src.group(1)
            if path.exists():
                fam_defs[fam.group(1).strip()] = {"file": str(path), "fallback": "NotoSansThai.ttf"}
    # Thai words for line breaking (Chromium's segmenter over all app text)
    runs = collect_thai_runs(texts)
    ref_dir = app / "build" / "ref" if ref else None
    info = analyze(app, pages, fam_defs, ref_dir, runs) if pages else {"fonts": {}, "imgs": {}, "words": []}
    words = sorted(set(info["words"]))
    if words:
        files["thai.dict"] = "\n".join(words).encode()
    # ---- fonts
    all_chars = set(texts) | BASE_CHARS | THAI_CHARS | set(SYMBOLS)
    all_chars = {c for c in all_chars if ord(c) >= 0x20 and not (0xD800 <= ord(c) < 0xE000)}
    wanted = {}  # (family, weight, size) -> chars used at that size
    for key, text in info["fonts"].items():
        fam_css, w, size, style = key.split("|")
        fam = resolve_family(fam_css, fam_defs)
        try:
            w = int(float(w))
            size = round(float(size.replace("px", "")) * 4) / 4
        except ValueError:
            continue
        if size < 6 or size > 160:
            continue
        wanted.setdefault((fam, w, size), set()).update(text)
    for k in INTERNAL_SIZES + DEFAULT_SIZES:
        wanted.setdefault(k, set())
    font_entries = []
    CACHE.mkdir(exist_ok=True)
    for (fam, w, size), used in sorted(wanted.items()):
        if size <= 34:
            chars = all_chars
        else:  # large text: digits/latin + what the page shows at this size
            chars = BASE_CHARS | set("°%฿€$−–—…") | (used & all_chars)
            if any("฀" <= c <= "๿" for c in used):
                chars |= THAI_CHARS
        if fam == "Inter" and (w, size) in [(x[1], x[2]) for x in INTERNAL_SIZES]:
            chars = chars | THAI_CHARS | set(SYMBOLS)
        d = fam_defs[fam]
        key = hashlib.sha1(json.dumps([fam, d, w, size, sorted(chars), TEXT_GAMMA, 5]).encode()).hexdigest()[:16]
        cache = CACHE / f"{key}.fnt"
        if cache.exists():
            data = cache.read_bytes()
        else:
            data = build_atlas(fam, w, size, chars, fam_defs)
            cache.write_bytes(data)
        name = f"fonts/{fam.replace(' ', '')}-{w}-{str(size).replace('.', '_')}.fnt"
        files[name] = data
        font_entries.append({"file": name, "family": fam, "weight": w, "size": size})
    # ---- images
    from PIL import Image
    for rel in images:
        src = app / rel
        try:
            if rel.lower().endswith(".svg"):
                im = render_svg(src)
            else:
                im = Image.open(src)
                im.load()
        except Exception as e:  # noqa
            log(f"  ! image {rel}: {e}")
            continue
        maxdim = info["imgs"].get(rel, 0) or info["imgs"].get("./" + rel, 0)
        if maxdim and max(im.size) > maxdim * 2:
            sc = maxdim * 2 / max(im.size)
            im = im.resize((max(1, int(im.width * sc)), max(1, int(im.height * sc))), Image.LANCZOS)
        mask = rel.lower().endswith(".svg") and is_monochrome(im)
        files[rel] = to_img1(im, mask)
    manifest = {
        "entry": "index.html" if "index.html" in files else (pages[0] if pages else ""),
        "fonts": font_entries,
        "aliases": {**{k: v for k, v in ALIASES.items()}, **{f.lower(): f for f in fam_defs}},
        "built": int(time.time()),
    }
    files["manifest.json"] = json.dumps(manifest, ensure_ascii=False).encode()
    blob = pack_bundle(files)
    out.write_bytes(blob)
    fonts_kb = sum(len(files[f["file"]]) for f in font_entries) / 1024
    log(f"bundle {out} {len(blob) / 1024:.0f} KB  ({len(files)} files, {len(font_entries)} font atlases {fonts_kb:.0f} KB,"
        f" {len(words)} Thai words)  in {time.time() - t0:.1f}s")
    return out


def render_svg(path):
    """Rasterize an SVG with Chromium at its intrinsic size (x2 for scaling headroom)."""
    from PIL import Image
    from playwright.sync_api import sync_playwright
    svg = path.read_text()
    m_w = re.search(r'\bwidth="([\d.]+)', svg)
    m_h = re.search(r'\bheight="([\d.]+)', svg)
    vb = re.search(r'viewBox="[\d.\-]+[ ,]+[\d.\-]+[ ,]+([\d.]+)[ ,]+([\d.]+)', svg)
    w = float(m_w.group(1)) if m_w else (float(vb.group(1)) if vb else 64)
    h = float(m_h.group(1)) if m_h else (float(vb.group(2)) if vb else 64)
    scale = 2
    with sync_playwright() as p:
        b = p.chromium.launch()
        pg = b.new_page(viewport={"width": int(w * scale), "height": int(h * scale)})
        import base64  # a file:// image is blocked on an about:blank page
        uri = "data:image/svg+xml;base64," + base64.b64encode(path.read_bytes()).decode()
        pg.set_content(f"<html><body style='margin:0;background:transparent'><img src='{uri}'"
                       f" style='width:{int(w * scale)}px;height:{int(h * scale)}px;display:block'></body></html>")
        pg.wait_for_timeout(50)
        png = pg.screenshot(omit_background=True)
        b.close()
    return Image.open(io.BytesIO(png))


# ---------------------------------------------------------------- simulator

SIM_DIR = ROOT / "sim"
SIM_BIN = SIM_DIR / "build" / "iotdash-sim"


def build_sim():
    bdir = SIM_DIR / "build"
    bdir.mkdir(exist_ok=True)
    if not (bdir / "CMakeCache.txt").exists():
        subprocess.run(["cmake", "-S", str(SIM_DIR), "-B", str(bdir), "-DCMAKE_BUILD_TYPE=Release"], check=True,
                       stdout=subprocess.DEVNULL)
    r = subprocess.run(["cmake", "--build", str(bdir), "-j", "8"], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-6000:], r.stderr[-6000:])
        sys.exit(1)


def cmd_sim(args):
    bundle = build(args.app, quiet=False)
    build_sim()
    cmd = [str(SIM_BIN), str(bundle)]
    if args.shot:
        cmd += ["--shot", args.shot]
    if args.script:
        cmd += ["--script", args.script]
    if args.page:
        cmd += ["--page", args.page]
    subprocess.run(cmd)


def cmd_shot(args):
    app = Path(args.app).resolve()
    bundle = build(app, ref=True)
    build_sim()
    page = args.page or "index.html"
    out = app / "build" / "sim.png"
    cmd = [str(SIM_BIN), str(bundle), "--shot", str(out), "--page", page]
    if args.script:
        cmd += ["--script", args.script]
    subprocess.run(cmd, check=True)
    if not out.exists():
        return
    from PIL import Image
    ref = app / "build" / "ref" / (page.replace("/", "_") + ".png")
    a = Image.open(out).convert("RGB")
    canvas = Image.new("RGB", (a.width * 2 + 10, a.height), (255, 0, 255))
    canvas.paste(a, (0, 0))
    if ref.exists():
        canvas.paste(Image.open(ref).convert("RGB"), (a.width + 10, 0))
    cmp_path = app / "build" / "compare.png"
    canvas.save(cmp_path)
    print("compare:", cmp_path)


# ---------------------------------------------------------------- device

PORT = "/dev/cu.usbmodem5B901262921"
BAUD = 921600
BUNDLE_OFFSET = 0x400000
PIO = str(Path.home() / ".platformio/penv/bin/pio")
T_FILE, T_BUNDLE, T_COMMIT, T_EVAL, T_SHOT, T_STATS, T_PING, T_OPEN, T_WIFI, T_RESET, T_TOUCH = range(1, 12)
# OS card access: write (chunked), delete (recursive), mkdir, list (JSON), format, reload the system
T_FS_WRITE, T_FS_DELETE, T_FS_MKDIR, T_FS_LIST, T_FS_FORMAT, T_OS_RELOAD, T_APPS_CHANGED = range(12, 19)
R_ACK, R_SHOT, R_TEXT = 0x81, 0x82, 0x83


class Link:
    """Framed dev protocol over the board's UART (see src/devlink.h)."""

    def __init__(self, port=PORT, baud=BAUD, echo=True):
        import serial
        self.buf = bytearray()
        self.echo = echo
        self.line = bytearray()
        # open without toggling DTR/RTS where the driver allows (they drive EN/IO0)
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, baud, 0.05
        self.s.dtr = False
        self.s.rts = False
        self.s.open()

    def wait_ready(self, timeout=12):
        """Opening the port may reset the board: ping until the player answers."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            self.send(T_PING)
            t1 = time.time()
            while time.time() - t1 < 0.4:
                for t, p in self.poll(0.05):
                    if t == R_ACK and p[:1] == bytes([T_PING]):
                        return p[2:].decode("utf-8", "replace")
        return None

    def send(self, typ, payload=b""):
        import zlib
        hdr = bytes([0xA5, 0x5A, typ]) + struct.pack("<I", len(payload))
        self.s.write(hdr + payload + struct.pack("<I", zlib.crc32(payload) & 0xFFFFFFFF))

    def poll(self, timeout=0.05):
        """Read input: print log text, return a list of (type, payload) frames."""
        import zlib
        if self.s.timeout != timeout:  # (re)setting it reconfigures the port and can drop input
            self.s.timeout = timeout
        data = self.s.read(max(1, self.s.in_waiting))
        if data:
            self.buf += data
        frames = []
        while self.buf:
            if self.buf[0] == 0xA5:
                if len(self.buf) < 2:
                    break
                if self.buf[1] != 0x5A:
                    self._text(self.buf[:1])
                    del self.buf[:1]
                    continue
                if len(self.buf) < 7:
                    break
                typ = self.buf[2]
                n = struct.unpack_from("<I", self.buf, 3)[0]
                if n > 16 * 1024 * 1024:
                    del self.buf[:2]
                    continue
                if len(self.buf) < 11 + n:
                    break
                payload = bytes(self.buf[7:7 + n])
                crc = struct.unpack_from("<I", self.buf, 7 + n)[0]
                del self.buf[:11 + n]
                if crc == (zlib.crc32(payload) & 0xFFFFFFFF):
                    frames.append((typ, payload))
                continue
            i = self.buf.find(b"\xA5")
            if i < 0:
                i = len(self.buf)
            self._text(self.buf[:i])
            del self.buf[:i]
        return frames

    def _text(self, b):
        for ch in b:
            if ch == 10:
                if self.echo:
                    print("[dev]", self.line.decode("utf-8", "replace").rstrip())
                self.line = bytearray()
            elif ch != 13:
                self.line.append(ch)

    def request(self, typ, payload=b"", want=R_ACK, timeout=10):
        self.send(typ, payload)
        t0 = time.time()
        while time.time() - t0 < timeout:
            for t, p in self.poll():
                # an ACK answers the request of its type (late PING answers of wait_ready are not it)
                if t == want and (want != R_ACK or p[:1] == bytes([typ])):
                    return p
        return None


def decode_shot(p, out):
    from PIL import Image
    w, h = struct.unpack_from("<HH", p, 0)
    px = bytearray()
    i = 4
    while i + 2 < len(p):
        n = p[i]
        px += bytes(p[i + 1:i + 3]) * n
        i += 3
    import numpy as np
    a = np.frombuffer(bytes(px[:w * h * 2]), dtype="<u2").reshape(h, w)
    r = ((a >> 11) & 31) * 255 // 31
    g = ((a >> 5) & 63) * 255 // 63
    b = (a & 31) * 255 // 31
    Image.fromarray(np.dstack([r, g, b]).astype("uint8")).save(out)
    return out


def cmd_flash(args):
    bundle = build(args.app)
    if args.fw:
        subprocess.run([PIO, "run", "-t", "upload", "-d", str(ROOT)], check=True)
    esptool = [str(Path.home() / ".platformio/penv/bin/python"), "-m", "esptool"]
    subprocess.run(esptool + ["--chip", "esp32s3", "--port", PORT, "--baud", "921600", "write_flash", hex(BUNDLE_OFFSET),
                              str(bundle)], check=True)
    print("bundle written; the board restarts into the app")


def cmd_dev(args):
    """Build + push the bundle over the serial link, then hot-update files on every save."""
    app = Path(args.app).resolve()
    link = Link()
    print("waiting for the board...")
    print("device:", link.wait_ready() or "(no answer)")
    bundle = build(app)
    data = bundle.read_bytes()
    t0 = time.time()
    r = link.request(T_BUNDLE, data, timeout=60)
    print(f"bundle pushed {len(data) // 1024} KB in {time.time() - t0:.1f}s:", r[2:].decode() if r else "timeout")
    if args.commit:
        r = link.request(T_COMMIT, timeout=30)
        print("commit:", r[2:].decode() if r else "timeout")

    def snapshot():
        files = {}
        for p in app.rglob("*"):
            if p.is_file() and "build" not in p.relative_to(app).parts:
                files[p] = p.stat().st_mtime
        return files

    last = snapshot()
    prev_files = unpack_bundle(data)
    print("watching", app, "- keys: s=screenshot, t=stats, e <js>, r=reload, c=commit, q=quit")
    import select
    while True:
        link.poll(0.05)
        cur = snapshot()
        if cur != last:
            last = cur
            time.sleep(0.15)
            try:
                bundle = build(app, quiet=True)
            except Exception as e:  # noqa
                print("build error:", e)
                continue
            files = unpack_bundle(bundle.read_bytes())
            changed = [k for k in files if prev_files.get(k) != files[k]]
            for k in changed:
                nb = k.encode()
                link.send(T_FILE, struct.pack("<H", len(nb)) + nb + files[k])
                print("hot:", k)
            prev_files = files
        if select.select([sys.stdin], [], [], 0)[0]:
            line = sys.stdin.readline().strip()
            if line == "q":
                break
            if line == "s":
                p = link.request(T_SHOT, want=R_SHOT, timeout=20)
                if p:
                    print("screenshot:", decode_shot(p, str(app / "build" / "device.png")))
            elif line == "t":
                r = link.request(T_STATS)
                print(r[2:].decode() if r else "timeout")
            elif line.startswith("e "):
                r = link.request(T_EVAL, line[2:].encode())
                print("=>", r[2:].decode() if r else "timeout")
            elif line == "r":
                link.send(T_OPEN, b"index.html")
            elif line == "c":
                r = link.request(T_COMMIT, timeout=30)
                print("commit:", r[2:].decode() if r else "timeout")


def unpack_bundle(blob):
    n = struct.unpack_from("<I", blob, 4)[0]
    p = 8
    out = {}
    for _ in range(n):
        ln = struct.unpack_from("<H", blob, p)[0]
        p += 2
        name = blob[p:p + ln].decode()
        p += ln
        off, size = struct.unpack_from("<II", blob, p)
        p += 8
        out[name] = blob[off:off + size]
    return out


def cmd_device(args):
    """One-shot device commands: shot, stats, eval, open, wifi, reset, push."""
    link = Link(echo=args.verbose or args.what in ("log", "probe"))
    if args.what != "log" and not link.wait_ready():
        print("board did not answer")
        return
    if args.what == "shot":
        p = link.request(T_SHOT, want=R_SHOT, timeout=20)
        out = args.arg or "device.png"
        print(decode_shot(p, out) if p else "timeout")
    elif args.what == "stats":
        r = link.request(T_STATS)
        print(r[2:].decode() if r else "timeout")
    elif args.what == "eval":
        r = link.request(T_EVAL, (args.arg or "").encode())
        print(r[2:].decode() if r else "timeout")
    elif args.what == "open":
        r = link.request(T_OPEN, (args.arg or "index.html").encode())
        print("ok" if r else "timeout")
    elif args.what == "wifi":
        ssid, _, pw = (args.arg or "").partition(":")
        r = link.request(T_WIFI, ssid.encode() + b"\0" + pw.encode())
        print(r[2:].decode() if r else "timeout")
    elif args.what == "reset":
        link.send(T_RESET)
    elif args.what == "tap":
        x, y = [int(v) for v in args.arg.split(",")]
        for ph in (0, 2):
            link.send(T_TOUCH, struct.pack("<BHH", ph, x, y))
            time.sleep(0.08)
    elif args.what == "log":
        t0 = time.time()
        while time.time() - t0 < float(args.arg or 5):
            link.poll(0.1)
    elif args.what == "probe":
        # voltage on GPIO6 (Sensor AD header pin 3), live. Optional SD line settings first:
        # cs0/cs1, sck0/sck1/sckz, mosi0/mosi1/mosiz, miso0/miso1/misoz (z = float: pull-up shows)
        ok = {"cs0", "cs1"} | {p + v for p in ("sck", "mosi", "miso") for v in "01z"}
        given = [a for a in [args.arg] + args.more if a]
        bad = [a for a in given if a not in ok]
        if bad:
            raise SystemExit(f"unknown: {' '.join(bad)} (use {' '.join(sorted(ok))})")
        link.s.write(("".join(a + "\n" for a in given) + "probe on\n").encode())
        print("GPIO6 = Sensor AD header pin 3 (pin 1 3V3, pin 2 GND). Ctrl-C to stop.")
        try:
            while True:
                link.poll(0.1)
        except KeyboardInterrupt:
            pass
        finally:
            link.s.write(b"probe off\nsdauto\n")
            time.sleep(0.3)
            link.poll(0.1)


def cmd_firmware(args):
    """PlatformIO in the SDK (platformio.ini): build, upload, or the serial monitor."""
    if not Path(PIO).exists():
        raise SystemExit("PlatformIO is not installed: https://platformio.org/install/cli")
    cmd = {"build": ["run"], "upload": ["run", "-t", "upload"], "monitor": ["device", "monitor"]}[args.what]
    raise SystemExit(subprocess.run([PIO, *cmd], cwd=ROOT).returncode)


def main():
    sys.path.insert(0, str(TOOLS))
    import project
    import storecmd
    ap = argparse.ArgumentParser(prog="iotdash", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="store_true", help="the SDK's OS version and where it is")
    sub = ap.add_subparsers(dest="cmd")
    project_cmds = project.add_commands(sub)
    o = sub.add_parser("os", help="the iotdash OS: build, deploy to a card, format, simulate")
    o.add_argument("what", choices=["build", "deploy", "format", "sim", "ref"])
    o.add_argument("--page", default="system/apps/settings/main.html", help="ref: page to render in Chromium")
    o.add_argument("--js", help="ref: script to run after load")
    o.add_argument("--app", help="ref: an app folder mounted at /app/")
    o.add_argument("--dark", action="store_true", help="ref: dark color scheme")
    o.add_argument("-o", "--out", help="ref: PNG path")
    o.add_argument("--to", help="sim (default), device, or the path of a mounted SD card")
    o.add_argument("--build", action="store_true", help="deploy: build first")
    o.add_argument("--script")
    o.add_argument("--shot")
    storecmd.add_commands(sub)
    f = sub.add_parser("firmware", help="the board's firmware (PlatformIO): build, upload, monitor")
    f.add_argument("what", nargs="?", default="build", choices=["build", "upload", "monitor"])
    de = sub.add_parser("device", help="the board over USB (dev link)")
    de.add_argument("what", choices=["shot", "stats", "eval", "open", "wifi", "reset", "tap", "log", "probe"])
    de.add_argument("arg", nargs="?")
    de.add_argument("more", nargs="*", help="probe: more line settings")
    de.add_argument("-v", "--verbose", action="store_true")
    lg = sub.add_parser("legacy", help="single-app bundles from before the OS (apps that replace the OS)")
    lg.add_argument("what", choices=["build", "sim", "shot", "flash", "dev"])
    lg.add_argument("app")
    lg.add_argument("-o", "--out")
    lg.add_argument("--ref", action="store_true", help="build: also save Chromium reference screenshots")
    lg.add_argument("--shot")
    lg.add_argument("--script")
    lg.add_argument("--page")
    lg.add_argument("--fw", action="store_true", help="flash: also build and upload the player firmware")
    lg.add_argument("--commit", action="store_true", help="dev: also write the bundle to flash")
    args = ap.parse_args()
    if args.version:
        import osbuild
        print(f"iotdash OS {osbuild.os_version()} (SDK {ROOT})")
        return
    if not args.cmd:
        ap.print_help()
        return
    if args.cmd in project_cmds:
        project.main(args)
    elif args.cmd == "os":
        import osbuild
        osbuild.main_os(args)
    elif args.cmd == "store":
        storecmd.main(args)
    elif args.cmd == "firmware":
        cmd_firmware(args)
    elif args.cmd == "device":
        cmd_device(args)
    elif args.cmd == "legacy":
        {"build": lambda: build(args.app, args.out, args.ref), "sim": lambda: cmd_sim(args),
         "shot": lambda: cmd_shot(args), "flash": lambda: cmd_flash(args), "dev": lambda: cmd_dev(args)}[args.what]()


if __name__ == "__main__":
    main()
