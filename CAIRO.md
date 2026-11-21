# Cairo backend

The backend needs Cairo >= 1.18 with cairo-ft, FreeType, GdkPixbuf and zlib, found with
pkg-config. It runs on Linux, macOS and Windows. Optional PDFium support enables source PDF previews and text extraction.
The Linux and Windows release builds include this support. Recognition uses an internally generated outline-free TrueType font and needs no installed font.

On macOS, with dependencies already installed:

```sh
cmake -S . -B build/cairo -DSNRENDER_BUILD_CAIRO=ON -DSNRENDER_WERROR=ON
cmake --build build/cairo --target all check
```

Generated PDFs use Cairo's default version (PDF 1.7 with Cairo 1.18), whose
object and cross-reference streams compress most objects, and stream directly,
including with recognition. Restricting Cairo 1.18 to PDF 1.4 made it free its
output stream twice when finishing after an internal failure. The structure
tests read objects through pypdf.
Cairo's text-cluster API maps scalar glyphs into one document-scoped FreeType
face. Words exceeding 32,768 bytes use chunks of at most 32 UTF-8 bytes. This retains extraction without painting any recognised glyph,
including emoji, and requires no PDF post-processing. Fractional link quads
are retained; Cairo emits integer enclosing annotation bounding boxes.

Cairo and FreeType may keep process-wide caches. Each rendering call has
its own Cairo objects. Raster composition and SVG determinism do not depend
on font availability. Cairo limits each raster side to 32,767 pixels.

The links suite asserts annotation counts, destinations, exact quads and mapped
positions on differently sized pages; it fails if Cairo changes its finish-time
coordinate behavior. CI prints the installed Cairo version and runs this suite
explicitly with `--no-tests=error`.

FreeType is the `freetype2` production dependency in pkg-config.

## Source PDF overlays

Enable with `-DSNRENDER_ENABLE_PDFIUM=ON -DPDFium_ROOT=/path/to/pdfium`.
Without the option, source-overlay rendering returns `SNR_ERR_UNSUPPORTED` and
existing notebook and transparent-mark rendering still works. The preservation
writer edits PDF objects directly; it never redraws source pages through Cairo.
PDFium renders source page previews and provides character boxes for text extraction. The Apple backend uses
CoreGraphics instead and needs no PDFium SDK.

Fetch a checksum-verified SDK explicitly with `python3 scripts/fetch-pdfium.py
--platform PLATFORM --output build/pdfium` (one command line). PLATFORM is
`linux-x64`, `linux-arm64`, `win-x64`, `win-arm64`, `mac-arm64` or `mac-x64`.
Windows needs the DLL on PATH as well as its import library at build time.
PDFium is dynamically linked and not installed or bundled by snrender. Preserve
its BSD-style license and all SDK third-party notices when distributing its
binary. CMake performs no network download. CI requires both PDF tests and
overlay tests with `SNRENDER_REQUIRE_PDF_TESTS=ON` and
`SNRENDER_REQUIRE_OVERLAY_TESTS=ON`.

The fetched SDK also has an MIT-licensed packaging layer: retain its top-level
`LICENSE` and complete `licenses/` directory, including `licenses/pdfium.txt`.
These dependency licenses are separate from snrender's MIT license.

Cairo overlay previews must not run concurrently with any other PDFium use in
the process, including rmrender-lib's. snrender-lib's private mutex cannot
serialize another library. Install a shared lock/unlock pair once with
`snr_pdfium_set_lock` before overlay calls, and hold that same lock around all
external PDFium calls. Late or repeated installation fails. PDF export uses
the object editor, not PDFium, and is unaffected by this concurrency constraint.
Shared consumers do not need PDFium at CMake configure time, but must supply
its runtime library. Static consumers also need `PDFium_ROOT` at configure time.
On Linux set `LD_LIBRARY_PATH` to the PDFium library directory; on Windows
put `pdfium.dll` on `PATH`; macOS Cairo builds use `DYLD_LIBRARY_PATH`.
Sheet placement uses contain-fit of the displayed rotated CropBox, centred on
both axes. The source-book evidence covers portrait uncropped reading only.
