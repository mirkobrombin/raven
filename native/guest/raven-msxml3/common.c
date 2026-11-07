/* SPDX-License-Identifier: MIT */
#include "mx.h"

void *mx_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), 0, size);
}

void *mx_zalloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void *mx_realloc(void *block, SIZE_T size)
{
	return block ? HeapReAlloc(GetProcessHeap(), 0, block, size) : HeapAlloc(GetProcessHeap(), 0, size);
}

void mx_free(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

xmlChar *mx_to_utf8_counted(const WCHAR *text, int length)
{
	int size;
	xmlChar *out;
	if (!text)
		return NULL;
	size = length ? WideCharToMultiByte(CP_UTF8, 0, text, length, NULL, 0, NULL, NULL) : 0;
	out = xmlMalloc(size + 1);
	if (!out)
		return NULL;
	if (size)
		WideCharToMultiByte(CP_UTF8, 0, text, length, (char *)out, size, NULL, NULL);
	out[size] = 0;
	return out;
}

xmlChar *mx_to_utf8(const WCHAR *text)
{
	return text ? mx_to_utf8_counted(text, lstrlenW(text)) : NULL;
}

WCHAR *mx_to_wide(const xmlChar *text)
{
	int size;
	WCHAR *out;
	if (!text)
		return NULL;
	size = MultiByteToWideChar(CP_UTF8, 0, (const char *)text, -1, NULL, 0);
	out = mx_alloc(size * sizeof(WCHAR));
	if (out)
		MultiByteToWideChar(CP_UTF8, 0, (const char *)text, -1, out, size);
	return out;
}

BSTR mx_bstr_counted(const xmlChar *text, int bytes)
{
	int size;
	BSTR out;
	if (!text || !bytes)
		return SysAllocStringLen(NULL, 0);
	size = MultiByteToWideChar(CP_UTF8, 0, (const char *)text, bytes, NULL, 0);
	out = SysAllocStringLen(NULL, size);
	if (out)
		MultiByteToWideChar(CP_UTF8, 0, (const char *)text, bytes, out, size);
	return out;
}

BSTR mx_bstr(const xmlChar *text)
{
	return text ? mx_bstr_counted(text, xmlStrlen(text)) : NULL;
}

HRESULT mx_return_string(const xmlChar *text, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!text)
		return S_FALSE;
	*out = mx_bstr(text);
	return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT mx_return_null_variant(VARIANT *out)
{
	V_VT(out) = VT_NULL;
	return S_FALSE;
}

BOOL mx_is_blank(const xmlChar *text)
{
	for (; text && *text; text++)
		if (*text != ' ' && *text != '\t' && *text != '\r' && *text != '\n')
			return FALSE;
	return TRUE;
}

/* Any value a caller hands in as text; a null reads as nothing at all. */
HRESULT mx_variant_text(const VARIANT *from, VARIANT *text)
{
	VariantInit(text);
	return VariantChangeTypeEx(text, (VARIANT *)from, MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT), 0, VT_BSTR);
}

/* --- Type information, one table entry per face the library shows to scripts. --- */

extern const GUID LIBID_MSXML2_own;

static const GUID mx_iid_xml_document2 = {0x2b8de2fe, 0x8d2d, 0x11d1, {0xb2, 0xfc, 0x00, 0xc0, 0x4f, 0xd9, 0x15, 0xa9}};
static const GUID mx_iid_xml_element2 = {0x2b8de2ff, 0x8d2d, 0x11d1, {0xb2, 0xfc, 0x00, 0xc0, 0x4f, 0xd9, 0x15, 0xa9}};

static const GUID mx_iid_xml_attribute = {0xd4d4a0fc, 0x3b73, 0x11d1, {0xb2, 0xb4, 0x00, 0xc0, 0x4f, 0xb9, 0x25, 0x96}};

static const IID *const type_iids[MX_TYPE_COUNT] = {
	[MX_TYPE_NODE] = &IID_IXMLDOMNode,
	[MX_TYPE_DOCUMENT] = &IID_IXMLDOMDocument,
	[MX_TYPE_DOCUMENT2] = &IID_IXMLDOMDocument2,
	[MX_TYPE_DOCUMENT3] = &IID_IXMLDOMDocument3,
	[MX_TYPE_ELEMENT] = &IID_IXMLDOMElement,
	[MX_TYPE_ATTRIBUTE] = &IID_IXMLDOMAttribute,
	[MX_TYPE_TEXT] = &IID_IXMLDOMText,
	[MX_TYPE_CDATA] = &IID_IXMLDOMCDATASection,
	[MX_TYPE_COMMENT] = &IID_IXMLDOMComment,
	[MX_TYPE_PI] = &IID_IXMLDOMProcessingInstruction,
	[MX_TYPE_FRAGMENT] = &IID_IXMLDOMDocumentFragment,
	[MX_TYPE_DOCTYPE] = &IID_IXMLDOMDocumentType,
	[MX_TYPE_ENTITY_REF] = &IID_IXMLDOMEntityReference,
	[MX_TYPE_ENTITY] = &IID_IXMLDOMEntity,
	[MX_TYPE_NOTATION] = &IID_IXMLDOMNotation,
	[MX_TYPE_NODE_LIST] = &IID_IXMLDOMNodeList,
	[MX_TYPE_NODE_MAP] = &IID_IXMLDOMNamedNodeMap,
	[MX_TYPE_SELECTION] = &IID_IXMLDOMSelection,
	[MX_TYPE_PARSE_ERROR] = &IID_IXMLDOMParseError,
	[MX_TYPE_IMPLEMENTATION] = &IID_IXMLDOMImplementation,
	[MX_TYPE_SCHEMA_CACHE] = &IID_IXMLDOMSchemaCollection2,
	[MX_TYPE_TEMPLATE] = &IID_IXSLTemplate,
	[MX_TYPE_PROCESSOR] = &IID_IXSLProcessor,
	[MX_TYPE_HTTP] = &IID_IXMLHTTPRequest,
	[MX_TYPE_SERVER_HTTP] = &IID_IServerXMLHTTPRequest,
	[MX_TYPE_WRITER] = &IID_IMXWriter,
	[MX_TYPE_ATTRIBUTES] = &IID_IMXAttributes,
	[MX_TYPE_VB_READER] = &IID_IVBSAXXMLReader,
	[MX_TYPE_NS_MANAGER] = &IID_IVBMXNamespaceManager,
	[MX_TYPE_NS_PREFIXES] = &IID_IMXNamespacePrefixes,
	[MX_TYPE_VB_ATTRIBUTES] = &IID_IVBSAXAttributes,
	[MX_TYPE_VB_LOCATOR] = &IID_IVBSAXLocator,
	[MX_TYPE_XML_DOCUMENT] = &mx_iid_xml_document2,
	[MX_TYPE_XML_ELEMENT] = &mx_iid_xml_element2,
	[MX_TYPE_XML_COLLECTION] = &IID_IXMLElementCollection,
	[MX_TYPE_XML_ATTRIBUTE] = &mx_iid_xml_attribute,
};

static ITypeLib *type_library;
static ITypeInfo *type_infos[MX_TYPE_COUNT];

HRESULT mx_type_info(mx_type type, ITypeInfo **out)
{
	HRESULT status;
	if (!type_library)
	{
		WCHAR path[MAX_PATH];
		ITypeLib *library;
		if (!GetModuleFileNameW(mx_instance, path, MAX_PATH))
			return E_FAIL;
		status = LoadTypeLibEx(path, REGKIND_NONE, &library);
		if (FAILED(status))
			return status;
		if (InterlockedCompareExchangePointer((void **)&type_library, library, NULL))
			ITypeLib_Release(library);
	}
	if (!type_infos[type])
	{
		ITypeInfo *info;
		status = ITypeLib_GetTypeInfoOfGuid(type_library, type_iids[type], &info);
		if (FAILED(status))
			return status;
		if (InterlockedCompareExchangePointer((void **)&type_infos[type], info, NULL))
			ITypeInfo_Release(info);
	}
	*out = type_infos[type];
	ITypeInfo_AddRef(*out);
	return S_OK;
}

void mx_release_type_infos(void)
{
	unsigned index;
	for (index = 0; index < MX_TYPE_COUNT; index++)
		if (type_infos[index])
		{
			ITypeInfo_Release(type_infos[index]);
			type_infos[index] = NULL;
		}
	if (type_library)
	{
		ITypeLib_Release(type_library);
		type_library = NULL;
	}
}

HRESULT mx_get_ids(mx_type type, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	ITypeInfo *info;
	HRESULT status;
	if (!names || !count || !ids)
		return E_INVALIDARG;
	status = mx_type_info(type, &info);
	if (FAILED(status))
		return status;
	status = ITypeInfo_GetIDsOfNames(info, names, count, ids);
	ITypeInfo_Release(info);
	return status;
}

HRESULT mx_invoke(mx_type type, void *object, DISPID id, REFIID riid, LCID lcid, WORD flags,
	DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	ITypeInfo *info;
	HRESULT status = mx_type_info(type, &info);
	if (FAILED(status))
		return status;
	status = ITypeInfo_Invoke(info, object, id, flags, params, result, excep, arg_err);
	ITypeInfo_Release(info);
	return status;
}

/* The answer about error information is an object of its own, handed out new each time and not tied to who gave it. */
typedef struct
{
	const ISupportErrorInfoVtbl *vtbl;
	LONG refs;
	const GUID *first;
	const GUID *const *more;
} mx_error_support;

static HRESULT WINAPI error_support_QueryInterface(ISupportErrorInfo *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_ISupportErrorInfo))
		return E_NOINTERFACE;
	*out = iface;
	ISupportErrorInfo_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI error_support_AddRef(ISupportErrorInfo *iface)
{
	return InterlockedIncrement(&((mx_error_support *)iface)->refs);
}

static ULONG WINAPI error_support_Release(ISupportErrorInfo *iface)
{
	LONG refs = InterlockedDecrement(&((mx_error_support *)iface)->refs);
	if (!refs)
		mx_free(iface);
	return refs;
}

static HRESULT WINAPI error_support_InterfaceSupportsErrorInfo(ISupportErrorInfo *iface, REFIID riid)
{
	mx_error_support *self = (mx_error_support *)iface;
	const GUID *const *at;
	if (self->first && IsEqualGUID(riid, self->first))
		return S_OK;
	for (at = self->more; at && *at; at++)
		if (IsEqualGUID(riid, *at))
			return S_OK;
	return S_FALSE;
}

static const ISupportErrorInfoVtbl error_support_table = {
	error_support_QueryInterface, error_support_AddRef, error_support_Release, error_support_InterfaceSupportsErrorInfo,
};

HRESULT mx_error_support_create(const GUID *first, const GUID *const *more, ISupportErrorInfo **out)
{
	mx_error_support *self = mx_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &error_support_table;
	self->refs = 1;
	self->first = first;
	self->more = more;
	*out = (ISupportErrorInfo *)self;
	return S_OK;
}

static mx_dispex *dispex_from(IDispatchEx *iface)
{
	return (mx_dispex *)iface;
}

static HRESULT WINAPI dispex_QueryInterface(IDispatchEx *iface, REFIID riid, void **out)
{
	return IDispatch_QueryInterface(dispex_from(iface)->outer, riid, out);
}

static ULONG WINAPI dispex_AddRef(IDispatchEx *iface)
{
	return IDispatch_AddRef(dispex_from(iface)->outer);
}

static ULONG WINAPI dispex_Release(IDispatchEx *iface)
{
	return IDispatch_Release(dispex_from(iface)->outer);
}

static HRESULT WINAPI dispex_GetTypeInfoCount(IDispatchEx *iface, UINT *out)
{
	return IDispatch_GetTypeInfoCount(dispex_from(iface)->outer, out);
}

static HRESULT WINAPI dispex_GetTypeInfo(IDispatchEx *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	return IDispatch_GetTypeInfo(dispex_from(iface)->outer, index, lcid, out);
}

static HRESULT WINAPI dispex_GetIDsOfNames(IDispatchEx *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return IDispatch_GetIDsOfNames(dispex_from(iface)->outer, riid, names, count, lcid, ids);
}

static HRESULT WINAPI dispex_Invoke(IDispatchEx *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return IDispatch_Invoke(dispex_from(iface)->outer, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI dispex_GetDispID(IDispatchEx *iface, BSTR name, DWORD flags, DISPID *out)
{
	mx_dispex *self = dispex_from(iface);
	if (!name || !out)
		return E_INVALIDARG;
	if (self->collection && *name >= L'0' && *name <= L'9')
	{
		const WCHAR *at = name;
		LONGLONG index = 0;
		while (*at >= L'0' && *at <= L'9' && index <= DISPID_DOM_COLLECTION_MAX)
			index = index * 10 + (*at++ - L'0');
		if (!*at && index + DISPID_DOM_COLLECTION_BASE <= DISPID_DOM_COLLECTION_MAX)
		{
			*out = (DISPID)(index + DISPID_DOM_COLLECTION_BASE);
			return S_OK;
		}
		return DISP_E_UNKNOWNNAME;
	}
	return IDispatch_GetIDsOfNames(self->outer, &IID_NULL, &name, 1, 0, out);
}

static HRESULT WINAPI dispex_InvokeEx(IDispatchEx *iface, DISPID id, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, IServiceProvider *caller)
{
	return IDispatch_Invoke(dispex_from(iface)->outer, id, &IID_NULL, lcid, flags, params, result, excep, NULL);
}

static HRESULT WINAPI dispex_DeleteMemberByName(IDispatchEx *iface, BSTR name, DWORD flags)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI dispex_DeleteMemberByDispID(IDispatchEx *iface, DISPID id)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI dispex_GetMemberProperties(IDispatchEx *iface, DISPID id, DWORD fetch, DWORD *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI dispex_GetMemberName(IDispatchEx *iface, DISPID id, BSTR *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI dispex_GetNextDispID(IDispatchEx *iface, DWORD flags, DISPID id, DISPID *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI dispex_GetNameSpaceParent(IDispatchEx *iface, IUnknown **out)
{
	return E_NOTIMPL;
}

static const IDispatchExVtbl dispex_table = {
	dispex_QueryInterface, dispex_AddRef, dispex_Release, dispex_GetTypeInfoCount, dispex_GetTypeInfo, dispex_GetIDsOfNames, dispex_Invoke,
	dispex_GetDispID, dispex_InvokeEx, dispex_DeleteMemberByName, dispex_DeleteMemberByDispID, dispex_GetMemberProperties,
	dispex_GetMemberName, dispex_GetNextDispID, dispex_GetNameSpaceParent,
};

void mx_dispex_init(mx_dispex *self, void *outer, BOOL collection)
{
	self->vtbl = &dispex_table;
	self->outer = outer;
	self->collection = collection;
}
