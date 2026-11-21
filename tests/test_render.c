#include "../src/snr_text.h"
/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <inttypes.h>
#include <math.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SNR_TEST_CAIRO
#include <poppler.h>
#endif
#ifdef SNR_TEST_APPLE
#include <CoreGraphics/CoreGraphics.h>
#endif
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      exit(1);                                                \
    }                                                         \
  } while (0)
#define OK(x) CHECK((x) == SNR_OK)
#include "png_decode.h"
static uint64_t digest(const uint8_t* p, size_t n) {
  uint64_t h = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * UINT64_C(1099511628211);
  return h;
}
static sn_notebook* load(const char* path) {
  FILE* f = fopen(path, "rb");
  CHECK(f);
  CHECK(!fseek(f, 0, SEEK_END));
  long len = ftell(f);
  CHECK(len > 0);
  rewind(f);
  uint8_t* b = malloc((size_t)len);
  CHECK(b);
  CHECK(fread(b, 1, (size_t)len, f) == (size_t)len);
  fclose(f);
  sn_notebook* n = nullptr;
  CHECK(sn_parse_notebook(b, (size_t)len, &n, nullptr) == SN_OK);
  free(b);
  return n;
}
static bool cancel(void* context) {
  (*(size_t*)context)++;
  return true;
}
static snr_status reject(void* context, const uint8_t* p, size_t n) {
  CHECK(p && n);
  (*(size_t*)context)++;
  return SNR_ERR_CANCELLED;
}
static snr_status append(void* context, const uint8_t* p, size_t n) {
  snr_buffer* b = context;
  uint8_t* bytes = realloc(b->bytes, b->size + n);
  CHECK(bytes);
  memcpy(bytes + b->size, p, n);
  b->bytes = bytes;
  b->size += n;
  return SNR_OK;
}
static void progress(void* context, size_t completed, size_t total) {
  size_t* calls = context;
  CHECK(completed == *calls);
  CHECK(completed <= total);
  (*calls)++;
}
#ifndef SNR_TEST_APPLE
static bool contains(const uint8_t* data, size_t size, const char* needle) {
  size_t length = strlen(needle);
  for (size_t i = 0; i + length <= size; i++)
    if (!memcmp(data + i, needle, length)) return true;
  return false;
}
/* From PDF 1.5 an indirect object is either in the file body or in an object
 * stream, which holds the same object text inside a Flate stream. Searching
 * the body and every inflated object stream therefore finds an object's text
 * wherever the writer placed it. */
static bool pdf_object_text(const snr_buffer* pdf, const char* needle) {
  if (contains(pdf->bytes, pdf->size, needle)) return true;
  static const char marker[] = "/Type /ObjStm";
  for (size_t i = 0; i + sizeof marker - 1 <= pdf->size; i++) {
    if (memcmp(pdf->bytes + i, marker, sizeof marker - 1)) continue;
    size_t start = i;
    while (start + 6 <= pdf->size && memcmp(pdf->bytes + start, "stream", 6))
      start++;
    start += 6;
    if (start < pdf->size && pdf->bytes[start] == '\r') start++;
    if (start < pdf->size && pdf->bytes[start] == '\n') start++;
    if (start >= pdf->size) break;
    size_t capacity = 1 << 16;
    uint8_t* text = malloc(capacity);
    CHECK(text);
    z_stream z = {.next_in = (Bytef*)(uintptr_t)(pdf->bytes + start),
                  .avail_in = (uInt)(pdf->size - start)};
    CHECK(inflateInit(&z) == Z_OK);
    int status;
    do {
      if (z.total_out == capacity) {
        capacity *= 2;
        text = realloc(text, capacity);
        CHECK(text);
      }
      z.next_out = text + z.total_out;
      z.avail_out = (uInt)(capacity - z.total_out);
      status = inflate(&z, Z_NO_FLUSH);
    } while (status == Z_OK);
    CHECK(status == Z_STREAM_END);
    bool found = contains(text, z.total_out, needle);
    inflateEnd(&z);
    free(text);
    if (found) return true;
  }
  return false;
}
#endif
static void pdf_check(const snr_buffer* pdf, size_t count, double width,
                      double height) {
  CHECK(pdf->size > 100 && !memcmp(pdf->bytes, "%PDF-", 5));
#ifdef SNR_TEST_APPLE
  CGDataProviderRef provider =
      CGDataProviderCreateWithData(nullptr, pdf->bytes, pdf->size, nullptr);
  CGPDFDocumentRef doc = CGPDFDocumentCreateWithProvider(provider);
  CHECK(doc);
  CHECK(CGPDFDocumentGetNumberOfPages(doc) == count);
  CGRect box =
      CGPDFPageGetBoxRect(CGPDFDocumentGetPage(doc, 1), kCGPDFMediaBox);
  CHECK(fabs(box.size.width - width) < 0.01);
  CHECK(fabs(box.size.height - height) < 0.01);
  CGPDFDocumentRelease(doc);
  CGDataProviderRelease(provider);
#else
  GBytes* bytes = g_bytes_new(pdf->bytes, pdf->size);
  PopplerDocument* doc =
      poppler_document_new_from_bytes(bytes, nullptr, nullptr);
  CHECK(doc);
  CHECK((size_t)poppler_document_get_n_pages(doc) == count);
  PopplerPage* page = poppler_document_get_page(doc, 0);
  double w, h;
  poppler_page_get_size(page, &w, &h);
  CHECK(fabs(w - width) < 0.01 && fabs(h - height) < 0.01);
  g_object_unref(page);
  g_object_unref(doc);
  g_bytes_unref(bytes);
#endif
}
static void roundtrip(const snr_bitmap* original, const snr_buffer* png) {
  size_t width, height;
  uint8_t* pixels = png_decode(png, &width, &height);
  CHECK(width == original->width && height == original->height);
  for (size_t i = 0; i < original->width * original->height; i++) {
    unsigned alpha = pixels[i * 4 + 3];
    CHECK(alpha == original->pixels[i * 4 + 3]);
    for (size_t c = 0; c < 3; c++)
      CHECK(pixels[i * 4 + c] * alpha / 255 == original->pixels[i * 4 + c]);
  }
  free(pixels);
}

static void failures(void) {
  sn_page p = {.size = {2, 2}};
  sn_notebook n = {.pages = &p, .page_count = 1};
  snr_options o = snr_options_default();
  snr_bitmap b = {0};
  snr_buffer buf = {0};
  snr_error e = {0};
  CHECK(snr_page_bitmap(nullptr, 0, nullptr, &b, &e) ==
        SNR_ERR_INVALID_ARGUMENT);
  CHECK(e.code == SNR_ERR_INVALID_ARGUMENT);
  o.abi_version = 0;
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_INVALID_ARGUMENT);
  o = snr_options_default();
  o.struct_size--;
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_INVALID_ARGUMENT);
  o = snr_options_default();
  o.scale = NAN;
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_INVALID_ARGUMENT);
  o.scale = 9;
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_LIMIT);
#ifdef SNR_TEST_CAIRO
  o = snr_options_default();
  p.size = (sn_size){32768, 1};
  CHECK(snr_page_bitmap(&n, 0, &o, &b, &e) == SNR_ERR_LIMIT);
  p.size = (sn_size){2, 2};
#endif
  o = snr_options_default();
  o.limits.maximum_pixels = 3;
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_LIMIT);
  o = snr_options_default();
  o.limits.maximum_decoded_pixels = 3;
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_LIMIT);
  o = snr_options_default();
  o.limits.maximum_work = 1;
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_LIMIT);
  o = snr_options_default();
  o.limits.maximum_pages = 0;
  CHECK(snr_document_pdf(&n, 0, 1, &o, &buf, nullptr) == SNR_ERR_LIMIT);
  o = snr_options_default();
  o.limits.maximum_output_bytes = 8;
  CHECK(snr_page_png(&n, 0, &o, &buf, nullptr) == SNR_ERR_LIMIT);
  CHECK(!buf.bytes);
  size_t calls = 0;
  o = snr_options_default();
  o.cancel = cancel;
  o.cancel_context = &calls;
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_CANCELLED);
  CHECK(calls > 0 && !b.pixels);
  o = snr_options_default();
  o.scale = 2;
  o.has_background = true;
  o.background = (snr_color){1, 0, 0, 1};
  OK(snr_page_bitmap(&n, 0, &o, &b, nullptr));
  CHECK(b.width == 4 && b.pixels[0] == 255 && b.pixels[1] == 0);
  snr_bitmap_free(&b);
  uint8_t broken[] = {137, 80, 78, 71, 13, 10, 26, 10, 1};
  sn_bitmap encoded = {.encoding = SN_ENCODING_PNG,
                       .bytes = {broken, sizeof broken}};
  sn_layer layer = {.bitmap = &encoded, .is_visible = true};
  p.layers = &layer;
  p.layer_count = 1;
  o.strict_images = true;
  CHECK(snr_page_bitmap(&n, 0, &o, &b, nullptr) == SNR_ERR_BACKEND);
  CHECK(!b.pixels);
  uint8_t huge[33] = {137,  80,   78,   71,   13,   10,   26,   10,
                      0,    0,    0,    13,   'I',  'H',  'D',  'R',
                      0x7f, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff};
  encoded.bytes = (sn_bytes){huge, sizeof huge};
  CHECK(snr_page_bitmap(&n, 0, nullptr, &b, nullptr) == SNR_ERR_LIMIT);
  encoded.bytes = (sn_bytes){broken, sizeof broken};
  layer.name = SN_LAYER_BACKGROUND;
  o = snr_options_default();
  o.include_background = false;
  OK(snr_page_bitmap(&n, 0, &o, &b, nullptr));
  snr_bitmap_free(&b);
  p.layer_count = 0;
  calls = 0;
  snr_output output = {SNR_OUTPUT_ABI, sizeof output, reject, &calls};
  CHECK(snr_page_png_to_output(&n, 0, nullptr, &output, nullptr) ==
        SNR_ERR_CANCELLED);
  CHECK(calls == 1);
  output.abi_version = 0;
  CHECK(snr_page_svg_to_output(&n, 0, nullptr, &output, nullptr) ==
        SNR_ERR_INVALID_ARGUMENT);
  o = snr_options_default();
  o.recognition = false;
  o.title = "Test";
  calls = 0;
  o.progress = progress;
  o.progress_context = &calls;
  OK(snr_document_pdf(&n, 0, SIZE_MAX, &o, &buf, nullptr));
  CHECK(calls == 2);
  snr_buffer_free(&buf);
  CHECK(snr_document_pdf(&n, 0, 2, nullptr, &buf, nullptr) ==
        SNR_ERR_INVALID_ARGUMENT);
  snr_buffer_free(nullptr);
  snr_bitmap_free(nullptr);
  snr_raster_free(nullptr);
}
#ifdef SNR_TEST_CAIRO
static void mixed_links(void) {
  sn_page pages[] = {{.number = 1, .size = {1404, 1872}},
                     {.number = 2, .size = {1872, 1404}},
                     {.number = 3, .size = {1000, 700}}};
  sn_link links[4] = {0};
  for (size_t i = 0; i < 3; i++)
    links[i] = (sn_link){.page_number = (int64_t)i + 1,
                         .kind = 0,
                         .has_target_page_number = true,
                         .target_page_number = (int64_t)((i + 1) % 3) + 1,
                         .rect = {20, 30, 100, 40, false}};
  links[3] = (sn_link){.page_number = 3,
                       .kind = 4,
                       .url = {"https://example.com/", 20},
                       .rect = {900, 600, 200, 200, false}};
  sn_notebook n = {
      .pages = pages, .page_count = 3, .links = links, .link_count = 4};
  snr_options o = snr_options_default();
  o.scale = 0.125;
  snr_buffer pdf = {0};
  OK(snr_document_pdf(&n, 0, 3, &o, &pdf, nullptr));
  GBytes* bytes = g_bytes_new(pdf.bytes, pdf.size);
  PopplerDocument* doc =
      poppler_document_new_from_bytes(bytes, nullptr, nullptr);
  CHECK(doc && poppler_document_get_n_pages(doc) == 3);
  for (int i = 0; i < 3; i++) {
    PopplerPage* page = poppler_document_get_page(doc, i);
    GList* mappings = poppler_page_get_link_mapping(page);
    CHECK(g_list_length(mappings) == (i == 2 ? 2 : 1));
    bool found = false;
    for (GList* it = mappings; it; it = it->next) {
      PopplerLinkMapping* m = it->data;
      if (m->action->type != POPPLER_ACTION_GOTO_DEST) continue;
      found = true;
      CHECK(fabs(m->area.x1 - 4.8) < 1.1);
      CHECK(fabs(m->area.y1 - (pages[i].size.height * 0.24 - 16.8)) < 1.1);
      CHECK(fabs(m->area.y2 - (pages[i].size.height * 0.24 - 7.2)) < 1.1);
    }
    CHECK(found);
    poppler_page_free_link_mapping(mappings);
    g_object_unref(page);
  }
  g_object_unref(doc);
  g_bytes_unref(bytes);
  snr_buffer_free(&pdf);
  puts("three distinct page heights retain all link positions");
}
#endif
static void unicode_pdf(void) {
  const sn_word words[] = {{{"Searchable", 10}, {10, 10, 30, 8, false}},
                           {{"Привет", 12}, {10, 25, 30, 8, false}},
                           {{"你好世界", 12}, {10, 40, 30, 8, false}}};
  sn_element element = {.words = words, .word_count = 3};
  sn_recognition recognition = {.elements = &element, .element_count = 1};
  sn_page pages[2] = {
      {.number = 1, .size = {1404, 1872}, .recognition = &recognition},
      {.number = 2,
       .size = {1872, 1404},
       .orientation = SN_ORIENTATION_LANDSCAPE}};
  sn_title titles[2] = {{.page_number = 1, .level = 1},
                        {.page_number = 2, .level = 2}};
  sn_link links[3] = {
      {.page_number = 1,
       .kind = 0,
       .has_target_page_number = true,
       .target_page_number = 2,
       .rect = {20, 30, 100, 40, false}},
      {.page_number = 1,
       .kind = 4,
       .url = {"https://example.com/", 20},
       .rect = {20, 90, 100, 40, false}},
      {.page_number = 2, .kind = 2, .rect = {20, 30, 100, 40, false}}};
  sn_notebook n = {.equipment = SN_EQUIPMENT_NOMAD,
                   .pages = pages,
                   .page_count = 2,
                   .titles = titles,
                   .title_count = 2,
                   .links = links,
                   .link_count = 3};
  snr_options o = snr_options_default();
  o.scale = 0.125;
  o.title = "Unicode search";
  snr_buffer pdf = {0};
  OK(snr_document_pdf(&n, 0, 2, &o, &pdf, nullptr));
  pdf_check(&pdf, 2, 336.96, 449.28);
#ifdef SNR_TEST_APPLE
  CGDataProviderRef provider =
      CGDataProviderCreateWithData(nullptr, pdf.bytes, pdf.size, nullptr);
  CGPDFDocumentRef doc = CGPDFDocumentCreateWithProvider(provider);
  CFDictionaryRef outline = CGPDFDocumentGetOutline(doc);
  CHECK(outline);
  CFArrayRef children = CFDictionaryGetValue(outline, kCGPDFOutlineChildren);
  CHECK(children && CFArrayGetCount(children) == 1);
  CGPDFArrayRef annots = nullptr;
  CHECK(CGPDFDictionaryGetArray(
      CGPDFPageGetDictionary(CGPDFDocumentGetPage(doc, 1)), "Annots", &annots));
  CHECK(CGPDFArrayGetCount(annots) == 2);
  CGPDFDictionaryRef annot = nullptr;
  CHECK(CGPDFArrayGetDictionary(annots, 0, &annot));
  CGPDFArrayRef rect = nullptr;
  CHECK(CGPDFDictionaryGetArray(annot, "Rect", &rect));
  double expected[] = {4.8, 432.48, 28.8, 442.08};
  for (size_t i = 0; i < 4; i++) {
    CGPDFReal value;
    CHECK(CGPDFArrayGetNumber(rect, i, &value));
    CHECK(fabs(value - expected[i]) < 0.01);
  }
  CGPDFArrayRef destination = nullptr;
  CHECK(CGPDFDictionaryGetArray(annot, "Dest", &destination));
  CGPDFDictionaryRef target_page = nullptr;
  CHECK(CGPDFArrayGetDictionary(destination, 0, &target_page));
  CHECK(target_page == CGPDFPageGetDictionary(CGPDFDocumentGetPage(doc, 2)));
  CFDictionaryRef first_outline = CFArrayGetValueAtIndex(children, 0);
  CFArrayRef nested =
      CFDictionaryGetValue(first_outline, kCGPDFOutlineChildren);
  CHECK(nested && CFArrayGetCount(nested) == 1);
  CGPDFDocumentRelease(doc);
  CGDataProviderRelease(provider);
#else
  const char quad[] =
      "/QuadPoints [ 4.8 442.08 28.8 442.08 28.8 432.48 4.8 432.48 ]";
  CHECK(pdf_object_text(&pdf, quad));
  GBytes* bytes = g_bytes_new(pdf.bytes, pdf.size);
  PopplerDocument* doc =
      poppler_document_new_from_bytes(bytes, nullptr, nullptr);
  CHECK(doc);
  PopplerPage* page = poppler_document_get_page(doc, 0);
  GList* mappings = poppler_page_get_link_mapping(page);
  CHECK(g_list_length(mappings) == 2);
  bool found_uri = false, found_dest = false;
  for (GList* it = mappings; it; it = it->next) {
    PopplerLinkMapping* mapping = it->data;
    if (mapping->action->type == POPPLER_ACTION_GOTO_DEST) {
      found_dest = true;
      CHECK(fabs(mapping->area.y1 - 432.48) < 1.1);
      CHECK(fabs(mapping->area.y2 - 442.08) < 1.1);
      PopplerDest* dest = mapping->action->goto_dest.dest;
      CHECK(dest->type == POPPLER_DEST_NAMED);
      PopplerDest* resolved = poppler_document_find_dest(doc, dest->named_dest);
      CHECK(resolved && resolved->page_num == 2);
      poppler_dest_free(resolved);
    } else if (mapping->action->type == POPPLER_ACTION_URI) {
      found_uri = true;
      CHECK(!strcmp(mapping->action->uri.uri, "https://example.com/"));
    }
  }
  CHECK(found_uri && found_dest);
  poppler_page_free_link_mapping(mappings);
  PopplerIndexIter* root = poppler_index_iter_new(doc);
  CHECK(root);
  PopplerIndexIter* child = poppler_index_iter_get_child(root);
  CHECK(child);
  PopplerAction* action = poppler_index_iter_get_action(child);
  CHECK(action && !strcmp(action->any.title, "Page 2 — title 2"));
  poppler_action_free(action);
  CHECK(!poppler_index_iter_next(child));
  poppler_index_iter_free(child);
  CHECK(!poppler_index_iter_next(root));
  poppler_index_iter_free(root);
  g_object_unref(page);
  g_object_unref(doc);
  g_bytes_unref(bytes);
#endif
  FILE* f = fopen("unicode.pdf", "wb");
  CHECK(f);
  CHECK(fwrite(pdf.bytes, 1, pdf.size, f) == pdf.size);
  CHECK(!fclose(f));
  snr_buffer_free(&pdf);
  o.recognition = false;
  OK(snr_document_pdf(&n, 0, 2, &o, &pdf, nullptr));
  f = fopen("unicode-off.pdf", "wb");
  CHECK(f);
  CHECK(fwrite(pdf.bytes, 1, pdf.size, f) == pdf.size);
  CHECK(!fclose(f));
  snr_buffer_free(&pdf);
}
int main(int argc, char** argv) {
  CHECK(argc == 2);
  if (!strcmp(SNR_SUITE, "limits")) {
    failures();
    return 0;
  }
  if (!strcmp(SNR_SUITE, "recognition") || !strcmp(SNR_SUITE, "links") ||
      !strcmp(SNR_SUITE, "outline")) {
    unicode_pdf();
#ifdef SNR_TEST_CAIRO
    if (!strcmp(SNR_SUITE, "links")) mixed_links();
#endif
    return 0;
  }
  char manifest[2048];
  snprintf(manifest, sizeof manifest, "%s/goldens.tsv", argv[1]);
  FILE* f = fopen(manifest, "r");
  CHECK(f);
  char name[512], g[32], a[32];
  size_t index, total = 0, golden = 0;
  char previous[512] = "";
  sn_notebook* n = nullptr;
  while (fscanf(f, "%511s %zu %31s %31s", name, &index, g, a) == 4) {
    if (strcmp(previous, name)) {
      sn_notebook_free(n);
      char path[2048];
      snprintf(path, sizeof path, "%s/fixtures/%s", argv[1], name);
      n = load(path);
      snprintf(previous, sizeof previous, "%s", name);
    }

    snr_options o = snr_options_default();
    o.has_background = true;
    if (!strcmp(SNR_SUITE, "composition")) {
      snr_raster raster = {0};
      OK(snr_page_raster(n, index, &o, &raster, nullptr));
      if (strcmp(g, "-")) {
        CHECK(digest(raster.gray, raster.width * raster.height) ==
              strtoull(g, nullptr, 16));
        CHECK(digest(raster.alpha, raster.width * raster.height) ==
              strtoull(a, nullptr, 16));
        golden++;
      }
      snr_raster_free(&raster);
      total++;
      continue;
    }
    o.scale = 0.125;
    if (!strcmp(SNR_SUITE, "png")) {
      snr_bitmap bitmap = {0};
      snr_buffer png = {0}, streamed = {0};
      OK(snr_page_bitmap(n, index, &o, &bitmap, nullptr));
      OK(snr_page_png(n, index, &o, &png, nullptr));
      roundtrip(&bitmap, &png);
      snr_output output = {SNR_OUTPUT_ABI, sizeof output, append, &streamed};
      OK(snr_page_png_to_output(n, index, &o, &output, nullptr));
      CHECK(streamed.size == png.size &&
            !memcmp(streamed.bytes, png.bytes, png.size));
      snr_bitmap_free(&bitmap);
      snr_buffer_free(&png);
      snr_buffer_free(&streamed);
      total++;
      continue;
    }
    if (!strcmp(SNR_SUITE, "svg")) {
      snr_buffer svg = {0}, again = {0};
      OK(snr_page_svg(n, index, &o, &svg, nullptr));
      OK(snr_page_svg(n, index, &o, &again, nullptr));
      CHECK(svg.size == again.size &&
            !memcmp(svg.bytes, again.bytes, svg.size));
      const char aspect[] = "preserveAspectRatio=\"none\"";
      bool found_aspect = false;
      for (size_t k = 0; k + sizeof aspect - 1 <= svg.size; k++)
        if (!memcmp(svg.bytes + k, aspect, sizeof aspect - 1))
          found_aspect = true;
      CHECK(found_aspect);
      snr_buffer_free(&svg);
      snr_buffer_free(&again);
      total++;
      continue;
    }
    snr_buffer pdf = {0};
    OK(snr_page_pdf(n, index, &o, &pdf, nullptr));
    double dpi = n->equipment == SN_EQUIPMENT_A5X ? 226 : 300;
    const sn_string* machine =
        sn_metadata_get(&n->pages[index].metadata, "PAGEMACHINETYPE");
    if (machine)
      dpi = (machine->length == 1 && machine->data[0] == '3') ? 226 : 300;
    pdf_check(&pdf, 1, n->pages[index].size.width * 72 / dpi,
              n->pages[index].size.height * 72 / dpi);
    if (!strcmp(SNR_SUITE, "pdf_structure")) {
      char path[128];
      snprintf(path, sizeof path, "fixture-%zu.pdf", total);
      FILE* saved = fopen(path, "wb");
      CHECK(saved);
      CHECK(fwrite(pdf.bytes, 1, pdf.size, saved) == pdf.size);
      fclose(saved);
      snprintf(path, sizeof path, "fixture-%zu.words", total);
      saved = fopen(path, "w");
      CHECK(saved);
      const sn_recognition* rec = n->pages[index].recognition;
      if (rec)
        for (size_t ei = 0; ei < rec->element_count; ei++) {
          const sn_element* el = rec->elements + ei;
          snr_text_line line =
              snr_text_line_create(el, n->pages[index].size.width * 25.4 / dpi,
                                   n->pages[index].size.height * 25.4 / dpi);
          for (size_t wi = 0; wi < el->word_count; wi++) {
            const sn_word* word = el->words + wi;
            sn_rect box = word->box;
            if (!snr_box_clip(&box, n->pages[index].size.width * 25.4 / dpi,
                              n->pages[index].size.height * 25.4 / dpi))
              continue;
            sn_rect original = box;
            /* Structured recognition deliberately normalizes vertical line
             * metrics. Search must still fit each emitted word rectangle. */
            snr_text_line_box(&line, el, wi,
                              n->pages[index].size.width * 25.4 / dpi,
                              n->pages[index].size.height * 25.4 / dpi, &box);
            snr_text clean = snr_recognition_text(word->label);
            CHECK(!clean.status);
            if (clean.length) {
              fprintf(saved, "%.9g %.9g %.9g %.9g ", box.x, box.y, box.width,
                      box.height);
              fprintf(saved, "%.9g %.9g %.9g %.9g %.9g ", original.x,
                      original.y, original.width, original.height,
                      line.valid ? line.normal_height : 0);
              for (size_t bi = 0; bi < clean.length; bi++)
                fprintf(saved, "%02x", (unsigned char)clean.data[bi]);
              fputc('\n', saved);
            }
            free(clean.data);
          }
        }
      fclose(saved);
      snr_buffer off = {0};
      o.recognition = false;
      OK(snr_page_pdf(n, index, &o, &off, nullptr));
      snprintf(path, sizeof path, "fixture-%zu-off.pdf", total);
      saved = fopen(path, "wb");
      CHECK(saved);
      CHECK(fwrite(off.bytes, 1, off.size, saved) == off.size);
      fclose(saved);
      snr_buffer_free(&off);
    }
    snr_buffer_free(&pdf);
    total++;
  }
  fclose(f);
  sn_notebook_free(n);
  printf("%s: %zu fixture pages; %zu exact gray/alpha goldens passed\n",
         SNR_SUITE, total, golden);
  return 0;
}
