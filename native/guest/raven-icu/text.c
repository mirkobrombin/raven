/* SPDX-License-Identifier: MIT */
#include "internal.h"

/* UText: ICU reads a text through this object. Here it is always a private UTF-16 copy, so nothing keeps the caller's buffer alive. */

static UText *fail_open(UText *text, UErrorCode *status, UErrorCode code)
{
	*status = code;
	return utext_close(text);
}

static UText *utext_new(UErrorCode *status)
{
	UText *text = icu_alloc(sizeof *text);
	if (!text)
		*status = U_MEMORY_ALLOCATION_ERROR;
	return text;
}

UText *__cdecl utext_close(UText *text)
{
	if (text) {
		icu_free(text->units);
		icu_free(text->native);
		icu_free(text);
	}
	return NULL;
}

UText *__cdecl utext_openUChars(UText *reuse, const UChar *units, INT64 length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	if (reuse) {
		*status = U_UNSUPPORTED_ERROR;
		return NULL;
	}
	if (!units && length) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return NULL;
	}
	UText *text = utext_new(status);
	if (!text)
		return NULL;
	text->length = units ? icu_length(units, length < 0 ? -1 : (INT)length) : 0;
	text->units = icu_alloc((text->length + 1) * sizeof(UChar));
	if (!text->units)
		return fail_open(text, status, U_MEMORY_ALLOCATION_ERROR);
	if (text->length)
		CopyMemory(text->units, units, text->length * sizeof(UChar));
	text->native_length = text->length;
	return text;
}

// Invalid bytes become U+FFFD, one for each byte, and keep their place in the offsets.
UText *__cdecl utext_openUTF8(UText *reuse, const char *bytes, INT64 length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	if (reuse) {
		*status = U_UNSUPPORTED_ERROR;
		return NULL;
	}
	if (!bytes && length) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return NULL;
	}
	UText *text = utext_new(status);
	if (!text)
		return NULL;
	INT64 size = !bytes ? 0 : length < 0 ? lstrlenA(bytes) : length;
	text->units = icu_alloc((size + 1) * sizeof(UChar));
	text->native = icu_alloc((size + 2) * sizeof(INT64));
	if (!text->units || !text->native)
		return fail_open(text, status, U_MEMORY_ALLOCATION_ERROR);
	INT count = 0;
	for (INT64 at = 0; at < size;) {
		BYTE lead = (BYTE)bytes[at];
		INT extra = lead < 0x80 ? 0 : lead >= 0xc2 && lead < 0xe0 ? 1 : lead >= 0xe0 && lead < 0xf0 ? 2 : lead >= 0xf0 && lead < 0xf5 ? 3 : -1;
		UChar32 c = extra == 0 ? lead : extra == 1 ? lead & 0x1f : extra == 2 ? lead & 0xf : lead & 7;
		BOOL good = extra >= 0 && at + extra < size;
		for (INT i = 1; good && i <= extra; i++) {
			BYTE more = (BYTE)bytes[at + i];
			good = (more & 0xc0) == 0x80;
			c = (c << 6) | (more & 0x3f);
		}
		if (good && ((extra == 2 && (c < 0x800 || (c >= 0xd800 && c < 0xe000))) || (extra == 3 && (c < 0x10000 || c > 0x10ffff))))
			good = FALSE;
		if (!good) {
			c = 0xfffd;
			extra = 0;
		}
		text->native[count] = at;
		if (c >= 0x10000) {
			text->units[count++] = (UChar)(0xd800 + ((c - 0x10000) >> 10));
			text->native[count] = at;
			text->units[count++] = (UChar)(0xdc00 + ((c - 0x10000) & 0x3ff));
		} else
			text->units[count++] = (UChar)c;
		at += extra + 1;
	}
	text->native[count] = size;
	text->length = count;
	text->native_length = size;
	return text;
}

UText *icu_utext_copy(const UText *from)
{
	UText *copy = icu_alloc(sizeof *copy);
	if (!copy)
		return NULL;
	copy->length = from->length;
	copy->native_length = from->native_length;
	copy->units = icu_alloc((from->length + 1) * sizeof(UChar));
	if (copy->units && from->length)
		CopyMemory(copy->units, from->units, from->length * sizeof(UChar));
	if (from->native) {
		copy->native = icu_alloc((from->length + 1) * sizeof(INT64));
		if (copy->native)
			CopyMemory(copy->native, from->native, (from->length + 1) * sizeof(INT64));
	}
	if (!copy->units || (from->native && !copy->native))
		return utext_close(copy);
	return copy;
}

INT64 icu_utext_native(const UText *text, INT units)
{
	return text->native ? text->native[units] : units;
}

// An index inside a character means the character it falls in.
INT icu_utext_units(const UText *text, INT64 native)
{
	if (native <= 0)
		return 0;
	if (native >= text->native_length)
		return text->length;
	if (!text->native)
		return (INT)native;
	INT low = 0, high = text->length;
	while (low < high) {
		INT middle = (low + high + 1) / 2;
		if (text->native[middle] <= native)
			low = middle;
		else
			high = middle - 1;
	}
	while (low > 0 && text->native[low - 1] == text->native[low])
		low--;
	return low;
}

INT64 __cdecl utext_nativeLength(UText *text)
{
	return text->native_length;
}

UChar32 __cdecl utext_char32At(UText *text, INT64 native)
{
	if (native < 0 || native >= text->native_length)
		return U_SENTINEL;
	return icu_at(text->units, text->length, icu_utext_units(text, native), NULL);
}

INT __cdecl utext_extract(UText *text, INT64 start, INT64 limit, UChar *dest, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	if (capacity < 0 || (capacity && !dest) || start > limit) {
		*status = capacity < 0 || (capacity && !dest) ? U_ILLEGAL_ARGUMENT_ERROR : U_INDEX_OUTOFBOUNDS_ERROR;
		return 0;
	}
	INT from = icu_utext_units(text, start), to = icu_utext_units(text, limit), count = to - from;
	if (dest && capacity)
		CopyMemory(dest, text->units + from, (count < capacity ? count : capacity) * sizeof(UChar));
	if (count < capacity)
		dest[count] = 0;
	return icu_finish(count, capacity, status);
}
