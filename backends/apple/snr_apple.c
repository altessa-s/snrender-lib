/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>
#include <limits.h>

#include "snr_internal.h"
static CGImageRef image(const snr_bitmap* b) {
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CFDataRef data =
      CFDataCreate(nullptr, b->pixels, (CFIndex)(b->stride * b->height));
  CGDataProviderRef provider =
      data ? CGDataProviderCreateWithCFData(data) : nullptr;
  if (data) CFRelease(data);
  CGImageRef result =
      provider && cs
          ? CGImageCreate(
                b->width, b->height, 8, 32, b->stride, cs,
                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big,
                provider, nullptr, false, kCGRenderingIntentDefault)
          : nullptr;
  if (provider) CGDataProviderRelease(provider);
  if (cs) CGColorSpaceRelease(cs);
  return result;
}
snr_status snr_decode(const sn_bitmap* encoded, size_t limit,
                      const snr_options* options, snr_bitmap* out) {
  size_t expected_width, expected_height;
  snr_status checked =
      snr_image_size(encoded, limit, &expected_width, &expected_height);
  if (checked) return checked;
  sn_bitmap framed = *encoded;
  checked = snr_image_frame(encoded, options, &framed.bytes.size);
  if (checked) return checked;
  encoded = &framed;

  if (!encoded->bytes.data || !encoded->bytes.size ||
      encoded->bytes.size > LONG_MAX)
    return SNR_ERR_BACKEND;
  CFDataRef data = CFDataCreateWithBytesNoCopy(nullptr, encoded->bytes.data,
                                               (CFIndex)encoded->bytes.size,
                                               kCFAllocatorNull);
  if (!data) return SNR_ERR_OUT_OF_MEMORY;
  CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
  CFRelease(data);
  if (!source) return SNR_ERR_BACKEND;
  CFDictionaryRef props =
      CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr);
  long long width = 0, height = 0;
  if (props) {
    CFNumberRef wn = CFDictionaryGetValue(props, kCGImagePropertyPixelWidth),
                hn = CFDictionaryGetValue(props, kCGImagePropertyPixelHeight);
    if (wn && CFGetTypeID(wn) == CFNumberGetTypeID())
      CFNumberGetValue(wn, kCFNumberLongLongType, &width);
    if (hn && CFGetTypeID(hn) == CFNumberGetTypeID())
      CFNumberGetValue(hn, kCFNumberLongLongType, &height);
    CFRelease(props);
  }
  if (width <= 0 || height <= 0 || (uint64_t)width != expected_width ||
      (uint64_t)height != expected_height) {
    CFRelease(source);
    return SNR_ERR_BACKEND;
  }
  if ((uint64_t)width > limit / (uint64_t)height ||
      (uint64_t)width > SIZE_MAX / 4 / (uint64_t)height) {
    CFRelease(source);
    return SNR_ERR_LIMIT;
  }
  if (CGImageSourceGetStatusAtIndex(source, 0) != kCGImageStatusComplete) {
    CFRelease(source);
    return SNR_ERR_BACKEND;
  }
  const void* cache_key = kCGImageSourceShouldCacheImmediately;
  const void* cache_value = kCFBooleanTrue;
  CFDictionaryRef decode_options = CFDictionaryCreate(
      nullptr, &cache_key, &cache_value, 1, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  if (!decode_options) {
    CFRelease(source);
    return SNR_ERR_OUT_OF_MEMORY;
  }
  CGImageRef img = CGImageSourceCreateImageAtIndex(source, 0, decode_options);
  CFRelease(decode_options);
  if (CGImageSourceGetStatusAtIndex(source, 0) != kCGImageStatusComplete) {
    if (img) CGImageRelease(img);
    img = nullptr;
  }
  CFRelease(source);
  if (!img) return SNR_ERR_BACKEND;
  snr_bitmap b = {.width = (size_t)width,
                  .height = (size_t)height,
                  .stride = (size_t)width * 4};
  b.pixels = calloc(b.height, b.stride);
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx =
      b.pixels && cs
          ? CGBitmapContextCreate(
                b.pixels, b.width, b.height, 8, b.stride, cs,
                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big)
          : nullptr;
  if (cs) CGColorSpaceRelease(cs);
  if (!ctx) {
    CGImageRelease(img);
    snr_bitmap_free(&b);
    return SNR_ERR_OUT_OF_MEMORY;
  }

  CGContextDrawImage(ctx, CGRectMake(0, 0, (double)b.width, (double)b.height),
                     img);
  CGContextRelease(ctx);
  CGImageRelease(img);
  *out = b;
  return SNR_OK;
}
static size_t put(void* info, const void* bytes, size_t size) {
  return snr_write(info, bytes, size) == SNR_OK ? size : 0;
}
static CGDataConsumerRef consumer(snr_writer* w) {
  CGDataConsumerCallbacks cb = {put, nullptr};
  return CGDataConsumerCreate(w, &cb);
}
snr_status snr_encode_png(const snr_bitmap* b, snr_writer* w) {
  CGImageRef img = image(b);
  if (!img) return SNR_ERR_OUT_OF_MEMORY;
  CGDataConsumerRef c = consumer(w);
  CGImageDestinationRef dest = c ? CGImageDestinationCreateWithDataConsumer(
                                       c, CFSTR("public.png"), 1, nullptr)
                                 : nullptr;
  if (c) CGDataConsumerRelease(c);
  if (!dest) {
    CGImageRelease(img);
    return SNR_ERR_OUT_OF_MEMORY;
  }
  CGImageDestinationAddImage(dest, img, nullptr);
  bool ok = CGImageDestinationFinalize(dest);
  CFRelease(dest);
  CGImageRelease(img);
  return w->status ? w->status : ok ? SNR_OK : SNR_ERR_BACKEND;
}
static CFStringRef text(sn_string str) {
  if (!str.data || str.length > LONG_MAX) return nullptr;
  return CFStringCreateWithBytes(nullptr, (const UInt8*)str.data,
                                 (CFIndex)str.length, kCFStringEncodingUTF8,
                                 false);
}
static CFStringRef destination(size_t page) {
  char s[64];
  snprintf(s, sizeof s, "page-%zu", page);
  return CFStringCreateWithCString(nullptr, s, kCFStringEncodingASCII);
}
static bool colour_font(CTFontRef font) {
  if (CTFontGetSymbolicTraits(font) & kCTFontColorGlyphsTrait) return true;
  const CTFontTableTag tags[] = {0x73626978, 0x434f4c52, 0x53564720,
                                 0x43424454};
  for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) {
    CFDataRef table =
        CTFontCopyTable(font, tags[i], kCTFontTableOptionNoOptions);
    if (table) {
      CFRelease(table);
      return true;
    }
  }
  return false;
}
static snr_status recognition_draw(CGContextRef ctx, const sn_page* p,
                                   double height, const snr_spacing* spacing) {
  if (!p->recognition) return SNR_OK;
  CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica"), 12, nullptr);
  if (!font) return SNR_ERR_OUT_OF_MEMORY;
  int ligatures = 0;
  CFNumberRef ligature = CFNumberCreate(nullptr, kCFNumberIntType, &ligatures);
  if (!ligature) {
    CFRelease(font);
    return SNR_ERR_OUT_OF_MEMORY;
  }
  const void* keys[] = {kCTFontAttributeName, kCTLigatureAttributeName,
                        kCTKernAttributeName};
  const void* values[] = {font, ligature, ligature};
  CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 3,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
  if (!attrs) {
    CFRelease(ligature);
    CFRelease(font);
    return SNR_ERR_OUT_OF_MEMORY;
  }
  snr_status status = SNR_OK;
  for (size_t i = 0; i < p->recognition->element_count; i++) {
    const sn_element* el = p->recognition->elements + i;
    snr_text_line text_line = snr_text_line_create(
        el, p->size.width * height / p->size.height * 25.4 / 72,
        height * 25.4 / 72);
    for (size_t j = 0; j < el->word_count; j++) {
      const sn_word* word = el->words + j;
      sn_rect r = word->box;
      if (!snr_box_clip(&r, p->size.width * height / p->size.height * 25.4 / 72,
                        height * 25.4 / 72))
        continue;
      snr_text_line_box(&text_line, el, j,
                        p->size.width * height / p->size.height * 25.4 / 72,
                        height * 25.4 / 72, &r);
      status = text_line.explicit_lines && text_line.valid
                   ? SNR_OK
                   : snr_word_spacing(&r, spacing);
      if (status) goto done;
      snr_text clean = snr_recognition_text(word->label);
      if (clean.status) {
        status = clean.status;
        goto done;
      }
      if (clean.length) {
        char* spaced = realloc(clean.data, clean.length + 2);
        if (!spaced) {
          free(clean.data);
          status = SNR_ERR_OUT_OF_MEMORY;
          goto done;
        }
        clean.data = spaced;
        clean.data[clean.length++] = ' ';
        clean.data[clean.length] = 0;
      }
      CFStringRef str =
          clean.length ? text((sn_string){clean.data, clean.length}) : nullptr;
      free(clean.data);
      if (!str) {
        if (clean.length) {
          status = SNR_ERR_OUT_OF_MEMORY;
          goto done;
        }
        continue;
      }
      bool dense = clean.length > 32768;
      if (dense) {
        r = word->box;
        snr_box_clip(&r, p->size.width * height / p->size.height * 25.4 / 72,
                     height * 25.4 / 72);
      }
      CTFontRef mono =
          dense ? CTFontCreateWithName(CFSTR("Courier"), 12, nullptr) : nullptr;
      CFMutableDictionaryRef word_attrs =
          dense ? CFDictionaryCreateMutableCopy(nullptr, 0, attrs) : nullptr;
      if (dense && (!mono || !word_attrs)) {
        if (mono) CFRelease(mono);
        if (word_attrs) CFRelease(word_attrs);
        CFRelease(str);
        status = SNR_ERR_OUT_OF_MEMORY;
        goto done;
      }
      if (dense) CFDictionarySetValue(word_attrs, kCTFontAttributeName, mono);
      CFAttributedStringRef a =
          CFAttributedStringCreate(nullptr, str, dense ? word_attrs : attrs);
      if (word_attrs) CFRelease(word_attrs);
      if (mono) CFRelease(mono);
      if (!a) {
        CFRelease(str);
        status = SNR_ERR_OUT_OF_MEMORY;
        goto done;
      }
      CTLineRef line = CTLineCreateWithAttributedString(a);
      CFRelease(a);
      if (!line) {
        CFRelease(str);
        status = SNR_ERR_OUT_OF_MEMORY;
        goto done;
      }
      CGFloat ascent = 0, descent = 0;
      double width =
          CTLineGetTypographicBounds(line, &ascent, &descent, nullptr) -
          CTLineGetTrailingWhitespaceWidth(line);
      if (width > 0 && ascent + descent > 0) {
        double factor = 72 / 25.4;
        double sy = r.height * factor / (ascent + descent);
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(
            ctx, r.x * factor,
            height - (r.y + r.height) * factor + descent * sy);
        /* Dense words retain a nominal monospace advance above Poppler's
         * tiny-glyph cutoff; explicit positions still fit the stored width.
         * Raw extraction preserves the text, while geometric readers may
         * suppress characters as overlapping duplicates. */
        double sx = dense ? 0.5 : r.width * factor / width;
        CGContextScaleCTM(ctx, sx, sy);
        CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
        CGContextSetTextPosition(ctx, 0, 0);
        CGContextSetTextDrawingMode(ctx, kCGTextInvisible);
        CFArrayRef runs = CTLineGetGlyphRuns(line);
        bool safe = true;
        for (CFIndex k = 0; k < CFArrayGetCount(runs); k++) {
          CTRunRef run = CFArrayGetValueAtIndex(runs, k);
          CTFontRef run_font = CFDictionaryGetValue(CTRunGetAttributes(run),
                                                    kCTFontAttributeName);
          if (colour_font(run_font)) safe = false;
        }
        if (safe && dense) {
          for (CFIndex k = 0; k < CFArrayGetCount(runs); k++) {
            CTRunRef run = CFArrayGetValueAtIndex(runs, k);
            CFIndex count = CTRunGetGlyphCount(run);
            CGGlyph* glyphs = malloc((size_t)count * sizeof *glyphs);
            CGPoint* positions = malloc((size_t)count * sizeof *positions);
            if (!glyphs || !positions) {
              free(glyphs);
              free(positions);
              status = SNR_ERR_OUT_OF_MEMORY;
              break;
            }
            CTRunGetGlyphs(run, CFRangeMake(0, count), glyphs);
            CTRunGetPositions(run, CFRangeMake(0, count), positions);
            for (CFIndex q = 0; q < count; q++)
              positions[q].x *= r.width * factor / width / sx;
            CTFontRef run_font = CFDictionaryGetValue(CTRunGetAttributes(run),
                                                      kCTFontAttributeName);
            CTFontDrawGlyphs(run_font, glyphs, positions, (size_t)count, ctx);
            free(glyphs);
            free(positions);
          }
        } else if (safe)
          CTLineDraw(line, ctx);
        else
          snr_log(SNR_LOG_DEBUG, "colour recognition word omitted");
        CGContextRestoreGState(ctx);
      }
      CFRelease(line);
      CFRelease(str);
      if (status) goto done;
    }
  }
done:
  CFRelease(attrs);
  CFRelease(ligature);
  CFRelease(font);
  return status;
}
static snr_status recognition(CGContextRef ctx, const sn_page* p, double height,
                              snr_text_work* work) {
  snr_spacing spacing;
  snr_status s = snr_spacing_create(
      p->recognition, p->size.width * height / p->size.height * 25.4 / 72,
      height * 25.4 / 72, work, &spacing);
  if (!s) s = recognition_draw(ctx, p, height, &spacing);
  free(spacing.boxes);
  return s;
}
static snr_status links(CGContextRef ctx, const sn_notebook* n,
                        const snr_plan* p, size_t first, size_t count) {
  for (size_t i = 0; i < n->link_count; i++) {
    const sn_link* l = n->links + i;
    if (l->page_number != p->page->number || l->rect.is_null ||
        l->direction != 0)
      continue;
    double f = p->points_per_pixel;
    sn_rect r = l->rect;
    if (!snr_box_clip(&r, p->page->size.width, p->page->size.height)) continue;
    CGRect rect =
        CGRectMake(r.x * f, (p->page->size.height - r.y - r.height) * f,
                   r.width * f, r.height * f);
    if (l->kind == 4) {
      if (!snr_uri_allowed(l->url)) continue;
      CFStringRef str = text(l->url);
      if (!str) return SNR_ERR_OUT_OF_MEMORY;
      CFURLRef url =
          str ? CFURLCreateWithString(nullptr, str, nullptr) : nullptr;
      if (url) {
        CGPDFContextSetURLForRect(ctx, url, rect);
        CFRelease(url);
      } else
        snr_log(SNR_LOG_DEBUG, "unparseable URI omitted");
      if (str) CFRelease(str);
    } else {
      size_t target = snr_link_target(n, l, first, count);
      if (target != SIZE_MAX) {
        CFStringRef dest = destination(target);
        if (!dest) return SNR_ERR_OUT_OF_MEMORY;
        CGPDFContextSetDestinationForRect(ctx, dest, rect);
        CFRelease(dest);
      } else
        snr_log(SNR_LOG_DEBUG, "link target outside exported document omitted");
    }
  }
  return SNR_OK;
}
/* Titles are handwritten snapshots and have no label in the model. Stable
 * page-and-title labels keep their navigation useful without inventing OCR. */
static snr_status pdf_outline(CGContextRef ctx, const sn_notebook* n,
                              size_t first, size_t count) {
  CFMutableDictionaryRef root =
      CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
  if (!root) return SNR_ERR_OUT_OF_MEMORY;
  snr_status status = SNR_OK;
  CFMutableArrayRef arrays[65] = {0};
  int64_t levels[65] = {0};
  size_t depth = 0;
  arrays[0] = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
  if (!arrays[0]) {
    CFRelease(root);
    return SNR_ERR_OUT_OF_MEMORY;
  }
  CFDictionarySetValue(root, kCGPDFOutlineChildren, arrays[0]);
  for (size_t i = 0; i < n->title_count; i++) {
    const sn_title* t = n->titles + i;
    size_t page = SIZE_MAX;
    for (size_t j = first; j < first + count; j++)
      if (n->pages[j].number == t->page_number) {
        page = j;
        break;
      }
    if (page == SIZE_MAX) continue;
    while (depth && levels[depth] >= t->level) {
      CFRelease(arrays[depth]);
      depth--;
    }
    char label[96];
    snprintf(label, sizeof label, "Page %lld — title %zu",
             (long long)t->page_number, i + 1);
    CFStringRef title =
        CFStringCreateWithCString(nullptr, label, kCFStringEncodingUTF8);
    CFIndex page_index = (CFIndex)(page - first + 1);
    CFNumberRef dest =
        CFNumberCreate(nullptr, kCFNumberCFIndexType, &page_index);
    CFMutableDictionaryRef node =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks);
    if (!title || !dest || !node) {
      if (title) CFRelease(title);
      if (dest) CFRelease(dest);
      if (node) CFRelease(node);
      status = SNR_ERR_OUT_OF_MEMORY;
      break;
    }
    CFDictionarySetValue(node, kCGPDFOutlineTitle, title);
    CFDictionarySetValue(node, kCGPDFOutlineDestination, dest);
    CFMutableArrayRef children =
        CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    if (!children) {
      CFRelease(node);
      CFRelease(title);
      CFRelease(dest);
      status = SNR_ERR_OUT_OF_MEMORY;
      break;
    }
    CFDictionarySetValue(node, kCGPDFOutlineChildren, children);
    CFArrayAppendValue(arrays[depth], node);
    CFRelease(node);
    CFRelease(title);
    CFRelease(dest);
    if (depth < 64) {
      depth++;
      arrays[depth] = children;
      levels[depth] = t->level;
    } else
      CFRelease(children);
  }
  if (!status) CGPDFContextSetOutline(ctx, root);
  for (size_t i = 0; i <= depth; i++) CFRelease(arrays[i]);
  CFRelease(root);
  return status;
}
snr_status snr_backend_pdf(const sn_notebook* n, size_t first, size_t count,
                           const snr_options* o, snr_writer* w, snr_error* e) {
  CGDataConsumerRef c = consumer(w);
  if (!c) return SNR_ERR_OUT_OF_MEMORY;
  CFStringRef title = CFStringCreateWithCString(
      nullptr, o->title ? o->title : "Supernote notebook",
      kCFStringEncodingUTF8);
  if (!title) {
    CGDataConsumerRelease(c);
    return SNR_ERR_OUT_OF_MEMORY;
  }
  const void* keys[] = {kCGPDFContextTitle, kCGPDFContextCreator};
  const void* vals[] = {title ? title : CFSTR("Supernote notebook"),
                        CFSTR("snrender")};
  CFDictionaryRef info =
      CFDictionaryCreate(nullptr, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  if (!info) {
    if (title) CFRelease(title);
    CGDataConsumerRelease(c);
    return SNR_ERR_OUT_OF_MEMORY;
  }
  CGContextRef ctx = CGPDFContextCreate(c, nullptr, info);
  CFRelease(info);
  if (title) CFRelease(title);
  CGDataConsumerRelease(c);
  if (!ctx) return SNR_ERR_OUT_OF_MEMORY;
  snr_status s = SNR_OK;
  size_t active_page = first;
  if (o->progress) o->progress(o->progress_context, 0, count);
  for (size_t i = first; i < first + count && !w->status; i++) {
    active_page = i;
    snr_plan p;
    s = snr_plan_make(n, i, o, &p, e);
    if (s) break;
    snr_bitmap b = {0};
    s = snr_compose(&p, &b, e);
    if (s) break;
    CGImageRef img = image(&b);
    if (!img) {
      snr_bitmap_free(&b);
      s = SNR_ERR_OUT_OF_MEMORY;
      break;
    }
    CGRect box = CGRectMake(0, 0, p.page->size.width * p.points_per_pixel,
                            p.page->size.height * p.points_per_pixel);
    CFDataRef box_data = CFDataCreate(nullptr, (const UInt8*)&box, sizeof box);
    if (!box_data) {
      CGImageRelease(img);
      snr_bitmap_free(&b);
      s = SNR_ERR_OUT_OF_MEMORY;
      break;
    }
    const void* key = kCGPDFContextMediaBox;
    const void* val = box_data;
    CFDictionaryRef page_info = CFDictionaryCreate(
        nullptr, &key, &val, 1, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!page_info) {
      CFRelease(box_data);
      CGImageRelease(img);
      snr_bitmap_free(&b);
      s = SNR_ERR_OUT_OF_MEMORY;
      break;
    }
    CGPDFContextBeginPage(ctx, page_info);
    CFRelease(page_info);
    CFRelease(box_data);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, box, img);
    CFStringRef dest = destination(i);
    if (dest) {
      CGPDFContextAddDestinationAtPoint(ctx, dest,
                                        CGPointMake(0, box.size.height));
      CFRelease(dest);
    } else
      s = SNR_ERR_OUT_OF_MEMORY;
    snr_text_work work = {p.work + n->title_count + n->link_count,
                          o->limits.maximum_work, &w->document_work,
                          o->limits.maximum_document_work};
    if (!s && o->recognition)
      s = recognition(ctx, p.page, box.size.height, &work);
    if (!s) s = links(ctx, n, &p, first, count);
    CGPDFContextEndPage(ctx);
    CGImageRelease(img);
    snr_bitmap_free(&b);
    if (s || w->status) break;
    if (o->progress) o->progress(o->progress_context, i - first + 1, count);
  }
  /* The first failure wins. A sink that failed while pages were still being
   * added failed before, or during, any failure found since; a sink that
   * refuses only the bytes that close the document after an earlier failure
   * leaves that failure as the result. */
  bool sink_first = w->status != SNR_OK;
  if (!s && !sink_first) s = pdf_outline(ctx, n, first, count);
  CGPDFContextClose(ctx);
  CGContextRelease(ctx);
  bool sink_failed = w->status && (!s || sink_first);
  if (sink_failed) s = w->status;
  if (!s && snr_cancelled(o)) s = SNR_ERR_CANCELLED;
  if (sink_failed)
    snr_fail(e, s, active_page, snr_writer_message(w));
  else if (s && (!e || !e->code))
    snr_fail(e, s, active_page, "PDF page output failed");
  return s;
}

snr_status snr_encode_pdf_bitmap(const snr_bitmap* b, double width,
                                 double height, snr_writer* w) {
  CGImageRef img = image(b);
  if (!img) return SNR_ERR_OUT_OF_MEMORY;
  CGDataConsumerRef c = consumer(w);
  CGRect box = CGRectMake(0, 0, width, height);
  CGContextRef ctx = c ? CGPDFContextCreate(c, &box, nullptr) : nullptr;
  if (c) CGDataConsumerRelease(c);
  if (!ctx) {
    CGImageRelease(img);
    return SNR_ERR_OUT_OF_MEMORY;
  }
  CGPDFContextBeginPage(ctx, nullptr);
  CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
  CGContextDrawImage(ctx, box, img);
  CGPDFContextEndPage(ctx);
  CGPDFContextClose(ctx);
  CGContextRelease(ctx);
  CGImageRelease(img);
  return w->status;
}
