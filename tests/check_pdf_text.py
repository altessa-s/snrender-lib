# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Generate source PDFs independently of the library's PDF writer."""
import pathlib
import subprocess
import sys
from pypdf import PdfWriter
from pypdf.generic import (ArrayObject, DecodedStreamObject, DictionaryObject,
                          NameObject, NumberObject)

probe, folder = sys.argv[1:]
folder = pathlib.Path(folder)
folder.mkdir(parents=True, exist_ok=True)
for rotation in (0, 90, 180, 270):
    for offset in (0, 1, -1):
        writer = PdfWriter()
        page = writer.add_blank_page(400, 300)
        ox, oy = (20 * offset, 30 * offset)
        page.mediabox = ArrayObject([NumberObject(v) for v in (-80, -80, 360, 270)])
        page.cropbox = ArrayObject([NumberObject(v) for v in (ox, oy, ox+300, oy+200)])
        page.rotate(rotation)
        # Deliberately maps simple source glyphs to Unicode scalars, testing
        # Cyrillic, a ligature, soft hyphen and the recognition invisible set
        # without an installed-font dependency.
        cmap = DecodedStreamObject()
        mappings = dict(zip('ABCDEFG', 'Жёлтый\ufb01'))
        mappings.update(dict(zip('IJKLMN', '\u00ad\u200b\u200c\u200d\u2060\ufeff')))
        cmap.set_data(('''/CIDInit /ProcSet findresource begin
12 dict begin begincmap /CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def
/CMapName /Test def /CMapType 2 def
1 begincodespacerange <00> <ff> endcodespacerange
''' + f'{len(mappings)} beginbfchar\n' + '\n'.join(
            f'<{ord(k):02x}> <{v.encode("utf-16-be").hex()}>' for k, v in mappings.items()) +
            '\nendbfchar endcmap CMapName currentdict /CMap defineresource pop end end').encode())
        latin = DictionaryObject({NameObject('/Type'): NameObject('/Font'),
                                  NameObject('/Subtype'): NameObject('/Type1'),
                                  NameObject('/BaseFont'): NameObject('/Helvetica')})
        mapped = DictionaryObject(latin)
        mapped[NameObject('/ToUnicode')] = writer._add_object(cmap)
        page[NameObject('/Resources')] = DictionaryObject({NameObject('/Font'): DictionaryObject({
            NameObject('/F1'): writer._add_object(latin), NameObject('/F2'): writer._add_object(mapped)})})
        content = DecodedStreamObject()
        lines = [('F1', 170, 'Alpha Beta'), ('F1', 145, 'multi-'),
                 ('F1', 120, 'line words'), ('F2', 95, 'ABCDEF'),
                 ('F2', 70, 'G'), ('F2', 45, 'aIbJcKdLeMfNg'),
                 ('F2', 20, 'aI'), ('F1', 5, 'b')]
        content.set_data(('\n'.join(f'BT /{font} 12 Tf {ox+20} {oy+y} Td ({text}) Tj ET'
                                   for font, y, text in lines)).encode())
        page[NameObject('/Contents')] = writer._add_object(content)
        path = folder / f'r{rotation}-offset{offset}.pdf'
        writer.write(path)
        subprocess.run([probe, str(path), str(rotation)], check=True)
print('PDF text fixtures: Latin, Cyrillic, ligature, cleaning, multiline, all rotations and box offsets passed')


# Keep expectations literal: do not derive them from either extraction engine.
def fixture(name, width, height, rotation, content, expected, rects,
            mappings=None, page_index=0, type3=False, crop=None):
    writer = PdfWriter()
    for _ in range(page_index):
        writer.add_blank_page(width, height)
    page = writer.add_blank_page(width, height)
    page.rotate(rotation)
    if crop:
        page.cropbox = ArrayObject([NumberObject(v) for v in crop])
    font = DictionaryObject({NameObject('/Type'): NameObject('/Font'),
                             NameObject('/Subtype'): NameObject('/Type1'),
                             NameObject('/BaseFont'): NameObject('/Helvetica')})
    if mappings:
        cmap = DecodedStreamObject()
        cmap.set_data(('/CIDInit /ProcSet findresource begin 12 dict begin begincmap '
                       '/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def '
                       '/CMapName /T def /CMapType 2 def '
                       '1 begincodespacerange <00> <ff> endcodespacerange\n'
                       + f'{len(mappings)} beginbfchar\n' + '\n'.join(
                           f'<{ord(k):02x}> <{v.encode("utf-16-be").hex()}>'
                           for k, v in mappings.items()) +
                       '\nendbfchar endcmap CMapName currentdict /CMap defineresource pop end end').encode())
        font[NameObject('/ToUnicode')] = writer._add_object(cmap)
    if type3:
        from pypdf.generic import FloatObject
        glyph = DecodedStreamObject()
        glyph.set_data(b'1000 0 0 -500 1000 500 d1 0 -500 1000 1000 re f')
        font = DictionaryObject({
            NameObject('/Type'): NameObject('/Font'), NameObject('/Subtype'): NameObject('/Type3'),
            NameObject('/FontBBox'): ArrayObject([NumberObject(v) for v in (0, -500, 1000, 500)]),
            NameObject('/FontMatrix'): ArrayObject([FloatObject(v) for v in (.001, 0, 0, .001, 0, 0)]),
            NameObject('/CharProcs'): DictionaryObject({NameObject('/A'): writer._add_object(glyph)}),
            NameObject('/Encoding'): DictionaryObject({NameObject('/Differences'): ArrayObject([NumberObject(65), NameObject('/A')])}),
            NameObject('/FirstChar'): NumberObject(65), NameObject('/LastChar'): NumberObject(65),
            NameObject('/Widths'): ArrayObject([NumberObject(1000)]),
            NameObject('/Resources'): DictionaryObject()})
    page[NameObject('/Resources')] = DictionaryObject({NameObject('/Font'): DictionaryObject({
        NameObject('/F1'): writer._add_object(font)})})
    stream = DecodedStreamObject()
    stream.set_data(content.encode())
    page[NameObject('/Contents')] = writer._add_object(stream)
    path = folder / f'{name}.pdf'
    writer.write(path)
    rectfile = folder / f'{name}.rects'
    rectfile.write_text('\n'.join(' '.join(map(str, r)) for r in rects))
    # A UTF-8 file, not argv: Windows converts arguments to the ANSI code page.
    textfile = folder / f'{name}.expected'
    textfile.write_bytes(expected.encode('utf-8'))
    subprocess.run([probe, str(path), str(rotation), str(textfile), str(rectfile), str(page_index)], check=True)


fixture('counter90', 300, 400, 90,
        'BT /F1 12 Tf 0 1 -1 0 50 20 Tm (Hello World) Tj ET',
        'Hello World', [(0, 0, 400, 300)])
fixture('counter270', 300, 400, 270,
        'BT /F1 12 Tf 0 -1 1 0 250 380 Tm (Hello World) Tj ET',
        'Hello World', [(0, 0, 400, 300)])
fixture('vertical', 300, 400, 0,
        'BT /F1 12 Tf 0 1 -1 0 50 20 Tm (Hello World) Tj ET',
        'Hello World', [(0, 0, 300, 400)])
# Baseline y=250/230 is displayed y=50/70. Rectangle order is reversed,
# overlaps are duplicated, and selected words omit the right column.
columns = ('BT /F1 12 Tf 20 250 Td (left one) Tj ET '
           'BT /F1 12 Tf 220 250 Td (right one) Tj ET '
           'BT /F1 12 Tf 20 230 Td (left two) Tj ET '
           'BT /F1 12 Tf 220 230 Td (right two) Tj ET')
fixture('columns-left', 400, 300, 0, columns, 'left one\nleft two',
        [(15, 55, 90, 20), (15, 35, 90, 20), (15, 35, 90, 20)])
fixture('columns-union', 400, 300, 0, columns, 'left one right one\nleft two right two',
        [(210, 30, 100, 50), (10, 30, 100, 50), (10, 30, 100, 50)])
fixture('astral', 400, 300, 0, 'BT /F1 12 Tf 20 250 Td (CACBC) Tj ET',
        'x𝐀x😀x', [(0, 0, 400, 300)], {'A': '𝐀', 'B': '😀', 'C': 'x'})
fixture('page-one', 400, 300, 0, 'BT /F1 12 Tf 20 250 Td (Second page) Tj ET',
        'Second page', [(0, 0, 400, 300)], page_index=1)
# Type3 glyph has exact horizontal centre x=26 on both engines, exercising
# left inclusion and right exclusion. PDFKit uses fallback vertical metrics;
# exact crop-top centres are covered separately by test_pdf_text_geometry.
for rotation, rect, expected in [(0, (26, 0, 10, 10), 'A'),
                                 (0, (16, 0, 10, 10), '')]:
    fixture(f'boundary-{rotation}-{expected or "empty"}', 400, 300, rotation,
            'BT /F1 12 Tf 20 250 Td (A) Tj ET', expected, [rect],
            type3=True, crop=(0, 0, 400, 250))
print('Counter-rotations, vertical, column union, astral, page 1 and exact boundary passed')
