# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Preservation and hostile-input checks against independent PDF readers."""
import base64
import json
import struct
import zlib
import pathlib
import subprocess
import sys
from pypdf import PdfWriter, PdfReader
from pypdf.generic import (NameObject, ArrayObject, FloatObject,
                          DictionaryObject, DecodedStreamObject, TextStringObject)

exe, cli, root, destination, pdftotext, pdftoppm = sys.argv[1:]
root, dest = pathlib.Path(root), pathlib.Path(destination)
dest.mkdir(parents=True, exist_ok=True)
mark = root / 'fixtures/samples/pdf-highlights-n6-20260016.mark'
# The fixture's HIGHLIGHTINFO block is at offset 24. Read its raw JSON without
# using the C reader or the renderer's placement calculations.
raw_mark=mark.read_bytes()
length=struct.unpack_from('<I',raw_mark,24)[0]
highlights=json.loads(base64.b64decode(raw_mark[28:28+length]))
markers=[]
for item in highlights['0']:
    if item['annotationType']==0:
        for r in item['mupdfRectList']:
            markers.append(((r['x0']+r['x1'])/2,(r['y0']+r['y1'])/2))

def make(path, count=4, huge=False, encrypted=False, markers=False):
    w = PdfWriter()
    for i in range(count):
        p = w.add_blank_page(612 if i % 2 == 0 else 500, 792 if i % 2 == 0 else 700)
        if huge:
            p.mediabox.upper_right = (1e20, 1e20)
        if i == 1:
            p.rotate(90)
            p.cropbox.lower_left = (10, 20)
            p.cropbox.upper_right = (480, 680)
        stream = DecodedStreamObject()
        # Blue markers lie inside document rectangles from the fixture JSON.
        ink = ''.join(f'0 0 1 rg {x-1} {792-y-1} 2 2 re f\n' for x,y in globals()['markers']) if markers and i == 0 else ''
        stream.set_data((f'BT /F1 12 Tf 40 740 Td (source page {i+1}) Tj ET\n'+ink).encode())
        font = DictionaryObject({NameObject('/Type'): NameObject('/Font'), NameObject('/Subtype'): NameObject('/Type1'), NameObject('/BaseFont'): NameObject('/Helvetica')})
        p[NameObject('/Resources')] = DictionaryObject({NameObject('/Font'): DictionaryObject({NameObject('/F1'): w._add_object(font)})})
        p[NameObject('/Contents')] = w._add_object(stream)
        link = DictionaryObject({NameObject('/Type'): NameObject('/Annot'), NameObject('/Subtype'): NameObject('/Link'), NameObject('/Rect'): ArrayObject([FloatObject(x) for x in (40, 700, 150, 720)]), NameObject('/A'): DictionaryObject({NameObject('/S'): NameObject('/URI'), NameObject('/URI'): TextStringObject('https://example.org/source')})})
        p[NameObject('/Annots')] = ArrayObject([w._add_object(link)])
    if count:
        w.add_outline_item('Original chapter', 0)
    if encrypted:
        w.encrypt('secret')
    w.write(path)

source, output = dest/'source.pdf', dest/'annotated.pdf'
make(source, markers=True)
result = subprocess.run([exe, str(mark), str(source), str(output)], capture_output=True, text=True)
assert result.returncode == 0, result.stderr
src, dst = PdfReader(source), PdfReader(output)
assert len(src.pages) == len(dst.pages)
assert src.outline[0].title == dst.outline[0].title
for a, b in zip(src.pages, dst.pages):
    for box in ('mediabox', 'cropbox', 'bleedbox', 'trimbox', 'artbox'):
        assert list(getattr(a, box)) == list(getattr(b, box))
    assert a.rotation == b.rotation
    assert a['/Annots'][0].get_object()['/A'] == b['/Annots'][0].get_object()['/A']
for i in range(1, len(src.pages)+1):
    def text(path):
        return subprocess.check_output([pdftotext, '-f', str(i), '-l', str(i), str(path), '-'])
    assert text(source) == text(output)
# An independent Poppler raster must darken the blue source marker underneath
# the highlight; this exercises the actual exported PDF, not the mapping helper.
for name, path in [('source', source), ('annotated', output)]:
    subprocess.run([pdftoppm, '-f', '1', '-singlefile', '-r', '72', str(path), str(dest/name)], check=True)
def ppm(path):
    data = path.read_bytes()
    header, dimensions, maximum, pixels = data.split(b'\n', 3)
    assert header == b'P6' and maximum == b'255'
    width, height = map(int, dimensions.split())
    return width, height, pixels
w, h, before = ppm(dest/'source.ppm')
ww, hh, after = ppm(dest/'annotated.ppm')
assert (w, h) == (ww, hh)
# Rectangles come from mupdfRectList via the reader, independently of screen fit.
for line in result.stdout.splitlines():
    page, x, y, width, height = map(float, line.split())
    if page != 1:
        continue
    px, py = int(x+width/2), int(y+height/2)
    assert min(after[(py*w+px)*3:(py*w+px)*3+3]) < 240, line
for x,y in markers:
    idx=(int(y)*w+int(x))*3
    assert before[idx+2]>240 and after[idx+2]<230
    # The retained blue channel proves the ink multiplies the source colour.
    assert after[idx+2]>after[idx]

# Strict mode validates paired geometry on the known Letter layout.
subprocess.run([exe, str(mark), str(source), str(dest/'strict.pdf'), 'strict'], check=True, capture_output=True)
mismatch=dest/'mismatch.pdf'
wbad=PdfWriter(clone_from=source)
wbad.pages[0].mediabox.upper_right=(800,900)
wbad.write(mismatch)
r=subprocess.run([exe,str(mark),str(mismatch),str(dest/'bad.pdf'),'strict'],capture_output=True)
assert r.returncode==2 and b'geometry disagree' in r.stderr
# Page-range export selects source indexes, including sparse unannotated pages.
subprocess.run([cli,str(mark),str(dest/'range.pdf'),'2','--source-pdf',str(source)],check=True)
selected=PdfReader(dest/'range.pdf')
assert len(selected.pages)==1 and selected.pages[0].rotation==90
assert list(selected.pages[0].cropbox)==list(src.pages[1].cropbox)
assert subprocess.check_output([pdftotext,'-f','2','-l','2',str(source),'-'])==subprocess.check_output([pdftotext,str(dest/'range.pdf'),'-'])
# Sparse ink is also placed on the rotated, cropped page and mixed-size pages.
subprocess.run([exe,str(mark),str(source),str(dest/'repeated.pdf'),'4'],check=True)
rep=PdfReader(dest/'repeated.pdf')
for a,b in zip(src.pages,rep.pages):
    assert list(a.mediabox)==list(b.mediabox) and a.rotation==b.rotation
    assert b['/Contents'] != a['/Contents']
epub=root/'fixtures/samples/epub-n6-20260016.mark'
r=subprocess.run([exe,str(epub),str(source),str(dest/'epub.pdf'),'epub'],capture_output=True)
assert r.returncode==2 and b'EPUB' in r.stderr
# The tool names unusable sources, honours --strict and --source-kind, and
# prints the overlay's warnings.
def tool(*args):
    return subprocess.run([cli, *map(str, args)], capture_output=True, text=True)
empty=dest/'empty.pdf'
empty.write_bytes(b'')
for path, expected in [(dest/'missing.pdf', 'No such file'), (dest, 'not a regular file'),
                       (empty, 'is empty')]:
    r=tool(mark, dest/'cli.pdf', '--source-pdf', path)
    assert r.returncode==1 and expected in r.stderr, (path, r.stderr)
r=tool(mark, dest/'cli.pdf', '--source-pdf', source, '--maximum-source-bytes', 100)
assert r.returncode==1 and 'exceeds the source byte limit' in r.stderr, r.stderr
r=tool(mark, dest/'cli.pdf', '--source-pdf', source, '--source-kind', 'docx')
assert r.returncode==2 and '--source-kind must be pdf or epub' in r.stderr, r.stderr
for option in (['--strict'], ['--source-kind', 'pdf']):
    r=tool(mark, dest/'cli.pdf', *option)
    assert r.returncode==2 and option[0]+' requires --source-pdf' in r.stderr, r.stderr
r=tool(mark, dest/'cli.pdf', '--source-pdf', source, '--source-kind', 'epub')
assert r.returncode==1 and 'EPUB' in r.stderr, r.stderr
r=tool(mark, dest/'cli.pdf', '--source-pdf', mismatch, '--strict')
assert r.returncode==1 and 'geometry disagree' in r.stderr, r.stderr
r=tool(mark, dest/'cli.pdf', '--source-pdf', mismatch)
assert r.returncode==0 and 'warning: document page 1: mark and PDF highlight geometry disagree' in r.stderr, r.stderr
# The EPUB fixture's only sheet is on page 22. Against five pages everything
# is dropped, the tool says so and the output is the source itself. Against
# forty, ink lands with no pair to confirm or refute it: nothing is said about
# belonging, and strict mode cannot refuse it either.
for count in (5, 40):
    unrelated=dest/f'unrelated-{count}.pdf'
    make(unrelated, count=count)
    out=dest/f'unrelated-{count}-out.pdf'
    r=tool(epub, out, '--source-pdf', unrelated)
    assert r.returncode==0, (count, r.stderr)
    assert ('warning: all annotations dropped' in r.stderr)==(count==5), (count, r.stderr)
    assert 'belong' not in r.stderr and 'paired' not in r.stderr, (count, r.stderr)
    assert (out.read_bytes()==unrelated.read_bytes())==(count==5)
r=tool(epub, dest/'unrelated-strict.pdf', '--source-pdf', dest/'unrelated-40.pdf', '--strict')
assert r.returncode==0, r.stderr
# Both PDF-backed fixture sheets exercise their independent sparse numbering.
digest=root/'fixtures/supernote-typescript/digest-n5-20230015-test.mark'
subprocess.run([exe,str(digest),str(source),str(dest/'digest.pdf')],check=True)
# Raster and PNG preview use the same source page, with no loss of colour.
subprocess.run([cli, str(mark), str(dest/'preview.png'), '--source-pdf', str(source)], check=True)
def png_pixels(path):
    data=path.read_bytes()
    width,height,depth,kind,_,_,interlace=struct.unpack('>IIBBBBB',data[16:29])
    assert depth==8 and kind in (2,6) and interlace==0
    channels=4 if kind==6 else 3
    compressed=b''
    offset=8
    while offset<len(data):
        length=struct.unpack_from('>I',data,offset)[0]
        if data[offset+4:offset+8]==b'IDAT':
            compressed+=data[offset+8:offset+8+length]
        offset+=length+12
    raw=zlib.decompress(compressed)
    previous=bytearray(width*channels)
    rows=[]
    for y in range(height):
        offset=y*(width*channels+1)
        mode=raw[offset]
        row=bytearray(raw[offset+1:offset+1+width*channels])
        for i in range(len(row)):
            a=row[i-channels] if i>=channels else 0
            b=previous[i]
            c=previous[i-channels] if i>=channels else 0
            if mode==1: predictor=a
            elif mode==2: predictor=b
            elif mode==3: predictor=(a+b)//2
            elif mode==4:
                q=a+b-c
                aa,bb,cc=abs(q-a),abs(q-b),abs(q-c)
                predictor=a if aa<=bb and aa<=cc else b if bb<=cc else c
            else:
                assert mode==0
                predictor=0
            row[i]=(row[i]+predictor)&255
        rows.append(row)
        previous=row
    return width,height,channels,rows
pw,ph,channels,rows=png_pixels(dest/'preview.png')
assert (pw,ph)==(612,792)
for x,y in markers:
    pixel=rows[int(y)][int(x)*channels:int(x)*channels+3]
    assert pixel[2]<230 and pixel[2]>pixel[0]
for name, kwargs in [('encrypted', {'encrypted': True}), ('huge', {'huge': True}), ('zero', {'count': 0}), ('many', {'count': 10001})]:
    path = dest/(name+'.pdf'); make(path, **kwargs)
    r = subprocess.run([exe, str(mark), str(path), str(dest/'bad.pdf')], capture_output=True, timeout=30)
    assert r.returncode == 2, (name, r.stderr)
for name, data in [('truncated', source.read_bytes()[:100]), ('mutated', source.read_bytes().replace(b'/Pages', b'/Bogus'))]:
    path=dest/(name+'.pdf'); path.write_bytes(data)
    r=subprocess.run([exe, str(mark), str(path), str(dest/'bad.pdf')], capture_output=True, timeout=30)
    assert r.returncode == 2, (name, r.stderr)
# A 300-page document carries ink on its first page; performance runs use a
# separate synthetic mark to distribute sheets across thirty pages.
print('overlay preservation, document-coordinate geometry and hostile PDFs passed')
