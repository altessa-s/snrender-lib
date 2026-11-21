/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "snr_internal.h"
snr_status snr_svg(const snr_plan* p, snr_writer* w, snr_error* e) {
  snr_bitmap bitmap = {0};
  snr_status s = snr_compose(p, &bitmap, e);
  if (s) return s;
  s = snr_encode_svg(&bitmap, (double)p->source_width, (double)p->source_height,
                     &p->options, w);
  snr_bitmap_free(&bitmap);
  return s;
}
/* %g has no grouping, but its decimal separator can contain multiple bytes.
 * Normalize that separator without changing the process/thread locale. */
static bool svg_number(double value, char text[64]) {
  char raw[64];
  int n = snprintf(raw, sizeof raw, "%.17g", value);
  if (n < 0 || (size_t)n >= sizeof raw) return false;
  size_t used = 0;
  bool point = false;
  for (size_t i = 0; i < (size_t)n; i++) {
    char c = raw[i];
    if ((c >= '0' && c <= '9') || c == 'e' || c == 'E' || c == '+' || c == '-')
      text[used++] = c;
    else if (!point) {
      text[used++] = '.';
      point = true;
    }
  }
  text[used] = 0;
  return true;
}
snr_status snr_encode_svg(const snr_bitmap* bitmap, double width, double height,
                          const snr_options* options, snr_writer* w) {
  snr_writer png = {.limit = w->limit};
  snr_status s = snr_encode_png(bitmap, &png);
  if (s) {
    /* The embedded PNG alone already exceeds the limit, or its buffer could
     * not grow: report that as this output's own failure. */
    if (png.status == s) {
      w->status = s;
      w->origin = png.origin;
    }
    snr_buffer_free(&png.buffer);
    return s;
  }
  char head[768], width_text[64], height_text[64];
  if (!svg_number(width, width_text) || !svg_number(height, height_text)) {
    snr_buffer_free(&png.buffer);
    return SNR_ERR_BACKEND;
  }
  int len =
      snprintf(head, sizeof head,
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg "
               "xmlns=\"http://www.w3.org/2000/svg\" "
               "xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\" "
               "width=\"%s\" height=\"%s\" viewBox=\"0 0 %s %s\"><image "
               "width=\"%s\" height=\"%s\" preserveAspectRatio=\"none\" "
               "xlink:href=\"data:image/png;base64,",
               width_text, height_text, width_text, height_text, width_text,
               height_text);
  s = snr_write(w, head, (size_t)len);
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char batch[49152];
  size_t used = 0;
  for (size_t i = 0; !s && i < png.buffer.size; i += 3) {
    if (i % 12288 == 0 && snr_cancelled(options)) {
      s = SNR_ERR_CANCELLED;
      break;
    }
    size_t left = png.buffer.size - i;
    uint32_t v = (uint32_t)png.buffer.bytes[i] << 16;
    if (left > 1) v |= (uint32_t)png.buffer.bytes[i + 1] << 8;
    if (left > 2) v |= png.buffer.bytes[i + 2];
    char b[4] = {alphabet[v >> 18], alphabet[(v >> 12) & 63],
                 left > 1 ? alphabet[(v >> 6) & 63] : '=',
                 left > 2 ? alphabet[v & 63] : '='};
    memcpy(batch + used, b, 4);
    used += 4;
    if (used == sizeof batch) {
      s = snr_write(w, batch, used);
      used = 0;
    }
  }
  if (!s && used) s = snr_write(w, batch, used);
  if (!s) {
    const char tail[] = "\"/></svg>\n";
    s = snr_write(w, tail, sizeof tail - 1);
  }
  snr_buffer_free(&png.buffer);
  return s;
}
