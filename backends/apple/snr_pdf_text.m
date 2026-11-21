/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include "snr_pdf_text.h"
#import <PDFKit/PDFKit.h>

#ifdef SNR_TESTING
size_t snr_test_text_exception_after = SIZE_MAX;
bool snr_test_text_exception_in_selection = false;
bool snr_test_text_exception_is_malloc = false;

static void snr_text_exception_probe(bool in_selection) {
  if (snr_test_text_exception_in_selection == in_selection &&
      snr_test_text_exception_after != SIZE_MAX && !snr_test_text_exception_after--) {
    @throw [NSException
        exceptionWithName:snr_test_text_exception_is_malloc ? NSMallocException
                                                            : NSInternalInconsistencyException
                   reason:nil
                 userInfo:nil];
  }
}
#endif

static snr_status snr_text_exception_status(NSException* exception) {
  return [exception.name isEqualToString:NSMallocException] ? SNR_ERR_OUT_OF_MEMORY
                                                            : SNR_ERR_BACKEND;
}

static snr_status snr_text_selection(PDFPage* page, NSRange range, uint32_t c,
                                     snr_pdf_text_state* state) {
  /* Catch inside the pool: without ARC exception cleanup, unwinding across
   * @autoreleasepool skips its drain. Explicitly clear every strong local
   * in @finally, on both normal and exceptional exits. */
  @autoreleasepool {
    PDFSelection* selection = nil;
    @try {
      selection = [page selectionForRange:range];
#ifdef SNR_TESTING
      snr_text_exception_probe(true);
#endif
      CGRect box = selection ? [selection boundsForPage:page] : CGRectNull;
      snr_status s = snr_pdf_text_charge(state, 1);
      if (s) return s;
      return snr_pdf_text_character(state, c, CGRectGetMinX(box), CGRectGetMinY(box),
                                    CGRectGetMaxX(box), CGRectGetMaxY(box));
    } @catch (NSException* exception) {
      return snr_text_exception_status(exception);
    } @finally {
      selection = nil;
    }
  }
}

snr_status snr_source_text(const uint8_t* bytes, size_t size, snr_pdf_text_state* state) {
  @autoreleasepool {
    NSData* data = nil;
    PDFDocument* doc = nil;
    PDFPage* page = nil;
    NSString* text = nil;
    @try {
      /* Borrow input for this synchronous call. The finally block releases
       * engine objects before the caller can release the bytes, even when
       * compiled with plain -fobjc-arc (without -fobjc-arc-exceptions). */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-qual"
      data = [NSData dataWithBytesNoCopy:(void*)bytes length:size freeWhenDone:NO];
#pragma clang diagnostic pop
      doc = [[PDFDocument alloc] initWithData:data];
      if (!doc || doc.isEncrypted || doc.pageCount != state->page_count) return SNR_ERR_BACKEND;
      page = [doc pageAtIndex:state->page];
      if (!page) return SNR_ERR_BACKEND;
      NSUInteger count = page.numberOfCharacters;
      if (count > (NSUInteger)NSIntegerMax || count > state->options.maximum_characters ||
          count > state->remaining / (state->rect_count + 1))
        return SNR_ERR_LIMIT;
      snr_status s = snr_pdf_text_charge(state, 0);
      if (s) return s;
      text = page.string;
      if (text.length != count) return SNR_ERR_BACKEND;
      for (NSUInteger i = 0; i < count; i++) {
        uint32_t c = [text characterAtIndex:i];
        /* characterBoundsAtIndex uses glyph indices on some PDFKit versions,
         * while string includes generated newlines and expanded ligatures.
         * A one-scalar range selection uses the string's UTF-16 indices. */
        NSUInteger start = i;
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < count) {
          uint32_t next = [text characterAtIndex:i + 1];
          if (next >= 0xdc00 && next <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + next - 0xdc00;
            i++;
          }
        }
#ifdef SNR_TESTING
        snr_text_exception_probe(false);
#endif
        s = snr_text_selection(page, NSMakeRange(start, i - start + 1), c, state);
        if (s) return s;
      }
      return SNR_OK;
    } @catch (NSException* exception) {
      return snr_text_exception_status(exception);
    } @finally {
      text = nil;
      page = nil;
      doc = nil;
      data = nil;
    }
  }
}
