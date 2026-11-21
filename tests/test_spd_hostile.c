/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
/* Exercise malformed tiles, geometry, limits, cancellation and sink errors. */
#include <float.h>
#include <math.h>
#include <snrender/snrender.h>
#include <snspd/snspd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static FILE* parity;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      abort();                                                \
    }                                                         \
  } while (0)

typedef struct {
  uint8_t* d;
  size_t n, cap;
} buf;
static void put(buf* b, const void* p, size_t n) {
  if (b->n + n > b->cap) {
    b->cap = (b->n + n) * 2 + 64;
    b->d = realloc(b->d, b->cap);
    if (!b->d) abort();
  }
  memcpy(b->d + b->n, p, n);
  b->n += n;
}
static void be32(buf* b, uint32_t v) {
  uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),
                  (uint8_t)v};
  put(b, x, 4);
}
static void chunk(buf* b, const char* tag, const uint8_t* data, size_t n) {
  be32(b, (uint32_t)n);
  uLong c = crc32(0, (const Bytef*)tag, 4);
  if (n) c = crc32(c, data, (uInt)n);
  put(b, tag, 4);
  if (n) put(b, data, n);
  be32(b, (uint32_t)c);
}
static void ihdr(buf* b, uint32_t w, uint32_t h, uint8_t depth, uint8_t type,
                 uint8_t interlace) {
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  put(b, sig, 8);
  uint8_t d[13] = {(uint8_t)(w >> 24),
                   (uint8_t)(w >> 16),
                   (uint8_t)(w >> 8),
                   (uint8_t)w,
                   (uint8_t)(h >> 24),
                   (uint8_t)(h >> 16),
                   (uint8_t)(h >> 8),
                   (uint8_t)h,
                   depth,
                   type,
                   0,
                   0,
                   interlace};
  chunk(b, "IHDR", d, 13);
}
static buf zcomp(const uint8_t* raw, size_t n) {
  buf z = {0};
  uLongf cap = compressBound((uLong)n);
  z.d = malloc(cap);
  if (compress2(z.d, &cap, raw, (uLong)n, 9) != Z_OK) abort();
  z.n = cap;
  return z;
}
/* Gray+alpha 8-bit, non-interlaced, rows = 128, extra raw bytes appended. */
static uint8_t* raw_ga(size_t rows, size_t extra, uint8_t filter, uint8_t gray,
                       uint8_t alpha, size_t* n) {
  *n = rows * (1 + 256) + extra;
  uint8_t* r = calloc(1, *n);
  for (size_t y = 0; y < rows; y++) {
    r[y * 257] = filter;
    for (size_t x = 0; x < 128; x++) {
      r[y * 257 + 1 + x * 2] = gray;
      r[y * 257 + 2 + x * 2] = alpha;
    }
  }
  return r;
}
static buf png_simple(size_t rows, size_t extra, uint8_t filter) {
  size_t n;
  uint8_t* r = raw_ga(rows, extra, filter, 0, 255, &n);
  buf z = zcomp(r, n);
  free(r);
  buf b = {0};
  ihdr(&b, 128, 128, 8, 4, 0);
  chunk(&b, "IDAT", z.d, z.n);
  chunk(&b, "IEND", nullptr, 0);
  free(z.d);
  return b;
}

/* Encode all five PNG filters and Adam7 without using either platform codec. */
static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
  int v = a + b - c, da = abs(v - a), db = abs(v - b), dc = abs(v - c);
  return da <= db && da <= dc ? a : db <= dc ? b : c;
}
static void sample(size_t x, size_t y, uint8_t* gray, uint8_t* alpha) {
  if (y < 64) {
    *gray = x < 64 ? 128 : 80;
    *alpha = x < 64 ? 255 : 128;
  } else {
    *gray = x < 64 ? 200 : 0;
    *alpha = x < 64 ? 128 : 0;
  }
}
static buf pattern(unsigned filter, bool adam7, bool split) {
  static const size_t xs[] = {0, 4, 0, 2, 0, 1, 0},
                      ys[] = {0, 0, 4, 0, 2, 0, 1},
                      dx[] = {8, 8, 4, 4, 2, 2, 1},
                      dy[] = {8, 8, 8, 4, 4, 2, 2};
  buf raw = {0};
  for (size_t pass = 0; pass < (adam7 ? 7u : 1u); pass++) {
    uint8_t previous[256] = {0};
    for (size_t y = adam7 ? ys[pass] : 0; y < 128; y += adam7 ? dy[pass] : 1) {
      uint8_t row[256], encoded[257];
      size_t n = 0;
      for (size_t x = adam7 ? xs[pass] : 0; x < 128;
           x += adam7 ? dx[pass] : 1) {
        sample(x, y, row + n, row + n + 1);
        n += 2;
      }
      encoded[0] = (uint8_t)filter;
      for (size_t k = 0; k < n; k++) {
        uint8_t a = k < 2 ? 0 : row[k - 2], b = previous[k],
                c = k < 2 ? 0 : previous[k - 2];
        unsigned predictor = filter == 0   ? 0
                             : filter == 1 ? a
                             : filter == 2 ? b
                             : filter == 3 ? ((unsigned)a + b) / 2
                                           : paeth(a, b, c);
        encoded[k + 1] = (uint8_t)((unsigned)row[k] - predictor);
      }
      put(&raw, encoded, n + 1);
      memcpy(previous, row, n);
    }
  }
  buf z = zcomp(raw.d, raw.n), png = {0};
  free(raw.d);
  ihdr(&png, 128, 128, 8, 4, (uint8_t)adam7);
  for (size_t offset = 0; offset < z.n;) {
    size_t n = split && z.n - offset > 17 ? 17 : z.n - offset;
    chunk(&png, "IDAT", z.d + offset, n);
    offset += n;
  }
  chunk(&png, "IEND", nullptr, 0);
  free(z.d);
  return png;
}
static void parity_patterns(void) {
  for (unsigned variant = 0; variant < 7; variant++) {
    buf png = pattern(variant < 5 ? variant : 4, variant == 5, variant == 6);
    spd_tile t = {.png = {png.d, png.n}};
    spd_layer l = {.tiles = &t, .tile_count = 1};
    spd_drawing d = {.layers = &l, .layer_count = 1};
    for (unsigned transparent = 0; transparent < 2; transparent++) {
      snr_spd_options o = snr_spd_options_default();
      o.render.strict_images = true;
      o.render.has_background = true;
      o.render.background = (snr_color){1, 1, 1, transparent ? 0 : 1};
      snr_bitmap b = {0};
      CHECK(snr_spd_bitmap(&d, &o, &b, nullptr) == SNR_OK);
      CHECK(b.width == 128 && b.height == 128);
      for (size_t y = 0; y < 128; y++) {
        for (size_t x = 0; x < 128; x++) {
          /* Hand-computed premultiplied values: (128,255), (40,128),
           * (100,128), (0,0); white adds 255-alpha to each colour. */
          unsigned gray = y < 64 ? (x < 64 ? 128 : 40) : (x < 64 ? 100 : 0);
          unsigned alpha = y < 64 ? (x < 64 ? 255 : 128) : (x < 64 ? 128 : 0);
          if (!transparent) {
            gray += 255 - alpha;
            alpha = 255;
          }
          const uint8_t* px = b.pixels + y * b.stride + x * 4;
          CHECK(px[0] == gray && px[1] == gray && px[2] == gray &&
                px[3] == alpha);
        }
        CHECK(fwrite(b.pixels + y * b.stride, 1, 512, parity) == 512);
      }
      snr_bitmap_free(&b);
    }
    free(png.d);
  }
}

static size_t g_writes, g_fail_at;
static snr_status sink_write(void* c, const uint8_t* bytes, size_t size) {
  (void)c;
  (void)bytes;
  (void)size;
  if (g_writes++ == g_fail_at) return (snr_status)777;
  if (g_writes > g_fail_at + 1) {
    fprintf(stderr, "WRITE AFTER FAILURE\n");
    abort();
  }
  return SNR_OK;
}
static size_t g_cancel_left;
static bool cancel_cb(void* c) {
  (void)c;
  if (g_cancel_left == 0) return true;
  g_cancel_left--;
  return false;
}

static spd_drawing one_tile(spd_tile* t, spd_layer* l, const buf* png) {
  *t = (spd_tile){
      .tid = 0, .tx = 0, .ty = 0, .png = {.data = png->d, .size = png->n}};
  *l = (spd_layer){.id = 1, .tiles = t, .tile_count = 1};
  return (spd_drawing){.canvas = {.width = 128,
                                  .height = 128,
                                  .has_width = true,
                                  .has_height = true},
                       .layers = l,
                       .layer_count = 1};
}
static int failures;
#define EXPECT(name, got, want)                                     \
  do {                                                              \
    snr_status g_ = (got);                                          \
    printf("%-44s got %d want %d %s\n", name, (int)g_, (int)(want), \
           g_ == (want) ? "ok" : "MISMATCH");                       \
    if (g_ != (want)) failures++;                                   \
  } while (0)
static snr_status render(const spd_drawing* d, bool strict, uint8_t* first) {
  snr_spd_options o = snr_spd_options_default();
  o.render.strict_images = strict;
  snr_bitmap b = {0};
  snr_error e = {0};
  snr_status s = snr_spd_bitmap(d, &o, &b, &e);
  if (!s && first) {
    memcpy(first, b.pixels, 4);
    CHECK(b.width == 128 && b.height == 128);
    unsigned gray = first[0];
    CHECK(gray == 0 || gray == 255);
    for (size_t y = 0; y < b.height; y++) {
      for (size_t x = 0; x < b.width; x++) {
        const uint8_t* px = b.pixels + y * b.stride + 4 * x;
        CHECK(px[0] == gray && px[1] == gray && px[2] == gray && px[3] == 255);
      }
      CHECK(fwrite(b.pixels + y * b.stride, 1, 512, parity) == 512);
    }
  }
  snr_bitmap_free(&b);
  return s;
}
static void tile_case(const char* name, buf png, snr_status strict_want) {
  spd_tile t;
  spd_layer l;
  spd_drawing d = one_tile(&t, &l, &png);
  uint8_t px[4] = {0};
  char label[128];
  snprintf(label, sizeof label, "%s strict", name);
  EXPECT(label, render(&d, true, px), strict_want);
  if (!strict_want)
    printf("   pixel %u %u %u %u\n", px[0], px[1], px[2], px[3]);
  snprintf(label, sizeof label, "%s skip", name);
  snr_status s = render(&d, false, px);
  EXPECT(label, s, SNR_OK);
  if (!s) {
    CHECK(px[0] == (strict_want ? 255 : 0));
    printf("   pixel %u %u %u %u\n", px[0], px[1], px[2], px[3]);
  }
  free(png.d);
}
int main(int argc, char** argv) {
  CHECK(argc == 2);
  char path[4096];
  snprintf(path, sizeof path, "%s/parity.rgba", argv[1]);
  parity = fopen(path, "wb");
  CHECK(parity);
  parity_patterns();
  /* ---- hostile tiles ---- */
  tile_case("valid", png_simple(128, 0, 0), SNR_OK);
  tile_case("filter 5", png_simple(128, 0, 5), SNR_ERR_BACKEND);
  tile_case("short stream (127 rows)", png_simple(127, 0, 0), SNR_ERR_BACKEND);
  tile_case("extra 1 byte", png_simple(128, 1, 0), SNR_ERR_BACKEND);
  tile_case("bomb 64MiB zeros", png_simple(128, 64u << 20, 0), SNR_ERR_BACKEND);
  {
    /* trailing bytes after stream end within IDAT */
    size_t n;
    uint8_t* r = raw_ga(128, 0, 0, 0, 255, &n);
    buf z = zcomp(r, n);
    free(r);
    put(&z, "junk", 4);
    buf b = {0};
    ihdr(&b, 128, 128, 8, 4, 0);
    chunk(&b, "IDAT", z.d, z.n);
    chunk(&b, "IEND", nullptr, 0);
    free(z.d);
    tile_case("trailing in IDAT", b, SNR_ERR_BACKEND);
  }
  {
    /* second IDAT after stream end */
    size_t n;
    uint8_t* r = raw_ga(128, 0, 0, 0, 255, &n);
    buf z = zcomp(r, n);
    free(r);
    buf b = {0};
    ihdr(&b, 128, 128, 8, 4, 0);
    chunk(&b, "IDAT", z.d, z.n);
    chunk(&b, "IDAT", (const uint8_t*)"x", 1);
    chunk(&b, "IEND", nullptr, 0);
    free(z.d);
    tile_case("IDAT after stream end", b, SNR_ERR_BACKEND);
  }
  {
    /* non-consecutive IDAT: IDAT, tEXt, IDAT. PNG spec forbids. */
    size_t n;
    uint8_t* r = raw_ga(128, 0, 0, 0, 255, &n);
    buf z = zcomp(r, n);
    free(r);
    buf b = {0};
    ihdr(&b, 128, 128, 8, 4, 0);
    size_t half = z.n / 2;
    chunk(&b, "IDAT", z.d, half);
    chunk(&b, "tEXt", (const uint8_t*)"k\0v", 3);
    chunk(&b, "IDAT", z.d + half, z.n - half);
    chunk(&b, "IEND", nullptr, 0);
    free(z.d);
    tile_case("split IDAT with tEXt between", b, SNR_ERR_BACKEND);
  }
  {
    /* IDAT before IHDR-required PLTE: palette type without PLTE */
    uint8_t raw[128 * 129];
    memset(raw, 0, sizeof raw);
    buf z = zcomp(raw, sizeof raw);
    buf b = {0};
    ihdr(&b, 128, 128, 8, 3, 0);
    chunk(&b, "IDAT", z.d, z.n);
    chunk(&b, "IEND", nullptr, 0);
    free(z.d);
    tile_case("palette without PLTE", b, SNR_ERR_BACKEND);
  }
  {
    /* palette with 1 entry, indices 200 (out of range) */
    uint8_t raw[128 * 129];
    memset(raw, 200, sizeof raw);
    for (size_t y = 0; y < 128; y++) raw[y * 129] = 0;
    buf z = zcomp(raw, sizeof raw);
    buf b = {0};
    ihdr(&b, 128, 128, 8, 3, 0);
    chunk(&b, "PLTE", (const uint8_t*)"\x10\x20\x30", 3);
    chunk(&b, "IDAT", z.d, z.n);
    chunk(&b, "IEND", nullptr, 0);
    free(z.d);
    tile_case("palette index out of range", b, SNR_ERR_BACKEND);
  }
  {
    /* gAMA 1.0 on mid-gray: backends may differ (color management) */
    size_t n;
    uint8_t* r = raw_ga(128, 0, 0, 128, 255, &n);
    buf z = zcomp(r, n);
    free(r);
    buf b = {0};
    ihdr(&b, 128, 128, 8, 4, 0);
    uint8_t g[4] = {0, 1, 0x86, 0xa0}; /* 100000 = 1.0 */
    chunk(&b, "gAMA", g, 4);
    chunk(&b, "IDAT", z.d, z.n);
    chunk(&b, "IEND", nullptr, 0);
    free(z.d);
    tile_case("gAMA 1.0 mid gray", b, SNR_ERR_BACKEND);
  }
  {
    /* 16-bit RGBA, 1-bit gray */
    size_t n = 128 * (1 + 128 * 8);
    uint8_t* r = calloc(1, n);
    for (size_t y = 0; y < 128; y++)
      for (size_t x = 0; x < 128; x++) {
        r[y * (1 + 1024) + 1 + x * 8 + 6] = 0xff;
        r[y * (1 + 1024) + 1 + x * 8 + 7] = 0xff;
      }
    buf z = zcomp(r, n);
    free(r);
    buf b = {0};
    ihdr(&b, 128, 128, 16, 6, 0);
    chunk(&b, "IDAT", z.d, z.n);
    chunk(&b, "IEND", nullptr, 0);
    free(z.d);
    tile_case("rgba16 black", b, SNR_ERR_BACKEND);
  }
  {
    uint8_t raw[128 * 17];
    memset(raw, 0, sizeof raw);
    buf z = zcomp(raw, sizeof raw);
    buf b = {0};
    ihdr(&b, 128, 128, 1, 0, 0);
    chunk(&b, "IDAT", z.d, z.n);
    chunk(&b, "IEND", nullptr, 0);
    free(z.d);
    tile_case("gray1 black", b, SNR_ERR_BACKEND);
  }
  {
    buf b = {0};
    ihdr(&b, 128, 128, 8, 4, 0);
    chunk(&b, "IEND", nullptr, 0);
    tile_case("no IDAT", b, SNR_ERR_BACKEND);
  }
  {
    buf b = {0};
    ihdr(&b, 1, 16384, 8, 4, 0);
    chunk(&b, "IDAT", (const uint8_t*)"x", 1);
    chunk(&b, "IEND", nullptr, 0);
    tile_case("1x16384 dims", b, SNR_ERR_LIMIT);
  }
  {
    buf b = {0};
    ihdr(&b, 129, 1, 8, 4, 0);
    chunk(&b, "IDAT", (const uint8_t*)"x", 1);
    chunk(&b, "IEND", nullptr, 0);
    tile_case("129x1 dims", b, SNR_ERR_LIMIT);
  }
  {
    buf b = {0};
    ihdr(&b, 128, 128, 8, 4, 0);
    tile_case("no IEND", b, SNR_ERR_BACKEND);
  }
  {
    buf b = {0};
    ihdr(&b, 128, 128, 8, 4, 2); /* interlace 2 */
    chunk(&b, "IDAT", (const uint8_t*)"x", 1);
    chunk(&b, "IEND", nullptr, 0);
    tile_case("interlace method 2", b, SNR_ERR_BACKEND);
  }

  {
    size_t n;
    uint8_t* r = raw_ga(128, 0, 0, 128, 255, &n);
    buf z = zcomp(r, n);
    free(r);
    const char* tags[] = {"PLTE", "gAMA", "cHRM", "sRGB", "iCCP",
                          "sBIT", "tRNS", "tEXt", "zTXt", "iTXt",
                          "tIME", "acTL", "vpAg"};
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) {
      buf png = {0};
      ihdr(&png, 128, 128, 8, 4, 0);
      /* Includes deliberately hostile ICC/text payloads: never reach codecs. */
      chunk(&png, tags[i], (const uint8_t*)"hostile\0payload", 15);
      chunk(&png, "IDAT", z.d, z.n);
      chunk(&png, "IEND", nullptr, 0);
      tile_case(tags[i], png, SNR_ERR_BACKEND);
    }
    for (unsigned type = 0; type <= 6; type += 2) {
      if (type == 4) continue;
      buf png = {0};
      ihdr(&png, 128, 128, 8, (uint8_t)type, 0);
      chunk(&png, "IDAT", z.d, z.n);
      chunk(&png, "IEND", nullptr, 0);
      tile_case("non-GA8 format", png, SNR_ERR_BACKEND);
    }
    buf png = {0};
    ihdr(&png, 128, 128, 8, 4, 0);
    chunk(&png, "IDAT", z.d, z.n);
    tile_case("IDAT but missing IEND", png, SNR_ERR_BACKEND);
    free(z.d);
  }

  /* ---- geometry ---- */
  buf ok = png_simple(128, 0, 0);
  spd_tile t;
  spd_layer l;
  spd_drawing d = one_tile(&t, &l, &ok);
  struct {
    const char* name;
    uint64_t tx;
    uint32_t ty;
    double vx, vy, zoom;
    bool has_zoom;
    snr_spd_page page;
    snr_status want;
  } g[] = {
      {"tx UINT64_MAX", UINT64_MAX, 0, 0, 0, 1, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_LIMIT},
      {"tx INT64_MAX", (uint64_t)INT64_MAX, 0, 0, 0, 1, true,
       SNR_SPD_PAINTED_BOUNDS, SNR_ERR_LIMIT},
      {"tx 2^45 painted", UINT64_C(1) << 45, 0, 0, 0, 1, true,
       SNR_SPD_PAINTED_BOUNDS, SNR_OK},
      {"tx 2^45 vx .3 painted", UINT64_C(1) << 45, 0, .3, 0, 1, true,
       SNR_SPD_PAINTED_BOUNDS, SNR_OK},
      {"ty UINT32_MAX painted", 0, UINT32_MAX, 0, 0, 1, true,
       SNR_SPD_PAINTED_BOUNDS, SNR_OK},
      {"tx UINT64_MAX canvas", UINT64_MAX, 0, 0, 0, 1, true, SNR_SPD_CANVAS,
       SNR_ERR_LIMIT},
      {"vx NaN", 0, 0, NAN, 0, 1, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_INVALID_ARGUMENT},
      {"vy inf", 0, 0, 0, INFINITY, 1, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_INVALID_ARGUMENT},
      {"vx inf", 0, 0, INFINITY, 0, 1, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_INVALID_ARGUMENT},
      {"vy NaN", 0, 0, 0, NAN, 1, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_INVALID_ARGUMENT},
      {"zoom inf", 0, 0, 0, 0, INFINITY, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_INVALID_ARGUMENT},
      {"zoom zero", 0, 0, 0, 0, 0, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_INVALID_ARGUMENT},
      {"zoom -1", 0, 0, 0, 0, -1, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_INVALID_ARGUMENT},
      {"zoom NaN", 0, 0, 0, 0, NAN, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_INVALID_ARGUMENT},
      {"zoom denorm", 0, 0, 0, 0, DBL_TRUE_MIN, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_OK},
      {"zoom 1e-300 tx 9", 9, 0, 0, 0, 1e-300, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_OK},
      {"zoom 1e300", 0, 0, 0, 0, 1e300, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_LIMIT},
      {"zoom 1e-3 canvas", 0, 0, 0, 0, 1e-3, true, SNR_SPD_CANVAS, SNR_OK},
      {"vx -1e308", 0, 0, -1e308, 0, 1, true, SNR_SPD_PAINTED_BOUNDS,
       SNR_ERR_LIMIT},
      {"vx -1e308 zoom 1e-300", 0, 0, -1e308, 0, 1e-300, true,
       SNR_SPD_PAINTED_BOUNDS, SNR_ERR_LIMIT},
      {"vx 1e15 canvas", 0, 0, 1e15, 0, 1, true, SNR_SPD_CANVAS, SNR_OK},
  };
  for (size_t i = 0; i < sizeof g / sizeof *g; i++) {
    t.tx = g[i].tx;
    t.ty = g[i].ty;
    d.canvas.viewport_x = g[i].vx;
    d.canvas.viewport_y = g[i].vy;
    d.canvas.viewport_scale = g[i].zoom;
    d.canvas.has_viewport_x = d.canvas.has_viewport_y = true;
    d.canvas.has_viewport_scale = g[i].has_zoom;
    snr_spd_options o = snr_spd_options_default();
    o.page = g[i].page;
    o.render.strict_images = true;
    snr_bitmap b = {0};
    snr_error e = {0};
    snr_status s = snr_spd_bitmap(&d, &o, &b, &e);
    EXPECT(g[i].name, s, g[i].want);
    if (!s)
      printf("   %zux%zu px0 %u/%u\n", b.width, b.height, b.pixels[0],
             b.pixels[3]);
    snr_bitmap_free(&b);
  }
  /* two tiles far apart within limit, painted bounds width */
  {
    spd_tile ts[2] = {{.tx = 0, .png = {ok.d, ok.n}},
                      {.tx = 100, .png = {ok.d, ok.n}}};
    spd_layer ll = {.id = 1, .tiles = ts, .tile_count = 2};
    spd_drawing dd = {.layers = &ll, .layer_count = 1};
    snr_spd_options o = snr_spd_options_default();
    snr_bitmap b = {0};
    EXPECT("two tiles 12928 wide", snr_spd_bitmap(&dd, &o, &b, nullptr),
           SNR_OK);
    printf("   %zux%zu\n", b.width, b.height);
    snr_bitmap_free(&b);
    /* PDF of empty drawing */
    spd_drawing empty = {0};
    snr_buffer pdf = {0};
    EXPECT("empty drawing pdf", snr_spd_pdf(&empty, &o, &pdf, nullptr), SNR_OK);
    snprintf(path, sizeof path, "%s/result.empty.pdf", argv[1]);
    FILE* f = fopen(path, "wb");
    CHECK(f && fwrite(pdf.bytes, 1, pdf.size, f) == pdf.size);
    CHECK(!fclose(f));
    snr_buffer_free(&pdf);
    o.dpi = 1e-300;
    EXPECT("dpi 1e-300 below minimum scale",
           snr_spd_bitmap(&dd, &o, &b, nullptr), SNR_ERR_LIMIT);
    snr_bitmap_free(&b);
    o.dpi = 1e308;
    dd.canvas.ppi = 1e-300;
    dd.canvas.has_ppi = true;
    EXPECT("dpi/ppi overflow", snr_spd_bitmap(&dd, &o, &b, nullptr),
           SNR_ERR_LIMIT);
    o.dpi = NAN;
    EXPECT("dpi NaN", snr_spd_bitmap(&dd, &o, &b, nullptr),
           SNR_ERR_INVALID_ARGUMENT);
    /* scale shrink with fractional: 0.37 */
    o = snr_spd_options_default();
    o.render.scale = 0.37;
    dd.canvas.has_ppi = false;
    EXPECT("scale .37", snr_spd_bitmap(&dd, &o, &b, nullptr), SNR_OK);
    snr_bitmap_free(&b);
  }

  {
    /* Exact conservative work reservation for one GA8 tile and a 128-square
     * page: seven pixel passes, maximum Adam7 stream, three encoded passes,
     * one layer and one listed record. The failure must happen in planning. */
    spd_drawing dd = one_tile(&t, &l, &ok);
    snr_spd_options o = snr_spd_options_default();
    size_t required = 7 * 16384 + 33008 + 3 * ok.n + 2;
    o.render.limits.maximum_work = required - 1;
    snr_bitmap bm = {.pixels = (uint8_t*)1};
    snr_error e = {0};
    EXPECT("work reservation before allocation",
           snr_spd_bitmap(&dd, &o, &bm, &e), SNR_ERR_LIMIT);
    CHECK(bm.pixels == (uint8_t*)1 && e.index == 0);
    CHECK(!strcmp(e.message, "drawing blend work limit"));
    bm = (snr_bitmap){0};
    o.render.limits.maximum_work = required;
    EXPECT("exact work reservation", snr_spd_bitmap(&dd, &o, &bm, &e), SNR_OK);
    snr_bitmap_free(&bm);
    o.render.limits.maximum_document_work = required - 1;
    EXPECT("document work reservation", snr_spd_bitmap(&dd, &o, &bm, &e),
           SNR_ERR_LIMIT);
    const double invalid_ppi[] = {NAN, INFINITY, -1, 0};
    for (size_t i = 0; i < sizeof invalid_ppi / sizeof *invalid_ppi; i++) {
      o = snr_spd_options_default();
      dd.canvas.has_ppi = true;
      dd.canvas.ppi = invalid_ppi[i];
      EXPECT("invalid ppi", snr_spd_bitmap(&dd, &o, &bm, &e),
             SNR_ERR_INVALID_ARGUMENT);
    }
    dd.canvas.has_ppi = false;
    const double invalid_dpi[] = {NAN, INFINITY, -1};
    for (size_t i = 0; i < sizeof invalid_dpi / sizeof *invalid_dpi; i++) {
      o.dpi = invalid_dpi[i];
      EXPECT("invalid dpi", snr_spd_bitmap(&dd, &o, &bm, &e),
             SNR_ERR_INVALID_ARGUMENT);
    }
  }

  /* ---- cancellation sweep + sink failure sweep for all formats ---- */
  {
    spd_tile ts[3] = {{.tx = 0, .png = {ok.d, ok.n}},
                      {.tx = 1, .ty = 1, .png = {ok.d, ok.n}},
                      {.tx = 3, .png = {ok.d, ok.n}}};
    spd_layer ll = {.id = 1, .tiles = ts, .tile_count = 3};
    spd_drawing dd = {.layers = &ll, .layer_count = 1};
    size_t cancelled = 0, completed = 0;
    size_t last_cancel_index = SIZE_MAX;
    for (int fmt = 0; fmt < 4; fmt++)
      for (size_t k = 0; k < 4000; k++) {
        snr_spd_options o = snr_spd_options_default();
        o.render.cancel = cancel_cb;
        o.render.scale = 0.5;
        g_cancel_left = k;
        snr_buffer out = {0};
        snr_bitmap bm = {0};
        snr_error error = {0};
        snr_status s = fmt == 0   ? snr_spd_bitmap(&dd, &o, &bm, &error)
                       : fmt == 1 ? snr_spd_png(&dd, &o, &out, &error)
                       : fmt == 2 ? snr_spd_svg(&dd, &o, &out, &error)
                                  : snr_spd_pdf(&dd, &o, &out, &error);
        if (s == SNR_ERR_CANCELLED) {
          cancelled++;
          if (fmt == 0) last_cancel_index = error.index;
          if (out.bytes || bm.pixels) {
            printf("cancel left output\n");
            failures++;
          }
        } else if (s == SNR_OK) {
          completed++;
          if (fmt == 0) CHECK(last_cancel_index == 0);
          snr_buffer_free(&out);
          snr_bitmap_free(&bm);
          break;
        } else {
          printf("cancel sweep fmt %d k %zu status %d\n", fmt, k, (int)s);
          failures++;
          break;
        }
      }
    printf("cancel sweep: %zu cancelled, %zu completed (want 4)\n", cancelled,
           completed);
    if (completed != 4) failures++;
    for (int fmt = 0; fmt < 3; fmt++) {
      size_t n = 0;
      for (g_fail_at = 0; g_fail_at < 100000; g_fail_at++) {
        g_writes = 0;
        snr_output sink = {.abi_version = SNR_OUTPUT_ABI,
                           .struct_size = sizeof sink,
                           .write = sink_write};
        snr_spd_options o = snr_spd_options_default();
        snr_error e = {0};
        snr_status s = fmt == 0   ? snr_spd_png_to_output(&dd, &o, &sink, &e)
                       : fmt == 1 ? snr_spd_svg_to_output(&dd, &o, &sink, &e)
                                  : snr_spd_pdf_to_output(&dd, &o, &sink, &e);
        if (s == SNR_OK) break;
        n++;
        if (s != (snr_status)777 || g_writes != g_fail_at + 1) {
          printf("sink fmt %d fail_at %zu status %d writes %zu\n", fmt,
                 g_fail_at, (int)s, g_writes);
          failures++;
          break;
        }
      }
      printf("sink sweep fmt %d: %zu failure points ok\n", fmt, n);
    }
    /* work limit sweep */
    size_t lim_ok = 0;
    for (size_t w = 0; w < 2000000; w += 997) {
      snr_spd_options o = snr_spd_options_default();
      o.render.limits.maximum_work = w;
      snr_bitmap bm = {0};
      snr_status s = snr_spd_bitmap(&dd, &o, &bm, nullptr);
      if (s == SNR_OK) {
        lim_ok = w;
        snr_bitmap_free(&bm);
        break;
      }
      if (s != SNR_ERR_LIMIT || bm.pixels) {
        printf("work sweep %zu status %d\n", w, (int)s);
        failures++;
        break;
      }
    }
    CHECK(lim_ok > 0);
    printf("work sweep first ok at %zu\n", lim_ok);
  }
  free(ok.d);
  CHECK(!fclose(parity));
  printf("failures: %d\n", failures);
  return failures != 0;
}
