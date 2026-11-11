#include <windows.h>
#include <commdlg.h>

/* A host file the person picks in the file dialog. Without arguments the program asks to open a file: it may then read the file but not
   change it. With --save it asks where to save: it may then write the file. The exit code says which part went wrong. */

static BOOL option(const WCHAR *line, const WCHAR *name)
{
	for (; *line; ++line) {
		unsigned index = 0;
		while (name[index] && line[index] == name[index])
			++index;
		if (!name[index])
			return TRUE;
	}
	return FALSE;
}

int raven_entry(void)
{
	const BOOL save = option(GetCommandLineW(), L"--save");
	WCHAR path[MAX_PATH] = {0};
	OPENFILENAMEW request = {sizeof request};
	request.lpstrFile = path;
	request.nMaxFile = MAX_PATH;
	request.lpstrFilter = L"Text\0*.txt\0All files\0*.*\0";
	request.Flags = OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
	if (!(save ? GetSaveFileNameW(&request) : GetOpenFileNameW(&request)))
		return 10;
	if (path[0] == L'C' || path[0] == L'c')
		return 11;
	if (save) {
		HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			return 20;
		DWORD written = 0;
		const BOOL done = WriteFile(file, "saved", 5, &written, NULL) && written == 5;
		CloseHandle(file);
		return done ? 0 : 21;
	}
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 30;
	char buffer[16];
	DWORD read = 0;
	const BOOL done = ReadFile(file, buffer, sizeof buffer, &read, NULL) && read == 6 && buffer[0] == 'p';
	CloseHandle(file);
	if (!done)
		return 31;
	/* Opening a file grants reading it, not changing it. */
	file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
		return 32;
	}
	return GetLastError() == ERROR_ACCESS_DENIED ? 0 : 33;
}
