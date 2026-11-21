#include <windows.h>

/* What the calls a program makes most often cost, each timed inside the program so that starting it is left out: a call answered without
 * leaving the process's user mode, one the kernel answers, setting an event nobody waits for, two threads handing a turn back and forth
 * through events, the same through waits for any or all of a set, and four threads taking one critical section or one slim lock. Each
 * prints "hotpath NAME NANOSECONDS" per operation, then "hotpath done". */

#define CONTENDERS 4

static LARGE_INTEGER frequency;
static HANDLE out;

static void write_text(const char *text)
{
	DWORD written, length = 0;
	while (text[length])
		++length;
	WriteFile(out, text, length, &written, NULL);
}

static void report(const char *name, LONGLONG ticks, LONGLONG operations)
{
	char line[96], digits[24];
	unsigned long long value = (unsigned long long)(ticks * 1000000000LL / frequency.QuadPart / operations);
	int count = 0, at = 0;
	do {
		digits[count++] = (char)('0' + value % 10);
		value /= 10;
	} while (value);
	for (const char *part = "hotpath "; *part; ++part)
		line[at++] = *part;
	for (const char *part = name; *part; ++part)
		line[at++] = *part;
	line[at++] = ' ';
	while (count)
		line[at++] = digits[--count];
	line[at++] = '\n';
	line[at] = 0;
	write_text(line);
}

static LONGLONG now(void)
{
	LARGE_INTEGER value;
	QueryPerformanceCounter(&value);
	return value.QuadPart;
}

/* Two threads handing a turn back and forth: the answering thread waits for `ping` (one of its events, or all of them) and sets `pong`. */
#define ROUNDS 20000

static HANDLE ping[4], pong;
static volatile LONG mode;

static DWORD WINAPI answer(void *parameter)
{
	(void)parameter;
	for (unsigned round = 0; round < ROUNDS; ++round) {
		DWORD waited = mode == 0 ? WaitForSingleObject(ping[0], INFINITE) :
			WaitForMultipleObjects(mode == 1 ? 4 : 2, ping, mode == 2, INFINITE);
		if (waited == WAIT_FAILED)
			return 1;
		SetEvent(pong);
	}
	return 0;
}

static int hand_over(const char *name, LONG how)
{
	mode = how;
	HANDLE thread = CreateThread(NULL, 0, answer, NULL, 0, NULL);
	if (!thread)
		return 0;
	const LONGLONG start = now();
	for (unsigned round = 0; round < ROUNDS; ++round) {
		if (how == 2) {
			SetEvent(ping[0]);
			SetEvent(ping[1]);
		} else {
			SetEvent(ping[how == 1 ? round % 4 : 0]);
		}
		if (WaitForSingleObject(pong, INFINITE) != WAIT_OBJECT_0)
			return 0;
	}
	const LONGLONG ticks = now() - start;
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	report(name, ticks, ROUNDS);
	return 1;
}

/* Four threads each taking the lock a number of times, released together. */
#define ACQUISITIONS 100000

static CRITICAL_SECTION section;
static SRWLOCK slim = SRWLOCK_INIT;
static HANDLE go;
static volatile LONG shared, slim_mode;

static DWORD WINAPI contend(void *parameter)
{
	(void)parameter;
	WaitForSingleObject(go, INFINITE);
	for (unsigned index = 0; index < ACQUISITIONS; ++index) {
		if (slim_mode) {
			AcquireSRWLockExclusive(&slim);
			++shared;
			ReleaseSRWLockExclusive(&slim);
		} else {
			EnterCriticalSection(&section);
			++shared;
			LeaveCriticalSection(&section);
		}
	}
	return 0;
}

static int contention(const char *name, LONG use_slim)
{
	HANDLE threads[CONTENDERS];
	slim_mode = use_slim;
	shared = 0;
	ResetEvent(go);
	for (int index = 0; index < CONTENDERS; ++index)
		if (!(threads[index] = CreateThread(NULL, 0, contend, NULL, 0, NULL)))
			return 0;
	const LONGLONG start = now();
	SetEvent(go);
	if (WaitForMultipleObjects(CONTENDERS, threads, TRUE, INFINITE) != WAIT_OBJECT_0)
		return 0;
	const LONGLONG ticks = now() - start;
	for (int index = 0; index < CONTENDERS; ++index)
		CloseHandle(threads[index]);
	if (shared != CONTENDERS * ACQUISITIONS)
		return 0;
	report(name, ticks, CONTENDERS * ACQUISITIONS);
	return 1;
}

void __stdcall entry(void)
{
	out = GetStdHandle(STD_OUTPUT_HANDLE);
	QueryPerformanceFrequency(&frequency);

	LONGLONG start = now();
	for (unsigned index = 0; index < 1000000; ++index)
		GetTickCount();
	report("user_call", now() - start, 1000000);

	start = now();
	for (unsigned index = 0; index < 20000; ++index)
		GetFileAttributesW(L"C:\\windows");
	report("kernel_call", now() - start, 20000);

	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	start = now();
	for (unsigned index = 0; index < 100000; ++index) {
		SetEvent(event);
		ResetEvent(event);
	}
	report("set_reset_event", now() - start, 200000);
	CloseHandle(event);

	for (int index = 0; index < 4; ++index)
		ping[index] = CreateEventW(NULL, FALSE, FALSE, NULL);
	pong = CreateEventW(NULL, FALSE, FALSE, NULL);
	go = CreateEventW(NULL, TRUE, FALSE, NULL);
	InitializeCriticalSection(&section);
	if (!hand_over("event_ping_pong", 0) || !hand_over("wait_any_ping_pong", 1) || !hand_over("wait_all_ping_pong", 2) ||
		!contention("critical_section_4", 0) || !contention("srw_lock_4", 1)) {
		write_text("hotpath failed\n");
		ExitProcess(1);
	}
	write_text("hotpath done\n");
	ExitProcess(0);
}
