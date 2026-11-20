#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// The ntdll functions libuv looks up by name when it starts, and NtSetInformationFile for the classes libuv sets.

typedef struct { ULONG_PTR status, information; } IoStatus;
typedef struct { LARGE_INTEGER creation, access, write, change; ULONG attributes; } BasicInformation;
typedef LONG (WINAPI *SetInformation)(HANDLE, IoStatus *, void *, ULONG, ULONG);

#define CHECK(code, condition) do { if (!(condition)) return code; } while (0)

enum { basic_information = 4, name_information = 9, disposition_information = 13, end_of_file_information = 20, disposition_information_ex = 64 };

static void join(WCHAR *out, const WCHAR *left, const WCHAR *right)
{
	unsigned i = 0, j = 0;
	while (left[i]) { out[i] = left[i]; ++i; }
	while (right[j]) out[i++] = right[j++];
	out[i] = 0;
}

int raven_entry(void)
{
	static const char *const names[] = {"RtlGetVersion", "RtlNtStatusToDosError", "NtDeviceIoControlFile", "NtQueryInformationFile",
		"NtSetInformationFile", "NtQueryVolumeInformationFile", "NtQueryDirectoryFile", "NtQuerySystemInformation", "NtQueryInformationProcess",
		"NtCreateFile"};
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	for (unsigned i = 0; i < sizeof(names) / sizeof(*names); ++i)
		CHECK(1 + i, GetProcAddress(ntdll, names[i]));
	SetInformation set = (SetInformation)GetProcAddress(ntdll, "NtSetInformationFile");

	const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
	WCHAR temporary[260], directory[260], path[260], inner[260];
	CHECK(20, GetTempPathW(260, temporary));
	join(directory, temporary, L"raven-set-information");
	join(path, directory, L"\\file.bin");
	join(inner, directory, L"\\inner.bin");
	CreateDirectoryW(directory, 0);
	HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, share, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	CHECK(21, file != INVALID_HANDLE_VALUE);
	DWORD written = 0;
	CHECK(22, WriteFile(file, "0123456789", 10, &written, 0) && written == 10);

	// The end of the file moves.
	IoStatus io;
	LARGE_INTEGER end;
	end.QuadPart = 4;
	CHECK(23, set(file, &io, &end, sizeof(end), end_of_file_information) == 0 && io.status == 0);
	CHECK(24, GetFileSize(file, 0) == 4);
	// A record shorter than the class's, a class that cannot be set, a record that is not memory and a size below zero are each refused.
	CHECK(25, set(file, &io, &end, 4, end_of_file_information) == (LONG)0xc0000004);
	CHECK(26, set(file, &io, &end, sizeof(end), name_information) == (LONG)0xc0000003);
	CHECK(27, set(file, &io, (void *)16, sizeof(end), end_of_file_information) == (LONG)0xc0000005);
	end.QuadPart = -1;
	CHECK(28, set(file, &io, &end, sizeof(end), end_of_file_information) == (LONG)0xc000000d && GetFileSize(file, 0) == 4);
	// A handle that is none is refused, and the failure leaves the last error as it was: the result is the status.
	SetLastError(1234);
	CHECK(29, set(0, &io, &end, sizeof(end), end_of_file_information) == (LONG)0xc0000008 && GetLastError() == 1234);

	// A time that is zero stays, and so do attributes that are zero; the last write time is set.
	BasicInformation basic;
	basic.creation.QuadPart = basic.access.QuadPart = basic.change.QuadPart = 0;
	basic.write.QuadPart = 0x01d0000000000000LL;
	basic.attributes = 0;
	FILETIME time;
	CHECK(30, set(file, &io, &basic, sizeof(basic), basic_information) == 0);
	CHECK(31, GetFileTime(file, 0, 0, &time) && time.dwHighDateTime == 0x01d00000 && time.dwLowDateTime == 0);
	CHECK(32, !(GetFileAttributesW(path) & FILE_ATTRIBUTE_READONLY));
	// The read-only attribute is set and cleared, as libuv changes a file's mode.
	basic.attributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE;
	CHECK(33, set(file, &io, &basic, sizeof(basic), basic_information) == 0 && (GetFileAttributesW(path) & FILE_ATTRIBUTE_READONLY));
	basic.attributes = FILE_ATTRIBUTE_ARCHIVE;
	CHECK(34, set(file, &io, &basic, sizeof(basic), basic_information) == 0 && !(GetFileAttributesW(path) & FILE_ATTRIBUTE_READONLY));
	// A handle that may only read attributes changes neither the attribute nor a time.
	HANDLE reader = CreateFileW(path, FILE_READ_ATTRIBUTES, share, 0, OPEN_EXISTING, 0, 0);
	basic.attributes = FILE_ATTRIBUTE_READONLY;
	basic.write.QuadPart = 0x01d1000000000000LL;
	CHECK(35, reader != INVALID_HANDLE_VALUE && set(reader, &io, &basic, sizeof(basic), basic_information) == (LONG)0xc0000022 &&
		!(GetFileAttributesW(path) & FILE_ATTRIBUTE_READONLY) && GetFileTime(file, 0, 0, &time) && time.dwHighDateTime == 0x01d00000);
	CloseHandle(reader);

	// Marked for deletion, the file goes when its last handle closes.
	BYTE remove = 1;
	CHECK(36, set(file, &io, &remove, sizeof(remove), disposition_information) == 0);
	CloseHandle(file);
	CHECK(37, GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);

	// A directory that holds a file reports that it is not empty. With POSIX semantics a name goes at once, under an open handle.
	file = CreateFileW(inner, GENERIC_WRITE | DELETE, share, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	HANDLE folder = CreateFileW(directory, DELETE | FILE_READ_ATTRIBUTES, share, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
	CHECK(38, file != INVALID_HANDLE_VALUE && folder != INVALID_HANDLE_VALUE);
	// FILE_DISPOSITION_DELETE, FILE_DISPOSITION_POSIX_SEMANTICS and FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE, as libuv passes them.
	ULONG posix = 0x13;
	CHECK(39, set(folder, &io, &posix, sizeof(posix), disposition_information_ex) == (LONG)0xc0000101);
	CHECK(40, set(file, &io, &posix, sizeof(posix), disposition_information_ex) == 0 && GetFileAttributesW(inner) == INVALID_FILE_ATTRIBUTES);
	CloseHandle(file);
	CHECK(41, set(folder, &io, &posix, sizeof(posix), disposition_information_ex) == 0);
	CloseHandle(folder);
	CHECK(42, GetFileAttributesW(directory) == INVALID_FILE_ATTRIBUTES);
	return 0;
}
