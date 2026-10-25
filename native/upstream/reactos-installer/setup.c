/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windef.h>
#include <winbase.h>

/* The URL component's private holder interface, an asynchronous bind context and the decompressing MIME filter's class. */
const GUID IID_IBindStatusCallbackHolder = {0x79eac9cc, 0xbaf9, 0x11ce, {0x8c, 0x82, 0x00, 0xaa, 0x00, 0x4b, 0xa9, 0x0b}};
const GUID IID_IAsyncBindCtx = {0x79eac9d4, 0xbaf9, 0x11ce, {0x8c, 0x82, 0x00, 0xaa, 0x00, 0x4b, 0xa9, 0x0b}};
const GUID CLSID_DeCompMimeFilter = {0x8f6b0360, 0xb80d, 0x11d0, {0xa9, 0xb3, 0x00, 0x60, 0x97, 0x94, 0x23, 0x11}};

/* The setup command runner of advpack, which the URL component asks for when it installs a downloaded control, loaded with its library
   on first use as the ReactOS build delay-loads it: programs that never install a control never need it. */
typedef HRESULT (WINAPI *setup_function)(HWND, const WCHAR *, const WCHAR *, const WCHAR *, const WCHAR *, HANDLE *, DWORD, void *);

HRESULT WINAPI RunSetupCommandW(HWND window, const WCHAR *command, const WCHAR *section, const WCHAR *directory, const WCHAR *title,
	HANDLE *process, DWORD flags, void *reserved)
{
	static setup_function run;
	if (!run) {
		HMODULE library = LoadLibraryW(L"advpack.dll");
		run = library ? (setup_function)GetProcAddress(library, "RunSetupCommandW") : NULL;
		if (!run)
			return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
	}
	return run(window, command, section, directory, title, process, flags, reserved);
}
