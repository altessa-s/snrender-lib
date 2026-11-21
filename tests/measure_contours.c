/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  long length = ftell(f);
  rewind(f);
  uint8_t* bytes = malloc((size_t)length);
  if (!bytes) return 1;
  size_t got = fread(bytes, 1, (size_t)length, f);
  fclose(f);
  sn_notebook* n = nullptr;
  sn_status status = sn_parse_notebook(bytes, got, &n, nullptr);
  free(bytes);
  if (status) return 1;
  for (size_t page = 0; page < n->page_count; page++) {
    const sn_page* p = n->pages + page;
    size_t width = (size_t)p->size.width, height = (size_t)p->size.height;
    for (size_t l = 0; l < p->layer_count; l++) {
      const sn_layer* layer = p->layers + l;
      if (!layer->is_visible || !layer->bitmap ||
          layer->bitmap->encoding != SN_ENCODING_RATTA_RLE ||
          layer->name == SN_LAYER_BACKGROUND)
        continue;
      sn_raster* target = nullptr;
      if (sn_bitmap_raster(layer->bitmap, width, height, &n->palette, &target,
                           nullptr) != SN_OK)
        continue;
      uint8_t* pixels = malloc(width * height * 4);
      if (!pixels) return 1;
      memset(pixels, 255, width * height * 4);
      CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
      CGContextRef ctx = CGBitmapContextCreate(
          pixels, width, height, 8, width * 4, cs,
          kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
      CGColorSpaceRelease(cs);
      if (!ctx) return 1;
      /* Page contours already use the bitmap's top-left pixel coordinate
       * system. */
      CGContextTranslateCTM(ctx, 0, (double)height);
      CGContextScaleCTM(ctx, 1, -1);
      size_t strokes = 0, missing = 0;
      for (size_t j = 0; j < p->drawn_stroke_count; j++) {
        const sn_stroke* stroke = p->drawn_strokes[j];
        if (stroke->layer != (uint32_t)sn_layer_name_identifier(layer->name))
          continue;
        strokes++;
        uint8_t gray = 0;
        if (!stroke->contour_count ||
            !sn_ink_color_gray(stroke->color, &gray)) {
          missing++;
          continue;
        }
        CGContextSetRGBFillColor(ctx, gray / 255.0, gray / 255.0, gray / 255.0,
                                 1);
        CGContextBeginPath(ctx);
        for (size_t k = 0; k < stroke->contour_count; k++) {
          const sn_contour* ring = stroke->contours + k;
          if (!ring->count) continue;
          CGContextMoveToPoint(ctx, ring->points[0].x, ring->points[0].y);
          for (size_t m = 1; m < ring->count; m++)
            CGContextAddLineToPoint(ctx, ring->points[m].x, ring->points[m].y);
          CGContextClosePath(ctx);
        }
        CGContextFillPath(ctx);
      }
      CGContextRelease(ctx);
      size_t changed = 0;
      unsigned worst = 0;
      uint64_t sum = 0;
      for (size_t i = 0; i < width * height; i++) {
        unsigned expected = ((unsigned)target->gray[i] * target->alpha[i] +
                             255u * (255 - target->alpha[i])) /
                            255;
        unsigned actual = pixels[i * 4];
        unsigned delta =
            actual > expected ? actual - expected : expected - actual;
        if (delta) changed++;
        if (delta > worst) worst = delta;
        sum += delta;
      }
      printf("%zu\t%d\t%zu\t%zu\t%zu\t%zu\t%u\t%.9f\n", page, (int)layer->name,
             strokes, missing, width * height, changed, worst,
             (double)sum / (double)(width * height));
      free(pixels);
      sn_raster_free(target);
    }
  }
  sn_notebook_free(n);
  return 0;
}
