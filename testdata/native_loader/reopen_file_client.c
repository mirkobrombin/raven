// A file opened again through a handle to it gives a second handle to the same file with access of its own, as the paint.net setup
// reopens the package it unpacked. The exit code names the first check that failed.
#include <windows.h>

int raven_entry(void)
{
	WCHAR folder[MAX_PATH], path[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"rof", 0, path))
		return 1;
	HANDLE writer = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (writer == INVALID_HANDLE_VALUE)
		return 2;
	DWORD done = 0;
	if (!WriteFile(writer, "raven", 5, &done, NULL) || done != 5)
		return 3;
	HANDLE reader = ReOpenFile(writer, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
	if (reader == INVALID_HANDLE_VALUE || reader == writer)
		return 4;
	char text[8] = {0};
	if (!ReadFile(reader, text, sizeof text, &done, NULL) || done != 5 || text[0] != 'r' || text[4] != 'n')
		return 5;
	// The new handle has only the access it asked for.
	if (WriteFile(reader, "x", 1, &done, NULL))
		return 6;
	if (ReOpenFile(NULL, GENERIC_READ, FILE_SHARE_READ, 0) != INVALID_HANDLE_VALUE || GetLastError() != ERROR_INVALID_HANDLE)
		return 7;
	CloseHandle(reader);
	CloseHandle(writer);
	return DeleteFileW(path) ? 0 : 8;
}
