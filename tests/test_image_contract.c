/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
  if (argc != 2) return 1;
  FILE* file = fopen(argv[1], "rb");
  if (!file) return 1;
  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  rewind(file);
  if (length < 4) return 1;
  size_t size = (size_t)length;
  unsigned char* data = malloc(size + 2);
  if (!data || fread(data, 1, size, file) != size) return 1;
  fclose(file);
  sn_bitmap image = {.encoding = SN_ENCODING_JPEG, .bytes = {data, size}};
  sn_layer layer = {.bitmap = &image, .is_visible = true};
  sn_page page = {.size = {1404, 1872}, .layers = &layer, .layer_count = 1};
  sn_notebook book = {.pages = &page, .page_count = 1};
  snr_options options = snr_options_default();
  options.scale = 0.0625;
  options.strict_images = true;
  snr_bitmap bitmap = {0};
  bool ok = snr_page_bitmap(&book, 0, &options, &bitmap, nullptr) == SNR_OK;
  snr_bitmap_free(&bitmap);
  for (unsigned i = 0; i < 2; i++) {
    if (!i)
      image.bytes.size = size - 2;
    else {
      memmove(data + 4, data + 2, size - 2);
      data[2] = 255;
      data[3] = 1;
      image.bytes.size = size + 2;
    }
    options.strict_images = true;
    ok = ok && snr_page_bitmap(&book, 0, &options, &bitmap, nullptr) ==
                   SNR_ERR_BACKEND;
    options.strict_images = false;
    ok = ok && snr_page_bitmap(&book, 0, &options, &bitmap, nullptr) == SNR_OK;
    if (bitmap.pixels) ok = ok && bitmap.pixels[0] == 255;
    snr_bitmap_free(&bitmap);
  }
  free(data);
  puts("JPEG missing EOI and TEM: omitted by default, rejected in strict mode");
  return ok ? 0 : 1;
}
