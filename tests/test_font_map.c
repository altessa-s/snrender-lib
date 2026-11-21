/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <limits.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../backends/linux/snr_font.h"
#include "../src/snr_text.h"

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  text_font* font = calloc(1, sizeof *font);
  if (!font) return 1;
  uint64_t document_work = 0;
  snr_text_work work = {0, SIZE_MAX, &document_work, UINT64_MAX};
  font->work = &work;
  snr_status s = font_create(font);
  printf("font_create: %d glyphs=%ld\n", (int)s,
         font->face ? font->face->num_glyphs : -1L);
  if (s) return 1;
  FT_Error e = FT_Load_Glyph(font->face, 1, FT_LOAD_NO_HINTING);
  printf("load glyph 1: %d advance=%ld\n", e,
         (long)font->face->glyph->advance.x);
  e = FT_Load_Glyph(font->face, 65534, FT_LOAD_NO_HINTING);
  printf("load glyph 65534: %d advance=%ld\n", e,
         (long)font->face->glyph->advance.x);
  /* Directory lengths include every padded table in the saved face. */
  size_t size = 12 + 8 * 16;
  for (int i = 0; i < 8; i++) {
    const uint8_t* entry = font->bytes + 12 + i * 16;
    size_t end = ((size_t)entry[8] << 24 | (size_t)entry[9] << 16 |
                  (size_t)entry[10] << 8 | entry[11]) +
                 ((size_t)entry[12] << 24 | (size_t)entry[13] << 16 |
                  (size_t)entry[14] << 8 | entry[15]);
    end = (end + 3) & ~(size_t)3;
    if (end > size) size = end;
  }
  FILE* f = fopen(argv[1], "wb");
  fwrite(font->bytes, 1, size, f);
  fclose(f);
  /* Distinct keys must exhaust the documented ceiling without wrapping IDs. */
  unsigned long glyph = 0;
  size_t inserted = 0;
  snr_status last = SNR_OK;
  for (uint32_t k = 0; k < 70000; k++) {
    char key[8];
    int n = snprintf(key, sizeof key, "%05u", k);
    last = scalar_glyph(font, key, (size_t)n, &glyph);
    if (last) break;
    inserted++;
  }
  printf("inserted %zu before status %d; last glyph %lu\n", inserted, (int)last,
         glyph);
  bool ok = inserted == 65533 && last == SNR_ERR_LIMIT;
  /* Exhausting new IDs must preserve existing lookups and reserved space. */
  last = scalar_glyph(font, "00000", 5, &glyph);
  printf("existing key: %d glyph %lu\n", (int)last, glyph);
  ok = ok && last == SNR_OK && glyph == 1;
  last = scalar_glyph(font, " ", 1, &glyph);
  printf("space: %d glyph %lu\n", (int)last, glyph);
  ok = ok && last == SNR_OK && glyph == 65534;
  last = scalar_glyph(font, "x", 0, &glyph);
  printf("empty key: %d\n", (int)last);
  ok = ok && last == SNR_ERR_INVALID_ARGUMENT;
  char big[40] = {0};
  last = scalar_glyph(font, big, 33, &glyph);
  printf("33-byte key: %d\n", (int)last);
  ok = ok && last == SNR_ERR_INVALID_ARGUMENT;
  printf("glyph map: %llu charged comparisons\n",
         (unsigned long long)document_work);
  ok = ok && document_work <= 22 * (65533 + 2) && work.page == document_work;
  work.page_limit = work.page;
  ok = ok && scalar_glyph(font, "00000", 5, &glyph) == SNR_ERR_LIMIT;
  work.page_limit = SIZE_MAX;
  work.document_limit = document_work;
  ok = ok && scalar_glyph(font, "00000", 5, &glyph) == SNR_ERR_LIMIT;
  font_free(font);
  for (unsigned order = 0; order < 3; order++) {
    font = calloc(1, sizeof *font);
    if (!font) return 1;
    work = (snr_text_work){0, SIZE_MAX, &document_work, UINT64_MAX};
    document_work = 0;
    font->work = &work;
    unsigned maximum = 0;
    for (unsigned i = 0; i < 65533; i++) {
      unsigned k = order == 0   ? i
                   : order == 1 ? 65532 - i
                   : i % 2      ? 65532 - i / 2
                                : i / 2;
      char key[8];
      int length = snprintf(key, sizeof key, "%05u", k);
      uint64_t before = document_work;
      ok = ok && scalar_glyph(font, key, (size_t)length, &glyph) == SNR_OK;
      unsigned comparisons = (unsigned)(document_work - before);
      if (comparisons > maximum) maximum = comparisons;
      ok = ok && comparisons <= 22 && scalar_height(font, font->root) <= 22;
    }
    for (unsigned k = 0; k < 65533; k++) {
      char key[8];
      int length = snprintf(key, sizeof key, "%05u", k);
      uint64_t before = document_work;
      ok = ok && scalar_glyph(font, key, (size_t)length, &glyph) == SNR_OK;
      ok = ok && document_work - before <= 22;
      unsigned expected = order == 0   ? k + 1
                          : order == 1 ? 65533 - k
                          : k <= 32766 ? k * 2 + 1
                                       : (65532 - k) * 2 + 2;
      ok = ok && glyph == expected;
    }
    printf(
        "order %u: 65533 keys, height %u, maximum insertion comparisons %u\n",
        order, scalar_height(font, font->root), maximum);
    font_free(font);
  }
  return ok ? 0 : 1;
}
