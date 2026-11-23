#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

typedef HRESULT (WINAPI *create_shared_visual)(HANDLE *);
typedef NTSTATUS (NTAPI *query_object)(HANDLE, OBJECT_INFORMATION_CLASS, void *, ULONG, ULONG *);

static BOOL is_shared_visual(query_object query, HANDLE handle)
{
	union { PUBLIC_OBJECT_TYPE_INFORMATION type; WCHAR storage[256]; } object;
	const WCHAR expected[] = L"Composition";
	return !query(handle, ObjectTypeInformation, &object, sizeof object, NULL) && object.type.TypeName.Length == sizeof expected - sizeof(WCHAR) &&
		object.type.TypeName.Buffer && !lstrcmpW(object.type.TypeName.Buffer, expected);
}

static int receive_visual(query_object query)
{
	HANDLE visual;
	DWORD read;
	const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	if (!ReadFile(input, &visual, sizeof visual, &read, NULL) || read != sizeof visual)
		return 10;
	return is_shared_visual(query, visual) && CloseHandle(visual) ? 0 : 11;
}

static int send_visual(HANDLE visual)
{
	WCHAR image[MAX_PATH], command[] = L"composition --receive-visual";
	const DWORD length = GetModuleFileNameW(NULL, image, ARRAYSIZE(image));
	HANDLE reader, writer;
	SECURITY_ATTRIBUTES inheritance = {sizeof inheritance, NULL, TRUE};
	if (!length || length == ARRAYSIZE(image) || !CreatePipe(&reader, &writer, &inheritance, 0))
		return 12;
	STARTUPINFOW startup = {sizeof startup};
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = reader;
	startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	PROCESS_INFORMATION child;
	if (!SetHandleInformation(writer, HANDLE_FLAG_INHERIT, 0) ||
		!CreateProcessW(image, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child)) {
		CloseHandle(reader);
		CloseHandle(writer);
		return 13;
	}
	CloseHandle(reader);
	HANDLE remote;
	DWORD written;
	const BOOL sent = DuplicateHandle(GetCurrentProcess(), visual, child.hProcess, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS) &&
		CloseHandle(visual) && WriteFile(writer, &remote, sizeof remote, &written, NULL) && written == sizeof remote;
	CloseHandle(writer);
	DWORD code;
	const BOOL received = WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &code);
	if (!received)
		TerminateProcess(child.hProcess, 1);
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	return sent && received ? (int)code : 14;
}

int raven_entry(void)
{
	const query_object query = (query_object)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject");
	if (!query)
		return 3;
	const WCHAR *command = GetCommandLineW();
	const WCHAR suffix[] = L" --receive-visual";
	const int length = lstrlenW(command), suffix_length = ARRAYSIZE(suffix) - 1;
	if (length >= suffix_length && !lstrcmpW(command + length - suffix_length, suffix))
		return receive_visual(query);
	const HMODULE composition = LoadLibraryW(L"dcomp.dll");
	if (!composition)
		return 1;
	const create_shared_visual create = (create_shared_visual)GetProcAddress(composition, MAKEINTRESOURCEA(1040));
	if (!create)
		return 2;
	if (create(NULL) != (HRESULT)0xc000000d)
		return 3;
	HANDLE visual = NULL;
	if (create(&visual) != S_OK || !visual)
		return 4;
	PUBLIC_OBJECT_BASIC_INFORMATION basic;
	if (query(visual, ObjectBasicInformation, &basic, sizeof basic, NULL) || basic.Attributes || basic.GrantedAccess != 1)
		return 5;
	HANDLE copy;
	const HANDLE self = GetCurrentProcess();
	if (!DuplicateHandle(self, visual, self, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS) || !CloseHandle(visual))
		return 6;
	if (query(visual, ObjectBasicInformation, &basic, sizeof basic, NULL) != (NTSTATUS)0xc0000008)
		return 7;
	if (!is_shared_visual(query, copy))
		return 8;
	return send_visual(copy);
}
