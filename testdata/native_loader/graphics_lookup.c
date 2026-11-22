#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int raven_entry(void)
{
	if (LoadLibraryW(L"C:\\raven-test-missing\\d3d11.dll"))
		return 1;
	HMODULE module = LoadLibraryW(L"d3d11.dll");
	if (!module)
		return 2;
	int (*answer)(void) = (void *)GetProcAddress(module, "plugin_answer");
	if (!answer || answer() != 42)
		return 3;
	return FreeLibrary(module) ? 0 : 4;
}
