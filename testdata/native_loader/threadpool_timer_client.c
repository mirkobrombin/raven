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

static volatile LONG work_count, work_chained, work_threads[128], work_overflow;

/* The first three hundred callbacks each ask for one more run, as .NET's dispatcher asks for another thread before it takes an item
   from its queue. Each notes the thread it runs on. */
static void CALLBACK counted(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work)
{
	const LONG self = (LONG)GetCurrentThreadId();
	int slot = 0;
	while (slot < 128 && work_threads[slot] != self && InterlockedCompareExchange(&work_threads[slot], self, 0) != 0)
		++slot;
	if (slot == 128)
		work_overflow = 1;
	if (InterlockedIncrement(&work_chained) <= 300)
		SubmitThreadpoolWork(work);
	InterlockedIncrement(&work_count);
}

static volatile LONG wait_count;
static TP_WAIT_RESULT wait_result;
static PTP_WAIT wait_seen;
static PVOID wait_context;
static PTP_CALLBACK_INSTANCE wait_instance;
static HANDLE wait_done;

static void CALLBACK waited(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WAIT wait, TP_WAIT_RESULT result)
{
	wait_instance = instance;
	wait_context = context;
	wait_seen = wait;
	wait_result = result;
	InterlockedIncrement(&wait_count);
	SetEvent(wait_done);
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
static HANDLE binders_go;
static WCHAR binder_folder[MAX_PATH];
static LONG binder_failures;
static void CALLBACK finished(PTP_CALLBACK_INSTANCE instance, PVOID context, PVOID overlapped, ULONG result, ULONG_PTR bytes, PTP_IO io);

static DWORD WINAPI binder(void *unused)
{
	WCHAR path[MAX_PATH];
	WaitForSingleObject(binders_go, INFINITE);
	for (int round = 0; round < 200; ++round) {
		if (!GetTempFileNameW(binder_folder, L"tpb", 0, path)) {
			InterlockedIncrement(&binder_failures);
			continue;
		}
		HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
		PTP_IO io = file != INVALID_HANDLE_VALUE ? CreateThreadpoolIo(file, finished, NULL, NULL) : NULL;
		if (!io)
			InterlockedIncrement(&binder_failures);
		else
			CloseThreadpoolIo(io);
		if (file != INVALID_HANDLE_VALUE)
			CloseHandle(file);
	}
	return 0;
}

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

	/* Threads that bind files to the pool at the same time all get their I/O objects, however their turns fall, as the threads of
	   .NET's pool open their files together. */
	HANDLE binders[8];
	binders_go = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!binders_go || !GetTempPathW(MAX_PATH, binder_folder)) return 26;
	for (int index = 0; index < 8; ++index)
		if (!(binders[index] = CreateThread(NULL, 0, binder, NULL, 0, NULL))) return 26;
	SetEvent(binders_go);
	if (WaitForMultipleObjects(8, binders, TRUE, 20000) != WAIT_OBJECT_0 || binder_failures) return 27;
	for (int index = 0; index < 8; ++index)
		CloseHandle(binders[index]);
	CloseHandle(binders_go);

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

	/* Submitted work waits in the pool's queue for a free worker, as on Windows, rather than starting a thread each: three hundred
	   submissions in a row and three hundred more from the callbacks all run, on workers the pool reuses, and the wait sees every one of
	   them finished. */
	PTP_WORK work = CreateThreadpoolWork(counted, NULL, NULL);
	if (!work) return 23;
	for (int index = 0; index < 300; ++index)
		SubmitThreadpoolWork(work);
	WaitForThreadpoolWorkCallbacks(work, FALSE);
	if (work_count != 600) return 24;
	if (work_overflow) return 25;
	CloseThreadpoolWork(work);

	/* Thread pool waits, as CoreMessaging waits for its queues: a wait starts unset, runs its callback once when its object is
	   signaled or its timeout passes, with the instance, the context, itself and the result, and has to be set again to run again. */
	wait_done = CreateEventW(NULL, FALSE, FALSE, NULL);
	HANDLE signal = CreateEventW(NULL, FALSE, FALSE, NULL);
	PTP_WAIT pool_wait = CreateThreadpoolWait(waited, (PVOID)0x7170, NULL);
	if (!wait_done || !signal || !pool_wait) return 28;
	SetThreadpoolWait(pool_wait, signal, NULL);
	SetEvent(signal);
	if (WaitForSingleObject(wait_done, 5000) != WAIT_OBJECT_0) return 29;
	WaitForThreadpoolWaitCallbacks(pool_wait, FALSE);
	if (wait_count != 1 || wait_result != WAIT_OBJECT_0 || wait_seen != pool_wait || wait_context != (PVOID)0x7170 || !wait_instance)
		return 30;
	SetEvent(signal);
	Sleep(60);
	if (wait_count != 1 || WaitForSingleObject(signal, 0) != WAIT_OBJECT_0) return 31;
	due = from_now(20);
	SetThreadpoolWait(pool_wait, signal, &due);
	if (WaitForSingleObject(wait_done, 5000) != WAIT_OBJECT_0) return 32;
	WaitForThreadpoolWaitCallbacks(pool_wait, FALSE);
	if (wait_count != 2 || wait_result != WAIT_TIMEOUT) return 32;
	/* A wait taken back before its object or its time comes never runs, and the Ex form says it was set. */
	due = from_now(600000);
	SetThreadpoolWait(pool_wait, signal, &due);
	if (!SetThreadpoolWaitEx(pool_wait, NULL, NULL, NULL) || SetThreadpoolWaitEx(pool_wait, NULL, NULL, NULL)) return 33;
	SetEvent(signal);
	Sleep(60);
	WaitForThreadpoolWaitCallbacks(pool_wait, TRUE);
	if (wait_count != 2) return 34;
	CloseThreadpoolWait(pool_wait);
	CloseHandle(signal);
	return 0;
}
