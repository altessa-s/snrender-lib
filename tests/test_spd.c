/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <locale.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SNR_SPD
#include <snspd/snspd.h>
#endif
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      abort();                                                \
    }                                                         \
  } while (0)
#ifdef SNR_SPD
#include "png_decode.h"
static spd_drawing* open_drawing(const char* dir, const char* name) {
  char path[4096];
  snprintf(path, sizeof path, "%s/%s.spd", dir, name);
  FILE* f = fopen(path, "rb");
  CHECK(f);
  CHECK(!fseek(f, 0, SEEK_END));
  long length = ftell(f);
  CHECK(length > 0);
  rewind(f);
  uint8_t* data = malloc((size_t)length);
  CHECK(data);
  CHECK(fread(data, 1, (size_t)length, f) == (size_t)length);
  CHECK(!fclose(f));
  spd_drawing* d = nullptr;
  spd_error e;
  CHECK(spd_open(data, (size_t)length, nullptr, &d, &e) == SPD_OK);
  free(data);
  return d;
}
static void pixel(const snr_bitmap* b, size_t x, size_t y, unsigned gray,
                  unsigned alpha) {
  CHECK(x < b->width && y < b->height);
  const uint8_t* p = b->pixels + y * b->stride + x * 4;
  for (size_t c = 0; c < 3; c++) CHECK(abs((int)p[c] - (int)gray) <= 1);
  CHECK(p[3] == alpha);
}
static bool cancel(void* context) {
  size_t* n = context;
  return (*n)-- == 0;
}
static snr_status refuse(void* context, const uint8_t* bytes, size_t size) {
  CHECK(bytes && size);
  (*(size_t*)context)++;
  return (snr_status)12345;
}
static void save(const char* dir, const char* ext, const snr_buffer* b) {
  char path[4096];
  snprintf(path, sizeof path, "%s/result.%s", dir, ext);
  FILE* f = fopen(path, "wb");
  CHECK(f);
  CHECK(fwrite(b->bytes, 1, b->size, f) == b->size);
  CHECK(!fclose(f));
}
/* Most cases check the centred canvas; the default is painted bounds. */
static snr_spd_options canvas_default(void) {
  snr_spd_options o = snr_spd_options_default();
  o.page = SNR_SPD_CANVAS;
  return o;
}
static void tests(const char* dir) {
  spd_drawing* d = open_drawing(dir, "valid");
  /* Explicit expectations also prevent fixture/parser changes from masking a
   * renderer transposition: tx spans 9..12, ty spans 20..21. */
  CHECK(d->layers[0].tiles[0].tx == 9 && d->layers[0].tiles[0].ty == 20);
  CHECK(d->layers[0].tiles[1].tx == 10 && d->layers[0].tiles[1].ty == 20);
  CHECK(d->layers[0].tiles[2].tx == 11 && d->layers[0].tiles[2].ty == 21);
  CHECK(d->layers[0].tiles[3].tx == 12 && d->layers[0].tiles[3].ty == 20);
  snr_spd_options o = canvas_default();
  snr_bitmap b = {0};
  snr_error e = {.code = (snr_status)42};
  CHECK(snr_spd_options_default().page == SNR_SPD_PAINTED_BOUNDS);
  {
    /* Null options are the defaults: the stored tile union, 4×2 tiles. */
    snr_bitmap bounds = {0};
    CHECK(snr_spd_bitmap(d, nullptr, &bounds, &e) == SNR_OK);
    CHECK(bounds.width == 4 * 128 && bounds.height == 2 * 128);
    snr_bitmap_free(&bounds);
  }
  CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_OK);
  CHECK(e.code == (snr_status)42);
  CHECK(b.width == 256 && b.height == 256);
  pixel(&b, 10, 10, 100, 255);
  pixel(&b, 80, 10, 184, 255);
  pixel(&b, 10, 80, 227, 255);
  pixel(&b, 160, 10, 32, 255);
  pixel(&b, 160, 160, 0, 255);
  pixel(&b, 10, 160, 255, 255);
  snr_bitmap_free(&b);
  o.include_reference_layer = false;
  CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_OK);
  pixel(&b, 160, 10, 255, 255);
  snr_bitmap_free(&b);
  o.render.has_background = true;
  o.render.background = (snr_color){1, 1, 1, 0};
  CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_OK);
  pixel(&b, 10, 80, 100, 128);
  pixel(&b, 160, 10, 0, 0);
  snr_bitmap_free(&b);
  o = canvas_default();
  spd_layer layers[3];
  memcpy(layers, d->layers, sizeof layers);
  spd_layer tmp = layers[0];
  layers[0] = layers[1];
  layers[1] = tmp;
  spd_drawing model = *d;
  model.layers = layers;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_OK);
  pixel(&b, 10, 10, 0, 255);
  snr_bitmap_free(&b);
  model = *d;
  model.canvas.viewport_x += 64;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_OK);
  pixel(&b, 1, 10, 184, 255);
  pixel(&b, 100, 10, 32, 255);
  pixel(&b, 250, 10, 0, 255);
  snr_bitmap_free(&b);
  model = *d;
  model.canvas.viewport_scale = 2;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_OK);
  /* Zoom about the saved centre: pattern starts at (-128,-128),
   * diagonal black starts at (128,128). Page size stays 256x256. */
  CHECK(b.width == 256 && b.height == 256);
  pixel(&b, 10, 10, 227, 255);
  pixel(&b, 200, 10, 32, 255);
  pixel(&b, 127, 160, 255, 255);
  pixel(&b, 128, 160, 0, 255);
  snr_bitmap_free(&b);
  model = *d;
  model.canvas.viewport_x += .5;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_OK);
  pixel(&b, 63, 10, 184, 255);
  snr_bitmap_free(&b);
  o.page = SNR_SPD_PAINTED_BOUNDS;
  CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_OK);
  /* Unequal x/y extents and asymmetric pixels independently pin the axes. */
  CHECK(b.width == 512 && b.height == 256);
  pixel(&b, 10, 10, 0, 255);
  pixel(&b, 138, 10, 100, 255);
  pixel(&b, 266, 10, 32, 255);
  pixel(&b, 394, 10, 0, 255);
  pixel(&b, 138, 160, 255, 255);
  pixel(&b, 266, 160, 0, 255);
  snr_bitmap_free(&b);
  /* Non-square canvas centred on a large 64-bit column. Check margins and
   * local tile orientation independently of the fixture's default frame. */
  spd_tile tile = d->layers[0].tiles[1];
  tile.tx = (UINT64_C(1) << 32) + 10;
  spd_layer one = {.tiles = &tile, .tile_count = 1};
  model = *d;
  model.layers = &one;
  model.layer_count = 1;
  model.canvas.width = 192;
  model.canvas.height = 320;
  model.canvas.viewport_x = (double)tile.tx * 128 + 64;
  model.canvas.viewport_y = 2624;
  o = canvas_default();
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_OK);
  CHECK(b.width == 192 && b.height == 320);
  pixel(&b, 31, 96, 255, 255);
  pixel(&b, 32, 95, 255, 255);
  pixel(&b, 32, 96, 0, 255);
  pixel(&b, 96, 96, 167, 255);
  pixel(&b, 32, 160, 255, 255);
  pixel(&b, 160, 96, 255, 255);
  snr_bitmap_free(&b);
  model.canvas.viewport_scale = 2;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_OK);
  CHECK(b.width == 192 && b.height == 320);
  /* Tile spans [-32,224) x [32,288): clipped on both horizontal sides. */
  pixel(&b, 0, 31, 255, 255);
  pixel(&b, 0, 32, 0, 255);
  pixel(&b, 95, 32, 0, 255);
  pixel(&b, 96, 32, 167, 255);
  pixel(&b, 191, 32, 167, 255);
  pixel(&b, 0, 160, 255, 255);
  snr_bitmap_free(&b);
  /* Move it up and then down: clipping uses the centred vertical frame too. */
  model.canvas.viewport_y += 32;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_OK);
  pixel(&b, 0, 0, 0, 255);
  pixel(&b, 96, 0, 167, 255);
  pixel(&b, 0, 96, 255, 255);
  snr_bitmap_free(&b);
  model.canvas.viewport_y -= 160;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_OK);
  pixel(&b, 0, 287, 255, 255);
  pixel(&b, 0, 288, 0, 255);
  pixel(&b, 96, 319, 167, 255);
  snr_bitmap_free(&b);
  o.page = SNR_SPD_PAINTED_BOUNDS;
  model.canvas.has_width = model.canvas.has_height = false;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_OK);
  CHECK(b.width == 256 && b.height == 256);
  pixel(&b, 0, 0, 0, 255);
  pixel(&b, 127, 0, 0, 255);
  pixel(&b, 128, 0, 167, 255);
  pixel(&b, 0, 128, 255, 255);
  snr_bitmap_free(&b);
  o = canvas_default();
  o.dpi = 64;
  o.render.scale =
      0; /* DPI overrides scale, including an unused invalid value. */
  CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_OK);
  CHECK(b.width == 128);
  pixel(&b, 5, 5, 100, 255);
  snr_bitmap_free(&b);
  o.dpi = 0;
  o.render.scale = 2;
  CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_OK);
  CHECK(b.width == 512);
  pixel(&b, 20, 20, 100, 255);
  snr_bitmap_free(&b);
  o = canvas_default();
  snr_buffer result = {0};
  CHECK(snr_spd_png(d, &o, &result, &e) == SNR_OK);
  size_t w, h;
  uint8_t* rgba = png_decode(&result, &w, &h);
  CHECK(w == 256 && h == 256);
  CHECK(rgba[(10 * w + 10) * 4] == 100);
  free(rgba);
  save(dir, "png", &result);
  snr_buffer_free(&result);
  CHECK(snr_spd_svg(d, &o, &result, &e) == SNR_OK);
  CHECK(result.size > 100 && !memcmp(result.bytes, "<?xml", 5));
  save(dir, "svg", &result);
  snr_buffer_free(&result);
  CHECK(snr_spd_pdf(d, &o, &result, &e) == SNR_OK);
  CHECK(result.size > 100 && !memcmp(result.bytes, "%PDF-", 5));
  save(dir, "pdf", &result);
  snr_buffer_free(&result);
  model = *d;
  model.canvas.width = 256.5;
  (void)setlocale(LC_NUMERIC, "fr_FR.UTF-8");
  CHECK(snr_spd_svg(&model, &o, &result, &e) == SNR_OK);
  const char expected_view[] = "viewBox=\"0 0 256.5 256\"";
  bool found_view = false;
  for (size_t i = 0; i + sizeof expected_view - 1 <= result.size; i++)
    if (!memcmp(result.bytes + i, expected_view, sizeof expected_view - 1))
      found_view = true;
  CHECK(found_view);
  snr_buffer_free(&result);
  (void)setlocale(LC_NUMERIC, "C");
  o.dpi = 64;
  CHECK(snr_spd_pdf(d, &o, &result, &e) == SNR_OK);
  save(dir, "half.pdf", &result);
  snr_buffer_free(&result);
  o = canvas_default();
  size_t writes = 0;
  snr_output sink = {.abi_version = SNR_OUTPUT_ABI,
                     .struct_size = sizeof sink,
                     .write = refuse,
                     .context = &writes};
  CHECK(snr_spd_png_to_output(d, &o, &sink, &e) == (snr_status)12345);
  CHECK(writes == 1 && !strcmp(e.message, "output callback failed"));
  writes = 0;
  CHECK(snr_spd_pdf_to_output(d, &o, &sink, &e) == (snr_status)12345);
  CHECK(writes == 1);
  writes = 0;
  CHECK(snr_spd_svg_to_output(d, &o, &sink, &e) == (snr_status)12345);
  CHECK(writes == 1);
  sink.struct_size = 0;
  CHECK(snr_spd_png_to_output(d, &o, &sink, &e) == SNR_ERR_INVALID_ARGUMENT);
  for (size_t n = 0; n < 8; n++) {
    o = canvas_default();
    switch (n) {
      case 0:
        o.maximum_tiles = 5;
        break;
      case 1:
        o.maximum_decoded_bytes = 65535;
        break;
      case 2:
        o.render.limits.maximum_pixels = 65535;
        break;
      case 3:
        o.render.limits.maximum_work = 1;
        break;
      case 4:
        o.render.limits.maximum_decoded_pixels = 65535;
        break;
      case 5:
        o.render.limits.maximum_output_bytes = 1;
        break;
      case 6:
        o.maximum_decoded_bytes = 6 * 65536 + 2 * 256 * 256 * 4 - 1;
        break;
      case 7:
        o.render.limits.maximum_document_work = 0;
        break;
    }
    result = (snr_buffer){.bytes = (uint8_t*)1, .size = 777};
    CHECK(snr_spd_png(d, &o, &result, &e) == SNR_ERR_LIMIT);
    CHECK(result.bytes == (uint8_t*)1 && result.size == 777);
  }
  result = (snr_buffer){0};
  for (size_t n = 0; n < 3; n++) {
    size_t calls = n == 0 ? 0 : n == 1 ? 5 : 50;
    o = canvas_default();
    o.render.cancel = cancel;
    o.render.cancel_context = &calls;
    CHECK(snr_spd_png(d, &o, &result, &e) == SNR_ERR_CANCELLED);
    CHECK(!result.bytes);
  }
  o = canvas_default();
  o.abi_version++;
  CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_ERR_INVALID_ARGUMENT);
  o = canvas_default();
  o.struct_size--;
  CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_ERR_INVALID_ARGUMENT);
  o = canvas_default();
  o.struct_size += 16;
  CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_OK);
  snr_bitmap_free(&b);
  o = canvas_default();
  CHECK(snr_spd_bitmap(nullptr, &o, &b, &e) == SNR_ERR_INVALID_ARGUMENT);
  CHECK(snr_spd_png(d, &o, nullptr, &e) == SNR_ERR_INVALID_ARGUMENT);
  model = *d;
  model.layers = nullptr;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_ERR_INVALID_ARGUMENT);
  model = *d;
  model.canvas.viewport_scale = 0;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_ERR_INVALID_ARGUMENT);
  model = *d;
  model.canvas.width = 1e100;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_ERR_LIMIT);
  model = *d;
  model.canvas.has_width = false;
  CHECK(snr_spd_bitmap(&model, &o, &b, &e) == SNR_ERR_INVALID_ARGUMENT);
  spd_drawing_free(d);
  const char* bad[] = {"bad", "crc", "wrong", "truncated", "huge", "decode"};
  for (size_t i = 0; i < 6; i++) {
    d = open_drawing(dir, bad[i]);
    o = canvas_default();
    snr_status expected = SNR_OK;
    CHECK(snr_spd_bitmap(d, &o, &b, &e) == expected);
    if (!expected) {
      pixel(&b, 10, 10, 255, 255);
      snr_bitmap_free(&b);
    }
    o.render.strict_images = true;
    CHECK(snr_spd_bitmap(d, &o, &b, &e) ==
          (i == 4 ? SNR_ERR_LIMIT : SNR_ERR_BACKEND));
    CHECK(!b.pixels);
    spd_drawing_free(d);
  }
  const char* encoded[] = {"adam7", "split"};
  for (size_t i = 0; i < 2; i++) {
    d = open_drawing(dir, encoded[i]);
    o = canvas_default();
    o.render.strict_images = true;
    CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_OK);
    pixel(&b, 10, 10, 0, 255);
    pixel(&b, 80, 10, 167, 255);
    pixel(&b, 10, 80, 255, 255);
    snr_bitmap_free(&b);
    spd_drawing_free(d);
  }
  d = open_drawing(dir, "empty");
  o = canvas_default();
  o.page = SNR_SPD_PAINTED_BOUNDS;
  CHECK(snr_spd_bitmap(d, &o, &b, &e) == SNR_OK);
  CHECK(b.width == 1 && b.height == 1);
  snr_bitmap_free(&b);
  spd_drawing_free(d);
}
#endif
int main(int argc, char** argv) {
#ifdef SNR_SPD
  CHECK(argc == 2);
  tests(argv[1]);
#else
  (void)argc;
  (void)argv;
  snr_error e = {0};
  snr_buffer b = {.bytes = (uint8_t*)1, .size = 23};
  snr_bitmap bm = {.pixels = (uint8_t*)1, .width = 23};
  CHECK(snr_spd_png(nullptr, nullptr, &b, &e) == SNR_ERR_UNSUPPORTED);
  CHECK(e.code == SNR_ERR_UNSUPPORTED && b.size == 23 &&
        b.bytes == (uint8_t*)1);
  CHECK(snr_spd_svg(nullptr, nullptr, &b, &e) == SNR_ERR_UNSUPPORTED);
  CHECK(snr_spd_pdf(nullptr, nullptr, &b, &e) == SNR_ERR_UNSUPPORTED);
  CHECK(snr_spd_png_to_output(nullptr, nullptr, nullptr, &e) ==
        SNR_ERR_UNSUPPORTED);
  CHECK(snr_spd_svg_to_output(nullptr, nullptr, nullptr, &e) ==
        SNR_ERR_UNSUPPORTED);
  CHECK(snr_spd_pdf_to_output(nullptr, nullptr, nullptr, &e) ==
        SNR_ERR_UNSUPPORTED);
  CHECK(snr_spd_bitmap(nullptr, nullptr, &bm, &e) == SNR_ERR_UNSUPPORTED);
  CHECK(bm.pixels == (uint8_t*)1 && bm.width == 23);
#endif
  puts("SPD tests passed");
  return 0;
}
