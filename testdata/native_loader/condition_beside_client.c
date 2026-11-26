// Two threads hand a turn back and forth through condition variables, as a renderer's threads hand each other its frames: under a
// slim lock, under a shared hold of it and under a critical section, without a wake getting lost; a sleep with nobody to wake it
// ends with ERROR_TIMEOUT. In parallel mode none of these sleeps or wakes takes the turn. The exit code names the first check that
// failed.
#include <windows.h>

#define ROUNDS 2000

static SRWLOCK lock = SRWLOCK_INIT;
static CRITICAL_SECTION section;
static CONDITION_VARIABLE changed = CONDITION_VARIABLE_INIT;
static volatile LONG turn;
static int kind;

static void hand(LONG mine)
{
	if (kind == 2) {
		EnterCriticalSection(&section);
		while (turn != mine)
			SleepConditionVariableCS(&changed, &section, 5000);
		turn = mine + 1;
		LeaveCriticalSection(&section);
		WakeAllConditionVariable(&changed);
		return;
	}
	const ULONG flags = kind == 1 ? CONDITION_VARIABLE_LOCKMODE_SHARED : 0;
	if (flags)
		AcquireSRWLockShared(&lock);
	else
		AcquireSRWLockExclusive(&lock);
	while (turn != mine)
		SleepConditionVariableSRW(&changed, &lock, 5000, flags);
	// A shared holder hands the turn on under an exclusive hold, which waits until the other thread sleeps and so cannot miss its wake.
	if (flags) {
		ReleaseSRWLockShared(&lock);
		AcquireSRWLockExclusive(&lock);
	}
	turn = mine + 1;
	ReleaseSRWLockExclusive(&lock);
	WakeConditionVariable(&changed);
}

static DWORD WINAPI partner(void *parameter)
{
	(void)parameter;
	for (LONG round = 0; round < ROUNDS; ++round)
		hand(round * 2 + 1);
	return 0;
}

int raven_entry(void)
{
	InitializeCriticalSection(&section);
	for (kind = 0; kind < 3; ++kind) {
		turn = 0;
		HANDLE thread = CreateThread(NULL, 0, partner, NULL, 0, NULL);
		if (!thread)
			return 1;
		for (LONG round = 0; round < ROUNDS; ++round)
			hand(round * 2);
		if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0 || turn != ROUNDS * 2)
			return 2 + kind;
		CloseHandle(thread);
	}
	AcquireSRWLockExclusive(&lock);
	SetLastError(0);
	const BOOL slept = SleepConditionVariableSRW(&changed, &lock, 50, 0);
	const DWORD error = GetLastError();
	ReleaseSRWLockExclusive(&lock);
	if (slept || error != ERROR_TIMEOUT)
		return 5;
	EnterCriticalSection(&section);
	SetLastError(0);
	if (SleepConditionVariableCS(&changed, &section, 50) || GetLastError() != ERROR_TIMEOUT)
		return 6;
	LeaveCriticalSection(&section);
	return 0;
}
