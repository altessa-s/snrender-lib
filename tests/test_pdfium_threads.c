/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <fpdfview.h>
#include <pthread.h>
#include <snrender/snrender.h>
#include <stdlib.h>
#include <string.h>

#include "source_pdf.h"
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static snr_bitmap reference;
static _Thread_local bool held;
static void lock(void* context) {
  (void)context;
  if (pthread_mutex_lock(&mutex)) abort();
  held = true;
}
static void unlock(void* context) {
  (void)context;
  held = false;
  if (pthread_mutex_unlock(&mutex)) abort();
}
static bool cancel(void* context) {
  (void)context;
  /* A cancellation callback may take the host lock. The old text scan called
   * it under that same lock; fail immediately instead of hanging the test. */
  if (held) abort();
  lock(nullptr);
  unlock(nullptr);
  return false;
}
static snr_status preview(snr_bitmap* b) {
  sn_notebook n = {.kind = SN_KIND_MARK, .sheet = {120, 160}};
  return snr_mark_page_bitmap(&n, source_pdf, sizeof source_pdf, 0, nullptr, b,
                              nullptr);
}
static void* render(void* context) {
  (void)context;
  for (unsigned i = 0; i < 30; i++) {
    snr_bitmap b = {0};
    if (preview(&b) || b.width != reference.width ||
        b.height != reference.height ||
        memcmp(b.pixels, reference.pixels, b.stride * b.height))
      abort();
    snr_bitmap_free(&b);
    snr_buffer text = {0};
    sn_rect rect = {0, 0, 120, 160, false};
    snr_pdf_text_options options = snr_pdf_text_options_default();
    options.cancel = cancel;
    if (snr_pdf_page_text(source_pdf, sizeof source_pdf, 0, &rect, 1, &options,
                          &text, nullptr) ||
        !text.bytes || text.size || text.bytes[0])
      abort();
    snr_buffer_free(&text);
  }
  return nullptr;
}
static void* foreign(void* context) {
  (void)context;
  for (unsigned i = 0; i < 120; i++) {
    lock(nullptr);
    FPDF_DOCUMENT d =
        FPDF_LoadMemDocument64(source_pdf, sizeof source_pdf, nullptr);
    if (!d) abort();
    FPDF_PAGE p = FPDF_LoadPage(d, 0);
    FPDF_BITMAP b = FPDFBitmap_Create(120, 160, 1);
    if (!p || !b) abort();
    FPDF_RenderPageBitmap(b, p, 0, 0, 120, 160, 0, 0);
    FPDFBitmap_Destroy(b);
    FPDF_ClosePage(p);
    FPDF_CloseDocument(d);
    unlock(nullptr);
  }
  return nullptr;
}
int main(void) {
  if (snr_pdfium_set_lock(lock, unlock, nullptr, nullptr)) return 1;
  if (preview(&reference)) return 1;
  if (snr_pdfium_set_lock(lock, unlock, nullptr, nullptr) !=
      SNR_ERR_INVALID_ARGUMENT)
    return 1;
  pthread_t workers[5];
  for (unsigned i = 0; i < 5; i++)
    if (pthread_create(&workers[i], nullptr, i == 4 ? foreign : render,
                       nullptr))
      abort();
  for (unsigned i = 0; i < 5; i++)
    if (pthread_join(workers[i], nullptr)) abort();
  snr_bitmap_free(&reference);
  return 0;
}
