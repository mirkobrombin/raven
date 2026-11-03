#include <windows.h>

static DWORD tls, fls;

// A new thread starts with empty slots, even when it gets the thread block of one that ended.
static DWORD WINAPI worker(void *parameter)
{
	(void)parameter;
	if (TlsGetValue(tls) || FlsGetValue(fls))
		return 1;
	TlsSetValue(tls, (void *)0x1234);
	FlsSetValue(fls, (void *)0x5678);
	return 0;
}

int main(void)
{
	tls = TlsAlloc();
	fls = FlsAlloc(NULL);
	if (tls == TLS_OUT_OF_INDEXES || fls == FLS_OUT_OF_INDEXES)
		return 1;
	for (int round = 0; round < 32; round++) {
		HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
		DWORD code = 1;
		if (!thread || WaitForSingleObject(thread, 10000) || !GetExitCodeThread(thread, &code))
			return 2;
		CloseHandle(thread);
		if (code)
			return 10 + round;
	}
	return 0;
}
