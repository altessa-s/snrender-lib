# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
import hashlib
import pathlib
import subprocess
import sys
import tempfile

import pypdf
from pypdf.generic import DictionaryObject, NameObject

rasterizer, generator, fixtures, build, structure, cases = sys.argv[1:]


def resources(owner, seen):
    """Yield every resource dictionary reachable from a page, following form
    XObjects (the recognition text is drawn inside a transparency group)."""
    found = owner.get('/Resources')
    if found is None:
        return
    found = found.get_object()
    if id(found) in seen:
        return
    seen.add(id(found))
    yield found
    for xobject in found.get('/XObject', DictionaryObject()).get_object().values():
        xobject = xobject.get_object()
        if xobject.get('/Subtype') == '/Form':
            yield from resources(xobject, seen)


def inspect(writer, strip_font, strip_mask):
    """Count embedded TrueType programs and soft masks, removing them as asked.
    Objects are reached through pypdf, so object streams make no difference."""
    fonts, masks, seen = [], 0, set()
    for page in writer.pages:
        for found in resources(page, seen):
            for font in found.get('/Font', DictionaryObject()).get_object().values():
                font = font.get_object()
                fonts.append(font)
                for descendant in font.get('/DescendantFonts', []):
                    descriptor = descendant.get_object().get('/FontDescriptor')
                    if descriptor is not None and '/FontFile2' in descriptor.get_object():
                        if strip_font:
                            del descriptor.get_object()[NameObject('/FontFile2')]
            for state in found.get('/ExtGState', DictionaryObject()).get_object().values():
                state = state.get_object()
                if isinstance(state.get('/SMask'), pypdf.generic.IndirectObject) or \
                        isinstance(state.get('/SMask'), DictionaryObject):
                    masks += 1
                    if strip_mask:
                        state[NameObject('/SMask')] = NameObject('/None')
    return fonts, masks


with tempfile.TemporaryDirectory(dir=build, prefix='font-substitution-') as directory:
    root = pathlib.Path(directory)
    subprocess.run([generator, fixtures], cwd=root, check=True)
    subprocess.run([structure, fixtures], cwd=root, check=True)
    for mode in ('scripts', 'narrow'):
        for suffix in ('', '-off'):
            subprocess.run([cases, mode, str(root / (mode + suffix + '.pdf'))] + (['off'] if suffix else []), check=True)
    checked = 0
    residual = 0
    for original in sorted(root.glob('*.pdf')):
        if original.stem.endswith('-off'):
            continue
        fonts, masks = inspect(pypdf.PdfWriter(clone_from=str(original)), False, False)
        embedded = [f for f in fonts
                    if any('/FontFile2' in d.get_object().get('/FontDescriptor', DictionaryObject()).get_object()
                           for d in f.get('/DescendantFonts', []))]
        if not embedded:
            continue
        assert all(f.get('/Encoding') != '/WinAnsiEncoding' for f in fonts), original
        assert all(f.get('/Encoding') == '/Identity-H' and '/ToUnicode' in f for f in embedded), original
        assert masks, 'No soft mask found'
        variants = []
        for name, font, mask in (('without-font', True, False), ('without-mask', False, True),
                                 ('without-both', True, True)):
            writer = pypdf.PdfWriter(clone_from=str(original))
            inspect(writer, font, mask)
            output = root / (name + '.pdf')
            with open(output, 'wb') as stream:
                writer.write(stream)
            stripped_fonts, stripped_masks = inspect(pypdf.PdfWriter(clone_from=str(output)), False, False)
            assert not mask or not stripped_masks, (original, name)
            assert not font or not any(
                '/FontFile2' in d.get_object().get('/FontDescriptor', DictionaryObject()).get_object()
                for f in stripped_fonts for d in f.get('/DescendantFonts', [])), (original, name)
            variants.append(output)
        for aa in ('no', 'yes'):
            for thin in ('none', 'solid', 'shape'):
                def raster(path):
                    data = subprocess.check_output([rasterizer, '-r', '600', '-gray',
                        '-aa', aa, '-aaVector', aa, '-thinlinemode', thin, str(path)])
                    return hashlib.sha256(data).digest()
                expected = raster(original.with_stem(original.stem + '-off'))
                assert raster(variants[0]) == expected, (original, 'font', aa, thin)
                assert raster(variants[1]) == expected, (original, 'mask', aa, thin)
                residual += raster(variants[2]) != expected
        checked += 1
    assert checked >= 12, checked
    print(f'{checked} symbolic text PDFs: font-only and mask-only removal equal recognition-off at 600 dpi, AA off/on, thin modes none/solid/shape')
    print(f'Both removed: {residual} raster comparisons show substituted text')
