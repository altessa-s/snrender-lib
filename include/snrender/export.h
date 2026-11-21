/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef SNRENDER_EXPORT_H
#define SNRENDER_EXPORT_H
/* SNR_API marks the exported functions. On Windows the build defines
 * SNRENDER_BUILDING inside a DLL and gives its users SNRENDER_SHARED; a static
 * library needs neither. Elsewhere symbols are hidden unless marked. */
#if defined(_WIN32) && defined(SNRENDER_BUILDING)
#define SNR_API __declspec(dllexport)
#elif defined(_WIN32) && defined(SNRENDER_SHARED)
#define SNR_API __declspec(dllimport)
#elif defined(_WIN32)
#define SNR_API
#elif defined(__GNUC__) || defined(__clang__)
#define SNR_API __attribute__((visibility("default")))
#else
#define SNR_API
#endif

#endif
