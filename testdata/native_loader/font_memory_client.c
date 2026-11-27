// A font a program carries in memory, as the EA app installer adds its own: AddFontMemResourceEx counts the faces of a real font
// file (font.ttf beside the program) and hands back a handle that RemoveFontMemResourceEx releases once; bytes that are not a font
// add nothing. The exit code names the first check that failed.
#include <windows.h>

int raven_entry(void)
{
	WCHAR path[MAX_PATH + 16];
	DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	while (length && path[length - 1] != '\\')
		--length;
	if (!length)
		return 1;
	const WCHAR name[] = L"font.ttf";
	for (DWORD at = 0; at < sizeof name / sizeof *name; ++at)
		path[length + at] = name[at];
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 2;
	DWORD size = GetFileSize(file, NULL), read = 0;
	BYTE *bytes = HeapAlloc(GetProcessHeap(), 0, size);
	if (!bytes || !ReadFile(file, bytes, size, &read, NULL) || read != size)
		return 3;
	CloseHandle(file);
	DWORD faces = 0;
	HANDLE font = AddFontMemResourceEx(bytes, size, NULL, &faces);
	if (!font || faces != 1)
		return 4;
	if (!RemoveFontMemResourceEx(font))
		return 5;
	if (RemoveFontMemResourceEx(font))
		return 6;
	static BYTE junk[64] = {'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't'};
	faces = 7;
	if (AddFontMemResourceEx(junk, sizeof junk, NULL, &faces))
		return 7;
	return 0;
}
