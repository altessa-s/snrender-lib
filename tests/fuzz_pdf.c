/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "source_pdf.h"
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 8388608) return 0;
  static const sn_rect rect = {20, 40, 60, 15, false};
  sn_highlight h = {.page_number = 1, .rects = &rect, .rect_count = 1};
  sn_notebook mark = {.kind = SN_KIND_MARK,
                      .sheet = {120, 160},
                      .highlights = &h,
                      .highlight_count = 1};
  snr_overlay_options o = snr_overlay_options_default();
  o.render.scale = 0.25;
  o.render.limits.maximum_pixels = 1048576;
  o.render.limits.maximum_document_work = 10000000;
  o.render.limits.maximum_output_bytes = 16777216;
  for (unsigned range = 0; range < 2; range++) {
    snr_buffer out = {0};
    snr_error e = {0};
    snr_status s = snr_mark_document_pdf(&mark, data, size, 0,
                                         range ? 1 : SIZE_MAX, &o, &out, &e);
    if (s && (out.bytes || out.size || e.code != s)) abort();
    if (!s) {
      snr_buffer again = {0};
      if (snr_mark_document_pdf(&mark, out.bytes, out.size, 0, SIZE_MAX, &o,
                                &again, &e))
        abort();
      snr_buffer_free(&again);
    }
    snr_buffer_free(&out);
  }
  /* Previews reach the source renderer and, for pages with annotation
   * appearances, the in-memory rewrite that makes them drawable. A failed
   * preview leaves its bitmap untouched. */
  for (size_t page = 0; page < 2; page++) {
    snr_bitmap bitmap = {0};
    snr_error e = {0};
    snr_status s =
        snr_mark_page_bitmap(&mark, data, size, page, &o, &bitmap, &e);
    if (s && (bitmap.pixels || bitmap.width || e.code != s)) abort();
    if (!s && (!bitmap.pixels || bitmap.stride < bitmap.width * 4)) abort();
    snr_bitmap_free(&bitmap);
  }
  snr_pdf_text_options text_options = snr_pdf_text_options_default();
  text_options.maximum_source_bytes = 8388608;
  text_options.maximum_rectangles = 8;
  text_options.maximum_characters = 32768;
  text_options.limits.maximum_work = 1000000;
  text_options.limits.maximum_document_work = 1000000;
  text_options.limits.maximum_output_bytes = 65536;
  sn_rect selection[2] = {rect, rect};
  if (size >= 4) {
    selection[1].x = data[0];
    selection[1].y = data[1];
    selection[1].width = data[2];
    selection[1].height = data[3];
  }
  snr_buffer text = {0};
  snr_error error = {0};
  snr_status status =
      snr_pdf_page_text(data, size, size ? data[0] % 2 : 0, selection, 2,
                        &text_options, &text, &error);
  if (status && (text.bytes || text.size || error.code != status)) abort();
  if (!status && (!text.bytes || text.bytes[text.size])) abort();
  snr_buffer_free(&text);
  return 0;
}
#ifdef SNR_PDF_REPLAY
int main(int argc, char** argv) {
  if (argc == 3 && !strcmp(argv[1], "--seed")) {
    FILE* f = fopen(argv[2], "wb");
    if (!f) return 1;
    bool ok = fwrite(source_pdf, 1, sizeof source_pdf, f) == sizeof source_pdf;
    return fclose(f) || !ok;
  }
  LLVMFuzzerTestOneInput(source_pdf, sizeof source_pdf);
  uint8_t mutated[sizeof source_pdf];
  for (size_t i = 0; i < sizeof source_pdf; i++) {
    memcpy(mutated, source_pdf, sizeof mutated);
    mutated[i] ^= 1;
    LLVMFuzzerTestOneInput(mutated, sizeof mutated);
  }
  for (int i = 1; i < argc; i++) {
    FILE* f = fopen(argv[i], "rb");
    if (!f) return 1;
    uint8_t* bytes = malloc(8388608);
    if (!bytes) return 1;
    size_t n = fread(bytes, 1, 8388608, f);
    fclose(f);
    LLVMFuzzerTestOneInput(bytes, n);
    free(bytes);
  }
  return 0;
}
#endif
