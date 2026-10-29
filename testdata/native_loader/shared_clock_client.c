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
	return 0;
}
