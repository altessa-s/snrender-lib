/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <zlib.h>

#include "snr_internal.h"
static uint32_t big32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}
/* Dimension preflight happens before handing compressed bytes to a platform
 * decoder. A decoder's resize callback may otherwise allocate the original
 * image before producing its smaller result. */
snr_status snr_image_size(const sn_bitmap* image, size_t limit, size_t* width,
                          size_t* height) {
  const uint8_t* b = image->bytes.data;
  size_t n = image->bytes.size;
  size_t w = 0, h = 0;
  if (!b) return SNR_ERR_BACKEND;
  if (image->encoding == SN_ENCODING_PNG) {
    static const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (n < 33 || memcmp(b, signature, 8) || big32(b + 8) != 13 ||
        memcmp(b + 12, "IHDR", 4))
      return SNR_ERR_BACKEND;
    w = big32(b + 16);
    h = big32(b + 20);
  } else if (image->encoding == SN_ENCODING_JPEG) {
    if (n < 4 || b[0] != 255 || b[1] != 216) return SNR_ERR_BACKEND;
    size_t i = 2;
    while (i < n) {
      if (b[i++] != 255) return SNR_ERR_BACKEND;
      while (i < n && b[i] == 255) i++;
      if (i == n) return SNR_ERR_BACKEND;
      unsigned marker = b[i++];
      if (marker == 217 || marker == 218) break;
      if (marker == 1) return SNR_ERR_BACKEND;
      if (marker >= 208 && marker <= 215) continue;
      if (n - i < 2) return SNR_ERR_BACKEND;
      size_t length = ((size_t)b[i] << 8) | b[i + 1];
      if (length < 2 || length > n - i) return SNR_ERR_BACKEND;
      if (marker >= 192 && marker <= 207 && marker != 196 && marker != 200 &&
          marker != 204) {
        if (length < 8) return SNR_ERR_BACKEND;
        h = ((size_t)b[i + 3] << 8) | b[i + 4];
        w = ((size_t)b[i + 5] << 8) | b[i + 6];
        break;
      }
      i += length;
    }
  } else
    return SNR_ERR_UNSUPPORTED;
  if (!w || !h) return SNR_ERR_BACKEND;
  if (w > 16777216 || h > 16777216 || w > limit / h || w > SIZE_MAX / 4 / h)
    return SNR_ERR_LIMIT;
  *width = w;
  *height = h;
  return SNR_OK;
}

/* Platform decoders receive only a complete image. Ignoring bytes after the
 * first terminator gives both implementations identical framing semantics. */
snr_status snr_image_frame(const sn_bitmap* image, const snr_options* options,
                           size_t* size) {
  const uint8_t* b = image->bytes.data;
  size_t n = image->bytes.size;
  if (!b) return SNR_ERR_BACKEND;
  if (image->encoding == SN_ENCODING_PNG) {
    if (n < 33 || memcmp(b, "\211PNG\r\n\032\n", 8)) return SNR_ERR_BACKEND;
    size_t offset = 8;
    bool data = false;
    while (n - offset >= 12) {
      const uint8_t* p = b + offset;
      size_t length = big32(p);
      if (length > n - offset - 12) return SNR_ERR_BACKEND;
      uLong crc = crc32(0, Z_NULL, 0);
      for (size_t j = 4; j < length + 8;) {
        if (snr_cancelled(options)) return SNR_ERR_CANCELLED;
        size_t part = length + 8 - j;
        if (part > 65536) part = 65536;
        crc = crc32(crc, p + j, (uInt)part);
        j += part;
      }
      if (crc != big32(p + length + 8)) return SNR_ERR_BACKEND;
      if (!memcmp(p + 4, "IDAT", 4)) data = true;
      if (!memcmp(p + 4, "IHDR", 4) && (offset != 8 || length != 13))
        return SNR_ERR_BACKEND;
      offset += length + 12;
      if (!memcmp(p + 4, "IEND", 4)) {
        if (length || !data) return SNR_ERR_BACKEND;
        *size = offset;
        return SNR_OK;
      }
    }
  } else if (image->encoding == SN_ENCODING_JPEG) {
    if (n < 4 || b[0] != 255 || b[1] != 216) return SNR_ERR_BACKEND;
    size_t i = 2;
    bool scan = false, entropy = false;
    while (i < n) {
      if (snr_cancelled(options)) return SNR_ERR_CANCELLED;
      if (entropy) {
        while (i < n && b[i] != 255) {
          if (i % 65536 == 0 && snr_cancelled(options))
            return SNR_ERR_CANCELLED;
          i++;
        }
      }
      if (i == n || b[i++] != 255) return SNR_ERR_BACKEND;
      while (i < n && b[i] == 255) i++;
      if (i == n) return SNR_ERR_BACKEND;
      unsigned marker = b[i++];
      if (entropy && (marker == 0 || (marker >= 208 && marker <= 215)))
        continue;
      if (marker == 217) {
        if (!scan) return SNR_ERR_BACKEND;
        *size = i;
        return SNR_OK;
      }
      if (marker == 0 || marker == 216 || (marker >= 208 && marker <= 215))
        return SNR_ERR_BACKEND;
      /* TEM is unused by ordinary image encoders and platform decoders disagree
       * on accepting it, so it is outside the shared image contract. */
      if (marker == 1) return SNR_ERR_BACKEND;
      entropy = false;
      if (n - i < 2) return SNR_ERR_BACKEND;
      size_t length = ((size_t)b[i] << 8) | b[i + 1];
      if (length < 2 || length > n - i) return SNR_ERR_BACKEND;
      if (marker == 218) {
        if (length < 6 || length != 6u + 2u * b[i + 2]) return SNR_ERR_BACKEND;
        scan = entropy = true;
      }
      i += length;
    }
  }
  return SNR_ERR_BACKEND;
}
