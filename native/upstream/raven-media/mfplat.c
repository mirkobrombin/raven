/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven Media Foundation component: mfplat.dll, the factories media libraries create their samples, types and buffers with.
 *
 * This runtime has no media pipeline behind those objects, and one handed back with no implementation makes every later call fail far
 * from the request that started it. Each factory reports the interface it would have to implement as unsupported instead, which is what
 * a library checks before it decides how to carry media. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define RAVEN_E_NOTIMPL ((HRESULT)0x80004005L)

static HRESULT unsupported(void **result)
{
	if (result)
		*result = NULL;
	return RAVEN_E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI MFCreateSample(void **sample)
{
	return unsupported(sample);
}

__declspec(dllexport) HRESULT WINAPI MFCreateMediaType(void **type)
{
	return unsupported(type);
}

__declspec(dllexport) HRESULT WINAPI MFCreateMemoryBuffer(DWORD max_length, void **buffer)
{
	(void)max_length;
	return unsupported(buffer);
}

__declspec(dllexport) HRESULT WINAPI MFCreateAlignedMemoryBuffer(DWORD max_length, DWORD alignment, void **buffer)
{
	(void)max_length;
	(void)alignment;
	return unsupported(buffer);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	(void)instance;
	(void)reason;
	(void)reserved;
	return TRUE;
}
