/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "docfile.h"
#include <propidl.h>
#include <string.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	return TRUE;
}

/* Bind contexts: the objects registered for the length of a bind and the named parameters. */

struct bound_object {
	IUnknown *object;
	WCHAR *name;
	struct bound_object *next;
};

typedef struct bind_context {
	IBindCtx IBindCtx_iface;
	LONG refs;
	BIND_OPTS3 options;
	struct bound_object *objects;
} bind_context;

static bind_context *bind_of(IBindCtx *iface)
{
	return CONTAINING_RECORD(iface, bind_context, IBindCtx_iface);
}

static HRESULT WINAPI bind_QueryInterface(IBindCtx *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IBindCtx)) {
		*out = iface;
		IBindCtx_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI bind_AddRef(IBindCtx *iface)
{
	return InterlockedIncrement(&bind_of(iface)->refs);
}

static void remove_object(struct bound_object **link)
{
	struct bound_object *gone = *link;

	*link = gone->next;
	IUnknown_Release(gone->object);
	mem_free(gone->name);
	mem_free(gone);
}

static ULONG WINAPI bind_Release(IBindCtx *iface)
{
	bind_context *object = bind_of(iface);
	ULONG refs = InterlockedDecrement(&object->refs);

	if (!refs) {
		while (object->objects)
			remove_object(&object->objects);
		if (object->options.pServerInfo)
			CoTaskMemFree(object->options.pServerInfo);
		mem_free(object);
	}
	return refs;
}

static HRESULT add_object(bind_context *context, IUnknown *object, const WCHAR *name)
{
	struct bound_object *entry = mem_alloc(sizeof(*entry));

	if (!entry)
		return E_OUTOFMEMORY;
	if (name) {
		SIZE_T bytes = (lstrlenW(name) + 1) * sizeof(WCHAR);
		entry->name = mem_alloc(bytes);
		if (!entry->name) {
			mem_free(entry);
			return E_OUTOFMEMORY;
		}
		memcpy(entry->name, name, bytes);
	}
	IUnknown_AddRef(object);
	entry->object = object;
	entry->next = context->objects;
	context->objects = entry;
	return S_OK;
}

static HRESULT WINAPI bind_RegisterObjectBound(IBindCtx *iface, IUnknown *object)
{
	if (!object)
		return E_INVALIDARG;
	return add_object(bind_of(iface), object, NULL);
}

static HRESULT WINAPI bind_RevokeObjectBound(IBindCtx *iface, IUnknown *object)
{
	struct bound_object **link = &bind_of(iface)->objects;

	if (!object)
		return E_INVALIDARG;
	for (; *link; link = &(*link)->next) {
		if ((*link)->object == object && !(*link)->name) {
			remove_object(link);
			return S_OK;
		}
	}
	return MK_E_NOTBOUND;
}

static HRESULT WINAPI bind_ReleaseBoundObjects(IBindCtx *iface)
{
	struct bound_object **link = &bind_of(iface)->objects;

	while (*link) {
		if (!(*link)->name)
			remove_object(link);
		else
			link = &(*link)->next;
	}
	return S_OK;
}

static HRESULT WINAPI bind_SetBindOptions(IBindCtx *iface, BIND_OPTS *options)
{
	bind_context *object = bind_of(iface);
	DWORD size;

	if (!options)
		return E_INVALIDARG;
	size = options->cbStruct < sizeof(object->options) ? options->cbStruct : sizeof(object->options);
	memcpy(&object->options, options, size);
	object->options.cbStruct = sizeof(object->options);
	return S_OK;
}

static HRESULT WINAPI bind_GetBindOptions(IBindCtx *iface, BIND_OPTS *options)
{
	bind_context *object = bind_of(iface);
	DWORD size, wanted;

	if (!options)
		return E_INVALIDARG;
	wanted = options->cbStruct;
	if (wanted < sizeof(BIND_OPTS))
		return E_INVALIDARG;
	size = wanted < sizeof(object->options) ? wanted : sizeof(object->options);
	memcpy(options, &object->options, size);
	options->cbStruct = size;
	return S_OK;
}

static HRESULT WINAPI bind_GetRunningObjectTable(IBindCtx *iface, IRunningObjectTable **table)
{
	if (!table)
		return E_POINTER;
	return GetRunningObjectTable(0, table);
}

static HRESULT WINAPI bind_RegisterObjectParam(IBindCtx *iface, WCHAR *name, IUnknown *object)
{
	bind_context *context = bind_of(iface);
	struct bound_object **link = &context->objects;

	if (!name || !object)
		return E_INVALIDARG;
	for (; *link; link = &(*link)->next) {
		if ((*link)->name && !lstrcmpW((*link)->name, name)) {
			remove_object(link);
			break;
		}
	}
	return add_object(context, object, name);
}

static HRESULT WINAPI bind_GetObjectParam(IBindCtx *iface, WCHAR *name, IUnknown **object)
{
	struct bound_object *entry;

	if (!object)
		return E_POINTER;
	*object = NULL;
	if (!name)
		return E_INVALIDARG;
	for (entry = bind_of(iface)->objects; entry; entry = entry->next) {
		if (entry->name && !lstrcmpW(entry->name, name)) {
			IUnknown_AddRef(entry->object);
			*object = entry->object;
			return S_OK;
		}
	}
	return E_FAIL;
}

static HRESULT WINAPI bind_EnumObjectParam(IBindCtx *iface, IEnumString **names)
{
	if (names)
		*names = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI bind_RevokeObjectParam(IBindCtx *iface, WCHAR *name)
{
	struct bound_object **link = &bind_of(iface)->objects;

	if (!name)
		return E_INVALIDARG;
	for (; *link; link = &(*link)->next) {
		if ((*link)->name && !lstrcmpW((*link)->name, name)) {
			remove_object(link);
			return S_OK;
		}
	}
	return E_FAIL;
}

static const IBindCtxVtbl bind_vtable = {
	bind_QueryInterface, bind_AddRef, bind_Release, bind_RegisterObjectBound, bind_RevokeObjectBound, bind_ReleaseBoundObjects,
	bind_SetBindOptions, bind_GetBindOptions, bind_GetRunningObjectTable, bind_RegisterObjectParam, bind_GetObjectParam,
	bind_EnumObjectParam, bind_RevokeObjectParam,
};

HRESULT WINAPI CreateBindCtx(DWORD reserved, IBindCtx **out)
{
	bind_context *object;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (reserved)
		return E_INVALIDARG;
	object = mem_alloc(sizeof(*object));
	if (!object)
		return E_OUTOFMEMORY;
	object->IBindCtx_iface.lpVtbl = &bind_vtable;
	object->refs = 1;
	BIND_OPTS3 defaults = { sizeof(defaults) };
	defaults.grfMode = STGM_READWRITE;
	defaults.locale = GetThreadLocale();
	defaults.dwClassContext = CLSCTX_SERVER;
	object->options = defaults;
	*out = &object->IBindCtx_iface;
	return S_OK;
}

HRESULT WINAPI BindMoniker(IMoniker *moniker, DWORD options, REFIID riid, void **out)
{
	IBindCtx *context;
	HRESULT status;

	if (!moniker || !out)
		return E_INVALIDARG;
	*out = NULL;
	status = CreateBindCtx(options, &context);
	if (FAILED(status))
		return status;
	status = IMoniker_BindToObject(moniker, context, NULL, riid, out);
	IBindCtx_Release(context);
	return status;
}

/* The class of a storage and of a stream. */

HRESULT WINAPI ReadClassStg(IStorage *storage, CLSID *clsid)
{
	STATSTG stat;
	HRESULT status;

	if (!storage)
		return E_INVALIDARG;
	if (!clsid)
		return E_INVALIDARG;
	status = IStorage_Stat(storage, &stat, STATFLAG_NONAME);
	if (SUCCEEDED(status))
		*clsid = stat.clsid;
	return status;
}

HRESULT WINAPI WriteClassStg(IStorage *storage, REFCLSID clsid)
{
	if (!storage)
		return E_INVALIDARG;
	if (!clsid)
		return STG_E_INVALIDPOINTER;
	return IStorage_SetClass(storage, clsid);
}

HRESULT WINAPI ReadClassStm(IStream *stream, CLSID *clsid)
{
	ULONG got = 0;
	HRESULT status;

	if (!stream || !clsid)
		return E_INVALIDARG;
	memset(clsid, 0, sizeof(*clsid));
	status = IStream_Read(stream, clsid, sizeof(*clsid), &got);
	if (FAILED(status))
		return status;
	return got == sizeof(*clsid) ? S_OK : STG_E_READFAULT;
}

HRESULT WINAPI WriteClassStm(IStream *stream, REFCLSID clsid)
{
	if (!stream || !clsid)
		return E_INVALIDARG;
	return IStream_Write(stream, clsid, sizeof(*clsid), NULL);
}

HRESULT WINAPI OleSaveToStream(IPersistStream *object, IStream *stream)
{
	CLSID clsid;
	HRESULT status;

	if (!object || !stream)
		return E_INVALIDARG;
	status = IPersistStream_GetClassID(object, &clsid);
	if (FAILED(status))
		return status;
	status = WriteClassStm(stream, &clsid);
	if (FAILED(status))
		return status;
	return IPersistStream_Save(object, stream, TRUE);
}

HRESULT WINAPI OleLoadFromStream(IStream *stream, REFIID riid, void **out)
{
	IPersistStream *persist;
	CLSID clsid;
	HRESULT status;

	if (!stream || !out)
		return E_INVALIDARG;
	*out = NULL;
	status = ReadClassStm(stream, &clsid);
	if (FAILED(status))
		return status;
	status = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, riid, out);
	if (FAILED(status))
		return status;
	status = IUnknown_QueryInterface((IUnknown *)*out, &IID_IPersistStream, (void **)&persist);
	if (SUCCEEDED(status)) {
		status = IPersistStream_Load(persist, stream);
		IPersistStream_Release(persist);
	}
	if (FAILED(status)) {
		IUnknown_Release((IUnknown *)*out);
		*out = NULL;
	}
	return status;
}

/* The \001CompObj stream of [MS-OLEDS]: user type, clipboard format and program id of an object. */

static const WCHAR comp_obj_name[] = L"\001CompObj";
static const WCHAR ole_name[] = L"\001Ole";

static const BYTE comp_obj_header[28] = {
	0x01, 0x00, 0xfe, 0xff, 0x03, 0x0a, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
};

static HRESULT write_counted_ansi(IStream *stream, const char *text)
{
	DWORD length = text ? (DWORD)strlen(text) + 1 : 0;
	HRESULT status = IStream_Write(stream, &length, 4, NULL);

	if (SUCCEEDED(status) && length)
		status = IStream_Write(stream, text, length, NULL);
	return status;
}

static HRESULT write_comp_obj(IStorage *storage, const WCHAR *user_type, const WCHAR *format_name, const char *prog_id)
{
	static const DWORD no_format = 0;
	char user[512] = "", format[256] = "";
	IStream *stream;
	HRESULT status;

	if (user_type)
		WideCharToMultiByte(CP_ACP, 0, user_type, -1, user, sizeof(user), NULL, NULL);
	if (format_name)
		WideCharToMultiByte(CP_ACP, 0, format_name, -1, format, sizeof(format), NULL, NULL);
	status = IStorage_CreateStream(storage, comp_obj_name, STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stream);
	if (FAILED(status))
		return status;
	status = IStream_Write(stream, comp_obj_header, sizeof(comp_obj_header), NULL);
	if (SUCCEEDED(status))
		status = write_counted_ansi(stream, user_type ? user : NULL);
	if (SUCCEEDED(status))
		status = format_name ? write_counted_ansi(stream, format) : IStream_Write(stream, &no_format, 4, NULL);
	if (SUCCEEDED(status))
		status = write_counted_ansi(stream, prog_id);
	IStream_Release(stream);
	return status;
}

static void program_id_of(const CLSID *clsid, char *text, DWORD size)
{
	char path[64];
	HKEY key;

	wsprintfA(path, "CLSID\\{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\\ProgID", clsid->Data1, clsid->Data2, clsid->Data3,
		clsid->Data4[0], clsid->Data4[1], clsid->Data4[2], clsid->Data4[3], clsid->Data4[4], clsid->Data4[5], clsid->Data4[6], clsid->Data4[7]);
	if (RegOpenKeyExA(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &key))
		return;
	if (RegQueryValueExA(key, NULL, NULL, NULL, (BYTE *)text, &size))
		text[0] = 0;
	RegCloseKey(key);
}

HRESULT WINAPI WriteFmtUserTypeStg(IStorage *storage, CLIPFORMAT format, WCHAR *user_type)
{
	WCHAR format_name[128];
	char prog_ansi[128] = "";
	STATSTG stat;
	HRESULT status;

	if (!storage)
		return E_INVALIDARG;
	status = IStorage_Stat(storage, &stat, STATFLAG_NONAME);
	if (FAILED(status))
		return status;
	program_id_of(&stat.clsid, prog_ansi, sizeof(prog_ansi));
	if (format && !GetClipboardFormatNameW(format, format_name, ARRAYSIZE(format_name)))
		format = 0;
	return write_comp_obj(storage, user_type, format ? format_name : NULL, prog_ansi[0] ? prog_ansi : NULL);
}

HRESULT WINAPI ReadFmtUserTypeStg(IStorage *storage, CLIPFORMAT *format, WCHAR **user_type)
{
	BYTE header[28];
	DWORD length = 0;
	ULONG got = 0;
	IStream *stream;
	char *text = NULL;
	HRESULT status;

	if (!storage)
		return E_INVALIDARG;
	if (format)
		*format = 0;
	if (user_type)
		*user_type = NULL;
	status = IStorage_OpenStream(storage, comp_obj_name, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream);
	if (FAILED(status))
		return status;
	status = IStream_Read(stream, header, sizeof(header), &got);
	if (SUCCEEDED(status) && got != sizeof(header))
		status = STG_E_READFAULT;
	if (SUCCEEDED(status))
		status = IStream_Read(stream, &length, 4, &got);
	if (SUCCEEDED(status) && (got != 4 || length > 0x10000))
		status = STG_E_READFAULT;
	if (SUCCEEDED(status) && length) {
		text = mem_alloc(length + 1);
		status = text ? IStream_Read(stream, text, length, &got) : E_OUTOFMEMORY;
	}
	if (SUCCEEDED(status) && user_type && text) {
		int chars = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
		*user_type = CoTaskMemAlloc((SIZE_T)chars * sizeof(WCHAR));
		if (*user_type)
			MultiByteToWideChar(CP_ACP, 0, text, -1, *user_type, chars);
	}
	mem_free(text);
	text = NULL;
	if (SUCCEEDED(status))
		status = IStream_Read(stream, &length, 4, &got);
	if (SUCCEEDED(status) && got == 4 && format && length && length < 0x10000 && length != 0xffffffff) {
		text = mem_alloc(length + 1);
		if (text && SUCCEEDED(IStream_Read(stream, text, length, &got)))
			*format = (CLIPFORMAT)RegisterClipboardFormatA(text);
		mem_free(text);
	}
	IStream_Release(stream);
	return status;
}

/* The convert bit is the second flag of the \001Ole stream. */

#define OLE_STREAM_VERSION 0x02000001
#define OLE_CONVERT_FLAG 4

HRESULT WINAPI GetConvertStg(IStorage *storage)
{
	IStream *stream;
	DWORD header[2] = { 0 };
	ULONG got = 0;
	HRESULT status;

	if (!storage)
		return E_INVALIDARG;
	status = IStorage_OpenStream(storage, ole_name, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream);
	if (FAILED(status))
		return status;
	status = IStream_Read(stream, header, sizeof(header), &got);
	IStream_Release(stream);
	if (FAILED(status))
		return status;
	if (got != sizeof(header) || header[0] != OLE_STREAM_VERSION)
		return E_FAIL;
	return header[1] & OLE_CONVERT_FLAG ? S_OK : S_FALSE;
}

HRESULT WINAPI SetConvertStg(IStorage *storage, BOOL convert)
{
	DWORD data[5] = { OLE_STREAM_VERSION, 0, 0, 0, 0 };
	IStream *stream;
	ULONG got = 0;
	HRESULT status;

	if (!storage)
		return E_INVALIDARG;
	status = IStorage_OpenStream(storage, ole_name, NULL, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &stream);
	if (status == STG_E_FILENOTFOUND) {
		status = IStorage_CreateStream(storage, ole_name, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stream);
	} else if (SUCCEEDED(status)) {
		LARGE_INTEGER origin;
		status = IStream_Read(stream, data, sizeof(data), &got);
		origin.QuadPart = 0;
		IStream_Seek(stream, origin, STREAM_SEEK_SET, NULL);
	}
	if (FAILED(status))
		return status;
	data[1] = convert ? data[1] | OLE_CONVERT_FLAG : data[1] & ~OLE_CONVERT_FLAG;
	status = IStream_Write(stream, data, sizeof(data), NULL);
	IStream_Release(stream);
	return status;
}

/* OLE 1 object conversion and the serialized values of the NT property sets are not offered. */

HRESULT WINAPI OleConvertIStorageToOLESTREAM(IStorage *storage, LPOLESTREAM stream)
{
	return E_NOTIMPL;
}

HRESULT WINAPI OleConvertIStorageToOLESTREAMEx(IStorage *storage, CLIPFORMAT format, LONG width, LONG height, DWORD size, STGMEDIUM *medium, LPOLESTREAM stream)
{
	return E_NOTIMPL;
}

HRESULT WINAPI OleConvertOLESTREAMToIStorage(LPOLESTREAM stream, IStorage *storage, const DVTARGETDEVICE *device)
{
	return E_NOTIMPL;
}

SERIALIZEDPROPERTYVALUE *WINAPI StgConvertVariantToProperty(const PROPVARIANT *value, USHORT code_page, SERIALIZEDPROPERTYVALUE *property, ULONG *size, PROPID id, BOOLEAN array, ULONG *count)
{
	return NULL;
}

BOOLEAN WINAPI StgConvertPropertyToVariant(const SERIALIZEDPROPERTYVALUE *property, USHORT code_page, PROPVARIANT *value, void *allocator)
{
	return FALSE;
}
