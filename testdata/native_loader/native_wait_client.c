#include <windows.h>

// The native waits of ntdll (Zig's standard library waits this way): NtDelayExecution sleeps for a native timeout and ends early for
// APCs or an alert when alertable, NtWaitForAlertByThreadId pairs with NtAlertThreadByThreadId (an alert that comes first is kept),
// and NtWaitForSingleObject and NtWaitForMultipleObjects wait as the Win32 waits do, alertable ones included. Returns 0, or the
// number of the check that failed.
typedef LONG NTSTATUS;
__declspec(dllimport) NTSTATUS NTAPI NtDelayExecution(BOOLEAN alertable, const LARGE_INTEGER *interval);
__declspec(dllimport) NTSTATUS NTAPI NtAlertThread(HANDLE thread);
__declspec(dllimport) NTSTATUS NTAPI NtTestAlert(void);
__declspec(dllimport) NTSTATUS NTAPI NtWaitForAlertByThreadId(const void *address, const LARGE_INTEGER *timeout);
__declspec(dllimport) NTSTATUS NTAPI NtAlertThreadByThreadId(ULONG_PTR thread_id);
__declspec(dllimport) NTSTATUS NTAPI NtWaitForSingleObject(HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout);
__declspec(dllimport) NTSTATUS NTAPI NtWaitForMultipleObjects(ULONG count, const HANDLE *handles, int wait_type, BOOLEAN alertable,
	const LARGE_INTEGER *timeout);

#define STATUS_USER_APC 0xc0
#define STATUS_ALERTED 0x101
#define STATUS_TIMEOUT 0x102

static volatile LONG apcs;
static volatile DWORD parked;

static void CALLBACK count_apc(ULONG_PTR parameter)
{
	apcs += (LONG)parameter;
}

static DWORD WINAPI park(void *parameter)
{
	(void)parameter;
	parked = GetCurrentThreadId();
	const LARGE_INTEGER ten_seconds = {.QuadPart = -100000000LL};
	return NtWaitForAlertByThreadId((const void *)&parked, &ten_seconds) == STATUS_ALERTED ? 0 : 1;
}

int raven_entry(void)
{
	const LARGE_INTEGER twenty_ms = {.QuadPart = -200000LL}, one_second = {.QuadPart = -10000000LL}, now = {.QuadPart = 0};
	const ULONGLONG before = GetTickCount64();
	if (NtDelayExecution(FALSE, &twenty_ms) != 0 || GetTickCount64() - before < 15)
		return 1;
	// An APC queued to this thread ends an alertable sleep at once.
	if (!QueueUserAPC(count_apc, GetCurrentThread(), 3) || NtDelayExecution(TRUE, &one_second) != STATUS_USER_APC || apcs != 3)
		return 2;
	// An alert ends the next alertable native wait, and only that one.
	if (NtAlertThread(GetCurrentThread()) != 0 || NtDelayExecution(TRUE, &one_second) != STATUS_ALERTED)
		return 3;
	if (NtDelayExecution(TRUE, &now) != 0 || NtTestAlert() != 0)
		return 4;
	// A thread-id alert that comes first is kept for the next wait; without one the wait times out.
	if (NtAlertThreadByThreadId(GetCurrentThreadId()) != 0 || NtWaitForAlertByThreadId(NULL, &one_second) != STATUS_ALERTED)
		return 5;
	if (NtWaitForAlertByThreadId(NULL, &twenty_ms) != STATUS_TIMEOUT)
		return 6;
	// Another thread parks until this one alerts it by its id.
	HANDLE thread = CreateThread(NULL, 0, park, NULL, 0, NULL);
	if (!thread)
		return 7;
	while (!parked)
		NtDelayExecution(FALSE, &now);
	if (NtAlertThreadByThreadId(parked) != 0 || NtWaitForSingleObject(thread, FALSE, &one_second) != 0)
		return 8;
	DWORD code = 1;
	if (!GetExitCodeThread(thread, &code) || code != 0)
		return 9;
	// The object waits: a signaled event, an unsignaled one that times out, an APC that ends an alertable wait, and the index of the
	// signaled handle of a wait for any.
	HANDLE set = CreateEventW(NULL, TRUE, TRUE, NULL), unset = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (NtWaitForSingleObject(set, FALSE, NULL) != 0 || NtWaitForSingleObject(unset, FALSE, &twenty_ms) != STATUS_TIMEOUT)
		return 10;
	if (!QueueUserAPC(count_apc, GetCurrentThread(), 4) || NtWaitForSingleObject(unset, TRUE, &one_second) != STATUS_USER_APC || apcs != 7)
		return 11;
	const HANDLE both[] = {unset, set};
	if (NtWaitForMultipleObjects(2, both, 1, FALSE, &one_second) != 1 || NtWaitForMultipleObjects(2, both, 0, FALSE, &twenty_ms) != STATUS_TIMEOUT)
		return 12;
	CloseHandle(thread);
	CloseHandle(set);
	CloseHandle(unset);
	return 0;
}
