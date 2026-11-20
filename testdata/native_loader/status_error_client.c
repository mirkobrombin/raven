#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// RtlNtStatusToDosError for statuses libuv turns into the errors Node.js reports, and for one that has no error.

typedef ULONG (WINAPI *StatusToError)(LONG);

int raven_entry(void)
{
	static const struct { ULONG status, error; } cases[] = {
		{0xc0000101, ERROR_DIR_NOT_EMPTY}, {0xc0000043, ERROR_SHARING_VIOLATION}, {0xc0000035, ERROR_ALREADY_EXISTS},
		{0xc00000d4, ERROR_NOT_SAME_DEVICE}, {0xc000007f, ERROR_DISK_FULL}, {0xc0000121, ERROR_ACCESS_DENIED},
		{0xc0000001, ERROR_GEN_FAILURE}, {0xc0000034, ERROR_FILE_NOT_FOUND}, {0, ERROR_SUCCESS}, {0xc0ab0001, ERROR_MR_MID_NOT_FOUND}};
	StatusToError convert = (StatusToError)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError");
	if (!convert) return 100;
	for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i)
		if (convert((LONG)cases[i].status) != cases[i].error) return 1 + i;
	return 0;
}
