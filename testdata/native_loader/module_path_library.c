#include <windows.h>

// A library with nothing in it but its entry point, for module_handle_path_client.c.
BOOL WINAPI raven_library_entry(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
