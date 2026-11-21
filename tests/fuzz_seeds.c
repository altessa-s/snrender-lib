/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Writes small, complete notebooks for the fuzz corpus. Device fixtures are
 * hundreds of kilobytes because they carry stroke logs, thumbnails and
 * recognition archives the renderer never reads; these seeds keep only what
 * rendering consumes, so mutations land on fields that change its paths.
 * Sheets cannot shrink: the reader derives page size from the device, so
 * every seed uses the smallest real sheet unless it tests another one.
 * Usage: fuzz_seeds output-directory layer.png layer.jpg */
#include <snrender/snrender.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct file {
  uint8_t* data;
  size_t size, capacity;
} file;
[[noreturn]] static void fail(const char* what) {
  fprintf(stderr, "fuzz_seeds: %s\n", what);
  exit(1);
}
static void append(file* f, const void* data, size_t size) {
  if (f->size + size > f->capacity) {
    size_t capacity = (f->size + size) * 2;
    uint8_t* grown = realloc(f->data, capacity);
    if (!grown) fail("out of memory");
    f->data = grown;
    f->capacity = capacity;
  }
  memcpy(f->data + f->size, data, size);
  f->size += size;
}
static void u32(file* f, uint32_t value) {
  uint8_t bytes[4];
  for (size_t i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (8 * i));
  append(f, bytes, 4);
}
/* Every referenced block is a little-endian length and its bytes; the
 * returned offset is what metadata and the footer refer to. */
static size_t block(file* f, const void* data, size_t size) {
  size_t address = f->size;
  u32(f, (uint32_t)size);
  append(f, data, size);
  return address;
}
/* MinGW reads the plain printf archetype as Microsoft's, which has no %zu;
 * the C runtime this is built against does format it. */
#if defined(__MINGW32__) && !defined(__clang__)
static size_t text(file* f, const char* format, ...)
    __attribute__((format(gnu_printf, 2, 3)));
#else
static size_t text(file* f, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
#endif
static size_t text(file* f, const char* format, ...) {
  char buffer[8192];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(buffer, sizeof buffer, format, arguments);
  va_end(arguments);
  if (length < 0 || (size_t)length >= sizeof buffer) fail("metadata length");
  return block(f, buffer, (size_t)length);
}
static const char* base64(const char* input, char* out, size_t capacity) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const uint8_t* in = (const uint8_t*)input;
  size_t size = strlen(input), j = 0;
  if ((size + 2) / 3 * 4 >= capacity) fail("base64 length");
  for (size_t i = 0; i < size; i += 3) {
    uint32_t bits = (uint32_t)in[i] << 16;
    if (i + 1 < size) bits |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < size) bits |= in[i + 2];
    out[j++] = alphabet[(bits >> 18) & 63];
    out[j++] = alphabet[(bits >> 12) & 63];
    out[j++] = i + 1 < size ? alphabet[(bits >> 6) & 63] : '=';
    out[j++] = i + 2 < size ? alphabet[bits & 63] : '=';
  }
  out[j] = 0;
  return out;
}
static size_t base64_block(file* f, const char* json) {
  char encoded[8192];
  return text(f, "%s", base64(json, encoded, sizeof encoded));
}

/* RATTA_RLE pairs are a palette code and a run length minus one; 0xff marks
 * a 0x4000-pixel block. Runs here never set the high bit otherwise, so no
 * pair depends on its neighbour. Unlisted pixels use the transparent code. */
typedef struct run {
  size_t start, length;
  uint8_t code;
} run;
constexpr uint8_t TRANSPARENT = 0x62;
static void emit(file* rle, uint8_t code, size_t length) {
  for (; length >= 0x4000; length -= 0x4000)
    append(rle, (uint8_t[]){code, 0xff}, 2);
  for (; length >= 128; length -= 128) append(rle, (uint8_t[]){code, 127}, 2);
  if (length) append(rle, (uint8_t[]){code, (uint8_t)(length - 1)}, 2);
}
static size_t rle_block(file* f, size_t width, size_t height, const run* runs,
                        size_t count) {
  file rle = {0};
  size_t cursor = 0, total = width * height;
  for (size_t i = 0; i < count; i++) {
    if (runs[i].start < cursor || runs[i].start + runs[i].length > total)
      fail("ink runs must be ordered and inside the sheet");
    emit(&rle, TRANSPARENT, runs[i].start - cursor);
    emit(&rle, runs[i].code, runs[i].length);
    cursor = runs[i].start + runs[i].length;
  }
  emit(&rle, TRANSPARENT, total - cursor);
  size_t address = block(f, rle.data, rle.size);
  free(rle.data);
  return address;
}
/* Horizontal bars, one per row, are the cheapest ink the format can hold. */
static size_t ink_layer(file* f, size_t width, size_t height, size_t top,
                        uint8_t code) {
  run runs[8];
  for (size_t i = 0; i < 8; i++)
    runs[i] = (run){(top + i * 3) * width + 100 + i * 40, 400, code};
  return rle_block(f, width, height, runs, 8);
}
static size_t layer(file* f, const char* name, const char* protocol,
                    size_t bitmap) {
  return text(f,
              "<LAYERTYPE:NOTE><LAYERPROTOCOL:%s><LAYERNAME:%s><LAYERPATH:0>"
              "<LAYERBITMAP:%zu><LAYERVECTORGRAPH:0><LAYERRECOGN:0>",
              protocol, name, bitmap);
}
static size_t bytes_block(file* f, const file* payload, size_t size) {
  return block(f, payload->data, size < payload->size ? size : payload->size);
}

typedef struct page_spec {
  size_t main, layer1, layer2, layer3, background, recognition;
  const char* extra;
} page_spec;
static size_t page(file* f, size_t number, const page_spec* p) {
  return text(f,
              "<PAGESTYLE:style_white><MAINLAYER:%zu><LAYER1:%zu><LAYER2:%zu>"
              "<LAYER3:%zu><BGLAYER:%zu><TOTALPATH:0><RECOGNTEXT:%zu>"
              "<RECOGNFILE:0><PAGEID:P%zu><DISABLE:none>%s",
              p->main, p->layer1, p->layer2, p->layer3, p->background,
              p->recognition, number, p->extra ? p->extra : "");
}

typedef struct notebook {
  file f, footer;
  const char* equipment;
} notebook;
static notebook begin(const char* kind, const char* version,
                      const char* equipment) {
  notebook n = {.equipment = equipment};
  char signature[32];
  snprintf(signature, sizeof signature, "%sSN_FILE_VER_%s", kind, version);
  if (strlen(signature) != 24) fail("signature length");
  append(&n.f, signature, 24);
  return n;
}
static void key(notebook* n, const char* format, size_t a, size_t b,
                size_t address) {
  char name[64], entry[96];
  snprintf(name, sizeof name, format, a, b);
  int length = snprintf(entry, sizeof entry, "<%s:%zu>", name, address);
  append(&n->footer, entry, (size_t)length);
}
/* The header comes last so a mark can point it at its highlight block. */
static file finish(notebook* n, const char* header_extra) {
  size_t header = text(&n->f,
                       "<MODULE_LABEL:SNFILE_FEATURE><APPLY_EQUIPMENT:%s>"
                       "<FILE_ID:Fseed>%s",
                       n->equipment, header_extra ? header_extra : "");
  char entry[64];
  int length = snprintf(entry, sizeof entry, "<FILE_FEATURE:%zu>", header);
  append(&n->footer, entry, (size_t)length);
  size_t footer = block(&n->f, n->footer.data, n->footer.size);
  append(&n->f, "tail", 4);
  u32(&n->f, (uint32_t)footer);
  free(n->footer.data);
  return n->f;
}

constexpr size_t W = 1404;
constexpr size_t H = 1872;
static const char layer_table[] =
    "<LAYERINFO:[{\"layerId\"#3,\"name\"#\"Layer 3\",\"isVisible\"#true},"
    "{\"layerId\"#2,\"name\"#\"Layer 2\",\"isVisible\"#false},"
    "{\"layerId\"#1,\"name\"#\"Layer 1\",\"isVisible\"#true},"
    "{\"layerId\"#0,\"name\"#\"Main Layer\",\"isCurrentLayer\"#true},"
    "{\"layerId\"#-1,\"name\"#\"Background Layer\",\"isBackgroundLayer\"#"
    "true}]><LAYERSEQ:LAYER3,LAYER2,LAYER1,MAINLAYER,BGLAYER>";

/* Every layer key, each with its own palette codes; layer 2 is hidden. */
static file layers(void) {
  notebook n = begin("note", "20230015", "A6X");
  page_spec p = {
      .main = layer(&n.f, "MAINLAYER", "RATTA_RLE",
                    ink_layer(&n.f, W, H, 100, 0x61)),
      .layer1 =
          layer(&n.f, "LAYER1", "RATTA_RLE", ink_layer(&n.f, W, H, 300, 0x66)),
      .layer2 =
          layer(&n.f, "LAYER2", "RATTA_RLE", ink_layer(&n.f, W, H, 500, 0x63)),
      .layer3 =
          layer(&n.f, "LAYER3", "RATTA_RLE", ink_layer(&n.f, W, H, 700, 0x68)),
      .background =
          layer(&n.f, "BGLAYER", "RATTA_RLE", ink_layer(&n.f, W, H, 900, 0x64)),
      .extra = layer_table};
  key(&n, "PAGE%zu", 1, 0, page(&n.f, 1, &p));
  return finish(&n, nullptr);
}
/* PNG and JPEG layers, plus a truncated PNG that strict mode rejects. */
static file images(const file* png, const file* jpeg) {
  notebook n = begin("note", "20230015", "N6");
  page_spec p = {
      .main = layer(&n.f, "MAINLAYER", "PNG", bytes_block(&n.f, png, SIZE_MAX)),
      .layer1 =
          layer(&n.f, "LAYER1", "RATTA_RLE", ink_layer(&n.f, W, H, 200, 0x61)),
      .layer2 = layer(&n.f, "LAYER2", "PNG", bytes_block(&n.f, png, 64)),
      .background = layer(&n.f, "BGLAYER", "RATTA_RLE",
                          bytes_block(&n.f, jpeg, SIZE_MAX))};
  key(&n, "PAGE%zu", 1, 0, page(&n.f, 1, &p));
  return finish(&n, nullptr);
}
/* Web, mail and rejected schemes, internal targets by page number and by
 * page identifier, another file, a backlink and a box beyond the sheet. */
static file links(void) {
  notebook n = begin("note", "20230015", "A6X");
  size_t blank =
      layer(&n.f, "BGLAYER", "RATTA_RLE", rle_block(&n.f, W, H, nullptr, 0));
  for (size_t i = 1; i <= 3; i++) {
    page_spec p = {.main = layer(&n.f, "MAINLAYER", "RATTA_RLE",
                                 ink_layer(&n.f, W, H, 100 * i, 0x61)),
                   .background = blank};
    key(&n, "PAGE%zu", i, 0, page(&n.f, i, &p));
  }
  static const struct {
    size_t page;
    int kind, direction;
    const char *target, *file_id, *page_id, *rect;
    size_t object;
  } specs[] = {
      {1, 4, 0, "https://example.com/a?b=(c)", "none", "none", "100,100,300,60",
       0},
      {1, 4, 0, "mailto:someone@example.com", "none", "none", "100,200,300,60",
       0},
      {1, 4, 0, "javascript:alert(1)", "none", "none", "100,300,300,60", 0},
      {1, 0, 0, "/note/seed.note", "Fseed", "none", "100,400,300,60", 3},
      {1, 0, 0, "/note/seed.note", "Fseed", "P2", "100,500,300,60", 0},
      {1, 0, 0, "/note/other.note", "Fother", "P1", "100,600,300,60", 1},
      {2, 0, 1, "/note/seed.note", "Fseed", "P1", "100,700,300,60", 1},
      {2, 4, 0, "HTTP://EXAMPLE.COM/upper", "none", "none", "1300,1800,400,400",
       0},
      {3, 0, 0, "/note/seed.note", "Fseed", "P1", "-50,-50,100,100", 0}};
  for (size_t i = 0; i < sizeof specs / sizeof *specs; i++) {
    char target[256], shown[64];
    size_t address = text(
        &n.f,
        "<LINKTYPE:%d><LINKINOUT:%d><LINKBITMAP:0><LINKSTYLE:20000000>"
        "<LINKRECT:%s><LINKRECTORI:%s><LINKPROTOCAL:RATTA_RLE><LINKFILE:%s>"
        "<LINKFILEID:%s><PAGEID:%s><OBJPAGE:%zu><SHOWTEXT:%s>",
        specs[i].kind, specs[i].direction, specs[i].rect, specs[i].rect,
        base64(specs[i].target, target, sizeof target), specs[i].file_id,
        specs[i].page_id, specs[i].object,
        base64("\xef\xbb\xbflink", shown, sizeof shown));
    key(&n, "LINKO_%04zu%010zu", specs[i].page, i, address);
  }
  return finish(&n, nullptr);
}
/* Nested headings across pages, on the legacy palette's format version. */
static file outline(void) {
  notebook n = begin("note", "20220011", "A5X");
  static const int levels[] = {1, 2, 3, 1, 2, 2};
  for (size_t i = 1; i <= 4; i++) {
    page_spec p = {.main = layer(&n.f, "MAINLAYER", "RATTA_RLE",
                                 ink_layer(&n.f, W, H, 50 * i, 0x61))};
    key(&n, "PAGE%zu", i, 0, page(&n.f, i, &p));
  }
  for (size_t i = 0; i < sizeof levels / sizeof *levels; i++) {
    size_t address =
        text(&n.f,
             "<TITLESEQNO:%zu><TITLELEVEL:%d><TITLERECT:100,%zu,600,80>"
             "<TITLERECTORI:100,%zu,600,80><TITLEBITMAP:0>"
             "<TITLEPROTOCOL:RATTA_RLE><TITLESTYLE:1000254>",
             i, levels[i], 100 + i * 100, 100 + i * 100);
    key(&n, "TITLE_%04zu%010zu", i / 2 + 1, i, address);
  }
  return finish(&n, nullptr);
}
/* Words in several scripts, a control character, an empty word and boxes
 * that are inverted or outside the sheet. Boxes are in millimetres. The JSON
 * spelling differs from the parsed label only where JSON escapes. */
static const struct seed_word {
  const char *json, *label;
  int x, y, width;
} seed_words[] = {
    {"Hello", "Hello", 10, 10, 20},
    {"\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82",
     "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", 35, 10, 25},
    {"\xce\xba\xce\xb1\xce\xbb\xce\xb7\xce\xbc\xce\xad\xcf\x81\xce\xb1",
     "\xce\xba\xce\xb1\xce\xbb\xce\xb7\xce\xbc\xce\xad\xcf\x81\xce\xb1", 65, 10,
     30},
    {"\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7",
     "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7", 10, 20, 20},
    {"\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", 35,
     20, 20},
    {"\xe4\xbd\xa0\xe5\xa5\xbd", "\xe4\xbd\xa0\xe5\xa5\xbd", 60, 20, 12},
    {"\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87",
     "\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87",
     75, 20, 20},
    {"\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa",
     "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa", 10, 30, 15},
    {"\xf0\x9f\x98\x80 ok", "\xf0\x9f\x98\x80 ok", 30, 30, 15},
    {"tab\\tstop", "tab\tstop", 50, 30, 15},
    {"", "", 70, 30, 5},
    {"inverted", "inverted", 90, 40, -20},
    {"outside", "outside", 500, 500, 20}};
constexpr size_t SEED_WORDS = sizeof seed_words / sizeof *seed_words;
static file recognition(void) {
  notebook n = begin("note", "20230015", "N6");
  char json[4096];
  size_t used = (size_t)snprintf(
      json, sizeof json,
      "{\"elements\":[{\"type\":\"Text\",\"label\":\"mixed\",\"words\":[");
  for (size_t i = 0; i < SEED_WORDS; i++) {
    int length = snprintf(json + used, sizeof json - used,
                          "%s{\"label\":\"%s\",\"bounding-box\":{\"x\":%d,"
                          "\"y\":%d,\"width\":%d,\"height\":5}}",
                          i ? "," : "", seed_words[i].json, seed_words[i].x,
                          seed_words[i].y, seed_words[i].width);
    if (length < 0 || (size_t)length >= sizeof json - used)
      fail("recognition length");
    used += (size_t)length;
  }
  if (snprintf(json + used, sizeof json - used,
               "]},{\"type\":\"Diagram\",\"label\":\"shape\"}]}") < 0)
    fail("recognition length");
  page_spec p = {.main = layer(&n.f, "MAINLAYER", "RATTA_RLE",
                               ink_layer(&n.f, W, H, 150, 0x61)),
                 .recognition = base64_block(&n.f, json)};
  key(&n, "PAGE%zu", 1, 0, page(&n.f, 1, &p));
  return finish(&n, nullptr);
}
/* A document mark: transparent default background and a highlight table. */
static file mark(bool overlay) {
  notebook n = begin("mark", "20230015", "N6");
  for (size_t i = 1; i <= 2; i++) {
    page_spec p = {.main = layer(&n.f, "MAINLAYER", "RATTA_RLE",
                                 ink_layer(&n.f, W, H, 400 * i, 0x67))};
    key(&n, "PAGE%zu", i, 0, page(&n.f, i, &p));
  }
  char header[64];
  snprintf(
      header, sizeof header, "<HIGHLIGHTINFO:%zu>",
      base64_block(
          &n.f,
          overlay ? "{\"0\":[{\"page\":0,\"annotationType\":0,\"colorType\":0,"
                    "\"annotationContent\":\"note\",\"rnRectList\":[{\"left\":"
                    "117,\"top\":234,\"right\":351,\"bottom\":468}],"
                    "\"mupdfRectList\":[{\"x0\":10,\"y0\":20,\"x1\":30,\"y1\":"
                    "40}]}]}"
                  : "{\"1\":[{\"page\":1,\"annotationType\":1,\"colorType\":2,"
                    "\"annotationContent\":\"note\",\"time\":1700000000000,"
                    "\"rnRectList\":[{\"left\":10,\"top\":20,\"right\":300,"
                    "\"bottom\":60}]}]}"));
  return finish(&n, header);
}
/* Five pages: landscape, a moved Manta page, a moved A5X page at 226 DPI, a
 * page with no layers and a page whose background is left unstacked. */
static file pages(void) {
  notebook n = begin("note", "20230015", "A6X");
  size_t blank =
      layer(&n.f, "BGLAYER", "RATTA_RLE", rle_block(&n.f, W, H, nullptr, 0));
  page_spec p[5] = {{.main = layer(&n.f, "MAINLAYER", "RATTA_RLE",
                                   ink_layer(&n.f, W, H, 100, 0x61)),
                     .background = blank},
                    {.main = layer(&n.f, "MAINLAYER", "RATTA_RLE",
                                   ink_layer(&n.f, H, W, 100, 0x61)),
                     .background = blank,
                     .extra = "<ORIENTATION:1090>"},
                    {.main = layer(&n.f, "MAINLAYER", "RATTA_RLE",
                                   ink_layer(&n.f, 1920, 2560, 100, 0x61)),
                     .extra = "<PAGEMACHINETYPE:N5>"},
                    {.main = layer(&n.f, "MAINLAYER", "RATTA_RLE",
                                   ink_layer(&n.f, W, H, 300, 0x65)),
                     .background = blank,
                     .extra = "<PAGEMACHINETYPE:A5X>"},
                    {.extra = "<LAYERSEQ:MAINLAYER>"}};
  p[4].background = blank;
  for (size_t i = 0; i < 5; i++)
    key(&n, "PAGE%zu", i + 1, 0, page(&n.f, i + 1, p + i));
  return finish(&n, nullptr);
}

static file load(const char* path) {
  file f = {0};
  FILE* in = fopen(path, "rb");
  if (!in) fail("cannot open image");
  uint8_t buffer[4096];
  for (size_t read; (read = fread(buffer, 1, sizeof buffer, in));)
    append(&f, buffer, read);
  if (ferror(in) || fclose(in) || !f.size) fail("cannot read image");
  return f;
}
/* Each seed must parse, through the reader's model, into exactly what it was
 * written to cover, or the corpus would silently stop covering it after a
 * format change in the generator or the reader. */
static const char* checking = "";
#define EXPECT(x)                                            \
  do {                                                       \
    if (!(x)) {                                              \
      fprintf(stderr, "fuzz_seeds: %s: %s\n", checking, #x); \
      exit(1);                                               \
    }                                                        \
  } while (0)

static bool equal(sn_string s, const char* text) {
  return s.data && s.length == strlen(text) && !memcmp(s.data, text, s.length);
}
static const sn_layer* named(const sn_page* p, sn_layer_name name) {
  const sn_layer* l = sn_page_layer(p, name);
  EXPECT(l && l->bitmap);
  return l;
}
static void sheet(const sn_page* p, double width, double height) {
  EXPECT(p->size.width == width && p->size.height == height);
}
/* Image dimensions from the PNG IHDR or the first JPEG frame header. */
static void dimensions(const sn_bitmap* b, size_t* width, size_t* height) {
  const uint8_t* d = b->bytes.data;
  size_t n = b->bytes.size;
  *width = *height = 0;
  if (b->encoding == SN_ENCODING_PNG && n >= 24) {
    *width =
        (size_t)d[16] << 24 | (size_t)d[17] << 16 | (size_t)d[18] << 8 | d[19];
    *height =
        (size_t)d[20] << 24 | (size_t)d[21] << 16 | (size_t)d[22] << 8 | d[23];
    return;
  }
  for (size_t i = 2; b->encoding == SN_ENCODING_JPEG && i + 9 <= n;) {
    if (d[i] != 0xff) return;
    uint8_t marker = d[i + 1];
    if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 &&
        marker != 0xcc) {
      *height = (size_t)d[i + 5] << 8 | d[i + 6];
      *width = (size_t)d[i + 7] << 8 | d[i + 8];
      return;
    }
    i += 2 + ((size_t)d[i + 2] << 8 | d[i + 3]);
  }
}

static void expect_layers(const sn_notebook* n) {
  EXPECT(n->page_count == 1 && n->pages[0].layer_count == 5);
  const sn_page* p = n->pages;
  sheet(p, W, H);
  EXPECT(p->layer_info_count == 5);
  for (int name = SN_LAYER_MAIN; name <= SN_LAYER_BACKGROUND; name++) {
    const sn_layer* l = named(p, (sn_layer_name)name);
    EXPECT(l->bitmap->encoding == SN_ENCODING_RATTA_RLE && l->is_stacked);
    EXPECT(l->is_visible == (name != SN_LAYER_2));
  }
}
static void expect_images(const sn_notebook* n, const file* png,
                          const file* jpeg) {
  EXPECT(n->page_count == 1 && n->pages[0].layer_count == 4);
  const sn_page* p = n->pages;
  sheet(p, W, H);
  const sn_layer* picture = named(p, SN_LAYER_MAIN);
  const sn_layer* truncated = named(p, SN_LAYER_2);
  const sn_layer* background = named(p, SN_LAYER_BACKGROUND);
  EXPECT(picture->bitmap->encoding == SN_ENCODING_PNG &&
         picture->bitmap->bytes.size == png->size);
  EXPECT(background->bitmap->encoding == SN_ENCODING_JPEG &&
         background->bitmap->bytes.size == jpeg->size);
  EXPECT(truncated->bitmap->encoding == SN_ENCODING_PNG &&
         truncated->bitmap->bytes.size == 64);
  EXPECT(named(p, SN_LAYER_1)->bitmap->encoding == SN_ENCODING_RATTA_RLE);
  size_t width, height;
  dimensions(picture->bitmap, &width, &height);
  EXPECT(width == 50 && height == 30);
  dimensions(background->bitmap, &width, &height);
  EXPECT(width == 50 && height == 30);
}
static void expect_links(const sn_notebook* n) {
  EXPECT(n->page_count == 3 && n->link_count == 9 && !n->title_count);
  size_t web = 0, internal = 0, back = 0;
  for (size_t i = 0; i < n->link_count; i++) {
    const sn_link* l = n->links + i;
    EXPECT(!l->rect.is_null && l->page_number >= 1 && l->page_number <= 3);
    web += l->kind == 4 && l->url.data;
    internal += l->kind == 0;
    back += l->direction == 1;
  }
  EXPECT(web == 4 && internal == 5 && back == 1);
  EXPECT(equal(n->links[0].url, "https://example.com/a?b=(c)"));
  EXPECT(equal(n->links[2].url, "javascript:alert(1)"));
  EXPECT(n->links[3].has_target_page_number &&
         n->links[3].target_page_number == 3);
  EXPECT(equal(n->links[4].target_page_id, "P2"));
  EXPECT(equal(n->links[5].target_file_id, "Fother"));
  EXPECT(n->links[7].rect.x == 1300 && n->links[8].rect.x == -50);
  EXPECT(equal(n->file_id, "Fseed") && equal(n->pages[1].id, "P2"));
}
static void expect_outline(const sn_notebook* n) {
  static const int64_t levels[] = {1, 2, 3, 1, 2, 2},
                       pages[] = {1, 1, 2, 2, 3, 3};
  EXPECT(n->page_count == 4 && n->title_count == 6);
  EXPECT(n->equipment == SN_EQUIPMENT_A5X && n->format_version == 20220011);
  for (size_t i = 0; i < 6; i++)
    EXPECT(n->titles[i].level == levels[i] &&
           n->titles[i].page_number == pages[i]);
}
static void expect_recognition(const sn_notebook* n) {
  EXPECT(n->page_count == 1 && n->pages[0].recognition);
  const sn_recognition* r = n->pages[0].recognition;
  EXPECT(r->element_count == 2 && equal(r->elements[1].kind, "Diagram"));
  const sn_element* text = r->elements;
  EXPECT(equal(text->kind, "Text") && text->word_count == SEED_WORDS);
  for (size_t i = 0; i < SEED_WORDS; i++) {
    EXPECT(equal(text->words[i].label, seed_words[i].label));
    EXPECT(text->words[i].box.x == seed_words[i].x &&
           text->words[i].box.width == seed_words[i].width);
  }
}
static void expect_mark(const sn_notebook* n) {
  EXPECT(n->kind == SN_KIND_MARK && n->page_count == 2);
  EXPECT(n->highlight_count == 1 && n->highlights[0].rect_count == 1);
  EXPECT(equal(n->highlights[0].comment, "note"));
  for (size_t i = 0; i < 2; i++)
    EXPECT(named(n->pages + i, SN_LAYER_MAIN)->is_visible);
}
static void expect_pages(const sn_notebook* n) {
  EXPECT(n->page_count == 5 && n->equipment == SN_EQUIPMENT_A6X);
  const sn_page* p = n->pages;
  sheet(p, W, H);
  sheet(p + 1, H, W);
  EXPECT(p[1].orientation == SN_ORIENTATION_LANDSCAPE);
  sheet(p + 2, 1920, 2560);
  sheet(p + 3, W, H);
  const sn_string* machine = sn_metadata_get(&p[3].metadata, "PAGEMACHINETYPE");
  EXPECT(machine && equal(*machine, "A5X"));
  EXPECT(p[4].layer_count == 1 && !p[4].layers[0].is_stacked &&
         !p[4].layers[0].is_visible);
}

typedef enum seed {
  LAYERS,
  IMAGES,
  LINKS,
  OUTLINE,
  RECOGNITION,
  MARK,
  PAGES
} seed;
static void save(const char* directory, const char* name, file f, seed kind,
                 const file* png, const file* jpeg) {
  checking = name;
  sn_notebook* n = nullptr;
  EXPECT(sn_parse_notebook(f.data, f.size, &n, nullptr) == SN_OK);
  EXPECT(!n->skipped_pages);
  switch (kind) {
    case LAYERS:
      expect_layers(n);
      break;
    case IMAGES:
      expect_images(n, png, jpeg);
      break;
    case LINKS:
      expect_links(n);
      break;
    case OUTLINE:
      expect_outline(n);
      break;
    case RECOGNITION:
      expect_recognition(n);
      break;
    case MARK:
      expect_mark(n);
      break;
    case PAGES:
      expect_pages(n);
      break;
  }
  sn_notebook_free(n);
  bool ok;
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", directory, name);
  FILE* out = fopen(path, "wb");
  if (!out) fail(path);
  ok = fwrite(f.data, 1, f.size, out) == f.size;
  if (fclose(out) || !ok) fail(path);
  free(f.data);
}

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s directory layer.png layer.jpg\n", argv[0]);
    return 2;
  }
  file png = load(argv[2]), jpeg = load(argv[3]);
  save(argv[1], "seed-layers.note", layers(), LAYERS, &png, &jpeg);
  save(argv[1], "seed-images.note", images(&png, &jpeg), IMAGES, &png, &jpeg);
  save(argv[1], "seed-links.note", links(), LINKS, &png, &jpeg);
  save(argv[1], "seed-outline.note", outline(), OUTLINE, &png, &jpeg);
  save(argv[1], "seed-recognition.note", recognition(), RECOGNITION, &png,
       &jpeg);
  save(argv[1], "seed-highlights.mark", mark(false), MARK, &png, &jpeg);
  save(argv[1], "seed-overlay.mark", mark(true), MARK, &png, &jpeg);
  save(argv[1], "seed-pages.note", pages(), PAGES, &png, &jpeg);
  free(png.data);
  free(jpeg.data);
  return 0;
}
