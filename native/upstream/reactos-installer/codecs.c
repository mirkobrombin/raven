/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windef.h>
#include <winbase.h>

/* The one imaging function automation uses, loaded with its library on the first picture that needs it, as a delay-loaded import
   would: programs that never decode a picture never load the imaging component and the libraries behind it. */
typedef HRESULT (WINAPI *convert_function)(const GUID *, void *, void **);

HRESULT WINAPI WICConvertBitmapSource(const GUID *format, void *source, void **converted)
{
	static convert_function convert;
	if (!convert) {
		HMODULE library = LoadLibraryW(L"windowscodecs.dll");
		convert = library ? (convert_function)GetProcAddress(library, "WICConvertBitmapSource") : NULL;
		if (!convert) {
			if (converted)
				*converted = NULL;
			return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
		}
	}
	return convert(format, source, converted);
}
