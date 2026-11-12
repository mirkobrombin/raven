#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static volatile LONG once_count, periodic_count, grouped_count;
static PTP_TIMER seen_timer;
static PVOID seen_context;
static PTP_CALLBACK_INSTANCE seen_instance;
static HANDLE once_done, periodic_done;

static void CALLBACK once(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer)
{
	seen_instance = instance;
	seen_context = context;
	seen_timer = timer;
	InterlockedIncrement(&once_count);
	SetEvent(once_done);
}

static void CALLBACK periodic(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer)
{
	if (InterlockedIncrement(&periodic_count) == 3)
		SetEvent(periodic_done);
}

static void CALLBACK grouped(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer)
{
	InterlockedIncrement(&grouped_count);
}

static volatile LONG io_count;
static PVOID io_context, io_overlapped;
static ULONG io_result;
static ULONG_PTR io_bytes;
static PTP_IO io_seen;
static PTP_CALLBACK_INSTANCE io_instance;
static HANDLE io_done;

static void CALLBACK finished(PTP_CALLBACK_INSTANCE instance, PVOID context, PVOID overlapped, ULONG result, ULONG_PTR bytes, PTP_IO io)
{
	io_instance = instance;
	io_context = context;
	io_overlapped = overlapped;
	io_result = result;
	io_bytes = bytes;
	io_seen = io;
	InterlockedIncrement(&io_count);
	SetEvent(io_done);
}

static FILETIME from_now(LONGLONG milliseconds)
{
	ULARGE_INTEGER due;
	due.QuadPart = (ULONGLONG)(-milliseconds * 10000);
	FILETIME time = {due.LowPart, due.HighPart};
	return time;
}

/* Thread pool timers as runtimes schedule their work with them: a timer starts unset, runs once at a relative or an absolute due
 * time with the context and itself, runs every period until a null due time stops it, and stops with the members of its cleanup group. */
int raven_entry(void)
{
	once_done = CreateEventW(NULL, FALSE, FALSE, NULL);
	periodic_done = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!once_done || !periodic_done) return 1;
	PTP_TIMER timer = CreateThreadpoolTimer(once, (PVOID)0x5150, NULL);
	if (!timer) return 2;
	if (IsThreadpoolTimerSet(timer)) return 3;
	FILETIME due = from_now(20);
	SetThreadpoolTimer(timer, &due, 0, 0);
	if (!IsThreadpoolTimerSet(timer)) return 4;
	if (WaitForSingleObject(once_done, 5000) != WAIT_OBJECT_0) return 5;
	WaitForThreadpoolTimerCallbacks(timer, FALSE);
	if (once_count != 1 || seen_timer != timer || seen_context != (PVOID)0x5150 || !seen_instance) return 6;

	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER absolute = {{now.dwLowDateTime, now.dwHighDateTime}};
	absolute.QuadPart += 20 * 10000;
	due.dwLowDateTime = absolute.LowPart;
	due.dwHighDateTime = absolute.HighPart;
	SetThreadpoolTimer(timer, &due, 0, 0);
	if (WaitForSingleObject(once_done, 5000) != WAIT_OBJECT_0 || once_count != 2) return 7;

	/* A far due time taken back with a null one never runs, and the Ex form says the timer was set. */
	due = from_now(600000);
	SetThreadpoolTimer(timer, &due, 0, 0);
	if (!SetThreadpoolTimerEx(timer, NULL, 0, 0)) return 8;
	if (IsThreadpoolTimerSet(timer)) return 9;
	if (SetThreadpoolTimerEx(timer, NULL, 0, 0)) return 10;
	WaitForThreadpoolTimerCallbacks(timer, TRUE);
	CloseThreadpoolTimer(timer);

	PTP_TIMER repeating = CreateThreadpoolTimer(periodic, NULL, NULL);
	if (!repeating) return 11;
	due = from_now(10);
	SetThreadpoolTimer(repeating, &due, 10, 0);
	if (WaitForSingleObject(periodic_done, 5000) != WAIT_OBJECT_0) return 12;
	SetThreadpoolTimer(repeating, NULL, 0, 0);
	WaitForThreadpoolTimerCallbacks(repeating, TRUE);
	const LONG stopped = periodic_count;
	Sleep(80);
	if (periodic_count != stopped || once_count != 2) return 13;
	CloseThreadpoolTimer(repeating);

	PTP_CLEANUP_GROUP group = CreateThreadpoolCleanupGroup();
	if (!group) return 14;
	TP_CALLBACK_ENVIRON environment;
	InitializeThreadpoolEnvironment(&environment);
	SetThreadpoolCallbackCleanupGroup(&environment, group, NULL);
	PTP_TIMER member = CreateThreadpoolTimer(grouped, NULL, &environment);
	if (!member) return 15;
	due = from_now(5);
	SetThreadpoolTimer(member, &due, 5, 0);
	Sleep(60);
	CloseThreadpoolCleanupGroupMembers(group, FALSE, NULL);
	const LONG closed = grouped_count;
	Sleep(60);
	if (!closed || grouped_count != closed) return 16;
	CloseThreadpoolCleanupGroup(group);

	/* An overlapped file bound to the pool: each operation started after StartThreadpoolIo ends in the callback with the context, the
	   operation, its result and the bytes moved, and the I/O object itself, as .NET's file streams write through it. */
	io_done = CreateEventW(NULL, FALSE, FALSE, NULL);
	WCHAR folder[MAX_PATH], path[MAX_PATH];
	if (!io_done || !GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"tpi", 0, path)) return 17;
	HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
	if (file == INVALID_HANDLE_VALUE) return 18;
	PTP_IO io = CreateThreadpoolIo(file, finished, (PVOID)0x6160, NULL);
	if (!io) return 19;
	OVERLAPPED operation = {0};
	StartThreadpoolIo(io);
	if (!WriteFile(file, "hello", 5, NULL, &operation) && GetLastError() != ERROR_IO_PENDING) {
		CancelThreadpoolIo(io);
		return 20;
	}
	if (WaitForSingleObject(io_done, 5000) != WAIT_OBJECT_0) return 21;
	WaitForThreadpoolIoCallbacks(io, FALSE);
	if (io_count != 1 || io_context != (PVOID)0x6160 || io_overlapped != &operation || io_result != 0 || io_bytes != 5 || io_seen != io ||
		!io_instance) return 22;
	CloseThreadpoolIo(io);
	CloseHandle(file);
	return 0;
}
