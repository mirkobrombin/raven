#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleidl.h>

/* A class served by another program: the client names the program as the class's LocalServer32, COM starts it with -Embedding, the
   server offers its class object with CoRegisterClassObject and the client calls the object it gets across the two processes. The
   object answers IOleWindow::GetWindow with the serving process's identifier. A second class is served by a Windows service its
   AppID names, started with the AppID's ServiceParameters. */

static const CLSID test_class = {0x5d1f0f7a, 0x3c55, 0x4e0b, {0x9a, 0x61, 0x2b, 0x7e, 0x40, 0x13, 0x8c, 0x52}};
static const CLSID service_class = {0x5d1f0f7b, 0x3c55, 0x4e0b, {0x9a, 0x61, 0x2b, 0x7e, 0x40, 0x13, 0x8c, 0x52}};
static const WCHAR service_name[] = L"RavenLocalServerTest";
static const WCHAR application[] = L"{5d1f0f7c-3c55-4e0b-9a61-2b7e40138c52}";
static BOOL arguments_seen = TRUE;

int memcmp(const void *left, const void *right, size_t count)
{
	const unsigned char *a = left, *b = right;
	for (size_t i = 0; i < count; i++)
		if (a[i] != b[i])
			return a[i] - b[i];
	return 0;
}

static LONG references;

static HRESULT WINAPI window_query(IOleWindow *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IOleWindow)) {
		*out = iface;
		InterlockedIncrement(&references);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI window_addref(IOleWindow *iface)
{
	(void)iface;
	return InterlockedIncrement(&references);
}

/* The last client gone, the server has nothing left to serve. */
static ULONG WINAPI window_release(IOleWindow *iface)
{
	(void)iface;
	const LONG left = InterlockedDecrement(&references);
	if (!left)
		PostQuitMessage(0);
	return left;
}

static HRESULT WINAPI window_get(IOleWindow *iface, HWND *window)
{
	(void)iface;
	if (!arguments_seen)
		return E_FAIL;
	*window = (HWND)(ULONG_PTR)GetCurrentProcessId();
	return S_OK;
}

static HRESULT WINAPI window_help(IOleWindow *iface, BOOL enter)
{
	(void)iface, (void)enter;
	return E_NOTIMPL;
}

static const IOleWindowVtbl window_table = { window_query, window_addref, window_release, window_get, window_help };
static IOleWindow object = { (IOleWindowVtbl *)&window_table };

static HRESULT WINAPI factory_query(IClassFactory *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_addref(IClassFactory *iface)
{
	(void)iface;
	return 2;
}

static ULONG WINAPI factory_release(IClassFactory *iface)
{
	(void)iface;
	return 1;
}

static HRESULT WINAPI factory_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	(void)iface, (void)outer;
	return window_query(&object, iid, out);
}

static HRESULT WINAPI factory_lock(IClassFactory *iface, BOOL locking)
{
	(void)iface, (void)locking;
	return S_OK;
}

static const IClassFactoryVtbl factory_table = { factory_query, factory_addref, factory_release, factory_create, factory_lock };
static IClassFactory factory = { (IClassFactoryVtbl *)&factory_table };

static int serve(const CLSID *clsid)
{
	DWORD cookie;
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
		return 20;
	if (FAILED(CoRegisterClassObject(clsid, (IUnknown *)&factory, CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED, &cookie)))
		return 21;
	if (FAILED(CoResumeClassObjects()))
		return 22;
	const DWORD start = GetTickCount();
	MSG message;
	for (;;) {
		if (GetTickCount() - start > 30000)
			return 23;
		if (MsgWaitForMultipleObjects(0, NULL, FALSE, 1000, QS_ALLINPUT) != WAIT_OBJECT_0)
			continue;
		if (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
			if (message.message == WM_QUIT)
				break;
			DispatchMessageW(&message);
		}
	}
	if (FAILED(CoRevokeClassObject(cookie)))
		return 24;
	CoUninitialize();
	return 0;
}

static SERVICE_STATUS_HANDLE status_handle;

static DWORD WINAPI control(DWORD code, DWORD type, void *data, void *context)
{
	(void)code, (void)type, (void)data, (void)context;
	return NO_ERROR;
}

static void report(DWORD state, DWORD code)
{
	SERVICE_STATUS status = {SERVICE_WIN32_OWN_PROCESS, state, 0, code};
	SetServiceStatus(status_handle, &status);
}

static int same(const WCHAR *left, const WCHAR *right)
{
	return lstrcmpW(left, right) == 0;
}

/* The service gets its name, then the ServiceParameters as separate arguments. */
static void WINAPI service_main(DWORD count, WCHAR **arguments)
{
	status_handle = RegisterServiceCtrlHandlerExW(service_name, control, NULL);
	arguments_seen = count == 3 && same(arguments[1], L"--com-service") && same(arguments[2], L"two words");
	report(SERVICE_RUNNING, 0);
	report(SERVICE_STOPPED, (DWORD)serve(&service_class));
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	for (const WCHAR *at = line; *at; at++) {
		if (at[0] == L'-' && at[1] == L'E' && at[2] == L'm' && at[3] == L'b')
			return serve(&test_class);
		if (at[0] == L'-' && at[1] == L'S' && at[2] == L'e' && at[3] == L'r') {
			SERVICE_TABLE_ENTRYW table[] = {{(WCHAR *)service_name, service_main}, {NULL, NULL}};
			return StartServiceCtrlDispatcherW(table) ? 0 : 40;
		}
	}
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
		return 1;
	WCHAR self[MAX_PATH], command[MAX_PATH + 4], key[128], text[40];
	GetModuleFileNameW(NULL, self, MAX_PATH);
	wsprintfW(command, L"\"%s\"", self);
	StringFromGUID2(&test_class, text, 40);
	wsprintfW(key, L"Software\\Classes\\CLSID\\%s\\LocalServer32", text);
	HKEY handle;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_WRITE, NULL, &handle, NULL))
		return 2;
	RegSetValueExW(handle, NULL, 0, REG_SZ, (const BYTE *)command, (lstrlenW(command) + 1) * sizeof(WCHAR));
	RegCloseKey(handle);
	/* Not in this process, and no library serves it. */
	IOleWindow *window = NULL;
	if (CoCreateInstance(&test_class, NULL, CLSCTX_INPROC_SERVER, &IID_IOleWindow, (void **)&window) != REGDB_E_CLASSNOTREG)
		return 3;
	if (FAILED(CoCreateInstance(&test_class, NULL, CLSCTX_LOCAL_SERVER, &IID_IOleWindow, (void **)&window)) || !window)
		return 4;
	HWND answer = NULL;
	if (FAILED(IOleWindow_GetWindow(window, &answer)))
		return 5;
	const DWORD server = (DWORD)(ULONG_PTR)answer;
	if (!server || server == GetCurrentProcessId())
		return 6;
	/* A second activation finds the running server instead of starting another. */
	IOleWindow *again = NULL;
	if (FAILED(CoCreateInstance(&test_class, NULL, CLSCTX_ALL, &IID_IOleWindow, (void **)&again)) || !again)
		return 7;
	answer = NULL;
	if (FAILED(IOleWindow_GetWindow(again, &answer)) || (DWORD)(ULONG_PTR)answer != server)
		return 8;
	IOleWindow_Release(again);
	IOleWindow_Release(window);
	/* The server ends once its object is released and withdraws its class. */
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, server);
	if (process && WaitForSingleObject(process, 30000) != WAIT_OBJECT_0)
		return 9;
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);

	/* The same through a service. */
	WCHAR image[MAX_PATH + 16];
	wsprintfW(image, L"\"%s\" -Service", self);
	SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (!manager)
		return 10;
	SC_HANDLE service = CreateServiceW(manager, service_name, service_name, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
		SERVICE_ERROR_NORMAL, image, NULL, NULL, NULL, NULL, NULL);
	/* A run that failed may have left the service behind. */
	if (!service && GetLastError() == ERROR_SERVICE_EXISTS)
		service = OpenServiceW(manager, service_name, SERVICE_ALL_ACCESS);
	if (!service)
		return 11;
	StringFromGUID2(&service_class, text, 40);
	wsprintfW(key, L"Software\\Classes\\CLSID\\%s", text);
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_WRITE, NULL, &handle, NULL))
		return 12;
	RegSetValueExW(handle, L"AppID", 0, REG_SZ, (const BYTE *)application, sizeof application);
	RegCloseKey(handle);
	WCHAR application_key[96];
	wsprintfW(application_key, L"Software\\Classes\\AppID\\%s", application);
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, application_key, 0, NULL, 0, KEY_WRITE, NULL, &handle, NULL))
		return 13;
	static const WCHAR parameters[] = L"--com-service \"two words\"";
	RegSetValueExW(handle, L"LocalService", 0, REG_SZ, (const BYTE *)service_name, sizeof service_name);
	RegSetValueExW(handle, L"ServiceParameters", 0, REG_SZ, (const BYTE *)parameters, sizeof parameters);
	RegCloseKey(handle);
	window = NULL;
	if (FAILED(CoCreateInstance(&service_class, NULL, CLSCTX_LOCAL_SERVER, &IID_IOleWindow, (void **)&window)) || !window)
		return 14;
	answer = NULL;
	if (FAILED(IOleWindow_GetWindow(window, &answer)))
		return 15;
	if (!(DWORD)(ULONG_PTR)answer || (DWORD)(ULONG_PTR)answer == GetCurrentProcessId())
		return 16;
	IOleWindow_Release(window);
	SERVICE_STATUS status = {0};
	for (int tries = 0; tries < 1500 && QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED; tries++)
		Sleep(20);
	if (status.dwCurrentState != SERVICE_STOPPED || status.dwWin32ExitCode)
		return 17;
	DeleteService(service);
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, application_key);
	return 0;
}
