#include <windows.h>

// The clocks in KUSER_SHARED_DATA move: the interrupt time and the system time a program reads straight from the page advance
// across a sleep, and the system time agrees with GetSystemTimeAsFileTime. Returns 0, or the number of the check that failed.
static ULONGLONG read_time(volatile const ULONG *field)
{
	for (;;) {
		const ULONG high = field[1], low = field[0];
		if (high == field[2])
			return (ULONGLONG)high << 32 | low;
	}
}

int raven_entry(void)
{
	volatile const ULONG *interrupt_time = (const ULONG *)0x7ffe0008, *system_time = (const ULONG *)0x7ffe0014;
	const ULONGLONG before = read_time(interrupt_time), wall_before = read_time(system_time);
	Sleep(100);
	const ULONGLONG after = read_time(interrupt_time), wall_after = read_time(system_time);
	// 100 ms is a million units of 100 ns; the page may lag by a tick.
	if (after - before < 800000)
		return 1;
	if (wall_after - wall_before < 800000)
		return 2;
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	const ULONGLONG reported = (ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime;
	if (reported > wall_after + 2000000 || wall_after > reported + 2000000)
		return 3;
	/* The tick counts are the page's tick count times its multiplier, as Windows reads them: each falls between the page's value read
	   just before and just after it. */
	volatile const ULONG *tick_count = (const ULONG *)0x7ffe0320, *multiplier = (const ULONG *)0x7ffe0004;
	for (int round = 0; round < 1000; ++round) {
		const ULONGLONG first = read_time(tick_count) * *multiplier >> 24, counted = GetTickCount64(), short_count = GetTickCount();
		const ULONGLONG last = read_time(tick_count) * *multiplier >> 24;
		if (counted < first || counted > last)
			return 4;
		if ((ULONG)(short_count - (ULONG)first) > (ULONG)(last - first))
			return 5;
	}
	/* The page moves by the millisecond, finer than Windows' default tick, so a short sleep shows on it. */
	const ULONGLONG ticks_before = GetTickCount64();
	Sleep(5);
	if (GetTickCount64() - ticks_before < 4)
		return 6;
	return 0;
}
