/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven browser control component: the WebBrowser control of ieframe, which Windows Forms, WPF and other hosts embed as an ActiveX control.
 *
 * The control activates in place in the window of its host, keeps its position and settings, takes navigations and tells the host's
 * DWebBrowserEvents2 sinks about them as the control does on Windows: BeforeNavigate2, which may cancel, then NavigateComplete2 and
 * DocumentComplete once the message loop of the control's thread runs. When the runtime brings a page engine, pages are fetched and
 * drawn by it, and the object the site offers through IDocHostUIHandler is their window.external; otherwise the window stays blank.
 * The control has no document. */
#define CINTERFACE
#define COBJMACROS
#define CONST_VTABLE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <ole2.h>
#include <olectl.h>
#include <exdisp.h>
#include <exdispid.h>
#include "webengine.h"

static const CLSID browser_class = {0x8856f961, 0x340a, 0x11d0, {0xa9, 0x6b, 0x00, 0xc0, 0x4f, 0xd7, 0x05, 0xa2}};
static const IID unknown_iid = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID factory_iid = {0x00000001, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID dispatch_iid = {0x00020400, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID web_browser_iid = {0xeab22ac1, 0x30c1, 0x11cf, {0xa7, 0xeb, 0x00, 0x00, 0xc0, 0x5b, 0xae, 0x0b}};
static const IID web_browser_app_iid = {0x0002df05, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID web_browser2_iid = {0xd30c1661, 0xcdaf, 0x11d0, {0x8a, 0x3e, 0x00, 0xc0, 0x4f, 0xc9, 0xe2, 0x6e}};
static const IID ole_object_iid = {0x00000112, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID ole_window_iid = {0x00000114, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID in_place_object_iid = {0x00000113, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID in_place_active_iid = {0x00000117, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID in_place_site_iid = {0x00000119, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID ole_control_iid = {0xb196b288, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}};
static const IID point_container_iid = {0xb196b284, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}};
static const IID point_iid = {0xb196b286, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}};
static const IID persist_iid = {0x0000010c, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID persist_stream_init_iid = {0x7fd52380, 0x4e07, 0x101b, {0xae, 0x2d, 0x08, 0x00, 0x2b, 0x2e, 0xc7, 0x13}};
static const IID events2_iid = {0x34a715a0, 0x6587, 0x11d0, {0x92, 0x4a, 0x00, 0x20, 0xaf, 0xc7, 0xac, 0x4d}};
static const IID null_iid = {0};
static const IID host_handler_iid = {0xbd3f23c0, 0xd43e, 0x11cf, {0x89, 0x3b, 0x00, 0xaa, 0x00, 0xbd, 0xce, 0x1a}};

#define SINKS 8
#define NAVIGATED (WM_APP + 0x51)

static LONG locks;
static HINSTANCE module;

__declspec(dllimport) void WINAPI RavenTrace(const char *text);

/* What callers ask for and the control does not have goes to the runtime's trace, which names what hosts expect. */
static void trace_guid(const char *what, REFGUID guid)
{
	char text[96];
	wsprintfA(text, "ieframe: %s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", what, guid->Data1, guid->Data2, guid->Data3, guid->Data4[0],
		guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
	RavenTrace(text);
}

typedef struct browser {
	IWebBrowser2 IWebBrowser2_iface;
	IOleObject IOleObject_iface;
	IOleInPlaceObject IOleInPlaceObject_iface;
	IOleInPlaceActiveObject IOleInPlaceActiveObject_iface;
	IOleControl IOleControl_iface;
	IConnectionPointContainer IConnectionPointContainer_iface;
	IConnectionPoint IConnectionPoint_iface;
	IPersistStreamInit IPersistStreamInit_iface;
	LONG refs;
	IOleClientSite *site;
	IOleInPlaceSite *place;
	HWND window;
	BOOL ui_active;
	RECT position;
	SIZEL extent;
	IDispatch *sinks[SINKS];
	BSTR location;
	BOOL pending;
	VARIANT_BOOL visible, silent, offline, register_browser, drop_target, theater, address_bar, resizable, status_bar, menu_bar, full_screen;
	int tool_bar;
	BSTR status_text;
} browser;

static browser *from_browser(IWebBrowser2 *iface)
{
	return CONTAINING_RECORD(iface, browser, IWebBrowser2_iface);
}

static browser *from_object(IOleObject *iface)
{
	return CONTAINING_RECORD(iface, browser, IOleObject_iface);
}

static browser *from_in_place(IOleInPlaceObject *iface)
{
	return CONTAINING_RECORD(iface, browser, IOleInPlaceObject_iface);
}

static browser *from_active(IOleInPlaceActiveObject *iface)
{
	return CONTAINING_RECORD(iface, browser, IOleInPlaceActiveObject_iface);
}

static browser *from_control(IOleControl *iface)
{
	return CONTAINING_RECORD(iface, browser, IOleControl_iface);
}

static browser *from_container(IConnectionPointContainer *iface)
{
	return CONTAINING_RECORD(iface, browser, IConnectionPointContainer_iface);
}

static browser *from_point(IConnectionPoint *iface)
{
	return CONTAINING_RECORD(iface, browser, IConnectionPoint_iface);
}

static browser *from_persist(IPersistStreamInit *iface)
{
	return CONTAINING_RECORD(iface, browser, IPersistStreamInit_iface);
}

/* The events go to every sink in turn, with the arguments last to first as IDispatch::Invoke takes them. */
static void fire(browser *control, DISPID event, VARIANT *arguments, UINT count)
{
	DISPPARAMS parameters = {arguments, NULL, count, 0};
	IWebBrowser2_AddRef(&control->IWebBrowser2_iface);
	for (int i = 0; i < SINKS; i++) {
		IDispatch *sink = control->sinks[i];
		if (!sink)
			continue;
		IDispatch_AddRef(sink);
		IDispatch_Invoke(sink, event, &null_iid, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &parameters, NULL, NULL, NULL);
		IDispatch_Release(sink);
	}
	IWebBrowser2_Release(&control->IWebBrowser2_iface);
}

/* NavigateComplete2 and DocumentComplete both take the control and the address it reached. */
static void fire_reached(browser *control, DISPID event)
{
	VARIANT address, arguments[2];
	V_VT(&address) = VT_BSTR;
	V_BSTR(&address) = control->location;
	V_VT(&arguments[0]) = VT_BYREF | VT_VARIANT;
	V_VARIANTREF(&arguments[0]) = &address;
	V_VT(&arguments[1]) = VT_DISPATCH;
	V_DISPATCH(&arguments[1]) = (IDispatch *)&control->IWebBrowser2_iface;
	fire(control, event, arguments, 2);
}

static void complete(browser *control)
{
	if (!control->pending)
		return;
	control->pending = FALSE;
	fire_reached(control, DISPID_NAVIGATECOMPLETE2);
	fire_reached(control, DISPID_DOCUMENTCOMPLETE);
}

/* BeforeNavigate2, whose sinks may cancel the navigation: TRUE when one did. */
static BOOL fire_before(browser *control, BSTR address)
{
	VARIANT url, flags, frame, data, headers, arguments[7];
	VARIANT_BOOL cancel = VARIANT_FALSE;
	V_VT(&url) = VT_BSTR;
	V_BSTR(&url) = address;
	V_VT(&flags) = VT_I4;
	V_I4(&flags) = 0;
	V_VT(&frame) = VT_EMPTY;
	V_VT(&data) = VT_EMPTY;
	V_VT(&headers) = VT_EMPTY;
	V_VT(&arguments[0]) = VT_BYREF | VT_BOOL;
	V_BOOLREF(&arguments[0]) = &cancel;
	VARIANT *by_reference[5] = {&headers, &data, &frame, &flags, &url};
	for (int i = 0; i < 5; i++) {
		V_VT(&arguments[i + 1]) = VT_BYREF | VT_VARIANT;
		V_VARIANTREF(&arguments[i + 1]) = by_reference[i];
	}
	V_VT(&arguments[6]) = VT_DISPATCH;
	V_DISPATCH(&arguments[6]) = (IDispatch *)&control->IWebBrowser2_iface;
	fire(control, DISPID_BEFORENAVIGATE2, arguments, 7);
	return cancel != VARIANT_FALSE;
}

static void set_location(browser *control, BSTR text)
{
	SysFreeString(control->location);
	control->location = text;
	control->pending = TRUE;
}

static HRESULT navigate(browser *control, const WCHAR *address)
{
	BSTR text = SysAllocString(address && *address ? address : L"about:blank");
	if (!text)
		return E_OUTOFMEMORY;
	char line[160] = "ieframe: navigate ";
	WideCharToMultiByte(CP_UTF8, 0, text, -1, line + 18, sizeof(line) - 19, NULL, NULL);
	RavenTrace(line);
	if (fire_before(control, text)) {
		SysFreeString(text);
		return S_OK;
	}
	set_location(control, text);
	// A page engine finishes the navigation when its page has loaded; without one the rest arrives from the message loop, and a control
	// not yet in a window finishes at once.
	if (control->window && engine_navigate(control->window, text))
		return S_OK;
	if (!control->window || !PostMessageW(control->window, NAVIGATED, 0, 0))
		complete(control);
	return S_OK;
}

/* What the site offers pages as window.external, through IDocHostUIHandler as for Internet Explorer's pages; a reference for the caller. */
typedef struct host_handler host_handler;
typedef struct {
	HRESULT(WINAPI *QueryInterface)(host_handler *self, REFIID iid, void **out);
	ULONG(WINAPI *AddRef)(host_handler *self);
	ULONG(WINAPI *Release)(host_handler *self);
	void *ShowContextMenu, *GetHostInfo, *ShowUI, *HideUI, *UpdateUI, *EnableModeless, *OnDocWindowActivate, *OnFrameWindowActivate,
		*ResizeBorder, *TranslateAccelerator, *GetOptionKeyPath, *GetDropTarget;
	HRESULT(WINAPI *GetExternal)(host_handler *self, IDispatch **out);
} host_handler_vtbl;
struct host_handler {
	const host_handler_vtbl *lpVtbl;
};

static IDispatch *site_external(browser *control)
{
	host_handler *handler = NULL;
	IDispatch *external = NULL;
	if (control->site && SUCCEEDED(IOleClientSite_QueryInterface(control->site, &host_handler_iid, (void **)&handler)) && handler) {
		if (FAILED(handler->lpVtbl->GetExternal(handler, &external)))
			external = NULL;
		handler->lpVtbl->Release(handler);
	}
	if (!external)
		RavenTrace("ieframe: the site offers pages no window.external");
	return external;
}

static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	browser *control = (browser *)GetWindowLongPtrW(window, GWLP_USERDATA);
	LRESULT result = 0;
	if (control && engine_message(window, message, wparam, lparam, &result))
		return result;
	if (message == NAVIGATED && control) {
		complete(control);
		return 0;
	}
	// The page engine reached an address of its own or the one asked for; a navigation the page started asks the sinks first.
	if (message == ENGINE_REACHED) {
		WCHAR *address = (WCHAR *)lparam;
		BSTR text = control ? SysAllocString(address) : NULL;
		HeapFree(GetProcessHeap(), 0, address);
		if (text) {
			set_location(control, text);
			complete(control);
		}
		return 0;
	}
	if (message == ENGINE_EXTERNAL)
		return control ? (LRESULT)site_external(control) : 0;
	if (message == ENGINE_BEFORE) {
		if (!control)
			return FALSE;
		BSTR text = SysAllocString((const WCHAR *)lparam);
		if (!text)
			return FALSE;
		if (fire_before(control, text)) {
			SysFreeString(text);
			return TRUE;
		}
		set_location(control, text);
		return FALSE;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static const WCHAR window_class[] = L"Shell Embedding";

static BOOL register_window_class(void)
{
	static LONG registered;
	if (registered)
		return TRUE;
	WNDCLASSEXW description = {sizeof(description)};
	description.style = CS_DBLCLKS;
	description.lpfnWndProc = window_procedure;
	description.hInstance = module;
	description.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	description.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	description.lpszClassName = window_class;
	if (!RegisterClassExW(&description) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return FALSE;
	registered = TRUE;
	return TRUE;
}

/* In-place activation as the site leads it: the site agrees, names its window and the control's place in it, and the control's
 * window opens there. */
static HRESULT activate(browser *control, IOleClientSite *site, BOOL ui, HWND container)
{
	if (!control->window) {
		if (!site)
			site = control->site;
		IOleInPlaceSite *place = NULL;
		if (!site || FAILED(IOleClientSite_QueryInterface(site, &in_place_site_iid, (void **)&place)) || !place)
			return E_NOINTERFACE;
		if (IOleInPlaceSite_CanInPlaceActivate(place) != S_OK) {
			IOleInPlaceSite_Release(place);
			return E_FAIL;
		}
		IOleInPlaceSite_OnInPlaceActivate(place);
		HWND parent = NULL;
		IOleInPlaceFrame *frame = NULL;
		IOleInPlaceUIWindow *document = NULL;
		RECT position = {0}, clip = {0};
		OLEINPLACEFRAMEINFO frame_info = {sizeof(frame_info)};
		// The window a verb names holds the control: Windows Forms parks the control's site in a hidden window until the control has a
		// parent, and passes that parent with the verb.
		IOleInPlaceSite_GetWindow(place, &parent);
		if (container && IsWindow(container))
			parent = container;
		if (SUCCEEDED(IOleInPlaceSite_GetWindowContext(place, &frame, &document, &position, &clip, &frame_info)))
			control->position = position;
		if (frame)
			IOleInPlaceFrame_Release(frame);
		if (document)
			IOleInPlaceUIWindow_Release(document);
		if (!register_window_class() || !parent) {
			IOleInPlaceSite_OnInPlaceDeactivate(place);
			IOleInPlaceSite_Release(place);
			return E_FAIL;
		}
		control->window = CreateWindowExW(0, window_class, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, control->position.left,
			control->position.top, control->position.right - control->position.left, control->position.bottom - control->position.top, parent, NULL,
			module, NULL);
		if (!control->window) {
			IOleInPlaceSite_OnInPlaceDeactivate(place);
			IOleInPlaceSite_Release(place);
			return E_FAIL;
		}
		SetWindowLongPtrW(control->window, GWLP_USERDATA, (LONG_PTR)control);
		control->place = place;
		// A navigation that came before the window goes to the page engine now, or completes from the loop.
		if (control->pending && !engine_navigate(control->window, control->location))
			PostMessageW(control->window, NAVIGATED, 0, 0);
	}
	if (ui && !control->ui_active) {
		control->ui_active = TRUE;
		IOleInPlaceSite_OnUIActivate(control->place);
	}
	return S_OK;
}

static void deactivate(browser *control)
{
	if (!control->window)
		return;
	IOleInPlaceSite *place = control->place;
	if (control->ui_active) {
		control->ui_active = FALSE;
		IOleInPlaceSite_OnUIDeactivate(place, FALSE);
	}
	engine_close(control->window);
	SetWindowLongPtrW(control->window, GWLP_USERDATA, 0);
	DestroyWindow(control->window);
	control->window = NULL;
	control->place = NULL;
	IOleInPlaceSite_OnInPlaceDeactivate(place);
	IOleInPlaceSite_Release(place);
}

static HRESULT WINAPI browser_query(IWebBrowser2 *iface, REFIID iid, void **out)
{
	browser *control = from_browser(iface);
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &dispatch_iid) || IsEqualIID(iid, &web_browser_iid) || IsEqualIID(iid, &web_browser_app_iid) ||
		IsEqualIID(iid, &web_browser2_iid))
		*out = &control->IWebBrowser2_iface;
	else if (IsEqualIID(iid, &ole_object_iid))
		*out = &control->IOleObject_iface;
	else if (IsEqualIID(iid, &ole_window_iid) || IsEqualIID(iid, &in_place_object_iid))
		*out = &control->IOleInPlaceObject_iface;
	else if (IsEqualIID(iid, &in_place_active_iid))
		*out = &control->IOleInPlaceActiveObject_iface;
	else if (IsEqualIID(iid, &ole_control_iid))
		*out = &control->IOleControl_iface;
	else if (IsEqualIID(iid, &point_container_iid))
		*out = &control->IConnectionPointContainer_iface;
	else if (IsEqualIID(iid, &persist_iid) || IsEqualIID(iid, &persist_stream_init_iid))
		*out = &control->IPersistStreamInit_iface;
	else {
		trace_guid("control has no", iid);
		*out = NULL;
		return E_NOINTERFACE;
	}
	IWebBrowser2_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI browser_add_ref(IWebBrowser2 *iface)
{
	return InterlockedIncrement(&from_browser(iface)->refs);
}

static ULONG WINAPI browser_release(IWebBrowser2 *iface)
{
	browser *control = from_browser(iface);
	const LONG refs = InterlockedDecrement(&control->refs);
	if (refs)
		return refs;
	control->refs = 1;
	deactivate(control);
	for (int i = 0; i < SINKS; i++)
		if (control->sinks[i])
			IDispatch_Release(control->sinks[i]);
	if (control->site)
		IOleClientSite_Release(control->site);
	SysFreeString(control->location);
	SysFreeString(control->status_text);
	HeapFree(GetProcessHeap(), 0, control);
	InterlockedDecrement(&locks);
	return 0;
}

static HRESULT WINAPI browser_type_info_count(IWebBrowser2 *iface, UINT *count)
{
	if (!count)
		return E_POINTER;
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI browser_type_info(IWebBrowser2 *iface, UINT index, LCID locale, ITypeInfo **out)
{
	if (out)
		*out = NULL;
	return DISP_E_BADINDEX;
}

static HRESULT WINAPI browser_names(IWebBrowser2 *iface, REFIID iid, LPOLESTR *names, UINT count, LCID locale, DISPID *ids)
{
	char text[96] = "ieframe: no member ";
	for (UINT i = 0; i < count; i++)
		ids[i] = DISPID_UNKNOWN;
	if (count && names[0])
		WideCharToMultiByte(CP_UTF8, 0, names[0], -1, text + 19, sizeof(text) - 20, NULL, NULL);
	RavenTrace(text);
	return DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI browser_invoke(IWebBrowser2 *iface, DISPID member, REFIID iid, LCID locale, WORD flags, DISPPARAMS *parameters, VARIANT *result,
	EXCEPINFO *exception, UINT *argument)
{
	return DISP_E_MEMBERNOTFOUND;
}

/* There is no history to go back or forward in. */
static HRESULT WINAPI browser_go_back(IWebBrowser2 *iface)
{
	return E_FAIL;
}

static HRESULT WINAPI browser_go_forward(IWebBrowser2 *iface)
{
	return E_FAIL;
}

static HRESULT WINAPI browser_go_home(IWebBrowser2 *iface)
{
	return navigate(from_browser(iface), L"about:blank");
}

static HRESULT WINAPI browser_go_search(IWebBrowser2 *iface)
{
	return navigate(from_browser(iface), L"about:blank");
}

static HRESULT WINAPI browser_navigate(IWebBrowser2 *iface, BSTR url, VARIANT *flags, VARIANT *frame, VARIANT *data, VARIANT *headers)
{
	return navigate(from_browser(iface), url);
}

static HRESULT WINAPI browser_refresh(IWebBrowser2 *iface)
{
	return S_OK;
}

static HRESULT WINAPI browser_refresh2(IWebBrowser2 *iface, VARIANT *level)
{
	return S_OK;
}

static HRESULT WINAPI browser_stop(IWebBrowser2 *iface)
{
	from_browser(iface)->pending = FALSE;
	return S_OK;
}

static HRESULT WINAPI browser_application(IWebBrowser2 *iface, IDispatch **out)
{
	if (!out)
		return E_POINTER;
	*out = (IDispatch *)iface;
	IWebBrowser2_AddRef(iface);
	return S_OK;
}

static HRESULT WINAPI browser_parent(IWebBrowser2 *iface, IDispatch **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return S_OK;
}

static HRESULT WINAPI browser_container(IWebBrowser2 *iface, IDispatch **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return S_OK;
}

/* No page is loaded, so there is no document to hand out. */
static HRESULT WINAPI browser_document(IWebBrowser2 *iface, IDispatch **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	RavenTrace("ieframe: no document");
	return S_OK;
}

static HRESULT WINAPI browser_top_level(IWebBrowser2 *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = VARIANT_TRUE;
	return S_OK;
}

static HRESULT WINAPI browser_type(IWebBrowser2 *iface, BSTR *out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return E_FAIL;
}

static HRESULT WINAPI browser_left(IWebBrowser2 *iface, LONG *out)
{
	if (!out)
		return E_POINTER;
	*out = from_browser(iface)->position.left;
	return S_OK;
}

static HRESULT WINAPI browser_top(IWebBrowser2 *iface, LONG *out)
{
	if (!out)
		return E_POINTER;
	*out = from_browser(iface)->position.top;
	return S_OK;
}

static HRESULT WINAPI browser_width(IWebBrowser2 *iface, LONG *out)
{
	const browser *control = from_browser(iface);
	if (!out)
		return E_POINTER;
	*out = control->position.right - control->position.left;
	return S_OK;
}

static HRESULT WINAPI browser_height(IWebBrowser2 *iface, LONG *out)
{
	const browser *control = from_browser(iface);
	if (!out)
		return E_POINTER;
	*out = control->position.bottom - control->position.top;
	return S_OK;
}

static void place_window(browser *control)
{
	if (control->window)
		MoveWindow(control->window, control->position.left, control->position.top, control->position.right - control->position.left,
			control->position.bottom - control->position.top, TRUE);
}

static HRESULT WINAPI browser_put_left(IWebBrowser2 *iface, LONG value)
{
	browser *control = from_browser(iface);
	control->position.right += value - control->position.left;
	control->position.left = value;
	place_window(control);
	return S_OK;
}

static HRESULT WINAPI browser_put_top(IWebBrowser2 *iface, LONG value)
{
	browser *control = from_browser(iface);
	control->position.bottom += value - control->position.top;
	control->position.top = value;
	place_window(control);
	return S_OK;
}

static HRESULT WINAPI browser_put_width(IWebBrowser2 *iface, LONG value)
{
	browser *control = from_browser(iface);
	control->position.right = control->position.left + value;
	place_window(control);
	return S_OK;
}

static HRESULT WINAPI browser_put_height(IWebBrowser2 *iface, LONG value)
{
	browser *control = from_browser(iface);
	control->position.bottom = control->position.top + value;
	place_window(control);
	return S_OK;
}

static HRESULT WINAPI browser_location_name(IWebBrowser2 *iface, BSTR *out)
{
	const browser *control = from_browser(iface);
	if (!out)
		return E_POINTER;
	*out = SysAllocString(control->location ? control->location : L"");
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI browser_location_url(IWebBrowser2 *iface, BSTR *out)
{
	const browser *control = from_browser(iface);
	if (!out)
		return E_POINTER;
	*out = SysAllocString(control->location ? control->location : L"");
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI browser_busy(IWebBrowser2 *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = from_browser(iface)->pending ? VARIANT_TRUE : VARIANT_FALSE;
	return S_OK;
}

static HRESULT WINAPI browser_quit(IWebBrowser2 *iface)
{
	return E_FAIL;
}

static HRESULT WINAPI browser_client_to_window(IWebBrowser2 *iface, int *width, int *height)
{
	return S_OK;
}

static HRESULT WINAPI browser_put_property(IWebBrowser2 *iface, BSTR name, VARIANT value)
{
	return S_OK;
}

static HRESULT WINAPI browser_get_property(IWebBrowser2 *iface, BSTR name, VARIANT *value)
{
	if (!value)
		return E_POINTER;
	V_VT(value) = VT_EMPTY;
	return S_OK;
}

static HRESULT WINAPI browser_name(IWebBrowser2 *iface, BSTR *out)
{
	if (!out)
		return E_POINTER;
	*out = SysAllocString(L"Internet Explorer");
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* As on Windows, a control in another program's window has no top-level window of its own to name. */
static HRESULT WINAPI browser_hwnd(IWebBrowser2 *iface, SHANDLE_PTR *out)
{
	if (!out)
		return E_POINTER;
	*out = 0;
	return E_FAIL;
}

static HRESULT WINAPI browser_full_name(IWebBrowser2 *iface, BSTR *out)
{
	if (out)
		*out = NULL;
	return E_FAIL;
}

static HRESULT WINAPI browser_path(IWebBrowser2 *iface, BSTR *out)
{
	if (out)
		*out = NULL;
	return E_FAIL;
}

#define FLAG(name, field) \
	static HRESULT WINAPI browser_get_##name(IWebBrowser2 *iface, VARIANT_BOOL *out) \
	{ \
		if (!out) \
			return E_POINTER; \
		*out = from_browser(iface)->field; \
		return S_OK; \
	} \
	static HRESULT WINAPI browser_put_##name(IWebBrowser2 *iface, VARIANT_BOOL value) \
	{ \
		from_browser(iface)->field = value ? VARIANT_TRUE : VARIANT_FALSE; \
		return S_OK; \
	}

FLAG(visible, visible)
FLAG(status_bar, status_bar)
FLAG(menu_bar, menu_bar)
FLAG(full_screen, full_screen)
FLAG(offline, offline)
FLAG(silent, silent)
FLAG(register_browser, register_browser)
FLAG(drop_target, drop_target)
FLAG(theater, theater)
FLAG(address_bar, address_bar)
FLAG(resizable, resizable)

static HRESULT WINAPI browser_status_text(IWebBrowser2 *iface, BSTR *out)
{
	const browser *control = from_browser(iface);
	if (!out)
		return E_POINTER;
	*out = SysAllocString(control->status_text ? control->status_text : L"");
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI browser_put_status_text(IWebBrowser2 *iface, BSTR text)
{
	browser *control = from_browser(iface);
	BSTR copy = SysAllocString(text ? text : L"");
	if (!copy)
		return E_OUTOFMEMORY;
	SysFreeString(control->status_text);
	control->status_text = copy;
	return S_OK;
}

static HRESULT WINAPI browser_tool_bar(IWebBrowser2 *iface, int *out)
{
	if (!out)
		return E_POINTER;
	*out = from_browser(iface)->tool_bar;
	return S_OK;
}

static HRESULT WINAPI browser_put_tool_bar(IWebBrowser2 *iface, int value)
{
	from_browser(iface)->tool_bar = value;
	return S_OK;
}

/* Navigate2 takes the address as a variant, text or an item list; only text names a place here. */
static HRESULT WINAPI browser_navigate2(IWebBrowser2 *iface, VARIANT *url, VARIANT *flags, VARIANT *frame, VARIANT *data, VARIANT *headers)
{
	if (!url)
		return E_INVALIDARG;
	while (V_VT(url) == (VT_BYREF | VT_VARIANT))
		url = V_VARIANTREF(url);
	if (V_VT(url) == VT_BSTR)
		return navigate(from_browser(iface), V_BSTR(url));
	if (V_VT(url) == (VT_BYREF | VT_BSTR))
		return navigate(from_browser(iface), *V_BSTRREF(url));
	return E_INVALIDARG;
}

static HRESULT WINAPI browser_query_status(IWebBrowser2 *iface, OLECMDID command, OLECMDF *out)
{
	if (!out)
		return E_POINTER;
	*out = 0;
	return S_OK;
}

static HRESULT WINAPI browser_exec(IWebBrowser2 *iface, OLECMDID command, OLECMDEXECOPT option, VARIANT *in, VARIANT *out)
{
	return OLECMDERR_E_NOTSUPPORTED;
}

static HRESULT WINAPI browser_show_bar(IWebBrowser2 *iface, VARIANT *bar, VARIANT *show, VARIANT *size)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI browser_ready_state(IWebBrowser2 *iface, READYSTATE *out)
{
	if (!out)
		return E_POINTER;
	*out = from_browser(iface)->pending ? READYSTATE_LOADING : READYSTATE_COMPLETE;
	return S_OK;
}

static const IWebBrowser2Vtbl browser_vtbl = {
	.QueryInterface = browser_query,
	.AddRef = browser_add_ref,
	.Release = browser_release,
	.GetTypeInfoCount = browser_type_info_count,
	.GetTypeInfo = browser_type_info,
	.GetIDsOfNames = browser_names,
	.Invoke = browser_invoke,
	.GoBack = browser_go_back,
	.GoForward = browser_go_forward,
	.GoHome = browser_go_home,
	.GoSearch = browser_go_search,
	.Navigate = browser_navigate,
	.Refresh = browser_refresh,
	.Refresh2 = browser_refresh2,
	.Stop = browser_stop,
	.get_Application = browser_application,
	.get_Parent = browser_parent,
	.get_Container = browser_container,
	.get_Document = browser_document,
	.get_TopLevelContainer = browser_top_level,
	.get_Type = browser_type,
	.get_Left = browser_left,
	.put_Left = browser_put_left,
	.get_Top = browser_top,
	.put_Top = browser_put_top,
	.get_Width = browser_width,
	.put_Width = browser_put_width,
	.get_Height = browser_height,
	.put_Height = browser_put_height,
	.get_LocationName = browser_location_name,
	.get_LocationURL = browser_location_url,
	.get_Busy = browser_busy,
	.Quit = browser_quit,
	.ClientToWindow = browser_client_to_window,
	.PutProperty = browser_put_property,
	.GetProperty = browser_get_property,
	.get_Name = browser_name,
	.get_HWND = browser_hwnd,
	.get_FullName = browser_full_name,
	.get_Path = browser_path,
	.get_Visible = browser_get_visible,
	.put_Visible = browser_put_visible,
	.get_StatusBar = browser_get_status_bar,
	.put_StatusBar = browser_put_status_bar,
	.get_StatusText = browser_status_text,
	.put_StatusText = browser_put_status_text,
	.get_ToolBar = browser_tool_bar,
	.put_ToolBar = browser_put_tool_bar,
	.get_MenuBar = browser_get_menu_bar,
	.put_MenuBar = browser_put_menu_bar,
	.get_FullScreen = browser_get_full_screen,
	.put_FullScreen = browser_put_full_screen,
	.Navigate2 = browser_navigate2,
	.QueryStatusWB = browser_query_status,
	.ExecWB = browser_exec,
	.ShowBrowserBar = browser_show_bar,
	.get_ReadyState = browser_ready_state,
	.get_Offline = browser_get_offline,
	.put_Offline = browser_put_offline,
	.get_Silent = browser_get_silent,
	.put_Silent = browser_put_silent,
	.get_RegisterAsBrowser = browser_get_register_browser,
	.put_RegisterAsBrowser = browser_put_register_browser,
	.get_RegisterAsDropTarget = browser_get_drop_target,
	.put_RegisterAsDropTarget = browser_put_drop_target,
	.get_TheaterMode = browser_get_theater,
	.put_TheaterMode = browser_put_theater,
	.get_AddressBar = browser_get_address_bar,
	.put_AddressBar = browser_put_address_bar,
	.get_Resizable = browser_get_resizable,
	.put_Resizable = browser_put_resizable,
};

static HRESULT WINAPI object_query(IOleObject *iface, REFIID iid, void **out)
{
	return browser_query(&from_object(iface)->IWebBrowser2_iface, iid, out);
}

static ULONG WINAPI object_add_ref(IOleObject *iface)
{
	return browser_add_ref(&from_object(iface)->IWebBrowser2_iface);
}

static ULONG WINAPI object_release(IOleObject *iface)
{
	return browser_release(&from_object(iface)->IWebBrowser2_iface);
}

static HRESULT WINAPI object_set_site(IOleObject *iface, IOleClientSite *site)
{
	browser *control = from_object(iface);
	if (site)
		IOleClientSite_AddRef(site);
	if (control->site)
		IOleClientSite_Release(control->site);
	control->site = site;
	return S_OK;
}

static HRESULT WINAPI object_get_site(IOleObject *iface, IOleClientSite **out)
{
	browser *control = from_object(iface);
	if (!out)
		return E_POINTER;
	*out = control->site;
	if (control->site)
		IOleClientSite_AddRef(control->site);
	return S_OK;
}

static HRESULT WINAPI object_host_names(IOleObject *iface, LPCOLESTR application, LPCOLESTR object)
{
	return S_OK;
}

static HRESULT WINAPI object_close(IOleObject *iface, DWORD option)
{
	deactivate(from_object(iface));
	return S_OK;
}

static HRESULT WINAPI object_set_moniker(IOleObject *iface, DWORD which, IMoniker *moniker)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI object_get_moniker(IOleObject *iface, DWORD assign, DWORD which, IMoniker **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI object_init_from_data(IOleObject *iface, IDataObject *data, BOOL creation, DWORD reserved)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI object_clipboard_data(IOleObject *iface, DWORD reserved, IDataObject **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI object_do_verb(IOleObject *iface, LONG verb, LPMSG message, IOleClientSite *site, LONG index, HWND parent, LPCRECT position)
{
	browser *control = from_object(iface);
	switch (verb) {
	case OLEIVERB_PRIMARY:
	case OLEIVERB_SHOW:
	case OLEIVERB_UIACTIVATE:
		return activate(control, site, TRUE, parent);
	case OLEIVERB_INPLACEACTIVATE:
		return activate(control, site, FALSE, parent);
	case OLEIVERB_HIDE:
		if (control->window)
			ShowWindow(control->window, SW_HIDE);
		return S_OK;
	case OLEIVERB_DISCARDUNDOSTATE:
		return S_OK;
	default:
		return OLEOBJ_S_INVALIDVERB;
	}
}

static HRESULT WINAPI object_enum_verbs(IOleObject *iface, IEnumOLEVERB **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI object_update(IOleObject *iface)
{
	return S_OK;
}

static HRESULT WINAPI object_up_to_date(IOleObject *iface)
{
	return S_OK;
}

static HRESULT WINAPI object_user_class(IOleObject *iface, CLSID *out)
{
	if (!out)
		return E_POINTER;
	*out = browser_class;
	return S_OK;
}

static HRESULT WINAPI object_user_type(IOleObject *iface, DWORD form, LPOLESTR *out)
{
	return OLE_S_USEREG;
}

static HRESULT WINAPI object_set_extent(IOleObject *iface, DWORD aspect, SIZEL *size)
{
	if (!size)
		return E_INVALIDARG;
	from_object(iface)->extent = *size;
	return S_OK;
}

static HRESULT WINAPI object_get_extent(IOleObject *iface, DWORD aspect, SIZEL *size)
{
	if (!size)
		return E_INVALIDARG;
	*size = from_object(iface)->extent;
	return S_OK;
}

/* The control's look never changes from a page, so there is nothing to tell an advise sink. */
static HRESULT WINAPI object_advise(IOleObject *iface, IAdviseSink *sink, DWORD *cookie)
{
	if (!cookie)
		return E_POINTER;
	*cookie = 1;
	return S_OK;
}

static HRESULT WINAPI object_unadvise(IOleObject *iface, DWORD cookie)
{
	return S_OK;
}

static HRESULT WINAPI object_enum_advise(IOleObject *iface, IEnumSTATDATA **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI object_misc_status(IOleObject *iface, DWORD aspect, DWORD *out)
{
	if (!out)
		return E_POINTER;
	*out = OLEMISC_SETCLIENTSITEFIRST | OLEMISC_ACTIVATEWHENVISIBLE | OLEMISC_INSIDEOUT | OLEMISC_CANTLINKINSIDE | OLEMISC_RECOMPOSEONRESIZE;
	return S_OK;
}

static HRESULT WINAPI object_color_scheme(IOleObject *iface, LOGPALETTE *palette)
{
	return E_NOTIMPL;
}

static const IOleObjectVtbl object_vtbl = {
	.QueryInterface = object_query,
	.AddRef = object_add_ref,
	.Release = object_release,
	.SetClientSite = object_set_site,
	.GetClientSite = object_get_site,
	.SetHostNames = object_host_names,
	.Close = object_close,
	.SetMoniker = object_set_moniker,
	.GetMoniker = object_get_moniker,
	.InitFromData = object_init_from_data,
	.GetClipboardData = object_clipboard_data,
	.DoVerb = object_do_verb,
	.EnumVerbs = object_enum_verbs,
	.Update = object_update,
	.IsUpToDate = object_up_to_date,
	.GetUserClassID = object_user_class,
	.GetUserType = object_user_type,
	.SetExtent = object_set_extent,
	.GetExtent = object_get_extent,
	.Advise = object_advise,
	.Unadvise = object_unadvise,
	.EnumAdvise = object_enum_advise,
	.GetMiscStatus = object_misc_status,
	.SetColorScheme = object_color_scheme,
};

static HRESULT WINAPI in_place_query(IOleInPlaceObject *iface, REFIID iid, void **out)
{
	return browser_query(&from_in_place(iface)->IWebBrowser2_iface, iid, out);
}

static ULONG WINAPI in_place_add_ref(IOleInPlaceObject *iface)
{
	return browser_add_ref(&from_in_place(iface)->IWebBrowser2_iface);
}

static ULONG WINAPI in_place_release(IOleInPlaceObject *iface)
{
	return browser_release(&from_in_place(iface)->IWebBrowser2_iface);
}

static HRESULT control_window(const browser *control, HWND *out)
{
	if (!out)
		return E_POINTER;
	*out = control->window;
	return control->window ? S_OK : E_FAIL;
}

static HRESULT WINAPI in_place_window(IOleInPlaceObject *iface, HWND *out)
{
	return control_window(from_in_place(iface), out);
}

static HRESULT WINAPI in_place_help(IOleInPlaceObject *iface, BOOL enter)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI in_place_deactivate(IOleInPlaceObject *iface)
{
	deactivate(from_in_place(iface));
	return S_OK;
}

static HRESULT WINAPI in_place_ui_deactivate(IOleInPlaceObject *iface)
{
	browser *control = from_in_place(iface);
	if (control->ui_active) {
		control->ui_active = FALSE;
		IOleInPlaceSite_OnUIDeactivate(control->place, FALSE);
	}
	return S_OK;
}

static HRESULT WINAPI in_place_rects(IOleInPlaceObject *iface, LPCRECT position, LPCRECT clip)
{
	browser *control = from_in_place(iface);
	if (!position)
		return E_INVALIDARG;
	control->position = *position;
	place_window(control);
	return S_OK;
}

static HRESULT WINAPI in_place_reactivate(IOleInPlaceObject *iface)
{
	return E_NOTIMPL;
}

static const IOleInPlaceObjectVtbl in_place_vtbl = {
	.QueryInterface = in_place_query,
	.AddRef = in_place_add_ref,
	.Release = in_place_release,
	.GetWindow = in_place_window,
	.ContextSensitiveHelp = in_place_help,
	.InPlaceDeactivate = in_place_deactivate,
	.UIDeactivate = in_place_ui_deactivate,
	.SetObjectRects = in_place_rects,
	.ReactivateAndUndo = in_place_reactivate,
};

static HRESULT WINAPI active_query(IOleInPlaceActiveObject *iface, REFIID iid, void **out)
{
	return browser_query(&from_active(iface)->IWebBrowser2_iface, iid, out);
}

static ULONG WINAPI active_add_ref(IOleInPlaceActiveObject *iface)
{
	return browser_add_ref(&from_active(iface)->IWebBrowser2_iface);
}

static ULONG WINAPI active_release(IOleInPlaceActiveObject *iface)
{
	return browser_release(&from_active(iface)->IWebBrowser2_iface);
}

static HRESULT WINAPI active_window(IOleInPlaceActiveObject *iface, HWND *out)
{
	return control_window(from_active(iface), out);
}

static HRESULT WINAPI active_help(IOleInPlaceActiveObject *iface, BOOL enter)
{
	return E_NOTIMPL;
}

/* With no page there are no keys of its own to take, so every key goes back to the host. */
static HRESULT WINAPI active_accelerator(IOleInPlaceActiveObject *iface, LPMSG message)
{
	return S_FALSE;
}

static HRESULT WINAPI active_frame_activate(IOleInPlaceActiveObject *iface, BOOL activate)
{
	return S_OK;
}

static HRESULT WINAPI active_document_activate(IOleInPlaceActiveObject *iface, BOOL activate)
{
	return S_OK;
}

static HRESULT WINAPI active_resize_border(IOleInPlaceActiveObject *iface, LPCRECT border, IOleInPlaceUIWindow *window, BOOL frame)
{
	return S_OK;
}

static HRESULT WINAPI active_modeless(IOleInPlaceActiveObject *iface, BOOL enable)
{
	return S_OK;
}

static const IOleInPlaceActiveObjectVtbl active_vtbl = {
	.QueryInterface = active_query,
	.AddRef = active_add_ref,
	.Release = active_release,
	.GetWindow = active_window,
	.ContextSensitiveHelp = active_help,
	.TranslateAccelerator = active_accelerator,
	.OnFrameWindowActivate = active_frame_activate,
	.OnDocWindowActivate = active_document_activate,
	.ResizeBorder = active_resize_border,
	.EnableModeless = active_modeless,
};

static HRESULT WINAPI control_query(IOleControl *iface, REFIID iid, void **out)
{
	return browser_query(&from_control(iface)->IWebBrowser2_iface, iid, out);
}

static ULONG WINAPI control_add_ref(IOleControl *iface)
{
	return browser_add_ref(&from_control(iface)->IWebBrowser2_iface);
}

static ULONG WINAPI control_release(IOleControl *iface)
{
	return browser_release(&from_control(iface)->IWebBrowser2_iface);
}

static HRESULT WINAPI control_info(IOleControl *iface, CONTROLINFO *info)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI control_mnemonic(IOleControl *iface, MSG *message)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI control_ambient(IOleControl *iface, DISPID property)
{
	return S_OK;
}

static HRESULT WINAPI control_freeze(IOleControl *iface, BOOL freeze)
{
	return S_OK;
}

static const IOleControlVtbl control_vtbl = {
	.QueryInterface = control_query,
	.AddRef = control_add_ref,
	.Release = control_release,
	.GetControlInfo = control_info,
	.OnMnemonic = control_mnemonic,
	.OnAmbientPropertyChange = control_ambient,
	.FreezeEvents = control_freeze,
};

static HRESULT WINAPI container_query(IConnectionPointContainer *iface, REFIID iid, void **out)
{
	return browser_query(&from_container(iface)->IWebBrowser2_iface, iid, out);
}

static ULONG WINAPI container_add_ref(IConnectionPointContainer *iface)
{
	return browser_add_ref(&from_container(iface)->IWebBrowser2_iface);
}

static ULONG WINAPI container_release(IConnectionPointContainer *iface)
{
	return browser_release(&from_container(iface)->IWebBrowser2_iface);
}

static HRESULT WINAPI container_enum(IConnectionPointContainer *iface, IEnumConnectionPoints **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

/* The events are those of DWebBrowserEvents2; the older DWebBrowserEvents are not sent. */
static HRESULT WINAPI container_find(IConnectionPointContainer *iface, REFIID iid, IConnectionPoint **out)
{
	browser *control = from_container(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(iid, &events2_iid)) {
		trace_guid("control sends no", iid);
		return CONNECT_E_NOCONNECTION;
	}
	*out = &control->IConnectionPoint_iface;
	IWebBrowser2_AddRef(&control->IWebBrowser2_iface);
	return S_OK;
}

static const IConnectionPointContainerVtbl container_vtbl = {
	.QueryInterface = container_query,
	.AddRef = container_add_ref,
	.Release = container_release,
	.EnumConnectionPoints = container_enum,
	.FindConnectionPoint = container_find,
};

static HRESULT WINAPI point_query(IConnectionPoint *iface, REFIID iid, void **out)
{
	browser *control = from_point(iface);
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &point_iid)) {
		*out = iface;
		IWebBrowser2_AddRef(&control->IWebBrowser2_iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI point_add_ref(IConnectionPoint *iface)
{
	return browser_add_ref(&from_point(iface)->IWebBrowser2_iface);
}

static ULONG WINAPI point_release(IConnectionPoint *iface)
{
	return browser_release(&from_point(iface)->IWebBrowser2_iface);
}

static HRESULT WINAPI point_interface(IConnectionPoint *iface, IID *out)
{
	if (!out)
		return E_POINTER;
	*out = events2_iid;
	return S_OK;
}

static HRESULT WINAPI point_container(IConnectionPoint *iface, IConnectionPointContainer **out)
{
	browser *control = from_point(iface);
	if (!out)
		return E_POINTER;
	*out = &control->IConnectionPointContainer_iface;
	IWebBrowser2_AddRef(&control->IWebBrowser2_iface);
	return S_OK;
}

static HRESULT WINAPI point_advise(IConnectionPoint *iface, IUnknown *sink, DWORD *cookie)
{
	browser *control = from_point(iface);
	IDispatch *dispatch = NULL;
	if (!sink || !cookie)
		return E_POINTER;
	*cookie = 0;
	if (FAILED(IUnknown_QueryInterface(sink, &events2_iid, (void **)&dispatch)) && FAILED(IUnknown_QueryInterface(sink, &dispatch_iid, (void **)&dispatch)))
		return CONNECT_E_CANNOTCONNECT;
	for (int i = 0; i < SINKS; i++) {
		if (control->sinks[i])
			continue;
		control->sinks[i] = dispatch;
		*cookie = i + 1;
		return S_OK;
	}
	IDispatch_Release(dispatch);
	return CONNECT_E_ADVISELIMIT;
}

static HRESULT WINAPI point_unadvise(IConnectionPoint *iface, DWORD cookie)
{
	browser *control = from_point(iface);
	if (!cookie || cookie > SINKS || !control->sinks[cookie - 1])
		return CONNECT_E_NOCONNECTION;
	IDispatch *sink = control->sinks[cookie - 1];
	control->sinks[cookie - 1] = NULL;
	IDispatch_Release(sink);
	return S_OK;
}

static HRESULT WINAPI point_enum(IConnectionPoint *iface, IEnumConnections **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static const IConnectionPointVtbl point_vtbl = {
	.QueryInterface = point_query,
	.AddRef = point_add_ref,
	.Release = point_release,
	.GetConnectionInterface = point_interface,
	.GetConnectionPointContainer = point_container,
	.Advise = point_advise,
	.Unadvise = point_unadvise,
	.EnumConnections = point_enum,
};

static HRESULT WINAPI persist_query(IPersistStreamInit *iface, REFIID iid, void **out)
{
	return browser_query(&from_persist(iface)->IWebBrowser2_iface, iid, out);
}

static ULONG WINAPI persist_add_ref(IPersistStreamInit *iface)
{
	return browser_add_ref(&from_persist(iface)->IWebBrowser2_iface);
}

static ULONG WINAPI persist_release(IPersistStreamInit *iface)
{
	return browser_release(&from_persist(iface)->IWebBrowser2_iface);
}

static HRESULT WINAPI persist_class(IPersistStreamInit *iface, CLSID *out)
{
	if (!out)
		return E_POINTER;
	*out = browser_class;
	return S_OK;
}

static HRESULT WINAPI persist_dirty(IPersistStreamInit *iface)
{
	return S_FALSE;
}

/* The control keeps no state worth saving, so loading and saving have nothing to carry. */
static HRESULT WINAPI persist_load(IPersistStreamInit *iface, LPSTREAM stream)
{
	return S_OK;
}

static HRESULT WINAPI persist_save(IPersistStreamInit *iface, LPSTREAM stream, BOOL clear)
{
	return S_OK;
}

static HRESULT WINAPI persist_size(IPersistStreamInit *iface, ULARGE_INTEGER *size)
{
	if (!size)
		return E_POINTER;
	size->QuadPart = 0;
	return S_OK;
}

static HRESULT WINAPI persist_init(IPersistStreamInit *iface)
{
	return S_OK;
}

static const IPersistStreamInitVtbl persist_vtbl = {
	.QueryInterface = persist_query,
	.AddRef = persist_add_ref,
	.Release = persist_release,
	.GetClassID = persist_class,
	.IsDirty = persist_dirty,
	.Load = persist_load,
	.Save = persist_save,
	.GetSizeMax = persist_size,
	.InitNew = persist_init,
};

static HRESULT create_browser(REFIID iid, void **out)
{
	browser *control = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*control));
	if (!control)
		return E_OUTOFMEMORY;
	control->IWebBrowser2_iface.lpVtbl = &browser_vtbl;
	control->IOleObject_iface.lpVtbl = &object_vtbl;
	control->IOleInPlaceObject_iface.lpVtbl = &in_place_vtbl;
	control->IOleInPlaceActiveObject_iface.lpVtbl = &active_vtbl;
	control->IOleControl_iface.lpVtbl = &control_vtbl;
	control->IConnectionPointContainer_iface.lpVtbl = &container_vtbl;
	control->IConnectionPoint_iface.lpVtbl = &point_vtbl;
	control->IPersistStreamInit_iface.lpVtbl = &persist_vtbl;
	control->refs = 1;
	control->visible = VARIANT_TRUE;
	control->register_browser = VARIANT_FALSE;
	control->drop_target = VARIANT_TRUE;
	control->resizable = VARIANT_TRUE;
	InterlockedIncrement(&locks);
	const HRESULT status = browser_query(&control->IWebBrowser2_iface, iid, out);
	browser_release(&control->IWebBrowser2_iface);
	return status;
}

static HRESULT WINAPI factory_query(IClassFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &factory_iid)) {
		*out = iface;
		return S_OK;
	}
	trace_guid("class factory has no", iid);
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_add_ref(IClassFactory *iface)
{
	return 2;
}

static ULONG WINAPI factory_release(IClassFactory *iface)
{
	return 1;
}

static HRESULT WINAPI factory_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return create_browser(iid, out);
}

static HRESULT WINAPI factory_lock(IClassFactory *iface, BOOL lock)
{
	if (lock)
		InterlockedIncrement(&locks);
	else
		InterlockedDecrement(&locks);
	return S_OK;
}

static const IClassFactoryVtbl factory_vtbl = {factory_query, factory_add_ref, factory_release, factory_create, factory_lock};
static IClassFactory browser_factory = {&factory_vtbl};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualCLSID(clsid, &browser_class))
		return CLASS_E_CLASSNOTAVAILABLE;
	return factory_query(&browser_factory, iid, out);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return locks ? S_FALSE : S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		module = instance;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}
