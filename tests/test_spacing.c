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
/* Compare the actual bands, including the tall run, with a control element
 * built from the same ordinary words and separators. */
static int tall_minority(const double* heights, size_t ordinary,
                         double expected, size_t tall_count, bool shared) {
  sn_word words[8 + 2 + 5];
  sn_rect reference[8 + 1];
  size_t n = 0;
  for (size_t i = 0; i < ordinary; i++) {
    if (i == 3) words[n++] = (sn_word){{"\n", 1}, {.is_null = true}};
    double h = heights[i];
    words[n++] =
        (sn_word){h == 0.2 ? (sn_string){".", 1} : (sn_string){"word", 4},
                  {5 + 10 * (double)(i < 3 ? i : i - 3),
                   (i < 3 ? 30 : 75) - h / 2, 8, h, false}};
  }
  sn_element el = {.words = words, .word_count = n};
  snr_text_line control = snr_text_line_create(&el, 120, 160);
  size_t control_count = n;
  for (size_t i = 0; i < control_count; i++) {
    reference[i] = words[i].box;
    if (!reference[i].is_null)
      snr_text_line_box(&control, &el, i, 120, 160, &reference[i]);
  }

  words[n++] = (sn_word){shared ? (sn_string){" ", 1} : (sn_string){"\n", 1},
                         {.is_null = true}};
  for (size_t i = 0; i < tall_count; i++) {
    double h = expected < 1 ? (i ? 160 : 1e300) : 150 - (double)i * 5;
    words[n++] = (sn_word){{"Tall", 4}, {55 + (double)i * 12, 0, 8, h, false}};
  }
  el.word_count = n;
  snr_text_line line = snr_text_line_create(&el, 120, 160);
  bool ok = fabs(control.height - expected) < 1e-9 &&
            fabs(line.height - control.height) < 1e-9;
  for (size_t i = 0; i < el.word_count; i++) {
    sn_rect r = words[i].box;
    if (!snr_box_clip(&r, 120, 160)) continue;
    double max_width = r.width + r.height * 0.24;
    snr_text_line_box(&line, &el, i, 120, 160, &r);
    ok &= line.valid && r.height > 0 && r.y >= 0 && r.y + r.height <= 160 &&
          r.height <= control.height + 1e-9 && r.width > 0 &&
          r.width <= max_width;
    if (i < control_count) {
      ok &= fabs(r.height - reference[i].height) < 1e-9 &&
            fabs(r.y - reference[i].y) <=
                (shared ? reference[i].height * 0.25 : 1e-9);
    }
  }
  if (!ok)
    fprintf(stderr,
            "%zu tall, %zu ordinary, shared=%d: height %.9g, control %.9g\n",
            tall_count, ordinary, shared, line.height, control.height);
  return !ok;
}
/* Reference from HEAD (032b704). Keep height and band selection independent
 * of the production cutoffs. */
static snr_text_line head_line_create(const sn_element* el, double width,
                                      double height) {
  snr_text_line line = {0};
  size_t count = 0;
  double tallest = 0, runner_up = 0;
  for (size_t j = 0; j < el->word_count; j++) {
    const sn_word* word = el->words + j;
    if (word->box.is_null && word->label.data && word->label.length &&
        (snr_text_break(word->label) || word->label.data[0] == ' ' ||
         word->label.data[0] == '\t'))
      line.explicit_lines = true;
    sn_rect r = word->box;
    if (snr_box_clip(&r, width, height)) {
      if (r.height > tallest) {
        runner_up = tallest;
        tallest = r.height;
      } else {
        runner_up = fmax(runner_up, r.height);
      }
      count++;
    }
  }
  if (count) {
    /* Trim an isolated upper outlier before it can raise the quarter-mean
     * threshold above every ordinary word. Two tall words still support the
     * height when the remaining boxes are mostly punctuation or slivers. */
    double ceiling =
        count > 1 && tallest > runner_up * 1.4 ? runner_up : tallest;
    count = 0;
    for (size_t j = 0; j < el->word_count; j++) {
      sn_rect r = el->words[j].box;
      if (snr_box_clip(&r, width, height) && r.height <= ceiling) {
        line.height += r.height;
        count++;
      }
    }
    double mean = line.height / (double)count;
    double minimum = height;
    double sum = 0;
    count = 0;
    for (size_t j = 0; j < el->word_count; j++) {
      sn_rect r = el->words[j].box;
      if (snr_box_clip(&r, width, height) && r.height <= ceiling &&
          r.height >= mean * 0.25) {
        minimum = fmin(minimum, r.height);
        sum += r.height;
        count++;
      }
    }
    mean = sum / (double)count;
    line.height = fmax(mean * 0.5, fmin(mean, minimum * 1.4));
  }
  return line;
}
/* Positive finite IEEE doubles sort by their unsigned representation. Select
 * the upper median with eight radix passes: bounded linear work, no allocation
 * or input mutation, even for adversarial box sequences. */
static double head_median_height(const sn_element* el, size_t begin, size_t end,
                                 size_t count, double width, double height) {
  uint64_t prefix = 0, mask = 0;
  size_t rank = count / 2;
  for (int shift = 56; shift >= 0; shift -= 8) {
    size_t bins[256] = {0};
    for (size_t i = begin; i < end; i++) {
      sn_rect r = el->words[i].box;
      if (!snr_box_clip(&r, width, height)) continue;
      uint64_t bits;
      static_assert(sizeof bits == sizeof r.height);
      memcpy(&bits, &r.height, sizeof bits);
      if ((bits & mask) == prefix) bins[(bits >> shift) & 255]++;
    }
    size_t bin = 0;
    while (bin < 255 && rank >= bins[bin]) rank -= bins[bin++];
    prefix |= (uint64_t)bin << shift;
    mask |= UINT64_C(255) << shift;
  }
  double median;
  memcpy(&median, &prefix, sizeof median);
  return median;
}
static void head_line_box(snr_text_line* line, const sn_element* el,
                          size_t index, double width, double height,
                          sn_rect* box) {
  if (!line->explicit_lines) return;
  if (index >= line->end) {
    line->valid = false;
    size_t end = index;
    double centers = 0, top = 0, bottom = height;
    double center_top = 0, center_bottom = height;
    double small_top = 0, small_bottom = height;
    double previous_x = -1;
    size_t count = 0;
    for (; end < el->word_count; end++) {
      const sn_word* word = el->words + end;
      if (word->box.is_null && snr_text_break(word->label)) break;
      sn_rect r = word->box;
      if (!snr_box_clip(&r, width, height)) continue;
      /* A word may itself have a wide box crossing a physical wrap. Start a
       * new physical band on a leftward move, retaining the stored sequence. */
      if (previous_x >= 0 && r.x < previous_x) break;
      previous_x = r.x;
      top = fmax(top, r.y - r.height * 0.24);
      bottom = fmin(bottom, r.y + r.height * 1.24);
      centers += r.y + r.height / 2;
      count++;
    }
    line->normal_height =
        count ? head_median_height(el, index, end, count, width, height) : 0;
    for (size_t j = index; j < end; j++) {
      const sn_word* word = el->words + j;
      sn_rect r = word->box;
      /* Small handwriting and punctuation use the tolerance window above.
       * Page-edge slivers cannot constrain the centre of a full band. */
      if (snr_box_clip(&r, width, height) && word->box.y >= 0 &&
          word->box.y <= height - word->box.height) {
        if (r.height >= line->normal_height) {
          center_top = fmax(center_top, r.y + r.height * 0.01);
          center_bottom = fmin(center_bottom, r.y + r.height * 0.99);
        } else {
          small_top = fmax(small_top, r.y);
          small_bottom = fmin(small_bottom, r.y + r.height);
        }
      }
    }
    /* Geometric extractors can return the whole line's search rectangle.
     * With disjoint centre windows no common centre can satisfy all words;
     * use the existing local spacing fallback for this physical run. */
    line->valid = count != 0 && center_top <= center_bottom;
    if (count) {
      double h = line->height;
      /* If small boxes straddle the normal-word overlap, grow the band just
       * enough for their quarter-line centre tolerance. Bound this adjustment
       * by the run median so outliers cannot create arbitrarily tall bands.
       * Leave a small margin for PDF coordinate rounding. */
      double clearance =
          fmax((small_top - small_bottom) / 2,
               fmax(small_top - center_bottom, center_top - small_bottom));
      if (line->valid)
        h = fmax(h, fmin(line->normal_height, clearance / 0.249));
      /* A page fit must not override the strict centre window. Shrink the
       * band if no centre in that window can fit its original height. */
      if (line->valid)
        h = fmin(h, fmin(height, 2 * fmin(center_bottom, height - center_top)));
      double center = centers / (double)count;
      if (bottom - top >= h)
        center = fmax(top + h / 2, fmin(center, bottom - h / 2));
      if (center_top <= center_bottom)
        center = fmax(center_top, fmin(center, center_bottom));
      /* Prefer a centre within every small box's search tolerance too, even
       * when the entire band cannot fit their expanded vertical extent.
       * Small boxes never invalidate the shared band. */
      double local_top = fmax(center_top, small_top - h * 0.249);
      double local_bottom = fmin(center_bottom, small_bottom + h * 0.249);
      if (local_top <= local_bottom)
        center = fmax(local_top, fmin(center, local_bottom));
      if (line->valid)
        center = fmax(fmax(h / 2, center_top),
                      fmin(center, fmin(height - h / 2, center_bottom)));
      line->top = fmax(0, center - h / 2);
      line->bottom = fmin(height, center + h / 2);
    }
    line->end = end;
  }
  if (line->valid && line->bottom - line->top >= 1e-6) {
    double tolerance = box->height * 0.24;
    box->y = line->top;
    box->height = line->bottom - line->top;
    /* Share the available width of a coincident run. Scan each run once,
     * including unboxed separators, so equal x positions never overlay text. */
    if (index >= line->coincident_end) {
      size_t end = index + 1, count = 1;
      double right = box->x + box->width;
      for (; end < line->end; end++) {
        sn_rect r = el->words[end].box;
        if (!snr_box_clip(&r, width, height)) continue;
        if (r.x != box->x) {
          right = fmin(right, r.x);
          break;
        }
        right = fmin(right, r.x + r.width);
        count++;
      }
      line->coincident_end = count > 1 ? end : index + 1;
      line->coincident_slot = 0;
      line->coincident_x = box->x;
      line->coincident_pitch = count > 1 ? (right - box->x) / (double)count : 0;
    }
    if (line->coincident_pitch > 0) {
      box->x = line->coincident_x +
               (double)line->coincident_slot++ * line->coincident_pitch;
      double gap = fmin(box->height * 0.5, line->coincident_pitch * 0.25);
      box->width = fmin(box->width, line->coincident_pitch - gap);
      return;
    }
    if (index + 1 < el->word_count) {
      size_t next = index + 1;
      while (next < line->end && el->words[next].box.is_null) next++;
      if (next < line->end) {
        sn_rect r = el->words[next].box;
        if (snr_box_clip(&r, width, height) && r.x > box->x) {
          double gap = box->height * 0.5;
          box->width = fmax(box->width * 0.05,
                            fmin(box->width + tolerance, r.x - box->x - gap));
        }
      }
    }
  }
}

static int height_examples(void) {
  const struct {
    size_t words, marks;
    double word, mark, page, expected;
  } cases[] = {
      {3, 4, 12, 1.1, 160, 12},
      {3, 4, 12, 1, 160, 12},
      {2, 6, 25, 0.3, 160, 25},
      {2, 6, 30, 0.3, 210, 30},
      {3, 4, 6, 4, 160, 34.0 / 7},
      {4, 5, 7.1, 5, 160, 53.4 / 9},
      {4, 5, 10, 6, 160, 70.0 / 9},
      /* Exactly half a page, a tied group, and a gap of exactly 1.4. */
      {2, 3, 80, 6, 160, 80},
      {3, 3, 150, 6, 160, 150},
      {2, 3, 140, 100, 160, 116}};
  sn_word words[21] = {0};
  sn_element el = {.words = words};
  int failed = 0;
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
    el.word_count = cases[i].words + cases[i].marks;
    for (size_t j = 0; j < el.word_count; j++) {
      double h = j < cases[i].words ? cases[i].word : cases[i].mark;
      words[j].box = (sn_rect){0, 0, 2, h, false};
    }
    double head = head_line_create(&el, 120, cases[i].page).height;
    double actual = snr_text_line_create(&el, 120, cases[i].page).height;
    if (fabs(head - cases[i].expected) > 1e-9 || actual != head) {
      fprintf(stderr, "%zu x %g and %zu x %g on %g: HEAD %.12g, got %.12g\n",
              cases[i].words, cases[i].word, cases[i].marks, cases[i].mark,
              cases[i].page, head, actual);
      failed++;
    }
  }
  /* Known limit: a graded tail has no gap to cut, even past half a page. */
  el.word_count = 21;
  double h = 6;
  for (size_t i = 0; i < el.word_count; i++) {
    if (i >= 11) h *= 1.39;
    words[i].box.height = h;
    words[i].box.width = 2;
  }
  double head = head_line_create(&el, 120, 300).height;
  double actual = snr_text_line_create(&el, 120, 300).height;
  if (actual != head || fabs(actual - 27.72) > 0.01) {
    fprintf(stderr, "graded tail: HEAD %.12g, got %.12g\n", head, actual);
    failed++;
  }
  return failed;
}

static uint32_t next_random(uint32_t* state) {
  *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
  return *state;
}

/* Walk both elements in model order, including injected words so the mixed
 * line's cached run and coincident slots advance as they do in the renderer. */
static int compare_bands(const char* name, size_t sample,
                         const sn_element* base, const sn_element* mixed,
                         const bool* injected, double page) {
  snr_text_line head = head_line_create(base, 120, page);
  snr_text_line control = snr_text_line_create(base, 120, page);
  snr_text_line line = snr_text_line_create(mixed, 120, page);
  if (head.height != control.height || line.height != control.height) {
    fprintf(stderr, "%s %zu: height HEAD %a, control %a, mixed %a\n", name,
            sample, head.height, control.height, line.height);
    return 1;
  }
  size_t j = 0;
  for (size_t i = 0; i < mixed->word_count; i++) {
    sn_rect actual = mixed->words[i].box;
    if (snr_box_clip(&actual, 120, page))
      snr_text_line_box(&line, mixed, i, 120, page, &actual);
    if (injected[i]) continue;
    size_t word = j++;
    sn_rect expected = base->words[word].box;
    if (!snr_box_clip(&expected, 120, page)) continue;
    sn_rect ordinary = expected;
    head_line_box(&head, base, word, 120, page, &expected);
    snr_text_line_box(&control, base, word, 120, page, &ordinary);
    /* Insertions can change the next x or the size of a coincident group.
     * Horizontal fitting is identical to HEAD on the original element. */
    if (control.valid != head.valid || ordinary.x != expected.x ||
        ordinary.width != expected.width || ordinary.y != expected.y ||
        ordinary.height != expected.height || line.valid != control.valid ||
        actual.y != ordinary.y || actual.height != ordinary.height) {
      fprintf(stderr,
              "%s %zu word %zu: HEAD %d [%g, %g, %g, %g], "
              "control %d [%g, %g, %g, %g], mixed %d [y=%g, h=%g]\n",
              name, sample, word, head.valid, expected.x, expected.y,
              expected.width, expected.height, control.valid, ordinary.x,
              ordinary.y, ordinary.width, ordinary.height, line.valid, actual.y,
              actual.height);
      return 1;
    }
  }
  return 0;
}

static int lone_outlier_median(void) {
  /* The retained 21.27 mm box sets the upper median to 5.863 mm. Removing
   * it from the run makes two disjoint words strict and loses the band. */
  sn_word base[] = {{{"a", 1}, {6.31, 10.45, 2, 1.475, false}},
                    {{"b", 1}, {14.58, 7.08, 2, 6.064, false}},
                    {{"c", 1}, {19.56, 13.54, 2, 4.751, false}},
                    {{"d", 1}, {23.05, 9.51, 2, 6.967, false}},
                    {{" ", 1}, {.is_null = true}},
                    {{"e", 1}, {24.24, 12.96, 2, 1.422, false}},
                    {{"f", 1}, {26.88, 8.38, 2, 4.429, false}},
                    {{"g", 1}, {28.50, 2.19, 2, 21.27, false}},
                    {{"h", 1}, {33.55, 10.41, 2, 5.863, false}}};
  enum { WORDS = sizeof base / sizeof *base };
  sn_word mixed[2 + WORDS] = {{{"Tall", 4}, {1, 0, 2, 251, false}},
                              {{"Tall", 4}, {4, 0, 2, 260, false}}};
  const bool injected[2 + WORDS] = {true, true};
  memcpy(mixed + 2, base, sizeof base);
  sn_element control = {.words = base, .word_count = WORDS};
  sn_element el = {.words = mixed, .word_count = 2 + WORDS};
  return compare_bands("lone outlier median", 0, &control, &el, injected,
                       305.99);
}

static int height_and_band_sweep(void) {
  uint32_t state = UINT32_C(0x51a7b0);
  for (size_t sample = 0; sample < 100000; sample++) {
    sn_word base[16 + 1] = {0}, mixed[31 + 1] = {0};
    bool injected[31 + 1] = {0};
    double page = 64 + next_random(&state) % 1024;
    size_t n = 2 + next_random(&state) % 15;
    size_t tall = 1 + next_random(&state) % (n - 1);
    for (size_t i = 0; i < n; i++) {
      /* Each distribution has no removable group before the injection. */
      unsigned k = next_random(&state) >> 16;
      double h;
      switch (sample % 5) {
        case 0:
          h = page * (k % 2 ? 0.002 : 0.12);
          break;
        case 1:
          h = page * (1 + k % 500) / 1000;
          break;
        case 2:
        case 3:
          /* A cluster above half the page, alone or with a shorter group
           * that ties it or loses by one box. */
          h = page * (520 + k % 81) / 1000;
          if (sample % 5 == 3 && i < n / 2) h /= 8;
          break;
        default:
          /* This group's maximum exceeds half the page, but its minimum
           * is exactly half. Even a strict minority must be retained. */
          h = page * (1 + k % 200) / 1000;
          if (i == 0) h = page / 2;
          if (i == 1) h = page * 0.60;
          break;
      }
      /* Some heights are the visible part of a box clipped at the top. */
      double y =
          k % 3 ? page * (0.30 + (double)(k % 5) / 256) - h / 2 : -page / 16;
      if (h == page / 2) y = 0;
      base[i].box = (sn_rect){0, y, 2, h - fmin(0, y), false};
    }
    /* Interleave the minority while preserving the addition order of base.
     * 0.86 page is more than 1.4 times the largest base height (0.60 page).
     * With smaller bases, also sample two separated page-spanning groups. */
    size_t ordinary = 0, added = 0;
    for (size_t i = 0; i < n + tall; i++) {
      double x = 1 + 3 * (double)(sample % 2 ? i : i / 2);
      if (next_random(&state) % (n + tall - i) < tall - added) {
        double h = page * (860 + next_random(&state) % 241) / 1000;
        if (sample % 5 == 0 && added % 2) h = page * 0.55;
        mixed[i].box = (sn_rect){x, 0, 2, h, false};
        injected[i] = true;
        added++;
      } else {
        base[ordinary].box.x = x;
        mixed[i] = base[ordinary++];
      }
    }
    base[n] = mixed[n + tall] = (sn_word){{" ", 1}, {.is_null = true}};
    sn_element control = {.words = base, .word_count = n + 1};
    sn_element el = {.words = mixed, .word_count = n + tall + 1};
    if (compare_bands("sweep", sample, &control, &el, injected, page)) return 1;
  }
  return 0;
}

static int mixed_run_edges(void) {
  /* T is a tall box. The wide boxes in the third case span an x reset.
   * The clipped boxes in the last case can only move the expanded window. */
  const struct {
    const char* order;
    size_t wrap;
    double word_y, tall_y, tall_height;
  } cases[] = {{"aaaaTT", 6, 152, 0, 150},
               {"TTaaaa", 6, 152, 0, 150},
               {"aaTTaa", 3, 152, 0, 150},
               {"aaaaTT", 6, 119, -1, 101}};
  int failed = 0;
  for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
    sn_word words[7] = {0}, base[5] = {0};
    size_t n = 0;
    for (size_t i = 0; i < 6; i++) {
      bool tall = cases[k].order[i] == 'T';
      bool wrapped = i >= cases[k].wrap;
      double x =
          wrapped ? 1 + 7 * (double)(i - cases[k].wrap) : 2 + 15 * (double)i;
      double y = tall ? cases[k].tall_y : cases[k].word_y + wrapped;
      words[i].box = (sn_rect){x, y, tall ? 60 : 8,
                               tall ? cases[k].tall_height : 6, false};
      if (!tall) base[n++] = words[i];
    }
    base[n] = words[6] = (sn_word){{"\n", 1}, {.is_null = true}};
    sn_element el = {.words = words, .word_count = 7};
    sn_element control = {.words = base, .word_count = n + 1};
    snr_text_line line = snr_text_line_create(&el, 120, 160);
    snr_text_line reference = snr_text_line_create(&control, 120, 160);
    n = 0;
    for (size_t i = 0; i < 6; i++) {
      sn_rect r = words[i].box;
      snr_box_clip(&r, 120, 160);
      snr_text_line_box(&line, &el, i, 120, 160, &r);
      if (cases[k].order[i] == 'T') continue;
      sn_rect expected = base[n].box;
      snr_text_line_box(&reference, &control, n++, 120, 160, &expected);
      if (!reference.valid || !line.valid || r.y != expected.y ||
          r.height != expected.height || r.height != 6) {
        fprintf(stderr,
                "%s word %zu: valid %d, band [%g, %g], control [%g, %g]\n",
                cases[k].order, i, line.valid, r.y, r.height, expected.y,
                expected.height);
        failed++;
        break;
      }
    }
  }
  return failed;
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
  failed += height_examples();
  failed += lone_outlier_median();
  failed += height_and_band_sweep();
  failed += mixed_run_edges();
  const double ordinary[] = {6, 6, 6, 6, 6, 6};
  const double uneven[] = {5, 5, 5, 5.5, 7, 9};
  const double dotted[] = {0.2, 0.2, 0.2, 0.2, 0.2, 0.2, 6, 6};
  const double tiny[] = {2e-6, 2e-6, 2e-6, 2e-6, 2e-6, 2e-6};
  failed += tall_minority(ordinary, 6, 6, 2, false);
  failed += tall_minority(ordinary, 6, 6, 2, true);
  failed += tall_minority(ordinary, 6, 6, 3, false);
  failed += tall_minority(uneven, 6, 36.5 / 6, 5, false);
  failed += tall_minority(dotted, 8, 6, 2, false);
  failed += tall_minority(tiny, 6, 2e-6, 2, false);
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
