# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Independent sheet residuals, marker coverage and regional preview checks."""
import base64
import json
import math
import pathlib
import struct
import subprocess
import sys
import zlib
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (ArrayObject, BooleanObject, DecodedStreamObject,
                           DictionaryObject, FloatObject, NameObject)

probe, cli, root, work, poppler = sys.argv[1:]
root, work = pathlib.Path(root), pathlib.Path(work)
work.mkdir(parents=True, exist_ok=True)
# Reuse only the file decoders, never the library's coordinate calculations.
source = (root / 'check_overlay.py').read_text()
ns = {'struct': struct, 'zlib': zlib}
exec(source[source.index('def png_pixels('):source.index('pw,ph,channels,rows=')], ns)
png = ns['png_pixels']

def run(*args):
    r = subprocess.run(list(map(str, args)), capture_output=True, text=True)
    assert r.returncode == 0, (args, r.stderr)
    return r.stdout

def render(path, page, scale, stem):
    run(poppler, '-cropbox', '-f', page + 1, '-l', page + 1,
        '-singlefile', '-r', 72 * scale, path, stem)
    raw = pathlib.Path(str(stem) + '.ppm').read_bytes()
    _, dims, _, pixels = raw.split(b'\n', 3)
    w, h = map(int, dims.split())
    return w, h, pixels

def make(path, count, media, crop, rects=(), rotation=0, tagged=False):
    writer = PdfWriter()
    for i in range(count):
        p = writer.add_blank_page(*media)
        p.cropbox = ArrayObject([FloatObject(x) for x in crop])
        p.rotate(rotation)
        stream = DecodedStreamObject()
        content = ''.join(f'0 0 1 rg {crop[0]+(r[0]+r[2])/2-2} {crop[3]-(r[1]+r[3])/2-2} 4 4 re f\n' for r in rects)
        stream.set_data(content.encode())
        p[NameObject('/Contents')] = writer._add_object(stream)
    if tagged:
        writer._root_object[NameObject('/MarkInfo')] = DictionaryObject({NameObject('/Marked'): BooleanObject(True)})
        writer._root_object[NameObject('/StructTreeRoot')] = writer._add_object(DictionaryObject({NameObject('/Type'): NameObject('/StructTreeRoot'), NameObject('/K'): ArrayObject()}))
    writer.write(path)

cases = [
    ('pdf-highlights-n6-20260016.mark', (1404,1872), (612,792), (0,0,612,792)),
    ('pdf-ink-highlights-tall-n6-20260016.mark', (1404,1872), (522,756), (0,0,522,756)),
    ('pdf-ink-highlights-n5-20260016.mark', (1920,2560), (549,684), (9,9,549,675)),
]
for name, sheet, media, crop in cases:
    mark = root / 'fixtures/samples' / name
    raw = mark.read_bytes()
    info = json.loads(base64.b64decode(raw[28:28+struct.unpack_from('<I',raw,24)[0]]))
    page = int(next(iter(info)))
    w, h = crop[2]-crop[0], crop[3]-crop[1]
    fit = min(sheet[0]/w, sheet[1]/h)
    ox, oy = (sheet[0]-w*fit)/2, (sheet[1]-h*fit)/2
    rectangles = []
    for item in info[str(page)]:
        for a,b in zip(item['rnRectList'], item['mupdfRectList']):
            residual = [a['left']-(b['x0']*fit+ox), a['right']-(b['x1']*fit+ox)]
            if item['annotationType'] == 0:
                residual += [a['top']-(b['y0']*fit+oy), a['bottom']-(b['y1']*fit+oy)]
                rectangles.append((b['x0'],b['y0'],b['x1'],b['y1']))
            assert max(map(abs,residual)) <= 2, (name,residual)
            print(name, 'paired residual pixels', [round(x,4) for x in residual])
    src, out = work/(name+'.pdf'), work/(name+'-out.pdf')
    make(src, page+1, media, crop, rectangles, tagged=True)
    run(probe,mark,src,out,1,page,'all')
    original, exported = PdfReader(src), PdfReader(out)
    assert exported.trailer['/Root']['/StructTreeRoot'] == original.trailer['/Root']['/StructTreeRoot']
    assert b'/Artifact BMC' in exported.pages[page].get_contents().get_data()
    pw,ph,before = render(src,page,1,work/'before')
    _,_,after = render(out,page,1,work/'after')
    for scale in (1,2):
        preview = work/(name+f'-{scale}.png')
        run(probe,mark,src,preview,scale,page,'all')
        iw,ih,ch,rows = png(preview)
        for x0,y0,x1,y1 in rectangles:
            x,y = int((x0+x1)/2),int((y0+y1)/2)
            at=(y*pw+x)*3
            assert before[at+2] > 240 and after[at+2] < 230
            pixel=rows[int((y0+y1)/2*scale)][int((x0+x1)/2*scale)*ch:][:3]
            assert pixel[2] < 230 and pixel[2] > pixel[0]
    # Standalone rendering supplies the sheet's ink bounds independently of
    # the overlay transform; highlights are excluded from both ink images.
    sheet_png=work/(name+'-sheet.png')
    run(cli,mark,sheet_png)
    sw,sh,ch,rows=png(sheet_png)
    points=[]
    for y,row in enumerate(rows):
        for x in range(sw):
            px=row[x*ch:x*ch+ch]
            if ch==4 and px[3]>200 and max(px[:3])<100:
                points.append((x,y))
    if points:
        expected=(min(x for x,y in points),min(y for x,y in points),max(x for x,y in points)+1,max(y for x,y in points)+1)
        expected=((expected[0]-ox)/fit,(expected[1]-oy)/fit,(expected[2]-ox)/fit,(expected[3]-oy)/fit)
        blank=work/(name+'-blank.pdf')
        make(blank,page+1,media,crop)
        ink=work/(name+'-ink.png')
        run(probe,mark,blank,ink,1,page,'ink')
        iw,ih,ch,rows=png(ink)
        points=[(x,y) for y,row in enumerate(rows) for x in range(iw) if max(row[x*ch:x*ch+3])<100]
        actual=(min(x for x,y in points),min(y for x,y in points),max(x for x,y in points)+1,max(y for x,y in points)+1)
        assert max(abs(a-b) for a,b in zip(expected,actual)) < 2, (name,expected,actual)
        print(name,'ink bounds points',actual,'expected',expected)
    # A mask-blind reader must retain white paper and visible gray ink.
    no_mask=PdfWriter(clone_from=out)
    def strip(obj):
        if isinstance(obj, DictionaryObject):
            obj.pop('/SMask',None)
            for v in obj.values():
                if isinstance(v,(DictionaryObject,ArrayObject)): strip(v)
        elif isinstance(obj,ArrayObject):
            for v in obj: strip(v)
    for obj in no_mask._objects: strip(obj)
    fallback=work/(name+'-nomask.pdf')
    no_mask.write(fallback)
    _,_,pixels=render(fallback,page,1,work/'nomask')
    assert sum(v>240 for v in pixels)/len(pixels) > .7

# Dense coloured regions make a centred 1x page fail locally even when most
# of a real document is white. All rotations include a nonzero crop origin.
for rotation in (0,90,180,270):
    src=work/f'regions-{rotation}.pdf'
    writer=PdfWriter()
    p=writer.add_blank_page(150,190)
    p.cropbox=ArrayObject([FloatObject(v) for v in (11,17,131,177)])
    p.rotate(rotation)
    stream=DecodedStreamObject()
    content=[]
    for y in range(4):
        for x in range(3):
            content.append(f'{(x+1)/4} {(y+1)/5} 0.2 rg {11+x*40} {17+y*40} 40 40 re f')
    stream.set_data('\n'.join(content).encode())
    p[NameObject('/Contents')]=writer._add_object(stream)
    writer.write(src)
    # Eight is the scale ceiling and fits the default pixel limit here.
    for scale in (.5,1,1.5,2,8):
        path=work/f'regions-{rotation}-{scale}.png'
        run(probe,'empty',src,path,scale,0,'all')
        w,h,ch,rows=png(path)
        rw,rh,ref=render(src,0,scale,work/'reference')
        assert (w,h)==(rw,rh)
        for gy in range(8):
            for gx in range(6):
                diffs=[]
                for y in range(int((gy+.2)*h/8),int((gy+.8)*h/8),max(1,int(scale))):
                    for x in range(int((gx+.2)*w/6),int((gx+.8)*w/6),max(1,int(scale))):
                        diffs.extend(abs(rows[y][x*ch+c]-ref[(y*w+x)*3+c]) for c in range(3))
                assert sum(diffs)/len(diffs)<5, (rotation,scale,gx,gy,sum(diffs)/len(diffs))
print('independent geometry, ink bounds, fallback and regional scales passed')

# Bounded xref recovery accepts nearby complete headers, not missing objects.
import re
base=(work/'regions-0.pdf').read_bytes()
xref=base.rfind(b'\nxref')
for delta in (-1,2,32,80):
    damaged=base[:xref]+re.sub(rb'(\d{10}) (\d{5}) n',lambda m:b'%010d %s n'%(int(m[1])+delta,m[2]),base[xref:])
    path=work/f'offset-{delta}.pdf'
    path.write_bytes(damaged)
    r=subprocess.run([probe,'empty',str(path),str(work/'recovered.pdf'),'1','0','all'],capture_output=True)
    assert (r.returncode==0)==(delta!=80), (delta,r.stderr)
for delta in (-2,3,80):
    damaged=re.sub(rb'startxref\s+(\d+)',lambda m:b'startxref\n'+str(int(m[1])+delta).encode(),base)
    path=work/f'start-{delta}.pdf'
    path.write_bytes(damaged)
    r=subprocess.run([probe,'empty',str(path),str(work/'recovered.pdf'),'1','0','all'],capture_output=True)
    assert (r.returncode==0)==(delta!=80), (delta,r.stderr)

def xref_stream(trim):
    data=b'%PDF-1.5\n'
    offsets=[0]
    for i,body in enumerate([b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Count 1 /Kids [3 0 R] >>',b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 120 160] >>'],1):
        offsets.append(len(data))
        data+=str(i).encode()+b' 0 obj '+body+b' endobj\n'
    offsets.append(len(data))
    rows=b''.join(bytes([bool(i)])+struct.pack('>IH',at,0 if i else 65535) for i,at in enumerate(offsets))
    compressed=zlib.compress(rows)
    if trim: compressed=compressed[:-trim]
    data+=b'4 0 obj << /Type /XRef /Size 5 /Root 1 0 R /W [1 4 2] /Length '+str(len(compressed)).encode()+b' /Filter /FlateDecode >> stream\n'+compressed+b'\nendstream endobj\nstartxref\n'+str(offsets[-1]).encode()+b'\n%%EOF\n'
    return data
for trim in (0,4,12):
    path=work/f'flate-{trim}.pdf'
    path.write_bytes(xref_stream(trim))
    r=subprocess.run([probe,'empty',str(path),str(work/'recovered.pdf'),'1','0','all'],capture_output=True)
    assert (r.returncode==0)==(trim!=12), (trim,r.stderr)

# A signature field is reported before writing, and range export removes its
# signed value. Full export must retain the original signed byte prefix.
writer=PdfWriter()
writer.add_blank_page(612,792)
sig=writer._add_object(DictionaryObject({NameObject('/Type'):NameObject('/Sig'),NameObject('/ByteRange'):ArrayObject([FloatObject(v) for v in (0,10,20,10)]),NameObject('/Contents'):NameObject('/Dummy')}))
field=writer._add_object(DictionaryObject({NameObject('/FT'):NameObject('/Sig'),NameObject('/V'):sig}))
writer._root_object[NameObject('/AcroForm')]=writer._add_object(DictionaryObject({NameObject('/Fields'):ArrayObject([field])}))
src=work/'signed.pdf'
writer.write(src)
out=work/'signed-out.pdf'
result=run(probe,root/'fixtures/samples/pdf-highlights-n6-20260016.mark',src,out,1,0,'all')
assert 'signatures=1' in result
assert out.read_bytes().startswith(src.read_bytes())

# Appearance-only pages must draw even without a page content stream.
writer=PdfWriter()
p=writer.add_blank_page(120,160)
ap=DecodedStreamObject()
ap.set_data(b'1 0 0 rg 0 0 40 30 re f')
ap.update({NameObject('/Type'):NameObject('/XObject'),NameObject('/Subtype'):NameObject('/Form'),NameObject('/BBox'):ArrayObject([FloatObject(v) for v in (0,0,40,30)])})
a=DictionaryObject({NameObject('/Type'):NameObject('/Annot'),NameObject('/Subtype'):NameObject('/Stamp'),NameObject('/Rect'):ArrayObject([FloatObject(v) for v in (20,30,60,60)]),NameObject('/AP'):DictionaryObject({NameObject('/N'):writer._add_object(ap)})})
p[NameObject('/Annots')]=ArrayObject([writer._add_object(a)])
src=work/'appearance.pdf'
writer.write(src)
out=work/'appearance.png'
run(probe,'empty',src,out,2,0,'all')
w,h,ch,rows=png(out)
assert rows[220][80*ch]>240 and rows[220][80*ch+1]<30
# Both backends draw a check-box widget whether or not the AcroForm lists it,
# draw the stamp, and leave the hidden square and the popup undrawn. Points on
# the 300x400 page: widget (120,280) green, stamp (40,355) red, hidden square
# (175,225) and popup (225,175) white.
for seed in ('appearances', 'acroform'):
    out=work/f'{seed}.png'
    run(probe,'empty',root/'data/pdf-seeds'/f'{seed}.pdf',out,1,0,'all')
    w,h,ch,rows=png(out)
    pixel=lambda x,y: tuple(rows[y][x*ch:x*ch+3])
    r,g,b=pixel(120,280)
    assert g>200 and r<60 and b<60, (seed,'widget',pixel(120,280))
    r,g,b=pixel(40,355)
    assert r>200 and g<60 and b<60, (seed,'stamp',pixel(40,355))
    for x,y in ((175,225),(225,175)):
        assert min(pixel(x,y))>240, (seed,'hidden or popup',pixel(x,y))
print('bounded recovery, signature diagnostics and annotation appearances passed')
