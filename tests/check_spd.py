# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Check physical PDF size/orientation and the SVG's embedded PNG independently."""
import base64
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET
from pypdf import PdfReader

root = pathlib.Path(sys.argv[1])
empty = PdfReader(root / 'result.empty.pdf')
assert len(empty.pages) == 1
assert float(empty.pages[0].mediabox.width) == 3
assert float(empty.pages[0].mediabox.height) == 3
reader = PdfReader(root / 'result.pdf')
assert len(reader.pages) == 1
half = PdfReader(root / 'result.half.pdf')
assert len(half.pages) == 1
assert abs(float(half.pages[0].mediabox.width) - 144) < 1e-6
assert abs(float(half.pages[0].mediabox.height) - 144) < 1e-6
page = reader.pages[0]
assert abs(float(page.mediabox.width) - 144) < 1e-6
assert abs(float(page.mediabox.height) - 144) < 1e-6
# Rasterize at the stored 128 ppi: 256 px / 128 ppi * 72 = 144 points.
data = subprocess.check_output([sys.argv[2], '-r', '128', '-singlefile', str(root / 'result.pdf')])
header, dims, maximum, pixels = data.split(b'\n', 3)
assert header == b'P6' and dims == b'256 256' and maximum == b'255'
for x, y, gray in [(10, 10, 100), (80, 10, 184), (10, 80, 227),
                   (160, 10, 32), (160, 160, 0), (10, 160, 255)]:
    sample = pixels[(y * 256 + x) * 3:][:3]
    assert all(abs(value - gray) <= 2 for value in sample), (x, y, sample, gray)
svg = ET.parse(root / 'result.svg').getroot()
assert svg.attrib['viewBox'] == '0 0 256 256'
image = svg.find('{http://www.w3.org/2000/svg}image')
uri = image.attrib['{http://www.w3.org/1999/xlink}href']
assert uri.startswith('data:image/png;base64,')
assert base64.b64decode(uri.split(',', 1)[1]) == (root / 'result.png').read_bytes()
print('SPD PDF size/pixels and SVG embedded PNG match independent expectations')
