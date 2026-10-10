# Vendored unchanged from /Users/geppsa-ard/Documents/top/iot/iotdash/tools/idbz.py (iotdash repo: tools/idbz.py).
# Do not edit here: change the original and copy it again.
"""IDBZ: one file that carries an iotdash app (app.idb, app.json, icon.img) and its description.

Nothing in it is compressed: a board installs a package by writing each file's bytes to the card
as they arrive (no second copy, no inflate), and a browser can slice the files out of it.

Layout (little endian):

    0    char[4]   magic "IDBZ"
    4    u16       format version (1)
    6    u16       number of files N
    8    u32       offset of the manifest (JSON, UTF-8)
    12   u32       length of the manifest
    16   u32       size of the whole package
    20   u8[32]    SHA-256 of the manifest
    52   u8[12]    zero
    64   N x 64    files: name (char[24], NUL padded), offset (u32), size (u32), SHA-256 (u8[32])
    ...            the manifest, then the files in table order, each at a multiple of 16

The manifest says what the package is:

    {"format": "idbz", "id": "calculator", "title": "Calculator", "version": "1.0",
     "description": "...", "os": {"built": "0.1.0", "min": "0.1.0"}, "built": 1760000000,
     "files": {"app.json": 120, "app.idb": 455000, "icon.img": 25000}}

app.json inside is the one the OS reads from /applications/<id>/ (title, version, hash, ...).
Used by tools/osbuild.py (app pack / app install --pack) and by the iotstore backend.
"""

import hashlib
import json
import struct

MAGIC = b"IDBZ"
FORMAT = 1
HEADER = 64
ENTRY = 64
ALIGN = 16
NAME_MAX = 23
# the files a package must carry, in the order the board writes them
FILES = ("app.json", "icon.img", "app.idb")


class PackError(ValueError):
    pass


def _pad(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def pack(manifest, files):
    """Package bytes from a manifest dict and {name: bytes}."""
    for name in FILES:
        if name not in files:
            raise PackError(f"missing {name}")
    names = list(FILES) + sorted(n for n in files if n not in FILES)
    for n in names:
        if len(n.encode()) > NAME_MAX or "/" in n:
            raise PackError(f"bad file name {n!r}")
    man = dict(manifest)
    man["format"] = "idbz"
    man["files"] = {n: len(files[n]) for n in names}
    mbytes = json.dumps(man, ensure_ascii=False, separators=(",", ":")).encode()
    table_end = HEADER + ENTRY * len(names)
    moff = table_end
    off = _pad(moff + len(mbytes))
    entries, chunks = [], []
    for n in names:
        data = files[n]
        entries.append((n, off, len(data), hashlib.sha256(data).digest()))
        chunks.append((off, data))
        off = _pad(off + len(data))
    total = chunks[-1][0] + len(chunks[-1][1])
    out = bytearray(total)
    struct.pack_into("<4sHHIII32s12x", out, 0, MAGIC, FORMAT, len(names), moff, len(mbytes), total,
                     hashlib.sha256(mbytes).digest())
    for i, (n, o, s, h) in enumerate(entries):
        struct.pack_into("<24sII32s", out, HEADER + ENTRY * i, n.encode(), o, s, h)
    out[moff:moff + len(mbytes)] = mbytes
    for o, data in chunks:
        out[o:o + len(data)] = data
    return bytes(out)


def read_header(blob):
    """(manifest dict, [(name, offset, size, sha256)]) from the start of a package; checks layout."""
    if len(blob) < HEADER or blob[:4] != MAGIC:
        raise PackError("not an IDBZ package")
    magic, ver, n, moff, mlen, total, mhash = struct.unpack_from("<4sHHIII32s", blob, 0)
    if ver != FORMAT:
        raise PackError(f"IDBZ format {ver} is not supported (this tool reads {FORMAT})")
    if n == 0 or n > 64 or HEADER + ENTRY * n > moff or moff + mlen > total:
        raise PackError("damaged package header")
    if len(blob) < moff + mlen:
        raise PackError("truncated package")
    mbytes = bytes(blob[moff:moff + mlen])
    if hashlib.sha256(mbytes).digest() != mhash:
        raise PackError("manifest checksum mismatch")
    try:
        manifest = json.loads(mbytes)
    except ValueError as e:
        raise PackError(f"bad manifest: {e}")
    entries = []
    for i in range(n):
        name, off, size, h = struct.unpack_from("<24sII32s", blob, HEADER + ENTRY * i)
        name = name.rstrip(b"\0").decode("utf-8", "replace")
        if off < moff + mlen or off + size > total:
            raise PackError(f"{name}: outside the package")
        entries.append((name, off, size, h))
    return manifest, entries, total


def unpack(blob, verify=True):
    """(manifest, {name: bytes}) from a whole package."""
    manifest, entries, total = read_header(blob)
    if len(blob) != total:
        raise PackError(f"size {len(blob)} but the header says {total}")
    files = {}
    for name, off, size, h in entries:
        data = bytes(blob[off:off + size])
        if verify and hashlib.sha256(data).digest() != h:
            raise PackError(f"{name}: checksum mismatch")
        files[name] = data
    for name in FILES:
        if name not in files:
            raise PackError(f"missing {name}")
    return manifest, files


def validate(blob):
    """The manifest of a well-formed package whose app.json agrees with it, else PackError."""
    manifest, files = unpack(blob)
    for k in ("id", "title", "version"):
        if not isinstance(manifest.get(k), str) or not manifest[k]:
            raise PackError(f"manifest has no {k}")
    app = json.loads(files["app.json"])
    if app.get("id") != manifest["id"] or app.get("version") != manifest["version"]:
        raise PackError("app.json does not match the manifest")
    if not files["app.idb"][:4]:
        raise PackError("empty app.idb")
    return manifest


if __name__ == "__main__":  # python3 idbz.py FILE: describe a package
    import sys
    data = open(sys.argv[1], "rb").read()
    m, entries, total = read_header(data)
    unpack(data)
    print(json.dumps(m, indent=2, ensure_ascii=False))
    for name, off, size, h in entries:
        print(f"  {name:<12} {size:>9} B at {off:<8} sha256 {h.hex()[:16]}…")
    print(f"  {total} B, checksums ok")
