/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
/* The test decoder uses only zlib, independently of both platform codecs.
 * Generated RGBA/RGB PNGs are eight-bit, noninterlaced images. */
#ifndef SNR_TEST_PNG_DECODE_H
#define SNR_TEST_PNG_DECODE_H
#include <zlib.h>
static uint32_t png_u32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static unsigned png_paeth(unsigned left, unsigned above, unsigned corner) {
  int prediction = (int)left + (int)above - (int)corner;
  int a = abs(prediction - (int)left), b = abs(prediction - (int)above),
      c = abs(prediction - (int)corner);
  return a <= b && a <= c ? left : b <= c ? above : corner;
}
static uint8_t* png_decode(const snr_buffer* input, size_t* width,
                           size_t* height) {
  CHECK(input->size >= 33 && !memcmp(input->bytes, "\211PNG\r\n\032\n", 8));
  const uint8_t* p = input->bytes;
  CHECK(!memcmp(p + 12, "IHDR", 4));
  *width = png_u32(p + 16);
  *height = png_u32(p + 20);
  CHECK(p[24] == 8 && p[28] == 0);
  unsigned type = p[25];
  size_t channels = type == 0   ? 1
                    : type == 2 ? 3
                    : type == 4 ? 2
                    : type == 6 ? 4
                                : 0;
  CHECK(channels);
  uint8_t* compressed = malloc(input->size);
  CHECK(compressed);
  size_t used = 0;
  for (size_t i = 8; i + 12 <= input->size;) {
    size_t length = png_u32(p + i);
    CHECK(length <= input->size - i - 12);
    CHECK(crc32(0, p + i + 4, (uInt)(length + 4)) ==
          png_u32(p + i + 8 + length));
    if (!memcmp(p + i + 4, "IDAT", 4)) {
      memcpy(compressed + used, p + i + 8, length);
      used += length;
    }
    i += length + 12;
  }
  size_t stride = *width * channels;
  uLongf size = (uLongf)((stride + 1) * *height);
  uint8_t* raw = malloc(size);
  CHECK(raw);
  CHECK(uncompress(raw, &size, compressed, (uLong)used) == Z_OK);
  CHECK(size == (stride + 1) * *height);
  free(compressed);
  uint8_t* rgba = malloc(*width * *height * 4);
  CHECK(rgba);
  for (size_t y = 0; y < *height; y++) {
    uint8_t* row = raw + y * (stride + 1) + 1;
    unsigned filter = row[-1];
    CHECK(filter <= 4);
    for (size_t x = 0; x < stride; x++) {
      unsigned left = x >= channels ? row[x - channels] : 0;
      unsigned above = y ? (row - stride - 1)[x] : 0;
      unsigned corner =
          y && x >= channels ? (row - stride - 1)[x - channels] : 0;
      unsigned prediction = filter == 1   ? left
                            : filter == 2 ? above
                            : filter == 3 ? (left + above) / 2
                            : filter == 4 ? png_paeth(left, above, corner)
                                          : 0;
      row[x] = (uint8_t)(row[x] + prediction);
    }
    for (size_t x = 0; x < *width; x++) {
      const uint8_t* src = row + x * channels;
      uint8_t* dst = rgba + (y * *width + x) * 4;
      for (size_t c = 0; c < 3; c++)
        dst[c] = src[type == 0 || type == 4 ? 0 : c];
      dst[3] = type == 4 || type == 6 ? src[channels - 1] : 255;
    }
  }
  free(raw);
  return rgba;
}
#endif
