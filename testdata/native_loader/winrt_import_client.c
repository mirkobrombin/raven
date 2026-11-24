#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <winstring.h>
#include <roapi.h>

/* A program that imports only the Windows Runtime's activation and strings, through the API sets runtimeobject's import library names,
 * and nothing of COM, the shell or user32: it activates the package manager by name and reads back its class name. */
int raven_entry(void)
{
	static const WCHAR name[] = L"Windows.Management.Deployment.PackageManager";
	HSTRING class_name = NULL, runtime_name = NULL;
	IInspectable *object = NULL;
	UINT32 length = 0;
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;
	if (WindowsCreateString(name, ARRAYSIZE(name) - 1, &class_name) != S_OK) return 2;
	if (RoActivateInstance(class_name, &object) != S_OK || !object) return 3;
	if (IInspectable_GetRuntimeClassName(object, &runtime_name) != S_OK) return 4;
	const WCHAR *text = WindowsGetStringRawBuffer(runtime_name, &length);
	if (length != ARRAYSIZE(name) - 1 || CompareStringOrdinal(text, (int)length, name, -1, FALSE) != CSTR_EQUAL) return 5;
	BOOL embedded = TRUE;
	if (WindowsStringHasEmbeddedNull(runtime_name, &embedded) != S_OK || embedded) return 6;
	static const WCHAR split[] = {L'a', 0, L'b'};
	HSTRING held = NULL;
	if (WindowsCreateString(split, ARRAYSIZE(split), &held) != S_OK || WindowsStringHasEmbeddedNull(held, &embedded) != S_OK || !embedded)
		return 7;
	WindowsDeleteString(held);
	embedded = TRUE;
	if (WindowsStringHasEmbeddedNull(NULL, &embedded) != S_OK || embedded || WindowsStringHasEmbeddedNull(runtime_name, NULL) != E_INVALIDARG)
		return 8;
	WindowsDeleteString(runtime_name);
	IInspectable_Release(object);
	WindowsDeleteString(class_name);
	RoUninitialize();
	/* What the core messaging runtime asks before it names its objects: outside an AppContainer the path is empty, as Windows answers it. */
	typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } path_string;
	typedef LONG (WINAPI *named_object_path)(HANDLE, PSID, BOOLEAN, path_string *);
	typedef BOOL (WINAPI *container_object_path)(HANDLE, PSID, ULONG, LPWSTR, PULONG);
	const named_object_path rtl_path = (named_object_path)(void (*)(void))GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
		"RtlGetAppContainerNamedObjectPath");
	if (!rtl_path) return 9;
	path_string path = {0xaaaa, 0xbbbb, (PWSTR)0x1234};
	if (rtl_path(NULL, NULL, FALSE, &path) != 0 || path.Length || path.MaximumLength || path.Buffer) return 10;
	path = (path_string){0xaaaa, 0xbbbb, (PWSTR)0x1234};
	if (rtl_path(GetCurrentProcessToken(), NULL, TRUE, &path) != 0 || path.Length || path.MaximumLength || path.Buffer) return 11;
	const container_object_path base_path = (container_object_path)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
		"GetAppContainerNamedObjectPath");
	WCHAR buffer[64];
	ULONG returned = 0xdead;
	SetLastError(0);
	if (!base_path || base_path(NULL, NULL, ARRAYSIZE(buffer), buffer, &returned) || GetLastError() != ERROR_NOT_APPCONTAINER || returned != 0xdead)
		return 12;
	return 0;
}
