/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Every entry point that writes through a sink must survive the sink failing
 * at any byte: an output limit reached part way, or a caller callback that
 * refuses a call or a byte. Each failure must return its status without
 * reporting partial output, the callback must not be called again after it
 * refused, and the next clean call must match a clean run byte for byte, so
 * no state survives the failure. Backends hand our writer to platform
 * encoders (Cairo PDF and PNG streams, CGDataConsumer), whose error paths are
 * exactly what this sweep exercises.
 *
 * The writer checks the limit once per write, so every limit inside one
 * write's byte range takes the same path as a limit at its first byte. A PDF
 * call costs about 20 ms under ASan and Cairo makes hundreds of small writes,
 * so by default the sweep fails sampled writes (see sampled), every value up
 * to 64 bytes, both sides of each 4 KiB boundary and the last bytes;
 * `exhaustive` as the first argument tries every byte value and every write
 * instead. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "source_pdf.h"

#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      fprintf(stderr, "%s:%d: %s (%s, limit or k %zu)\n", __FILE__, __LINE__, \
              #x, current_name, current_k);                                   \
      exit(1);                                                                \
    }                                                                         \
  } while (0)

static const char* current_name = "";
static size_t current_k;

/* The callback status is one the library never produces by itself here, so
 * returning it proves the callback's status passed through unchanged. */
constexpr snr_status REFUSED = SNR_ERR_UNSUPPORTED;
constexpr size_t NEVER = SIZE_MAX;

typedef struct sink {
  uint8_t* bytes;
  size_t size, capacity, calls, fail_call, fail_byte;
  size_t* ends; /* Cumulative size after each accepted write. */
  size_t ends_capacity;
  bool refused;
} sink;
static void* grow(void* data, size_t* capacity, size_t needed, size_t width) {
  if (needed <= *capacity) return data;
  size_t larger = needed * 2;
  void* grown = realloc(data, larger * width);
  CHECK(grown);
  *capacity = larger;
  return grown;
}
static snr_status collect(void* context, const uint8_t* data, size_t size) {
  sink* s = context;
  CHECK(data && size && !s->refused);
  s->calls++;
  if (s->calls == s->fail_call ||
      (s->fail_byte != NEVER && size > s->fail_byte - s->size)) {
    s->refused = true;
    return REFUSED;
  }
  s->ends = grow(s->ends, &s->ends_capacity, s->calls, sizeof *s->ends);
  s->bytes = grow(s->bytes, &s->capacity, s->size + size, 1);
  memcpy(s->bytes + s->size, data, size);
  s->size += size;
  s->ends[s->calls - 1] = s->size;
  return SNR_OK;
}
static sink fresh(void) {
  return (sink){.fail_call = NEVER, .fail_byte = NEVER};
}
static void reset(sink* s) {
  free(s->bytes);
  free(s->ends);
  *s = fresh();
}

typedef enum entry {
  PNG,
  SVG,
  PDF,
  DOCUMENT,
  PNG_OUT,
  SVG_OUT,
  PDF_OUT,
  DOCUMENT_OUT,
  ENTRIES
} entry;
static const char* const names[ENTRIES] = {
    "snr_page_png",           "snr_page_svg",
    "snr_page_pdf",           "snr_document_pdf",
    "snr_page_png_to_output", "snr_page_svg_to_output",
    "snr_page_pdf_to_output", "snr_document_pdf_to_output"};

/* Run one entry point. Buffer variants copy their result into the sink so
 * both kinds are compared the same way; a failing buffer variant must leave
 * its out-parameter empty. */
static snr_error last_error;
static snr_status run(const sn_notebook* n, entry which, const snr_options* o,
                      sink* s) {
  snr_buffer b = {0};
  snr_output output = {SNR_OUTPUT_ABI, sizeof output, collect, s};
  snr_error e = {0};
  snr_status status;
  switch (which) {
    case PNG:
      status = snr_page_png(n, 0, o, &b, &e);
      break;
    case SVG:
      status = snr_page_svg(n, 0, o, &b, &e);
      break;
    case PDF:
      status = snr_page_pdf(n, 0, o, &b, &e);
      break;
    case DOCUMENT:
      status = snr_document_pdf(n, 0, SIZE_MAX, o, &b, &e);
      break;
    case PNG_OUT:
      status = snr_page_png_to_output(n, 0, o, &output, &e);
      break;
    case SVG_OUT:
      status = snr_page_svg_to_output(n, 0, o, &output, &e);
      break;
    case PDF_OUT:
      status = snr_page_pdf_to_output(n, 0, o, &output, &e);
      break;
    default:
      status = snr_document_pdf_to_output(n, 0, SIZE_MAX, o, &output, &e);
      break;
  }
  CHECK(status == SNR_OK || e.code == status);
  last_error = e;
  if (which < PNG_OUT) {
    if (status) {
      CHECK(!b.bytes && !b.size);
    } else {
      CHECK(b.bytes && b.size);
      s->bytes = b.bytes;
      s->size = s->capacity = b.size;
    }
  }
  return status;
}

typedef struct values {
  size_t* items;
  size_t count, capacity;
} values;
static void add(values* v, size_t value, size_t full) {
  if (!value || value > full + 1) return;
  v->items = grow(v->items, &v->capacity, v->count + 1, sizeof *v->items);
  v->items[v->count++] = value;
}
static int ascending(const void* a, const void* b) {
  size_t x = *(const size_t*)a, y = *(const size_t*)b;
  return (x > y) - (x < y);
}
/* Writes chosen for failure: all of the first 256 and last 64, where headers,
 * fonts and the trailer are written, and every eighth one between, where
 * Cairo streams page content in runs of similar writes. A failure inside one
 * write's range takes the same path as a failure at its first byte. Under
 * AddressSanitizer a call costs ten to twenty times more, so the dense windows
 * shrink to the first 64 and last 16 writes and the stride grows to 32; the
 * byte values below 65, the 4 KiB boundaries and the last bytes stay. */
#if defined(__SANITIZE_ADDRESS__)
constexpr bool SANITIZED = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool SANITIZED = true;
#else
constexpr bool SANITIZED = false;
#endif
#else
constexpr bool SANITIZED = false;
#endif
static bool sampled(size_t write, size_t writes, bool exhaustive) {
  size_t head = SANITIZED ? 64 : 256, tail = SANITIZED ? 16 : 64,
         stride = SANITIZED ? 32 : 8;
  return exhaustive || write < head || write + tail >= writes ||
         write % stride == 0;
}
/* Limits or refused byte positions to try, sorted and without repeats. */
static values positions(const sink* clean, bool exhaustive) {
  values v = {0};
  size_t full = clean->size;
  for (size_t k = 1; k <= full + 1; k++)
    if (exhaustive || k <= 64 || k + 1 >= full) add(&v, k, full);
  for (size_t j = 0; !exhaustive && j < clean->calls; j++)
    if (sampled(j, clean->calls, false))
      add(&v, j ? clean->ends[j - 1] : 0, full);
  for (size_t b = 4096; !exhaustive && b <= full + 1; b += 4096) {
    add(&v, b - 1, full);
    add(&v, b, full);
    add(&v, b + 1, full);
  }
  qsort(v.items, v.count, sizeof *v.items, ascending);
  size_t kept = 0;
  for (size_t i = 0; i < v.count; i++)
    if (!kept || v.items[kept - 1] != v.items[i]) v.items[kept++] = v.items[i];
  v.count = kept;
  return v;
}

/* CoreGraphics stamps every PDF with the current time and a random file
 * identifier, both of fixed length; Cairo's are fixed. Everything else must
 * match byte for byte. */
static bool starts(const uint8_t* at, const uint8_t* end, const char* text) {
  size_t length = strlen(text);
  return (size_t)(end - at) >= length && !memcmp(at, text, length);
}
static bool same(const sink* a, const sink* b) {
  if (a->size != b->size) return false;
  const uint8_t *x = a->bytes, *y = b->bytes, *end = x + a->size;
  for (size_t i = 0; i < a->size; i++) {
    char close = starts(x + i, end, "(D:")     ? ')'
                 : starts(x + i, end, "/ID [") ? ']'
                                               : 0;
    if (close) {
      while (i < a->size && x[i] != close) i++;
      if (i == a->size || y[i] != close) return false;
    } else if (x[i] != y[i]) {
      return false;
    }
  }
  return true;
}

static size_t total_calls;
static size_t layer_bytes; /* Encoded size of the notebooks' only layer. */
static void expect_clean(const sn_notebook* n, entry which,
                         const snr_options* o, const sink* clean) {
  sink s = fresh();
  CHECK(run(n, which, o, &s) == SNR_OK && same(&s, clean));
  total_calls++;
  reset(&s);
}

/* Unused, with precedence, in the allocation build of this file. */
[[maybe_unused]] static void sweep(const sn_notebook* n, const char* label,
                                   const snr_options* base, bool exhaustive,
                                   bool pdf_only) {
  for (entry which = 0; which < ENTRIES; which++) {
    if (pdf_only && which != PDF && which != DOCUMENT && which != PDF_OUT &&
        which != DOCUMENT_OUT)
      continue;
    current_name = names[which];
    current_k = 0;
    /* The streaming twin of each entry reveals the write boundaries; buffer
     * variants pass the same writes to the same limit check. */
    entry twin = which < PNG_OUT ? which + PNG_OUT : which;
    sink clean = fresh(), streamed = fresh();
    clock_t started = clock();
    CHECK(run(n, which, base, &clean) == SNR_OK);
    double clean_ms = (double)(clock() - started) * 1000 / CLOCKS_PER_SEC;
    CHECK(run(n, twin, base, &streamed) == SNR_OK);
    CHECK(same(&streamed, &clean));
    size_t full = clean.size;
    values tried = positions(&streamed, exhaustive);
    started = clock();
    size_t before = total_calls;
    for (size_t i = 0; i < tried.count; i++) {
      size_t limit = tried.items[i];
      current_k = limit;
      snr_options o = *base;
      o.limits.maximum_output_bytes = limit;
      sink s = fresh();
      snr_status status = run(n, which, &o, &s);
      total_calls++;
      if (limit < full) {
        /* Below the layer's own encoded size the plan refuses the page
         * before anything is written. */
        CHECK(status == SNR_ERR_LIMIT &&
              !strcmp(last_error.message,
                      limit < layer_bytes ? "layer work or encoded size limit"
                                          : "output byte limit exceeded"));
      } else {
        CHECK(status == SNR_OK && same(&s, &clean));
      }
      reset(&s);
      if (status) expect_clean(n, which, base, &clean);
    }
    if (which >= PNG_OUT) {
      /* The callback refusing its k-th call, then its k-th byte. */
      for (size_t k = 1; k <= streamed.calls; k++) {
        if (!sampled(k - 1, streamed.calls, exhaustive)) continue;
        current_k = k;
        sink s = fresh();
        s.fail_call = k;
        CHECK(run(n, which, base, &s) == REFUSED && s.calls == k &&
              !strcmp(last_error.message, "output callback failed"));
        total_calls++;
        reset(&s);
        expect_clean(n, which, base, &clean);
      }
      for (size_t i = 0; i < tried.count; i++) {
        size_t k = tried.items[i];
        current_k = k;
        sink s = fresh();
        s.fail_byte = k - 1;
        CHECK(run(n, which, base, &s) == (k > full ? SNR_OK : REFUSED));
        CHECK(k > full ||
              !strcmp(last_error.message, "output callback failed"));
        total_calls++;
        reset(&s);
        expect_clean(n, which, base, &clean);
      }
    }
    printf(
        "%-11s %-27s %6zu bytes %4zu writes %5zu positions %6zu calls "
        "%7.1f ms clean %8.1f s\n",
        label, names[which], full, streamed.calls, tried.count,
        total_calls - before, clean_ms,
        (double)(clock() - started) / CLOCKS_PER_SEC);
    fflush(stdout);
    free(tried.items);
    reset(&clean);
    reset(&streamed);
  }
}

/* Precedence between the sink and the library's own failures. The first
 * failure is returned: a cancellation found before the sink failed wins even
 * when the callback then refuses the bytes that close the document, and a
 * refusal that came first wins over a cancellation requested afterwards. A
 * refusal's status is returned unchanged, whatever the value. */
typedef struct race {
  size_t polls, cancel_at, writes;
  bool cancelled, refused, refuse_after_cancel, refuse_first, cancel_on_refusal;
  snr_status refusal;
} race;
static bool race_cancel(void* context) {
  race* r = context;
  if (r->cancel_on_refusal) return r->cancelled = r->refused;
  if (r->polls++ >= r->cancel_at) r->cancelled = true;
  return r->cancelled;
}
static snr_status race_write(void* context, const uint8_t* data, size_t size) {
  race* r = context;
  CHECK(data && size && !r->refused);
  r->writes++;
  if (r->refuse_first || (r->refuse_after_cancel && r->cancelled)) {
    r->refused = true;
    return r->refusal;
  }
  return SNR_OK;
}
static snr_status race_run(const sn_notebook* n, entry which,
                           const snr_options* base, race* r) {
  snr_options o = *base;
  o.cancel = race_cancel;
  o.cancel_context = r;
  snr_output output = {SNR_OUTPUT_ABI, sizeof output, race_write, r};
  snr_error e = {0};
  snr_status status =
      which == PNG_OUT   ? snr_page_png_to_output(n, 0, &o, &output, &e)
      : which == SVG_OUT ? snr_page_svg_to_output(n, 0, &o, &output, &e)
      : which == PDF_OUT
          ? snr_page_pdf_to_output(n, 0, &o, &output, &e)
          : snr_document_pdf_to_output(n, 0, SIZE_MAX, &o, &output, &e);
  CHECK(status == SNR_OK || e.code == status);
  last_error = e;
  return status;
}
[[maybe_unused]] static void precedence(const sn_notebook* n,
                                        const snr_options* base) {
  static const snr_status refusals[] = {SNR_ERR_LIMIT, SNR_ERR_OUT_OF_MEMORY,
                                        SNR_ERR_CANCELLED, REFUSED,
                                        (snr_status)1000};
  for (entry which = PNG_OUT; which < ENTRIES; which++) {
    current_name = names[which];
    for (size_t i = 0; i < sizeof refusals / sizeof *refusals; i++) {
      current_k = i;
      race r = {
          .cancel_at = SIZE_MAX, .refuse_first = true, .refusal = refusals[i]};
      CHECK(race_run(n, which, base, &r) == refusals[i] && r.writes == 1 &&
            !strcmp(last_error.message, "output callback failed"));
      r = (race){
          .refuse_first = true, .cancel_on_refusal = true, .refusal = REFUSED};
      CHECK(race_run(n, which, base, &r) == REFUSED &&
            !strcmp(last_error.message, "output callback failed"));
    }
    race clean = {.cancel_at = SIZE_MAX};
    CHECK(race_run(n, which, base, &clean) == SNR_OK && clean.polls > 1);
    size_t points[] = {0, 1, clean.polls / 2, clean.polls - 1};
    for (size_t i = 0; i < sizeof points / sizeof *points; i++) {
      current_k = points[i];
      race r = {.cancel_at = points[i],
                .refuse_after_cancel = true,
                .refusal = REFUSED};
      CHECK(race_run(n, which, base, &r) == SNR_ERR_CANCELLED);
      CHECK(strcmp(last_error.message, "output callback failed"));
    }
    printf("precedence  %-27s %zu cancellation points, %zu statuses\n",
           names[which], sizeof points / sizeof *points,
           sizeof refusals / sizeof *refusals);
  }
}

#ifdef SNR_TESTING
/* Built as test_output_allocation against a library copy compiled with
 * SNR_TESTING, whose buffer growth fails on request: every growth of the
 * owned output buffer is failed in turn. */
extern size_t snr_test_growth_failure;
static void allocation(const sn_notebook* n, const char* label,
                       const snr_options* base) {
  for (entry which = PNG; which < PNG_OUT; which++) {
    current_name = names[which];
    sink clean = fresh();
    CHECK(run(n, which, base, &clean) == SNR_OK);
    size_t failures = 0;
    for (size_t k = 0;; k++) {
      current_k = k;
      snr_test_growth_failure = k;
      sink s = fresh();
      snr_status status = run(n, which, base, &s);
      snr_test_growth_failure = SIZE_MAX;
      reset(&s);
      if (!status) break;
      CHECK(status == SNR_ERR_OUT_OF_MEMORY &&
            !strcmp(last_error.message, "output allocation failed"));
      failures++;
      expect_clean(n, which, base, &clean);
    }
    CHECK(failures);
    printf("%-11s %-27s %zu buffer growths failed in turn\n", label,
           names[which], failures);
    reset(&clean);
  }
}
#endif

/* RATTA_RLE stripes give the encoders real content: one ink run per row. */
static uint8_t* stripes(size_t width, size_t height, size_t* size) {
  uint8_t* rle = malloc(height * 4);
  if (!rle) return nullptr;
  size_t used = 0;
  for (size_t y = 0; y < height; y++) {
    size_t ink = 8 + (y * 7) % (width - 16);
    rle[used++] = (uint8_t)(0x61 + y % 4);
    rle[used++] = (uint8_t)(ink - 1);
    rle[used++] = 0x62;
    rle[used++] = (uint8_t)(width - ink - 1);
  }
  *size = used;
  return rle;
}

/* Overlay writers use the same refusing sink as the notebook encoders. */
static void overlay_sweep(const sn_notebook* source) {
  sn_notebook mark = *source;
  mark.kind = SN_KIND_MARK;
  snr_overlay_options o = snr_overlay_options_default();
  for (unsigned kind = 0; kind < 2; kind++) {
    current_name = kind ? "mark PNG" : "mark PDF";
    sink clean = fresh();
    snr_error e = {0};
    snr_output output = {SNR_OUTPUT_ABI, sizeof output, collect, &clean};
    snr_status status =
        kind ? snr_mark_page_png_to_output(&mark, source_pdf, sizeof source_pdf,
                                           0, &o, &output, &e)
             : snr_mark_document_pdf_to_output(&mark, source_pdf,
                                               sizeof source_pdf, 0, SIZE_MAX,
                                               &o, &output, &e);
    if (status == SNR_ERR_UNSUPPORTED) {
      reset(&clean);
      continue;
    }
    CHECK(status == SNR_OK);
    for (size_t k = 1; k <= clean.calls; k++) {
      current_k = k;
      sink failed = fresh();
      failed.fail_call = k;
      output.context = &failed;
      status =
          kind ? snr_mark_page_png_to_output(
                     &mark, source_pdf, sizeof source_pdf, 0, &o, &output, &e)
               : snr_mark_document_pdf_to_output(&mark, source_pdf,
                                                 sizeof source_pdf, 0, SIZE_MAX,
                                                 &o, &output, &e);
      CHECK(status == REFUSED && failed.refused && failed.calls == k);
      CHECK(!strcmp(e.message, "output callback failed"));
      reset(&failed);
    }
#ifdef SNR_TESTING
    bool completed = false;
    for (size_t k = 0; k < 1000; k++) {
      snr_buffer b = {0};
      snr_test_growth_failure = k;
      status = kind
                   ? snr_mark_page_png(&mark, source_pdf, sizeof source_pdf, 0,
                                       &o, &b, &e)
                   : snr_mark_document_pdf(&mark, source_pdf, sizeof source_pdf,
                                           0, SIZE_MAX, &o, &b, &e);
      snr_test_growth_failure = SIZE_MAX;
      if (!status) {
        completed = true;
        snr_buffer_free(&b);
        break;
      }
      CHECK(status == SNR_ERR_OUT_OF_MEMORY && !b.bytes);
      CHECK(!strcmp(e.message, "output allocation failed"));
    }
    CHECK(completed);
#endif
    reset(&clean);
  }
}
int main(int argc, char** argv) {
  bool exhaustive = argc > 1 && !strcmp(argv[1], "exhaustive");
  constexpr size_t W = 120;
  constexpr size_t H = 160;
  size_t rle_size = 0;
  uint8_t* rle = stripes(W, H, &rle_size);
  layer_bytes = rle_size;
  if (!rle) return 1;
  sn_bitmap bitmap = {.encoding = SN_ENCODING_RATTA_RLE,
                      .bytes = {rle, rle_size}};
  sn_layer layer = {.name = SN_LAYER_MAIN,
                    .bitmap = &bitmap,
                    .is_stacked = true,
                    .is_visible = true};
  sn_word words[] = {{{"Sink", 4}, {1, 1, 4, 1.5, false}},
                     {{"\xd0\x9c\xd0\xb8\xd1\x80", 6}, {1, 4, 4, 1.5, false}},
                     {{"\xe4\xbd\xa0\xe5\xa5\xbd", 6}, {1, 7, 3, 1.5, false}}};
  sn_element element = {.kind = {"Text", 4}, .words = words, .word_count = 3};
  sn_recognition recognition = {.elements = &element, .element_count = 1};
  sn_page pages[3];
  for (size_t i = 0; i < 3; i++)
    pages[i] = (sn_page){.number = (int64_t)i + 1,
                         .size = {W, H},
                         .layers = &layer,
                         .layer_count = 1,
                         .recognition = i < 2 ? &recognition : nullptr};
  sn_link links[] = {{.page_number = 1,
                      .kind = 4,
                      .rect = {10, 10, 40, 10, false},
                      .url = {"https://example.com/", 20}},
                     {.page_number = 1,
                      .kind = 0,
                      .rect = {10, 30, 40, 10, false},
                      .target_page_number = 3,
                      .has_target_page_number = true},
                     {.page_number = 2,
                      .kind = 0,
                      .rect = {10, 50, 40, 10, false},
                      .target_page_number = 1,
                      .has_target_page_number = true}};
  sn_title titles[] = {{.page_number = 1, .level = 1},
                       {.page_number = 2, .level = 2},
                       {.page_number = 3, .level = 1}};
  sn_notebook small = {.palette = sn_palette_current(),
                       .equipment = SN_EQUIPMENT_NOMAD,
                       .pages = pages + 2,
                       .page_count = 1};
  sn_notebook multi = {.palette = sn_palette_current(),
                       .equipment = SN_EQUIPMENT_NOMAD,
                       .pages = pages,
                       .page_count = 3,
                       .links = links,
                       .link_count = 3,
                       .titles = titles,
                       .title_count = 3};
  snr_options o = snr_options_default();
#ifdef SNR_TESTING
  (void)exhaustive;
  allocation(&small, "one page", &o);
  allocation(&multi, "three pages", &o);
#else
  clock_t start = clock();
  sweep(&small, "one page", &o, exhaustive, false);
  /* PNG and SVG of the first page match the one-page case byte for byte:
   * recognition, links and outline exist only in PDF. */
  sweep(&multi, "three pages", &o, exhaustive, true);
  precedence(&multi, &o);
  printf("%zu calls in %.1f s%s\n", total_calls,
         (double)(clock() - start) / CLOCKS_PER_SEC,
         exhaustive ? " (every value)" : "");
#endif
  overlay_sweep(&multi);
  free(rle);
  return 0;
}
