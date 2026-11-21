/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "../../src/snr_text.h"

/* A small zero-area TrueType face avoids scale-specific Type 3 text subsets.
 * Unicode mappings come from Cairo's text clusters, not the font's cmap.
 * Each used scalar or cluster receives a slot; unused slots are never embedded.
 */
typedef struct {
  char bytes[32];
  size_t length;
  unsigned glyph, left, right, height;
} scalar_slot;
typedef struct {
  FT_Library library;
  FT_Face face;
  uint8_t* bytes;
  scalar_slot* slots;
  size_t capacity, used;
  unsigned root;
  bool space_seeded;
  snr_text_work* work;
} text_font;
static void be16(uint8_t* p, unsigned v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
static void be32(uint8_t* p, uint32_t v) {
  be16(p, v >> 16);
  be16(p + 2, v & 65535);
}
static snr_status font_create(text_font* font) {
  enum { glyph_count = 65535, table_count = 8 };
  const char* tags[] = {"head", "hhea", "maxp", "hmtx",
                        "loca", "glyf", "name", "post"};
  size_t sizes[] = {
      54, 36, 32, glyph_count * 4, (glyph_count + 1) * 4, glyph_count * 24,
      6,  32};
  size_t offsets[table_count], size = 12 + table_count * 16;
  for (size_t i = 0; i < table_count; i++) {
    offsets[i] = size;
    if (sizes[i] > SIZE_MAX - 3) return SNR_ERR_LIMIT;
    size_t padded = (sizes[i] + 3) & ~(size_t)3;
    if (padded > SIZE_MAX - size) return SNR_ERR_LIMIT;
    size += padded;
  }
  /* Validate each complete table span before forming pointers or writing its
   * fixed fields and bounded glyph arrays, including checksum padding. */
  const size_t required[] = {
      54, 36, 32, glyph_count * 4, (glyph_count + 1) * 4, glyph_count * 24,
      6,  32};
  for (size_t i = 0; i < table_count; i++)
    if (sizes[i] < required[i] || offsets[i] > size ||
        ((sizes[i] + 3) & ~(size_t)3) > size - offsets[i])
      return SNR_ERR_LIMIT;
  if (size > LONG_MAX || size > UINT32_MAX) return SNR_ERR_LIMIT;
  font->bytes = calloc(size, 1);
  if (!font->bytes) return SNR_ERR_OUT_OF_MEMORY;
  uint8_t* b = font->bytes;
  be32(b, 0x10000);
  be16(b + 4, table_count);
  be16(b + 6, 128);
  be16(b + 8, 3);
  uint8_t* head = b + offsets[0];
  be32(head, 0x10000);
  be32(head + 4, 0x10000);
  be32(head + 12, 0x5f0f3cf5);
  be16(head + 18, 1000);
  be16(head + 38, 65036);
  be16(head + 40, 1000);
  be16(head + 42, 500);
  be16(head + 46, 8);
  be16(head + 48, 2);
  be16(head + 50, 1);
  uint8_t* hhea = b + offsets[1];
  be32(hhea, 0x10000);
  be16(hhea + 4, 500);
  be16(hhea + 6, 65036);
  be16(hhea + 10, 1000);
  be16(hhea + 16, 1000);
  be16(hhea + 18, 1);
  be16(hhea + 34, glyph_count);
  be32(b + offsets[2], 0x10000);
  be16(b + offsets[2] + 4, glyph_count);
  be16(b + offsets[2] + 6, 2);
  be16(b + offsets[2] + 8, 1);
  be16(b + offsets[2] + 14, 1);
  for (unsigned i = 0; i < glyph_count - 1; i++)
    be16(b + offsets[3] + i * 4, 1000);
  be16(b + offsets[3] + (glyph_count - 1) * 4, 100);
  for (unsigned i = 0; i <= glyph_count; i++)
    be32(b + offsets[4] + i * 4, i * 24);
  /* Coincident contour points cannot paint even when a consumer ignores the
   * mask. The offset point allows outward-rounded metrics to retain narrow
   * text operations without giving the contour any paintable area. */
  for (unsigned i = 0; i < glyph_count; i++) {
    uint8_t* glyph = b + offsets[5] + i * 24;
    be16(glyph, 1);
    be16(glyph + 10, 1);
    glyph[14] = glyph[15] = 1;
    be16(glyph + 16, 500);
    be16(glyph + 20, 250);
    be16(glyph + 6, 1000);
    be16(glyph + 4, 65036);
    be16(glyph + 8, 500);
  }
  be16(b + offsets[6] + 4, 6);
  be32(b + offsets[7], 0x30000);
  for (size_t i = 0; i < table_count; i++) {
    uint8_t* entry = b + 12 + i * 16;
    memcpy(entry, tags[i], 4);
    uint32_t checksum = 0;
    for (size_t j = 0; j < sizes[i]; j += 4) {
      const uint8_t* v = b + offsets[i] + j;
      checksum += ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
                  ((uint32_t)v[2] << 8) | v[3];
    }
    be32(entry + 4, checksum);
    be32(entry + 8, (uint32_t)offsets[i]);
    be32(entry + 12, (uint32_t)sizes[i]);
  }
  /* Sorted directory records and the sfnt checksum keep the generated face
   * valid for consumers that verify more than the outline data. */
  for (size_t i = 1; i < table_count; i++) {
    uint8_t entry[16];
    memcpy(entry, b + 12 + i * 16, 16);
    size_t j = i;
    while (j && memcmp(b + 12 + (j - 1) * 16, entry, 4) > 0) {
      memcpy(b + 12 + j * 16, b + 12 + (j - 1) * 16, 16);
      j--;
    }
    memcpy(b + 12 + j * 16, entry, 16);
  }
  uint32_t checksum = 0;
  for (size_t i = 0; i < size; i += 4)
    checksum += ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                ((uint32_t)b[i + 2] << 8) | b[i + 3];
  be32(head + 8, 0xb1b0afbau - checksum);
  FT_Error error = FT_Init_FreeType(&font->library);
  if (!error)
    error = FT_New_Memory_Face(font->library, b, (FT_Long)size, 0, &font->face);
  return error == FT_Err_Out_Of_Memory ? SNR_ERR_OUT_OF_MEMORY
         : error                       ? SNR_ERR_BACKEND
                                       : SNR_OK;
}

/* Arena indices survive growth, while AVL balance bounds every key search
 * independently of insertion order and the bytes supplied by the caller. */
static unsigned scalar_height(const text_font* font, unsigned node) {
  return node ? font->slots[node - 1].height : 0;
}
static void scalar_height_update(text_font* font, unsigned node) {
  scalar_slot* slot = &font->slots[node - 1];
  unsigned left = scalar_height(font, slot->left);
  unsigned right = scalar_height(font, slot->right);
  slot->height = 1 + (left > right ? left : right);
}
static unsigned scalar_rotate(text_font* font, unsigned node, bool left) {
  scalar_slot* slot = &font->slots[node - 1];
  unsigned next = left ? slot->right : slot->left;
  scalar_slot* child = &font->slots[next - 1];
  if (left) {
    slot->right = child->left;
    child->left = node;
  } else {
    slot->left = child->right;
    child->right = node;
  }
  scalar_height_update(font, node);
  scalar_height_update(font, next);
  return next;
}
static unsigned scalar_balance(text_font* font, unsigned node) {
  scalar_height_update(font, node);
  scalar_slot* slot = &font->slots[node - 1];
  unsigned left = scalar_height(font, slot->left);
  unsigned right = scalar_height(font, slot->right);
  if (left > right + 1) {
    scalar_slot* child = &font->slots[slot->left - 1];
    if (scalar_height(font, child->right) > scalar_height(font, child->left))
      slot->left = scalar_rotate(font, slot->left, true);
    return scalar_rotate(font, node, false);
  }
  if (right > left + 1) {
    scalar_slot* child = &font->slots[slot->right - 1];
    if (scalar_height(font, child->left) > scalar_height(font, child->right))
      slot->right = scalar_rotate(font, slot->right, false);
    return scalar_rotate(font, node, true);
  }
  return node;
}
static snr_status scalar_glyph(text_font* font, const char* bytes,
                               size_t length, unsigned long* glyph) {
  if (!length || length > sizeof font->slots[0].bytes)
    return SNR_ERR_INVALID_ARGUMENT;
  if ((length == 1 && bytes[0] == ' ') ||
      (length == 2 && bytes[0] == ' ' && bytes[1] == ' ')) {
    *glyph = 65534;
    return SNR_OK;
  }
  /* An AVL tree with at most 65,533 nodes has height at most 22. */
  unsigned path[23];
  bool directions[23];
  size_t depth = 0;
  unsigned node = font->root;
  while (node) {
    snr_status status = snr_text_charge(font->work);
    if (status) return status;
    scalar_slot* slot = &font->slots[node - 1];
    int comparison = length < slot->length ? -1
                     : length > slot->length
                         ? 1
                         : memcmp(bytes, slot->bytes, length);
    if (!comparison) {
      *glyph = slot->glyph;
      return SNR_OK;
    }
    path[depth] = node;
    directions[depth++] = comparison > 0;
    node = comparison > 0 ? slot->right : slot->left;
  }
  if (font->used == 65533) return SNR_ERR_LIMIT;
  if (font->used == font->capacity) {
    size_t capacity = font->capacity ? font->capacity * 2 : 256;
    if (capacity > 65533) capacity = 65533;
    scalar_slot* slots = realloc(font->slots, capacity * sizeof *slots);
    if (!slots) return SNR_ERR_OUT_OF_MEMORY;
    font->slots = slots;
    font->capacity = capacity;
  }
  node = (unsigned)++font->used;
  scalar_slot* slot = &font->slots[node - 1];
  *slot = (scalar_slot){.length = length, .glyph = node, .height = 1};
  memcpy(slot->bytes, bytes, length);
  *glyph = node;
  while (depth) {
    depth--;
    scalar_slot* parent = &font->slots[path[depth] - 1];
    if (directions[depth])
      parent->right = node;
    else
      parent->left = node;
    node = scalar_balance(font, path[depth]);
  }
  font->root = node;
  return SNR_OK;
}
static void font_free(void* data) {
  text_font* font = data;
  if (font->face) FT_Done_Face(font->face);
  if (font->library) FT_Done_FreeType(font->library);
  free(font->bytes);
  free(font->slots);
  free(font);
}
