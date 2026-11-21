/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <limits.h>
#include <stdarg.h>
#include <zlib.h>

#include "snr_overlay_internal.h"

snr_overlay_options snr_overlay_options_default(void) {
  return (snr_overlay_options){SNR_OVERLAY_ABI,
                               sizeof(snr_overlay_options),
                               snr_options_default(),
                               SNR_DOCUMENT_PDF,
                               false,
                               {536870912},
                               nullptr};
}
snr_status snr_mark_page_has_annotations(const sn_notebook* n, size_t page,
                                         bool* out, snr_error* e) {
  if (!n || n->kind != SN_KIND_MARK || !out || page >= INT64_MAX ||
      (n->page_count && !n->pages) || (n->highlight_count && !n->highlights))
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page,
                    "invalid mark or document page");
  bool found = false;
  for (size_t i = 0; i < n->page_count; i++)
    found |= n->pages[i].number == (int64_t)page + 1;
  for (size_t i = 0; i < n->highlight_count; i++)
    found |= n->highlights[i].page_number == (int64_t)page + 1;
  *out = found;
  return SNR_OK;
}
typedef struct {
  snr_overlay_options options;
  spe_limits limits;
  spe_pdf_page_geometry* pages;
  size_t count;
  uint64_t remaining;
} overlay_plan;
static void display(const spe_pdf_page_geometry* g, double* width,
                    double* height) {
  bool swapped = g->rotate == 90 || g->rotate == 270;
  *width = swapped ? g->crop.height : g->crop.width;
  *height = swapped ? g->crop.width : g->crop.height;
}
typedef struct {
  double scale, x, y;
} sheet_fit;
/* Both margins belong to the device sheet, so tall pages retain their full
 * height and their horizontal paper margins do not become document ink. */
static sheet_fit fit_sheet(sn_size sheet, const spe_pdf_page_geometry* page) {
  double w, h;
  display(page, &w, &h);
  double scale = fmin(sheet.width / w, sheet.height / h);
  return (sheet_fit){scale, (sheet.width - w * scale) / 2,
                     (sheet.height - h * scale) / 2};
}
/* Exported ink has render.scale pixels per sheet pixel. A preview has
 * render.scale pixels per PDF point, so its sheet needs only scale / fit
 * pixels per sheet pixel; rendering it denser would charge the pixel limit
 * for detail the nearest sampling then discards. The preview density is never
 * raised above render.scale, so no request that fits today stops fitting. */
static double sheet_density(const snr_options* o, sn_size sheet,
                            const spe_pdf_page_geometry* page, bool preview) {
  if (!preview) return o->scale;
  double fit = fit_sheet(sheet, page).scale;
  if (!isfinite(fit) || fit <= 1) return o->scale;
  return fmax(o->limits.minimum_scale, o->scale / fit);
}
/* A one-based document page, or zero for the whole mark; a negative residual
 * is not reported. */
static void warning(size_t page, const char* reason, double residual) {
  char message[192];
  char where[48] = "";
  if (page) snprintf(where, sizeof where, "document page %zu: ", page);
  if (residual >= 0)
    snprintf(message, sizeof message, "%s%s (worst residual %.3f sheet pixels)",
             where, reason, residual);
  else
    snprintf(message, sizeof message, "%s%s", where, reason);
  snr_log(SNR_LOG_WARNING, message);
}
static int page_order(const void* a, const void* b) {
  int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
  return (x > y) - (x < y);
}
static const sn_page* sheet(const sn_notebook* n, size_t page, size_t* index) {
  for (size_t i = 0; i < n->page_count; i++)
    if (n->pages[i].number == (int64_t)page + 1) {
      *index = i;
      return &n->pages[i];
    }
  return nullptr;
}
static snr_status prepare(const sn_notebook* n, const uint8_t* pdf, size_t size,
                          const snr_overlay_options* given, bool preview,
                          overlay_plan* p, snr_error* e) {
  if (!n || n->kind != SN_KIND_MARK || !pdf || !size ||
      (n->page_count && !n->pages) || (n->highlight_count && !n->highlights))
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0,
                    "invalid mark or source PDF");
  if (given && (given->abi_version != SNR_OVERLAY_ABI ||
                given->struct_size < sizeof *given))
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0,
                    "invalid overlay options ABI");
  p->options = given ? *given : snr_overlay_options_default();
  snr_overlay_result* result = p->options.result;
  if (result) {
    if (result->abi_version != SNR_OVERLAY_RESULT_ABI ||
        result->struct_size < sizeof *result)
      return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0,
                      "invalid overlay result ABI");
    *result = (snr_overlay_result){.abi_version = SNR_OVERLAY_RESULT_ABI,
                                   .struct_size = sizeof *result};
  }
  snr_options render = p->options.render;
  snr_status s = snr_options_read(&render, &p->options.render, e);
  if (s) return s;
  if (p->options.source_kind == SNR_DOCUMENT_EPUB)
    return snr_fail(e, SNR_ERR_UNSUPPORTED, 0,
                    "EPUB device reflow layout is unavailable");
  if (p->options.source_kind != SNR_DOCUMENT_PDF)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0,
                    "invalid source document kind");
#if defined(SNR_CAIRO_BACKEND) && !defined(SNR_PDFIUM)
  return snr_fail(e, SNR_ERR_UNSUPPORTED, 0,
                  "PDF overlay requires SNRENDER_ENABLE_PDFIUM");
#endif
  snr_options* o = &p->options.render;
  if (snr_cancelled(o))
    return snr_fail(e, SNR_ERR_CANCELLED, 0, "PDF overlay cancelled");
  if (n->page_count > o->limits.maximum_pages ||
      n->highlight_count > o->limits.maximum_work)
    return snr_fail(e, SNR_ERR_LIMIT, 0, "mark annotation limit");
  if (size > p->options.limits.maximum_source_bytes)
    return snr_fail(e, SNR_ERR_LIMIT, 0, "source PDF byte limit exceeded");
  size_t scratch = size > SIZE_MAX / 16 ? SIZE_MAX : size * 16;
  if (scratch < 1048576) scratch = 1048576;
  p->remaining = o->limits.maximum_document_work;
  p->limits = (spe_limits){.maximum_elements = o->limits.maximum_work,
                           .maximum_plan_bytes = scratch,
                           .maximum_pdf_bytes = SIZE_MAX,
                           .maximum_draws = o->limits.maximum_pages,
                           .maximum_coordinate = 14400,
                           .remaining = &p->remaining,
                           .cancel = o->cancel,
                           .context = o->cancel_context};
  size_t signatures = 0;
  s = spe_pdf_pages(pdf, size, &p->limits, &p->pages, &p->count, &signatures,
                    e);
  if (s) return s;
  if (!p->count)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0, "source PDF has no pages");
  if (result) result->signature_fields = signatures;
  if (signatures)
    snr_log(SNR_LOG_WARNING,
            "source PDF has signature fields: full export appends changes "
            "after signing; page-range export rewrites and drops signatures");
  uint64_t work = 0;
  for (size_t i = 0; !s && i < p->count; i++) {
    const spe_pdf_page_geometry* g = &p->pages[i];
    if (snr_cancelled(o)) {
      s = snr_fail(e, SNR_ERR_CANCELLED, i, "PDF overlay cancelled");
      break;
    }
    if (!g->has_media || !isfinite(g->media.width) ||
        !isfinite(g->media.height) || g->media.width <= 0 ||
        g->media.height <= 0 || g->media.width > 14400 ||
        g->media.height > 14400 || !isfinite(g->crop.width) ||
        !isfinite(g->crop.height) || g->crop.width <= 0 || g->crop.height <= 0)
      s = snr_fail(e, SNR_ERR_LIMIT, i, "source PDF page dimension limit");
    double w, h;
    display(g, &w, &h);
    double pixels = ceil(w * o->scale) * ceil(h * o->scale);
    if (pixels > (double)o->limits.maximum_pixels ||
        pixels > (double)o->limits.maximum_work ||
        pixels > (double)(SIZE_MAX / 4))
      s = snr_fail(e, SNR_ERR_LIMIT, i, "source PDF pixel or work limit");
    if (!s) {
      uint64_t charge = (uint64_t)pixels + n->page_count + n->highlight_count;
      if (charge > p->remaining - work)
        s = snr_fail(e, SNR_ERR_LIMIT, i, "document work limit");
      else
        work += charge;
    }
  }
  for (size_t i = 0; !s && i < n->page_count; i++) {
    const sn_page* page = &n->pages[i];
    if (page->number <= 0 || (uint64_t)page->number > p->count) {
      if (p->options.strict)
        s = snr_fail(e, SNR_ERR_INVALID_ARGUMENT,
                     page->number > 0 ? (size_t)page->number - 1 : 0,
                     "mark page outside source PDF");
      if (!p->options.strict) {
        warning(page->number > 0 ? (size_t)page->number : 0,
                "sheet dropped outside source PDF", -1);
        if (result) result->sheets_dropped++;
      }
      continue;
    }
    snr_plan ink;
    snr_options ink_options = *o;
    ink_options.limits.maximum_output_bytes = SIZE_MAX;
    ink_options.scale = sheet_density(
        o, page->size, &p->pages[(size_t)page->number - 1], preview);
    s = snr_image_plan(n, i, &ink_options, &ink, e);
    if (s && e) e->index = (size_t)page->number - 1;
    if (!s && ink.work > p->remaining - work)
      s = snr_fail(e, SNR_ERR_LIMIT, (size_t)page->number - 1,
                   "document work limit");
    if (!s) work += ink.work;
  }
  /* Pages checked by pairs are counted once each, from a sorted list of the
   * pages whose highlights carried pairs. */
  size_t agreeing = 0, paired_count = 0;
  double worst_all = 0;
  int64_t* paired_pages = n->highlight_count
                              ? calloc(n->highlight_count, sizeof *paired_pages)
                              : nullptr;
  if (n->highlight_count && !paired_pages && !s)
    s = snr_fail(e, SNR_ERR_OUT_OF_MEMORY, 0, "overlay allocation failed");
  for (size_t i = 0; !s && i < n->highlight_count; i++) {
    const sn_highlight* h = &n->highlights[i];
    if (h->rect_count > o->limits.maximum_work ||
        h->document_rect_count > o->limits.maximum_work ||
        (h->rect_count && !h->rects) ||
        (h->document_rect_count && !h->document_rects)) {
      s = snr_fail(e, SNR_ERR_LIMIT,
                   h->page_number > 0 ? (size_t)h->page_number - 1 : 0,
                   "highlight rectangle limit");
      break;
    }
    if (h->page_number <= 0 || (uint64_t)h->page_number > p->count) {
      if (p->options.strict)
        s = snr_fail(e, SNR_ERR_INVALID_ARGUMENT,
                     h->page_number > 0 ? (size_t)h->page_number - 1 : 0,
                     "highlight page outside source PDF");
      if (!p->options.strict) {
        warning(h->page_number > 0 ? (size_t)h->page_number : 0,
                "highlight dropped outside source PDF", -1);
        if (result) result->highlights_dropped++;
      }
      continue;
    }
    size_t index = 0;
    const sn_page* pg = sheet(n, (size_t)h->page_number - 1, &index);
    sn_size sz = pg ? pg->size : n->sheet;
    sheet_fit fit = fit_sheet(sz, &p->pages[(size_t)h->page_number - 1]);
    double worst = 0;
    bool paired = false;
    for (size_t j = 0; j < h->rect_count && j < h->document_rect_count; j++) {
      sn_rect a = h->rects[j], b = h->document_rects[j];
      if (a.is_null || b.is_null) continue;
      paired = true;
      double residual =
          fmax(fabs(a.x - (b.x * fit.scale + fit.x)),
               fabs(a.x + a.width - ((b.x + b.width) * fit.scale + fit.x)));
      /* Underlines store a thin strip rather than the text line's height. */
      if (h->kind == 0) {
        residual = fmax(residual, fabs(a.y - (b.y * fit.scale + fit.y)));
        residual = fmax(residual, fabs(a.y + a.height -
                                       ((b.y + b.height) * fit.scale + fit.y)));
      }
      if (!isfinite(residual)) residual = INFINITY;
      worst = fmax(worst, residual);
      if (residual <= 3) agreeing++;
    }
    if (paired) {
      size_t page = (size_t)h->page_number;
      paired_pages[paired_count++] = h->page_number;
      worst_all = fmax(worst_all, worst);
      if (worst > 3) {
        if (p->options.strict)
          s = snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page - 1,
                       "mark and PDF highlight geometry disagree");
        else
          warning(page, "mark and PDF highlight geometry disagree", worst);
      }
    }
  }
  size_t checked = 0;
  if (paired_count) {
    qsort(paired_pages, paired_count, sizeof *paired_pages, page_order);
    for (size_t i = 0; i < paired_count; i++)
      checked += !i || paired_pages[i] != paired_pages[i - 1];
  }
  free(paired_pages);
  bool annotations = n->page_count || n->highlight_count, any = false;
  for (size_t i = 0; i < n->page_count; i++)
    any |= n->pages[i].number > 0 && (uint64_t)n->pages[i].number <= p->count;
  for (size_t i = 0; i < n->highlight_count; i++)
    any |= n->highlights[i].page_number > 0 &&
           (uint64_t)n->highlights[i].page_number <= p->count;
  if (result) {
    result->worst_paired_residual = worst_all;
    result->pages_checked_by_pairs = checked;
    result->no_pairs_agree = checked && !agreeing;
    result->all_annotations_dropped = annotations && !any;
  }
  /* A mark exported against the wrong PDF either refers to pages the PDF
   * lacks or lands on pages whose text its highlights do not match. A mark
   * without pairs carries no evidence either way, so nothing is said about
   * whether it belongs. */
  if (!s && annotations && !any)
    warning(0,
            "all annotations dropped: the mark refers only to pages the "
            "source PDF lacks",
            -1);
  else if (!s && checked && !agreeing)
    warning(0, "no paired highlight rectangle agrees with the source PDF",
            worst_all);

  for (size_t i = 0; !s && i < p->count; i++) {
    size_t index = 0;
    const sn_page* pg = sheet(n, i, &index);
    size_t rects = 0;
    for (size_t j = 0; j < n->highlight_count; j++) {
      if (n->highlights[j].page_number == (int64_t)i + 1) {
        if (n->highlights[j].rect_count > SIZE_MAX - rects) {
          s = snr_fail(e, SNR_ERR_LIMIT, i, "highlight work limit");
          break;
        }
        rects += n->highlights[j].rect_count;
      }
    }
    if (!pg && !rects) continue;
    double width, height;
    display(&p->pages[i], &width, &height);
    sn_size sz = pg ? pg->size : n->sheet;
    if (!isfinite(sz.width) || !isfinite(sz.height) || sz.width <= 0 ||
        sz.height <= 0) {
      s = snr_fail(e, SNR_ERR_INVALID_ARGUMENT, i,
                   "invalid mark sheet dimensions");
      break;
    }
    /* A preview's ink layer has the preview's own size; an exported layer
     * keeps sheet density. */
    double density =
        preview ? o->scale
                : fmax(1, fit_sheet(sz, &p->pages[i]).scale) * o->scale;
    double pixels = ceil(width * density) * ceil(height * density);
    double charge = pixels * (8 + (double)rects) + (double)n->page_count +
                    (double)n->highlight_count;
    if (!isfinite(charge) || charge >= (double)UINT64_MAX ||
        pixels > (double)o->limits.maximum_pixels ||
        charge > (double)o->limits.maximum_work ||
        charge > (double)(p->remaining - work))
      s = snr_fail(e, SNR_ERR_LIMIT, i, "overlay pixel or work limit");
    else
      work += (uint64_t)charge;
  }
  if (!s) p->remaining -= work;
  if (s) {
    free(p->pages);
    p->pages = nullptr;
  }
  return s;
}
/* The stored gray is a multiplication factor over paper. Converting it to
 * black coverage lets an ordinary PDF soft mask retain the same appearance
 * over coloured source content. */
static snr_status ink_bitmap(const sn_notebook* n, size_t page,
                             const overlay_plan* p, bool preview, size_t width,
                             size_t height, snr_bitmap* out, snr_error* e) {
  size_t index = 0;
  const sn_page* pg = sheet(n, page, &index);
  sn_size sz = pg ? pg->size : n->sheet;
  if (!isfinite(sz.width) || !isfinite(sz.height) || sz.width <= 0 ||
      sz.height <= 0)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page,
                    "invalid mark sheet dimensions");
  snr_bitmap src = {0}, dst = {0};
  snr_options o = p->options.render;
  o.limits.maximum_output_bytes = SIZE_MAX;
  o.has_background = true;
  o.background = (snr_color){0, 0, 0, 0};
  o.include_background = false;
  o.scale = sheet_density(&p->options.render, sz, &p->pages[page], preview);
  snr_status s = pg ? snr_page_bitmap(n, index, &o, &src, e) : SNR_OK;
  if (s) {
    if (e) e->index = page;
    return s;
  }
  dst = (snr_bitmap){.pixels = calloc(width * height, 4),
                     .width = width,
                     .height = height,
                     .stride = width * 4};
  if (!dst.pixels) {
    snr_bitmap_free(&src);
    return snr_fail(e, SNR_ERR_OUT_OF_MEMORY, page,
                    "overlay bitmap allocation failed");
  }
  double dw, dh;
  display(&p->pages[page], &dw, &dh);
  sheet_fit fit = fit_sheet(sz, &p->pages[page]);
  for (size_t y = 0; y < height; y++) {
    if (snr_cancelled(&o)) {
      s = snr_fail(e, SNR_ERR_CANCELLED, page, "PDF overlay cancelled");
      break;
    }
    double sy = ((double)y + 0.5) * dh / (double)height * fit.scale + fit.y;
    if (src.pixels && sy >= 0 && sy < sz.height) {
      size_t yy = (size_t)(sy * (double)src.height / sz.height);
      for (size_t x = 0; x < width; x++) {
        double sx = ((double)x + 0.5) * dw / (double)width * fit.scale + fit.x;
        if (sx < 0 || sx >= sz.width) continue;
        size_t xx = (size_t)(sx * (double)src.width / sz.width);
        const uint8_t* a = src.pixels + yy * src.stride + xx * 4;
        dst.pixels[y * dst.stride + x * 4 + 3] =
            (uint8_t)(a[3] - (77u * a[0] + 150u * a[1] + 29u * a[2]) / 256u);
      }
    }
  }
  /* Screen rectangles retain the device's underline thickness; their paired
   * document rectangles serve as an independent geometry check above. */
  for (size_t i = 0; !s && i < n->highlight_count; i++) {
    const sn_highlight* h = &n->highlights[i];
    if (h->page_number != (int64_t)page + 1) continue;
    for (size_t j = 0; j < h->rect_count; j++) {
      sn_rect r = h->rects[j];
      if (r.is_null || !isfinite(r.x) || !isfinite(r.y) || !isfinite(r.width) ||
          !isfinite(r.height) || r.width <= 0 || r.height <= 0)
        continue;
      double x0 = fmax(0, fmin((double)width,
                               (r.x - fit.x) / fit.scale / dw * (double)width));
      double x1 =
          fmax(0, fmin((double)width, (r.x + r.width - fit.x) / fit.scale / dw *
                                          (double)width));
      double y0 = fmax(0, fmin((double)height, (r.y - fit.y) / fit.scale / dh *
                                                   (double)height));
      double y1 =
          fmax(0, fmin((double)height, (r.y + r.height - fit.y) / fit.scale /
                                           dh * (double)height));
      for (size_t y = (size_t)y0; y < (size_t)ceil(y1); y++) {
        if (snr_cancelled(&o)) {
          s = snr_fail(e, SNR_ERR_CANCELLED, page, "PDF overlay cancelled");
          break;
        }
        for (size_t x = (size_t)x0; x < (size_t)ceil(x1); x++) {
          uint8_t* a = &dst.pixels[y * dst.stride + x * 4 + 3];
          uint8_t coverage = h->kind == 1 ? 180 : 64;
          if (*a < coverage) *a = coverage;
        }
      }
    }
  }
  snr_bitmap_free(&src);
  if (s)
    snr_bitmap_free(&dst);
  else
    *out = dst;
  return s;
}
snr_status snr_source_flatten(const uint8_t* bytes, size_t size, size_t page,
                              const spe_limits* limits, bool orphans_only,
                              snr_buffer* out, size_t* drawn, snr_error* e) {
  *drawn = 0;
  spe_pdf_update* u = nullptr;
  snr_status s =
      spe_pdf_update_create(bytes, size, false, limits, limits->cancel,
                            limits->context, nullptr, &u, e);
  if (!s) s = spe_pdf_update_appearances(u, page, orphans_only, drawn, e);
  if (!s && *drawn) s = spe_pdf_update_finish(u, out, nullptr, e);
  spe_pdf_update_free(u);
  if (s && e) e->index = page;
  return s;
}
static snr_status preview(const sn_notebook* n, const uint8_t* pdf, size_t size,
                          size_t page, const snr_overlay_options* o,
                          snr_bitmap* out, snr_error* e) {
  if (!out) return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page, "null output");
  overlay_plan p = {0};
  snr_status s = prepare(n, pdf, size, o, true, &p, e);
  if (s) return s;
  if (page >= p.count) {
    free(p.pages);
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page, "invalid document page");
  }
  double w, h;
  display(&p.pages[page], &w, &h);
  size_t width = (size_t)ceil(w * p.options.render.scale),
         height = (size_t)ceil(h * p.options.render.scale);
  snr_bitmap ink = {0},
             b = {.width = width, .height = height, .stride = width * 4};
  b.pixels = calloc(width * height, 4);
  if (!b.pixels)
    s = snr_fail(e, SNR_ERR_OUT_OF_MEMORY, page,
                 "source bitmap allocation failed");
  if (!s) s = snr_source_bitmap(pdf, size, page, p.count, &p.limits, &b, e);
  if (!s) s = ink_bitmap(n, page, &p, true, width, height, &ink, e);
  if (!s)
    for (size_t y = 0; y < height; y++) {
      if (snr_cancelled(&p.options.render)) {
        s = snr_fail(e, SNR_ERR_CANCELLED, page, "PDF overlay cancelled");
        break;
      }
      for (size_t x = 0; x < width; x++)
        for (size_t c = 0; c < 3; c++) {
          size_t i = y * width + x;
          b.pixels[i * 4 + c] =
              (uint8_t)((unsigned)b.pixels[i * 4 + c] *
                        (255u - ink.pixels[i * 4 + 3]) / 255u);
        }
    }
  if (!s && p.options.result) {
    bool annotated = false;
    s = snr_mark_page_has_annotations(n, page, &annotated, e);
    p.options.result->annotated_pages_drawn = annotated ? 1 : 0;
  }
  snr_bitmap_free(&ink);
  free(p.pages);
  if (s)
    snr_bitmap_free(&b);
  else
    *out = b;
  return s;
}
snr_status snr_mark_page_bitmap(const sn_notebook* n, const uint8_t* pdf,
                                size_t size, size_t page,
                                const snr_overlay_options* o, snr_bitmap* out,
                                snr_error* e) {
  return preview(n, pdf, size, page, o, out, e);
}
static snr_status png(const sn_notebook* n, const uint8_t* pdf, size_t size,
                      size_t page, const snr_overlay_options* o,
                      snr_buffer* out, const snr_output* output, snr_error* e) {
  if (!out && !output)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page, "null output");
  snr_status s = output ? snr_output_validate(output, e) : SNR_OK;
  if (s) return s;
  snr_bitmap b = {0};
  s = preview(n, pdf, size, page, o, &b, e);
  if (s) return s;
  snr_writer writer = {.output = output,
                       .limit = o ? o->render.limits.maximum_output_bytes
                                  : snr_limits_default().maximum_output_bytes};
  s = snr_encode_png(&b, &writer);
  snr_bitmap_free(&b);
  if (s) {
    snr_buffer_free(&writer.buffer);
    return snr_fail(e, s, page,
                    snr_writer_message(&writer) ? snr_writer_message(&writer)
                                                : "PNG output failed");
  }
  if (out) *out = writer.buffer;
  return SNR_OK;
}
snr_status snr_mark_page_png(const sn_notebook* n, const uint8_t* pdf,
                             size_t size, size_t page,
                             const snr_overlay_options* o, snr_buffer* out,
                             snr_error* e) {
  return png(n, pdf, size, page, o, out, nullptr, e);
}
snr_status snr_mark_page_png_to_output(const sn_notebook* n, const uint8_t* pdf,
                                       size_t size, size_t page,
                                       const snr_overlay_options* o,
                                       const snr_output* out, snr_error* e) {
  return png(n, pdf, size, page, o, nullptr, out, e);
}
/* The image PDF has one stream and isolated resources, so importing it cannot
 * alter source resources or exercise Cairo's PDF write closure. */
static void pdf_text(snr_writer* w, const char* fmt, ...) {
  char text[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof text) {
    if (!w->status) w->status = SNR_ERR_BACKEND;
    return;
  }
  (void)snr_write(w, text, (size_t)n);
}
static snr_status image_pdf(const snr_bitmap* b, size_t limit, snr_buffer* out,
                            snr_error* e) {
  size_t pixels = b->width * b->height;
  if (pixels > ULONG_MAX)
    return snr_fail(e, SNR_ERR_LIMIT, 0, "overlay image byte limit");
  uLongf bound = compressBound((uLong)pixels);
  uint8_t* raw = malloc(pixels);
  uint8_t* compressed = malloc((size_t)bound);
  if (!raw || !compressed) {
    free(raw);
    free(compressed);
    return snr_fail(e, SNR_ERR_OUT_OF_MEMORY, 0,
                    "overlay compression allocation failed");
  }
  snr_writer w = {.limit = limit};
  size_t offsets[7] = {0};
  pdf_text(&w, "%%PDF-1.4\n");
  offsets[1] = w.total;
  pdf_text(&w, "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n");
  offsets[2] = w.total;
  pdf_text(&w, "2 0 obj << /Type /Pages /Count 1 /Kids [3 0 R] >> endobj\n");
  offsets[3] = w.total;
  pdf_text(
      &w,
      "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 1 1] /Resources << "
      "/ExtGState << /Multiply << /BM /Multiply >> >> /XObject << /Ink 5 0 R "
      ">> >> /Contents 4 0 R >> endobj\n");
  offsets[4] = w.total;
  pdf_text(&w,
           "4 0 obj << /Length 21 >> stream\n/Multiply gs\n/Ink "
           "Do\n\nendstream endobj\n");
  for (unsigned id = 5; id <= 6; id++) {
    for (size_t i = 0; i < pixels; i++)
      raw[i] = id == 5 ? (uint8_t)(255 - b->pixels[i * 4 + 3])
                       : (b->pixels[i * 4 + 3] ? 255 : 0);
    uLongf length = bound;
    int compressed_status =
        compress2(compressed, &length, raw, (uLong)pixels, Z_BEST_SPEED);
    if (compressed_status != Z_OK) {
      if (!w.status)
        w.status = compressed_status == Z_MEM_ERROR ? SNR_ERR_OUT_OF_MEMORY
                                                    : SNR_ERR_BACKEND;
      break;
    }
    offsets[id] = w.total;
    pdf_text(&w,
             "%u 0 obj << /Type /XObject /Subtype /Image /Width %zu /Height "
             "%zu /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter "
             "/FlateDecode %s /Length %lu >> stream\n",
             id, b->width, b->height, id == 5 ? "/SMask 6 0 R" : "", length);
    (void)snr_write(&w, compressed, (size_t)length);
    pdf_text(&w, "\nendstream endobj\n");
  }
  size_t xref = w.total;
  pdf_text(&w, "xref\n0 7\n0000000000 65535 f \n");
  for (size_t i = 1; i < 7; i++) pdf_text(&w, "%010zu 00000 n \n", offsets[i]);
  pdf_text(&w, "trailer << /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
           xref);
  free(raw);
  free(compressed);
  if (w.status) {
    snr_buffer_free(&w.buffer);
    return snr_fail(e, w.status, 0,
                    snr_writer_message(&w) ? snr_writer_message(&w)
                    : w.status == SNR_ERR_OUT_OF_MEMORY
                        ? "overlay compression allocation failed"
                        : "overlay PDF encoding failed");
  }
  *out = w.buffer;
  return SNR_OK;
}
static snr_status forward(void* context, const uint8_t* bytes, size_t size) {
  return snr_write(context, bytes, size);
}
/* Undo the source's displayed rotation while retaining its actual page box. */
static spe_transform placement(const spe_pdf_page_geometry* g) {
  spe_rect r = g->crop;
  switch (g->rotate) {
    case 90:
      return (spe_transform){0, r.height, -r.width, 0, r.x + r.width, r.y};
    case 180:
      return (spe_transform){-r.width,      0, 0, -r.height, r.x + r.width,
                             r.y + r.height};
    case 270:
      return (spe_transform){0, -r.height, r.width, 0, r.x, r.y + r.height};
    default:
      return (spe_transform){r.width, 0, 0, r.height, r.x, r.y};
  }
}
static snr_status document_overlay(const sn_notebook* n, const uint8_t* pdf,
                                   size_t size, size_t first, size_t count,
                                   const snr_overlay_options* options,
                                   snr_buffer* out, const snr_output* output,
                                   snr_error* e) {
  if (!out && !output)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, first, "null output");
  snr_status s = output ? snr_output_validate(output, e) : SNR_OK;
  if (s) return s;
  overlay_plan p = {0};
  s = prepare(n, pdf, size, options, false, &p, e);
  if (s) return s;
  if (first >= p.count || !count ||
      (count != SIZE_MAX && count > p.count - first)) {
    free(p.pages);
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, first, "invalid page range");
  }
  if (count == SIZE_MAX) count = p.count - first;
  snr_buffer selected = {0};
  if (first || count != p.count) {
    size_t* indices = calloc(count, sizeof *indices);
    if (!indices)
      s = snr_fail(e, SNR_ERR_OUT_OF_MEMORY, first,
                   "PDF selection allocation failed");
    else {
      for (size_t i = 0; i < count; i++) indices[i] = first + i;
      s = spe_pdf_select_pages(pdf, size, indices, count, &p.limits,
                               p.options.render.cancel,
                               p.options.render.cancel_context, &selected, e);
      free(indices);
      if (!s) {
        pdf = selected.bytes;
        size = selected.size;
      }
    }
  }
  snr_options* o = &p.options.render;
  snr_writer writer = {.output = output,
                       .limit = o->limits.maximum_output_bytes};
  snr_output sink = {SNR_OUTPUT_ABI, sizeof sink, forward, &writer};
  spe_pdf_update* update = nullptr;
  if (!s)
    s = spe_pdf_update_create(pdf, size, false, &p.limits, o->cancel,
                              o->cancel_context, &sink, &update, e);
  if (!s && o->progress) o->progress(o->progress_context, 0, count);
  for (size_t i = 0; !s && i < count; i++) {
    size_t page = first + i;
    if (snr_cancelled(o)) {
      s = snr_fail(e, SNR_ERR_CANCELLED, page, "PDF overlay cancelled");
      break;
    }
    bool annotated = false;
    s = snr_mark_page_has_annotations(n, page, &annotated, e);
    if (!s && annotated) {
      double w, h;
      display(&p.pages[page], &w, &h);
      size_t index = 0;
      const sn_page* pg = sheet(n, page, &index);
      double density =
          fit_sheet(pg ? pg->size : n->sheet, &p.pages[page]).scale * o->scale;
      double ww = ceil(w * density), hh = ceil(h * density);
      if (!isfinite(ww * hh) || ww * hh > (double)o->limits.maximum_pixels ||
          ww * hh > (double)(SIZE_MAX / 4))
        s = snr_fail(e, SNR_ERR_LIMIT, page, "overlay image pixel limit");
      snr_bitmap ink = {0};
      snr_buffer encoded = {0};
      if (!s)
        s = ink_bitmap(n, page, &p, false, (size_t)ww, (size_t)hh, &ink, e);
      if (!s) s = image_pdf(&ink, o->limits.maximum_output_bytes, &encoded, e);
      snr_bitmap_free(&ink);
      if (!s) {
        spe_pdf_overlay overlay = {.page = i,
                                   .pdf = encoded.bytes,
                                   .size = encoded.size,
                                   .transform = placement(&p.pages[page])};
        s = spe_pdf_update_page(update, &overlay, 1, e);
        if (!s && p.options.result) p.options.result->annotated_pages_drawn++;
      }
      if (s && e) e->index = page;
      snr_buffer_free(&encoded);
    }
    if (!s && o->progress) o->progress(o->progress_context, i + 1, count);
  }
  if (!s) s = spe_pdf_update_finish(update, nullptr, nullptr, e);
  spe_pdf_update_free(update);
  snr_buffer_free(&selected);
  free(p.pages);
  if (writer.status)
    s = snr_fail(e, writer.status, first, snr_writer_message(&writer));
  if (s)
    snr_buffer_free(&writer.buffer);
  else if (out)
    *out = writer.buffer;
  return s;
}
snr_status snr_mark_document_pdf(const sn_notebook* n, const uint8_t* pdf,
                                 size_t size, size_t first, size_t count,
                                 const snr_overlay_options* o, snr_buffer* out,
                                 snr_error* e) {
  return document_overlay(n, pdf, size, first, count, o, out, nullptr, e);
}
snr_status snr_mark_document_pdf_to_output(const sn_notebook* n,
                                           const uint8_t* pdf, size_t size,
                                           size_t first, size_t count,
                                           const snr_overlay_options* o,
                                           const snr_output* out,
                                           snr_error* e) {
  return document_overlay(n, pdf, size, first, count, o, nullptr, out, e);
}
