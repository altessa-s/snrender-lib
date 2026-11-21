/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  enum { count = 270, scalars = 256 };
  char* labels = malloc(count * scalars * 4);
  sn_word* words = calloc(count, sizeof *words);
  if (!labels || !words) return 1;
  for (size_t i = 0; i < count; i++) {
    char* label = labels + i * scalars * 4;
    size_t length = 0;
    for (size_t j = 0; j < scalars; j++) {
      unsigned cp = 0x1000 + (unsigned)(i * scalars + j);
      if (cp < 0x10000) {
        label[length++] = (char)(0xe0 | (cp >> 12));
        label[length++] = (char)(0x80 | ((cp >> 6) & 63));
        label[length++] = (char)(0x80 | (cp & 63));
      } else {
        label[length++] = (char)(0xf0 | (cp >> 18));
        label[length++] = (char)(0x80 | ((cp >> 12) & 63));
        label[length++] = (char)(0x80 | ((cp >> 6) & 63));
        label[length++] = (char)(0x80 | (cp & 63));
      }
    }
    words[i] =
        (sn_word){{label, length}, {1, 1 + (double)i * 0.5, 100, 0.4, false}};
  }
  sn_element el = {.words = words, .word_count = count};
  sn_recognition rec = {.elements = &el, .element_count = 1};
  sn_page page = {.number = 1, .size = {1404, 1872}, .recognition = &rec};
  sn_notebook book = {.pages = &page, .page_count = 1};
  snr_options options = snr_options_default();
  options.scale = 0.0625;
  snr_buffer pdf = {0};
  snr_error error = {0};
  snr_status s = snr_page_pdf(&book, 0, &options, &pdf, &error);
  printf("glyph ceiling: %s: %s\n", snr_status_string(s), error.message);
  bool ok = s == SNR_ERR_LIMIT && strstr(error.message, "65,533");
  snr_buffer_free(&pdf);
  free(words);
  free(labels);
  return ok ? 0 : 1;
}
