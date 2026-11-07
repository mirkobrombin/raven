/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windef.h>
#include <winbase.h>
#include <unknwn.h>

/* msftedit.dll, the rich edit control 4.1 and later: the same control as riched20.dll, which also registers its RichEdit50W class.
 * The import below makes the loader bring riched20.dll in first, so its classes are there for the program; the exports go on to it. */
HRESULT WINAPI CreateTextServices(IUnknown *outer, void *host, IUnknown **services);
static void *const imported[] __attribute__((used)) = {(void *)CreateTextServices};

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
