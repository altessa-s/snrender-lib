/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <pthread.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t* pdf;
static size_t size;
static sn_rect rect = {0, 0, 400, 300, false};
static void extract(const snr_pdf_text_options* options) {
  snr_buffer b = {0};
  if (snr_pdf_page_text(pdf, size, 0, &rect, 1, options, &b, nullptr) ||
      b.size != 11 || memcmp(b.bytes, "Hello World", 12))
    abort();
  snr_buffer_free(&b);
}
typedef struct {
  size_t polls, trigger;
  bool called;
} cancellation;
static bool reenter(void* context) {
  cancellation* c = context;
  c->polls++;
  if (!c->called && c->polls == c->trigger) {
    c->called = true;
    extract(nullptr);
  }
  return false;
}
static void* worker(void* context) {
  (void)context;
  snr_pdf_text_options options = snr_pdf_text_options_default();
  cancellation count = {0};
  options.cancel = reenter;
  options.cancel_context = &count;
  extract(&options);
  if (count.polls < 10) abort();
  for (unsigned i = 0; i < 20; i++) {
    /* Last five polls are after engine loading, during scalar hit testing.
     * Reenter there as well as concurrently from the other worker threads. */
    cancellation c = {.trigger = count.polls - 5};
    options.cancel_context = &c;
    extract(&options);
    if (!c.called) abort();
  }
  return nullptr;
}
int main(int argc, char** argv) {
  if (argc != 2) abort();
  FILE* f = fopen(argv[1], "rb");
  if (!f || fseek(f, 0, SEEK_END)) abort();
  long length = ftell(f);
  if (length <= 0 || fseek(f, 0, SEEK_SET)) abort();
  size = (size_t)length;
  pdf = malloc(size);
  if (!pdf || fread(pdf, 1, size, f) != size) abort();
  fclose(f);
  pthread_t threads[4];
  for (unsigned i = 0; i < 4; i++)
    if (pthread_create(&threads[i], nullptr, worker, nullptr)) abort();
  for (unsigned i = 0; i < 4; i++)
    if (pthread_join(threads[i], nullptr)) abort();
  free(pdf);
  return 0;
}
