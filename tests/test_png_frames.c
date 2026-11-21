/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Renders each PNG file as a strict-mode layer and prints the status. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char** argv) {
  for (int a = 1; a < argc; a++) {
    FILE* f = fopen(argv[a], "rb");
    if (!f) continue;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    uint8_t* buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) return 1;
    fclose(f);
    sn_bitmap enc = {.encoding = SN_ENCODING_PNG, .bytes = {buf, (size_t)len}};
    sn_layer layer = {.bitmap = &enc, .is_visible = true};
    sn_page page = {
        .number = 1, .size = {64, 64}, .layers = &layer, .layer_count = 1};
    sn_notebook nb = {.pages = &page, .page_count = 1};
    snr_options o = snr_options_default();
    o.strict_images = true;
    o.limits.maximum_decoded_pixels = 1u << 26;
    o.limits.maximum_output_bytes = 1u << 30;
    snr_bitmap b = {0};
    snr_error e = {0};
    clock_t t = clock();
    snr_status s = snr_page_bitmap(&nb, 0, &o, &b, &e);
    const char* name = strrchr(argv[a], '/');
    printf("%-16s %-16s %.3fs %s\n", name ? name + 1 : argv[a],
           snr_status_string(s), (double)(clock() - t) / CLOCKS_PER_SEC,
           s ? e.message : "");
    bool reject = strstr(argv[a], "len_") || strstr(argv[a], "no_iend") ||
                  strstr(argv[a], "iend_nonzero") ||
                  strstr(argv[a], "truncated_crc") ||
                  strstr(argv[a], "only_header");
    if (s != (reject ? SNR_ERR_BACKEND : SNR_OK)) return 1;
    snr_bitmap_free(&b);
    free(buf);
  }
  return 0;
}
