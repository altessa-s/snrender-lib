/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <snrender/snrender.h>
#include <string.h>
#ifdef SNR_CONSUMER_SPD
#include <snspd/snspd.h>
#endif
int main(void) {
  sn_page page = {.size = {10, 10}};
  sn_notebook n = {.pages = &page, .page_count = 1};
  snr_buffer b = {0};
  if (snr_page_png(&n, 0, nullptr, &b, nullptr) != SNR_OK) return 1;
  int result = b.size < 8 || memcmp(b.bytes, "\211PNG", 4);
  snr_buffer_free(&b);
  /* Even invalid input must link the platform text implementation from a
   * static archive, including its Objective-C/PDFKit dependencies on Apple. */
  if (snr_pdf_page_text(nullptr, 0, 0, nullptr, 0, nullptr, &b, nullptr) !=
      SNR_ERR_INVALID_ARGUMENT)
    return 1;
  snr_spd_options drawing = snr_spd_options_default();
  snr_status status = snr_spd_png(nullptr, &drawing, &b, nullptr);
  if (status != SNR_ERR_INVALID_ARGUMENT && status != SNR_ERR_UNSUPPORTED)
    return 1;
#ifdef SNR_CONSUMER_SPD
  spd_drawing model = {
      .canvas = {
          .width = 10, .height = 10, .has_width = true, .has_height = true}};
  if (snr_spd_png(&model, &drawing, &b, nullptr) != SNR_OK) return 1;
  if (b.size < 8 || memcmp(b.bytes, "\211PNG", 4)) return 1;
  snr_buffer_free(&b);
#endif
  return result;
}
