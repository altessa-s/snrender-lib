/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  const char* mode = argv[1];
  size_t n = (size_t)atol(argv[2]);
  sn_word* words = nullptr;
  size_t wc = 0;
  char* text = nullptr;
  if (!strcmp(mode, "row") || !strcmp(mode, "same")) {
    words = calloc(n, sizeof *words);
    for (size_t i = 0; i < n; i++) {
      double x =
          !strcmp(mode, "row") ? 1 + (double)i * (110.0 / (double)n) : 10;
      words[wc++] =
          (sn_word){{"a", 1}, {x, 50, 110.0 / (double)n + 0.001, 5, false}};
    }
  } else if (!strcmp(mode, "chosen")) {
    static const unsigned scalars[] = {
        0x4ec1, 0x4ed9, 0x4ee9, 0x4ffe, 0x5089, 0x51e0, 0x5246, 0x52c3, 0x5323,
        0x535c, 0x54ac, 0x553a, 0x554d, 0x5781, 0x5794, 0x57ae, 0x57ec, 0x5842,
        0x59d0, 0x5ae6, 0x5bad, 0x5cd1, 0x5cd5, 0x5ecf, 0x6013, 0x6111, 0x6346,
        0x650a, 0x665d, 0x672a, 0x6753, 0x679c, 0x6841, 0x6851, 0x6a95, 0x6adb,
        0x6c76, 0x6d29, 0x6d8d, 0x6e91, 0x6f81, 0x6fb4, 0x7003, 0x709c, 0x7193,
        0x73ff, 0x758d, 0x77dc, 0x787c, 0x79f5, 0x7b96, 0x7c1c, 0x7e2d, 0x7f0f,
        0x8030, 0x8069, 0x8080, 0x80ab, 0x80da, 0x812a, 0x81e5, 0x8295, 0x834b,
        0x8445, 0x84d1, 0x8614, 0x8665, 0x867e, 0x87ed, 0x8880, 0x89cc, 0x8c0b,
        0x8dc7, 0x8f07, 0x8f24, 0x8f46, 0x9045, 0x91b9, 0x91f6, 0x9219};
    if (n > sizeof scalars / sizeof *scalars) return 2;
    text = malloc(n * 3);
    words = calloc(1, sizeof *words);
    for (size_t i = 0; i < n; i++) {
      unsigned cp = scalars[i];
      text[i * 3] = (char)(0xe0 | (cp >> 12));
      text[i * 3 + 1] = (char)(0x80 | ((cp >> 6) & 63));
      text[i * 3 + 2] = (char)(0x80 | (cp & 63));
    }
    words[wc++] = (sn_word){{text, n * 3}, {5, 5, 100, 8, false}};
  } else if (!strcmp(mode, "heights")) {
    text = calloc(n, 16);
    words = calloc(n, sizeof *words);
    for (size_t i = 0; i < n; i++) {
      char* label = text + i * 16;
      int length = snprintf(label, 16, "w%zu", i);
      words[wc++] =
          (sn_word){{label, (size_t)length},
                    {2 + (double)(i % 15) * 7.5, 2 + (double)(i / 15) * 0.75, 6,
                     3 + (double)i * 0.0007, false}};
    }
  } else
    return 2;
  sn_element el = {.words = words, .word_count = wc};
  sn_recognition rec = {.elements = &el, .element_count = 1};
  sn_page page = {.number = 1, .size = {1404, 1872}, .recognition = &rec};
  sn_notebook nb = {.pages = &page, .page_count = 1};
  snr_options o = snr_options_default();
  o.scale = 1.0 / 16;
  snr_buffer pdf = {0};
  snr_error e = {0};
  clock_t t = clock();
  snr_status s = snr_document_pdf(&nb, 0, SIZE_MAX, &o, &pdf, &e);
  printf("%s n=%zu words=%zu: %s %s size=%zu cpu=%.2fs\n", mode, n, wc,
         snr_status_string(s), s ? e.message : "", pdf.size,
         (double)(clock() - t) / CLOCKS_PER_SEC);
  if (!s && !strcmp(mode, "heights") && pdf.size > 160000) {
    snr_buffer_free(&pdf);
    free(words);
    free(text);
    return 1;
  }
  if (!s) {
    FILE* f = fopen(argv[3], "wb");
    if (f) {
      fwrite(pdf.bytes, 1, pdf.size, f);
      fclose(f);
    }
  }
  snr_buffer_free(&pdf);
  free(words);
  free(text);
  return s ? 1 : 0;
}
