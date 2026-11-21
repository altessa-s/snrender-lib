/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <math.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      abort();                                                \
    }                                                         \
  } while (0)
static sn_rect displayed(sn_rect r, int rotation) {
  switch (rotation) {
    case 90:
      return (sn_rect){200 - r.y - r.height, r.x, r.height, r.width, false};
    case 180:
      return (sn_rect){300 - r.x - r.width, 200 - r.y - r.height, r.width,
                       r.height, false};
    case 270:
      return (sn_rect){r.y, 300 - r.x - r.width, r.height, r.width, false};
    default:
      return r;
  }
}
#ifdef SNR_TEXT_SUPPORTED
static void expect(const uint8_t* pdf, size_t size, const sn_rect* rects,
                   size_t count, const snr_pdf_text_options* o, size_t page,
                   const char* expected) {
  snr_buffer b = {0};
  snr_error e = {.code = SNR_ERR_BACKEND, .index = 444};
  snr_status s = snr_pdf_page_text(pdf, size, page, rects, count, o, &b, &e);
  if (s || b.size != strlen(expected) || memcmp(b.bytes, expected, b.size)) {
    fprintf(stderr, "text status %u: %s; expected [%s], got [%.*s]\n", s,
            e.message, expected, (int)b.size,
            b.bytes ? (const char*)b.bytes : "");
    abort();
  }
  CHECK(b.bytes && !b.bytes[b.size] && e.index == 444);
  snr_buffer_free(&b);
}
#endif
static void failure(const uint8_t* pdf, size_t size, size_t page,
                    const sn_rect* r, size_t count,
                    const snr_pdf_text_options* o, snr_status wanted) {
  uint8_t sentinel = 0;
  snr_buffer b = {&sentinel, 77};
  snr_error e = {0};
  snr_status s = snr_pdf_page_text(pdf, size, page, r, count, o, &b, &e);
  if (s != wanted)
    fprintf(stderr, "expected error %u got %u (%s)\n", wanted, s, e.message);
  CHECK(s == wanted && e.code == s && e.index == page && e.message[0]);
  CHECK(b.bytes == &sentinel && b.size == 77);
}
#ifdef SNR_TEXT_SUPPORTED
static bool cancelled(void* context) {
  size_t* remaining = context;
  if (*remaining) {
    (*remaining)--;
    return false;
  }
  return true;
}
#endif
int main(int argc, char** argv) {
  CHECK(argc == 3 || argc == 6);
  FILE* f = fopen(argv[1], "rb");
  CHECK(f && !fseek(f, 0, SEEK_END));
  long length = ftell(f);
  CHECK(length > 0 && !fseek(f, 0, SEEK_SET));
  size_t size = (size_t)length;
  uint8_t* pdf = malloc(size);
  CHECK(pdf && fread(pdf, 1, size, f) == size);
  fclose(f);
  if (argc == 6) {
    /* Independent generator supplies literal expected strings and rectangles.
     */
    sn_rect rects[8];
    size_t count = 0;
    FILE* rectangles = fopen(argv[4], "r");
    CHECK(rectangles);
    while (count < 8 && fscanf(rectangles, "%lf %lf %lf %lf", &rects[count].x,
                               &rects[count].y, &rects[count].width,
                               &rects[count].height) == 4) {
      rects[count++].is_null = false;
    }
    fclose(rectangles);
    /* The expected text arrives as a UTF-8 file: Windows passes command-line
     * arguments through the ANSI code page, which cannot carry every scalar. */
    char expected[4096];
    FILE* text = fopen(argv[3], "rb");
    CHECK(text);
    size_t used = fread(expected, 1, sizeof expected - 1, text);
    CHECK(!ferror(text) && feof(text));
    fclose(text);
    expected[used] = '\0';
#ifdef SNR_TEXT_SUPPORTED
    expect(pdf, size, rects, count, nullptr, (size_t)atoi(argv[5]), expected);
#else
    (void)expected;
    failure(pdf, size, (size_t)atoi(argv[5]), rects, count, nullptr,
            SNR_ERR_UNSUPPORTED);
#endif
    free(pdf);
    return 0;
  }
  int rotation = atoi(argv[2]);
  sn_rect r = displayed((sn_rect){19, 16, 100, 18, false}, rotation);
  snr_pdf_text_options o = snr_pdf_text_options_default();
#ifndef SNR_TEXT_SUPPORTED
  failure(pdf, size, 0, &r, 1, nullptr, SNR_ERR_UNSUPPORTED);
#else
  expect(pdf, size, &r, 1, nullptr, 0, "Alpha Beta");
  struct {
    snr_pdf_text_options prefix;
    uint64_t future;
  } extended = {.prefix = snr_pdf_text_options_default()};
  extended.prefix.struct_size = sizeof extended;
  expect(pdf, size, &r, 1, &extended.prefix, 0, "Alpha Beta");
  sn_rect narrow = displayed((sn_rect){20, 16, 7, 18, false}, rotation);
  expect(pdf, size, &narrow, 1, nullptr, 0, "A");
  narrow = displayed((sn_rect){20, 16, 1, 18, false}, rotation);
  expect(pdf, size, &narrow, 1, nullptr, 0, "");
  sn_rect union_rects[] = {
      displayed((sn_rect){19, 66, 100, 18, false}, rotation),
      displayed((sn_rect){19, 41, 100, 18, false}, rotation),
      displayed((sn_rect){19, 41, 100, 18, false}, rotation)};
  expect(pdf, size, union_rects, 3, nullptr, 0, "multi-line words");
  union_rects[1] = r;
  expect(pdf, size, union_rects, 2, nullptr, 0, "Alpha Beta\nline words");
  r = displayed((sn_rect){19, 91, 100, 18, false}, rotation);
  expect(pdf, size, &r, 1, nullptr, 0, "Жёлтый");
  r = displayed((sn_rect){19, 116, 100, 18, false}, rotation);
  expect(pdf, size, &r, 1, nullptr, 0, "fi");
  r = displayed((sn_rect){19, 141, 200, 18, false}, rotation);
#ifdef SNR_TEST_PDFKIT
  expect(pdf, size, &r, 1, nullptr, 0, "ab cdefg");
#else
  expect(pdf, size, &r, 1, nullptr, 0, "abcdefg");
#endif
  r = displayed((sn_rect){19, 166, 100, 34, false}, rotation);
#ifdef SNR_TEST_PDFKIT
  expect(pdf, size, &r, 1, nullptr, 0, "ab");
#else
  expect(pdf, size, &r, 1, nullptr, 0, "a-b");
#endif
  r = (sn_rect){-100, -100, 5, 5, false};
  expect(pdf, size, &r, 1, nullptr, 0, "");
  r.is_null = true;
  expect(pdf, size, &r, 1, nullptr, 0, "");
  expect(pdf, size, nullptr, 0, nullptr, 0, "");
  r = displayed((sn_rect){19, 16, 100, 18, false}, rotation);
  failure(pdf, size, 1, &r, 1, nullptr, SNR_ERR_INVALID_ARGUMENT);
  failure((const uint8_t*)"bad", 3, 0, &r, 1, nullptr,
          SNR_ERR_INVALID_ARGUMENT);
  o.maximum_source_bytes = size - 1;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_LIMIT);
  o = snr_pdf_text_options_default();
  o.maximum_characters = 1;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_LIMIT);
  o = snr_pdf_text_options_default();
  o.limits.maximum_pages = 0;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_LIMIT);
  o = snr_pdf_text_options_default();
  o.limits.maximum_work = 1;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_LIMIT);
  o = snr_pdf_text_options_default();
  o.limits.maximum_document_work = 1;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_LIMIT);
  o = snr_pdf_text_options_default();
  o.limits.maximum_output_bytes = 10;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_LIMIT);
  o.limits.maximum_output_bytes = 11;
  expect(pdf, size, &r, 1, &o, 0, "Alpha Beta");
  o.limits.maximum_output_bytes = 0;
  failure(pdf, size, 0, nullptr, 0, &o, SNR_ERR_LIMIT);
  o = snr_pdf_text_options_default();
  size_t remaining = 0;
  o.cancel = cancelled;
  o.cancel_context = &remaining;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_CANCELLED);
  remaining = SIZE_MAX;
  expect(pdf, size, &r, 1, &o, 0, "Alpha Beta");
  size_t polls = SIZE_MAX - remaining;
  CHECK(polls > 10);
  remaining = polls - 10; /* Cancel during the character scan, after parsing. */
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_CANCELLED);
#ifdef SNR_TESTING
  extern size_t snr_test_growth_failure;
  o = snr_pdf_text_options_default();
  snr_test_growth_failure = 0;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_OUT_OF_MEMORY);
  snr_test_growth_failure = SIZE_MAX;
  expect(pdf, size, &r, 1, &o, 0, "Alpha Beta");
#endif
#endif
  o = snr_pdf_text_options_default();
  o.struct_size--;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_INVALID_ARGUMENT);
  o = snr_pdf_text_options_default();
  o.abi_version++;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_INVALID_ARGUMENT);
  o = snr_pdf_text_options_default();
  o.maximum_rectangles = 0;
  failure(pdf, size, 0, &r, 1, &o, SNR_ERR_LIMIT);
  failure(pdf, size, 0, &r, SIZE_MAX, nullptr, SNR_ERR_LIMIT);
  failure(nullptr, size, 0, &r, 1, nullptr, SNR_ERR_INVALID_ARGUMENT);
  failure(pdf, 0, 0, &r, 1, nullptr, SNR_ERR_INVALID_ARGUMENT);
  failure(pdf, size, 0, nullptr, 1, nullptr, SNR_ERR_INVALID_ARGUMENT);
  r = (sn_rect){0, 0, -1, 10, false};
  failure(pdf, size, 0, &r, 1, nullptr, SNR_ERR_INVALID_ARGUMENT);
  r = (sn_rect){NAN, 0, 10, 10, false};
  failure(pdf, size, 0, &r, 1, nullptr, SNR_ERR_INVALID_ARGUMENT);
  free(pdf);
  return 0;
}
