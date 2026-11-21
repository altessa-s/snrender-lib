# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
import pathlib
import subprocess
import sys
import tempfile

swift, script, generator = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="recognition-pdfkit-", dir=pathlib.Path.cwd()) as temporary:
    root = pathlib.Path(temporary)
    modes = ("order", "line_edge", "line_dot03", "line_dot06", "line_dot1", "line_same_x", "line_disjoint", "line_slivers", "line_tall_base", "line_tall", "line_tall_same")
    pdfs = [root / (mode + ".pdf") for mode in modes]
    for mode, pdf in zip(modes, pdfs):
        subprocess.run([generator, mode, str(pdf)], check=True)
    subprocess.run([swift, "-module-cache-path", str(root / "swift-cache"), script, *map(str, pdfs)], check=True)
