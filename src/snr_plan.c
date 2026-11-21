/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "snr_internal.h"
snr_status snr_plan_make(const sn_notebook* n, size_t index,
                         const snr_options* options, snr_plan* out,
                         snr_error* e) {
  if (!n || !n->pages || index >= n->page_count)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, index, "invalid page");
  snr_plan p = {.notebook = n, .page = n->pages + index, .index = index};
  snr_status s = snr_options_read(options, &p.options, e);
  if (s) {
    if (e) e->index = index;
    return s;
  }
  if (snr_cancelled(&p.options))
    return snr_fail(e, SNR_ERR_CANCELLED, index, "render cancelled");
  double w = p.page->size.width, h = p.page->size.height;
  if (!isfinite(w) || !isfinite(h) || w < 1 || h < 1 || floor(w) != w ||
      floor(h) != h || (p.page->layer_count && !p.page->layers))
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, index,
                    "invalid page dimensions or layers");
  double sw = ceil(w * p.options.scale), sh = ceil(h * p.options.scale);
  /* Keep floating-to-integer conversions below an exactly representable bound.
   */
  if (w > 16777216 || h > 16777216 || !isfinite(sw) || !isfinite(sh) ||
      sw > 16777216 || sh > 16777216)
    return snr_fail(e, SNR_ERR_LIMIT, index, "dimension limit");
#ifdef SNR_CAIRO_BACKEND
  if (sw > 32767 || sh > 32767)
    return snr_fail(e, SNR_ERR_LIMIT, index, "Cairo dimension limit");
#endif
  p.source_width = (size_t)w;
  p.source_height = (size_t)h;
  p.width = (size_t)sw;
  p.height = (size_t)sh;
  const snr_limits* l = &p.options.limits;
  if (p.source_width > l->maximum_decoded_pixels / p.source_height ||
      p.source_width > SIZE_MAX / 4 / p.source_height ||
      p.width > l->maximum_pixels / p.height ||
      p.width > SIZE_MAX / 4 / p.height)
    return snr_fail(e, SNR_ERR_LIMIT, index, "pixel limit");
  size_t pixels = p.source_width * p.source_height;
  p.work = p.width * p.height;
  if (p.work > l->maximum_work || pixels > l->maximum_work - p.work)
    return snr_fail(e, SNR_ERR_LIMIT, index, "work limit");
  p.work += pixels;
  if (p.page->layer_count > l->maximum_work - p.work)
    return snr_fail(e, SNR_ERR_LIMIT, index, "layer traversal limit");
  p.work += p.page->layer_count;
  if (p.options.recognition && p.page->recognition) {
    const sn_recognition* r = p.page->recognition;
    if (r->element_count > l->maximum_work - p.work)
      return snr_fail(e, SNR_ERR_LIMIT, index, "recognition work limit");
    p.work += r->element_count;
    for (size_t i = 0; i < r->element_count; i++) {
      const sn_element* el = r->elements + i;
      if (el->word_count > l->maximum_work - p.work)
        return snr_fail(e, SNR_ERR_LIMIT, index, "recognition work limit");
      p.work += el->word_count;
      for (size_t j = 0; j < el->word_count; j++) {
        /* Reserving a glyph's bookkeeping for every input byte bounds
         * both sanitization and the worst-case number of new scalar IDs. */
        if (el->words[j].label.length > (l->maximum_work - p.work) / 256 ||
            el->words[j].label.length > 65536)
          return snr_fail(e, SNR_ERR_LIMIT, index, "recognition word limit");
        p.work += el->words[j].label.length * 256;
      }
    }
  }
  for (size_t i = 0; i < p.page->layer_count; i++) {
    const sn_layer* layer = p.page->layers + i;
    if (!layer->is_visible || !layer->bitmap ||
        (!p.options.include_background && layer->name == SN_LAYER_BACKGROUND))
      continue;
    if (pixels > l->maximum_work - p.work ||
        layer->bitmap->bytes.size > l->maximum_output_bytes)
      return snr_fail(e, SNR_ERR_LIMIT, index,
                      "layer work or encoded size limit");
    p.work += pixels;
    size_t charge = pixels;
    if (layer->bitmap->encoding != SN_ENCODING_RATTA_RLE) {
      size_t encoded = layer->bitmap->bytes.size;
      if (encoded > l->maximum_work - p.work)
        return snr_fail(e, SNR_ERR_LIMIT, index, "encoded image work limit");
      p.work += encoded;
      size_t iw = 0, ih = 0;
      snr_status checked =
          snr_image_size(layer->bitmap, l->maximum_decoded_pixels, &iw, &ih);
      if (checked == SNR_ERR_LIMIT || (checked && p.options.strict_images))
        return snr_fail(e, checked, index, "encoded image dimensions rejected");
      charge = iw * ih;
    }
    if (charge > l->maximum_work - p.work)
      return snr_fail(e, SNR_ERR_LIMIT, index, "decoded image work limit");
    p.work += charge;
  }
  p.points_per_pixel = 72 / snr_page_dpi(n, p.page);
  *out = p;
  return SNR_OK;
}

/* Image outputs never inspect or reserve work for PDF recognition. */
snr_status snr_image_plan(const sn_notebook* n, size_t page,
                          const snr_options* input, snr_plan* out,
                          snr_error* e) {
  if (!n || !n->pages || page >= n->page_count)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page, "invalid page");
  snr_options options;
  snr_status s = snr_options_read(input, &options, e);
  if (s) {
    if (e) e->index = page;
    return s;
  }
  options.recognition = false;
  return snr_plan_make(n, page, &options, out, e);
}
