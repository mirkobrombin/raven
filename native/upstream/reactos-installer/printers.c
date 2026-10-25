/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windef.h>
#include <winbase.h>

/* The print processor folder, which the setup API names among the folders an INF file may install to. The spooler's library is loaded
   on first use, as the ReactOS build delay-loads it: setup information that never names that folder never needs it. */
typedef BOOL (WINAPI *folder_function)(WCHAR *, WCHAR *, DWORD, BYTE *, DWORD, DWORD *);

BOOL WINAPI GetPrintProcessorDirectoryW(WCHAR *server, WCHAR *environment, DWORD level, BYTE *buffer, DWORD size, DWORD *needed)
{
	static folder_function folder;
	if (!folder) {
		HMODULE library = LoadLibraryW(L"winspool.drv");
		folder = library ? (folder_function)GetProcAddress(library, "GetPrintProcessorDirectoryW") : NULL;
		if (!folder) {
			SetLastError(ERROR_MOD_NOT_FOUND);
			return FALSE;
		}
	}
	return folder(server, environment, level, buffer, size, needed);
}
