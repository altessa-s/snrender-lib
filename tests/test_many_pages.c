/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  rewind(f);
  uint8_t* b = malloc((size_t)len);
  if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) return 1;
  fclose(f);
  sn_notebook* n = nullptr;
  if (sn_parse_notebook(b, (size_t)len, &n, nullptr)) return 1;
  size_t count = (size_t)atol(argv[2]);
  sn_page* pages = calloc(count, sizeof *pages);
  for (size_t i = 0; i < count; i++) {
    pages[i] = n->pages[0];
    pages[i].number = (int64_t)i + 1;
  }
  sn_notebook copy = *n;
  copy.pages = pages;
  copy.page_count = count;
  copy.link_count = 0;
  copy.title_count = 0;
  snr_options o = snr_options_default();
  o.recognition = argc > 3;
  snr_buffer pdf = {0};
  snr_error e = {0};
  clock_t t = clock();
  snr_status s = snr_document_pdf(&copy, 0, SIZE_MAX, &o, &pdf, &e);
  printf("%zu pages (%gx%g, %zu layers): %s %s size=%zu cpu=%.1fs\n", count,
         pages[0].size.width, pages[0].size.height, pages[0].layer_count,
         snr_status_string(s), s ? e.message : "", pdf.size,
         (double)(clock() - t) / CLOCKS_PER_SEC);
  if (!s && argc > 4) {
    FILE* output = fopen(argv[4], "wb");
    if (!output || fwrite(pdf.bytes, 1, pdf.size, output) != pdf.size) return 1;
    fclose(output);
  }
  snr_buffer_free(&pdf);
  if (s != SNR_OK) return 1;
  o.limits.maximum_work = 1;
  s = snr_document_pdf(&copy, 0, SIZE_MAX, &o, &pdf, &e);
  if (s != SNR_ERR_LIMIT) return 1;
  puts("small per-page budget refused");
  free(pages);
  sn_notebook_free(n);
  free(b);
  return 0;
}
