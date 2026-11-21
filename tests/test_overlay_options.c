/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "source_pdf.h"
#define CHECK(x)                                      \
  do {                                                \
    if (!(x)) {                                       \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
      return 1;                                       \
    }                                                 \
  } while (0)
#ifdef SNR_OVERLAY_SUPPORTED
static size_t warnings, all_warnings;
static void count_warning(snr_log_level level, const char* message,
                          void* context) {
  (void)context;
  if (level == SNR_LOG_WARNING) all_warnings++;
  if (level == SNR_LOG_WARNING && strstr(message, "disagree")) warnings++;
}
#endif
int main(void) {
  sn_page page = {.number = 2, .size = {120, 160}};
  sn_notebook n = {.kind = SN_KIND_MARK,
                   .sheet = {120, 160},
                   .pages = &page,
                   .page_count = 1};
  snr_overlay_options o = snr_overlay_options_default();
  snr_buffer b = {0};
  snr_bitmap bitmap = {0};
  snr_error e = {0};
  bool found = false;
  CHECK(snr_mark_page_has_annotations(&n, 1, &found, &e) == SNR_OK && found);
  CHECK(snr_mark_page_has_annotations(&n, 0, &found, &e) == SNR_OK && !found);
  CHECK(snr_mark_page_has_annotations(&n, SIZE_MAX, &found, &e) ==
        SNR_ERR_INVALID_ARGUMENT);
  o.abi_version++;
  CHECK(snr_mark_page_bitmap(&n, source_pdf, sizeof source_pdf, 0, &o, &bitmap,
                             &e) == SNR_ERR_INVALID_ARGUMENT);
  o = snr_overlay_options_default();
  o.source_kind = SNR_DOCUMENT_EPUB;
  CHECK(snr_mark_page_bitmap(&n, source_pdf, sizeof source_pdf, 0, &o, &bitmap,
                             &e) == SNR_ERR_UNSUPPORTED);
  o = snr_overlay_options_default();
#ifdef SNR_OVERLAY_SUPPORTED
  CHECK(snr_mark_document_pdf(&n, source_pdf, sizeof source_pdf, 0, SIZE_MAX,
                              &o, &b, &e) == SNR_OK);
  CHECK(b.size == sizeof source_pdf && !memcmp(b.bytes, source_pdf, b.size));
  snr_buffer_free(&b);
  snr_overlay_result result = {.abi_version = SNR_OVERLAY_RESULT_ABI,
                               .struct_size = sizeof result};
  o.result = &result;
  CHECK(snr_mark_document_pdf(&n, source_pdf, sizeof source_pdf, 0, SIZE_MAX,
                              &o, &b, &e) == SNR_OK);
  CHECK(result.sheets_dropped == 1 && result.all_annotations_dropped &&
        !result.annotated_pages_drawn);
  snr_buffer_free(&b);
  o.limits.maximum_source_bytes = sizeof source_pdf - 1;
  CHECK(snr_mark_document_pdf(&n, source_pdf, sizeof source_pdf, 0, SIZE_MAX,
                              &o, &b, &e) == SNR_ERR_LIMIT);
  CHECK(!strcmp(e.message, "source PDF byte limit exceeded"));
  o.limits.maximum_source_bytes++;
  CHECK(snr_mark_document_pdf(&n, source_pdf, sizeof source_pdf, 0, SIZE_MAX,
                              &o, &b, &e) == SNR_OK);
  snr_buffer_free(&b);
  o.render.limits.maximum_output_bytes = sizeof source_pdf - 1;
  CHECK(snr_mark_document_pdf(&n, source_pdf, sizeof source_pdf, 0, SIZE_MAX,
                              &o, &b, &e) == SNR_ERR_LIMIT);
  CHECK(!strcmp(e.message, "output byte limit exceeded"));
  o.render.limits.maximum_output_bytes++;
  CHECK(snr_mark_document_pdf(&n, source_pdf, sizeof source_pdf, 0, SIZE_MAX,
                              &o, &b, &e) == SNR_OK);
  snr_buffer_free(&b);
  o.render.limits.maximum_output_bytes = 536870912;
  /* An annotated export reports the output limit by its own name at every
   * limit below its size, and succeeds at exactly that size. The sheet has
   * the page's size, so the pair agrees at scale one. */
  sn_rect screen = {12, 16, 48, 32, false}, document = screen;
  sn_highlight h = {.page_number = 1,
                    .rects = &screen,
                    .rect_count = 1,
                    .document_rects = &document,
                    .document_rect_count = 1};
  sn_notebook marked = {.kind = SN_KIND_MARK,
                        .sheet = {120, 160},
                        .highlights = &h,
                        .highlight_count = 1};
  CHECK(snr_mark_document_pdf(&marked, source_pdf, sizeof source_pdf, 0,
                              SIZE_MAX, &o, &b, &e) == SNR_OK);
  CHECK(result.annotated_pages_drawn == 1 &&
        result.pages_checked_by_pairs == 1 && !result.no_pairs_agree &&
        result.worst_paired_residual < 0.001);
  snr_buffer full = b;
  b = (snr_buffer){0};
  for (size_t limit = 1; limit < full.size; limit++) {
    o.render.limits.maximum_output_bytes = limit;
    CHECK(snr_mark_document_pdf(&marked, source_pdf, sizeof source_pdf, 0,
                                SIZE_MAX, &o, &b, &e) == SNR_ERR_LIMIT);
    CHECK(!b.bytes && !strcmp(e.message, "output byte limit exceeded"));
  }
  o.render.limits.maximum_output_bytes = full.size;
  CHECK(snr_mark_document_pdf(&marked, source_pdf, sizeof source_pdf, 0,
                              SIZE_MAX, &o, &b, &e) == SNR_OK);
  CHECK(b.size == full.size && !memcmp(b.bytes, full.bytes, b.size));
  snr_buffer_free(&b);
  snr_buffer_free(&full);
  o.render.limits.maximum_output_bytes = 536870912;
  /* A disagreeing pair is a warning with its residual in default mode and
   * an error in strict mode. */
  document.x += 10;
  warnings = 0;
  snr_log_install(count_warning, nullptr);
  CHECK(snr_mark_document_pdf(&marked, source_pdf, sizeof source_pdf, 0,
                              SIZE_MAX, &o, &b, &e) == SNR_OK);
  snr_log_silence();
  CHECK(warnings >= 1 && result.no_pairs_agree &&
        result.worst_paired_residual > 9.9 &&
        result.worst_paired_residual < 10.1);
  snr_buffer_free(&b);
  o.strict = true;
  CHECK(snr_mark_document_pdf(&marked, source_pdf, sizeof source_pdf, 0,
                              SIZE_MAX, &o, &b,
                              &e) == SNR_ERR_INVALID_ARGUMENT &&
        strstr(e.message, "disagree"));
  /* A sheet beyond the source is dropped with a count in default mode and
   * refused, at its zero-based page index, in strict mode. */
  o.strict = false;
  CHECK(snr_mark_document_pdf(&n, source_pdf, sizeof source_pdf, 0, SIZE_MAX,
                              &o, &b, &e) == SNR_OK);
  CHECK(result.sheets_dropped == 1 && result.all_annotations_dropped);
  snr_buffer_free(&b);
  o.strict = true;
  CHECK(snr_mark_document_pdf(&n, source_pdf, sizeof source_pdf, 0, SIZE_MAX,
                              &o, &b, &e) == SNR_ERR_INVALID_ARGUMENT);
  CHECK(e.index == 1);
  o.strict = false;
  CHECK(snr_mark_document_pdf(&n, source_pdf, sizeof source_pdf, 1, 1, &o, &b,
                              &e) == SNR_ERR_INVALID_ARGUMENT);
  /* Handwriting alone has no pairs: the export draws it without any warning
   * about belonging, and strict mode accepts it. */
  sn_page ink = {.number = 1, .size = {120, 160}};
  sn_notebook handwriting = {.kind = SN_KIND_MARK,
                             .sheet = {120, 160},
                             .pages = &ink,
                             .page_count = 1};
  all_warnings = 0;
  snr_log_install(count_warning, nullptr);
  for (int strict = 0; strict < 2; strict++) {
    o.strict = strict;
    CHECK(snr_mark_document_pdf(&handwriting, source_pdf, sizeof source_pdf, 0,
                                SIZE_MAX, &o, &b, &e) == SNR_OK);
    CHECK(result.annotated_pages_drawn == 1 && !result.no_pairs_agree &&
          !result.pages_checked_by_pairs);
    snr_buffer_free(&b);
  }
  snr_log_silence();
  CHECK(!all_warnings);
  o.strict = false;
  /* A sheet four times denser than its page is previewed at page density, so
   * a pixel limit below the sheet's own size still admits the preview; the
   * export keeps sheet density and stays bound by it. */
  sn_page dense = {.number = 1, .size = {480, 640}};
  sn_notebook large = {.kind = SN_KIND_MARK,
                       .sheet = {480, 640},
                       .pages = &dense,
                       .page_count = 1};
  o.render.limits.maximum_pixels = 100000;
  CHECK(snr_mark_page_bitmap(&large, source_pdf, sizeof source_pdf, 0, &o,
                             &bitmap, &e) == SNR_OK);
  CHECK(bitmap.width == 120 && bitmap.height == 160);
  snr_bitmap_free(&bitmap);
  CHECK(snr_mark_document_pdf(&large, source_pdf, sizeof source_pdf, 0,
                              SIZE_MAX, &o, &b, &e) == SNR_ERR_LIMIT);
#else
  CHECK(snr_mark_document_pdf(&n, source_pdf, sizeof source_pdf, 0, SIZE_MAX,
                              &o, &b, &e) == SNR_ERR_UNSUPPORTED);
  CHECK(strstr(e.message, "SNRENDER_ENABLE_PDFIUM"));
  CHECK(snr_mark_page_bitmap(&n, source_pdf, sizeof source_pdf, 0, &o, &bitmap,
                             &e) == SNR_ERR_UNSUPPORTED);
  CHECK(snr_mark_page_png(&n, source_pdf, sizeof source_pdf, 0, &o, &b, &e) ==
        SNR_ERR_UNSUPPORTED);
#endif
  CHECK(!b.bytes && !bitmap.pixels);
  return 0;
}
