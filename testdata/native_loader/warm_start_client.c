#include <windows.h>
#include <tlhelp32.h>

/* A start served by a warm host: it says READY, reads one line from its standard input, writes it back after GOT, and returns 23, so
   whoever runs it sees the streams and the exit status the start had, wherever it ran. Then it says how many processes of its own name
   the process list shows and whether asking each for its image was quick: a start the host forked ahead is no process until its request
   comes, so this one alone is seen. */

static int is_own_name(const WCHAR *name)
{
	const WCHAR own[] = L"warm-start-client.exe";
	for (int index = 0; own[index]; index++) {
		WCHAR character = name[index];
		if (character >= 'A' && character <= 'Z')
			character = (WCHAR)(character + 'a' - 'A');
		if (character != own[index])
			return 0;
	}
	return !name[sizeof own / sizeof *own - 1];
}

int raven_entry(void)
{
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
	char line[256] = "GOT ";
	DWORD count = 0, done = 0;
	WriteFile(output, "READY\n", 6, &done, NULL);
	while (count < sizeof line - 5) {
		DWORD got = 0;
		if (!ReadFile(input, line + 4 + count, 1, &got, NULL) || !got)
			break;
		if (line[4 + count++] == '\n')
			break;
	}
	WriteFile(output, line, 4 + count, &done, NULL);
	int seen = 0, slow = 0;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W entry = {sizeof(entry)};
	for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
		if (!is_own_name(entry.szExeFile))
			continue;
		seen++;
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
		if (!process)
			continue;
		WCHAR path[MAX_PATH];
		DWORD size = MAX_PATH;
		const ULONGLONG before = GetTickCount64();
		QueryFullProcessImageNameW(process, 0, path, &size);
		slow |= GetTickCount64() - before > 2000;
		CloseHandle(process);
	}
	CloseHandle(snapshot);
	char report[] = "SEEN 0 QUICK\n";
	report[5] = (char)('0' + (seen > 9 ? 9 : seen));
	if (slow)
		report[7] = 'S', report[8] = 'L', report[9] = 'O', report[10] = 'W';
	WriteFile(output, report, sizeof report - 1, &done, NULL);
	return 23;
}
