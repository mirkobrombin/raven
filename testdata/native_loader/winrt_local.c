#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <activation.h>
#include <roapi.h>
#include <winstring.h>

static const WCHAR class_name[] = L"Raven.Test.LocalComponent";

#ifdef RAVEN_WINRT_SERVER
static ULONG WINAPI retain(IActivationFactory *self) { return 2; }
static ULONG WINAPI release(IActivationFactory *self) { return 1; }

static BOOL same_iid(REFIID left, REFIID right)
{
	const BYTE *a = (const BYTE *)left, *b = (const BYTE *)right;
	for (unsigned i = 0; i < sizeof(IID); ++i)
		if (a[i] != b[i])
			return FALSE;
	return TRUE;
}

static HRESULT WINAPI query(IActivationFactory *self, REFIID iid, void **out)
{
	*out = NULL;
	if (!same_iid(iid, &IID_IUnknown) && !same_iid(iid, &IID_IInspectable) && !same_iid(iid, &IID_IActivationFactory))
		return E_NOINTERFACE;
	*out = self;
	return S_OK;
}

static HRESULT WINAPI name(IActivationFactory *self, HSTRING *out)
{
	return WindowsCreateString(class_name, ARRAYSIZE(class_name) - 1, out);
}

static HRESULT WINAPI activate(IActivationFactory *self, IInspectable **out)
{
	*out = (IInspectable *)self;
	return S_OK;
}

static IActivationFactoryVtbl table = {query, retain, release, NULL, name, NULL, activate};
static IActivationFactory factory = {&table};

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved) { return TRUE; }

__declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING requested, IActivationFactory **out)
{
	if (lstrcmpW(WindowsGetStringRawBuffer(requested, NULL), class_name))
		return CLASS_E_CLASSNOTAVAILABLE;
	*out = &factory;
	return S_OK;
}
#else
int raven_entry(void)
{
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED)))
		return 1;
	HSTRING name = NULL;
	if (FAILED(WindowsCreateString(class_name, ARRAYSIZE(class_name) - 1, &name)))
		return 2;
	IInspectable *object = NULL;
	HRESULT result = RoActivateInstance(name, &object);
	WindowsDeleteString(name);
	if (result == REGDB_E_CLASSNOTREG)
		return 3;
	if (FAILED(result) || !object)
		return 4;
	if (FAILED(IInspectable_GetRuntimeClassName(object, &name)))
		return 5;
	const BOOL matched = !lstrcmpW(WindowsGetStringRawBuffer(name, NULL), class_name);
	WindowsDeleteString(name);
	IInspectable_Release(object);
	static const WCHAR missing[] = L"Raven.Test.LocalComponent.Missing";
	if (FAILED(WindowsCreateString(missing, ARRAYSIZE(missing) - 1, &name)))
		return 6;
	object = (void *)1;
	result = RoActivateInstance(name, &object);
	WindowsDeleteString(name);
	RoUninitialize();
	return matched && result == REGDB_E_CLASSNOTREG && !object ? 0 : 7;
}
#endif
