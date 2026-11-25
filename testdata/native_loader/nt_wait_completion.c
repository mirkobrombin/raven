#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

typedef NTSTATUS (NTAPI *create_packet)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
typedef NTSTATUS (NTAPI *associate_packet)(HANDLE, HANDLE, HANDLE, void *, void *, NTSTATUS, ULONG_PTR, BOOLEAN *);
typedef NTSTATUS (NTAPI *cancel_packet)(HANDLE, BOOLEAN);

static DWORD WINAPI signal_event(void *event)
{
	return SetEvent(event) ? 0 : 1;
}

static int received(HANDLE port, DWORD timeout)
{
	DWORD bytes = 0;
	ULONG_PTR key = 0;
	OVERLAPPED *context = NULL;
	return GetQueuedCompletionStatus(port, &bytes, &key, &context, timeout) && bytes == 23 && key == 17 && (ULONG_PTR)context == 19;
}

static int empty(HANDLE port)
{
	DWORD bytes = 0;
	ULONG_PTR key = 0;
	OVERLAPPED *context = NULL;
	return !GetQueuedCompletionStatus(port, &bytes, &key, &context, 0) && GetLastError() == WAIT_TIMEOUT;
}

int raven_entry(void)
{
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	const create_packet create = (create_packet)GetProcAddress(ntdll, "NtCreateWaitCompletionPacket");
	const associate_packet associate = (associate_packet)GetProcAddress(ntdll, "NtAssociateWaitCompletionPacket");
	const cancel_packet cancel = (cancel_packet)GetProcAddress(ntdll, "NtCancelWaitCompletionPacket");
	if (!create || !associate || !cancel)
		return 1;
	HANDLE packet = NULL;
	const HANDLE event = CreateEventW(NULL, FALSE, TRUE, NULL);
	const HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (!event || !port || create(&packet, 1, NULL) || !packet)
		return 2;
	void *const key = (void *)(ULONG_PTR)17, *const context = (void *)(ULONG_PTR)19;
	BOOLEAN signaled = FALSE;
	if (associate(packet, port, event, key, context, 0, 23, &signaled) || !signaled || WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
		return 3;
	if (associate(packet, port, event, key, context, 0, 23, NULL) != (NTSTATUS)0xc00000ef ||
		cancel(packet, FALSE) != 0x103 || !received(port, 0) || !empty(port))
		return 4;
	if (!SetEvent(event) || associate(packet, port, event, key, context, 0, 23, NULL) || cancel(packet, TRUE) || !empty(port))
		return 5;
	signaled = TRUE;
	if (associate(packet, port, event, key, context, 0, 23, &signaled) || signaled || cancel(packet, FALSE) || !SetEvent(event) || !empty(port))
		return 6;
	if (cancel(packet, TRUE) != (NTSTATUS)0xc0000120 || !ResetEvent(event) || associate(packet, port, event, key, context, 0, 23, NULL))
		return 7;
	HANDLE worker = CreateThread(NULL, 0, signal_event, event, 0, NULL);
	if (!worker || !received(port, 2000) || WaitForSingleObject(worker, 2000) != WAIT_OBJECT_0 || !empty(port))
		return 8;
	// A port is itself a waitable queue: signaled while it holds packets, and a wait on it takes none of them.
	if (WaitForSingleObject(port, 0) != WAIT_TIMEOUT || !PostQueuedCompletionStatus(port, 23, 17, (OVERLAPPED *)(ULONG_PTR)19) ||
		WaitForSingleObject(port, 0) != WAIT_OBJECT_0 || WaitForSingleObject(port, 0) != WAIT_OBJECT_0 || !received(port, 0) ||
		WaitForSingleObject(port, 0) != WAIT_TIMEOUT)
		return 10;
	return CloseHandle(worker) && CloseHandle(packet) && CloseHandle(event) && CloseHandle(port) ? 0 : 9;
}
