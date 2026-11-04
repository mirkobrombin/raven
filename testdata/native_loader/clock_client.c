#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The millisecond clocks are the performance counter in milliseconds, whether the runtime or the program reads them. */

static LONGLONG distance(ULONGLONG left, ULONGLONG right)
{
	return left > right ? (LONGLONG)(left - right) : (LONGLONG)(right - left);
}

int raven_entry(void)
{
	/* Long enough for the runtime to have calibrated the time stamp counter against the performance counter. */
	Sleep(400);
	LARGE_INTEGER frequency, counter;
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart != 10000000 || !QueryPerformanceCounter(&counter))
		return 1;
	const ULONGLONG ticks = GetTickCount64();
	if (distance(ticks, (ULONGLONG)counter.QuadPart / 10000) > 5)
		return 2;
	if (distance(GetTickCount(), (DWORD)GetTickCount64()) > 5)
		return 3;
	Sleep(50);
	const ULONGLONG later = GetTickCount64();
	if (later < ticks + 45 || later > ticks + 250)
		return 4;
	/* The local time, which the program reads for each second the runtime publishes, agrees with the system time made local and
	 * goes on without stepping back over the end of a second. */
	FILETIME system, local, read;
	SYSTEMTIME now;
	GetSystemTimeAsFileTime(&system);
	GetLocalTime(&now);
	if (!FileTimeToLocalFileTime(&system, &local) || !SystemTimeToFileTime(&now, &read))
		return 5;
	const ULONGLONG made = ((ULONGLONG)local.dwHighDateTime << 32) | local.dwLowDateTime;
	ULONGLONG previous = ((ULONGLONG)read.dwHighDateTime << 32) | read.dwLowDateTime;
	if (distance(made, previous) > 500000)
		return 6;
	const ULONGLONG until = GetTickCount64() + 1200;
	while (GetTickCount64() < until) {
		GetLocalTime(&now);
		if (now.wMilliseconds > 999 || now.wSecond > 59 || !SystemTimeToFileTime(&now, &read))
			return 7;
		const ULONGLONG value = ((ULONGLONG)read.dwHighDateTime << 32) | read.dwLowDateTime;
		if (value + 50000 < previous)
			return 8;
		previous = value;
	}
	return 0;
}
