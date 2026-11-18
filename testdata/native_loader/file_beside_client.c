// Transfers of plain files go on beside the other calls of the process: two threads each write records to a file of their own, read
// them back and ask for the file pointer and size many times, and both append records to one shared handle, whose file pointer must hand each write a place of its own. A
// read at the end of a file still succeeds with no bytes, and a read without a count or an OVERLAPPED still fails with
// ERROR_INVALID_PARAMETER. All of it runs several times over, so that threads end while the others' transfers are served: a transfer
// answered beside the turn reaches the guest's memory whichever thread the turn has just switched to. The exit code names the first
// check that failed.
#include <windows.h>

#define ROUNDS 1200
#define RECORD 512
#define PASSES 8

static HANDLE shared;

static int same(const BYTE *a, const BYTE *b, DWORD size)
{
	for (DWORD at = 0; at < size; ++at)
		if (a[at] != b[at])
			return 0;
	return 1;
}

static void fill(BYTE *record, DWORD id, DWORD round)
{
	for (DWORD at = 0; at < RECORD; ++at)
		record[at] = (BYTE)(id * 101 + round * 7 + at);
}

static DWORD WINAPI worker(void *parameter)
{
	const DWORD id = (DWORD)(ULONG_PTR)parameter;
	WCHAR name[] = L"beside0.bin";
	name[6] = (WCHAR)(L'0' + id);
	HANDLE own = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (own == INVALID_HANDLE_VALUE)
		return 1;
	BYTE record[RECORD], back[RECORD];
	for (DWORD round = 0; round < ROUNDS; ++round) {
		DWORD done = 0;
		fill(record, id, round);
		if (!WriteFile(own, record, RECORD, &done, NULL) || done != RECORD)
			return 2;
		if (SetFilePointer(own, -RECORD, NULL, FILE_CURRENT) == INVALID_SET_FILE_POINTER)
			return 3;
		if (!ReadFile(own, back, RECORD, &done, NULL) || done != RECORD || !same(record, back, RECORD))
			return 4;
		// The pointer and the size, asked in both forms, follow the records written so far.
		LARGE_INTEGER zero = {0}, at, size;
		DWORD high = 1;
		if (!SetFilePointerEx(own, zero, &at, FILE_CURRENT) || at.QuadPart != (LONGLONG)(round + 1) * RECORD)
			return 12;
		if (GetFileSize(own, &high) != (round + 1) * RECORD || high || !GetFileSizeEx(own, &size) || size.QuadPart != at.QuadPart)
			return 13;
		// A record of the shared file starts with the writer and the round, so the reader can tell each one apart.
		*(DWORD *)record = id;
		*(DWORD *)(record + 4) = round;
		if (!WriteFile(shared, record, RECORD, &done, NULL) || done != RECORD)
			return 5;
	}
	DWORD done = 1;
	if (!ReadFile(own, back, RECORD, &done, NULL) || done)
		return 6;
	SetLastError(0);
	if (ReadFile(own, back, RECORD, NULL, NULL) || GetLastError() != ERROR_INVALID_PARAMETER)
		return 7;
	CloseHandle(own);
	return DeleteFileW(name) ? 0 : 8;
}

// Every record of both writers is in the shared file once, whole, and no two overlap. A thread of its own reads them, as the writers did.
static DWORD WINAPI check(void *unused)
{
	(void)unused;
	LARGE_INTEGER size;
	if (!GetFileSizeEx(shared, &size) || size.QuadPart != 2LL * ROUNDS * RECORD)
		return 2;
	if (SetFilePointer(shared, 0, NULL, FILE_BEGIN))
		return 3;
	static BYTE seen[2][ROUNDS];
	BYTE record[RECORD], expected[RECORD];
	for (DWORD round = 0; round < ROUNDS; ++round)
		seen[0][round] = seen[1][round] = 0;
	for (DWORD index = 0; index < 2 * ROUNDS; ++index) {
		DWORD done = 0;
		if (!ReadFile(shared, record, RECORD, &done, NULL) || done != RECORD)
			return 4;
		const DWORD id = *(DWORD *)record, round = *(DWORD *)(record + 4);
		if (id > 1 || round >= ROUNDS || seen[id][round]++)
			return 5;
		fill(expected, id, round);
		if (!same(record + 8, expected + 8, RECORD - 8))
			return 6;
	}
	return 0;
}

static int pass(void)
{
	shared = CreateFileW(L"beside-shared.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (shared == INVALID_HANDLE_VALUE)
		return 9;
	HANDLE threads[2];
	for (DWORD id = 0; id < 2; ++id)
		if (!(threads[id] = CreateThread(NULL, 0, worker, (void *)(ULONG_PTR)id, 0, NULL)))
			return 10;
	if (WaitForMultipleObjects(2, threads, TRUE, 60000) != WAIT_OBJECT_0)
		return 11;
	for (DWORD id = 0; id < 2; ++id) {
		DWORD code = 1;
		if (!GetExitCodeThread(threads[id], &code) || code)
			return 20 + (int)id * 10 + (int)code;
	}
	HANDLE checker = CreateThread(NULL, 0, check, NULL, 0, NULL);
	DWORD code = 1;
	if (!checker || WaitForSingleObject(checker, 60000) != WAIT_OBJECT_0 || !GetExitCodeThread(checker, &code) || code)
		return 40 + (int)code;
	CloseHandle(checker);
	CloseHandle(threads[0]);
	CloseHandle(threads[1]);
	CloseHandle(shared);
	return DeleteFileW(L"beside-shared.bin") ? 0 : 17;
}

int raven_entry(void)
{
	for (int round = 0; round < PASSES; ++round) {
		const int outcome = pass();
		if (outcome)
			return outcome;
	}
	return 0;
}
