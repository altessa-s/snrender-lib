# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Compare full decoded pixels from test_spd_hostile on two backend builds.

Usage: python3 tests/check_spd_parity.py build/apple/spd-fixtures build/cairo/spd-fixtures
Each producer also asserts every pixel against hand-computed expectations.
"""
import hashlib
import pathlib
import sys

a, b = (pathlib.Path(arg) / 'parity.rgba' for arg in sys.argv[1:])
left, right = a.read_bytes(), b.read_bytes()
assert left and len(left) % (128 * 128 * 4) == 0
assert left == right, f'Backend pixels differ: {a} vs {b}'
print(f'{len(left) // (128 * 128 * 4)} full RGBA images identical; sha256 {hashlib.sha256(left).hexdigest()}')
