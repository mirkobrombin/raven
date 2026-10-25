/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winerror.h>

/* What the Windows Installer component needs from libraries Raven does not ship. */

/* No compatibility database: every package runs as it is, with no shim to apply. */
BOOL WINAPI ApphelpCheckRunAppEx(HANDLE file, PVOID first, PVOID second, PCWSTR application, PVOID environment, USHORT type, PULONG reason,
	PVOID *data, PULONG data_size, PVOID *sxs, PULONG sxs_size, PULONG fusion, PULONG64 flags, PULONG more)
{
	if (reason)
		*reason = 0;
	return TRUE;
}

BOOL WINAPI SE_DynamicShim(LPCWSTR image, PVOID database, PVOID result, LPCSTR module, LPDWORD token)
{
	return FALSE;
}

PVOID WINAPI SdbInitDatabase(DWORD flags, LPCWSTR path)
{
	return NULL;
}

PVOID WINAPI SdbReleaseDatabase(PVOID database)
{
	return NULL;
}

/* Binary patches of installed files are not applied yet: a package that carries them fails that file, visibly. */
BOOL WINAPI ApplyPatchToFileW(LPCWSTR patch, LPCWSTR old, LPCWSTR target, ULONG flags)
{
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

/* The script parser interface of the program's width, which script custom actions ask the engine for. */
#ifdef _WIN64
const GUID IID_IActiveScriptParse64 = {0xc7ef7658, 0xe1ee, 0x480e, {0x97, 0xea, 0xd5, 0x2c, 0xb4, 0xd7, 0x6d, 0x17}};
#else
const GUID IID_IActiveScriptParse32 = {0xbb1a2ae2, 0xa4f9, 0x11cf, {0x8f, 0x20, 0x00, 0x80, 0x5f, 0x2c, 0xd0, 0x64}};
#endif

int __cdecl wine_dbg_printf(const char *format, ...)
{
	return 0;
}

int __cdecl wine_dbg_log(int class, void *channel, const char *function, const char *format, ...)
{
	return 0;
}

/* Interfaces and a class the XML component names that mingw-w64's uuid library leaves out. */
const GUID CLSID_HTMLDocument = {0x25336920, 0x03f9, 0x11cf, {0x8f, 0xd0, 0x00, 0xaa, 0x00, 0x68, 0x6f, 0x13}};
const GUID IID_IDispatchEx = {0xa6ef9860, 0xc720, 0x11d0, {0x93, 0x37, 0x00, 0xa0, 0xc9, 0x0d, 0xca, 0xa9}};
const GUID IID_IObjectIdentity = {0xca04b7e6, 0x0d21, 0x11d1, {0x8c, 0xc5, 0x00, 0xc0, 0x4f, 0xc2, 0xb0, 0x85}};
const GUID IID_IPersistHistory = {0x91a565c1, 0xe38f, 0x11d0, {0x94, 0xbf, 0x00, 0xa0, 0xc9, 0x05, 0x5c, 0xbf}};
