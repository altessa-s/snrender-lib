# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

import pathlib
import re
import sys
text = pathlib.Path("include/snrender/snrender.h").read_text()
version = ".".join(re.search(r"#define SNRENDER_VERSION_" + part + r" (\d+)", text)[1] for part in ("MAJOR", "MINOR", "PATCH"))
assert sys.argv[1] == "v" + version
print(version)
