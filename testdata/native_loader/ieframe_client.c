// The WebBrowser control as a host embeds it: created, sited, activated in place in the host's window, then sent to an address. The
// host's event sink hears BeforeNavigate2 at once and NavigateComplete2 and DocumentComplete from the message loop; a sink that
// cancels keeps the control where it was.
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <olectl.h>
#include <exdisp.h>
#include <exdispid.h>

static const CLSID browser_class = {0x8856f961, 0x340a, 0x11d0, {0xa9, 0x6b, 0x00, 0xc0, 0x4f, 0xd7, 0x05, 0xa2}};
static const IID unknown_iid = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID dispatch_iid = {0x00020400, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID web_browser2_iid = {0xd30c1661, 0xcdaf, 0x11d0, {0x8a, 0x3e, 0x00, 0xc0, 0x4f, 0xc9, 0xe2, 0x6e}};
static const IID ole_object_iid = {0x00000112, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID client_site_iid = {0x00000118, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID in_place_object_iid = {0x00000113, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID in_place_active_iid = {0x00000117, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID in_place_site_iid = {0x00000119, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID ole_control_iid = {0xb196b288, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}};
static const IID point_container_iid = {0xb196b284, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}};
static const IID events2_iid = {0x34a715a0, 0x6587, 0x11d0, {0x92, 0x4a, 0x00, 0x20, 0xaf, 0xc7, 0xac, 0x4d}};

static HWND host;
static int activations, deactivations, ui_activations;
static DISPID heard[16];
static int heard_count;
static IDispatch *sender;
static int wrong_sender;

static HRESULT WINAPI site_query(IOleClientSite *iface, REFIID iid, void **out);

static ULONG WINAPI site_add_ref(IOleClientSite *iface)
{
	return 2;
}

static ULONG WINAPI site_release(IOleClientSite *iface)
{
	return 1;
}

static HRESULT WINAPI site_save(IOleClientSite *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI site_moniker(IOleClientSite *iface, DWORD assign, DWORD which, IMoniker **out)
{
	*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI site_container(IOleClientSite *iface, IOleContainer **out)
{
	*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT WINAPI site_show(IOleClientSite *iface)
{
	return S_OK;
}

static HRESULT WINAPI site_on_show(IOleClientSite *iface, BOOL show)
{
	return S_OK;
}

static HRESULT WINAPI site_layout(IOleClientSite *iface)
{
	return E_NOTIMPL;
}

static IOleClientSiteVtbl site_vtbl = {
	.QueryInterface = site_query,
	.AddRef = site_add_ref,
	.Release = site_release,
	.SaveObject = site_save,
	.GetMoniker = site_moniker,
	.GetContainer = site_container,
	.ShowObject = site_show,
	.OnShowWindow = site_on_show,
	.RequestNewObjectLayout = site_layout,
};
static IOleClientSite site = {&site_vtbl};

static HRESULT WINAPI place_query(IOleInPlaceSite *iface, REFIID iid, void **out)
{
	return site_query(&site, iid, out);
}

static ULONG WINAPI place_add_ref(IOleInPlaceSite *iface)
{
	return 2;
}

static ULONG WINAPI place_release(IOleInPlaceSite *iface)
{
	return 1;
}

static HRESULT WINAPI place_window(IOleInPlaceSite *iface, HWND *out)
{
	*out = host;
	return S_OK;
}

static HRESULT WINAPI place_help(IOleInPlaceSite *iface, BOOL enter)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI place_can_activate(IOleInPlaceSite *iface)
{
	return S_OK;
}

static HRESULT WINAPI place_on_activate(IOleInPlaceSite *iface)
{
	activations++;
	return S_OK;
}

static HRESULT WINAPI place_on_ui_activate(IOleInPlaceSite *iface)
{
	ui_activations++;
	return S_OK;
}

static HRESULT WINAPI place_context(IOleInPlaceSite *iface, IOleInPlaceFrame **frame, IOleInPlaceUIWindow **document, LPRECT position, LPRECT clip,
	LPOLEINPLACEFRAMEINFO info)
{
	*frame = NULL;
	*document = NULL;
	SetRect(position, 10, 20, 210, 170);
	SetRect(clip, 10, 20, 210, 170);
	return S_OK;
}

static HRESULT WINAPI place_scroll(IOleInPlaceSite *iface, SIZE extent)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI place_on_ui_deactivate(IOleInPlaceSite *iface, BOOL undoable)
{
	return S_OK;
}

static HRESULT WINAPI place_on_deactivate(IOleInPlaceSite *iface)
{
	deactivations++;
	return S_OK;
}

static HRESULT WINAPI place_discard(IOleInPlaceSite *iface)
{
	return S_OK;
}

static HRESULT WINAPI place_undo(IOleInPlaceSite *iface)
{
	return S_OK;
}

static HRESULT WINAPI place_moved(IOleInPlaceSite *iface, LPCRECT position)
{
	return S_OK;
}

static IOleInPlaceSiteVtbl place_vtbl = {
	.QueryInterface = place_query,
	.AddRef = place_add_ref,
	.Release = place_release,
	.GetWindow = place_window,
	.ContextSensitiveHelp = place_help,
	.CanInPlaceActivate = place_can_activate,
	.OnInPlaceActivate = place_on_activate,
	.OnUIActivate = place_on_ui_activate,
	.GetWindowContext = place_context,
	.Scroll = place_scroll,
	.OnUIDeactivate = place_on_ui_deactivate,
	.OnInPlaceDeactivate = place_on_deactivate,
	.DiscardUndoState = place_discard,
	.DeactivateAndUndo = place_undo,
	.OnPosRectChange = place_moved,
};
static IOleInPlaceSite place = {&place_vtbl};

static HRESULT WINAPI site_query(IOleClientSite *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &client_site_iid))
		*out = &site;
	else if (IsEqualIID(iid, &in_place_site_iid))
		*out = &place;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	return S_OK;
}

static HRESULT WINAPI sink_query(IDispatch *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &dispatch_iid) || IsEqualIID(iid, &events2_iid)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI sink_add_ref(IDispatch *iface)
{
	return 2;
}

static ULONG WINAPI sink_release(IDispatch *iface)
{
	return 1;
}

static HRESULT WINAPI sink_count(IDispatch *iface, UINT *count)
{
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI sink_info(IDispatch *iface, UINT index, LCID locale, ITypeInfo **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI sink_names(IDispatch *iface, REFIID iid, LPOLESTR *names, UINT count, LCID locale, DISPID *ids)
{
	return E_NOTIMPL;
}

// Every event names the control first; BeforeNavigate2 to "about:cancel" is turned down.
static HRESULT WINAPI sink_invoke(IDispatch *iface, DISPID member, REFIID iid, LCID locale, WORD flags, DISPPARAMS *parameters, VARIANT *result,
	EXCEPINFO *exception, UINT *argument)
{
	if (heard_count < 16)
		heard[heard_count++] = member;
	VARIANT *first = &parameters->rgvarg[parameters->cArgs - 1];
	if (V_VT(first) != VT_DISPATCH || V_DISPATCH(first) != sender)
		wrong_sender++;
	if (member == DISPID_BEFORENAVIGATE2) {
		VARIANT *url = V_VARIANTREF(&parameters->rgvarg[5]);
		if (parameters->cArgs != 7 || V_VT(&parameters->rgvarg[0]) != (VT_BYREF | VT_BOOL) || V_VT(url) != VT_BSTR)
			wrong_sender++;
		else if (!lstrcmpW(V_BSTR(url), L"about:cancel"))
			*V_BOOLREF(&parameters->rgvarg[0]) = VARIANT_TRUE;
	}
	return S_OK;
}

static IDispatchVtbl sink_vtbl = {
	.QueryInterface = sink_query,
	.AddRef = sink_add_ref,
	.Release = sink_release,
	.GetTypeInfoCount = sink_count,
	.GetTypeInfo = sink_info,
	.GetIDsOfNames = sink_names,
	.Invoke = sink_invoke,
};
static IDispatch sink = {&sink_vtbl};

static void pump(void)
{
	MSG message;
	for (int round = 0; round < 50; round++)
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
}

static int location_is(IWebBrowser2 *browser, const wchar_t *wanted)
{
	BSTR location = NULL;
	if (IWebBrowser2_get_LocationURL(browser, &location) || !location)
		return 0;
	const int same = !lstrcmpW(location, wanted);
	SysFreeString(location);
	return same;
}

static int embed(void)
{
	WNDCLASSW description = {0};
	description.lpfnWndProc = DefWindowProcW;
	description.hInstance = GetModuleHandleW(NULL);
	description.lpszClassName = L"browser host";
	if (!RegisterClassW(&description))
		return 10;
	host = CreateWindowExW(0, L"browser host", L"host", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, description.hInstance, NULL);
	if (!host)
		return 10;

	IWebBrowser2 *browser = NULL;
	if (CoCreateInstance(&browser_class, NULL, CLSCTX_INPROC_SERVER, &web_browser2_iid, (void **)&browser) || !browser)
		return 11;
	// The interfaces a host takes the control apart into.
	IOleObject *object = NULL;
	IOleInPlaceObject *in_place = NULL;
	IUnknown *other = NULL;
	IConnectionPointContainer *container = NULL;
	if (IWebBrowser2_QueryInterface(browser, &ole_object_iid, (void **)&object) || IWebBrowser2_QueryInterface(browser, &in_place_object_iid, (void **)&in_place))
		return 12;
	if (IWebBrowser2_QueryInterface(browser, &in_place_active_iid, (void **)&other))
		return 12;
	IUnknown_Release(other);
	if (IWebBrowser2_QueryInterface(browser, &ole_control_iid, (void **)&other))
		return 12;
	IUnknown_Release(other);
	if (IWebBrowser2_QueryInterface(browser, &point_container_iid, (void **)&container))
		return 12;
	IUnknown *identity = NULL, *again = NULL;
	IWebBrowser2_QueryInterface(browser, &unknown_iid, (void **)&identity);
	IOleObject_QueryInterface(object, &unknown_iid, (void **)&again);
	if (!identity || identity != again)
		return 13;
	IUnknown_Release(identity);
	IUnknown_Release(again);
	IWebBrowser2_QueryInterface(browser, &dispatch_iid, (void **)&sender);
	IDispatch_Release(sender);

	IConnectionPoint *point = NULL;
	DWORD cookie = 0;
	if (IConnectionPointContainer_FindConnectionPoint(container, &events2_iid, &point) || IConnectionPoint_Advise(point, (IUnknown *)&sink, &cookie) || !cookie)
		return 14;

	// In place in the host's window, at the place the site gives.
	RECT position;
	SetRect(&position, 10, 20, 210, 170);
	if (IOleObject_SetClientSite(object, &site) || IOleObject_DoVerb(object, OLEIVERB_INPLACEACTIVATE, NULL, &site, 0, host, &position) || activations != 1)
		return 20;
	HWND window = NULL;
	RECT placed;
	if (IOleInPlaceObject_GetWindow(in_place, &window) || !window || GetParent(window) != host || !GetWindowRect(window, &placed))
		return 21;
	MapWindowPoints(NULL, host, (POINT *)&placed, 2);
	if (placed.left != 10 || placed.top != 20 || placed.right - placed.left != 200 || placed.bottom - placed.top != 150)
		return 22;

	// The start comes at once, the end from the message loop.
	BSTR address = SysAllocString(L"about:blank");
	READYSTATE state = READYSTATE_UNINITIALIZED;
	if (IWebBrowser2_Navigate(browser, address, NULL, NULL, NULL, NULL) || heard_count != 1 || heard[0] != DISPID_BEFORENAVIGATE2)
		return 30;
	if (IWebBrowser2_get_ReadyState(browser, &state) || state != READYSTATE_LOADING)
		return 31;
	pump();
	if (heard_count != 3 || heard[1] != DISPID_NAVIGATECOMPLETE2 || heard[2] != DISPID_DOCUMENTCOMPLETE || wrong_sender)
		return 32;
	if (IWebBrowser2_get_ReadyState(browser, &state) || state != READYSTATE_COMPLETE || !location_is(browser, L"about:blank"))
		return 33;
	SysFreeString(address);

	// A cancelled navigation leaves the control where it was.
	VARIANT target;
	V_VT(&target) = VT_BSTR;
	V_BSTR(&target) = SysAllocString(L"about:cancel");
	if (IWebBrowser2_Navigate2(browser, &target, NULL, NULL, NULL, NULL))
		return 34;
	pump();
	VariantClear(&target);
	if (heard_count != 4 || heard[3] != DISPID_BEFORENAVIGATE2 || !location_is(browser, L"about:blank"))
		return 35;

	SetRect(&position, 0, 0, 300, 100);
	if (IOleInPlaceObject_SetObjectRects(in_place, &position, &position) || !GetWindowRect(window, &placed) || placed.right - placed.left != 300)
		return 40;
	if (IOleObject_DoVerb(object, OLEIVERB_UIACTIVATE, NULL, &site, 0, host, &position) || ui_activations != 1)
		return 41;
	if (IOleInPlaceObject_InPlaceDeactivate(in_place) || deactivations != 1 || IsWindow(window))
		return 42;
	if (IConnectionPoint_Unadvise(point, cookie) || IConnectionPoint_Unadvise(point, cookie) != CONNECT_E_NOCONNECTION)
		return 43;
	IConnectionPoint_Release(point);
	IConnectionPointContainer_Release(container);
	IOleInPlaceObject_Release(in_place);
	IOleObject_SetClientSite(object, NULL);
	IOleObject_Release(object);
	IWebBrowser2_Release(browser);
	DestroyWindow(host);
	return 0;
}

int raven_entry(void)
{
	if (FAILED(OleInitialize(NULL)))
		return 1;
	const int result = embed();
	OleUninitialize();
	return result;
}
