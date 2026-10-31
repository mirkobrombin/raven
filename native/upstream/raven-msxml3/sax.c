/* SPDX-License-Identifier: MIT */
#include "mx.h"

/* --- A list of attributes, read through the same code by the reader's callbacks and the public class. --- */

typedef struct
{
	BSTR uri, local, qname, type, value;
} sax_attribute;

typedef struct
{
	sax_attribute *items;
	int count, room;
} sax_table;

static void table_clear(sax_table *table)
{
	int index;
	for (index = 0; index < table->count; index++)
	{
		sax_attribute *item = &table->items[index];
		SysFreeString(item->uri);
		SysFreeString(item->local);
		SysFreeString(item->qname);
		SysFreeString(item->type);
		SysFreeString(item->value);
	}
	table->count = 0;
}

static sax_attribute *table_add(sax_table *table)
{
	sax_attribute *item;
	if (table->count == table->room)
	{
		int room = table->room ? table->room * 2 : 8;
		sax_attribute *grown = mx_realloc(table->items, room * sizeof(*grown));
		if (!grown)
			return NULL;
		table->items = grown;
		table->room = room;
	}
	item = &table->items[table->count++];
	memset(item, 0, sizeof(*item));
	return item;
}

static BOOL same_text(BSTR have, const WCHAR *text, int length)
{
	return (int)SysStringLen(have) == length && (!length || !memcmp(have, text, length * sizeof(WCHAR)));
}

static int table_find_name(const sax_table *table, const WCHAR *uri, int uri_length, const WCHAR *local, int local_length)
{
	int index;
	for (index = 0; index < table->count; index++)
		if (same_text(table->items[index].uri, uri, uri_length) && same_text(table->items[index].local, local, local_length))
			return index;
	return -1;
}

static int table_find_qname(const sax_table *table, const WCHAR *qname, int length)
{
	int index;
	if (!length)
		return -1;
	for (index = 0; index < table->count; index++)
		if (same_text(table->items[index].qname, qname, length))
			return index;
	return -1;
}

static HRESULT give_text(BSTR text, const WCHAR **out, int *length)
{
	if (!out || !length)
		return E_POINTER;
	*out = text;
	*length = SysStringLen(text);
	return S_OK;
}

static HRESULT give_copy(BSTR text, BSTR *out)
{
	if (!out)
		return E_POINTER;
	*out = SysAllocStringLen(text, SysStringLen(text));
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* One face of a list. A reader's face answers for itself only; the public class's answers for the whole object too. */
typedef struct
{
	const ISAXAttributesVtbl *vtbl;
	sax_table *table;
	IUnknown *owner;
	BOOL open;
} attr_face;

typedef struct
{
	const IVBSAXAttributesVtbl *vtbl;
	sax_table *table;
	IUnknown *owner;
	BOOL open;
} vb_attr_face;

#define FACE_TABLE(iface) (((attr_face *)(iface))->table)
#define IN_RANGE(table, index) ((index) >= 0 && (index) < (table)->count)

static HRESULT WINAPI attr_QueryInterface(ISAXAttributes *iface, REFIID riid, void **out)
{
	attr_face *self = (attr_face *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->open)
		return IUnknown_QueryInterface(self->owner, riid, out);
	if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_ISAXAttributes))
		return E_NOINTERFACE;
	*out = iface;
	IUnknown_AddRef(self->owner);
	return S_OK;
}

static ULONG WINAPI attr_AddRef(ISAXAttributes *iface)
{
	return IUnknown_AddRef(((attr_face *)iface)->owner);
}

static ULONG WINAPI attr_Release(ISAXAttributes *iface)
{
	return IUnknown_Release(((attr_face *)iface)->owner);
}

static HRESULT WINAPI attr_getLength(ISAXAttributes *iface, int *length)
{
	if (!length)
		return E_POINTER;
	*length = FACE_TABLE(iface)->count;
	return S_OK;
}

static HRESULT WINAPI attr_getURI(ISAXAttributes *iface, int index, const WCHAR **out, int *length)
{
	sax_table *table = FACE_TABLE(iface);
	return IN_RANGE(table, index) ? give_text(table->items[index].uri, out, length) : E_INVALIDARG;
}

static HRESULT WINAPI attr_getLocalName(ISAXAttributes *iface, int index, const WCHAR **out, int *length)
{
	sax_table *table = FACE_TABLE(iface);
	return IN_RANGE(table, index) ? give_text(table->items[index].local, out, length) : E_INVALIDARG;
}

static HRESULT WINAPI attr_getQName(ISAXAttributes *iface, int index, const WCHAR **out, int *length)
{
	sax_table *table = FACE_TABLE(iface);
	return IN_RANGE(table, index) ? give_text(table->items[index].qname, out, length) : E_INVALIDARG;
}

static HRESULT WINAPI attr_getName(ISAXAttributes *iface, int index, const WCHAR **uri, int *uri_length, const WCHAR **local, int *local_length, const WCHAR **qname, int *qname_length)
{
	sax_table *table = FACE_TABLE(iface);
	if (!IN_RANGE(table, index))
		return E_INVALIDARG;
	if (!uri || !uri_length || !local || !local_length || !qname || !qname_length)
		return E_POINTER;
	give_text(table->items[index].uri, uri, uri_length);
	give_text(table->items[index].local, local, local_length);
	give_text(table->items[index].qname, qname, qname_length);
	return S_OK;
}

static HRESULT WINAPI attr_getIndexFromName(ISAXAttributes *iface, const WCHAR *uri, int uri_length, const WCHAR *local, int local_length, int *out)
{
	int index;
	if (!out)
		return E_POINTER;
	if (!uri || !local)
		return E_INVALIDARG;
	index = table_find_name(FACE_TABLE(iface), uri, uri_length, local, local_length);
	if (index < 0)
		return E_INVALIDARG;
	*out = index;
	return S_OK;
}

static HRESULT WINAPI attr_getIndexFromQName(ISAXAttributes *iface, const WCHAR *qname, int length, int *out)
{
	int index;
	if (!out)
		return E_POINTER;
	if (!qname)
		return E_INVALIDARG;
	index = table_find_qname(FACE_TABLE(iface), qname, length);
	if (index < 0)
		return E_INVALIDARG;
	*out = index;
	return S_OK;
}

static HRESULT WINAPI attr_getType(ISAXAttributes *iface, int index, const WCHAR **out, int *length)
{
	sax_table *table = FACE_TABLE(iface);
	return IN_RANGE(table, index) ? give_text(table->items[index].type, out, length) : E_INVALIDARG;
}

static HRESULT WINAPI attr_getTypeFromName(ISAXAttributes *iface, const WCHAR *uri, int uri_length, const WCHAR *local, int local_length, const WCHAR **out, int *length)
{
	int index;
	if (!out || !length)
		return E_POINTER;
	if (!uri || !local)
		return E_INVALIDARG;
	index = table_find_name(FACE_TABLE(iface), uri, uri_length, local, local_length);
	return index < 0 ? E_INVALIDARG : give_text(FACE_TABLE(iface)->items[index].type, out, length);
}

static HRESULT WINAPI attr_getTypeFromQName(ISAXAttributes *iface, const WCHAR *qname, int qname_length, const WCHAR **out, int *length)
{
	int index;
	if (!out || !length)
		return E_POINTER;
	if (!qname)
		return E_INVALIDARG;
	index = table_find_qname(FACE_TABLE(iface), qname, qname_length);
	return index < 0 ? E_INVALIDARG : give_text(FACE_TABLE(iface)->items[index].type, out, length);
}

static HRESULT WINAPI attr_getValue(ISAXAttributes *iface, int index, const WCHAR **out, int *length)
{
	sax_table *table = FACE_TABLE(iface);
	return IN_RANGE(table, index) ? give_text(table->items[index].value, out, length) : E_INVALIDARG;
}

static HRESULT WINAPI attr_getValueFromName(ISAXAttributes *iface, const WCHAR *uri, int uri_length, const WCHAR *local, int local_length, const WCHAR **out, int *length)
{
	int index;
	if (!out || !length)
		return E_POINTER;
	if (!uri || !local)
		return E_INVALIDARG;
	index = table_find_name(FACE_TABLE(iface), uri, uri_length, local, local_length);
	return index < 0 ? E_INVALIDARG : give_text(FACE_TABLE(iface)->items[index].value, out, length);
}

static HRESULT WINAPI attr_getValueFromQName(ISAXAttributes *iface, const WCHAR *qname, int qname_length, const WCHAR **out, int *length)
{
	int index;
	if (!out || !length)
		return E_POINTER;
	if (!qname)
		return E_INVALIDARG;
	index = table_find_qname(FACE_TABLE(iface), qname, qname_length);
	return index < 0 ? E_INVALIDARG : give_text(FACE_TABLE(iface)->items[index].value, out, length);
}

static const ISAXAttributesVtbl attr_table_face = {
	attr_QueryInterface, attr_AddRef, attr_Release, attr_getLength, attr_getURI, attr_getLocalName, attr_getQName,
	attr_getName, attr_getIndexFromName, attr_getIndexFromQName, attr_getType, attr_getTypeFromName, attr_getTypeFromQName,
	attr_getValue, attr_getValueFromName, attr_getValueFromQName,
};

/* The same list, for scripts. */

#define VB_TABLE(iface) (((vb_attr_face *)(iface))->table)

static HRESULT WINAPI vb_attr_QueryInterface(IVBSAXAttributes *iface, REFIID riid, void **out)
{
	vb_attr_face *self = (vb_attr_face *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->open)
		return IUnknown_QueryInterface(self->owner, riid, out);
	if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IDispatch) && !IsEqualGUID(riid, &IID_IVBSAXAttributes))
		return E_NOINTERFACE;
	*out = iface;
	IUnknown_AddRef(self->owner);
	return S_OK;
}

static ULONG WINAPI vb_attr_AddRef(IVBSAXAttributes *iface)
{
	return IUnknown_AddRef(((vb_attr_face *)iface)->owner);
}

static ULONG WINAPI vb_attr_Release(IVBSAXAttributes *iface)
{
	return IUnknown_Release(((vb_attr_face *)iface)->owner);
}

static HRESULT WINAPI vb_attr_GetTypeInfoCount(IVBSAXAttributes *iface, UINT *count)
{
	*count = 1;
	return S_OK;
}

static HRESULT WINAPI vb_attr_GetTypeInfo(IVBSAXAttributes *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	return mx_type_info(MX_TYPE_VB_ATTRIBUTES, out);
}

static HRESULT WINAPI vb_attr_GetIDsOfNames(IVBSAXAttributes *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_VB_ATTRIBUTES, riid, names, count, lcid, ids);
}

static HRESULT WINAPI vb_attr_Invoke(IVBSAXAttributes *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_VB_ATTRIBUTES, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI vb_attr_get_length(IVBSAXAttributes *iface, int *length)
{
	if (!length)
		return E_POINTER;
	*length = VB_TABLE(iface)->count;
	return S_OK;
}

static HRESULT WINAPI vb_attr_getURI(IVBSAXAttributes *iface, int index, BSTR *out)
{
	sax_table *table = VB_TABLE(iface);
	return IN_RANGE(table, index) ? give_copy(table->items[index].uri, out) : E_INVALIDARG;
}

static HRESULT WINAPI vb_attr_getLocalName(IVBSAXAttributes *iface, int index, BSTR *out)
{
	sax_table *table = VB_TABLE(iface);
	return IN_RANGE(table, index) ? give_copy(table->items[index].local, out) : E_INVALIDARG;
}

static HRESULT WINAPI vb_attr_getQName(IVBSAXAttributes *iface, int index, BSTR *out)
{
	sax_table *table = VB_TABLE(iface);
	return IN_RANGE(table, index) ? give_copy(table->items[index].qname, out) : E_INVALIDARG;
}

static HRESULT WINAPI vb_attr_getIndexFromName(IVBSAXAttributes *iface, BSTR uri, BSTR local, int *out)
{
	int index;
	if (!out)
		return E_POINTER;
	index = table_find_name(VB_TABLE(iface), uri, SysStringLen(uri), local, SysStringLen(local));
	if (index < 0)
		return E_INVALIDARG;
	*out = index;
	return S_OK;
}

static HRESULT WINAPI vb_attr_getIndexFromQName(IVBSAXAttributes *iface, BSTR qname, int *out)
{
	int index;
	if (!out)
		return E_POINTER;
	index = table_find_qname(VB_TABLE(iface), qname, SysStringLen(qname));
	if (index < 0)
		return E_INVALIDARG;
	*out = index;
	return S_OK;
}

static HRESULT WINAPI vb_attr_getType(IVBSAXAttributes *iface, int index, BSTR *out)
{
	sax_table *table = VB_TABLE(iface);
	return IN_RANGE(table, index) ? give_copy(table->items[index].type, out) : E_INVALIDARG;
}

static HRESULT WINAPI vb_attr_getTypeFromName(IVBSAXAttributes *iface, BSTR uri, BSTR local, BSTR *out)
{
	int index = table_find_name(VB_TABLE(iface), uri, SysStringLen(uri), local, SysStringLen(local));
	return index < 0 ? E_INVALIDARG : give_copy(VB_TABLE(iface)->items[index].type, out);
}

static HRESULT WINAPI vb_attr_getTypeFromQName(IVBSAXAttributes *iface, BSTR qname, BSTR *out)
{
	int index = table_find_qname(VB_TABLE(iface), qname, SysStringLen(qname));
	return index < 0 ? E_INVALIDARG : give_copy(VB_TABLE(iface)->items[index].type, out);
}

static HRESULT WINAPI vb_attr_getValue(IVBSAXAttributes *iface, int index, BSTR *out)
{
	sax_table *table = VB_TABLE(iface);
	return IN_RANGE(table, index) ? give_copy(table->items[index].value, out) : E_INVALIDARG;
}

static HRESULT WINAPI vb_attr_getValueFromName(IVBSAXAttributes *iface, BSTR uri, BSTR local, BSTR *out)
{
	int index = table_find_name(VB_TABLE(iface), uri, SysStringLen(uri), local, SysStringLen(local));
	return index < 0 ? E_INVALIDARG : give_copy(VB_TABLE(iface)->items[index].value, out);
}

static HRESULT WINAPI vb_attr_getValueFromQName(IVBSAXAttributes *iface, BSTR qname, BSTR *out)
{
	int index = table_find_qname(VB_TABLE(iface), qname, SysStringLen(qname));
	return index < 0 ? E_INVALIDARG : give_copy(VB_TABLE(iface)->items[index].value, out);
}

static const IVBSAXAttributesVtbl vb_attr_table_face = {
	vb_attr_QueryInterface, vb_attr_AddRef, vb_attr_Release, vb_attr_GetTypeInfoCount, vb_attr_GetTypeInfo, vb_attr_GetIDsOfNames, vb_attr_Invoke,
	vb_attr_get_length, vb_attr_getURI, vb_attr_getLocalName, vb_attr_getQName, vb_attr_getIndexFromName, vb_attr_getIndexFromQName,
	vb_attr_getType, vb_attr_getTypeFromName, vb_attr_getTypeFromQName, vb_attr_getValue, vb_attr_getValueFromName, vb_attr_getValueFromQName,
};

/* --- The public attributes class. --- */

typedef struct
{
	const IMXAttributesVtbl *vtbl;
	attr_face sax;
	vb_attr_face vb;
	mx_dispex dispex;
	LONG refs;
	sax_table table;
} mx_attributes;

static HRESULT WINAPI list_QueryInterface(IMXAttributes *iface, REFIID riid, void **out)
{
	mx_attributes *self = (mx_attributes *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IMXAttributes) || IsEqualGUID(riid, &IID_IDispatch))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &self->dispex;
	else if (IsEqualGUID(riid, &IID_ISAXAttributes))
		*out = &self->sax;
	else if (IsEqualGUID(riid, &IID_IVBSAXAttributes))
		*out = &self->vb;
	else
		return E_NOINTERFACE;
	IMXAttributes_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI list_AddRef(IMXAttributes *iface)
{
	return InterlockedIncrement(&((mx_attributes *)iface)->refs);
}

static ULONG WINAPI list_Release(IMXAttributes *iface)
{
	mx_attributes *self = (mx_attributes *)iface;
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		table_clear(&self->table);
		mx_free(self->table.items);
		mx_free(self);
	}
	return left;
}

static HRESULT WINAPI list_GetTypeInfoCount(IMXAttributes *iface, UINT *count)
{
	*count = 1;
	return S_OK;
}

static HRESULT WINAPI list_GetTypeInfo(IMXAttributes *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	return mx_type_info(MX_TYPE_ATTRIBUTES, out);
}

static HRESULT WINAPI list_GetIDsOfNames(IMXAttributes *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_ATTRIBUTES, riid, names, count, lcid, ids);
}

static HRESULT WINAPI list_Invoke(IMXAttributes *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_ATTRIBUTES, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

/* A field given to one of the setters replaces what was there. */
static HRESULT replace_text(BSTR *field, BSTR text)
{
	BSTR copy = SysAllocStringLen(text, SysStringLen(text));
	if (!copy)
		return E_OUTOFMEMORY;
	SysFreeString(*field);
	*field = copy;
	return S_OK;
}

static HRESULT fill_attribute(sax_attribute *item, BSTR uri, BSTR local, BSTR qname, BSTR type, BSTR value)
{
	HRESULT status = replace_text(&item->uri, uri);
	if (SUCCEEDED(status))
		status = replace_text(&item->local, local);
	if (SUCCEEDED(status))
		status = replace_text(&item->qname, qname);
	if (SUCCEEDED(status))
		status = replace_text(&item->type, type);
	if (SUCCEEDED(status))
		status = replace_text(&item->value, value);
	return status;
}

static HRESULT WINAPI list_addAttribute(IMXAttributes *iface, BSTR uri, BSTR local, BSTR qname, BSTR type, BSTR value)
{
	mx_attributes *self = (mx_attributes *)iface;
	sax_attribute *item;
	if (!uri || !local || !qname || !type || !value)
		return E_INVALIDARG;
	item = table_add(&self->table);
	return item ? fill_attribute(item, uri, local, qname, type, value) : E_OUTOFMEMORY;
}

/* The source of a copy may be any object that lists attributes, native or for scripts. */
static HRESULT copy_from_source(mx_attributes *self, const VARIANT *source, int only)
{
	const VARIANT *from = source;
	IUnknown *object;
	ISAXAttributes *list = NULL;
	int count = 0, index;
	HRESULT status;
	while (V_VT(from) == (VT_VARIANT | VT_BYREF))
		from = V_VARIANTREF(from);
	if (V_VT(from) != VT_UNKNOWN && V_VT(from) != VT_DISPATCH)
		return E_INVALIDARG;
	object = V_UNKNOWN(from);
	if (!object)
		return E_INVALIDARG;
	status = IUnknown_QueryInterface(object, &IID_ISAXAttributes, (void **)&list);
	if (FAILED(status))
		return E_INVALIDARG;
	status = ISAXAttributes_getLength(list, &count);
	for (index = only >= 0 ? only : 0; SUCCEEDED(status) && index < (only >= 0 ? only + 1 : count); index++)
	{
		const WCHAR *uri, *local, *qname, *type, *value;
		int uri_length, local_length, qname_length, type_length, value_length;
		sax_attribute *item;
		status = ISAXAttributes_getName(list, index, &uri, &uri_length, &local, &local_length, &qname, &qname_length);
		if (SUCCEEDED(status))
			status = ISAXAttributes_getType(list, index, &type, &type_length);
		if (SUCCEEDED(status))
			status = ISAXAttributes_getValue(list, index, &value, &value_length);
		if (FAILED(status))
			break;
		item = table_add(&self->table);
		if (!item)
		{
			status = E_OUTOFMEMORY;
			break;
		}
		item->uri = SysAllocStringLen(uri, uri_length);
		item->local = SysAllocStringLen(local, local_length);
		item->qname = SysAllocStringLen(qname, qname_length);
		item->type = SysAllocStringLen(type, type_length);
		item->value = SysAllocStringLen(value, value_length);
	}
	ISAXAttributes_Release(list);
	return status;
}

static HRESULT WINAPI list_addAttributeFromIndex(IMXAttributes *iface, VARIANT source, int index)
{
	return index < 0 ? E_INVALIDARG : copy_from_source((mx_attributes *)iface, &source, index);
}

static HRESULT WINAPI list_clear(IMXAttributes *iface)
{
	table_clear(&((mx_attributes *)iface)->table);
	return S_OK;
}

static HRESULT WINAPI list_removeAttribute(IMXAttributes *iface, int index)
{
	sax_table *table = &((mx_attributes *)iface)->table;
	sax_attribute *item;
	if (!IN_RANGE(table, index))
		return E_INVALIDARG;
	item = &table->items[index];
	SysFreeString(item->uri);
	SysFreeString(item->local);
	SysFreeString(item->qname);
	SysFreeString(item->type);
	SysFreeString(item->value);
	memmove(item, item + 1, (table->count - index - 1) * sizeof(*item));
	table->count--;
	return S_OK;
}

static HRESULT WINAPI list_setAttribute(IMXAttributes *iface, int index, BSTR uri, BSTR local, BSTR qname, BSTR type, BSTR value)
{
	sax_table *table = &((mx_attributes *)iface)->table;
	return IN_RANGE(table, index) ? fill_attribute(&table->items[index], uri, local, qname, type, value) : E_INVALIDARG;
}

static HRESULT WINAPI list_setAttributes(IMXAttributes *iface, VARIANT source)
{
	table_clear(&((mx_attributes *)iface)->table);
	return copy_from_source((mx_attributes *)iface, &source, -1);
}

#define LIST_SETTER(name, field) \
	static HRESULT WINAPI list_##name(IMXAttributes *iface, int index, BSTR text) \
	{ \
		sax_table *table = &((mx_attributes *)iface)->table; \
		return IN_RANGE(table, index) ? replace_text(&table->items[index].field, text) : E_INVALIDARG; \
	}

LIST_SETTER(setLocalName, local)
LIST_SETTER(setQName, qname)
LIST_SETTER(setType, type)
LIST_SETTER(setURI, uri)
LIST_SETTER(setValue, value)

static const IMXAttributesVtbl list_table = {
	list_QueryInterface, list_AddRef, list_Release, list_GetTypeInfoCount, list_GetTypeInfo, list_GetIDsOfNames, list_Invoke,
	list_addAttribute, list_addAttributeFromIndex, list_clear, list_removeAttribute, list_setAttribute, list_setAttributes,
	list_setLocalName, list_setQName, list_setType, list_setURI, list_setValue,
};

HRESULT mx_sax_attributes_create(mx_version version, REFIID riid, void **out)
{
	mx_attributes *self = mx_zalloc(sizeof(*self));
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &list_table;
	self->refs = 1;
	self->sax.vtbl = &attr_table_face;
	self->sax.table = &self->table;
	self->sax.owner = (IUnknown *)self;
	self->sax.open = TRUE;
	self->vb.vtbl = &vb_attr_table_face;
	self->vb.table = &self->table;
	self->vb.owner = (IUnknown *)self;
	self->vb.open = TRUE;
	mx_dispex_init(&self->dispex, self, FALSE);
	status = IMXAttributes_QueryInterface((IMXAttributes *)self, riid, out);
	IMXAttributes_Release((IMXAttributes *)self);
	return status;
}

/* --- The reader. --- */

typedef enum
{
	SLOT_CONTENT,
	SLOT_ERROR,
	SLOT_DTD,
	SLOT_RESOLVER,
	SLOT_LEXICAL,
	SLOT_DECLARATION,
	SLOT_COUNT,
} handler_slot;

/* Which features exist and what they start as. */
typedef enum
{
	FEATURE_NAMESPACES,
	FEATURE_PREFIXES,
	FEATURE_NORMALIZE_LINES,
	FEATURE_GENERAL_ENTITIES,
	FEATURE_PARAMETER_ENTITIES,
	FEATURE_PROHIBIT_DTD,
	FEATURE_SERVER_REQUEST,
	FEATURE_COUNT,
} reader_feature;

static const struct
{
	const WCHAR *name;
	VARIANT_BOOL start;
} features[FEATURE_COUNT] = {
	{L"http://xml.org/sax/features/namespaces", VARIANT_TRUE},
	{L"http://xml.org/sax/features/namespace-prefixes", VARIANT_TRUE},
	{L"normalize-line-breaks", VARIANT_TRUE},
	{L"http://xml.org/sax/features/external-general-entities", VARIANT_TRUE},
	{L"http://xml.org/sax/features/external-parameter-entities", VARIANT_TRUE},
	{L"prohibit-dtd", VARIANT_FALSE},
	{L"server-http-request", VARIANT_FALSE},
};

typedef struct
{
	const ISAXXMLReaderVtbl *vtbl;
	const IVBSAXXMLReaderVtbl *vb_vtbl;
	mx_dispex dispex;
	LONG refs;
	mx_version version;
	IUnknown *handlers[2][SLOT_COUNT];
	VARIANT_BOOL feature[FEATURE_COUNT];
	WCHAR *base_url, *secure_url;
	BSTR declared_version, declared_encoding;
	LONG max_size, max_depth;
	BOOL busy;
} sax_reader;

#define READER_OF_VB(iface) CONTAINING_RECORD(iface, sax_reader, vb_vtbl)

/* One parse: what the callbacks see, alive for as long as a handler keeps the locator. */
typedef struct
{
	const ISAXLocatorVtbl *locator_vtbl;
	const IVBSAXLocatorVtbl *vb_locator_vtbl;
	attr_face attrs;
	vb_attr_face vb_attrs;
	const IUnknownVtbl *owner_vtbl;
	LONG refs;
	sax_table table;

	sax_reader *reader;
	BOOL vb;
	xmlParserCtxtPtr context;
	void *current;
	const char *data;
	size_t size;

	/* Where the reader has got to in the text, and the place the locator reports. */
	size_t position, scanned;
	int scan_line, scan_column;
	int line, column;
	BOOL started;

	HRESULT status;
	xmlChar *pending;
	size_t pending_length, pending_room;
	xmlChar **prefixes;
	int prefix_count, prefix_room;
	int *marks;
	int depth, mark_room;
} sax_run;

static IUnknown *run_owner(sax_run *run)
{
	return (IUnknown *)&run->owner_vtbl;
}

static void run_release(sax_run *run)
{
	if (!InterlockedDecrement(&run->refs))
	{
		int index;
		for (index = 0; index < run->prefix_count; index++)
			xmlFree(run->prefixes[index]);
		mx_free(run->prefixes);
		mx_free(run->marks);
		xmlFree(run->pending);
		table_clear(&run->table);
		mx_free(run->table.items);
		mx_free(run);
	}
}

#define RUN_OF_OWNER(iface) CONTAINING_RECORD(iface, sax_run, owner_vtbl)

static HRESULT WINAPI owner_QueryInterface(IUnknown *iface, REFIID riid, void **out)
{
	*out = NULL;
	if (!IsEqualGUID(riid, &IID_IUnknown))
		return E_NOINTERFACE;
	*out = iface;
	IUnknown_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI owner_AddRef(IUnknown *iface)
{
	return InterlockedIncrement(&RUN_OF_OWNER(iface)->refs);
}

static ULONG WINAPI owner_Release(IUnknown *iface)
{
	sax_run *run = RUN_OF_OWNER(iface);
	ULONG left = run->refs - 1;
	run_release(run);
	return left;
}

static const IUnknownVtbl owner_table = {owner_QueryInterface, owner_AddRef, owner_Release};

/* The locator. */

#define RUN_OF_LOCATOR(iface) CONTAINING_RECORD(iface, sax_run, locator_vtbl)
#define RUN_OF_VB_LOCATOR(iface) CONTAINING_RECORD(iface, sax_run, vb_locator_vtbl)

static HRESULT WINAPI locator_QueryInterface(ISAXLocator *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_ISAXLocator))
		return E_NOINTERFACE;
	*out = iface;
	ISAXLocator_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI locator_AddRef(ISAXLocator *iface)
{
	return InterlockedIncrement(&RUN_OF_LOCATOR(iface)->refs);
}

static ULONG WINAPI locator_Release(ISAXLocator *iface)
{
	sax_run *run = RUN_OF_LOCATOR(iface);
	ULONG left = run->refs - 1;
	run_release(run);
	return left;
}

static HRESULT WINAPI locator_getColumnNumber(ISAXLocator *iface, int *out)
{
	if (!out)
		return E_POINTER;
	*out = RUN_OF_LOCATOR(iface)->column;
	return S_OK;
}

static HRESULT WINAPI locator_getLineNumber(ISAXLocator *iface, int *out)
{
	if (!out)
		return E_POINTER;
	*out = RUN_OF_LOCATOR(iface)->line;
	return S_OK;
}

static HRESULT WINAPI locator_getPublicId(ISAXLocator *iface, const WCHAR **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return S_OK;
}

static HRESULT WINAPI locator_getSystemId(ISAXLocator *iface, const WCHAR **out)
{
	if (!out)
		return E_POINTER;
	*out = RUN_OF_LOCATOR(iface)->reader ? RUN_OF_LOCATOR(iface)->reader->base_url : NULL;
	return S_OK;
}

static const ISAXLocatorVtbl locator_table = {
	locator_QueryInterface, locator_AddRef, locator_Release, locator_getColumnNumber, locator_getLineNumber, locator_getPublicId, locator_getSystemId,
};

static HRESULT WINAPI vb_locator_QueryInterface(IVBSAXLocator *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IDispatch) && !IsEqualGUID(riid, &IID_IVBSAXLocator))
		return E_NOINTERFACE;
	*out = iface;
	IVBSAXLocator_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI vb_locator_AddRef(IVBSAXLocator *iface)
{
	return InterlockedIncrement(&RUN_OF_VB_LOCATOR(iface)->refs);
}

static ULONG WINAPI vb_locator_Release(IVBSAXLocator *iface)
{
	sax_run *run = RUN_OF_VB_LOCATOR(iface);
	ULONG left = run->refs - 1;
	run_release(run);
	return left;
}

static HRESULT WINAPI vb_locator_GetTypeInfoCount(IVBSAXLocator *iface, UINT *count)
{
	*count = 1;
	return S_OK;
}

static HRESULT WINAPI vb_locator_GetTypeInfo(IVBSAXLocator *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	return mx_type_info(MX_TYPE_VB_LOCATOR, out);
}

static HRESULT WINAPI vb_locator_GetIDsOfNames(IVBSAXLocator *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_VB_LOCATOR, riid, names, count, lcid, ids);
}

static HRESULT WINAPI vb_locator_Invoke(IVBSAXLocator *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_VB_LOCATOR, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI vb_locator_get_columnNumber(IVBSAXLocator *iface, int *out)
{
	if (!out)
		return E_POINTER;
	*out = RUN_OF_VB_LOCATOR(iface)->column;
	return S_OK;
}

static HRESULT WINAPI vb_locator_get_lineNumber(IVBSAXLocator *iface, int *out)
{
	if (!out)
		return E_POINTER;
	*out = RUN_OF_VB_LOCATOR(iface)->line;
	return S_OK;
}

static HRESULT WINAPI vb_locator_get_publicId(IVBSAXLocator *iface, BSTR *out)
{
	if (!out)
		return E_POINTER;
	*out = SysAllocString(L"");
	return S_OK;
}

static HRESULT WINAPI vb_locator_get_systemId(IVBSAXLocator *iface, BSTR *out)
{
	sax_reader *reader = RUN_OF_VB_LOCATOR(iface)->reader;
	if (!out)
		return E_POINTER;
	*out = SysAllocString(reader && reader->base_url ? reader->base_url : L"");
	return S_OK;
}

static const IVBSAXLocatorVtbl vb_locator_table = {
	vb_locator_QueryInterface, vb_locator_AddRef, vb_locator_Release, vb_locator_GetTypeInfoCount, vb_locator_GetTypeInfo, vb_locator_GetIDsOfNames, vb_locator_Invoke,
	vb_locator_get_columnNumber, vb_locator_get_lineNumber, vb_locator_get_publicId, vb_locator_get_systemId,
};

/* --- Position in the text. Lines end at a line feed, a carriage return, or the pair; characters, not bytes, count. --- */

static void scan_to(sax_run *run, size_t target)
{
	const unsigned char *data = (const unsigned char *)run->data;
	if (target > run->size)
		target = run->size;
	while (run->scanned < target)
	{
		unsigned char byte = data[run->scanned];
		if (byte == '\n' || (byte == '\r' && !(run->scanned + 1 < run->size && data[run->scanned + 1] == '\n')))
		{
			run->scan_line++;
			run->scan_column = 1;
		}
		else if (byte == '\r')
			;
		else if ((byte & 0xc0) != 0x80)
			run->scan_column += byte >= 0xf0 ? 2 : 1;
		run->scanned++;
	}
}

static void report_at(sax_run *run, size_t target)
{
	scan_to(run, target);
	run->line = run->scan_line;
	run->column = run->scan_column;
}

static size_t main_offset(sax_run *run)
{
	xmlParserInputPtr input = run->context->inputTab ? run->context->inputTab[0] : run->context->input;
	return input ? (size_t)input->consumed + (size_t)(input->cur - input->base) : run->position;
}

/* Whether the text under the parser right now is the document's own and not some entity's. */
static BOOL in_document(sax_run *run, void *context)
{
	return context == run->context && run->context->inputNr == 1;
}

static size_t find_text(sax_run *run, size_t from, const char *text, size_t length)
{
	size_t at;
	for (at = from; at + length <= run->size; at++)
		if (!memcmp(run->data + at, text, length))
			return at;
	return run->size;
}

static IUnknown *slot_of(sax_run *run, handler_slot slot)
{
	return run->reader->handlers[run->vb ? 1 : 0][slot];
}

static IDispatch *vb_locator_of(sax_run *run)
{
	return (IDispatch *)&run->vb_locator_vtbl;
}

static BSTR arg_of(const xmlChar *text)
{
	return text ? mx_bstr(text) : SysAllocString(L"");
}

/* Anything but success from a handler ends the parse; the error handler hears about it once. */
static void stop_with(sax_run *run, HRESULT status, const WCHAR *message)
{
	IUnknown *errors;
	if (run->status != S_OK)
		return;
	run->status = status;
	errors = slot_of(run, SLOT_ERROR);
	if (errors)
	{
		BSTR text = SysAllocString(message ? message : L"");
		if (run->vb)
			IVBSAXErrorHandler_fatalError((IVBSAXErrorHandler *)errors, (IVBSAXLocator *)vb_locator_of(run), &text, status);
		else
			ISAXErrorHandler_fatalError((ISAXErrorHandler *)errors, (ISAXLocator *)&run->locator_vtbl, text, status);
		SysFreeString(text);
	}
	if (run->current)
		xmlStopParser(run->current);
}

static void check_status(sax_run *run, HRESULT status)
{
	if (status != S_OK)
		stop_with(run, status, NULL);
}

static BOOL run_active(sax_run *run)
{
	return run->status == S_OK;
}

/* --- Text. --- */

typedef enum
{
	TEXT_CHARACTERS,
	TEXT_COMMENT,
} text_sink;

static void deliver_text(sax_run *run, text_sink sink, const xmlChar *text, size_t length)
{
	IUnknown *handler = slot_of(run, sink == TEXT_COMMENT ? SLOT_LEXICAL : SLOT_CONTENT);
	BSTR arg;
	if (!handler || !run_active(run))
		return;
	arg = mx_bstr_counted(text, (int)length);
	if (sink == TEXT_COMMENT)
		check_status(run, run->vb ? IVBSAXLexicalHandler_comment((IVBSAXLexicalHandler *)handler, &arg)
			: ISAXLexicalHandler_comment((ISAXLexicalHandler *)handler, arg, SysStringLen(arg)));
	else
		check_status(run, run->vb ? IVBSAXContentHandler_characters((IVBSAXContentHandler *)handler, &arg)
			: ISAXContentHandler_characters((ISAXContentHandler *)handler, arg, SysStringLen(arg)));
	SysFreeString(arg);
}

/* Walk text the parser has decoded along its source. With line breaks being normalized, every break the
   source wrote as a carriage return closes one delivery; the pair leaves the column running on the same
   line while a lone one starts the next line. */
static void walk_text(sax_run *run, text_sink sink, const xmlChar *text, size_t length, BOOL raw_source)
{
	BOOL normalize = run->reader->feature[FEATURE_NORMALIZE_LINES] == VARIANT_TRUE;
	xmlChar *out = xmlMalloc(length * 2 + 2);
	size_t used = 0, index = 0;
	size_t at = run->position;
	int line = run->line, column = run->column;
	int chunk_column = 0;
	if (!out)
	{
		stop_with(run, E_OUTOFMEMORY, NULL);
		return;
	}
	while (index < length)
	{
		xmlChar byte = text[index];
		if (raw_source && at < run->size && run->data[at] == '\r')
		{
			BOOL pair = at + 1 < run->size && run->data[at + 1] == '\n';
			int width = pair ? 2 : 1;
			index++;
			at += width;
			if (normalize)
			{
				out[used++] = '\n';
				run->line = line;
				run->column = column;
				deliver_text(run, sink, out, used);
				used = 0;
				if (pair)
					column += chunk_column + width;
				else
				{
					line++;
					column = 1;
				}
				chunk_column = 0;
				if (!run_active(run))
					break;
			}
			else
			{
				if (pair)
					out[used++] = '\r';
				out[used++] = pair ? '\n' : '\r';
			}
			continue;
		}
		if (raw_source && at < run->size && run->data[at] == '&')
		{
			const char *close = memchr(run->data + at, ';', run->size - at);
			if (close)
				at = (size_t)(close - run->data) + 1;
		}
		else if (raw_source && at < run->size && run->data[at] == (char)byte)
		{
			at++;
			if ((byte & 0xc0) != 0x80)
				chunk_column += byte >= 0xf0 ? 2 : 1;
		}
		out[used++] = byte;
		index++;
	}
	if (used || !length)
	{
		run->line = line;
		run->column = column;
		deliver_text(run, sink, out, used);
	}
	xmlFree(out);
	if (raw_source)
		run->position = at;
}

/* The parser hands a run of text over in pieces; a handler sees it whole, up to the next markup. */
static void flush_pending(sax_run *run)
{
	size_t length = run->pending_length;
	if (!length)
		return;
	run->pending_length = 0;
	report_at(run, run->position);
	walk_text(run, TEXT_CHARACTERS, run->pending, length, TRUE);
}

static sax_run *run_of(void *context)
{
	return ((xmlParserCtxtPtr)context)->_private;
}

static void XMLCALL on_characters(void *context, const xmlChar *text, int length)
{
	sax_run *run = run_of(context);
	if (!run_active(run) || length <= 0)
		return;
	if (!in_document(run, context))
	{
		deliver_text(run, TEXT_CHARACTERS, text, length);
		return;
	}
	if (run->pending_length + length + 1 > run->pending_room)
	{
		size_t room = (run->pending_length + length + 1) * 2;
		xmlChar *grown = xmlRealloc(run->pending, room);
		if (!grown)
		{
			stop_with(run, E_OUTOFMEMORY, NULL);
			return;
		}
		run->pending = grown;
		run->pending_room = room;
	}
	memcpy(run->pending + run->pending_length, text, length);
	run->pending_length += length;
}

static void XMLCALL on_cdata(void *context, const xmlChar *text, int length)
{
	sax_run *run = run_of(context);
	IUnknown *lexical;
	if (!run_active(run))
		return;
	flush_pending(run);
	lexical = slot_of(run, SLOT_LEXICAL);
	if (in_document(run, context))
	{
		run->position = find_text(run, run->position, "<![CDATA[", 9) + 9;
		report_at(run, run->position);
	}
	if (lexical && run_active(run))
		check_status(run, run->vb ? IVBSAXLexicalHandler_startCDATA((IVBSAXLexicalHandler *)lexical) : ISAXLexicalHandler_startCDATA((ISAXLexicalHandler *)lexical));
	if (run_active(run))
		walk_text(run, TEXT_CHARACTERS, text, length, in_document(run, context));
	if (lexical && run_active(run))
		check_status(run, run->vb ? IVBSAXLexicalHandler_endCDATA((IVBSAXLexicalHandler *)lexical) : ISAXLexicalHandler_endCDATA((ISAXLexicalHandler *)lexical));
	if (in_document(run, context))
		run->position = find_text(run, run->position, "]]>", 3) + 3;
}

static void XMLCALL on_comment(void *context, const xmlChar *text)
{
	sax_run *run = run_of(context);
	size_t length = xmlStrlen(text);
	if (!run_active(run))
		return;
	flush_pending(run);
	if (in_document(run, context))
	{
		run->position = find_text(run, run->position, "<!--", 4) + 4;
		report_at(run, run->position);
		walk_text(run, TEXT_COMMENT, text, length, TRUE);
		run->position = find_text(run, run->position, "-->", 3) + 3;
	}
	else
		deliver_text(run, TEXT_COMMENT, text, length);
}

static void XMLCALL on_instruction(void *context, const xmlChar *target, const xmlChar *data)
{
	sax_run *run = run_of(context);
	IUnknown *handler;
	BSTR target_arg, data_arg = NULL;
	if (!run_active(run))
		return;
	flush_pending(run);
	if (in_document(run, context))
	{
		size_t start = find_text(run, run->position, "<?", 2) + 2 + xmlStrlen(target);
		size_t end = find_text(run, start, "?>", 2);
		while (start < end && strchr(" \t\r\n", run->data[start]))
			start++;
		report_at(run, start);
		if (run->reader->feature[FEATURE_NORMALIZE_LINES] != VARIANT_TRUE)
			data_arg = mx_bstr_counted((const xmlChar *)run->data + start, (int)(end - start));
		run->position = end + 2;
	}
	handler = slot_of(run, SLOT_CONTENT);
	if (!data_arg)
		data_arg = arg_of(data);
	target_arg = arg_of(target);
	if (handler)
		check_status(run, run->vb ? IVBSAXContentHandler_processingInstruction((IVBSAXContentHandler *)handler, &target_arg, &data_arg)
			: ISAXContentHandler_processingInstruction((ISAXContentHandler *)handler, target_arg, SysStringLen(target_arg), data_arg, SysStringLen(data_arg)));
	SysFreeString(target_arg);
	SysFreeString(data_arg);
}

/* --- Elements. --- */

static BSTR qualified_arg(const xmlChar *prefix, const xmlChar *local)
{
	BSTR out;
	xmlChar *name;
	if (!prefix)
		return arg_of(local);
	name = xmlStrdup(prefix);
	name = xmlStrcat(name, (const xmlChar *)":");
	name = xmlStrcat(name, local);
	out = arg_of(name);
	xmlFree(name);
	return out;
}

/* What a declaration says about an attribute's type; undeclared ones hold plain character data. */
static const WCHAR *declared_type(sax_run *run, void *context, const xmlChar *element, const xmlChar *local, const xmlChar *prefix)
{
	xmlDocPtr tree = ((xmlParserCtxtPtr)context)->myDoc;
	xmlAttributePtr found = NULL;
	if (tree && tree->intSubset)
		found = xmlGetDtdQAttrDesc(tree->intSubset, element, local, prefix);
	if (found)
		switch (found->atype)
		{
		case XML_ATTRIBUTE_ID: return L"ID";
		case XML_ATTRIBUTE_IDREF: return L"IDREF";
		case XML_ATTRIBUTE_IDREFS: return L"IDREFS";
		case XML_ATTRIBUTE_ENTITY: return L"ENTITY";
		case XML_ATTRIBUTE_ENTITIES: return L"ENTITIES";
		case XML_ATTRIBUTE_NMTOKEN: return L"NMTOKEN";
		case XML_ATTRIBUTE_NMTOKENS: return L"NMTOKENS";
		case XML_ATTRIBUTE_ENUMERATION: return L"ENUMERATION";
		case XML_ATTRIBUTE_NOTATION: return L"NOTATION";
		default: break;
		}
	return L"CDATA";
}

static void XMLCALL on_start_element(void *context, const xmlChar *local, const xmlChar *prefix, const xmlChar *uri,
	int namespace_count, const xmlChar **namespaces, int attribute_count, int defaulted, const xmlChar **attributes)
{
	sax_run *run = run_of(context);
	sax_reader *reader = run->reader;
	IUnknown *content;
	BOOL names = reader->feature[FEATURE_NAMESPACES] == VARIANT_TRUE || reader->version < MX_4;
	BSTR uri_arg, local_arg, qname_arg;
	int index;
	if (!run_active(run))
		return;
	flush_pending(run);
	if (in_document(run, context))
	{
		size_t at = main_offset(run);
		if (at < run->size)
			at += run->data[at] == '/' ? 2 : 1;
		report_at(run, at);
		run->position = at;
	}
	run->depth++;
	if (reader->max_depth && run->depth > reader->max_depth)
	{
		stop_with(run, E_ABORT, L"Maximum element depth exceeded.\r\n");
		return;
	}
	if (run->depth >= run->mark_room)
	{
		int room = run->mark_room ? run->mark_room * 2 : 32;
		int *grown = mx_realloc(run->marks, room * sizeof(*grown));
		if (!grown)
		{
			stop_with(run, E_OUTOFMEMORY, NULL);
			return;
		}
		run->marks = grown;
		run->mark_room = room;
	}
	run->marks[run->depth] = run->prefix_count;
	content = slot_of(run, SLOT_CONTENT);
	for (index = 0; index < namespace_count && run_active(run); index++)
	{
		const xmlChar *name = namespaces[index * 2], *value = namespaces[index * 2 + 1];
		if (run->prefix_count == run->prefix_room)
		{
			int room = run->prefix_room ? run->prefix_room * 2 : 16;
			xmlChar **grown = mx_realloc(run->prefixes, room * sizeof(*grown));
			if (!grown)
			{
				stop_with(run, E_OUTOFMEMORY, NULL);
				return;
			}
			run->prefixes = grown;
			run->prefix_room = room;
		}
		run->prefixes[run->prefix_count++] = xmlStrdup(name ? name : (const xmlChar *)"");
		if (content)
		{
			BSTR name_arg = arg_of(name), value_arg = arg_of(value);
			check_status(run, run->vb ? IVBSAXContentHandler_startPrefixMapping((IVBSAXContentHandler *)content, &name_arg, &value_arg)
				: ISAXContentHandler_startPrefixMapping((ISAXContentHandler *)content, name_arg, SysStringLen(name_arg), value_arg, SysStringLen(value_arg)));
			SysFreeString(name_arg);
			SysFreeString(value_arg);
		}
	}
	if (!run_active(run))
		return;
	if (!content)
		return;
	table_clear(&run->table);
	if (reader->feature[FEATURE_PREFIXES] == VARIANT_TRUE)
		for (index = 0; index < namespace_count; index++)
		{
			sax_attribute *item = table_add(&run->table);
			if (!item)
				break;
			item->uri = SysAllocString(L"");
			item->local = SysAllocString(L"");
			item->qname = namespaces[index * 2] ? qualified_arg((const xmlChar *)"xmlns", namespaces[index * 2]) : SysAllocString(L"xmlns");
			item->type = SysAllocString(L"CDATA");
			item->value = arg_of(namespaces[index * 2 + 1]);
		}
	for (index = 0; index < attribute_count; index++)
	{
		const xmlChar **entry = attributes + index * 5;
		sax_attribute *item = table_add(&run->table);
		if (!item)
			break;
		item->uri = names ? arg_of(entry[2]) : SysAllocString(L"");
		item->local = names ? arg_of(entry[0]) : SysAllocString(L"");
		item->qname = qualified_arg(entry[1], entry[0]);
		item->type = SysAllocString(declared_type(run, context, local, entry[0], entry[1]));
		item->value = mx_bstr_counted(entry[3], (int)(entry[4] - entry[3]));
	}
	uri_arg = names ? arg_of(uri) : SysAllocString(L"");
	local_arg = names ? arg_of(local) : SysAllocString(L"");
	qname_arg = qualified_arg(prefix, local);
	check_status(run, run->vb ? IVBSAXContentHandler_startElement((IVBSAXContentHandler *)content, &uri_arg, &local_arg, &qname_arg, (IVBSAXAttributes *)&run->vb_attrs)
		: ISAXContentHandler_startElement((ISAXContentHandler *)content, uri_arg, SysStringLen(uri_arg), local_arg, SysStringLen(local_arg), qname_arg, SysStringLen(qname_arg), (ISAXAttributes *)&run->attrs));
	SysFreeString(uri_arg);
	SysFreeString(local_arg);
	SysFreeString(qname_arg);
}

static void XMLCALL on_end_element(void *context, const xmlChar *local, const xmlChar *prefix, const xmlChar *uri)
{
	sax_run *run = run_of(context);
	sax_reader *reader = run->reader;
	IUnknown *content;
	BOOL names = reader->feature[FEATURE_NAMESPACES] == VARIANT_TRUE || reader->version < MX_4;
	int floor;
	if (!run_active(run))
		return;
	flush_pending(run);
	if (in_document(run, context))
	{
		size_t at = main_offset(run);
		/* An end tag of its own reports just inside it; an empty-element tag where it stopped. */
		if (at >= 2 && at <= run->size && !(run->data[at - 2] == '/' && run->data[at - 1] == '>'))
		{
			size_t back = at;
			while (back >= 2 && !(run->data[back - 2] == '<' && run->data[back - 1] == '/'))
				back--;
			report_at(run, back);
		}
		else
			report_at(run, at);
		run->position = at;
	}
	content = slot_of(run, SLOT_CONTENT);
	if (content)
	{
		BSTR uri_arg = names ? arg_of(uri) : SysAllocString(L""), local_arg = names ? arg_of(local) : SysAllocString(L""), qname_arg = qualified_arg(prefix, local);
		check_status(run, run->vb ? IVBSAXContentHandler_endElement((IVBSAXContentHandler *)content, &uri_arg, &local_arg, &qname_arg)
			: ISAXContentHandler_endElement((ISAXContentHandler *)content, uri_arg, SysStringLen(uri_arg), local_arg, SysStringLen(local_arg), qname_arg, SysStringLen(qname_arg)));
		SysFreeString(uri_arg);
		SysFreeString(local_arg);
		SysFreeString(qname_arg);
	}
	floor = run->depth > 0 && run->marks ? run->marks[run->depth] : 0;
	while (run->prefix_count > floor)
	{
		xmlChar *name = run->prefixes[--run->prefix_count];
		if (content && run_active(run))
		{
			BSTR name_arg = arg_of(name);
			check_status(run, run->vb ? IVBSAXContentHandler_endPrefixMapping((IVBSAXContentHandler *)content, &name_arg)
				: ISAXContentHandler_endPrefixMapping((ISAXContentHandler *)content, name_arg, SysStringLen(name_arg)));
			SysFreeString(name_arg);
		}
		xmlFree(name);
	}
	run->depth--;
}

/* --- The document as a whole. --- */

static void XMLCALL on_start_document(void *context)
{
	sax_run *run = run_of(context);
	sax_reader *reader = run->reader;
	xmlParserCtxtPtr parser = context;
	IUnknown *content = slot_of(run, SLOT_CONTENT);
	xmlSAX2StartDocument(context);
	SysFreeString(reader->declared_version);
	reader->declared_version = parser->version ? arg_of(parser->version) : NULL;
	if (content)
	{
		/* A locator refused outright stops things; anything else here is not a failure. */
		HRESULT status = run->vb ? IVBSAXContentHandler_putref_documentLocator((IVBSAXContentHandler *)content, (IVBSAXLocator *)vb_locator_of(run))
			: ISAXContentHandler_putDocumentLocator((ISAXContentHandler *)content, (ISAXLocator *)&run->locator_vtbl);
		if (FAILED(status))
		{
			stop_with(run, status, NULL);
			return;
		}
		check_status(run, run->vb ? IVBSAXContentHandler_startDocument((IVBSAXContentHandler *)content) : ISAXContentHandler_startDocument((ISAXContentHandler *)content));
	}
}

static void XMLCALL on_end_document(void *context)
{
	sax_run *run = run_of(context);
	IUnknown *content = slot_of(run, SLOT_CONTENT);
	xmlSAX2EndDocument(context);
	if (!run_active(run) || !((xmlParserCtxtPtr)context)->wellFormed)
		return;
	run->line = 0;
	run->column = 0;
	if (content)
		check_status(run, run->vb ? IVBSAXContentHandler_endDocument((IVBSAXContentHandler *)content) : ISAXContentHandler_endDocument((ISAXContentHandler *)content));
}

static void XMLCALL on_reference(void *context, const xmlChar *name)
{
	sax_run *run = run_of(context);
	IUnknown *content = slot_of(run, SLOT_CONTENT);
	if (!run_active(run))
		return;
	flush_pending(run);
	if (content)
	{
		BSTR arg = arg_of(name);
		check_status(run, run->vb ? IVBSAXContentHandler_skippedEntity((IVBSAXContentHandler *)content, &arg) : ISAXContentHandler_skippedEntity((ISAXContentHandler *)content, arg, SysStringLen(arg)));
		SysFreeString(arg);
	}
}

/* --- Declarations. --- */

static void XMLCALL on_internal_subset(void *context, const xmlChar *name, const xmlChar *public_id, const xmlChar *system_id)
{
	sax_run *run = run_of(context);
	IUnknown *lexical = slot_of(run, SLOT_LEXICAL);
	xmlSAX2InternalSubset(context, name, public_id, system_id);
	if (run->reader->feature[FEATURE_PROHIBIT_DTD] == VARIANT_TRUE)
	{
		stop_with(run, (HRESULT)0xC00CE5A5, L"DTD is prohibited.\r\n");
		return;
	}
	if (lexical && run_active(run))
	{
		BSTR name_arg = arg_of(name), public_arg = arg_of(public_id), system_arg = arg_of(system_id);
		check_status(run, run->vb ? IVBSAXLexicalHandler_startDTD((IVBSAXLexicalHandler *)lexical, &name_arg, &public_arg, &system_arg)
			: ISAXLexicalHandler_startDTD((ISAXLexicalHandler *)lexical, name_arg, SysStringLen(name_arg), public_arg, SysStringLen(public_arg), system_arg, SysStringLen(system_arg)));
		SysFreeString(name_arg);
		SysFreeString(public_arg);
		SysFreeString(system_arg);
	}
}

/* The parser calls this once the whole doctype has been read, whichever parts it had. */
static void XMLCALL on_external_subset(void *context, const xmlChar *name, const xmlChar *public_id, const xmlChar *system_id)
{
	sax_run *run = run_of(context);
	IUnknown *lexical = slot_of(run, SLOT_LEXICAL);
	if (lexical && run_active(run))
		check_status(run, run->vb ? IVBSAXLexicalHandler_endDTD((IVBSAXLexicalHandler *)lexical) : ISAXLexicalHandler_endDTD((ISAXLexicalHandler *)lexical));
}

static void XMLCALL on_entity_declaration(void *context, const xmlChar *name, int type, const xmlChar *public_id, const xmlChar *system_id, xmlChar *content)
{
	sax_run *run = run_of(context);
	IUnknown *declarations = slot_of(run, SLOT_DECLARATION);
	BOOL parameter = type == XML_INTERNAL_PARAMETER_ENTITY || type == XML_EXTERNAL_PARAMETER_ENTITY;
	xmlSAX2EntityDecl(context, name, type, public_id, system_id, content);
	if (declarations && run_active(run))
	{
		BSTR name_arg = qualified_arg(NULL, name);
		if (parameter)
		{
			xmlChar *marked = xmlStrdup((const xmlChar *)"%");
			marked = xmlStrcat(marked, name);
			SysFreeString(name_arg);
			name_arg = arg_of(marked);
			xmlFree(marked);
		}
		if (type == XML_INTERNAL_GENERAL_ENTITY || type == XML_INTERNAL_PARAMETER_ENTITY)
		{
			BSTR value_arg = arg_of(content);
			check_status(run, run->vb ? IVBSAXDeclHandler_internalEntityDecl((IVBSAXDeclHandler *)declarations, &name_arg, &value_arg)
				: ISAXDeclHandler_internalEntityDecl((ISAXDeclHandler *)declarations, name_arg, SysStringLen(name_arg), value_arg, SysStringLen(value_arg)));
			SysFreeString(value_arg);
		}
		else if (type == XML_EXTERNAL_GENERAL_PARSED_ENTITY || type == XML_EXTERNAL_PARAMETER_ENTITY)
		{
			BSTR public_arg = arg_of(public_id), system_arg = arg_of(system_id);
			check_status(run, run->vb ? IVBSAXDeclHandler_externalEntityDecl((IVBSAXDeclHandler *)declarations, &name_arg, &public_arg, &system_arg)
				: ISAXDeclHandler_externalEntityDecl((ISAXDeclHandler *)declarations, name_arg, SysStringLen(name_arg), public_arg, SysStringLen(public_arg), system_arg, SysStringLen(system_arg)));
			SysFreeString(public_arg);
			SysFreeString(system_arg);
		}
		SysFreeString(name_arg);
	}
}

static void XMLCALL on_element_declaration(void *context, const xmlChar *name, int type, xmlElementContentPtr content)
{
	sax_run *run = run_of(context);
	IUnknown *declarations = slot_of(run, SLOT_DECLARATION);
	if (declarations && run_active(run))
	{
		char model[5000] = "";
		BSTR name_arg = arg_of(name), model_arg;
		if (type == XML_ELEMENT_TYPE_EMPTY)
			strcpy(model, "EMPTY");
		else if (type == XML_ELEMENT_TYPE_ANY)
			strcpy(model, "ANY");
		else if (content)
			xmlSnprintfElementContent(model, sizeof(model), content, 1);
		model_arg = arg_of((const xmlChar *)model);
		check_status(run, run->vb ? IVBSAXDeclHandler_elementDecl((IVBSAXDeclHandler *)declarations, &name_arg, &model_arg)
			: ISAXDeclHandler_elementDecl((ISAXDeclHandler *)declarations, name_arg, SysStringLen(name_arg), model_arg, SysStringLen(model_arg)));
		SysFreeString(name_arg);
		SysFreeString(model_arg);
	}
	xmlSAX2ElementDecl(context, name, type, content);
}

static void XMLCALL on_attribute_declaration(void *context, const xmlChar *element, const xmlChar *name, int type, int use, const xmlChar *value, xmlEnumerationPtr tree)
{
	sax_run *run = run_of(context);
	IUnknown *declarations = slot_of(run, SLOT_DECLARATION);
	if (declarations && run_active(run))
	{
		static const char *const type_names[] = {
			[XML_ATTRIBUTE_CDATA] = "CDATA", [XML_ATTRIBUTE_ID] = "ID", [XML_ATTRIBUTE_IDREF] = "IDREF", [XML_ATTRIBUTE_IDREFS] = "IDREFS",
			[XML_ATTRIBUTE_ENTITY] = "ENTITY", [XML_ATTRIBUTE_ENTITIES] = "ENTITIES", [XML_ATTRIBUTE_NMTOKEN] = "NMTOKEN", [XML_ATTRIBUTE_NMTOKENS] = "NMTOKENS",
			[XML_ATTRIBUTE_ENUMERATION] = "", [XML_ATTRIBUTE_NOTATION] = "NOTATION ",
		};
		xmlChar *kind = xmlStrdup((const xmlChar *)type_names[type]);
		const char *fallback = use == XML_ATTRIBUTE_REQUIRED ? "#REQUIRED" : use == XML_ATTRIBUTE_IMPLIED ? "#IMPLIED" : use == XML_ATTRIBUTE_FIXED ? "#FIXED" : "";
		BSTR element_arg = arg_of(element), name_arg = arg_of(name), kind_arg, fallback_arg = arg_of((const xmlChar *)fallback), value_arg = arg_of(value);
		if (tree)
		{
			xmlEnumerationPtr item;
			kind = xmlStrcat(kind, (const xmlChar *)"(");
			for (item = tree; item; item = item->next)
			{
				kind = xmlStrcat(kind, item->name);
				if (item->next)
					kind = xmlStrcat(kind, (const xmlChar *)"|");
			}
			kind = xmlStrcat(kind, (const xmlChar *)")");
		}
		kind_arg = arg_of(kind);
		xmlFree(kind);
		check_status(run, run->vb ? IVBSAXDeclHandler_attributeDecl((IVBSAXDeclHandler *)declarations, &element_arg, &name_arg, &kind_arg, &fallback_arg, &value_arg)
			: ISAXDeclHandler_attributeDecl((ISAXDeclHandler *)declarations, element_arg, SysStringLen(element_arg), name_arg, SysStringLen(name_arg), kind_arg, SysStringLen(kind_arg), fallback_arg, SysStringLen(fallback_arg), value_arg, SysStringLen(value_arg)));
		SysFreeString(element_arg);
		SysFreeString(name_arg);
		SysFreeString(kind_arg);
		SysFreeString(fallback_arg);
		SysFreeString(value_arg);
	}
	xmlSAX2AttributeDecl(context, element, name, type, use, value, tree);
}

static void XMLCALL on_notation_declaration(void *context, const xmlChar *name, const xmlChar *public_id, const xmlChar *system_id)
{
	sax_run *run = run_of(context);
	IUnknown *handler = slot_of(run, SLOT_DTD);
	xmlSAX2NotationDecl(context, name, public_id, system_id);
	if (handler && run_active(run))
	{
		BSTR name_arg = arg_of(name), public_arg = arg_of(public_id), system_arg = arg_of(system_id);
		check_status(run, run->vb ? IVBSAXDTDHandler_notationDecl((IVBSAXDTDHandler *)handler, &name_arg, &public_arg, &system_arg)
			: ISAXDTDHandler_notationDecl((ISAXDTDHandler *)handler, name_arg, SysStringLen(name_arg), public_arg, SysStringLen(public_arg), system_arg, SysStringLen(system_arg)));
		SysFreeString(name_arg);
		SysFreeString(public_arg);
		SysFreeString(system_arg);
	}
}

static void XMLCALL on_unparsed_declaration(void *context, const xmlChar *name, const xmlChar *public_id, const xmlChar *system_id, const xmlChar *notation)
{
	sax_run *run = run_of(context);
	IUnknown *handler = slot_of(run, SLOT_DTD);
	xmlSAX2UnparsedEntityDecl(context, name, public_id, system_id, notation);
	if (handler && run_active(run))
	{
		BSTR name_arg = arg_of(name), public_arg = arg_of(public_id), system_arg = arg_of(system_id), notation_arg = arg_of(notation);
		check_status(run, run->vb ? IVBSAXDTDHandler_unparsedEntityDecl((IVBSAXDTDHandler *)handler, &name_arg, &public_arg, &system_arg, &notation_arg)
			: ISAXDTDHandler_unparsedEntityDecl((ISAXDTDHandler *)handler, name_arg, SysStringLen(name_arg), public_arg, SysStringLen(public_arg), system_arg, SysStringLen(system_arg), notation_arg, SysStringLen(notation_arg)));
		SysFreeString(name_arg);
		SysFreeString(public_arg);
		SysFreeString(system_arg);
		SysFreeString(notation_arg);
	}
}

/* Warnings and complaints about what a DTD declares are the parser's own business. Anything worse ends the parse with the code a document would report. */
static void XMLCALL on_error(void *context, const xmlError *error)
{
	sax_run *run = run_of(context);
	mx_error_info info = {0};
	if (!run || error->level < XML_ERR_ERROR || error->domain == XML_FROM_VALID || error->domain == XML_FROM_DTD || !run_active(run))
		return;
	flush_pending(run);
	if (!run_active(run))
		return;
	report_at(run, main_offset(run));
	mx_error_from_libxml(&info, error, NULL);
	stop_with(run, info.code < 0 ? (HRESULT)info.code : E_FAIL, info.reason);
	mx_error_clear(&info);
}

/* Run the parser over text already in UTF-8. */
static HRESULT parse_text(sax_reader *reader, BOOL vb, const char *data, size_t size, const WCHAR *url)
{
	sax_run *run;
	xmlParserCtxtPtr context;
	xmlDocPtr tree;
	HRESULT status;
	if (reader->max_size && size > (size_t)reader->max_size * 1024)
		return E_ABORT;
	run = mx_zalloc(sizeof(*run));
	if (!run)
		return E_OUTOFMEMORY;
	context = xmlNewParserCtxt();
	if (!context)
	{
		mx_free(run);
		return E_OUTOFMEMORY;
	}
	run->locator_vtbl = &locator_table;
	run->vb_locator_vtbl = &vb_locator_table;
	run->owner_vtbl = &owner_table;
	run->refs = 1;
	run->attrs.vtbl = &attr_table_face;
	run->attrs.table = &run->table;
	run->attrs.owner = run_owner(run);
	run->vb_attrs.vtbl = &vb_attr_table_face;
	run->vb_attrs.table = &run->table;
	run->vb_attrs.owner = run_owner(run);
	run->reader = reader;
	run->vb = vb;
	run->context = context;
	run->current = context;
	run->data = data;
	run->size = size;
	run->scan_line = 1;
	run->scan_column = 1;
	context->_private = run;
	context->sax->startDocument = on_start_document;
	context->sax->endDocument = on_end_document;
	context->sax->startElementNs = on_start_element;
	context->sax->endElementNs = on_end_element;
	context->sax->characters = on_characters;
	context->sax->ignorableWhitespace = on_characters;
	context->sax->cdataBlock = on_cdata;
	context->sax->comment = on_comment;
	context->sax->processingInstruction = on_instruction;
	context->sax->reference = on_reference;
	context->sax->internalSubset = on_internal_subset;
	context->sax->externalSubset = on_external_subset;
	context->sax->entityDecl = on_entity_declaration;
	context->sax->elementDecl = on_element_declaration;
	context->sax->attributeDecl = on_attribute_declaration;
	context->sax->notationDecl = on_notation_declaration;
	context->sax->unparsedEntityDecl = on_unparsed_declaration;
	context->sax->serror = on_error;
	tree = xmlCtxtReadMemory(context, data, (int)size, NULL, "UTF-8",
		XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET | XML_PARSE_NOENT | XML_PARSE_IGNORE_ENC | XML_PARSE_BIG_LINES);
	if (tree)
		xmlFreeDoc(tree);
	if (run->status == S_OK && !context->wellFormed)
		run->status = E_FAIL;
	status = run->status;
	/* A locator kept past the parse answers without the reader or the parser behind it. */
	run->reader = NULL;
	run->context = NULL;
	run->current = NULL;
	run->line = 0;
	run->column = 0;
	xmlFreeParserCtxt(context);
	run_release(run);
	return status;
}

static HRESULT parse_wide(sax_reader *reader, BOOL vb, const WCHAR *text, int length)
{
	xmlChar *utf8 = mx_to_utf8_counted(text, length);
	HRESULT status;
	if (!utf8)
		return E_OUTOFMEMORY;
	status = parse_text(reader, vb, (const char *)utf8, xmlStrlen(utf8), NULL);
	xmlFree(utf8);
	return status;
}

static HRESULT parse_bytes(sax_reader *reader, BOOL vb, const char *data, ULONG size, const WCHAR *url)
{
	BOOL unknown = FALSE;
	int decoded_size = 0;
	char *decoded = mx_decode_input(data, (int)size, &decoded_size, &unknown);
	HRESULT status;
	if (!decoded)
		return unknown ? (HRESULT)0xC00CE56F : E_OUTOFMEMORY;
	status = parse_text(reader, vb, decoded, decoded_size, url);
	mx_free(decoded);
	return status;
}

static HRESULT parse_input(sax_reader *reader, BOOL vb, const VARIANT *input)
{
	const VARIANT *from = input;
	HRESULT status = E_INVALIDARG;
	if (reader->busy)
		return E_FAIL;
	while (V_VT(from) == (VT_VARIANT | VT_BYREF))
		from = V_VARIANTREF(from);
	reader->busy = TRUE;
	if (V_VT(from) == VT_BSTR)
		status = parse_wide(reader, vb, V_BSTR(from), SysStringLen(V_BSTR(from)));
	else if (V_VT(from) == (VT_BSTR | VT_BYREF))
		status = parse_wide(reader, vb, *V_BSTRREF(from), SysStringLen(*V_BSTRREF(from)));
	else if (V_VT(from) == (VT_ARRAY | VT_UI1))
	{
		SAFEARRAY *array = V_ARRAY(from);
		LONG low = 0, high = -1;
		void *bytes;
		SafeArrayGetLBound(array, 1, &low);
		SafeArrayGetUBound(array, 1, &high);
		status = SafeArrayAccessData(array, &bytes);
		if (SUCCEEDED(status))
		{
			status = parse_bytes(reader, vb, bytes, (ULONG)(high - low + 1), NULL);
			SafeArrayUnaccessData(array);
		}
	}
	else if ((V_VT(from) == VT_UNKNOWN || V_VT(from) == VT_DISPATCH) && V_UNKNOWN(from))
	{
		IXMLDOMDocument *document;
		IStream *stream;
		if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(from), &IID_IXMLDOMDocument, (void **)&document)))
		{
			BSTR markup = NULL;
			status = IXMLDOMDocument_get_xml(document, &markup);
			if (SUCCEEDED(status))
				status = parse_wide(reader, vb, markup, SysStringLen(markup));
			SysFreeString(markup);
			IXMLDOMDocument_Release(document);
		}
		else if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(from), &IID_IStream, (void **)&stream))
			|| SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(from), &IID_ISequentialStream, (void **)&stream)))
		{
			char *data = NULL;
			ULONG size = 0;
			status = mx_read_stream(stream, &data, &size);
			if (SUCCEEDED(status))
			{
				status = parse_bytes(reader, vb, data, size, NULL);
				mx_free(data);
			}
			IStream_Release(stream);
		}
	}
	reader->busy = FALSE;
	return status;
}

static HRESULT parse_location(sax_reader *reader, BOOL vb, const WCHAR *url)
{
	char *data = NULL;
	ULONG size = 0;
	HRESULT status;
	if (!url)
		return E_INVALIDARG;
	if (reader->busy)
		return E_FAIL;
	status = mx_read_url(url, &data, &size);
	if (FAILED(status))
		return status;
	reader->busy = TRUE;
	status = parse_bytes(reader, vb, data, size, url);
	reader->busy = FALSE;
	mx_free(data);
	return status;
}

/* --- Settings. --- */

static int feature_index(const WCHAR *name)
{
	int index;
	if (name)
		for (index = 0; index < FEATURE_COUNT; index++)
			if (!wcscmp(name, features[index].name))
				return index;
	return -1;
}

static HRESULT get_feature(sax_reader *self, const WCHAR *name, VARIANT_BOOL *out)
{
	int index = feature_index(name);
	if (!out)
		return E_POINTER;
	if (index < 0)
		return E_INVALIDARG;
	*out = self->feature[index];
	return S_OK;
}

static HRESULT put_feature(sax_reader *self, const WCHAR *name, VARIANT_BOOL value)
{
	int index = feature_index(name);
	if (index < 0)
		return E_INVALIDARG;
	self->feature[index] = value == VARIANT_TRUE ? VARIANT_TRUE : VARIANT_FALSE;
	return S_OK;
}

static void set_handler(sax_reader *self, BOOL vb, handler_slot slot, IUnknown *handler)
{
	IUnknown *old = self->handlers[vb ? 1 : 0][slot];
	if (handler)
		IUnknown_AddRef(handler);
	self->handlers[vb ? 1 : 0][slot] = handler;
	if (old)
		IUnknown_Release(old);
}

static HRESULT get_handler(sax_reader *self, BOOL vb, handler_slot slot, void **out)
{
	if (!out)
		return E_POINTER;
	*out = self->handlers[vb ? 1 : 0][slot];
	if (*out)
		IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static HRESULT put_size_property(LONG *field, const VARIANT *value, LONG limit)
{
	VARIANT number;
	HRESULT status;
	VariantInit(&number);
	status = VariantChangeType(&number, value, 0, VT_I4);
	if (FAILED(status))
		return status;
	if (V_I4(&number) < 0 || (limit && V_I4(&number) >= limit))
		return E_INVALIDARG;
	*field = V_I4(&number);
	return S_OK;
}

static HRESULT get_property(sax_reader *self, BOOL vb, const WCHAR *name, VARIANT *out)
{
	handler_slot slot;
	if (!out)
		return E_POINTER;
	if (!name)
		return E_INVALIDARG;
	if (!wcscmp(name, L"max-xml-size") || !wcscmp(name, L"max-element-depth"))
	{
		V_VT(out) = VT_I4;
		V_I4(out) = name[4] == 'x' ? self->max_size : self->max_depth;
		return S_OK;
	}
	if (!wcscmp(name, L"xmldecl-version") || !wcscmp(name, L"xmldecl-encoding"))
	{
		BSTR have = name[8] == 'v' ? self->declared_version : self->declared_encoding;
		V_VT(out) = VT_BSTR;
		V_BSTR(out) = SysAllocStringLen(have, SysStringLen(have));
		return S_OK;
	}
	if (!wcscmp(name, L"http://xml.org/sax/properties/lexical-handler"))
		slot = SLOT_LEXICAL;
	else if (!wcscmp(name, L"http://xml.org/sax/properties/declaration-handler"))
		slot = SLOT_DECLARATION;
	else
		return E_INVALIDARG;
	V_VT(out) = vb ? VT_DISPATCH : VT_UNKNOWN;
	return get_handler(self, vb, slot, (void **)&V_UNKNOWN(out));
}

static HRESULT put_property(sax_reader *self, BOOL vb, const WCHAR *name, const VARIANT *value)
{
	const VARIANT *from = value;
	const IID *wanted;
	handler_slot slot;
	IUnknown *handler = NULL;
	if (!name)
		return E_INVALIDARG;
	if (!wcscmp(name, L"max-xml-size"))
		return put_size_property(&self->max_size, value, 4194304);
	if (!wcscmp(name, L"max-element-depth"))
		return put_size_property(&self->max_depth, value, 0);
	if (!wcscmp(name, L"http://xml.org/sax/properties/lexical-handler"))
	{
		slot = SLOT_LEXICAL;
		wanted = vb ? &IID_IVBSAXLexicalHandler : &IID_ISAXLexicalHandler;
	}
	else if (!wcscmp(name, L"http://xml.org/sax/properties/declaration-handler"))
	{
		slot = SLOT_DECLARATION;
		wanted = vb ? &IID_IVBSAXDeclHandler : &IID_ISAXDeclHandler;
	}
	else
		return E_INVALIDARG;
	while (V_VT(from) == (VT_VARIANT | VT_BYREF))
		from = V_VARIANTREF(from);
	if (V_VT(from) == VT_UNKNOWN || V_VT(from) == VT_DISPATCH)
	{
		if (V_UNKNOWN(from))
		{
			HRESULT status = IUnknown_QueryInterface(V_UNKNOWN(from), wanted, (void **)&handler);
			if (FAILED(status))
				return status;
		}
	}
	else if (V_VT(from) != VT_EMPTY)
		return E_INVALIDARG;
	set_handler(self, vb, slot, handler);
	if (handler)
		IUnknown_Release(handler);
	return S_OK;
}

static HRESULT replace_url(WCHAR **field, const WCHAR *url)
{
	WCHAR *copy = NULL;
	if (url)
	{
		SIZE_T size = (lstrlenW(url) + 1) * sizeof(WCHAR);
		copy = mx_alloc(size);
		if (!copy)
			return E_OUTOFMEMORY;
		memcpy(copy, url, size);
	}
	mx_free(*field);
	*field = copy;
	return S_OK;
}

/* --- The native face. --- */

static HRESULT WINAPI reader_QueryInterface(ISAXXMLReader *iface, REFIID riid, void **out)
{
	sax_reader *self = (sax_reader *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ISAXXMLReader))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IVBSAXXMLReader))
		*out = &self->vb_vtbl;
	else if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &self->dispex;
	else
		return E_NOINTERFACE;
	ISAXXMLReader_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI reader_AddRef(ISAXXMLReader *iface)
{
	return InterlockedIncrement(&((sax_reader *)iface)->refs);
}

static ULONG WINAPI reader_Release(ISAXXMLReader *iface)
{
	sax_reader *self = (sax_reader *)iface;
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		int side, slot;
		for (side = 0; side < 2; side++)
			for (slot = 0; slot < SLOT_COUNT; slot++)
				if (self->handlers[side][slot])
					IUnknown_Release(self->handlers[side][slot]);
		mx_free(self->base_url);
		mx_free(self->secure_url);
		SysFreeString(self->declared_version);
		SysFreeString(self->declared_encoding);
		mx_free(self);
	}
	return left;
}

static HRESULT WINAPI reader_getFeature(ISAXXMLReader *iface, const WCHAR *name, VARIANT_BOOL *out)
{
	return get_feature((sax_reader *)iface, name, out);
}

static HRESULT WINAPI reader_putFeature(ISAXXMLReader *iface, const WCHAR *name, VARIANT_BOOL value)
{
	return put_feature((sax_reader *)iface, name, value);
}

static HRESULT WINAPI reader_getProperty(ISAXXMLReader *iface, const WCHAR *name, VARIANT *out)
{
	return get_property((sax_reader *)iface, FALSE, name, out);
}

static HRESULT WINAPI reader_putProperty(ISAXXMLReader *iface, const WCHAR *name, VARIANT value)
{
	return put_property((sax_reader *)iface, FALSE, name, &value);
}

#define NATIVE_HANDLER(get, put, type, slot) \
	static HRESULT WINAPI reader_##get(ISAXXMLReader *iface, type **out) \
	{ \
		return get_handler((sax_reader *)iface, FALSE, slot, (void **)out); \
	} \
	static HRESULT WINAPI reader_##put(ISAXXMLReader *iface, type *handler) \
	{ \
		set_handler((sax_reader *)iface, FALSE, slot, (IUnknown *)handler); \
		return S_OK; \
	}

NATIVE_HANDLER(getEntityResolver, putEntityResolver, ISAXEntityResolver, SLOT_RESOLVER)
NATIVE_HANDLER(getContentHandler, putContentHandler, ISAXContentHandler, SLOT_CONTENT)
NATIVE_HANDLER(getDTDHandler, putDTDHandler, ISAXDTDHandler, SLOT_DTD)
NATIVE_HANDLER(getErrorHandler, putErrorHandler, ISAXErrorHandler, SLOT_ERROR)

static HRESULT WINAPI reader_getBaseURL(ISAXXMLReader *iface, const WCHAR **out)
{
	if (!out)
		return E_POINTER;
	*out = ((sax_reader *)iface)->base_url;
	return S_OK;
}

static HRESULT WINAPI reader_putBaseURL(ISAXXMLReader *iface, const WCHAR *url)
{
	return replace_url(&((sax_reader *)iface)->base_url, url);
}

static HRESULT WINAPI reader_getSecureBaseURL(ISAXXMLReader *iface, const WCHAR **out)
{
	if (!out)
		return E_POINTER;
	*out = ((sax_reader *)iface)->secure_url;
	return S_OK;
}

static HRESULT WINAPI reader_putSecureBaseURL(ISAXXMLReader *iface, const WCHAR *url)
{
	return replace_url(&((sax_reader *)iface)->secure_url, url);
}

static HRESULT WINAPI reader_parse(ISAXXMLReader *iface, VARIANT input)
{
	return parse_input((sax_reader *)iface, FALSE, &input);
}

static HRESULT WINAPI reader_parseURL(ISAXXMLReader *iface, const WCHAR *url)
{
	return parse_location((sax_reader *)iface, FALSE, url);
}

static const ISAXXMLReaderVtbl reader_table = {
	reader_QueryInterface, reader_AddRef, reader_Release, reader_getFeature, reader_putFeature, reader_getProperty, reader_putProperty,
	reader_getEntityResolver, reader_putEntityResolver, reader_getContentHandler, reader_putContentHandler,
	reader_getDTDHandler, reader_putDTDHandler, reader_getErrorHandler, reader_putErrorHandler,
	reader_getBaseURL, reader_putBaseURL, reader_getSecureBaseURL, reader_putSecureBaseURL, reader_parse, reader_parseURL,
};

/* --- The face for scripts. --- */

static HRESULT WINAPI vb_reader_QueryInterface(IVBSAXXMLReader *iface, REFIID riid, void **out)
{
	return reader_QueryInterface((ISAXXMLReader *)READER_OF_VB(iface), riid, out);
}

static ULONG WINAPI vb_reader_AddRef(IVBSAXXMLReader *iface)
{
	return reader_AddRef((ISAXXMLReader *)READER_OF_VB(iface));
}

static ULONG WINAPI vb_reader_Release(IVBSAXXMLReader *iface)
{
	return reader_Release((ISAXXMLReader *)READER_OF_VB(iface));
}

static HRESULT WINAPI vb_reader_GetTypeInfoCount(IVBSAXXMLReader *iface, UINT *count)
{
	*count = 1;
	return S_OK;
}

static HRESULT WINAPI vb_reader_GetTypeInfo(IVBSAXXMLReader *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	return mx_type_info(MX_TYPE_VB_READER, out);
}

static HRESULT WINAPI vb_reader_GetIDsOfNames(IVBSAXXMLReader *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_VB_READER, riid, names, count, lcid, ids);
}

/* A handler is a reference, yet scripts may set it as a plain property too. */
static HRESULT WINAPI vb_reader_Invoke(IVBSAXXMLReader *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	if (flags == DISPATCH_PROPERTYPUT && (id == DISPID_SAX_XMLREADER_CONTENTHANDLER || id == DISPID_SAX_XMLREADER_DTDHANDLER
		|| id == DISPID_SAX_XMLREADER_ERRORHANDLER || id == DISPID_SAX_XMLREADER_ENTITYRESOLVER))
		flags = DISPATCH_PROPERTYPUTREF;
	return mx_invoke(MX_TYPE_VB_READER, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI vb_reader_getFeature(IVBSAXXMLReader *iface, BSTR name, VARIANT_BOOL *out)
{
	return get_feature(READER_OF_VB(iface), name, out);
}

static HRESULT WINAPI vb_reader_putFeature(IVBSAXXMLReader *iface, BSTR name, VARIANT_BOOL value)
{
	return put_feature(READER_OF_VB(iface), name, value);
}

static HRESULT WINAPI vb_reader_getProperty(IVBSAXXMLReader *iface, BSTR name, VARIANT *out)
{
	return get_property(READER_OF_VB(iface), TRUE, name, out);
}

static HRESULT WINAPI vb_reader_putProperty(IVBSAXXMLReader *iface, BSTR name, VARIANT value)
{
	return put_property(READER_OF_VB(iface), TRUE, name, &value);
}

#define VB_HANDLER(get, put, type, slot) \
	static HRESULT WINAPI vb_reader_##get(IVBSAXXMLReader *iface, type **out) \
	{ \
		return get_handler(READER_OF_VB(iface), TRUE, slot, (void **)out); \
	} \
	static HRESULT WINAPI vb_reader_##put(IVBSAXXMLReader *iface, type *handler) \
	{ \
		set_handler(READER_OF_VB(iface), TRUE, slot, (IUnknown *)handler); \
		return S_OK; \
	}

VB_HANDLER(get_entityResolver, putref_entityResolver, IVBSAXEntityResolver, SLOT_RESOLVER)
VB_HANDLER(get_contentHandler, putref_contentHandler, IVBSAXContentHandler, SLOT_CONTENT)
VB_HANDLER(get_dtdHandler, putref_dtdHandler, IVBSAXDTDHandler, SLOT_DTD)
VB_HANDLER(get_errorHandler, putref_errorHandler, IVBSAXErrorHandler, SLOT_ERROR)

static HRESULT WINAPI vb_reader_get_baseURL(IVBSAXXMLReader *iface, BSTR *out)
{
	WCHAR *url = READER_OF_VB(iface)->base_url;
	if (!out)
		return E_POINTER;
	*out = SysAllocString(url ? url : L"");
	return S_OK;
}

static HRESULT WINAPI vb_reader_put_baseURL(IVBSAXXMLReader *iface, BSTR url)
{
	return replace_url(&READER_OF_VB(iface)->base_url, url);
}

static HRESULT WINAPI vb_reader_get_secureBaseURL(IVBSAXXMLReader *iface, BSTR *out)
{
	WCHAR *url = READER_OF_VB(iface)->secure_url;
	if (!out)
		return E_POINTER;
	*out = SysAllocString(url ? url : L"");
	return S_OK;
}

static HRESULT WINAPI vb_reader_put_secureBaseURL(IVBSAXXMLReader *iface, BSTR url)
{
	return replace_url(&READER_OF_VB(iface)->secure_url, url);
}

static HRESULT WINAPI vb_reader_parse(IVBSAXXMLReader *iface, VARIANT input)
{
	return parse_input(READER_OF_VB(iface), TRUE, &input);
}

static HRESULT WINAPI vb_reader_parseURL(IVBSAXXMLReader *iface, BSTR url)
{
	return parse_location(READER_OF_VB(iface), TRUE, url);
}

static const IVBSAXXMLReaderVtbl vb_reader_table = {
	vb_reader_QueryInterface, vb_reader_AddRef, vb_reader_Release, vb_reader_GetTypeInfoCount, vb_reader_GetTypeInfo, vb_reader_GetIDsOfNames, vb_reader_Invoke,
	vb_reader_getFeature, vb_reader_putFeature, vb_reader_getProperty, vb_reader_putProperty,
	vb_reader_get_entityResolver, vb_reader_putref_entityResolver, vb_reader_get_contentHandler, vb_reader_putref_contentHandler,
	vb_reader_get_dtdHandler, vb_reader_putref_dtdHandler, vb_reader_get_errorHandler, vb_reader_putref_errorHandler,
	vb_reader_get_baseURL, vb_reader_put_baseURL, vb_reader_get_secureBaseURL, vb_reader_put_secureBaseURL, vb_reader_parse, vb_reader_parseURL,
};

HRESULT mx_sax_reader_create(mx_version version, REFIID riid, void **out)
{
	sax_reader *self = mx_zalloc(sizeof(*self));
	HRESULT status;
	int index;
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &reader_table;
	self->vb_vtbl = &vb_reader_table;
	mx_dispex_init(&self->dispex, &self->vb_vtbl, FALSE);
	self->refs = 1;
	self->version = version;
	self->max_depth = version >= MX_6 ? 256 : 5000;
	for (index = 0; index < FEATURE_COUNT; index++)
		self->feature[index] = features[index].start;
	status = ISAXXMLReader_QueryInterface((ISAXXMLReader *)self, riid, out);
	ISAXXMLReader_Release((ISAXXMLReader *)self);
	return status;
}
