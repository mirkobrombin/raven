#include <windows.h>
#include <dbghelp.h>

// dbghelp's symbol handler as far as walking a stack needs it (Dart starts one for its crash reports): the options are kept, the
// handler starts and ends, and a code address gives its function table entry and the base of its module. Returns 0, or the number of
// the check that failed.
int raven_entry(void)
{
	if (SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS) != (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS) ||
		SymGetOptions() != (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS))
		return 1;
	HANDLE process = GetCurrentProcess();
	if (!SymInitialize(process, NULL, TRUE))
		return 2;
	const DWORD64 code = (DWORD64)(ULONG_PTR)raven_entry;
	const RUNTIME_FUNCTION *function = SymFunctionTableAccess64(process, code);
	const DWORD64 base = SymGetModuleBase64(process, code);
	if (base != (DWORD64)(ULONG_PTR)GetModuleHandleW(NULL))
		return 3;
	if (!function || base + function->BeginAddress > code || base + function->EndAddress <= code)
		return 4;
	if (!SymCleanup(process))
		return 5;
	return 0;
}
