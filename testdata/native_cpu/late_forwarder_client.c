#include <windows.h>

typedef unsigned int (*add_fn)(unsigned int);

/* A library loaded at run time whose exports forward to one nobody has loaded yet: GetProcAddress loads the target as it reaches it,
 * by name and by ordinal, and the target is then in the process; a forwarder to a library that is not there finds nothing. */
unsigned int raven_entry(void)
{
	if (GetModuleHandleA("late_target.dll"))
		return 1;
	HMODULE facade = LoadLibraryA("late_facade.dll");
	if (!facade || GetModuleHandleA("late_target.dll"))
		return 2;
	add_fn named = (add_fn)GetProcAddress(facade, "Named");
	HMODULE target = GetModuleHandleA("late_target.dll");
	if (!named || !target)
		return 3;
	add_fn ordinal = (add_fn)GetProcAddress(facade, (LPCSTR)2);
	if (!ordinal || ordinal != named || named != (add_fn)GetProcAddress(target, "Add"))
		return 4;
	if (named(10) + ordinal(20) != 40)
		return 5;
	SetLastError(0);
	if (GetProcAddress(facade, "Missing") || GetLastError() != ERROR_MOD_NOT_FOUND)
		return 6;
	return 0;
}
