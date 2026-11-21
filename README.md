# snrender-lib

A C23 library for rendering parsed Supernote notebooks. It accepts immutable
`sn_notebook` values from SNNote and writes page rasters, PNG, PDF and SVG.

## What it renders

Visible stored layers, in their recorded order, through the notebook palette.
Native RLE layers preserve exact gray values. ImageIO or GdkPixbuf decodes PNG
and JPEG layers. Bitmaps contain premultiplied RGBA; rasters expose straight
gray and alpha. Scale 1 means one output pixel per stored pixel. Downscaling uses deterministic integer area averaging in premultiplied space.
Scale 1 stays exact; enlargement uses nearest-neighbour sampling.

PDF and SVG embed the composed page image. Stroke contours do not reproduce
stored pixels exactly, so they do not replace bitmap ink. PDF adds title
outlines, internal and web links, and an invisible searchable recognition layer.
Keywords and document highlights are not emitted as PDF annotations.

A `.mark` renders its own pages over transparency through the original APIs.
The source-overlay APIs added in 0.2.0 accept a PDF and register those sheets
to its pages. EPUB reflow is unsupported; see Annotated source PDFs below.

## Build and install

CMake 3.21+, a C23 compiler, zlib and SNNote are required. Static is the default;
`BUILD_SHARED_LIBS=ON` selects a shared library. CMake first finds an installed
`SNNote` 1.0 package, then uses a sibling `snnote-lib` checkout, then FetchContent
from `https://github.com/altessa-s/snnote-lib` at the immutable commit in
`SNR_READER_COMMIT` in CMakeLists.txt. Releases bump that value.
`SNRENDER_READER_SOURCE` explicitly selects a source checkout, required for
fully instrumented sanitizer and fuzz builds.

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DSNRENDER_WERROR=ON
cmake --build build/release --target all check
cmake --install build/release --prefix "$PWD/build/install"
```

Apple builds use CoreGraphics, CoreText, ImageIO and PDFKit. The installed target is
`SNRender::snrender`, package `SNRender`, and pkg-config module `snrender`.
Tests and the command-line tool default on in a top-level build. LTO defaults
on for optimized shared builds. `SNRENDER_BUILD_TESTS`, `SNRENDER_BUILD_TOOLS`,
`SNRENDER_WERROR`, `SNRENDER_LTO`, `SNRENDER_BUILD_FUZZERS`,
`SNRENDER_BUILD_APPLE` and `SNRENDER_BUILD_CAIRO` control the build.

## Cairo backend

Cairo >= 1.18 with cairo-ft, FreeType and GdkPixbuf are the default outside Apple platforms. On macOS,
select `SNRENDER_BUILD_CAIRO=ON`; this disables the Apple backend. Source PDF previews and text extraction use PDFium when enabled.
The Linux and Windows release builds enable it. See [CAIRO.md](CAIRO.md) and [LINUX.md](LINUX.md).

### Windows

Use MSYS2 UCRT64 and GCC with C23 support. See [WINDOWS.md](WINDOWS.md).

## Draw a page

```c
#include <snrender/snrender.h>

/* notebook has already been parsed with sn_parse_notebook. */
snr_options options = snr_options_default();
options.scale = 1;
snr_bitmap bitmap = {0};
snr_error error = {0};
snr_status status = snr_page_bitmap(notebook, 0, &options, &bitmap, &error);
if (status == SNR_OK) {
    /* Consume bitmap.pixels, with bitmap.stride bytes per row. */
    snr_bitmap_free(&bitmap);
}
```

## Render a notebook page

```sh
build/release/snrender input.note build/page.png 1
build/release/snrender input.note build/page.svg 1 2
build/release/snrender input.note build/notebook.pdf
```

The tool writes a temporary file beside the destination and replaces the
destination only after rendering and closing succeed. Failed exports preserve
existing files, including when the destination is the source PDF. On POSIX,
new files follow the umask and replacements preserve the destination's mode
when the filesystem supports setting permissions (otherwise the tool warns).
Existing destinations must be writable; read-only files are rejected before
writing.
Atomic replacement requires write permission on the destination directory.
It replaces a destination symlink itself (leaving its target unchanged), and
replaces only the named directory entry of a hard-linked file; other hard links
retain the old contents. The tool does not fall back to direct writes.

The optional page argument is one-based. C entry points use zero-based indexes.
The optional scale controls raster resolution; PDF sheet size stays fixed.
An explicit background color overrides the white notebook or transparent mark
default. `include_background=false` hides the stored template layer.

## Documents and PDFs

`snr_document_pdf` takes a first page and count; `SIZE_MAX` means the remaining
pages. Its options include progress and cancellation callbacks. Recognition is
on by default. Boxes are millimetres from the top left and become PDF points
by multiplying by `72 / 25.4`. Pixel dimensions become points using 226 dpi
for A5X and 300 dpi for the other known devices, with the page's device override.
Landscape pages keep the reader's oriented dimensions.

Title snapshots have no textual labels, so outline entries use stable
`Page N — title M` labels and retain the stored level hierarchy. Web links use
URI actions for http, https and mailto only (case-insensitive schemes).
Other schemes are omitted with a content-free diagnostic. Links into the exported notebook resolve by page ID, falling back
to its stored page number. Other-file and out-of-range links are dropped with a
diagnostic. No linked file is opened.

The `_to_output` entry points write to a versioned callback. The first failure
of a call is its result. A callback's non-OK result that comes first is
returned unchanged, whatever its value, and the callback is not called again.
A cancellation, limit or other error found before the callback refused stays
the result; the bytes that close a PDF may still be offered after it, and a
refusal of them does not replace it. Error messages name the origin: "output
callback failed", "output byte limit exceeded" or "output allocation failed".
Discard partial files. Both PDF backends stream directly in every mode. No PDF
rewriting occurs.
Cairo stores recognition UTF-8 as clusters associated with internal blank
glyphs; Apple uses invisible text and omits colour-font words. Recognition never
paints over the page, performs no OCR, and does not change handwriting. Invalid
UTF-8 and invalid, empty or wholly outside boxes are omitted. Extraction depends
on the reader supporting standard PDF text mappings. PNG streams directly;
SVG holds its bounded embedded PNG before writing base64 chunks.

## SVG output

Each page is a standalone SVG 1.1 document with an embedded PNG data URI.
It has no external assets, fonts, script or network references. Repeated calls
with identical inputs produce identical SVG bytes on the same backend.

## Ownership and failures

Initialize options with `snr_options_default`. Null options select defaults.
All fallible functions return `snr_status` and are `[[nodiscard]]`. On failure,
outputs remain unchanged and intermediate storage is freed. Errors are optional
and change only on failure. Free each owning result with its matching free
function; null is accepted and the result is zeroed.

Known limit: under allocation failure, Cairo can still crash inside its own
PDF code (null dereferences in its out-of-memory handling, seen by failing
single allocations), which this library cannot make safe. The library's own
allocations are checked and reported as `SNR_ERR_OUT_OF_MEMORY`. PDFium also has unsafe out-of-memory handling: injected allocation failures can crash inside PDFium during Cairo previews. Library allocation checks do not make PDFium allocation failure safe.

Each call owns its graphics contexts. Immutable notebooks may be shared between
threads; callbacks execute on the calling thread. Log installation is synchronized,
but callback contexts must outlive calls that could have observed them. Callbacks
must return normally and must not reenter rendering.

Limits bound output pixels, decoded image pixels, document pages, output bytes,
scale, per-page work and cumulative document work. Defaults are 16,777,216 output pixels, 16,777,216
source/decoded pixels, 10,000 PDF pages, 1,000,000,000 work units per page,
100,000,000,000 cumulative work units per PDF call,
512 MiB encoded output per call and scale 1/64 through 8. Recognition words
are limited to 65,536 UTF-8 bytes in notebook PDF exports; image-only outputs
(including mark ink) do not inspect or charge recognition. Cairo additionally limits each output side
to 32,767 pixels. PNG/JPEG encoded bytes and decoded dimensions are charged before
decoding; framing validation polls cancellation in bounded blocks; the reader's RLE limits also apply. Zero limits permit no work.

The tool accepts `--maximum-pixels`, `--maximum-decoded-pixels`,
`--maximum-pages`, `--maximum-work`, `--maximum-document-work`, `--maximum-output-bytes`,
`--minimum-scale` and `--maximum-scale`, each followed by a number.
`--strict-images` opts into aborting on damaged layers. By default both backends
omit a damaged image layer, log a content-free message and continue. ImageIO
and GdkPixbuf may accept different damaged files; that is platform codec
behaviour. Limit, allocation and cancellation errors still abort.

JPEG input must contain EOI; truncated camera files without it are skipped in
default mode and rejected in strict mode. TEM markers are rejected by the shared
preflight on both backends.

Link and recognition boxes are clipped before passing coordinates to platform
APIs. Cairo retains fractional clickable quadrilaterals; its enclosing PDF
`/Rect` is an integer-rounded bounding box, as emitted by Cairo's public API.

## Validation

`check` has separate executables for composition/goldens, scaling, limits and
cancellation, API contracts, PNG, SVG, PDF structure, links, nested outlines,
recognition, hostile inputs, damaged images and 300-page documents. The composition suite covers
32 fixtures and 69 pages with 63 exact gray/alpha comparisons. Two additional
PDF mark fixtures exercise overlay geometry on Nomad and Manta sheets.

For a changed composition fixture, run SNNote’s `sndump path/to/fixture` in
text mode. SHA-256 of its complete stdout gives the entry in
`tests/expected-dumps.sha256`. Each successful `raster_status` record provides
the page’s `gray` and `alpha` values for `tests/goldens.tsv`. Pages that require
an external image decoder have dashes in those columns.

The independent PNG test decoder uses zlib. Test builds require poppler-glib;
suites using libpng, Python, pdftotext or pdftoppm are explicitly skipped when
their optional tools are absent. The PDF object checks (`pdf_text`,
`font_substitution`) read objects through the pypdf Python module, because
Cairo's PDF 1.7 output keeps most objects in compressed object streams; they
are skipped with a message when pypdf is missing, and CI installs it on every
platform. `SNRENDER_TEST_PYTHONPATH` adds a module directory for them.
Tests compare
recognition-on/off pixels for every fixture page and synthetic text cases.

`SNRENDER_BUILD_FUZZERS=ON` instruments both the renderer and source-built
reader. A hash of each input selects one of the ten render entry points, a page
or page range, and option variations (scale, recognition, strict images,
background, standard or tight limits, cancellation, failing output), so one
execution composes at most four pages. An input that parses as a `.mark` also
runs one of six overlay operations over a fixed source PDF. A second target,
`fuzz_pdf`, mutates the source PDF itself through export, range export and
previews of its first two pages, starting from `tests/data/pdf-seeds`, whose
annotations exercise the preview's appearance rewrite. A Clang installation
with the libFuzzer runtime is required. The nightly runs each target for half
an hour in its own job. The notebook corpus is the fixtures of at most 256 KiB
plus small notebooks written by the `fuzz_seeds` target; the `fuzz_replay_*`
and `fuzz_pdf_replay_*` tests replay every fixture and seed through the targets
without libFuzzer. `SNRENDER_REQUIRE_PDF_TESTS=ON` makes a missing pypdf or Poppler
tool a configuration error instead of a skipped test. CI covers macOS,
Linux and Windows, shared/static builds, sanitizers, exports and consumers.
CI action tags follow rmrender-lib's workflow convention. Release compilation
uses GCC 15, matching that project's C23 release container.

## Bindings

Sources are ordinary C23 with no generated implementation or asset code. Vendor
`src/`, `include/`, and exactly one backend directory; compile the Cairo backend
with its platform dependencies. Private cross-file names also use `snr_`, so a
binding can rename both public and private symbols. The caller owns the reader's
notebook for the duration of each call.

## See also

[SNNote](https://github.com/altessa-s/snnote-lib) provides the parser, model and
format description.

## Acknowledgements

Brandon Philips’s supernote-typescript project provides 27 Apache-2.0 test fixtures.
[Fixture origins, filenames and licence](tests/fixtures/supernote-typescript/README.md).

## License

MIT-style; see [LICENSE](LICENSE) and [COPYRIGHT](COPYRIGHT).

### Trademark

Supernote is a trademark of Ratta Software. This project is not affiliated with, endorsed by or sponsored by Ratta Software.

## Recognition text

Recognition text removes Unicode Cc controls, surrogate code points,
noncharacters, and these search-breaking format characters: U+00AD (soft
hyphen), U+200B..U+200D (zero-width space/non-joiner/joiner), U+2060 (word
joiner), and U+FEFF (BOM/zero-width no-break space). Other Unicode Cf characters
are retained conservatively, including bidi controls, Arabic signs, interlinear
annotations, Egyptian controls and tags; variation selectors (Mn) are retained.
This changes only PDF text, never the notebook or recognition model. An empty
result is omitted. Malformed UTF-8 is omitted as a word.
Cairo uses an internally generated zero-area TrueType face through FreeType.
It requires no installed font. Normal words use scalar glyphs and multi-scalar Latin clusters; words exceeding
32,768 bytes use bounded 32-byte UTF-8 chunks to accommodate extractor limits.
The arena-backed AVL tree keys only text, and a document permits at most 65,533
distinct scalar/chunk entries; excess returns `SNR_ERR_LIMIT` with an explicit
recognition-font diagnostic. Box dimensions are applied through the text matrix.
Recognition reserves 256 work units per input byte, including worst-case new
glyph storage, in both the page and cumulative document budgets. Actual spacing
neighbour visits and glyph-map comparisons are charged in addition. Keys compare
by length, then bytes. AVL balance bounds searches to at most 22 comparisons
at the entry ceiling, independently of insertion order.

Arabic and Hebrew scalar runs retain logical text with right-to-left positions.
Poppler returns logical words, but pypdf still reverses these runs. This is a
known pypdf extraction limitation. Mixed-direction text is not fully laid out.
Very long words require `pdftotext -raw` to avoid Poppler's duplicate-text
suppression; the default geometric extraction can omit repeated characters.
For recognition with explicit unboxed space/newline words, both backends consume
line boundaries in model order. Each recognized line prefers a shared baseline,
and the element uses a common height based on clipped word boxes. An isolated
tallest box more than 1.4 times the next tallest is excluded before computing
the mean, so a lone tall outlier cannot enlarge other lines. Two taller boxes
still support a normal height among majority punctuation or slivers. The remaining
boxes at least a quarter of their initial mean height determine the height:
their mean, capped at 1.4
times their minimum, with a floor of half their mean. Even a majority of tiny
dots or edge-clipped slivers cannot set the element's font height when taller
words pass that threshold. Shared metrics reduce line splitting from uneven handwriting
baselines, punctuation heights and font sizes. Each word retains its horizontal
placement except consecutive boxes at the same x, which share the available
width with a positive gap. For distinct x positions, Cairo reserves half an em
before the next word; Apple reserves half the line band height. Horizontal fitting may extend into
the original quarter-height search tolerance. Vertical selection bounds follow
the common line band, clamped to the page. Its centre is chosen within the
original unclipped boxes at least as tall as the physical run's median height. Smaller boxes
contribute to the existing 0.24-box-height tolerance window without constraining
that strict centre intersection; disjoint punctuation and short words can share
a band. A page-edge sliver cannot constrain a full band. Lines whose full-height
boxes are vertically disjoint use the local geometric fallback below.
When needed, a band can grow up to the run's median height to keep small-box
centres within a quarter-line margin while retaining the normal-word overlap.
If fitting the band to the page would move its centre outside that overlap,
the band shrinks instead.
A leftward move within a model line starts a new physical
band, preserving word order across wraps.
Tests check Poppler and, on macOS, PDFKit extraction order, phrase search, and
local word rectangles. Poppler may expand gaps into multiple spaces, and both
extractors can insert additional line breaks at physical wraps. Extreme
overlapping or interleaved layouts remain subject to viewer geometry heuristics.
Lines using a shared band bypass
`snr_word_spacing`: neighbouring words are separated within each element only.
Overlapping shared-band lines from different elements receive no neighbour
separation. Lines using the fallback retain its cross-element neighbour checks.

Models without explicit separators use the existing geometric fallback.
Its recognition guarantee is stored order in the stream, words never fused,
each word findable on its box for ordered nonoverlapping lines whose pitch is
strictly greater than `H/16 + 0.05W`, where H and W are the stored height and
width. The minimum band is H/4, its separator reserves another quarter of
that height, and at least 5% of the box width remains for text. For H=5 mm and
W approximately equal to pitch, this requires pitch > 0.329 mm, or fewer than
about 334 words across 110 mm. The permanent boundary test uses 320 words.
Overlapping or nested boxes and extreme height ratios are outside this guarantee.
Denser, overlapping, nested or interleaved layouts can fuse even in raw extraction.
Extreme height ratios can also suppress words in default extraction.
In this fallback, geometric reading order is the viewer's.
All recognition fixture pages on both backends are checked with exact
whitespace-delimited `pdftotext -raw` tokens, the default extraction token
multiset, and Poppler GLib whole-word search on every expected box. Expectations
use the same sanitizer and box clipping code as rendering. Crowded text uses a
centred band chosen from nearby boxes, from full height down to one quarter
height, and reserves a quarter em before nearby boxes.
Search rectangles must fit inside the emitted box expanded by one quarter of
its height (the clipped original box in the fallback, the normalized word box
for structured recognition). Every real fixture also checks that each match's
vertical centre lies within the original clipped word box, allowing a quarter
of the line height on either side for boxes below the run's median height;
normal-size words remain
strict. This check is independent of the normalized band's position.
Poppler sometimes returns the containing line's vertical
extent, not just the matched punctuation's extent. This permits font metric rounding
and uneven baselines while keeping matches local to the handwriting.
Every boxed word still ends with a real U+0020 glyph with positive advance,
including separately recognized punctuation. Model newlines select the next
positioned line; they are not passed to a shaper as control glyphs.
The 65,536-byte dense-word probe is exact in raw mode on both backends. Default
extraction is only asserted to return a nonempty, shorter ordered subsequence;
it is not complete. Apple uses explicit monospace glyph positions for these
extreme words to avoid Poppler's 50,000-tiny-glyph cutoff.
Apple emits one invisible CTLine per word. CoreText shaping can cause Arabic
and other scripts to extract as fragments; known examples include Devanagari,
Mongolian and Egyptian hieroglyphs. Hebrew is intact. Its public tagged PDF
ActualText property does not override pdftotext's content-stream extraction.
Apple rejects colour fonts by both the colour trait and sbix, COLR, SVG and CBDT
tables. Unknown future colour-font formats are not covered by the table guard.

PDF calls preflight every page before drawing and cumulatively charge the same
pixel, layer, image-dimension, annotation and recognition work as the per-page
budget. A zero document budget rejects even a one-page PDF. Options ABI 2 adds
the document ceiling; initialize options with `snr_options_default()`.

Cairo uses one generated TrueType face with symbolic Identity-H text codes and
ToUnicode mappings, including its reserved spacer glyph. Registering that glyph
without text before assigning U+0020 keeps Cairo from creating a separate
WinAnsi subset. The first word call includes two leading spacer positions
(the first initially unmapped); subsequent words need only their trailing
spacer. Poppler honours the single-space ToUnicode mapping. No Type 3 font is used.
Two coincident contour points at (500, 250) cannot paint; outward-rounded metrics
retain narrow text operations. A page-sized text form is shown through a soft
mask whose opacity is zero throughout the page. Removing either the embedded font alone
or the mask alone leaves the raster unchanged. If a consumer ignores both the
embedded font and the mask, substituted text can become visible.

## Annotated source PDFs (0.2.0)

`snrender annotation.mark annotated.pdf --source-pdf original.pdf` retains every
source page and adds the mark ink. Supply a one-based page argument for one
source page, or choose PNG output for a preview. Source PDF bytes are borrowed
by the library; the tool reads a local file and performs no network access.

The eight overlay functions use `snr_overlay_options`
with `SNR_OVERLAY_ABI`. `snr_mark_document_pdf` and its `_to_output` counterpart
accept a zero-based source page range (`SIZE_MAX` selects the remainder).
`snr_mark_page_bitmap`, `snr_mark_page_png`, and `snr_mark_page_png_to_output`
produce previews at `ceil(displayed points * render.scale)` pixels. A preview
renders its sheet at that page density (render.scale divided by the sheet fit,
never above render.scale), so the pixel limit applies to what is drawn; PDF
export renders ink at sheet pixels times render.scale.
`snr_mark_page_has_annotations` includes sparse sheets and highlight-only pages.
Start options with `snr_overlay_options_default()`.

Full-document export appends a PDF revision; original vector content, text,
links, outline, boxes and rotation stay in their original objects. Selecting
a range rewrites the selected pages, retaining document objects and disabling
links to removed destinations. This is not secure redaction. Existing digital
signatures retain their signed byte ranges in a full-document incremental export; viewers report changes after signing. A page-range export rewrites the document and drops signatures. A source signature field produces a warning and is counted in the overlay result. Encryption, certification permissions, non-unit
UserUnit, unsupported stream filters and malformed structures are refused.
The ink is a transparent grayscale image; its resolution uses the mark sheet
and rendering scale. A source page without ink passes through unchanged.

Device evidence supports contain-fit: scale the displayed CropBox after rotation
by `min(sheet_width / page_width, sheet_height / page_height)` and centre it on
both sheet axes. Document highlight rectangles are relative to the CropBox.
The evidence covers three page shapes on two devices (Nomad N6 and Manta N5),
in portrait, uncropped reading. Landscape sheets, device crop and zoom reading
modes, and EPUB are not verified. The N6 fixture's page is height-limited and centred
horizontally; the N5 fixture's page and US Letter on the 1404 by 1872 sheet are
width-limited and centred vertically. Sparse
`PAGE22` maps to source page index 21. Both highlight arrays in the C model use
one-based page numbers; screen rectangles retain device underline thickness.
The displayed CropBox and rotation determine the fitting rectangle. `strict`
rejects references beyond the source page count and paired highlight coordinates
that disagree by more than three screen pixels. Otherwise out-of-range marks
are omitted with page-numbered warnings and ink is fitted to the supplied source geometry. Paired rectangles are always checked; default mode warns with the worst residual and continues. `--strict` enables strict mode in the tool. An optional `snr_overlay_options.result` points to an ABI-versioned `snr_overlay_result`, initialized with `SNR_OVERLAY_RESULT_ABI` and `sizeof`. It reports annotated pages drawn, sheets and highlights dropped, worst residual in sheet pixels, pages checked by pairs, signature field count, and flags for all annotations dropped or no pairs agreeing (set only when pairs were checked). Results reset on entry and retain partial diagnostics on failure.
A mark without paired rectangles (handwriting only, or highlights without
document rectangles) carries no evidence for or against its source: nothing is
logged about it, `pages_checked_by_pairs` is zero, and strict mode accepts it,
since strictness cannot prove that such a mark belongs to the PDF. The model does
not supply a source page count, page size or source identity that can establish
that the chosen PDF is the original. Crop/zoom modes are not decoded by the
reader; the default fitting rule is the only supported placement rule.

The reader cannot distinguish PDF-backed and EPUB-backed marks. Callers must
identify the source using `source_kind`; defaults and the CLI's `--source-pdf`
assert PDF. The tool accepts `--source-kind pdf|epub` and
`--maximum-source-bytes N`, names a missing, empty, oversized or non-regular
source file before rendering, and prints warnings on stderr. Previews draw the
normal appearance of each visible source annotation (form fields, stamps,
existing highlights) on both backends; CoreGraphics does not draw them itself,
so the Apple backend first flattens them into an in-memory incremental update
of the page. PDFium draws annotations itself and AcroForm fields through a form
environment without callbacks (the PDFium build has no JavaScript); widgets
outside the AcroForm field tree, which neither path draws, are flattened the
same way first. `SNR_DOCUMENT_EPUB` returns `SNR_ERR_UNSUPPORTED`: reproducing device
reflow requires layout information that the reader does not expose. Existing
sheet-only rendering remains available for either kind of mark.

Limits apply to the entire source page tree, even with a selected output range.
`overlay.limits.maximum_source_bytes` defaults to 512 MiB, suitable for large
books independently of the encoded output limit. Parser scratch is bounded by
16 times the source size, with a 1 MiB minimum allowance. Image and compression
storage is bounded by the pixel limit. Encoded output alone uses
`maximum_output_bytes` and reports "output byte limit exceeded"; page count uses
`maximum_pages`. A missing xref Flate end marker is tolerated only for complete
xref rows of the exact length required by /W and /Index. Object and startxref
offsets may be corrected within 32 bytes. No full xref rebuild is attempted;
anything requiring one remains refused. Source boxes are
limited to 14,400 points per side. Pixel allocations, annotation scans, parser
operations and document work are charged before or during processing. Inflated
xref/object-stream bytes consume work and the decoded-byte budget even when
decoding fails; failed object streams are not retried. Native
PDF rasterization occurs within the platform reader; cancellation is checked
before and after that synchronous step and during annotation rows.

Apple uses CoreGraphics without PDFium. Cairo overlays require the optional
`SNRENDER_ENABLE_PDFIUM=ON` and an explicit `PDFium_ROOT`. With it off, all new
rendering calls report `SNR_ERR_UNSUPPORTED`; the annotation query remains
available. No download happens at configuration time. The checksum-pinned
`scripts/fetch-pdfium.py` fetches the SDK used in CI. PDFium has a BSD-style
license and bundled third-party notices; it is a separate dynamically linked
dependency, not installed by snrender. Distributors must ship the SDK's license
and third-party notices with any redistributed PDFium binary. The object editor
is adapted from the MIT-licensed rmrender-lib source and uses zlib.

`SNRENDER_REQUIRE_OVERLAY_TESTS=ON` makes missing overlay test dependencies a
configuration error. The tests use synthetic sources and Poppler/pypdf to check
geometry and preservation. Owner-authorized mark samples are distributed; the source books are not. Real device crop/zoom and EPUB layout remain unverified.

`snr_pdf_page_text` extracts a highlight's rectangle union from source PDF
bytes without a notebook. Pass `highlight.document_rects` and
`highlight.document_rect_count`, and subtract one from its stored page number:

```c
snr_buffer text = {0};
snr_error error = {0};
snr_status status = snr_pdf_page_text(pdf, pdf_size, source_page_index,
                                      highlight.document_rects,
                                      highlight.document_rect_count,
                                      NULL, &text, &error);
if (status == SNR_OK) {
  /* text.bytes is NUL-terminated UTF-8; text.size excludes the NUL. */
  consume_text((const char*)text.bytes, text.size);
  snr_buffer_free(&text);
}
```

Rectangles use PDF points from the top-left of the displayed rotated CropBox,
intersected with MediaBox, matching the overlay convention. A character is
included once if its box centre falls inside any rectangle (left/top inclusive,
right/bottom exclusive). Rectangle order has no effect. Text follows the
engine's natural reading order; complex columns and unusual encodings remain
engine-dependent. Apple uses PDFKit single-character range selections with ARC
and the system PDFKit framework; Cairo uses PDFium character boxes and the same
process lock as previews. CoreGraphics alone provides no text selection API.
Cairo without PDFium reports `SNR_ERR_UNSUPPORTED`.

Extraction collapses whitespace to spaces or LF, trims it at both ends, and
drops the recognition layer's invisible characters, controls and invalid
scalars. A hard hyphen at a wrap is kept and the wrap removed; a soft hyphen
and its wrap are removed when the engine exposes it. PDFium designates some
line-end hyphens with an ambiguous marker; these are kept as '-' because the
public API does not expose the original hard/soft distinction. PDFKit can
remove soft hyphens before extraction, leaving an ordinary LF. The same PDF
can yield slightly different strings per platform for soft-hyphen wraps,
ligatures, invisible-character whitespace, column order and malformed mappings. No OCR or dictionary-based word
joining is performed.

Initialize custom options with `snr_pdf_text_options_default`. Defaults permit
512 MiB of source bytes, 10,000 source pages, 4,096 rectangles and 1,048,576
engine characters per page. Output is bounded by `limits.maximum_output_bytes`,
including the trailing NUL. Parsing and character/rectangle visits share the
smaller of the per-page and document work budgets. Cancellation is polled during
parsing and scanning, outside the PDFium/host lock. PDFium copies bounded
scalar/box snapshots under the lock and then hit-tests them after unlock.
Native PDF engine loading is synchronous and cannot be
interrupted or fully metered. Empty unions and selections succeed with an empty
NUL-terminated string. Existing options and ABI prefixes are unchanged.

The fetched SDK also has an MIT-licensed packaging layer: retain its top-level
`LICENSE` and complete `licenses/` directory, including `licenses/pdfium.txt`.
These dependency licenses are separate from snrender's MIT license.

Cairo preview and text extraction functions must not run concurrently with any other PDFium use in
the process, including rmrender-lib's. The private mutex serializes only
snrender-lib calls. A host that uses PDFium elsewhere must install its shared
lock/unlock callbacks with `snr_pdfium_set_lock` once, before any preview or text extraction call,
and use that same lock for every external PDFium operation. Installation after
PDFium use begins or a second installation returns an error. Full-document
and range PDF exports do not use PDFium and are unaffected by this constraint.

The overlay image carries its own gray ink on white paper, with a binary soft
mask and Multiply blending, so ignoring the mask does not produce a black page.
Added drawing is marked as `/Artifact`; the source StructTreeRoot is untouched.

Shared-library consumers do not need the PDFium SDK at CMake configure time.
They must supply the PDFium binary and its search paths at link and runtime.
On Linux, when PDFium is outside the system library paths, GNU ld needs
`-Wl,-rpath-link,/path/to/pdfium/lib` when linking the consumer against
libsnrender. For a CMake executable, pass
`-DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,/path/to/pdfium/lib"` at configuration
time, or add `target_link_options(app PRIVATE
"LINKER:-rpath-link,/path/to/pdfium/lib")`. This link-time flag does not set a
runtime path: Linux also needs `libpdfium.so` on the loader path, for example
with `LD_LIBRARY_PATH=/path/to/pdfium/lib`. Windows needs `pdfium.dll` beside
the executable or on `PATH`, along with snrender's DLL for shared installs.
macOS Cairo builds need `libpdfium.dylib` on `DYLD_LIBRARY_PATH`. These explicit
paths keep snrender's build machine SDK location out of its installed library
and CMake package and allow PDFium to be deployed separately. Static consumers
still need `PDFium_ROOT` at configure time when previews were enabled, plus the
same runtime setup.

Windows builds stage CMake's known runtime DLL dependencies, including PDFium
and shared snrender, beside each built CLI and test executable. This also
covers executables spawned by the Python tests. Toolkit DLLs supplied by
MSYS2 remain available through its `PATH`. Installation does not bundle PDFium;
installed consumers and packaged tools use the deployment paths above.

## Release archives

Version tags publish `snrender <version>` with
`snrender-<version>-linux-x86_64.tar.gz`,
`snrender-<version>-macos-arm64.tar.gz`, and
`snrender-<version>-windows-x86_64.zip`. Each contains the installed static
library, public headers, CMake package and pkg-config files, command-line tool,
LICENSE, COPYRIGHT and README.md. Builds use Release, WERROR, tools enabled,
`BUILD_SHARED_LIBS=OFF`, Apple on macOS and Cairo on Linux/Windows, with PDFium
previews enabled on Cairo. Dependencies are not bundled. Consumers supply
snnote-lib at pinned commit `2cab83674ba017abe0e0e42eb5b6e5558a27155f`,
snspd-lib at pinned commit `0ac7ba43dcf4b798d9ac5c845dc2eee2b7d45b3b`, zlib,
and, for Cairo builds, Cairo, FreeType and GdkPixbuf. Cairo previews additionally
need PDFium at build and runtime, including its license notices when redistributed.
The Linux and Windows command-line tools link the PDFium shared library
directly, so they do not start without it, even to render a `.note` with no
source PDF: put libpdfium on `LD_LIBRARY_PATH` (Linux) or beside the tool or on
`PATH` (Windows). The macOS tool uses CoreGraphics/PDFKit and needs no PDFium.
The GCC 15 build and tests must pass before publishing.

## Atelier drawings (`.spd`)

Enable with `-DSNRENDER_ENABLE_SPD=ON`. Like PDFium, this is an explicit
opt-in; the default OFF never searches for or fetches SNSpd. Resolution uses
`SNRENDER_SPD_SOURCE`, installed `find_package(SNSpd 1.0)`, sibling
`../snspd-lib`, then FetchContent pinned to `SNR_SPD_COMMIT`. CMake warns if
a source checkout differs from the pin or an installed package cannot be
verified against it. The eight new
functions remain exported in disabled builds and return `SNR_ERR_UNSUPPORTED`.
Static installed consumers need the SNSpd package; pkg-config records it in
`Requires.private`. No new image decoder is required.

```c
#include <snspd/snspd.h>
#include <snrender/snrender.h>

/* Open input with spd_open and the reader's own parsing/memory/work limits. */
snr_spd_options options = snr_spd_options_default();
options.render.scale = 0.5;
options.render.strict_images = true;
snr_buffer png = {0};
snr_status status = snr_spd_png(drawing, &options, &png, &error);
/* snr_spd_pdf / snr_spd_svg, or their _to_output variants, use the same options.
 * snr_spd_bitmap returns premultiplied RGBA8. Free results with existing frees. */
snr_buffer_free(&png);
```

A parsed immutable `spd_drawing` is borrowed for each call. Options have an
independent `SNR_SPD_ABI` and `struct_size`; start with the default initializer.
Outputs change only on success; errors change only on failure. Callbacks and
output failure rules match the notebook API. Parsing is the caller's separate
operation; rendering cancellation cannot interrupt `spd_open`.

SNSpd's corrected column-major tile coordinates are used directly:
`tid = tx*4096 + ty`, with 64-bit `tx` horizontal and 32-bit `ty` vertical.
The default page is `SNR_SPD_PAINTED_BOUNDS`: the union of the stored tiles,
moved to the top left. It is the framing Atelier's embedded thumbnail uses
(correlation 0.98 against the thumbnail of the one real export). The
`SNR_SPD_CANVAS` page is `surface.width × surface.height`, centred
on the saved viewport point. A tile's canvas origin is
`((tx*128-vp.x)*vp.scale + surface.width/2,
(ty*128-vp.y)*vp.scale + surface.height/2)`.
The viewport scale applies to the tile offsets and tile size; the canvas size
stays fixed. Fractional viewport points are supported; missing coordinates and
scale mean zero and one. Tile pixels use nearest-neighbour sampling at page
pixel centres, and tiles outside the canvas are clipped.

**Centre is inferred from one real export, not established format semantics.**
With the corrected axes, all 143 stored tiles in that sample fit inside a
1920×2560 canvas centred on its saved viewport point; only 29 overlap a canvas
anchored at that point's top-left. This makes centre more plausible for that
sample. Transparent/off-page tiles and independent thumbnail fitting remain
possible; additional independent exports are needed to confirm page anchoring
and viewport scale semantics. The reader's geometry helpers subtract the
viewport point without applying scale or a half-page offset. See SNSpd's
[format evidence](https://github.com/altessa-s/snspd-lib/blob/v1.0.0/docs/format.md#tile-grid).

`SNR_SPD_PAINTED_BOUNDS` instead exports the union of selected stored tile
rectangles, including transparent/damaged tiles, translating it to the top
left; an empty union produces a 1×1 page. Bounds include viewport scale and
allow missing canvas dimensions. Dimensions or coordinates outside supported
precision return `SNR_ERR_LIMIT`.

Layers composite **in file order**, source-over on white by default. PNG gray
and alpha are decoded through ImageIO on Apple and the existing GdkPixbuf PNG
path on Cairo. Alpha is coverage; gray is ink luminance. Use
`render.has_background=true` and `render.background=(snr_color){1,1,1,0}` for
transparency. `include_reference_layer=false` omits id 9999. It defaults true:
the available layer fields 3/4 have no established hidden/reference semantics,
and the sample's Reference Layer has neither field and no tiles. No unknown
visibility bits are guessed.

PNG output dimensions are `ceil(page dimensions * render.scale)`. Nonzero
`dpi` overrides that scale with `dpi/canvas.ppi`; missing ppi defaults to 300.
Downscaling box-averages premultiplied channels, enlargement uses nearest
neighbour. SVG retains page pixel dimensions and embeds the PNG. PDF is one
raster page with dimensions `page pixels * 72 / ppi` points, independent of
output scale/DPI. PDF page sides are clamped to at least 3 points (including
empty drawings) and limited to 14,400 points. Recognition,
notebook background inclusion, title and progress fields are unused.

Planning checks `maximum_tiles` (262,144, counting all layers including
excluded reference tiles), source/output pixel ceilings from `render.limits`,
and `maximum_decoded_bytes` (256 MiB, cumulative dimension-valid RGBA tile
bytes plus both page buffers) before page allocation or platform decoding.
Encoded output bytes are limited as the encoder writes. Opaque platform
codec scratch is outside decoded-byte accounting. All selected tiles are
checked even outside the canvas. Only 128×128, 8-bit gray+alpha PNG tiles are
accepted, with non-interlaced or Adam7 data. The chunk whitelist is IHDR,
consecutive IDATs, then IEND; all ancillary chunks, including text, timestamps,
colour profiles and gamma, are rejected before platform decoding. Other pixel
formats are excluded because Atelier writes gray+alpha and platform handling
of palettes and colour metadata can differ. Bytes after IEND are ignored by
both backends through the existing shared framing contract.
`render.strict_images=false` logs and skips bad PNGs, including undersized and
oversized tiles; true returns `SNR_ERR_BACKEND` for malformed/unsupported tiles
or `SNR_ERR_LIMIT` for oversized dimensions. A bounded zlib/scanline check
rejects invalid compressed data even when ImageIO reports a complete image;
pixels are still decoded by the platform codec. Resource limits, allocation
failures and cancellation always abort.

Planning reserves work for layers, all listed tile records, selected encoded
bytes, and two further encoded passes per dimension-valid tile (validator CRC,
backend CRC and platform input in total). Each dimension-valid tile also
reserves 16,384 decode units, the maximum 33,008 GA8 scanline bytes and a
worst-case blend rectangle: `min(source width, ceil(128*vp.scale))` times
`min(source height, ceil(128*vp.scale))`. It also reserves four source-page
pixel passes and output pixels. These conservative bounds apply before page
allocation; damaged or clipped tiles may use less work. Both `maximum_work`
and `maximum_document_work` cap this single-page work. The budget meters
library operations, not time inside opaque platform codecs.
Cancellation is checked before planning, per layer/tile/page row and during
resampling/base64 conversion, and before/after encoding; opaque encoding and
a single bounded tile decode run synchronously.

`tests/generate_spd.py` creates only synthetic SQLite/gray-alpha PNG fixtures;
no reader build-tree assets or private drawings enter the repository. Tests
check known pixels, clipping, fractional viewport/scale, 64-bit tile columns,
layer order, reference exclusion, backgrounds, malformed/huge tiles, limits,
cancellation, output failures, PDF points/orientation and SVG embedding.
SQLite seeds also enter the existing render fuzz harness and bounded replay.

`test_spd_hostile` covers validator/geometry boundaries, all five PNG filters,
Adam7, split IDATs, cancellation, sink failures and work reservations. It
asserts full RGBA images against hand-computed expectations and writes
`spd-fixtures/parity.rgba`. After running it in both backend builds, compare
those pixels locally with:

```sh
python3 tests/check_spd_parity.py build/apple/spd-fixtures build/cairo/spd-fixtures
```

Source PDF geometry inspection parses the page tree once per call. Signature
warnings count each reachable field object once through the catalog's AcroForm
field tree. This diagnostic is best effort: missing or damaged fields are skipped,
cycles are visited once, and traversal stops at 64 levels. Unreferenced damaged
objects also do not prevent incremental export or preview. Cancellation and work
limits still apply to signature inspection.
Preview flattening skips annotation appearances whose placement exceeds the
coordinate limit, while retaining other usable appearances.
