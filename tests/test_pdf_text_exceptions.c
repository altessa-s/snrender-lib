/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
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

/* These hooks exist only in the static SNR_TESTING library, like the output
 * allocation hook. Run this test under leaks --atExit as well as ASan: ARC
 * without -fobjc-arc-exceptions must release objects and drain both pools. */
#ifdef SNR_TESTING
extern size_t snr_test_text_exception_after;
extern bool snr_test_text_exception_in_selection;
extern bool snr_test_text_exception_is_malloc;
#endif

int main(int argc, char** argv) {
  CHECK(argc == 2);
  FILE* f = fopen(argv[1], "rb");
  CHECK(f && !fseek(f, 0, SEEK_END));
  long length = ftell(f);
  CHECK(length > 0 && !fseek(f, 0, SEEK_SET));
  size_t size = (size_t)length;
  uint8_t* pdf = malloc(size);
  CHECK(pdf && fread(pdf, 1, size, f) == size);
  fclose(f);

  sn_rect rect = {19, 16, 100, 18, false};
  for (unsigned selection = 0; selection < 2; selection++) {
    for (unsigned oom = 0; oom < 2; oom++) {
      snr_test_text_exception_in_selection = selection != 0;
      snr_test_text_exception_is_malloc = oom != 0;
      /* Throw before output allocation, then after partial output. Repetition
       * makes leaked document/text/selection objects visible to leaks. */
      for (size_t after = 0; after <= 5; after += 5) {
        for (unsigned repeat = 0; repeat < 128; repeat++) {
          snr_test_text_exception_after = after;
          uint8_t sentinel = 0;
          snr_buffer out = {&sentinel, 77};
          snr_error error = {0};
          snr_status wanted = oom ? SNR_ERR_OUT_OF_MEMORY : SNR_ERR_BACKEND;
          CHECK(snr_pdf_page_text(pdf, size, 0, &rect, 1, nullptr, &out,
                                  &error) == wanted);
          CHECK(error.code == wanted && error.index == 0 && error.message[0]);
          CHECK(out.bytes == &sentinel && out.size == 77);
          CHECK(snr_test_text_exception_after == SIZE_MAX);
        }
        /* The same borrowed bytes remain usable after every failure mode. */
        snr_buffer out = {0};
        CHECK(snr_pdf_page_text(pdf, size, 0, &rect, 1, nullptr, &out,
                                nullptr) == SNR_OK);
        CHECK(out.size == 10 && !memcmp(out.bytes, "Alpha Beta", 10));
        snr_buffer_free(&out);
      }
    }
  }
  free(pdf);
  puts("PDFKit: 1024 injected exceptions and recovery passed");
  return 0;
}
