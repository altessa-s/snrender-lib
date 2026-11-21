/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Replays files through the fuzz target without libFuzzer, so the ordinary
 * suites keep every operation of the target building and running clean under
 * whatever sanitizers the configuration uses. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_render.h"

static size_t executions;
static bool hashed;
static uint32_t selector;

static bool replay_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  uint8_t* data = nullptr;
  size_t size = 0, capacity = 0;
  for (;;) {
    if (size == capacity) {
      capacity = capacity ? capacity * 2 : 65536;
      uint8_t* grown = realloc(data, capacity);
      if (!grown) break;
      data = grown;
    }
    size_t read = fread(data + size, 1, capacity - size, f);
    size += read;
    if (!read) break;
  }
  bool ok = !ferror(f) && size < capacity;
  if (fclose(f)) ok = false;
  if (ok) {
    (void)fuzz_render(data, size,
                      hashed ? fuzz_selector(data, size) : selector);
    executions++;
  }
  free(data);
  return ok;
}

/* Directories are read one level deep, as libFuzzer reads a corpus. */
static bool replay(const char* path) {
  DIR* directory = opendir(path);
  if (!directory) return errno == ENOTDIR && replay_file(path);
  bool ok = true;
  for (struct dirent* entry; ok && (entry = readdir(directory));) {
    if (entry->d_name[0] == '.') continue;
    size_t length = strlen(path) + strlen(entry->d_name) + 2;
    char* child = malloc(length);
    if (!child) {
      ok = false;
      break;
    }
    snprintf(child, length, "%s/%s", path, entry->d_name);
    DIR* nested = opendir(child);
    if (nested)
      ok = !closedir(nested);
    else
      ok = replay_file(child);
    if (!ok) fprintf(stderr, "cannot replay %s\n", child);
    free(child);
  }
  return !closedir(directory) && ok;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s selector|hash path...\n", argv[0]);
    return 2;
  }
  hashed = !strcmp(argv[1], "hash");
  if (!hashed) {
    char* end;
    unsigned long value = strtoul(argv[1], &end, 10);
    if (*end || value > UINT32_MAX) return 2;
    selector = (uint32_t)value;
  }
  for (int i = 2; i < argc; i++)
    if (!replay(argv[i])) {
      fprintf(stderr, "cannot replay %s\n", argv[i]);
      return 1;
    }
  if (!executions) return 1;
  printf("%zu executions with selector %s\n", executions, argv[1]);
  return 0;
}
