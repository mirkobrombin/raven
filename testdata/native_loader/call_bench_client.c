#include <windows.h>

// A fixed measure of what crossing into the system costs: one line per case with the nanoseconds each operation took, the same program
// under any runtime. The cases: a call that does next to nothing (SetEvent on an event nobody waits for), a wake handed back and forth
// between two threads, WaitForMultipleObjects for any and for all of eight signalled events, and four threads contending for a critical
// section and for an SRW lock. A command line of "short" runs each case a hundredth as many times, to check that it runs at all.

static HANDLE out;
static LARGE_INTEGER frequency;

static void write_text(const char *text)
{
	DWORD written = 0, length = 0;
	while (text[length])
		++length;
	WriteFile(out, text, length, &written, 0);
}

static void write_number(unsigned long long value)
{
	char digits[24];
	int at = 23;
	digits[at] = 0;
	do {
		digits[--at] = (char)('0' + value % 10);
		value /= 10;
	} while (value);
	write_text(digits + at);
}

static unsigned long long now(void)
{
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (unsigned long long)counter.QuadPart;
}

static void report(const char *name, unsigned long long started, unsigned long long operations)
{
	const unsigned long long ticks = now() - started;
	write_text(name);
	write_text(" ");
	write_number(ticks * 1000000000ULL / (unsigned long long)frequency.QuadPart / operations);
	write_text(" ns/op ");
	write_number(operations);
	write_text(" ops\r\n");
}

static HANDLE ping, pong;
static volatile LONG rounds;

static DWORD WINAPI ponger(void *unused)
{
	(void)unused;
	for (LONG round = 0; round < rounds; ++round) {
		WaitForSingleObject(ping, INFINITE);
		SetEvent(pong);
	}
	return 0;
}

static CRITICAL_SECTION section;
static SRWLOCK lock = SRWLOCK_INIT;
static volatile LONG shared, turns;

static DWORD WINAPI section_worker(void *unused)
{
	(void)unused;
	for (LONG turn = 0; turn < turns; ++turn) {
		EnterCriticalSection(&section);
		++shared;
		LeaveCriticalSection(&section);
	}
	return 0;
}

static DWORD WINAPI lock_worker(void *unused)
{
	(void)unused;
	for (LONG turn = 0; turn < turns; ++turn) {
		AcquireSRWLockExclusive(&lock);
		++shared;
		ReleaseSRWLockExclusive(&lock);
	}
	return 0;
}

static int contend(const char *name, LPTHREAD_START_ROUTINE worker, LONG each)
{
	HANDLE threads[4];
	shared = 0;
	turns = each;
	const unsigned long long started = now();
	for (int index = 0; index < 4; ++index)
		if (!(threads[index] = CreateThread(0, 0, worker, 0, 0, 0)))
			return 0;
	WaitForMultipleObjects(4, threads, TRUE, INFINITE);
	report(name, started, (unsigned long long)each * 4);
	for (int index = 0; index < 4; ++index)
		CloseHandle(threads[index]);
	return shared == each * 4;
}

int raven_entry(void)
{
	out = GetStdHandle(STD_OUTPUT_HANDLE);
	QueryPerformanceFrequency(&frequency);
	const char *line = GetCommandLineA();
	int scale = 1;
	for (const char *at = line; *at; ++at)
		if (at[0] == 's' && at[1] == 'h' && at[2] == 'o' && at[3] == 'r' && at[4] == 't')
			scale = 100;

	HANDLE event = CreateEventA(0, TRUE, FALSE, 0);
	const LONG calls = 200000 / scale;
	unsigned long long started = now();
	for (LONG index = 0; index < calls; ++index)
		SetEvent(event);
	report("set_event", started, calls);

	ping = CreateEventA(0, FALSE, FALSE, 0);
	pong = CreateEventA(0, FALSE, FALSE, 0);
	rounds = 20000 / scale;
	HANDLE other = CreateThread(0, 0, ponger, 0, 0, 0);
	started = now();
	for (LONG round = 0; round < rounds; ++round) {
		SetEvent(ping);
		if (WaitForSingleObject(pong, 5000) != WAIT_OBJECT_0)
			return 2;
	}
	report("event_ping_pong", started, rounds);
	WaitForSingleObject(other, INFINITE);

	HANDLE events[8];
	for (int index = 0; index < 8; ++index)
		events[index] = CreateEventA(0, TRUE, TRUE, 0);
	const LONG waits = 100000 / scale;
	started = now();
	for (LONG index = 0; index < waits; ++index)
		if (WaitForMultipleObjects(8, events, FALSE, 0) != WAIT_OBJECT_0)
			return 3;
	report("wait_any_8", started, waits);
	started = now();
	for (LONG index = 0; index < waits; ++index)
		if (WaitForMultipleObjects(8, events, TRUE, 0) != WAIT_OBJECT_0)
			return 4;
	report("wait_all_8", started, waits);

	InitializeCriticalSection(&section);
	if (!contend("section_4_threads", section_worker, 200000 / scale))
		return 5;
	if (!contend("srw_4_threads", lock_worker, 200000 / scale))
		return 6;
	return 0;
}
