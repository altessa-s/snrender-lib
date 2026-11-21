# Linux

Use a compiler with complete C23 support (GCC 15 or a recent Clang). Debian
build dependencies (Cairo >= 1.18) are `cmake ninja-build pkg-config libcairo2-dev libfreetype-dev
libgdk-pixbuf-2.0-dev zlib1g-dev`. Test builds additionally require
`libpng-dev libpoppler-glib-dev poppler-utils python3 python3-pypdf`. Poppler
GLib is mandatory when Cairo tests are enabled; missing libpng, Python, pypdf or
Poppler command-line tools produce explicit suite-skip messages. Clang sanitizer and libFuzzer builds also
need the matching `libclang-rt` development package.

```sh
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DSNRENDER_WERROR=ON
cmake --build build/linux --target all check
cmake --install build/linux --prefix "$PWD/build/install"
```

Consumers use `find_package(SNRender CONFIG REQUIRED)` and link
`SNRender::snrender`. Shared-library runtime search paths must include the
installation's library directory if it is outside the system loader's paths.

The Cairo backend also requires cairo-ft and FreeType (freetype2 in pkg-config).

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
Put the directory containing `libpdfium.so` on `LD_LIBRARY_PATH` at runtime.
