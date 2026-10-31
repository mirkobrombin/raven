/* SPDX-License-Identifier: MIT */
#define MX_LEGACY_MODEL
#include "mx.h"

/* The object model of the oldest parser: a plain tree of upper-cased names over the same libxml2 tree as everything
 * else. Wrappers come and go per call and only pin the tree; the tree owns its nodes, so nothing a program holds keeps a
 * node's relatives apart. What this model calls text differs from the DOM: blanks between tags do not exist, CDATA is
 * text, and the data of a processing instruction reads as a text item next to it. */

#define MX_E_INVALID_VERSION ((HRESULT)0xC00CE57F)
#define MX_E_INVALID_ENCODING ((HRESULT)0xC00CE56E)

typedef struct
{
	const IXMLElement2Vtbl *vtbl;
	LONG refs;
	xmlNodePtr node;
} element;

typedef struct
{
	const IXMLElementCollectionVtbl *vtbl;
	const IEnumVARIANTVtbl *enum_vtbl;
	LONG refs;
	xmlNodePtr owner;
	BOOL attributes, enum_only;
	BSTR filter;
	LONG position;
} collection;

typedef struct
{
	const IXMLAttributeVtbl *vtbl;
	LONG refs;
	BSTR name, value;
} attribute;

typedef struct
{
	const IXMLDocument2Vtbl *vtbl;
	const IPersistStreamInitVtbl *persist_vtbl;
	const IPersistMonikerVtbl *moniker_vtbl;
	const IXMLErrorVtbl *error_vtbl;
	LONG refs;
	xmlDocPtr tree;
	BSTR url, charset;
	LONG ready;
	VARIANT_BOOL async;
	mx_error_info error;
} document;

static HRESULT element_create(xmlNodePtr node, IXMLElement2 **out);
static HRESULT collection_create(xmlNodePtr owner, BOOL attributes, const WCHAR *filter, IXMLElementCollection **out);

static BSTR upper_bstr(const xmlChar *text)
{
	BSTR result = mx_bstr(text);
	if (result)
		CharUpperW(result);
	return result;
}

/* --- Attributes: what is declared on an element, namespaces included, in one numbering. --- */

static xmlChar *attribute_name(xmlAttrPtr attr)
{
	xmlChar *name = NULL;
	if (attr->ns && attr->ns->prefix)
	{
		name = xmlStrdup(attr->ns->prefix);
		name = xmlStrcat(name, BAD_CAST ":");
	}
	return xmlStrcat(name, attr->name);
}

static xmlChar *declaration_name(xmlNsPtr space)
{
	xmlChar *name = xmlStrdup(BAD_CAST "xmlns");
	if (space->prefix)
	{
		name = xmlStrcat(name, BAD_CAST ":");
		name = xmlStrcat(name, space->prefix);
	}
	return name;
}

static BOOL names_match(const xmlChar *stored, const WCHAR *wanted)
{
	WCHAR *wide = mx_to_wide(stored);
	BOOL same = wide && !lstrcmpiW(wide, wanted);
	mx_free(wide);
	return same;
}

static xmlAttrPtr find_attribute(xmlNodePtr node, const WCHAR *name)
{
	xmlAttrPtr attr;
	if (node->type != XML_ELEMENT_NODE)
		return NULL;
	for (attr = node->properties; attr; attr = attr->next)
	{
		xmlChar *stored = attribute_name(attr);
		BOOL same = names_match(stored, name);
		xmlFree(stored);
		if (same)
			return attr;
	}
	return NULL;
}

static xmlNsPtr find_declaration(xmlNodePtr node, const WCHAR *name)
{
	xmlNsPtr space;
	if (node->type != XML_ELEMENT_NODE)
		return NULL;
	for (space = node->nsDef; space; space = space->next)
	{
		xmlChar *stored = declaration_name(space);
		BOOL same = names_match(stored, name);
		xmlFree(stored);
		if (same)
			return space;
	}
	return NULL;
}

static HRESULT WINAPI attribute_QueryInterface(IXMLAttribute *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IXMLAttribute))
	{
		*out = iface;
		IXMLAttribute_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI attribute_AddRef(IXMLAttribute *iface)
{
	return InterlockedIncrement(&((attribute *)iface)->refs);
}

static ULONG WINAPI attribute_Release(IXMLAttribute *iface)
{
	attribute *self = (attribute *)iface;
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		SysFreeString(self->name);
		SysFreeString(self->value);
		mx_free(self);
	}
	return left;
}

static HRESULT WINAPI attribute_GetTypeInfoCount(IXMLAttribute *iface, UINT *count)
{
	*count = 1;
	return S_OK;
}

static HRESULT WINAPI attribute_GetTypeInfo(IXMLAttribute *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	return mx_type_info(MX_TYPE_XML_ATTRIBUTE, out);
}

static HRESULT WINAPI attribute_GetIDsOfNames(IXMLAttribute *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_XML_ATTRIBUTE, riid, names, count, lcid, ids);
}

static HRESULT WINAPI attribute_Invoke(IXMLAttribute *iface, DISPID id, REFIID riid, LCID lcid, WORD flags,
	DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_XML_ATTRIBUTE, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT copy_bstr(BSTR from, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = SysAllocString(from);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI attribute_get_name(IXMLAttribute *iface, BSTR *out)
{
	return copy_bstr(((attribute *)iface)->name, out);
}

static HRESULT WINAPI attribute_get_value(IXMLAttribute *iface, BSTR *out)
{
	return copy_bstr(((attribute *)iface)->value, out);
}

static const IXMLAttributeVtbl attribute_vtbl = {
	attribute_QueryInterface,
	attribute_AddRef,
	attribute_Release,
	attribute_GetTypeInfoCount,
	attribute_GetTypeInfo,
	attribute_GetIDsOfNames,
	attribute_Invoke,
	attribute_get_name,
	attribute_get_value,
};

/* An attribute object is a reading of the attribute taken when it was asked for. */
static HRESULT attribute_create(const xmlChar *name, const xmlChar *value, IDispatch **out)
{
	attribute *self = mx_zalloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &attribute_vtbl;
	self->refs = 1;
	self->name = upper_bstr(name);
	self->value = mx_bstr(value ? value : BAD_CAST "");
	*out = (IDispatch *)self;
	return S_OK;
}

/* --- Collections. --- */

static inline collection *from_enum(IEnumVARIANT *iface)
{
	return CONTAINING_RECORD(iface, collection, enum_vtbl);
}

static BOOL child_matches(xmlNodePtr child, const collection *self)
{
	return !self->filter || (child->type == XML_ELEMENT_NODE && names_match(child->name, self->filter));
}

static BOOL attribute_matches(const xmlChar *name, const collection *self)
{
	return !self->filter || names_match(name, self->filter);
}

static LONG collection_count(const collection *self)
{
	LONG count = 0;
	if (self->attributes)
	{
		xmlNsPtr space;
		xmlAttrPtr attr;
		for (space = self->owner->nsDef; space; space = space->next)
		{
			xmlChar *name = declaration_name(space);
			count += attribute_matches(name, self);
			xmlFree(name);
		}
		for (attr = self->owner->properties; attr; attr = attr->next)
		{
			xmlChar *name = attribute_name(attr);
			count += attribute_matches(name, self);
			xmlFree(name);
		}
	}
	else
	{
		xmlNodePtr child;
		for (child = self->owner->children; child; child = child->next)
			count += child_matches(child, self);
	}
	return count;
}

static HRESULT collection_item(const collection *self, LONG index, IDispatch **out)
{
	if (self->attributes)
	{
		xmlNsPtr space;
		xmlAttrPtr attr;
		for (space = self->owner->nsDef; space; space = space->next)
		{
			xmlChar *name = declaration_name(space);
			if (attribute_matches(name, self) && !index--)
			{
				HRESULT status = attribute_create(name, space->href, out);
				xmlFree(name);
				return status;
			}
			xmlFree(name);
		}
		for (attr = self->owner->properties; attr; attr = attr->next)
		{
			xmlChar *name = attribute_name(attr);
			if (attribute_matches(name, self) && !index--)
			{
				xmlChar *value = xmlNodeListGetString(attr->doc, attr->children, 1);
				HRESULT status = attribute_create(name, value, out);
				xmlFree(value);
				xmlFree(name);
				return status;
			}
			xmlFree(name);
		}
	}
	else
	{
		xmlNodePtr child;
		for (child = self->owner->children; child; child = child->next)
			if (child_matches(child, self) && !index--)
				return element_create(child, (IXMLElement2 **)out);
	}
	return E_FAIL;
}

static HRESULT WINAPI collection_QueryInterface(IXMLElementCollection *iface, REFIID riid, void **out)
{
	collection *self = (collection *)iface;
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IEnumVARIANT))
		*out = &self->enum_vtbl;
	else if (self->enum_only)
		*out = IsEqualGUID(riid, &IID_IUnknown) ? &self->enum_vtbl : NULL;
	else if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IXMLElementCollection))
		*out = iface;
	else
		*out = NULL;
	if (!*out)
		return E_NOINTERFACE;
	InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG WINAPI collection_AddRef(IXMLElementCollection *iface)
{
	return InterlockedIncrement(&((collection *)iface)->refs);
}

static ULONG WINAPI collection_Release(IXMLElementCollection *iface)
{
	collection *self = (collection *)iface;
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		mx_tree_unref(self->owner->doc);
		SysFreeString(self->filter);
		mx_free(self);
	}
	return left;
}

static HRESULT WINAPI collection_GetTypeInfoCount(IXMLElementCollection *iface, UINT *count)
{
	*count = 1;
	return S_OK;
}

static HRESULT WINAPI collection_GetTypeInfo(IXMLElementCollection *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	return mx_type_info(MX_TYPE_XML_COLLECTION, out);
}

static HRESULT WINAPI collection_GetIDsOfNames(IXMLElementCollection *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_XML_COLLECTION, riid, names, count, lcid, ids);
}

static HRESULT WINAPI collection_Invoke(IXMLElementCollection *iface, DISPID id, REFIID riid, LCID lcid, WORD flags,
	DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_XML_COLLECTION, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI collection_put_length(IXMLElementCollection *iface, LONG value)
{
	return E_FAIL;
}

static HRESULT WINAPI collection_get_length(IXMLElementCollection *iface, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = collection_count((collection *)iface);
	return S_OK;
}

static HRESULT WINAPI collection_get__newEnum(IXMLElementCollection *iface, IUnknown **out)
{
	collection *self = (collection *)iface;
	IXMLElementCollection *copy;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	status = collection_create(self->owner, self->attributes, self->filter, &copy);
	if (FAILED(status))
		return status;
	((collection *)copy)->enum_only = TRUE;
	*out = (IUnknown *)&((collection *)copy)->enum_vtbl;
	return S_OK;
}

static BOOL variant_index(const VARIANT *value, LONG *index)
{
	VARIANT number;
	VariantInit(&number);
	if (V_VT(value) == VT_NULL || V_VT(value) == VT_EMPTY || V_VT(value) == VT_ERROR
		|| FAILED(VariantChangeType(&number, (VARIANT *)value, 0, VT_I4)))
		return FALSE;
	*index = V_I4(&number);
	return TRUE;
}

/* The first argument is a position, or a name; a name that several items share reads as those items, and the second
 * argument then picks one of them. */
static HRESULT WINAPI collection_item_method(IXMLElementCollection *iface, VARIANT first, VARIANT second, IDispatch **out)
{
	collection *self = (collection *)iface;
	LONG index;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (V_VT(&first) == VT_BSTR)
	{
		IXMLElementCollection *matching;
		LONG count;
		HRESULT status = collection_create(self->owner, self->attributes, V_BSTR(&first), &matching);
		if (FAILED(status))
			return status;
		count = collection_count((collection *)matching);
		if (variant_index(&second, &index))
			status = index < 0 ? E_INVALIDARG : collection_item((collection *)matching, index, out);
		else if (!count)
			status = E_FAIL;
		else if (count == 1)
			status = collection_item((collection *)matching, 0, out);
		else
		{
			*out = (IDispatch *)matching;
			return S_OK;
		}
		IXMLElementCollection_Release(matching);
		return status;
	}
	if (!variant_index(&first, &index))
		return E_INVALIDARG;
	if (index < 0)
		return E_INVALIDARG;
	return collection_item(self, index, out);
}

static const IXMLElementCollectionVtbl collection_vtbl = {
	collection_QueryInterface,
	collection_AddRef,
	collection_Release,
	collection_GetTypeInfoCount,
	collection_GetTypeInfo,
	collection_GetIDsOfNames,
	collection_Invoke,
	collection_put_length,
	collection_get_length,
	collection_get__newEnum,
	collection_item_method,
};

static HRESULT WINAPI enum_QueryInterface(IEnumVARIANT *iface, REFIID riid, void **out)
{
	return collection_QueryInterface((IXMLElementCollection *)from_enum(iface), riid, out);
}

static ULONG WINAPI enum_AddRef(IEnumVARIANT *iface)
{
	return InterlockedIncrement(&from_enum(iface)->refs);
}

static ULONG WINAPI enum_Release(IEnumVARIANT *iface)
{
	return collection_Release((IXMLElementCollection *)from_enum(iface));
}

static HRESULT WINAPI enum_Next(IEnumVARIANT *iface, ULONG wanted, VARIANT *items, ULONG *fetched)
{
	collection *self = from_enum(iface);
	ULONG got = 0;
	if (!items)
		return E_INVALIDARG;
	while (got < wanted)
	{
		IDispatch *item = NULL;
		if (FAILED(collection_item(self, self->position, &item)))
			break;
		self->position++;
		V_VT(&items[got]) = VT_DISPATCH;
		V_DISPATCH(&items[got]) = item;
		got++;
	}
	if (got < wanted)
		V_VT(&items[got]) = VT_EMPTY;
	if (fetched)
		*fetched = got;
	return got == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Skip(IEnumVARIANT *iface, ULONG count)
{
	collection *self = from_enum(iface);
	LONG length = collection_count(self);
	if ((ULONG)(length - self->position) < count)
	{
		self->position = length;
		return S_FALSE;
	}
	self->position += count;
	return S_OK;
}

static HRESULT WINAPI enum_Reset(IEnumVARIANT *iface)
{
	from_enum(iface)->position = 0;
	return S_OK;
}

static HRESULT WINAPI enum_Clone(IEnumVARIANT *iface, IEnumVARIANT **out)
{
	collection *self = from_enum(iface);
	IXMLElementCollection *copy;
	HRESULT status;
	if (!out)
		return E_POINTER;
	status = collection_create(self->owner, self->attributes, self->filter, &copy);
	if (FAILED(status))
		return status;
	((collection *)copy)->enum_only = self->enum_only;
	((collection *)copy)->position = self->position;
	*out = (IEnumVARIANT *)&((collection *)copy)->enum_vtbl;
	return S_OK;
}

static const IEnumVARIANTVtbl enum_vtbl = {
	enum_QueryInterface,
	enum_AddRef,
	enum_Release,
	enum_Next,
	enum_Skip,
	enum_Reset,
	enum_Clone,
};

static HRESULT collection_create(xmlNodePtr owner, BOOL attributes, const WCHAR *filter, IXMLElementCollection **out)
{
	collection *self = mx_zalloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &collection_vtbl;
	self->enum_vtbl = &enum_vtbl;
	self->refs = 1;
	self->owner = owner;
	self->attributes = attributes;
	self->filter = filter ? SysAllocString(filter) : NULL;
	mx_tree_ref(owner->doc);
	*out = (IXMLElementCollection *)self;
	return S_OK;
}

/* --- Elements. --- */

static const IXMLElement2Vtbl element_vtbl;

static HRESULT WINAPI element_QueryInterface(IXMLElement2 *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IXMLElement)
		|| IsEqualGUID(riid, &IID_IXMLElement2))
	{
		*out = iface;
		IXMLElement2_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI element_AddRef(IXMLElement2 *iface)
{
	return InterlockedIncrement(&((element *)iface)->refs);
}

static ULONG WINAPI element_Release(IXMLElement2 *iface)
{
	element *self = (element *)iface;
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		mx_tree_unref(self->node->doc);
		mx_free(self);
	}
	return left;
}

static HRESULT WINAPI element_GetTypeInfoCount(IXMLElement2 *iface, UINT *count)
{
	*count = 1;
	return S_OK;
}

static HRESULT WINAPI element_GetTypeInfo(IXMLElement2 *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	return mx_type_info(MX_TYPE_XML_ELEMENT, out);
}

static HRESULT WINAPI element_GetIDsOfNames(IXMLElement2 *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_XML_ELEMENT, riid, names, count, lcid, ids);
}

static HRESULT WINAPI element_Invoke(IXMLElement2 *iface, DISPID id, REFIID riid, LCID lcid, WORD flags,
	DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_XML_ELEMENT, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI element_get_tagName(IXMLElement2 *iface, BSTR *out)
{
	xmlNodePtr node = ((element *)iface)->node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (node->type != XML_ELEMENT_NODE && node->type != XML_PI_NODE)
		return E_NOTIMPL;
	/* An element made without a name reads as having none. */
	if (!node->name || !*node->name)
		return S_OK;
	*out = upper_bstr(node->name);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI element_put_tagName(IXMLElement2 *iface, BSTR name)
{
	xmlNodePtr node = ((element *)iface)->node;
	xmlChar *text;
	if (!name)
		return E_INVALIDARG;
	if (node->type != XML_ELEMENT_NODE)
		return E_NOTIMPL;
	text = mx_to_utf8(name);
	if (!text)
		return E_OUTOFMEMORY;
	xmlNodeSetName(node, text);
	xmlFree(text);
	return S_OK;
}

static HRESULT WINAPI element_get_parent(IXMLElement2 *iface, IXMLElement2 **out)
{
	xmlNodePtr parent = ((element *)iface)->node->parent;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!parent || parent->type == XML_DOCUMENT_NODE)
		return S_FALSE;
	return element_create(parent, out);
}

static HRESULT WINAPI element_setAttribute(IXMLElement2 *iface, BSTR name, VARIANT value)
{
	xmlNodePtr node = ((element *)iface)->node;
	xmlAttrPtr attr;
	VARIANT text;
	xmlChar *utf8;
	HRESULT status;
	if (!name)
		return E_INVALIDARG;
	if (node->type != XML_ELEMENT_NODE)
		return E_NOTIMPL;
	status = mx_variant_text(&value, &text);
	if (FAILED(status))
		return E_INVALIDARG;
	utf8 = mx_to_utf8(V_BSTR(&text) ? V_BSTR(&text) : L"");
	VariantClear(&text);
	if (!utf8)
		return E_OUTOFMEMORY;
	attr = find_attribute(node, name);
	if (attr)
		xmlSetNsProp(node, attr->ns, attr->name, utf8);
	else
	{
		xmlChar *key = mx_to_utf8(name);
		if (key)
			xmlNewProp(node, key, utf8);
		xmlFree(key);
	}
	xmlFree(utf8);
	return S_OK;
}

static HRESULT WINAPI element_getAttribute(IXMLElement2 *iface, BSTR name, VARIANT *out)
{
	xmlNodePtr node = ((element *)iface)->node;
	xmlAttrPtr attr;
	xmlNsPtr space;
	if (!name || !out)
		return E_INVALIDARG;
	VariantInit(out);
	V_BSTR(out) = NULL;
	if ((attr = find_attribute(node, name)))
	{
		xmlChar *value = xmlNodeListGetString(attr->doc, attr->children, 1);
		V_BSTR(out) = mx_bstr(value ? value : BAD_CAST "");
		xmlFree(value);
	}
	else if ((space = find_declaration(node, name)))
		V_BSTR(out) = mx_bstr(space->href ? space->href : BAD_CAST "");
	else
		return S_FALSE;
	V_VT(out) = VT_BSTR;
	return S_OK;
}

static HRESULT WINAPI element_removeAttribute(IXMLElement2 *iface, BSTR name)
{
	xmlAttrPtr attr;
	if (!name)
		return E_INVALIDARG;
	attr = find_attribute(((element *)iface)->node, name);
	if (!attr)
		return S_FALSE;
	xmlRemoveProp(attr);
	return S_OK;
}

static HRESULT WINAPI element_get_children(IXMLElement2 *iface, IXMLElementCollection **out)
{
	xmlNodePtr node = ((element *)iface)->node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (node->type != XML_ELEMENT_NODE || !node->children)
		return S_FALSE;
	return collection_create(node, FALSE, NULL, out);
}

static HRESULT WINAPI element_get_type(IXMLElement2 *iface, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	switch (((element *)iface)->node->type)
	{
	case XML_ELEMENT_NODE:
		*out = XMLELEMTYPE_ELEMENT;
		break;
	case XML_TEXT_NODE:
	case XML_CDATA_SECTION_NODE:
	case XML_ENTITY_REF_NODE:
		*out = XMLELEMTYPE_TEXT;
		break;
	case XML_COMMENT_NODE:
		*out = XMLELEMTYPE_COMMENT;
		break;
	case XML_PI_NODE:
		*out = XMLELEMTYPE_PI;
		break;
	default:
		*out = XMLELEMTYPE_OTHER;
		break;
	}
	return S_OK;
}

static HRESULT WINAPI element_get_text(IXMLElement2 *iface, BSTR *out)
{
	xmlChar *text;
	if (!out)
		return E_INVALIDARG;
	text = xmlNodeGetContent(((element *)iface)->node);
	*out = mx_bstr(text ? text : BAD_CAST "");
	xmlFree(text);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI element_put_text(IXMLElement2 *iface, BSTR value)
{
	xmlNodePtr node = ((element *)iface)->node;
	xmlChar *text, *escaped;
	if (node->type == XML_ELEMENT_NODE)
		return E_NOTIMPL;
	text = mx_to_utf8(value ? value : L"");
	if (!text)
		return E_OUTOFMEMORY;
	/* Setting content reads entity references out of the text, which is not what was asked. */
	escaped = xmlEncodeSpecialChars(node->doc, text);
	xmlNodeSetContent(node, escaped);
	xmlFree(escaped);
	xmlFree(text);
	return S_OK;
}

/* Linked by hand: the library's own insertion merges neighbouring text, and each of them is an item here. */
static void link_child(xmlNodePtr parent, xmlNodePtr child, LONG index)
{
	xmlNodePtr at = parent->children;
	while (at && index-- > 0)
		at = at->next;
	child->parent = parent;
	child->next = at;
	child->prev = at ? at->prev : parent->last;
	if (child->prev)
		child->prev->next = child;
	else
		parent->children = child;
	if (at)
		at->prev = child;
	else
		parent->last = child;
}

static HRESULT WINAPI element_addChild(IXMLElement2 *iface, IXMLElement2 *given, LONG index, LONG reserved)
{
	xmlNodePtr node = ((element *)iface)->node;
	xmlNodePtr child, up;
	if (!given || given->lpVtbl != &element_vtbl)
		return E_INVALIDARG;
	child = ((element *)given)->node;
	if (child->doc != node->doc)
		return E_INVALIDARG;
	for (up = node; up; up = up->parent)
		if (up == child)
			return E_INVALIDARG;
	if (child->parent)
		xmlUnlinkNode(child);
	else
		mx_tree_adopted(child);
	if (index < 0)
		index = LONG_MAX;
	link_child(node, child, index);
	return S_OK;
}

static HRESULT WINAPI element_removeChild(IXMLElement2 *iface, IXMLElement2 *given)
{
	xmlNodePtr child;
	if (!given || given->lpVtbl != &element_vtbl)
		return E_INVALIDARG;
	child = ((element *)given)->node;
	if (child->parent != ((element *)iface)->node)
		return E_INVALIDARG;
	xmlUnlinkNode(child);
	mx_tree_orphan(child);
	return S_OK;
}

static HRESULT WINAPI element_get_attributes(IXMLElement2 *iface, IXMLElementCollection **out)
{
	xmlNodePtr node = ((element *)iface)->node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (node->type != XML_ELEMENT_NODE)
		return S_FALSE;
	return collection_create(node, TRUE, NULL, out);
}

static const IXMLElement2Vtbl element_vtbl = {
	element_QueryInterface,
	element_AddRef,
	element_Release,
	element_GetTypeInfoCount,
	element_GetTypeInfo,
	element_GetIDsOfNames,
	element_Invoke,
	element_get_tagName,
	element_put_tagName,
	element_get_parent,
	element_setAttribute,
	element_getAttribute,
	element_removeAttribute,
	element_get_children,
	element_get_type,
	element_get_text,
	element_put_text,
	element_addChild,
	element_removeChild,
	element_get_attributes,
};

static HRESULT element_create(xmlNodePtr node, IXMLElement2 **out)
{
	element *self = mx_zalloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &element_vtbl;
	self->refs = 1;
	self->node = node;
	mx_tree_ref(node->doc);
	*out = (IXMLElement2 *)self;
	return S_OK;
}

/* --- Loading. --- */

/* Bring a parsed tree to this model's shape: see the top of the file. */
static void reshape(xmlNodePtr parent)
{
	xmlNodePtr child = parent->children;
	while (child)
	{
		xmlNodePtr next = child->next;
		if (child->type == XML_TEXT_NODE && mx_is_blank(child->content))
		{
			xmlUnlinkNode(child);
			xmlFreeNode(child);
		}
		else if (child->type == XML_PI_NODE && child->content && *child->content)
		{
			xmlNodePtr data = xmlNewDocText(child->doc, child->content);
			if (data)
			{
				data->parent = parent;
				data->prev = child;
				data->next = next;
				child->next = data;
				if (next)
					next->prev = data;
				else
					parent->last = data;
			}
			xmlNodeSetContent(child, BAD_CAST "");
		}
		else if (child->type == XML_ELEMENT_NODE)
			reshape(child);
		child = next;
	}
}

/* The value of one pseudo-attribute of the declaration at the start of the text, or NULL. */
static xmlChar *declaration_value(const char *text, int size, const char *name)
{
	const char *end, *at;
	int length = lstrlenA(name);
	if (size < 5 || memcmp(text, "<?xml", 5))
		return NULL;
	for (end = text; end < text + size - 1 && memcmp(end, "?>", 2); end++)
		;
	for (at = text + 5; at + length < end; at++)
	{
		const char *value = at + length;
		const char *close;
		if (memcmp(at, name, length))
			continue;
		while (value < end && (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n'))
			value++;
		if (value >= end || *value != '=')
			continue;
		value++;
		while (value < end && (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n'))
			value++;
		if (value >= end || (*value != '"' && *value != '\''))
			continue;
		for (close = value + 1; close < end && *close != *value; close++)
			;
		return xmlStrndup((const xmlChar *)value + 1, (int)(close - value - 1));
	}
	return NULL;
}

static void document_set_tree(document *self, xmlDocPtr tree)
{
	mx_tree_ref(tree);
	if (self->tree)
		mx_tree_unref(self->tree);
	self->tree = tree;
}

static void document_reset(document *self)
{
	document_set_tree(self, mx_tree_new_document(MX_1));
	SysFreeString(self->charset);
	self->charset = NULL;
	self->ready = READYSTATE_COMPLETE;
}

static HRESULT document_parse(document *self, const char *data, ULONG size)
{
	BOOL unknown = FALSE;
	int text_size = 0;
	char *text = mx_decode_input(data, (int)size, &text_size, &unknown);
	xmlChar *version, *charset;
	xmlDocPtr tree = NULL;
	HRESULT status = S_OK;
	if (!text)
		return unknown ? MX_E_INVALID_ENCODING : E_OUTOFMEMORY;
	version = declaration_value(text, text_size, "version");
	if (version && xmlStrcmp(version, BAD_CAST "1.0"))
		status = MX_E_INVALID_VERSION;
	xmlFree(version);
	charset = declaration_value(text, text_size, "encoding");
	if (status == S_OK)
	{
		mx_error_clear(&self->error);
		status = mx_parse_memory(text, text_size, "UTF-8", MX_1, TRUE, FALSE, FALSE, self->url, 0, &tree, &self->error);
		if (status != S_OK || !tree)
			status = self->error.code < 0 ? (HRESULT)self->error.code : E_FAIL;
	}
	if (status == S_OK)
	{
		xmlNodePtr root = xmlDocGetRootElement(tree);
		mx_tree_attach(tree, MX_1);
		if (root)
			reshape(root);
		document_set_tree(self, tree);
		SysFreeString(self->charset);
		self->charset = charset ? mx_bstr(charset) : NULL;
	}
	xmlFree(charset);
	mx_free(text);
	self->ready = READYSTATE_COMPLETE;
	return status;
}

static HRESULT document_load_stream(document *self, IStream *stream)
{
	char *data = NULL;
	ULONG size = 0;
	HRESULT status = mx_read_stream(stream, &data, &size);
	if (FAILED(status))
		return status;
	status = document_parse(self, data, size);
	mx_free(data);
	return status;
}

/* --- The document. --- */

static inline document *from_persist(IPersistStreamInit *iface)
{
	return CONTAINING_RECORD(iface, document, persist_vtbl);
}

static inline document *from_moniker(IPersistMoniker *iface)
{
	return CONTAINING_RECORD(iface, document, moniker_vtbl);
}

static inline document *from_error(IXMLError *iface)
{
	return CONTAINING_RECORD(iface, document, error_vtbl);
}

static HRESULT WINAPI document_QueryInterface(IXMLDocument2 *iface, REFIID riid, void **out)
{
	document *self = (document *)iface;
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IXMLDocument)
		|| IsEqualGUID(riid, &IID_IXMLDocument2))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_IPersistStreamInit) || IsEqualGUID(riid, &IID_IPersistStream))
		*out = &self->persist_vtbl;
	else if (IsEqualGUID(riid, &IID_IPersistMoniker))
		*out = &self->moniker_vtbl;
	else if (IsEqualGUID(riid, &IID_IXMLError))
		*out = &self->error_vtbl;
	else
	{
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG WINAPI document_AddRef(IXMLDocument2 *iface)
{
	return InterlockedIncrement(&((document *)iface)->refs);
}

static ULONG WINAPI document_Release(IXMLDocument2 *iface)
{
	document *self = (document *)iface;
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		mx_tree_unref(self->tree);
		mx_error_clear(&self->error);
		SysFreeString(self->url);
		SysFreeString(self->charset);
		mx_free(self);
	}
	return left;
}

static HRESULT WINAPI document_GetTypeInfoCount(IXMLDocument2 *iface, UINT *count)
{
	*count = 1;
	return S_OK;
}

static HRESULT WINAPI document_GetTypeInfo(IXMLDocument2 *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	return mx_type_info(MX_TYPE_XML_DOCUMENT, out);
}

static HRESULT WINAPI document_GetIDsOfNames(IXMLDocument2 *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_XML_DOCUMENT, riid, names, count, lcid, ids);
}

static HRESULT WINAPI document_Invoke(IXMLDocument2 *iface, DISPID id, REFIID riid, LCID lcid, WORD flags,
	DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_XML_DOCUMENT, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI document_get_root(IXMLDocument2 *iface, IXMLElement2 **out)
{
	xmlNodePtr root;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	root = xmlDocGetRootElement(((document *)iface)->tree);
	if (!root)
		return E_FAIL;
	return element_create(root, out);
}

static HRESULT WINAPI document_get_fileSize(IXMLDocument2 *iface, BSTR *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI document_get_fileModifiedDate(IXMLDocument2 *iface, BSTR *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI document_get_fileUpdatedDate(IXMLDocument2 *iface, BSTR *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI document_get_URL(IXMLDocument2 *iface, BSTR *out)
{
	document *self = (document *)iface;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!self->url)
		return S_FALSE;
	return copy_bstr(self->url, out);
}

static HRESULT WINAPI document_put_URL(IXMLDocument2 *iface, BSTR url)
{
	document *self = (document *)iface;
	char *data = NULL;
	ULONG size = 0;
	HRESULT status;
	if (!url)
		return E_INVALIDARG;
	status = mx_read_url(url, &data, &size);
	if (FAILED(status))
		return status;
	SysFreeString(self->url);
	self->url = SysAllocString(url);
	status = document_parse(self, data, size);
	mx_free(data);
	return status;
}

static HRESULT WINAPI document_get_mimeType(IXMLDocument2 *iface, BSTR *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI document_get_readyState(IXMLDocument2 *iface, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = ((document *)iface)->ready;
	return S_OK;
}

static HRESULT WINAPI document_get_charset(IXMLDocument2 *iface, BSTR *out)
{
	document *self = (document *)iface;
	if (!out)
		return E_INVALIDARG;
	*out = SysAllocString(self->charset ? self->charset : L"UTF-8");
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI document_put_charset(IXMLDocument2 *iface, BSTR value)
{
	document *self = (document *)iface;
	if (!value)
		return E_INVALIDARG;
	SysFreeString(self->charset);
	self->charset = SysAllocString(value);
	return S_OK;
}

static HRESULT WINAPI document_get_version(IXMLDocument2 *iface, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = SysAllocString(L"1.0");
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI document_get_doctype(IXMLDocument2 *iface, BSTR *out)
{
	xmlDtdPtr dtd;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	dtd = xmlGetIntSubset(((document *)iface)->tree);
	if (!dtd || !dtd->name)
		return S_FALSE;
	*out = upper_bstr(dtd->name);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI document_get_dtdURL(IXMLDocument2 *iface, BSTR *out)
{
	xmlDtdPtr dtd;
	if (!out)
		return E_INVALIDARG;
	dtd = xmlGetIntSubset(((document *)iface)->tree);
	return mx_return_string(dtd ? dtd->SystemID : NULL, out);
}

static HRESULT WINAPI document_createElement(IXMLDocument2 *iface, VARIANT type, VARIANT name, IXMLElement2 **out)
{
	xmlDocPtr tree = ((document *)iface)->tree;
	xmlChar *tag = NULL;
	xmlNodePtr node = NULL;
	LONG kind;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!variant_index(&type, &kind))
		return E_INVALIDARG;
	if (V_VT(&name) == VT_BSTR && V_BSTR(&name))
		tag = mx_to_utf8(V_BSTR(&name));
	switch (kind)
	{
	case XMLELEMTYPE_ELEMENT:
		node = xmlNewDocNode(tree, NULL, tag ? tag : BAD_CAST "", NULL);
		break;
	case XMLELEMTYPE_TEXT:
		node = xmlNewDocText(tree, BAD_CAST "");
		break;
	case XMLELEMTYPE_COMMENT:
		node = xmlNewDocComment(tree, BAD_CAST "");
		break;
	case XMLELEMTYPE_PI:
		node = xmlNewDocPI(tree, tag ? tag : BAD_CAST "", BAD_CAST "");
		break;
	default:
		xmlFree(tag);
		return E_NOTIMPL;
	}
	xmlFree(tag);
	if (!node)
		return E_OUTOFMEMORY;
	mx_tree_orphan(node);
	status = element_create(node, out);
	return status;
}

static HRESULT WINAPI document_get_async(IXMLDocument2 *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = ((document *)iface)->async;
	return S_OK;
}

static HRESULT WINAPI document_put_async(IXMLDocument2 *iface, VARIANT_BOOL value)
{
	/* Loading finishes before the call returns either way. */
	((document *)iface)->async = value;
	return S_OK;
}

static const IXMLDocument2Vtbl document_vtbl = {
	document_QueryInterface,
	document_AddRef,
	document_Release,
	document_GetTypeInfoCount,
	document_GetTypeInfo,
	document_GetIDsOfNames,
	document_Invoke,
	document_get_root,
	document_get_fileSize,
	document_get_fileModifiedDate,
	document_get_fileUpdatedDate,
	document_get_URL,
	document_put_URL,
	document_get_mimeType,
	document_get_readyState,
	document_get_charset,
	document_put_charset,
	document_get_version,
	document_get_doctype,
	document_get_dtdURL,
	document_createElement,
	document_get_async,
	document_put_async,
};

/* --- Persistence over a stream. --- */

static HRESULT WINAPI persist_QueryInterface(IPersistStreamInit *iface, REFIID riid, void **out)
{
	return document_QueryInterface((IXMLDocument2 *)from_persist(iface), riid, out);
}

static ULONG WINAPI persist_AddRef(IPersistStreamInit *iface)
{
	return document_AddRef((IXMLDocument2 *)from_persist(iface));
}

static ULONG WINAPI persist_Release(IPersistStreamInit *iface)
{
	return document_Release((IXMLDocument2 *)from_persist(iface));
}

static HRESULT WINAPI persist_GetClassID(IPersistStreamInit *iface, CLSID *out)
{
	if (!out)
		return E_POINTER;
	*out = CLSID_XMLDocument;
	return S_OK;
}

static HRESULT WINAPI persist_IsDirty(IPersistStreamInit *iface)
{
	return S_FALSE;
}

static HRESULT WINAPI persist_Load(IPersistStreamInit *iface, IStream *stream)
{
	if (!stream)
		return E_INVALIDARG;
	return document_load_stream(from_persist(iface), stream);
}

static HRESULT WINAPI persist_Save(IPersistStreamInit *iface, IStream *stream, BOOL clear_dirty)
{
	xmlChar *text = NULL;
	int size = 0;
	ULONG written;
	HRESULT status;
	if (!stream)
		return E_INVALIDARG;
	xmlDocDumpMemoryEnc(from_persist(iface)->tree, &text, &size, "UTF-8");
	if (!text)
		return E_OUTOFMEMORY;
	status = IStream_Write(stream, text, size, &written);
	xmlFree(text);
	return status;
}

static HRESULT WINAPI persist_GetSizeMax(IPersistStreamInit *iface, ULARGE_INTEGER *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI persist_InitNew(IPersistStreamInit *iface)
{
	document_reset(from_persist(iface));
	return S_OK;
}

static const IPersistStreamInitVtbl persist_vtbl = {
	persist_QueryInterface,
	persist_AddRef,
	persist_Release,
	persist_GetClassID,
	persist_IsDirty,
	persist_Load,
	persist_Save,
	persist_GetSizeMax,
	persist_InitNew,
};

/* --- Persistence through a moniker: reading only. --- */

static HRESULT WINAPI moniker_QueryInterface(IPersistMoniker *iface, REFIID riid, void **out)
{
	return document_QueryInterface((IXMLDocument2 *)from_moniker(iface), riid, out);
}

static ULONG WINAPI moniker_AddRef(IPersistMoniker *iface)
{
	return document_AddRef((IXMLDocument2 *)from_moniker(iface));
}

static ULONG WINAPI moniker_Release(IPersistMoniker *iface)
{
	return document_Release((IXMLDocument2 *)from_moniker(iface));
}

static HRESULT WINAPI moniker_GetClassID(IPersistMoniker *iface, CLSID *out)
{
	if (!out)
		return E_POINTER;
	*out = CLSID_XMLDocument;
	return S_OK;
}

static HRESULT WINAPI moniker_IsDirty(IPersistMoniker *iface)
{
	return S_FALSE;
}

static HRESULT WINAPI moniker_Load(IPersistMoniker *iface, BOOL available, IMoniker *name, IBindCtx *context, DWORD mode)
{
	IBindCtx *own = NULL;
	IStream *stream = NULL;
	HRESULT status;
	if (!name)
		return E_INVALIDARG;
	if (!context && FAILED(status = CreateBindCtx(0, &own)))
		return status;
	status = IMoniker_BindToStorage(name, context ? context : own, NULL, &IID_IStream, (void **)&stream);
	if (own)
		IBindCtx_Release(own);
	if (FAILED(status))
		return status;
	status = document_load_stream(from_moniker(iface), stream);
	IStream_Release(stream);
	return status;
}

static HRESULT WINAPI moniker_Save(IPersistMoniker *iface, IMoniker *name, IBindCtx *context, BOOL remember)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI moniker_SaveCompleted(IPersistMoniker *iface, IMoniker *name, IBindCtx *context)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI moniker_GetCurMoniker(IPersistMoniker *iface, IMoniker **out)
{
	return E_NOTIMPL;
}

static const IPersistMonikerVtbl moniker_vtbl = {
	moniker_QueryInterface,
	moniker_AddRef,
	moniker_Release,
	moniker_GetClassID,
	moniker_IsDirty,
	moniker_Load,
	moniker_Save,
	moniker_SaveCompleted,
	moniker_GetCurMoniker,
};

/* --- The last parse error. --- */

static HRESULT WINAPI error_QueryInterface(IXMLError *iface, REFIID riid, void **out)
{
	return document_QueryInterface((IXMLDocument2 *)from_error(iface), riid, out);
}

static ULONG WINAPI error_AddRef(IXMLError *iface)
{
	return document_AddRef((IXMLDocument2 *)from_error(iface));
}

static ULONG WINAPI error_Release(IXMLError *iface)
{
	return document_Release((IXMLDocument2 *)from_error(iface));
}

static HRESULT WINAPI error_GetErrorInfo(IXMLError *iface, XML_ERROR *out)
{
	const mx_error_info *info = &from_error(iface)->error;
	if (!out)
		return E_POINTER;
	ZeroMemory(out, sizeof(*out));
	out->_nLine = info->line;
	out->_ich = info->column;
	out->_pchBuf = info->source ? SysAllocString(info->source) : NULL;
	out->_pszFound = info->reason ? SysAllocString(info->reason) : NULL;
	return S_OK;
}

static const IXMLErrorVtbl error_vtbl = {
	error_QueryInterface,
	error_AddRef,
	error_Release,
	error_GetErrorInfo,
};

HRESULT mx_legacy_document_create(REFIID riid, void **out)
{
	document *self = mx_zalloc(sizeof(*self));
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &document_vtbl;
	self->persist_vtbl = &persist_vtbl;
	self->moniker_vtbl = &moniker_vtbl;
	self->error_vtbl = &error_vtbl;
	self->refs = 1;
	self->async = VARIANT_TRUE;
	document_reset(self);
	status = IXMLDocument2_QueryInterface((IXMLDocument2 *)self, riid, out);
	IXMLDocument2_Release((IXMLDocument2 *)self);
	return status;
}
