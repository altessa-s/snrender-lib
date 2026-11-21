# Windows

Use MSYS2 UCRT64 with `mingw-w64-ucrt-x86_64-gcc`, `cmake`, `ninja`, `pkgconf`,
`cairo` (>= 1.18), `freetype`, `zlib` and `gdk-pixbuf2` packages using that same prefix. Test builds
also require the prefixed `libpng`, `poppler`, `python` and `python-pip`
packages, and pypdf installed with that pip. The C23 API
requires a recent GCC. Microsoft Visual C is not a supported compiler.

```sh
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DSNRENDER_WERROR=ON
cmake --build build/windows --target all check
cmake --install build/windows --prefix "$PWD/build/install"
```

For DLLs add `-DBUILD_SHARED_LIBS=ON`. CMake propagates `SNRENDER_SHARED` to
consumers and uses `SNRENDER_BUILDING` inside the DLL. Distribute the backend's
runtime DLLs with the application, and make GdkPixbuf loaders and fonts
available. The DLL basename carries the ABI major; the import library uses
the unversioned target name.

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
Put the SDK's `bin` directory containing `pdfium.dll` on `PATH` at runtime.
The fetched Windows import library has not been linked locally under MSYS2
UCRT64 GCC. The Windows CI job is the required proof: PDFium discovery, linking
and overlay tests are required and must fail if the SDK cannot be linked.
