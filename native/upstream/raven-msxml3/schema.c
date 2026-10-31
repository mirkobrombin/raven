/* SPDX-License-Identifier: MIT */
#include "node.h"

/* A collection of parsed schemas by namespace. The same object, read only and without schemas, lists
 * the namespaces a document uses. */
typedef struct
{
	xmlChar *uri;
	xmlDocPtr tree;
	xmlSchemaPtr schema;
	BOOL old_format;
} schema_item;

typedef struct
{
	const IXMLDOMSchemaCollection2Vtbl *vtbl;
	const ISupportErrorInfoVtbl *errors_vtbl;
	mx_dispex dispex;
	LONG refs;
	mx_version version;
	schema_item *items;
	LONG count, room;
	VARIANT_BOOL validate_on_load;
	BOOL read_only;
} schema_cache;

static schema_cache *cache_from(IXMLDOMSchemaCollection2 *iface)
{
	return (schema_cache *)iface;
}

const GUID mx_private_cache_iid = {0x7c4b3f1f, 0x52a0, 0x4f6e, {0x9d, 0x11, 0x3a, 0x6c, 0x88, 0x20, 0xe5, 0x18}};

static HRESULT WINAPI cache_QueryInterface(IXMLDOMSchemaCollection2 *iface, REFIID riid, void **out)
{
	schema_cache *self = cache_from(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualGUID(riid, &mx_private_cache_iid))
	{
		*out = self;
		return S_OK;
	}
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IXMLDOMSchemaCollection) ||
		(self->version >= MX_4 && IsEqualGUID(riid, &IID_IXMLDOMSchemaCollection2)))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_ISupportErrorInfo))
		*out = &self->errors_vtbl;
	else if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &self->dispex;
	else
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI cache_AddRef(IXMLDOMSchemaCollection2 *iface)
{
	return InterlockedIncrement(&cache_from(iface)->refs);
}

static void item_clear(schema_item *item)
{
	xmlFree(item->uri);
	if (item->schema)
		xmlSchemaFree(item->schema);
	if (item->tree)
		mx_tree_unref(item->tree);
}

static ULONG WINAPI cache_Release(IXMLDOMSchemaCollection2 *iface)
{
	schema_cache *self = cache_from(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	LONG index;
	if (left)
		return left;
	for (index = 0; index < self->count; index++)
		item_clear(&self->items[index]);
	mx_free(self->items);
	mx_free(self);
	return 0;
}

static HRESULT WINAPI cache_GetTypeInfoCount(IXMLDOMSchemaCollection2 *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI cache_GetTypeInfo(IXMLDOMSchemaCollection2 *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return index ? DISP_E_BADINDEX : mx_type_info(MX_TYPE_SCHEMA_CACHE, out);
}

static HRESULT WINAPI cache_GetIDsOfNames(IXMLDOMSchemaCollection2 *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_SCHEMA_CACHE, riid, names, count, lcid, ids);
}

static HRESULT WINAPI cache_Invoke(IXMLDOMSchemaCollection2 *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_SCHEMA_CACHE, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static schema_item *find_item(schema_cache *self, const xmlChar *uri)
{
	LONG index;
	for (index = 0; index < self->count; index++)
		if (!xmlStrcmp(self->items[index].uri, uri ? uri : (const xmlChar *)""))
			return &self->items[index];
	return NULL;
}

static schema_item *new_item(schema_cache *self, const xmlChar *uri)
{
	schema_item *item = find_item(self, uri);
	if (item)
	{
		item_clear(item);
		memset(item, 0, sizeof(*item));
	}
	else
	{
		if (self->count == self->room)
		{
			self->room = self->room ? self->room * 2 : 4;
			self->items = mx_realloc(self->items, self->room * sizeof(*self->items));
		}
		item = &self->items[self->count++];
		memset(item, 0, sizeof(*item));
	}
	item->uri = xmlStrdup(uri ? uri : (const xmlChar *)"");
	return item;
}

/* The two schema languages tell themselves apart by the root element's namespace. */
static const xmlChar xsd_namespace[] = "http://www.w3.org/2001/XMLSchema";
static const xmlChar xdr_namespace[] = "urn:schemas-microsoft-com:xml-data";

static void quiet_schema_message(void *context, const char *message, ...)
{
}

static void quiet_schema_error(void *context, const xmlError *error)
{
	if (context && error && error->level >= XML_ERR_ERROR)
		mx_error_from_libxml(context, error, NULL);
}

static HRESULT take_schema_tree(schema_cache *self, const xmlChar *uri, xmlDocPtr tree)
{
	xmlNodePtr root = xmlDocGetRootElement(tree);
	schema_item *item;
	if (!root || !root->ns)
		return E_FAIL;
	if (!xmlStrcmp(root->ns->href, xdr_namespace))
	{
		item = new_item(self, uri);
		item->tree = tree;
		mx_tree_ref(tree);
		item->old_format = TRUE;
		return S_OK;
	}
	if (!xmlStrcmp(root->ns->href, xsd_namespace))
	{
		xmlDocPtr copy = xmlCopyDoc(tree, 1);
		xmlSchemaParserCtxtPtr parser;
		xmlSchemaPtr schema;
		xmlChar *target;
		if (!copy)
			return E_OUTOFMEMORY;
		target = xmlGetProp(xmlDocGetRootElement(copy), (const xmlChar *)"targetNamespace");
		/* The name given has to be the schema's own, except the empty one in later releases. */
		if (self->version >= MX_6 ? uri && *uri && xmlStrcmp(uri, target ? target : (const xmlChar *)"") : FALSE)
		{
			xmlFree(target);
			xmlFreeDoc(copy);
			return E_FAIL;
		}
		xmlFree(target);
		parser = xmlSchemaNewDocParserCtxt(copy);
		if (!parser)
		{
			xmlFreeDoc(copy);
			return E_OUTOFMEMORY;
		}
		xmlSchemaSetParserErrors(parser, quiet_schema_message, quiet_schema_message, NULL);
		schema = xmlSchemaParse(parser);
		xmlSchemaFreeParserCtxt(parser);
		if (!schema)
		{
			xmlFreeDoc(copy);
			return E_FAIL;
		}
		item = new_item(self, uri);
		item->schema = schema;
		item->tree = tree;
		mx_tree_ref(tree);
		return S_OK;
	}
	return E_FAIL;
}

static HRESULT WINAPI cache_add(IXMLDOMSchemaCollection2 *iface, BSTR uri, VARIANT source)
{
	schema_cache *self = cache_from(iface);
	xmlChar *space;
	HRESULT status;
	if (self->read_only)
		return E_FAIL;
	if (!uri && self->version >= MX_6 && V_VT(&source) == VT_NULL)
		return E_FAIL;
	space = mx_to_utf8(uri ? uri : L"");
	switch (V_VT(&source))
	{
	case VT_NULL:
		{
			schema_item *item = find_item(self, space);
			if (item)
			{
				item_clear(item);
				*item = self->items[--self->count];
			}
			status = S_OK;
		}
		break;
	case VT_BSTR:
		{
			char *data;
			ULONG size;
			xmlDocPtr tree = NULL;
			mx_error_info error = {0};
			status = mx_read_url(V_BSTR(&source), &data, &size);
			if (SUCCEEDED(status))
			{
				status = mx_parse_memory(data, (int)size, NULL, self->version, FALSE, FALSE, FALSE, V_BSTR(&source), 0, &tree, &error);
				mx_free(data);
				mx_error_clear(&error);
				if (status == S_OK)
				{
					mx_tree_attach(tree, self->version);
					mx_tree_ref(tree);
					status = take_schema_tree(self, space, tree);
					mx_tree_unref(tree);
				}
				else
					status = E_FAIL;
			}
			else
				status = E_FAIL;
		}
		break;
	case VT_DISPATCH:
	case VT_UNKNOWN:
		{
			IXMLDOMDocument *document = NULL;
			IXMLDOMNode *node = NULL;
			xmlNodePtr given;
			schema_cache *other = NULL;
			if (!V_UNKNOWN(&source))
			{
				status = E_INVALIDARG;
				break;
			}
			if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(&source), &mx_private_cache_iid, (void **)&other)))
			{
				status = IXMLDOMSchemaCollection2_addCollection(iface, (IXMLDOMSchemaCollection *)other);
				break;
			}
			if (FAILED(IUnknown_QueryInterface(V_UNKNOWN(&source), &IID_IXMLDOMDocument2, (void **)&document)))
			{
				status = E_INVALIDARG;
				break;
			}
			given = mx_node_get((IUnknown *)document);
			status = given ? take_schema_tree(self, space, (xmlDocPtr)given) : E_FAIL;
			IXMLDOMDocument_Release(document);
			if (node)
				IXMLDOMNode_Release(node);
		}
		break;
	default:
		status = E_INVALIDARG;
		break;
	}
	xmlFree(space);
	return status;
}

static HRESULT WINAPI cache_get(IXMLDOMSchemaCollection2 *iface, BSTR uri, IXMLDOMNode **out)
{
	schema_cache *self = cache_from(iface);
	xmlChar *space;
	schema_item *item;
	if (out)
		*out = NULL;
	if (self->version >= MX_6)
		return E_NOTIMPL;
	if (!out)
		return E_POINTER;
	space = mx_to_utf8(uri ? uri : L"");
	item = find_item(self, space);
	xmlFree(space);
	if (!item || !item->tree)
		return S_OK;
	return mx_node_create(xmlDocGetRootElement(item->tree), out);
}

static HRESULT WINAPI cache_remove(IXMLDOMSchemaCollection2 *iface, BSTR uri)
{
	schema_cache *self = cache_from(iface);
	xmlChar *space;
	schema_item *item;
	if (self->read_only || self->version >= MX_6)
		return E_NOTIMPL;
	space = mx_to_utf8(uri ? uri : L"");
	item = find_item(self, space);
	xmlFree(space);
	if (item)
	{
		item_clear(item);
		*item = self->items[--self->count];
	}
	return S_OK;
}

static HRESULT WINAPI cache_get_length(IXMLDOMSchemaCollection2 *iface, LONG *out)
{
	if (!out)
		return E_POINTER;
	*out = cache_from(iface)->count;
	return S_OK;
}

static HRESULT WINAPI cache_get_namespaceURI(IXMLDOMSchemaCollection2 *iface, LONG index, BSTR *out)
{
	schema_cache *self = cache_from(iface);
	if (!out)
		return E_POINTER;
	if (index < 0 || index >= self->count)
	{
		if (self->version >= MX_6)
			*out = NULL;
		return E_FAIL;
	}
	return mx_return_string(self->items[index].uri, out);
}

static HRESULT WINAPI cache_addCollection(IXMLDOMSchemaCollection2 *iface, IXMLDOMSchemaCollection *other)
{
	schema_cache *self = cache_from(iface), *from;
	LONG index;
	if (!other)
		return E_POINTER;
	if (FAILED(IXMLDOMSchemaCollection_QueryInterface(other, &mx_private_cache_iid, (void **)&from)))
		return E_INVALIDARG;
	if (from == self)
		return S_OK;
	for (index = 0; index < from->count; index++)
		if (from->items[index].tree)
		{
			HRESULT status = take_schema_tree(self, from->items[index].uri, from->items[index].tree);
			if (FAILED(status))
				return status;
		}
	return S_OK;
}

/* Walks the namespaces by number, as strings. */
typedef struct
{
	const IEnumVARIANTVtbl *vtbl;
	LONG refs;
	IXMLDOMSchemaCollection2 *cache;
	LONG position;
} cache_enum;

static HRESULT WINAPI cache_enum_QueryInterface(IEnumVARIANT *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IEnumVARIANT))
		return E_NOINTERFACE;
	*out = iface;
	IEnumVARIANT_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI cache_enum_AddRef(IEnumVARIANT *iface)
{
	return InterlockedIncrement(&((cache_enum *)iface)->refs);
}

static ULONG WINAPI cache_enum_Release(IEnumVARIANT *iface)
{
	cache_enum *self = (cache_enum *)iface;
	LONG refs = InterlockedDecrement(&self->refs);
	if (!refs)
	{
		IXMLDOMSchemaCollection2_Release(self->cache);
		mx_free(self);
	}
	return refs;
}

static HRESULT WINAPI cache_enum_Next(IEnumVARIANT *iface, ULONG wanted, VARIANT *items, ULONG *fetched)
{
	cache_enum *self = (cache_enum *)iface;
	ULONG got = 0, rest;
	if (fetched)
		*fetched = 0;
	if (!items && wanted)
		return E_INVALIDARG;
	while (got < wanted)
	{
		BSTR uri = NULL;
		if (IXMLDOMSchemaCollection2_get_namespaceURI(self->cache, self->position, &uri) != S_OK)
			break;
		self->position++;
		V_VT(&items[got]) = VT_BSTR;
		V_BSTR(&items[got]) = uri;
		got++;
	}
	if (fetched)
		*fetched = got;
	for (rest = got; rest < wanted; rest++)
		V_VT(&items[rest]) = VT_EMPTY;
	return got == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI cache_enum_Skip(IEnumVARIANT *iface, ULONG count)
{
	((cache_enum *)iface)->position += count;
	return S_OK;
}

static HRESULT WINAPI cache_enum_Reset(IEnumVARIANT *iface)
{
	((cache_enum *)iface)->position = 0;
	return S_OK;
}

static HRESULT WINAPI cache_enum_Clone(IEnumVARIANT *iface, IEnumVARIANT **out)
{
	return E_NOTIMPL;
}

static const IEnumVARIANTVtbl cache_enum_table = {
	cache_enum_QueryInterface, cache_enum_AddRef, cache_enum_Release, cache_enum_Next, cache_enum_Skip, cache_enum_Reset, cache_enum_Clone,
};

static HRESULT WINAPI cache_get__newEnum(IXMLDOMSchemaCollection2 *iface, IUnknown **out)
{
	cache_enum *self;
	if (!out)
		return E_POINTER;
	self = mx_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &cache_enum_table;
	self->refs = 1;
	self->cache = iface;
	self->position = 0;
	IXMLDOMSchemaCollection2_AddRef(iface);
	*out = (IUnknown *)self;
	return S_OK;
}

static HRESULT WINAPI cache_validate(IXMLDOMSchemaCollection2 *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI cache_put_validateOnLoad(IXMLDOMSchemaCollection2 *iface, VARIANT_BOOL value)
{
	cache_from(iface)->validate_on_load = value;
	return S_OK;
}

static HRESULT WINAPI cache_get_validateOnLoad(IXMLDOMSchemaCollection2 *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = cache_from(iface)->validate_on_load;
	return S_OK;
}

static HRESULT WINAPI cache_getSchema(IXMLDOMSchemaCollection2 *iface, BSTR uri, ISchema **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI cache_getDeclaration(IXMLDOMSchemaCollection2 *iface, IXMLDOMNode *node, ISchemaItem **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static const IXMLDOMSchemaCollection2Vtbl cache_table = {
	cache_QueryInterface, cache_AddRef, cache_Release, cache_GetTypeInfoCount, cache_GetTypeInfo, cache_GetIDsOfNames, cache_Invoke,
	cache_add, cache_get, cache_remove, cache_get_length, cache_get_namespaceURI, cache_addCollection, cache_get__newEnum,
	cache_validate, cache_put_validateOnLoad, cache_get_validateOnLoad, cache_getSchema, cache_getDeclaration,
};

static HRESULT WINAPI cache_errors_QueryInterface(ISupportErrorInfo *iface, REFIID riid, void **out)
{
	return cache_QueryInterface((IXMLDOMSchemaCollection2 *)CONTAINING_RECORD(iface, schema_cache, errors_vtbl), riid, out);
}

static ULONG WINAPI cache_errors_AddRef(ISupportErrorInfo *iface)
{
	return cache_AddRef((IXMLDOMSchemaCollection2 *)CONTAINING_RECORD(iface, schema_cache, errors_vtbl));
}

static ULONG WINAPI cache_errors_Release(ISupportErrorInfo *iface)
{
	return cache_Release((IXMLDOMSchemaCollection2 *)CONTAINING_RECORD(iface, schema_cache, errors_vtbl));
}

static HRESULT WINAPI cache_errors_InterfaceSupportsErrorInfo(ISupportErrorInfo *iface, REFIID riid)
{
	return IsEqualGUID(riid, &IID_IXMLDOMSchemaCollection) || IsEqualGUID(riid, &IID_IXMLDOMSchemaCollection2) ? S_OK : S_FALSE;
}

static const ISupportErrorInfoVtbl cache_errors_table = {
	cache_errors_QueryInterface, cache_errors_AddRef, cache_errors_Release, cache_errors_InterfaceSupportsErrorInfo,
};

static schema_cache *cache_new(mx_version version)
{
	schema_cache *self = mx_zalloc(sizeof(*self));
	if (!self)
		return NULL;
	self->vtbl = &cache_table;
	self->errors_vtbl = &cache_errors_table;
	mx_dispex_init(&self->dispex, self, FALSE);
	self->refs = 1;
	self->version = version;
	self->validate_on_load = VARIANT_TRUE;
	return self;
}

HRESULT mx_schema_cache_create(mx_version version, REFIID riid, void **out)
{
	schema_cache *self = cache_new(version);
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	status = cache_QueryInterface((IXMLDOMSchemaCollection2 *)self, riid, out);
	cache_Release((IXMLDOMSchemaCollection2 *)self);
	return status;
}

/* --- The namespaces a document uses. --- */

static void collect_namespaces(schema_cache *self, xmlNodePtr node)
{
	xmlNodePtr at;
	if (node->type == XML_ELEMENT_NODE)
	{
		xmlNsPtr space;
		for (space = node->nsDef; space; space = space->next)
			if (space->href && !find_item(self, space->href))
				new_item(self, space->href);
	}
	for (at = node->children; at; at = at->next)
		if (at->type == XML_ELEMENT_NODE)
			collect_namespaces(self, at);
}

HRESULT mx_namespace_view_create(xmlDocPtr tree, IXMLDOMSchemaCollection **out)
{
	schema_cache *self = cache_new(mx_tree_of(tree)->version);
	if (!self)
		return E_OUTOFMEMORY;
	self->read_only = TRUE;
	collect_namespaces(self, (xmlNodePtr)tree);
	*out = (IXMLDOMSchemaCollection *)self;
	return S_OK;
}

/* --- Checking a tree against the collection. --- */

xmlSchemaPtr mx_schema_of_uri(IUnknown *cache, const xmlChar *uri)
{
	schema_cache *self;
	schema_item *item;
	if (FAILED(IUnknown_QueryInterface(cache, &mx_private_cache_iid, (void **)&self)))
		return NULL;
	item = find_item(self, uri);
	return item ? item->schema : NULL;
}

BOOL mx_schema_covers(IUnknown *cache, xmlNodePtr element)
{
	schema_cache *self;
	if (!element || FAILED(IUnknown_QueryInterface(cache, &mx_private_cache_iid, (void **)&self)))
		return FALSE;
	return find_item(self, element->ns && element->ns->href ? element->ns->href : (const xmlChar *)"") != NULL;
}

HRESULT mx_schema_validate(IUnknown *cache, xmlDocPtr tree, xmlNodePtr node, mx_error_info *error)
{
	xmlNodePtr root = node->type == XML_DOCUMENT_NODE ? xmlDocGetRootElement((xmlDocPtr)node) : node;
	xmlSchemaPtr schema;
	xmlSchemaValidCtxtPtr check;
	int result;
	if (!root || root->type != XML_ELEMENT_NODE)
		return E_FAIL;
	schema = mx_schema_of_uri(cache, root->ns && root->ns->href ? root->ns->href : (const xmlChar *)"");
	if (!schema)
	{
		schema_cache *self;
		schema_item *item = NULL;
		if (SUCCEEDED(IUnknown_QueryInterface(cache, &mx_private_cache_iid, (void **)&self)))
			item = find_item(self, root->ns && root->ns->href ? root->ns->href : (const xmlChar *)"");
		/* The old schema language is accepted and not enforced. */
		if (item && item->old_format)
			return S_OK;
		if (error)
		{
			mx_error_clear(error);
			error->code = (LONG)0xC00CE014;
			error->reason = SysAllocString(L"Validate failed: no schema for this namespace.\r\n");
		}
		return S_FALSE;
	}
	check = xmlSchemaNewValidCtxt(schema);
	if (!check)
		return E_OUTOFMEMORY;
	xmlSchemaSetValidStructuredErrors(check, (xmlStructuredErrorFunc)quiet_schema_error, error);
	result = node->type == XML_DOCUMENT_NODE ? xmlSchemaValidateDoc(check, tree) : xmlSchemaValidateOneElement(check, node);
	xmlSchemaFreeValidCtxt(check);
	return result ? S_FALSE : S_OK;
}
