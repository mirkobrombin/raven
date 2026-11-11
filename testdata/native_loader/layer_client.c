#include <windows.h>

/* Programs of one prefix that see each other without changing each other. One program plays every part, chosen by its first option:
   --seed     leaves the files and the value every program shares;
   --install  installs (run as setup-layer.exe): itself as C:\Program Files\LayerApp\layer-app.exe, a settings file and a key beside it,
              and a copy of itself as C:\Windows\layer-check.exe, which belongs to the system;
   --use      runs as the installed program: changes its own things and the shared ones, checks that it sees its changes, and has
              layer-check.exe, which another identity starts, check that the shared things are unchanged;
   --base     checks the shared things as every other program sees them;
   --fresh    checks, as the installed program again, that it sees the shared things unchanged once its layer is gone. */

static BOOL write_text(const WCHAR *path, const char *text)
{
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	DWORD length = 0, written = 0;
	while (text[length])
		++length;
	const BOOL done = WriteFile(file, text, length, &written, NULL) && written == length;
	CloseHandle(file);
	return done;
}

static BOOL holds(const WCHAR *path, const char *text)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	char buffer[64];
	DWORD read = 0;
	const BOOL done = ReadFile(file, buffer, sizeof buffer, &read, NULL);
	CloseHandle(file);
	DWORD index = 0;
	for (; text[index]; ++index)
		if (index >= read || buffer[index] != text[index])
			return FALSE;
	return done && index == read;
}

static BOOL present(const WCHAR *path)
{
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL set_level(const WCHAR *key_name, const WCHAR *name, DWORD level)
{
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, key_name, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
		return FALSE;
	const LONG written = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&level, sizeof level);
	RegCloseKey(key);
	return !written;
}

static DWORD level_of(const WCHAR *key_name, const WCHAR *name)
{
	DWORD level = 0, size = sizeof level;
	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, key_name, 0, KEY_READ, &key))
		return 0;
	if (RegQueryValueExW(key, name, NULL, NULL, (BYTE *)&level, &size))
		level = 0;
	RegCloseKey(key);
	return level;
}

/* Which of the names starting with layer- a listing of C:\ shows: 1 for layer-shared.txt, 2 for layer-other.txt, 4 for layer-new.txt. */
static int listed(void)
{
	WIN32_FIND_DATAW found;
	HANDLE search = FindFirstFileW(L"C:\\layer-*", &found);
	if (search == INVALID_HANDLE_VALUE)
		return 0;
	int seen = 0;
	do {
		const WCHAR *name = found.cFileName;
		if (name[6] == L's')
			seen |= 1;
		else if (name[6] == L'o')
			seen |= 2;
		else if (name[6] == L'n')
			seen |= 4;
	} while (FindNextFileW(search, &found));
	FindClose(search);
	return seen;
}

static BOOL option(const WCHAR *line, WCHAR letter)
{
	for (; *line; ++line)
		if (line[0] == L'-' && line[1] == L'-' && line[2] == letter)
			return TRUE;
	return FALSE;
}

/* The shared things as every program but the installed one sees them, after the installed one ran. */
static int check_base(void)
{
	if (!holds(L"C:\\layer-shared.txt", "base"))
		return 20;
	if (!present(L"C:\\layer-other.txt"))
		return 21;
	if (present(L"C:\\layer-new.txt"))
		return 22;
	if (level_of(L"Software\\RavenShared", L"Level") != 1)
		return 23;
	if (listed() != 3)
		return 24;
	/* What the installed program owns it changed in place, for everyone. */
	if (!holds(L"C:\\Program Files\\LayerApp\\settings.txt", "used"))
		return 25;
	if (level_of(L"Software\\LayerApp", L"Version") != 2)
		return 26;
	return 0;
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	if (option(line, L's')) {
		if (!write_text(L"C:\\layer-shared.txt", "base") || !write_text(L"C:\\layer-other.txt", "other"))
			return 1;
		return set_level(L"Software\\RavenShared", L"Level", 1) ? 0 : 2;
	}
	if (option(line, L'i')) {
		WCHAR self[MAX_PATH];
		if (!GetModuleFileNameW(NULL, self, MAX_PATH))
			return 3;
		CreateDirectoryW(L"C:\\Program Files\\LayerApp", NULL);
		if (!CopyFileW(self, L"C:\\Program Files\\LayerApp\\layer-app.exe", FALSE) || !CopyFileW(self, L"C:\\Windows\\layer-check.exe", FALSE))
			return 4;
		if (!write_text(L"C:\\Program Files\\LayerApp\\settings.txt", "installed"))
			return 5;
		return set_level(L"Software\\LayerApp", L"Version", 1) ? 0 : 6;
	}
	if (option(line, L'u')) {
		if (!write_text(L"C:\\Program Files\\LayerApp\\settings.txt", "used"))
			return 7;
		if (!write_text(L"C:\\layer-shared.txt", "app"))
			return 8;
		if (!DeleteFileW(L"C:\\layer-other.txt"))
			return 9;
		if (!write_text(L"C:\\layer-new.txt", "new"))
			return 10;
		if (!set_level(L"Software\\RavenShared", L"Level", 2) || !set_level(L"Software\\LayerApp", L"Version", 2))
			return 11;
		/* It sees what it changed. */
		if (!holds(L"C:\\layer-shared.txt", "app"))
			return 12;
		if (present(L"C:\\layer-other.txt") || !holds(L"C:\\layer-new.txt", "new"))
			return 13;
		if (level_of(L"Software\\RavenShared", L"Level") != 2)
			return 14;
		if (listed() != 5)
			return 15;
		/* A program it starts takes the identity of its own executable, which belongs to the system. */
		WCHAR command[] = L"C:\\Windows\\layer-check.exe --base";
		STARTUPINFOW startup = {sizeof startup};
		PROCESS_INFORMATION process;
		if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
			return 16;
		WaitForSingleObject(process.hProcess, 120000);
		DWORD code = 1;
		GetExitCodeProcess(process.hProcess, &code);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return code ? (int)code : 0;
	}
	if (option(line, L'b'))
		return check_base();
	if (option(line, L'f')) {
		if (!holds(L"C:\\layer-shared.txt", "base") || !present(L"C:\\layer-other.txt") || present(L"C:\\layer-new.txt"))
			return 30;
		if (level_of(L"Software\\RavenShared", L"Level") != 1)
			return 31;
		return listed() == 3 ? 0 : 32;
	}
	return 40;
}
