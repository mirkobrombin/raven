/* SPDX-License-Identifier: MIT */
#include "internal.h"

/* Unicode normalization and internationalized domain names, on the system's NormalizeString and IdnToAscii. */

/* A normalizer is one of the four forms; its address is the form's entry in this table. */
typedef struct UNormalizer2 {
	NORM_FORM form;
} UNormalizer2;

static const UNormalizer2 forms[] = {{NormalizationC}, {NormalizationD}, {NormalizationKC}, {NormalizationKD}};

const UNormalizer2 *__cdecl unorm2_getNFCInstance(UErrorCode *status)
{
	return U_FAILED(*status) ? NULL : &forms[0];
}

const UNormalizer2 *__cdecl unorm2_getNFDInstance(UErrorCode *status)
{
	return U_FAILED(*status) ? NULL : &forms[1];
}

const UNormalizer2 *__cdecl unorm2_getNFKCInstance(UErrorCode *status)
{
	return U_FAILED(*status) ? NULL : &forms[2];
}

const UNormalizer2 *__cdecl unorm2_getNFKDInstance(UErrorCode *status)
{
	return U_FAILED(*status) ? NULL : &forms[3];
}

INT __cdecl unorm2_normalize(const UNormalizer2 *normalizer, const UChar *source, INT length, UChar *dest, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	if (!normalizer || (!source && length) || capacity < 0 || (!dest && capacity)) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	INT count = icu_length(source, length);
	if (!count) {
		if (dest && capacity > 0)
			dest[0] = 0;
		return icu_finish(0, capacity, status);
	}
	INT needed = NormalizeString(normalizer->form, source, count, NULL, 0);
	if (needed <= 0) {
		*status = U_INVALID_CHAR_FOUND;
		return 0;
	}
	// The size asked first is an estimate; the text itself says how long it came out.
	UChar *result = icu_alloc(needed * sizeof(UChar));
	if (!result) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return 0;
	}
	INT made = NormalizeString(normalizer->form, source, count, result, needed);
	if (made <= 0) {
		icu_free(result);
		*status = U_INVALID_CHAR_FOUND;
		return 0;
	}
	if (dest)
		CopyMemory(dest, result, (made < capacity ? made : capacity) * sizeof(UChar));
	if (made < capacity)
		dest[made] = 0;
	icu_free(result);
	return icu_finish(made, capacity, status);
}

UBool __cdecl unorm2_isNormalized(const UNormalizer2 *normalizer, const UChar *source, INT length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	if (!normalizer || (!source && length)) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	INT count = icu_length(source, length);
	return count ? (UBool)(IsNormalizedString(normalizer->form, source, count) != 0) : 1;
}

/* IDNA after UTS #46: a domain name to its ASCII form and back. Errors are bits of UIDNAInfo, as the result is still returned. */

enum { UIDNA_USE_STD3_RULES = 2, UIDNA_ERROR_DISALLOWED = 0x80 };

typedef struct UIDNA {
	DWORD flags;
} UIDNA;

typedef struct UIDNAInfo {
	SHORT size;
	UBool transitional_different;
	UBool reserved_b3;
	UINT errors;
	INT reserved_i2;
	INT reserved_i3;
} UIDNAInfo;

UIDNA *__cdecl uidna_openUTS46(UINT options, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	UIDNA *idna = icu_alloc(sizeof *idna);
	if (!idna) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	idna->flags = options & UIDNA_USE_STD3_RULES ? IDN_USE_STD3_ASCII_RULES : 0;
	return idna;
}

void __cdecl uidna_close(UIDNA *idna)
{
	icu_free(idna);
}

static INT convert_name(const UIDNA *idna, BOOL to_ascii, const UChar *name, INT length, UChar *dest, INT capacity, UIDNAInfo *info,
	UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	if (!idna || !name || capacity < 0 || (!dest && capacity) || !info || info->size < (SHORT)sizeof(UIDNAInfo)) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	info->transitional_different = 0;
	info->errors = 0;
	INT count = icu_length(name, length);
	UChar result[256];
	INT made = count ? (to_ascii ? IdnToAscii : IdnToUnicode)(idna->flags, name, count, result, ARRAYSIZE(result)) : 0;
	if (count && made <= 0) {
		// The name as given, with the error that it holds what IDNA does not allow.
		info->errors = UIDNA_ERROR_DISALLOWED;
		made = count < (INT)ARRAYSIZE(result) ? count : (INT)ARRAYSIZE(result);
		CopyMemory(result, name, made * sizeof(UChar));
	}
	if (dest)
		CopyMemory(dest, result, (made < capacity ? made : capacity) * sizeof(UChar));
	if (made < capacity)
		dest[made] = 0;
	return icu_finish(made, capacity, status);
}

INT __cdecl uidna_nameToASCII(const UIDNA *idna, const UChar *name, INT length, UChar *dest, INT capacity, UIDNAInfo *info, UErrorCode *status)
{
	return convert_name(idna, TRUE, name, length, dest, capacity, info, status);
}

INT __cdecl uidna_nameToUnicode(const UIDNA *idna, const UChar *name, INT length, UChar *dest, INT capacity, UIDNAInfo *info, UErrorCode *status)
{
	return convert_name(idna, FALSE, name, length, dest, capacity, info, status);
}
