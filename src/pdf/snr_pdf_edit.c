/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* mmap's MAP_ANON under a strict C standard on glibc. */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include "snr_pdf_edit.h"

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <zlib.h>

#include "spe_sha256.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* Bounded arena; parsed scalar tokens retain their exact source spelling.
 * References are the only scalar rewritten when importing an overlay. */
typedef struct mem {
  struct mem* next;
} mem;

typedef struct {
  spe_limits limits;
  snr_status status;
  const char* reason;
  size_t allocated, work;
  bool interrupted; /* Cancellation or exhausted work must remain fatal. */
  mem* memory;
  snr_cancel_fn cancel;
  void* context;
} job;

/* Records the first failure and its reason; later failures are ignored. */
static void stop(job* j, snr_status s, const char* why) {
  if (j->status == SNR_OK) {
    j->status = s;
    j->reason = why;
  }
}

/* Charges one unit of work and polls cancellation. Returns false once the
 * job has failed. */
static bool tick(job* j) {
  if (j->status != SNR_OK) return false;

  if (j->cancel && j->cancel(j->context))
    stop(j, SNR_ERR_CANCELLED, "PDF update cancelled");
  if (j->limits.remaining) {
    if (!*j->limits.remaining)
      stop(j, SNR_ERR_LIMIT, "document work limit");
    else
      --*j->limits.remaining;
  }
  if (j->work >= j->limits.maximum_elements)
    stop(j, SNR_ERR_LIMIT, "PDF work limit");
  else
    j->work++;
  j->interrupted = j->status != SNR_OK;
  return j->status == SNR_OK;
}

/* Zeroed arena allocation charged to the scratch budget; freed by
 * release(). Returns null, failing the job, when over budget or out of
 * memory. */
static void* take(job* j, size_t n) {
  if (j->status != SNR_OK) return nullptr;
  if (n > SIZE_MAX - sizeof(mem) ||
      n + sizeof(mem) > j->limits.maximum_plan_bytes - j->allocated) {
    stop(j, SNR_ERR_LIMIT, "PDF scratch limit");
    return nullptr;
  }

  mem* m = calloc(1, sizeof *m + n);
  if (!m) {
    stop(j, SNR_ERR_OUT_OF_MEMORY, "PDF allocation failed");
    return nullptr;
  }
  m->next = j->memory;
  j->memory = m;
  j->allocated += sizeof *m + n;
  return m + 1;
}

typedef enum { SCALAR, NAME, ARRAY, DICT, REF } kind;

typedef struct node {
  kind type;
  const uint8_t* raw;
  size_t length;
  struct node *child, *next;
  uint32_t number, generation;
} node;

typedef struct {
  job* j;
  const uint8_t* data;
  size_t size, pos;
} scan;

/* PDF whitespace, which includes NUL. */
static bool space(uint8_t c) {
  return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

/* True for whitespace and the PDF delimiter characters. */
static bool delim(uint8_t c) {
  return space(c) || strchr("()<>[]{}/%", c) != nullptr;
}

/* Skips whitespace and % comments. */
static void ws(scan* s) {
  while (s->pos < s->size) {
    uint8_t c = s->data[s->pos];
    if (space(c))
      s->pos++;
    else if (c == '%') {
      while (s->pos < s->size && s->data[s->pos] != '\n' &&
             s->data[s->pos] != '\r')
        s->pos++;
    } else
      break;
  }
}

/* Consumes w if it is the next complete token. */
static bool word(scan* s, const char* w) {
  ws(s);
  size_t n = strlen(w);
  if (n > s->size - s->pos || memcmp(s->data + s->pos, w, n) ||
      (s->pos + n < s->size && !delim(s->data[s->pos + n])))
    return false;
  s->pos += n;
  return true;
}

/* Reads an unsigned decimal token without sign. On overflow or a
 * non-delimited token, only leading whitespace is consumed. */
static bool integer(scan* s, uint64_t* v) {
  ws(s);
  size_t p = s->pos;
  uint64_t n = 0;

  while (p < s->size && s->data[p] >= '0' && s->data[p] <= '9') {
    unsigned d = (unsigned)(s->data[p++] - '0');
    if (n > (UINT64_MAX - d) / 10) return false;
    n = n * 10 + d;
  }

  if (p == s->pos || (p < s->size && !delim(s->data[p]))) return false;
  s->pos = p;
  *v = n;
  return true;
}

/* A small offset error is recoverable only when a nearby complete header
 * identifies the expected object. The window cannot turn into a rebuild. */
static bool header_near(scan* s, uint64_t number, uint64_t generation,
                        bool xref) {
  size_t original = s->pos;
  for (size_t distance = 0; distance <= 32; distance++) {
    for (unsigned side = 0; side < (distance ? 2u : 1u); side++) {
      if (!tick(s->j)) return false;
      if ((!side && distance > original) ||
          (side && distance >= s->size - original))
        continue;
      size_t at = side ? original + distance : original - distance;
      if (at >= s->size || (at && !delim(s->data[at - 1]))) continue;
      scan probe = *s;
      probe.pos = at;
      if (xref && word(&probe, "xref")) {
        s->pos = at;
        return true;
      }
      probe.pos = at;
      uint64_t id, gen;
      if (integer(&probe, &id) && integer(&probe, &gen) &&
          word(&probe, "obj") &&
          (xref ? id && id <= UINT32_MAX && gen <= 65535
                : id == number && gen == generation)) {
        s->pos = xref ? at : probe.pos;
        return true;
      }
    }
  }
  return false;
}

/* Value of a hexadecimal digit, or -1. */
static int hex(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/* True if n is a name token equal to name after decoding #xx escapes. */
static bool named(const node* n, const char* name) {
  if (!n || n->type != NAME) return false;
  size_t k = 0;

  for (size_t i = 1; i < n->length; i++) {
    uint8_t c = n->raw[i];
    if (c == '#') {
      if (i + 2 >= n->length) return false;
      int a = hex(n->raw[++i]), b = hex(n->raw[++i]);
      if (a < 0 || b < 0) return false;
      c = (uint8_t)(a * 16 + b);
    }
    if (!name[k] || c != (uint8_t)name[k++]) return false;
  }

  return name[k] == 0;
}

/* Orders two validated name tokens by their bytes after decoding #xx
 * escapes; 0 when they name the same thing. */
static int name_order(const node* a, const node* b) {
  size_t i = 1, k = 1;

  while (i < a->length && k < b->length) {
    unsigned x = a->raw[i++], y = b->raw[k++];
    if (x == '#') {
      x = (unsigned)(hex(a->raw[i]) * 16 + hex(a->raw[i + 1]));
      i += 2;
    }
    if (y == '#') {
      y = (unsigned)(hex(b->raw[k]) * 16 + hex(b->raw[k + 1]));
      k += 2;
    }
    if (x != y) return x < y ? -1 : 1;
  }

  return (i < a->length) - (k < b->length);
}

/* Compares two validated name tokens after decoding #xx escapes. */
static bool same_name(const node* a, const node* b) {
  return !name_order(a, b);
}

/* Unresolved value of key in dictionary d; null if absent or d is not a
 * dictionary. */
static node* get(node* d, const char* key) {
  if (!d || d->type != DICT) return nullptr;
  for (node* n = d->child; n && n->next; n = n->next->next)
    if (named(n, key)) return n->next;
  return nullptr;
}

/* Charges n units of work at once, as n ticks would. */
static bool charge(job* j, size_t n) {
  if (!tick(j)) return false;
  if (n > j->limits.maximum_elements - j->work) {
    stop(j, SNR_ERR_LIMIT, "PDF work limit");
    j->interrupted = true;
    return false;
  }
  if (j->limits.remaining) {
    if (n > *j->limits.remaining) {
      stop(j, SNR_ERR_LIMIT, "document work limit");
      j->interrupted = true;
      return false;
    }
    *j->limits.remaining -= n;
  }
  j->work += n;
  return true;
}

/* qsort order of dictionary keys by decoded name. */
static int key_order(const void* a, const void* b) {
  return name_order(*(const node* const*)a, *(const node* const*)b);
}

/* False when dictionary d, of count keys, names a key twice (the job still
 * fine) or the check fails the job. A small dictionary is checked pair by
 * pair; a larger one sorted, so that its work grows as n log n rather than
 * n², and a page's resources naming each of its thousands of images once
 * stay well inside the work budget. */
static bool unique_keys(job* j, node* d, size_t count) {
  if (count <= 16) {
    for (node* k = d->child; k; k = k->next->next)
      for (node* other = k->next->next; other; other = other->next->next) {
        if (!tick(j)) return false;
        if (same_name(k, other)) return false;
      }
    return true;
  }

  size_t bytes = count * sizeof(node*), depth = 0;
  for (size_t n = count; n; n >>= 1) depth++;
  if (count > SIZE_MAX / sizeof(node*) / 2 ||
      bytes > j->limits.maximum_plan_bytes - j->allocated) {
    stop(j, SNR_ERR_LIMIT, "PDF scratch limit");
    return false;
  }
  if (!charge(j, count * depth)) return false;
  node** keys = malloc(bytes);
  if (!keys) {
    stop(j, SNR_ERR_OUT_OF_MEMORY, "PDF allocation failed");
    return false;
  }
  size_t n = 0;
  for (node* k = d->child; k; k = k->next->next) keys[n++] = k;
  qsort(keys, n, sizeof *keys, key_order);
  bool unique = true;
  for (size_t i = 1; i < n && unique; i++)
    unique = name_order(keys[i - 1], keys[i]) != 0;
  free(keys);
  return unique;
}

/* Parses one direct object into arena nodes. Strings, numbers and keywords
 * stay raw scalars; "n g R" becomes a reference. Rejects duplicate
 * dictionary keys, malformed tokens and nesting of 64 levels. */
static node* parse(scan* s, unsigned depth) {
  if (!tick(s->j)) return nullptr;
  if (depth >= 64) {
    stop(s->j, SNR_ERR_LIMIT, "PDF object nesting limit");
    return nullptr;
  }
  ws(s);
  if (s->pos >= s->size) goto bad;

  node* n = take(s->j, sizeof *n);
  if (!n) return nullptr;
  size_t start = s->pos;
  uint8_t c = s->data[s->pos++];
  n->raw = s->data + start;

  if (c == '[' || (c == '<' && s->pos < s->size && s->data[s->pos] == '<')) {
    n->type = c == '[' ? ARRAY : DICT;
    if (n->type == DICT) s->pos++;
    node** tail = &n->child;
    size_t count = 0;

    while (s->j->status == SNR_OK) {
      ws(s);
      if (s->pos >= s->size) goto bad;
      if (n->type == ARRAY && s->data[s->pos] == ']') {
        s->pos++;
        break;
      }
      if (n->type == DICT && s->size - s->pos >= 2 && s->data[s->pos] == '>' &&
          s->data[s->pos + 1] == '>') {
        if (count % 2) goto bad;
        s->pos += 2;
        if (!unique_keys(s->j, n, count / 2)) {
          if (s->j->status != SNR_OK) return nullptr;
          goto bad;
        }
        break;
      }

      node* v = parse(s, depth + 1);
      if (!v) return nullptr;

      if (n->type == DICT && count % 2 == 0 && v->type != NAME) goto bad;

      *tail = v;
      tail = &v->next;
      count++;
    }
  } else if (c == '(') {
    unsigned nesting = 1;

    while (s->pos < s->size && nesting) {
      c = s->data[s->pos++];
      if (c == '\\') {
        if (s->pos == s->size) goto bad;
        s->pos++;
      } else if (c == '(') {
        if (nesting >= 64) goto bad;
        nesting++;
      } else if (c == ')')
        nesting--;
    }

    if (nesting) goto bad;
  } else if (c == '<') {
    while (s->pos < s->size && s->data[s->pos] != '>') {
      c = s->data[s->pos++];
      if (!space(c) && hex(c) < 0) goto bad;
    }
    if (s->pos == s->size) goto bad;
    s->pos++;
  } else {
    if (c == '/')
      n->type = NAME;
    else if (delim(c))
      goto bad;

    while (s->pos < s->size && !delim(s->data[s->pos])) s->pos++;

    if (n->type == NAME) {
      for (size_t i = start + 1; i < s->pos; i++)
        if (s->data[i] == '#') {
          if (s->pos - i < 3 || hex(s->data[i + 1]) < 0 ||
              hex(s->data[i + 2]) < 0)
            goto bad;
          i += 2;
        }
    } else {
      size_t end = s->pos;
      scan t = *s;
      t.pos = start;
      uint64_t a, b;
      if (integer(&t, &a) && integer(&t, &b) && word(&t, "R")) {
        if (a > UINT32_MAX || b > 65535) goto bad;
        n->type = REF;
        n->number = (uint32_t)a;
        n->generation = (uint32_t)b;
        s->pos = t.pos;
      } else {
        bool valid = (end - start == 4 && !memcmp(n->raw, "null", 4)) ||
                     (end - start == 4 && !memcmp(n->raw, "true", 4)) ||
                     (end - start == 5 && !memcmp(n->raw, "false", 5));
        size_t i = start;
        bool digits = false, dot = false;
        if (s->data[i] == '+' || s->data[i] == '-') i++;

        for (; i < end; i++) {
          uint8_t b0 = s->data[i];
          if (b0 >= '0' && b0 <= '9')
            digits = true;
          else if (b0 == '.' && !dot)
            dot = true;
          else {
            digits = false;
            break;
          }
        }
        if (!valid && !digits) goto bad;
      }
    }
  }

  n->length = s->pos - start;
  return n;

bad:
  stop(s->j, SNR_ERR_INVALID_ARGUMENT, "malformed PDF object");
  return nullptr;
}

/* PDF integers may carry an explicit plus sign. */
static bool uint_value(node* n, uint64_t* out) {
  if (!n || n->type != SCALAR) return false;
  scan s = {.data = n->raw,
            .size = n->length,
            .pos = n->length > 1 && n->raw[0] == '+'};
  return integer(&s, out) && s.pos == s.size;
}

/* zlib state is heap memory charged to the scratch budget and released by
 * inflateEnd; each block records its accounted size in front of the payload. */
typedef struct {
  size_t size;
  max_align_t align;
} zblock;

/* zlib allocator charged to the job's scratch budget. */
static voidpf pdf_zalloc(voidpf context, uInt count, uInt size) {
  job* j = context;
  if (j->status != SNR_OK) return nullptr;
  if (size && count > (SIZE_MAX - sizeof(zblock)) / size) {
    stop(j, SNR_ERR_LIMIT, "zlib allocation overflow");
    return nullptr;
  }
  size_t n = (size_t)count * size + sizeof(zblock);
  if (n > j->limits.maximum_plan_bytes - j->allocated) {
    stop(j, SNR_ERR_LIMIT, "PDF scratch limit");
    return nullptr;
  }
  zblock* b = calloc(1, n);
  if (!b) {
    stop(j, SNR_ERR_OUT_OF_MEMORY, "zlib allocation failed");
    return nullptr;
  }
  b->size = n;
  j->allocated += n;
  return b + 1;
}

/* zlib deallocator; returns the block's size to the scratch budget. */
static void pdf_zfree(voidpf context, voidpf address) {
  job* j = context;
  if (!address) return;
  zblock* b = (zblock*)address - 1;
  j->allocated -= b->size;
  free(b);
}

/* Grows an arena block that is not yet linked into the arena list. *held is
 * the size already charged to the scratch budget. */
static bool grow_block(job* j, mem** block, size_t* held, size_t bytes) {
  size_t total = sizeof(mem) + bytes;
  if (total > *held &&
      total - *held > j->limits.maximum_plan_bytes - j->allocated) {
    stop(j, SNR_ERR_LIMIT, "PDF scratch limit");
    return false;
  }
  mem* m = realloc(*block, total);
  if (!m) {
    stop(j, SNR_ERR_OUT_OF_MEMORY, "PDF allocation failed");
    return false;
  }
  j->allocated = j->allocated - *held + total;
  *block = m;
  *held = total;
  return true;
}

enum { DECODED_STREAM_LIMIT = 64 << 20 };

/* Decode only the filters needed by xref/object streams. Flate output is
 * inflated once into a single growing buffer, bounded per stream and by the
 * caller's per-file decoded-byte budget, then adopted by the request arena. */
static bool decode_stream(job* j, size_t* budget, node* dict,
                          const uint8_t* raw, size_t length,
                          const uint8_t** out, size_t* size) {
  node* filter = get(dict, "Filter");
  *out = raw;
  *size = length;
  if (!filter) return true;
  if (filter->type == ARRAY && filter->child && !filter->child->next)
    filter = filter->child;
  if (!named(filter, "FlateDecode")) {
    stop(j, SNR_ERR_UNSUPPORTED, "PDF stream filter");
    return false;
  }
  if (length > UINT_MAX) {
    stop(j, SNR_ERR_LIMIT, "encoded PDF stream limit");
    return false;
  }

  z_stream codec = {.zalloc = pdf_zalloc,
                    .zfree = pdf_zfree,
                    .opaque = j,
                    .next_in = (Bytef*)(uintptr_t)raw,
                    .avail_in = (uInt)length};
  if (inflateInit(&codec) != Z_OK) {
    stop(j, SNR_ERR_OUT_OF_MEMORY, "zlib initialisation failed");
    return false;
  }

  mem* block = nullptr;
  size_t held = 0, capacity = 0;
  int status = Z_OK;

  while (tick(j)) {
    if (!codec.avail_out) {
      size_t next = capacity ? capacity * 2 : length < 1024 ? 1024 : length;
      if (next > DECODED_STREAM_LIMIT) next = DECODED_STREAM_LIMIT;
      if (next <= capacity) {
        stop(j, SNR_ERR_LIMIT, "decoded PDF stream limit");
        break;
      }
      if (next > *budget) {
        stop(j, SNR_ERR_LIMIT, "decoded PDF data budget");
        break;
      }
      if (!grow_block(j, &block, &held, next)) break;
      codec.next_out = (Bytef*)(block + 1) + capacity;
      codec.avail_out = (uInt)(next - capacity);
      capacity = next;
    }

    size_t before = (size_t)codec.total_out;
    status = inflate(&codec, Z_NO_FLUSH);
    if (!charge(j, (size_t)codec.total_out - before)) break;
    if (status != Z_OK && !(status == Z_BUF_ERROR && !codec.avail_out)) break;
  }

  *budget -= (size_t)codec.total_out;
  inflateEnd(&codec);
  bool incomplete_xref = named(get(dict, "Type"), "XRef") && !codec.avail_in &&
                         status == Z_BUF_ERROR;
  if (j->status == SNR_OK && status != Z_STREAM_END && !incomplete_xref)
    stop(j,
         status == Z_MEM_ERROR ? SNR_ERR_OUT_OF_MEMORY
                               : SNR_ERR_INVALID_ARGUMENT,
         "invalid Flate stream");
  if (j->status != SNR_OK) {
    free(block);
    j->allocated -= held;
    return false;
  }

  block->next = j->memory;
  j->memory = block;
  *size = (size_t)codec.total_out;
  uint8_t* decoded = (uint8_t*)(block + 1);
  *out = decoded;

  node* params = get(dict, "DecodeParms");
  if (params && params->type == ARRAY && params->child && !params->child->next)
    params = params->child;
  if (!params || (params->type == SCALAR && params->length == 4 &&
                  !memcmp(params->raw, "null", 4)))
    return true;
  if (params->type != DICT) goto bad;

  uint64_t predictor = 1, columns = 1, colors = 1, bits = 8;
  if ((get(params, "Predictor") &&
       !uint_value(get(params, "Predictor"), &predictor)) ||
      (get(params, "Columns") &&
       !uint_value(get(params, "Columns"), &columns)) ||
      (get(params, "Colors") && !uint_value(get(params, "Colors"), &colors)) ||
      (get(params, "BitsPerComponent") &&
       !uint_value(get(params, "BitsPerComponent"), &bits)))
    goto bad;
  if (predictor == 1) return true;
  if (bits != 8 || !colors || !columns || columns > SIZE_MAX / colors) goto bad;
  size_t width = (size_t)(columns * colors);
  if (!width || width >= 64u * 1024u * 1024u) goto bad;

  if (predictor == 2) {
    if (*size % width) goto bad;
    for (size_t row = 0; row < *size; row += width) {
      if (!tick(j)) return false;
      for (size_t x = (size_t)colors; x < width; x++)
        decoded[row + x] =
            (uint8_t)(decoded[row + x] + decoded[row + x - colors]);
    }
    return true;
  }

  if (predictor < 10 || predictor > 15 || *size % (width + 1)) goto bad;
  size_t rows = *size / (width + 1);
  for (size_t row = 0; row < rows; row++) {
    if (!tick(j)) return false;
    unsigned row_filter = decoded[row * (width + 1)];
    if (row_filter > 4) goto bad;
    for (size_t x = 0; x < width; x++) {
      unsigned raw_value = decoded[row * (width + 1) + 1 + x];
      int left = x >= colors ? decoded[row * width + x - colors] : 0;
      int up = row ? decoded[(row - 1) * width + x] : 0;
      int corner =
          row && x >= colors ? decoded[(row - 1) * width + x - colors] : 0;
      int prediction = 0;
      if (row_filter == 1) prediction = left;
      if (row_filter == 2) prediction = up;
      if (row_filter == 3) prediction = (left + up) / 2;
      if (row_filter == 4) {
        int p = left + up - corner;
        int a = abs(p - left), b = abs(p - up), c = abs(p - corner);
        prediction = a <= b && a <= c ? left : b <= c ? up : corner;
      }
      decoded[row * width + x] = (uint8_t)(raw_value + (unsigned)prediction);
    }
  }

  *size = rows * width;
  return true;

bad:
  stop(j, SNR_ERR_INVALID_ARGUMENT, "invalid PDF predictor");
  return false;
}

/* Decoded object stream with its parsed (object number, offset) header. */
typedef struct {
  const uint8_t* data;
  size_t size, first, count;
  uint64_t* slots;
} packed;

/* Sorted cross-reference index: newest free entries shadow older live objects.
 * revision counts sections from the newest; table marks classic-table rows. */
typedef struct entry {
  uint32_t number, generation, mapped, container, index;
  size_t revision;
  size_t offset;
  bool live, reading, visited, table, bad;
  node* object;
  const uint8_t* stream;
  size_t stream_size;
  packed* packed;
} entry;

typedef struct page {
  entry* object;
  node *resources, *media, *crop, *rotate;
  struct page* next;
} page;

/* Each cross-reference stream is decoded once and reused by both index passes.
 */
typedef struct {
  size_t offset, size;
  node* dict;
  const uint8_t* data;
} xref_stream;

enum { MAXIMUM_REVISIONS = 256, MAXIMUM_XREF_STREAMS = 2 * MAXIMUM_REVISIONS };
static const size_t decoded_file_budget = (size_t)256 << 20;

typedef struct {
  job* j;
  const uint8_t* bytes;
  size_t size, last, next_number;
  bool xref_streams;
  entry* entries;
  size_t count;
  node *trailer, *root;
  page *pages, *last_page;
  size_t page_count;
  size_t decoded_budget; /* Remaining decoded bytes for this file. */
  xref_stream* streams;
  size_t stream_count;
} file;

/* Index entry for object id, or null when the index has none. */
static entry* lookup(file* f, uint32_t id) {
  size_t lo = 0, hi = f->count;
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    if (f->entries[m].number < id)
      lo = m + 1;
    else
      hi = m;
  }

  return lo < f->count && f->entries[lo].number == id ? &f->entries[lo]
                                                      : nullptr;
}

static node* resolve(file* f, node* n, unsigned depth);
static node* object(file* f, entry* e, unsigned depth);

/* An in-use xref row that names no object: offset 0 outside an object
 * stream. Such rows are skipped when every object is visited. */
static bool absent(const entry* e) {
  return e->live && !e->offset && !e->container;
}

/* Decodes an object stream and its header once per file. */
static packed* unpack(file* f, entry* container, unsigned depth) {
  if (!container || container->bad) goto bad;
  /* Cache failure pessimistically, including allocation/header failures. */
  node* d = object(f, container, depth);
  container->bad = true;
  uint64_t count, first;
  if (!d || !container || container->container || container->generation ||
      !named(get(d, "Type"), "ObjStm") || !uint_value(get(d, "N"), &count) ||
      !uint_value(get(d, "First"), &first) || !count ||
      count > f->j->limits.maximum_elements)
    goto bad;

  packed* p = take(f->j, sizeof *p);
  if (!p || !decode_stream(f->j, &f->decoded_budget, d, container->stream,
                           container->stream_size, &p->data, &p->size))
    return nullptr;

  /* Every header pair needs at least four bytes ("1 0 "), bounding the table.
   */
  if (first > p->size || count > (first + 1) / 4) goto bad;
  p->first = (size_t)first;
  p->count = (size_t)count;

  p->slots = take(f->j, p->count * 2 * sizeof *p->slots);
  if (!p->slots) return nullptr;

  scan header = {f->j, p->data, p->first, 0};
  for (size_t i = 0; i < p->count; i++) {
    if (!tick(f->j)) return nullptr;
    uint64_t id, offset;
    if (!integer(&header, &id) || !integer(&header, &offset) || !id ||
        id > UINT32_MAX || offset >= p->size - p->first ||
        (i && offset <= p->slots[2 * i - 1]))
      goto bad;
    p->slots[2 * i] = id;
    p->slots[2 * i + 1] = offset;
  }

  container->bad = false;
  container->packed = p;
  return p;

bad:
  stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid PDF object stream");
  return nullptr;
}

/* Parses an indirect object once and caches it on the entry, reading it
 * from the file body or its object stream. For a stream object, also
 * records the raw stream bytes. Fails on free, missing or cyclic
 * objects. */
static node* object(file* f, entry* e, unsigned depth) {
  if (!e || !e->live) {
    stop(f->j, SNR_ERR_INVALID_ARGUMENT, "missing PDF object");
    return nullptr;
  }
  if (absent(e)) {
    stop(f->j, SNR_ERR_INVALID_ARGUMENT, "reference to an absent PDF object");
    return nullptr;
  }
  if (e->bad) {
    stop(f->j, SNR_ERR_INVALID_ARGUMENT, "previously failed PDF object");
    return nullptr;
  }
  if (e->object) return e->object;
  if (e->reading || depth >= 64) {
    stop(f->j, SNR_ERR_INVALID_ARGUMENT, "cyclic PDF object");
    return nullptr;
  }

  e->reading = true;
  if (e->container) {
    entry* container = lookup(f, e->container);
    packed* p = container ? container->packed : nullptr;
    if (!p && !(p = unpack(f, container, depth + 1))) goto bad;
    if (e->index >= p->count || p->slots[2 * (size_t)e->index] != e->number)
      goto bad;
    uint64_t selected = p->slots[2 * (size_t)e->index + 1];
    uint64_t end = (size_t)e->index + 1 < p->count
                       ? p->slots[2 * (size_t)e->index + 3]
                       : p->size - p->first;
    scan body = {f->j, p->data + p->first + selected, (size_t)(end - selected),
                 0};

    node* n = parse(&body, 0);
    ws(&body);
    if (!n || body.pos != body.size) goto bad;

    e->object = n;
    e->reading = false;
    return n;
  }

  scan s = {f->j, f->bytes, f->size, e->offset};
  if (!header_near(&s, e->number, e->generation, false)) goto bad;

  node* n = parse(&s, 0);
  if (!n) goto bad;

  if (n->type == DICT && word(&s, "stream")) {
    if (s.pos < f->size && s.data[s.pos] == '\r')
      s.pos++;
    else if (s.pos >= f->size || s.data[s.pos] != '\n')
      goto bad;
    if (s.pos < f->size && s.data[s.pos] == '\n') s.pos++;
    uint64_t length;
    node* len = resolve(f, get(n, "Length"), depth + 1);
    if (!uint_value(len, &length) || length > f->size - s.pos) goto bad;
    e->stream = f->bytes + s.pos;
    e->stream_size = (size_t)length;
    s.pos += (size_t)length;
    if (!word(&s, "endstream")) goto bad;
  }

  if (!word(&s, "endobj")) goto bad;

  e->object = n;
  e->reading = false;
  return n;

bad:
  e->reading = false;
  e->bad = true;
  stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid PDF object or stream length");
  return nullptr;
}

/* Follows references until a direct object; other nodes pass through. */
static node* resolve(file* f, node* n, unsigned depth) {
  if (!n || n->type != REF) return n;
  if (depth >= 64) {
    stop(f->j, SNR_ERR_LIMIT, "PDF reference depth");
    return nullptr;
  }
  entry* e = lookup(f, n->number);
  if (!e || e->generation != n->generation) {
    stop(f->j, SNR_ERR_INVALID_ARGUMENT, "PDF reference generation mismatch");
    return nullptr;
  }

  node* v = object(f, e, depth);
  return v ? resolve(f, v, depth + 1) : nullptr;
}

/* PDF decimal numbers, without locale-dependent conversion or exponent syntax.
 */
static bool decimal(node* n, double* out) {
  if (!n || n->type != SCALAR || !n->length) return false;
  size_t i = 0;
  bool negative = false, dot = false, digit = false;
  if (n->raw[i] == '+' || n->raw[i] == '-') negative = n->raw[i++] == '-';
  double value = 0, fraction = .1;

  for (; i < n->length; i++) {
    unsigned c = n->raw[i];
    if (c == '.' && !dot) {
      dot = true;
      continue;
    }
    if (c < '0' || c > '9') return false;
    digit = true;
    if (dot) {
      value += (double)(c - '0') * fraction;
      fraction *= .1;
    } else
      value = value * 10 + (double)(c - '0');
  }

  *out = negative ? -value : value;
  return digit && isfinite(value);
}

/* True if both values resolve to arrays of four equal numbers. */
static bool equal_boxes(file* f, node* a, node* b) {
  a = resolve(f, a, 0);
  b = resolve(f, b, 0);
  if (!a || !b || a->type != ARRAY || b->type != ARRAY) return false;
  a = a->child;
  b = b->child;

  for (size_t i = 0; i < 4; i++) {
    double x, y;
    if (!a || !b || !decimal(resolve(f, a, 0), &x) ||
        !decimal(resolve(f, b, 0), &y) || x != y)
      return false;
    a = a->next;
    b = b->next;
  }

  return !a && !b;
}

/* qsort order: object number, newest revision, table rows before stream
 * rows, then original row order. */
static int entry_order(const void* a, const void* b) {
  const entry *x = a, *y = b;
  if (x->number != y->number) return x->number < y->number ? -1 : 1;
  if (x->revision != y->revision) return x->revision < y->revision ? -1 : 1;
  if (x->table != y->table) return x->table ? -1 : 1;
  return (x->mapped > y->mapped) - (x->mapped < y->mapped);
}

/* Parses and decodes the cross-reference stream object at offset; cached
 * per file so both index passes share it. */
static xref_stream* load_xref_stream(file* f, size_t offset) {
  for (size_t i = 0; i < f->stream_count; i++)
    if (f->streams[i].offset == offset) return &f->streams[i];
  if (!f->streams &&
      !(f->streams = take(f->j, MAXIMUM_XREF_STREAMS * sizeof *f->streams)))
    return nullptr;

  scan s = {f->j, f->bytes, f->size, offset};
  uint64_t number, gen, length, size;
  if (f->stream_count == MAXIMUM_XREF_STREAMS || !integer(&s, &number) ||
      !number || number > UINT32_MAX || !integer(&s, &gen) || gen > 65535 ||
      !word(&s, "obj"))
    goto bad;

  node* d = parse(&s, 0);
  if (!d || d->type != DICT || !named(get(d, "Type"), "XRef") ||
      !uint_value(get(d, "Length"), &length) ||
      !uint_value(get(d, "Size"), &size) || !size || size > UINT32_MAX ||
      !word(&s, "stream"))
    goto bad;
  if (s.pos >= s.size || (s.data[s.pos] != '\r' && s.data[s.pos] != '\n'))
    goto bad;
  if (s.data[s.pos] == '\r') s.pos++;
  if (s.pos < s.size && s.data[s.pos] == '\n') s.pos++;
  if (length > s.size - s.pos) goto bad;

  xref_stream x = {.offset = offset, .dict = d};
  if (!decode_stream(f->j, &f->decoded_budget, d, s.data + s.pos,
                     (size_t)length, &x.data, &x.size))
    return nullptr;

  s.pos += (size_t)length;
  if (!word(&s, "endstream") || !word(&s, "endobj")) goto bad;
  f->streams[f->stream_count] = x;
  return &f->streams[f->stream_count++];

bad:
  stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid PDF cross-reference stream");
  return nullptr;
}

/* Counts the rows of the xref stream at offset, also storing them when
 * fill is set. Returns the stream dictionary, which acts as the trailer. */
static node* stream_section(file* f, size_t offset, bool fill, size_t* used,
                            size_t revision) {
  xref_stream* x = load_xref_stream(f, offset);
  if (!x) return nullptr;
  node* d = x->dict;
  const uint8_t* data = x->data;
  size_t count = x->size;

  uint64_t size;
  if (!uint_value(get(d, "Size"), &size)) goto bad;

  node* w = get(d, "W");
  if (!w || w->type != ARRAY) goto bad;
  uint64_t widths[3];
  node* v = w->child;
  for (unsigned i = 0; i < 3; i++) {
    if (!v || !uint_value(v, &widths[i]) || widths[i] > 8) goto bad;
    v = v->next;
  }
  size_t stride = (size_t)(widths[0] + widths[1] + widths[2]);
  if (v || !stride) goto bad;

  node* index = get(d, "Index");
  if (index && (index->type != ARRAY || !index->child)) goto bad;
  node* range = index ? index->child : nullptr;
  size_t cursor = 0;

  do {
    uint64_t first = 0, n = size;
    if (range) {
      if (!uint_value(range, &first) || !range->next ||
          !uint_value(range->next, &n))
        goto bad;
      range = range->next->next;
    }
    if (first > size || n > size - first || n > (count - cursor) / stride)
      goto bad;

    for (uint64_t i = 0; i < n; i++) {
      if (!tick(f->j)) return nullptr;
      uint64_t fields[3] = {widths[0] ? 0 : 1, 0, 0};
      for (unsigned k = 0; k < 3; k++)
        for (uint64_t j = 0; j < widths[k]; j++)
          fields[k] = (fields[k] << 8) | data[cursor++];
      entry e = {.number = (uint32_t)(first + i), .revision = revision};
      if (fields[0] && !e.number) goto bad;
      if (fields[0] == 0) {
        if (fields[2] > 65535) goto bad;
      } else if (fields[0] == 1) {
        /* Offset 0 with generation 0 is an absent object, as in a table. */
        if ((!fields[1] && fields[2]) || fields[1] >= f->size ||
            fields[2] > 65535)
          goto bad;
        e.live = true;
        e.offset = (size_t)fields[1];
        e.generation = (uint32_t)fields[2];
      } else if (fields[0] == 2) {
        if (!fields[1] || fields[1] >= size || fields[2] > UINT32_MAX) goto bad;
        e.live = true;
        e.container = (uint32_t)fields[1];
        e.index = (uint32_t)fields[2];
      } else
        goto bad;
      if (*used == SIZE_MAX) goto bad;
      if (fill) {
        if (*used >= f->count) goto bad;
        f->entries[*used] = e;
      }
      (*used)++;
    }
  } while (range);

  if (cursor != count) goto bad;
  return d;

bad:
  stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid PDF cross-reference stream");
  return nullptr;
}

/* Build xrefs in two passes. No allocation proportional to a hostile /Size. */
static bool sections(file* f, bool fill) {
  size_t offset = f->last, used = 0, iterations = 0;
  size_t visited[MAXIMUM_REVISIONS];

  while (f->j->status == SNR_OK) {
    if (++iterations > MAXIMUM_REVISIONS) {
      stop(f->j, SNR_ERR_LIMIT, "PDF revision limit");
      break;
    }
    for (size_t i = 0; i + 1 < iterations; i++)
      if (visited[i] == offset) goto bad;
    visited[iterations - 1] = offset;

    scan s = {f->j, f->bytes, f->size, offset};
    if (!header_near(&s, 0, 0, true)) goto bad;
    offset = s.pos;
    if (iterations == 1) f->last = offset;
    node* d = nullptr;
    bool classic = word(&s, "xref");
    if (!classic) {
      f->xref_streams = true;
      d = stream_section(f, offset, fill, &used, iterations);
      if (!d) return false;
    }

    while (classic && !word(&s, "trailer")) {
      if (!tick(f->j)) return false;
      uint64_t first, count;
      if (!integer(&s, &first) || !integer(&s, &count) || first > UINT32_MAX ||
          count > UINT32_MAX - first)
        goto bad;
      if (count > f->j->limits.maximum_elements) {
        stop(f->j, SNR_ERR_LIMIT, "PDF xref entry limit");
        return false;
      }

      for (uint64_t i = 0; i < count; i++) {
        uint64_t at, gen;
        if (!tick(f->j)) return false;
        if (!integer(&s, &at) || !integer(&s, &gen) || gen > 65535) goto bad;
        bool live = word(&s, "n");
        if (!live && !word(&s, "f")) goto bad;
        /* Quartz writes an in-use row at offset 0 for an object number it
         * never emits. The row is kept as an absent object: harmless while
         * unreferenced, an error once anything resolves it. */
        if (live && ((at == 0 && gen != 0) || at >= f->size || first + i == 0))
          goto bad;
        if (used == SIZE_MAX) goto bad;
        if (fill) {
          if (used >= f->count) goto bad;
          f->entries[used] = (entry){.number = (uint32_t)(first + i),
                                     .generation = (uint32_t)gen,
                                     .offset = (size_t)at,
                                     .live = live,
                                     .table = true,
                                     .revision = iterations};
        }
        used++;
      }
    }

    if (classic) d = parse(&s, 0);
    if (!d || d->type != DICT) goto bad;

    if (get(d, "Encrypt")) {
      stop(f->j, SNR_ERR_UNSUPPORTED, "encrypted PDF");
      return false;
    }

    if (classic && get(d, "XRefStm")) {
      f->xref_streams = true;
      uint64_t hybrid;
      if (!uint_value(get(d, "XRefStm"), &hybrid) || !hybrid ||
          hybrid >= f->size || hybrid == offset)
        goto bad;
      /* Companion rows share this revision; open_file prefers the table. */
      node* extra = stream_section(f, (size_t)hybrid, fill, &used, iterations);
      if (!extra) return false;
      if (get(extra, "Encrypt")) {
        stop(f->j, SNR_ERR_UNSUPPORTED, "encrypted PDF");
        return false;
      }
    }

    if (fill) {
      if (!f->trailer)
        f->trailer = d;
      else { /* Carry missing trailer values from earlier revisions. */
        for (node* k = d->child; k; k = k->next->next)
          if ((named(k, "Root") && !get(f->trailer, "Root")) ||
              (named(k, "Info") && !get(f->trailer, "Info")) ||
              (named(k, "ID") && !get(f->trailer, "ID"))) {
            node *key = take(f->j, sizeof *key),
                 *value = take(f->j, sizeof *value);
            if (!key || !value) return false;
            *key = *k;
            *value = *k->next;
            value->next = f->trailer->child;
            key->next = value;
            f->trailer->child = key;
          }
      }
    }

    uint64_t prev;
    node* p = get(d, "Prev");
    if (!p) break;
    if (!uint_value(p, &prev) || prev == 0 || prev >= f->size) goto bad;
    offset = (size_t)prev;
  }

  if (!fill) f->count = used;
  return f->j->status == SNR_OK;

bad:
  stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid PDF cross-reference table");
  return false;
}

/* Walks the page tree depth-first, appending each page with its inherited
 * Resources, MediaBox, CropBox and Rotate. Refuses cycles, bad nodes and
 * a /UserUnit other than 1. */
static bool tree(file* f, node* ref, node* resources, node* media, node* crop,
                 node* rotate, unsigned depth) {
  if (!tick(f->j)) return false;
  if (depth >= 64 || !ref || ref->type != REF) goto bad;
  entry* e = lookup(f, ref->number);
  if (!e || e->visited || e->generation != ref->generation) goto bad;
  e->visited = true;
  node* d = object(f, e, 0);
  if (!d || d->type != DICT) goto bad;

  node* v;
  if ((v = get(d, "Resources"))) resources = v;
  if ((v = get(d, "MediaBox"))) media = v;
  if ((v = get(d, "CropBox"))) crop = v;
  if ((v = get(d, "Rotate"))) rotate = v;

  if (get(d, "UserUnit")) {
    double unit;
    if (!decimal(resolve(f, get(d, "UserUnit"), 0), &unit) || unit != 1) {
      stop(f->j, SNR_ERR_UNSUPPORTED, "non-unit PDF UserUnit");
      return false;
    }
  }

  if (named(get(d, "Type"), "Pages")) {
    node* kids = resolve(f, get(d, "Kids"), 0);
    if (!kids || kids->type != ARRAY) goto bad;
    for (node* k = kids->child; k; k = k->next)
      if (!tree(f, k, resources, media, crop, rotate, depth + 1)) return false;
  } else if (named(get(d, "Type"), "Page")) {
    if (f->page_count >= f->j->limits.maximum_draws) {
      stop(f->j, SNR_ERR_LIMIT, "PDF page limit");
      return false;
    }
    page* p = take(f->j, sizeof *p);
    if (!p) return false;
    *p = (page){e, resources, media, crop, rotate, nullptr};
    if (f->last_page)
      f->last_page->next = p;
    else
      f->pages = p;
    f->last_page = p;
    f->page_count++;
  } else
    goto bad;

  return true;

bad:
  stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid or cyclic PDF page tree");
  return false;
}

/* Indexes every xref revision of a PDF (newest row per object wins),
 * resolves the catalog and collects the pages. Refuses encryption and
 * catalog /Perms. Allocations live in the job's arena. */
static bool open_file(file* f, job* j, const uint8_t* bytes, size_t size) {
  *f = (file){.j = j,
              .bytes = bytes,
              .size = size,
              .decoded_budget = decoded_file_budget};

  if (size > j->limits.maximum_pdf_bytes) {
    stop(j, SNR_ERR_LIMIT, "PDF input size limit");
    return false;
  }

  if (!bytes || size < 9 || memcmp(bytes, "%PDF-", 5)) goto bad;

  size_t at = size - 9;
  bool found = false;
  for (;;) {
    if (!memcmp(bytes + at, "startxref", 9)) {
      found = true;
      break;
    }
    if (at == 0 || size - at > 65536) break;
    at--;
  }
  if (!found) goto bad;

  scan s = {j, bytes, size, at + 9};
  uint64_t offset;
  if (!integer(&s, &offset) || offset == 0 || offset >= size) goto bad;
  f->last = (size_t)offset;

  if (!sections(f, false)) return false;
  if (f->count > SIZE_MAX / sizeof(entry) || f->count > UINT32_MAX) goto bad;

  f->entries = take(j, f->count * sizeof(entry));
  if (!f->entries) return false;

  if (!sections(f, true)) return false;

  for (size_t i = 0; i < f->count; i++) f->entries[i].mapped = (uint32_t)i;

  /* Rows sort by object, then newest revision, then classic table before
   * stream rows; a row repeated within one section is adjacent. */
  qsort(f->entries, f->count, sizeof(entry), entry_order);
  size_t unique = 0;
  for (size_t i = 0; i < f->count;) {
    size_t end = i + 1;
    while (end < f->count && f->entries[end].number == f->entries[i].number) {
      if (!tick(j)) return false;
      if (f->entries[end].revision == f->entries[end - 1].revision &&
          f->entries[end].table == f->entries[end - 1].table)
        goto bad;
      end++;
    }
    /* Hybrid files (ISO 32000-1 7.5.8.4): the table wins; its companion
     * stream supplies an object only where the table marks it free. */
    size_t best = i;
    if (f->entries[i].table && !f->entries[i].live && i + 1 < end &&
        f->entries[i + 1].revision == f->entries[i].revision)
      best = i + 1;
    f->entries[unique] = f->entries[best];
    f->entries[unique++].mapped = 0;
    i = end;
  }

  f->count = unique;
  uint64_t declared;
  if (!uint_value(get(f->trailer, "Size"), &declared) ||
      declared > UINT32_MAX || !declared)
    goto bad;
  f->next_number = (size_t)declared;
  if (unique && f->entries[unique - 1].number >= f->next_number)
    f->next_number = (size_t)f->entries[unique - 1].number + 1;

  node* root = get(f->trailer, "Root");
  f->root = root && root->type == REF ? resolve(f, root, 0) : nullptr;
  if (!f->root || f->root->type != DICT) goto bad;

  if (get(f->root, "Perms")) {
    stop(j, SNR_ERR_UNSUPPORTED,
         "certified/rights-managed PDF requires signature permission handling");
    return false;
  }

  return tree(f, get(f->root, "Pages"), nullptr, nullptr, nullptr, nullptr, 0);

bad:
  stop(j, SNR_ERR_INVALID_ARGUMENT, "invalid PDF file");
  return false;
}

typedef struct update {
  uint32_t id, gen;
  size_t offset;
  struct update* next;
} update;

/* Output already moved out of a chunked writer's buffer: mapped pages of
 * their own rather than heap blocks, so that releasing a chunk while the
 * output is assembled returns its memory at once. */
typedef struct chunk {
  struct chunk* next;
  size_t size, mapped;
  uint8_t bytes[];
} chunk;

/* A chunk able to hold size bytes; null when no memory is left. */
static chunk* chunk_new(size_t size) {
  size_t mapped = sizeof(chunk) + size;
#ifdef _WIN32
  chunk* c =
      VirtualAlloc(nullptr, mapped, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void* p = mmap(nullptr, mapped, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
  chunk* c = p == MAP_FAILED ? nullptr : p;
#endif
  if (c) *c = (chunk){.size = size, .mapped = mapped};
  return c;
}

static void chunk_free(chunk* c) {
#ifdef _WIN32
  VirtualFree(c, 0, MEM_RELEASE);
#else
  munmap(c, c->mapped);
#endif
}

typedef spe_sha256 digester;
typedef spe_sha256_digest digest;

/* Adds n bytes to the digest. */
static void absorb(digester* h, const void* bytes, size_t n) {
  spe_sha256_update(h, bytes, n);
}

/* Adds v as eight bytes. */
static void absorb_size(digester* h, uint64_t v) {
  uint8_t b[8];
  for (unsigned k = 0; k < 8; k++) b[k] = (uint8_t)(v >> (8 * k));
  absorb(h, b, sizeof b);
}

/* The first eight bytes of d, where a table of digests starts looking. */
static size_t digest_slot(const digest* d) {
  uint64_t v = 0;
  for (unsigned k = 0; k < 8; k++) v |= (uint64_t)d->bytes[k] << (8 * k);
  return (size_t)v;
}

/* Objects an update has imported, by digest: an object imported again,
 * identical down to the objects it refers to, is referred to rather than
 * written again. Only SHA-256 digests and object numbers are kept, in an
 * open addressing table charged to j's scratch budget: 18 MiB at most, and no
 * more than a quarter of the budget, so that the update's own records keep
 * the rest. Past MAXIMUM_SHARED entries, or once the table may not grow,
 * objects are no longer recorded, those recorded still shared. */
typedef struct {
  digest d;
  uint32_t number;
} shared_object;

typedef struct ledger {
  job* j;
  shared_object* slots;
  size_t capacity, count;
} ledger;

enum { MAXIMUM_SHARED = 1 << 18 };

/* The number of the object recorded under d; 0 when there is none. */
static uint32_t ledger_find(const ledger* l, digest d) {
  if (!l->capacity) return 0;
  for (size_t at = digest_slot(&d) & (l->capacity - 1); l->slots[at].number;
       at = (at + 1) & (l->capacity - 1))
    if (!memcmp(l->slots[at].d.bytes, d.bytes, sizeof d.bytes))
      return l->slots[at].number;
  return 0;
}

/* Records object number under d, unless the ledger is full. */
static void ledger_add(ledger* l, digest d, uint32_t number) {
  job* j = l->j;
  if (l->count >= MAXIMUM_SHARED || ledger_find(l, d)) return;
  if (l->count + 1 > l->capacity / 2) {
    size_t capacity = l->capacity ? l->capacity * 2 : 1024,
           bytes = capacity * sizeof *l->slots,
           held = l->capacity * sizeof *l->slots;
    if (bytes > j->limits.maximum_plan_bytes / 4 ||
        bytes - held > j->limits.maximum_plan_bytes - j->allocated)
      return;
    shared_object* grown = calloc(capacity, sizeof *grown);
    if (!grown) {
      stop(j, SNR_ERR_OUT_OF_MEMORY, "PDF allocation failed");
      return;
    }
    for (size_t i = 0; i < l->capacity; i++)
      if (l->slots[i].number) {
        size_t at = digest_slot(&l->slots[i].d) & (capacity - 1);
        while (grown[at].number) at = (at + 1) & (capacity - 1);
        grown[at] = l->slots[i];
      }
    free(l->slots);
    j->allocated = j->allocated - held + bytes;
    l->slots = grown;
    l->capacity = capacity;
  }
  size_t at = digest_slot(&d) & (l->capacity - 1);
  while (l->slots[at].number) at = (at + 1) & (l->capacity - 1);
  l->slots[at] = (shared_object){d, number};
  l->count++;
}

/* renumber, when set, is the file whose objects are written under new numbers
 * (its entries' mapped field), trailer values included. records, when set,
 * is the job holding the xref records instead of j. A chunked writer moves
 * each full megabytes of output aside instead of growing one buffer, so that
 * a long file never needs room for twice its size. A writer with an output
 * hands its bytes on to it instead, a chunk's worth at a time, keeping none
 * once they are written. shared, when set, is the ledger of imported objects
 * that imports refer to instead of writing them again. */
typedef struct {
  job* j;
  snr_buffer data;
  size_t capacity, next_number;
  update* updates;
  size_t count;
  file* renumber;
  job* records;
  bool chunked;
  chunk *chunks, *last_chunk;
  size_t flushed;
  const snr_output* output;
  ledger* shared;
} writer;

enum { CHUNK_BYTES = 4 << 20 };

/* Offset in the output of the next byte written. */
static size_t position(const writer* w) { return w->flushed + w->data.size; }

/* Hands n bytes, which follow those buffered, to the writer's output. */
static void emit(writer* w, const void* bytes, size_t n) {
  if (w->j->status != SNR_OK || !n) return;
  snr_status s = w->output->write(w->output->context, bytes, n);
  if (s != SNR_OK)
    stop(w->j, s, "PDF output write failed");
  else
    w->flushed += n;
}

/* Hands what is buffered to the writer's output. */
static void drain(writer* w) {
  size_t n = w->data.size;
  w->data.size = 0;
  emit(w, w->data.bytes, n);
}

/* Appends bytes to the output, growing it up to maximum_pdf_bytes. */
static void put(writer* w, const void* bytes, size_t n) {
  if (w->j->status != SNR_OK || !n) return;
  if (n > w->j->limits.maximum_pdf_bytes - position(w)) {
    stop(w->j, SNR_ERR_LIMIT, "output byte limit exceeded");
    return;
  }

  /* Output goes on once a chunk's worth is buffered; as much again or more
   * goes on at once, unbuffered. */
  if (w->output && w->data.size + n > CHUNK_BYTES) {
    drain(w);
    if (n >= CHUNK_BYTES) {
      emit(w, bytes, n);
      return;
    }
    if (w->j->status != SNR_OK) return;
  }

  if (w->chunked && !w->output && w->data.size >= CHUNK_BYTES &&
      n > w->capacity - w->data.size) {
    chunk* c = chunk_new(w->data.size);
    if (!c) {
      stop(w->j, SNR_ERR_OUT_OF_MEMORY, "output allocation failed");
      return;
    }
    memcpy(c->bytes, w->data.bytes, w->data.size);
    if (w->last_chunk)
      w->last_chunk->next = c;
    else
      w->chunks = c;
    w->last_chunk = c;
    w->flushed += w->data.size;
    w->data.size = 0;
  }

  size_t needed = w->data.size + n;
  if (needed > w->capacity) {
    size_t cap = w->capacity ? w->capacity : 4096;
    while (cap < needed) {
      if (cap > w->j->limits.maximum_pdf_bytes / 2) {
        cap = w->j->limits.maximum_pdf_bytes;
        break;
      }
      cap *= 2;
    }
    if (cap > w->j->limits.maximum_pdf_bytes)
      cap = w->j->limits.maximum_pdf_bytes;
    void* p = realloc(w->data.bytes, cap);
    if (!p) {
      stop(w->j, SNR_ERR_OUT_OF_MEMORY, "output allocation failed");
      return;
    }
    w->data.bytes = p;
    w->capacity = cap;
  }

  memcpy(w->data.bytes + w->data.size, bytes, n);
  w->data.size += n;
}

/* Appends a NUL-terminated string. */
static void text(writer* w, const char* s) { put(w, s, strlen(s)); }

/* Appends printf-style text; output longer than 511 bytes fails the
 * job. */
static void format(writer* w, const char* fmt, ...) {
  char b[512];
  va_list a;
  va_start(a, fmt);
  int n = vsnprintf(b, sizeof b, fmt, a);
  va_end(a);

  if (n < 0 || (size_t)n >= sizeof b) {
    stop(w->j, SNR_ERR_LIMIT, "PDF number formatting");
    return;
  }
  text(w, b);
}

/* C numeric locale may use a comma. PDF decimal tokens always use a dot. */
static void real(writer* w, double v) {
  if (!isfinite(v) || fabs(v) > w->j->limits.maximum_coordinate) {
    stop(w->j, SNR_ERR_LIMIT, "PDF coordinate limit");
    return;
  }

  char scientific[64], digits[24], fixed[400];
  int n = snprintf(scientific, sizeof scientific, "%.17e", fabs(v));
  if (n < 0 || (size_t)n >= sizeof scientific) {
    stop(w->j, SNR_ERR_LIMIT, "PDF number formatting");
    return;
  }

  char* exponent = strchr(scientific, 'e');
  if (!exponent) {
    stop(w->j, SNR_ERR_INVALID_ARGUMENT, "PDF numeric locale");
    return;
  }

  size_t nd = 0;
  for (char* p = scientific; p < exponent; p++)
    if (*p >= '0' && *p <= '9') {
      if (nd == sizeof digits) {
        stop(w->j, SNR_ERR_LIMIT, "PDF decimal digits");
        return;
      }
      digits[nd++] = *p;
    }

  int point = 1 + (int)strtol(exponent + 1, nullptr, 10);
  size_t used = 0;
  if (v < 0) fixed[used++] = '-';

  if (point <= 0) {
    fixed[used++] = '0';
    fixed[used++] = '.';
    for (int i = 0; i < -point; i++) fixed[used++] = '0';
    memcpy(fixed + used, digits, nd);
    used += nd;
  } else {
    for (int i = 0; i < point; i++)
      fixed[used++] = (size_t)i < nd ? digits[i] : '0';
    if ((size_t)point < nd) {
      fixed[used++] = '.';
      memcpy(fixed + used, digits + point, nd - (size_t)point);
      used += nd - (size_t)point;
    }
  }

  fixed[used] = 0;
  if (strchr(fixed, '.')) {
    while (used && fixed[used - 1] == '0') fixed[--used] = 0;
    if (used && fixed[used - 1] == '.') fixed[--used] = 0;
  }

  text(w, fixed);
}

/* Next unused object number; 0 after failing the job on exhaustion. */
static uint32_t reserve(writer* w) {
  if (w->next_number >= UINT32_MAX) {
    stop(w->j, SNR_ERR_LIMIT, "PDF object number limit");
    return 0;
  }
  return (uint32_t)w->next_number++;
}

/* Opens indirect object id and records its offset for the xref. */
static void begin(writer* w, uint32_t id, uint32_t gen) {
  if (!tick(w->j)) return;
  if (!id) {
    stop(w->j, SNR_ERR_INVALID_ARGUMENT, "PDF object number 0 is reserved");
    return;
  }
  update* u = take(w->records ? w->records : w->j, sizeof *u);
  if (!u) {
    if (w->records) stop(w->j, w->records->status, w->records->reason);
    return;
  }

  *u = (update){id, gen, position(w), w->updates};
  w->updates = u;
  w->count++;
  format(w, "%u %u obj\n", id, gen);
}

/* Closes the current indirect object. */
static void end(writer* w) { text(w, "\nendobj\n"); }

/* Serializes a node. With import set, references are renumbered to the
 * object numbers assigned to that file's imported objects. */
static void value(writer* w, file* import, node* n, unsigned depth) {
  if (!n) {
    text(w, "null");
    return;
  }
  if (!tick(w->j)) return;
  if (depth >= 64) {
    stop(w->j, SNR_ERR_LIMIT, "PDF write nesting");
    return;
  }

  if (n->type == REF && import) {
    entry* e = lookup(import, n->number);
    if (!e || !e->live || !e->mapped || e->generation != n->generation) {
      stop(w->j, SNR_ERR_INVALID_ARGUMENT, "missing imported reference");
      return;
    }
    format(w, "%u 0 R", e->mapped);
  } else if (n->type == ARRAY || n->type == DICT) {
    text(w, n->type == ARRAY ? "[" : "<<");
    for (node* c = n->child; c; c = c->next) {
      value(w, import, c, depth + 1);
      text(w, " ");
    }
    text(w, n->type == ARRAY ? "]" : ">>");
  } else
    put(w, n->raw, n->length);
}

/* Writes the entries of dictionary d except the count keys in skip. */
static void fields(writer* w, file* import, node* d, const char* const* skip,
                   size_t count) {
  if (!d) return;
  if (d->type != DICT) {
    stop(w->j, SNR_ERR_INVALID_ARGUMENT, "expected PDF dictionary");
    return;
  }

  for (node* k = d->child; k; k = k->next->next) {
    bool omit = false;
    for (size_t i = 0; i < count; i++)
      if (named(k, skip[i])) omit = true;
    if (!omit) {
      value(w, import, k, 0);
      text(w, " ");
      value(w, import, k->next, 0);
      text(w, "\n");
    }
  }
}

/* Writes a new unfiltered stream object holding s; returns its number. */
static uint32_t stream_text(writer* w, const char* s) {
  uint32_t id = reserve(w);
  begin(w, id, 0);
  format(w, "<< /Length %zu >>\nstream\n", strlen(s));
  text(w, s);
  text(w, "\nendstream");
  end(w);
  return id;
}

/* Entry of the indirect content stream that contents refers to, either
 * directly or as a one-element array; null after failing the job
 * otherwise. */
static entry* single_stream(file* f, node* contents) {
  if (!contents) return nullptr;

  node* v = resolve(f, contents, 0);
  if (v && v->type == ARRAY) {
    if (!v->child || v->child->next) {
      stop(f->j, SNR_ERR_UNSUPPORTED, "overlay PDF needs one content stream");
      return nullptr;
    }
    contents = v->child;
  }

  if (contents->type != REF) {
    stop(f->j, SNR_ERR_INVALID_ARGUMENT, "content stream must be indirect");
    return nullptr;
  }

  entry* e = lookup(f, contents->number);
  if (!e || e->generation != contents->generation || !object(f, e, 0) ||
      !e->stream) {
    stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid content stream");
    return nullptr;
  }

  return e;
}

static bool import_reachable(writer* w, file* f, node* const* roots,
                             size_t count);
static size_t dict_values(node* d, const char* except, node** out, size_t room);

/* Imports a one-page overlay file and wraps its page as a Form XObject: the
 * objects its resources, box, group and content stream's other entries refer
 * to come along, not the file's catalog, page tree, page or content stream
 * object, which the form replaces. Returns the form's object number, or 0
 * on failure. */
static uint32_t form(writer* w, file* f) {
  page* p = f->pages;
  uint64_t rotation = 0;
  if (p->rotate &&
      (!uint_value(resolve(f, p->rotate, 0), &rotation) || rotation % 360)) {
    stop(w->j, SNR_ERR_UNSUPPORTED, "rotated overlay PDF");
    return 0;
  }
  entry* e = single_stream(f, get(p->object->object, "Contents"));
  if (!e) {
    stop(w->j, SNR_ERR_INVALID_ARGUMENT, "overlay PDF has no content stream");
    return 0;
  }
  node* box = resolve(f, p->media, 0);
  if (!box || box->type != ARRAY) {
    stop(w->j, SNR_ERR_INVALID_ARGUMENT, "overlay has no media box");
    return 0;
  }

  node* group = get(p->object->object, "Group");
  node* roots[64];
  size_t n = 0;
  roots[n++] = p->resources;
  roots[n++] = box;
  if (group) roots[n++] = group;
  n += dict_values(e->object, "Length", roots + n, 64 - n);
  if (!import_reachable(w, f, roots, n)) return 0;

  uint32_t id = reserve(w);
  begin(w, id, 0);
  text(w, "<< /Type /XObject /Subtype /Form /FormType 1\n/BBox ");
  value(w, f, box, 0);
  text(w, "\n/Resources ");
  value(w, f, p->resources, 0);
  const char* skip[] = {"Type",      "Subtype", "FormType", "BBox",
                        "Resources", "Matrix",  "Group",    "Length"};
  text(w, "\n");
  fields(w, f, e->object, skip, sizeof skip / sizeof *skip);

  if (group) {
    text(w, "/Group ");
    value(w, f, group, 0);
    text(w, "\n");
  }

  format(w, "/Length %zu >>\nstream\n", e->stream_size);
  put(w, e->stream, e->stream_size);
  text(w, "\nendstream");
  end(w);
  return id;
}

/* Writes a page's existing content-stream references, each checked to be
 * an indirect stream. */
static void contents(writer* w, file* f, node* n) {
  if (!n) return;
  node* v = resolve(f, n, 0);
  if (!v || (v->type == SCALAR && v->length == 4 && !memcmp(v->raw, "null", 4)))
    return;

  if (v->type == ARRAY) {
    for (node* c = v->child; c; c = c->next) {
      if (!single_stream(f, c)) return;
      value(w, nullptr, c, 0);
      text(w, " ");
    }
  } else {
    if (!single_stream(f, n)) return;
    value(w, nullptr, n, 0);
    text(w, " ");
  }
}

/* Writes a rectangle as a PDF [llx lly urx ury] array. */
static void rectangle(writer* w, spe_rect b) {
  text(w, "[");
  real(w, b.x);
  text(w, " ");
  real(w, b.y);
  text(w, " ");
  real(w, b.x + b.width);
  text(w, " ");
  real(w, b.y + b.height);
  text(w, "]");
}

enum { MAXIMUM_PARTS = 8 };

/* Rewrites source page p so that it draws the parts, one-page overlay files
 * each imported with its objects as a Form XObject, in order under the
 * first part's transform, after its own contents between the streams open
 * ("q") and close ("Q"). With the first part's replace_box, also replaces
 * MediaBox/CropBox and keeps the old MediaBox as ArtBox. */
static void overlay(writer* w, file* source, page* p,
                    const spe_pdf_overlay* parts, size_t count, uint32_t open,
                    uint32_t close) {
  const spe_pdf_overlay* o = &parts[0];
  uint32_t forms[MAXIMUM_PARTS];
  for (size_t i = 0; i < count && w->j->status == SNR_OK; i++) {
    file imported;
    if (!open_file(&imported, w->j, parts[i].pdf, parts[i].size)) return;
    if (imported.page_count != 1) {
      stop(w->j, SNR_ERR_INVALID_ARGUMENT, "overlay PDF must have one page");
      return;
    }
    forms[i] = form(w, &imported);
  }
  if (w->j->status != SNR_OK) return;

  node *resources = resolve(source, p->resources, 0),
       *objects = resolve(source, get(resources, "XObject"), 0);
  if ((resources && resources->type != DICT) ||
      (objects && objects->type != DICT)) {
    stop(w->j, SNR_ERR_INVALID_ARGUMENT, "invalid page resources");
    return;
  }

  char names[MAXIMUM_PARTS][32];
  for (size_t i = 0; i < count; i++) {
    unsigned suffix = 0;
    do {
      if (!tick(w->j)) return;
      snprintf(names[i], sizeof names[i], "RMR%u_%u", forms[i], suffix++);
    } while (get(objects, names[i]));
  }

  /* The overlay lives in its own Form resource scope, so its gs/image/font
   * names can never replace names used by the original page. */
  uint32_t invoke = reserve(w);
  writer content = {.j = w->j};
  text(&content, "/Artifact BMC\nq\n");
  double m[] = {o->transform.a, o->transform.b,  o->transform.c,
                o->transform.d, o->transform.tx, o->transform.ty};
  for (size_t i = 0; i < 6; i++) {
    real(&content, m[i]);
    text(&content, " ");
  }
  text(&content, "cm\n");
  for (size_t i = 0; i < count; i++) format(&content, "/%s Do\n", names[i]);
  text(&content, "Q\nEMC\n");
  begin(w, invoke, 0);
  format(w, "<< /Length %zu >>\nstream\n", content.data.size);
  put(w, content.data.bytes, content.data.size);
  text(w, "\nendstream");
  end(w);
  free(content.data.bytes);

  begin(w, p->object->number, p->object->generation);
  text(w, "<<\n");
  const char* skip[] = {"Contents", "Resources", "MediaBox", "CropBox",
                        "ArtBox"};
  fields(w, nullptr, p->object->object, skip, o->replace_box ? 5 : 2);

  if (o->replace_box) {
    text(w, "/MediaBox ");
    rectangle(w, o->box);
    text(w, "\n/CropBox ");
    if (p->crop && !equal_boxes(source, p->crop, p->media))
      value(w, nullptr, p->crop, 0);
    else
      rectangle(w, o->box);
    if (p->media) {
      text(w, "\n/ArtBox ");
      value(w, nullptr, p->media, 0);
    }
    text(w, "\n");
  }

  format(w, "/Contents [%u 0 R ", open);
  contents(w, source, get(p->object->object, "Contents"));
  format(w, "%u 0 R %u 0 R]\n/Resources <<\n", close, invoke);
  const char* xskip[] = {"XObject"};
  fields(w, nullptr, resources, xskip, 1);
  text(w, "/XObject <<\n");
  fields(w, nullptr, objects, nullptr, 0);
  for (size_t i = 0; i < count; i++)
    format(w, "/%s %u 0 R\n", names[i], forms[i]);
  text(w, ">>\n>>\n>>");
  end(w);
}

/* A new trailer or xref stream dictionary carries only document identity
 * from the source trailer; stream keys such as /F or /DL never transfer. */
static void trailer_fields(writer* w, node* trailer) {
  static const char* const keep[] = {"Root", "Info", "ID"};
  for (node* k = trailer->child; k; k = k->next->next)
    for (size_t i = 0; i < sizeof keep / sizeof *keep; i++)
      if (named(k, keep[i])) {
        value(w, nullptr, k, 0);
        text(w, " ");
        value(w, w->renumber, k->next, 0);
        text(w, "\n");
      }
}

/* qsort order of written objects by object number. */
static int update_order(const void* a, const void* b) {
  const update *x = a, *y = b;
  return (x->id > y->id) - (x->id < y->id);
}

/* Writes the cross-reference section and trailer for the objects written
 * so far: a complete table for a fresh file, otherwise an update chained
 * to the source's last xref, as a table, stream or both (hybrid). */
static void finish(writer* w, file* f) {
  size_t offset = position(w);
  bool hybrid = f->xref_streams && get(f->trailer, "XRefStm") != nullptr;
  size_t hybrid_offset = offset;
  if (f->xref_streams) begin(w, reserve(w), 0);

  if (w->count > SIZE_MAX / sizeof(update)) {
    stop(w->j, SNR_ERR_LIMIT, "xref allocation limit");
    return;
  }

  update* list = take(w->j, w->count * sizeof *list);
  if (!list) return;
  size_t i = 0;
  for (update* u = w->updates; u; u = u->next) list[i++] = *u;
  qsort(list, w->count, sizeof *list, update_order);

  if (!f->last) {
    /* A fresh file has no preceding xref to supply missing rows. Include
     * free entries for holes left by packed/xref objects or source gaps. */
    format(w, "xref\n0 %zu\n0000000000 65535 f \n", w->next_number);
    i = 0;

    for (size_t id = 1; id < w->next_number && tick(w->j); id++) {
      if (i < w->count && list[i].id == id) {
        if (list[i].offset > 9999999999ULL) {
          stop(w->j, SNR_ERR_LIMIT, "classic xref offset limit");
          return;
        }
        format(w, "%010zu %05u n \n", list[i].offset, list[i].gen);
        i++;
      } else
        text(w, "0000000000 65535 f \n");
    }

    format(w, "trailer\n<< /Size %zu\n", w->next_number);
    trailer_fields(w, f->trailer);
    format(w, ">>\nstartxref\n%zu\n%%%%EOF\n", offset);
    return;
  }

  if (f->xref_streams) {
    if (w->count > SIZE_MAX / 11) {
      stop(w->j, SNR_ERR_LIMIT, "xref stream length");
      return;
    }

    format(w, "<< /Type /XRef /Size %zu ", w->next_number);
    if (!hybrid) format(w, "/Prev %zu ", f->last);
    text(w, "/W [1 8 2] /Index [");
    for (i = 0; i < w->count; i++) format(w, "%u 1 ", list[i].id);
    format(w, "] /Length %zu\n", w->count * 11);
    trailer_fields(w, f->trailer);

    text(w, ">>\nstream\n");
    for (i = 0; i < w->count; i++) {
      uint8_t row[11] = {1};
      uint64_t at = list[i].offset;
      for (unsigned k = 0; k < 8; k++) row[8 - k] = (uint8_t)(at >> (k * 8));
      row[9] = (uint8_t)(list[i].gen >> 8);
      row[10] = (uint8_t)list[i].gen;
      put(w, row, sizeof row);
    }
    text(w, "\nendstream");
    end(w);

    if (!hybrid) {
      format(w, "startxref\n%zu\n%%%%EOF\n", offset);
      return;
    }
    offset = position(w);
  }

  text(w, "xref\n");
  for (i = 0; i < w->count; i++) {
    if (list[i].offset > 9999999999ULL) {
      stop(w->j, SNR_ERR_LIMIT, "classic xref offset limit");
      return;
    }
    format(w, "%u 1\n%010zu %05u n \n", list[i].id, list[i].offset,
           list[i].gen);
  }

  format(w, "trailer\n<< /Size %zu /Prev %zu\n", w->next_number, f->last);
  if (hybrid) format(w, "/XRefStm %zu\n", hybrid_offset);
  trailer_fields(w, f->trailer);
  format(w, ">>\nstartxref\n%zu\n%%%%EOF\n", offset);
}

/* Frees every arena block owned by the job. */
static void release(job* j) {
  while (j->memory) {
    mem* next = j->memory->next;
    free(j->memory);
    j->memory = next;
  }
}

/* A page's inherited MediaBox, normalised to positive size, and /Rotate
 * reduced to 0-270. False if either is malformed or not a multiple of
 * 90. */
static bool geometry(file* f, const page* p, spe_pdf_page_geometry* out) {
  *out = (spe_pdf_page_geometry){0};

  if (p->media) {
    node *box = resolve(f, p->media, 0),
         *c = box && box->type == ARRAY ? box->child : nullptr;
    double v[4];
    for (size_t k = 0; k < 4; k++, c = c->next)
      if (!c || !decimal(resolve(f, c, 0), &v[k])) return false;
    if (c) return false;
    out->has_media = true;
    out->media = (spe_rect){fmin(v[0], v[2]), fmin(v[1], v[3]),
                            fabs(v[2] - v[0]), fabs(v[3] - v[1])};
  }

  out->crop = out->media;
  if (p->crop) {
    node* box = resolve(f, p->crop, 0);
    node* c = box && box->type == ARRAY ? box->child : nullptr;
    double v[4];
    for (size_t k = 0; k < 4; k++, c = c->next)
      if (!c || !decimal(resolve(f, c, 0), &v[k])) return false;
    if (c) return false;
    double x = fmax(out->media.x, fmin(v[0], v[2]));
    double y = fmax(out->media.y, fmin(v[1], v[3]));
    out->crop = (spe_rect){
        x, y, fmin(out->media.x + out->media.width, fmax(v[0], v[2])) - x,
        fmin(out->media.y + out->media.height, fmax(v[1], v[3])) - y};
  }
  if (p->rotate) {
    double r;
    if (!decimal(resolve(f, p->rotate, 0), &r) || fabs(r) > 1e9 ||
        fmod(r, 90) != 0)
      return false;
    long quarter = (long)r % 360;
    out->rotate = (int32_t)(quarter < 0 ? quarter + 360 : quarter);
  }

  return true;
}

/* Signature diagnostics are best effort. Malformed optional objects must not
 * prevent rendering; cancellation and work exhaustion still end the job. */
static void signature_recover(job* j) {
  if (!j->interrupted) {
    j->status = SNR_OK;
    j->reason = nullptr;
  }
}
static node* signature_resolve(file* f, node* n, uint8_t* seen) {
  for (unsigned depth = 0; n && n->type == REF; depth++) {
    if (depth >= 64 || !tick(f->j)) return nullptr;
    entry* e = lookup(f, n->number);
    if (!e || !e->live || e->generation != n->generation || absent(e))
      return nullptr;
    if (seen) {
      if (seen[e - f->entries]) return nullptr;
      seen[e - f->entries] = 1;
    }
    n = object(f, e, 0);
    if (f->j->status != SNR_OK) {
      signature_recover(f->j);
      return nullptr;
    }
  }
  return n;
}
/* Track arrays as well as fields so shared Kids never multiply traversal. */
static void signature_fields(file* f, node* array, size_t depth, uint8_t* seen,
                             size_t* count) {
  if (!array || depth >= 64 || !tick(f->j)) return;
  array = signature_resolve(f, array, seen);
  if (!array || array->type != ARRAY) return;
  for (node* c = array->child; c && tick(f->j); c = c->next) {
    node* field = signature_resolve(f, c, seen);
    if (named(signature_resolve(f, get(field, "FT"), nullptr), "Sig"))
      (*count)++;
    signature_fields(f, get(field, "Kids"), depth + 1, seen, count);
  }
}

snr_status spe_pdf_pages(const uint8_t* source, size_t size,
                         const spe_limits* given, spe_pdf_page_geometry** pages,
                         size_t* count, size_t* signatures, snr_error* error) {
  if (!source || !size || !count || !pages)
    return snr_fail(error, SNR_ERR_INVALID_ARGUMENT, 0,
                    "missing PDF page input");

  job j = {.limits = given ? *given : spe_limits_default()};
  j.cancel = j.limits.cancel;
  j.context = j.limits.context;
  file f;

  spe_pdf_page_geometry* result = nullptr;
  if (tick(&j) && open_file(&f, &j, source, size)) {
    if (f.page_count > j.limits.maximum_plan_bytes / sizeof *result)
      stop(&j, SNR_ERR_LIMIT, "PDF geometry allocation limit");
    else if (f.page_count && !(result = calloc(f.page_count, sizeof *result)))
      stop(&j, SNR_ERR_OUT_OF_MEMORY, "PDF geometry allocation failed");
    size_t i = 0;
    for (page* p = f.pages; p && tick(&j); p = p->next, i++) {
      spe_pdf_page_geometry g;
      if (!geometry(&f, p, &g))
        stop(&j, SNR_ERR_INVALID_ARGUMENT, "invalid PDF page box or rotation");
      else
        result[i] = g;
    }
    if (j.status == SNR_OK) *count = f.page_count;
    if (signatures && j.status == SNR_OK) {
      *signatures = 0;
      uint8_t* seen = take(&j, f.count);
      if (seen) {
        node* form = signature_resolve(&f, get(f.root, "AcroForm"), nullptr);
        signature_fields(&f, get(form, "Fields"), 0, seen, signatures);
      } else {
        signature_recover(&j);
      }
    }
  }

  if (j.status == SNR_OK)
    *pages = result;
  else
    free(result);
  release(&j);
  return j.status == SNR_OK
             ? SNR_OK
             : snr_fail(error, j.status, 0,
                        j.reason ? j.reason : "PDF inspection failed");
}

static void header(writer* w, const uint8_t* source, size_t size) {
  size_t end = 5, major = 0, minor = 0;
  while (end < size && end < 16 && source[end] >= '0' && source[end] <= '9')
    end++, major++;
  if (end < size && source[end] == '.')
    for (end++;
         end < size && end < 16 && source[end] >= '0' && source[end] <= '9';
         end++)
      minor++;

  bool valid = major && minor && end < size &&
               (source[end] == '\r' || source[end] == '\n');
  if (valid)
    put(w, source, end);
  else
    text(w, "%PDF-1.7");
  text(w, "\n%\342\343\317\323\n");
}

typedef struct {
  node *original, *dict;
  entry* entry;
  uint32_t id;
} selected_annot;

typedef struct {
  page* original;
  node* dict;
  uint32_t id;
  selected_annot* annots;
  size_t count;
} selected_page;

/* New arena node of the given kind whose raw text copies text_value. */
static node* select_token(job* j, kind type, const char* text_value) {
  node* n = take(j, sizeof *n);
  size_t size = strlen(text_value);
  uint8_t* raw = take(j, size);
  if (!n || !raw) return nullptr;

  memcpy(raw, text_value, size);
  *n = (node){.type = type, .raw = raw, .length = size};
  return n;
}

/* New reference node for id and generation. */
static node* select_ref(job* j, uint32_t id, uint32_t generation) {
  char raw[48];
  snprintf(raw, sizeof raw, "%u %u R", id, generation);
  node* n = select_token(j, REF, raw);
  if (n) {
    n->number = id;
    n->generation = generation;
  }
  return n;
}

/* Deep copy of a node into the arena, with its sibling link cleared. */
static node* select_clone(job* j, const node* n, unsigned depth) {
  if (!n || !tick(j)) return nullptr;
  if (depth >= 64) {
    stop(j, SNR_ERR_LIMIT, "page copy nesting");
    return nullptr;
  }
  node* copy = take(j, sizeof *copy);
  if (!copy) return nullptr;

  *copy = *n;
  copy->child = copy->next = nullptr;
  node** tail = &copy->child;

  for (const node* c = n->child; c && j->status == SNR_OK; c = c->next) {
    *tail = select_clone(j, c, depth + 1);
    if (*tail) tail = &(*tail)->next;
  }

  return copy;
}

/* Replaces the value of key in dict, or prepends the key if absent. */
static void select_set(job* j, node* dict, const char* key, node* v) {
  if (!dict || !v || j->status != SNR_OK) return;

  for (node* k = dict->child; k; k = k->next->next)
    if (named(k, key)) {
      v->next = k->next->next;
      k->next = v;
      return;
    }

  char raw[64];
  snprintf(raw, sizeof raw, "/%s", key);
  node* k = select_token(j, NAME, raw);
  if (k) {
    v->next = dict->child;
    k->next = v;
    dict->child = k;
  }
}

/* Unlinks the entry for key from dict, if present. */
static void select_remove(node* dict, const char* key) {
  if (!dict) return;
  for (node** at = &dict->child; *at; at = &(*at)->next->next)
    if (named(*at, key)) {
      *at = (*at)->next->next;
      return;
    }
}

typedef struct {
  node value;
  node* tail;
} selection_array;

/* New empty array that select_append extends in constant time. */
static node* select_array(job* j) {
  selection_array* a = take(j, sizeof *a);
  if (a) a->value.type = ARRAY;
  return a ? &a->value : nullptr;
}

/* Only arrays created by select_array are appended to. */
static void select_append(node* array, node* item) {
  if (!array || !item) return;
  selection_array* a = (selection_array*)array;
  if (a->tail)
    a->tail->next = item;
  else
    array->child = item;
  a->tail = item;
}

/* True if a widget dictionary also carries field keys (merged field). */
static bool select_merged(node* dict) {
  return get(dict, "T") || get(dict, "FT") || get(dict, "V") || get(dict, "DV");
}

/* Page and annotation references are remapped without traversing indirect
 * objects. Self-links and popup/reply relations use the current page copy. */
static void select_refs(file* f, node* n, const selected_page* local,
                        unsigned depth) {
  if (!n || !tick(f->j)) return;
  if (depth >= 64) {
    stop(f->j, SNR_ERR_LIMIT, "page reference nesting");
    return;
  }

  if (n->type == REF) {
    entry* e = lookup(f, n->number);
    if (!e || !e->live || e->generation != n->generation) return;
    uint32_t id = e->mapped;
    if (local) {
      if (e == local->original->object) id = local->id;
      for (size_t i = 0; i < local->count && tick(f->j); i++)
        if (e == local->annots[i].entry) id = local->annots[i].id;
    }
    bool is_page = e->object && named(get(e->object, "Type"), "Page");

    if (id || is_page) {
      node* replacement =
          id ? select_ref(f->j, id, 0) : select_token(f->j, SCALAR, "null");
      if (replacement) {
        replacement->next = n->next;
        *n = *replacement;
      }
    }
  } else if (n->type == DICT) {
    bool widget = named(get(n, "Subtype"), "Widget");
    for (node* k = n->child; k && f->j->status == SNR_OK; k = k->next->next)
      if (!(widget && named(k, "Parent")))
        select_refs(f, k->next, local, depth + 1);
  } else
    for (node* c = n->child; c && f->j->status == SNR_OK; c = c->next)
      select_refs(f, c, local, depth + 1);
}

/* Rebuilds a form field array for the selected pages. Widgets become their
 * page copies and merged field/widgets keep the field with per-copy kids.
 * Fields left without widgets are dropped unless no source page held
 * them. Retained fields are marked 2 in seen. */
static node* select_fields(file* f, node* array, node** widgets, uint8_t* seen,
                           const uint8_t* attached, unsigned depth) {
  array = resolve(f, array, 0);
  if (!array || array->type != ARRAY || depth >= 64) {
    stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid form field tree");
    return nullptr;
  }
  node* result = select_array(f->j);

  for (node* ref = array->child; ref && tick(f->j); ref = ref->next) {
    entry* e = ref->type == REF ? lookup(f, ref->number) : nullptr;
    node* dict = resolve(f, ref, 0);
    if (!e || !dict || dict->type != DICT || seen[e - f->entries]) {
      stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid or repeated form field");
      break;
    }
    size_t index = (size_t)(e - f->entries);
    seen[index] = 1;

    if (named(get(dict, "Subtype"), "Widget")) {
      node* copies = widgets[index];
      if (!copies || !copies->child) {
        /* Fields with no widget in any source page can carry hidden
         * values used by calculations; page removal must not drop them. */
        if (!attached[index]) {
          seen[index] = 2;
          select_append(result, select_clone(f->j, ref, 0));
        }
        continue;
      }
      if (!select_merged(dict)) {
        for (node* c = copies->child; c && tick(f->j); c = c->next)
          select_append(result, select_clone(f->j, c, 0));
        seen[index] = 2;
        continue;
      }
      /* Split a merged field/widget: retain its field identity and
       * values, and give each selected page its own child widget. */
      const char* keys[] = {
          "Type",   "Subtype", "Rect", "P", "AP",           "AS", "F",    "BS",
          "Border", "MK",      "H",    "A", "StructParent", "OC", "Popup"};
      for (size_t k = 0; k < sizeof keys / sizeof *keys; k++)
        select_remove(dict, keys[k]);
      select_set(f->j, dict, "Kids", select_clone(f->j, copies, 0));
    } else if (get(dict, "Kids")) {
      node* kids = select_fields(f, get(dict, "Kids"), widgets, seen, attached,
                                 depth + 1);
      if (!kids || !kids->child) continue;
      select_set(f->j, dict, "Kids", kids);
    }

    seen[index] = 2;
    select_append(result, select_clone(f->j, ref, 0));
  }

  return result;
}

/* Page labels are keyed by page index. Each output page keeps the label its
 * source page showed: it starts a range copied from the source range, with
 * /St advanced to the source page's position in that range. */
typedef struct label_range {
  uint64_t key;
  node* dict;
  struct label_range* next;
} label_range;

/* Collects every (first page index, label dictionary) pair of a page
 * label number tree into a list. */
static void select_label_ranges(file* f, node* tree, label_range** out,
                                unsigned depth) {
  tree = resolve(f, tree, 0);
  if (!tree || !tick(f->j)) return;
  if (depth >= 64 || tree->type != DICT) {
    stop(f->j, depth >= 64 ? SNR_ERR_LIMIT : SNR_ERR_INVALID_ARGUMENT,
         "invalid page labels");
    return;
  }
  node *nums = resolve(f, get(tree, "Nums"), 0),
       *kids = resolve(f, get(tree, "Kids"), 0);
  if ((nums && nums->type != ARRAY) || (kids && kids->type != ARRAY)) {
    stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid page labels");
    return;
  }

  for (node* k = nums ? nums->child : nullptr; k && tick(f->j);
       k = k->next->next) {
    uint64_t key;
    node* d = k->next ? resolve(f, k->next, 0) : nullptr;
    label_range* r = d && d->type == DICT && uint_value(k, &key)
                         ? take(f->j, sizeof *r)
                         : nullptr;
    if (!r) {
      if (f->j->status == SNR_OK)
        stop(f->j, SNR_ERR_INVALID_ARGUMENT, "invalid page labels");
      return;
    }
    *r = (label_range){key, d, *out};
    *out = r;
  }
  for (node* k = kids ? kids->child : nullptr; k && f->j->status == SNR_OK;
       k = k->next)

    select_label_ranges(f, k, out, depth + 1);
}

/* New /PageLabels number tree giving each selected page the label it had
 * in the source; null on failure. */
static node* select_labels(file* f, const size_t* indices, size_t count) {
  job* j = f->j;
  label_range* ranges = nullptr;
  select_label_ranges(f, get(f->root, "PageLabels"), &ranges, 0);
  node *nums = select_array(j), *labels = take(j, sizeof *labels);
  const label_range* previous = nullptr;

  for (size_t i = 0; i < count && j->status == SNR_OK && tick(j); i++) {
    const label_range* best = nullptr;
    for (const label_range* r = ranges; r && tick(j); r = r->next)
      if (r->key <= indices[i] && (!best || r->key > best->key)) best = r;
    if (i && best == previous && indices[i] == indices[i - 1] + 1) continue;
    previous = best;
    char raw[32];
    snprintf(raw, sizeof raw, "%zu", i);
    select_append(nums, select_token(j, SCALAR, raw));
    if (!best) {
      select_append(nums, select_token(j, DICT, ""));
      continue;
    }

    uint64_t start = 1;
    node* st = resolve(f, get(best->dict, "St"), 0);
    if (st && (!uint_value(st, &start) || !start || start > INT32_MAX)) {
      stop(j, SNR_ERR_INVALID_ARGUMENT, "invalid page label start");
      break;
    }
    if (indices[i] - best->key > INT32_MAX - start) {
      stop(j, SNR_ERR_LIMIT, "page label start limit");
      break;
    }
    start += indices[i] - best->key;

    node* d = select_clone(j, best->dict, 0);
    snprintf(raw, sizeof raw, "%llu", (unsigned long long)start);
    select_set(j, d, "St", select_token(j, SCALAR, raw));
    select_append(nums, d);
  }

  if (!labels || j->status != SNR_OK) return nullptr;
  *labels = (node){.type = DICT};
  select_set(j, labels, "Nums", nums);
  return labels;
}

snr_status spe_pdf_select_pages(const uint8_t* source, size_t size,
                                const size_t* indices, size_t count,
                                const spe_limits* limits, snr_cancel_fn cancel,
                                void* context, snr_buffer* out,
                                snr_error* error) {
  if (!source || !size || !indices || !count || !out)
    return snr_fail(error, SNR_ERR_INVALID_ARGUMENT, 0,
                    "missing PDF selection input");

  job j = {.limits = limits ? *limits : spe_limits_default(),
           .cancel = cancel,
           .context = context};
  writer w = {.j = &j};
  file f;
  if (count > j.limits.maximum_draws ||
      count > SIZE_MAX / sizeof(selected_page))
    stop(&j, SNR_ERR_LIMIT, "PDF selection page limit");

  if (tick(&j) && open_file(&f, &j, source, size)) {
    w.next_number = f.next_number;
    if (f.page_count > SIZE_MAX / sizeof(page*) ||
        f.count > SIZE_MAX / sizeof(node*))
      stop(&j, SNR_ERR_LIMIT, "PDF selection object limit");

    selected_page* copies = take(&j, count * sizeof *copies);
    page** pages = take(&j, f.page_count * sizeof *pages);
    node** widgets = take(&j, f.count * sizeof *widgets);
    uint8_t* seen = take(&j, f.count);
    uint8_t* attached = take(&j, f.count);
    uint32_t tree_id = reserve(&w);
    node* kids = select_array(&j);

    /* Load objects before installing new references: every imported
     * object remains bounded by the same parser and work limits. */
    for (size_t i = 0; i < f.count && tick(&j); i++)
      if (f.entries[i].live && !absent(&f.entries[i]))
        object(&f, &f.entries[i], 0);

    if (j.status == SNR_OK) {
      size_t pi = 0;
      for (page* p = f.pages; p && tick(&j); p = p->next) {
        pages[pi++] = p;
        node* annots = resolve(&f, get(p->object->object, "Annots"), 0);
        if (annots && annots->type != ARRAY) {
          stop(&j, SNR_ERR_INVALID_ARGUMENT, "invalid annotations");
          break;
        }
        for (node* n = annots ? annots->child : nullptr; n && tick(&j);
             n = n->next) {
          entry* e = n->type == REF ? lookup(&f, n->number) : nullptr;
          if (e && e->generation == n->generation) attached[e - f.entries] = 1;
        }
      }

      for (size_t i = 0; i < count && tick(&j); i++) {
        if (indices[i] >= f.page_count) {
          stop(&j, SNR_ERR_INVALID_ARGUMENT, "selected page out of range");
          break;
        }
        selected_page* p = &copies[i];
        p->original = pages[indices[i]];
        p->id = reserve(&w);
        if (!p->original->object->mapped) p->original->object->mapped = p->id;
        select_append(kids, select_ref(&j, p->id, 0));
        node* annots =
            resolve(&f, get(p->original->object->object, "Annots"), 0);
        if (annots && annots->type != ARRAY) {
          stop(&j, SNR_ERR_INVALID_ARGUMENT, "invalid annotations");
          break;
        }

        for (node* n = annots ? annots->child : nullptr; n && tick(&j);
             n = n->next)
          p->count++;
        if (p->count > SIZE_MAX / sizeof *p->annots) {
          stop(&j, SNR_ERR_LIMIT, "annotation count limit");
          break;
        }

        p->annots = take(&j, p->count * sizeof *p->annots);
        size_t k = 0;
        for (node* n = annots ? annots->child : nullptr; n && tick(&j);
             n = n->next, k++) {
          selected_annot* a = &p->annots[k];
          a->original = resolve(&f, n, 0);
          a->entry = n->type == REF ? lookup(&f, n->number) : nullptr;
          if (!a->original || a->original->type != DICT) {
            stop(&j, SNR_ERR_INVALID_ARGUMENT, "invalid annotation dictionary");
            break;
          }
          a->id = reserve(&w);
          a->dict = select_clone(&j, a->original, 0);
          if (named(get(a->original, "Subtype"), "Widget")) {
            if (!a->entry) {
              stop(&j, SNR_ERR_INVALID_ARGUMENT, "widget must be indirect");
              break;
            }
            size_t index = (size_t)(a->entry - f.entries);
            if (!widgets[index]) widgets[index] = select_array(&j);
            select_append(widgets[index], select_ref(&j, a->id, 0));
          } else if (a->entry && !a->entry->mapped)
            a->entry->mapped = a->id;
        }
      }
    }

    node* form =
        j.status == SNR_OK ? resolve(&f, get(f.root, "AcroForm"), 0) : nullptr;
    if (form && (form->type != DICT || get(form, "XFA")))
      stop(&j, SNR_ERR_UNSUPPORTED,
           "page selection requires an AcroForm without XFA");

    /* Clone pages before modifying merged field dictionaries. */
    for (size_t i = 0; i < count && tick(&j); i++) {
      selected_page* p = &copies[i];
      p->dict = select_clone(&j, p->original->object->object, 0);
      select_set(&j, p->dict, "Parent", select_ref(&j, tree_id, 0));
      const char* names[] = {"Resources", "MediaBox", "CropBox", "Rotate"};
      node* inherited[] = {p->original->resources, p->original->media,
                           p->original->crop, p->original->rotate};
      for (size_t k = 0; k < 4; k++)
        if (inherited[k])
          select_set(&j, p->dict, names[k], select_clone(&j, inherited[k], 0));

      node* annots = select_array(&j);
      for (size_t k = 0; k < p->count && tick(&j); k++) {
        selected_annot* a = &p->annots[k];
        select_refs(&f, a->dict, p, 0);
        select_set(&j, a->dict, "P", select_ref(&j, p->id, 0));
        if (named(get(a->original, "Subtype"), "Widget") &&
            select_merged(a->original)) {
          const char* keys[] = {"FT", "T", "TU", "TM", "Ff", "V", "DV", "Kids"};
          for (size_t q = 0; q < sizeof keys / sizeof *keys; q++)
            select_remove(a->dict, keys[q]);
          select_set(&j, a->dict, "Parent",
                     select_ref(&j, a->entry->number, a->entry->generation));
        }
        select_append(annots, select_ref(&j, a->id, 0));
      }

      select_set(&j, p->dict, "Annots", annots);
      select_refs(&f, p->dict, p, 0);
    }

    if (form && j.status == SNR_OK) {
      node* fields =
          select_fields(&f, get(form, "Fields"), widgets, seen, attached, 0);
      select_set(&j, form, "Fields", fields);
      node* order = resolve(&f, get(form, "CO"), 0);

      if (order) {
        node* retained = select_array(&j);
        if (order->type != ARRAY)
          stop(&j, SNR_ERR_INVALID_ARGUMENT, "invalid calculation order");
        for (node* n = order->child; n && tick(&j); n = n->next) {
          entry* e = n->type == REF ? lookup(&f, n->number) : nullptr;
          if (e && seen[e - f.entries] == 2)
            select_append(retained, select_clone(&j, n, 0));
        }
        select_set(&j, form, "CO", retained);
      }
    }

    for (size_t i = 0; i < f.count && tick(&j); i++)
      if (widgets[i] && !seen[i])
        stop(&j, SNR_ERR_INVALID_ARGUMENT,
             "widget missing from form field tree");

    if (get(f.root, "PageLabels"))
      select_set(&j, f.root, "PageLabels", select_labels(&f, indices, count));
    select_set(&j, f.root, "Pages", select_ref(&j, tree_id, 0));

    if (j.status == SNR_OK) {
      header(&w, source, size);
      begin(&w, tree_id, 0);
      format(&w, "<< /Type /Pages /Count %zu /Kids ", count);
      value(&w, nullptr, kids, 0);
      text(&w, " >>");
      end(&w);
    }

    for (size_t i = 0; i < count && tick(&j); i++) {
      selected_page* p = &copies[i];
      begin(&w, p->id, 0);
      value(&w, nullptr, p->dict, 0);
      end(&w);
      for (size_t k = 0; k < p->count && tick(&j); k++) {
        begin(&w, p->annots[k].id, 0);
        value(&w, nullptr, p->annots[k].dict, 0);
        end(&w);
      }
    }

    for (size_t i = 0; i < f.count && tick(&j); i++) {
      entry* e = &f.entries[i];
      if (!e->live || absent(e)) continue;
      node* type = get(e->object, "Type");
      if (e->stream && (named(type, "ObjStm") || named(type, "XRef"))) continue;
      if (named(get(e->object, "FT"), "Sig")) select_remove(e->object, "V");
      if (named(type, "Sig")) {
        select_remove(e->object, "ByteRange");
        select_remove(e->object, "Contents");
      }
      select_refs(&f, e->object, nullptr, 0);
      begin(&w, e->number, e->generation);
      value(&w, nullptr, e->object, 0);
      if (e->stream) {
        text(&w, "\nstream\n");
        put(&w, e->stream, e->stream_size);
        text(&w, "\nendstream");
      }
      end(&w);
    }

    if (j.status == SNR_OK) {
      f.last = 0;
      f.xref_streams = false;
      finish(&w, &f);
    }
  }

  if (j.status == SNR_OK)
    *out = w.data;
  else
    free(w.data.bytes);
  release(&j);
  return j.status == SNR_OK
             ? SNR_OK
             : snr_fail(error, j.status, 0,
                        j.reason ? j.reason : "PDF page selection failed");
}

/* Moves the output, chunks first, into one exactly sized buffer, freeing
 * each chunk as it is copied; with an output, hands it the rest instead. */
static void assemble(writer* w, snr_buffer* out) {
  if (w->output) {
    drain(w);
    return;
  }
  size_t size = position(w);
  uint8_t* bytes = malloc(size);
  if (!bytes) {
    stop(w->j, SNR_ERR_OUT_OF_MEMORY, "output allocation failed");
    return;
  }
  size_t at = 0;
  for (chunk* c = w->chunks; c;) {
    chunk* next = c->next;
    memcpy(bytes + at, c->bytes, c->size);
    at += c->size;
    chunk_free(c);
    c = next;
  }
  w->chunks = w->last_chunk = nullptr;
  memcpy(bytes + at, w->data.bytes, w->data.size);
  free(w->data.bytes);
  w->data = (snr_buffer){0};
  w->capacity = w->flushed = 0;
  *out = (snr_buffer){bytes, size};
}

static bool reach(writer* w, file* f, node* n, entry** stack, size_t* top) {
  if (!n || !tick(f->j)) return f->j->status == SNR_OK;
  if (n->type == REF) {
    entry* e = lookup(f, n->number);
    /* A reference to nothing, such as an object Quartz reserves as a live
     * row at offset zero and never writes, fails when written. */
    if (e && e->live && !absent(e) && e->generation == n->generation &&
        !e->mapped) {
      e->mapped = reserve(w);
      stack[(*top)++] = e;
    }
    return true;
  }
  if (n->type == ARRAY || n->type == DICT)
    for (node* c = n->child; c; c = c->next)
      if (!reach(w, f, c, stack, top)) return false;
  return true;
}

/* Marks share() leaves on an import's objects. */
enum { SHARE_OPEN = 1, SHARE_DONE = 2, SHARE_ABLE = 4, SHARE_OWN = 8 };

/* share()'s marks and digests, by entry index of the imported file. */
typedef struct {
  uint8_t* marks;
  digest* sums;
} sharing;

/* Pushes on todo, grown as needed, each object n refers to that share() has
 * not yet opened. */
static bool push_refs(file* f, node* n, const uint8_t* marks, entry*** todo,
                      size_t* top, size_t* room) {
  if (!n || !tick(f->j)) return f->j->status == SNR_OK;
  if (n->type == REF) {
    entry* e = lookup(f, n->number);
    if (!e || !e->live || absent(e) || e->generation != n->generation ||
        marks[e - f->entries] & (SHARE_OPEN | SHARE_DONE))
      return true;
    if (*top == *room) {
      size_t grown = *room ? *room * 2 : 256;
      entry** more = take(f->j, grown * sizeof *more);
      if (!more) return false;
      if (*top) memcpy(more, *todo, *top * sizeof *more);
      *todo = more;
      *room = grown;
    }
    (*todo)[(*top)++] = e;
    return true;
  }
  if (n->type == ARRAY || n->type == DICT)
    for (node* c = n->child; c; c = c->next)
      if (!push_refs(f, c, marks, todo, top, room)) return false;
  return true;
}

/* Adds name token n, its #xx escapes decoded, to h. */
static void absorb_name(digester* h, const node* n) {
  size_t length = 0;
  for (size_t i = 1; i < n->length; i++, length++)
    if (n->raw[i] == '#') i += 2;
  absorb_size(h, length);
  for (size_t i = 1; i < n->length; i++) {
    uint8_t c = n->raw[i];
    if (c == '#') {
      c = (uint8_t)(hex(n->raw[i + 1]) * 16 + hex(n->raw[i + 2]));
      i += 2;
    }
    absorb(h, &c, 1);
  }
}

/* Adds n's canonical form to h: its kind, a dictionary's keys decoded and in
 * order, a reference as the digest of the object referred to. False, the job
 * still fine, when n refers to an object that cannot be shared. */
static bool canonical(file* f, const sharing* s, digester* h, node* n,
                      unsigned depth) {
  if (!tick(f->j) || depth >= 64) return false;
  uint8_t tag = (uint8_t)n->type;
  absorb(h, &tag, 1);
  if (n->type == REF) {
    entry* e = lookup(f, n->number);
    if (!e || e->generation != n->generation ||
        !(s->marks[e - f->entries] & SHARE_ABLE))
      return false;
    absorb(h, s->sums[e - f->entries].bytes, sizeof(digest));
    return true;
  }
  if (n->type == NAME) {
    absorb_name(h, n);
    return true;
  }
  if (n->type == SCALAR) {
    absorb_size(h, n->length);
    absorb(h, n->raw, n->length);
    return true;
  }

  size_t count = 0;
  for (node* c = n->child; c; c = c->next) count++;
  absorb_size(h, count);
  if (n->type == ARRAY) {
    for (node* c = n->child; c; c = c->next)
      if (!canonical(f, s, h, c, depth + 1)) return false;
    return true;
  }

  node *few[16], **keys = few;
  count /= 2;
  if (count > 16) {
    /* Sorting costs work as unique_keys() charges it. */
    size_t bits = 0;
    for (size_t k = count; k; k >>= 1) bits++;
    if (!charge(f->j, count * bits) ||
        !(keys = take(f->j, count * sizeof *keys)))
      return false;
  }
  size_t at = 0;
  for (node* k = n->child; k; k = k->next->next) keys[at++] = k;
  qsort(keys, count, sizeof *keys, key_order);
  for (size_t i = 0; i < count; i++) {
    absorb_name(h, keys[i]);
    if (!canonical(f, s, h, keys[i]->next, depth + 1)) return false;
  }
  return true;
}

/* Objects whose identity matters, never shared: the document's structure,
 * its pages, annotations and optional content. */
static bool distinct(node* n) {
  static const char* const kinds[] = {
      "Catalog", "Pages", "Page",           "Annot",      "OCG",
      "OCMD",    "Sig",   "StructTreeRoot", "StructElem", "Outlines"};
  node* type = get(n, "Type");
  for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
    if (named(type, kinds[i])) return true;
  return get(n, "Subtype") && get(n, "Rect"); /* An annotation untyped. */
}

/* The digest of imported object e: its canonical form, then its stream's
 * bytes as stored. False, the job still fine, when e cannot be shared. */
static bool object_digest(file* f, const sharing* s, entry* e, digest* out) {
  digester h;
  spe_sha256_init(&h);
  absorb(&h, e->stream ? "S" : "O", 1);
  if (!canonical(f, s, &h, e->object, 0)) return false;
  if (e->stream) {
    if (!charge(f->j, e->stream_size / 4096)) return false;
    absorb_size(&h, e->stream_size);
    absorb(&h, e->stream, e->stream_size);
  }
  *out = spe_sha256_final(&h);
  return true;
}

/* Ahead of an import into a writer keeping a ledger: digests every object the
 * roots reach, those it refers to first, and gives each the ledger has seen
 * the number recorded there, so that the import refers to it instead of
 * writing it again, nor what only it refers to. An object is shared when it
 * is neither a page's content nor of distinct identity, takes no part in a
 * reference cycle, and refers only to objects themselves shared. */
static bool share(writer* w, file* f, node* const* roots, size_t count,
                  sharing* s) {
  job* j = f->j;
  s->marks = take(j, f->count);
  s->sums = take(j, f->count * sizeof *s->sums);
  if (!s->marks || !s->sums) return false;
  for (page* p = f->pages; p; p = p->next) {
    node* c = get(p->object->object, "Contents");
    entry* e = c && c->type == REF ? lookup(f, c->number) : nullptr;
    if (e) s->marks[e - f->entries] |= SHARE_OWN;
    if (e && e->object) c = e->object;
    for (node* k = c && c->type == ARRAY ? c->child : nullptr; k; k = k->next)
      if (k->type == REF && (e = lookup(f, k->number)))
        s->marks[e - f->entries] |= SHARE_OWN;
  }

  entry** todo = nullptr;
  size_t top = 0, room = 0;
  for (size_t i = 0; i < count; i++)
    if (!push_refs(f, roots[i], s->marks, &todo, &top, &room)) return false;
  while (top && j->status == SNR_OK) {
    entry* e = todo[top - 1];
    size_t at = (size_t)(e - f->entries);
    if (!(s->marks[at] & (SHARE_OPEN | SHARE_DONE))) {
      s->marks[at] |= SHARE_OPEN;
      node* n = object(f, e, 0);
      if (!n || !push_refs(f, n, s->marks, &todo, &top, &room)) return false;
      continue;
    }
    top--;
    if (s->marks[at] & SHARE_DONE) continue;
    /* Every object e refers to is done, or open: a cycle through e. */
    s->marks[at] = (uint8_t)((s->marks[at] & ~SHARE_OPEN) | SHARE_DONE);
    if (s->marks[at] & SHARE_OWN || distinct(e->object) ||
        !object_digest(f, s, e, &s->sums[at]))
      continue;
    s->marks[at] |= SHARE_ABLE;
    uint32_t number = ledger_find(w->shared, s->sums[at]);
    if (number) e->mapped = number;
  }
  return j->status == SNR_OK;
}

/* Numbers and writes the objects the roots refer to and those they refer to
 * in turn: what a form or page imported from f draws with, without the
 * file's catalog, page tree or document information. Objects numbered
 * before the call (the page, its tree) are referred to, not written; with a
 * ledger, so are those an earlier import wrote, and the objects written are
 * recorded in it. */
static bool import_reachable(writer* w, file* f, node* const* roots,
                             size_t count) {
  sharing s = {0};
  if (w->shared && !share(w, f, roots, count, &s)) return false;
  entry** stack = take(f->j, (f->count + 1) * sizeof *stack);
  if (!stack) return false;
  size_t top = 0, written = 0;
  for (size_t i = 0; i < count; i++)
    if (!reach(w, f, roots[i], stack, &top)) return false;

  entry** order = take(f->j, (f->count + 1) * sizeof *order);
  if (!order) return false;
  while (top) {
    entry* e = stack[--top];
    order[written++] = e;
    node* n = object(f, e, 0);
    if (!n || !reach(w, f, n, stack, &top)) return false;
  }

  for (size_t i = 0; i < written && w->j->status == SNR_OK; i++) {
    entry* e = order[i];
    begin(w, e->mapped, 0);
    value(w, f, e->object, 0);
    if (e->stream) {
      text(w, "\nstream\n");
      put(w, e->stream, e->stream_size);
      text(w, "\nendstream");
    }
    end(w);
  }
  for (size_t i = 0; s.marks && i < written && w->j->status == SNR_OK; i++) {
    size_t at = (size_t)(order[i] - f->entries);
    if (s.marks[at] & SHARE_ABLE)
      ledger_add(w->shared, s.sums[at], order[i]->mapped);
  }
  return w->j->status == SNR_OK && f->j->status == SNR_OK;
}

/* The values of dictionary d other than its key `except`, as roots. */
static size_t dict_values(node* d, const char* except, node** out,
                          size_t room) {
  size_t n = 0;
  for (node* k = d && d->type == DICT ? d->child : nullptr; k && n < room;
       k = k->next->next)
    if (!named(k, except)) out[n++] = k->next;
  return n;
}

/* The objects a form made of page p needs: its resources and box, and the
 * filter parameters of its single content stream. */

/* An incremental update or a fresh rewrite written page by page
 * (spe_pdf_edit.h). The source is parsed once under the update's own job,
 * which also keeps the xref rows; each page's overlays are parsed and
 * imported under a job of their own, released once they are written, so that
 * neither memory nor work grows with the number of pages, and a page costs no
 * more than its own overlays do. An object an overlay imports identical to
 * one an earlier page's did (a grain mask, a color profile) is referred to
 * instead of written again; only a ledger of digests and object numbers,
 * bounded, outlives the page. */
struct spe_pdf_update {
  job j;
  writer w;
  file f;
  page** pages;  /* source pages by index */
  uint8_t* done; /* pages written: given overlays, or (fresh) at finish */
  uint8_t* skip; /* fresh: source objects written with their pages */
  uint32_t open, close;
  size_t updated, original;
  bool fresh, started;
  ledger shared; /* objects the pages' overlays imported */
};

/* The update's failure as an snr_status, filling error. */
static snr_status update_status(spe_pdf_update* u, snr_error* error) {
  job* j = &u->j;
  return j->status == SNR_OK ? SNR_OK
                             : snr_fail(error, j->status, 0,
                                        j->reason  ? j->reason
                                        : u->fresh ? "PDF rewrite failed"
                                                   : "PDF update failed");
}

void spe_pdf_update_free(spe_pdf_update* u) {
  if (!u) return;
  free(u->w.data.bytes);
  for (chunk* c = u->w.chunks; c;) {
    chunk* next = c->next;
    chunk_free(c);
    c = next;
  }
  free(u->shared.slots);
  release(&u->j);
  free(u);
}

/* Writes source object e as it is. */
static void copy_object(writer* w, file* f, entry* e) {
  node* n = object(f, e, 0);
  if (!n) return;
  begin(w, e->number, e->generation);
  value(w, nullptr, n, 0);
  if (e->stream) {
    text(w, "\nstream\n");
    put(w, e->stream, e->stream_size);
    text(w, "\nendstream");
  }
  end(w);
}

snr_status spe_pdf_update_create(const uint8_t* source, size_t size, bool fresh,
                                 const spe_limits* limits, snr_cancel_fn cancel,
                                 void* context, const snr_output* output,
                                 spe_pdf_update** out, snr_error* error) {
  if (!source || !size || !out)
    return snr_fail(
        error, SNR_ERR_INVALID_ARGUMENT, 0,
        fresh ? "missing PDF rewrite input" : "missing PDF update input");
  spe_pdf_update* u = calloc(1, sizeof *u);
  if (!u)
    return snr_fail(error, SNR_ERR_OUT_OF_MEMORY, 0, "PDF update allocation");
  u->j = (job){.limits = limits ? *limits : spe_limits_default(),
               .cancel = cancel,
               .context = context};
  /* Without an output the file is collected in chunks, assembled once. */
  u->shared.j = &u->j;
  u->w = (writer){
      .j = &u->j, .output = output, .chunked = !output, .shared = &u->shared};
  u->fresh = fresh;
  u->original = size;
  job* j = &u->j;
  file* f = &u->f;

  if (tick(j) && open_file(f, j, source, size)) {
    u->w.next_number = f->next_number;
    if (f->page_count > SIZE_MAX / sizeof(page*))
      stop(j, SNR_ERR_LIMIT, "PDF page limit");
    u->pages = take(j, f->page_count * sizeof *u->pages);
    u->done = take(j, f->page_count);
    if (fresh) u->skip = take(j, f->count);
    size_t at = 0;
    for (page* p = f->pages; p && j->status == SNR_OK; p = p->next) {
      u->pages[at++] = p;
      if (fresh) u->skip[p->object - f->entries] = 1;
    }
  }

  if (j->status == SNR_OK && !fresh) put(&u->w, source, size);
  /* A fresh file keeps object identities and generations, but not historical
   * revisions or object streams; its pages are written as they are given
   * their overlays, the rest now. */
  if (j->status == SNR_OK && fresh) {
    header(&u->w, source, size);
    for (size_t i = 0; i < f->count && tick(j); i++) {
      entry* e = &f->entries[i];
      if (!e->live || absent(e) || u->skip[i]) continue;
      node* n = object(f, e, 0);
      if (!n) break;
      node* type = n->type == DICT ? get(n, "Type") : nullptr;
      if (e->stream && (named(type, "ObjStm") || named(type, "XRef"))) continue;
      copy_object(&u->w, f, e);
    }
  }

  snr_status s = update_status(u, error);
  if (s != SNR_OK) {
    spe_pdf_update_free(u);
    return s;
  }
  *out = u;
  return SNR_OK;
}

snr_status spe_pdf_update_page(spe_pdf_update* u, const spe_pdf_overlay* parts,
                               size_t count, snr_error* error) {
  job* j = &u->j;
  writer* w = &u->w;
  if (!parts || !count || count > MAXIMUM_PARTS)
    stop(j, SNR_ERR_INVALID_ARGUMENT, "missing PDF update input");
  else if (u->updated >= j->limits.maximum_draws)
    stop(j, SNR_ERR_LIMIT, "overlay count limit");
  if (!tick(j)) return update_status(u, error);

  const spe_pdf_overlay* o = &parts[0];
  double determinant =
      o->transform.a * o->transform.d - o->transform.b * o->transform.c;
  bool valid =
      isfinite(determinant) && determinant != 0 &&
      (!o->replace_box ||
       (isfinite(o->box.x) && isfinite(o->box.y) && isfinite(o->box.width) &&
        isfinite(o->box.height) && o->box.width > 0 && o->box.height > 0));
  for (size_t i = 0; i < count; i++)
    valid = valid && parts[i].pdf && parts[i].size && parts[i].page == o->page;
  if (!valid)
    stop(j, SNR_ERR_INVALID_ARGUMENT, "invalid overlay transform/box");
  else if (o->page >= u->f.page_count)
    stop(j, SNR_ERR_INVALID_ARGUMENT, "overlay page outside source");
  else if (u->done[o->page])
    stop(j, SNR_ERR_INVALID_ARGUMENT, "duplicate page overlay");
  if (j->status != SNR_OK) return update_status(u, error);
  u->done[o->page] = 1;

  /* An incremental update starts on a line of its own. */
  if (!u->fresh && !u->started) text(w, "\n");
  u->started = true;
  if (!u->open) {
    u->open = stream_text(w, "q\n");
    u->close = stream_text(w, "Q\n");
  }

  /* The overlays' parse, import and work are this page's alone; their xref
   * rows stay with the update. */
  job scratch = {
      .limits = j->limits, .cancel = j->cancel, .context = j->context};
  if (j->status == SNR_OK) {
    w->j = &scratch;
    w->records = j;
    overlay(w, &u->f, u->pages[o->page], parts, count, u->open, u->close);
    w->j = j;
    w->records = nullptr;
    if (scratch.status != SNR_OK)
      stop(j, scratch.status,
           scratch.reason ? scratch.reason : "PDF update failed");
    release(&scratch);
  }
  if (j->status == SNR_OK) u->updated++;
  return update_status(u, error);
}

/* Reads an array of exactly count numbers. */
static bool numbers(file* f, node* array, double* out, size_t count) {
  node* a = resolve(f, array, 0);
  if (!a || a->type != ARRAY) return false;
  size_t i = 0;
  for (node* c = a->child; c; c = c->next) {
    if (i == count || !decimal(resolve(f, c, 0), &out[i])) return false;
    i++;
  }
  return i == count;
}

/* The normal appearance stream an annotation shows, or null. A dictionary of
 * appearance states selects the one named by /AS. */
static node* normal_appearance(file* f, node* annotation) {
  node* ap = resolve(f, get(annotation, "AP"), 0);
  node* n = get(ap, "N");
  if (n && n->type != REF) {
    node* states = resolve(f, n, 0);
    node* state = get(annotation, "AS");
    n = nullptr;
    if (states && states->type == DICT && state && state->type == NAME)
      for (node* k = states->child; k && k->next; k = k->next->next)
        if (same_name(k, state)) n = k->next;
  }
  if (!n || n->type != REF) return nullptr;
  entry* e = lookup(f, n->number);
  if (!e || e->generation != n->generation || !object(f, e, 0) || !e->stream)
    return nullptr;
  return n;
}

/* True when a widget, through its /Parent chain, belongs to a field listed in
 * the AcroForm. Form renderers draw only such widgets; a widget outside the
 * field tree is still an annotation with an appearance that viewers draw. */
static bool form_field(writer* w, file* f, node* annotation) {
  node* form = resolve(f, get(f->root, "AcroForm"), 0);
  node* fields =
      form && form->type == DICT ? resolve(f, get(form, "Fields"), 0) : nullptr;
  if (!fields || fields->type != ARRAY || !annotation ||
      annotation->type != REF)
    return false;
  uint32_t top = annotation->number;
  node* dict = resolve(f, annotation, 0);
  for (unsigned depth = 0; depth < 64 && dict && dict->type == DICT; depth++) {
    node* parent = get(dict, "Parent");
    if (!parent || parent->type != REF) break;
    top = parent->number;
    dict = resolve(f, parent, 0);
  }
  for (node* c = fields->child; c && tick(w->j); c = c->next)
    if (c->type == REF && c->number == top) return true;
  return false;
}

/* Rewrites page p so that, after its own content, it invokes the normal
 * appearance of each visible annotation, placed as PDF 32000-1 12.5.5
 * defines: the appearance's bounding box, transformed by its /Matrix, is
 * mapped onto the annotation's /Rect. Hidden, NoView and Popup annotations
 * are not drawn, as viewers do not draw them either. With orphans_only, only
 * widgets outside the form's field tree are drawn, for a renderer that draws
 * every other annotation itself. The appearances stay the file's own objects,
 * referred to by the page's extended resources. */
static void appearances(writer* w, file* f, page* p, uint32_t open,
                        uint32_t close, bool orphans_only, size_t* drawn) {
  node* annots = resolve(f, get(p->object->object, "Annots"), 0);
  if (!annots || annots->type != ARRAY) return;
  size_t count = 0;
  for (node* c = annots->child; c; c = c->next) count++;
  node** forms = take(w->j, count * sizeof *forms);
  double (*placement)[6] = take(w->j, count * sizeof *placement);
  if (count && (!forms || !placement)) return;
  size_t used = 0;
  for (node* c = annots->child; c && tick(w->j); c = c->next) {
    node* a = resolve(f, c, 0);
    if (!a || a->type != DICT) continue;
    if (named(get(a, "Subtype"), "Popup")) continue;
    if (orphans_only &&
        (!named(get(a, "Subtype"), "Widget") || form_field(w, f, c)))
      continue;
    double flags = 0;
    if (get(a, "F") && decimal(resolve(f, get(a, "F"), 0), &flags) &&
        flags >= 0 && flags < 4294967296.0 && ((uint32_t)flags & (2u | 32u)))
      continue;
    node* form = normal_appearance(f, a);
    double rect[4], box[4], m[6] = {1, 0, 0, 1, 0, 0};
    if (!form || !numbers(f, get(a, "Rect"), rect, 4)) continue;
    node* dict = lookup(f, form->number)->object;
    if (!numbers(f, get(dict, "BBox"), box, 4)) continue;
    if (get(dict, "Matrix") && !numbers(f, get(dict, "Matrix"), m, 6)) continue;
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (unsigned corner = 0; corner < 4; corner++) {
      double x = box[corner & 1 ? 2 : 0], y = box[corner & 2 ? 3 : 1];
      double tx = m[0] * x + m[2] * y + m[4], ty = m[1] * x + m[3] * y + m[5];
      x0 = fmin(x0, tx);
      y0 = fmin(y0, ty);
      x1 = fmax(x1, tx);
      y1 = fmax(y1, ty);
    }
    double left = fmin(rect[0], rect[2]), right = fmax(rect[0], rect[2]);
    double bottom = fmin(rect[1], rect[3]), top = fmax(rect[1], rect[3]);
    if (!(x1 - x0 > 0) || !(y1 - y0 > 0) || !(right - left > 0) ||
        !(top - bottom > 0))
      continue;
    double sx = (right - left) / (x1 - x0), sy = (top - bottom) / (y1 - y0);
    double* t = placement[used];
    t[0] = sx;
    t[1] = 0;
    t[2] = 0;
    t[3] = sy;
    t[4] = left - x0 * sx;
    t[5] = bottom - y0 * sy;
    bool finite = true;
    for (size_t i = 0; i < 6; i++)
      finite = finite && isfinite(t[i]) &&
               fabs(t[i]) <= w->j->limits.maximum_coordinate;
    if (finite) forms[used++] = form;
  }
  if (w->j->status != SNR_OK || !used) return;

  node *resources = resolve(f, p->resources, 0),
       *objects = resolve(f, get(resources, "XObject"), 0);
  if ((resources && resources->type != DICT) ||
      (objects && objects->type != DICT)) {
    stop(w->j, SNR_ERR_INVALID_ARGUMENT, "invalid page resources");
    return;
  }
  char (*names)[32] = take(w->j, used * sizeof *names);
  if (!names) return;
  for (size_t i = 0; i < used; i++) {
    unsigned suffix = 0;
    do {
      if (!tick(w->j)) return;
      snprintf(names[i], sizeof names[i], "SNRAP%zu_%u", i, suffix++);
    } while (get(objects, names[i]));
  }

  uint32_t invoke = reserve(w);
  writer content = {.j = w->j};
  for (size_t i = 0; i < used; i++) {
    text(&content, "q\n");
    for (size_t k = 0; k < 6; k++) {
      real(&content, placement[i][k]);
      text(&content, " ");
    }
    format(&content, "cm\n/%s Do\nQ\n", names[i]);
  }
  begin(w, invoke, 0);
  format(w, "<< /Length %zu >>\nstream\n", content.data.size);
  put(w, content.data.bytes, content.data.size);
  text(w, "\nendstream");
  end(w);
  free(content.data.bytes);

  begin(w, p->object->number, p->object->generation);
  text(w, "<<\n");
  const char* skip[] = {"Contents", "Resources"};
  fields(w, nullptr, p->object->object, skip, 2);
  format(w, "/Contents [%u 0 R ", open);
  contents(w, f, get(p->object->object, "Contents"));
  format(w, "%u 0 R %u 0 R]\n/Resources <<\n", close, invoke);
  const char* xskip[] = {"XObject"};
  fields(w, nullptr, resources, xskip, 1);
  text(w, "/XObject <<\n");
  fields(w, nullptr, objects, nullptr, 0);
  for (size_t i = 0; i < used; i++) {
    format(w, "/%s ", names[i]);
    value(w, nullptr, forms[i], 0);
    text(w, "\n");
  }
  text(w, ">>\n>>\n>>");
  end(w);
  *drawn = used;
}

snr_status spe_pdf_update_appearances(spe_pdf_update* u, size_t index,
                                      bool orphans_only, size_t* drawn,
                                      snr_error* error) {
  job* j = &u->j;
  writer* w = &u->w;
  *drawn = 0;
  if (u->fresh || index >= u->f.page_count || u->done[index])
    stop(j, SNR_ERR_INVALID_ARGUMENT, "invalid appearance page");
  if (!tick(j)) return update_status(u, error);
  if (!u->started) text(w, "\n");
  if (!u->open) {
    u->open = stream_text(w, "q\n");
    u->close = stream_text(w, "Q\n");
  }
  u->started = true;
  appearances(w, &u->f, u->pages[index], u->open, u->close, orphans_only,
              drawn);
  if (j->status == SNR_OK) u->done[index] = 1;
  return update_status(u, error);
}

snr_status spe_pdf_update_finish(spe_pdf_update* u, snr_buffer* out,
                                 spe_pdf_incremental_report* report,
                                 snr_error* error) {
  job* j = &u->j;
  writer* w = &u->w;
  file* f = &u->f;
  if (!out && !w->output)
    stop(j, SNR_ERR_INVALID_ARGUMENT, "missing PDF update output");

  if (u->fresh) {
    for (size_t i = 0; i < f->page_count && tick(j); i++)
      if (!u->done[i]) copy_object(w, f, u->pages[i]->object);
    if (j->status == SNR_OK) {
      f->last = 0;
      f->xref_streams = false;
      finish(w, f);
    }
  } else if (u->started && j->status == SNR_OK) {
    /* A soft mask needs PDF 1.4 even when the original header predates it.
     * The catalog override leaves the original revision's bytes intact. */
    node* version = get(f->root, "Version");
    bool old_version = !version || named(version, "1.0") ||
                       named(version, "1.1") || named(version, "1.2") ||
                       named(version, "1.3");
    if (f->size >= 8 && !memcmp(f->bytes, "%PDF-1.", 7) && f->bytes[7] < '4' &&
        old_version) {
      node* ref = get(f->trailer, "Root");
      begin(w, ref->number, ref->generation);
      text(w, "<< /Version /1.4 ");
      const char* skip[] = {"Version"};
      fields(w, nullptr, f->root, skip, 1);
      text(w, ">>");
      end(w);
    }
    finish(w, f);
  }

  /* Zero overlays leave an exact copy of the source. */
  if (j->status == SNR_OK) assemble(w, out);
  snr_status s = update_status(u, error);
  if (s == SNR_OK && report)
    *report = (spe_pdf_incremental_report){u->updated, u->original, !u->fresh};
  stop(j, SNR_ERR_INVALID_ARGUMENT, "PDF update already finished");
  return s;
}
