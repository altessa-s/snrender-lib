/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#ifndef SNR_PDF_TEXT_H
#define SNR_PDF_TEXT_H
#include "pdf/snr_pdf_edit.h"
typedef struct {
  snr_pdf_text_options options;
  spe_pdf_page_geometry geometry;
  const sn_rect* rects;
  size_t rect_count, page, page_count;
  uint64_t remaining;
  snr_writer writer;
  uint32_t previous;
  char separator;
  bool skipped, discretionary;
} snr_pdf_text_state;
snr_status snr_pdf_text_charge(snr_pdf_text_state* state, size_t count);
snr_status snr_pdf_text_character(snr_pdf_text_state* state, uint32_t scalar,
                                  double left, double bottom, double right,
                                  double top);
snr_status snr_source_text(const uint8_t* bytes, size_t size,
                           snr_pdf_text_state* state);
#endif
