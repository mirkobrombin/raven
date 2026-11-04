// Answers the guest gives without the runtime: the text of a GUID, a point in a rectangle, the thread's DPI awareness context, the
// keyboard layout, WinEvent hooks and the depth of a list. The parallel run takes the served forms, which must answer the same.
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <objbase.h>

static SLIST_HEADER list;
static SLIST_ENTRY entries[2];

int raven_entry(void)
{
	static const GUID guid = {0x01234567, 0x89ab, 0xcdef, {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};
	static const WCHAR expected[] = L"{01234567-89AB-CDEF-0123-456789ABCDEF}";
	WCHAR text[40];
	for (int index = 0; index < 40; ++index)
		text[index] = 0x55;
	if (StringFromGUID2(&guid, text, 39) != 39)
		return 1;
	for (int index = 0; index < 39; ++index)
		if (text[index] != expected[index])
			return 2;
	if (text[39] != 0x55 || StringFromGUID2(&guid, text, 38) != 0)
		return 3;
	RECT rect = {1, 2, 5, 6};
	POINT point = {1, 2};
	if (!PtInRect(&rect, point))
		return 4;
	const POINT outside[] = {{5, 2}, {4, 6}, {4, 1}, {0, 3}};
	for (int index = 0; index < 4; ++index)
		if (PtInRect(&rect, outside[index]))
			return 5;
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
	if (SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) != DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)
		return 6;
	if (GetThreadDpiAwarenessContext() != DPI_AWARENESS_CONTEXT_SYSTEM_AWARE)
		return 7;
	if (SetThreadDpiAwarenessContext((DPI_AWARENESS_CONTEXT)0x77) || GetThreadDpiAwarenessContext() != DPI_AWARENESS_CONTEXT_SYSTEM_AWARE)
		return 8;
	if ((ULONG_PTR)GetKeyboardLayout(0) != 0x04090409 || IsWinEventHookInstalled(EVENT_OBJECT_CREATE))
		return 9;
	InitializeSListHead(&list);
	InterlockedPushEntrySList(&list, &entries[0]);
	InterlockedPushEntrySList(&list, &entries[1]);
	if (QueryDepthSList(&list) != 2 || InterlockedPopEntrySList(&list) != &entries[1] || QueryDepthSList(&list) != 1)
		return 10;
	return 0;
}
