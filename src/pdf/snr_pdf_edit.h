/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#ifndef SNR_PDF_EDIT_H
#define SNR_PDF_EDIT_H
#include "../snr_internal.h"
typedef struct {
  size_t maximum_elements, maximum_plan_bytes, maximum_pdf_bytes, maximum_draws;
  double maximum_coordinate;
  uint64_t* remaining;
  snr_cancel_fn cancel;
  void* context;
} spe_limits;
static inline spe_limits spe_limits_default(void) {
  return (spe_limits){10000000, 268435456, 536870912, 10000,
                      1000000,  nullptr,   nullptr,   nullptr};
}
typedef struct {
  double x, y, width, height;
} spe_rect;
typedef struct {
  double a, b, c, d, tx, ty;
} spe_transform;
typedef struct {
  size_t page;
  const uint8_t* pdf;
  size_t size;
  spe_transform transform;
  bool replace_box;
  spe_rect box;
} spe_pdf_overlay;
typedef struct {
  size_t pages_updated, original_bytes;
  bool original_revision_preserved;
} spe_pdf_incremental_report;
typedef struct {
  spe_rect media, crop;
  bool has_media;
  int32_t rotate;
} spe_pdf_page_geometry;
typedef struct spe_pdf_update spe_pdf_update;
/* On success, returns caller-owned geometry (free with free) and its count
 * from one parse. On failure, the geometry pointer remains unchanged. */
snr_status spe_pdf_pages(const uint8_t*, size_t, const spe_limits*,
                         spe_pdf_page_geometry**, size_t*, size_t*, snr_error*);
snr_status spe_pdf_select_pages(const uint8_t*, size_t, const size_t*, size_t,
                                const spe_limits*, snr_cancel_fn, void*,
                                snr_buffer*, snr_error*);
snr_status spe_pdf_update_create(const uint8_t*, size_t, bool,
                                 const spe_limits*, snr_cancel_fn, void*,
                                 const snr_output*, spe_pdf_update**,
                                 snr_error*);
snr_status spe_pdf_update_page(spe_pdf_update*, const spe_pdf_overlay*, size_t,
                               snr_error*);
/* Makes a page also draw its visible annotations' normal appearances, for
 * renderers that draw page content only, or with orphans_only just the
 * widgets outside the AcroForm field tree, for renderers that draw every
 * other annotation; drawn receives their number. */
snr_status spe_pdf_update_appearances(spe_pdf_update*, size_t,
                                      bool orphans_only, size_t*, snr_error*);
snr_status spe_pdf_update_finish(spe_pdf_update*, snr_buffer*,
                                 spe_pdf_incremental_report*, snr_error*);
void spe_pdf_update_free(spe_pdf_update*);
#endif
