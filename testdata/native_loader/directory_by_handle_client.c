#include <windows.h>

// A directory read through GetFileInformationByHandleEx: FileIdBothDirectoryInfo lists every entry once, with the file's
// identifier, then fails with ERROR_NO_MORE_FILES; FileFullDirectoryRestartInfo starts again from the first entry. Returns 0, or
// the number of the check that failed.
static int same(const WCHAR *a, ULONG bytes, const WCHAR *b)
{
	ULONG at = 0;
	for (; at < bytes / 2 && b[at]; ++at)
		if (a[at] != b[at])
			return 0;
	return at == bytes / 2 && !b[at];
}

int raven_entry(void)
{
	WCHAR folder[MAX_PATH + 32];
	DWORD length = GetTempPathW(MAX_PATH, folder);
	if (!length)
		return 1;
	const WCHAR tail[] = L"raven-directory-by-handle";
	for (unsigned index = 0; index < sizeof tail / sizeof *tail; ++index)
		folder[length + index] = tail[index];
	CreateDirectoryW(folder, NULL);
	WCHAR file[MAX_PATH + 64];
	unsigned at = 0;
	for (; folder[at]; ++at)
		file[at] = folder[at];
	const WCHAR name[] = L"\\entry.txt";
	for (unsigned index = 0; index < sizeof name / sizeof *name; ++index)
		file[at + index] = name[index];
	HANDLE created = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (created == INVALID_HANDLE_VALUE)
		return 2;
	CloseHandle(created);
	HANDLE directory = CreateFileW(folder, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (directory == INVALID_HANDLE_VALUE)
		return 3;
	static __attribute__((aligned(8))) BYTE buffer[4096];
	int found = 0, dots = 0;
	for (;;) {
		if (!GetFileInformationByHandleEx(directory, FileIdBothDirectoryInfo, buffer, sizeof buffer)) {
			if (GetLastError() != ERROR_NO_MORE_FILES)
				return 4;
			break;
		}
		for (BYTE *record = buffer;;) {
			const FILE_ID_BOTH_DIR_INFO *entry = (const FILE_ID_BOTH_DIR_INFO *)record;
			if (same(entry->FileName, entry->FileNameLength, L"entry.txt")) {
				++found;
				if (!entry->FileId.QuadPart)
					return 5;
			} else if (same(entry->FileName, entry->FileNameLength, L".") || same(entry->FileName, entry->FileNameLength, L"..")) {
				++dots;
			}
			if (!entry->NextEntryOffset)
				break;
			record += entry->NextEntryOffset;
		}
	}
	if (found != 1 || dots != 2)
		return 6;
	if (!GetFileInformationByHandleEx(directory, FileFullDirectoryRestartInfo, buffer, sizeof buffer))
		return 7;
	CloseHandle(directory);
	DeleteFileW(file);
	RemoveDirectoryW(folder);
	return 0;
}
