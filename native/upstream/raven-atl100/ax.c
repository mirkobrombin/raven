/* SPDX-License-Identifier: MIT */
#include "atl.h"
#include <wchar.h>

/* The control host. A window gets a host when a control is attached to it; the host answers as the control's site, keeps the
 * control the size of the window, and goes away with the window. It lives in a window property so the window's own data stays
 * free for its owner. */

static const WCHAR host_property[] = L"RavenAtlAxHost";
static const IID iid_host_window = {0xb6ea2050, 0x048a, 0x11d1, {0x82, 0xb9, 0x00, 0xc0, 0x4f, 0xb9, 0x94, 0x2e}};

typedef struct host host;
typedef struct
{
	HRESULT (WINAPI *QueryInterface)(host *, REFIID, void **);
	ULONG (WINAPI *AddRef)(host *);
	ULONG (WINAPI *Release)(host *);
	HRESULT (WINAPI *CreateControl)(host *, const WCHAR *, HWND, IStream *);
	HRESULT (WINAPI *CreateControlEx)(host *, const WCHAR *, HWND, IStream *, IUnknown **, REFIID, IUnknown *);
	HRESULT (WINAPI *AttachControl)(host *, IUnknown *, HWND);
	HRESULT (WINAPI *QueryControl)(host *, REFIID, void **);
	HRESULT (WINAPI *SetExternalDispatch)(host *, IDispatch *);
	HRESULT (WINAPI *SetExternalUIHandler)(host *, IUnknown *);
} host_vtbl_type;

struct host
{
	const host_vtbl_type *vtbl;
	IOleClientSite client_site;
	IOleContainer container;
	IOleInPlaceSite in_place_site;
	IOleInPlaceFrame frame;
	IOleControlSite control_site;
	LONG refs;
	HWND window;
	WNDPROC previous_procedure;
	IUnknown *control;
	IOleObject *ole_object;
	IOleInPlaceObject *in_place_object;
	IUnknown *sink;
	IID sink_iid;
	DWORD sink_cookie;
};

#define HOST(pointer, member) CONTAINING_RECORD(pointer, host, member)

static HRESULT WINAPI host_QueryInterface(host *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &iid_host_window))
		*out = self;
	else if (IsEqualGUID(riid, &IID_IOleClientSite))
		*out = &self->client_site;
	else if (IsEqualGUID(riid, &IID_IOleContainer) || IsEqualGUID(riid, &IID_IParseDisplayName))
		*out = &self->container;
	else if (IsEqualGUID(riid, &IID_IOleInPlaceSite) || IsEqualGUID(riid, &IID_IOleWindow))
		*out = &self->in_place_site;
	else if (IsEqualGUID(riid, &IID_IOleInPlaceFrame) || IsEqualGUID(riid, &IID_IOleInPlaceUIWindow))
		*out = &self->frame;
	else if (IsEqualGUID(riid, &IID_IOleControlSite))
		*out = &self->control_site;
	else
	{
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG WINAPI host_AddRef(host *self)
{
	return InterlockedIncrement(&self->refs);
}

static void release_control(host *self)
{
	if (self->sink)
	{
		extern HRESULT WINAPI AtlUnadvise(IUnknown *, const IID *, DWORD);
		AtlUnadvise(self->control, &self->sink_iid, self->sink_cookie);
		IUnknown_Release(self->sink);
		self->sink = NULL;
	}
	if (self->in_place_object)
	{
		IOleInPlaceObject_InPlaceDeactivate(self->in_place_object);
		IOleInPlaceObject_Release(self->in_place_object);
		self->in_place_object = NULL;
	}
	if (self->ole_object)
	{
		IOleObject_Close(self->ole_object, OLECLOSE_NOSAVE);
		IOleObject_SetClientSite(self->ole_object, NULL);
		IOleObject_Release(self->ole_object);
		self->ole_object = NULL;
	}
	if (self->control)
	{
		IUnknown_Release(self->control);
		self->control = NULL;
	}
}

static ULONG WINAPI host_Release(host *self)
{
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		release_control(self);
		atl_free(self);
	}
	return left;
}

/* Sites forward what they have in common to the host. */
#define SITE_UNKNOWN(prefix, type, member) \
	static HRESULT WINAPI prefix##_QueryInterface(type *self, REFIID riid, void **out) \
	{ \
		return host_QueryInterface(HOST(self, member), riid, out); \
	} \
	static ULONG WINAPI prefix##_AddRef(type *self) \
	{ \
		return host_AddRef(HOST(self, member)); \
	} \
	static ULONG WINAPI prefix##_Release(type *self) \
	{ \
		return host_Release(HOST(self, member)); \
	}

SITE_UNKNOWN(client, IOleClientSite, client_site)
SITE_UNKNOWN(container, IOleContainer, container)
SITE_UNKNOWN(place, IOleInPlaceSite, in_place_site)
SITE_UNKNOWN(frame, IOleInPlaceFrame, frame)
SITE_UNKNOWN(control, IOleControlSite, control_site)

static HRESULT WINAPI client_SaveObject(IOleClientSite *self)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI client_GetMoniker(IOleClientSite *self, DWORD assign, DWORD which, IMoniker **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI client_GetContainer(IOleClientSite *self, IOleContainer **out)
{
	if (!out)
		return E_POINTER;
	*out = &HOST(self, client_site)->container;
	IOleContainer_AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI client_ShowObject(IOleClientSite *self)
{
	return S_OK;
}

static HRESULT WINAPI client_OnShowWindow(IOleClientSite *self, BOOL show)
{
	return S_OK;
}

static HRESULT WINAPI client_RequestNewObjectLayout(IOleClientSite *self)
{
	return E_NOTIMPL;
}

static const IOleClientSiteVtbl client_vtbl = {
	client_QueryInterface,
	client_AddRef,
	client_Release,
	client_SaveObject,
	client_GetMoniker,
	client_GetContainer,
	client_ShowObject,
	client_OnShowWindow,
	client_RequestNewObjectLayout,
};

static HRESULT WINAPI container_ParseDisplayName(IOleContainer *self, IBindCtx *context, LPOLESTR name, ULONG *eaten, IMoniker **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI container_EnumObjects(IOleContainer *self, DWORD flags, IEnumUnknown **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI container_LockContainer(IOleContainer *self, BOOL lock)
{
	return S_OK;
}

static const IOleContainerVtbl container_vtbl = {
	container_QueryInterface,
	container_AddRef,
	container_Release,
	container_ParseDisplayName,
	container_EnumObjects,
	container_LockContainer,
};

static HRESULT WINAPI place_GetWindow(IOleInPlaceSite *self, HWND *out)
{
	if (!out)
		return E_POINTER;
	*out = HOST(self, in_place_site)->window;
	return S_OK;
}

static HRESULT WINAPI place_ContextSensitiveHelp(IOleInPlaceSite *self, BOOL enter)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI place_CanInPlaceActivate(IOleInPlaceSite *self)
{
	return HOST(self, in_place_site)->window ? S_OK : S_FALSE;
}

static HRESULT WINAPI place_OnInPlaceActivate(IOleInPlaceSite *self)
{
	host *owner = HOST(self, in_place_site);
	if (!owner->in_place_object && owner->control)
		IUnknown_QueryInterface(owner->control, &IID_IOleInPlaceObject, (void **)&owner->in_place_object);
	return S_OK;
}

static HRESULT WINAPI place_OnUIActivate(IOleInPlaceSite *self)
{
	return S_OK;
}

static HRESULT WINAPI place_GetWindowContext(IOleInPlaceSite *self, IOleInPlaceFrame **frame, IOleInPlaceUIWindow **document, RECT *position, RECT *clip, OLEINPLACEFRAMEINFO *info)
{
	host *owner = HOST(self, in_place_site);
	if (!frame || !document || !position || !clip || !info)
		return E_POINTER;
	*frame = &owner->frame;
	IOleInPlaceFrame_AddRef(*frame);
	*document = NULL;
	GetClientRect(owner->window, position);
	*clip = *position;
	info->fMDIApp = FALSE;
	info->hwndFrame = owner->window;
	info->haccel = NULL;
	info->cAccelEntries = 0;
	return S_OK;
}

static HRESULT WINAPI place_Scroll(IOleInPlaceSite *self, SIZE extent)
{
	return S_FALSE;
}

static HRESULT WINAPI place_OnUIDeactivate(IOleInPlaceSite *self, BOOL undoable)
{
	return S_OK;
}

static HRESULT WINAPI place_OnInPlaceDeactivate(IOleInPlaceSite *self)
{
	return S_OK;
}

static HRESULT WINAPI place_DiscardUndoState(IOleInPlaceSite *self)
{
	return S_OK;
}

static HRESULT WINAPI place_DeactivateAndUndo(IOleInPlaceSite *self)
{
	return S_OK;
}

static HRESULT WINAPI place_OnPosRectChange(IOleInPlaceSite *self, const RECT *position)
{
	host *owner = HOST(self, in_place_site);
	if (owner->in_place_object)
		IOleInPlaceObject_SetObjectRects(owner->in_place_object, position, position);
	return S_OK;
}

static const IOleInPlaceSiteVtbl place_vtbl = {
	place_QueryInterface,
	place_AddRef,
	place_Release,
	place_GetWindow,
	place_ContextSensitiveHelp,
	place_CanInPlaceActivate,
	place_OnInPlaceActivate,
	place_OnUIActivate,
	place_GetWindowContext,
	place_Scroll,
	place_OnUIDeactivate,
	place_OnInPlaceDeactivate,
	place_DiscardUndoState,
	place_DeactivateAndUndo,
	place_OnPosRectChange,
};

static HRESULT WINAPI frame_GetWindow(IOleInPlaceFrame *self, HWND *out)
{
	if (!out)
		return E_POINTER;
	*out = HOST(self, frame)->window;
	return S_OK;
}

static HRESULT WINAPI frame_ContextSensitiveHelp(IOleInPlaceFrame *self, BOOL enter)
{
	return E_NOTIMPL;
}

/* The host has no toolbars or menus to negotiate; asking for border space is answered plainly. */
static HRESULT WINAPI frame_GetBorder(IOleInPlaceFrame *self, RECT *out)
{
	return INPLACE_E_NOTOOLSPACE;
}

static HRESULT WINAPI frame_RequestBorderSpace(IOleInPlaceFrame *self, const BORDERWIDTHS *widths)
{
	return INPLACE_E_NOTOOLSPACE;
}

static HRESULT WINAPI frame_SetBorderSpace(IOleInPlaceFrame *self, const BORDERWIDTHS *widths)
{
	return S_OK;
}

static HRESULT WINAPI frame_SetActiveObject(IOleInPlaceFrame *self, IOleInPlaceActiveObject *object, LPCOLESTR name)
{
	return S_OK;
}

static HRESULT WINAPI frame_InsertMenus(IOleInPlaceFrame *self, HMENU menu, OLEMENUGROUPWIDTHS *widths)
{
	return S_OK;
}

static HRESULT WINAPI frame_SetMenu(IOleInPlaceFrame *self, HMENU menu, HOLEMENU descriptor, HWND window)
{
	return S_OK;
}

static HRESULT WINAPI frame_RemoveMenus(IOleInPlaceFrame *self, HMENU menu)
{
	return S_OK;
}

static HRESULT WINAPI frame_SetStatusText(IOleInPlaceFrame *self, LPCOLESTR text)
{
	return S_OK;
}

static HRESULT WINAPI frame_EnableModeless(IOleInPlaceFrame *self, BOOL enable)
{
	return S_OK;
}

static HRESULT WINAPI frame_TranslateAccelerator(IOleInPlaceFrame *self, MSG *message, WORD id)
{
	return S_FALSE;
}

static const IOleInPlaceFrameVtbl frame_vtbl = {
	frame_QueryInterface,
	frame_AddRef,
	frame_Release,
	frame_GetWindow,
	frame_ContextSensitiveHelp,
	frame_GetBorder,
	frame_RequestBorderSpace,
	frame_SetBorderSpace,
	frame_SetActiveObject,
	frame_InsertMenus,
	frame_SetMenu,
	frame_RemoveMenus,
	frame_SetStatusText,
	frame_EnableModeless,
	frame_TranslateAccelerator,
};

static HRESULT WINAPI control_OnControlInfoChanged(IOleControlSite *self)
{
	return S_OK;
}

static HRESULT WINAPI control_LockInPlaceActive(IOleControlSite *self, BOOL lock)
{
	return S_OK;
}

static HRESULT WINAPI control_GetExtendedControl(IOleControlSite *self, IDispatch **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI control_TransformCoords(IOleControlSite *self, POINTL *metric, POINTF *container, DWORD flags)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI control_TranslateAccelerator(IOleControlSite *self, MSG *message, DWORD modifiers)
{
	return S_FALSE;
}

static HRESULT WINAPI control_OnFocus(IOleControlSite *self, BOOL got)
{
	return S_OK;
}

static HRESULT WINAPI control_ShowPropertyFrame(IOleControlSite *self)
{
	return E_NOTIMPL;
}

static const IOleControlSiteVtbl control_vtbl = {
	control_QueryInterface,
	control_AddRef,
	control_Release,
	control_OnControlInfoChanged,
	control_LockInPlaceActive,
	control_GetExtendedControl,
	control_TransformCoords,
	control_TranslateAccelerator,
	control_OnFocus,
	control_ShowPropertyFrame,
};

/* The window's procedure while it has a host */
static LRESULT CALLBACK hosted_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	host *self = GetPropW(window, host_property);
	WNDPROC previous;
	if (!self)
		return DefWindowProcW(window, message, wparam, lparam);
	previous = self->previous_procedure;
	if (message == WM_SIZE && self->in_place_object)
	{
		RECT area = {0, 0, LOWORD(lparam), HIWORD(lparam)};
		IOleInPlaceObject_SetObjectRects(self->in_place_object, &area, &area);
	}
	else if (message == WM_NCDESTROY)
	{
		release_control(self);
		RemovePropW(window, host_property);
		SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)previous);
		self->window = NULL;
		host_Release(self);
	}
	return CallWindowProcW(previous, window, message, wparam, lparam);
}

static void bind_window(host *self, HWND window)
{
	if (!window || self->window)
		return;
	self->window = window;
	SetPropW(window, host_property, self);
	host_AddRef(self);
	self->previous_procedure = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)hosted_window);
}

static HRESULT activate(host *self, IUnknown *control)
{
	HRESULT status;
	IPersistStreamInit *persist;
	release_control(self);
	self->control = control;
	IUnknown_AddRef(control);
	if (SUCCEEDED(IUnknown_QueryInterface(control, &IID_IOleObject, (void **)&self->ole_object)))
		IOleObject_SetClientSite(self->ole_object, &self->client_site);
	if (SUCCEEDED(IUnknown_QueryInterface(control, &IID_IPersistStreamInit, (void **)&persist)))
	{
		IPersistStreamInit_InitNew(persist);
		IPersistStreamInit_Release(persist);
	}
	if (!self->ole_object || !self->window)
		return self->window ? S_OK : S_FALSE;
	{
		RECT area;
		SIZEL metric;
		GetClientRect(self->window, &area);
		metric.cx = MulDiv(area.right, 2540, 96);
		metric.cy = MulDiv(area.bottom, 2540, 96);
		IOleObject_SetExtent(self->ole_object, DVASPECT_CONTENT, &metric);
		status = IOleObject_DoVerb(self->ole_object, OLEIVERB_INPLACEACTIVATE, NULL, &self->client_site, 0, self->window, &area);
	}
	return FAILED(status) && status != E_NOTIMPL ? status : S_OK;
}

static BOOL starts_with(const WCHAR *text, const WCHAR *prefix)
{
	size_t size = wcslen(prefix);
	return wcslen(text) >= size && !_wcsnicmp(text, prefix, size);
}

/* What the text names: a class by identifier or program name, or a page to open in the web browser class. */
static HRESULT resolve_name(const WCHAR *name, CLSID *clsid, BOOL *page)
{
	*page = FALSE;
	if (starts_with(name, L"mshtml:") || wcsstr(name, L"://") || (name[0] && name[1] == ':'))
	{
		*page = TRUE;
		*clsid = CLSID_WebBrowser;
		return S_OK;
	}
	if (name[0] == '{' && SUCCEEDED(CLSIDFromString(name, clsid)))
		return S_OK;
	return FAILED(CLSIDFromProgID(name, clsid)) ? CO_E_CLASSSTRING : S_OK;
}

static HRESULT create_named(const WCHAR *name, const WCHAR *license, IUnknown **out)
{
	CLSID clsid;
	BOOL page;
	HRESULT status = resolve_name(name, &clsid, &page);
	if (FAILED(status))
		return status;
	if (license)
	{
		IClassFactory2 *factory;
		if (SUCCEEDED(CoGetClassObject(&clsid, CLSCTX_INPROC_SERVER, NULL, &IID_IClassFactory2, (void **)&factory)))
		{
			BSTR key = SysAllocString(license);
			status = IClassFactory2_CreateInstanceLic(factory, NULL, NULL, &IID_IUnknown, key, (void **)out);
			SysFreeString(key);
			IClassFactory2_Release(factory);
			return status;
		}
	}
	status = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, &IID_IUnknown, (void **)out);
	if (FAILED(status) || !page)
		return status;
	return S_OK;
}

static void open_page(IUnknown *control, const WCHAR *name)
{
	IWebBrowser2 *browser;
	BSTR target;
	VARIANT empty;
	if (FAILED(IUnknown_QueryInterface(control, &IID_IWebBrowser2, (void **)&browser)))
		return;
	target = SysAllocString(starts_with(name, L"mshtml:") ? L"about:blank" : name);
	VariantInit(&empty);
	IWebBrowser2_Navigate(browser, target, &empty, &empty, &empty, &empty);
	SysFreeString(target);
	IWebBrowser2_Release(browser);
}

static HRESULT WINAPI host_CreateControlEx(host *self, const WCHAR *name, HWND window, IStream *stream, IUnknown **out, REFIID sink_iid, IUnknown *sink);

static HRESULT host_create_licensed(host *self, const WCHAR *name, HWND window, IStream *stream, IUnknown **out, REFIID sink_iid, IUnknown *sink, const WCHAR *license)
{
	IUnknown *control = NULL;
	CLSID clsid;
	BOOL page = FALSE;
	HRESULT status;
	if (out)
		*out = NULL;
	bind_window(self, window);
	if (!name || !*name)
		return S_OK;
	if (FAILED(status = create_named(name, license, &control)))
		return status;
	status = activate(self, control);
	if (SUCCEEDED(resolve_name(name, &clsid, &page)) && page)
		open_page(control, name);
	if (SUCCEEDED(status) && sink && sink_iid && !IsEqualGUID(sink_iid, &IID_NULL))
	{
		extern HRESULT WINAPI AtlAdvise(IUnknown *, IUnknown *, const IID *, DWORD *);
		if (SUCCEEDED(AtlAdvise(control, sink, sink_iid, &self->sink_cookie)))
		{
			self->sink = sink;
			self->sink_iid = *sink_iid;
			IUnknown_AddRef(sink);
		}
	}
	if (out)
	{
		*out = control;
		IUnknown_AddRef(control);
	}
	IUnknown_Release(control);
	return status;
}

static HRESULT WINAPI host_CreateControlEx(host *self, const WCHAR *name, HWND window, IStream *stream, IUnknown **out, REFIID sink_iid, IUnknown *sink)
{
	return host_create_licensed(self, name, window, stream, out, sink_iid, sink, NULL);
}

static HRESULT WINAPI host_CreateControl(host *self, const WCHAR *name, HWND window, IStream *stream)
{
	return host_create_licensed(self, name, window, stream, NULL, NULL, NULL, NULL);
}

static HRESULT WINAPI host_AttachControl(host *self, IUnknown *control, HWND window)
{
	if (!control)
		return E_INVALIDARG;
	bind_window(self, window);
	return activate(self, control);
}

static HRESULT WINAPI host_QueryControl(host *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->control ? IUnknown_QueryInterface(self->control, riid, out) : E_NOINTERFACE;
}

static HRESULT WINAPI host_SetExternalDispatch(host *self, IDispatch *dispatch)
{
	return S_OK;
}

static HRESULT WINAPI host_SetExternalUIHandler(host *self, IUnknown *handler)
{
	return S_OK;
}

static const host_vtbl_type host_vtbl = {
	host_QueryInterface,
	host_AddRef,
	host_Release,
	host_CreateControl,
	host_CreateControlEx,
	host_AttachControl,
	host_QueryControl,
	host_SetExternalDispatch,
	host_SetExternalUIHandler,
};

static host *host_new(void)
{
	host *self = atl_alloc(sizeof(*self));
	if (!self)
		return NULL;
	memset(self, 0, sizeof(*self));
	self->vtbl = &host_vtbl;
	self->client_site.lpVtbl = (IOleClientSiteVtbl *)&client_vtbl;
	self->container.lpVtbl = (IOleContainerVtbl *)&container_vtbl;
	self->in_place_site.lpVtbl = (IOleInPlaceSiteVtbl *)&place_vtbl;
	self->frame.lpVtbl = (IOleInPlaceFrameVtbl *)&frame_vtbl;
	self->control_site.lpVtbl = (IOleControlSiteVtbl *)&control_vtbl;
	self->refs = 1;
	return self;
}

HRESULT WINAPI AtlAxCreateControlLicEx(const WCHAR *name, HWND window, IStream *stream, IUnknown **container, IUnknown **control, REFIID sink_iid, IUnknown *sink, const WCHAR *license)
{
	host *self = host_new();
	HRESULT status;
	if (container)
		*container = NULL;
	if (control)
		*control = NULL;
	if (!self)
		return E_OUTOFMEMORY;
	status = host_create_licensed(self, name, window, stream, control, sink_iid, sink, license);
	if (FAILED(status))
	{
		if (self->window)
			DestroyWindow(self->window);
		host_Release(self);
		return status;
	}
	if (container)
		*container = (IUnknown *)self;
	else
		host_Release(self);
	return status;
}

HRESULT WINAPI AtlAxCreateControlEx(const WCHAR *name, HWND window, IStream *stream, IUnknown **container, IUnknown **control, REFIID sink_iid, IUnknown *sink)
{
	return AtlAxCreateControlLicEx(name, window, stream, container, control, sink_iid, sink, NULL);
}

HRESULT WINAPI AtlAxCreateControlLic(const WCHAR *name, HWND window, IStream *stream, IUnknown **container, const WCHAR *license)
{
	return AtlAxCreateControlLicEx(name, window, stream, container, NULL, NULL, NULL, license);
}

HRESULT WINAPI AtlAxCreateControl(const WCHAR *name, HWND window, IStream *stream, IUnknown **container)
{
	return AtlAxCreateControlLicEx(name, window, stream, container, NULL, NULL, NULL, NULL);
}

HRESULT WINAPI AtlAxAttachControl(IUnknown *control, HWND window, IUnknown **container)
{
	host *self;
	HRESULT status;
	if (!control)
		return E_INVALIDARG;
	if (!(self = host_new()))
		return E_OUTOFMEMORY;
	status = host_AttachControl(self, control, window);
	if (SUCCEEDED(status) && !window)
		status = S_FALSE;
	if (FAILED(status))
	{
		host_Release(self);
		return status;
	}
	if (container)
		*container = (IUnknown *)self;
	else
		host_Release(self);
	return status;
}

HRESULT WINAPI AtlAxGetHost(HWND window, IUnknown **out)
{
	host *self = GetPropW(window, host_property);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!self)
		return E_FAIL;
	return host_QueryInterface(self, &IID_IUnknown, (void **)out);
}

HRESULT WINAPI AtlAxGetControl(HWND window, IUnknown **out)
{
	host *self = GetPropW(window, host_property);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!self || !self->control)
		return E_FAIL;
	*out = self->control;
	IUnknown_AddRef(*out);
	return S_OK;
}

/* The window classes: the text of the window names the control. */
static LRESULT CALLBACK ax_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_CREATE)
	{
		WCHAR name[1024];
		name[0] = 0;
		GetWindowTextW(window, name, ARRAYSIZE(name));
		if (name[0] && FAILED(AtlAxCreateControlLicEx(name, window, NULL, NULL, NULL, NULL, NULL, NULL)))
			return -1;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

BOOL WINAPI AtlAxWinInit(void)
{
	static const WCHAR *const names[] = {L"AtlAxWin100", L"AtlAxWinLic100"};
	UINT index;
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	for (index = 0; index < ARRAYSIZE(names); index++)
	{
		WNDCLASSEXW info;
		memset(&info, 0, sizeof(info));
		info.cbSize = sizeof(info);
		info.style = CS_GLOBALCLASS | CS_DBLCLKS;
		info.lpfnWndProc = ax_window;
		info.lpszClassName = names[index];
		if (!RegisterClassExW(&info) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return FALSE;
	}
	return TRUE;
}

/* A dialog's controls come from registered classes; one that names a control by identifier in its template does not build here. */
INT_PTR WINAPI AtlAxDialogBoxW(HINSTANCE instance, const WCHAR *name, HWND parent, DLGPROC procedure, LPARAM parameter)
{
	AtlAxWinInit();
	return DialogBoxParamW(instance, name, parent, procedure, parameter);
}

INT_PTR WINAPI AtlAxDialogBoxA(HINSTANCE instance, const char *name, HWND parent, DLGPROC procedure, LPARAM parameter)
{
	AtlAxWinInit();
	return DialogBoxParamA(instance, name, parent, procedure, parameter);
}

HWND WINAPI AtlAxCreateDialogW(HINSTANCE instance, const WCHAR *name, HWND parent, DLGPROC procedure, LPARAM parameter)
{
	AtlAxWinInit();
	return CreateDialogParamW(instance, name, parent, procedure, parameter);
}

HWND WINAPI AtlAxCreateDialogA(HINSTANCE instance, const char *name, HWND parent, DLGPROC procedure, LPARAM parameter)
{
	AtlAxWinInit();
	return CreateDialogParamA(instance, name, parent, procedure, parameter);
}
