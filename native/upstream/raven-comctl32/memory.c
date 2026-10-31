/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>

/* The allocator the common controls share with programs that call it by ordinal, and text conversions over it. */

LPVOID WINAPI Alloc(DWORD size)
{
	return LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, size);
}

LPVOID WINAPI ReAlloc(LPVOID memory, DWORD size)
{
	return memory ? LocalReAlloc(memory, size, LMEM_MOVEABLE | LMEM_ZEROINIT) : Alloc(size);
}

BOOL WINAPI Free(LPVOID memory)
{
	return !LocalFree(memory);
}

DWORD WINAPI GetSize(LPVOID memory)
{
	return (DWORD)LocalSize(memory);
}

/* Both return the length of what was converted, without its terminator; with no buffer, the length it would take. */
INT Str_GetPtrWtoA(LPCWSTR source, LPSTR out, INT capacity)
{
	if (!source) {
		if (out && capacity > 0)
			out[0] = 0;
		return 0;
	}
	INT length = WideCharToMultiByte(CP_ACP, 0, source, -1, NULL, 0, NULL, NULL) - 1;
	if (!out)
		return length;
	if (capacity <= 0)
		return 0;
	if (length >= capacity)
		length = capacity - 1;
	WideCharToMultiByte(CP_ACP, 0, source, -1, out, length, NULL, NULL);
	out[length] = 0;
	return length;
}

INT Str_GetPtrAtoW(LPCSTR source, LPWSTR out, INT capacity)
{
	if (!source) {
		if (out && capacity > 0)
			out[0] = 0;
		return 0;
	}
	INT length = MultiByteToWideChar(CP_ACP, 0, source, -1, NULL, 0) - 1;
	if (!out)
		return length;
	if (capacity <= 0)
		return 0;
	if (length >= capacity)
		length = capacity - 1;
	MultiByteToWideChar(CP_ACP, 0, source, -1, out, length);
	out[length] = 0;
	return length;
}

BOOL Str_SetPtrAtoW(LPWSTR *slot, LPCSTR source)
{
	if (!source) {
		Free(*slot);
		*slot = NULL;
		return TRUE;
	}
	INT length = MultiByteToWideChar(CP_ACP, 0, source, -1, NULL, 0);
	LPWSTR copy = ReAlloc(*slot, length * sizeof(WCHAR));
	if (!copy)
		return FALSE;
	MultiByteToWideChar(CP_ACP, 0, source, -1, copy, length);
	*slot = copy;
	return TRUE;
}

BOOL Str_SetPtrWtoA(LPSTR *slot, LPCWSTR source)
{
	if (!source) {
		Free(*slot);
		*slot = NULL;
		return TRUE;
	}
	INT length = WideCharToMultiByte(CP_ACP, 0, source, -1, NULL, 0, NULL, NULL);
	LPSTR copy = ReAlloc(*slot, length);
	if (!copy)
		return FALSE;
	WideCharToMultiByte(CP_ACP, 0, source, -1, copy, length, NULL, NULL);
	*slot = copy;
	return TRUE;
}

/* ---- notifications ---- */

LRESULT WINAPI SendNotifyEx(HWND to, HWND from, UINT code, LPNMHDR header, DWORD format)
{
	NMHDR own;
	if (!to && from)
		to = GetParent(from);
	if (!to)
		return 0;
	if (!header)
		header = &own;
	header->hwndFrom = from;
	header->idFrom = from ? (UINT_PTR)GetWindowLongPtrW(from, GWLP_ID) : 0;
	header->code = code;
	return SendMessageW(to, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

LRESULT WINAPI SendNotify(HWND to, HWND from, UINT code, LPNMHDR header)
{
	return SendNotifyEx(to, from, code, header, 0);
}
