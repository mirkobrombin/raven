#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

typedef NTSTATUS (NTAPI *create_port)(PHANDLE, POBJECT_ATTRIBUTES, const void *);
typedef NTSTATUS (NTAPI *query_object)(HANDLE, ULONG, void *, ULONG, ULONG *);

int raven_entry(void)
{
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	const create_port create = (create_port)GetProcAddress(ntdll, "NtAlpcCreatePort");
	const query_object query = (query_object)GetProcAddress(ntdll, "NtQueryObject");
	if (!create || !query)
		return 1;
	HANDLE anonymous = NULL;
	if (create(&anonymous, NULL, NULL) || !anonymous || !CloseHandle(anonymous))
		return 2;

	WCHAR port_name[] = L"\\RPC Control\\RavenAlpcFixture";
	UNICODE_STRING name = {sizeof port_name - sizeof(WCHAR), sizeof port_name, port_name};
	OBJECT_ATTRIBUTES attributes = {sizeof attributes, NULL, &name, 0x40, NULL, NULL};
	if (create(NULL, &attributes, NULL) != (NTSTATUS)0xc0000005)
		return 3;
	HANDLE port = NULL, second = NULL;
	if (create(&port, &attributes, NULL) || !port)
		return 4;
	if (create(&second, &attributes, NULL) != (NTSTATUS)0xc0000035)
		return 5;
	union { UNICODE_STRING text; ULONG words[64]; } result;
	if (query(port, 0, &result, sizeof result, NULL) || result.words[1] != (STANDARD_RIGHTS_ALL | 1))
		return 6;
	if (query(port, 1, &result, sizeof result, NULL) || result.text.Length != name.Length ||
		!result.text.Buffer || lstrcmpiW(result.text.Buffer, port_name))
		return 7;
	if (query(port, 2, &result, sizeof result, NULL) || !result.text.Buffer || lstrcmpW(result.text.Buffer, L"ALPC Port"))
		return 8;
	if (!CloseHandle(port) || create(&second, &attributes, NULL) || !second || !CloseHandle(second))
		return 9;
	return 0;
}
