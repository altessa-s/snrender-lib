/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <snrender/snrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Exclusive creation in the destination directory keeps failed exports from
 * touching the old file and permits an atomic replacement on success. */
static FILE* temporary_output(const char* destination, char** path) {
  *path = nullptr;
#ifdef _WIN32
  DWORD attributes = GetFileAttributesA(destination);
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_READONLY)) {
    errno = EACCES;
    return nullptr;
  }
#else
  struct stat destination_stat;
  if (stat(destination, &destination_stat) == 0 && access(destination, W_OK))
    return nullptr;
#endif
  size_t size = strlen(destination) + 80;
  *path = malloc(size);
  if (!*path) return nullptr;
#ifdef _WIN32
  for (unsigned i = 0; i < 1000; i++) {
    snprintf(*path, size, "%s.snrender-%d-%u.tmp", destination, _getpid(), i);
    FILE* file = fopen(*path, "wbx");
    if (file) return file;
    if (errno != EEXIST) break;
  }
#else
  snprintf(*path, size, "%s.snrender-XXXXXX", destination);
  int fd = mkstemp(*path);
  if (fd >= 0) {
    struct stat existing;
    mode_t mask = umask(0);
    umask(mask);
    mode_t mode = stat(destination, &existing) == 0 ? existing.st_mode & 07777
                                                    : 0666 & ~mask;
    if (fchmod(fd, mode)) {
      int error = errno;
      fprintf(stderr, "warning: output permissions: %s\n", strerror(error));
    }
    FILE* file = fdopen(fd, "wb");
    if (file) return file;
    int error = errno;
    close(fd);
    remove(*path);
    errno = error;
  }
#endif
  int error = errno;
  free(*path);
  *path = nullptr;
  errno = error;
  return nullptr;
}
static bool replace_output(const char* temporary, const char* destination) {
#ifdef _WIN32
  return MoveFileExA(temporary, destination, MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return rename(temporary, destination) == 0;
#endif
}
/* Overlay warnings say when annotations may be misplaced or were dropped;
 * debug notes about omitted optional content stay quiet. */
static void log_message(snr_log_level level, const char* message,
                        void* context) {
  (void)context;
  if (level == SNR_LOG_WARNING) fprintf(stderr, "warning: %s\n", message);
  if (level == SNR_LOG_ERROR) fprintf(stderr, "error: %s\n", message);
}
static snr_status write_file(void* context, const uint8_t* bytes, size_t size) {
  return fwrite(bytes, 1, size, context) == size ? SNR_OK : SNR_ERR_BACKEND;
}
int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: snrender INPUT OUTPUT.{png,pdf,svg} [PAGE (1-based)] "
            "[SCALE] [--maximum-pixels N] [--maximum-decoded-pixels N]\n"
            "[--maximum-document-work N] [--maximum-pages N] [--maximum-work "
            "N] [--maximum-output-bytes N]\n"
            "[--minimum-scale N] [--maximum-scale N] [--strict-images] "
            "[--source-pdf FILE] [--source-kind pdf|epub] [--strict] "
            "[--maximum-source-bytes N]\n");
    return 2;
  }
  FILE* f = fopen(argv[1], "rb");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  if (fseek(f, 0, SEEK_END)) {
    fclose(f);
    return 1;
  }
  long length = ftell(f);
  rewind(f);
  if (length <= 0 || (unsigned long)length > 536870912) {
    fclose(f);
    return 1;
  }
  uint8_t* bytes = malloc((size_t)length);
  if (!bytes) {
    fclose(f);
    return 1;
  }
  size_t got = fread(bytes, 1, (size_t)length, f);
  fclose(f);
  sn_notebook* n = nullptr;
  sn_error err;
  sn_status parsed = sn_parse_notebook(bytes, got, &n, &err);
  free(bytes);
  if (parsed) {
    fprintf(stderr, "%s\n", err.message);
    return 1;
  }
  snr_overlay_options overlay = snr_overlay_options_default();
  const char* source_path = nullptr;
  size_t page = 0;
  snr_options o = snr_options_default();
  char* end = nullptr;
  int next = 3;
  bool single_page = false;
  /* Overlay options apply only with a source PDF; naming the last one given
   * lets the tool refuse them rather than ignore them. */
  const char* overlay_option = nullptr;
  if (next < argc && strncmp(argv[next], "--", 2)) {
    single_page = true;
    unsigned long long value = strtoull(argv[next++], &end, 10);
    if (!value || *end || value > SIZE_MAX) {
      sn_notebook_free(n);
      return 2;
    }
    page = (size_t)value - 1;
  }
  if (next < argc && strncmp(argv[next], "--", 2)) {
    o.scale = strtod(argv[next++], &end);
    if (*end) {
      sn_notebook_free(n);
      return 2;
    }
  }
  while (next < argc) {
    const char* key = argv[next++];
    if (!strcmp(key, "--strict")) {
      overlay.strict = true;
      overlay_option = "--strict";
      continue;
    }
    if (!strcmp(key, "--strict-images")) {
      o.strict_images = true;
      continue;
    }
    if (next == argc) {
      sn_notebook_free(n);
      return 2;
    }
    const char* value = argv[next++];
    if (!strcmp(key, "--source-pdf")) {
      source_path = value;
      continue;
    }
    if (!strcmp(key, "--source-kind")) {
      if (!strcmp(value, "pdf"))
        overlay.source_kind = SNR_DOCUMENT_PDF;
      else if (!strcmp(value, "epub"))
        overlay.source_kind = SNR_DOCUMENT_EPUB;
      else {
        fprintf(stderr, "--source-kind must be pdf or epub, not %s\n", value);
        sn_notebook_free(n);
        return 2;
      }
      overlay_option = "--source-kind";
      continue;
    }
    size_t* limit = nullptr;
    if (!strcmp(key, "--maximum-pixels"))
      limit = &o.limits.maximum_pixels;
    else if (!strcmp(key, "--maximum-decoded-pixels"))
      limit = &o.limits.maximum_decoded_pixels;
    else if (!strcmp(key, "--maximum-pages"))
      limit = &o.limits.maximum_pages;
    else if (!strcmp(key, "--maximum-work"))
      limit = &o.limits.maximum_work;
    else if (!strcmp(key, "--maximum-output-bytes"))
      limit = &o.limits.maximum_output_bytes;
    else if (!strcmp(key, "--maximum-source-bytes"))
      limit = &overlay.limits.maximum_source_bytes;
    if (!strcmp(key, "--maximum-document-work")) {
      errno = 0;
      unsigned long long v = strtoull(value, &end, 10);
      if (errno || value == end || *end || *value == '-') {
        sn_notebook_free(n);
        return 2;
      }
      o.limits.maximum_document_work = v;
      continue;
    }
    errno = 0;
    if (limit) {
      unsigned long long parsed_limit = strtoull(value, &end, 10);
      if (errno || value == end || *end || *value == '-' ||
          parsed_limit > SIZE_MAX) {
        sn_notebook_free(n);
        return 2;
      }
      *limit = (size_t)parsed_limit;
    } else {
      double parsed_limit = strtod(value, &end);
      if (errno || value == end || *end) {
        sn_notebook_free(n);
        return 2;
      }
      if (!strcmp(key, "--minimum-scale"))
        o.limits.minimum_scale = parsed_limit;
      else if (!strcmp(key, "--maximum-scale"))
        o.limits.maximum_scale = parsed_limit;
      else {
        sn_notebook_free(n);
        return 2;
      }
    }
  }
  if (!source_path && overlay_option) {
    fprintf(stderr, "%s requires --source-pdf\n", overlay_option);
    sn_notebook_free(n);
    return 2;
  }
  if (!source_path && page >= n->page_count) {
    sn_notebook_free(n);
    return 2;
  }
  const char* ext = strrchr(argv[2], '.');
  if (!ext ||
      (strcmp(ext, ".pdf") && strcmp(ext, ".png") && strcmp(ext, ".svg"))) {
    sn_notebook_free(n);
    return 2;
  }
  snr_buffer source = {0};
  if (source_path) {
    // Checked before opening: Windows refuses to open a directory with
    // "Permission denied", which would hide what is wrong with the path.
    struct stat info;
    if (stat(source_path, &info)) {
      perror(source_path);
      sn_notebook_free(n);
      return 1;
    }
    if (!S_ISREG(info.st_mode)) {
      fprintf(stderr, "%s: source PDF is not a regular file\n", source_path);
      sn_notebook_free(n);
      return 1;
    }
    FILE* input = fopen(source_path, "rb");
    if (!input) {
      perror(source_path);
      sn_notebook_free(n);
      return 1;
    }
    long source_size = -1;
    if (!fseek(input, 0, SEEK_END)) source_size = ftell(input);
    const char* problem =
        source_size < 0 || fseek(input, 0, SEEK_SET) ? "is unreadable"
        : !source_size                               ? "is empty"
        : (unsigned long)source_size > overlay.limits.maximum_source_bytes
            ? "exceeds the source byte limit"
            : nullptr;
    if (problem) {
      fprintf(stderr, "%s: source PDF %s\n", source_path, problem);
      fclose(input);
      sn_notebook_free(n);
      return 1;
    }
    source.bytes = malloc((size_t)source_size);
    if (source.bytes)
      source.size = fread(source.bytes, 1, (size_t)source_size, input);
    fclose(input);
    if (source.size != (size_t)source_size) {
      fprintf(stderr, "%s: source PDF read or allocation failed\n",
              source_path);
      snr_buffer_free(&source);
      sn_notebook_free(n);
      return 1;
    }
  }
  char* temporary = nullptr;
  f = temporary_output(argv[2], &temporary);
  if (!f) {
    perror(argv[2]);
    snr_buffer_free(&source);
    sn_notebook_free(n);
    return 1;
  }
  snr_output output = {SNR_OUTPUT_ABI, sizeof output, write_file, f};
  snr_error error = {0};
  snr_status s;
  if (source_path) snr_log_install(log_message, nullptr);
  overlay.render = o;
  if (source_path && !strcmp(ext, ".pdf"))
    s = snr_mark_document_pdf_to_output(n, source.bytes, source.size, page,
                                        single_page ? 1 : SIZE_MAX, &overlay,
                                        &output, &error);
  else if (source_path && !strcmp(ext, ".png"))
    s = snr_mark_page_png_to_output(n, source.bytes, source.size, page,
                                    &overlay, &output, &error);
  else if (source_path) {
    s = SNR_ERR_UNSUPPORTED;
    snprintf(error.message, sizeof error.message,
             "source PDF supports PDF and PNG output");
  } else if (!strcmp(ext, ".pdf"))
    s = snr_document_pdf_to_output(n, page, single_page ? 1 : SIZE_MAX, &o,
                                   &output, &error);
  else if (!strcmp(ext, ".png"))
    s = snr_page_png_to_output(n, page, &o, &output, &error);
  else
    s = snr_page_svg_to_output(n, page, &o, &output, &error);
  snr_buffer_free(&source);
  if (fclose(f) && !s) s = SNR_ERR_BACKEND;
  sn_notebook_free(n);
  if (!s && !replace_output(temporary, argv[2])) {
    s = SNR_ERR_BACKEND;
    snprintf(error.message, sizeof error.message,
             "destination replacement failed");
  }
  if (s) {
    remove(temporary);
    free(temporary);
    fprintf(stderr, "%s: %s\n", snr_status_string(s), error.message);
    return 1;
  }
  free(temporary);
  return 0;
}
