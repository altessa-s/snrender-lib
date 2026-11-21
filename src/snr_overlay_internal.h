/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#ifndef SNR_OVERLAY_INTERNAL_H
#define SNR_OVERLAY_INTERNAL_H
#include "pdf/snr_pdf_edit.h"
/* Source readers render the displayed crop, after the page's rotation, with
 * the annotations' normal appearances. limits bound any PDF editing the
 * reader needs for that. */
snr_status snr_source_bitmap(const uint8_t* bytes, size_t size, size_t page,
                             size_t count, const spe_limits* limits,
                             snr_bitmap* bitmap, snr_error* error);
/* Rewrites one page in memory so that its content also draws annotation
 * appearances the renderer would skip (see spe_pdf_update_appearances).
 * drawn is zero, and out empty, when nothing needed drawing. */
snr_status snr_source_flatten(const uint8_t* bytes, size_t size, size_t page,
                              const spe_limits* limits, bool orphans_only,
                              snr_buffer* out, size_t* drawn, snr_error* error);
#endif
