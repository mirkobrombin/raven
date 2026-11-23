#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

typedef HRESULT (WINAPI *create_shared_visual)(HANDLE *);
typedef NTSTATUS (NTAPI *query_object)(HANDLE, OBJECT_INFORMATION_CLASS, void *, ULONG, ULONG *);

int raven_entry(void)
{
	const HMODULE composition = LoadLibraryW(L"dcomp.dll");
	if (!composition)
		return 1;
	const create_shared_visual create = (create_shared_visual)GetProcAddress(composition, MAKEINTRESOURCEA(1040));
	if (!create)
		return 2;
	const query_object query = (query_object)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject");
	if (!query || create(NULL) != (HRESULT)0xc000000d)
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
	union { PUBLIC_OBJECT_TYPE_INFORMATION type; WCHAR storage[256]; } object;
	static const WCHAR expected[] = L"Composition";
	if (query(copy, ObjectTypeInformation, &object, sizeof object, NULL) || object.type.TypeName.Length != sizeof expected - sizeof(WCHAR) ||
		!object.type.TypeName.Buffer || lstrcmpW(object.type.TypeName.Buffer, expected))
		return 8;
	return CloseHandle(copy) ? 0 : 9;
}
