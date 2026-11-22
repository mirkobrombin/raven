#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <appmodel.h>

#define FAMILY L"Raven.MsixTest_gh7ecn69rvjyc"
#ifdef _WIN64
#define ARCH L"x64"
#else
#define ARCH L"x86"
#endif
#define FULL_NAME L"Raven.MsixTest_1.2.3.4_" ARCH L"__gh7ecn69rvjyc"

typedef LONG (WINAPI *package_name)(UINT32 *, PWSTR);
typedef LONG (WINAPI *package_id)(UINT32 *, BYTE *);

static BOOL same(package_name query, const WCHAR *expected)
{
	UINT32 length = 0;
	WCHAR text[MAX_PATH];
	if (!query || query(&length, NULL) != ERROR_INSUFFICIENT_BUFFER || length != lstrlenW(expected) + 1)
		return FALSE;
	return query(&length, text) == ERROR_SUCCESS && !lstrcmpW(text, expected);
}

int raven_entry(void)
{
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	if (!kernel)
		return 1;
	if (!same((package_name)GetProcAddress(kernel, "GetCurrentPackageFullName"), FULL_NAME))
		return 2;
	if (!same((package_name)GetProcAddress(kernel, "GetCurrentPackageFamilyName"), FAMILY))
		return 3;
	if (!same((package_name)GetProcAddress(kernel, "GetCurrentApplicationUserModelId"), FAMILY L"!Main"))
		return 4;
	if (!same((package_name)GetProcAddress(kernel, "GetCurrentPackagePath"), L"C:\\Program Files\\WindowsApps\\" FULL_NAME))
		return 5;
	package_id query = (package_id)GetProcAddress(kernel, "GetCurrentPackageId");
	union { PACKAGE_ID id; BYTE bytes[512]; } result;
	UINT32 length = 0;
	if (!query || query(&length, NULL) != ERROR_INSUFFICIENT_BUFFER || length > sizeof(result) ||
		query(&length, result.bytes) != ERROR_SUCCESS || lstrcmpW(result.id.name, L"Raven.MsixTest") ||
		lstrcmpW(result.id.publisherId, L"gh7ecn69rvjyc"))
		return 6;
	HANDLE file = CreateFileW(L"C:\\Program Files\\WindowsApps\\" FULL_NAME L"\\app.exe", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_ACCESS_DENIED)
		return 7;
	return 0;
}
