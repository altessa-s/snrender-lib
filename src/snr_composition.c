/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "snr_internal.h"
/* Compose straight channels before premultiplying. Integer divisions follow
 * the palette's coverage rule so opaque export pixels retain their exact gray.
 */
static snr_status compose(const snr_plan* p, snr_bitmap* out, snr_error* e,
                          bool premultiply) {
  size_t count = p->source_width * p->source_height;
  uint8_t* pixels = malloc(count * 4);
  if (!pixels)
    return snr_fail(e, SNR_ERR_OUT_OF_MEMORY, p->index, "page allocation");
  for (size_t i = 0; i < count; i++) {
    memset(pixels + i * 4, 255, 3);
    pixels[i * 4 + 3] = 0;
  }
  snr_status s = SNR_OK;
  for (size_t l = p->page->layer_count; l > 0; l--) {
    const sn_layer* layer = p->page->layers + l - 1;
    if (!layer->is_visible || !layer->bitmap ||
        (!p->options.include_background && layer->name == SN_LAYER_BACKGROUND))
      continue;
    if (snr_cancelled(&p->options)) {
      s = SNR_ERR_CANCELLED;
      break;
    }
    sn_raster* r = nullptr;
    snr_bitmap b = {0};
    if (layer->bitmap->encoding == SN_ENCODING_RATTA_RLE) {
      sn_status rs =
          sn_bitmap_raster(layer->bitmap, p->source_width, p->source_height,
                           &p->notebook->palette, &r, nullptr);
      s = rs == SN_OK                  ? SNR_OK
          : rs == SN_ERR_LIMIT         ? SNR_ERR_LIMIT
          : rs == SN_ERR_OUT_OF_MEMORY ? SNR_ERR_OUT_OF_MEMORY
                                       : SNR_ERR_BACKEND;
    } else if (layer->bitmap->encoding == SN_ENCODING_PNG ||
               layer->bitmap->encoding == SN_ENCODING_JPEG)
      s = snr_decode(layer->bitmap, p->options.limits.maximum_decoded_pixels,
                     &p->options, &b);
    else
      s = SNR_ERR_UNSUPPORTED;
    if (s) {
      sn_raster_free(r);
      snr_bitmap_free(&b);
      if (!p->options.strict_images && s == SNR_ERR_BACKEND) {
        snr_log(SNR_LOG_ERROR, "damaged image layer omitted");
        s = SNR_OK;
        continue;
      }
      break;
    }
    for (size_t y = 0; y < p->source_height; y++) {
      if (snr_cancelled(&p->options)) {
        s = SNR_ERR_CANCELLED;
        break;
      }
      for (size_t x = 0; x < p->source_width; x++) {
        size_t i = y * p->source_width + x;
        unsigned a;
        unsigned rgb[3];
        if (r) {
          a = r->alpha[i];
          rgb[0] = rgb[1] = rgb[2] = r->gray[i];
        } else {
          size_t q =
              ((size_t)((uint64_t)y * b.height / p->source_height)) * b.stride +
              ((size_t)((uint64_t)x * b.width / p->source_width)) * 4;
          a = b.pixels[q + 3];
          for (size_t c = 0; c < 3; c++)
            rgb[c] = a ? (unsigned)b.pixels[q + c] * 255 / a : 255;
        }
        uint8_t* d = pixels + i * 4;
        /* Use the same quantized destination coverage in numerator and
         * denominator: independently truncated products can exceed 255. */
        unsigned ba = d[3], covered = a + ba * (255 - a) / 255;
        if (a) {
          for (size_t c = 0; c < 3; c++)
            d[c] = (uint8_t)((rgb[c] * a + (unsigned)d[c] * (covered - a)) /
                             covered);
          d[3] = (uint8_t)covered;
        }
      }
    }
    sn_raster_free(r);
    snr_bitmap_free(&b);
    if (s) break;
  }
  snr_bitmap result = {0};
  if (!s) {
    result = (snr_bitmap){
        .width = p->width, .height = p->height, .stride = p->width * 4};
    result.pixels = malloc(p->width * p->height * 4);
    if (!result.pixels) s = SNR_ERR_OUT_OF_MEMORY;
  }
  snr_color bg =
      p->options.has_background
          ? p->options.background
          : (snr_color){1, 1, 1, p->notebook->kind == SN_KIND_MARK ? 0 : 1};
  unsigned bc[3] = {(unsigned)lround(bg.red * 255),
                    (unsigned)lround(bg.green * 255),
                    (unsigned)lround(bg.blue * 255)},
           ba = (unsigned)lround(bg.alpha * 255);
  if (!s)
    for (size_t y = 0; y < p->height; y++) {
      if (snr_cancelled(&p->options)) {
        s = SNR_ERR_CANCELLED;
        break;
      }
      for (size_t x = 0; x < p->width; x++) {
        if (p->options.scale < 1) {
          /* Integer overlap lengths share the output grid's denominator.
           * Averaging covered channels prevents transparent edges darkening. */
          uint64_t left = (uint64_t)x * p->source_width;
          uint64_t right = (uint64_t)(x + 1) * p->source_width;
          uint64_t top = (uint64_t)y * p->source_height;
          uint64_t bottom = (uint64_t)(y + 1) * p->source_height;
          uint64_t sum[4] = {0};
          for (size_t sy = (size_t)(top / p->height);
               sy < (bottom + p->height - 1) / p->height; sy++) {
            uint64_t yt = (uint64_t)sy * p->height, yb = yt + p->height;
            uint64_t wy = (bottom < yb ? bottom : yb) - (top > yt ? top : yt);
            for (size_t sx = (size_t)(left / p->width);
                 sx < (right + p->width - 1) / p->width; sx++) {
              uint64_t xl = (uint64_t)sx * p->width, xr = xl + p->width;
              uint64_t weight =
                  wy * ((right < xr ? right : xr) - (left > xl ? left : xl));
              const uint8_t* d = pixels + (sy * p->source_width + sx) * 4;
              unsigned a = d[3], covered = a + ba * (255 - a) / 255;
              for (size_t c = 0; c < 3; c++)
                sum[c] += weight * ((unsigned)d[c] * a + bc[c] * (covered - a));
              sum[3] += weight * covered;
            }
          }
          uint64_t area = (uint64_t)p->source_width * p->source_height;
          uint8_t* dst = result.pixels + y * result.stride + x * 4;
          unsigned alpha = (unsigned)((sum[3] + area / 2) / area);
          for (size_t c = 0; c < 3; c++) {
            uint64_t denominator = premultiply ? area * 255 : sum[3];
            dst[c] = denominator
                         ? (uint8_t)((sum[c] + denominator / 2) / denominator)
                         : 0;
          }
          if (!premultiply && sum[3]) {
            /* Retain luminance precision until the straight gray division;
             * rounding RGB first can move gray by more than half a level. */
            /* Each dimension is at most 2^24, so area is at most 2^48.
             * A channel sum is at most 65025*area. Split the 8-bit
             * coefficients before multiplying: high plus rounding is below
             * (65025+128)*2^48, hence below UINT64_MAX on every target. */
            uint64_t high =
                77 * (sum[0] >> 8) + 150 * (sum[1] >> 8) + 29 * (sum[2] >> 8);
            uint64_t low = 77 * (sum[0] & 255) + 150 * (sum[1] & 255) +
                           29 * (sum[2] & 255);
            high += low >> 8;
            uint64_t rounded =
                high + sum[3] / 2 + ((low & 255) + (sum[3] & 1) * 128) / 256;
            uint8_t gray = (uint8_t)(rounded / sum[3]);
            dst[0] = dst[1] = dst[2] = gray;
          }
          dst[3] = (uint8_t)alpha;
          continue;
        }
        const uint8_t* d =
            pixels + (((size_t)((uint64_t)y * p->source_height / p->height)) *
                          p->source_width +
                      (size_t)((uint64_t)x * p->source_width / p->width)) *
                         4;
        uint8_t* dst = result.pixels + y * result.stride + x * 4;
        unsigned a = d[3], covered = a + ba * (255 - a) / 255;
        for (size_t c = 0; c < 3; c++) {
          unsigned straight =
              covered ? ((unsigned)d[c] * a + bc[c] * (covered - a)) / covered
                      : 0;
          dst[c] = (uint8_t)(premultiply ? straight * covered / 255 : straight);
        }
        dst[3] = (uint8_t)covered;
      }
    }
  free(pixels);
  if (s) {
    snr_bitmap_free(&result);
    return snr_fail(e, s, p->index,
                    s == SNR_ERR_CANCELLED ? "composition cancelled"
                    : s == SNR_ERR_LIMIT   ? "image decode limit"
                    : s == SNR_ERR_OUT_OF_MEMORY
                        ? "composition allocation failed"
                        : "image layer decode failed");
  }
  *out = result;
  return SNR_OK;
}
snr_status snr_compose(const snr_plan* p, snr_bitmap* out, snr_error* e) {
  return compose(p, out, e, true);
}
snr_status snr_page_bitmap(const sn_notebook* n, size_t page,
                           const snr_options* o, snr_bitmap* out,
                           snr_error* e) {
  if (!out) return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page, "null output");
  snr_plan p;
  snr_status s = snr_image_plan(n, page, o, &p, e);
  return s ? s : snr_compose(&p, out, e);
}
snr_status snr_page_raster(const sn_notebook* n, size_t page,
                           const snr_options* o, snr_raster* out,
                           snr_error* e) {
  if (!out) return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, page, "null output");
  snr_bitmap b = {0};
  snr_plan plan;
  snr_status s = snr_image_plan(n, page, o, &plan, e);
  if (!s) s = compose(&plan, &b, e, false);
  if (s) return s;
  size_t count = b.width * b.height;
  snr_raster r = {.width = b.width,
                  .height = b.height,
                  .gray = malloc(count),
                  .alpha = malloc(count)};
  if (!r.gray || !r.alpha) {
    snr_raster_free(&r);
    snr_bitmap_free(&b);
    return snr_fail(e, SNR_ERR_OUT_OF_MEMORY, page, "gray allocation");
  }
  for (size_t i = 0; i < count; i++) {
    unsigned a = b.pixels[i * 4 + 3];
    unsigned gray = (77u * b.pixels[i * 4] + 150u * b.pixels[i * 4 + 1] +
                     29u * b.pixels[i * 4 + 2]) /
                    256;
    r.gray[i] = (uint8_t)(a ? gray : 255);
    r.alpha[i] = (uint8_t)a;
  }
  snr_bitmap_free(&b);
  *out = r;
  return SNR_OK;
}
