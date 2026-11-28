#include <windows.h>
#include <shlwapi.h>

/* The ANSI forms of the shell's path and string functions answer as the wide ones do: files and folders are found, paths are built and
 * changed in the program's own buffers, and a pointer they answer points into the program's own text. The exit code names the first check
 * that failed. */
int raven_entry(void)
{
	char windows[MAX_PATH], path[MAX_PATH], other[MAX_PATH];
	const char *text = "C:\\Games\\Need for Speed\\Core\\Activation64.dll";
	if (!GetWindowsDirectoryA(windows, MAX_PATH))
		return 1;
	if (!PathFileExistsA(windows) || PathFileExistsA("C:\\raven-missing\\nothing.txt") || PathFileExistsA(NULL))
		return 2;
	if (!PathIsDirectoryA(windows) || PathIsDirectoryEmptyA(windows))
		return 3;
	if (PathIsRelativeA(windows) || !PathIsRelativeA("Core\\Activation64.dll") || !PathIsRootA("C:\\") || PathIsRootA(windows))
		return 4;
	if (!PathIsUNCA("\\\\server\\share") || PathGetDriveNumberA(windows) != 2 || !PathIsFileSpecA("file.txt") || PathIsFileSpecA(text))
		return 5;
	if (!PathMatchSpecA("Activation64.dll", "*.dll") || PathMatchSpecA("Activation64.dll", "*.exe"))
		return 6;
	if (!PathIsPrefixA("C:\\Games", text) || PathIsPrefixA("D:\\Games", text))
		return 7;
	// Buffers changed in place, and the pointers into them.
	lstrcpyA(path, "C:\\Games");
	char *end = PathAddBackslashA(path);
	if (end != path + 9 || lstrcmpA(path, "C:\\Games\\"))
		return 8;
	if (!PathAppendA(path, "Core") || lstrcmpA(path, "C:\\Games\\Core") || !PathAddExtensionA(path, ".dll") || lstrcmpA(path, "C:\\Games\\Core.dll"))
		return 9;
	PathRemoveExtensionA(path);
	if (lstrcmpA(path, "C:\\Games\\Core") || !PathRemoveFileSpecA(path) || lstrcmpA(path, "C:\\Games"))
		return 10;
	lstrcpyA(path, "C:\\Games\\");
	if (PathRemoveBackslashA(path) != path + 8 || lstrcmpA(path, "C:\\Games"))
		return 11;
	lstrcpyA(path, text);
	PathStripPathA(path);
	if (lstrcmpA(path, "Activation64.dll"))
		return 12;
	lstrcpyA(path, "C:\\Program Files\\Game.exe");
	PathQuoteSpacesA(path);
	if (lstrcmpA(path, "\"C:\\Program Files\\Game.exe\""))
		return 13;
	PathUnquoteSpacesA(path);
	if (lstrcmpA(path, "C:\\Program Files\\Game.exe"))
		return 14;
	// Paths built into buffers.
	if (PathCombineA(path, "C:\\Games\\Need for Speed", "Core\\..\\Core\\Activation64.dll") != path ||
		lstrcmpA(path, "C:\\Games\\Need for Speed\\Core\\Activation64.dll"))
		return 15;
	if (!PathCanonicalizeA(other, "C:\\Games\\.\\x\\..\\Core") || lstrcmpA(other, "C:\\Games\\Core"))
		return 16;
	if (PathCommonPrefixA(text, "C:\\Games\\Other", other) != 8 || lstrcmpA(other, "C:\\Games"))
		return 17;
	if (!PathRelativePathToA(other, "C:\\Games\\Need for Speed", FILE_ATTRIBUTE_DIRECTORY, text, 0) ||
		lstrcmpA(other, ".\\Core\\Activation64.dll"))
		return 18;
	if (PathSkipRootA(text) != text + 3)
		return 19;
	// Strings: pointers into the program's text, numbers, comparisons, trimming.
	if (StrChrA(text, 'A') != text + 29 || StrStrA(text, "Core") != text + 24 || StrStrIA(text, "core") != text + 24)
		return 20;
	if (StrPBrkA(text, "\\") != text + 2 || StrChrA(text, 'z'))
		return 21;
	if (StrCmpNA("Activation", "Active", 5) || !StrCmpNA("Activation", "Active", 6) || StrCmpNIA("ACTIVE", "active", 6))
		return 22;
	int value = 0;
	if (StrToIntA("1234") != 1234 || !StrToIntExA("0x1f", STIF_SUPPORT_HEX, &value) || value != 31)
		return 23;
	lstrcpyA(path, "  padded  ");
	if (!StrTrimA(path, " ") || lstrcmpA(path, "padded"))
		return 24;
	return 0;
}
