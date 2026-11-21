/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
/* Mutates an encoded image and renders it as a layer, many times. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 1;
  static uint8_t orig[1 << 20], buf[1 << 20];
  size_t n = fread(orig, 1, sizeof orig, f);
  fclose(f);
  int jpeg = !strcmp(argv[2], "jpeg");
  long iters = atol(argv[3]);
  srand(12345);
  size_t counts[8] = {0};
  for (long it = 0; it < iters; it++) {
    memcpy(buf, orig, n);
    size_t len = n;
    int edits = 1 + rand() % 8;
    for (int k = 0; k < edits; k++) {
      size_t pos = (size_t)rand() % len;
      switch (rand() % 4) {
        case 0:
          buf[pos] = (uint8_t)rand();
          break;
        case 1:
          buf[pos] ^= (uint8_t)(1u << (rand() % 8));
          break;
        case 2:
          len = pos + 1;
          break;
        default:
          buf[pos] = (uint8_t)(rand() % 2 ? 0xff : 0);
          break;
      }
    }
    if (it == 0) len = n; /* unmodified baseline */
    if (it == 0) memcpy(buf, orig, n);
    sn_bitmap enc = {.encoding = jpeg ? SN_ENCODING_JPEG : SN_ENCODING_PNG,
                     .bytes = {buf, len}};
    sn_layer layer = {.bitmap = &enc, .is_visible = true};
    sn_page page = {
        .number = 1, .size = {37, 23}, .layers = &layer, .layer_count = 1};
    sn_notebook nb = {.pages = &page, .page_count = 1};
    snr_options o = snr_options_default();
    o.limits.maximum_decoded_pixels = 1 << 20;
    snr_bitmap b = {0};
    snr_status s = snr_page_bitmap(&nb, 0, &o, &b, nullptr);
    if (it == 0) printf("baseline status %s\n", snr_status_string(s));
    if (s != SNR_OK && s != SNR_ERR_LIMIT) return 1;
    counts[s < 8 ? s : 7]++;
    snr_bitmap_free(&b);
  }
  for (int i = 0; i < 7; i++)
    printf("%s: %zu\n", snr_status_string((snr_status)i), counts[i]);
  return 0;
}
