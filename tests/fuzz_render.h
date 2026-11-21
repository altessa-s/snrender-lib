/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef SNR_FUZZ_RENDER_H
#define SNR_FUZZ_RENDER_H
#include <stddef.h>
#include <stdint.h>

/* One execution renders one page or one page range through one entry point.
 * The selector's remainder modulo FUZZ_OPERATIONS names the entry point, so
 * selectors 0 through FUZZ_OPERATIONS - 1 reach every operation with the plain
 * options; higher digits choose the page and vary the options. */
constexpr uint32_t FUZZ_OPERATIONS = 10;
/* Every selector runs the operation above exactly as before overlays existed.
 * An input that parses as a mark then also runs one overlay operation over a
 * fixed source PDF: export to buffer and to output, bitmap, PNG to buffer and
 * to output, and the annotation query, chosen by the remainder modulo
 * FUZZ_OVERLAY_OPERATIONS of the selector without its top bit. The replay
 * tests name overlay operation k with FUZZ_OVERLAY_SELECTOR + k. */
constexpr uint32_t FUZZ_OVERLAY_SELECTOR = 0x80000000u;
constexpr uint32_t FUZZ_OVERLAY_OPERATIONS = 6;
/* Inputs above the nightly -max_len are ignored rather than parsed, so a
 * different command line cannot make one execution unexpectedly expensive. */
constexpr size_t FUZZ_MAXIMUM_INPUT = 8388608;
/* Hash of the whole input; every mutation therefore draws a fresh operation
 * and option set without the notebook bytes losing their footer address. */
uint32_t fuzz_selector(const uint8_t* data, size_t size);
/* Run one selected operation. Returns zero; violated API contracts abort. */
int fuzz_render(const uint8_t* data, size_t size, uint32_t selector);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
#endif
