/* SPDX-License-Identifier: MIT */
#include "vbs.h"

void *vbs_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}

void *vbs_grow(void *block, SIZE_T size)
{
	if (!block)
		return vbs_alloc(size);
	return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, block, size);
}

void vbs_free(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

WCHAR *vbs_copy_n(const WCHAR *text, SIZE_T length)
{
	WCHAR *copy = vbs_alloc((length + 1) * sizeof(WCHAR));
	memcpy(copy, text, length * sizeof(WCHAR));
	return copy;
}

WCHAR *vbs_copy(const WCHAR *text)
{
	return text ? vbs_copy_n(text, lstrlenW(text)) : NULL;
}

/* Names compare without regard to case; only the letters of ASCII and Latin-1 fold, which is all identifiers use in practice. */
WCHAR vbs_lower(WCHAR c)
{
	if ((c >= L'A' && c <= L'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
		return c + 0x20;
	return c;
}

BOOL vbs_same(const WCHAR *a, const WCHAR *b)
{
	if (!a || !b)
		return a == b;
	for (; *a && *b; a++, b++)
		if (vbs_lower(*a) != vbs_lower(*b))
			return FALSE;
	return *a == *b;
}

const WCHAR *vbs_error_text(LONG number)
{
	static const struct {
		LONG number;
		const WCHAR *text;
	} texts[] = {
		{ 5, L"Invalid procedure call or argument" },
		{ 6, L"Overflow" },
		{ 7, L"Out of memory" },
		{ 9, L"Subscript out of range" },
		{ 10, L"This array is fixed or temporarily locked" },
		{ 11, L"Division by zero" },
		{ 13, L"Type mismatch" },
		{ 14, L"Out of string space" },
		{ 28, L"Out of stack space" },
		{ 35, L"Sub or Function not defined" },
		{ 48, L"Error in loading DLL" },
		{ 51, L"Internal error" },
		{ 53, L"File not found" },
		{ 70, L"Permission denied" },
		{ 76, L"Path not found" },
		{ 91, L"Object variable not set" },
		{ 92, L"For loop not initialized" },
		{ 94, L"Invalid use of Null" },
		{ 424, L"Object required" },
		{ 429, L"ActiveX component can't create object" },
		{ 438, L"Object doesn't support this property or method" },
		{ 445, L"Object doesn't support this action" },
		{ 450, L"Wrong number of arguments or invalid property assignment" },
		{ 451, L"Object not a collection" },
		{ 500, L"Variable is undefined" },
		{ 501, L"Illegal assignment" },
		{ 502, L"Object not safe for scripting" },
		{ 503, L"Object not safe for initializing" },
		{ 505, L"Invalid or unqualified reference" },
		{ 506, L"Class not defined" },
		{ 1002, L"Syntax error" },
		{ 1041, L"Name redefined" },
	};
	for (UINT i = 0; i < ARRAYSIZE(texts); i++)
		if (texts[i].number == number)
			return texts[i].text;
	return L"Unknown runtime error";
}

