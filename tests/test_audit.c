/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <stdint.h>
#include <zlib.h>

#include "../src/pdf/snr_pdf_edit.h"
#include "source_pdf.h"
#define CHECK(x)                                      \
  do {                                                \
    if (!(x)) {                                       \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
      return 1;                                       \
    }                                                 \
  } while (0)
static snr_status collect(void* context, const uint8_t* bytes, size_t size) {
  size_t* calls = context;
  (void)bytes;
  (void)size;
  (*calls)++;
  return SNR_OK;
}
static bool cancel(void* context) { return ++*(size_t*)context == 3; }
static bool countdown(void* context) { return --*(size_t*)context == 0; }
static void be32(uint8_t* b, uint32_t n) {
  for (unsigned i = 0; i < 4; i++) b[i] = (uint8_t)(n >> (24 - i * 8));
}
static int images(void) {
  sn_word word = {.label = {.data = "x", .length = 65537}};
  sn_element element = {.words = &word, .word_count = 1};
  sn_recognition recognition = {.elements = &element, .element_count = 1};
  sn_page page = {.size = {2, 2}, .recognition = &recognition};
  sn_notebook book = {.pages = &page, .page_count = 1, .kind = SN_KIND_MARK};
  snr_options o = snr_options_default();
  snr_bitmap bitmap = {0};
  snr_raster raster = {0};
  snr_buffer output = {0};
  CHECK(snr_page_bitmap(&book, 0, &o, &bitmap, nullptr) == SNR_OK);
  snr_bitmap_free(&bitmap);
  CHECK(snr_page_raster(&book, 0, &o, &raster, nullptr) == SNR_OK);
  snr_raster_free(&raster);
  CHECK(snr_page_png(&book, 0, &o, &output, nullptr) == SNR_OK);
  snr_buffer_free(&output);
  CHECK(snr_page_svg(&book, 0, &o, &output, nullptr) == SNR_OK);
  snr_buffer_free(&output);
  CHECK(snr_page_pdf(&book, 0, &o, &output, nullptr) == SNR_ERR_LIMIT);
  page.recognition = nullptr;

  uint8_t white[16];
  memset(white, 128, sizeof white); /* premultiplied white at half alpha */
  snr_bitmap source = {.pixels = white, .width = 2, .height = 2, .stride = 8};
  snr_writer png = {.limit = SIZE_MAX};
  CHECK(snr_encode_png(&source, &png) == SNR_OK);
  sn_bitmap image = {.encoding = SN_ENCODING_PNG,
                     .bytes = {png.buffer.bytes, png.buffer.size}};
  sn_layer layers[2] = {{.bitmap = &image, .is_visible = true},
                        {.bitmap = &image, .is_visible = true}};
  page.layers = layers;
  for (unsigned background = 0; background < 2; background++) {
    page.layer_count = background ? 1 : 2;
    o.has_background = background != 0;
    o.background = (snr_color){1, 1, 1, 128.0 / 255};
    for (unsigned scale = 0; scale < 2; scale++) {
      o.scale = scale ? 0.5 : 1;
      CHECK(snr_page_raster(&book, 0, &o, &raster, nullptr) == SNR_OK);
      CHECK(raster.gray[0] == 255 && raster.alpha[0] == 191);
      snr_raster_free(&raster);
      CHECK(snr_page_bitmap(&book, 0, &o, &bitmap, nullptr) == SNR_OK);
      CHECK(bitmap.pixels[0] == 191 && bitmap.pixels[3] == 191);
      snr_bitmap_free(&bitmap);
    }
  }
  o = snr_options_default();
  o.limits.maximum_work = 20; /* dimensions fit, encoded image does not */
  CHECK(snr_page_bitmap(&book, 0, &o, &bitmap, nullptr) == SNR_ERR_LIMIT);
  snr_buffer_free(&png.buffer);

  /* A large single ancillary chunk must poll within the chunk. */
  size_t length = 200000, size = 33 + length + 12;
  uint8_t* bytes = calloc(size, 1);
  CHECK(bytes);
  memcpy(bytes, "\211PNG\r\n\032\n", 8);
  be32(bytes + 8, 13);
  memcpy(bytes + 12, "IHDR", 4);
  be32(bytes + 16, 1);
  be32(bytes + 20, 1);
  be32(bytes + 29, (uint32_t)crc32(0, bytes + 12, 17));
  be32(bytes + 33, (uint32_t)length);
  memcpy(bytes + 37, "tEXt", 4);
  image.bytes = (sn_bytes){bytes, size};
  size_t calls = 0, framed = 0;
  o = snr_options_default();
  o.cancel = cancel;
  o.cancel_context = &calls;
  CHECK(snr_image_frame(&image, &o, &framed) == SNR_ERR_CANCELLED);
  free(bytes);

  /* Incompressible pixels make batching observable across multiple flushes. */
  uint8_t pixels[256 * 256 * 4];
  uint32_t random = 1;
  for (size_t i = 0; i < sizeof pixels; i++) {
    random = random * 1664525u + 1013904223u;
    pixels[i] = i % 4 == 3 ? 255 : (uint8_t)(random >> 24);
  }
  source = (snr_bitmap){
      .pixels = pixels, .width = 256, .height = 256, .stride = 1024};
  calls = 0;
  snr_output sink = {SNR_OUTPUT_ABI, sizeof sink, collect, &calls};
  snr_writer writer = {.output = &sink, .limit = SIZE_MAX};
  o = snr_options_default();
  CHECK(snr_encode_svg(&source, 256, 256, &o, &writer) == SNR_OK);
  CHECK(calls > 3 && calls < 16);
  return 0;
}
/* Every fixture has one good signature before the optional damaged graph. */
static int signature_graphs(void) {
  const char* fields[] = {
      "[4 0 R 99 0 R]", "[4 0 R 5 0 R]",     "[4 0 R]", "[4 0 R 4 0 R]",
      "[4 0 R 5 1 R]",  "[4 0 R 0 65535 R]", "[4 0 R]"};
  for (size_t test = 0; test < sizeof fields / sizeof *fields; test++) {
    char source[16384];
    size_t offsets[85] = {0};
    size_t total = test == 3 ? 43 : test == 6 ? 84 : 5;
    size_t used = (size_t)snprintf(source, sizeof source, "%%PDF-1.4\n");
    for (size_t id = 1; id <= total; id++) {
      offsets[id] = used;
      used += (size_t)snprintf(source + used, sizeof source - used,
                               "%zu 0 obj\n", id);
      if (id == 1)
        used += (size_t)snprintf(source + used, sizeof source - used,
                                 "<< /Type /Catalog /Pages 2 0 R /AcroForm "
                                 "<< /Fields %s >> >>",
                                 fields[test]);
      else if (id == 2)
        used += (size_t)snprintf(source + used, sizeof source - used,
                                 "<< /Type /Pages /Count 1 /Kids [3 0 R] >>");
      else if (id == 3)
        used += (size_t)snprintf(source + used, sizeof source - used,
                                 "<< /Type /Page /Parent 2 0 R "
                                 "/MediaBox [0 0 200 200] >>");
      else if ((test == 3 || test == 6) && id < total)
        used += (size_t)snprintf(source + used, sizeof source - used,
                                 "<< /FT /Sig /Kids [%zu 0 R %zu 0 R] >>",
                                 id + 1, id + 1);
      else
        used += (size_t)snprintf(source + used, sizeof source - used, "%s",
                                 id == 4 && test == 2
                                     ? "<< /FT /Sig /Kids [4 0 R] >>"
                                 : id == 5 && test == 1 ? "garbage"
                                                        : "<< /FT /Sig >>");
      used +=
          (size_t)snprintf(source + used, sizeof source - used, "\nendobj\n");
    }
    size_t xref = used;
    used += (size_t)snprintf(source + used, sizeof source - used,
                             "xref\n0 %zu\n0000000000 65535 f \n", total + 1);
    for (size_t id = 1; id <= total; id++)
      used += (size_t)snprintf(source + used, sizeof source - used,
                               "%010zu 00000 n \n", offsets[id]);
    used += (size_t)snprintf(source + used, sizeof source - used,
                             "trailer\n<< /Size %zu /Root 1 0 R >>\n"
                             "startxref\n%zu\n%%%%EOF\n",
                             total + 1, xref);
    CHECK(used < sizeof source);
    spe_limits limits = spe_limits_default();
    uint64_t remaining = 100000;
    limits.remaining = &remaining;
    spe_pdf_page_geometry* pages = nullptr;
    size_t count = 0, signatures = 0;
    CHECK(spe_pdf_pages((uint8_t*)source, used, &limits, &pages, &count,
                        &signatures, nullptr) == SNR_OK);
    CHECK(count == 1 && signatures == (test == 3 ? 40 : test == 6 ? 64 : 1));
    free(pages);
    /* Optional inspection must not swallow cancellation or work exhaustion. */
    uint64_t consumed = 100000 - remaining;
    remaining = consumed - 1;
    pages = nullptr;
    CHECK(spe_pdf_pages((uint8_t*)source, used, &limits, &pages, &count,
                        &signatures, nullptr) == SNR_ERR_LIMIT);
    CHECK(!pages);
    remaining = 100000;
    size_t calls = (size_t)consumed;
    limits.cancel = countdown;
    limits.context = &calls;
    CHECK(spe_pdf_pages((uint8_t*)source, used, &limits, &pages, &count,
                        &signatures, nullptr) == SNR_ERR_CANCELLED);
    CHECK(!pages && !calls);
  }
  return 0;
}
/* Many xref type-2 fields share a truncated Flate object stream. Decoded
 * bytes must be charged even on failure, and the container tried only once. */
static int damaged_object_stream(void) {
  enum { FIELDS = 500, DECODED = 60 << 20, TOTAL = FIELDS + 7 };
  uint8_t* zeros = calloc(DECODED, 1);
  uLongf compressed_size = compressBound(DECODED);
  uint8_t* compressed = malloc(compressed_size);
  CHECK(zeros && compressed);
  CHECK(compress2(compressed, &compressed_size, zeros, DECODED, 9) == Z_OK);
  free(zeros);
  CHECK(compressed_size > 4);
  compressed_size -= 4; /* Missing Adler checksum: all output, no stream end. */
  size_t capacity = (size_t)compressed_size + 65536;
  uint8_t* source = malloc(capacity);
  CHECK(source);
  size_t offsets[7] = {0};
  size_t used = (size_t)snprintf((char*)source, capacity, "%%PDF-1.5\n");
#define APPEND(...) \
  used += (size_t)snprintf((char*)source + used, capacity - used, __VA_ARGS__)
  offsets[1] = used;
  APPEND(
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R ");
  for (size_t i = 0; i < FIELDS; i++) APPEND("%zu 0 R ", i + 7);
  APPEND("] >> >>\nendobj\n");
  offsets[2] = used;
  APPEND("2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n");
  offsets[3] = used;
  APPEND(
      "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
      ">>\nendobj\n");
  offsets[4] = used;
  APPEND("4 0 obj\n<< /FT /Sig >>\nendobj\n");
  offsets[5] = used;
  APPEND(
      "5 0 obj\n<< /Type /ObjStm /N %d /First 4000 /Filter /FlateDecode "
      "/Length %lu >>\nstream\n",
      FIELDS, (unsigned long)compressed_size);
  memcpy(source + used, compressed, compressed_size);
  used += compressed_size;
  free(compressed);
  APPEND("\nendstream\nendobj\n");
  offsets[6] = used;
  APPEND(
      "6 0 obj\n<< /Type /XRef /Size %d /Root 1 0 R /W [1 4 4] /Length %d "
      ">>\nstream\n",
      TOTAL, TOTAL * 9);
  for (size_t id = 0; id < TOTAL; id++) {
    source[used++] = id == 0 ? 0 : id < 7 ? 1 : 2;
    be32(source + used, id < 7 ? (uint32_t)offsets[id] : 5);
    used += 4;
    be32(source + used, id == 0 ? 65535 : id < 7 ? 0 : (uint32_t)(id - 7));
    used += 4;
  }
  APPEND("\nendstream\nendobj\nstartxref\n%zu\n%%%%EOF\n", offsets[6]);
#undef APPEND
  CHECK(used < capacity);
  spe_limits limits = spe_limits_default();
  limits.maximum_elements = DECODED + 100000;
  uint64_t remaining = DECODED + 100000;
  limits.remaining = &remaining;
  spe_pdf_page_geometry* pages = nullptr;
  size_t count = 0, signatures = 0;
  CHECK(spe_pdf_pages(source, used, &limits, &pages, &count, &signatures,
                      nullptr) == SNR_OK);
  CHECK(count == 1 && signatures == 1);
  CHECK(remaining > 0 && remaining < 100000);
  free(pages);
  pages = nullptr;
  /* Work exhaustion during a failed decode must not be recovered. */
  remaining = 1000000;
  CHECK(spe_pdf_pages(source, used, &limits, &pages, &count, &signatures,
                      nullptr) == SNR_ERR_LIMIT);
  CHECK(!pages);
  free(source);
  return 0;
}

static int pdf(void) {
  char source[4096];
  size_t offsets[9] = {0};
  size_t used = (size_t)snprintf(source, sizeof source, "%%PDF-1.4\n");
  const char* objects[] = {
      "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R] >> >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [6 0 R] >>",
      "garbage", /* live but unreachable and invalid */
      "<< /FT /Sig /Kids [8 0 R] >>",
      "<< /Subtype /Widget /Rect [0 0 100 100] /AP << /N 7 0 R >> >>",
      ("<< /Type /XObject /Subtype /Form /BBox [5000 5000 5001 5001] /Length 0 "
       ">>\nstream\n\nendstream"),
      "<< /Parent 5 0 R >>"};
  for (size_t i = 1; i <= 8; i++) {
    offsets[i] = used;
    used += (size_t)snprintf(source + used, sizeof source - used,
                             "%zu 0 obj\n%s\nendobj\n", i, objects[i - 1]);
  }
  size_t xref = used;
  used += (size_t)snprintf(source + used, sizeof source - used,
                           "xref\n0 9\n0000000000 65535 f \n");
  for (size_t i = 1; i <= 8; i++)
    used += (size_t)snprintf(source + used, sizeof source - used,
                             "%010zu 00000 n \n", offsets[i]);
  used += (size_t)snprintf(
      source + used, sizeof source - used,
      "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n", xref);
  spe_limits limits = spe_limits_default();
  limits.maximum_coordinate = 14400;
  spe_pdf_page_geometry* pages = nullptr;
  size_t count = 0, signatures = 0;
  CHECK(spe_pdf_pages((uint8_t*)source, used, &limits, &pages, &count,
                      &signatures, nullptr) == SNR_OK);
  CHECK(count == 1 && signatures == 1 && pages[0].media.width == 200);
  free(pages);
  spe_pdf_update* update = nullptr;
  CHECK(spe_pdf_update_create((uint8_t*)source, used, false, &limits, nullptr,
                              nullptr, nullptr, &update, nullptr) == SNR_OK);
  size_t drawn = 99;
  CHECK(spe_pdf_update_appearances(update, 0, false, &drawn, nullptr) ==
        SNR_OK);
  CHECK(drawn == 0);
  spe_pdf_update_free(update);
  /* Budget one inspection of a blank PDF, with one unit for native text.
   * The former sizing pass used the same budget a second time. */
  uint64_t remaining = 1000000;
  limits.remaining = &remaining;
  pages = nullptr;
  CHECK(spe_pdf_pages(source_pdf, sizeof source_pdf, &limits, &pages, &count,
                      nullptr, nullptr) == SNR_OK);
  free(pages);
  snr_pdf_text_options text = snr_pdf_text_options_default();
  text.limits.maximum_work = (size_t)(1000000 - remaining + 1);
  snr_buffer extracted = {0};
  snr_status status = snr_pdf_page_text(source_pdf, sizeof source_pdf, 0,
                                        nullptr, 0, &text, &extracted, nullptr);
  /* Cairo builds without PDFium deliberately have no extraction backend. */
#if defined(SNR_CAIRO_BACKEND) && !defined(SNR_PDFIUM)
  CHECK(status == SNR_ERR_UNSUPPORTED);
#else
  CHECK(status == SNR_OK);
#endif
  snr_buffer_free(&extracted);
  return 0;
}
int main(void) {
  return images() || signature_graphs() || damaged_object_stream() || pdf();
}
