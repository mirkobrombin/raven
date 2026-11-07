/* SPDX-License-Identifier: MIT */
#include "gdip.h"

typedef struct
{
	UINT32 version;
	void *debug_callback;
	BOOL suppress_thread, suppress_codecs;
} startup_input;

typedef struct
{
	void *hook, *unhook;
} startup_output;

static void *process_heap;

void *gp_alloc(SIZE_T size)
{
	return HeapAlloc(process_heap ? process_heap : GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void *gp_realloc(void *memory, SIZE_T size)
{
	HANDLE heap = process_heap ? process_heap : GetProcessHeap();
	return memory ? HeapReAlloc(heap, HEAP_ZERO_MEMORY, memory, size) : HeapAlloc(heap, HEAP_ZERO_MEMORY, size);
}

void gp_free(void *memory)
{
	if (memory)
		HeapFree(process_heap ? process_heap : GetProcessHeap(), 0, memory);
}

void *gp_dup(const void *data, SIZE_T size)
{
	void *copy = gp_alloc(size ? size : 1);
	if (copy && size)
		memcpy(copy, data, size);
	return copy;
}

void *GDIPAPI GdipAlloc(SIZE_T size)
{
	return gp_alloc(size);
}

void GDIPAPI GdipFree(void *memory)
{
	gp_free(memory);
}

static GpStatus __stdcall notification_hook(ULONG_PTR *token)
{
	if (!token)
		return InvalidParameter;
	*token = 0xdeadbeef;
	return Ok;
}

static void __stdcall notification_unhook(ULONG_PTR token)
{
	(void)token;
}

GpStatus __stdcall GdiplusStartup(ULONG_PTR *token, const startup_input *input, startup_output *output)
{
	if (!token || !input)
		return InvalidParameter;
	if (input->version < 1 || input->version > 2)
		return UnsupportedGdiplusVersion;
	if (input->suppress_thread) {
		if (!output)
			return InvalidParameter;
		output->hook = notification_hook;
		output->unhook = notification_unhook;
	}
	*token = 1;
	return Ok;
}

void __stdcall GdiplusShutdown(ULONG_PTR token)
{
	(void)token;
}

GpStatus __stdcall GdiplusNotificationHook(ULONG_PTR *token)
{
	return notification_hook(token);
}

void __stdcall GdiplusNotificationUnhook(ULONG_PTR token)
{
	notification_unhook(token);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		process_heap = GetProcessHeap();
		DisableThreadLibraryCalls(module);
	}
	return TRUE;
}
