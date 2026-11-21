# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Archive the installed tree with the same top directory on all platforms."""
import pathlib
import shutil
import sys

version, platform = sys.argv[1:]
name = f'snrender-{version}-{platform}'
root = pathlib.Path('build')
stage = root / name
shutil.copytree(root / 'stage', stage, dirs_exist_ok=True)
(root / 'dist').mkdir(exist_ok=True)
shutil.make_archive(str(root / 'dist' / name),
                    'zip' if platform.startswith('windows') else 'gztar',
                    root_dir=root, base_dir=name)
