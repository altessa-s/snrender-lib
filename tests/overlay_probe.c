/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static snr_buffer read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) exit(2);
  if (fseek(f, 0, SEEK_END)) exit(2);
  long length = ftell(f);
  if (length <= 0) exit(2);
  rewind(f);
  snr_buffer b = {malloc((size_t)length), (size_t)length};
  if (!b.bytes || fread(b.bytes, 1, b.size, f) != b.size) exit(2);
  fclose(f);
  return b;
}
int main(int argc, char** argv) {
  if (argc != 7) return 2;
  snr_buffer pdf = read_file(argv[2]), mark = {0}, out = {0};
  sn_notebook blank = {.kind = SN_KIND_MARK, .sheet = {1404, 1872}};
  sn_notebook* parsed = nullptr;
  if (strcmp(argv[1], "empty")) {
    mark = read_file(argv[1]);
    if (sn_parse_notebook(mark.bytes, mark.size, &parsed, nullptr)) return 2;
    blank = *parsed;
  }
  if (!strcmp(argv[6], "ink")) {
    blank.highlights = nullptr;
    blank.highlight_count = 0;
  }
  snr_overlay_options o = snr_overlay_options_default();
  o.render.limits.maximum_pixels = 67108864;
  o.render.scale = strtod(argv[4], nullptr);
  size_t page = (size_t)strtoull(argv[5], nullptr, 10);
  snr_overlay_result result = {.abi_version = SNR_OVERLAY_RESULT_ABI,
                               .struct_size = sizeof result};
  o.result = &result;
  snr_error e = {0};
  const char* ext = strrchr(argv[3], '.');
  snr_status s =
      ext && !strcmp(ext, ".png")
          ? snr_mark_page_png(&blank, pdf.bytes, pdf.size, page, &o, &out, &e)
          : snr_mark_document_pdf(&blank, pdf.bytes, pdf.size, 0, SIZE_MAX, &o,
                                  &out, &e);
  if (s)
    fprintf(stderr, "%u: %s\n", (unsigned)s, e.message);
  else {
    FILE* f = fopen(argv[3], "wb");
    if (!f || fwrite(out.bytes, 1, out.size, f) != out.size) return 2;
    fclose(f);
    printf(
        "drawn=%zu sheets=%zu highlights=%zu residual=%.6f checked=%zu "
        "signatures=%zu all_dropped=%d no_pairs=%d\n",
        result.annotated_pages_drawn, result.sheets_dropped,
        result.highlights_dropped, result.worst_paired_residual,
        result.pages_checked_by_pairs, result.signature_fields,
        result.all_annotations_dropped, result.no_pairs_agree);
  }
  snr_buffer_free(&out);
  snr_buffer_free(&pdf);
  snr_buffer_free(&mark);
  sn_notebook_free(parsed);
  return s ? 1 : 0;
}
