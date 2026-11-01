/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_ICU_INTERNAL_H
#define RAVEN_ICU_INTERNAL_H
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

/* ICU's C interface as its public documentation gives it, on Raven's own text functions. Objects are opaque to callers, so they
 * are laid out for this code; only the function signatures and the numeric constants are ICU's. */

typedef WCHAR UChar;
typedef INT UChar32;
typedef INT UErrorCode;
typedef signed char UBool;

#define U_ZERO_ERROR 0
#define U_STRING_NOT_TERMINATED_WARNING (-124)
#define U_ILLEGAL_ARGUMENT_ERROR 1
#define U_MEMORY_ALLOCATION_ERROR 7
#define U_INDEX_OUTOFBOUNDS_ERROR 8
#define U_INVALID_CHAR_FOUND 10
#define U_BUFFER_OVERFLOW_ERROR 15
#define U_UNSUPPORTED_ERROR 16
#define U_REGEX_RULE_SYNTAX 0x10301
#define U_REGEX_INVALID_STATE 0x10302
#define U_REGEX_BAD_ESCAPE_SEQUENCE 0x10303
#define U_REGEX_UNIMPLEMENTED 0x10305
#define U_REGEX_MISMATCHED_PAREN 0x10306
#define U_REGEX_BAD_INTERVAL 0x10308
#define U_REGEX_MAX_LT_MIN 0x10309
#define U_REGEX_INVALID_BACK_REF 0x1030a
#define U_REGEX_INVALID_FLAG 0x1030b
#define U_REGEX_MISSING_CLOSE_BRACKET 0x1030f
#define U_REGEX_INVALID_RANGE 0x10310
#define U_REGEX_STACK_OVERFLOW 0x10311
#define U_REGEX_TIME_OUT 0x10312

#define U_FAILED(code) ((code) > U_ZERO_ERROR)
#define U_SENTINEL (-1)

/* A text in UTF-16. native holds, for a text opened from UTF-8, the byte offset of each code unit. */
typedef struct UText {
	UChar *units;
	INT length;
	INT64 *native;
	INT64 native_length;
} UText;

void *icu_alloc(SIZE_T size);
void icu_free(void *memory);
INT icu_finish(INT length, INT capacity, UErrorCode *status);
INT icu_length(const UChar *text, INT length);
UChar32 icu_at(const UChar *text, INT length, INT index, INT *next);
UChar32 icu_lower(UChar32 c);
UChar32 icu_upper(UChar32 c);
BOOL icu_is_letter(UChar32 c);
BOOL icu_is_digit(UChar32 c);
BOOL icu_is_space(UChar32 c);
BOOL icu_is_word(UChar32 c);
BOOL icu_is_mark(UChar32 c);
WORD icu_kind(UChar32 c);
INT icu_utext_units(const UText *text, INT64 native);
INT64 icu_utext_native(const UText *text, INT units);
UText *icu_utext_copy(const UText *text);

UText *__cdecl utext_openUChars(UText *reuse, const UChar *text, INT64 length, UErrorCode *status);
UText *__cdecl utext_close(UText *text);
#endif
