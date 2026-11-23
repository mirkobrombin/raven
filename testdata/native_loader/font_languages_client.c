#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int raven_entry(void)
{
	HMODULE module = LoadLibraryW(L"bcp47langs.dll");
	if (!module)
		return 1;
	HRESULT (WINAPI *fallback)(const WCHAR *, size_t, WCHAR *, size_t *) =
		(void *)GetProcAddress(module, "GetFontFallbackLanguageList");
	if (!fallback)
		return 2;

	const WCHAR locale[] = L"en-US";
	WCHAR list[256];
	size_t length = 0;
	if (fallback(locale, 1, list, &length) != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) ||
		length <= 1 || length >= ARRAYSIZE(list))
		return 3;
	const size_t capacity = length;
	for (size_t index = 0; index <= capacity; ++index)
		list[index] = L'!';
	length = 0;
	if (fallback(locale, capacity, list, &length) != S_OK ||
		length < ARRAYSIZE(locale) - 1 || length > capacity || list[capacity] != L'!')
		return 4;
	for (size_t index = 0; index + 1 < ARRAYSIZE(locale); ++index)
		if (list[index] != locale[index])
			return 5;
	size_t terminator = ARRAYSIZE(locale) - 1;
	while (terminator < capacity && list[terminator])
		++terminator;
	if (terminator == capacity)
		return 9;
	length = 0;
	if (fallback(NULL, ARRAYSIZE(list), list, &length) != S_OK || !length || length > ARRAYSIZE(list))
		return 6;
	if (fallback(locale, 0, list, &length) != E_INVALIDARG ||
		fallback(locale, ARRAYSIZE(list), NULL, &length) != E_INVALIDARG ||
		fallback(L"not a language tag", ARRAYSIZE(list), list, &length) != E_INVALIDARG)
		return 7;
	return FreeLibrary(module) ? 0 : 8;
}
