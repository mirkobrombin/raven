/* SPDX-License-Identifier: MIT */
#include "node.h"

/* --- One object for both kinds of node list: the live children of a parent, or a found set. --- */

typedef struct
{
	const IXMLDOMSelectionVtbl *vtbl;
	const ISupportErrorInfoVtbl *errors_vtbl;
	mx_dispex dispex;
	LONG refs;
	xmlNodePtr parent;
	xmlNodePtr *items;
	LONG count, position;
	xmlNodePtr cursor;
	BOOL walking;
	void *enumerator;
	xmlDocPtr held;
	BSTR expression;
} mx_list;

static mx_list *list_from(IXMLDOMSelection *iface)
{
	return (mx_list *)iface;
}

static HRESULT enum_shared(void **slot, IUnknown *collection, IXMLDOMNodeList *list, IXMLDOMNamedNodeMap *map, void **out);
static void enum_detach(void *slot);

/* Declarations under a document type are not its children. */
static BOOL list_counts(xmlNodePtr node)
{
	return node->type != XML_ENTITY_DECL && node->type != XML_ELEMENT_DECL && node->type != XML_ATTRIBUTE_DECL && node->type != XML_NAMESPACE_DECL;
}

static xmlNodePtr list_item(mx_list *self, LONG index)
{
	xmlNodePtr at;
	if (index < 0)
		return NULL;
	if (!self->parent)
		return index < self->count ? self->items[index] : NULL;
	for (at = self->parent->children; at; at = at->next)
		if (list_counts(at) && !index--)
			return at;
	return NULL;
}

static LONG list_length(mx_list *self)
{
	xmlNodePtr at;
	LONG count = 0;
	if (!self->parent)
		return self->count;
	for (at = self->parent->children; at; at = at->next)
		if (list_counts(at))
			count++;
	return count;
}

static HRESULT WINAPI list_QueryInterface(IXMLDOMSelection *iface, REFIID riid, void **out)
{
	mx_list *self = list_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IXMLDOMNodeList) ||
		(!self->parent && IsEqualGUID(riid, &IID_IXMLDOMSelection)))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_ISupportErrorInfo))
		*out = &self->errors_vtbl;
	else if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &self->dispex;
	else if (IsEqualGUID(riid, &IID_IEnumVARIANT))
		return enum_shared(&self->enumerator, (IUnknown *)iface, (IXMLDOMNodeList *)iface, NULL, out);
	else
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI list_AddRef(IXMLDOMSelection *iface)
{
	return InterlockedIncrement(&list_from(iface)->refs);
}

static ULONG WINAPI list_Release(IXMLDOMSelection *iface)
{
	mx_list *self = list_from(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	enum_detach(self->enumerator);
	if (self->held)
		mx_tree_unref(self->held);
	mx_free(self->items);
	SysFreeString(self->expression);
	mx_free(self);
	return 0;
}

static HRESULT WINAPI list_GetTypeInfoCount(IXMLDOMSelection *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI list_GetTypeInfo(IXMLDOMSelection *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return index ? DISP_E_BADINDEX : mx_type_info(MX_TYPE_NODE_LIST, out);
}

static HRESULT WINAPI list_GetIDsOfNames(IXMLDOMSelection *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(list_from(iface)->parent ? MX_TYPE_NODE_LIST : MX_TYPE_SELECTION, riid, names, count, lcid, ids);
}

/* A member named by number reads as an item of the collection. */
static HRESULT collection_member(HRESULT status, IXMLDOMNode *node, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	if (!(flags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET)) || (params && params->cArgs))
	{
		if (node)
			IXMLDOMNode_Release(node);
		return flags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET) ? DISP_E_BADPARAMCOUNT : DISP_E_MEMBERNOTFOUND;
	}
	if (FAILED(status))
		return status;
	if (result)
	{
		V_VT(result) = VT_DISPATCH;
		V_DISPATCH(result) = (IDispatch *)node;
	}
	else if (node)
		IXMLDOMNode_Release(node);
	return S_OK;
}

static HRESULT WINAPI list_get_item(IXMLDOMSelection *iface, LONG index, IXMLDOMNode **out);

/* The default member reads an item by number, and the length is no method, whatever the type library would allow. */
static BOOL common_member(IDispatch *collection, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result, HRESULT *status)
{
	if (id == DISPID_DOM_NODELIST_LENGTH && flags == DISPATCH_METHOD)
	{
		if (result)
			V_I4(result) = 0;
		*status = DISP_E_MEMBERNOTFOUND;
		return TRUE;
	}
	if (id != DISPID_VALUE)
		return FALSE;
	if (result)
		V_DISPATCH(result) = NULL;
	if (!(flags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET)))
		*status = DISP_E_MEMBERNOTFOUND;
	else if (!params || params->cArgs != 1)
		*status = DISP_E_BADPARAMCOUNT;
	else
	{
		VARIANT number;
		VariantInit(&number);
		*status = VariantChangeType(&number, params->rgvarg, 0, VT_I4);
		if (SUCCEEDED(*status))
		{
			IXMLDOMNodeList *list;
			IXMLDOMNamedNodeMap *map;
			IXMLDOMNode *node = NULL;
			if (SUCCEEDED(IDispatch_QueryInterface(collection, &IID_IXMLDOMNodeList, (void **)&list)))
			{
				*status = IXMLDOMNodeList_get_item(list, V_I4(&number), &node);
				IXMLDOMNodeList_Release(list);
			}
			else if (SUCCEEDED(IDispatch_QueryInterface(collection, &IID_IXMLDOMNamedNodeMap, (void **)&map)))
			{
				*status = IXMLDOMNamedNodeMap_get_item(map, V_I4(&number), &node);
				IXMLDOMNamedNodeMap_Release(map);
			}
			if (SUCCEEDED(*status))
				*status = S_OK;
			if (result)
			{
				V_VT(result) = VT_DISPATCH;
				V_DISPATCH(result) = (IDispatch *)node;
			}
			else if (node)
				IXMLDOMNode_Release(node);
		}
	}
	return TRUE;
}

static HRESULT WINAPI list_Invoke(IXMLDOMSelection *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	if (id >= DISPID_DOM_COLLECTION_BASE && id <= DISPID_DOM_COLLECTION_MAX)
	{
		IXMLDOMNode *node = NULL;
		HRESULT status = list_get_item(iface, id - DISPID_DOM_COLLECTION_BASE, &node);
		return collection_member(status, node, flags, params, result);
	}
	{
		HRESULT status;
		if (common_member((IDispatch *)iface, id, flags, params, result, &status))
			return status;
	}
	return mx_invoke(list_from(iface)->parent ? MX_TYPE_NODE_LIST : MX_TYPE_SELECTION, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI list_get_item(IXMLDOMSelection *iface, LONG index, IXMLDOMNode **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	/* Reading a query result by number moves its position too. */
	if (!list_from(iface)->parent && index >= 0 && index < list_from(iface)->count)
		list_from(iface)->position = index + 1;
	return mx_node_return(list_item(list_from(iface), index), out);
}

static HRESULT WINAPI list_get_length(IXMLDOMSelection *iface, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = list_length(list_from(iface));
	return S_OK;
}

static HRESULT WINAPI list_nextNode(IXMLDOMSelection *iface, IXMLDOMNode **out)
{
	mx_list *self = list_from(iface);
	xmlNodePtr node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (self->parent)
	{
		/* Walking children follows the links, so taking one out on the way skips nothing. */
		node = self->walking ? self->cursor : self->parent->children;
		while (node && !list_counts(node))
			node = node->next;
		if (node && node->parent != self->parent)
			node = NULL;
		if (!node)
			return S_FALSE;
		self->cursor = node->next;
		self->walking = TRUE;
	}
	else
		node = list_item(self, self->position);
	if (!node)
		return S_FALSE;
	self->position++;
	return mx_node_return(node, out);
}

static HRESULT WINAPI list_reset(IXMLDOMSelection *iface)
{
	list_from(iface)->position = 0;
	list_from(iface)->walking = FALSE;
	return S_OK;
}

static HRESULT WINAPI list_get__newEnum(IXMLDOMSelection *iface, IUnknown **out)
{
	if (!out)
		return E_INVALIDARG;
	return mx_enum_create((IUnknown *)iface, out);
}

static HRESULT WINAPI list_get_expr(IXMLDOMSelection *iface, BSTR *out)
{
	mx_list *self = list_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = SysAllocString(self->expression ? self->expression : L"");
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI list_put_expr(IXMLDOMSelection *iface, BSTR expression)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI list_get_context(IXMLDOMSelection *iface, IXMLDOMNode **out)
{
	mx_list *self = list_from(iface);
	if (!out)
		return E_INVALIDARG;
	return mx_node_return(self->count ? self->items[0]->parent : NULL, out);
}

static HRESULT WINAPI list_putref_context(IXMLDOMSelection *iface, IXMLDOMNode *node)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI list_peekNode(IXMLDOMSelection *iface, IXMLDOMNode **out)
{
	mx_list *self = list_from(iface);
	if (!out)
		return E_INVALIDARG;
	return mx_node_return(list_item(self, self->position), out);
}

static HRESULT WINAPI list_matches(IXMLDOMSelection *iface, IXMLDOMNode *node, IXMLDOMNode **out)
{
	mx_list *self = list_from(iface);
	xmlNodePtr given = mx_node_get((IUnknown *)node);
	LONG index;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	for (index = 0; given && index < list_length(self); index++)
		if (list_item(self, index) == given)
		{
			*out = node;
			IXMLDOMNode_AddRef(node);
			return S_OK;
		}
	return S_FALSE;
}

static HRESULT WINAPI list_removeNext(IXMLDOMSelection *iface, IXMLDOMNode **out)
{
	mx_list *self = list_from(iface);
	xmlNodePtr node = list_item(self, self->position);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!node)
		return S_FALSE;
	memmove(self->items + self->position, self->items + self->position + 1, (self->count - self->position - 1) * sizeof(*self->items));
	self->count--;
	xmlUnlinkNode(node);
	mx_tree_orphan(node);
	return mx_node_return(node, out);
}

static HRESULT WINAPI list_removeAll(IXMLDOMSelection *iface)
{
	mx_list *self = list_from(iface);
	LONG index;
	for (index = self->position; index < self->count; index++)
	{
		xmlUnlinkNode(self->items[index]);
		mx_tree_orphan(self->items[index]);
	}
	self->count = self->position;
	return S_OK;
}

static HRESULT WINAPI list_clone(IXMLDOMSelection *iface, IXMLDOMSelection **out)
{
	mx_list *self = list_from(iface);
	IXMLDOMNodeList *copy;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	status = mx_found_list((xmlNodePtr)self->held, self->items, self->count, &copy);
	if (SUCCEEDED(status))
	{
		status = IXMLDOMNodeList_QueryInterface(copy, &IID_IXMLDOMSelection, (void **)out);
		IXMLDOMNodeList_Release(copy);
	}
	return status;
}

static HRESULT WINAPI list_getProperty(IXMLDOMSelection *iface, BSTR name, VARIANT *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI list_setProperty(IXMLDOMSelection *iface, BSTR name, VARIANT value)
{
	return E_NOTIMPL;
}

static const IXMLDOMSelectionVtbl list_table = {
	list_QueryInterface, list_AddRef, list_Release, list_GetTypeInfoCount, list_GetTypeInfo, list_GetIDsOfNames, list_Invoke,
	list_get_item, list_get_length, list_nextNode, list_reset, list_get__newEnum,
	list_get_expr, list_put_expr, list_get_context, list_putref_context, list_peekNode, list_matches,
	list_removeNext, list_removeAll, list_clone, list_getProperty, list_setProperty,
};

static HRESULT WINAPI list_errors_QueryInterface(ISupportErrorInfo *iface, REFIID riid, void **out)
{
	return list_QueryInterface((IXMLDOMSelection *)CONTAINING_RECORD(iface, mx_list, errors_vtbl), riid, out);
}

static ULONG WINAPI list_errors_AddRef(ISupportErrorInfo *iface)
{
	return list_AddRef((IXMLDOMSelection *)CONTAINING_RECORD(iface, mx_list, errors_vtbl));
}

static ULONG WINAPI list_errors_Release(ISupportErrorInfo *iface)
{
	return list_Release((IXMLDOMSelection *)CONTAINING_RECORD(iface, mx_list, errors_vtbl));
}

static HRESULT WINAPI list_errors_InterfaceSupportsErrorInfo(ISupportErrorInfo *iface, REFIID riid)
{
	return IsEqualGUID(riid, &IID_IXMLDOMNodeList) ? S_OK : S_FALSE;
}

static const ISupportErrorInfoVtbl list_errors_table = {
	list_errors_QueryInterface, list_errors_AddRef, list_errors_Release, list_errors_InterfaceSupportsErrorInfo,
};

static mx_list *list_new(xmlDocPtr tree)
{
	mx_list *self = mx_zalloc(sizeof(*self));
	if (!self)
		return NULL;
	self->vtbl = &list_table;
	mx_dispex_init(&self->dispex, self, TRUE);
	self->errors_vtbl = &list_errors_table;
	self->refs = 1;
	self->held = tree;
	if (tree)
		mx_tree_ref(tree);
	return self;
}

HRESULT mx_children_list(xmlNodePtr parent, IXMLDOMNodeList **out)
{
	mx_list *self = list_new(parent->doc);
	if (!self)
		return E_OUTOFMEMORY;
	self->parent = parent;
	/* Character nodes hold their text in the same place other nodes hold children. */
	switch (parent->type)
	{
	case XML_ELEMENT_NODE:
	case XML_ATTRIBUTE_NODE:
	case XML_DOCUMENT_NODE:
	case XML_DOCUMENT_FRAG_NODE:
	case XML_ENTITY_REF_NODE:
	case XML_ENTITY_DECL:
		break;
	default:
		self->parent = NULL;
		break;
	}
	*out = (IXMLDOMNodeList *)self;
	return S_OK;
}

HRESULT mx_found_list(xmlNodePtr context, xmlNodePtr *items, LONG count, IXMLDOMNodeList **out)
{
	mx_list *self = list_new(context->doc);
	if (!self)
		return E_OUTOFMEMORY;
	if (count)
	{
		self->items = mx_alloc(count * sizeof(*items));
		if (!self->items)
		{
			list_Release((IXMLDOMSelection *)self);
			return E_OUTOFMEMORY;
		}
		memcpy(self->items, items, count * sizeof(*items));
	}
	self->count = count;
	*out = (IXMLDOMNodeList *)self;
	return S_OK;
}

/* --- An enumerator for either kind of collection. --- */

typedef struct
{
	const IEnumVARIANTVtbl *vtbl;
	LONG refs;
	IUnknown *collection;
	IXMLDOMNodeList *as_list;
	IXMLDOMNamedNodeMap *as_map;
	LONG position;
	BOOL shared;
} mx_enum;

static HRESULT WINAPI enum_QueryInterface(IEnumVARIANT *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	/* The face a collection itself answers with belongs to it for everything but enumerating. */
	if (((mx_enum *)iface)->shared && ((mx_enum *)iface)->collection && !IsEqualGUID(riid, &IID_IEnumVARIANT))
		return IUnknown_QueryInterface(((mx_enum *)iface)->collection, riid, out);
	if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IEnumVARIANT))
		return E_NOINTERFACE;
	*out = iface;
	IEnumVARIANT_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI enum_AddRef(IEnumVARIANT *iface)
{
	return InterlockedIncrement(&((mx_enum *)iface)->refs);
}

static ULONG WINAPI enum_Release(IEnumVARIANT *iface)
{
	mx_enum *self = (mx_enum *)iface;
	ULONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	if (!self->shared)
	{
		if (self->as_list)
			IXMLDOMNodeList_Release(self->as_list);
		if (self->as_map)
			IXMLDOMNamedNodeMap_Release(self->as_map);
	}
	mx_free(self);
	return 0;
}

static HRESULT enum_item(mx_enum *self, LONG index, IXMLDOMNode **out)
{
	if (!self->as_list && !self->as_map)
		return S_FALSE;
	return self->as_list ? IXMLDOMNodeList_get_item(self->as_list, index, out) : IXMLDOMNamedNodeMap_get_item(self->as_map, index, out);
}

static HRESULT WINAPI enum_Next(IEnumVARIANT *iface, ULONG wanted, VARIANT *items, ULONG *fetched)
{
	mx_enum *self = (mx_enum *)iface;
	IXMLDOMSelection *selection = NULL;
	ULONG got = 0;
	if (fetched)
		*fetched = 0;
	if (!items && wanted)
		return E_INVALIDARG;
	if (self->as_list)
		IXMLDOMNodeList_QueryInterface(self->as_list, &IID_IXMLDOMSelection, (void **)&selection);
	while (got < wanted)
	{
		IXMLDOMNode *node = NULL, *skipped = NULL;
		if (enum_item(self, self->position, &node) != S_OK)
			break;
		self->position++;
		/* A selection's own cursor moves one further than the item read through it. */
		if (selection && IXMLDOMSelection_nextNode(selection, &skipped) == S_OK)
			IXMLDOMNode_Release(skipped);
		V_VT(&items[got]) = VT_DISPATCH;
		V_DISPATCH(&items[got]) = (IDispatch *)node;
		got++;
	}
	if (selection)
		IXMLDOMSelection_Release(selection);
	if (fetched)
		*fetched = got;
	for (ULONG rest = got; rest < wanted; rest++)
		V_VT(&items[rest]) = VT_EMPTY;
	return got == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Skip(IEnumVARIANT *iface, ULONG count)
{
	((mx_enum *)iface)->position += count;
	return S_OK;
}

static HRESULT WINAPI enum_Reset(IEnumVARIANT *iface)
{
	((mx_enum *)iface)->position = 0;
	return S_OK;
}

static HRESULT WINAPI enum_Clone(IEnumVARIANT *iface, IEnumVARIANT **out)
{
	return E_NOTIMPL;
}

static const IEnumVARIANTVtbl enum_table = {
	enum_QueryInterface, enum_AddRef, enum_Release, enum_Next, enum_Skip, enum_Reset, enum_Clone,
};

HRESULT mx_enum_create(IUnknown *collection, IUnknown **out)
{
	mx_enum *self = mx_zalloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &enum_table;
	self->refs = 1;
	self->collection = collection;
	if (FAILED(IUnknown_QueryInterface(collection, &IID_IXMLDOMNodeList, (void **)&self->as_list)))
		IUnknown_QueryInterface(collection, &IID_IXMLDOMNamedNodeMap, (void **)&self->as_map);
	*out = (IUnknown *)self;
	return S_OK;
}

/* The enumerator a collection answers with itself: one for the collection's life, which holds no reference back. */
static HRESULT enum_shared(void **slot, IUnknown *collection, IXMLDOMNodeList *list, IXMLDOMNamedNodeMap *map, void **out)
{
	mx_enum *self = *slot;
	if (!self)
	{
		self = mx_zalloc(sizeof(*self));
		if (!self)
			return E_OUTOFMEMORY;
		self->vtbl = &enum_table;
		self->refs = 1;
		self->shared = TRUE;
		self->collection = collection;
		self->as_list = list;
		self->as_map = map;
		*slot = self;
	}
	IEnumVARIANT_AddRef((IEnumVARIANT *)self);
	*out = self;
	return S_OK;
}

static void enum_detach(void *slot)
{
	mx_enum *self = slot;
	if (!self)
		return;
	self->collection = NULL;
	self->as_list = NULL;
	self->as_map = NULL;
	IEnumVARIANT_Release((IEnumVARIANT *)self);
}

/* --- Attribute maps. A namespace declaration is an attribute here, though libxml keeps it apart. --- */

/* The one a new element got for its own name does not count as written. */
static xmlNsPtr visible_declarations(xmlNodePtr owner)
{
	return (owner->extra & MX_IMPLIED_DECLARATION) && owner->nsDef ? owner->nsDef->next : owner->nsDef;
}

typedef struct
{
	const IXMLDOMNamedNodeMapVtbl *vtbl;
	const ISupportErrorInfoVtbl *errors_vtbl;
	mx_dispex dispex;
	LONG refs;
	xmlNodePtr owner, declaration;
	xmlDocPtr held;
	xmlNodePtr *fixed;
	LONG fixed_count, position;
	void *enumerator;
} mx_map;

static mx_map *map_from(IXMLDOMNamedNodeMap *iface)
{
	return (mx_map *)iface;
}

static LONG map_length(mx_map *self)
{
	LONG count = 0;
	xmlAttrPtr attribute;
	xmlNsPtr space;
	if (!self->owner)
		return self->fixed_count;
	if (self->owner->type != XML_ELEMENT_NODE)
		return 0;
	for (space = visible_declarations(self->owner); space; space = space->next)
		count++;
	for (attribute = self->owner->properties; attribute; attribute = attribute->next)
		count++;
	return count;
}

/* The declaration as an attribute object of its own, valid as long as the tree. */
const char mx_declaration_mark[] = "declaration";

xmlNodePtr mx_declaration_attribute(xmlNodePtr owner, xmlNsPtr space)
{
	xmlChar *name = xmlStrdup((const xmlChar *)"xmlns");
	xmlAttrPtr attribute;
	if (space->prefix)
	{
		name = xmlStrcat(name, (const xmlChar *)":");
		name = xmlStrcat(name, space->prefix);
	}
	attribute = xmlNewDocProp(owner->doc, name, NULL);
	xmlFree(name);
	if (!attribute)
		return NULL;
	if (space->href)
	{
		xmlChar *value = xmlEncodeSpecialChars(owner->doc, space->href);
		xmlNodeSetContent((xmlNodePtr)attribute, value);
		xmlFree(value);
	}
	attribute->psvi = (void *)mx_declaration_mark;
	mx_tree_orphan((xmlNodePtr)attribute);
	return (xmlNodePtr)attribute;
}

static xmlNodePtr map_item(mx_map *self, LONG index)
{
	xmlAttrPtr attribute;
	xmlNsPtr space;
	if (index < 0)
		return NULL;
	if (!self->owner)
		return index < self->fixed_count ? self->fixed[index] : NULL;
	if (self->owner->type != XML_ELEMENT_NODE)
		return NULL;
	/* Attributes come in the order written, and a parser notes when they came before the declarations. */
	if (self->owner->extra & MX_ATTRIBUTES_FIRST)
		for (attribute = self->owner->properties; attribute; attribute = attribute->next)
			if (!index--)
				return (xmlNodePtr)attribute;
	for (space = visible_declarations(self->owner); space; space = space->next)
		if (!index--)
			return mx_declaration_attribute(self->owner, space);
	if (!(self->owner->extra & MX_ATTRIBUTES_FIRST))
		for (attribute = self->owner->properties; attribute; attribute = attribute->next)
			if (!index--)
				return (xmlNodePtr)attribute;
	return NULL;
}

static HRESULT WINAPI map_QueryInterface(IXMLDOMNamedNodeMap *iface, REFIID riid, void **out)
{
	mx_map *self = map_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IXMLDOMNamedNodeMap))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_ISupportErrorInfo))
		*out = &self->errors_vtbl;
	else if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &self->dispex;
	else if (IsEqualGUID(riid, &IID_IEnumVARIANT))
		return enum_shared(&self->enumerator, (IUnknown *)iface, NULL, iface, out);
	else
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI map_AddRef(IXMLDOMNamedNodeMap *iface)
{
	return InterlockedIncrement(&map_from(iface)->refs);
}

static ULONG WINAPI map_Release(IXMLDOMNamedNodeMap *iface)
{
	mx_map *self = map_from(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	enum_detach(self->enumerator);
	if (self->held)
		mx_tree_unref(self->held);
	mx_free(self->fixed);
	mx_free(self);
	return 0;
}

static HRESULT WINAPI map_GetTypeInfoCount(IXMLDOMNamedNodeMap *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI map_GetTypeInfo(IXMLDOMNamedNodeMap *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return index ? DISP_E_BADINDEX : mx_type_info(MX_TYPE_NODE_MAP, out);
}

static HRESULT WINAPI map_GetIDsOfNames(IXMLDOMNamedNodeMap *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_NODE_MAP, riid, names, count, lcid, ids);
}

static HRESULT WINAPI map_get_item(IXMLDOMNamedNodeMap *iface, LONG index, IXMLDOMNode **out);

static HRESULT WINAPI map_Invoke(IXMLDOMNamedNodeMap *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	if (id >= DISPID_DOM_COLLECTION_BASE && id <= DISPID_DOM_COLLECTION_MAX)
	{
		IXMLDOMNode *node = NULL;
		HRESULT status = map_get_item(iface, id - DISPID_DOM_COLLECTION_BASE, &node);
		return collection_member(status, node, flags, params, result);
	}
	{
		HRESULT status;
		if (common_member((IDispatch *)iface, id, flags, params, result, &status))
			return status;
	}
	return mx_invoke(MX_TYPE_NODE_MAP, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static xmlNodePtr map_find(mx_map *self, const xmlChar *qualified)
{
	const xmlChar *colon = xmlStrchr(qualified, ':');
	xmlChar *prefix = colon ? xmlStrndup(qualified, (int)(colon - qualified)) : NULL;
	const xmlChar *local = colon ? colon + 1 : qualified;
	xmlNodePtr found = NULL;
	if (!self->owner)
	{
		LONG index;
		for (index = 0; index < self->fixed_count && !found; index++)
			if (!xmlStrcmp(self->fixed[index]->name, qualified))
				found = self->fixed[index];
	}
	else if (self->owner->type == XML_ELEMENT_NODE)
	{
		xmlAttrPtr attribute;
		xmlNsPtr space;
		if (!xmlStrcmp(qualified, (const xmlChar *)"xmlns") || (prefix && !xmlStrcmp(prefix, (const xmlChar *)"xmlns")))
			for (space = visible_declarations(self->owner); space && !found; space = space->next)
				if (prefix ? space->prefix && !xmlStrcmp(space->prefix, local) : !space->prefix)
					found = mx_declaration_attribute(self->owner, space);
		for (attribute = self->owner->properties; attribute && !found; attribute = attribute->next)
			if (!xmlStrcmp(attribute->name, local) && (prefix ? attribute->ns && attribute->ns->prefix && !xmlStrcmp(attribute->ns->prefix, prefix) : !attribute->ns))
				found = (xmlNodePtr)attribute;
		for (attribute = self->owner->properties; attribute && !found; attribute = attribute->next)
			if (!xmlStrcmp(attribute->name, qualified) && !attribute->ns)
				found = (xmlNodePtr)attribute;
	}
	xmlFree(prefix);
	return found;
}

static HRESULT WINAPI map_getNamedItem(IXMLDOMNamedNodeMap *iface, BSTR name, IXMLDOMNode **out)
{
	xmlChar *utf8;
	xmlNodePtr found;
	if (!name || !out)
		return E_INVALIDARG;
	*out = NULL;
	utf8 = mx_to_utf8(name);
	found = map_find(map_from(iface), utf8);
	xmlFree(utf8);
	return mx_node_return(found, out);
}

static void declaration_items(mx_map *self, xmlNodePtr owner);

/* Changes what a declaration says while what was written of it stays: the first change sets the written text aside. */
static void declaration_edit(mx_map *self, const xmlChar *name, const xmlChar *value)
{
	xmlNodePtr owner = self->declaration;
	xmlBufferPtr rewritten = xmlBufferCreate();
	LONG index;
	BOOL placed = FALSE;
	if (!owner->psvi && !xmlStrcmp(name, (const xmlChar *)"version"))
	{
		xmlNodePtr written = xmlNewDocText(owner->doc, owner->content);
		if (written)
		{
			mx_tree_hold(owner->doc, written);
			owner->psvi = written;
		}
	}
	for (index = 0; index < self->fixed_count; index++)
	{
		const xmlNodePtr item = self->fixed[index];
		xmlChar *current = xmlStrEqual(item->name, name) ? (value ? xmlStrdup(value) : NULL) : xmlNodeGetContent(item);
		placed |= xmlStrEqual(item->name, name);
		if (!current)
			continue;
		if (xmlBufferLength(rewritten))
			xmlBufferCCat(rewritten, " ");
		xmlBufferCat(rewritten, item->name);
		xmlBufferCCat(rewritten, "=\"");
		xmlBufferCat(rewritten, current);
		xmlBufferCCat(rewritten, "\"");
		xmlFree(current);
	}
	if (!placed && value)
	{
		if (xmlBufferLength(rewritten))
			xmlBufferCCat(rewritten, " ");
		xmlBufferCat(rewritten, name);
		xmlBufferCCat(rewritten, "=\"");
		xmlBufferCat(rewritten, value);
		xmlBufferCCat(rewritten, "\"");
	}
	xmlNodeSetContent(owner, xmlBufferLength(rewritten) ? xmlBufferContent(rewritten) : (const xmlChar *)"");
	xmlBufferFree(rewritten);
	declaration_items(self, owner);
}

static HRESULT WINAPI map_setNamedItem(IXMLDOMNamedNodeMap *iface, IXMLDOMNode *node, IXMLDOMNode **out)
{
	mx_map *self = map_from(iface);
	xmlNodePtr given = mx_node_get((IUnknown *)node);
	xmlAttrPtr existing;
	if (out)
		*out = NULL;
	if (!node)
		return E_INVALIDARG;
	if (self->declaration && given && given->type == XML_ATTRIBUTE_NODE)
	{
		xmlChar *value = xmlNodeGetContent(given);
		declaration_edit(self, given->name, value);
		xmlFree(value);
		if (out)
		{
			*out = node;
			IXMLDOMNode_AddRef(node);
		}
		return S_OK;
	}
	if (!self->owner)
		return E_FAIL;
	if (!given || given->type != XML_ATTRIBUTE_NODE)
		return E_FAIL;
	if (given->parent)
		return E_INVALIDARG;
	existing = given->ns ? xmlHasNsProp(self->owner, given->name, given->ns->href) : xmlHasNsProp(self->owner, given->name, NULL);
	if (existing)
	{
		xmlUnlinkNode((xmlNodePtr)existing);
		mx_tree_orphan((xmlNodePtr)existing);
	}
	mx_tree_adopted(given);
	xmlAddChild(self->owner, given);
	if (out)
	{
		*out = node;
		IXMLDOMNode_AddRef(node);
	}
	return S_OK;
}

static HRESULT WINAPI map_removeNamedItem(IXMLDOMNamedNodeMap *iface, BSTR name, IXMLDOMNode **out)
{
	mx_map *self = map_from(iface);
	xmlChar *utf8;
	xmlNodePtr found;
	if (!name)
		return E_INVALIDARG;
	if (out)
		*out = NULL;
	utf8 = mx_to_utf8(name);
	if (self->declaration)
	{
		HRESULT status = S_FALSE;
		LONG index;
		for (index = 0; index < self->fixed_count; index++)
			if (!xmlStrcmp(self->fixed[index]->name, utf8))
			{
				status = S_OK;
				if (out)
					mx_node_return(self->fixed[index], out);
				declaration_edit(self, utf8, NULL);
				break;
			}
		xmlFree(utf8);
		return status;
	}
	if (!self->owner)
	{
		xmlFree(utf8);
		return E_FAIL;
	}
	found = map_find(self, utf8);
	xmlFree(utf8);
	if (!found)
		return S_FALSE;
	if (found->parent)
	{
		xmlUnlinkNode(found);
		mx_tree_orphan(found);
	}
	return out ? mx_node_return(found, out) : S_OK;
}

static HRESULT WINAPI map_get_item(IXMLDOMNamedNodeMap *iface, LONG index, IXMLDOMNode **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return mx_node_return(map_item(map_from(iface), index), out);
}

static HRESULT WINAPI map_get_length(IXMLDOMNamedNodeMap *iface, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = map_length(map_from(iface));
	return S_OK;
}

static xmlAttrPtr map_find_qualified(mx_map *self, const WCHAR *name, const WCHAR *uri)
{
	xmlChar *local = mx_to_utf8(name), *space = uri && *uri ? mx_to_utf8(uri) : NULL;
	xmlAttrPtr found = self->owner && self->owner->type == XML_ELEMENT_NODE ? xmlHasNsProp(self->owner, local, space) : NULL;
	xmlFree(local);
	xmlFree(space);
	return found;
}

static HRESULT WINAPI map_getQualifiedItem(IXMLDOMNamedNodeMap *iface, BSTR name, BSTR uri, IXMLDOMNode **out)
{
	if (!name || !out)
		return E_INVALIDARG;
	*out = NULL;
	return mx_node_return((xmlNodePtr)map_find_qualified(map_from(iface), name, uri), out);
}

static HRESULT WINAPI map_removeQualifiedItem(IXMLDOMNamedNodeMap *iface, BSTR name, BSTR uri, IXMLDOMNode **out)
{
	mx_map *self = map_from(iface);
	xmlAttrPtr found;
	if (!name)
		return E_INVALIDARG;
	if (out)
		*out = NULL;
	found = map_find_qualified(self, name, uri);
	if (!found)
		return S_FALSE;
	xmlUnlinkNode((xmlNodePtr)found);
	mx_tree_orphan((xmlNodePtr)found);
	return out ? mx_node_return((xmlNodePtr)found, out) : S_OK;
}

static HRESULT WINAPI map_nextNode(IXMLDOMNamedNodeMap *iface, IXMLDOMNode **out)
{
	mx_map *self = map_from(iface);
	xmlNodePtr node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	node = map_item(self, self->position);
	if (!node)
		return S_FALSE;
	self->position++;
	return mx_node_return(node, out);
}

static HRESULT WINAPI map_reset(IXMLDOMNamedNodeMap *iface)
{
	map_from(iface)->position = 0;
	return S_OK;
}

static HRESULT WINAPI map_get__newEnum(IXMLDOMNamedNodeMap *iface, IUnknown **out)
{
	if (!out)
		return E_INVALIDARG;
	return mx_enum_create((IUnknown *)iface, out);
}

static const IXMLDOMNamedNodeMapVtbl map_table = {
	map_QueryInterface, map_AddRef, map_Release, map_GetTypeInfoCount, map_GetTypeInfo, map_GetIDsOfNames, map_Invoke,
	map_getNamedItem, map_setNamedItem, map_removeNamedItem, map_get_item, map_get_length,
	map_getQualifiedItem, map_removeQualifiedItem, map_nextNode, map_reset, map_get__newEnum,
};

static HRESULT WINAPI map_errors_QueryInterface(ISupportErrorInfo *iface, REFIID riid, void **out)
{
	return map_QueryInterface((IXMLDOMNamedNodeMap *)CONTAINING_RECORD(iface, mx_map, errors_vtbl), riid, out);
}

static ULONG WINAPI map_errors_AddRef(ISupportErrorInfo *iface)
{
	return map_AddRef((IXMLDOMNamedNodeMap *)CONTAINING_RECORD(iface, mx_map, errors_vtbl));
}

static ULONG WINAPI map_errors_Release(ISupportErrorInfo *iface)
{
	return map_Release((IXMLDOMNamedNodeMap *)CONTAINING_RECORD(iface, mx_map, errors_vtbl));
}

static HRESULT WINAPI map_errors_InterfaceSupportsErrorInfo(ISupportErrorInfo *iface, REFIID riid)
{
	return IsEqualGUID(riid, &IID_IXMLDOMNamedNodeMap) ? S_OK : S_FALSE;
}

static const ISupportErrorInfoVtbl map_errors_table = {
	map_errors_QueryInterface, map_errors_AddRef, map_errors_Release, map_errors_InterfaceSupportsErrorInfo,
};

static mx_map *map_new(xmlDocPtr tree)
{
	mx_map *self = mx_zalloc(sizeof(*self));
	if (!self)
		return NULL;
	self->vtbl = &map_table;
	mx_dispex_init(&self->dispex, self, TRUE);
	self->errors_vtbl = &map_errors_table;
	self->refs = 1;
	self->held = tree;
	if (tree)
		mx_tree_ref(tree);
	return self;
}

/* The pseudo attributes of a declaration, each an attribute object of its own. */
static void declaration_items(mx_map *self, xmlNodePtr owner)
{
	const xmlChar *at = owner->content;
	LONG room = 0;
	self->fixed_count = 0;
	while (at && *at)
	{
		const xmlChar *name = at, *equal = xmlStrchr(at, '='), *close;
		xmlChar *key, *value;
		xmlAttrPtr attribute;
		if (!equal || (equal[1] != '"' && equal[1] != '\''))
			break;
		close = xmlStrchr(equal + 2, equal[1]);
		if (!close)
			break;
		key = xmlStrndup(name, (int)(equal - name));
		value = xmlStrndup(equal + 2, (int)(close - equal - 2));
		attribute = xmlNewDocProp(owner->doc, key, value);
		xmlFree(key);
		xmlFree(value);
		if (attribute)
		{
			if (self->fixed_count == room)
			{
				room = room ? room * 2 : 4;
				self->fixed = mx_realloc(self->fixed, room * sizeof(*self->fixed));
			}
			self->fixed[self->fixed_count++] = (xmlNodePtr)attribute;
			mx_tree_orphan((xmlNodePtr)attribute);
		}
		at = close + 1;
		while (*at == ' ')
			at++;
	}
}

HRESULT mx_attribute_map(xmlNodePtr owner, IXMLDOMNamedNodeMap **out)
{
	mx_map *self = map_new(owner->doc);
	if (!self)
		return E_OUTOFMEMORY;
	if (owner->type == XML_PI_NODE)
	{
		self->declaration = owner;
		declaration_items(self, owner);
	}
	else
		self->owner = owner;
	*out = (IXMLDOMNamedNodeMap *)self;
	return S_OK;
}

typedef struct
{
	mx_map *map;
	xmlDocPtr tree;
	BOOL notations;
	LONG room;
} dtd_walk;

/* Notations have no tree node in libxml: each gets an element-shaped stand-in with its names. */
static xmlNodePtr notation_node(xmlDocPtr tree, xmlNotationPtr notation)
{
	xmlNodePtr node = xmlNewDocNode(tree, NULL, notation->name, NULL);
	if (!node)
		return NULL;
	if (notation->PublicID)
		xmlSetProp(node, (const xmlChar *)"public", notation->PublicID);
	if (notation->SystemID)
		xmlSetProp(node, (const xmlChar *)"system", notation->SystemID);
	node->type = XML_NOTATION_NODE;
	mx_tree_orphan(node);
	return node;
}

static void dtd_visit(void *payload, void *context, const xmlChar *name)
{
	dtd_walk *walk = context;
	mx_map *map = walk->map;
	xmlNodePtr node;
	if (map->fixed_count == walk->room)
	{
		walk->room = walk->room ? walk->room * 2 : 8;
		map->fixed = mx_realloc(map->fixed, walk->room * sizeof(*map->fixed));
	}
	node = walk->notations ? notation_node(walk->tree, payload) : payload;
	if (node)
		map->fixed[map->fixed_count++] = node;
}

HRESULT mx_dtd_map(xmlDocPtr tree, xmlHashTablePtr table, IXMLDOMNamedNodeMap **out)
{
	mx_map *self = map_new(tree);
	if (!self)
		return E_OUTOFMEMORY;
	if (table)
	{
		xmlDtdPtr type = xmlGetIntSubset(tree);
		dtd_walk walk = {self, tree, type && table == type->notations, 0};
		xmlHashScan(table, dtd_visit, &walk);
	}
	*out = (IXMLDOMNamedNodeMap *)self;
	return S_OK;
}

/* --- Parse errors. --- */

typedef struct
{
	const IXMLDOMParseError2Vtbl *vtbl;
	const ISupportErrorInfoVtbl *errors_vtbl;
	mx_dispex dispex;
	LONG refs;
	mx_error_info info;
} mx_parse_error;

static mx_parse_error *error_from(IXMLDOMParseError2 *iface)
{
	return (mx_parse_error *)iface;
}

void mx_error_clear(mx_error_info *info)
{
	SysFreeString(info->url);
	SysFreeString(info->reason);
	SysFreeString(info->source);
	memset(info, 0, sizeof(*info));
}

/* The codes programs test for; anything else keeps the general parse failure. */
static LONG error_code_of(int code)
{
	switch (code)
	{
	case XML_ERR_DOCUMENT_EMPTY: return (LONG)0xC00CE558;
	case XML_ERR_DOCUMENT_END: return (LONG)0xC00CE555;
	case XML_ERR_TAG_NAME_MISMATCH: return (LONG)0xC00CE56D;
	case XML_ERR_TAG_NOT_FINISHED: return (LONG)0xC00CE55F;
	case XML_ERR_LT_IN_ATTRIBUTE: return (LONG)0xC00CE506;
	case XML_ERR_ATTRIBUTE_NOT_STARTED: return (LONG)0xC00CE502;
	case XML_ERR_NAME_REQUIRED: return (LONG)0xC00CE504;
	case XML_ERR_UNDECLARED_ENTITY: return (LONG)0xC00CE002;
	case XML_ERR_ATTRIBUTE_REDEFINED: return (LONG)0xC00CE554;
	case XML_ERR_DOCTYPE_NOT_FINISHED: return (LONG)0xC00CE55E;
	default: return (LONG)0xC00CE500;
	}
}

static LONG validity_code_of(const xmlError *error)
{
	switch (error->code)
	{
	case XML_DTD_UNKNOWN_ELEM: return (LONG)0xC00CE00D;
	case XML_DTD_ROOT_NAME: return (LONG)0xC00CE013;
	case XML_DTD_UNKNOWN_ATTRIBUTE: return (LONG)0xC00CE015;
	case XML_DTD_ATTRIBUTE_DEFAULT: return (LONG)0xC00CE016;
	case XML_DTD_ATTRIBUTE_VALUE: return (LONG)0xC00CE017;
	case XML_DTD_MISSING_ATTRIBUTE: return (LONG)0xC00CE020;
	default:
		return error->message && strstr(error->message, "CDATA") ? (LONG)0xC00CE018 : (LONG)0xC00CE014;
	}
}

void mx_error_from_libxml(mx_error_info *info, const xmlError *error, const WCHAR *url)
{
	mx_error_clear(info);
	if (url)
		info->url = SysAllocString(url);
	if (!error)
	{
		info->code = (LONG)0xC00CE558;
		info->reason = SysAllocString(L"XML document must have a top level element.\r\n");
		return;
	}
	info->code = error->domain == XML_FROM_SCHEMASV ? (LONG)0xC00CE014 : error->domain == XML_FROM_VALID ? validity_code_of(error) : error_code_of(error->code);
	info->line = error->line;
	info->column = error->int2;
	info->reason = error->message ? mx_bstr((const xmlChar *)error->message) : SysAllocString(L"");
	if (error->str1)
		info->source = mx_bstr((const xmlChar *)error->str1);
}

static HRESULT WINAPI error_QueryInterface(IXMLDOMParseError2 *iface, REFIID riid, void **out)
{
	mx_parse_error *self = error_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IXMLDOMParseError) || IsEqualGUID(riid, &IID_IXMLDOMParseError2))
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

static ULONG WINAPI error_AddRef(IXMLDOMParseError2 *iface)
{
	return InterlockedIncrement(&error_from(iface)->refs);
}

static ULONG WINAPI error_Release(IXMLDOMParseError2 *iface)
{
	mx_parse_error *self = error_from(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	mx_error_clear(&self->info);
	mx_free(self);
	return 0;
}

static HRESULT WINAPI error_GetTypeInfoCount(IXMLDOMParseError2 *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI error_GetTypeInfo(IXMLDOMParseError2 *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return index ? DISP_E_BADINDEX : mx_type_info(MX_TYPE_PARSE_ERROR, out);
}

static HRESULT WINAPI error_GetIDsOfNames(IXMLDOMParseError2 *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_PARSE_ERROR, riid, names, count, lcid, ids);
}

static HRESULT WINAPI error_Invoke(IXMLDOMParseError2 *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_PARSE_ERROR, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI error_get_errorCode(IXMLDOMParseError2 *iface, LONG *out)
{
	mx_parse_error *self = error_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = self->info.code;
	return self->info.code ? S_OK : S_FALSE;
}

static HRESULT return_error_text(BSTR text, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!text)
		return S_FALSE;
	*out = SysAllocString(text);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI error_get_url(IXMLDOMParseError2 *iface, BSTR *out)
{
	return return_error_text(error_from(iface)->info.url, out);
}

static HRESULT WINAPI error_get_reason(IXMLDOMParseError2 *iface, BSTR *out)
{
	return return_error_text(error_from(iface)->info.reason, out);
}

static HRESULT WINAPI error_get_srcText(IXMLDOMParseError2 *iface, BSTR *out)
{
	return return_error_text(error_from(iface)->info.source, out);
}

static HRESULT WINAPI error_get_line(IXMLDOMParseError2 *iface, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = error_from(iface)->info.line;
	return S_OK;
}

static HRESULT WINAPI error_get_linepos(IXMLDOMParseError2 *iface, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = error_from(iface)->info.column;
	return S_OK;
}

static HRESULT WINAPI error_get_filepos(IXMLDOMParseError2 *iface, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = error_from(iface)->info.position;
	return S_OK;
}

static HRESULT WINAPI error_get_errorXPath(IXMLDOMParseError2 *iface, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return S_FALSE;
}

static HRESULT WINAPI error_get_allErrors(IXMLDOMParseError2 *iface, IXMLDOMParseErrorCollection **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI error_errorParameters(IXMLDOMParseError2 *iface, LONG index, BSTR *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI error_get_errorParametersCount(IXMLDOMParseError2 *iface, LONG *out)
{
	return E_NOTIMPL;
}

static const IXMLDOMParseError2Vtbl error_table = {
	error_QueryInterface, error_AddRef, error_Release, error_GetTypeInfoCount, error_GetTypeInfo, error_GetIDsOfNames, error_Invoke,
	error_get_errorCode, error_get_url, error_get_reason, error_get_srcText, error_get_line, error_get_linepos, error_get_filepos,
	error_get_errorXPath, error_get_allErrors, error_errorParameters, error_get_errorParametersCount,
};

static HRESULT WINAPI error_errors_QueryInterface(ISupportErrorInfo *iface, REFIID riid, void **out)
{
	return error_QueryInterface((IXMLDOMParseError2 *)CONTAINING_RECORD(iface, mx_parse_error, errors_vtbl), riid, out);
}

static ULONG WINAPI error_errors_AddRef(ISupportErrorInfo *iface)
{
	return error_AddRef((IXMLDOMParseError2 *)CONTAINING_RECORD(iface, mx_parse_error, errors_vtbl));
}

static ULONG WINAPI error_errors_Release(ISupportErrorInfo *iface)
{
	return error_Release((IXMLDOMParseError2 *)CONTAINING_RECORD(iface, mx_parse_error, errors_vtbl));
}

static HRESULT WINAPI error_errors_InterfaceSupportsErrorInfo(ISupportErrorInfo *iface, REFIID riid)
{
	return IsEqualGUID(riid, &IID_IXMLDOMParseError) ? S_OK : S_FALSE;
}

static const ISupportErrorInfoVtbl error_errors_table = {
	error_errors_QueryInterface, error_errors_AddRef, error_errors_Release, error_errors_InterfaceSupportsErrorInfo,
};

HRESULT mx_parse_error_create(const mx_error_info *info, IXMLDOMParseError **out)
{
	mx_parse_error *self = mx_zalloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &error_table;
	mx_dispex_init(&self->dispex, self, FALSE);
	self->errors_vtbl = &error_errors_table;
	self->refs = 1;
	self->info = *info;
	self->info.url = info->url ? SysAllocString(info->url) : NULL;
	self->info.reason = info->reason ? SysAllocString(info->reason) : NULL;
	self->info.source = info->source ? SysAllocString(info->source) : NULL;
	*out = (IXMLDOMParseError *)self;
	return S_OK;
}

/* --- The feature list. --- */

typedef struct
{
	const IXMLDOMImplementationVtbl *vtbl;
	mx_dispex dispex;
	LONG refs;
} mx_implementation;

static HRESULT WINAPI implementation_QueryInterface(IXMLDOMImplementation *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &((mx_implementation *)iface)->dispex;
	else if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IDispatch) && !IsEqualGUID(riid, &IID_IXMLDOMImplementation))
		return E_NOINTERFACE;
	else
		*out = iface;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI implementation_AddRef(IXMLDOMImplementation *iface)
{
	return InterlockedIncrement(&((mx_implementation *)iface)->refs);
}

static ULONG WINAPI implementation_Release(IXMLDOMImplementation *iface)
{
	mx_implementation *self = (mx_implementation *)iface;
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left)
		mx_free(self);
	return left;
}

static HRESULT WINAPI implementation_GetTypeInfoCount(IXMLDOMImplementation *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI implementation_GetTypeInfo(IXMLDOMImplementation *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return index ? DISP_E_BADINDEX : mx_type_info(MX_TYPE_IMPLEMENTATION, out);
}

static HRESULT WINAPI implementation_GetIDsOfNames(IXMLDOMImplementation *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_IMPLEMENTATION, riid, names, count, lcid, ids);
}

static HRESULT WINAPI implementation_Invoke(IXMLDOMImplementation *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_IMPLEMENTATION, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI implementation_hasFeature(IXMLDOMImplementation *iface, BSTR feature, BSTR version, VARIANT_BOOL *out)
{
	BOOL known;
	if (!feature || !out)
		return E_INVALIDARG;
	known = !lstrcmpiW(feature, L"DOM") || !lstrcmpiW(feature, L"XML") || !lstrcmpiW(feature, L"MS-DOM");
	*out = known && (!version || !lstrcmpW(version, L"1.0")) ? VARIANT_TRUE : VARIANT_FALSE;
	return S_OK;
}

static const IXMLDOMImplementationVtbl implementation_table = {
	implementation_QueryInterface, implementation_AddRef, implementation_Release, implementation_GetTypeInfoCount,
	implementation_GetTypeInfo, implementation_GetIDsOfNames, implementation_Invoke, implementation_hasFeature,
};

HRESULT mx_implementation_create(IXMLDOMImplementation **out)
{
	mx_implementation *self = mx_zalloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &implementation_table;
	mx_dispex_init(&self->dispex, self, FALSE);
	self->refs = 1;
	*out = (IXMLDOMImplementation *)self;
	return S_OK;
}
