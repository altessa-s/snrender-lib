/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <math.h>
#include <poppler.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>

#include "recognition_order.h"

static int fixture(const char* pdf, const char* labels) {
  char* uri = g_filename_to_uri(pdf, nullptr, nullptr);
  PopplerDocument* document =
      poppler_document_new_from_file(uri, nullptr, nullptr);
  g_free(uri);
  if (!document) return 1;
  PopplerPage* page = poppler_document_get_page(document, 0);
  double width, height;
  poppler_page_get_size(page, &width, &height);
  char* contents = nullptr;
  if (!g_file_get_contents(labels, &contents, nullptr, nullptr)) return 1;
  char** lines = g_strsplit(contents, "\n", -1);
  bool ok = true;
  size_t count = 0;
  for (size_t i = 0; lines[i]; i++) {
    double x, y, w, h, original_x, original_y, original_w, original_h, normal_h;
    int offset = 0;
    if (sscanf(lines[i], "%lf %lf %lf %lf %lf %lf %lf %lf %lf %n", &x, &y, &w,
               &h, &original_x, &original_y, &original_w, &original_h,
               &normal_h, &offset) != 9)
      continue;
    const char* hex = lines[i] + offset;
    size_t length = strlen(hex) / 2;
    char* label = g_malloc(length + 1);
    for (size_t j = 0; j < length; j++)
      label[j] = (char)((g_ascii_xdigit_value(hex[j * 2]) << 4) |
                        g_ascii_xdigit_value(hex[j * 2 + 1]));
    label[length] = 0;
    char** words = g_strsplit_set(label, " \t\r\n", -1);
    for (size_t j = 0; words[j]; j++) {
      if (!*words[j]) continue;
      GList* matches = poppler_page_find_text_with_options(
          page, words[j], POPPLER_FIND_WHOLE_WORDS_ONLY);
      bool found = false;
      double f = 72 / 25.4;
      /* Font metric rounding and PDF coordinate quantization fit comfortably
       * within one quarter of the stored height. Poppler can include the line
       * extent for punctuation beside taller words. */
      double tolerance = h * f * 0.25;
      /* Undersized punctuation may sit outside the normal words' overlap.
       * Keep normal words strict and allow small boxes a quarter-line margin.
       */
      double center_tolerance =
          normal_h > 0 && original_h < normal_h ? tolerance : 0;
      for (GList* node = matches; node; node = node->next) {
        PopplerRectangle* r = node->data;
        double center = height - (r->y1 + r->y2) / 2;
        if (center >= original_y * f - center_tolerance &&
            center <= (original_y + original_h) * f + center_tolerance &&
            r->x1 >= x * f - tolerance && r->x2 <= (x + w) * f + tolerance &&
            r->y1 >= height - (y + h) * f - tolerance &&
            r->y2 <= height - y * f + tolerance && r->x2 > r->x1 &&
            r->y2 > r->y1) {
          if (!found) printf("selection height: %.6f\n", r->y2 - r->y1);
          found = true;
        }
      }
      if (!found)
        for (GList* node = matches; node; node = node->next) {
          PopplerRectangle* r = node->data;
          printf("match %.4f %.4f %.4f %.4f\n", r->x1, r->y1, r->x2, r->y2);
        }
      if (!found)
        printf(
            "%s: no whole-word rectangle on box for %s [%.4f %.4f %.4f %.4f]\n",
            pdf, words[j], x, y, w, h);
      ok = ok && found;
      count++;
      g_list_free_full(matches, (GDestroyNotify)poppler_rectangle_free);
    }
    g_strfreev(words);
    g_free(label);
  }
  printf("%s: %zu word boxes checked\n", pdf, count);
  g_strfreev(lines);
  g_free(contents);
  g_object_unref(page);
  g_object_unref(document);
  return ok ? 0 : 1;
}

static PopplerDocument* render_element(const sn_element* element) {
  sn_recognition recognition = {.elements = element, .element_count = 1};
  sn_page page = {
      .number = 1, .size = {1404, 1872}, .recognition = &recognition};
  sn_notebook notebook = {.pages = &page, .page_count = 1};
  snr_options options = snr_options_default();
  options.scale = 0.0625;
  snr_buffer pdf = {0};
  if (snr_page_pdf(&notebook, 0, &options, &pdf, nullptr)) return nullptr;
  GBytes* bytes = g_bytes_new(pdf.bytes, pdf.size);
  PopplerDocument* document =
      poppler_document_new_from_bytes(bytes, nullptr, nullptr);
  g_bytes_unref(bytes);
  snr_buffer_free(&pdf);
  return document;
}

static bool recognition_order(void) {
  PopplerDocument* document = render_element(&order_element);
  if (!document) return false;
  PopplerPage* sheet = poppler_document_get_page(document, 0);
  char* text = poppler_page_get_text(sheet);
  /* Geometric extraction can expand a gap to multiple spaces. Preserve line
   * breaks while collapsing those spaces for the exact order assertion. */
  size_t used = 0;
  for (size_t i = 0; text && text[i]; i++) {
    if (text[i] == ' ' && used && text[used - 1] == ' ') continue;
    if (text[i] == '\n' && used && text[used - 1] == ' ') used--;
    text[used++] = text[i];
  }
  if (text) text[used] = 0;
  bool ok = text && !strcmp(text, order_expected);
  if (!ok) printf("recognition order: %s\n", text ? text : "(null)");
  g_free(text);
  const char* labels[] = {"Alpha", "Beta", "Gamma",  "Delta", "Epsilon",
                          "Zeta",  "Eta",  "Жёлтый", "цвет"};
  const size_t indices[] = {0, 2, 6, 8, 12, 14, 16, 18, 20};
  double height = 1872 * 72.0 / 300, factor = 72 / 25.4;
  for (size_t i = 0; i < sizeof indices / sizeof *indices; i++) {
    sn_rect box = order_words[indices[i]].box;
    GList* matches = poppler_page_find_text_with_options(
        sheet, labels[i], POPPLER_FIND_CASE_SENSITIVE);
    ok = ok && matches && !matches->next;
    if (matches) {
      PopplerRectangle* r = matches->data;
      /* Shared line metrics may extend a selection vertically, but its center
       * must remain in this word's box, with its own horizontal bounds. */
      double center = height - (r->y1 + r->y2) / 2;
      ok = ok && fabs(r->x1 - box.x * factor) < 0.1 &&
           r->x2 <= (box.x + box.width + box.height * 0.25) * factor + 0.1 &&
           r->x2 > r->x1 && center >= box.y * factor &&
           center <= (box.y + box.height) * factor;
    }
    g_list_free_full(matches, (GDestroyNotify)poppler_rectangle_free);
  }
  GList* matches = poppler_page_find_text(sheet, "Gamma Delta");
  ok = ok && matches;
  g_list_free_full(matches, (GDestroyNotify)poppler_rectangle_free);
  g_object_unref(sheet);
  g_object_unref(document);
  return ok;
}

static bool tall_word_bands(void) {
  sn_word words[] = {{{"One", 3}, {6, 32, 12, 6, false}},
                     {{"Two", 3}, {25, 32, 12, 6, false}},
                     {{"Three", 5}, {44, 32, 14, 6, false}},
                     {{"\n", 1}, {.is_null = true}},
                     {{"Four", 4}, {6, 53, 12, 6, false}},
                     {{"Five", 4}, {25, 53, 12, 6, false}},
                     {{"Six", 3}, {44, 53, 14, 6, false}},
                     {{"\n", 1}, {.is_null = true}},
                     {{"Seven", 5}, {68, 0, 14, 150, false}},
                     {{"Eight", 5}, {91, 0, 14, 150, false}}};
  PopplerRectangle control[7] = {0};
  bool ok = true;
  for (int variant = 0; variant < 3; variant++) {
    sn_element el = {.words = words, .word_count = variant ? 10 : 7};
    if (variant == 2) words[7].label = (sn_string){" ", 1};
    PopplerDocument* document = render_element(&el);
    if (!document) return false;
    PopplerPage* page = poppler_document_get_page(document, 0);
    for (size_t i = 0; i < el.word_count; i++) {
      if (words[i].box.is_null) continue;
      GList* matches = poppler_page_find_text_with_options(
          page, words[i].label.data, POPPLER_FIND_WHOLE_WORDS_ONLY);
      bool found = matches && !matches->next;
      if (found) {
        PopplerRectangle r = *(PopplerRectangle*)matches->data;
        double h = r.y2 - r.y1;
        found = h > 0 && fabs(h - 6 * 72 / 25.4) < 0.1;
        if (!variant)
          control[i] = r;
        else if (i < 7)
          found &= fabs(r.y1 - control[i].y1) < 0.1 &&
                   fabs(r.y2 - control[i].y2) < 0.1;
      }
      if (!found)
        fprintf(stderr, "tall box PDF variant %d: %s band\n", variant,
                words[i].label.data);
      ok &= found;
      g_list_free_full(matches, (GDestroyNotify)poppler_rectangle_free);
    }
    g_object_unref(page);
    g_object_unref(document);
  }
  return ok;
}

int main(int argc, char** argv) {
  if (argc == 3) return fixture(argv[1], argv[2]);

  if (!recognition_order() || !tall_word_bands()) return 1;

  sn_word words[] = {{{"Searchable", 10}, {20, 30, 40, 10, false}},
                     {{"phrase", 6}, {62, 30, 30, 10, false}},
                     {{"Next", 4}, {20, 50, 40, 10, false}}};
  sn_element element = {.words = words, .word_count = 3};
  sn_recognition recognition = {.elements = &element, .element_count = 1};
  sn_page page = {
      .number = 1, .size = {1404, 1872}, .recognition = &recognition};
  sn_notebook notebook = {.pages = &page, .page_count = 1};
  snr_options options = snr_options_default();
  options.scale = 0.0625;
  snr_buffer pdf = {0};
  if (snr_page_pdf(&notebook, 0, &options, &pdf, nullptr)) return 1;
  GBytes* bytes = g_bytes_new(pdf.bytes, pdf.size);
  PopplerDocument* document =
      poppler_document_new_from_bytes(bytes, nullptr, nullptr);
  g_bytes_unref(bytes);
  snr_buffer_free(&pdf);
  if (!document) return 1;
  PopplerPage* sheet = poppler_document_get_page(document, 0);
  char* text = poppler_page_get_text(sheet);
  printf("extracted: %s\n", text);
  bool ok = !strcmp(text, "Searchable phrase\nNext");
  g_free(text);
  GList* matches = poppler_page_find_text_with_options(
      sheet, "Searchable", POPPLER_FIND_WHOLE_WORDS_ONLY);
  double factor = 72 / 25.4;
  if (!matches || matches->next) ok = false;
  if (matches) {
    PopplerRectangle* r = matches->data;
    double height = 1872 * 72.0 / 300;
    printf("selection: %.4f %.4f %.4f %.4f\n", r->x1, r->y1, r->x2, r->y2);
    ok = ok && fabs(r->x1 - 20 * factor) < 1 &&
         fabs(r->x2 - 59.5 * factor) < 1 &&
         fabs(r->y1 - (height - 40 * factor)) < 1 &&
         fabs(r->y2 - (height - 30 * factor)) < 1;
  }
  g_list_free_full(matches, (GDestroyNotify)poppler_rectangle_free);
  matches = poppler_page_find_text(sheet, "Searchable phrase");
  printf("phrase matches: %u\n", g_list_length(matches));
  ok = ok && matches;
  g_list_free_full(matches, (GDestroyNotify)poppler_rectangle_free);
  g_object_unref(sheet);
  g_object_unref(document);
  /* A text-only excess must be rejected even when the raster fits. */
  size_t work = 1404u * 1872u + 88u * 117u + 4u + 20u * 256u;
  options.limits.maximum_work = work;
  ok = ok &&
       snr_page_pdf(&notebook, 0, &options, &pdf, nullptr) == SNR_ERR_LIMIT;
  options.limits.maximum_work = snr_limits_default().maximum_work;
  options.limits.maximum_document_work = work;
  ok = ok &&
       snr_page_pdf(&notebook, 0, &options, &pdf, nullptr) == SNR_ERR_LIMIT;
  snr_buffer_free(&pdf);
  /* Whitespace-only labels must remain valid when a cluster contains the
   * appended separator as well as the stored whitespace. */
  words[0].label = (sn_string){" ", 1};
  words[1].label = (sn_string){"  ", 2};
  words[2].label = (sn_string){"   ", 3};
  options.limits.maximum_document_work =
      snr_limits_default().maximum_document_work;
  ok = ok && snr_page_pdf(&notebook, 0, &options, &pdf, nullptr) == SNR_OK;
  snr_buffer_free(&pdf);
  return ok ? 0 : 1;
}
