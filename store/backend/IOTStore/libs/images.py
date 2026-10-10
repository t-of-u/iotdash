"""
App icons: the board's IMG1 (iotdash tools/iotdash.py to_img1) turned into PNG for the web.

IMG1: struct "<4sHHHH" = b"IMG1", width, height, kind, 0, then
    kind 0  RGB565 little endian (w*h*2 bytes)
    kind 1  RGB565 then A8 alpha (w*h bytes)
    kind 2  A8 mask only (drawn black with that alpha)
"""

import struct
import zlib


class ImageError(ValueError):
    pass


def img1_to_rgba(data: bytes):
    if len(data) < 12 or data[:4] != b"IMG1":
        raise ImageError("not an IMG1 image")
    _, w, h, kind, _ = struct.unpack_from("<4sHHHH", data, 0)
    n = w * h
    if not 0 < n <= 1024 * 1024:
        raise ImageError("bad image size")
    body = data[12:]
    out = bytearray(n * 4)
    if kind == 2:
        if len(body) < n:
            raise ImageError("truncated mask")
        for i in range(n):
            out[i * 4 + 3] = body[i]
        return w, h, bytes(out)
    if kind not in (0, 1) or len(body) < n * 2 + (n if kind == 1 else 0):
        raise ImageError("truncated image")
    px = struct.unpack_from(f"<{n}H", body, 0)
    alpha = body[n * 2:n * 3] if kind == 1 else None
    for i, c in enumerate(px):
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        o = i * 4
        out[o] = (r << 3) | (r >> 2)
        out[o + 1] = (g << 2) | (g >> 4)
        out[o + 2] = (b << 3) | (b >> 2)
        out[o + 3] = alpha[i] if alpha is not None else 255
    return w, h, bytes(out)


def png(w: int, h: int, rgba: bytes) -> bytes:
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    stride = w * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def img1_to_png(data: bytes) -> bytes:
    return png(*img1_to_rgba(data))
