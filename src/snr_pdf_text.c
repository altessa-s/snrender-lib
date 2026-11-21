/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include "snr_pdf_text.h"

snr_pdf_text_options snr_pdf_text_options_default(void) {
  return (snr_pdf_text_options){.abi_version = SNR_PDF_TEXT_ABI,
                                .struct_size = sizeof(snr_pdf_text_options),
                                .limits = snr_limits_default(),
                                .maximum_source_bytes = 536870912,
                                .maximum_rectangles = 4096,
                                .maximum_characters = 1048576};
}
snr_status snr_pdf_text_charge(snr_pdf_text_state* s, size_t count) {
  if (s->options.cancel && s->options.cancel(s->options.cancel_context))
    return SNR_ERR_CANCELLED;
  if (count > s->remaining) return SNR_ERR_LIMIT;
  s->remaining -= count;
  return SNR_OK;
}
/* Engine order supplies logical text and line separators, including vertical
 * and counter-rotated text. Test centres in displayed crop coordinates.
 * Backends charge one unit per scalar before calling; charge hit tests here. */
snr_status snr_pdf_text_character(snr_pdf_text_state* s, uint32_t c,
                                  double left, double bottom, double right,
                                  double top) {
  snr_status status = snr_pdf_text_charge(s, s->rect_count);
  if (status) return status;
  if (c == '\r' || c == '\n' || c == 0x2028 || c == 0x2029) {
    s->separator = '\n';
    return SNR_OK;
  }
  if (c == ' ' || c == '\t' || c == 0xa0 || c == 0x1680 ||
      (c >= 0x2000 && c <= 0x200a) || c == 0x202f || c == 0x205f ||
      c == 0x3000) {
    if (!s->separator) s->separator = ' ';
    return SNR_OK;
  }
  bool soft = c == 0xad;
  if (!soft && (c == 0x200b || c == 0x200c || c == 0x200d || c == 0x2060 ||
                c == 0xfeff || c < 0x20 || (c >= 0x7f && c <= 0x9f) ||
                (c >= 0xd800 && c <= 0xdfff) || (c >= 0xfdd0 && c <= 0xfdef) ||
                (c & 0xffff) >= 0xfffe || c > 0x10ffff))
    return SNR_OK;
  if (!isfinite(left) || !isfinite(right) || !isfinite(top) ||
      !isfinite(bottom) || right < left || top < bottom)
    return SNR_OK;
  const spe_rect crop = s->geometry.crop;
  double x = left / 2 + right / 2 - crop.x;
  double y = bottom / 2 + top / 2 - crop.y;
  double dx = x, dy = crop.height - y;
  switch (s->geometry.rotate) {
    case 90:
      dx = y;
      dy = x;
      break;
    case 180:
      dx = crop.width - x;
      dy = y;
      break;
    case 270:
      dx = crop.height - y;
      dy = crop.width - x;
      break;
    default:
      break;
  }
  bool inside = false;
  bool swapped = s->geometry.rotate == 90 || s->geometry.rotate == 270;
  double width = swapped ? crop.height : crop.width;
  double height = swapped ? crop.width : crop.height;
  if (dx >= 0 && dx < width && dy >= 0 && dy < height)
    for (size_t i = 0; i < s->rect_count; i++) {
      sn_rect r = s->rects[i];
      if (!r.is_null && dx >= r.x && dy >= r.y && dx - r.x < r.width &&
          dy - r.y < r.height) {
        inside = true;
        break;
      }
    }
  if (!inside) {
    if (!soft) s->skipped = true;
    return SNR_OK;
  }
  if (soft) {
    s->discretionary = true;
    return SNR_OK;
  }
  char separator = s->separator;
  if (s->skipped && !separator) separator = ' ';
  /* Keep a printed hard hyphen; suppress only its following wrap. A soft
   * hyphen disappears together with the wrap. Do not guess dictionary words. */
  if (separator == '\n' && !s->skipped &&
      (s->previous == '-' || s->previous == 0x2010 || s->discretionary))
    separator = 0;
  if (s->previous && separator) status = snr_write(&s->writer, &separator, 1);
  uint8_t utf8[4];
  size_t n;
  if (c < 0x80) {
    utf8[0] = (uint8_t)c;
    n = 1;
  } else if (c < 0x800) {
    utf8[0] = (uint8_t)(0xc0 | (c >> 6));
    utf8[1] = (uint8_t)(0x80 | (c & 63));
    n = 2;
  } else if (c < 0x10000) {
    utf8[0] = (uint8_t)(0xe0 | (c >> 12));
    utf8[1] = (uint8_t)(0x80 | ((c >> 6) & 63));
    utf8[2] = (uint8_t)(0x80 | (c & 63));
    n = 3;
  } else {
    utf8[0] = (uint8_t)(0xf0 | (c >> 18));
    utf8[1] = (uint8_t)(0x80 | ((c >> 12) & 63));
    utf8[2] = (uint8_t)(0x80 | ((c >> 6) & 63));
    utf8[3] = (uint8_t)(0x80 | (c & 63));
    n = 4;
  }
  if (!status) status = snr_write(&s->writer, utf8, n);
  s->previous = c;
  s->separator = 0;
  s->skipped = s->discretionary = false;
  return status;
}
snr_status snr_pdf_page_text(const uint8_t* pdf, size_t size, size_t page,
                             const sn_rect* rects, size_t rect_count,
                             const snr_pdf_text_options* given, snr_buffer* out,
                             snr_error* e) {
  if (!pdf || !size || !out || (rect_count && !rects) ||
      (given && (given->abi_version != SNR_PDF_TEXT_ABI ||
                 given->struct_size < sizeof *given)))
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page,
                    "invalid PDF text input or options ABI");
  snr_pdf_text_state s = {
      .options = given ? *given : snr_pdf_text_options_default(),
      .rects = rects,
      .rect_count = rect_count,
      .page = page};
  s.remaining = s.options.limits.maximum_document_work;
  if (s.remaining > s.options.limits.maximum_work)
    s.remaining = s.options.limits.maximum_work;
  snr_status status = snr_pdf_text_charge(&s, rect_count);
  if (!status &&
      (size > s.options.maximum_source_bytes ||
       rect_count > s.options.maximum_rectangles || rect_count == SIZE_MAX))
    status = SNR_ERR_LIMIT;
  if (status)
    return snr_fail(e, status, page, "PDF text input limit or cancellation");
  for (size_t i = 0; i < rect_count; i++) {
    sn_rect r = rects[i];
    if (!r.is_null && (!isfinite(r.x) || !isfinite(r.y) || !isfinite(r.width) ||
                       !isfinite(r.height) || r.width < 0 || r.height < 0))
      return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page,
                      "invalid PDF text rectangle");
  }
#if defined(SNR_CAIRO_BACKEND) && !defined(SNR_PDFIUM)
  return snr_fail(e, SNR_ERR_UNSUPPORTED, page,
                  "PDF text requires SNRENDER_ENABLE_PDFIUM");
#endif
  size_t scratch = size > SIZE_MAX / 16 ? SIZE_MAX : size * 16;
  if (scratch < 1048576) scratch = 1048576;
  spe_limits limits = {.maximum_elements = s.options.limits.maximum_work,
                       .maximum_plan_bytes = scratch,
                       .maximum_pdf_bytes = SIZE_MAX,
                       .maximum_draws = s.options.limits.maximum_pages,
                       .maximum_coordinate = 14400,
                       .remaining = &s.remaining,
                       .cancel = s.options.cancel,
                       .context = s.options.cancel_context};
  spe_pdf_page_geometry* pages = nullptr;
  status = spe_pdf_pages(pdf, size, &limits, &pages, &s.page_count, nullptr, e);
  if (status) {
    if (e) e->index = page;
    return status;
  }
  if (page >= s.page_count) {
    free(pages);
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page,
                    "PDF text page outside source PDF");
  }
  for (size_t i = 0; !status && i < s.page_count; i++) {
    spe_pdf_page_geometry g = pages[i];
    if (!g.has_media || !isfinite(g.media.width) || !isfinite(g.media.height) ||
        g.media.width <= 0 || g.media.height <= 0 || g.media.width > 14400 ||
        g.media.height > 14400 || !isfinite(g.crop.width) ||
        !isfinite(g.crop.height) || g.crop.width <= 0 || g.crop.height <= 0)
      status = SNR_ERR_LIMIT;
  }
  s.geometry = pages[page];
  free(pages);
  if (status)
    return snr_fail(e, status, page, "source PDF page dimension limit");
  s.writer.limit = s.options.limits.maximum_output_bytes;
  if (!status) status = snr_source_text(pdf, size, &s);
  if (!status) status = snr_pdf_text_charge(&s, 0);
  if (!status) status = snr_write(&s.writer, "", 1);
  if (!status) {
    s.writer.buffer.size--; /* NUL is stored but is not part of the text. */
    *out = s.writer.buffer;
    return SNR_OK;
  }
  snr_buffer_free(&s.writer.buffer);
  return snr_fail(e, status, page,
                  status == SNR_ERR_CANCELLED ? "PDF text cancelled"
                  : status == SNR_ERR_LIMIT
                      ? "PDF text work, character or output limit exceeded"
                  : status == SNR_ERR_OUT_OF_MEMORY
                      ? "PDF text allocation failed"
                      : "PDF text extraction failed");
}
