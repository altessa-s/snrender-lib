/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef SNRENDER_H
#define SNRENDER_H
#include <snnote/snnote.h>

#include "export.h"
#define SNRENDER_VERSION_MAJOR 1
#define SNRENDER_VERSION_MINOR 0
#define SNRENDER_VERSION_PATCH 0
#ifdef __cplusplus
extern "C" {
#endif

typedef enum snr_status : uint32_t {
  SNR_OK,                   /* The result is complete. */
  SNR_ERR_INVALID_ARGUMENT, /* Invalid pointer, range, dimensions or ABI. */
  SNR_ERR_OUT_OF_MEMORY,    /* Allocation failed. */
  SNR_ERR_LIMIT,            /* A configured or platform ceiling was exceeded. */
  SNR_ERR_UNSUPPORTED,      /* Unsupported layer encoding. */
  SNR_ERR_BACKEND,          /* Platform encoding or image decoding failed. */
  SNR_ERR_CANCELLED         /* Cancellation was requested. */
} snr_status;
typedef struct {
  snr_status code; /* Failing status. */
  size_t index; /* Zero-based page (source page for overlays); zero for global
                   errors. */
  char message[192]; /* NUL-terminated diagnostic without notebook content. */
} snr_error;
typedef struct {
  double red, green, blue, alpha; /* Straight sRGB/coverage, finite in [0,1]. */
} snr_color;
typedef struct {
  uint8_t* bytes; /* Owned file bytes; release with snr_buffer_free. */
  size_t size;    /* Byte count; no trailing NUL is promised. */
} snr_buffer;
/* Premultiplied RGBA8, tightly packed, first row at the top. */
typedef struct {
  uint8_t* pixels; /* Owned channels; release with snr_bitmap_free. */
  size_t width, height, stride; /* Pixel dimensions and bytes per row. */
} snr_bitmap;
/* Straight gray and coverage planes, first row at the top. */
typedef struct {
  uint8_t *gray, *alpha; /* Owned planes; transparent gray is 255. */
  size_t width,
      height; /* Pixel dimensions; each plane has width*height bytes. */
} snr_raster;
/* Limits are checked before allocation. Defaults: 16,777,216 output and
 * decoded pixels, 10,000 document pages, 100,000,000,000 document and
 * 1,000,000,000 per-page pixel/layer work units, 512 MiB output bytes, scale
 * 1/64 through 8. Recognition words are limited to 65,536 UTF-8 bytes. Zero
 * limits allow no work. The reader's own RLE limit remains an additional
 * ceiling. */
typedef struct {
  size_t maximum_pixels; /* Output pixels per page; default 16,777,216. */
  size_t maximum_decoded_pixels; /* Source sheet/image pixels; same default. */
  size_t maximum_pages;          /* Pages per PDF call; default 10,000. */
  size_t maximum_work; /* Per-page work; default 1e9. Charges source/output
                        * pixels, layers, encoded header dimensions,
                        * annotation scans and 256 units per recognition byte
                        * (including worst-case new glyph storage), plus actual
                        * spacing neighbour and glyph-map comparisons. */
  uint64_t maximum_document_work; /* Cumulative PDF work; default 1e11.
                                   * All pages are checked before drawing. */
  size_t maximum_output_bytes;    /* Encoded bytes per call; default 512 MiB. */
  double minimum_scale;           /* Inclusive lower bound; default 1/64. */
  double maximum_scale;           /* Inclusive upper bound; default 8. */
} snr_limits;
/* Return true to cancel. Contexts are borrowed until the call returns. */
typedef bool (*snr_cancel_fn)(void* context);
/* PDF progress: zero before rendering, then one call per completed page. */
typedef void (*snr_progress_fn)(void* context, size_t completed, size_t total);
typedef enum snr_log_level : uint32_t {
  SNR_LOG_DEBUG, /* An optional annotation or recognition run was omitted. */
  SNR_LOG_WARNING = 2, /* Annotation placement may be unreliable. */
  SNR_LOG_ERROR = 1    /* A damaged image layer was omitted. */
} snr_log_level;
/* Message storage is borrowed and valid only during the callback. */
typedef void (*snr_log_sink)(snr_log_level level, const char* message,
                             void* context);
#define SNR_OPTIONS_ABI 2u
/* Start with snr_options_default. Null selects defaults. Larger structures
 * are accepted; this ABI's complete prefix is required. Background defaults
 * to white for notebooks and transparent for marks. An explicit background
 * overrides that choice. Scale affects images, not physical PDF sheet size.
 * Callbacks run synchronously on the calling thread and must return normally.
 * Cancellation is checked before pages, layers and rows. */
typedef struct {
  uint32_t abi_version; /* Matching SNR_OPTIONS_ABI or SNR_OUTPUT_ABI. */
  size_t struct_size; /* sizeof this complete structure, or a larger prefix. */
  double scale; /* Default 1; output sides are ceil(sheet pixels * scale). */
  bool
      include_background; /* Include notebook background layer; default true. */
  bool has_background;    /* Use the explicit canvas colour; default false. */
  bool recognition;       /* Searchable nonpainting PDF text; default true.
                           * Stored order in the stream, words never fused, each
                           * word findable on its box for ordered nonoverlapping lines
                           * with pitch > H/16 + 0.05W (height H, width W).
                           * Overlapping or nested boxes and extreme height ratios
                           * are outside this guarantee. Denser lines may fuse.
                           * Geometric order is the viewer's. Fixture search
                           * tolerance is 1/4 box height.
                           * Very dense long words require raw extraction; default
                           * geometric extraction may suppress repeated characters.
                           * Cairo permits 65,533 distinct scalar/chunk entries per
                           * document; exceeding this returns SNR_ERR_LIMIT with a
                           * diagnostic identifying the recognition font ceiling.
                           * Other formats remain image-only. No OCR is performed.
                           * Invalid UTF-8 and invalid/outside boxes are omitted.
                           * Cc controls, surrogates and noncharacters are removed.
                           * Apple uses one CTLine per word; shaped Arabic and Indic
                           * text may extract as fragments. Hebrew remains intact.
                           * Cairo uses visual-order RTL clusters: pdftotext returns
                           * logical Arabic/Hebrew, but pypdf exposes visual order.
                           * Full Unicode bidi layout and mixed-script reading order
                           * are not guaranteed by either backend.
                           * Apple drops colour-font words; Cairo maps text clusters
                           * to a blank glyph, so colour glyphs never reach the page.
                           */
  snr_color
      background;    /* Explicit canvas colour when has_background is true. */
  snr_limits limits; /* Resource ceilings from snr_limits_default. */
  snr_cancel_fn cancel;     /* Optional synchronous cancellation callback. */
  void* cancel_context;     /* Borrowed context; default null. */
  snr_progress_fn progress; /* Optional synchronous PDF progress callback. */
  void* progress_context;   /* Borrowed context; default null. */
  const char* title;        /* Borrowed NUL-terminated UTF-8 PDF metadata title.
                             * Null selects "Supernote notebook". Notebook titles
                             * are bitmap regions: outline labels are generated as
                             * "Page N — title K" and nested by their level. */
  /* Default false: log and skip damaged image layers. True aborts on decode
   * failure. JPEG requires EOI and excludes TEM; incomplete camera files are
   * skipped by default and rejected in strict mode. Limits, cancellation and
   * allocation failures always abort. */
  bool strict_images;
} snr_options;
#define SNR_OUTPUT_ABI 1u
/* Bytes arrive in order, never with size zero. The first failure of a call is
 * its result. A non-OK callback result that is the first failure is returned
 * unchanged, whatever its value, with the message "output callback failed";
 * the callback is never called again in that call. A cancellation, limit or
 * other error found before the callback refused stays the result: the bytes
 * that close a PDF may still be offered after such an error, and a refusal of
 * them does not replace it. Output that outgrows maximum_output_bytes while
 * it is written reports "output byte limit exceeded"; a page whose stored
 * layers alone exceed that limit is refused earlier, by the plan, with its
 * own message. A failed output buffer reports "output allocation failed".
 * Discard partial output on failure. The callback must not reenter the
 * library. */
typedef struct {
  uint32_t abi_version; /* Matching SNR_OPTIONS_ABI or SNR_OUTPUT_ABI. */
  size_t struct_size; /* sizeof this complete structure, or a larger prefix. */
  snr_status (*write)(void* context, const uint8_t* bytes, size_t size);
  void* context; /* Borrowed write callback context. */
} snr_output;
/* Coordinates: notebook sheets and link boxes use device pixels, origin at
 * top left, x right and y down. Recognition boxes use millimetres. PDF size
 * is sheet pixels * 72 / DPI points, independent of scale. A5X uses 226 DPI;
 * A6X, Nomad, Manta and unknown devices use 300 DPI. PAGEMACHINETYPE overrides
 * the notebook device for moved pages. SVG retains the stored pixel viewBox.
 * Downscaling uses deterministic integer box averaging in premultiplied space;
 * scale 1 is exact and enlargement uses nearest neighbour.
 * Cairo output sides cannot exceed 32,767 pixels even with raised limits.
 * Only forward links within the exported notebook and http, https, mailto
 * URIs are emitted, with case-insensitive scheme matching. External files,
 * backlinks and targets outside the range are omitted. Boxes are clipped to
 * the sheet; empty, non-finite and outside boxes are omitted with a log line.
 *
 * Each call owns its contexts. Immutable notebooks can be shared between
 * concurrent calls. Outputs change only on success; optional errors change only
 * on failure. Owning results must be freed before reuse and must not be copied.
 * Free functions accept null and zero the supplied value. */
/* Return static version text, which must not be freed. */
SNR_API const char* snr_version(void);
/* Return static status text, with a fallback for unknown codes. */
SNR_API const char* snr_status_string(snr_status status);
/* Return independent values with the defaults documented above. */
SNR_API snr_limits snr_limits_default(void);
SNR_API snr_options snr_options_default(void);
SNR_API void snr_buffer_free(snr_buffer* buffer);
SNR_API void snr_bitmap_free(snr_bitmap* bitmap);
SNR_API void snr_raster_free(snr_raster* raster);
/* Install and silence may race with rendering. Keep callback contexts alive
 * until every rendering thread that could have seen them has joined. The
 * sink is silent by default and never receives notebook content. */
SNR_API void snr_log_install(snr_log_sink sink, void* context);
SNR_API void snr_log_silence(void);
/* Render one zero-based page into owned premultiplied pixels.
 * Notebook and options are borrowed, immutable, and never modified. Output
 * is untouched on failure; the optional error is untouched on success. */
[[nodiscard]] SNR_API snr_status snr_page_bitmap(const sn_notebook* notebook,
                                                 size_t page,
                                                 const snr_options* options,
                                                 snr_bitmap* out,
                                                 snr_error* error);
/* Render one zero-based page into owned straight gray/coverage planes.
 * Notebook and options are borrowed, immutable, and never modified. Output
 * is untouched on failure; the optional error is untouched on success. */
[[nodiscard]] SNR_API snr_status snr_page_raster(const sn_notebook* notebook,
                                                 size_t page,
                                                 const snr_options* options,
                                                 snr_raster* out,
                                                 snr_error* error);
/* Encode one zero-based page as an RGBA PNG.
 * Notebook and options are borrowed, immutable, and never modified. Output
 * is untouched on failure; the optional error is untouched on success. */
[[nodiscard]] SNR_API snr_status snr_page_png(const sn_notebook* notebook,
                                              size_t page,
                                              const snr_options* options,
                                              snr_buffer* out,
                                              snr_error* error);
/* Encode one zero-based page as SVG containing a lossless PNG.
 * Notebook and options are borrowed, immutable, and never modified. Output
 * is untouched on failure; the optional error is untouched on success. */
[[nodiscard]] SNR_API snr_status snr_page_svg(const sn_notebook* notebook,
                                              size_t page,
                                              const snr_options* options,
                                              snr_buffer* out,
                                              snr_error* error);
/* Encode one zero-based page as PDF with annotations and recognition.
 * Notebook and options are borrowed, immutable, and never modified. Output
 * is untouched on failure; the optional error is untouched on success. */
[[nodiscard]] SNR_API snr_status snr_page_pdf(const sn_notebook* notebook,
                                              size_t page,
                                              const snr_options* options,
                                              snr_buffer* out,
                                              snr_error* error);
/* Range is zero-based, first inclusive, count positive; SIZE_MAX means all
 * remaining pages. Destinations outside the exported range are omitted. */
[[nodiscard]] SNR_API snr_status snr_document_pdf(const sn_notebook* notebook,
                                                  size_t first, size_t count,
                                                  const snr_options* options,
                                                  snr_buffer* out,
                                                  snr_error* error);
/* Streaming equivalents. The descriptor is never modified and error stays
 * untouched on success. On failure discard any partial external byte stream.
 * PNG/PDF stream as encoded; SVG buffers its PNG for base64 conversion. */
[[nodiscard]] SNR_API snr_status snr_page_png_to_output(
    const sn_notebook* notebook, size_t page, const snr_options* options,
    const snr_output* output, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_page_svg_to_output(
    const sn_notebook* notebook, size_t page, const snr_options* options,
    const snr_output* output, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_document_pdf_to_output(
    const sn_notebook* notebook, size_t first, size_t count,
    const snr_options* options, const snr_output* output, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_page_pdf_to_output(
    const sn_notebook* notebook, size_t page, const snr_options* options,
    const snr_output* output, snr_error* error);
/* Document overlay options have an independent ABI so existing rendering
 * options and all 19 original functions retain their contracts. The reader
 * cannot identify PDF versus EPUB marks; callers must supply the source kind.
 * EPUB layout is unsupported. Null options assert a PDF source with defaults.
 * Strict mode rejects out-of-range annotations and paired highlight geometry
 * inconsistent by more than 3 sheet pixels; default mode logs warnings and
 * counts omissions and residuals in the optional result. No source page count
 * or size is stored in the reader model, so absence of such evidence cannot
 * validate the source's identity. Sheet coordinates use the minimum of the
 * sheet/page width and height ratios, centred on both axes of the displayed
 * rotated CropBox. Document rectangles are relative to that CropBox. Previews
 * use ceil(displayed PDF points * render.scale) pixels and draw the source page
 * with its visible annotations' normal appearances on both backends. The
 * result's no_pairs_agree is set when paired rectangles were checked and none
 * agrees within 3 pixels. A mark without pairs (handwriting only, or
 * highlights without document rectangles) carries no evidence either way:
 * pages_checked_by_pairs is zero, nothing is logged about it, and strict mode
 * accepts it, because strictness cannot prove that such a mark belongs to the
 * PDF. PDF export keeps
 * original page objects and adds a transparent raster ink overlay. Source bytes
 * use overlay.limits.maximum_source_bytes (512 MiB); scratch uses max(1 MiB, 16
 * * source size). Encoded output uses render.limits.maximum_output_bytes;
 * parser operations use maximum_work, source pages use maximum_pages. Source
 * boxes must be finite, positive and at most 14,400 points per side. Page
 * ranges and page queries use zero-based source document indexes. */
#define SNR_OVERLAY_ABI 2u
#define SNR_OVERLAY_RESULT_ABI 1u
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  size_t annotated_pages_drawn, sheets_dropped, highlights_dropped;
  double worst_paired_residual;
  size_t pages_checked_by_pairs, signature_fields;
  bool all_annotations_dropped, no_pairs_agree;
} snr_overlay_result;
typedef struct {
  size_t
      maximum_source_bytes; /* Default 512 MiB; parser scratch is at most
                             * 16 times the source size, with a 1 MiB floor. */
} snr_overlay_limits;
typedef enum snr_document_kind : uint32_t {
  SNR_DOCUMENT_PDF,
  SNR_DOCUMENT_EPUB
} snr_document_kind;
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  snr_options render;
  snr_document_kind source_kind;
  bool strict;
  snr_overlay_limits limits;
  /* Optional borrowed result, initialized with ABI and sizeof. Reset on entry
   * and filled as work proceeds, including diagnostics on failed calls. */
  snr_overlay_result* result;
} snr_overlay_options;
/* Set once before PDFium use. Both callbacks and context must remain alive
 * for the process lifetime. Late or repeated installation is rejected.
 * Cairo previews must not overlap any other PDFium use in the process,
 * including rmrender-lib, unless every user shares this lock. A host whose
 * callbacks take a non-recursive lock must not hold it while calling a
 * preview, or the preview deadlocks. PDF export does not use PDFium. Other
 * backends return SNR_ERR_UNSUPPORTED. */
[[nodiscard]] SNR_API snr_status snr_pdfium_set_lock(void (*lock)(void*),
                                                     void (*unlock)(void*),
                                                     void* context,
                                                     snr_error* error);
SNR_API snr_overlay_options snr_overlay_options_default(void);
/* Reports whether a sheet or highlight refers to this document page. */
[[nodiscard]] SNR_API snr_status snr_mark_page_has_annotations(
    const sn_notebook* mark, size_t page, bool* out, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_mark_document_pdf(
    const sn_notebook* mark, const uint8_t* pdf, size_t size, size_t first,
    size_t count, const snr_overlay_options* options, snr_buffer* out,
    snr_error* error);
[[nodiscard]] SNR_API snr_status snr_mark_document_pdf_to_output(
    const sn_notebook* mark, const uint8_t* pdf, size_t size, size_t first,
    size_t count, const snr_overlay_options* options, const snr_output* out,
    snr_error* error);
[[nodiscard]] SNR_API snr_status snr_mark_page_bitmap(
    const sn_notebook* mark, const uint8_t* pdf, size_t size, size_t page,
    const snr_overlay_options* options, snr_bitmap* out, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_mark_page_png(
    const sn_notebook* mark, const uint8_t* pdf, size_t size, size_t page,
    const snr_overlay_options* options, snr_buffer* out, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_mark_page_png_to_output(
    const sn_notebook* mark, const uint8_t* pdf, size_t size, size_t page,
    const snr_overlay_options* options, const snr_output* out,
    snr_error* error);
/* Extract the union of document_rects from one zero-based source PDF page.
 * Points (1/72 inch), x right/y down, relative to the top-left of the displayed
 * rotated CropBox (intersected with MediaBox), as used by mark overlays.
 * Characters are included once when their box centre is inside any rectangle:
 * left/top inclusive, right/bottom exclusive. Null/empty/outside rects select
 * nothing; negative sizes and nonfinite non-null rects are invalid.
 * Apple boxes use single-scalar PDFSelection bounds; PDFium uses glyph boxes.
 * Logical reading order comes from the PDF engine, independent of rect order.
 * Whitespace is collapsed to spaces or LF, with no leading/trailing whitespace.
 * A hard hyphen at a line wrap is retained and the wrap removed; U+00AD and
 * its wrap are removed when the engine exposes it. PDFium's discretionary
 * hyphen markers are kept as '-' because its public API loses the original
 * hard/soft distinction; PDFKit can omit soft hyphens, leaving LF.
 * U+200B..200D, U+2060, U+FEFF, Cc controls, surrogates
 * and Unicode noncharacters are removed, matching recognition text cleanup.
 * No OCR, dictionary dehyphenation or bidi reordering is performed. Engines
 * may expand ligatures or normalize invisible characters into whitespace.
 * The same PDF can yield slightly different strings per platform: soft-hyphen
 * wraps, ligature expansion, invisible-character whitespace, column order and
 * malformed text mappings depend on the engine. Output is
 * owned UTF-8 with an extra NUL (size excludes it); an empty match succeeds
 * with size zero. Free with snr_buffer_free. Outputs and optional errors follow
 * the existing success/failure contracts. Apple uses PDFKit; Cairo requires
 * PDFium or returns SNR_ERR_UNSUPPORTED. The PDFium process lock also covers
 * extraction; cancellation callbacks run outside that lock. Limits: source/
 * scratch/pages/geometry
 * match overlays. maximum_work and maximum_document_work bound parsing plus one
 * unit per rectangle and per character/rectangle test (including whitespace).
 * maximum_output_bytes counts the final NUL too. Character and rectangle
 * ceilings apply before iteration. Cancellation runs before parsing and during
 * scans; opaque engine loading is synchronous and cannot be interrupted or
 * fully metered by these limits. */
#define SNR_PDF_TEXT_ABI 1u
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  snr_limits
      limits; /* Only work, document work, pages and output bytes apply. */
  size_t
      maximum_source_bytes; /* Default 512 MiB; scratch max(1 MiB,16*size). */
  size_t
      maximum_rectangles; /* Default 4,096; zero permits only an empty union. */
  size_t maximum_characters; /* Engine page characters; default 1,048,576. */
  snr_cancel_fn cancel;
  void* cancel_context;
} snr_pdf_text_options;
/* Null options use defaults; larger structures accepted, complete prefix
 * required. Start with this initializer before overriding fields. */
SNR_API snr_pdf_text_options snr_pdf_text_options_default(void);
[[nodiscard]] SNR_API snr_status snr_pdf_page_text(
    const uint8_t* pdf, size_t size, size_t page, const sn_rect* rects,
    size_t rect_count, const snr_pdf_text_options* options, snr_buffer* out,
    snr_error* error);
/* Atelier drawings have their own options ABI. The forward declaration keeps
 * SNSpd optional for consumers; include <snspd/snspd.h> to open a drawing.
 * These functions are always exported. Builds with SNRENDER_ENABLE_SPD=OFF
 * return SNR_ERR_UNSUPPORTED, leaving outputs untouched.
 * Parsed drawings and all their arrays/bytes are borrowed and immutable.
 * Outputs, errors, output callbacks and free functions follow the contracts
 * above. Error index is the zero-based tile within its layer, or zero globally.
 */
typedef struct spd_drawing spd_drawing;
#define SNR_SPD_ABI 1u
typedef enum snr_spd_page : uint32_t {
  SNR_SPD_CANVAS,
  SNR_SPD_PAINTED_BOUNDS
} snr_spd_page;
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  /* scale, background, strict_images, limits and cancellation apply. Defaults
   * are white, scale 1, skip damaged tiles. Recognition, include_background,
   * progress and title are unused. Larger complete prefixes are accepted. */
  snr_options render;
  /* Zero uses render.scale. Otherwise output scale = dpi / canvas.ppi;
   * PDF physical size is always page pixels * 72 / canvas.ppi. Missing ppi
   * defaults to 300. PDF sides are clamped to at least 3 points. SVG uses page
   * pixel coordinates and an embedded PNG. */
  double dpi;
  /* Default painted bounds translates the selected stored tile union to the
   * top left; empty bounds produce 1x1. It matches Atelier's own thumbnail
   * framing. Canvas clips to surface.width/height centred on the saved
   * viewport: (tile*128 - vp) * vp.scale + surface size / 2; centre is an
   * inference from one real export. */
  snr_spd_page page;
  /* Default true: fields 3/4 have no established visibility semantics. False
   * omits layer id 9999. No visibility flags are guessed from unknown fields.
   */
  bool include_reference_layer;
  size_t
      maximum_tiles; /* All listed tiles, including omitted; default 262144. */
  /* Cumulative decoded RGBA tile bytes plus source/output page buffers;
   * excludes opaque platform codec/encoder scratch. Default 256 MiB. */
  size_t maximum_decoded_bytes;
} snr_spd_options;
/* Geometry and work accounting are documented in README's Atelier section.
 * Null options select defaults. Zero ceilings permit no corresponding work. */
SNR_API snr_spd_options snr_spd_options_default(void);
[[nodiscard]] SNR_API snr_status snr_spd_bitmap(const spd_drawing* drawing,
                                                const snr_spd_options* options,
                                                snr_bitmap* out,
                                                snr_error* error);
[[nodiscard]] SNR_API snr_status snr_spd_png(const spd_drawing* drawing,
                                             const snr_spd_options* options,
                                             snr_buffer* out, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_spd_svg(const spd_drawing* drawing,
                                             const snr_spd_options* options,
                                             snr_buffer* out, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_spd_pdf(const spd_drawing* drawing,
                                             const snr_spd_options* options,
                                             snr_buffer* out, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_spd_png_to_output(
    const spd_drawing* drawing, const snr_spd_options* options,
    const snr_output* out, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_spd_svg_to_output(
    const spd_drawing* drawing, const snr_spd_options* options,
    const snr_output* out, snr_error* error);
[[nodiscard]] SNR_API snr_status snr_spd_pdf_to_output(
    const spd_drawing* drawing, const snr_spd_options* options,
    const snr_output* out, snr_error* error);
#ifdef __cplusplus
}
#endif
#endif
