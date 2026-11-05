#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <objbase.h>

/* Side-by-side classes and visual styles as a program with the common controls 6 manifest sees them: its buttons are the version 6
   class under their plain name, contexts made from a manifest file activate and deactivate, and the default button of the visual style
   is drawn in the accent color. A context also serves classes without the registry: a library's comClass, which COM then loads, and
   the clrClass of a strong-named assembly the program carries in its own folder, which the side-by-side library describes to the
   managed runtime. */

#ifndef ICC_STANDARD_CLASSES
#define ICC_STANDARD_CLASSES 0x4000
#endif
/* The push button part and its default state, from the visual styles' part and state tables. */
#define BP_PUSHBUTTON 1
#define PBS_DEFAULTED 5
#ifndef BCM_GETIDEALSIZE
#define BCM_GETIDEALSIZE 0x1601
#endif

int raven_entry(void)
{
	INITCOMMONCONTROLSEX classes = {sizeof classes, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
	WCHAR name[64], path[MAX_PATH];
	if (!InitCommonControlsEx(&classes))
		return 1;
	HWND button = CreateWindowExW(0, L"Button", L"OK", WS_POPUP | BS_PUSHBUTTON, 0, 0, 80, 24, NULL, NULL, GetModuleHandleW(NULL), NULL);
	if (!button)
		return 2;
	if (!GetClassNameW(button, name, 64) || lstrcmpW(name, L"Button"))
		return 3;
	SIZE ideal = {0, 0};
	if (!SendMessageW(button, BCM_GETIDEALSIZE, 0, (LPARAM)&ideal))
		return 4;
	DestroyWindow(button);

	GetModuleFileNameW(NULL, path, MAX_PATH);
	for (WCHAR *at = path + lstrlenW(path); at > path; --at)
		if (at[-1] == L'\\') {
			lstrcpyW(at, L"actctx.manifest");
			break;
		}
	ACTCTXW request = {sizeof request};
	request.lpSource = path;
	HANDLE context = CreateActCtxW(&request);
	if (context == INVALID_HANDLE_VALUE)
		return 5;
	ULONG_PTR cookie = 0;
	HANDLE current = NULL;
	if (!ActivateActCtx(context, &cookie) || !cookie || !GetCurrentActCtx(&current) || current != context)
		return 6;
	ReleaseActCtx(current);
	if (!DeactivateActCtx(0, cookie))
		return 7;
	ReleaseActCtx(context);

	if (!IsThemeActive())
		return 8;
	HWND owner = CreateWindowExW(0, L"Button", L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
	HTHEME theme = OpenThemeData(owner, L"Button");
	if (!theme)
		return 9;
	BITMAPINFO info = {{sizeof(BITMAPINFOHEADER), 40, -20, 1, 32, BI_RGB}};
	void *bits = NULL;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bitmap || !bits)
		return 10;
	SelectObject(dc, bitmap);
	RECT whole = {0, 0, 40, 20};
	if (DrawThemeBackground(theme, dc, BP_PUSHBUTTON, PBS_DEFAULTED, &whole, NULL) != S_OK)
		return 11;
	const BYTE *center = (const BYTE *)bits + (10 * 40 + 20) * 4;
	if (center[0] != 0xb8 || center[1] != 0x5f || center[2] != 0x00)
		return 12;
	CloseThemeData(theme);
	DestroyWindow(owner);

	/* SXS_GUID_INFORMATION_CLR and the flags of SxsLookupClrGuid, as its documentation declares them. */
	typedef struct {
		DWORD size, flags;
		const WCHAR *runtime, *type, *identity;
	} clr_information;
	typedef BOOL (WINAPI *lookup_function)(DWORD, GUID *, HANDLE, void *, SIZE_T, SIZE_T *);
	static const GUID native_class = {0x6f1c9a3e, 0x2b7d, 0x4e15, {0x9c, 0x80, 0x3a, 0x5d, 0x7e, 0x2f, 0x4b, 0x11}};
	static GUID managed_class = {0x2a4e6c80, 0x1b3d, 0x4f57, {0xa9, 0xcb, 0x0d, 0x2f, 0x4e, 0x6a, 0x8c, 0x1e}};
	static const GUID unknown_class = {0x2a4e6c80, 0x1b3d, 0x4f57, {0xa9, 0xcb, 0x0d, 0x2f, 0x4e, 0x6a, 0x8c, 0x1f}};
	for (WCHAR *at = path + lstrlenW(path); at > path; --at)
		if (at[-1] == L'\\') {
			lstrcpyW(at, L"actctx_classes.manifest");
			break;
		}
	HANDLE classes_context = CreateActCtxW(&request);
	if (classes_context == INVALID_HANDLE_VALUE)
		return 13;
	ACTCTX_SECTION_KEYED_DATA data = {sizeof data};
	if (FindActCtxSectionGuid(0, NULL, ACTIVATION_CONTEXT_SECTION_COM_SERVER_REDIRECTION, &native_class, &data))
		return 14;
	if (!ActivateActCtx(classes_context, &cookie))
		return 15;
	if (!FindActCtxSectionGuid(0, NULL, ACTIVATION_CONTEXT_SECTION_COM_SERVER_REDIRECTION, &native_class, &data) || !data.lpData || data.ulAssemblyRosterIndex != 1)
		return 16;
	if (FindActCtxSectionGuid(0, NULL, ACTIVATION_CONTEXT_SECTION_COM_SERVER_REDIRECTION, &unknown_class, &data) || GetLastError() != ERROR_SXS_KEY_NOT_FOUND)
		return 17;
	/* COM looks in the context before the registry: the class is known, only its library is missing. */
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	IUnknown *object = NULL;
	if (CoCreateInstance(&native_class, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&object) == REGDB_E_CLASSNOTREG)
		return 18;
	if (CoCreateInstance(&unknown_class, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&object) != REGDB_E_CLASSNOTREG)
		return 19;
	DeactivateActCtx(0, cookie);
	lookup_function lookup = (lookup_function)GetProcAddress(LoadLibraryW(L"sxs.dll"), "SxsLookupClrGuid");
	if (!lookup)
		return 20;
	SIZE_T needed = 0;
	if (lookup(0x20001, &managed_class, classes_context, NULL, 0, &needed) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed <= sizeof(clr_information))
		return 21;
	static BYTE buffer[1024];
	clr_information *information = (clr_information *)buffer;
	if (!lookup(0x20001, &managed_class, classes_context, buffer, sizeof buffer, &needed) || information->flags != 2 ||
		lstrcmpW(information->type, L"Raven.Test.Managed.Helper") || lstrcmpW(information->runtime, L"v4.0.30319") ||
		lstrcmpW(information->identity, L"Raven.Test.Managed,version='2.0.0.0',publicKeyToken='0123456789abcdef',type='win32'"))
		return 22;
	/* A class is not a surrogate, and a context not in effect is not searched without the flag that names it. */
	if (lookup(0x10001, &managed_class, classes_context, buffer, sizeof buffer, &needed) || lookup(0x20000, &managed_class, NULL, buffer, sizeof buffer, &needed))
		return 23;
	ReleaseActCtx(classes_context);
	CoUninitialize();
	return 0;
}
