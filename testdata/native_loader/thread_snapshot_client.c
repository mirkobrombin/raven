#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

static DWORD WINAPI worker(void *argument)
{
	(void)argument;
	return 0;
}

static int find_thread(HANDLE snapshot, DWORD owner, DWORD id, LONG priority)
{
	THREADENTRY32 entry = {sizeof(entry)};
	BOOL more = Thread32First(snapshot, &entry);
	int found = 0;
	while (more) {
		if (entry.dwSize != sizeof(entry) || entry.cntUsage || entry.tpDeltaPri || entry.dwFlags)
			return -1;
		if (entry.th32OwnerProcessID == owner && entry.th32ThreadID == id) {
			if (entry.tpBasePri != priority || found)
				return -1;
			found = 1;
		}
		more = Thread32Next(snapshot, &entry);
	}
	return GetLastError() == ERROR_NO_MORE_FILES ? found : -1;
}

/* The child started with "parent": its parent in the process snapshot is named by its program, the same file as the child's,
   however the parent was started. */
static int check_parent(void)
{
	WCHAR own[MAX_PATH], parent_name[MAX_PATH];
	DWORD parent = 0;
	BOOL found_self = FALSE, found_parent = FALSE;
	PROCESSENTRY32W entry;
	/* Two passes: the first finds this process and its parent's id, the second the parent. */
	for (int pass = 0; pass < 2; ++pass) {
		HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (processes == INVALID_HANDLE_VALUE)
			return 20;
		entry.dwSize = sizeof(entry);
		for (BOOL more = Process32FirstW(processes, &entry); more; more = Process32NextW(processes, &entry)) {
			if (!pass && entry.th32ProcessID == GetCurrentProcessId()) {
				parent = entry.th32ParentProcessID;
				lstrcpynW(own, entry.szExeFile, MAX_PATH);
				found_self = TRUE;
			} else if (pass && found_self && entry.th32ProcessID == parent) {
				lstrcpynW(parent_name, entry.szExeFile, MAX_PATH);
				found_parent = TRUE;
			}
			entry.dwSize = sizeof(entry);
		}
		CloseHandle(processes);
	}
	if (!found_self || !found_parent)
		return 21;
	return lstrcmpiW(own, parent_name) ? 22 : 0;
}

static BOOL started_as_child(void)
{
	const WCHAR *line = GetCommandLineW();
	for (; *line; ++line)
		if (line[0] == L'p' && line[1] == L'a' && line[2] == L'r' && line[3] == L'e' && line[4] == L'n' && line[5] == L't' && !line[6])
			return TRUE;
	return FALSE;
}

int raven_entry(void)
{
	if (started_as_child())
		return check_parent();
	DWORD id, later_id;
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, &id);
	if (!thread || !SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL))
		return 1;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0xffffffff);
	if (snapshot == INVALID_HANDLE_VALUE || find_thread(snapshot, GetCurrentProcessId(), GetCurrentThreadId(), 8) != 1 ||
		find_thread(snapshot, GetCurrentProcessId(), id, 9) != 1)
		return 2;
	struct { THREADENTRY32 entry; DWORD sentinel; } oversized = {{sizeof(oversized)}, 0x12345678};
	if (!Thread32First(snapshot, &oversized.entry) || oversized.entry.dwSize != sizeof(THREADENTRY32) || oversized.sentinel != 0x12345678)
		return 3;
	THREADENTRY32 entry = {0};
	if (Thread32First(snapshot, &entry) || GetLastError() != ERROR_BAD_LENGTH)
		return 4;
	entry.dwSize = sizeof(entry);
	if (Thread32First(INVALID_HANDLE_VALUE, &entry) || GetLastError() != ERROR_INVALID_HANDLE)
		return 5;
	HANDLE empty = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (empty == INVALID_HANDLE_VALUE || Thread32First(empty, &entry) || GetLastError() != ERROR_NO_MORE_FILES)
		return 6;
	CloseHandle(empty);
	HANDLE later = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, &later_id);
	if (!later || find_thread(snapshot, GetCurrentProcessId(), later_id, 8) != 0 || !TerminateThread(thread, 0))
		return 7;
	if (find_thread(snapshot, GetCurrentProcessId(), id, 9) != 1)
		return 8;
	HANDLE current = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (current == INVALID_HANDLE_VALUE || find_thread(current, GetCurrentProcessId(), id, 9) != 0 ||
		find_thread(current, GetCurrentProcessId(), later_id, 8) != 1)
		return 9;
	CloseHandle(current);
	TerminateThread(later, 0);
	CloseHandle(later);
	CloseHandle(thread);
	CloseHandle(snapshot);
	WCHAR path[MAX_PATH];
	STARTUPINFOW startup = {sizeof(startup)};
	PROCESS_INFORMATION child;
	if (!GetModuleFileNameW(NULL, path, MAX_PATH) || !CreateProcessW(path, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &child))
		return 10;
	HANDLE peers = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, GetCurrentProcessId());
	int result = peers != INVALID_HANDLE_VALUE && GetThreadId(child.hThread) == child.dwThreadId &&
		find_thread(peers, child.dwProcessId, child.dwThreadId, 8) == 1;
	TerminateProcess(child.hProcess, 0);
	WaitForSingleObject(child.hProcess, 5000);
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	if (peers != INVALID_HANDLE_VALUE)
		CloseHandle(peers);
	if (!result)
		return 11;
	/* A child reads this process's name from the snapshot; the test starts this process with arguments of its own. */
	WCHAR line[MAX_PATH + 16];
	line[0] = L'"';
	lstrcpynW(line + 1, path, MAX_PATH);
	lstrcatW(line, L"\" parent");
	DWORD code = 1;
	if (!CreateProcessW(path, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child))
		return 12;
	WaitForSingleObject(child.hProcess, 20000);
	GetExitCodeProcess(child.hProcess, &code);
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	return code ? (int)code : 0;
}
