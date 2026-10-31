/* SPDX-License-Identifier: MIT */
#include "atl.h"
#include <wchar.h>

static HINSTANCE this_module;
static LONG locks;
BOOL atl_per_user;

void *atl_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), 0, size);
}

void atl_free(void *block)
{
	HeapFree(GetProcessHeap(), 0, block);
}

/* Unimplemented exports of the public list end here: they stop the caller rather than answer with a guess. */
void atl_unavailable(void)
{
	RaiseException(0xC0000002, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

HRESULT WINAPI AtlAdvise(IUnknown *container, IUnknown *sink, const IID *iid, DWORD *cookie)
{
	IConnectionPointContainer *points;
	IConnectionPoint *point;
	HRESULT status;
	if (!container)
		return E_INVALIDARG;
	status = IUnknown_QueryInterface(container, &IID_IConnectionPointContainer, (void **)&points);
	if (FAILED(status))
		return status;
	status = IConnectionPointContainer_FindConnectionPoint(points, iid, &point);
	IConnectionPointContainer_Release(points);
	if (FAILED(status))
		return status;
	status = IConnectionPoint_Advise(point, sink, cookie);
	IConnectionPoint_Release(point);
	return status;
}

HRESULT WINAPI AtlUnadvise(IUnknown *container, const IID *iid, DWORD cookie)
{
	IConnectionPointContainer *points;
	IConnectionPoint *point;
	HRESULT status;
	if (!container)
		return E_INVALIDARG;
	status = IUnknown_QueryInterface(container, &IID_IConnectionPointContainer, (void **)&points);
	if (FAILED(status))
		return status;
	status = IConnectionPointContainer_FindConnectionPoint(points, iid, &point);
	IConnectionPointContainer_Release(points);
	if (FAILED(status))
		return status;
	status = IConnectionPoint_Unadvise(point, cookie);
	IConnectionPoint_Release(point);
	return status;
}

HRESULT WINAPI AtlFreeMarshalStream(IStream *stream)
{
	if (stream)
	{
		CoReleaseMarshalData(stream);
		IStream_Release(stream);
	}
	return S_OK;
}

HRESULT WINAPI AtlMarshalPtrInProc(IUnknown *object, const IID *iid, IStream **out)
{
	LARGE_INTEGER start;
	HRESULT status = CreateStreamOnHGlobal(NULL, TRUE, out);
	if (FAILED(status))
		return status;
	status = CoMarshalInterface(*out, iid, object, MSHCTX_INPROC, NULL, MSHLFLAGS_TABLESTRONG);
	if (FAILED(status))
	{
		IStream_Release(*out);
		*out = NULL;
		return status;
	}
	start.QuadPart = 0;
	return IStream_Seek(*out, start, STREAM_SEEK_SET, NULL);
}

HRESULT WINAPI AtlUnmarshalPtr(IStream *stream, const IID *iid, IUnknown **out)
{
	return CoUnmarshalInterface(stream, iid, (void **)out);
}

HRESULT WINAPI AtlComModuleGetClassObject(atl_com_module *module, REFCLSID clsid, REFIID riid, void **out)
{
	atl_object_entry **entry;
	if (!module)
		return E_INVALIDARG;
	for (entry = module->first; entry && entry < module->last; entry++)
	{
		atl_object_entry *object = *entry;
		HRESULT status = S_OK;
		if (!object || !IsEqualGUID(object->clsid, clsid) || !object->get_class_object)
			continue;
		if (!object->factory)
		{
			EnterCriticalSection(&module->lock);
			if (!object->factory)
				status = object->get_class_object(object->create_instance, &IID_IUnknown, (void **)&object->factory);
			LeaveCriticalSection(&module->lock);
		}
		if (FAILED(status))
			return status;
		if (object->factory)
			return IUnknown_QueryInterface(object->factory, riid, out);
	}
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI AtlComModuleRegisterClassObjects(atl_com_module *module, DWORD context, DWORD flags)
{
	atl_object_entry **entry;
	HRESULT status = S_FALSE;
	if (!module)
		return E_INVALIDARG;
	for (entry = module->first; entry && entry < module->last; entry++)
	{
		atl_object_entry *object = *entry;
		IUnknown *factory;
		if (!object || !object->get_class_object)
			continue;
		status = object->get_class_object(object->create_instance, &IID_IUnknown, (void **)&factory);
		if (FAILED(status))
			return status;
		status = CoRegisterClassObject(object->clsid, factory, context, flags, &object->registered);
		IUnknown_Release(factory);
		if (FAILED(status))
			return status;
	}
	return status;
}

HRESULT WINAPI AtlComModuleRevokeClassObjects(atl_com_module *module)
{
	atl_object_entry **entry;
	if (!module)
		return E_INVALIDARG;
	for (entry = module->first; entry && entry < module->last; entry++)
		if (*entry && (*entry)->registered)
		{
			HRESULT status = CoRevokeClassObject((*entry)->registered);
			if (FAILED(status))
				return status;
			(*entry)->registered = 0;
		}
	return S_OK;
}

static void key_path(WCHAR *path, const GUID *clsid, const WCHAR *group, const GUID *category)
{
	WCHAR guid[40];
	lstrcpyW(path, L"CLSID\\");
	StringFromGUID2(clsid, guid, ARRAYSIZE(guid));
	lstrcatW(path, guid);
	if (group)
	{
		lstrcatW(path, L"\\");
		lstrcatW(path, group);
	}
	if (category)
	{
		lstrcatW(path, L"\\");
		StringFromGUID2(category, guid, ARRAYSIZE(guid));
		lstrcatW(path, guid);
	}
}

HRESULT WINAPI AtlRegisterClassCategoriesHelper(REFCLSID clsid, const atl_category_entry *map, BOOL registering)
{
	static const WCHAR *const groups[2] = {L"Implemented Categories", L"Required Categories"};
	const atl_category_entry *entry;
	WCHAR path[256];
	for (entry = map; entry && entry->kind; entry++)
	{
		const WCHAR *group = groups[entry->kind == 2];
		key_path(path, clsid, group, entry->category);
		if (registering)
		{
			HKEY key;
			LONG error = RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
			if (error)
				return HRESULT_FROM_WIN32(error);
			RegCloseKey(key);
		}
		else
		{
			RegDeleteKeyW(HKEY_CLASSES_ROOT, path);
			/* Only the last category takes the group with it; a non-empty key refuses to go. */
			key_path(path, clsid, group, NULL);
			RegDeleteKeyW(HKEY_CLASSES_ROOT, path);
		}
	}
	return S_OK;
}

HRESULT WINAPI AtlComModuleUnregisterServer(atl_com_module *module, BOOL type_library, const CLSID *clsid)
{
	atl_object_entry **entry;
	if (!module)
		return E_INVALIDARG;
	for (entry = module->first; entry && entry < module->last; entry++)
	{
		atl_object_entry *object = *entry;
		HRESULT status;
		if (!object || (clsid && !IsEqualGUID(object->clsid, clsid)))
			continue;
		if (object->categories && FAILED(status = AtlRegisterClassCategoriesHelper(object->clsid, object->categories(), FALSE)))
			return status;
		if (FAILED(status = object->update_registry(FALSE)))
			return status;
	}
	if (type_library)
	{
		WCHAR path[MAX_PATH];
		ITypeLib *library;
		if (GetModuleFileNameW(module->type_library_module, path, ARRAYSIZE(path)) && SUCCEEDED(LoadTypeLib(path, &library)))
		{
			TLIBATTR *attributes;
			if (SUCCEEDED(ITypeLib_GetLibAttr(library, &attributes)))
			{
				UnRegisterTypeLib(&attributes->guid, attributes->wMajorVerNum, attributes->wMinorVerNum, attributes->lcid, attributes->syskind);
				ITypeLib_ReleaseTLibAttr(library, attributes);
			}
			ITypeLib_Release(library);
		}
	}
	return S_OK;
}

BOOL WINAPI AtlWaitWithMessageLoop(HANDLE handle)
{
	for (;;)
	{
		DWORD result = MsgWaitForMultipleObjects(1, &handle, FALSE, INFINITE, QS_ALLINPUT);
		MSG message;
		if (result == WAIT_OBJECT_0)
			return TRUE;
		if (result != WAIT_OBJECT_0 + 1)
			return FALSE;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
}

HDC WINAPI AtlCreateTargetDC(HDC reference, DVTARGETDEVICE *target)
{
	const WCHAR *driver = L"DISPLAY", *device = NULL, *port = NULL;
	const DEVMODEW *mode = NULL;
	if (target)
	{
		const BYTE *start = (const BYTE *)target;
		driver = (const WCHAR *)(start + target->tdDriverNameOffset);
		if (target->tdDeviceNameOffset)
			device = (const WCHAR *)(start + target->tdDeviceNameOffset);
		if (target->tdPortNameOffset)
			port = (const WCHAR *)(start + target->tdPortNameOffset);
		if (target->tdExtDevmodeOffset)
			mode = (const DEVMODEW *)(start + target->tdExtDevmodeOffset);
	}
	return reference ? CreateICW(driver, device, port, mode) : CreateDCW(driver, device, port, mode);
}

static void scale_size(const SIZEL *from, SIZEL *to, BOOL to_pixels)
{
	HDC screen = GetDC(NULL);
	int x = GetDeviceCaps(screen, LOGPIXELSX), y = GetDeviceCaps(screen, LOGPIXELSY);
	ReleaseDC(NULL, screen);
	to->cx = to_pixels ? MulDiv(from->cx, x, 2540) : MulDiv(from->cx, 2540, x);
	to->cy = to_pixels ? MulDiv(from->cy, y, 2540) : MulDiv(from->cy, 2540, y);
}

void WINAPI AtlHiMetricToPixel(const SIZEL *metric, SIZEL *pixels)
{
	scale_size(metric, pixels, TRUE);
}

void WINAPI AtlPixelToHiMetric(const SIZEL *pixels, SIZEL *metric)
{
	scale_size(pixels, metric, FALSE);
}

IUnknown *WINAPI AtlComPtrAssign(IUnknown **slot, IUnknown *value)
{
	if (value)
		IUnknown_AddRef(value);
	if (*slot)
		IUnknown_Release(*slot);
	*slot = value;
	return value;
}

IUnknown *WINAPI AtlComQIPtrAssign(IUnknown **slot, IUnknown *value, REFIID riid)
{
	IUnknown *previous = *slot;
	*slot = NULL;
	if (value)
		IUnknown_QueryInterface(value, riid, (void **)slot);
	if (previous)
		IUnknown_Release(previous);
	return *slot;
}

HRESULT WINAPI AtlInternalQueryInterface(void *self, const atl_interface_entry *entries, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown))
	{
		*out = (BYTE *)self + entries->data;
		IUnknown_AddRef((IUnknown *)*out);
		return S_OK;
	}
	for (; entries->function; entries++)
	{
		if (entries->iid && !IsEqualGUID(entries->iid, riid))
			continue;
		if (entries->function == (atl_creator_arg *)1)
		{
			*out = (BYTE *)self + entries->data;
			IUnknown_AddRef((IUnknown *)*out);
			return S_OK;
		}
		{
			HRESULT status = entries->function(self, riid, out, entries->data);
			if (status == S_OK)
				return status;
			if (FAILED(status) && entries->iid)
				return status;
		}
	}
	return E_NOINTERFACE;
}

DWORD WINAPI AtlGetVersion(void *reserved)
{
	return 0x0a00;
}

void WINAPI AtlWinModuleAddCreateWndData(atl_win_module *module, atl_create_data *data, void *object)
{
	data->object = object;
	data->thread = GetCurrentThreadId();
	EnterCriticalSection(&module->lock);
	data->next = module->list;
	module->list = data;
	LeaveCriticalSection(&module->lock);
}

void *WINAPI AtlWinModuleExtractCreateWndData(atl_win_module *module)
{
	atl_create_data **link;
	void *object = NULL;
	EnterCriticalSection(&module->lock);
	for (link = &module->list; *link; link = &(*link)->next)
		if ((*link)->thread == GetCurrentThreadId())
		{
			object = (*link)->object;
			*link = (*link)->next;
			break;
		}
	LeaveCriticalSection(&module->lock);
	return object;
}

HRESULT WINAPI AtlWinModuleInit(atl_win_module *module)
{
	if (!module)
		return E_INVALIDARG;
	if (module->size != sizeof(*module))
		return E_INVALIDARG;
	InitializeCriticalSection(&module->lock);
	module->list = NULL;
	return S_OK;
}

/* Saving and loading a control's property map needs the map's own conventions, which nothing here has been asked to follow yet. */
HRESULT WINAPI AtlIPersistStreamInit_Load(IStream *stream, const void *map, void *object, IUnknown *unknown)
{
	return E_NOTIMPL;
}

HRESULT WINAPI AtlIPersistStreamInit_Save(IStream *stream, BOOL clear, const void *map, void *object, IUnknown *unknown)
{
	return E_NOTIMPL;
}

HRESULT WINAPI AtlIPersistPropertyBag_Load(IPropertyBag *bag, IErrorLog *log, const void *map, void *object, IUnknown *unknown)
{
	return E_NOTIMPL;
}

HRESULT WINAPI AtlIPersistPropertyBag_Save(IPropertyBag *bag, BOOL clear, BOOL all, const void *map, void *object, IUnknown *unknown)
{
	return E_NOTIMPL;
}

/* The default source interface of a type's class, marked as both source and default among what it implements. */
static HRESULT source_of_class(ITypeLib *library, REFCLSID clsid, IID *iid)
{
	ITypeInfo *info, *other;
	TYPEATTR *attributes;
	UINT index;
	HRESULT status;
	*iid = IID_NULL;
	if (FAILED(ITypeLib_GetTypeInfoOfGuid(library, clsid, &info)))
		return S_OK;
	if (FAILED(status = ITypeInfo_GetTypeAttr(info, &attributes)))
	{
		ITypeInfo_Release(info);
		return status;
	}
	for (index = 0; index < attributes->cImplTypes; index++)
	{
		INT flags;
		HREFTYPE reference;
		TYPEATTR *found;
		if (FAILED(ITypeInfo_GetImplTypeFlags(info, index, &flags)) || (flags & (IMPLTYPEFLAG_FSOURCE | IMPLTYPEFLAG_FDEFAULT)) != (IMPLTYPEFLAG_FSOURCE | IMPLTYPEFLAG_FDEFAULT))
			continue;
		if (FAILED(ITypeInfo_GetRefTypeOfImplType(info, index, &reference)) || FAILED(ITypeInfo_GetRefTypeInfo(info, reference, &other)))
			continue;
		if (SUCCEEDED(ITypeInfo_GetTypeAttr(other, &found)))
		{
			*iid = found->guid;
			ITypeInfo_ReleaseTypeAttr(other, found);
		}
		ITypeInfo_Release(other);
		break;
	}
	ITypeInfo_ReleaseTypeAttr(info, attributes);
	ITypeInfo_Release(info);
	return S_OK;
}

HRESULT WINAPI AtlGetObjectSourceInterface(IUnknown *object, GUID *library_id, IID *iid, WORD *major, WORD *minor)
{
	IDispatch *dispatch;
	ITypeInfo *info;
	ITypeLib *library;
	TLIBATTR *attributes;
	IProvideClassInfo2 *provider;
	IPersist *persist;
	CLSID clsid;
	UINT index;
	HRESULT status;
	if (!object)
		return E_INVALIDARG;
	if (FAILED(status = IUnknown_QueryInterface(object, &IID_IDispatch, (void **)&dispatch)))
		return status;
	status = IDispatch_GetTypeInfo(dispatch, 0, 0, &info);
	IDispatch_Release(dispatch);
	if (FAILED(status))
		return status;
	status = ITypeInfo_GetContainingTypeLib(info, &library, &index);
	ITypeInfo_Release(info);
	if (FAILED(status))
		return status;
	if (FAILED(status = ITypeLib_GetLibAttr(library, &attributes)))
	{
		ITypeLib_Release(library);
		return status;
	}
	*library_id = attributes->guid;
	*major = attributes->wMajorVerNum;
	*minor = attributes->wMinorVerNum;
	ITypeLib_ReleaseTLibAttr(library, attributes);
	*iid = IID_NULL;
	if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IProvideClassInfo2, (void **)&provider)))
	{
		status = IProvideClassInfo2_GetGUID(provider, GUIDKIND_DEFAULT_SOURCE_DISP_IID, iid);
		IProvideClassInfo2_Release(provider);
	}
	else if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IPersist, (void **)&persist)))
	{
		status = IPersist_GetClassID(persist, &clsid);
		IPersist_Release(persist);
		if (SUCCEEDED(status))
			status = source_of_class(library, &clsid, iid);
	}
	ITypeLib_Release(library);
	return status;
}

HRESULT WINAPI AtlLoadTypeLib(HINSTANCE module, const WCHAR *index, BSTR *path, ITypeLib **out)
{
	WCHAR name[MAX_PATH + 16];
	HRESULT status;
	DWORD size;
	if (!path || !out)
		return E_INVALIDARG;
	*path = NULL;
	*out = NULL;
	if (!(size = GetModuleFileNameW(module, name, MAX_PATH)))
		return HRESULT_FROM_WIN32(GetLastError());
	if (index)
		lstrcatW(name, index);
	status = LoadTypeLib(name, out);
	if (FAILED(status) && !index)
	{
		/* The library may sit beside the module as a file of its own. */
		WCHAR *dot = wcsrchr(name, '.');
		if (dot)
		{
			lstrcpyW(dot, L".tlb");
			status = LoadTypeLib(name, out);
		}
	}
	if (SUCCEEDED(status) && !(*path = SysAllocString(name)))
	{
		ITypeLib_Release(*out);
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	return status;
}

HRESULT WINAPI AtlModuleAddTermFunc(atl_module *module, void (WINAPI *function)(DWORD_PTR), DWORD_PTR argument)
{
	atl_term_entry *entry = atl_alloc(sizeof(*entry));
	if (!entry)
		return E_OUTOFMEMORY;
	entry->function = function;
	entry->argument = argument;
	EnterCriticalSection(&module->lock);
	entry->next = module->terms;
	module->terms = entry;
	LeaveCriticalSection(&module->lock);
	return S_OK;
}

void WINAPI AtlCallTermFunc(atl_module *module)
{
	while (module->terms)
	{
		atl_term_entry *entry = module->terms;
		module->terms = entry->next;
		entry->function(entry->argument);
		atl_free(entry);
	}
}

HRESULT WINAPI AtlSetPerUserRegistration(BOOL value)
{
	atl_per_user = value;
	return S_OK;
}

HRESULT WINAPI AtlGetPerUserRegistration(BYTE *out)
{
	if (!out)
		return E_POINTER;
	*out = atl_per_user != 0;
	return S_OK;
}

/* The class object: only the registrar is ever asked for by class. */

static HRESULT WINAPI factory_QueryInterface(IClassFactory *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IClassFactory))
	{
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IClassFactory *self)
{
	return 2;
}

static ULONG WINAPI factory_Release(IClassFactory *self)
{
	return 1;
}

static HRESULT WINAPI factory_CreateInstance(IClassFactory *self, IUnknown *outer, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return atl_registrar_create(riid, out);
}

static HRESULT WINAPI factory_LockServer(IClassFactory *self, BOOL lock)
{
	if (lock)
		InterlockedIncrement(&locks);
	else
		InterlockedDecrement(&locks);
	return S_OK;
}

static const IClassFactoryVtbl factory_vtbl = {
	factory_QueryInterface,
	factory_AddRef,
	factory_Release,
	factory_CreateInstance,
	factory_LockServer,
};

static IClassFactory factory = {&factory_vtbl};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	if (IsEqualGUID(clsid, &CLSID_Registrar))
		return factory_QueryInterface(&factory, riid, out);
	if (out)
		*out = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return locks ? S_FALSE : S_OK;
}

HRESULT WINAPI DllRegisterServer(void)
{
	WCHAR module[MAX_PATH], path[96];
	HKEY key, server;
	GetModuleFileNameW(this_module, module, ARRAYSIZE(module));
	key_path(path, &CLSID_Registrar, NULL, NULL);
	if (RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
		return E_FAIL;
	RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)L"Registrar Class", sizeof(L"Registrar Class"));
	if (!RegCreateKeyExW(key, L"InProcServer32", 0, NULL, 0, KEY_WRITE, NULL, &server, NULL))
	{
		RegSetValueExW(server, NULL, 0, REG_SZ, (const BYTE *)module, (DWORD)(wcslen(module) + 1) * sizeof(WCHAR));
		RegSetValueExW(server, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Both", sizeof(L"Both"));
		RegCloseKey(server);
	}
	RegCloseKey(key);
	return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	WCHAR path[96];
	key_path(path, &CLSID_Registrar, NULL, NULL);
	return atl_delete_tree(HKEY_CLASSES_ROOT, path);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		this_module = instance;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}
