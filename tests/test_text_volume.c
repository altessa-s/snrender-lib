/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const char* mode = argv[1];
  size_t n = argc > 3 ? (size_t)atol(argv[3]) : 1;
  snr_options o = snr_options_default();
  o.scale = 1.0 / 16;
  snr_buffer pdf = {0};
  snr_error e = {0};
  snr_status s;
  clock_t t = clock();
  if (!strcmp(mode, "bounds")) {
    static char w[16][128];
    sn_word words[16];
    size_t k = 0;
    /* 32 ASCII, 33 ASCII, 31 ASCII + 3-byte char, 30 + 4-byte char,
     * base+combining straddling 32, Arabic with harakat straddling. */
    memset(w[k], 'a', 32);
    words[k] = (sn_word){{w[k], 32}, {5, 5, 60, 4, false}};
    k++;
    memset(w[k], 'b', 33);
    words[k] = (sn_word){{w[k], 33}, {5, 10, 60, 4, false}};
    k++;
    memset(w[k], 'c', 31);
    memcpy(w[k] + 31, "\xE4\xBD\xA0", 3);
    words[k] = (sn_word){{w[k], 34}, {5, 15, 60, 4, false}};
    k++;
    memset(w[k], 'd', 30);
    memcpy(w[k] + 30, "\xF0\x9F\x98\x80", 4);
    words[k] = (sn_word){{w[k], 34}, {5, 20, 60, 4, false}};
    k++;
    memset(w[k], 'e', 31);
    memcpy(w[k] + 31, "\xCC\x81\xCC\x82", 4);
    words[k] = (sn_word){{w[k], 35}, {5, 25, 60, 4, false}};
    k++;
    for (size_t i = 0; i < 12; i++) memcpy(w[k] + i * 4, "\xD8\xA8\xD9\x8E", 4);
    words[k] = (sn_word){{w[k], 48}, {5, 30, 60, 4, false}};
    k++;
    memcpy(w[k], "x\xD8", 2); /* truncated after sanitizer -> omitted */
    words[k] = (sn_word){{w[k], 2}, {5, 35, 60, 4, false}};
    k++;
    memset(w[k], 'f', 1);
    words[k] = (sn_word){{w[k], 1}, {5, 40, 0.000002, 4, false}};
    k++;
    sn_element el = {.words = words, .word_count = k};
    sn_recognition rec = {.elements = &el, .element_count = 1};
    sn_page page = {.number = 1, .size = {1404, 1872}, .recognition = &rec};
    sn_notebook nb = {.pages = &page, .page_count = 1};
    s = snr_document_pdf(&nb, 0, SIZE_MAX, &o, &pdf, &e);
  } else if (!strcmp(mode, "unique")) {
    /* Distinct words expose font storage that incorrectly follows text volume.
     */
    size_t W = 2000;
    sn_page* pages = calloc(n, sizeof *pages);
    sn_element* els = calloc(n, sizeof *els);
    sn_recognition* recs = calloc(n, sizeof *recs);
    sn_word* words = calloc(n * W, sizeof *words);
    char* text = malloc(n * W * 32 + 1);
    for (size_t p = 0; p < n; p++) {
      for (size_t i = 0; i < W; i++) {
        char* c = text + (p * W + i) * 32;
        snprintf(c, 33, "w%015zu%016zu", p, i);
        words[p * W + i] =
            (sn_word){{c, 32},
                      {1 + (double)(i % 20) * 5, 1 + (double)(i / 20) * 1.5, 4,
                       0.5 + (double)(p * W + i) / (double)(n * W), false}};
      }
      els[p] = (sn_element){.words = words + p * W, .word_count = W};
      recs[p] = (sn_recognition){.elements = els + p, .element_count = 1};
      pages[p] = (sn_page){.number = (int64_t)p + 1,
                           .size = {1404, 1872},
                           .recognition = recs + p};
    }
    sn_notebook nb = {.pages = pages, .page_count = n};
    s = snr_document_pdf(&nb, 0, SIZE_MAX, &o, &pdf, &e);
    free(text);
    free(words);
    free(recs);
    free(els);
    free(pages);
    if (!s && pdf.size > n * 40000 + 20000) return 1;
  } else if (!strcmp(mode, "reuse")) {
    /* Identical rasters on pages 1-2 with different recognition and links;
     * page 3 differs only by explicit background through a mark-kind? Not
     * expressible per page; instead page 3 has a different size. */
    sn_word w1[] = {{{"PageOne", 7}, {5, 5, 40, 8, false}}};
    sn_word w2[] = {{{"PageTwo", 7}, {50, 50, 40, 8, false}}};
    sn_element e1 = {.words = w1, .word_count = 1},
               e2 = {.words = w2, .word_count = 1};
    sn_recognition r1 = {.elements = &e1, .element_count = 1},
                   r2 = {.elements = &e2, .element_count = 1};
    sn_link links[] = {{.page_number = 2,
                        .kind = 4,
                        .url = {"https://two/", 12},
                        .rect = {100, 100, 200, 50, false}}};
    sn_page pages[3] = {{.number = 1, .size = {1404, 1872}, .recognition = &r1},
                        {.number = 2, .size = {1404, 1872}, .recognition = &r2},
                        {.number = 3, .size = {1872, 1404}}};
    sn_notebook nb = {.equipment = SN_EQUIPMENT_NOMAD,
                      .pages = pages,
                      .page_count = 3,
                      .links = links,
                      .link_count = 1};
    s = snr_document_pdf(&nb, 0, SIZE_MAX, &o, &pdf, &e);
  } else
    return 2;
  printf("%s n=%zu: %s %s size=%zu cpu=%.2fs\n", mode, n, snr_status_string(s),
         s ? e.message : "", pdf.size, (double)(clock() - t) / CLOCKS_PER_SEC);
  if (!s) {
    FILE* f = fopen(argv[2], "wb");
    if (f) {
      fwrite(pdf.bytes, 1, pdf.size, f);
      fclose(f);
    }
  }
  snr_buffer_free(&pdf);
  return s ? 1 : 0;
}
