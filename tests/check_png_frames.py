# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

import pathlib
import subprocess
import sys
executable, generator, directory = sys.argv[1:]
subprocess.run([sys.executable, generator, directory], check=True)
files = sorted(pathlib.Path(directory).glob("*.png"))
assert len(files) == 17
subprocess.run([executable, *map(str, files)], check=True)
print("17 PNG framing outcomes matched")
