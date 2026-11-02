/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven multimedia thread component: avrt.dll, the interface programs take a multimedia thread through.
 *
 * Nothing here schedules threads: the scheduler is a Windows service with no counterpart in this runtime, and a program that asks for a
 * task is one that raises the priority of its own thread and sets a period it keeps itself. The handle handed back stands for the
 * registration the program believes it made, so the calls it makes with it are answered as they would be for a granted task. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HANDLE granted(LPDWORD index)
{
	if (index)
		*index = 0;
	return (HANDLE)1;
}

__declspec(dllexport) HANDLE WINAPI AvSetMmThreadCharacteristicsA(LPCSTR task, LPDWORD index)
{
	(void)task;
	return granted(index);
}

__declspec(dllexport) HANDLE WINAPI AvSetMmThreadCharacteristicsW(LPCWSTR task, LPDWORD index)
{
	(void)task;
	return granted(index);
}

__declspec(dllexport) BOOL WINAPI AvRevertMmThreadCharacteristics(HANDLE handle)
{
	return handle != NULL;
}

__declspec(dllexport) BOOL WINAPI AvSetMmThreadPriority(HANDLE handle, int priority)
{
	(void)priority;
	return handle != NULL;
}

__declspec(dllexport) HRESULT WINAPI AvQuerySystemResponsiveness(HANDLE handle, PULONG value)
{
	if (!handle)
		return (HRESULT)0x80070057L;
	if (value)
		*value = 0;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	(void)instance;
	(void)reason;
	(void)reserved;
	return TRUE;
}
