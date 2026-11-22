#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef DWORD (WINAPI *display_uniqueness)(void);

int raven_entry(void)
{
	HMODULE contract = LoadLibraryW(L"api-ms-win-dx-d3dkmt-l1-1-0.dll");
	HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
	if (!contract || !gdi)
		return 1;
	display_uniqueness query = (display_uniqueness)GetProcAddress(contract, "GdiEntry13");
	display_uniqueness direct = (display_uniqueness)GetProcAddress(gdi, "GdiEntry13");
	if (!query || query != direct || query() != direct())
		return 2;
	return 0;
}
