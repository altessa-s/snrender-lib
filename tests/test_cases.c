/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <math.h>
#include <png.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      exit(1);                                                \
    }                                                         \
  } while (0)
static snr_status discard(void* ctx, const uint8_t* data, size_t size) {
  (void)ctx;
  CHECK(data && size);
  return SNR_OK;
}
static void save(const char* mode, bool on, const snr_buffer* b) {
  char path[128];
  snprintf(path, sizeof path, "hostile-%s%s.pdf", mode, on ? "" : "-off");
  FILE* f = fopen(path, "wb");
  CHECK(f);
  CHECK(fwrite(b->bytes, 1, b->size, f) == b->size);
  CHECK(!fclose(f));
}
static void hostile(void) {
  const char* modes[] = {"plain", "url",   "emoji", "tiny", "huge",  "linkhuge",
                         "bt",    "paren", "title", "nan",  "scheme"};
  for (size_t mode = 0; mode < sizeof modes / sizeof *modes; mode++) {
    sn_word words[] = {{{"Hidden", 6}, {10, 10, 80, 30, false}},
                       {{"Second", 6}, {10, 60, 80, 30, false}}};
    sn_element el = {.words = words, .word_count = 2};
    sn_recognition rec = {.elements = &el, .element_count = 1};
    sn_page page = {.number = 1, .size = {1404, 1872}, .recognition = &rec};
    sn_link links[40] = {0};
    char urls[40][80];
    sn_notebook n = {.equipment = SN_EQUIPMENT_NOMAD,
                     .pages = &page,
                     .page_count = 1,
                     .links = links};
    snr_options o = snr_options_default();
    o.scale = 0.125;
    if (mode == 1)
      for (size_t i = 0; i < 40; i++) {
        snprintf(urls[i], sizeof urls[i], "http://a/x/Contents %zu 0 R", i);
        links[i] = (sn_link){.page_number = 1,
                             .kind = 4,
                             .url = {urls[i], strlen(urls[i])},
                             .rect = {20, 30 + (double)i * 10, 100, 5, false}};
        n.link_count++;
      }
    if (mode == 2) words[0].label = (sn_string){"😀🎉", 8};
    if (mode == 3) words[0].box.width = 1e-300;
    if (mode == 4) words[0].box.x = 1e300;
    if (mode == 5 || mode == 7 || mode == 10) {
      const char* uri = mode == 7
                            ? "http://a/)>>/S/JavaScript/JS(app.alert(1))("
                        : mode == 10 ? "javascript:alert(1)"
                                     : "http://x/";
      links[0] = (sn_link){.page_number = 1,
                           .kind = 4,
                           .url = {uri, strlen(uri)},
                           .rect = {20, 30, mode == 5 ? 1e300 : 100, 5, false}};
      n.link_count = 1;
    }
    if (mode == 6) words[0].label = (sn_string){"BT\nET\n", 6};
    if (mode == 8) o.title = "Info /Contents 9 0 R (BT) <FFFF> % /Type /Page";
    if (mode == 9) words[0].box.x = NAN;
    for (int on = 0; on < 2; on++) {
      o.recognition = on != 0;
      snr_buffer b = {0};
      CHECK(snr_page_pdf(&n, 0, &o, &b, nullptr) == SNR_OK);
      save(modes[mode], o.recognition, &b);
      snr_buffer_free(&b);
    }
  }
  puts("hostile URLs, title, emoji, boxes and operators passed");
}
static void scaling(void) {
  uint8_t pixels[4 * 4 * 4];
  for (size_t i = 0; i < 16; i++) {
    unsigned value = i % 2 ? 255 : 0;
    for (size_t c = 0; c < 3; c++) pixels[i * 4 + c] = (uint8_t)value;
    pixels[i * 4 + 3] = 255;
  }
  png_image image = {.version = PNG_IMAGE_VERSION,
                     .width = 4,
                     .height = 4,
                     .format = PNG_FORMAT_RGBA};
  png_alloc_size_t size = 0;
  CHECK(
      png_image_write_to_memory(&image, nullptr, &size, 0, pixels, 0, nullptr));
  uint8_t* bytes = malloc(size);
  CHECK(bytes);
  CHECK(png_image_write_to_memory(&image, bytes, &size, 0, pixels, 0, nullptr));
  sn_bitmap encoded = {.encoding = SN_ENCODING_PNG, .bytes = {bytes, size}};
  sn_layer layer = {.bitmap = &encoded, .is_visible = true};
  sn_page page = {.size = {4, 4}, .layers = &layer, .layer_count = 1};
  sn_notebook n = {.pages = &page, .page_count = 1};
  snr_options o = snr_options_default();
  for (int divisor = 1; divisor <= 4; divisor *= 2) {
    o.scale = 1.0 / divisor;
    snr_bitmap b = {0};
    CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_OK);
    if (divisor == 1)
      CHECK(!memcmp(b.pixels, pixels, sizeof pixels));
    else
      for (size_t i = 0; i < b.width * b.height; i++) {
        CHECK(b.pixels[i * 4] == 128 && b.pixels[i * 4 + 3] == 255);
      }
    snr_bitmap_free(&b);
  }
  free(bytes);
  for (size_t i = 0; i < 16; i++) {
    pixels[i * 4] = i % 2 ? 255 : 0;
    pixels[i * 4 + 1] = 0;
    pixels[i * 4 + 2] = i % 2 ? 0 : 255;
    pixels[i * 4 + 3] = i % 2 ? 255 : 0;
  }
  size = 0;
  CHECK(
      png_image_write_to_memory(&image, nullptr, &size, 0, pixels, 0, nullptr));
  bytes = malloc(size);
  CHECK(bytes);
  CHECK(png_image_write_to_memory(&image, bytes, &size, 0, pixels, 0, nullptr));
  encoded.bytes = (sn_bytes){bytes, size};
  n.kind = SN_KIND_MARK;
  for (int divisor = 2; divisor <= 4; divisor *= 2) {
    o.scale = 1.0 / divisor;
    snr_bitmap b = {0};
    CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_OK);
    for (size_t i = 0; i < b.width * b.height; i++) {
      CHECK(b.pixels[i * 4] == 128 && b.pixels[i * 4 + 1] == 0);
      CHECK(b.pixels[i * 4 + 2] == 0 && b.pixels[i * 4 + 3] == 128);
    }
    snr_bitmap_free(&b);
  }
  free(bytes);
  puts("exact scale 1 and box averages at 1/2 and 1/4 passed");
}
static void rational_scaling(void) {
  uint8_t pixels[16 * 4];
  for (size_t i = 0; i < 16; i++) {
    pixels[i * 4] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = (uint8_t)(i * 17);
    pixels[i * 4 + 3] = (uint8_t)(1 + i % 4);
  }
  png_image image = {.version = PNG_IMAGE_VERSION,
                     .width = 4,
                     .height = 4,
                     .format = PNG_FORMAT_RGBA};
  png_alloc_size_t size = 0;
  CHECK(
      png_image_write_to_memory(&image, nullptr, &size, 0, pixels, 0, nullptr));
  uint8_t* bytes = malloc(size);
  CHECK(bytes &&
        png_image_write_to_memory(&image, bytes, &size, 0, pixels, 0, nullptr));
  sn_bitmap encoded = {.encoding = SN_ENCODING_PNG, .bytes = {bytes, size}};
  sn_layer layer = {.bitmap = &encoded, .is_visible = true};
  sn_page page = {.size = {4, 4}, .layers = &layer, .layer_count = 1};
  sn_notebook n = {.kind = SN_KIND_MARK, .pages = &page, .page_count = 1};
  snr_options o = snr_options_default();
  snr_raster source = {0};
  CHECK(snr_page_raster(&n, 0, &o, &source, nullptr) == SNR_OK);
  const double scales[] = {0.9999, 0.75, 0.5, 0.25};
  for (size_t k = 0; k < 4; k++) {
    o.scale = scales[k];
    snr_raster out = {0};
    CHECK(snr_page_raster(&n, 0, &o, &out, nullptr) == SNR_OK);
    for (size_t y = 0; y < out.height; y++)
      for (size_t x = 0; x < out.width; x++) {
        uint64_t gray = 0, alpha = 0;
        for (size_t sy = 0; sy < 4; sy++)
          for (size_t sx = 0; sx < 4; sx++) {
            size_t left = x * 4 > sx * out.width ? x * 4 : sx * out.width;
            size_t right = (x + 1) * 4 < (sx + 1) * out.width
                               ? (x + 1) * 4
                               : (sx + 1) * out.width;
            size_t top = y * 4 > sy * out.height ? y * 4 : sy * out.height;
            size_t bottom = (y + 1) * 4 < (sy + 1) * out.height
                                ? (y + 1) * 4
                                : (sy + 1) * out.height;
            if (right <= left || bottom <= top) continue;
            uint64_t weight = (right - left) * (bottom - top);
            size_t i = sy * 4 + sx;
            alpha += weight * source.alpha[i];
            gray += weight * source.alpha[i] * source.gray[i];
          }
        CHECK(fabs((double)out.gray[y * out.width + x] -
                   (double)gray / (double)alpha) <= 0.5);
      }
    snr_raster_free(&out);
  }
  snr_raster_free(&source);
  free(bytes);
  puts("low-alpha rational gray error <= 0.5 at four scales");
}
static void document_budget(void) {
  sn_page pages[] = {{.size = {4, 4}}, {.size = {4, 4}}};
  sn_notebook n = {.pages = pages, .page_count = 2};
  snr_options o = snr_options_default();
  snr_buffer b = {0};
  snr_error error = {0};
  o.limits.maximum_document_work = 64;
  CHECK(snr_document_pdf(&n, 0, 2, &o, &b, &error) == SNR_OK);
  snr_buffer_free(&b);
  o.limits.maximum_document_work = 63;
  CHECK(snr_document_pdf(&n, 0, 2, &o, &b, &error) == SNR_ERR_LIMIT);
  CHECK(error.index == 1 && !b.bytes);
  o.limits.maximum_document_work = 0;
  CHECK(snr_page_pdf(&n, 0, &o, &b, &error) == SNR_ERR_LIMIT);
  puts("document budget accepts exact bound and refuses one unit below it");
}
static void contract(void) {
  sn_page page = {.size = {4, 4}};
  sn_notebook n = {.pages = &page, .page_count = 1};
  snr_error error = {
      .code = SNR_ERR_UNSUPPORTED, .index = 99, .message = "untouched"};
  snr_error saved = error;
  snr_buffer b = {0};
  snr_output output = {SNR_OUTPUT_ABI, sizeof output, discard, nullptr};
  typedef snr_status (*buffer_fn)(const sn_notebook*, size_t,
                                  const snr_options*, snr_buffer*, snr_error*);
  buffer_fn functions[] = {snr_page_png, snr_page_svg, snr_page_pdf};
  for (size_t i = 0; i < 3; i++) {
    error = saved;
    CHECK(functions[i](&n, 0, nullptr, &b, &error) == SNR_OK);
    CHECK(!memcmp(&error, &saved, sizeof error));
    snr_buffer keep = b;
    CHECK(functions[i](&n, 1, nullptr, &b, &error) == SNR_ERR_INVALID_ARGUMENT);
    CHECK(!memcmp(&keep, &b, sizeof b));
    snr_buffer_free(&b);
  }
  typedef snr_status (*output_fn)(const sn_notebook*, size_t,
                                  const snr_options*, const snr_output*,
                                  snr_error*);
  output_fn streams[] = {snr_page_png_to_output, snr_page_svg_to_output,
                         snr_page_pdf_to_output};
  for (size_t i = 0; i < 3; i++) {
    error = saved;
    CHECK(streams[i](&n, 0, nullptr, &output, &error) == SNR_OK);
    CHECK(!memcmp(&error, &saved, sizeof error));
    snr_output keep = output;
    CHECK(streams[i](&n, 1, nullptr, &output, &error) ==
          SNR_ERR_INVALID_ARGUMENT);
    CHECK(!memcmp(&keep, &output, sizeof output));
  }
  error = saved;
  CHECK(snr_document_pdf(&n, 0, SIZE_MAX, nullptr, &b, &error) == SNR_OK);
  CHECK(!memcmp(&error, &saved, sizeof error));
  snr_buffer keep = b;
  CHECK(snr_document_pdf(&n, 1, SIZE_MAX, nullptr, &b, &error) ==
        SNR_ERR_INVALID_ARGUMENT);
  CHECK(!memcmp(&keep, &b, sizeof b));
  snr_buffer_free(&b);
  error = saved;
  CHECK(snr_document_pdf_to_output(&n, 0, SIZE_MAX, nullptr, &output, &error) ==
        SNR_OK);
  CHECK(!memcmp(&error, &saved, sizeof error));
  snr_output kept_output = output;
  CHECK(snr_document_pdf_to_output(&n, 1, 1, nullptr, &output, &error) ==
        SNR_ERR_INVALID_ARGUMENT);
  CHECK(!memcmp(&kept_output, &output, sizeof output));
  snr_bitmap bitmap = {0};
  error = saved;
  CHECK(snr_page_bitmap(&n, 0, nullptr, &bitmap, &error) == SNR_OK);
  CHECK(!memcmp(&error, &saved, sizeof error));
  snr_bitmap kept_bitmap = bitmap;
  CHECK(snr_page_bitmap(&n, 1, nullptr, &bitmap, &error) ==
        SNR_ERR_INVALID_ARGUMENT);
  CHECK(!memcmp(&kept_bitmap, &bitmap, sizeof bitmap));
  snr_bitmap_free(&bitmap);
  snr_raster raster = {0};
  error = saved;
  CHECK(snr_page_raster(&n, 0, nullptr, &raster, &error) == SNR_OK);
  CHECK(!memcmp(&error, &saved, sizeof error));
  snr_raster kept_raster = raster;
  CHECK(snr_page_raster(&n, 1, nullptr, &raster, &error) ==
        SNR_ERR_INVALID_ARGUMENT);
  CHECK(!memcmp(&kept_raster, &raster, sizeof raster));
  snr_raster_free(&raster);
  puts("all ten render entry point contracts passed");
}
static void damaged(void) {
  const uint8_t broken[] = {137, 80, 78, 71, 13, 10, 26, 10, 1};
  sn_bitmap encoded = {.encoding = SN_ENCODING_PNG,
                       .bytes = {broken, sizeof broken}};
  sn_layer layer = {.bitmap = &encoded, .is_visible = true};
  sn_page page = {.size = {4, 4}, .layers = &layer, .layer_count = 1};
  sn_notebook n = {.pages = &page, .page_count = 1};
  snr_bitmap b = {0};
  CHECK(snr_page_bitmap(&n, 0, nullptr, &b, nullptr) == SNR_OK);
  CHECK(b.pixels[0] == 255);
  snr_bitmap_free(&b);
  snr_options o = snr_options_default();
  o.strict_images = true;
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_BACKEND);
  CHECK(!b.pixels);
  encoded.encoding = SN_ENCODING_JPEG;
  CHECK(snr_page_bitmap(&n, 0, nullptr, &b, nullptr) == SNR_OK);
  snr_bitmap_free(&b);
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_BACKEND);
  puts("damaged PNG/JPEG omission and strict failure passed");
}
int main(void) {
  if (!strcmp(SNR_SUITE, "scaling")) {
    scaling();
    rational_scaling();
  }
  if (!strcmp(SNR_SUITE, "options")) {
    contract();
    document_budget();
  }
  if (!strcmp(SNR_SUITE, "hostile")) hostile();
  if (!strcmp(SNR_SUITE, "damaged_images")) damaged();
  return 0;
}
