/* WritePrivateProfileString keeps a profile's encoding: a Unicode file (UTF-16 after a byte order mark, as the NSIS InstallOptions plugin
   makes its page files) gets Unicode lines that read back, and an ANSI file gets ANSI ones. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

static const char unicode_start[] = "\xff\xfe[\0S\0e\0t\0t\0i\0n\0g\0s\0]\0\r\0\n\0N\0u\0m\0F\0i\0e\0l\0d\0s\0=\0003\0\r\0\n\0"
	"[\0F\0i\0e\0l\0d\0 \0001\0]\0\r\0\n\0T\0y\0p\0e\0=\0B\0i\0t\0m\0a\0p\0\r\0\n\0";
static const char ansi_start[] = "[Settings]\r\nNumFields=3\r\n";
static const char ansi_end[] = "[Settings]\r\nNumFields=3\r\nRect=1044\r\n\r\n[Field 1]\r\nType=Label\r\n";

static int put_file(const WCHAR *name, const char *bytes, DWORD size)
{
	HANDLE file = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD written = 0;
	BOOL ok;
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	ok = WriteFile(file, bytes, size, &written, NULL) && written == size;
	CloseHandle(file);
	return ok;
}

static DWORD get_file(const WCHAR *name, char *bytes, DWORD capacity)
{
	HANDLE file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD read = 0;
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	if (!ReadFile(file, bytes, capacity, &read, NULL))
		read = 0;
	CloseHandle(file);
	return read;
}

static int same_bytes(const char *left, const char *right, DWORD size)
{
	for (DWORD index = 0; index < size; index++)
		if (left[index] != right[index])
			return 0;
	return 1;
}

static int has_value(const WCHAR *section, const WCHAR *key, const WCHAR *expected, const WCHAR *file)
{
	WCHAR value[128];
	DWORD length = GetPrivateProfileStringW(section, key, L"(none)", value, 128, file);
	return length == (DWORD)lstrlenW(expected) && !lstrcmpW(value, expected);
}

/* Every line of the Unicode file is UTF-16 after its byte order mark: its characters are ASCII or the one arrow it was given, where a
   line written in bytes would pair two letters into one character. */
static int all_wide(const WCHAR *name)
{
	WCHAR characters[2048];
	DWORD size = get_file(name, (char *)characters, sizeof characters);
	if (size < 4 || size % 2 || characters[0] != 0xfeff)
		return 0;
	for (DWORD index = 1; index < size / 2; index++)
		if (characters[index] >= 0x80 && characters[index] != 0x2192)
			return 0;
	return 1;
}

int raven_entry(void)
{
	WCHAR directory[MAX_PATH], unicode[MAX_PATH], ansi[MAX_PATH], missing[MAX_PATH];
	char bytes[512], path[MAX_PATH];
	DWORD length = GetTempPathW(MAX_PATH - 32, directory);
	if (!length)
		return 1;
	lstrcpyW(unicode, directory);
	lstrcatW(unicode, L"raven-profile-unicode.ini");
	lstrcpyW(ansi, directory);
	lstrcatW(ansi, L"raven-profile-ansi.ini");
	lstrcpyW(missing, directory);
	lstrcatW(missing, L"raven-no-such-directory\\profile.ini");

	/* A Unicode file: replaced, added and removed values, a new section and text beyond Latin-1 all read back, and it stays UTF-16. */
	if (!put_file(unicode, unicode_start, sizeof unicode_start - 1))
		return 2;
	if (!WritePrivateProfileStringW(L"Settings", L"Rect", L"1044", unicode))
		return 3;
	if (!WritePrivateProfileStringW(L"Field 1", L"Type", L"Label", unicode))
		return 4;
	if (!WritePrivateProfileStringW(L"Field 2", L"Text", L"Next \x2192 Install", unicode))
		return 5;
	/* The ANSI call writes to a Unicode file in Unicode too. */
	if (!WideCharToMultiByte(CP_ACP, 0, unicode, -1, path, MAX_PATH, NULL, NULL) || !WritePrivateProfileStringA("Field 2", "Type", "Bitmap", path))
		return 6;
	if (!WritePrivateProfileStringW(L"Settings", L"Unused", L"1", unicode) || !WritePrivateProfileStringW(L"Settings", L"Unused", NULL, unicode))
		return 7;
	if (GetPrivateProfileIntW(L"Settings", L"NumFields", 0, unicode) != 3)
		return 8;
	if (GetPrivateProfileIntW(L"Settings", L"Rect", 1018, unicode) != 1044)
		return 9;
	if (GetPrivateProfileIntW(L"Settings", L"Unused", 7, unicode) != 7)
		return 10;
	if (!has_value(L"Field 1", L"Type", L"Label", unicode))
		return 11;
	if (!has_value(L"Field 2", L"Type", L"Bitmap", unicode))
		return 12;
	if (!has_value(L"Field 2", L"Text", L"Next \x2192 Install", unicode))
		return 13;
	if (!all_wide(unicode))
		return 14;
	if (!WritePrivateProfileStringW(L"Field 2", NULL, NULL, unicode) || !has_value(L"Field 2", L"Type", L"(none)", unicode) ||
		!has_value(L"Field 1", L"Type", L"Label", unicode) || !all_wide(unicode))
		return 15;

	/* An ANSI file stays ANSI, line for line. */
	if (!put_file(ansi, ansi_start, sizeof ansi_start - 1))
		return 20;
	if (!WritePrivateProfileStringW(L"Settings", L"Rect", L"1044", ansi) || !WritePrivateProfileStringW(L"Field 1", L"Type", L"Label", ansi))
		return 21;
	length = get_file(ansi, bytes, sizeof bytes);
	if (length != sizeof ansi_end - 1 || !same_bytes(bytes, ansi_end, length))
		return 22;
	if (GetPrivateProfileIntW(L"Settings", L"Rect", 1018, ansi) != 1044 || !has_value(L"Field 1", L"Type", L"Label", ansi))
		return 23;

	/* A file in a directory that does not exist is not written. */
	SetLastError(0);
	if (WritePrivateProfileStringW(L"Settings", L"Rect", L"1044", missing) || GetLastError() != ERROR_PATH_NOT_FOUND)
		return 30;

	DeleteFileW(unicode);
	DeleteFileW(ansi);
	return 0;
}
