/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef SNR_INTERNAL_H
#define SNR_INTERNAL_H
#include <math.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  const sn_notebook* notebook;
  const sn_page* page;
  snr_options options;
  size_t width, height, source_width, source_height, work, index;
  double points_per_pixel;
} snr_plan;
/* Where a writer's first failure came from. The status alone cannot say: a
 * caller's callback may return any status, including SNR_ERR_LIMIT. */
typedef enum snr_origin : uint32_t {
  SNR_ORIGIN_NONE,
  SNR_ORIGIN_LIMIT,      /* maximum_output_bytes would be exceeded */
  SNR_ORIGIN_CALLBACK,   /* the caller's write callback returned non-OK */
  SNR_ORIGIN_ALLOCATION, /* growing the owned output buffer failed */
} snr_origin;
typedef struct {
  const snr_output* output;
  snr_buffer buffer;
  size_t limit, total, capacity;
  uint64_t document_work;
  snr_status status; /* First failure; every later write is dropped. */
  snr_origin origin;
} snr_writer;
snr_status snr_fail(snr_error* error, snr_status status, size_t index,
                    const char* message);
snr_status snr_options_read(const snr_options* input, snr_options* out,
                            snr_error* error);
snr_status snr_image_plan(const sn_notebook*, size_t, const snr_options*,
                          snr_plan*, snr_error*);
snr_status snr_plan_make(const sn_notebook* notebook, size_t page,
                         const snr_options* options, snr_plan* out,
                         snr_error* error);
snr_status snr_compose(const snr_plan* plan, snr_bitmap* out, snr_error* error);
bool snr_cancelled(const snr_options* options);
void snr_log(snr_log_level level, const char* message);
snr_status snr_write(snr_writer* writer, const void* bytes, size_t size);
/* Diagnostic for the writer's first failure, or null when it has none. */
const char* snr_writer_message(const snr_writer* writer);
#ifdef SNR_TESTING
/* Test builds only: the buffer growth this many growths from now fails. */
extern size_t snr_test_growth_failure;
#endif
snr_status snr_output_validate(const snr_output* output, snr_error* error);
snr_status snr_image_frame(const sn_bitmap* image, const snr_options* options,
                           size_t* size);
snr_status snr_image_size(const sn_bitmap* image, size_t limit, size_t* width,
                          size_t* height);
snr_status snr_decode(const sn_bitmap* encoded, size_t limit,
                      const snr_options* options, snr_bitmap* out);
snr_status snr_encode_png(const snr_bitmap* bitmap, snr_writer* writer);
snr_status snr_backend_pdf(const sn_notebook* notebook, size_t first,
                           size_t count, const snr_options* options,
                           snr_writer* writer, snr_error* error);
snr_status snr_encode_pdf_bitmap(const snr_bitmap* bitmap, double width,
                                 double height, snr_writer* writer);
snr_status snr_encode_svg(const snr_bitmap* bitmap, double width, double height,
                          const snr_options* options, snr_writer* writer);
snr_status snr_svg(const snr_plan* plan, snr_writer* writer, snr_error* error);
size_t snr_link_target(const sn_notebook* notebook, const sn_link* link,
                       size_t first, size_t count);
#include "snr_text.h"
bool snr_uri_allowed(sn_string uri);
double snr_page_dpi(const sn_notebook* notebook, const sn_page* page);
#endif
