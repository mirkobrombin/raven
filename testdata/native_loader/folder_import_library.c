#include <windows.h>

// A library that lives in a folder below the program's, imported by a path relative to it, for folder_import_client.c.
BOOL WINAPI raven_library_entry(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	return TRUE;
}

__declspec(dllexport) int folder_value(void)
{
	return 42;
}
