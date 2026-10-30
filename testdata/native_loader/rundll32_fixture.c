#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// An entry reports the first character of the command line it was given as the exit status of its process.
__declspec(dllexport) void CALLBACK Report(HWND window,HINSTANCE instance,LPSTR line,int show)
{
	(void)window;(void)instance;(void)show;
	ExitProcess((UINT)(line[0]-'0'+40));
}

__declspec(dllexport) void CALLBACK ReportedW(HWND window,HINSTANCE instance,LPWSTR line,int show)
{
	(void)window;(void)instance;(void)show;
	ExitProcess((UINT)(line[0]-L'0'+50));
}

BOOL WINAPI raven_library_entry(HINSTANCE instance,DWORD reason,LPVOID reserved)
{
	(void)instance;(void)reason;(void)reserved;
	return TRUE;
}
