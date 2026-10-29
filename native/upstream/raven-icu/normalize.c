/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The Unicode normalization of kernel32, NormalizeString and IsNormalizedString, on ICU's normalizers: linked into the runtime's icu.dll,
 * which kernel32 forwards the two functions to. Behaviour as the Windows documentation gives it: a length of -1 takes the string to its
 * terminating null and counts that null in the result, a destination length of 0 asks for the length needed, a buffer too small
 * answers ERROR_INSUFFICIENT_BUFFER and the negated length needed, and an unpaired surrogate answers ERROR_NO_UNICODE_TRANSLATION and
 * its negated position. Success leaves the last error alone, but for a string IsNormalizedString finds not normalized, which answers
 * ERROR_SUCCESS. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unicode/unorm2.h>

static const UNormalizer2 *normalizer(NORM_FORM form)
{
	UErrorCode status = U_ZERO_ERROR;
	const UNormalizer2 *found = NULL;
	switch (form) {
	case NormalizationC: found = unorm2_getNFCInstance(&status); break;
	case NormalizationD: found = unorm2_getNFDInstance(&status); break;
	case NormalizationKC: found = unorm2_getNFKCInstance(&status); break;
	case NormalizationKD: found = unorm2_getNFKDInstance(&status); break;
	default: return NULL;
	}
	return U_SUCCESS(status) ? found : NULL;
}

/* The source length with its null when it is given as -1, or -1 for a malformed one. */
static int source_length(LPCWSTR source, int length)
{
	if (!source || length < -1 || !length)
		return -1;
	if (length == -1)
		for (length = 0;; ++length)
			if (!source[length])
				return length + 1;
	return length;
}

/* The position of the first unpaired surrogate, or -1 when there is none. */
static int unpaired(LPCWSTR text, int length)
{
	for (int index = 0; index < length; ++index) {
		if (IS_HIGH_SURROGATE(text[index])) {
			if (index + 1 < length && IS_LOW_SURROGATE(text[index + 1])) {
				++index;
				continue;
			}
			return index;
		}
		if (IS_LOW_SURROGATE(text[index]))
			return index;
	}
	return -1;
}

__declspec(dllexport) int WINAPI NormalizeString(NORM_FORM form, LPCWSTR source, int source_count, LPWSTR destination, int destination_count)
{
	/* ICU's first use initializes it, which may leave a last error of its own behind. */
	const DWORD last_error = GetLastError();
	const UNormalizer2 *norm = normalizer(form);
	const int length = source_length(source, source_count);
	if (!norm || length < 0 || destination_count < 0 || (destination_count && !destination)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	const int bad = unpaired(source, length);
	if (bad >= 0) {
		SetLastError(ERROR_NO_UNICODE_TRANSLATION);
		return -bad;
	}
	/* ICU writes no terminator of its own: a null in the source is a character it keeps, so the -1 form counts it as Windows does. */
	UErrorCode status = U_ZERO_ERROR;
	const int needed = unorm2_normalize(norm, (const UChar *)source, length, (UChar *)destination, destination_count, &status);
	if (status == U_BUFFER_OVERFLOW_ERROR || (U_SUCCESS(status) && needed > destination_count)) {
		if (!destination_count) {
			SetLastError(last_error);
			return needed;
		}
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return -needed;
	}
	if (U_FAILURE(status)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	SetLastError(last_error);
	return needed;
}

__declspec(dllexport) BOOL WINAPI IsNormalizedString(NORM_FORM form, LPCWSTR source, int source_count)
{
	const DWORD last_error = GetLastError();
	const UNormalizer2 *norm = normalizer(form);
	const int length = source_length(source, source_count);
	if (!norm || length < 0) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (unpaired(source, length) >= 0) {
		SetLastError(ERROR_NO_UNICODE_TRANSLATION);
		return FALSE;
	}
	UErrorCode status = U_ZERO_ERROR;
	const UBool normalized = unorm2_isNormalized(norm, (const UChar *)source, length, &status);
	if (U_FAILURE(status)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	SetLastError(normalized ? last_error : ERROR_SUCCESS);
	return normalized ? TRUE : FALSE;
}
