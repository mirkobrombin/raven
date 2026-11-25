#include <windows.h>
#include <stdint.h>

typedef DWORD (WINAPI *read_fn)(void);
typedef DWORD (WINAPI *write_fn)(DWORD);

static BOOL listed(HMODULE module, BOOL expected)
{
	uintptr_t peb;
	__asm__("movq %%gs:0x60,%0" : "=r"(peb));
	const uintptr_t loader = *(uintptr_t *)(peb + 0x18);
	if (!loader)
		return FALSE;
	for (unsigned offset = 0x10; offset <= 0x30; offset += 0x10) {
		uintptr_t *head = (uintptr_t *)(loader + offset);
		uintptr_t *previous = head;
		unsigned matches = 0, count = 0;
		for (uintptr_t *link = (uintptr_t *)head[0]; link != head; link = (uintptr_t *)link[0]) {
			if (++count > 10000 || link[1] != (uintptr_t)previous)
				return FALSE;
			const uintptr_t entry = (uintptr_t)link - (offset - 0x10);
			matches += *(uintptr_t *)(entry + 0x30) == (uintptr_t)module;
			previous = link;
		}
		if (head[1] != (uintptr_t)previous || matches != (unsigned)expected)
			return FALSE;
	}
	return TRUE;
}

typedef void (WINAPI *unload_trace_fn)(ULONG **, ULONG **, void **);

// ntdll keeps the libraries unloaded last, which crash reporters read: the one at this base, by its name, is among them.
static BOOL unloaded(HMODULE module)
{
	unload_trace_fn trace = (unload_trace_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetUnloadEventTraceEx");
	ULONG *size = NULL, *count = NULL;
	void *ring = NULL;
	if (!trace)
		return FALSE;
	trace(&size, &count, &ring);
	if (!size || !count || !ring || *size != 104 || *count != 64)
		return FALSE;
	static const WCHAR name[] = L"tlsprobe.dll";
	for (ULONG index = 0; index < *count; ++index) {
		const unsigned char *record = (const unsigned char *)ring + index * *size;
		const WCHAR *image = (const WCHAR *)(record + 28);
		unsigned at = 0;
		while (name[at] && (image[at] | 0x20) == name[at])
			++at;
		if (*(void *const *)record == (void *)module && *(const SIZE_T *)(record + 8) && !name[at] && !image[at] &&
			*(const ULONG *)(record + 20))
			return TRUE;
	}
	return FALSE;
}

DWORD raven_entry(void)
{
	SetLastError(99);
	if (GetModuleHandleA("tlsprobe.dll"))
		return 1;
	volatile unsigned char *reserved = VirtualAlloc((void *)0x190000000ULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (reserved != (void *)0x190000000ULL)
		return 16;
	reserved[0] = 0x5a;
	reserved[65535] = 0xa5;
	HMODULE first = LoadLibraryA("TLSPROBE");
	if (!first || (void *)first == (void *)reserved || !listed(first, TRUE))
		return 2;
	read_fn read = (read_fn)GetProcAddress(first, "ReadTLSValue");
	write_fn write = (write_fn)GetProcAddress(first, "WriteTLSValue");
	read_fn query = (read_fn)GetProcAddress(first, "QueryTLS");
	if (!read || !write || !query || read() != 19 || !query() || write(91) != 19)
		return 3;
	HMODULE second = LoadLibraryW(L"tlsprobe.dll");
	if (second != first || read() != 91 || !FreeLibrary(first) || read() != 91 || !listed(second, TRUE))
		return 4;
	if (!FreeLibrary(second) || GetModuleHandleW(L"tlsprobe.dll") || !listed(second, FALSE))
		return 5;
	if (FreeLibrary(second) || GetLastError() != ERROR_MOD_NOT_FOUND)
		return 6;
	if (!unloaded(second))
		return 19;
	first = LoadLibraryA("tlsprobe.dll");
	if (!first)
		return 7;
	read = (read_fn)GetProcAddress(first, "ReadTLSValue");
	if (!read || read() != 19 || !FreeLibrary(first))
		return 8;
	first = LoadLibraryExW(L"tlsprobe.dll", NULL, LOAD_LIBRARY_AS_DATAFILE);
	if (!first || ((uintptr_t)first & ~(uintptr_t)3) == (uintptr_t)reserved || !FreeLibrary(first))
		return 17;
	MEMORY_BASIC_INFORMATION info;
	if (reserved[0] != 0x5a || reserved[65535] != 0xa5 || VirtualQuery((void *)reserved, &info, sizeof(info)) != sizeof(info) ||
		info.State != MEM_COMMIT || info.Type != MEM_PRIVATE || !VirtualFree((void *)reserved, 0, MEM_RELEASE))
		return 18;
	HMODULE parent = LoadLibraryA("parent.dll");
	if (!parent)
		return 9;
	query = (read_fn)GetProcAddress(parent, "QueryDependency");
	if (!query || !query() || !GetModuleHandleA("tlsprobe.dll"))
		return 10;
	first = LoadLibraryA("tlsprobe.dll");
	if (!first || !FreeLibrary(parent) || !GetModuleHandleA("tlsprobe.dll") || !FreeLibrary(first))
		return 11;
	if (GetModuleHandleA("parent.dll") || GetModuleHandleA("tlsprobe.dll"))
		return 12;
	if (LoadLibraryA("reject.dll") || GetLastError() != ERROR_DLL_INIT_FAILED)
		return 13;
	if (GetModuleHandleA("reject.dll") || GetModuleHandleA("tlsprobe.dll"))
		return 14;
	if (LoadLibraryA("missing.dll") || GetLastError() != ERROR_MOD_NOT_FOUND)
		return 15;
	return 0;
}
