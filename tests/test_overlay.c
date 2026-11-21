/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                      \
  do {                                                \
    if (!(x)) {                                       \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
      exit(1);                                        \
    }                                                 \
  } while (0)
static snr_buffer read_file(const char* name) {
  FILE* f = fopen(name, "rb");
  CHECK(f);
  CHECK(!fseek(f, 0, SEEK_END));
  long length = ftell(f);
  CHECK(length >= 0);
  rewind(f);
  snr_buffer b = {malloc((size_t)length + 1), (size_t)length};
  CHECK(b.bytes);
  CHECK(fread(b.bytes, 1, b.size, f) == b.size);
  CHECK(!fclose(f));
  return b;
}
static bool cancel(void* context) {
  (void)context;
  return true;
}
typedef struct {
  size_t completed, total, calls;
} progress_record;
static void progress(void* context, size_t completed, size_t total) {
  progress_record* record = context;
  CHECK(completed == record->calls);
  CHECK(!record->calls || total == record->total);
  record->completed = completed;
  record->total = total;
  record->calls++;
}
int main(int argc, char** argv) {
  CHECK(argc >= 4 && argc <= 6);
  snr_buffer mark = read_file(argv[1]), pdf = read_file(argv[2]), out = {0};
  sn_notebook* n = nullptr;
  CHECK(sn_parse_notebook(mark.bytes, mark.size, &n, nullptr) == SN_OK);
  sn_notebook expanded = *n;
  sn_page* copies = nullptr;
  if (argc >= 5 && strcmp(argv[4], "strict") && strcmp(argv[4], "epub")) {
    size_t count = (size_t)strtoul(argv[4], nullptr, 10);
    size_t step = argc == 6 ? (size_t)strtoul(argv[5], nullptr, 10) : 1;
    CHECK(count && count <= 300 && n->page_count);
    copies = calloc(count, sizeof *copies);
    CHECK(copies);
    for (size_t i = 0; i < count; i++) {
      copies[i] = n->pages[0];
      copies[i].number = (int64_t)(i * step + 1);
    }
    expanded.pages = copies;
    expanded.page_count = count;
  }
  snr_overlay_options o = snr_overlay_options_default();
  if (argc == 5 && !strcmp(argv[4], "strict")) o.strict = true;
  if (argc == 5 && !strcmp(argv[4], "epub")) o.source_kind = SNR_DOCUMENT_EPUB;
  progress_record record = {0};
  o.render.progress = progress;
  o.render.progress_context = &record;
  snr_error e = {0};
  snr_status s = snr_mark_document_pdf(&expanded, pdf.bytes, pdf.size, 0,
                                       SIZE_MAX, &o, &out, &e);
  if (s) {
    fprintf(stderr, "%u: %s\n", (unsigned)s, e.message);
    free(copies);
    sn_notebook_free(n);
    snr_buffer_free(&mark);
    snr_buffer_free(&pdf);
    return 2;
  }
  CHECK(record.calls == record.total + 1 && record.completed == record.total);
  FILE* f = fopen(argv[3], "wb");
  CHECK(f);
  CHECK(fwrite(out.bytes, 1, out.size, f) == out.size);
  CHECK(!fclose(f));
  snr_buffer_free(&out);
  free(copies);
  if (argc >= 5) {
    sn_notebook_free(n);
    snr_buffer_free(&mark);
    snr_buffer_free(&pdf);
    return 0;
  }
  o.source_kind = SNR_DOCUMENT_EPUB;
  CHECK(snr_mark_document_pdf(n, pdf.bytes, pdf.size, 0, SIZE_MAX, &o, &out,
                              &e) == SNR_ERR_UNSUPPORTED);
  o = snr_overlay_options_default();
  o.render.cancel = cancel;
  CHECK(snr_mark_document_pdf(n, pdf.bytes, pdf.size, 0, SIZE_MAX, &o, &out,
                              &e) == SNR_ERR_CANCELLED);
  o = snr_overlay_options_default();
  o.render.limits.maximum_pages = 0;
  CHECK(snr_mark_document_pdf(n, pdf.bytes, pdf.size, 0, SIZE_MAX, &o, &out,
                              &e) == SNR_ERR_LIMIT);
  o = snr_overlay_options_default();
  o.render.limits.maximum_pixels = 1;
  CHECK(snr_mark_page_png(n, pdf.bytes, pdf.size, 0, &o, &out, &e) ==
        SNR_ERR_LIMIT);
  CHECK(!out.bytes);
  bool has = false;
  CHECK(snr_mark_page_has_annotations(n, 0, &has, &e) == SNR_OK);
  for (size_t i = 0; i < n->highlight_count; i++) {
    const sn_highlight* h = &n->highlights[i];
    for (size_t j = 0; j < h->document_rect_count; j++) {
      sn_rect r = h->document_rects[j];
      if (h->kind == 0)
        printf("%lld %.9g %.9g %.9g %.9g\n", (long long)h->page_number, r.x,
               r.y, r.width, r.height);
    }
  }
  sn_notebook_free(n);
  snr_buffer_free(&mark);
  snr_buffer_free(&pdf);
  return 0;
}
