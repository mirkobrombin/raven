#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int raven_entry(void)
{
	HMODULE threads = LoadLibraryW(L"api-ms-win-core-processthreads-l1-1-0.dll");
	HMODULE loader = LoadLibraryW(L"api-ms-win-core-libraryloader-l1-2-0.dll");
	if (!threads || !loader)
		return 1;

	BOOL (WINAPI *open_process_token)(HANDLE, DWORD, HANDLE *) = (void *)GetProcAddress(threads, "OpenProcessToken");
	BOOL (WINAPI *open_thread_token)(HANDLE, DWORD, BOOL, HANDLE *) = (void *)GetProcAddress(threads, "OpenThreadToken");
	int (WINAPI *load_ansi)(HINSTANCE, UINT, char *, int) = (void *)GetProcAddress(loader, "LoadStringA");
	int (WINAPI *load_wide)(HINSTANCE, UINT, WCHAR *, int) = (void *)GetProcAddress(loader, "LoadStringW");
	if (!open_process_token || !open_thread_token || !load_ansi || !load_wide)
		return 16 + !open_process_token + 2 * !open_thread_token + 4 * !load_ansi + 8 * !load_wide;

	HANDLE token = NULL;
	if (!open_process_token(GetCurrentProcess(), TOKEN_QUERY, &token) || !token || !CloseHandle(token))
		return 2;
	token = NULL;
	if (open_thread_token(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) || GetLastError() != ERROR_NO_TOKEN || token)
		return 3;

	HINSTANCE instance = GetModuleHandleW(NULL);
	char text[8];
	WCHAR wide[8];
	if (load_ansi(instance, 2, text, ARRAYSIZE(text)) != 4 || lstrcmpA(text, "tail"))
		return 4;
	if (load_wide(instance, 2, wide, ARRAYSIZE(wide)) != 4 || lstrcmpW(wide, L"tail"))
		return 5;
	if (load_ansi(instance, 1234, text, ARRAYSIZE(text)) || load_wide(instance, 1234, wide, ARRAYSIZE(wide)))
		return 6;

	HMODULE base = LoadLibraryW(L"kernelbase.dll");
	HMODULE sync = LoadLibraryW(L"api-ms-win-core-synch-l1-2-0.dll");
	if (!base || base == GetModuleHandleW(L"kernel32.dll") || sync != base)
		return 7;
	BOOL (WINAPI *wait_on_address)(volatile void *, void *, SIZE_T, DWORD) =
		(void *)GetProcAddress(sync, "WaitOnAddress");
	if (!wait_on_address)
		return 8;
	volatile LONG address = 1;
	LONG compare = 0;
	if (!wait_on_address(&address, &compare, sizeof address, 0))
		return 9;
	compare = 1;
	if (wait_on_address(&address, &compare, sizeof address, 0) || GetLastError() != ERROR_TIMEOUT)
		return 10;
	return 0;
}
