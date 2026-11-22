#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* A system service's export has the 24 bytes of Windows 10: mov r10, rcx; mov eax, number; test byte [flag], 1; jnz; syscall; ret;
   another way into the service in two bytes; ret. A sandbox checks those bytes, copies them elsewhere and runs the copy, so the other
   way works from any address. With a host thread for each guest thread the flag is set and the export makes no system call; a program
   run one thread at a time finds it clear, which the exit code 100 tells. A system call of the program's own works either way. */

typedef LONG(NTAPI *query_event)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef LONG(NTAPI *set_event)(HANDLE, LONG *);

static int entry_shape(const BYTE *code)
{
	return code[0] == 0x4c && code[1] == 0x8b && code[2] == 0xd1 && code[3] == 0xb8 && code[8] == 0xf6 && code[9] == 0x04 &&
		code[10] == 0x25 && code[15] == 0x01 && code[16] == 0x75 && code[17] == 0x03 && code[18] == 0x0f && code[19] == 0x05 &&
		code[20] == 0xc3 && code[23] == 0xc3;
}

static int queried(query_event query, HANDLE event, LONG signaled)
{
	LONG state[2] = {-1, -1};
	ULONG length = 0;
	return query(event, 0, state, sizeof state, &length) == 0 && length == sizeof state && state[0] == 0 && state[1] == signaled;
}

int raven_entry(void)
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	const BYTE *query = (const BYTE *)GetProcAddress(ntdll, "NtQueryEvent");
	const BYTE *set = (const BYTE *)GetProcAddress(ntdll, "NtSetEvent");
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	volatile BYTE *room = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (!query || !set || !event || !room)
		return 1;
	if (!entry_shape(query) || !entry_shape(set))
		return 2;
	for (unsigned at = 0; at < 24; ++at) {
		room[at] = query[at];
		room[32 + at] = set[at];
	}
	/* The system call alone, as a program that keeps only a service's number makes it. */
	for (unsigned at = 0; at < 8; ++at)
		room[64 + at] = query[at];
	room[72] = 0x0f;
	room[73] = 0x05;
	room[74] = 0xc3;
	FlushInstructionCache(GetCurrentProcess(), (const void *)room, 4096);
	const query_event exported = (query_event)(ULONG_PTR)query, moved = (query_event)(ULONG_PTR)room;
	const query_event raw = (query_event)(ULONG_PTR)(room + 64);
	const set_event moved_set = (set_event)(ULONG_PTR)(room + 32);
	if (!queried(exported, event, 0))
		return 3;
	if (!queried(moved, event, 0))
		return 4;
	if (!queried(raw, event, 0))
		return 5;
	/* A service the host answers in the turn comes in the same way. */
	LONG previous = -1;
	if (moved_set(event, &previous) != 0 || previous != 0 || !queried(exported, event, 1))
		return 6;
	LONG state[2];
	if ((ULONG)moved((HANDLE)(ULONG_PTR)0xfff0, 0, state, sizeof state, NULL) != 0xc0000008UL ||
		(ULONG)moved(event, 0, state, 4, NULL) != 0xc0000004UL)
		return 7;
	for (unsigned round = 0; round < 10000; ++round)
		if (!queried(round & 1 ? moved : exported, event, 1))
			return 8;
	DWORD flag;
	((BYTE *)&flag)[0] = query[11];
	((BYTE *)&flag)[1] = query[12];
	((BYTE *)&flag)[2] = query[13];
	((BYTE *)&flag)[3] = query[14];
	return *(const volatile BYTE *)(ULONG_PTR)flag & 1 ? 0 : 100;
}
