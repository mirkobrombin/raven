#include <windows.h>
#include <objbase.h>

/* COM's task allocator: CoTaskMemAlloc, CoTaskMemRealloc and CoTaskMemFree as Microsoft documents them, the same memory through
   CoGetMalloc, and a block the system hands out (StringFromCLSID) freed by the program. They run in the program's own process, in the
   component that holds the process heap, as Windows runs them in user mode; the test prints the nanoseconds an allocation and its
   release took. Returns 0, or the number of the first failing check. */

static int inside(const char *library, const char *name, const WCHAR *module)
{
	const BYTE *base = (const BYTE *)GetModuleHandleW(module);
	const BYTE *entry = (const BYTE *)GetProcAddress(LoadLibraryA(library), name);
	if (!base || !entry)
		return 0;
	const IMAGE_NT_HEADERS *headers = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
	return entry >= base && entry < base + headers->OptionalHeader.SizeOfImage;
}

int raven_entry(void)
{
	BYTE *block = CoTaskMemAlloc(100);
	if (!block)
		return 1;
	for (int at = 0; at < 100; ++at)
		block[at] = (BYTE)at;
	BYTE *grown = CoTaskMemRealloc(block, 5000);
	if (!grown)
		return 2;
	for (int at = 0; at < 100; ++at)
		if (grown[at] != (BYTE)at)
			return 3;
	grown[4999] = 1;
	/* A size of zero frees the block and answers NULL; no block reallocates as a new one. */
	if (CoTaskMemRealloc(grown, 0))
		return 4;
	BYTE *fresh = CoTaskMemRealloc(NULL, 10);
	if (!fresh)
		return 5;
	CoTaskMemFree(fresh);
	CoTaskMemFree(NULL);
	void *empty = CoTaskMemAlloc(0);
	if (!empty)
		return 6;
	CoTaskMemFree(empty);
	IMalloc *allocator = NULL;
	if (CoGetMalloc(MEMCTX_TASK, &allocator) != S_OK || !allocator)
		return 7;
	void *through = allocator->lpVtbl->Alloc(allocator, 64);
	if (!through)
		return 8;
	CoTaskMemFree(through);
	through = CoTaskMemAlloc(64);
	allocator->lpVtbl->Free(allocator, through);
	LPOLESTR text = NULL;
	if (StringFromCLSID(&IID_IUnknown, &text) != S_OK || !text || text[0] != L'{' || text[37] != L'}' || text[38])
		return 9;
	CoTaskMemFree(text);
	if (!inside("ole32.dll", "CoTaskMemAlloc", L"raven-rtl.dll") || !inside("ole32.dll", "CoTaskMemFree", L"raven-rtl.dll") ||
		!inside("ole32.dll", "CoTaskMemRealloc", L"raven-rtl.dll") || !inside("combase.dll", "CoTaskMemAlloc", L"raven-rtl.dll"))
		return 10;
	LARGE_INTEGER frequency, started, ended;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&started);
	for (int at = 0; at < 20000; ++at)
		CoTaskMemFree(CoTaskMemAlloc(32 + at % 64));
	QueryPerformanceCounter(&ended);
	char line[64];
	const int length = wsprintfA(line, "CoTaskMemAlloc and CoTaskMemFree: %d ns\n",
		(int)((ended.QuadPart - started.QuadPart) * 1000000000 / frequency.QuadPart / 20000));
	DWORD written;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, length, &written, NULL);
	return 0;
}
