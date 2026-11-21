/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include "snr_internal.h"
#ifdef SNR_SPD
#include <snspd/snspd.h>
#include <zlib.h>
#endif

snr_spd_options snr_spd_options_default(void) {
  return (snr_spd_options){.abi_version = SNR_SPD_ABI,
                           .struct_size = sizeof(snr_spd_options),
                           .render = snr_options_default(),
                           .page = SNR_SPD_PAINTED_BOUNDS,
                           .include_reference_layer = true,
                           .maximum_tiles = 262144,
                           .maximum_decoded_bytes = 268435456};
}
#ifdef SNR_SPD
typedef struct {
  snr_spd_options options;
  size_t width, height, source_width, source_height, work, decoded;
  double x, y, page_width, page_height, ppi, vx, vy, zoom;
} spd_plan;

static snr_status charge(spd_plan* p, size_t n) {
  if (n > p->options.render.limits.maximum_work - p->work) return SNR_ERR_LIMIT;
  p->work += n;
  return SNR_OK;
}
static bool selected(const spd_plan* p, const spd_layer* l) {
  return p->options.include_reference_layer || l->id != 9999;
}
static void tile_origin(const spd_plan* p, const spd_tile* t, double* x,
                        double* y) {
  *x = ((double)t->tx * 128 - p->vx) * p->zoom;
  *y = ((double)t->ty * 128 - p->vy) * p->zoom;
}
static sn_bitmap tile_image(const spd_tile* t) {
  return (sn_bitmap){.encoding = SN_ENCODING_PNG,
                     .bytes = {.data = t->png.data, .size = t->png.size}};
}
static snr_status tile_size(const spd_tile* t) {
  sn_bitmap image = tile_image(t);
  size_t w, h;
  snr_status s = snr_image_size(&image, 128 * 128, &w, &h);
  if (s) return s;
  if (w > 128 || h > 128) return SNR_ERR_LIMIT;
  return w == 128 && h == 128 ? SNR_OK : SNR_ERR_BACKEND;
}
/* ImageIO can report a complete image after a damaged IDAT decode. Validate
 * the bounded scanline stream before the platform codec, without reconstructing
 * any pixels. zlib is already a renderer dependency. Adam7 has at most 240
 * rows for a 128-square gray+alpha tile (33008 scanline bytes). */
static uint32_t png_u32(const uint8_t* b) {
  return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 |
         b[3];
}
static snr_status validate_tile(spd_plan* p, const spd_tile* t) {
  sn_bitmap image = tile_image(t);
  size_t size;
  snr_status s = snr_image_frame(&image, &p->options.render, &size);
  if (s) return s;
  const uint8_t* bytes = t->png.data;
  /* Atelier writes GA8. Excluding other pixel formats and every ancillary
   * chunk keeps colour management, palette errors and metadata parsers out of
   * the platform-dependent contract. No chunks may separate IDATs. */
  if (bytes[24] != 8 || bytes[25] != 4 || bytes[26] || bytes[27] ||
      bytes[28] > 1)
    return SNR_ERR_BACKEND;
  for (size_t offset = 33; offset < size;) {
    const uint8_t* tag = bytes + offset + 4;
    if (memcmp(tag, "IDAT", 4) && memcmp(tag, "IEND", 4))
      return SNR_ERR_BACKEND;
    offset += (size_t)png_u32(bytes + offset) + 12;
  }
  static const unsigned xstart[] = {0, 4, 0, 2, 0, 1, 0},
                        ystart[] = {0, 0, 4, 0, 2, 0, 1};
  static const unsigned xstep[] = {8, 8, 4, 4, 2, 2, 1},
                        ystep[] = {8, 8, 8, 4, 4, 2, 2};
  size_t filters[256], rows = 0, expected = 0;
  for (size_t pass = 0; pass < (bytes[28] ? 7u : 1u); pass++) {
    size_t w =
        bytes[28] ? (128 - xstart[pass] + xstep[pass] - 1) / xstep[pass] : 128;
    size_t h =
        bytes[28] ? (128 - ystart[pass] + ystep[pass] - 1) / ystep[pass] : 128;
    size_t row = w * 2 + 1;
    for (size_t y = 0; y < h; y++) {
      filters[rows++] = expected;
      expected += row;
    }
  }
  z_stream stream = {0};
  int initialized = inflateInit(&stream);
  if (initialized != Z_OK)
    return initialized == Z_MEM_ERROR ? SNR_ERR_OUT_OF_MEMORY : SNR_ERR_BACKEND;
  bool ended = false;
  size_t filter = 0;
  uint8_t output[4096];
  for (size_t offset = 8; !s && offset < size;) {
    size_t length = png_u32(bytes + offset);
    if (!memcmp(bytes + offset + 4, "IDAT", 4)) {
      if (ended && length) {
        s = SNR_ERR_BACKEND;
        break;
      }
      /* zlib does not modify input despite its historical mutable pointer. */
      stream.next_in = (Bytef*)(uintptr_t)(bytes + offset + 8);
      stream.avail_in = (uInt)length;
      if (length) do {
          if (snr_cancelled(&p->options.render)) {
            s = SNR_ERR_CANCELLED;
            break;
          }
          size_t before = (size_t)stream.total_out;
          stream.next_out = output;
          stream.avail_out = sizeof output;
          int result = inflate(&stream, Z_NO_FLUSH);
          size_t after = (size_t)stream.total_out;
          if (after > expected) {
            s = SNR_ERR_BACKEND;
            break;
          }
          while (filter < rows && filters[filter] < after) {
            if (output[filters[filter] - before] > 4) {
              s = SNR_ERR_BACKEND;
              break;
            }
            filter++;
          }
          if (s) break;
          if (result == Z_STREAM_END) {
            ended = true;
            if (stream.avail_in) s = SNR_ERR_BACKEND;
            break;
          }
          if (result == Z_BUF_ERROR && !stream.avail_in && after == before)
            break; /* More compressed bytes may arrive in the next IDAT. */
          if (result != Z_OK) {
            s = result == Z_MEM_ERROR ? SNR_ERR_OUT_OF_MEMORY : SNR_ERR_BACKEND;
            break;
          }
        } while (stream.avail_in || !stream.avail_out);
    }
    offset += length + 12;
  }
  if (!s && (!ended || stream.total_out != expected)) s = SNR_ERR_BACKEND;
  inflateEnd(&stream);
  return s;
}

static snr_status make_plan(const spd_drawing* d, const snr_spd_options* input,
                            spd_plan* p, snr_error* e) {
  *p = (spd_plan){.options = snr_spd_options_default()};
  if (input) {
    if (input->abi_version != SNR_SPD_ABI || input->struct_size < sizeof *input)
      return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0,
                      "invalid drawing options ABI");
    p->options = *input;
  }
  snr_spd_options* o = &p->options;
  if (!d || (d->layer_count && !d->layers) || !isfinite(o->dpi) || o->dpi < 0 ||
      (o->page != SNR_SPD_CANVAS && o->page != SNR_SPD_PAINTED_BOUNDS))
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0,
                    "invalid drawing or options");
  const spd_canvas* c = &d->canvas;
  p->ppi = c->has_ppi ? c->ppi : 300;
  p->vx = c->has_viewport_x ? c->viewport_x : 0;
  p->vy = c->has_viewport_y ? c->viewport_y : 0;
  p->zoom = c->has_viewport_scale ? c->viewport_scale : 1;
  if (!isfinite(p->ppi) || p->ppi <= 0 || !isfinite(p->vx) ||
      !isfinite(p->vy) || !isfinite(p->zoom) || p->zoom <= 0)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0,
                    "invalid drawing viewport or ppi");
  if (o->dpi != 0) {
    o->render.scale = o->dpi / p->ppi;
    if (!isfinite(o->render.scale) || o->render.scale <= 0)
      return snr_fail(e, SNR_ERR_LIMIT, 0, "drawing DPI scale limit");
  }
  snr_options checked;
  snr_status s = snr_options_read(&o->render, &checked, e);
  if (s) return s;
  o->render = checked;
  if (o->render.limits.maximum_document_work < o->render.limits.maximum_work)
    o->render.limits.maximum_work =
        (size_t)o->render.limits.maximum_document_work;
  if (snr_cancelled(&o->render))
    return snr_fail(e, SNR_ERR_CANCELLED, 0, "drawing cancelled");
  double right = 0, bottom = 0;
  bool bounds = false;
  size_t count = 0, valid_tiles = 0;
  s = charge(p, d->layer_count);
  if (s) return snr_fail(e, s, 0, "drawing layer work limit");
  for (size_t i = 0; i < d->layer_count; i++) {
    if (snr_cancelled(&o->render))
      return snr_fail(e, SNR_ERR_CANCELLED, 0, "drawing cancelled");
    const spd_layer* l = d->layers + i;
    if (l->tile_count && !l->tiles)
      return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0,
                      "invalid drawing tile array");
    if (l->tile_count > o->maximum_tiles - count || charge(p, l->tile_count))
      return snr_fail(e, SNR_ERR_LIMIT, 0, "drawing tile or work limit");
    count += l->tile_count;
    if (!selected(p, l)) continue;
    for (size_t j = 0; j < l->tile_count; j++) {
      if (snr_cancelled(&o->render))
        return snr_fail(e, SNR_ERR_CANCELLED, j, "drawing cancelled");
      const spd_tile* t = l->tiles + j;
      double x, y;
      tile_origin(p, t, &x, &y);
      double r = x + 128 * p->zoom, b = y + 128 * p->zoom;
      if (!isfinite(x) || !isfinite(y) || !isfinite(r) || !isfinite(b) ||
          r <= x || b <= y)
        return snr_fail(e, SNR_ERR_LIMIT, j,
                        "drawing coordinate precision limit");
      if (!bounds) {
        p->x = x;
        p->y = y;
        right = r;
        bottom = b;
        bounds = true;
      } else {
        p->x = fmin(p->x, x);
        p->y = fmin(p->y, y);
        right = fmax(right, r);
        bottom = fmax(bottom, b);
      }
      if (charge(p, t->png.size))
        return snr_fail(e, SNR_ERR_LIMIT, j, "drawing encoded work limit");
      s = tile_size(t);
      if (s && o->render.strict_images)
        return snr_fail(e, s, j, "drawing tile dimensions rejected");
      if (s) continue;
      /* Reserve all encoded passes: validator CRC, backend CRC, and codec
       * input. Also reserve the largest GA8 scanline stream (Adam7), before
       * either page allocation. Damaged tiles may consume less work. */
      if (charge(p, t->png.size) || charge(p, t->png.size) ||
          charge(p, 33008) || 65536 > o->maximum_decoded_bytes - p->decoded ||
          charge(p, 16384))
        return snr_fail(e, SNR_ERR_LIMIT, j,
                        "drawing decoded bytes or work limit");
      p->decoded += 65536;
      valid_tiles++;
    }
  }
  if (o->page == SNR_SPD_CANVAS) {
    if (!c->has_width || !c->has_height || !isfinite(c->width) ||
        !isfinite(c->height) || c->width <= 0 || c->height <= 0)
      return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0,
                      "missing or invalid drawing canvas");
    p->page_width = c->width;
    p->page_height = c->height;
    /* Saved viewport is inferred to be the page centre; see README.
     * Tile offsets are already scaled about that point. */
    p->x = -c->width / 2;
    p->y = -c->height / 2;
  } else {
    p->page_width = bounds ? right - p->x : 1;
    p->page_height = bounds ? bottom - p->y : 1;
  }
  double sw = ceil(p->page_width), sh = ceil(p->page_height);
  double w = ceil(p->page_width * o->render.scale),
         h = ceil(p->page_height * o->render.scale);
  if (!isfinite(w) || !isfinite(h) || sw > 16777216 || sh > 16777216 ||
      w > 16777216 || h > 16777216 || w < 1 || h < 1)
    return snr_fail(e, SNR_ERR_LIMIT, 0, "drawing dimension limit");
#ifdef SNR_CAIRO_BACKEND
  if (w > 32767 || h > 32767)
    return snr_fail(e, SNR_ERR_LIMIT, 0, "Cairo dimension limit");
#endif
  p->source_width = (size_t)sw;
  p->source_height = (size_t)sh;
  p->width = (size_t)w;
  p->height = (size_t)h;
  const snr_limits* limits = &o->render.limits;
  if (p->source_width > limits->maximum_decoded_pixels / p->source_height ||
      p->width > limits->maximum_pixels / p->height ||
      p->source_width > SIZE_MAX / 4 / p->source_height ||
      p->width > SIZE_MAX / 4 / p->height)
    return snr_fail(e, SNR_ERR_LIMIT, 0, "drawing pixel limit");
  size_t source = p->source_width * p->source_height,
         output = p->width * p->height;
  if (source * 4 > o->maximum_decoded_bytes - p->decoded)
    return snr_fail(e, SNR_ERR_LIMIT, 0, "drawing source byte limit");
  p->decoded += source * 4;
  if (output * 4 > o->maximum_decoded_bytes - p->decoded || charge(p, source) ||
      charge(p, source) || charge(p, source) || charge(p, source) ||
      charge(p, output))
    return snr_fail(e, SNR_ERR_LIMIT, 0, "drawing output bytes or work limit");
  /* Each dimension-valid tile can visit at most ceil(edge) samples per
   * axis, clipped to the source page. Reserve this bound for every tile;
   * overlap, off-page placement and invalid streams only reduce actual work. */
  size_t bx = (size_t)fmin(sw, ceil(128 * p->zoom)),
         by = (size_t)fmin(sh, ceil(128 * p->zoom));
  size_t blend = bx * by;
  if (blend && valid_tiles > (limits->maximum_work - p->work) / blend)
    return snr_fail(e, SNR_ERR_LIMIT, 0, "drawing blend work limit");
  p->work += valid_tiles * blend;
  return SNR_OK;
}

/* Box average premultiplied channels when shrinking; nearest when enlarging. */
static snr_status resize(const snr_bitmap* src, const spd_plan* p,
                         snr_bitmap* out) {
  snr_bitmap b = {
      .width = p->width, .height = p->height, .stride = p->width * 4};
  b.pixels = malloc(b.stride * b.height);
  if (!b.pixels) return SNR_ERR_OUT_OF_MEMORY;
  for (size_t y = 0; y < b.height; y++) {
    if (snr_cancelled(&p->options.render)) {
      snr_bitmap_free(&b);
      return SNR_ERR_CANCELLED;
    }
    for (size_t x = 0; x < b.width; x++) {
      uint8_t* dst = b.pixels + y * b.stride + x * 4;
      if (b.width >= src->width && b.height >= src->height) {
        size_t sx = x * src->width / b.width, sy = y * src->height / b.height;
        memcpy(dst, src->pixels + sy * src->stride + sx * 4, 4);
        continue;
      }
      uint64_t left = (uint64_t)x * src->width,
               right = (uint64_t)(x + 1) * src->width;
      uint64_t top = (uint64_t)y * src->height,
               bottom = (uint64_t)(y + 1) * src->height, sum[4] = {0};
      for (size_t sy = (size_t)(top / b.height);
           sy < (bottom + b.height - 1) / b.height; sy++) {
        uint64_t yt = (uint64_t)sy * b.height, yb = yt + b.height;
        uint64_t wy = (bottom < yb ? bottom : yb) - (top > yt ? top : yt);
        for (size_t sx = (size_t)(left / b.width);
             sx < (right + b.width - 1) / b.width; sx++) {
          uint64_t xl = (uint64_t)sx * b.width, xr = xl + b.width;
          uint64_t weight =
              wy * ((right < xr ? right : xr) - (left > xl ? left : xl));
          const uint8_t* a = src->pixels + sy * src->stride + sx * 4;
          for (size_t k = 0; k < 4; k++) sum[k] += weight * a[k];
        }
      }
      uint64_t area = src->width * src->height;
      for (size_t k = 0; k < 4; k++)
        dst[k] = (uint8_t)((sum[k] + area / 2) / area);
    }
  }
  *out = b;
  return SNR_OK;
}
static snr_status compose(const spd_drawing* d, spd_plan* p, snr_bitmap* out,
                          snr_error* e) {
  snr_bitmap b = {.width = p->source_width,
                  .height = p->source_height,
                  .stride = p->source_width * 4};
  b.pixels = malloc(b.height * b.stride);
  if (!b.pixels)
    return snr_fail(e, SNR_ERR_OUT_OF_MEMORY, 0, "drawing allocation failed");
  snr_color bg = p->options.render.has_background ? p->options.render.background
                                                  : (snr_color){1, 1, 1, 1};
  uint8_t rgba[4] = {(uint8_t)lround(bg.red * bg.alpha * 255),
                     (uint8_t)lround(bg.green * bg.alpha * 255),
                     (uint8_t)lround(bg.blue * bg.alpha * 255),
                     (uint8_t)lround(bg.alpha * 255)};
  snr_status s = SNR_OK;
  for (size_t y = 0; y < b.height; y++) {
    if (snr_cancelled(&p->options.render)) {
      s = SNR_ERR_CANCELLED;
      break;
    }
    for (size_t x = 0; x < b.width; x++)
      memcpy(b.pixels + y * b.stride + x * 4, rgba, 4);
  }
  size_t index = 0;
  for (size_t i = 0; !s && i < d->layer_count; i++) {
    const spd_layer* l = d->layers + i;
    if (!selected(p, l)) continue;
    for (size_t j = 0; !s && j < l->tile_count; j++) {
      index = j;
      if (snr_cancelled(&p->options.render)) {
        s = SNR_ERR_CANCELLED;
        break;
      }
      const spd_tile* t = l->tiles + j;
      s = tile_size(t);
      /* Dimension failures are image errors in skip mode, including oversized
       * headers. Never suppress resource failures from validation/decoding. */
      if (s && !p->options.render.strict_images) {
        snr_log(SNR_LOG_ERROR, "drawing tile dimensions omitted");
        s = SNR_OK;
        continue;
      }
      snr_bitmap tile = {0};
      if (!s) s = validate_tile(p, t);
      if (!s) {
        sn_bitmap image = tile_image(t);
        s = snr_decode(&image, 16384, &p->options.render, &tile);
      }
      if (s) {
        snr_bitmap_free(&tile);
        if (s == SNR_ERR_BACKEND && !p->options.render.strict_images) {
          snr_log(SNR_LOG_ERROR, "damaged drawing tile omitted");
          s = SNR_OK;
          continue;
        }
        break;
      }
      double ox, oy;
      tile_origin(p, t, &ox, &oy);
      ox -= p->x;
      oy -= p->y;
      double edge = 128 * p->zoom;
      size_t x0 = (size_t)fmin((double)b.width, fmax(0, ceil(ox - .5)));
      size_t y0 = (size_t)fmin((double)b.height, fmax(0, ceil(oy - .5)));
      size_t x1 = (size_t)fmin((double)b.width, fmax(0, ceil(ox + edge - .5)));
      size_t y1 = (size_t)fmin((double)b.height, fmax(0, ceil(oy + edge - .5)));
      for (size_t y = y0; !s && y < y1; y++) {
        if (snr_cancelled(&p->options.render)) {
          s = SNR_ERR_CANCELLED;
          break;
        }
        size_t sy =
            (size_t)fmin(127, fmax(0, floor(((double)y + .5 - oy) / p->zoom)));
        for (size_t x = x0; x < x1; x++) {
          size_t sx = (size_t)fmin(
              127, fmax(0, floor(((double)x + .5 - ox) / p->zoom)));
          const uint8_t* a = tile.pixels + sy * tile.stride + sx * 4;
          uint8_t* dst = b.pixels + y * b.stride + x * 4;
          for (size_t k = 0; k < 4; k++)
            dst[k] =
                (uint8_t)(a[k] + ((unsigned)dst[k] * (255 - a[3]) + 127) / 255);
        }
      }
      snr_bitmap_free(&tile);
    }
  }
  if (!s) {
    index = 0; /* Resampling failures describe the whole page. */
    s = resize(&b, p, out);
  }
  snr_bitmap_free(&b);
  return s ? snr_fail(e, s, index, "drawing composition failed") : SNR_OK;
}
#endif

snr_status snr_spd_bitmap(const spd_drawing* d, const snr_spd_options* o,
                          snr_bitmap* out, snr_error* e) {
#ifdef SNR_SPD
  if (!out)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0, "null drawing output");
  spd_plan p;
  snr_status s = make_plan(d, o, &p, e);
  return s ? s : compose(d, &p, out, e);
#else
  (void)d;
  (void)o;
  (void)out;
  return snr_fail(e, SNR_ERR_UNSUPPORTED, 0, "Atelier support disabled");
#endif
}
static snr_status drawing_output(const spd_drawing* d, const snr_spd_options* o,
                                 snr_buffer* out, const snr_output* output,
                                 snr_error* e, unsigned format) {
#ifdef SNR_SPD
  if (!out && !output)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0, "null drawing output");
  snr_status s = output ? snr_output_validate(output, e) : SNR_OK;
  spd_plan p;
  if (!s) s = make_plan(d, o, &p, e);
  if (s) return s;
  double pw = 0, ph = 0;
  if (format == 2) {
    pw = p.page_width * 72 / p.ppi;
    ph = p.page_height * 72 / p.ppi;
    if (!p.options.render.limits.maximum_pages || !isfinite(pw) ||
        !isfinite(ph) || pw <= 0 || ph <= 0 || pw > 14400 || ph > 14400)
      return snr_fail(e, SNR_ERR_LIMIT, 0, "drawing PDF page limit");
    pw = fmax(3, pw);
    ph = fmax(3, ph);
  }
  snr_bitmap b = {0};
  s = compose(d, &p, &b, e);
  if (s) return s;
  snr_writer w = {.output = output,
                  .limit = p.options.render.limits.maximum_output_bytes};
  if (snr_cancelled(&p.options.render))
    s = SNR_ERR_CANCELLED;
  else if (format == 0)
    s = snr_encode_png(&b, &w);
  else if (format == 1)
    s = snr_encode_svg(&b, p.page_width, p.page_height, &p.options.render, &w);
  else
    s = snr_encode_pdf_bitmap(&b, pw, ph, &w);
  snr_bitmap_free(&b);
  if (!s && snr_cancelled(&p.options.render)) s = SNR_ERR_CANCELLED;
  if (s) {
    snr_buffer_free(&w.buffer);
    const char* message = w.status == s ? snr_writer_message(&w) : nullptr;
    return snr_fail(e, s, 0, message ? message : "drawing output failed");
  }
  if (out) *out = w.buffer;
  return SNR_OK;
#else
  (void)d;
  (void)o;
  (void)out;
  (void)output;
  (void)format;
  return snr_fail(e, SNR_ERR_UNSUPPORTED, 0, "Atelier support disabled");
#endif
}
#define DRAWING_OUTPUT(name, format)                                         \
  snr_status snr_spd_##name(const spd_drawing* d, const snr_spd_options* o,  \
                            snr_buffer* b, snr_error* e) {                   \
    return drawing_output(d, o, b, nullptr, e, format);                      \
  }                                                                          \
  snr_status snr_spd_##name##_to_output(const spd_drawing* d,                \
                                        const snr_spd_options* o,            \
                                        const snr_output* b, snr_error* e) { \
    return drawing_output(d, o, nullptr, b, e, format);                      \
  }
DRAWING_OUTPUT(png, 0)
DRAWING_OUTPUT(svg, 1)
DRAWING_OUTPUT(pdf, 2)
