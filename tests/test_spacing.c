/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <stdio.h>

#include "../src/snr_text.h"
static int exercise(size_t count, bool same) {
  sn_word* words = calloc(count, sizeof *words);
  if (!words) return 1;
  for (size_t i = 0; i < count; i++)
    words[i] = (sn_word){{"a", 1},
                         {same ? 10 : 1 + (double)i * 110 / (double)count, 50,
                          110.0 / (double)count + 0.001, 5, false}};
  sn_element el = {.words = words, .word_count = count};
  sn_recognition rec = {.elements = &el, .element_count = 1};
  uint64_t total = 0;
  snr_text_work work = {0, SIZE_MAX, &total, UINT64_MAX};
  snr_spacing spacing;
  snr_status status = snr_spacing_create(&rec, 120, 160, &work, &spacing);
  for (size_t i = 0; !status && i < count; i++) {
    sn_rect r = words[i].box;
    status = snr_word_spacing(&r, &spacing);
  }
  bool ok = !status && total <= 52 * count && work.page == total &&
            spacing.count == (same ? 1 : count);
  printf("%zu %s boxes: %llu charged visits\n", count,
         same ? "identical" : "row", (unsigned long long)total);
  work.page_limit = work.page;
  sn_rect r = words[0].box;
  ok = ok && snr_word_spacing(&r, &spacing) == SNR_ERR_LIMIT;
  work.page_limit = SIZE_MAX;
  work.document_limit = total;
  ok = ok && snr_word_spacing(&r, &spacing) == SNR_ERR_LIMIT;
  free(spacing.boxes);
  free(words);
  return ok ? 0 : 1;
}
int main(void) {
  const char dirty[] =
      "a\xc2\xad\xe2\x80\x8b\xe2\x80\x8c\xe2\x80\x8d"
      "\xe2\x81\xa0\xef\xbb\xbf"
      "b\xe2\x80\xae";
  snr_text clean = snr_recognition_text((sn_string){dirty, sizeof dirty - 1});
  bool clean_ok = !clean.status && clean.length == 5 &&
                  !memcmp(clean.data, "ab\xe2\x80\xae", 5);
  free(clean.data);
  if (!clean_ok) return 1;
  int failed =
      exercise(40000, false) + exercise(160000, false) + exercise(160000, true);
  sn_word words[] = {{{"a", 1}, {-10, 0, 11, 2, false}},
                     {{"a", 1}, {500, 0, 10000, 10000, false}},
                     {{"\xff", 1}, {0, 0, 10, 100, false}}};
  sn_element el = {.words = words, .word_count = 3};
  sn_recognition rec = {.elements = &el, .element_count = 1};
  uint64_t total = 0;
  snr_text_work work = {0, SIZE_MAX, &total, UINT64_MAX};
  snr_spacing spacing;
  snr_status s = snr_spacing_create(&rec, 120, 160, &work, &spacing);
  if (s || spacing.count != 1 || spacing.band_height != 0.375 ||
      spacing.boxes[0].box.width != 1)
    failed++;
  free(spacing.boxes);
  sn_word isolated[] = {{{"a", 1}, {1, 10, 5, 10, false}},
                        {{"b", 1}, {100, 14.5, 5, 1, false}}};
  el.words = isolated;
  el.word_count = 2;
  s = snr_spacing_create(&rec, 120, 160, &work, &spacing);
  sn_rect box = isolated[0].box;
  if (s || snr_word_spacing(&box, &spacing) || box.height != 10) failed++;
  free(spacing.boxes);
  /* A tall word elsewhere must not push a top/bottom-edge line off-page. */
  sn_word bands[] = {{{"Edge", 4}, {1, 0, 10, 0.1, false}},
                     {{"\n", 1}, {.is_null = true}},
                     {{"Tall", 4}, {1, 20, 10, 20, false}},
                     {{"\n", 1}, {.is_null = true}},
                     {{"End", 3}, {1, 159.9999, 10, 6, false}}};
  el.words = bands;
  el.word_count = sizeof bands / sizeof *bands;
  snr_text_line line = snr_text_line_create(&el, 120, 160);
  if (fabs(line.height - 0.1) > 1e-9) failed++;
  for (size_t i = 0; i < el.word_count; i += 2) {
    box = bands[i].box;
    if (!snr_box_clip(&box, 120, 160)) return 1;
    snr_text_line_box(&line, &el, i, 120, 160, &box);
    if (box.y < 0 || box.height <= 0 || box.y + box.height > 160) failed++;
  }
  sn_word slivers[10];
  for (size_t i = 0; i < 10; i++)
    slivers[i] = (sn_word){{".", 1},
                           {1 + (double)i * 10, 10, 5, i < 8 ? 0.2 : 6, false}};
  el.words = slivers;
  el.word_count = sizeof slivers / sizeof *slivers;
  line = snr_text_line_create(&el, 120, 160);
  if (fabs(line.height - 6) > 1e-9) failed++;
  if (fabs(snr_text_median_height(&el, 0, el.word_count, el.word_count, 120,
                                  160) -
           0.2) > 1e-9)
    failed++;
  const double heights[] = {6, 2, 9, 4, 7};
  for (size_t i = 0; i < sizeof heights / sizeof *heights; i++)
    slivers[i].box.height = heights[i];
  if (snr_text_median_height(&el, 0, 5, 5, 120, 160) != 6 ||
      snr_text_median_height(&el, 0, 4, 4, 120, 160) != 6)
    failed++;
  /* Truly disjoint full-height words must still use the local fallback. */
  sn_word disjoint[] = {{{"A", 1}, {1, 10, 10, 6, false}},
                        {{" ", 1}, {.is_null = true}},
                        {{"B", 1}, {20, 20, 10, 6, false}}};
  el.words = disjoint;
  el.word_count = sizeof disjoint / sizeof *disjoint;
  line = snr_text_line_create(&el, 120, 160);
  box = disjoint[0].box;
  snr_text_line_box(&line, &el, 0, 120, 160, &box);
  if (line.valid || box.y != 10 || box.height != 6) failed++;
  /* A capped element height can be shorter than both disjoint small boxes.
   * The run median must still exclude the short word from the intersection. */
  sn_word capped[] = {{{"Alpha", 5}, {8, 10, 15, 6, false}},
                      {{" ", 1}, {.is_null = true}},
                      {{"a", 1}, {28, 14.6, 3, 3.35, false}},
                      {{".", 1}, {48, 9.95, 3, 4.03, false}},
                      {{"\n", 1}, {.is_null = true}},
                      {{"Short", 5}, {8, 20, 15, 2, false}}};
  el.words = capped;
  el.word_count = sizeof capped / sizeof *capped;
  line = snr_text_line_create(&el, 120, 160);
  box = capped[0].box;
  snr_text_line_box(&line, &el, 0, 120, 160, &box);
  if (fabs(line.height - 2.8) > 1e-9 ||
      fabs(line.normal_height - 4.03) > 1e-9 || !line.valid)
    failed++;
  /* Two small boxes can require more than the capped font band's tolerance.
   * Retain a shared band and satisfy the original-box centre assertion. */
  sn_word quotes[] = {{{"A", 1}, {8, 10, 10, 8, false}},
                      {{" ", 1}, {.is_null = true}},
                      {{"'", 1}, {20, 10, 2, 2, false}},
                      {{".", 1}, {25, 15, 2, 2, false}},
                      {{"B", 1}, {30, 10, 10, 8, false}}};
  el.words = quotes;
  el.word_count = sizeof quotes / sizeof *quotes;
  line = snr_text_line_create(&el, 120, 160);
  for (size_t i = 0; i < el.word_count; i++) {
    sn_rect original = quotes[i].box;
    if (original.is_null) continue;
    box = original;
    snr_text_line_box(&line, &el, i, 120, 160, &box);
    double center = box.y + box.height / 2;
    double margin =
        original.height < line.normal_height ? box.height * 0.25 : 0;
    if (!line.valid || box.height <= line.height ||
        box.height > line.normal_height || center < original.y - margin ||
        center > original.y + original.height + margin)
      failed++;
  }
  /* A lone tall box, on this run or another line, must not change the
   * ordinary words' band or horizontal fitting. */
  sn_word tall[] = {{{"Hello", 5}, {8, 10, 15, 6, false}},
                    {{" ", 1}, {.is_null = true}},
                    {{"world", 5}, {28, 10, 15, 6, false}},
                    {{"\n", 1}, {.is_null = true}},
                    {{"Tall", 4}, {48, 0, 15, 150, false}}};
  el.words = tall;
  el.word_count = 3;
  line = snr_text_line_create(&el, 120, 160);
  sn_rect reference = tall[0].box;
  snr_text_line_box(&line, &el, 0, 120, 160, &reference);
  for (size_t same = 0; same < 2; same++) {
    el.word_count = sizeof tall / sizeof *tall;
    tall[3].label = same ? (sn_string){" ", 1} : (sn_string){"\n", 1};
    tall[4].box.height = same ? 200 : 150;
    line = snr_text_line_create(&el, 120, 160);
    box = tall[0].box;
    snr_text_line_box(&line, &el, 0, 120, 160, &box);
    double center = box.y + box.height / 2;
    if (!line.valid || line.height != 6 || (!same && box.y != reference.y) ||
        box.height != reference.height || box.width != reference.width ||
        center < 10 || center > 16)
      failed++;
  }
  /* Neither a separate huge box nor one sharing the run may turn a tiny
   * valid sliver into a page-height band or collapse its horizontal width. */
  sn_word huge[] = {{{"Sliver", 6}, {8, 10, 15, 2e-6, false}},
                    {{"\n", 1}, {.is_null = true}},
                    {{"Huge", 4}, {28, 0, 15, 1e300, false}}};
  el.words = huge;
  el.word_count = sizeof huge / sizeof *huge;
  for (size_t same = 0; same < 2; same++) {
    huge[1].label = same ? (sn_string){" ", 1} : (sn_string){"\n", 1};
    line = snr_text_line_create(&el, 120, 160);
    box = huge[0].box;
    snr_text_line_box(&line, &el, 0, 120, 160, &box);
    double center = box.y + box.height / 2;
    if (!line.valid || box.height > 2.1e-6 || box.width < 15 ||
        box.width > 15 + 2e-6 * 0.25 || center < 10 || center > 10 + 2e-6)
      failed++;
  }
  /* Exercise the page fit independently of the element estimator. A very
   * large requested band must shrink around the run's strict centre window. */
  sn_word edge[] = {{{"Edge", 4}, {1, 0, 10, 0.1, false}},
                    {{"\n", 1}, {.is_null = true}}};
  el.words = edge;
  el.word_count = sizeof edge / sizeof *edge;
  for (size_t bottom = 0; bottom < 2; bottom++) {
    edge[0].box.y = bottom ? 159.9 : 0;
    line = snr_text_line_create(&el, 120, 160);
    line.height = 150;
    box = edge[0].box;
    snr_text_line_box(&line, &el, 0, 120, 160, &box);
    double center = box.y + box.height / 2;
    if (!line.valid || center < edge[0].box.y || center > edge[0].box.y + 0.1 ||
        box.y < 0 || box.y + box.height > 160)
      failed++;
  }
  return failed ? 1 : 0;
}
