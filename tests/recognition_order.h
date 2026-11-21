/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#ifndef RECOGNITION_ORDER_H
#define RECOGNITION_ORDER_H
#include <snrender/snrender.h>
#define ORDER_TEXT(s) {s, sizeof(s) - 1}
/* Deliberately uneven boxes, touching punctuation, and unboxed separators.
 * These are synthetic recognition words, independent of notebook fixtures. */
static const sn_word order_words[] = {
    {ORDER_TEXT("Al\xc2\xadpha"), {8, 10, 15, 6, false}},
    {ORDER_TEXT(" "), {.is_null = true}},
    {ORDER_TEXT("Be\xe2\x80\x8bta"), {25, 11, 12, 5, false}},
    {ORDER_TEXT(" "), {.is_null = true}},
    {ORDER_TEXT(":"), {36, 12, 2, 3, false}},
    {ORDER_TEXT(" "), {.is_null = true}},
    {ORDER_TEXT("Ga\xe2\x80\x8cmma"), {41, 10.5, 20, 6.5, false}},
    {ORDER_TEXT(" "), {.is_null = true}},
    {ORDER_TEXT("Del\xe2\x80\x8dta"), {64, 9.5, 16, 7, false}},
    {ORDER_TEXT(" "), {.is_null = true}},
    {ORDER_TEXT("."), {79, 13, 2, 2, false}},
    {ORDER_TEXT("\n"), {.is_null = true}},
    {ORDER_TEXT("Ep\xe2\x81\xa0silon"), {8, 18, 22, 7, false}},
    {ORDER_TEXT(" "), {.is_null = true}},
    {ORDER_TEXT("Ze\xef\xbb\xbf"
                "ta"),
     {33, 19, 12, 5, false}},
    {ORDER_TEXT(" "), {.is_null = true}},
    {ORDER_TEXT("Eta"), {48, 17.5, 12, 6, false}},
    {ORDER_TEXT("\n"), {.is_null = true}},
    {ORDER_TEXT("\xd0\x96\xd1\x91\xd0\xbb\xd1\x82\xd1\x8b\xd0\xb9"),
     {8, 26, 22, 6, false}},
    {ORDER_TEXT(" "), {.is_null = true}},
    {ORDER_TEXT("\xd1\x86\xd0\xb2\xd0\xb5\xd1\x82"), {33, 27, 16, 5, false}},
};
static const char order_expected[] =
    "Alpha Beta : Gamma Delta .\nEpsilon Zeta Eta\n"
    "\xd0\x96\xd1\x91\xd0\xbb\xd1\x82\xd1\x8b\xd0\xb9 "
    "\xd1\x86\xd0\xb2\xd0\xb5\xd1\x82";
static const sn_element order_element = {
    .label = ORDER_TEXT(order_expected),
    .words = order_words,
    .word_count = sizeof order_words / sizeof *order_words};
#undef ORDER_TEXT
#endif
