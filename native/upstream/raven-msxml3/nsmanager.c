/* SPDX-License-Identifier: MIT */
#include "node.h"

/* Contexts on a stack, each holding the prefixes declared at its level. The manager carries a native face (wide strings
 * with a caller's buffer) and an automation one (BSTRs), over the same state. */
typedef struct binding
{
	struct binding *next;
	BSTR prefix, uri;
} binding;

typedef struct context
{
	struct context *below;
	binding *bindings;
} context;

typedef struct
{
	const IMXNamespaceManagerVtbl *vtbl;
	const IVBMXNamespaceManagerVtbl *vb_vtbl;
	mx_dispex dispex;
	LONG refs;
	context *top;
	VARIANT_BOOL override;
} manager;

static manager *from_native(IMXNamespaceManager *iface)
{
	return (manager *)iface;
}

static manager *from_vb(IVBMXNamespaceManager *iface)
{
	return CONTAINING_RECORD(iface, manager, vb_vtbl);
}

static const WCHAR xml_prefix[] = L"xml";
static const WCHAR xml_uri[] = L"http://www.w3.org/XML/1998/namespace";
static const WCHAR xmlns_prefix[] = L"xmlns";

static binding *new_binding(context *level, const WCHAR *prefix, const WCHAR *uri)
{
	binding *at = mx_alloc(sizeof(*at));
	if (!at)
		return NULL;
	at->prefix = SysAllocString(prefix);
	at->uri = SysAllocString(uri);
	/* The newest comes first, except that "xml" stays ahead of everything. */
	if (level->bindings && !lstrcmpW(level->bindings->prefix, xml_prefix))
	{
		at->next = level->bindings->next;
		level->bindings->next = at;
	}
	else
	{
		at->next = level->bindings;
		level->bindings = at;
	}
	return at;
}

static context *new_context(context *below)
{
	context *level = mx_zalloc(sizeof(*level));
	if (level)
		level->below = below;
	return level;
}

static void free_context(context *level)
{
	binding *at;
	while ((at = level->bindings))
	{
		level->bindings = at->next;
		SysFreeString(at->prefix);
		SysFreeString(at->uri);
		mx_free(at);
	}
	mx_free(level);
}

static const binding *find_in_level(const context *level, const WCHAR *prefix)
{
	const binding *at;
	for (at = level->bindings; at; at = at->next)
		if (!lstrcmpW(at->prefix, prefix))
			return at;
	return NULL;
}

static const WCHAR *uri_of_prefix(const manager *self, const WCHAR *prefix)
{
	const context *level;
	for (level = self->top; level; level = level->below)
	{
		const binding *found = find_in_level(level, prefix);
		if (found)
			return found->uri;
	}
	return NULL;
}

static HRESULT WINAPI native_QueryInterface(IMXNamespaceManager *iface, REFIID riid, void **out)
{
	manager *self = from_native(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IMXNamespaceManager))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IVBMXNamespaceManager))
		*out = &self->vb_vtbl;
	else if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &self->dispex;
	else
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI native_AddRef(IMXNamespaceManager *iface)
{
	return InterlockedIncrement(&from_native(iface)->refs);
}

static ULONG WINAPI native_Release(IMXNamespaceManager *iface)
{
	manager *self = from_native(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	while (self->top)
	{
		context *level = self->top;
		self->top = level->below;
		free_context(level);
	}
	mx_free(self);
	return 0;
}

static HRESULT WINAPI native_putAllowOverride(IMXNamespaceManager *iface, VARIANT_BOOL value)
{
	from_native(iface)->override = value;
	return S_OK;
}

static HRESULT WINAPI native_getAllowOverride(IMXNamespaceManager *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = from_native(iface)->override;
	return S_OK;
}

static HRESULT WINAPI native_reset(IMXNamespaceManager *iface)
{
	manager *self = from_native(iface);
	while (self->top && self->top->below)
	{
		context *level = self->top;
		self->top = level->below;
		free_context(level);
	}
	if (self->top)
	{
		context *fresh = new_context(NULL);
		if (fresh)
		{
			new_binding(fresh, xml_prefix, xml_uri);
			free_context(self->top);
			self->top = fresh;
		}
	}
	return S_OK;
}

static HRESULT WINAPI native_pushContext(IMXNamespaceManager *iface)
{
	manager *self = from_native(iface);
	context *level = new_context(self->top);
	if (!level)
		return E_OUTOFMEMORY;
	self->top = level;
	return S_OK;
}

static HRESULT WINAPI native_pushNodeContext(IMXNamespaceManager *iface, IXMLDOMNode *node, VARIANT_BOOL deep)
{
	manager *self = from_native(iface);
	xmlNodePtr given;
	HRESULT status;
	if (!node)
		return E_INVALIDARG;
	given = mx_node_get((IUnknown *)node);
	if (!given)
		return E_INVALIDARG;
	status = native_pushContext(iface);
	if (FAILED(status))
		return status;
	for (; given && given->type == XML_ELEMENT_NODE; given = deep ? given->parent : NULL)
	{
		xmlNsPtr space;
		for (space = given->nsDef; space; space = space->next)
		{
			WCHAR *prefix = space->prefix ? mx_to_wide(space->prefix) : NULL;
			WCHAR *uri = mx_to_wide(space->href ? space->href : (const xmlChar *)"");
			if (!find_in_level(self->top, prefix ? prefix : L""))
				new_binding(self->top, prefix ? prefix : L"", uri);
			mx_free(prefix);
			mx_free(uri);
		}
	}
	return S_OK;
}

static HRESULT WINAPI native_popContext(IMXNamespaceManager *iface)
{
	manager *self = from_native(iface);
	context *level = self->top;
	if (!level->below)
		return E_FAIL;
	self->top = level->below;
	free_context(level);
	return S_OK;
}

static HRESULT WINAPI native_declarePrefix(IMXNamespaceManager *iface, const WCHAR *prefix, const WCHAR *uri)
{
	manager *self = from_native(iface);
	const WCHAR *name = prefix ? prefix : L"";
	binding *found;
	if (!uri)
	{
		if (*name)
			return E_INVALIDARG;
		uri = L"";
	}
	if (!lstrcmpW(name, xml_prefix) || !lstrcmpW(name, xmlns_prefix) || !lstrcmpW(uri, xml_uri))
		return E_INVALIDARG;
	found = (binding *)find_in_level(self->top, name);
	if (found)
	{
		if (!self->override)
			return E_FAIL;
		SysFreeString(found->uri);
		found->uri = SysAllocString(uri);
		return S_FALSE;
	}
	return new_binding(self->top, name, uri) ? S_OK : E_OUTOFMEMORY;
}

/* The prefixes in scope, the nearest first and each only once, with the numbering that shows them to a caller. */
static HRESULT prefix_at(const manager *self, LONG index, const WCHAR **out)
{
	const context *level;
	if (index < 0)
		return E_INVALIDARG;
	for (level = self->top; level; level = level->below)
	{
		const binding *at;
		for (at = level->bindings; at; at = at->next)
		{
			const context *nearer;
			BOOL hidden = FALSE;
			for (nearer = self->top; nearer != level && !hidden; nearer = nearer->below)
				hidden = find_in_level(nearer, at->prefix) != NULL;
			if (hidden)
				continue;
			if (!index--)
			{
				*out = at->prefix;
				return S_OK;
			}
		}
	}
	return E_FAIL;
}

/* A caller's buffer takes the text and the count; too small a buffer is told so and no text goes in. */
static HRESULT give_text(const WCHAR *text, WCHAR *buffer, int *size)
{
	int length = lstrlenW(text);
	if (buffer)
	{
		if (*size <= length)
			return E_XML_BUFFERTOOSMALL;
		memcpy(buffer, text, (length + 1) * sizeof(WCHAR));
	}
	*size = length;
	return S_OK;
}

static HRESULT WINAPI native_getDeclaredPrefix(IMXNamespaceManager *iface, LONG index, WCHAR *prefix, int *size)
{
	manager *self = from_native(iface);
	const WCHAR *found;
	HRESULT status;
	if (!size)
		return E_POINTER;
	status = prefix_at(self, index, &found);
	return status == S_OK ? give_text(found, prefix, size) : status;
}

static HRESULT WINAPI native_getPrefix(IMXNamespaceManager *iface, const WCHAR *uri, LONG index, WCHAR *prefix, int *size)
{
	manager *self = from_native(iface);
	const context *level;
	if (!uri || !*uri || !size)
		return E_INVALIDARG;
	if (index < 0)
		return E_INVALIDARG;
	for (level = self->top; level; level = level->below)
	{
		const binding *at;
		for (at = level->bindings; at; at = at->next)
		{
			const WCHAR *current;
			if (lstrcmpW(at->uri, uri))
				continue;
			current = uri_of_prefix(self, at->prefix);
			if (current && !lstrcmpW(current, uri) && !index--)
				return give_text(at->prefix, prefix, size);
		}
	}
	return E_FAIL;
}

static HRESULT WINAPI native_getURI(IMXNamespaceManager *iface, const WCHAR *prefix, IXMLDOMNode *node, WCHAR *uri, int *size)
{
	manager *self = from_native(iface);
	const WCHAR *found = NULL;
	if (!prefix)
		return E_INVALIDARG;
	if (!size)
		return E_POINTER;
	if (node)
	{
		xmlNodePtr given = mx_node_get((IUnknown *)node);
		xmlChar *utf8 = *prefix ? mx_to_utf8(prefix) : NULL;
		xmlNsPtr space = given && given->doc ? xmlSearchNs(given->doc, given, utf8) : NULL;
		HRESULT status = S_FALSE;
		xmlFree(utf8);
		if (space && space->href)
		{
			WCHAR *wide = mx_to_wide(space->href);
			status = give_text(wide, uri, size);
			mx_free(wide);
		}
		else if (uri)
			*uri = 0;
		return status;
	}
	if (!lstrcmpW(prefix, xmlns_prefix))
		return E_INVALIDARG;
	found = uri_of_prefix(self, prefix);
	if (!found)
	{
		if (uri)
			*uri = 0;
		*size = 0;
		return S_FALSE;
	}
	return give_text(found, uri, size);
}

static const IMXNamespaceManagerVtbl native_table = {
	native_QueryInterface, native_AddRef, native_Release, native_putAllowOverride, native_getAllowOverride, native_reset, native_pushContext,
	native_pushNodeContext, native_popContext, native_declarePrefix, native_getDeclaredPrefix, native_getPrefix, native_getURI,
};

/* --- The list of prefixes the automation face hands out. --- */

typedef struct
{
	const IMXNamespacePrefixesVtbl *vtbl;
	mx_dispex dispex;
	LONG refs;
	BSTR *items;
	LONG count;
} prefix_list;

static HRESULT WINAPI prefixes_QueryInterface(IMXNamespacePrefixes *iface, REFIID riid, void **out)
{
	prefix_list *self = (prefix_list *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IMXNamespacePrefixes))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &self->dispex;
	else
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI prefixes_AddRef(IMXNamespacePrefixes *iface)
{
	return InterlockedIncrement(&((prefix_list *)iface)->refs);
}

static ULONG WINAPI prefixes_Release(IMXNamespacePrefixes *iface)
{
	prefix_list *self = (prefix_list *)iface;
	ULONG left = InterlockedDecrement(&self->refs);
	LONG index;
	if (left)
		return left;
	for (index = 0; index < self->count; index++)
		SysFreeString(self->items[index]);
	mx_free(self->items);
	mx_free(self);
	return 0;
}

static HRESULT WINAPI prefixes_GetTypeInfoCount(IMXNamespacePrefixes *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT prefixes_info(ITypeInfo **out)
{
	return mx_type_info(MX_TYPE_NS_PREFIXES, out);
}

static HRESULT WINAPI prefixes_GetTypeInfo(IMXNamespacePrefixes *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return index ? DISP_E_BADINDEX : prefixes_info(out);
}

static HRESULT WINAPI prefixes_GetIDsOfNames(IMXNamespacePrefixes *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_NS_PREFIXES, riid, names, count, lcid, ids);
}

static HRESULT WINAPI prefixes_Invoke(IMXNamespacePrefixes *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_NS_PREFIXES, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI prefixes_get_item(IMXNamespacePrefixes *iface, LONG index, BSTR *out)
{
	prefix_list *self = (prefix_list *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (index < 0 || index >= self->count)
		return E_INVALIDARG;
	*out = SysAllocString(self->items[index]);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI prefixes_get_length(IMXNamespacePrefixes *iface, LONG *out)
{
	if (!out)
		return E_POINTER;
	*out = ((prefix_list *)iface)->count;
	return S_OK;
}

static HRESULT WINAPI prefixes_get__newEnum(IMXNamespacePrefixes *iface, IUnknown **out)
{
	return E_NOTIMPL;
}

static const IMXNamespacePrefixesVtbl prefixes_table = {
	prefixes_QueryInterface, prefixes_AddRef, prefixes_Release, prefixes_GetTypeInfoCount, prefixes_GetTypeInfo, prefixes_GetIDsOfNames,
	prefixes_Invoke, prefixes_get_item, prefixes_get_length, prefixes_get__newEnum,
};

static HRESULT new_prefix_list(prefix_list **out)
{
	prefix_list *self = mx_zalloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &prefixes_table;
	mx_dispex_init(&self->dispex, self, TRUE);
	self->refs = 1;
	*out = self;
	return S_OK;
}

static void list_add(prefix_list *self, const WCHAR *text)
{
	self->items = mx_realloc(self->items, (self->count + 1) * sizeof(*self->items));
	self->items[self->count++] = SysAllocString(text);
}

/* --- The automation face. --- */

static HRESULT WINAPI vb_QueryInterface(IVBMXNamespaceManager *iface, REFIID riid, void **out)
{
	return native_QueryInterface((IMXNamespaceManager *)from_vb(iface), riid, out);
}

static ULONG WINAPI vb_AddRef(IVBMXNamespaceManager *iface)
{
	return native_AddRef((IMXNamespaceManager *)from_vb(iface));
}

static ULONG WINAPI vb_Release(IVBMXNamespaceManager *iface)
{
	return native_Release((IMXNamespaceManager *)from_vb(iface));
}

static HRESULT WINAPI vb_GetTypeInfoCount(IVBMXNamespaceManager *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI vb_GetTypeInfo(IVBMXNamespaceManager *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return index ? DISP_E_BADINDEX : mx_type_info(MX_TYPE_NS_MANAGER, out);
}

static HRESULT WINAPI vb_GetIDsOfNames(IVBMXNamespaceManager *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_NS_MANAGER, riid, names, count, lcid, ids);
}

static HRESULT WINAPI vb_Invoke(IVBMXNamespaceManager *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_NS_MANAGER, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI vb_put_allowOverride(IVBMXNamespaceManager *iface, VARIANT_BOOL value)
{
	from_vb(iface)->override = value;
	return S_OK;
}

static HRESULT WINAPI vb_get_allowOverride(IVBMXNamespaceManager *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = from_vb(iface)->override;
	return S_OK;
}

static HRESULT WINAPI vb_reset(IVBMXNamespaceManager *iface)
{
	return native_reset((IMXNamespaceManager *)from_vb(iface));
}

static HRESULT WINAPI vb_pushContext(IVBMXNamespaceManager *iface)
{
	return native_pushContext((IMXNamespaceManager *)from_vb(iface));
}

static HRESULT WINAPI vb_pushNodeContext(IVBMXNamespaceManager *iface, IXMLDOMNode *node, VARIANT_BOOL deep)
{
	return native_pushNodeContext((IMXNamespaceManager *)from_vb(iface), node, deep);
}

static HRESULT WINAPI vb_popContext(IVBMXNamespaceManager *iface)
{
	return native_popContext((IMXNamespaceManager *)from_vb(iface));
}

static HRESULT WINAPI vb_declarePrefix(IVBMXNamespaceManager *iface, BSTR prefix, BSTR uri)
{
	return native_declarePrefix((IMXNamespaceManager *)from_vb(iface), prefix, uri);
}

static HRESULT WINAPI vb_getDeclaredPrefixes(IVBMXNamespaceManager *iface, IMXNamespacePrefixes **out)
{
	manager *self = from_vb(iface);
	prefix_list *list;
	const WCHAR *found;
	LONG index;
	HRESULT status;
	if (!out)
		return E_POINTER;
	status = new_prefix_list(&list);
	if (FAILED(status))
		return status;
	for (index = 0; prefix_at(self, index, &found) == S_OK; index++)
		list_add(list, found);
	*out = (IMXNamespacePrefixes *)list;
	return S_OK;
}

static HRESULT WINAPI vb_getPrefixes(IVBMXNamespaceManager *iface, BSTR uri, IMXNamespacePrefixes **out)
{
	manager *self = from_vb(iface);
	prefix_list *list;
	const WCHAR *found;
	LONG index;
	HRESULT status;
	if (!uri || !out)
		return E_POINTER;
	status = new_prefix_list(&list);
	if (FAILED(status))
		return status;
	for (index = 0; prefix_at(self, index, &found) == S_OK; index++)
	{
		const WCHAR *current = uri_of_prefix(self, found);
		if (current && !lstrcmpW(current, uri))
			list_add(list, found);
	}
	*out = (IMXNamespacePrefixes *)list;
	return S_OK;
}

static HRESULT return_uri(const WCHAR *uri, VARIANT *out)
{
	if (!uri)
		return mx_return_null_variant(out);
	V_VT(out) = VT_BSTR;
	V_BSTR(out) = SysAllocString(uri);
	return V_BSTR(out) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI vb_getURI(IVBMXNamespaceManager *iface, BSTR prefix, VARIANT *out)
{
	if (!prefix || !out)
		return E_POINTER;
	if (!lstrcmpW(prefix, xmlns_prefix))
		return E_INVALIDARG;
	return return_uri(uri_of_prefix(from_vb(iface), prefix), out);
}

static HRESULT WINAPI vb_getURIFromNode(IVBMXNamespaceManager *iface, BSTR prefix, IXMLDOMNode *node, VARIANT *out)
{
	xmlNodePtr given;
	xmlChar *utf8;
	xmlNsPtr space;
	HRESULT status;
	if (!prefix || !out)
		return E_POINTER;
	given = node ? mx_node_get((IUnknown *)node) : NULL;
	if (!given)
		return E_INVALIDARG;
	utf8 = *prefix ? mx_to_utf8(prefix) : NULL;
	space = given->doc ? xmlSearchNs(given->doc, given, utf8) : NULL;
	xmlFree(utf8);
	if (!space || !space->href)
		return mx_return_null_variant(out);
	{
		WCHAR *wide = mx_to_wide(space->href);
		status = return_uri(wide, out);
		mx_free(wide);
	}
	return status;
}

static const IVBMXNamespaceManagerVtbl vb_table = {
	vb_QueryInterface, vb_AddRef, vb_Release, vb_GetTypeInfoCount, vb_GetTypeInfo, vb_GetIDsOfNames, vb_Invoke, vb_put_allowOverride,
	vb_get_allowOverride, vb_reset, vb_pushContext, vb_pushNodeContext, vb_popContext, vb_declarePrefix, vb_getDeclaredPrefixes, vb_getPrefixes,
	vb_getURI, vb_getURIFromNode,
};

HRESULT mx_namespace_manager_create(mx_version version, REFIID riid, void **out)
{
	manager *self = mx_zalloc(sizeof(*self));
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &native_table;
	self->vb_vtbl = &vb_table;
	mx_dispex_init(&self->dispex, &self->vb_vtbl, FALSE);
	self->refs = 1;
	self->override = VARIANT_TRUE;
	self->top = new_context(NULL);
	if (self->top)
		new_binding(self->top, xml_prefix, xml_uri);
	status = native_QueryInterface((IMXNamespaceManager *)self, riid, out);
	native_Release((IMXNamespaceManager *)self);
	return status;
}
