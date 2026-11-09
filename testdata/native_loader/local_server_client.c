#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleidl.h>

/* A class served by another program: the client names the program as the class's LocalServer32, COM starts it with -Embedding, the
   server offers its class object with CoRegisterClassObject and the client calls the object it gets across the two processes. The
   object answers IOleWindow::GetWindow with the serving process's identifier. */

static const CLSID test_class = {0x5d1f0f7a, 0x3c55, 0x4e0b, {0x9a, 0x61, 0x2b, 0x7e, 0x40, 0x13, 0x8c, 0x52}};

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

static int serve(void)
{
	DWORD cookie;
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
		return 20;
	if (FAILED(CoRegisterClassObject(&test_class, (IUnknown *)&factory, CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED, &cookie)))
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

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	for (const WCHAR *at = line; *at; at++)
		if (at[0] == L'-' && at[1] == L'E' && at[2] == L'm' && at[3] == L'b')
			return serve();
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
	return 0;
}
