/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <stdlib.h>

#include "snr_pdf_text.h"

/* Exact synthetic glyph boxes avoid native font metric rounding. Invert the
 * documented display transform, then exercise each edge of the crop and rect.
 */
static void centre(int rotation, double dx, double dy, bool included) {
  sn_rect rect = {0, 0, 80, 60, false};
  double x, y;
  double w = rotation == 90 || rotation == 270 ? 60 : 80;
  double h = rotation == 90 || rotation == 270 ? 80 : 60;
  switch (rotation) {
    case 90:
      x = dy;
      y = dx;
      break;
    case 180:
      x = w - dx;
      y = dy;
      break;
    case 270:
      x = w - dy;
      y = h - dx;
      break;
    default:
      x = dx;
      y = h - dy;
      break;
  }
  x -= 40;
  y -= 30;
  snr_pdf_text_state s = {
      .geometry = {.crop = {-40, -30, w, h}, .rotate = rotation},
      .rects = &rect,
      .rect_count = 1,
      .remaining = 10,
      .writer = {.limit = 10}};
  if (snr_pdf_text_character(&s, 'A', x - 1, y - 1, x + 1, y + 1) ||
      s.writer.buffer.size != (size_t)included ||
      (included && s.writer.buffer.bytes[0] != 'A'))
    abort();
  snr_buffer_free(&s.writer.buffer);
}
int main(void) {
  for (int r = 0; r < 360; r += 90) {
    centre(r, 0, 30, true);
    centre(r, 40, 0, true);
    centre(r, 80, 30, false);
    centre(r, 40, 60, false);
    centre(r, -1, 30, false);
    centre(r, 40, -1, false);
  }
  return 0;
}
