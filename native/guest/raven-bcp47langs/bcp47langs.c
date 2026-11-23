/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllexport) HRESULT WINAPI GetFontFallbackLanguageList(const WCHAR *locale, size_t capacity, WCHAR *list, size_t *required)
{
	if (!capacity || !list || !required)
		return E_INVALIDARG;

	/* The locale APIs currently resolve to the one language configured in Raven. */
	WCHAR name[LOCALE_NAME_MAX_LENGTH];
	const int length = locale ? ResolveLocaleName(locale, name, ARRAYSIZE(name)) : GetUserDefaultLocaleName(name, ARRAYSIZE(name));
	if (length <= 1)
		return E_INVALIDARG;

	*required = (size_t)length;
	if (capacity < *required)
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	for (int index = 0; index < length; ++index)
		list[index] = name[index];
	return S_OK;
}
