# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Generate the image decoder fixtures with zlib and libjpeg-turbo's cjpeg."""
from pathlib import Path
import struct
import subprocess
import zlib


def chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data
            + struct.pack(">I", zlib.crc32(kind + data)))


directory = Path(__file__).resolve().parent
width, height = 50, 30
rgb = bytearray()
scanlines = bytearray()
for y in range(height):
    scanlines.append(0)  # PNG filter type
    for x in range(width):
        pixel = bytes((x * 5, y * 8, (x * 13 + y * 17) % 256))
        rgb.extend(pixel)
        scanlines.extend(pixel)
        scanlines.append((x * 7 + y * 11) % 256)

header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
(directory / "layer.png").write_bytes(
    b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
    + chunk(b"IDAT", zlib.compress(scanlines, 9)) + chunk(b"IEND", b""))
ppm = f"P6\n{width} {height}\n255\n".encode() + rgb
subprocess.run(["cjpeg", "-quality", "85", "-outfile", directory / "layer.jpg"],
               input=ppm, check=True)
