#include <windows.h>
#include <stdarg.h>

/* Messages as installers log them: read from the program's own message table with inserts from a va_list, and from a string with an
   argument array, into buffers the system allocates; the returned pointer is as wide as the program's pointers. */

static struct {
	WCHAR *text;
	DWORD canary;
} allocated = {NULL, 0x5eed5eed};

static DWORD from_table(DWORD id, ...)
{
	va_list arguments;
	va_start(arguments, id);
	const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, id, 0, (WCHAR *)&allocated.text, 0,
		&arguments);
	va_end(arguments);
	return length;
}

int raven_entry(void)
{
	WCHAR buffer[64];
	/* The text ends with the line break of its definition, which the message compiler writes as it found it. */
	static const WCHAR expected[] = L"Detect begin, 3 packages, alpha";
	const DWORD length = from_table(10, 3, L"alpha");
	if (length < ARRAYSIZE(expected) || !allocated.text || CompareStringOrdinal(allocated.text, ARRAYSIZE(expected) - 1, expected, -1, FALSE) != CSTR_EQUAL ||
		allocated.text[ARRAYSIZE(expected) - 1] != L'\r' && allocated.text[ARRAYSIZE(expected) - 1] != L'\n')
		return 1;
	if (allocated.canary != 0x5eed5eed)
		return 2;
	LocalFree(allocated.text);
	const DWORD_PTR inserts[] = {(DWORD_PTR)L"x", (DWORD_PTR)-5};
	if (FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY, L"%1!s! and %2!d!", 0, 0, buffer, 64, (va_list *)inserts) != 8 ||
		lstrcmpW(buffer, L"x and -5"))
		return 3;
	if (FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, 11, 0, buffer, 64, NULL) ||
		GetLastError() != ERROR_MR_MID_NOT_FOUND)
		return 4;
	/* The table has no entry for this region: its language's neutral form answers, not the table that sorts first. */
	if (!FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, 10, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), buffer, 64, NULL) ||
		buffer[0] != L'D')
		return 5;
	if (!FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, 10, MAKELANGID(LANG_CHINESE, SUBLANG_NEUTRAL), buffer, 64, NULL) ||
		buffer[0] != L'T')
		return 6;
	/* An HRESULT that carries a system error is worded as that error, as error dialogs and runtimes ask for it. */
	if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, 0x8007007e, 0, buffer, 64, NULL) ||
		lstrcmpW(buffer, L"The specified module could not be found.\r\n"))
		return 7;
	return 0;
}
