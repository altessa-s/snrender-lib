# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
import subprocess
import sys
extractor, generator, output = sys.argv[1:]
subprocess.run([generator, 'row', '320', output], check=True)
words = subprocess.check_output([extractor, '-raw', output, '-'], text=True).split()
assert words == ['a'] * 320, len(words)
print('320 words in 110 mm: all raw boundaries preserved')
