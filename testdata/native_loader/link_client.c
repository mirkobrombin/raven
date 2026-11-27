// The Windows side of a link: the Linux program that started the runtime shares memory with this one, and the two take turns on its
// words. The first word is the other side's count and the second this side's answer to it; the third holds how many rounds to play. A
// word outside the link is refused, and a wait on a word nobody changes runs out. Returns 0 when every round was answered in turn.

#include <windows.h>

__declspec(dllimport) void *WINAPI RavenLinkMap(SIZE_T *size);
__declspec(dllimport) DWORD WINAPI RavenLinkWait(const volatile LONG *word, LONG seen, DWORD milliseconds);
__declspec(dllimport) BOOL WINAPI RavenLinkWake(volatile LONG *word);

DWORD raven_entry(void)
{
	SIZE_T size = 0;
	volatile LONG *const words = RavenLinkMap(&size);
	if (!words || size < 4096 || RavenLinkMap(NULL) != (void *)words)
		return 10;
	const LONG rounds = words[2];
	for (LONG round = 1; round <= rounds; ++round) {
		if (RavenLinkWait(&words[0], round - 1, 5000) != WAIT_OBJECT_0 || words[0] != round)
			return 20;
		words[1] = round;
		RavenLinkWake(&words[1]);
	}
	LONG outside = 0;
	if (RavenLinkWait(&outside, 0, 0) != WAIT_FAILED || GetLastError() != ERROR_INVALID_PARAMETER || RavenLinkWake(&outside))
		return 30;
	if (RavenLinkWait(&words[3], 0, 20) != WAIT_TIMEOUT)
		return 40;
	return 0;
}
