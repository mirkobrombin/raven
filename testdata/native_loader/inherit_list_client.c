#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <winternl.h>

typedef NTSTATUS (NTAPI *create_section_fn)(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *, LARGE_INTEGER *, ULONG, ULONG, HANDLE);

static WCHAR *hex(WCHAR *at, ULONG_PTR value)
{
	WCHAR digits[17];
	int count=0;
	do { digits[count++]=L"0123456789abcdef"[value&15]; value>>=4; } while (value);
	while (count) *at++=digits[--count];
	return at;
}

static const WCHAR *read_hex(const WCHAR *at, ULONG_PTR *value)
{
	*value=0;
	while (*at==L' ') ++at;
	for (;; ++at) {
		if (*at>=L'0' && *at<=L'9') *value=*value*16+(*at-L'0');
		else if (*at>=L'a' && *at<=L'f') *value=*value*16+(*at-L'a'+10);
		else return at;
	}
}

// The child is handed an event, a section, its parent process and the server end of a pipe nobody has opened yet: it writes into
// the section, finds its parent still running, takes the parent's connection on the pipe and answers on it, then signals the event.
static int child(const WCHAR *arguments)
{
	ULONG_PTR event, section, parent, pipe;
	arguments=read_hex(arguments, &event);
	arguments=read_hex(arguments, &section);
	arguments=read_hex(arguments, &parent);
	read_hex(arguments, &pipe);
	char *view=MapViewOfFile((HANDLE)section, FILE_MAP_WRITE, 0, 0, 4096);
	if (!view) return 51;
	view[0]='R';
	UnmapViewOfFile(view);
	if (WaitForSingleObject((HANDLE)parent, 0)!=WAIT_TIMEOUT) return 53;
	if (!ConnectNamedPipe((HANDLE)pipe, NULL) && GetLastError()!=ERROR_PIPE_CONNECTED) return 54;
	DWORD moved=0;
	if (!WriteFile((HANDLE)pipe, "P", 1, &moved, NULL) || moved!=1) return 55;
	if (!SetEvent((HANDLE)event)) return 52;
	return 0;
}

static BOOL spawn(HANDLE *list, SIZE_T count, const WCHAR *line, PROCESS_INFORMATION *process)
{
	static unsigned char storage[256];
	SIZE_T size=sizeof(storage);
	LPPROC_THREAD_ATTRIBUTE_LIST attributes=(LPPROC_THREAD_ATTRIBUTE_LIST)storage;
	if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return FALSE;
	if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, count*sizeof(HANDLE), NULL, NULL)) return FALSE;
	STARTUPINFOEXW startup={0};
	startup.StartupInfo.cb=sizeof(startup);
	startup.lpAttributeList=attributes;
	static WCHAR buffer[1024];
	lstrcpyW(buffer, line);
	BOOL made=CreateProcessW(NULL, buffer, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &startup.StartupInfo, process);
	DWORD error=GetLastError();
	DeleteProcThreadAttributeList(attributes);
	SetLastError(error);
	return made;
}

// Handles made inheritable through their security or object attributes, rather than SetHandleInformation, may be named in a
// handle list and reach the child; a handle that is not inheritable makes the creation fail.
int raven_entry(void)
{
	const WCHAR *line=GetCommandLineW();
	const WCHAR *mark=line;
	while (*mark && !(mark[0]==L'-' && mark[1]==L'-' && mark[2]==L'c')) ++mark;
	if (*mark) ExitProcess(child(mark+7));

	SECURITY_ATTRIBUTES inheritable={sizeof(inheritable), NULL, TRUE};
	HANDLE event=CreateEventW(&inheritable, TRUE, FALSE, NULL);
	HANDLE plain=CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!event || !plain) return 1;
	DWORD flags=0;
	if (!GetHandleInformation(event, &flags) || !(flags&HANDLE_FLAG_INHERIT)) return 2;
	if (!GetHandleInformation(plain, &flags) || (flags&HANDLE_FLAG_INHERIT)) return 3;
	HANDLE mutex=CreateMutexW(&inheritable, FALSE, NULL);
	HANDLE semaphore=CreateSemaphoreW(&inheritable, 0, 1, NULL);
	if (!GetHandleInformation(mutex, &flags) || !(flags&HANDLE_FLAG_INHERIT)) return 4;
	if (!GetHandleInformation(semaphore, &flags) || !(flags&HANDLE_FLAG_INHERIT)) return 5;

	create_section_fn create_section=(create_section_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateSection");
	if (!create_section) return 6;
	OBJECT_ATTRIBUTES object={.Length=sizeof(object), .Attributes=OBJ_INHERIT};
	LARGE_INTEGER length={.QuadPart=4096};
	HANDLE section=NULL;
	if (create_section(&section, SECTION_ALL_ACCESS, &object, &length, PAGE_READWRITE, SEC_COMMIT, NULL)) return 7;
	if (!GetHandleInformation(section, &flags) || !(flags&HANDLE_FLAG_INHERIT)) return 8;
	HANDLE parent=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION, TRUE, GetCurrentProcessId());
	HANDLE pipe=CreateNamedPipeW(L"\\\\.\\pipe\\raven-inherit-list", PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, 2, 64, 64, 0, &inheritable);
	if (!parent || pipe==INVALID_HANDLE_VALUE) return 16;

	WCHAR self[MAX_PATH];
	DWORD written=GetModuleFileNameW(NULL, self, MAX_PATH);
	if (!written) return 9;
	static WCHAR command[1024];
	WCHAR *at=command;
	*at++=L'"';
	for (DWORD index=0; index<written; ++index) *at++=self[index];
	lstrcpyW(at, L"\" --child ");
	at+=lstrlenW(at);
	at=hex(at, (ULONG_PTR)event);
	*at++=L' ';
	at=hex(at, (ULONG_PTR)section);
	*at++=L' ';
	at=hex(at, (ULONG_PTR)parent);
	*at++=L' ';
	at=hex(at, (ULONG_PTR)pipe);
	*at=0;

	PROCESS_INFORMATION process;
	HANDLE refused[2]={event, plain};
	if (spawn(refused, 2, command, &process) || GetLastError()!=ERROR_INVALID_PARAMETER) return 10;
	HANDLE listed[4]={event, section, parent, pipe};
	if (!spawn(listed, 4, command, &process)) return 11;
	CloseHandle(pipe);
	HANDLE client=CreateFileW(L"\\\\.\\pipe\\raven-inherit-list", GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (client==INVALID_HANDLE_VALUE) return 17;
	char answer=0;
	DWORD moved=0;
	if (!ReadFile(client, &answer, 1, &moved, NULL) || moved!=1 || answer!='P') return 18;
	CloseHandle(client);
	if (WaitForSingleObject(process.hProcess, 30000)) return 12;
	DWORD code=1;
	if (!GetExitCodeProcess(process.hProcess, &code) || code) return 20+(int)code;
	if (WaitForSingleObject(event, 0)) return 13;
	char *view=MapViewOfFile(section, FILE_MAP_READ, 0, 0, 4096);
	if (!view || view[0]!='R') return 14;
	UnmapViewOfFile(view);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);

	// A window without a region has no region box to report.
	RECT box;
	if (GetWindowRgnBox(NULL, &box)!=ERROR || GetLastError()!=ERROR_INVALID_WINDOW_HANDLE) return 15;
	return 0;
}
