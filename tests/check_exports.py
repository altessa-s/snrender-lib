# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

import pathlib
import re
import subprocess
import sys
expected = set(re.findall(r"SNR_API[^;]*?\b(snr_\w+)\s*\(", pathlib.Path("include/snrender/snrender.h").read_text()))
args = ["nm", "-gU", sys.argv[1]] if sys.platform == "darwin" else ["nm", "-D", "--defined-only", sys.argv[1]]
actual = set(re.findall(r"\b_?(snr_\w+)\b", subprocess.check_output(args, text=True)))
assert len(expected) == int(sys.argv[2]), expected
assert actual == expected, (actual - expected, expected - actual)
print(f"{len(actual)} exports match {len(expected)} public functions")
