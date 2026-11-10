#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <shellapi.h>

// Every way a program could reach a host file the sandbox does not grant: the Win32 and native calls, the namespaces in front of a
// path, the listing of the host drive, the shell, and a Linux system call made directly. The program's own folder is granted, so Z:
// exists and leads to it, and to nothing else.

static int same(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right)
		++left, ++right;
	return *left == *right;
}

static int contains(const WCHAR *text, const WCHAR *part)
{
	for (; *text; ++text) {
		const WCHAR *a = text, *b = part;
		while (*a && *b && *a == *b)
			++a, ++b;
		if (!*b)
			return 1;
	}
	return 0;
}

// Opens /etc/passwd with the host's own system call and reads it; the child that does this exits with 99 only if it read the file.
static int linux_open(void)
{
	static const char path[] = "/etc/passwd";
	char buffer[8] = {0};
	long descriptor;
#ifdef _WIN64
	__asm__ volatile("syscall" : "=a"(descriptor) : "a"(2L), "D"(path), "S"(0L) : "rcx", "r11", "memory");
	if (descriptor >= 0) {
		long count;
		__asm__ volatile("syscall" : "=a"(count) : "a"(0L), "D"(descriptor), "S"(buffer), "d"(4L) : "rcx", "r11", "memory");
	}
#else
	__asm__ volatile("int $0x80" : "=a"(descriptor) : "a"(5L), "b"(path), "c"(0L) : "memory");
	if (descriptor >= 0) {
		long count;
		__asm__ volatile("int $0x80" : "=a"(count) : "a"(3L), "b"(descriptor), "c"(buffer), "d"(4L) : "memory");
	}
#endif
	return buffer[0] == 'r' && buffer[1] == 'o' && buffer[2] == 'o' && buffer[3] == 't' ? 99 : 0;
}

int raven_entry(void)
{
	if (contains(GetCommandLineW(), L" linux-open"))
		ExitProcess(linux_open());

	HANDLE file = CreateFileW(L"Z:\\etc\\passwd", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_ACCESS_DENIED) return 1;
	file = CreateFileW(L"\\\\?\\Z:\\etc\\passwd", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_ACCESS_DENIED) return 2;
	if (GetFileAttributesW(L"Z:\\etc") != INVALID_FILE_ATTRIBUTES) return 3;
	if (CreateDirectoryW(L"Z:\\raven-sandbox", NULL) || GetLastError() != ERROR_ACCESS_DENIED) return 4;

	UNICODE_STRING name;
	RtlInitUnicodeString(&name, L"\\??\\Z:\\etc\\passwd");
	OBJECT_ATTRIBUTES attributes;
	InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
	IO_STATUS_BLOCK status;
	HANDLE native = NULL;
	if (NT_SUCCESS(NtCreateFile(&native, GENERIC_READ | SYNCHRONIZE, &attributes, &status, NULL, 0, FILE_SHARE_READ, FILE_OPEN,
			FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0)) || native) return 5;

	// The host drive lists only the way to the program's own folder.
	WCHAR program[MAX_PATH];
	if (!GetModuleFileNameW(NULL, program, MAX_PATH) || program[0] != L'Z') return 6;
	WCHAR first[MAX_PATH];
	int length = 0;
	for (const WCHAR *at = program + 3; *at && *at != L'\\'; ++at)
		first[length++] = *at;
	first[length] = 0;
	WIN32_FIND_DATAW found;
	HANDLE search = FindFirstFileW(L"Z:\\*", &found);
	if (search == INVALID_HANDLE_VALUE) return 7;
	int listed = 0, way = 0;
	do {
		++listed;
		way |= same(found.cFileName, first);
		if (same(found.cFileName, L"etc") || same(found.cFileName, L"proc")) return 8;
	} while (FindNextFileW(search, &found));
	FindClose(search);
	if (!way || listed != 1) return 9;

	// A document no program of the prefix opens is not handed to the host, and neither is a host file.
	if ((INT_PTR)ShellExecuteW(NULL, L"open", L"C:\\windows\\win.ini", NULL, NULL, SW_SHOWNORMAL) > 32) return 10;
	if ((INT_PTR)ShellExecuteW(NULL, L"open", L"Z:\\etc\\passwd", NULL, NULL, SW_SHOWNORMAL) > 32) return 11;
	if ((INT_PTR)ShellExecuteW(NULL, L"open", L"file:///etc/passwd", NULL, NULL, SW_SHOWNORMAL) > 32) return 12;

	// The Linux system call, made in a child of its own: whatever the runtime does with it, the child must not read the file.
	WCHAR line[MAX_PATH + 16];
	int at = 0;
	line[at++] = L'"';
	for (const WCHAR *c = program; *c; ++c)
		line[at++] = *c;
	for (const WCHAR *c = L"\" linux-open"; *c; ++c)
		line[at++] = *c;
	line[at] = 0;
	STARTUPINFOW startup = {sizeof(startup)};
	PROCESS_INFORMATION child;
	if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child)) return 13;
	if (WaitForSingleObject(child.hProcess, 20000) != WAIT_OBJECT_0) return 14;
	DWORD code = 0;
	if (!GetExitCodeProcess(child.hProcess, &code) || code == 99) return 15;
	return 0;
}
