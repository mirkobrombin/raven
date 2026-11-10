#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
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

// Run in a prefix whose sandbox.conf says "network off": nothing leaves the computer, while the computer itself still answers.
static int network_off(void)
{
	WSADATA data;
	if (WSAStartup(0x202, &data)) return 30;
	SOCKET out = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in remote = {0};
	remote.sin_family = AF_INET;
	remote.sin_port = htons(80);
	remote.sin_addr.s_addr = htonl(0x01010101);
	if (connect(out, (struct sockaddr *)&remote, sizeof(remote)) == 0 || WSAGetLastError() != WSAEACCES) return 31;
	if (sendto(socket(AF_INET, SOCK_DGRAM, 0), "x", 1, 0, (struct sockaddr *)&remote, sizeof(remote)) >= 0 || WSAGetLastError() != WSAEACCES) return 32;
	struct addrinfo *found = NULL;
	if (getaddrinfo("example.org", "80", NULL, &found) == 0) return 33;
	// A program still listens, to the computer only, and reaches itself.
	SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in own = {0};
	int length = sizeof(own);
	own.sin_family = AF_INET;
	if (bind(listener, (struct sockaddr *)&own, sizeof(own)) || listen(listener, 1) || getsockname(listener, (struct sockaddr *)&own, &length)) return 34;
	if (own.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return 35;
	if (connect(socket(AF_INET, SOCK_STREAM, 0), (struct sockaddr *)&own, sizeof(own))) return 36;
	return 0;
}

// Run in a prefix of its own: a document opens with the program the prefix registered for its kind.
static int associate(const WCHAR *program)
{
	WCHAR command[MAX_PATH + 32];
	int at = 0;
	command[at++] = L'"';
	for (const WCHAR *c = program; *c; ++c)
		command[at++] = *c;
	for (const WCHAR *c = L"\" opened \"%1\""; *c; ++c)
		command[at++] = *c;
	command[at] = 0;
	if (RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.ravtest", NULL, REG_SZ, L"Raven.Test", 22) ||
		RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Classes\\Raven.Test\\shell\\open\\command", NULL, REG_SZ, command, (at + 1) * 2)) return 40;
	CreateDirectoryW(L"C:\\raven-associate", NULL);
	DeleteFileW(L"C:\\raven-associate\\opened.txt");
	HANDLE document = CreateFileW(L"C:\\raven-associate\\letter.ravtest", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (document == INVALID_HANDLE_VALUE) return 41;
	CloseHandle(document);
	if ((INT_PTR)ShellExecuteW(NULL, L"open", L"C:\\raven-associate\\letter.ravtest", NULL, NULL, SW_SHOWNORMAL) <= 32) return 42;
	for (int waited = 0; waited < 200; ++waited) {
		if (GetFileAttributesW(L"C:\\raven-associate\\opened.txt") != INVALID_FILE_ATTRIBUTES)
			return 0;
		Sleep(100);
	}
	return 43;
}

// The program registered for the kind: it notes the document it was handed.
static int opened(void)
{
	if (!contains(GetCommandLineW(), L"letter.ravtest")) return 44;
	HANDLE note = CreateFileW(L"C:\\raven-associate\\opened.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (note == INVALID_HANDLE_VALUE) return 45;
	CloseHandle(note);
	return 0;
}

int raven_entry(void)
{
	if (contains(GetCommandLineW(), L" opened "))
		ExitProcess(opened());
	if (contains(GetCommandLineW(), L" associate")) {
		WCHAR self[MAX_PATH];
		return GetModuleFileNameW(NULL, self, MAX_PATH) ? associate(self) : 46;
	}
	if (contains(GetCommandLineW(), L" linux-open"))
		ExitProcess(linux_open());
	if (contains(GetCommandLineW(), L" network-off"))
		return network_off();

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

	// The host drive lists only the ways to granted folders: the program's own, and the system's fonts and the runtime, which every
	// program reads.
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
		if (same(found.cFileName, L"etc") || same(found.cFileName, L"proc") || same(found.cFileName, L"root") ||
			same(found.cFileName, L"boot") || same(found.cFileName, L"dev")) return 8;
	} while (FindNextFileW(search, &found));
	FindClose(search);
	if (!way || listed > 4) return 9;

	// A document no program of the prefix opens is not handed to the host, and neither is a host file.
	if ((INT_PTR)ShellExecuteW(NULL, L"open", L"C:\\windows\\win.ini", NULL, NULL, SW_SHOWNORMAL) > 32) return 10;
	if ((INT_PTR)ShellExecuteW(NULL, L"open", L"Z:\\etc\\passwd", NULL, NULL, SW_SHOWNORMAL) > 32) return 11;
	if ((INT_PTR)ShellExecuteW(NULL, L"open", L"file:///etc/passwd", NULL, NULL, SW_SHOWNORMAL) > 32) return 12;

	// The runtime's private calls answer only its own control panel and file dialog.
	HMODULE host = LoadLibraryW(L"ravenhost.dll");
	if (!host) return 16;
	BOOL (WINAPI *grant)(const WCHAR *, DWORD) = (void *)GetProcAddress(host, "RavenGrantPath");
	BOOL (WINAPI *configure)(const WCHAR *) = (void *)GetProcAddress(host, "RavenSandboxWrite");
	if (!grant || !configure) return 17;
	SetLastError(0);
	if (grant(L"Z:\\etc\\passwd", TRUE) || GetLastError() != ERROR_ACCESS_DENIED) return 18;
	if (configure(L"write /\n") || GetLastError() != ERROR_ACCESS_DENIED) return 19;
	if (GetFileAttributesW(L"Z:\\etc") != INVALID_FILE_ATTRIBUTES) return 20;

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
