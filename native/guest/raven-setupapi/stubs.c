/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>

/* Exports with no meaning in a runtime without plug and play, drivers or catalogs. Each pops its
 * caller's arguments and reports that nothing was done, so an import resolves and a call fails the
 * way the API fails. stubs.inc lists every one with its count of argument words. */

#define ARGS_0 void
#define ARGS_1 UINT_PTR a
#define ARGS_2 UINT_PTR a, UINT_PTR b
#define ARGS_3 ARGS_2, UINT_PTR c
#define ARGS_4 ARGS_3, UINT_PTR d
#define ARGS_5 ARGS_4, UINT_PTR e
#define ARGS_6 ARGS_5, UINT_PTR f
#define ARGS_7 ARGS_6, UINT_PTR g
#define ARGS_8 ARGS_7, UINT_PTR h
#define ARGS_9 ARGS_8, UINT_PTR i
#define ARGS_10 ARGS_9, UINT_PTR j
#define ARGS_11 ARGS_10, UINT_PTR k
#define ARGS_12 ARGS_11, UINT_PTR l
#define ARGS_13 ARGS_12, UINT_PTR m
#define ARGS_14 ARGS_13, UINT_PTR n

#define CR_CALL_NOT_IMPLEMENTED 0x00000078

#define CM(name, words) DWORD WINAPI name(ARGS_##words) { return CR_CALL_NOT_IMPLEMENTED; }
#define FAIL(name, words) UINT_PTR WINAPI name(ARGS_##words) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return 0; }

#include "stubs.inc"
