# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

import zlib, struct, pathlib

import sys
out = pathlib.Path(sys.argv[1])
out.mkdir(exist_ok=True)


def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)


SIG = b"\x89PNG\r\n\x1a\n"


def ihdr(w, h, depth=8, ctype=6, interlace=0):
    return chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace))


def pix(x, y, depth, ctype):
    if ctype == 6:
        v = [x * 7 % 256, y * 9 % 256, (x + y) % 256, 200]
    elif ctype == 0:
        v = [(x * 11 + y) % 256]
    else:
        v = [x * 7 % 256, y * 9 % 256, (x + y) % 256]
    if depth == 16:
        return b"".join(struct.pack(">H", c * 257) for c in v)
    return bytes(v)


def raw(w, h, depth=8, ctype=6, xs=None, ys=None):
    xs = xs if xs is not None else range(w)
    ys = ys if ys is not None else range(h)
    return b"".join(b"\x00" + b"".join(pix(x, y, depth, ctype) for x in xs) for y in ys)


def png(w, h, depth=8, ctype=6, extra=b"", idat=None, trailing=b"", pre=b""):
    data = idat if idat is not None else chunk(b"IDAT", zlib.compress(raw(w, h, depth, ctype)))
    return SIG + ihdr(w, h, depth, ctype) + pre + extra + data + chunk(b"IEND", b"") + trailing


W, H = 37, 23
cases = {}
cases["base"] = png(W, H)
cases["rgb16"] = png(W, H, 16, 6)
cases["gray8"] = png(W, H, 8, 0)
cases["rgb8"] = png(W, H, 8, 2)
cases["ancillary"] = png(W, H, extra=chunk(b"tEXt", b"Comment\x00hello") + chunk(b"pHYs", struct.pack(">IIB", 2835, 2835, 1)) + chunk(b"gAMA", struct.pack(">I", 45455)) + chunk(b"zzZz", b""))
cases["trailing"] = png(W, H, trailing=b"\x00" * 16)
cases["trailing_text"] = png(W, H, trailing=b"garbage after IEND\n")
# APNG: acTL + fcTL before IDAT (default image is frame 0)
actl = chunk(b"acTL", struct.pack(">II", 1, 0))
fctl = chunk(b"fcTL", struct.pack(">IIIIIHHBB", 0, W, H, 0, 0, 1, 10, 0, 0))
cases["apng"] = png(W, H, pre=actl + fctl)
# Interlaced (Adam7)
passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
data = b""
for x0, y0, dx, dy in passes:
    xs = list(range(x0, W, dx))
    ys = list(range(y0, H, dy))
    if xs and ys:
        data += raw(W, H, 8, 6, xs, ys)
cases["interlaced"] = SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 1)) + chunk(b"IDAT", zlib.compress(data)) + chunk(b"IEND", b"")
# IDAT split into 1-byte chunks (huge chunk count)
z = zlib.compress(raw(W, H))
cases["split_idat"] = png(W, H, idat=b"".join(chunk(b"IDAT", z[i:i + 1]) for i in range(len(z))))
# Hostile framing
base = cases["base"]
cases["len_7fffffff"] = base[:33] + struct.pack(">I", 0x7fffffff) + b"tEXt" + b"x" * 20
cases["len_ffffffff"] = base[:33] + struct.pack(">I", 0xffffffff) + b"tEXt" + b"x" * 20
cases["no_iend"] = base[:-12]
cases["iend_nonzero"] = base[:-12] + chunk(b"IEND", b"x")
cases["truncated_crc"] = base[:-2]
cases["double_iend"] = base + chunk(b"IEND", b"")
cases["only_header"] = base[:33]
for k, v in cases.items():
    (out / f"{k}.png").write_bytes(v)
