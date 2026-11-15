#include <windows.h>

/* An APC queued to a thread that has ended is refused with ERROR_GEN_FAILURE, by QueueUserAPC and QueueUserAPC2; one queued to a live
 * thread through either call still runs in its next alertable wait. */
static volatile ULONG_PTR ran;

static void CALLBACK count(ULONG_PTR value)
{
	ran += value;
}

static DWORD WINAPI quick(void *parameter)
{
	return 0;
}

typedef BOOL (WINAPI *QueueApc2)(PAPCFUNC, HANDLE, ULONG_PTR, int);

UINT raven_entry(void)
{
	HANDLE thread = CreateThread(NULL, 0, quick, NULL, 0, NULL);
	if (!thread || WaitForSingleObject(thread, 10000))
		return 1;
	SetLastError(0);
	if (QueueUserAPC(count, thread, 1) || GetLastError() != ERROR_GEN_FAILURE)
		return 2;
	QueueApc2 queue2 = (QueueApc2)GetProcAddress(GetModuleHandleA("kernel32.dll"), "QueueUserAPC2");
	if (!queue2)
		return 3;
	SetLastError(0);
	if (queue2(count, thread, 1, 0) || GetLastError() != ERROR_GEN_FAILURE)
		return 4;
	CloseHandle(thread);
	if (!QueueUserAPC(count, GetCurrentThread(), 2) || SleepEx(0, TRUE) != WAIT_IO_COMPLETION || ran != 2)
		return 5;
	/* QUEUE_USER_APC_FLAGS_NONE: a plain user APC, which the next alertable wait runs with its whole pointer-sized argument. */
	const ULONG_PTR wide = sizeof(ULONG_PTR) == 8 ? (ULONG_PTR)0x1234567800000003ULL : 3;
	if (!queue2(count, GetCurrentThread(), wide, 0) || SleepEx(0, TRUE) != WAIT_IO_COMPLETION || ran != 2 + wide)
		return 6;
	return 0;
}
