#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* DuplicateHandle with a process that is neither the caller's child nor its parent, as a game's Steam client library does with the
   Steam client that started the game's launcher: the first process starts a second, which starts a third; the third opens the first,
   takes an event from it and sets it, and hands it an event of its own, already set. The third process's exit code is the value its
   event has in the first, which checks both events; a code that is not a multiple of four names the check that failed. */

static const wchar_t *after_space(const wchar_t *text)
{
	int quoted = 0;
	for (; *text && (quoted || *text != L' '); ++text)
		if (*text == L'"')
			quoted = !quoted;
	while (*text == L' ')
		++text;
	return text;
}

static ULONG_PTR number(const wchar_t **text)
{
	ULONG_PTR value = 0;
	while (**text >= L'0' && **text <= L'9')
		value = value * 10 + (ULONG_PTR)(*(*text)++ - L'0');
	while (**text == L' ')
		++*text;
	return value;
}

static void append_number(wchar_t *text, ULONG_PTR value)
{
	wchar_t digits[24];
	int count = 0;
	do digits[count++] = (wchar_t)(L'0' + value % 10); while (value /= 10);
	while (*text)
		++text;
	*text++ = L' ';
	while (count)
		*text++ = digits[--count];
	*text = 0;
}

/* Starts this program again with the words given and answers its exit code. */
static DWORD start(const wchar_t *mode, ULONG_PTR first, ULONG_PTR second)
{
	wchar_t command[MAX_PATH + 96] = L"\"";
	GetModuleFileNameW(NULL, command + 1, MAX_PATH);
	lstrcatW(command, L"\" ");
	lstrcatW(command, mode);
	append_number(command, first);
	append_number(command, second);
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION started;
	DWORD code = 9;
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &started))
		return 7;
	if (WaitForSingleObject(started.hProcess, 30000) != WAIT_OBJECT_0 || !GetExitCodeProcess(started.hProcess, &code))
		code = 11;
	CloseHandle(started.hThread);
	CloseHandle(started.hProcess);
	return code;
}

static DWORD third(ULONG_PTR first, ULONG_PTR event)
{
	HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, (DWORD)first), taken = NULL, mine, given = NULL;
	if (!process)
		return 1;
	if (!DuplicateHandle(process, (HANDLE)event, GetCurrentProcess(), &taken, 0, FALSE, DUPLICATE_SAME_ACCESS))
		return 2;
	if (!SetEvent(taken))
		return 3;
	mine = CreateEventW(NULL, TRUE, TRUE, NULL);
	if (!mine || !DuplicateHandle(GetCurrentProcess(), mine, process, &given, 0, FALSE, DUPLICATE_SAME_ACCESS) || !given)
		return 5;
	CloseHandle(mine);
	CloseHandle(taken);
	CloseHandle(process);
	return (DWORD)(ULONG_PTR)given;
}

int raven_entry(void)
{
	const wchar_t *words = after_space(GetCommandLineW());
	if (words[0] == L's' && words[1] == L' ') {
		words += 2;
		const ULONG_PTR first = number(&words), event = number(&words);
		return (int)start(L"t", first, event);
	}
	if (words[0] == L't' && words[1] == L' ') {
		words += 2;
		const ULONG_PTR first = number(&words), event = number(&words);
		return (int)third(first, event);
	}
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!event)
		return 13;
	const DWORD code = start(L"s", GetCurrentProcessId(), (ULONG_PTR)event);
	if (!code || code % 4)
		return code ? (int)code : 15;
	if (WaitForSingleObject(event, 0) != WAIT_OBJECT_0)
		return 17;
	if (WaitForSingleObject((HANDLE)(ULONG_PTR)code, 0) != WAIT_OBJECT_0)
		return 19;
	return 0;
}
