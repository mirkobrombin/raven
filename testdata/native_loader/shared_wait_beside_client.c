// Waits for events other processes share go on beside the other calls of the process: a thread here and a thread of a child process
// (this program started with "child") play ping-pong on two named events, waiting for one alone and for one of two, and a wait for a
// named event nobody sets still times out. The exit code names the first check that failed.
#include <windows.h>

#define ROUNDS 1500

static HANDLE ping, pong, never;

static DWORD WINAPI player(void *unused)
{
	(void)unused;
	HANDLE either[2] = {never, pong};
	for (DWORD round = 0; round < ROUNDS; ++round) {
		if (!SetEvent(ping))
			return 1;
		// Every other round waits for one of two, the shared event that is never set first.
		const DWORD result = round & 1 ? WaitForMultipleObjects(2, either, FALSE, 5000) : WaitForSingleObject(pong, 5000);
		if (result != (round & 1 ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0))
			return 2;
	}
	return WaitForSingleObject(never, 50) == WAIT_TIMEOUT ? 0 : 3;
}

static DWORD WINAPI answerer(void *unused)
{
	(void)unused;
	for (DWORD round = 0; round < ROUNDS; ++round) {
		if (WaitForSingleObject(ping, 5000) != WAIT_OBJECT_0)
			return 1;
		if (!SetEvent(pong))
			return 2;
	}
	return 0;
}

static int run(LPTHREAD_START_ROUTINE routine)
{
	HANDLE thread = CreateThread(NULL, 0, routine, NULL, 0, NULL);
	DWORD code = 1;
	if (!thread || WaitForSingleObject(thread, 60000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code))
		return 9;
	return (int)code;
}

int raven_entry(void)
{
	const char *line = GetCommandLineA();
	int child = 0;
	for (const char *at = line; *at; ++at)
		if (at[0] == 'c' && at[1] == 'h' && at[2] == 'i' && at[3] == 'l' && at[4] == 'd')
			child = 1;
	if (child) {
		ping = OpenEventA(SYNCHRONIZE, FALSE, "RavenSharedBesidePing");
		pong = OpenEventA(EVENT_MODIFY_STATE, FALSE, "RavenSharedBesidePong");
		return ping && pong ? run(answerer) : 8;
	}
	ping = CreateEventA(NULL, FALSE, FALSE, "RavenSharedBesidePing");
	pong = CreateEventA(NULL, FALSE, FALSE, "RavenSharedBesidePong");
	never = CreateEventA(NULL, TRUE, FALSE, "RavenSharedBesideNever");
	if (!ping || !pong || !never)
		return 10;
	char path[MAX_PATH], command[MAX_PATH + 16];
	const DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
	if (!length || length >= MAX_PATH)
		return 11;
	DWORD at = 0;
	command[at++] = '"';
	for (DWORD index = 0; index < length; ++index)
		command[at++] = path[index];
	for (const char *rest = "\" child"; *rest; ++rest)
		command[at++] = *rest;
	command[at] = 0;
	STARTUPINFOA startup = {sizeof startup};
	PROCESS_INFORMATION process;
	if (!CreateProcessA(path, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
		return 12;
	const int played = run(player);
	if (played)
		return 20 + played;
	DWORD code = 1;
	if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &code) || code)
		return 30 + (int)code;
	return 0;
}
