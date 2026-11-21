/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
  if (argc != 4) return 2;
  CFURLRef pdf_url = CFURLCreateFromFileSystemRepresentation(
      nullptr, (const UInt8*)argv[1], (CFIndex)strlen(argv[1]), false);
  CFURLRef png_url = CFURLCreateFromFileSystemRepresentation(
      nullptr, (const UInt8*)argv[2], (CFIndex)strlen(argv[2]), false);
  CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(pdf_url);
  CGImageSourceRef src = CGImageSourceCreateWithURL(png_url, nullptr);
  CFRelease(pdf_url);
  CFRelease(png_url);
  if (!doc || !src) return 1;
  CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
  CFRelease(src);
  if (!image) return 1;
  size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
  uint8_t* a = calloc(height, width * 4);
  uint8_t* b = calloc(height, width * 4);
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef ca = CGBitmapContextCreate(
                   a, width, height, 8, width * 4, cs,
                   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big),
               cb = CGBitmapContextCreate(
                   b, width, height, 8, width * 4, cs,
                   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(cs);
  if (!ca || !cb) return 1;
  CGRect box = CGRectMake(0, 0, (double)width, (double)height);
  CGContextSetRGBFillColor(ca, 1, 1, 1, 1);
  CGContextFillRect(ca, box);
  CGContextSetRGBFillColor(cb, 1, 1, 1, 1);
  CGContextFillRect(cb, box);
  CGContextDrawImage(ca, box, image);
  CGPDFPageRef page = CGPDFDocumentGetPage(doc, 1);
  CGRect media = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
  CGContextScaleCTM(cb, (double)width / media.size.width,
                    (double)height / media.size.height);
  CGContextSetInterpolationQuality(cb, kCGInterpolationNone);
  CGContextDrawPDFPage(cb, page);
  unsigned maximum = 0;
  double sum = 0;
  size_t changed = 0;
  for (size_t i = 0; i < width * height; i++) {
    unsigned delta =
        a[i * 4] > b[i * 4] ? a[i * 4] - b[i * 4] : b[i * 4] - a[i * 4];
    if (delta) changed++;
    if (delta > maximum) maximum = delta;
    sum += delta;
  }
  printf("%zu\t%u\t%.9f\n", changed, maximum, sum / (double)(width * height));
  FILE* f = fopen(argv[3], "wb");
  if (!f) return 1;
  fprintf(f, "P6\n%zu %zu\n255\n", width, height);
  for (size_t i = 0; i < width * height; i++) fwrite(b + i * 4, 1, 3, f);
  fclose(f);
  CGContextRelease(ca);
  CGContextRelease(cb);
  CGImageRelease(image);
  CGPDFDocumentRelease(doc);
  free(a);
  free(b);
  return 0;
}
