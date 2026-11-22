#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

typedef NTSTATUS (NTAPI *create_completion)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
typedef NTSTATUS (NTAPI *allocate_reserve)(PHANDLE, POBJECT_ATTRIBUTES, ULONG);
typedef NTSTATUS (NTAPI *set_completion)(HANDLE, HANDLE, void *, void *, NTSTATUS, ULONG_PTR);
struct completion_record {
	void *key, *context;
	IO_STATUS_BLOCK result;
};
typedef NTSTATUS (NTAPI *remove_completion)(HANDLE, struct completion_record *, ULONG, ULONG *, LARGE_INTEGER *, BOOLEAN);

int raven_entry(void)
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll)
		return 1;
	const create_completion create = (create_completion)GetProcAddress(ntdll, "NtCreateIoCompletion");
	const allocate_reserve allocate = (allocate_reserve)GetProcAddress(ntdll, "NtAllocateReserveObject");
	const set_completion post = (set_completion)GetProcAddress(ntdll, "NtSetIoCompletionEx");
	const remove_completion remove = (remove_completion)GetProcAddress(ntdll, "NtRemoveIoCompletionEx");
	if (!create || !allocate || !post || !remove)
		return 2;
	const ACCESS_MASK query_state = 1, modify_state = 2;
	const ULONG reserve_io_completion = 1;
	HANDLE port = NULL, query_only = NULL, reserve = NULL;
	LARGE_INTEGER now = {0};
	struct completion_record records[3] = {0};
	ULONG removed = 77;
	if (create(&port, query_state | modify_state, NULL, 0) || !port ||
		create(&query_only, query_state, NULL, 0) || !query_only ||
		allocate(&reserve, NULL, reserve_io_completion) || !reserve)
		return 3;
	if (post(query_only, reserve, NULL, NULL, 0, 0) != (NTSTATUS)0xc0000022 ||
		remove(query_only, records, 3, &removed, &now, FALSE) != (NTSTATUS)0xc0000022)
		return 4;
	const ULONG_PTR input = (ULONG_PTR)0x1122334455667788ULL;
	const ULONG_PTR key = input ^ 5, context = input ^ 9;
	const NTSTATUS failure = (NTSTATUS)0xc0000022;
	if (!PostQueuedCompletionStatus(port, 21, 19, (OVERLAPPED *)(ULONG_PTR)7) ||
		post(port, reserve, (void *)key, (void *)context, failure, input))
		return 5;
	if (post(port, reserve, NULL, NULL, 0, 0) != (NTSTATUS)0xc00000f0)
		return 6;
	records[2].result.Information = 91;
	if (remove(port, records, 3, &removed, &now, FALSE) || removed != 2 ||
		(ULONG_PTR)records[0].key != 19 || (ULONG_PTR)records[0].context != 7 ||
		records[0].result.Status || records[0].result.Information != 21 ||
		(ULONG_PTR)records[1].key != key || (ULONG_PTR)records[1].context != context ||
		records[1].result.Status != failure || records[1].result.Information != input || records[2].result.Information != 91)
		return 7;
	if (remove(port, records, 3, &removed, &now, FALSE) != 0x102 || removed != 0)
		return 8;
	if (post(port, reserve, (void *)key, (void *)context, failure, input) || !CloseHandle(reserve))
		return 9;
	DWORD bytes = 0;
	ULONG_PTR output_key = 0;
	OVERLAPPED *overlapped = NULL;
	if (GetQueuedCompletionStatus(port, &bytes, &output_key, &overlapped, 0) || GetLastError() != ERROR_ACCESS_DENIED ||
		bytes != (DWORD)input || output_key != key || (ULONG_PTR)overlapped != context)
		return 10;
	return CloseHandle(query_only) && CloseHandle(port) ? 0 : 11;
}
