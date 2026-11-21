#!/usr/bin/env python3
# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Synthetic-only fixtures, adapted from snspd-lib/tests/generate_fixtures.py.

Uses Python's SQLite and zlib; never reads real drawings or a reader build tree.
"""
import pathlib
import sqlite3
import struct
import sys
import zlib


def vi(n):
    out = bytearray()
    while n >= 128:
        out.append((n & 127) | 128)
        n >>= 7
    return bytes(out + bytes([n]))


def field(n, data):
    return vi(n * 8 + 2) + vi(len(data)) + data


def layer(n, name):
    return field(1, b'\x08' + vi(n) + field(2, name))


def png(kind, width=128, height=128, interlace=False, split=False):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    raw = bytearray()
    passes = [(0, 0, 1, 1)] if not interlace else [(0, 0, 8, 8), (4, 0, 8, 8),
        (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    for xs, ys, xd, yd in passes:
        for y in range(ys, height, yd):
            raw.append(0)
            for x in range(xs, width, xd):
                if kind == 'pattern':
                    gray, alpha = ((0, 255) if x < 64 else (80, 128)) if y < 64 else (0, 0)
                else:
                    gray, alpha = {'black': (0, 255), 'over': (200, 128), 'ref': (32, 255)}[kind]
                raw.extend((gray, alpha))
    compressed = zlib.compress(raw, 9)
    stride = 17 if split else len(compressed)
    data = b''.join(chunk(b'IDAT', compressed[i:i+stride]) for i in range(0, len(compressed), stride))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 4, 0, 0, int(interlace)))
            + data + chunk(b'IEND', b''))



def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    for variant in ['valid', 'bad', 'crc', 'wrong', 'huge', 'truncated', 'decode', 'adam7', 'split', 'empty']:
        path = directory / (variant + '.spd')
        path.unlink(missing_ok=True)
        with sqlite3.connect(path) as db:
            db.execute('CREATE TABLE config(name TEXT PRIMARY KEY NOT NULL,value BLOB)')
            config = {'fmt_ver': b'2', 'surface.width': b'256', 'surface.height': b'256',
                      'ppi': b'128', 'vp.x': b'1408', 'vp.y': b'2688', 'vp.scale': b'1',
                      'ls': layer(1, b'Ink') + layer(2, b'Over') + layer(9999, b'Reference Layer')}
            db.executemany('INSERT INTO config VALUES(?,?)', config.items())
            for n in [1, 2, 9999]:
                db.execute(f'CREATE TABLE surface_{n}(tid INTEGER PRIMARY KEY NOT NULL,tile BLOB NOT NULL) WITHOUT ROWID')
            # Absolute tile bounds are 4 columns by 2 rows; the 2x2 canvas
            # is centred on (11,21), with tiles clipped on its left/right.
            tiles = [(10, 20, png('pattern')), (11, 21, png('black')),
                     (9, 20, png('black')), (12, 20, png('black'))]
            damaged = {'bad': b'not png', 'crc': png('black')[:-1] + b'\x01',
                       'wrong': png('black', 64), 'huge': png('black')[:16] + struct.pack('>II', 0xffffffff, 0xffffffff) + png('black')[24:],
                       'truncated': png('black')[:-12]}
            broken = png('black')[:33]
            payload = b'not a zlib stream'
            chunk = b'IDAT' + payload
            damaged['decode'] = broken + struct.pack('>I', len(payload)) + chunk + struct.pack('>I', zlib.crc32(chunk)) + png('black')[-12:]
            if variant in damaged:
                tiles = [(10, 20, damaged[variant])]
            if variant in ['adam7', 'split']:
                tiles = [(10, 20, png('pattern', interlace=variant == 'adam7', split=variant == 'split'))]
            if variant == 'empty':
                tiles = []
            db.executemany('INSERT INTO surface_1 VALUES(?,?)', [(x * 4096 + y, b) for x, y, b in tiles])
            if variant == 'valid':
                db.execute('INSERT INTO surface_2 VALUES(?,?)', (10 * 4096 + 20, png('over')))
                db.execute('INSERT INTO surface_9999 VALUES(?,?)', (11 * 4096 + 20, png('ref')))


if __name__ == '__main__':
    generate(pathlib.Path(sys.argv[1]))
