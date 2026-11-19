#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Calls a program makes when the system has them and goes on without: each one answers, and the process lives on.

typedef struct { void *callback, *context; } Recipient;
typedef DWORD (WINAPI *PowerRegister)(DWORD, void *, void **);
typedef DWORD (WINAPI *PowerUnregister)(void *);
typedef BOOL (WINAPI *InformationByName)(const WCHAR *, int, void *, ULONG);
typedef BOOL (WINAPI *SetLanguages)(DWORD, const WCHAR *, ULONG *);
typedef BOOL (WINAPI *PrivilegeName)(const WCHAR *, LUID *, WCHAR *, DWORD *);

#define CHECK(code, condition) do { if (!(condition)) return code; } while (0)

static LONG breakpoints, reported;

static int same(const WCHAR *left, const WCHAR *right)
{
	unsigned i = 0;
	for (; right[i]; ++i) if (left[i] != right[i]) return 0;
	return !left[i];
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *raised)
{
	const EXCEPTION_RECORD *record = raised->ExceptionRecord;
	if (record->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
#ifdef _WIN64
	DWORD64 *instruction = &raised->ContextRecord->Rip;
#else
	DWORD *instruction = &raised->ContextRecord->Eip;
#endif
	// The breakpoint is reported on its instruction, with one parameter of zero.
	if ((ULONG_PTR)record->ExceptionAddress == *instruction && *(BYTE *)record->ExceptionAddress == 0xcc &&
		record->NumberParameters == 1 && !record->ExceptionInformation[0]) ++reported;
	// Going on from the breakpoint runs it again; the third time steps over it.
	if (++breakpoints == 3) ++*instruction;
	return EXCEPTION_CONTINUE_EXECUTION;
}

int raven_entry(void)
{
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll"), power = LoadLibraryW(L"powrprof.dll"), security = LoadLibraryW(L"advapi32.dll");
	CHECK(1, power && security);

	// A notification of suspend and resume registers and unregisters, as libuv asks for one.
	PowerRegister subscribe = (PowerRegister)GetProcAddress(power, "PowerRegisterSuspendResumeNotification");
	PowerUnregister unsubscribe = (PowerUnregister)GetProcAddress(power, "PowerUnregisterSuspendResumeNotification");
	CHECK(2, subscribe && unsubscribe);
	Recipient recipient = {on_exception, 0};
	void *registration = 0;
	CHECK(3, subscribe(2, &recipient, &registration) == ERROR_SUCCESS && registration);
	CHECK(4, unsubscribe(registration) == ERROR_SUCCESS && subscribe(2, &recipient, 0) == ERROR_INVALID_PARAMETER);

	// The file system does not answer by name alone, which sends libuv to its handle path.
	InformationByName by_name = (InformationByName)GetProcAddress(kernel, "GetFileInformationByName");
	BYTE information[128];
	CHECK(5, by_name);
	SetLastError(0);
	CHECK(6, !by_name(L"C:\\Windows", 0, information, sizeof(information)) && GetLastError() == ERROR_NOT_SUPPORTED);

	// The process language list takes a list and stays the one language, as the thread's does.
	SetLanguages set_languages = (SetLanguages)GetProcAddress(kernel, "SetProcessPreferredUILanguages");
	ULONG count = 0, size = 16;
	WCHAR languages[16];
	CHECK(7, set_languages && set_languages(MUI_LANGUAGE_NAME, L"fr-FR\0", &count) && count == 1);
	CHECK(8, GetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, &count, languages, &size) && count == 1 && same(languages, L"en-US"));

	// A privilege has the name it is looked up by; a short buffer learns the size, and an unknown value is no privilege.
	PrivilegeName privilege_name = (PrivilegeName)GetProcAddress(security, "LookupPrivilegeNameW");
	LUID privilege;
	WCHAR name[32];
	DWORD length = 32;
	CHECK(9, privilege_name && LookupPrivilegeValueW(0, L"SeDebugPrivilege", &privilege));
	CHECK(10, privilege_name(0, &privilege, name, &length) && length == 16 && same(name, L"SeDebugPrivilege"));
	length = 16;
	CHECK(11, !privilege_name(0, &privilege, name, &length) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && length == 17);
	privilege.LowPart = 999;
	length = 32;
	CHECK(12, !privilege_name(0, &privilege, name, &length) && GetLastError() == ERROR_NO_SUCH_PRIVILEGE);

	// DebugBreak raises a breakpoint exception, which a handler takes and the program goes on from, call after call.
	void *handler = AddVectoredExceptionHandler(1, on_exception);
	for (int call = 1; call <= 4; ++call) {
		breakpoints = 0;
		DebugBreak();
		CHECK(13, breakpoints == 3 && reported == 3 * call);
	}
	RemoveVectoredExceptionHandler(handler);
	return 0;
}
