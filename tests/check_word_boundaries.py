# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
from collections import Counter
import pathlib
import subprocess
import sys
import tempfile

extractor, generator, fixtures, geometry = sys.argv[1:]
failures = 0
checked = 0
with tempfile.TemporaryDirectory(dir=pathlib.Path.cwd() / "build" if (pathlib.Path.cwd() / "CMakeLists.txt").exists() else pathlib.Path.cwd(), prefix="word-boundaries-") as temporary:
    root = pathlib.Path(temporary)
    subprocess.run([generator, fixtures], cwd=root, check=True)
    for labels in sorted(root.glob("fixture-*.words")):
        if not labels.read_text():
            continue
        pdf = labels.with_suffix(".pdf")
        expected = []
        for line in labels.read_text().splitlines():
            *box, label = line.split()
            expected.extend(bytes.fromhex(label).decode("utf-8").split())
        raw = subprocess.check_output([extractor, "-raw", str(pdf), "-"], text=True).split()
        actual = subprocess.check_output([extractor, str(pdf), "-"], text=True).split()
        result = subprocess.run([geometry, str(pdf), str(labels)])
        failures += result.returncode != 0
        checked += 1
        if raw != expected or Counter(actual) != Counter(expected):
            failures += 1
            print(labels.stem, "expected:", expected, "raw:", raw, "missing:", Counter(expected)-Counter(actual), "extra:", Counter(actual)-Counter(expected), flush=True)
    print(f"{checked} recognition fixture pages checked; {failures} failed checks", flush=True)
raise SystemExit(bool(failures))
