#define INITGUID
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <asyncinfo.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.foundation.h>
#include <windows.management.deployment.h>

typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClass uri_class;
typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClassFactory uri_factory;
typedef __x_ABI_CWindows_CManagement_CDeployment_CIPackageManager package_manager;
typedef __FIAsyncOperationWithProgress_2_Windows__CManagement__CDeployment__CDeploymentResult_DeploymentProgress deployment_operation;

static HRESULT package_uri(uri_class **uri)
{
	WCHAR path[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
	if (!length || length == ARRAYSIZE(path))
		return E_FAIL;
	while (length && path[length - 1] != L'\\')
		--length;
	static const WCHAR file[] = L"appx-good.msix";
	if (!length || length + ARRAYSIZE(file) > ARRAYSIZE(path))
		return E_FAIL;
	lstrcpyW(path + length, file);
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return E_FAIL;

	WCHAR url[8 + MAX_PATH];
	lstrcpyW(url, L"file:///");
	for (const WCHAR *at = path; *at; ++at)
		url[8 + (at - path)] = *at == L'\\' ? L'/' : *at;
	url[8 + lstrlenW(path)] = 0;

	static const WCHAR name[] = L"Windows.Foundation.Uri";
	HSTRING text = NULL;
	HRESULT result = WindowsCreateString(name, ARRAYSIZE(name) - 1, &text);
	uri_factory *factory = NULL;
	if (SUCCEEDED(result))
		result = RoGetActivationFactory(text, &IID___x_ABI_CWindows_CFoundation_CIUriRuntimeClassFactory, (void **)&factory);
	WindowsDeleteString(text);
	if (FAILED(result))
		return result;
	text = NULL;
	result = WindowsCreateString(url, lstrlenW(url), &text);
	if (SUCCEEDED(result))
		result = factory->lpVtbl->CreateUri(factory, text, uri);
	WindowsDeleteString(text);
	factory->lpVtbl->Release(factory);
	return result;
}

int raven_entry(void)
{
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED)))
		return 1;
	static const WCHAR name[] = L"Windows.Management.Deployment.PackageManager";
	HSTRING class_name = NULL;
	if (FAILED(WindowsCreateString(name, ARRAYSIZE(name) - 1, &class_name)))
		return 2;
	IInspectable *object = NULL;
	HRESULT result = RoActivateInstance(class_name, &object);
	WindowsDeleteString(class_name);
	package_manager *manager = NULL;
	if (FAILED(result) || FAILED(IInspectable_QueryInterface(object,
			&IID___x_ABI_CWindows_CManagement_CDeployment_CIPackageManager, (void **)&manager)))
		return 3;
	IInspectable_Release(object);
	uri_class *uri = NULL;
	if (FAILED(package_uri(&uri)))
		return 4;

	/* Opening the fixture through the packaging API succeeds, but deployment must check its signature. */
	deployment_operation *operation = NULL;
	result = manager->lpVtbl->AddPackageAsync(manager, uri, NULL, 0, &operation);
	uri->lpVtbl->Release(uri);
	manager->lpVtbl->Release(manager);
	if (result == E_NOTIMPL)
		return 5;
	if (FAILED(result) || !operation)
		return 6;
	IAsyncInfo *info = NULL;
	result = IUnknown_QueryInterface((IUnknown *)operation, &IID_IAsyncInfo, (void **)&info);
	IUnknown_Release((IUnknown *)operation);
	if (FAILED(result))
		return 7;
	AsyncStatus status = Started;
	for (int retry = 0; retry < 500 && status == Started; ++retry) {
		if (FAILED(IAsyncInfo_get_Status(info, &status)))
			return 8;
		if (status == Started)
			Sleep(10);
	}
	HRESULT error = S_OK;
	result = IAsyncInfo_get_ErrorCode(info, &error);
	IAsyncInfo_Release(info);
	return status == Error && SUCCEEDED(result) && FAILED(error) && error != E_NOTIMPL ? 0 : 9;
}
