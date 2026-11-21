/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <stdatomic.h>

#include "snr_internal.h"
static atomic_flag log_lock = ATOMIC_FLAG_INIT;
static void lock_log(void) {
  while (atomic_flag_test_and_set_explicit(&log_lock, memory_order_acquire)) {
  }
}
static void unlock_log(void) {
  atomic_flag_clear_explicit(&log_lock, memory_order_release);
}
#define SNR_STRING_(x) #x
#define SNR_STRING(x) SNR_STRING_(x)
static snr_log_sink sink;
static void* sink_context;
const char* snr_version(void) {
  return SNR_STRING(SNRENDER_VERSION_MAJOR) "." SNR_STRING(
      SNRENDER_VERSION_MINOR) "." SNR_STRING(SNRENDER_VERSION_PATCH);
}
const char* snr_status_string(snr_status s) {
  static const char* const names[] = {
      "success",     "invalid argument", "out of memory", "limit exceeded",
      "unsupported", "backend failure",  "cancelled"};
  return (unsigned)s < sizeof names / sizeof *names ? names[s]
                                                    : "unknown status";
}
snr_status snr_fail(snr_error* e, snr_status s, size_t i, const char* text) {
  if (e && s != SNR_OK) {
    *e = (snr_error){.code = s, .index = i};
    snprintf(e->message, sizeof e->message, "%s", text);
  }
  return s;
}
snr_limits snr_limits_default(void) {
  return (snr_limits){16777216,        16777216,  10000,    1000000000,
                      100000000000ULL, 536870912, 1.0 / 64, 8};
}
snr_options snr_options_default(void) {
  return (snr_options){.abi_version = SNR_OPTIONS_ABI,
                       .struct_size = sizeof(snr_options),
                       .scale = 1,
                       .include_background = true,
                       .recognition = true,
                       .background = {1, 1, 1, 1},
                       .limits = snr_limits_default()};
}
void snr_buffer_free(snr_buffer* b) {
  if (b) {
    free(b->bytes);
    *b = (snr_buffer){0};
  }
}
void snr_bitmap_free(snr_bitmap* b) {
  if (b) {
    free(b->pixels);
    *b = (snr_bitmap){0};
  }
}
void snr_raster_free(snr_raster* b) {
  if (b) {
    free(b->gray);
    free(b->alpha);
    *b = (snr_raster){0};
  }
}
void snr_log_install(snr_log_sink s, void* c) {
  lock_log();
  sink = s;
  sink_context = c;
  unlock_log();
}
void snr_log_silence(void) { snr_log_install(nullptr, nullptr); }
void snr_log(snr_log_level level, const char* text) {
  lock_log();
  snr_log_sink callback = sink;
  void* context = sink_context;
  unlock_log();
  if (callback) callback(level, text, context);
}
bool snr_cancelled(const snr_options* o) {
  return o->cancel && o->cancel(o->cancel_context);
}
snr_status snr_options_read(const snr_options* i, snr_options* o,
                            snr_error* e) {
  *o = snr_options_default();
  if (i) {
    if (i->abi_version != SNR_OPTIONS_ABI || i->struct_size < sizeof *i)
      return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0, "invalid options ABI");
    *o = *i;
  }
  if (!isfinite(o->scale) || o->scale <= 0 ||
      !isfinite(o->limits.minimum_scale) ||
      !isfinite(o->limits.maximum_scale) || o->limits.minimum_scale <= 0 ||
      o->limits.maximum_scale < o->limits.minimum_scale)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0, "invalid scale");
  if (o->scale < o->limits.minimum_scale || o->scale > o->limits.maximum_scale)
    return snr_fail(e, SNR_ERR_LIMIT, 0, "scale limit");
  double c[] = {o->background.red, o->background.green, o->background.blue,
                o->background.alpha};
  for (size_t j = 0; j < 4; j++)
    if (!isfinite(c[j]) || c[j] < 0 || c[j] > 1)
      return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0, "invalid background");
  return SNR_OK;
}
snr_status snr_output_validate(const snr_output* o, snr_error* e) {
  if (!o || o->abi_version != SNR_OUTPUT_ABI || o->struct_size < sizeof *o ||
      !o->write)
    return snr_fail(e, SNR_ERR_INVALID_ARGUMENT, 0, "invalid output ABI");
  return SNR_OK;
}
#ifdef SNR_TESTING
size_t snr_test_growth_failure = SIZE_MAX;
/* Test builds count buffer growths down and fail the one that reaches zero,
 * so the allocation path is reachable without failing the system allocator.
 */
static bool growth_fails(void) {
  return snr_test_growth_failure != SIZE_MAX && !snr_test_growth_failure--;
}
#else
static bool growth_fails(void) { return false; }
#endif
/* The first failure is kept with its origin and every later write is
 * dropped, so a refusing callback is never called again. */
snr_status snr_write(snr_writer* w, const void* bytes, size_t n) {
  if (w->status != SNR_OK || !n) return w->status;
  if (n > w->limit - w->total) {
    w->origin = SNR_ORIGIN_LIMIT;
    return w->status = SNR_ERR_LIMIT;
  }
  if (w->output) {
    w->status = w->output->write(w->output->context, bytes, n);
    if (w->status != SNR_OK) w->origin = SNR_ORIGIN_CALLBACK;
  } else {
    if (w->total + n > w->capacity) {
      size_t capacity = w->capacity ? w->capacity : 4096;
      while (capacity < w->total + n) {
        if (capacity > w->limit / 2) {
          capacity = w->limit;
          break;
        }
        capacity *= 2;
      }
      if (capacity > w->limit) capacity = w->limit;
      uint8_t* p =
          growth_fails() ? nullptr : realloc(w->buffer.bytes, capacity);
      if (!p) {
        w->origin = SNR_ORIGIN_ALLOCATION;
        return w->status = SNR_ERR_OUT_OF_MEMORY;
      }
      w->buffer.bytes = p;
      w->capacity = capacity;
    }
    memcpy(w->buffer.bytes + w->total, bytes, n);
    w->buffer.size = w->total + n;
  }
  if (w->status == SNR_OK) w->total += n;
  return w->status;
}
/* Messages name the origin, not the status: a callback's status is returned
 * unchanged, whichever value it chose. */
const char* snr_writer_message(const snr_writer* w) {
  switch (w->origin) {
    case SNR_ORIGIN_LIMIT:
      return "output byte limit exceeded";
    case SNR_ORIGIN_CALLBACK:
      return "output callback failed";
    case SNR_ORIGIN_ALLOCATION:
      return "output allocation failed";
    default:
      return nullptr;
  }
}
static bool same(sn_string a, sn_string b) {
  return a.length && a.length == b.length && a.data && b.data &&
         !memcmp(a.data, b.data, a.length);
}
size_t snr_link_target(const sn_notebook* n, const sn_link* l, size_t first,
                       size_t count) {
  if (l->kind != 0 ||
      (l->target_file_id.length && !same(l->target_file_id, n->file_id)))
    return SIZE_MAX;
  for (size_t i = first; i < first + count; i++)
    if (same(l->target_page_id, n->pages[i].id) ||
        (!l->target_page_id.length && l->has_target_page_number &&
         l->target_page_number == n->pages[i].number))
      return i;
  return SIZE_MAX;
}
/* Recognition uses physical millimetres. The page's device overrides the
 * notebook's device when a sheet was moved between devices. */
double snr_page_dpi(const sn_notebook* n, const sn_page* p) {
  const sn_string* machine = sn_metadata_get(&p->metadata, "PAGEMACHINETYPE");
  if (machine && machine->data) {
    if ((machine->length == 1 && machine->data[0] == '3') ||
        (machine->length == 3 && !memcmp(machine->data, "A5X", 3)))
      return 226;
    return 300;
  }
  return n->equipment == SN_EQUIPMENT_A5X ? 226 : 300;
}

bool snr_uri_allowed(sn_string uri) {
  const char* schemes[] = {"http:", "https:", "mailto:"};
  for (size_t i = 0; i < 3; i++) {
    size_t length = strlen(schemes[i]);
    if (!uri.data || uri.length < length) continue;
    size_t j = 0;
    for (; j < length; j++) {
      unsigned char c = (unsigned char)uri.data[j];
      if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
      if (c != (unsigned char)schemes[i][j]) break;
    }
    if (j == length) return true;
  }
  snr_log(SNR_LOG_DEBUG, "URI scheme omitted");
  return false;
}
