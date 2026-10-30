#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef LONG (WINAPI *current_function)(UINT32 *, WCHAR *);
typedef LONG (WINAPI *process_function)(HANDLE, UINT32 *, WCHAR *);

/* A program in the prefix has no package identity, and neither has any process it opens: the current process's package names and
 * application user model ID, and the same asked of a process by its handle, all answer APPMODEL_ERROR_NO_PACKAGE with a length of
 * zero, as winget asks before it decides where its data lives. */
int raven_entry(void)
{
	static const char *current_names[] = {"GetCurrentPackageFullName", "GetCurrentPackageFamilyName", "GetCurrentApplicationUserModelId"};
	static const char *process_names[] = {"GetPackageFullName", "GetPackageFamilyName", "GetApplicationUserModelId"};
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	if (!kernel) return 20;
	HANDLE self = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
	if (!self) return 21;
	for (int index = 0; index < 3; ++index) {
		current_function current = (current_function)GetProcAddress(kernel, current_names[index]);
		process_function process = (process_function)GetProcAddress(kernel, process_names[index]);
		if (!current || !process) return 1 + index * 10;
		WCHAR name[128];
		UINT32 length = ARRAYSIZE(name);
		if (current(&length, name) != APPMODEL_ERROR_NO_PACKAGE || length) return 2 + index * 10;
		length = ARRAYSIZE(name);
		if (process(GetCurrentProcess(), &length, name) != APPMODEL_ERROR_NO_PACKAGE || length) return 3 + index * 10;
		length = ARRAYSIZE(name);
		if (process(self, &length, name) != APPMODEL_ERROR_NO_PACKAGE || length) return 4 + index * 10;
		length = 0;
		if (process(self, &length, NULL) != APPMODEL_ERROR_NO_PACKAGE || length) return 5 + index * 10;
	}
	CloseHandle(self);
	return 0;
}
