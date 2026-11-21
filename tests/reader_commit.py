# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
import pathlib
import re

text = pathlib.Path("CMakeLists.txt").read_text()
commit = re.search(r"set\(SNR_READER_COMMIT ([0-9a-f]{40})\)", text).group(1)
print("commit=" + commit)

spd_commit = re.search(r"set\(SNR_SPD_COMMIT ([0-9a-f]{40})\)", text).group(1)
print("spd_commit=" + spd_commit)
