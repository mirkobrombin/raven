// Waking the threads that wait on an address goes on beside the other calls of the process: two threads hand a turn back and forth
// through WaitOnAddress and WakeByAddressSingle without a wake getting lost, and wakes that find nobody waiting return at once. The
// exit code names the first check that failed.
#include <windows.h>

static volatile LONG turn, finished;
static volatile LONG idle;

static DWORD WINAPI partner(void *parameter)
{
	(void)parameter;
	for (LONG round = 0; round < 2000; ++round) {
		LONG mine = round * 2 + 1;
		while (turn != mine) {
			LONG seen = turn;
			if (seen != mine)
				WaitOnAddress(&turn, &seen, sizeof seen, 5000);
		}
		InterlockedExchange(&turn, mine + 1);
		WakeByAddressSingle((void *)&turn);
	}
	InterlockedExchange(&finished, 1);
	return 0;
}

int raven_entry(void)
{
	HANDLE thread = CreateThread(NULL, 0, partner, NULL, 0, NULL);
	if (!thread)
		return 1;
	for (LONG round = 0; round < 2000; ++round) {
		LONG mine = round * 2;
		while (turn != mine) {
			LONG seen = turn;
			if (seen != mine)
				WaitOnAddress(&turn, &seen, sizeof seen, 5000);
		}
		InterlockedExchange(&turn, mine + 1);
		WakeByAddressSingle((void *)&turn);
	}
	if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0 || !finished || turn != 4000)
		return 2;
	for (int round = 0; round < 2000; ++round)
		WakeByAddressAll((void *)&idle);
	return 0;
}
