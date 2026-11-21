/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <CoreGraphics/CoreGraphics.h>

#include "snr_overlay_internal.h"
/* True when the page has an annotation with an appearance dictionary. */
static bool has_appearances(CGPDFPageRef p) {
  CGPDFArrayRef annotations = nullptr;
  if (!CGPDFDictionaryGetArray(CGPDFPageGetDictionary(p), "Annots",
                               &annotations))
    return false;
  for (size_t i = 0; i < CGPDFArrayGetCount(annotations); i++) {
    CGPDFDictionaryRef annotation = nullptr, appearance = nullptr;
    if (CGPDFArrayGetDictionary(annotations, i, &annotation) &&
        CGPDFDictionaryGetDictionary(annotation, "AP", &appearance))
      return true;
  }
  return false;
}
/* CoreGraphics draws page content but never annotations, which PDFium draws.
 * A page with appearances is therefore first rewritten, in memory, to invoke
 * them all from its own content; the rewritten copy is what gets drawn. */
static snr_status draw(const uint8_t* bytes, size_t size, size_t page,
                       size_t count, const spe_limits* limits, bool rewrite,
                       snr_bitmap* b, snr_error* e) {
  CGDataProviderRef provider =
      CGDataProviderCreateWithData(nullptr, bytes, size, nullptr);
  CGPDFDocumentRef doc =
      provider ? CGPDFDocumentCreateWithProvider(provider) : nullptr;
  if (provider) CGDataProviderRelease(provider);
  snr_status s = SNR_OK;
  CGPDFPageRef p = nullptr;
  if (!doc || CGPDFDocumentIsEncrypted(doc))
    s = snr_fail(e, SNR_ERR_BACKEND, page,
                 "source PDF is damaged or encrypted");
  else if (CGPDFDocumentGetNumberOfPages(doc) != count)
    s = snr_fail(e, SNR_ERR_BACKEND, page, "source PDF page tree disagreement");
  else if (!(p = CGPDFDocumentGetPage(doc, page + 1)))
    s = snr_fail(e, SNR_ERR_BACKEND, page, "source PDF rendering failed");
  if (!s && rewrite && has_appearances(p)) {
    snr_buffer flat = {0};
    size_t drawn = 0;
    s = snr_source_flatten(bytes, size, page, limits, false, &flat, &drawn, e);
    CGPDFDocumentRelease(doc);
    if (!s)
      s = drawn ? draw(flat.bytes, flat.size, page, count, limits, false, b, e)
                : draw(bytes, size, page, count, limits, false, b, e);
    snr_buffer_free(&flat);
    return s;
  }
  if (!s) {
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef c =
        rgb ? CGBitmapContextCreate(
                  b->pixels, b->width, b->height, 8, b->stride, rgb,
                  kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big)
            : nullptr;
    if (rgb) CGColorSpaceRelease(rgb);
    if (!c)
      s = snr_fail(e, SNR_ERR_BACKEND, page, "source PDF rendering failed");
    else {
      CGContextSetRGBFillColor(c, 1, 1, 1, 1);
      CGContextFillRect(c,
                        CGRectMake(0, 0, (double)b->width, (double)b->height));
      /* CGPDFPageGetDrawingTransform never scales up, so the transform is
       * built here: the displayed crop, after rotation, fills the bitmap at
       * any scale. */
      CGRect crop = CGPDFPageGetBoxRect(p, kCGPDFCropBox);
      int rotation = ((CGPDFPageGetRotationAngle(p) % 360) + 360) % 360;
      bool swapped = rotation == 90 || rotation == 270;
      double w = swapped ? crop.size.height : crop.size.width;
      double h = swapped ? crop.size.width : crop.size.height;
      CGContextScaleCTM(c, (double)b->width / w, (double)b->height / h);
      switch (rotation) {
        case 90:
          CGContextTranslateCTM(c, 0, crop.size.width);
          CGContextRotateCTM(c, -M_PI_2);
          break;
        case 180:
          CGContextTranslateCTM(c, crop.size.width, crop.size.height);
          CGContextRotateCTM(c, M_PI);
          break;
        case 270:
          CGContextTranslateCTM(c, crop.size.height, 0);
          CGContextRotateCTM(c, M_PI_2);
          break;
        default:
          break;
      }
      CGContextTranslateCTM(c, -crop.origin.x, -crop.origin.y);
      /* A page without content draws nothing. Asking CoreGraphics to draw it
       * anyway makes its missing-stream path retain an otherwise unreachable
       * CGPDFContentStream. */
      CGPDFObjectRef contents = nullptr;
      if (CGPDFDictionaryGetObject(CGPDFPageGetDictionary(p), "Contents",
                                   &contents) &&
          CGPDFObjectGetType(contents) != kCGPDFObjectTypeNull)
        CGContextDrawPDFPage(c, p);
    }
    if (c) CGContextRelease(c);
  }
  if (doc) CGPDFDocumentRelease(doc);
  return s;
}
snr_status snr_source_bitmap(const uint8_t* bytes, size_t size, size_t page,
                             size_t count, const spe_limits* limits,
                             snr_bitmap* b, snr_error* e) {
  return draw(bytes, size, page, count, limits, true, b, e);
}

snr_status snr_pdfium_set_lock(void (*lock)(void*), void (*unlock)(void*),
                               void* context, snr_error* error) {
  (void)lock;
  (void)unlock;
  (void)context;
  return snr_fail(error, SNR_ERR_UNSUPPORTED, 0, "PDFium backend unavailable");
}
