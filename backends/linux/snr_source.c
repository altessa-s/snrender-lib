/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include "snr_overlay_internal.h"
#include "snr_pdf_text.h"
#ifdef SNR_PDFIUM
#include <fpdf_edit.h>
#include <fpdf_formfill.h>
#include <fpdf_text.h>
#include <fpdfview.h>
#include <glib.h>
#include <limits.h>
/* PDFium handles are confined to the serialized call; the engine stays alive
 * because other libraries in the process may also own PDFium handles. */
static GMutex mutex;
static gsize initialized;
static bool started, installed;
static void (*host_lock)(void*);
static void (*host_unlock)(void*);
static void* host_context;
snr_status snr_pdfium_set_lock(void (*lock)(void*), void (*unlock)(void*),
                               void* context, snr_error* error) {
  g_mutex_lock(&mutex);
  snr_status s = SNR_OK;
  if (!lock || !unlock || started || installed)
    s = snr_fail(error, SNR_ERR_INVALID_ARGUMENT, 0,
                 "PDFium lock requires one installation before PDFium use");
  else {
    host_lock = lock;
    host_unlock = unlock;
    host_context = context;
    installed = true;
  }
  g_mutex_unlock(&mutex);
  return s;
}
snr_status snr_source_bitmap(const uint8_t* bytes, size_t size, size_t page,
                             size_t count, const spe_limits* limits,
                             snr_bitmap* b, snr_error* e) {
  /* PDFium draws ordinary annotation appearances during the page render and
   * form-field widgets through a form environment. The environment has no
   * callbacks: version 1 calls only stable optional interfaces, and this
   * PDFium is built without V8 or XFA, so documents cannot run JavaScript.
   * Widgets outside the AcroForm field tree reach neither path, so the page
   * is first rewritten, before PDFium is locked, to draw just those. */
  snr_buffer flat = {0};
  size_t orphans = 0;
  snr_status flattened =
      snr_source_flatten(bytes, size, page, limits, true, &flat, &orphans, e);
  if (flattened) return flattened;
  if (orphans) {
    bytes = flat.bytes;
    size = flat.size;
  }
  g_mutex_lock(&mutex);
  started = true;
  if (host_lock) {
    g_mutex_unlock(&mutex);
    host_lock(host_context);
  }
  if (!initialized) {
    FPDF_InitLibrary();
    initialized = 1;
  }
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes, size, nullptr);
  snr_status s = SNR_OK;
  FPDF_PAGE p = nullptr;
  FPDF_BITMAP bitmap = nullptr;
  FPDF_FORMFILLINFO info = {.version = 1};
  FPDF_FORMHANDLE form = nullptr;
  if (!doc)
    s = snr_fail(e, SNR_ERR_BACKEND, page,
                 "source PDF is damaged or encrypted");
  else if ((size_t)FPDF_GetPageCount(doc) != count)
    s = snr_fail(e, SNR_ERR_BACKEND, page, "source PDF page tree disagreement");
  else if (page > INT_MAX || b->width > INT_MAX || b->height > INT_MAX ||
           b->stride > INT_MAX)
    s = snr_fail(e, SNR_ERR_LIMIT, page, "PDFium bitmap dimension limit");
  else {
    form = FPDFDOC_InitFormFillEnvironment(doc, &info);
    p = FPDF_LoadPage(doc, (int)page);
    bitmap = FPDFBitmap_CreateEx((int)b->width, (int)b->height, FPDFBitmap_BGRA,
                                 b->pixels, (int)b->stride);
    if (!p || !bitmap)
      s = snr_fail(e, SNR_ERR_BACKEND, page, "source PDF rendering failed");
    else {
      if (form) FORM_OnAfterLoadPage(p, form);
      FPDFBitmap_FillRect(bitmap, 0, 0, (int)b->width, (int)b->height,
                          0xffffffff);
      FPDF_RenderPageBitmap(bitmap, p, 0, 0, (int)b->width, (int)b->height, 0,
                            FPDF_ANNOT);
      if (form)
        FPDF_FFLDraw(form, bitmap, p, 0, 0, (int)b->width, (int)b->height, 0,
                     FPDF_ANNOT);
      if (form) FORM_OnBeforeClosePage(p, form);
      for (size_t i = 0; i < b->width * b->height; i++) {
        uint8_t t = b->pixels[i * 4];
        b->pixels[i * 4] = b->pixels[i * 4 + 2];
        b->pixels[i * 4 + 2] = t;
      }
    }
  }
  if (bitmap) FPDFBitmap_Destroy(bitmap);
  if (p) FPDF_ClosePage(p);
  if (form) FPDFDOC_ExitFormFillEnvironment(form);
  if (doc) FPDF_CloseDocument(doc);
  if (host_unlock)
    host_unlock(host_context);
  else
    g_mutex_unlock(&mutex);
  snr_buffer_free(&flat);
  return s;
}
/* Only scalar/box snapshots leave the lock; no caller callbacks or rectangle
 * tests run while PDFium or the optional host lock is held. */
typedef struct {
  uint32_t scalar;
  double left, bottom, right, top;
} text_character;
snr_status snr_source_text(const uint8_t* bytes, size_t size,
                           snr_pdf_text_state* state) {
  snr_status s = snr_pdf_text_charge(state, 0);
  if (s) return s;
  text_character* characters = nullptr;
  size_t copied = 0;
  g_mutex_lock(&mutex);
  started = true;
  if (host_lock) {
    g_mutex_unlock(&mutex);
    host_lock(host_context);
  }
  if (!initialized) {
    FPDF_InitLibrary();
    initialized = 1;
  }
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes, size, nullptr);
  FPDF_PAGE page = nullptr;
  FPDF_TEXTPAGE text = nullptr;
  if (!doc || (size_t)FPDF_GetPageCount(doc) != state->page_count ||
      state->page > INT_MAX)
    s = SNR_ERR_BACKEND;
  if (!s) {
    page = FPDF_LoadPage(doc, (int)state->page);
    if (page) {
      /* Change this temporary handle only. Boxes stay in user space; the
       * shared hit test retains the original crop and rotation. */
      FPDFPage_SetRotation(page, 0);
      text = FPDFText_LoadPage(page);
    }
    if (!text) s = SNR_ERR_BACKEND;
  }
  if (!s) {
    int count = FPDFText_CountChars(text);
    if (count < 0)
      s = SNR_ERR_BACKEND;
    else if ((size_t)count > state->options.maximum_characters ||
             (size_t)count > SIZE_MAX / sizeof *characters ||
             (uint64_t)count > state->remaining / (state->rect_count + 1))
      s = SNR_ERR_LIMIT;
    else if (count &&
             !(characters = malloc((size_t)count * sizeof *characters)))
      s = SNR_ERR_OUT_OF_MEMORY;
    for (int i = 0; !s && i < count; i++) {
      text_character c = {.scalar = FPDFText_GetUnicode(text, i),
                          .left = NAN,
                          .bottom = NAN,
                          .right = NAN,
                          .top = NAN};
      (void)FPDFText_GetCharBox(text, i, &c.left, &c.right, &c.bottom, &c.top);
      /* The public text-object API also returns processed text, not the
       * original glyph code. Preserve an ambiguous discretionary marker as
       * a hard hyphen; only an exposed U+00AD may be removed. */
      if (c.scalar == 2 && FPDFText_IsHyphen(text, i) == 1) c.scalar = '-';
      if (c.scalar >= 0xd800 && c.scalar <= 0xdbff && i + 1 < count) {
        uint32_t next = FPDFText_GetUnicode(text, i + 1);
        if (next >= 0xdc00 && next <= 0xdfff) {
          double left, right, bottom, top;
          if (FPDFText_GetCharBox(text, i + 1, &left, &right, &bottom, &top)) {
            c.left = fmin(c.left, left);
            c.right = fmax(c.right, right);
            c.bottom = fmin(c.bottom, bottom);
            c.top = fmax(c.top, top);
          }
          c.scalar = 0x10000 + ((c.scalar - 0xd800) << 10) + next - 0xdc00;
          i++;
        }
      }
      /* Count was bounded above; charge once per decoded scalar. Poll only
       * after unlock so cancellation may take locks or call this library. */
      state->remaining--;
      characters[copied++] = c;
    }
  }
  if (text) FPDFText_ClosePage(text);
  if (page) FPDF_ClosePage(page);
  if (doc) FPDF_CloseDocument(doc);
  if (host_unlock)
    host_unlock(host_context);
  else
    g_mutex_unlock(&mutex);
  for (size_t i = 0; !s && i < copied; i++) {
    text_character c = characters[i];
    s = snr_pdf_text_character(state, c.scalar, c.left, c.bottom, c.right,
                               c.top);
  }
  free(characters);
  return s;
}

#else
snr_status snr_source_text(const uint8_t* bytes, size_t size,
                           snr_pdf_text_state* state) {
  (void)bytes;
  (void)size;
  (void)state;
  return SNR_ERR_UNSUPPORTED;
}
snr_status snr_pdfium_set_lock(void (*lock)(void*), void (*unlock)(void*),
                               void* context, snr_error* error) {
  (void)lock;
  (void)unlock;
  (void)context;
  return snr_fail(error, SNR_ERR_UNSUPPORTED, 0, "PDFium backend unavailable");
}
snr_status snr_source_bitmap(const uint8_t* bytes, size_t size, size_t page,
                             size_t count, const spe_limits* limits,
                             snr_bitmap* b, snr_error* e) {
  (void)bytes;
  (void)size;
  (void)count;
  (void)limits;
  (void)b;
  return snr_fail(e, SNR_ERR_UNSUPPORTED, page,
                  "PDF overlay requires SNRENDER_ENABLE_PDFIUM");
}
#endif
