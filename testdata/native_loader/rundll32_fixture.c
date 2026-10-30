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

/* What regsvr32 calls: registering succeeds, and the installer entry fails so its exit code can be told apart. */
__declspec(dllexport) HRESULT __stdcall DllRegisterServer(void) { return S_OK; }
__declspec(dllexport) HRESULT __stdcall DllInstall(BOOL install, const WCHAR *line) { (void)install; (void)line; return E_FAIL; }
