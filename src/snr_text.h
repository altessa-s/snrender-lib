/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#ifndef SNR_TEXT_H
#define SNR_TEXT_H
#include <math.h>
#include <snrender/snrender.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  char* data;
  size_t length;
  snr_status status;
} snr_text;
/* Clip before arithmetic in device coordinates so hostile magnitudes never
 * reach platform transforms or attribute formatters. */
static inline bool snr_box_clip(sn_rect* r, double width, double height) {
  if (r->is_null || !isfinite(r->x) || !isfinite(r->y) || !isfinite(r->width) ||
      !isfinite(r->height) || r->width <= 0 || r->height <= 0 ||
      r->x >= width || r->y >= height || r->x <= -r->width ||
      r->y <= -r->height) {
    return false;
  }
  double right = fmin(width, r->x + r->width);
  double bottom = fmin(height, r->y + r->height);
  r->x = fmax(0, r->x);
  r->y = fmax(0, r->y);
  r->width = right - r->x;
  r->height = bottom - r->y;
  if (r->width < 1e-6 || r->height < 1e-6) {
    return false;
  }
  return true;
}
/* Sanitize before either platform sees text so extraction has the same scalar
 * policy even when its decoder accepts UTF-8 encodings of surrogates. */
static inline snr_text snr_recognition_text(sn_string input) {
  snr_text result = {0};
  if (!input.data || input.length > 65536) return result;
  char* bytes = malloc(input.length + 1);
  if (!bytes) return (snr_text){.status = SNR_ERR_OUT_OF_MEMORY};
  size_t used = 0;
  for (size_t i = 0; i < input.length;) {
    size_t start = i;
    uint32_t c = (uint8_t)input.data[i++];
    size_t extra = c < 0x80                 ? 0
                   : c >= 0xc2 && c <= 0xdf ? 1
                   : c >= 0xe0 && c <= 0xef ? 2
                   : c >= 0xf0 && c <= 0xf4 ? 3
                                            : 4;
    if (extra == 4 || extra > input.length - i) goto invalid;
    if (extra) c &= (1u << (6 - extra)) - 1;
    for (size_t j = 0; j < extra; j++) {
      uint8_t next = (uint8_t)input.data[i++];
      if ((next & 0xc0) != 0x80) goto invalid;
      c = (c << 6) | (next & 0x3f);
    }
    if ((extra == 1 && c < 0x80) || (extra == 2 && c < 0x800) ||
        (extra == 3 && c < 0x10000) || c > 0x10ffff)
      goto invalid;
    /* Remove search-breaking discretionary/zero-width formatting only.
     * Keep bidi controls, Arabic signs, interlinear annotations, Egyptian
     * controls and tags: the remaining Cf characters can carry meaning.
     * In particular, variation selectors (Mn) are not formatting noise. */
    if (c == 0x00ad || (c >= 0x200b && c <= 0x200d) || c == 0x2060 ||
        c == 0xfeff)
      continue;
    if (c < 0x20 || (c >= 0x7f && c <= 0x9f) || (c >= 0xd800 && c <= 0xdfff) ||
        (c >= 0xfdd0 && c <= 0xfdef) || (c & 0xffff) >= 0xfffe)
      continue;
    memcpy(bytes + used, input.data + start, i - start);
    used += i - start;
  }
  bytes[used] = 0;
  return (snr_text){.data = bytes, .length = used};
invalid:
  free(bytes);
  return result;
}

/* Recognition separators are words without boxes. Consume their line breaks
 * in model order. A common mean height and baseline keep an uneven handwritten
 * line together in geometric extractors; individual x positions and widths
 * keep word selections local. Exclude a lone tallest box more than 1.4 times
 * the runner-up before averaging boxes at least a quarter of the initial
 * mean, then cap at 1.4 times their minimum with a half-mean floor: even a
 * majority of tiny punctuation or clipped slivers must not collapse the
 * entire element. A literal intersection can collapse the font size and split
 * a line into columns. The 0.24-box margin below leaves room for PDF coordinate
 * rounding within the quarter-box search tolerance. Only boxes at least the
 * run's median height require a strict centre intersection;
 * smaller boxes use the tolerance window above. Disjoint full-height boxes
 * and models without separators retain the bounded fallback below. */
typedef struct {
  size_t end, coincident_end, coincident_slot;
  double top, bottom, height, normal_height, coincident_x, coincident_pitch;
  bool explicit_lines, valid;
} snr_text_line;
static inline bool snr_text_break(sn_string label) {
  return label.data && (memchr(label.data, '\n', label.length) ||
                        memchr(label.data, '\r', label.length));
}
static inline snr_text_line snr_text_line_create(const sn_element* el,
                                                 double width, double height) {
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
static inline double snr_text_median_height(const sn_element* el, size_t begin,
                                            size_t end, size_t count,
                                            double width, double height) {
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
static inline void snr_text_line_box(snr_text_line* line, const sn_element* el,
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
        count ? snr_text_median_height(el, index, end, count, width, height)
              : 0;
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
/* Runtime operations share the preflight reservation so extra work cannot
 * escape either ceiling when a layout or a glyph map needs more attention. */
typedef struct {
  size_t page, page_limit;
  uint64_t* document;
  uint64_t document_limit;
} snr_text_work;
static inline snr_status snr_text_charge(snr_text_work* work) {
  if (work->page == work->page_limit || *work->document == work->document_limit)
    return SNR_ERR_LIMIT;
  work->page++;
  (*work->document)++;
  return SNR_OK;
}
typedef struct {
  sn_rect box;
  double band;
} snr_spacing_box;
typedef struct {
  snr_spacing_box* boxes;
  size_t count;
  double band_height;
  snr_text_work* work;
} snr_spacing;
static inline int snr_baseline_compare(const void* a, const void* b) {
  const snr_spacing_box* x = a;
  const snr_spacing_box* y = b;
  const double left[] = {x->band, x->box.x, x->box.y, x->box.width,
                         x->box.height};
  const double right[] = {y->band, y->box.x, y->box.y, y->box.width,
                          y->box.height};
  for (size_t i = 0; i < 5; i++)
    if (left[i] != right[i]) return left[i] > right[i] ? 1 : -1;
  return 0;
}
/* Heap sorting makes the construction bound independent of libc's choice
 * of pivot strategy for qsort on adversarial coordinate sequences. */
static inline void snr_spacing_sift(snr_spacing_box* boxes, size_t root,
                                    size_t count) {
  while (root < count / 2) {
    size_t child = root * 2 + 1;
    if (child + 1 < count &&
        snr_baseline_compare(boxes + child, boxes + child + 1) < 0)
      child++;
    if (snr_baseline_compare(boxes + root, boxes + child) >= 0) break;
    snr_spacing_box swap = boxes[root];
    boxes[root] = boxes[child];
    boxes[child] = swap;
    root = child;
  }
}
static inline void snr_spacing_sort(snr_spacing_box* boxes, size_t count) {
  for (size_t i = count / 2; i > 0; i--) snr_spacing_sift(boxes, i - 1, count);
  for (size_t i = count; i > 1; i--) {
    snr_spacing_box swap = boxes[0];
    boxes[0] = boxes[i - 1];
    boxes[i - 1] = swap;
    snr_spacing_sift(boxes, 0, i - 1);
  }
}
static inline snr_status snr_spacing_create(const sn_recognition* rec,
                                            double width, double height,
                                            snr_text_work* work,
                                            snr_spacing* out) {
  *out = (snr_spacing){.work = work};
  if (!rec) return SNR_OK;
  size_t count = 0;
  for (size_t i = 0; i < rec->element_count; i++) {
    if (rec->elements[i].word_count >
        SIZE_MAX / sizeof(snr_spacing_box) - count)
      return SNR_ERR_LIMIT;
    count += rec->elements[i].word_count;
  }
  if (!count) return SNR_OK;
  out->boxes = malloc(count * sizeof *out->boxes);
  if (!out->boxes) return SNR_ERR_OUT_OF_MEMORY;
  for (size_t i = 0; i < rec->element_count; i++) {
    const sn_element* el = rec->elements + i;
    for (size_t j = 0; j < el->word_count; j++) {
      sn_rect r = el->words[j].box;
      if (!snr_box_clip(&r, width, height)) continue;
      snr_text clean = snr_recognition_text(el->words[j].label);
      bool present = clean.length != 0;
      free(clean.data);
      if (clean.status) return clean.status;
      if (!present) continue;
      out->boxes[out->count++].box = r;
      out->band_height = fmax(out->band_height, r.height * 0.1875);
    }
  }
  for (size_t i = 0; i < out->count; i++) {
    sn_rect r = out->boxes[i].box;
    out->boxes[i].band = floor((r.y + r.height / 2) / out->band_height);
  }
  snr_spacing_sort(out->boxes, out->count);
  size_t used = 0;
  for (size_t i = 0; i < out->count; i++) {
    if (used && !snr_baseline_compare(out->boxes + used - 1, out->boxes + i))
      continue;
    out->boxes[used++] = out->boxes[i];
  }
  out->count = used;
  return SNR_OK;
}
/* Thirteen bands cover overlapping boxes even at the maximum clipped
 * height. At most two entries on each side of x provide the immediate
 * separator plus the query's own entry. For an ordered nonoverlapping line,
 * the immediate right edge is the tightest separation constraint; farther
 * words cannot consume that gap. Coincident runs are collapsed beforehand.
 * Overlapping or interleaved lines have no geometric separation guarantee,
 * but still make at most 52 charged visits per word. */
static inline snr_status snr_word_spacing(sn_rect* box,
                                          const snr_spacing* spacing) {
  if (!spacing->count) return SNR_OK;
  double center = box->y + box->height / 2;
  double band = floor(center / spacing->band_height);
  sn_rect neighbours[52];
  size_t count = 0;
  for (int offset = -6; offset <= 6; offset++) {
    double target = band + offset;
    size_t low = 0, high = spacing->count;
    while (low < high) {
      size_t mid = low + (high - low) / 2;
      snr_spacing_box r = spacing->boxes[mid];
      if (r.band < target || (r.band == target && r.box.x < box->x))
        low = mid + 1;
      else
        high = mid;
    }
    size_t begin = low > 2 ? low - 2 : 0;
    size_t end = low + 2 < spacing->count ? low + 2 : spacing->count;
    for (size_t j = begin; j < end; j++) {
      if (spacing->boxes[j].band != target) continue;
      snr_status status = snr_text_charge(spacing->work);
      if (status) return status;
      sn_rect r = spacing->boxes[j].box;
      if (r.x == box->x && r.y == box->y && r.width == box->width &&
          r.height == box->height)
        continue;
      neighbours[count++] = r;
    }
  }
  /* Separate uneven baselines before their line extents absorb punctuation.
   * Solving the vertical clearance and quarter-em horizontal gap constraints
   * for height retains the largest band allowed by both neighbours. */
  double minimum_width = box->width * 0.05;
  double h = box->height;
  for (size_t i = 0; i < count; i++) {
    sn_rect r = neighbours[i];
    double dy = fabs(r.y + r.height / 2 - center);
    double reach = fmax(box->height, r.height);
    if (r.x > box->x + box->width + reach || box->x > r.x + r.width + reach)
      continue;
    if (dy >= (box->height + r.height) / 2) continue;
    if (dy > 0) h = fmin(h, dy / 1.5);
    h = fmin(h, r.height * 1.5 - 2 * dy);
    if (r.x > box->x) h = fmin(h, (r.x - box->x - minimum_width) * 4);
    if (r.x < box->x) h = fmin(h, (box->x - r.x - r.width * 0.05) * 4);
  }
  h = fmax(box->height * 0.25, h);
  double right = box->x + box->width;
  for (size_t i = 0; i < count; i++) {
    sn_rect r = neighbours[i];
    double dy = fabs(r.y + r.height / 2 - center);
    if (r.x > box->x && dy <= fmax(h, r.height * 0.25) * 0.75)
      right = fmin(right, r.x - fmax(h, r.height * 0.25) * 0.25);
  }
  box->y = center - h / 2;
  box->height = h;
  box->width = fmax(minimum_width, right - box->x);
  return SNR_OK;
}
#endif
