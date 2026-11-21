# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

import pathlib
import pypdf
import subprocess
import sys
import tempfile
import os
import time
import xml.etree.ElementTree as ET

tool, ppm, fixtures, recognition, structure, hostile, cases, cairo, fonts, density = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="pdf-text-", dir=os.getcwd()) as directory:
    root = pathlib.Path(directory)
    for executable in (recognition, structure, hostile):
        subprocess.run([executable, fixtures], cwd=root, check=True)
    line_modes = ("line_edge", "line_dot03", "line_dot06", "line_dot1", "line_same_x", "line_disjoint", "line_slivers", "line_tall_base", "line_tall", "line_tall_same")
    for mode in (*line_modes, "order", "order_clean", "order_single", "narrow", "scripts", "longfirst", "long5000", "long", "edge", "links3", "uri", "big00", "bigemoji", "size1x1", "size0.000001x5", "size200x200"):
        for suffix in ("", "-off"):
            subprocess.run([cases, mode, str(root / (mode + suffix + ".pdf"))] + (["off"] if suffix else []), check=True)

    expected_order = "Alpha Beta : Gamma Delta .\nEpsilon Zeta Eta\nЖёлтый цвет"
    for mode, expected in (("order", expected_order), ("order_clean", expected_order), ("order_single", expected_order.splitlines()[0])):
        for flags in ([], ["-raw"]):
            extracted = subprocess.check_output([tool, *flags, str(root / (mode + ".pdf")), "-"], text=True)
            # Poppler may expand a physical gap to several ASCII spaces.
            actual = "\n".join(" ".join(line.split()) for line in extracted.strip().splitlines())
            assert actual == expected, (mode, flags, extracted)
    for mode in line_modes:
        expected = ("Echo Echo Echo" if mode == "line_same_x" else "Alpha Beta .") + "\nGamma Delta"
        if mode == "line_disjoint":
            expected = "Alpha a .\nGamma Delta"
        elif mode == "line_slivers":
            expected = ". . . . . . . . Gamma Delta"
        elif mode.startswith("line_tall"):
            expected = "Hello world" + ("" if mode == "line_tall_base" else " Tall" if mode == "line_tall_same" else "\nTall")
        for flags in ([], ["-raw"]):
            extracted = subprocess.check_output([tool, *flags, str(root / (mode + ".pdf")), "-"], text=True)
            actual = "\n".join(" ".join(line.split()) for line in extracted.strip().splitlines())
            line_expected = "Hello world\n\nTall" if mode == "line_tall" and not flags else expected
            assert actual == line_expected, (mode, flags, extracted)
    def hello_bounds(mode):
        bbox = subprocess.check_output([tool, "-bbox", str(root / (mode + ".pdf")), "-"], text=True)
        return [word.attrib for word in ET.fromstring(bbox).iter("{http://www.w3.org/1999/xhtml}word")
                if word.text in ("Hello", "world")]
    baseline = hello_bounds("line_tall_base")
    assert len(baseline) == 2
    assert hello_bounds("line_tall") == baseline, (hello_bounds("line_tall"), baseline)
    for word in hello_bounds("line_tall_same"):
        center = (float(word["yMin"]) + float(word["yMax"])) / 2
        assert 10 * 72 / 25.4 <= center <= 16 * 72 / 25.4, word
        assert abs(float(word["yMax"]) - float(word["yMin"]) - 6 * 72 / 25.4) < 0.1, word
    bbox = subprocess.check_output([tool, "-bbox", str(root / "line_same_x.pdf"), "-"], text=True)
    echoes = sorted((float(word.attrib["xMin"]), float(word.attrib["xMax"]))
                    for word in ET.fromstring(bbox).iter("{http://www.w3.org/1999/xhtml}word")
                    if word.text == "Echo")
    assert len(echoes) == 3 and all(a[1] < b[0] for a, b in zip(echoes, echoes[1:])), echoes
    print("Earlier line cases stay intact; lone tall boxes preserve ordinary word rectangles")
    invisible = (0x00ad, 0x200b, 0x200c, 0x200d, 0x2060, 0xfeff)
    extracted = pypdf.PdfReader(root / "order.pdf").pages[0].extract_text()
    assert not any(chr(c) in extracted for c in invisible), repr(extracted)
    print("Recognition line order, explicit separators, punctuation and format filtering passed")

    subprocess.run([density, "heights", "3000", str(root / "heights.pdf")], check=True)
    for pdf in sorted(root.glob("*.pdf")):
        listing = subprocess.check_output([fonts, str(pdf)], text=True)
        assert "Type 3" not in listing, (pdf, listing)
    assert (root / "heights.pdf").stat().st_size <= 160000
    print("All fixtures and 3000 distinct heights: no Type 3 fonts; heights PDF <= 160000 bytes")

    on = subprocess.check_output([tool, str(pathlib.Path(directory) / "unicode.pdf"), "-"], text=True)
    for word in ("Searchable", "Привет", "你好世界"):
        assert word in on, (word, on)
    off = subprocess.check_output([tool, str(pathlib.Path(directory) / "unicode-off.pdf"), "-"], text=True)
    assert not off.strip(), off
    print("Latin, Cyrillic and CJK extraction passed; disabled recognition has no text")

    root = pathlib.Path(directory)
    for on_pdf in sorted(root.glob("fixture-*.pdf")) + sorted(root.glob("hostile-*.pdf")) + [root / "unicode.pdf"] + [root / (mode + ".pdf") for mode in line_modes]:
        if on_pdf.stem.endswith("-off"):
            continue
        off_pdf = on_pdf.with_stem(on_pdf.stem + "-off")
        def raster(path):
            return subprocess.check_output([ppm, "-r", "50", str(path)])
        assert raster(on_pdf) == raster(off_pdf), on_pdf
        if on_pdf.name.startswith("hostile-"):
            extracted = subprocess.check_output([tool, str(on_pdf), "-"], text=True)
            assert "Second" in extracted, (on_pdf, extracted)
    print("All fixture, three-script and hostile recognition rasters are identical")

    # Objects may sit in compressed object streams, so the check reads every
    # indirect object through pypdf. The uri case, whose web links both
    # backends emit, proves that the scan finds URI actions at all.
    def uri_actions(path):
        reader = pypdf.PdfReader(path)
        def count(value):
            # Direct dictionaries and arrays are searched; references are not
            # followed, since every indirect object is visited below.
            if isinstance(value, pypdf.generic.DictionaryObject):
                return ("/URI" in value) + sum(count(v) for v in value.values())
            if isinstance(value, pypdf.generic.ArrayObject):
                return sum(count(v) for v in value)
            return 0
        return sum(count(reader.get_object(number)) for number in range(1, int(reader.trailer["/Size"])))
    assert uri_actions(root / "uri.pdf") > 0
    assert uri_actions(root / "hostile-scheme.pdf") == 0
    print("Disallowed URI scheme is absent")

    for mode, length in (("longfirst", 600), ("long5000", 5000), ("long", 65536)):
        result = subprocess.run([tool, "-raw", str(root / (mode + ".pdf")), "-"], capture_output=True, text=True, check=True)
        assert not result.stderr, result.stderr
        expected = "".join(chr(97 + i % 26) for i in range(length))
        assert expected in result.stdout.split(), (mode, len(result.stdout))
        if mode == "long":
            geometric = subprocess.check_output([tool, str(root / "long.pdf"), "-"], text=True).split()
            assert len(geometric) == 1 and 0 < len(geometric[0]) < length
            cursor = iter(expected)
            assert all(any(c == wanted for c in cursor) for wanted in geometric[0])
            print(f"Dense word: raw {length} exact bytes; default {len(geometric[0])} bytes, ordered subsequence")
    if cairo == "ON":
        time.sleep(1.1)
        subprocess.run([cases, "scripts", str(root / "deterministic.pdf")], check=True)
        assert (root / "scripts.pdf").read_bytes() == (root / "deterministic.pdf").read_bytes()
        print("Separate Cairo runs produced byte-identical PDFs")
    narrow = subprocess.check_output([tool, "-raw", str(root / "narrow.pdf"), "-"], text=True)
    assert narrow.split() == ["a"], narrow
    print("Narrow single-glyph word survives extraction")
    edge = subprocess.check_output([tool, str(root / "edge.pdf"), "-"], text=True)
    for word in ("LeftEdge", "ClippedLeft", "Inside", "NearLeft"):
        assert word in edge, (word, edge)
    scripts = subprocess.check_output([tool, str(root / "scripts.pdf"), "-"], text=True)
    assert not any(chr(c) in scripts for c in (0, 1, 2, 27, 127)), repr(scripts)
    assert "abcd" in scripts and "[31m" in scripts and "CleanText" in scripts
    if cairo == "ON":
        for word in ("مرحبا", "שלום", "नमस्ते"):
            assert word in scripts, (word, scripts)
    else:
        assert "שלום" in scripts
        assert "مرحبا" in scripts and "नमस्ते" not in scripts, scripts
        for fragment in ("नम", "स्ते"):
            assert fragment in scripts, (fragment, scripts)
    for mode in (*line_modes, "order", "order_clean", "order_single", "scripts", "edge", "longfirst", "big00", "bigemoji"):
        assert raster(root / (mode + ".pdf")) == raster(root / (mode + "-off.pdf")), mode
    print("Long words, clip edges, script extraction and control filtering passed")

    extracted = pypdf.PdfReader(root / "scripts.pdf").pages[0].extract_text()
    if cairo == "ON":
        # pypdf reverses RTL runs even with logical clusters and RTL positions.
        assert "ابحرم" in extracted and "םולש" in extracted
        assert "नमस्ते" in extracted
    else:
        assert "नम" in extracted and "स्ते" in extracted
    doc = pypdf.PdfReader(root / "links3.pdf")
    for i, page in enumerate(doc.pages):
        annotations = page["/Annots"]
        assert len(annotations) == (2 if i == 2 else 1)
        a = annotations[0].get_object()
        expected = [4.8, float(page.mediabox.top) - 16.8, 28.8, float(page.mediabox.top) - 7.2]
        assert all(abs(float(got) - want) < 1.1 for got, want in zip(a["/Rect"], expected))
    print("pypdf script behavior and three-page link positions passed")
