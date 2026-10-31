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
	WindowsDeleteString(runtime_name);
	IInspectable_Release(object);
	WindowsDeleteString(class_name);
	RoUninitialize();
	return 0;
}
