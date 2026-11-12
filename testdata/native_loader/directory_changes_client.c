// A program watches a folder through an overlapped ReadDirectoryChangesW and hears of a new file and a rename through its completion
// port, then closes the folder and hears the waiting call end as aborted, as the .NET FileSystemWatcher MusicBee keeps on its music
// folder does. The exit code names the first check that failed.
#include <windows.h>

static DWORD records[1024];

static int same(const WCHAR *name, DWORD bytes, const WCHAR *expected)
{
	DWORD length = 0;
	while (expected[length])
		++length;
	if (bytes != length * 2)
		return 0;
	for (DWORD index = 0; index < length; ++index)
		if (name[index] != expected[index])
			return 0;
	return 1;
}

// Waits for the call's completion and looks for a record with the action and the name; a second action and name, when given, must be
// the record right after it.
static int heard(HANDLE port, OVERLAPPED *overlapped, DWORD action, const WCHAR *name, DWORD next_action, const WCHAR *next_name)
{
	DWORD bytes = 0;
	ULONG_PTR key = 0;
	OVERLAPPED *done = NULL;
	if (!GetQueuedCompletionStatus(port, &bytes, &key, &done, 5000) || key != 7 || done != overlapped || !bytes)
		return 0;
	const BYTE *at = (const BYTE *)records;
	for (;;) {
		const FILE_NOTIFY_INFORMATION *record = (const FILE_NOTIFY_INFORMATION *)at;
		if (record->Action == action && same(record->FileName, record->FileNameLength, name)) {
			if (!next_name)
				return 1;
			const FILE_NOTIFY_INFORMATION *after = (const FILE_NOTIFY_INFORMATION *)(at + record->NextEntryOffset);
			return record->NextEntryOffset && after->Action == next_action && same(after->FileName, after->FileNameLength, next_name);
		}
		if (!record->NextEntryOffset)
			return 0;
		at += record->NextEntryOffset;
	}
}

int raven_entry(void)
{
	WCHAR folder[MAX_PATH], file[MAX_PATH], renamed[MAX_PATH];
	if (!GetTempPathW(MAX_PATH - 40, folder))
		return 1;
	lstrcatW(folder, L"raven-directory-changes");
	CreateDirectoryW(folder, NULL);
	lstrcpyW(file, folder);
	lstrcatW(file, L"\\a.txt");
	lstrcpyW(renamed, folder);
	lstrcatW(renamed, L"\\b.txt");
	DeleteFileW(file);
	DeleteFileW(renamed);
	HANDLE directory = CreateFileW(folder, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	if (directory == INVALID_HANDLE_VALUE)
		return 2;
	HANDLE port = CreateIoCompletionPort(directory, NULL, 7, 0);
	if (!port)
		return 3;
	const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE;
	OVERLAPPED overlapped = {0};
	if (!ReadDirectoryChangesW(directory, records, sizeof records, FALSE, filter, NULL, &overlapped, NULL))
		return 4;
	HANDLE created = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	DWORD written = 0;
	if (created == INVALID_HANDLE_VALUE || !WriteFile(created, "song", 4, &written, NULL) || !CloseHandle(created))
		return 5;
	if (!heard(port, &overlapped, FILE_ACTION_ADDED, L"a.txt", 0, NULL))
		return 6;
	// Changes made between two calls wait for the second one.
	if (!MoveFileW(file, renamed))
		return 7;
	OVERLAPPED again = {0};
	if (!ReadDirectoryChangesW(directory, records, sizeof records, FALSE, filter, NULL, &again, NULL))
		return 8;
	if (!heard(port, &again, FILE_ACTION_RENAMED_OLD_NAME, L"a.txt", FILE_ACTION_RENAMED_NEW_NAME, L"b.txt"))
		return 9;
	OVERLAPPED last = {0};
	if (!ReadDirectoryChangesW(directory, records, sizeof records, FALSE, filter, NULL, &last, NULL) || !CloseHandle(directory))
		return 10;
	DWORD bytes = 1;
	ULONG_PTR key = 0;
	OVERLAPPED *done = NULL;
	if (GetQueuedCompletionStatus(port, &bytes, &key, &done, 5000) || GetLastError() != ERROR_OPERATION_ABORTED || done != &last || bytes)
		return 11;
	DeleteFileW(renamed);
	RemoveDirectoryW(folder);
	CloseHandle(port);
	return 0;
}
