/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <cairo-ft.h>
#include <cairo-pdf.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <limits.h>

#include "snr_internal.h"
/* The loader announces dimensions before allocating its pixel buffer. Abort
 * oversized input at that boundary, including images with tiny payloads. */
typedef struct {
  size_t limit;
  bool refused;
} decode_limit;
static void prepared(GdkPixbufLoader* loader, int width, int height,
                     gpointer data) {
  decode_limit* l = data;
  if (width <= 0 || height <= 0 || (size_t)width > l->limit / (size_t)height ||
      (size_t)width > SIZE_MAX / 4 / (size_t)height) {
    l->refused = true;
    gdk_pixbuf_loader_set_size(loader, 1, 1);
  }
}
snr_status snr_decode(const sn_bitmap* encoded, size_t limit,
                      const snr_options* options, snr_bitmap* out) {
  size_t expected_width, expected_height;
  snr_status checked =
      snr_image_size(encoded, limit, &expected_width, &expected_height);
  if (checked) return checked;
  sn_bitmap framed = *encoded;
  checked = snr_image_frame(encoded, options, &framed.bytes.size);
  if (checked) return checked;
  encoded = &framed;

  if (!encoded->bytes.data || !encoded->bytes.size) return SNR_ERR_BACKEND;
  GError* error = nullptr;
  GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type(
      encoded->encoding == SN_ENCODING_JPEG ? "jpeg" : "png", &error);
  if (!loader) {
    snr_status status =
        error && error->domain == GDK_PIXBUF_ERROR &&
                error->code == GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY
            ? SNR_ERR_OUT_OF_MEMORY
            : SNR_ERR_BACKEND;
    g_clear_error(&error);
    return status;
  }
  decode_limit l = {limit, false};
  g_signal_connect(loader, "size-prepared", G_CALLBACK(prepared), &l);
  bool ok = true;
  /* Small input chunks ensure the size callback precedes a large decode. */
  for (size_t i = 0; i < encoded->bytes.size && ok && !l.refused;) {
    size_t chunk = encoded->bytes.size - i;
    if (chunk > 4096) chunk = 4096;
    ok =
        gdk_pixbuf_loader_write(loader, encoded->bytes.data + i, chunk, &error);
    i += chunk;
  }
  bool memory_error = error && error->domain == GDK_PIXBUF_ERROR &&
                      error->code == GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY;
  g_clear_error(&error);
  bool closed = gdk_pixbuf_loader_close(loader, &error);
  memory_error =
      memory_error || (error && error->domain == GDK_PIXBUF_ERROR &&
                       error->code == GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY);
  g_clear_error(&error);
  ok = ok && closed;
  GdkPixbuf* pix =
      ok && !l.refused ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
  snr_status s = l.refused      ? SNR_ERR_LIMIT
                 : memory_error ? SNR_ERR_OUT_OF_MEMORY
                 : pix          ? SNR_OK
                                : SNR_ERR_BACKEND;
  snr_bitmap b = {0};
  if (!s) {
    b.width = (size_t)gdk_pixbuf_get_width(pix);
    b.height = (size_t)gdk_pixbuf_get_height(pix);
    if (b.width != expected_width || b.height != expected_height) {
      g_object_unref(loader);
      return SNR_ERR_BACKEND;
    }
    b.stride = b.width * 4;
    b.pixels = malloc(b.stride * b.height);
    if (!b.pixels)
      s = SNR_ERR_OUT_OF_MEMORY;
    else {
      const uint8_t* src = gdk_pixbuf_read_pixels(pix);
      size_t stride = (size_t)gdk_pixbuf_get_rowstride(pix),
             channels = (size_t)gdk_pixbuf_get_n_channels(pix);
      for (size_t y = 0; y < b.height; y++)
        for (size_t x = 0; x < b.width; x++) {
          const uint8_t* a = src + y * stride + x * channels;
          uint8_t* d = b.pixels + y * b.stride + x * 4;
          unsigned alpha = channels == 4 ? a[3] : 255;
          for (size_t c = 0; c < 3; c++)
            d[c] = (uint8_t)((unsigned)a[c] * alpha / 255);
          d[3] = (uint8_t)alpha;
        }
    }
  }
  g_object_unref(loader);
  if (s)
    snr_bitmap_free(&b);
  else
    *out = b;
  return s;
}
/* Cairo never learns that the sink failed. Its PDF surface has freed a stream
 * twice when finishing after a write error (Cairo 1.18 with PDF 1.4), and
 * nothing it would still do on that path is worth the risk. The writer keeps
 * the first failure, drops every
 * later byte, and callers report its status once Cairo has finished on its
 * ordinary path. */
static cairo_status_t put(void* context, const unsigned char* data,
                          unsigned int length) {
  (void)snr_write(context, data, length);
  return CAIRO_STATUS_SUCCESS;
}
static cairo_surface_t* surface(const snr_bitmap* b) {
  if (b->width > INT_MAX || b->height > INT_MAX) return nullptr;
  cairo_surface_t* s = cairo_image_surface_create(
      CAIRO_FORMAT_ARGB32, (int)b->width, (int)b->height);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return nullptr;
  }
  uint8_t* bytes = cairo_image_surface_get_data(s);
  size_t stride = (size_t)cairo_image_surface_get_stride(s);
  for (size_t y = 0; y < b->height; y++)
    for (size_t x = 0; x < b->width; x++) {
      const uint8_t* p = b->pixels + y * b->stride + x * 4;
      uint32_t pixel = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                       ((uint32_t)p[1] << 8) | p[2];
      memcpy(bytes + y * stride + x * 4, &pixel, 4);
    }
  cairo_surface_mark_dirty(s);
  return s;
}
snr_status snr_encode_png(const snr_bitmap* b, snr_writer* w) {
  cairo_surface_t* s = surface(b);
  if (!s) return SNR_ERR_OUT_OF_MEMORY;
  cairo_status_t status = cairo_surface_write_to_png_stream(s, put, w);
  cairo_surface_destroy(s);
  return w->status                          ? w->status
         : status == CAIRO_STATUS_NO_MEMORY ? SNR_ERR_OUT_OF_MEMORY
         : status                           ? SNR_ERR_BACKEND
                                            : SNR_OK;
}
#include "snr_font.h"
static cairo_user_data_key_t font_key;
static bool winansi_scalar(gunichar cp) {
  if (cp <= 255) return true;
  static const gunichar extra[] = {
      0x20ac, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6,  0x2030,
      0x160,  0x2039, 0x152, 0x17d,  0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
      0x2013, 0x2014, 0x2dc, 0x2122, 0x161,  0x203a, 0x153,  0x17e,  0x178};
  for (size_t i = 0; i < sizeof extra / sizeof *extra; i++)
    if (extra[i] == cp) return true;
  return false;
}
/* Multi-scalar Latin clusters avoid character codes in the symbolic subset.
 * The spacer is registered without a Unicode value before assigning U+0020,
 * so it joins the same subset instead of creating a WinAnsi subset. */
static const char* cluster_end(const char* at, const char* end, bool packed) {
  const char* next = g_utf8_next_char(at);
  if (at == end - 1) return end;
  if (packed) {
    while (next < end - 1 && (size_t)(g_utf8_next_char(next) - at) <= 32)
      next = g_utf8_next_char(next);
    if (next < end - 1 && g_utf8_next_char(next) == end - 1 &&
        winansi_scalar(g_utf8_get_char(next)) && next > g_utf8_next_char(at))
      next = g_utf8_prev_char(next);
  } else if (winansi_scalar(g_utf8_get_char(at)) && next < end) {
    next = g_utf8_next_char(next);
    if (next < end - 1 && g_utf8_next_char(next) == end - 1 &&
        winansi_scalar(g_utf8_get_char(next)))
      next = end - 1;
  }
  return next;
}
static snr_status recognition_draw(cairo_t* ctx, cairo_font_face_t* face,
                                   text_font* mapping, const sn_page* page,
                                   double width, double height,
                                   const snr_spacing* spacing) {
  if (!page->recognition) return SNR_OK;
  bool alternate = false;
  for (size_t i = 0; i < page->recognition->element_count; i++) {
    const sn_element* el = page->recognition->elements + i;
    snr_text_line text_line =
        snr_text_line_create(el, width * 25.4 / 72, height * 25.4 / 72);
    for (size_t j = 0; j < el->word_count; j++) {
      const sn_word* word = el->words + j;
      sn_rect r = word->box;
      if (!snr_box_clip(&r, width * 25.4 / 72, height * 25.4 / 72)) continue;
      snr_text_line_box(&text_line, el, j, width * 25.4 / 72,
                        height * 25.4 / 72, &r);
      snr_status spaced_status = text_line.explicit_lines && text_line.valid
                                     ? SNR_OK
                                     : snr_word_spacing(&r, spacing);
      if (spaced_status) return spaced_status;
      snr_text clean = snr_recognition_text(word->label);
      if (clean.status) return clean.status;
      if (!clean.length) {
        free(clean.data);
        continue;
      }
      char* spaced = realloc(clean.data, clean.length + 3);
      if (!spaced) {
        free(clean.data);
        return SNR_ERR_OUT_OF_MEMORY;
      }
      clean.data = spaced;
      clean.data[clean.length++] = ' ';
      clean.data[clean.length] = 0;
      bool packed = clean.length > 32768;
      size_t count = 0;
      for (const char* c = clean.data; c < clean.data + clean.length;) {
        c = cluster_end(c, clean.data + clean.length, packed);
        count++;
      }
      cairo_glyph_t* glyphs = cairo_glyph_allocate((int)count + 2);
      cairo_text_cluster_t* clusters =
          cairo_text_cluster_allocate((int)count + 2);
      if (!glyphs || !clusters) {
        cairo_glyph_free(glyphs);
        cairo_text_cluster_free(clusters);
        free(clean.data);
        return SNR_ERR_OUT_OF_MEMORY;
      }
      /* Reverse only the positions of contiguous RTL scalars. The text and
       * cluster mapping remain logical, allowing consumers to recover order. */
      const char* cursor = clean.data;
      for (size_t k = 0; k < count; k++) {
        const char* next =
            cluster_end(cursor, clean.data + clean.length, packed);
        unsigned long id = 0;
        snr_status status =
            scalar_glyph(mapping, cursor, (size_t)(next - cursor), &id);
        if (status) {
          cairo_glyph_free(glyphs);
          cairo_text_cluster_free(clusters);
          free(clean.data);
          return status;
        }
        glyphs[k] = (cairo_glyph_t){id, (double)k, 0};
        clusters[k] = (cairo_text_cluster_t){(int)(next - cursor), 1};
        cursor = next;
      }
      cursor = clean.data;
      for (size_t k = 0; !packed && k < count;) {
        gunichar cp = g_utf8_get_char(cursor);
        const char* end_text =
            cluster_end(cursor, clean.data + clean.length, packed);
        if (cp < 0x590 || cp > 0x8ff) {
          k++;
          cursor = end_text;
          continue;
        }
        size_t end = k + 1;
        while (end < count) {
          cp = g_utf8_get_char(end_text);
          if (cp < 0x590 || cp > 0x8ff) break;
          end_text = cluster_end(end_text, clean.data + clean.length, packed);
          end++;
        }
        for (size_t q = k; q < end; q++)
          glyphs[q].x = (double)(end - 1 - q + k);
        k = end;
        cursor = end_text;
      }
      cairo_save(ctx);
      cairo_translate(ctx, r.x * 72 / 25.4, (r.y + r.height / 2) * 72 / 25.4);
      bool trailing = glyphs[count - 1].index == 65534 &&
                      clusters[count - 1].num_bytes == 1;

      cairo_scale(ctx,
                  r.width / r.height / (double)(count - (trailing ? 1 : 0)), 1);
      cairo_set_font_face(ctx, face);
      double size = r.height * 72 / 25.4;
      cairo_set_font_size(ctx, size);
      for (size_t k = 0; k < count; k++) glyphs[k].x *= size;
      /* A source transition closes the preceding PDF text object so extraction
       * keeps independently positioned words in their stored order. */
      alternate = !alternate;
      double colour = alternate ? 0 : 1;
      cairo_set_source_rgb(ctx, colour, colour, colour);
      if (!mapping->space_seeded) {
        /* An initially unmapped spacer enters the CID subset. Its following
         * U+0020 cluster supplies ToUnicode without creating a Latin subset. */
        memmove(glyphs + 2, glyphs, count * sizeof *glyphs);
        memmove(clusters + 2, clusters, count * sizeof *clusters);
        glyphs[0] = (cairo_glyph_t){65534, -size * 0.2, 0};
        glyphs[1] = (cairo_glyph_t){65534, -size * 0.1, 0};
        clusters[0] = (cairo_text_cluster_t){0, 1};
        clusters[1] = (cairo_text_cluster_t){1, 1};
        memmove(clean.data + 1, clean.data, clean.length + 1);
        clean.data[0] = ' ';
        clean.length++;
        count += 2;
        mapping->space_seeded = true;
      }
      cairo_show_text_glyphs(ctx, clean.data, (int)clean.length, glyphs,
                             (int)count, clusters, (int)count, 0);
      cairo_restore(ctx);
      cairo_glyph_free(glyphs);
      cairo_text_cluster_free(clusters);
      free(clean.data);
    }
  }
  return SNR_OK;
}
static snr_status recognition(cairo_t* ctx, cairo_font_face_t* face,
                              text_font* mapping, const sn_page* page,
                              double width, double height,
                              snr_text_work* work) {
  mapping->work = work;
  snr_spacing spacing;
  snr_status s = snr_spacing_create(page->recognition, width * 25.4 / 72,
                                    height * 25.4 / 72, work, &spacing);
  if (!s) {
    cairo_font_options_t* font_options = cairo_font_options_create();
    cairo_font_options_set_hint_metrics(font_options, CAIRO_HINT_METRICS_ON);
    cairo_font_options_set_hint_style(font_options, CAIRO_HINT_STYLE_NONE);
    cairo_set_font_options(ctx, font_options);
    cairo_font_options_destroy(font_options);
    cairo_push_group(ctx);
    s = recognition_draw(ctx, face, mapping, page, width, height, &spacing);
    cairo_pop_group_to_source(ctx);
    /* The ramp begins beyond the page. A soft mask retains the text in its
     * form while keeping every on-page point transparent under substitution. */
    cairo_pattern_t* mask = cairo_pattern_create_linear(0, 0, 0, height * 4);
    cairo_pattern_add_color_stop_rgba(mask, 0, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(mask, 0.5, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(mask, 1, 0, 0, 0, 1);
    cairo_mask(ctx, mask);
    cairo_pattern_destroy(mask);
  }
  free(spacing.boxes);
  return s;
}
static char* quote(sn_string s, snr_status* status) {
  if (s.length > (SIZE_MAX - 1) / 2) return nullptr;
  char* b = malloc(s.length * 2 + 1);
  if (!b) {
    *status = SNR_ERR_OUT_OF_MEMORY;
    return nullptr;
  }
  size_t j = 0;
  for (size_t i = 0; i < s.length; i++) {
    unsigned char c = (unsigned char)s.data[i];
    if (c < 32) {
      free(b);
      return nullptr;
    }
    if (c == '\'' || c == '\\') b[j++] = '\\';
    b[j++] = (char)c;
  }
  b[j] = 0;
  return b;
}
static snr_status links(cairo_t* ctx, const sn_notebook* n, const snr_plan* p,
                        size_t first, size_t count) {
  for (size_t i = 0; i < n->link_count; i++) {
    const sn_link* l = n->links + i;
    if (l->page_number != p->page->number || l->rect.is_null ||
        l->direction != 0)
      continue;
    sn_rect r = l->rect;
    if (!snr_box_clip(&r, p->page->size.width, p->page->size.height)) continue;
    if (l->kind == 4 && !snr_uri_allowed(l->url)) continue;
    double f = p->points_per_pixel;
    /* Cairo serializes link quads when finishing the document, using the
     * final sheet height. Offset their input to retain this sheet's origin. */
    const sn_page* last = n->pages + first + count - 1;
    double y = r.y * f + last->size.height * 72 / snr_page_dpi(n, last) -
               p->page->size.height * f;
    snr_status status = SNR_OK;
    char* uri = l->kind == 4 ? quote(l->url, &status) : nullptr;
    if (status) return status;
    size_t target = snr_link_target(n, l, first, count);
    if (!uri && target == SIZE_MAX) {
      snr_log(SNR_LOG_DEBUG, "link target outside exported document omitted");
      continue;
    }
    size_t size = uri ? strlen(uri) + 256 : 256;
    char* attr = malloc(size);
    if (!attr) {
      free(uri);
      return SNR_ERR_OUT_OF_MEMORY;
    }
    int formatted;
    if (uri)
      formatted = snprintf(attr, size, "rect=[%.8f %.8f %.8f %.8f] uri='%s'",
                           r.x * f, y, r.width * f, r.height * f, uri);
    else
      formatted =
          snprintf(attr, size, "rect=[%.8f %.8f %.8f %.8f] dest='page-%zu'",
                   r.x * f, y, r.width * f, r.height * f, target);
    if (formatted < 0 || (size_t)formatted >= size) {
      snr_log(SNR_LOG_DEBUG, "unrepresentable link omitted");
      free(attr);
      free(uri);
      continue;
    }
    cairo_tag_begin(ctx, CAIRO_TAG_LINK, attr);
    cairo_tag_end(ctx, CAIRO_TAG_LINK);
    free(attr);
    free(uri);
  }
  return SNR_OK;
}
static void outlines(cairo_surface_t* pdf, const sn_notebook* n, size_t first,
                     size_t count) {
  int parents[65] = {CAIRO_PDF_OUTLINE_ROOT};
  int64_t levels[65] = {0};
  size_t depth = 0;
  for (size_t i = 0; i < n->title_count; i++) {
    const sn_title* t = n->titles + i;
    size_t page = SIZE_MAX;
    for (size_t j = first; j < first + count; j++)
      if (n->pages[j].number == t->page_number) {
        page = j;
        break;
      }
    if (page == SIZE_MAX) continue;
    while (depth && levels[depth] >= t->level) depth--;
    char title[96], attr[96];
    snprintf(title, sizeof title, "Page %lld — title %zu",
             (long long)t->page_number, i + 1);
    snprintf(attr, sizeof attr, "dest='page-%zu'", page);
    int id = cairo_pdf_surface_add_outline(pdf, parents[depth], title, attr,
                                           CAIRO_PDF_OUTLINE_FLAG_OPEN);
    if (depth < 64) {
      depth++;
      parents[depth] = id;
      levels[depth] = t->level;
    }
  }
}
snr_status snr_backend_pdf(const sn_notebook* n, size_t first, size_t count,
                           const snr_options* o, snr_writer* w, snr_error* e) {
  snr_writer* sink = w;
  /* Cairo's default version (1.7 with Cairo 1.18) writes object and xref
   * streams. Restricting to 1.4 sends Cairo 1.18 through a finish path that
   * frees its output stream twice after any internal failure, including an
   * allocation failure, so the version is left at the default. */
  cairo_surface_t* pdf = cairo_pdf_surface_create_for_stream(put, sink, 1, 1);
  cairo_t* ctx = cairo_create(pdf);
  text_font* mapping = calloc(1, sizeof *mapping);
  snr_status s = mapping ? font_create(mapping) : SNR_ERR_OUT_OF_MEMORY;
  cairo_font_face_t* face = s ? nullptr
                              : cairo_ft_font_face_create_for_ft_face(
                                    mapping->face, FT_LOAD_NO_HINTING);
  if (face && cairo_font_face_status(face)) s = SNR_ERR_OUT_OF_MEMORY;
  bool attached = false;
  if (!s) {
    if (cairo_font_face_set_user_data(face, &font_key, mapping, font_free))
      s = SNR_ERR_OUT_OF_MEMORY;
    else
      attached = true;
  }
  cairo_pdf_surface_set_metadata(pdf, CAIRO_PDF_METADATA_TITLE,
                                 o->title ? o->title : "Supernote notebook");
  cairo_pdf_surface_set_metadata(pdf, CAIRO_PDF_METADATA_CREATOR, "snrender");
  cairo_pdf_surface_set_metadata(pdf, CAIRO_PDF_METADATA_CREATE_DATE,
                                 "2000-01-01T00:00:00Z");
  size_t active_page = first;
  snr_bitmap previous = {0};
  cairo_surface_t* previous_image = nullptr;
  if (o->progress) o->progress(o->progress_context, 0, count);
  /* Once the sink has failed nobody receives the document; stop adding pages
   * and let Cairo finish what it already holds. */
  for (size_t i = first; i < first + count && !s && !sink->status; i++) {
    active_page = i;
    snr_plan p;
    s = snr_plan_make(n, i, o, &p, e);
    if (s) break;
    snr_bitmap b = {0};
    s = snr_compose(&p, &b, e);
    if (s) break;
    double width = p.page->size.width * p.points_per_pixel,
           height = p.page->size.height * p.points_per_pixel;
    cairo_pdf_surface_set_size(pdf, width, height);
    /* Consecutive blank or duplicated sheets share their image object, so
     * the PDF need not store the same compressed pixels hundreds of times. */
    bool repeated = previous_image && previous.width == b.width &&
                    previous.height == b.height &&
                    !memcmp(previous.pixels, b.pixels, b.stride * b.height);
    cairo_surface_t* img =
        repeated ? cairo_surface_reference(previous_image) : surface(&b);
    if (!img) {
      snr_bitmap_free(&b);
      s = SNR_ERR_OUT_OF_MEMORY;
      break;
    }
    cairo_save(ctx);
    cairo_scale(ctx, width / (double)b.width, height / (double)b.height);
    cairo_set_source_surface(ctx, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(ctx), CAIRO_FILTER_NEAREST);
    cairo_paint(ctx);
    cairo_restore(ctx);
    if (!repeated) {
      if (previous_image) cairo_surface_destroy(previous_image);
      snr_bitmap_free(&previous);
      previous_image = cairo_surface_reference(img);
      previous = b;
      b = (snr_bitmap){0};
    }
    cairo_surface_destroy(img);
    snr_bitmap_free(&b);
    char dest[96];
    snprintf(dest, sizeof dest, "name='page-%zu' x=0 y=0", i);
    cairo_tag_begin(ctx, CAIRO_TAG_DEST, dest);
    cairo_tag_end(ctx, CAIRO_TAG_DEST);
    snr_text_work work = {p.work + n->title_count + n->link_count,
                          o->limits.maximum_work, &w->document_work,
                          o->limits.maximum_document_work};
    if (o->recognition)
      s = recognition(ctx, face, mapping, p.page, width, height, &work);
    if (s) break;
    s = links(ctx, n, &p, first, count);
    if (s) break;
    cairo_show_page(ctx);
    if (sink->status) break;
    if (cairo_status(ctx) != CAIRO_STATUS_SUCCESS) {
      s = cairo_status(ctx) == CAIRO_STATUS_NO_MEMORY ? SNR_ERR_OUT_OF_MEMORY
                                                      : SNR_ERR_BACKEND;
      break;
    }
    if (o->progress) o->progress(o->progress_context, i - first + 1, count);
  }
  bool font_limit = mapping && mapping->used == 65533;
  if (previous_image) cairo_surface_destroy(previous_image);
  snr_bitmap_free(&previous);
  /* The first failure wins. A sink that failed while pages were still being
   * added failed before, or during, any failure found since; a sink that
   * refuses only the bytes that close the document after an earlier failure
   * leaves that failure as the result. */
  bool sink_first = sink->status != SNR_OK;
  if (!s && !sink_first) outlines(pdf, n, first, count);
  cairo_destroy(ctx);
  if (face) cairo_font_face_destroy(face);
  cairo_surface_finish(pdf);
  /* Cairo itself has seen every write succeed; the sink is consulted here. */
  bool sink_failed = sink->status && (!s || sink_first);
  if (sink_failed) s = sink->status;
  if (!s && cairo_surface_status(pdf) != CAIRO_STATUS_SUCCESS)
    s = cairo_surface_status(pdf) == CAIRO_STATUS_NO_MEMORY
            ? SNR_ERR_OUT_OF_MEMORY
            : SNR_ERR_BACKEND;
  cairo_surface_destroy(pdf);
  if (!attached && mapping) font_free(mapping);

  if (!s && snr_cancelled(o)) s = SNR_ERR_CANCELLED;

  if (sink_failed)
    snr_fail(e, s, active_page, snr_writer_message(sink));
  else if (s && (!e || !e->code))
    snr_fail(
        e, s, active_page,
        s == SNR_ERR_LIMIT
            ? (font_limit ? "recognition font exceeds 65,533 distinct entries"
                          : "PDF work limit exceeded")
            : "PDF page output failed");
  return s;
}

/* One raster page, shared by optional drawing formats. */
snr_status snr_encode_pdf_bitmap(const snr_bitmap* b, double width,
                                 double height, snr_writer* w) {
  cairo_surface_t* pdf =
      cairo_pdf_surface_create_for_stream(put, w, width, height);
  cairo_t* ctx = cairo_create(pdf);
  cairo_surface_t* img = surface(b);
  snr_status s = img ? SNR_OK : SNR_ERR_OUT_OF_MEMORY;
  if (!s) {
    cairo_scale(ctx, width / (double)b->width, height / (double)b->height);
    cairo_set_source_surface(ctx, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(ctx), CAIRO_FILTER_NEAREST);
    cairo_paint(ctx);
    cairo_show_page(ctx);
    if (cairo_status(ctx))
      s = cairo_status(ctx) == CAIRO_STATUS_NO_MEMORY ? SNR_ERR_OUT_OF_MEMORY
                                                      : SNR_ERR_BACKEND;
  }
  if (img) cairo_surface_destroy(img);
  cairo_destroy(ctx);
  cairo_surface_finish(pdf);
  cairo_status_t status = cairo_surface_status(pdf);
  cairo_surface_destroy(pdf);
  if (w->status) return w->status;
  if (s) return s;
  return status == CAIRO_STATUS_NO_MEMORY ? SNR_ERR_OUT_OF_MEMORY
         : status                         ? SNR_ERR_BACKEND
                                          : SNR_OK;
}
