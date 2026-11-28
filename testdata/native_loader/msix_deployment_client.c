#define INITGUID
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <asyncinfo.h>
#include <roapi.h>
#include <shellapi.h>
#include <winstring.h>
#include <windows.foundation.h>
#include <windows.management.deployment.h>

typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClass uri_class;
typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClassFactory uri_factory;
typedef __x_ABI_CWindows_CManagement_CDeployment_CIPackageManager package_manager;
typedef __FIAsyncOperationWithProgress_2_Windows__CManagement__CDeployment__CDeploymentResult_DeploymentProgress deployment_operation;

static void report_status(HRESULT result)
{
	char text[] = "Deployment status 0x00000000\n";
	static const char digits[] = "0123456789abcdef";
	for (unsigned index = 0; index < 8; ++index)
		text[20 + index] = digits[((DWORD)result >> (28 - 4 * index)) & 15];
	DWORD written;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, sizeof(text) - 1, &written, NULL);
}

static HRESULT package_uri(const WCHAR *file, uri_class **uri)
{
	WCHAR path[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
	if (!length || length == ARRAYSIZE(path))
		return E_FAIL;
	while (length && path[length - 1] != L'\\')
		--length;
	if (!length || length + lstrlenW(file) + 1 > ARRAYSIZE(path))
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

static int install(package_manager *manager, const WCHAR *file, HRESULT expected)
{
	uri_class *uri = NULL;
	if (FAILED(package_uri(file, &uri)))
		return 4;

	deployment_operation *operation = NULL;
	HRESULT result = manager->lpVtbl->AddPackageAsync(manager, uri, NULL, 0, &operation);
	uri->lpVtbl->Release(uri);
	if (result == E_NOTIMPL)
		return 5;
	if (FAILED(result) || !operation) {
		report_status(result);
		return 6;
	}
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
	if (status == (FAILED(expected) ? Error : Completed) && SUCCEEDED(result) && error == expected)
		return 0;
	report_status(FAILED(result) ? result : error);
	return 9;
}

#ifdef MSIX_FULL_TRUST
#define FAMILY L"Raven.MsixTest_gh7ecn69rvjyc"
#ifdef _WIN64
#define ARCH L"x64"
#else
#define ARCH L"x86"
#endif
#define FULL_NAME L"Raven.MsixTest_1.2.3.4_" ARCH L"__gh7ecn69rvjyc"

static int check_package_path(void)
{
	typedef LONG (WINAPI *package_path)(PCWSTR, UINT32 *, PWSTR);
	const package_path query = (package_path)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetPackagePathByFullName");
	if (!query)
		return 41;
	const WCHAR expected[] = L"C:\\Program Files\\WindowsApps\\" FULL_NAME;
	WCHAR found[ARRAYSIZE(expected)];
	UINT32 length = 0;
	if (query(FULL_NAME, &length, NULL) != ERROR_INSUFFICIENT_BUFFER || length != ARRAYSIZE(expected))
		return 42;
	--length;
	if (query(FULL_NAME, &length, found) != ERROR_INSUFFICIENT_BUFFER || length != ARRAYSIZE(expected))
		return 43;
	if (query(FULL_NAME, &length, found) != ERROR_SUCCESS || lstrcmpW(found, expected))
		return 44;
	if (query(L"raven.msixTEST_1.2.3.4_" ARCH L"__gh7ecn69rvjyc", &length, found) != ERROR_SUCCESS || lstrcmpW(found, expected))
		return 45;
	if (query(L"Raven.MsixTest_9.9.9.9_" ARCH L"__gh7ecn69rvjyc", &length, NULL) != ERROR_NOT_FOUND)
		return 46;
	if (query(FULL_NAME, NULL, NULL) != ERROR_INVALID_PARAMETER ||
		query(L"..\\Raven.MsixTest", &length, found) != ERROR_INVALID_PARAMETER)
		return 47;
	return 0;
}

static int launch(const WCHAR *target, BOOL exists)
{
	SHELLEXECUTEINFOW info = {sizeof(info)};
	info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
	info.lpVerb = L"open";
	info.lpFile = target;
	info.nShow = SW_HIDE;
	if (!ShellExecuteExW(&info))
		return !exists && GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : 21;
	if (!exists || !info.hProcess)
		return 22;
	DWORD code = 1;
	const BOOL finished = WaitForSingleObject(info.hProcess, 15000) == WAIT_OBJECT_0 && GetExitCodeProcess(info.hProcess, &code);
	CloseHandle(info.hProcess);
	return finished ? (int)code + (code ? 30 : 0) : 23;
}
#endif

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
#ifdef MSIX_FULL_TRUST
	const WCHAR *line = GetCommandLineW();
	const int length = lstrlenW(line);
	if (length >= 11 && !lstrcmpW(line + length - 11, L"--untrusted")) {
		const int check = install(manager, L"app.msix", CERT_E_UNTRUSTEDROOT);
		return check ? check : launch(L"shell:AppsFolder\\" FAMILY L"!Main", FALSE);
	}
	int check = install(manager, L"app-tampered.msix", TRUST_E_BAD_DIGEST);
	if (check)
		return 10 + check;
	check = launch(L"shell:AppsFolder\\" FAMILY L"!Main", FALSE);
	if (check)
		return check;
	check = install(manager, L"app.msix", S_OK);
	if (check)
		return check;
	check = launch(L"shell:AppsFolder\\" FAMILY L"!Missing", FALSE);
	if (check)
		return check;
	manager->lpVtbl->Release(manager);
	check = launch(L"shell:AppsFolder\\" FAMILY L"!Main", TRUE);
	if (check)
		return check;
	/* The package's app of the Windows Runtime runs its view in its core window, with the package's identity. */
	check = launch(L"shell:AppsFolder\\" FAMILY L"!View", TRUE);
	if (check)
		return 100 + check;
	/* Run again, it finds the settings the first run stored; its codes from 50 say so. */
	check = launch(L"shell:AppsFolder\\" FAMILY L"!View", TRUE);
	return check ? 100 + check : check_package_path();
#else
	const int check = install(manager, L"appx-good.msix", TRUST_E_NOSIGNATURE);
	manager->lpVtbl->Release(manager);
	return check;
#endif
}
