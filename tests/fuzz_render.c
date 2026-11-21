/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "fuzz_render.h"

#include <math.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "source_pdf.h"
#ifdef SNR_FUZZ_SPD
#include <snspd/snspd.h>
#endif

/* A violated contract must stop the run in every configuration, including
 * Release builds of the replay test where assert would compile away. */
#define FUZZ_CHECK(x)                                                   \
  do {                                                                  \
    if (!(x)) {                                                         \
      fprintf(stderr, "%s:%d: contract: %s\n", __FILE__, __LINE__, #x); \
      abort();                                                          \
    }                                                                   \
  } while (0)

constexpr uint32_t NEVER = UINT32_MAX;
constexpr snr_status UNTOUCHED = (snr_status)0x7fffffffu;

typedef enum profile { STANDARD, TIGHT_WORK, TIGHT_PIXELS } profile;
typedef struct choice {
  uint32_t operation;
  size_t page;
  double scale;
  bool recognition, strict_images, title;
  profile limits;
  size_t count;
  uint32_t background, cancel_after, fail_after;
  size_t stop_at;
} choice;

/* A sink can fail at any byte, and which encoder stream is open when it does
 * decides the cleanup path, so the byte count is drawn log-uniformly from 1 to
 * 65,536: exact small values, headers and every larger band are equally
 * likely. No selector bits remain, so the count is mixed from all of them. */
static size_t byte_count(uint32_t selector) {
  uint32_t h = selector * 2654435761u;
  h ^= h >> 15;
  uint32_t band = h % 17, r = h / 17;
  if (!band) return 1;
  size_t low = (size_t)1 << (band - 1);
  return low + r % (low + 1);
}

/* Mixed-radix decoding keeps each field independent of the others, so a
 * campaign meets every combination at the rate of its field sizes. Variants
 * that end a call early are kept rare: they are cheap, but a campaign spent
 * mostly on them would seldom compose a page. Scales stay at or below 1/8:
 * every render composes the full device sheet whatever the output size, so
 * larger scales would add encoding cost, not paths. */
static choice decode(uint32_t selector) {
  static const double scales[] = {1.0 / 64, 1.0 / 24, 1.0 / 16, 1.0 / 8};
  static const profile profiles[] = {STANDARD, STANDARD, TIGHT_WORK,
                                     TIGHT_PIXELS};
  /* Zero is the one invalid range; larger counts are clamped to the pages
   * that remain, so short notebooks do not turn most ranges into errors. */
  static const size_t counts[] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, 1, 1, 2, 3, 0};
  static const uint32_t cancels[] = {NEVER, NEVER, NEVER, NEVER,
                                     NEVER, NEVER, 0,     2000};
  choice c = {.operation = selector % FUZZ_OPERATIONS,
              .stop_at = byte_count(selector)};
  selector /= FUZZ_OPERATIONS;
  c.page = selector % 256;
  selector /= 256;
  c.scale = scales[selector & 3];
  selector >>= 2;
  c.recognition = !(selector & 1);
  selector >>= 1;
  c.strict_images = selector & 1;
  selector >>= 1;
  c.limits = profiles[selector & 3];
  selector >>= 2;
  c.count = counts[selector & 7];
  selector >>= 3;
  c.background = selector & 3;
  selector >>= 2;
  c.cancel_after = cancels[selector & 7];
  selector >>= 3;
  /* The callback fails on its first write or after the drawn byte count. */
  c.fail_after = (selector & 7) < 6    ? NEVER
                 : (selector & 7) == 6 ? 0
                                       : (uint32_t)c.stop_at;
  selector >>= 3;
  c.title = selector & 1;
  return c;
}

/* The standard limits bound memory to a few tens of megabytes per call while
 * admitting every device sheet at every chosen scale, with up to five visible
 * layers and the largest fixture layer. Four pages per document keep the
 * dearest execution near four page compositions. The tight profiles fail at
 * different stages: work and output limits after planning or part way through
 * encoding, pixel limits while planning, so each check and its cleanup is
 * fuzzed. The tight output limit is the drawn byte count. */
static snr_limits limits(profile p, size_t stop_at) {
  snr_limits l = snr_limits_default();
  l.maximum_pixels = p == TIGHT_PIXELS ? 1024 : 81920;
  l.maximum_decoded_pixels = p == TIGHT_PIXELS ? 3000000 : 5000000;
  l.maximum_pages = p == STANDARD ? 4 : 1;
  l.maximum_work = p == TIGHT_WORK ? 8000000 : 60000000;
  l.maximum_document_work = p == TIGHT_WORK ? 10000000 : 240000000;
  l.maximum_output_bytes = p == TIGHT_WORK ? stop_at : 4194304;
  return l;
}

/* The refusing callback returns a value outside the library's statuses, so a
 * result equal to it can only be the callback's status passed through; every
 * listed status, SNR_ERR_UNSUPPORTED included, is produced by the library on
 * some fuzzed input. */
constexpr snr_status REFUSED = (snr_status)0x52454655u;
typedef struct sink {
  size_t written, fail_after;
  bool failed;
} sink;
static snr_status write_bytes(void* context, const uint8_t* bytes,
                              size_t size) {
  sink* s = context;
  FUZZ_CHECK(bytes && size && !s->failed);
  if (s->fail_after != NEVER && size > s->fail_after - s->written) {
    s->failed = true;
    return REFUSED;
  }
  s->written += size;
  return SNR_OK;
}
typedef struct counter {
  uint32_t calls, limit;
} counter;
static bool cancel(void* context) {
  counter* c = context;
  return c->limit != NEVER && c->calls++ >= c->limit;
}
typedef struct progress_state {
  size_t next, total;
} progress_state;
/* PDF progress reports zero first, then each completed page in order. */
static void progress(void* context, size_t completed, size_t total) {
  progress_state* p = context;
  FUZZ_CHECK(completed == p->next && completed <= total);
  FUZZ_CHECK(!completed || total == p->total);
  p->total = total;
  p->next++;
}
static void log_sink(snr_log_level level, const char* message, void* context) {
  (void)context;
  FUZZ_CHECK(level == SNR_LOG_DEBUG || level == SNR_LOG_ERROR ||
             level == SNR_LOG_WARNING);
  FUZZ_CHECK(message && strlen(message) < 4096);
}
static bool zero(const void* value, size_t size) {
  const uint8_t* bytes = value;
  for (size_t i = 0; i < size; i++)
    if (bytes[i]) return false;
  return true;
}
static void check_buffer(snr_status s, const snr_buffer* b,
                         const snr_options* o, const char* prefix) {
  if (s) {
    FUZZ_CHECK(zero(b, sizeof *b));
    return;
  }
  FUZZ_CHECK(b->bytes && b->size >= strlen(prefix));
  FUZZ_CHECK(b->size <= o->limits.maximum_output_bytes);
  FUZZ_CHECK(!memcmp(b->bytes, prefix, strlen(prefix)));
}

uint32_t fuzz_selector(const uint8_t* data, size_t size) {
  uint64_t h = 14695981039346656037u;
  for (size_t i = 0; i < size; i++) h = (h ^ data[i]) * 1099511628211u;
  return (uint32_t)(h ^ (h >> 32));
}

/* The fuzzed notebook as a mark over the fixed source PDF. The source bytes
 * themselves are mutated by the separate fuzz_pdf target. */
static void overlay(const sn_notebook* n, uint32_t selector) {
  choice c = decode(selector);
  uint32_t operation = selector % FUZZ_OVERLAY_OPERATIONS;
  counter cancelled = {0, c.cancel_after};
  progress_state reported = {0};
  snr_overlay_options o = snr_overlay_options_default();
  o.render.scale = c.scale;
  o.render.recognition = c.recognition;
  o.render.strict_images = c.strict_images;
  o.render.limits = limits(c.limits, c.stop_at);
  o.render.cancel = cancel;
  o.render.cancel_context = &cancelled;
  o.render.progress = progress;
  o.render.progress_context = &reported;
  o.strict = c.title;
  snr_overlay_result result = {.abi_version = SNR_OVERLAY_RESULT_ABI,
                               .struct_size = sizeof result};
  o.result = &result;
  sink stream = {.fail_after = c.fail_after};
  snr_output output = {SNR_OUTPUT_ABI, sizeof output, write_bytes, &stream};
  snr_error e = {.code = UNTOUCHED};
  snr_bitmap bitmap = {0};
  snr_buffer buffer = {0};
  bool streamed = false, annotated = false;
  const uint8_t* pdf = source_pdf;
  size_t size = sizeof source_pdf;
  size_t count = c.count == SIZE_MAX ? SIZE_MAX : 1;
  snr_status s;
  switch (operation) {
    case 0:
      s = snr_mark_document_pdf(n, pdf, size, 0, count, &o, &buffer, &e);
      check_buffer(s, &buffer, &o.render, "%PDF-");
      break;
    case 1:
      s = snr_mark_document_pdf_to_output(n, pdf, size, 0, count, &o, &output,
                                          &e);
      streamed = true;
      break;
    case 2:
      s = snr_mark_page_bitmap(n, pdf, size, 0, &o, &bitmap, &e);
      break;
    case 3:
      s = snr_mark_page_png(n, pdf, size, 0, &o, &buffer, &e);
      check_buffer(s, &buffer, &o.render, "\x89PNG");
      break;
    case 4:
      s = snr_mark_page_png_to_output(n, pdf, size, 0, &o, &output, &e);
      streamed = true;
      break;
    default:
      s = snr_mark_page_has_annotations(n, c.page, &annotated, &e);
      break;
  }
  FUZZ_CHECK((s <= SNR_ERR_CANCELLED || (streamed && s == REFUSED)) &&
             snr_status_string(s));
  if (s) {
    FUZZ_CHECK(e.code == s && memchr(e.message, 0, sizeof e.message));
  } else {
    FUZZ_CHECK(e.code == UNTOUCHED);
  }
  if (streamed) {
    FUZZ_CHECK(s || (!stream.failed && stream.written));
    FUZZ_CHECK(!stream.failed || s == REFUSED ||
               (s != SNR_OK && s <= SNR_ERR_CANCELLED));
    FUZZ_CHECK(s != REFUSED ||
               (stream.failed && !strcmp(e.message, "output callback failed")));
  }
  if (operation == 2)
    FUZZ_CHECK(s ? zero(&bitmap, sizeof bitmap)
                 : bitmap.pixels &&
                       bitmap.width == (size_t)ceil(120 * c.scale) &&
                       bitmap.height == (size_t)ceil(160 * c.scale));
  /* The result keeps its ABI and reports residuals as non-negative. */
  FUZZ_CHECK(result.abi_version == SNR_OVERLAY_RESULT_ABI &&
             !(result.worst_paired_residual < 0) &&
             result.annotated_pages_drawn <= 1);
  snr_bitmap_free(&bitmap);
  snr_buffer_free(&buffer);
}

#ifdef SNR_FUZZ_SPD
/* SQLite inputs share the notebook harness and its scheduled campaign. The
 * parser owns its independent byte/memory/VM limits; rendering owns pixel,
 * tile, decoded-byte and work ceilings. No real drawing seeds are used. */
static int fuzz_drawing(const uint8_t* data, size_t size, uint32_t selector) {
  spd_options parse = spd_options_default();
  parse.limits.max_file_size = 2097152;
  parse.limits.max_memory_bytes = 33554432;
  parse.limits.max_work = 16777216;
  parse.limits.max_tiles_per_layer = 2048;
  spd_drawing* d = nullptr;
  if (spd_open(data, size, &parse, &d, nullptr) != SPD_OK) return 0;
  snr_spd_options o = snr_spd_options_default();
  o.render.scale = .125;
  o.render.limits.maximum_pixels = 1048576;
  o.render.limits.maximum_decoded_pixels = 1048576;
  o.render.limits.maximum_work = 16777216;
  o.render.limits.maximum_output_bytes = 4194304;
  o.maximum_tiles = 2048;
  o.maximum_decoded_bytes = 33554432;
  o.render.strict_images = (selector & 8) != 0;
  o.include_reference_layer = (selector & 16) != 0;
  o.page = selector & 32 ? SNR_SPD_PAINTED_BOUNDS : SNR_SPD_CANVAS;
  snr_buffer b = {0};
  snr_bitmap bitmap = {0};
  snr_error e = {.code = UNTOUCHED};
  snr_status status;
  switch (selector % 4) {
    case 0:
      status = snr_spd_png(d, &o, &b, &e);
      break;
    case 1:
      status = snr_spd_svg(d, &o, &b, &e);
      break;
    case 2:
      status = snr_spd_pdf(d, &o, &b, &e);
      break;
    default:
      status = snr_spd_bitmap(d, &o, &bitmap, &e);
      break;
  }
  FUZZ_CHECK(status <= SNR_ERR_CANCELLED);
  FUZZ_CHECK(status ? e.code == status && !b.bytes && !bitmap.pixels
                    : e.code == UNTOUCHED);
  snr_buffer_free(&b);
  snr_bitmap_free(&bitmap);
  spd_drawing_free(d);
  return 0;
}
#endif

int fuzz_render(const uint8_t* data, size_t size, uint32_t selector) {
#ifdef SNR_FUZZ_SPD
  if (size >= 16 && !memcmp(data, "SQLite format 3", 15))
    return fuzz_drawing(data, size, selector);
#endif
  if (size > FUZZ_MAXIMUM_INPUT) return 0;
  FUZZ_CHECK(snr_version());
  snr_log_install(log_sink, nullptr);
  sn_notebook* n = nullptr;
  if (sn_parse_notebook(data, size, &n, nullptr) != SN_OK) return 0;
  choice c = decode(selector);
  /* A notebook without readable pages still reaches argument validation. */
  size_t page = n->page_count ? c.page % n->page_count : 0;
  counter cancelled = {0, c.cancel_after};
  progress_state reported = {0};
  snr_options o = snr_options_default();
  o.scale = c.scale;
  o.recognition = c.recognition;
  o.strict_images = c.strict_images;
  o.limits = limits(c.limits, c.stop_at);
  o.include_background = c.background != 1;
  o.has_background = c.background >= 2;
  o.background = c.background == 2 ? (snr_color){0.25, 0.5, 0.75, 1}
                                   : (snr_color){1, 0, 0, 0.5};
  o.cancel = cancel;
  o.cancel_context = &cancelled;
  o.progress = progress;
  o.progress_context = &reported;
  if (c.title) o.title = "Info (BT) \\ <FFFF> % /Type /Page \xc3\xa9";
  sink stream = {.fail_after = c.fail_after};
  snr_output output = {SNR_OUTPUT_ABI, sizeof output, write_bytes, &stream};
  snr_error e = {.code = UNTOUCHED};
  snr_status s;
  snr_bitmap bitmap = {0};
  snr_raster raster = {0};
  snr_buffer buffer = {0};
  bool streamed = false;
  size_t count = c.count;
  if (count != SIZE_MAX && page < n->page_count && count > n->page_count - page)
    count = n->page_count - page;
  switch (c.operation) {
    case 0:
      s = snr_page_bitmap(n, page, &o, &bitmap, &e);
      break;
    case 1:
      s = snr_page_raster(n, page, &o, &raster, &e);
      break;
    case 2:
      s = snr_page_png(n, page, &o, &buffer, &e);
      check_buffer(s, &buffer, &o, "\x89PNG");
      break;
    case 3:
      s = snr_page_svg(n, page, &o, &buffer, &e);
      check_buffer(s, &buffer, &o, "<?xml");
      break;
    case 4:
      s = snr_page_pdf(n, page, &o, &buffer, &e);
      check_buffer(s, &buffer, &o, "%PDF-");
      break;
    case 5:
      s = snr_page_png_to_output(n, page, &o, &output, &e);
      streamed = true;
      break;
    case 6:
      s = snr_page_svg_to_output(n, page, &o, &output, &e);
      streamed = true;
      break;
    case 7:
      s = snr_page_pdf_to_output(n, page, &o, &output, &e);
      streamed = true;
      break;
    case 8:
      s = snr_document_pdf(n, page, count, &o, &buffer, &e);
      check_buffer(s, &buffer, &o, "%PDF-");
      break;
    case 9:
      s = snr_document_pdf_to_output(n, page, count, &o, &output, &e);
      streamed = true;
      break;
    default:
      abort();
  }
  FUZZ_CHECK((s <= SNR_ERR_CANCELLED || (streamed && s == REFUSED)) &&
             snr_status_string(s));
  if (s) {
    FUZZ_CHECK(e.code == s && memchr(e.message, 0, sizeof e.message));
  } else {
    FUZZ_CHECK(e.code == UNTOUCHED);
  }
  if (streamed) {
    FUZZ_CHECK(s || (!stream.failed && stream.written));
    /* A refused write is returned unchanged unless the library had already
     * failed by itself: the bytes that close a document are still offered
     * after such a failure, and the callback may refuse them too. Only the
     * callback produces REFUSED, so it cannot appear without a refusal. */
    FUZZ_CHECK(!stream.failed || s == REFUSED ||
               (s != SNR_OK && s <= SNR_ERR_CANCELLED));
    FUZZ_CHECK(s != REFUSED ||
               (stream.failed && !strcmp(e.message, "output callback failed")));
  }
  /* Success implies a valid page, so its size predicts the output sides. */
  size_t width = 0, height = 0;
  if (!s && c.operation < 2) {
    width = (size_t)ceil(n->pages[page].size.width * c.scale);
    height = (size_t)ceil(n->pages[page].size.height * c.scale);
  }
  if (c.operation == 0)
    FUZZ_CHECK(s ? zero(&bitmap, sizeof bitmap)
                 : bitmap.pixels && bitmap.width == width &&
                       bitmap.height == height && bitmap.stride == width * 4);
  if (c.operation == 1)
    FUZZ_CHECK(s ? zero(&raster, sizeof raster)
                 : raster.gray && raster.alpha && raster.width == width &&
                       raster.height == height);
  snr_bitmap_free(&bitmap);
  snr_raster_free(&raster);
  snr_buffer_free(&buffer);
  FUZZ_CHECK(!bitmap.pixels && !raster.gray && !buffer.bytes);
  /* A mark also goes through one overlay operation, after and independently
   * of the operation above, so every input keeps that operation's share. */
  if (n->kind == SN_KIND_MARK) overlay(n, selector & ~FUZZ_OVERLAY_SELECTOR);
  snr_log_silence();
  sn_notebook_free(n);
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  return fuzz_render(data, size, fuzz_selector(data, size));
}
