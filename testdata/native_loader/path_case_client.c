#include <windows.h>

// Names are found whatever their case, at once after the folder changes: a file made, renamed or deleted under one spelling is seen
// so under another by the next call, as the runtime keeps the folders' listings it has read. Returns 0, or the number of the check.
#define CHECK(number, condition) do { if (!(condition)) return number; } while (0)

static WCHAR folder[MAX_PATH];

static const WCHAR *in(const WCHAR *name)
{
	static WCHAR path[MAX_PATH];
	lstrcpyW(path, folder);
	lstrcatW(path, name);
	return path;
}

static BOOL there(const WCHAR *name)
{
	return GetFileAttributesW(in(name)) != INVALID_FILE_ATTRIBUTES;
}

int raven_entry(void)
{
	CHECK(1, GetTempPathW(MAX_PATH, folder) && lstrcatW(folder, L"raven-case\\") && CreateDirectoryW(folder, NULL));
	CHECK(2, !there(L"Some.TXT") && GetLastError() == ERROR_FILE_NOT_FOUND);
	HANDLE file = CreateFileW(in(L"some.txt"), GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(3, file != INVALID_HANDLE_VALUE && CloseHandle(file));
	CHECK(4, there(L"SOME.txt") && there(L"some.txt"));
	WCHAR from[MAX_PATH];
	lstrcpyW(from, in(L"some.TXT"));
	CHECK(5, MoveFileW(from, in(L"Other.txt")));
	CHECK(6, !there(L"SOME.TXT") && there(L"other.TXT"));
	CHECK(7, DeleteFileW(in(L"OTHER.txt")) && !there(L"other.txt"));
	CHECK(8, CreateDirectoryW(in(L"Sub"), NULL) && there(L"SUB"));
	file = CreateFileW(in(L"SUB\\X.TXT"), GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(9, file != INVALID_HANDLE_VALUE && CloseHandle(file));
	CHECK(10, there(L"sub\\x.txt") && DeleteFileW(in(L"Sub\\x.TXT")) && !there(L"sub\\X.txt"));
	CHECK(11, RemoveDirectoryW(in(L"sub")) && !there(L"Sub"));
	// A folder made again where one was removed is a new folder, empty.
	CHECK(12, CreateDirectoryW(in(L"SUB"), NULL) && !there(L"sub\\x.txt") && RemoveDirectoryW(in(L"Sub")));
	for (int round = 0; round < 2000; ++round)
		CHECK(13, GetFileAttributesW(folder) & FILE_ATTRIBUTE_DIRECTORY);
	CHECK(14, RemoveDirectoryW(folder));
	return 0;
}
