/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recognition_order.h"

#define S(x) {x, sizeof(x) - 1}
static const sn_string scripts[] = {
    S("Latin"),
    S("Clean\xC2\x80\xEF\xB7\x90\xED\xA0\x80"
      "Text"),
    S("\x01\x7f\xC2\x9f"),
    S("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"),
    S("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C"),
    S("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"),
    S("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"),
    S("abc \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D 123"),
    S("\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5"
      "\x87"),
    S("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91"
      "\xA7"),
    S("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"),
    S("1\xEF\xB8\x8F\xE2\x83\xA3"),
    S("\xE2\x98\xBA\xEF\xB8\x8E"),
    S("\xE2\x98\xBA\xEF\xB8\x8F"),
    S("\xC2\xA9\xEF\xB8\x8F \xE2\x84\xA2\xEF\xB8\x8F"),
    S("a\xCC\x81\xCC\x82\xCC\x83\xCC\x84\xCC\x85\xCC\x86\xCC\x87"),
    S("\xF0\x93\x80\x80\xF0\x93\x80\x81"),
    S("\xF4\x8F\xBF\xBD"),
    S("\xE2\x80\xAE"
      "evil\xE2\x80\xAC"),
    S("()<>[]{}/%\\ BDC EMC ET BT"),
    S("\x01\x02\x7f\x1b[31m"),
    S("\xFF\xFE"),
    S("\xC0\xAF"),
    S("\xED\xA0\x80"),
    S("ab\0cd"),
    S("\xEF\xBB\xBF"
      "bom"),
    S("\xE0\xBD\x96\xE0\xBD\xBC\xE0\xBD\x91"),
    S("\xE1\xA0\xAE\xE1\xA0\xA4\xE1\xA0\xA9"),
    S("\xF0\x9F\x98\x80"),
    S("\xE2\x9D\xA4"),
};
static const sn_string uris[] = {
    S(" javascript:alert(1)"),
    S("\tjavascript:alert(1)"),
    S("HTTP://upper/"),
    S("http:\\\\evil\\x"),
    S("javascript:http://x"),
    S("http://a\nb"),
    S("http://a\0javascript"),
    S("MAILTO:x@y"),
    S("file:///etc/passwd"),
    S("\xEF\xBB\xBFhttp://bom"),
    S("http:"),
    S("https:javascript:alert(1)"),
    S("hTtPs://mixed"),
    S("smb://host/share"),
    S("http://x/'y'\\z"),
};

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const char* mode = argv[1];
  size_t nscript = sizeof scripts / sizeof *scripts;
  enum { WORD_CAPACITY = sizeof scripts / sizeof *scripts + 1 };
  static_assert(sizeof order_words / sizeof *order_words <= WORD_CAPACITY);
  sn_word* words = calloc(WORD_CAPACITY, sizeof *words);
  if (!words) return 1;
  size_t wc = 0;
  static char longword[65536];
  if (!strcmp(mode, "scripts")) {
    for (size_t i = 0; i < nscript; i++)
      words[wc++] =
          (sn_word){scripts[i], {5, 3 + (double)i * 5.4, 60, 4.5, false}};
  } else if (!strcmp(mode, "long") || !strcmp(mode, "long5000")) {
    for (size_t i = 0; i < sizeof longword; i++)
      longword[i] = (char)('a' + i % 26);
    words[wc++] = (sn_word){
        {longword, !strcmp(mode, "long5000") ? 5000 : sizeof longword},
        {5, 5, 100, 10, false}};
  } else if (!strcmp(mode, "narrow")) {
    words[wc++] = (sn_word){S("a"), {5, 5, 0.01, 5, false}};
  } else if (!strncmp(mode, "size", 4)) {
    double w = 10, h = 5;
    sscanf(mode + 4, "%lfx%lf", &w, &h);
    words[wc++] = (sn_word){S("Word"), {1, 1, w, h, false}};
  } else if (!strcmp(mode, "longfirst")) {
    for (size_t i = 0; i < 600; i++) longword[i] = (char)('a' + i % 26);
    words[wc++] = (sn_word){{longword, 600}, {5, 5, 100, 10, false}};
    words[wc++] = (sn_word){S("Second"), {5, 30, 50, 10, false}};
    words[wc++] = (sn_word){S("Third"), {5, 50, 50, 10, false}};
  } else if (!strcmp(mode, "edge")) {
    words[wc++] = (sn_word){S("ClippedLeft"), {-2, 25, 32, 8, false}};
    words[wc++] = (sn_word){S("LeftEdge"), {0, 10, 30, 8, false}};
    words[wc++] = (sn_word){S("BottomClip"), {10, 152, 30, 20, false}};
    words[wc++] = (sn_word){S("Inside"), {10, 40, 30, 8, false}};
    words[wc++] = (sn_word){S("NearLeft"), {0.01, 60, 30, 8, false}};
  } else if (!strcmp(mode, "big00")) {
    words[wc++] = (sn_word){S("Big"), {0, 0, 118, 150, false}};
  } else if (!strcmp(mode, "bigemoji")) {
    words[wc++] = (sn_word){S("\xF0\x9F\x98\x80 Big"), {1, 1, 100, 100, false}};
  } else if (!strcmp(mode, "emoji1")) {
    words[wc++] = (sn_word){S("\xF0\x9F\x98\x80 Big"), {1, 1, 10, 5, false}};
  } else if (!strcmp(mode, "big")) {
    words[wc++] = (sn_word){S("\xF0\x9F\x98\x80 Big"), {0, 0, 118, 150, false}};
  }
  sn_element el = {.words = words, .word_count = wc};
  if (!strcmp(mode, "order") || !strcmp(mode, "order_single") ||
      !strcmp(mode, "order_clean")) {
    el = order_element;
    if (!strcmp(mode, "order_single")) el.word_count = 11;
    if (!strcmp(mode, "order_clean")) {
      memcpy(words, order_words, sizeof order_words);
      words[0].label = (sn_string)S("Alpha");
      words[2].label = (sn_string)S("Beta");
      words[6].label = (sn_string)S("Gamma");
      words[8].label = (sn_string)S("Delta");
      words[12].label = (sn_string)S("Epsilon");
      words[14].label = (sn_string)S("Zeta");
      el.words = words;
    }
  }
  if (!strncmp(mode, "line_", 5)) {
    const sn_word line_words[] = {
        {S("Alpha"), {8, 10, 15, 6, false}},  {S(" "), {.is_null = true}},
        {S("Beta"), {28, 10, 15, 6, false}},  {S(" "), {.is_null = true}},
        {S("."), {48, 10, 3, 6, false}},      {S("\n"), {.is_null = true}},
        {S("Gamma"), {8, 18, 20, 6, false}},  {S(" "), {.is_null = true}},
        {S("Delta"), {33, 18, 16, 6, false}},
    };
    static_assert(sizeof line_words / sizeof *line_words <= WORD_CAPACITY);
    memcpy(words, line_words, sizeof line_words);
    el.words = words;
    el.word_count = sizeof line_words / sizeof *line_words;
    if (!strncmp(mode, "line_tall", 9)) {
      words[0].label = (sn_string)S("Hello");
      words[2].label = (sn_string)S("world");
      el.word_count = 3;
      if (strcmp(mode, "line_tall_base")) {
        bool same_line = !strcmp(mode, "line_tall_same");
        words[3] = (sn_word){same_line ? (sn_string)S(" ") : (sn_string)S("\n"),
                             {.is_null = true}};
        words[4] = (sn_word){
            S("Tall"),
            {same_line ? 48 : 8, 0, 15, same_line ? 200 : 150, false}};
        el.word_count = 5;
      }
    } else if (!strncmp(mode, "line_dot", 8)) {
      double h = !strcmp(mode, "line_dot03")   ? 0.3
                 : !strcmp(mode, "line_dot06") ? 0.6
                                               : 1;
      words[4].box.y = 13 - h / 2;
      words[4].box.height = h;
    } else if (!strcmp(mode, "line_disjoint")) {
      /* The punctuation ends above the short word: no vertical overlap,
       * although both fit the small-box tolerance. */
      words[2].label = (sn_string)S("a");
      words[2].box.y = 14.6;
      words[2].box.height = 3.35;
      words[4].box.y = 9.95;
      words[4].box.height = 4.03;
    } else if (!strcmp(mode, "line_slivers")) {
      /* Eight tiny boxes must not shrink the two full-height words. */
      enum { SLIVER_WORDS = 10 };
      static_assert(SLIVER_WORDS * 2 <= WORD_CAPACITY);
      el.word_count = 0;
      for (size_t i = 0; i < SLIVER_WORDS; i++) {
        words[el.word_count++] = (sn_word){
            i < 8    ? (sn_string)S(".")
            : i == 8 ? (sn_string)S("Gamma")
                     : (sn_string)S("Delta"),
            {8 + (double)i * 8, i < 8 ? 12.9 : 10, 7, i < 8 ? 0.2 : 6, false}};
        words[el.word_count++] = (sn_word){S(" "), {.is_null = true}};
      }
    } else if (!strcmp(mode, "line_edge")) {
      double bottom = 1872 * 25.4 / 300;
      for (size_t i = 0; i < el.word_count; i++)
        if (!words[i].box.is_null) words[i].box.y += bottom - 24;
      words[8].box.y = bottom - 0.0001;
    } else if (!strcmp(mode, "line_same_x")) {
      for (size_t i = 0; i < 5; i += 2) {
        words[i].label = (sn_string)S("Echo");
        words[i].box.x = 8;
        words[i].box.width = 40;
      }
    }
  }
  sn_recognition rec = {.elements = &el, .element_count = 1};
  sn_link links[64];
  size_t lc = 0;
  memset(links, 0, sizeof links);
  sn_page pages[3] = {{.number = 1, .size = {1404, 1872}, .recognition = &rec},
                      {.number = 2, .size = {1872, 1404}},
                      {.number = 3, .size = {1000, 700}}};
  size_t pc = 1;
  if (!strcmp(mode, "uri")) {
    for (size_t i = 0; i < sizeof uris / sizeof *uris; i++)
      links[lc++] =
          (sn_link){.page_number = 1,
                    .kind = 4,
                    .url = uris[i],
                    .rect = {20, 20 + (double)i * 60, 200, 40, false}};
    static char huge[200000];
    memcpy(huge, "http://", 7);
    memset(huge + 7, 'a', sizeof huge - 7);
    links[lc++] = (sn_link){.page_number = 1,
                            .kind = 4,
                            .url = {huge, sizeof huge},
                            .rect = {20, 1500, 200, 40, false}};
  } else if (!strcmp(mode, "links3")) {
    pc = 3;
    for (int p = 1; p <= 3; p++)
      links[lc++] = (sn_link){.page_number = p,
                              .kind = 0,
                              .has_target_page_number = true,
                              .target_page_number = p == 3 ? 1 : p + 1,
                              .rect = {20, 30, 100, 40, false}};
    links[lc++] = (sn_link){.page_number = 3,
                            .kind = 4,
                            .url = S("https://example.com/"),
                            .rect = {900, 600, 200, 200, false}};
  }
  sn_notebook n = {.equipment = SN_EQUIPMENT_NOMAD,
                   .pages = pages,
                   .page_count = pc,
                   .links = links,
                   .link_count = lc};
  snr_options o = snr_options_default();
  o.scale = 0.25;
  if (argc > 3 && !strcmp(argv[3], "off")) o.recognition = false;
  snr_buffer pdf = {0};
  snr_error e = {0};
  snr_status s = snr_document_pdf(&n, 0, SIZE_MAX, &o, &pdf, &e);
  printf("%s: %s %s size=%zu\n", mode, snr_status_string(s), s ? e.message : "",
         pdf.size);
  if (!s) {
    FILE* f = fopen(argv[2], "wb");
    if (f) {
      fwrite(pdf.bytes, 1, pdf.size, f);
      fclose(f);
    }
  }
  snr_buffer_free(&pdf);
  free(words);
  return s ? 1 : 0;
}
