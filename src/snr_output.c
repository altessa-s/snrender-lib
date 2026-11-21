/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "snr_internal.h"
static snr_status page_output(const sn_notebook* n, size_t page,
                              const snr_options* o, snr_buffer* out,
                              const snr_output* output, snr_error* e,
                              bool svg) {
  if (!out && !output)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page, "null output");
  snr_status s = output ? snr_output_validate(output, e) : SNR_OK;
  if (s) return s;
  snr_plan p;
  s = snr_image_plan(n, page, o, &p, e);
  if (s) return s;
  snr_writer w = {.output = output,
                  .limit = p.options.limits.maximum_output_bytes};
  snr_error detail = {0};
  if (svg)
    s = snr_svg(&p, &w, &detail);
  else {
    snr_bitmap b = {0};
    s = snr_compose(&p, &b, &detail);
    if (!s) s = snr_encode_png(&b, &w);
    snr_bitmap_free(&b);
  }
  if (!s) s = w.status;
  if (s) {
    snr_buffer_free(&w.buffer);
    if (detail.code) {
      if (e) *e = detail;
      return s;
    }
    const char* written = s == w.status ? snr_writer_message(&w) : nullptr;
    return snr_fail(e, s, page, written ? written : "page output failed");
  }
  if (out) *out = w.buffer;
  return SNR_OK;
}
snr_status snr_page_png(const sn_notebook* n, size_t p, const snr_options* o,
                        snr_buffer* b, snr_error* e) {
  return page_output(n, p, o, b, nullptr, e, false);
}
snr_status snr_page_svg(const sn_notebook* n, size_t p, const snr_options* o,
                        snr_buffer* b, snr_error* e) {
  return page_output(n, p, o, b, nullptr, e, true);
}
snr_status snr_page_png_to_output(const sn_notebook* n, size_t p,
                                  const snr_options* o, const snr_output* b,
                                  snr_error* e) {
  return page_output(n, p, o, nullptr, b, e, false);
}
snr_status snr_page_svg_to_output(const sn_notebook* n, size_t p,
                                  const snr_options* o, const snr_output* b,
                                  snr_error* e) {
  return page_output(n, p, o, nullptr, b, e, true);
}
static snr_status document(const sn_notebook* n, size_t first, size_t count,
                           const snr_options* input, snr_buffer* out,
                           const snr_output* output, snr_error* e) {
  if (!n || !n->pages || first >= n->page_count || (!out && !output))
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, first,
                    "invalid document or output");
  snr_options o;
  snr_status s = snr_options_read(input, &o, e);
  if (s) return s;
  if (output && (s = snr_output_validate(output, e))) return s;
  if (count == SIZE_MAX) count = n->page_count - first;
  if (!count || count > n->page_count - first)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, first, "invalid page range");
  if (count > o.limits.maximum_pages)
    return snr_fail(e, SNR_ERR_LIMIT, first, "document page limit");
  if (n->title_count > o.limits.maximum_work ||
      n->link_count > o.limits.maximum_work - n->title_count)
    return snr_fail(e, SNR_ERR_LIMIT, first, "annotation work limit");
  size_t work = n->title_count + n->link_count;
  uint64_t document_work = 0;
  for (size_t i = first; i < first + count; i++) {
    snr_plan p;
    s = snr_plan_make(n, i, &o, &p, e);
    if (s) return s;
    if (p.work > o.limits.maximum_work - work)
      return snr_fail(e, SNR_ERR_LIMIT, i, "page work limit");
    uint64_t charge = (uint64_t)p.work + work;
    if (charge > o.limits.maximum_document_work - document_work)
      return snr_fail(e, SNR_ERR_LIMIT, i, "document work limit");
    document_work += charge;
  }
  snr_writer w = {.output = output,
                  .limit = o.limits.maximum_output_bytes,
                  .document_work = document_work};
  snr_error detail = {0};
  s = snr_backend_pdf(n, first, count, &o, &w, &detail);
  if (!s) s = w.status;
  if (s) {
    snr_buffer_free(&w.buffer);
    if (detail.code) {
      if (e) *e = detail;
      return s;
    }
    return snr_fail(e, s, first, "PDF output failed");
  }
  if (out) *out = w.buffer;
  return SNR_OK;
}
snr_status snr_document_pdf(const sn_notebook* n, size_t first, size_t count,
                            const snr_options* o, snr_buffer* out,
                            snr_error* e) {
  return document(n, first, count, o, out, nullptr, e);
}
snr_status snr_document_pdf_to_output(const sn_notebook* n, size_t first,
                                      size_t count, const snr_options* o,
                                      const snr_output* out, snr_error* e) {
  return document(n, first, count, o, nullptr, out, e);
}
snr_status snr_page_pdf(const sn_notebook* n, size_t page, const snr_options* o,
                        snr_buffer* out, snr_error* e) {
  return document(n, page, 1, o, out, nullptr, e);
}

snr_status snr_page_pdf_to_output(const sn_notebook* n, size_t page,
                                  const snr_options* o, const snr_output* out,
                                  snr_error* e) {
  return snr_document_pdf_to_output(n, page, 1, o, out, e);
}
