#include <windows.h>

// A private side-by-side assembly, as Ruby ships its libraries: the program's library private_middle.dll carries a manifest that
// depends on the assembly raven.private, whose manifest and library are in the folder raven.private beside the program. The loader
// finds private_target.dll there for the library's imports. Returns 0, or the number of the check that failed.
__declspec(dllimport) int middle_value(void);

int raven_entry(void)
{
	if (middle_value() != 43)
		return 1;
	HMODULE target = GetModuleHandleW(L"private_target.dll");
	static WCHAR path[MAX_PATH];
	const DWORD length = target ? GetModuleFileNameW(target, path, MAX_PATH) : 0;
	static const WCHAR tail[] = L"\\raven.private\\private_target.dll";
	const DWORD count = sizeof tail / sizeof *tail - 1;
	if (length < count)
		return 2;
	for (DWORD index = 0; index < count; ++index)
		if ((path[length - count + index] | 0x20) != (tail[index] | 0x20))
			return 3;
	return 0;
}
