# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Failed exports preserve existing destinations, including source aliases."""
import os
import stat
import pathlib
import subprocess
import sys
import tempfile

cli, seeds, root = sys.argv[1:]
root = pathlib.Path(root)
with tempfile.TemporaryDirectory() as directory:
    directory = pathlib.Path(directory)
    subprocess.run([seeds, str(directory), str(root / 'data/layer.png'),
                    str(root / 'data/layer.jpg')], check=True)
    note = directory / 'seed-layers.note'
    mark = directory / 'seed-overlay.mark'
    destination = directory / 'existing.png'
    original = b'preserve this existing destination\n'
    destination.write_bytes(original)
    if os.name == 'posix':
        destination.chmod(0o640)
    result = subprocess.run([cli, str(note), str(destination),
                             '--maximum-work', '0'], capture_output=True)
    assert result.returncode != 0
    assert destination.read_bytes() == original
    readonly = directory / 'readonly.png'
    readonly.write_bytes(original)
    readonly.chmod(0o444)
    try:
        # POSIX root can write 0444 files; access(W_OK) preserves that behavior.
        if os.name == 'nt' or not os.access(readonly, os.W_OK):
            result = subprocess.run([cli, str(note), str(readonly), '1', '0.0625'],
                                    capture_output=True)
            assert result.returncode != 0
            assert readonly.read_bytes() == original
            assert not list(directory.glob('*.snrender-*'))
    finally:
        readonly.chmod(0o644)
    source = directory / 'source.pdf'
    original = b'%PDF-1.4\ninvalid source deliberately rejected\n'
    source.write_bytes(original)
    result = subprocess.run([cli, str(mark), str(source), '--source-pdf', str(source)],
                            capture_output=True)
    assert result.returncode != 0
    assert source.read_bytes() == original
    # Replacement failure also leaves the destination untouched.
    blocked = directory / 'directory.png'
    blocked.mkdir()
    result = subprocess.run([cli, str(note), str(blocked), '1', '0.0625'],
                            capture_output=True)
    assert result.returncode != 0 and blocked.is_dir()
    result = subprocess.run([cli, str(note), str(destination), '1', '0.0625'],
                            capture_output=True)
    assert result.returncode == 0, result.stderr
    assert destination.read_bytes().startswith(b'\x89PNG')
    if os.name == 'posix':
        assert stat.S_IMODE(destination.stat().st_mode) == 0o640
        fresh = directory / 'fresh.png'
        mask = os.umask(0o027)
        try:
            result = subprocess.run([cli, str(note), str(fresh), '1', '0.0625'],
                                    capture_output=True)
        finally:
            os.umask(mask)
        assert result.returncode == 0, result.stderr
        assert stat.S_IMODE(fresh.stat().st_mode) == 0o640
        # Atomic replacement changes only the named entry, including links.
        target = directory / 'target.png'
        target.write_bytes(b'original target')
        for kind in ('symlink', 'hardlink'):
            alias = directory / (kind + '.png')
            if kind == 'symlink':
                alias.symlink_to(target)
            else:
                os.link(target, alias)
            result = subprocess.run([cli, str(note), str(alias), '1', '0.0625'],
                                    capture_output=True)
            assert result.returncode == 0, result.stderr
            assert not alias.is_symlink()
            assert alias.read_bytes().startswith(b'\x89PNG')
            assert target.read_bytes() == b'original target'
    assert not list(directory.glob('*.snrender-*'))
