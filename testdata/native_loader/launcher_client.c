#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* A launcher that imports only kernel32 and leaves the window work to a library beside it, loaded by name: the library's controls must
   be there all the same. Built for both widths. */

typedef HWND(WINAPI *make_window)(void);

int raven_entry(void)
{
	HMODULE work = LoadLibraryW(L"launcher_work.dll");
	if (!work)
		return 1;
	const make_window make = (make_window)(void *)GetProcAddress(work, "make_window");
	if (!make)
		return 2;
	if (!make())
		return 3;
	return 0;
}
