/* hlink.dll: shortcut names, special references and the ordinal Office links to. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>

int raven_entry(void)
{
	HMODULE library = LoadLibraryW(L"hlink.dll");
	HRESULT (WINAPI *is_shortcut)(LPCWSTR);
	HRESULT (WINAPI *get_special)(ULONG, LPWSTR *);
	HRESULT (WINAPI *set_special)(ULONG, LPCWSTR);
	HRESULT (WINAPI *translate)(LPCWSTR, DWORD, LPWSTR *);
	LPWSTR text = NULL;
	if (!library)
		return 1;
	is_shortcut = (void *)GetProcAddress(library, (LPCSTR)25);
	get_special = (void *)GetProcAddress(library, (LPCSTR)22);
	set_special = (void *)GetProcAddress(library, (LPCSTR)21);
	translate = (void *)GetProcAddress(library, (LPCSTR)31);
	if (!is_shortcut || (void *)is_shortcut != (void *)GetProcAddress(library, "HlinkIsShortcut") || !get_special || !set_special || !translate)
		return 2;
	if (is_shortcut(L"c:\\a\\b.URL") != S_OK || is_shortcut(L"c:\\a\\b.txt") != S_FALSE || is_shortcut(NULL) != E_INVALIDARG)
		return 3;
	if (get_special(0, &text) != S_OK || !text || lstrcmpW(text, L"about:blank"))
		return 4;
	CoTaskMemFree(text);
	if (set_special(0, L"http://example.invalid/") != S_OK || get_special(0, &text) != S_OK || lstrcmpW(text, L"http://example.invalid/"))
		return 5;
	CoTaskMemFree(text);
	if (translate(L"http://example.invalid/", 0, &text) != S_FALSE || text)
		return 6;
	return 0;
}
