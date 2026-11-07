/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_VBSCRIPT_REGEX_H
#define RAVEN_VBSCRIPT_REGEX_H

#include <windows.h>

/* Regular expressions in the syntax the script languages share (that of ECMAScript 3): alternation, greedy and lazy quantifiers,
 * capturing and non-capturing groups, lookahead, back references, classes and the \d \w \s escapes, anchors and word boundaries. A
 * backtracking matcher runs them; it depends on nothing but Windows itself so either engine can build it. */

typedef struct rx rx;

#define RX_IGNORE_CASE 1
#define RX_MULTILINE 2

/* The errors of a pattern, numbered as the script engines report them. */
#define RX_E_SYNTAX 5017
#define RX_E_BRACKET 5019
#define RX_E_PARENTHESIS 5020

/* Compiles a pattern; NULL with *error set when it is not one. */
rx *rx_compile(const WCHAR *pattern, UINT length, DWORD flags, LONG *error);
void rx_free(rx *re);
/* The number of capturing groups, without the whole match. */
UINT rx_groups(const rx *re);
/* Finds the first match at or after start. On success spans[2 * n] and spans[2 * n + 1] are where group n starts and ends, -1 for a
 * group that took no part; group 0 is the whole match. spans holds 2 * (rx_groups + 1) entries. */
BOOL rx_search(const rx *re, const WCHAR *text, UINT length, UINT start, int *spans);

#endif
