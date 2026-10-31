/* SPDX-License-Identifier: MIT */
#include "node.h"

static const xmlChar dt_namespace[] = "urn:schemas-microsoft-com:datatypes";
static const xmlChar xmlns_namespace[] = "http://www.w3.org/2000/xmlns/";

/* --- Trees and who keeps them alive. --- */

mx_tree *mx_tree_of(xmlDocPtr tree)
{
	return tree->_private;
}

void mx_tree_attach(xmlDocPtr tree, mx_version version)
{
	mx_tree *data = mx_zalloc(sizeof(*data));
	data->version = version;
	data->props = mx_zalloc(sizeof(*data->props));
	data->props->refs = 1;
	data->props->xpath = version >= MX_4;
	data->props->prohibit_dtd = version >= MX_6;
	data->props->validate_parse = TRUE;
	data->props->resolve_externals = version < MX_6;
	data->props->max_element_depth = version >= MX_6 ? 256 : 5000;
	tree->_private = data;
}

xmlDocPtr mx_tree_new_document(mx_version version)
{
	xmlDocPtr tree = xmlNewDoc(NULL);
	if (tree)
		mx_tree_attach(tree, version);
	return tree;
}

static void release_properties(mx_properties *props)
{
	mx_select_ns *space;
	if (InterlockedDecrement(&props->refs))
		return;
	while ((space = props->namespaces))
	{
		props->namespaces = space->next;
		xmlFree(space->prefix);
		xmlFree(space->uri);
		mx_free(space);
	}
	if (props->schemas)
		IUnknown_Release(props->schemas);
	xmlFree(props->selection_namespaces);
	mx_free(props);
}

void mx_tree_share_properties(xmlDocPtr tree, xmlDocPtr from)
{
	mx_tree *data = mx_tree_of(tree);
	release_properties(data->props);
	data->props = mx_tree_of(from)->props;
	InterlockedIncrement(&data->props->refs);
}

void mx_tree_detach(xmlDocPtr tree)
{
	mx_tree *data = mx_tree_of(tree);
	release_properties(data->props);
	mx_free(data);
	tree->_private = NULL;
}

/* What the tree does not show but must still give back lives under a holder that nothing links to. */
void mx_tree_hold(xmlDocPtr tree, xmlNodePtr node)
{
	xmlNodePtr holder = tree->psvi;
	if (!holder)
		holder = tree->psvi = xmlNewDocNode(tree, NULL, (const xmlChar *)"held", NULL);
	node->parent = holder;
	node->prev = NULL;
	node->next = holder->children;
	if (holder->children)
		holder->children->prev = node;
	else
		holder->last = node;
	holder->children = node;
}

void mx_tree_free_doc(xmlDocPtr tree)
{
	if (tree->psvi)
		xmlFreeNode(tree->psvi);
	tree->psvi = NULL;
	xmlFreeDoc(tree);
}

xmlNodePtr mx_blank_before(xmlNodePtr node)
{
	return node->extra & MX_BLANK_BEFORE ? node->psvi : NULL;
}

xmlNodePtr mx_blank_at_end(xmlNodePtr node)
{
	xmlNodePtr at = node->psvi;
	if (!(node->extra & MX_BLANK_AT_END) || !at)
		return NULL;
	return node->extra & MX_BLANK_BEFORE ? at->psvi : at;
}

void mx_tree_ref(xmlDocPtr tree)
{
	InterlockedIncrement(&mx_tree_of(tree)->refs);
}

void mx_tree_unref(xmlDocPtr tree)
{
	mx_tree *data = mx_tree_of(tree);
	mx_orphan *orphan;
	if (InterlockedDecrement(&data->refs))
		return;
	while ((orphan = data->orphans))
	{
		data->orphans = orphan->next;
		xmlFreeNode(orphan->node);
		mx_free(orphan);
	}
	release_properties(data->props);
	SysFreeString(data->url);
	while (data->pin_count)
		mx_tree_unref(data->pins[--data->pin_count]);
	mx_free(data->pins);
	mx_free(data);
	tree->_private = NULL;
	mx_tree_free_doc(tree);
}

/* A node outside the tree still belongs to it and goes away with it. */
void mx_tree_orphan(xmlNodePtr node)
{
	mx_tree *data = mx_tree_of(node->doc);
	mx_orphan *orphan = mx_alloc(sizeof(*orphan));
	if (!orphan)
		return;
	orphan->node = node;
	orphan->next = data->orphans;
	data->orphans = orphan;
}

void mx_tree_adopted(xmlNodePtr node)
{
	mx_tree *data = node->doc ? mx_tree_of(node->doc) : NULL;
	mx_orphan **at;
	if (!data)
		return;
	for (at = &data->orphans; *at; at = &(*at)->next)
		if ((*at)->node == node)
		{
			mx_orphan *found = *at;
			*at = found->next;
			mx_free(found);
			return;
		}
}

/* --- Linking without libxml's habit of merging adjacent text. --- */

static void splice(xmlNodePtr parent, xmlNodePtr child, xmlNodePtr before)
{
	xmlNodePtr previous = before ? before->prev : parent->last;
	child->parent = parent;
	child->prev = previous;
	child->next = before;
	if (previous)
		previous->next = child;
	else
		parent->children = child;
	if (before)
		before->prev = child;
	else
		parent->last = child;
}

static BOOL has_ancestor(xmlNodePtr node, xmlNodePtr candidate)
{
	for (; node; node = node->parent)
		if (node == candidate)
			return TRUE;
	return FALSE;
}

static BOOL document_accepts(xmlNodePtr parent, xmlNodePtr child)
{
	xmlNodePtr at;
	if (parent->type != XML_DOCUMENT_NODE)
		return TRUE;
	switch (child->type)
	{
	case XML_ELEMENT_NODE:
		for (at = parent->children; at; at = at->next)
			if (at->type == XML_ELEMENT_NODE && at != child)
				return FALSE;
		return TRUE;
	case XML_COMMENT_NODE:
	case XML_PI_NODE:
	case XML_DOCUMENT_FRAG_NODE:
		return TRUE;
	default:
		return FALSE;
	}
}

HRESULT mx_link_before(xmlNodePtr parent, xmlNodePtr child, xmlNodePtr before)
{
	xmlDocPtr old_tree = child->doc;
	BOOL was_free = !child->parent;
	if (before && before->parent != parent)
		return E_INVALIDARG;
	if (has_ancestor(parent, child) || child->type == XML_DOCUMENT_NODE || child->type == XML_ATTRIBUTE_NODE ||
		child->type == XML_DTD_NODE || child->type == XML_ENTITY_DECL || !document_accepts(parent, child))
		return E_FAIL;
	switch (parent->type)
	{
	case XML_ELEMENT_NODE:
	case XML_DOCUMENT_NODE:
	case XML_DOCUMENT_FRAG_NODE:
	case XML_ENTITY_REF_NODE:
		break;
	case XML_ATTRIBUTE_NODE:
		if (child->type != XML_TEXT_NODE && child->type != XML_ENTITY_REF_NODE)
			return E_FAIL;
		break;
	default:
		return E_FAIL;
	}
	if (child == before)
		return S_OK;
	if (child->type == XML_DOCUMENT_FRAG_NODE)
	{
		xmlNodePtr at = child->children;
		while (at)
		{
			xmlNodePtr next = at->next;
			HRESULT status = mx_link_before(parent, at, before);
			if (FAILED(status))
				return status;
			at = next;
		}
		return S_OK;
	}
	if (was_free && old_tree)
		mx_tree_adopted(child);
	xmlUnlinkNode(child);
	if (child->doc != parent->doc)
	{
		/* Objects of the moved node still hold the tree it left, which keeps the one it went to alive for them. */
		mx_tree *left = child->doc ? mx_tree_of(child->doc) : NULL;
		if (left && parent->doc)
		{
			xmlDocPtr *room = mx_realloc(left->pins, (left->pin_count + 1) * sizeof(*room));
			if (room)
			{
				left->pins = room;
				left->pins[left->pin_count++] = parent->doc;
				mx_tree_ref(parent->doc);
			}
		}
		xmlSetTreeDoc(child, parent->doc);
	}
	splice(parent, child, before);
	if (child->type == XML_ELEMENT_NODE && parent->doc)
		xmlReconciliateNs(parent->doc, child);
	return S_OK;
}

/* --- Common object plumbing. --- */


/* Only tells this library's own objects from anyone else's. */
const GUID mx_private_node_iid = {0x7c4b3f1e, 0x52a0, 0x4f6e, {0x9d, 0x11, 0x3a, 0x6c, 0x88, 0x20, 0xe5, 0x17}};

void mx_node_init(mx_node *self, const void *vtbl, xmlNodePtr node, mx_type type, const IID *iid)
{
	self->vtbl = vtbl;
	mx_dispex_init(&self->dispex, self, FALSE);
	self->refs = 1;
	self->node = node;
	self->held = node->doc;
	self->type = type;
	self->iid = iid;
	if (self->held)
		mx_tree_ref(self->held);
}

HRESULT WINAPI nd_QueryInterface(IXMLDOMNode *iface, REFIID riid, void **out)
{
	mx_node *self = mx_node_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualGUID(riid, &mx_private_node_iid))
	{
		*out = self;
		return S_OK;
	}
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IXMLDOMNode) || IsEqualGUID(riid, self->iid))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_ISupportErrorInfo))
	{
		static const GUID *const plain[] = {&IID_IXMLDOMNode, NULL};
		static const GUID *const document[] = {&IID_IXMLDOMNode, &IID_IXMLDOMDocument, &IID_IXMLDOMDocument2, NULL};
		return mx_error_support_create(self->iid, IsEqualGUID(self->iid, &IID_IXMLDOMDocument3) ? document : plain, (ISupportErrorInfo **)out);
	}
	else if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &self->dispex;
	else if (self->query && self->query(self, riid, out) == S_OK)
		return S_OK;
	else
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

ULONG WINAPI nd_AddRef(IXMLDOMNode *iface)
{
	return InterlockedIncrement(&mx_node_from(iface)->refs);
}

ULONG WINAPI nd_Release(IXMLDOMNode *iface)
{
	mx_node *self = mx_node_from(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	if (self->destroy)
		self->destroy(self);
	if (self->held)
		mx_tree_unref(self->held);
	mx_free(self);
	return 0;
}

HRESULT WINAPI nd_GetTypeInfoCount(IXMLDOMNode *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

HRESULT WINAPI nd_GetTypeInfo(IXMLDOMNode *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	if (index)
		return DISP_E_BADINDEX;
	return mx_type_info(mx_node_from(iface)->type, out);
}

HRESULT WINAPI nd_GetIDsOfNames(IXMLDOMNode *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(mx_node_from(iface)->type, riid, names, count, lcid, ids);
}

HRESULT WINAPI nd_Invoke(IXMLDOMNode *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(mx_node_from(iface)->type, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

/* --- Names and values. --- */

const xmlChar *mx_local_name(const xmlChar *qualified, const xmlChar **prefix_end)
{
	const xmlChar *colon = qualified ? (const xmlChar *)xmlStrchr(qualified, ':') : NULL;
	if (prefix_end)
		*prefix_end = colon;
	return colon ? colon + 1 : qualified;
}

static xmlChar *qualified_name(xmlNodePtr node)
{
	switch (node->type)
	{
	case XML_ELEMENT_NODE:
	case XML_ATTRIBUTE_NODE:
		if (node->ns && node->ns->prefix)
		{
			xmlChar *name = xmlStrdup(node->ns->prefix);
			name = xmlStrcat(name, (const xmlChar *)":");
			return xmlStrcat(name, node->name);
		}
		return xmlStrdup(node->name);
	default:
		return xmlStrdup(node->name);
	}
}

HRESULT WINAPI nd_get_nodeName(IXMLDOMNode *iface, BSTR *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	const WCHAR *fixed = NULL;
	xmlChar *name;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	switch (node->type)
	{
	case XML_TEXT_NODE: fixed = L"#text"; break;
	case XML_CDATA_SECTION_NODE: fixed = L"#cdata-section"; break;
	case XML_COMMENT_NODE: fixed = L"#comment"; break;
	case XML_DOCUMENT_NODE: fixed = L"#document"; break;
	case XML_DOCUMENT_FRAG_NODE: fixed = L"#document-fragment"; break;
	default: break;
	}
	if (fixed)
	{
		*out = SysAllocString(fixed);
		return *out ? S_OK : E_OUTOFMEMORY;
	}
	name = qualified_name(node);
	status = mx_return_string(name, out);
	xmlFree(name);
	return status;
}

HRESULT WINAPI nd_get_nodeValue(IXMLDOMNode *iface, VARIANT *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *content;
	if (!out)
		return E_INVALIDARG;
	switch (node->type)
	{
	case XML_TEXT_NODE:
	case XML_CDATA_SECTION_NODE:
	case XML_COMMENT_NODE:
	case XML_PI_NODE:
	case XML_ATTRIBUTE_NODE:
		content = xmlNodeGetContent(node);
		V_VT(out) = VT_BSTR;
		V_BSTR(out) = content ? mx_bstr(content) : SysAllocStringLen(NULL, 0);
		xmlFree(content);
		return V_BSTR(out) ? S_OK : E_OUTOFMEMORY;
	default:
		return mx_return_null_variant(out);
	}
}

/* A line end inside the tree is one line feed, as a parser would have left it. */
void mx_normalize_lines(xmlChar *text)
{
	xmlChar *to = text;
	for (; *text; text++)
	{
		if (*text == '\r')
		{
			*to++ = '\n';
			if (text[1] == '\n')
				text++;
		}
		else
			*to++ = *text;
	}
	*to = 0;
}

static HRESULT set_content(xmlNodePtr node, const xmlChar *text)
{
	if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE || node->type == XML_COMMENT_NODE || node->type == XML_PI_NODE)
	{
		const xmlChar *closing = node->type == XML_COMMENT_NODE ? (const xmlChar *)"-->" : node->type == XML_PI_NODE ? (const xmlChar *)"?>" : NULL;
		if (closing && text && xmlStrstr(text, closing))
			return E_INVALIDARG;
		/* Stored as it is: nothing in a character node is markup. */
		if (node->content && node->content != (xmlChar *)&node->properties)
			xmlFree(node->content);
		node->content = xmlStrdup(text ? text : (const xmlChar *)"");
		mx_normalize_lines(node->content);
		node->properties = NULL;
		node->nsDef = NULL;
		return S_OK;
	}
	if (node->type == XML_ATTRIBUTE_NODE)
	{
		xmlChar *escaped = xmlEncodeSpecialChars(node->doc, text ? text : (const xmlChar *)"");
		xmlNodeSetContent(node, escaped);
		xmlFree(escaped);
		/* An attribute set to nothing still has its empty text. */
		if (!node->children)
		{
			xmlNodePtr empty = xmlNewDocText(node->doc, (const xmlChar *)"");
			if (empty)
			{
				empty->parent = node;
				node->children = node->last = empty;
			}
		}
		return S_OK;
	}
	return E_FAIL;
}

HRESULT WINAPI nd_put_nodeValue(IXMLDOMNode *iface, VARIANT value)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	VARIANT text;
	xmlChar *utf8;
	HRESULT status;
	switch (node->type)
	{
	case XML_TEXT_NODE:
	case XML_CDATA_SECTION_NODE:
	case XML_COMMENT_NODE:
	case XML_PI_NODE:
	case XML_ATTRIBUTE_NODE:
		break;
	default:
		return E_FAIL;
	}
	if (node->type == XML_PI_NODE && !xmlStrcmp(node->name, (const xmlChar *)"xml"))
		return E_FAIL;
	status = mx_variant_text(&value, &text);
	if (FAILED(status))
		return status;
	utf8 = mx_to_utf8(V_BSTR(&text));
	status = set_content(node, utf8);
	xmlFree(utf8);
	VariantClear(&text);
	return status;
}

HRESULT WINAPI nd_get_nodeType(IXMLDOMNode *iface, DOMNodeType *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	if (!out)
		return E_INVALIDARG;
	switch (node->type)
	{
	case XML_DTD_NODE: *out = NODE_DOCUMENT_TYPE; break;
	case XML_ENTITY_DECL: *out = NODE_ENTITY; break;
	case XML_NOTATION_NODE: *out = NODE_NOTATION; break;
	default: *out = (DOMNodeType)node->type; break;
	}
	return S_OK;
}

/* --- Walking the tree. --- */

HRESULT mx_node_return(xmlNodePtr node, IXMLDOMNode **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return node ? mx_node_create(node, out) : S_FALSE;
}

/* Declarations of the document type are reached through it, never as children. */
static xmlNodePtr visible(xmlNodePtr node, BOOL forward)
{
	while (node && (node->type == XML_ENTITY_DECL || node->type == XML_ELEMENT_DECL || node->type == XML_ATTRIBUTE_DECL || node->type == XML_NAMESPACE_DECL))
		node = forward ? node->next : node->prev;
	return node;
}

HRESULT WINAPI nd_get_parentNode(IXMLDOMNode *iface, IXMLDOMNode **out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	if (!out)
		return E_INVALIDARG;
	switch (node->type)
	{
	case XML_ATTRIBUTE_NODE:
	case XML_DOCUMENT_NODE:
	case XML_DOCUMENT_FRAG_NODE:
	case XML_ENTITY_DECL:
	case XML_NOTATION_NODE:
		*out = NULL;
		return S_FALSE;
	default:
		return mx_node_return(node->parent, out);
	}
}

static BOOL holds_children(xmlNodePtr node)
{
	switch (node->type)
	{
	case XML_ELEMENT_NODE:
	case XML_ATTRIBUTE_NODE:
	case XML_DOCUMENT_NODE:
	case XML_DOCUMENT_FRAG_NODE:
	case XML_ENTITY_REF_NODE:
	case XML_ENTITY_DECL:
		return TRUE;
	default:
		return FALSE;
	}
}

HRESULT WINAPI nd_get_childNodes(IXMLDOMNode *iface, IXMLDOMNodeList **out)
{
	if (!out)
		return E_INVALIDARG;
	return mx_children_list(mx_node_from(iface)->node, out);
}

HRESULT WINAPI nd_get_firstChild(IXMLDOMNode *iface, IXMLDOMNode **out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	return mx_node_return(holds_children(node) ? visible(node->children, TRUE) : NULL, out);
}

HRESULT WINAPI nd_get_lastChild(IXMLDOMNode *iface, IXMLDOMNode **out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	return mx_node_return(holds_children(node) ? visible(node->last, FALSE) : NULL, out);
}

HRESULT WINAPI nd_get_previousSibling(IXMLDOMNode *iface, IXMLDOMNode **out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	if (node->type == XML_ATTRIBUTE_NODE || node->type == XML_DOCUMENT_NODE || node->type == XML_DOCUMENT_FRAG_NODE)
		return mx_node_return(NULL, out);
	return mx_node_return(visible(node->prev, FALSE), out);
}

HRESULT WINAPI nd_get_nextSibling(IXMLDOMNode *iface, IXMLDOMNode **out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	if (node->type == XML_ATTRIBUTE_NODE || node->type == XML_DOCUMENT_NODE || node->type == XML_DOCUMENT_FRAG_NODE)
		return mx_node_return(NULL, out);
	return mx_node_return(visible(node->next, TRUE), out);
}

HRESULT WINAPI nd_get_attributes(IXMLDOMNode *iface, IXMLDOMNamedNodeMap **out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (node->type == XML_ELEMENT_NODE || (node->type == XML_PI_NODE && !xmlStrcmp(node->name, (const xmlChar *)"xml")))
		return mx_attribute_map(node, out);
	return S_FALSE;
}

/* --- Changing the tree. --- */

xmlNodePtr mx_node_get(IUnknown *object)
{
	extern const GUID mx_private_node_iid;
	mx_node *self;
	if (!object || FAILED(IUnknown_QueryInterface(object, &mx_private_node_iid, (void **)&self)))
		return NULL;
	return self->node;
}

static HRESULT link_and_return(mx_node *self, IXMLDOMNode *child, xmlNodePtr before, IXMLDOMNode **out)
{
	xmlNodePtr given = mx_node_get((IUnknown *)child);
	HRESULT status;
	if (out)
		*out = NULL;
	if (!child)
		return E_INVALIDARG;
	if (!given)
		return E_FAIL;
	status = mx_link_before(self->node, given, before);
	if (FAILED(status))
		return status;
	if (out)
	{
		*out = child;
		IXMLDOMNode_AddRef(child);
	}
	return S_OK;
}

HRESULT WINAPI nd_insertBefore(IXMLDOMNode *iface, IXMLDOMNode *child, VARIANT before, IXMLDOMNode **out)
{
	mx_node *self = mx_node_from(iface);
	xmlNodePtr place = NULL;
	if (!child)
		return E_INVALIDARG;
	if (V_VT(&before) == VT_UNKNOWN || V_VT(&before) == VT_DISPATCH)
	{
		if (V_UNKNOWN(&before))
		{
			place = mx_node_get(V_UNKNOWN(&before));
			if (!place)
				return E_FAIL;
		}
	}
	else if (V_VT(&before) != VT_EMPTY && V_VT(&before) != VT_ERROR && V_VT(&before) != VT_NULL)
		return E_FAIL;
	if (!holds_children(self->node))
	{
		if (out)
			*out = NULL;
		return E_FAIL;
	}
	return link_and_return(self, child, place, out);
}

HRESULT WINAPI nd_appendChild(IXMLDOMNode *iface, IXMLDOMNode *child, IXMLDOMNode **out)
{
	mx_node *self = mx_node_from(iface);
	if (!holds_children(self->node))
	{
		if (out)
			*out = NULL;
		return child ? E_FAIL : E_INVALIDARG;
	}
	return link_and_return(self, child, NULL, out);
}

HRESULT WINAPI nd_removeChild(IXMLDOMNode *iface, IXMLDOMNode *child, IXMLDOMNode **out)
{
	mx_node *self = mx_node_from(iface);
	xmlNodePtr given;
	if (!child)
		return E_INVALIDARG;
	if (out)
		*out = NULL;
	given = mx_node_get((IUnknown *)child);
	if (!given || given->parent != self->node)
		return E_INVALIDARG;
	if (given->type == XML_DTD_NODE)
		return E_FAIL;
	xmlUnlinkNode(given);
	mx_tree_orphan(given);
	if (!self->node->children)
		self->node->extra |= MX_EXPLICIT_END;
	if (out)
	{
		*out = child;
		IXMLDOMNode_AddRef(child);
	}
	return S_OK;
}

HRESULT WINAPI nd_replaceChild(IXMLDOMNode *iface, IXMLDOMNode *child, IXMLDOMNode *old, IXMLDOMNode **out)
{
	mx_node *self = mx_node_from(iface);
	xmlNodePtr given, leaving, after;
	HRESULT status;
	if (!child || !old)
		return E_INVALIDARG;
	if (out)
		*out = NULL;
	given = mx_node_get((IUnknown *)child);
	leaving = mx_node_get((IUnknown *)old);
	if (!given || !leaving)
		return E_FAIL;
	if (leaving->parent != self->node)
		return E_INVALIDARG;
	if (given == leaving)
	{
		if (out)
		{
			*out = old;
			IXMLDOMNode_AddRef(old);
		}
		return S_OK;
	}
	after = leaving->next == given ? given->next : leaving->next;
	xmlUnlinkNode(leaving);
	status = mx_link_before(self->node, given, after);
	if (FAILED(status))
	{
		splice(self->node, leaving, after);
		return status;
	}
	mx_tree_orphan(leaving);
	if (out)
	{
		*out = old;
		IXMLDOMNode_AddRef(old);
	}
	return S_OK;
}

HRESULT WINAPI nd_hasChildNodes(IXMLDOMNode *iface, VARIANT_BOOL *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	BOOL any = holds_children(node) && visible(node->children, TRUE);
	if (!out)
		return E_INVALIDARG;
	*out = any ? VARIANT_TRUE : VARIANT_FALSE;
	return any ? S_OK : S_FALSE;
}

HRESULT WINAPI nd_get_ownerDocument(IXMLDOMNode *iface, IXMLDOMDocument **out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (node->type == XML_DOCUMENT_NODE)
		return S_FALSE;
	return mx_document_wrap(node->doc, &IID_IXMLDOMDocument, (void **)out);
}

HRESULT WINAPI nd_cloneNode(IXMLDOMNode *iface, VARIANT_BOOL deep, IXMLDOMNode **out)
{
	xmlNodePtr node = mx_node_from(iface)->node, copy;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (node->type == XML_DOCUMENT_NODE)
	{
		xmlDocPtr tree = xmlCopyDoc((xmlDocPtr)node, deep ? 1 : 0);
		if (!tree)
			return E_OUTOFMEMORY;
		mx_tree_attach(tree, mx_tree_of((xmlDocPtr)node)->version);
		mx_tree_share_properties(tree, (xmlDocPtr)node);
		return mx_document_wrap(tree, &IID_IXMLDOMNode, (void **)out);
	}
	copy = xmlDocCopyNode(node, node->doc, deep ? 1 : 2);
	if (!copy)
		return E_FAIL;
	if (!copy->children)
		copy->extra |= node->extra & MX_EXPLICIT_END;
	copy->parent = NULL;
	mx_tree_orphan(copy);
	status = mx_node_create(copy, out);
	return status;
}

/* --- Text. --- */

HRESULT WINAPI nd_get_nodeTypeString(IXMLDOMNode *iface, BSTR *out)
{
	static const WCHAR *const names[] = {
		[XML_ELEMENT_NODE] = L"element", [XML_ATTRIBUTE_NODE] = L"attribute", [XML_TEXT_NODE] = L"text", [XML_CDATA_SECTION_NODE] = L"cdatasection",
		[XML_ENTITY_REF_NODE] = L"entityreference", [XML_ENTITY_NODE] = L"entity", [XML_PI_NODE] = L"processinginstruction", [XML_COMMENT_NODE] = L"comment",
		[XML_DOCUMENT_NODE] = L"document", [XML_DOCUMENT_TYPE_NODE] = L"documenttype", [XML_DOCUMENT_FRAG_NODE] = L"documentfragment", [XML_NOTATION_NODE] = L"notation",
		[XML_DTD_NODE] = L"documenttype", [XML_ENTITY_DECL] = L"entity",
	};
	xmlNodePtr node = mx_node_from(iface)->node;
	if (!out)
		return E_INVALIDARG;
	*out = SysAllocString(node->type < ARRAYSIZE(names) && names[node->type] ? names[node->type] : L"");
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* A subtree's text is its text nodes run together. Blank ends of the text nodes go unless the
 * document, or an xml:space on the way, asks to keep them; marked sections are always kept whole. */
/* Outside preserved space, blanks before the first text or CDATA section go and a run of nodes holding only blanks reads as
 * one space; once text has begun, the blanks inside a text node stay as they are. */
static void gather_text(xmlNodePtr node, xmlBufferPtr out, BOOL preserve, BOOL *pending_space, BOOL *started)
{
	xmlNodePtr at;
	for (at = node->children; at; at = at->next)
	{
		BOOL keep = preserve;
		if ((at->extra & MX_BLANK_BEFORE) && *started)
			*pending_space = TRUE;
		switch (at->type)
		{
		case XML_ELEMENT_NODE:
			{
				int space = xmlNodeGetSpacePreserve(at);
				if (space >= 0)
					keep = space == 1;
				gather_text(at, out, keep, pending_space, started);
			}
			break;
		case XML_ENTITY_REF_NODE:
		case XML_ENTITY_DECL:
			gather_text(at, out, keep, pending_space, started);
			break;
		case XML_CDATA_SECTION_NODE:
			if (*pending_space)
				xmlBufferAdd(out, (const xmlChar *)" ", 1);
			*pending_space = FALSE;
			*started = TRUE;
			if (at->content)
				xmlBufferCat(out, at->content);
			break;
		case XML_TEXT_NODE:
			{
				const xmlChar *from = at->content;
				if (!from)
					break;
				if (keep)
				{
					if (*pending_space)
						xmlBufferAdd(out, (const xmlChar *)" ", 1);
					*pending_space = FALSE;
					*started = TRUE;
					xmlBufferCat(out, from);
					break;
				}
				while (*from == ' ' || *from == '\t' || *from == '\r' || *from == '\n')
					from++;
				if (!*from)
				{
					if (at->content[0] && *started)
						*pending_space = TRUE;
					break;
				}
				if (*pending_space)
					xmlBufferAdd(out, (const xmlChar *)" ", 1);
				*pending_space = FALSE;
				xmlBufferCat(out, *started ? at->content : from);
				*started = TRUE;
			}
			break;
		default:
			break;
		}
	}
	if (node->type == XML_ELEMENT_NODE && (node->extra & MX_BLANK_AT_END) && *started)
		*pending_space = TRUE;
}

xmlChar *mx_node_text(xmlNodePtr node)
{
	switch (node->type)
	{
	case XML_ELEMENT_NODE:
	case XML_DOCUMENT_NODE:
	case XML_DOCUMENT_FRAG_NODE:
	case XML_ENTITY_REF_NODE:
	case XML_ENTITY_DECL:
		{
			xmlBufferPtr buffer = xmlBufferCreate();
			xmlChar *result;
			BOOL pending = FALSE, started = FALSE, preserve = node->doc && mx_tree_of(node->doc) ? mx_tree_of(node->doc)->props->preserve_space : FALSE;
			if (node->type == XML_ELEMENT_NODE)
			{
				int space = xmlNodeGetSpacePreserve(node);
				if (space >= 0)
					preserve = space == 1;
			}
			gather_text(node, buffer, preserve, &pending, &started);
			result = xmlStrdup(xmlBufferContent(buffer) ? xmlBufferContent(buffer) : (const xmlChar *)"");
			if (result && !preserve)
			{
				int size = xmlStrlen(result);
				while (size && (result[size - 1] == ' ' || result[size - 1] == '\t' || result[size - 1] == '\r' || result[size - 1] == '\n'))
					result[--size] = 0;
			}
			xmlBufferFree(buffer);
			return result;
		}
	case XML_TEXT_NODE:
		{
			/* Outside preserved space, the text of a text node comes without its outer blanks. */
			xmlChar *text = xmlNodeGetContent(node);
			BOOL preserve = node->doc && mx_tree_of(node->doc) ? mx_tree_of(node->doc)->props->preserve_space : FALSE;
			int space = node->parent ? xmlNodeGetSpacePreserve(node->parent) : -1;
			xmlChar *start = text, *end;
			if (space >= 0)
				preserve = space == 1;
			if (preserve || !text)
				return text;
			while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
				start++;
			end = start + xmlStrlen(start);
			while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
				end--;
			*end = 0;
			memmove(text, start, end - start + 1);
			return text;
		}
	default:
		return xmlNodeGetContent(node);
	}
}

HRESULT WINAPI nd_get_text(IXMLDOMNode *iface, BSTR *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *text;
	if (!out)
		return E_INVALIDARG;
	text = mx_node_text(node);
	*out = text ? mx_bstr(text) : SysAllocStringLen(NULL, 0);
	xmlFree(text);
	return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI nd_put_text(IXMLDOMNode *iface, BSTR text)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *utf8;
	HRESULT status = S_OK;
	if (node->type == XML_ATTRIBUTE_NODE && ((xmlAttrPtr)node)->psvi == mx_declaration_mark)
		return E_FAIL;
	switch (node->type)
	{
	case XML_DOCUMENT_NODE:
	case XML_DTD_NODE:
	case XML_ENTITY_REF_NODE:
	case XML_ENTITY_DECL:
	case XML_NOTATION_NODE:
		return E_FAIL;
	default:
		break;
	}
	utf8 = mx_to_utf8(text ? text : L"");
	if (node->type == XML_ELEMENT_NODE || node->type == XML_DOCUMENT_FRAG_NODE)
	{
		xmlNodePtr at = node->children;
		while (at)
		{
			xmlNodePtr next = at->next;
			xmlUnlinkNode(at);
			mx_tree_orphan(at);
			at = next;
		}
		if (node->extra & MX_BLANK_AT_END)
		{
			node->extra &= ~MX_BLANK_AT_END;
			if (!(node->extra & MX_BLANK_BEFORE))
				node->psvi = NULL;
		}
		if (*utf8)
		{
			xmlNodePtr added = xmlNewDocText(node->doc, utf8);
			if (added)
				splice(node, added, NULL);
		}
	}
	else
		status = set_content(node, utf8);
	xmlFree(utf8);
	return status;
}

HRESULT WINAPI nd_get_specified(IXMLDOMNode *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = VARIANT_TRUE;
	return S_OK;
}

HRESULT WINAPI nd_get_definition(IXMLDOMNode *iface, IXMLDOMNode **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return S_FALSE;
}

/* --- Typed values, named by a dt:dt attribute. --- */

static xmlChar *data_type_of(xmlNodePtr node)
{
	if (node->type != XML_ELEMENT_NODE)
		return NULL;
	return xmlGetNsProp(node, (const xmlChar *)"dt", dt_namespace);
}

static const struct
{
	const char *name;
	VARTYPE type;
} data_types[] = {
	{"bin.base64", VT_ARRAY | VT_UI1}, {"bin.hex", VT_ARRAY | VT_UI1}, {"boolean", VT_BOOL}, {"char", VT_UI2},
	{"date", VT_DATE}, {"date.tz", VT_DATE}, {"dateTime", VT_DATE}, {"dateTime.tz", VT_DATE},
	{"entity", VT_BSTR}, {"entities", VT_BSTR}, {"enumeration", VT_BSTR}, {"fixed.14.4", VT_CY}, {"float", VT_R8},
	{"i1", VT_I1}, {"i2", VT_I2}, {"i4", VT_I4}, {"i8", VT_I8}, {"id", VT_BSTR}, {"idref", VT_BSTR}, {"idrefs", VT_BSTR},
	{"int", VT_I4}, {"nmtoken", VT_BSTR}, {"nmtokens", VT_BSTR}, {"notation", VT_BSTR}, {"number", VT_BSTR},
	{"r4", VT_R4}, {"r8", VT_R8}, {"string", VT_BSTR}, {"time", VT_DATE}, {"time.tz", VT_DATE},
	{"ui1", VT_UI1}, {"ui2", VT_UI2}, {"ui4", VT_UI4}, {"ui8", VT_UI8}, {"uri", VT_BSTR}, {"uuid", VT_BSTR},
};

static BOOL find_data_type(const xmlChar *name, VARTYPE *type)
{
	unsigned index;
	for (index = 0; name && index < ARRAYSIZE(data_types); index++)
		if (!xmlStrcasecmp(name, (const xmlChar *)data_types[index].name))
		{
			if (type)
				*type = data_types[index].type;
			return TRUE;
		}
	return FALSE;
}

static int base64_value(xmlChar c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static HRESULT decode_binary(const xmlChar *text, BOOL base64, VARIANT *out)
{
	SIZE_T length = xmlStrlen(text), used = 0, index;
	BYTE *data = mx_alloc(length + 1);
	SAFEARRAY *array;
	void *target;
	if (!data)
		return E_OUTOFMEMORY;
	if (base64)
	{
		unsigned bits = 0, count = 0;
		for (index = 0; index < length; index++)
		{
			int value = base64_value(text[index]);
			if (value < 0)
				continue;
			bits = (bits << 6) | value;
			count += 6;
			if (count >= 8)
			{
				count -= 8;
				data[used++] = (BYTE)(bits >> count);
			}
		}
	}
	else
	{
		int high = -1;
		for (index = 0; index < length; index++)
		{
			xmlChar c = text[index];
			int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (value < 0)
				continue;
			if (high < 0)
				high = value;
			else
			{
				data[used++] = (BYTE)(high << 4 | value);
				high = -1;
			}
		}
	}
	array = SafeArrayCreateVector(VT_UI1, 0, (ULONG)used);
	if (array && SUCCEEDED(SafeArrayAccessData(array, &target)))
	{
		memcpy(target, data, used);
		SafeArrayUnaccessData(array);
	}
	mx_free(data);
	if (!array)
		return E_OUTOFMEMORY;
	V_VT(out) = VT_ARRAY | VT_UI1;
	V_ARRAY(out) = array;
	return S_OK;
}

static BOOL read_number(const char **at, int *value)
{
	const char *from = *at;
	int number = 0;
	if (*from < '0' || *from > '9')
		return FALSE;
	while (*from >= '0' && *from <= '9')
		number = number * 10 + (*from++ - '0');
	*value = number;
	*at = from;
	return TRUE;
}

/* ISO 8601 as data types write it; the variant date has no zone, so one is simply dropped. */
static HRESULT parse_iso_date(const xmlChar *text, VARIANT *out)
{
	SYSTEMTIME when = {0};
	const char *at = (const char *)text;
	DOUBLE date, offset = 0;
	int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0;
	if (read_number(&at, &year) && *at == '-')
	{
		at++;
		if (!read_number(&at, &month) || *at++ != '-' || !read_number(&at, &day))
			return E_FAIL;
		if (*at == 'T')
			at++;
		else
			at = "";
	}
	else
	{
		year = 0;
		at = (const char *)text;
	}
	if (*at)
	{
		if (!read_number(&at, &hour) || *at++ != ':' || !read_number(&at, &minute))
			return E_FAIL;
		if (*at == ':')
		{
			at++;
			read_number(&at, &second);
		}
		if (*at == '.')
		{
			at++;
			while (*at >= '0' && *at <= '9')
				at++;
		}
		/* A zone offset counts into the value as it is written. */
		if (*at == '+' || *at == '-')
		{
			int sign = *at++ == '-' ? -1 : 1, zone_hour = 0, zone_minute = 0;
			read_number(&at, &zone_hour);
			if (*at == ':')
			{
				at++;
				read_number(&at, &zone_minute);
			}
			offset = sign * (zone_hour * 60 + zone_minute) / 1440.0;
		}
	}
	when.wYear = year ? year : 1899;
	when.wMonth = year ? month : 12;
	when.wDay = year ? day : 30;
	when.wHour = hour;
	when.wMinute = minute;
	when.wSecond = second;
	if (!SystemTimeToVariantTime(&when, &date))
		return E_FAIL;
	V_VT(out) = VT_DATE;
	V_DATE(out) = date + offset;
	return S_OK;
}

HRESULT WINAPI nd_get_nodeTypedValue(IXMLDOMNode *iface, VARIANT *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *name, *text;
	VARTYPE type = VT_BSTR;
	HRESULT status = S_OK;
	if (!out)
		return E_INVALIDARG;
	V_VT(out) = VT_EMPTY;
	if (node->type == XML_ENTITY_REF_NODE || node->type == XML_DOCUMENT_NODE || node->type == XML_DOCUMENT_FRAG_NODE || node->type == XML_DTD_NODE || node->type == XML_ENTITY_DECL || node->type == XML_NOTATION_NODE)
		return mx_return_null_variant(out);
	name = data_type_of(node);
	if (!name || !find_data_type(name, &type))
	{
		BSTR plain;
		xmlFree(name);
		status = nd_get_text(iface, &plain);
		if (SUCCEEDED(status))
		{
			V_VT(out) = VT_BSTR;
			V_BSTR(out) = plain;
		}
		return status;
	}
	/* Text declared as such stays exactly as written. */
	text = type == VT_BSTR ? xmlNodeGetContent(node) : mx_node_text(node);
	if (!xmlStrcasecmp(name, (const xmlChar *)"bin.base64"))
		status = decode_binary(text, TRUE, out);
	else if (!xmlStrcasecmp(name, (const xmlChar *)"bin.hex"))
		status = decode_binary(text, FALSE, out);
	else if (type == VT_DATE)
		status = parse_iso_date(text, out);
	else if (type == VT_BOOL)
	{
		V_VT(out) = VT_BOOL;
		V_BOOL(out) = !xmlStrcmp(text, (const xmlChar *)"1") || !xmlStrcasecmp(text, (const xmlChar *)"true") ? VARIANT_TRUE : VARIANT_FALSE;
	}
	else
	{
		VARIANT source;
		V_VT(&source) = VT_BSTR;
		V_BSTR(&source) = mx_bstr(text);
		if (type == VT_BSTR)
			*out = source;
		else
		{
			status = VariantChangeTypeEx(out, &source, MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT), 0, type);
			VariantClear(&source);
		}
	}
	xmlFree(text);
	xmlFree(name);
	return status;
}

static xmlChar *encode_binary(const BYTE *data, ULONG size, BOOL base64)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	xmlChar *out = xmlMalloc((size / 3 + 2) * 4 + size * 2 + 1), *to = out;
	ULONG index;
	if (!out)
		return NULL;
	if (base64)
	{
		for (index = 0; index + 2 < size; index += 3)
		{
			*to++ = alphabet[data[index] >> 2];
			*to++ = alphabet[(data[index] & 3) << 4 | data[index + 1] >> 4];
			*to++ = alphabet[(data[index + 1] & 15) << 2 | data[index + 2] >> 6];
			*to++ = alphabet[data[index + 2] & 63];
		}
		if (size - index == 1)
		{
			*to++ = alphabet[data[index] >> 2];
			*to++ = alphabet[(data[index] & 3) << 4];
			*to++ = '=';
			*to++ = '=';
		}
		else if (size - index == 2)
		{
			*to++ = alphabet[data[index] >> 2];
			*to++ = alphabet[(data[index] & 3) << 4 | data[index + 1] >> 4];
			*to++ = alphabet[(data[index + 1] & 15) << 2];
			*to++ = '=';
		}
	}
	else
		for (index = 0; index < size; index++)
		{
			static const char digits[] = "0123456789abcdef";
			*to++ = digits[data[index] >> 4];
			*to++ = digits[data[index] & 15];
		}
	*to = 0;
	return out;
}

HRESULT WINAPI nd_put_nodeTypedValue(IXMLDOMNode *iface, VARIANT value)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *name = data_type_of(node);
	HRESULT status;
	BSTR text = NULL;
	if (name && V_VT(&value) == (VT_ARRAY | VT_UI1) && (!xmlStrcmp(name, (const xmlChar *)"bin.base64") || !xmlStrcmp(name, (const xmlChar *)"bin.hex")))
	{
		void *data;
		LONG low = 0, high = -1;
		xmlChar *encoded;
		SafeArrayGetLBound(V_ARRAY(&value), 1, &low);
		SafeArrayGetUBound(V_ARRAY(&value), 1, &high);
		status = SafeArrayAccessData(V_ARRAY(&value), &data);
		if (SUCCEEDED(status))
		{
			encoded = encode_binary(data, (ULONG)(high - low + 1), name[4] == 'b');
			SafeArrayUnaccessData(V_ARRAY(&value));
			text = mx_bstr(encoded);
			xmlFree(encoded);
		}
	}
	else if (V_VT(&value) == VT_BOOL)
		text = SysAllocString(V_BOOL(&value) ? L"1" : L"0");
	else
	{
		VARIANT converted;
		status = mx_variant_text(&value, &converted);
		if (SUCCEEDED(status))
			text = V_BSTR(&converted);
		else
			text = NULL;
	}
	xmlFree(name);
	if (!text)
		return E_FAIL;
	status = nd_put_text(iface, text);
	SysFreeString(text);
	return status;
}

HRESULT WINAPI nd_get_dataType(IXMLDOMNode *iface, VARIANT *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *name;
	if (!out)
		return E_INVALIDARG;
	V_VT(out) = VT_NULL;
	switch (node->type)
	{
	case XML_ELEMENT_NODE:
		name = data_type_of(node);
		if (!name)
			return S_FALSE;
		V_VT(out) = VT_BSTR;
		V_BSTR(out) = mx_bstr(name);
		xmlFree(name);
		return S_OK;
	case XML_ENTITY_DECL:
		V_VT(out) = VT_BSTR;
		V_BSTR(out) = SysAllocString(L"");
		return S_OK;
	default:
		return S_FALSE;
	}
}

HRESULT WINAPI nd_put_dataType(IXMLDOMNode *iface, BSTR type)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *name;
	xmlNsPtr space;
	if (!type || !*type)
		return E_INVALIDARG;
	name = mx_to_utf8(type);
	if (!find_data_type(name, NULL))
	{
		xmlFree(name);
		return E_FAIL;
	}
	if (node->type != XML_ELEMENT_NODE)
	{
		xmlFree(name);
		return node->type == XML_ATTRIBUTE_NODE || node->type == XML_TEXT_NODE ? S_FALSE : E_FAIL;
	}
	space = xmlSearchNsByHref(node->doc, node, dt_namespace);
	if (!space)
		space = xmlNewNs(node, dt_namespace, (const xmlChar *)"dt");
	if (space)
		xmlSetNsProp(node, space, (const xmlChar *)"dt", name);
	xmlFree(name);
	return S_OK;
}

/* --- Markup. --- */

/* Lines end in CR LF on the way out of a whole document, as they do on disk under Windows. */
static BSTR with_line_ends(const xmlChar *text, int bytes)
{
	int index, breaks = 0, to = 0;
	xmlChar *wide;
	BSTR out;
	for (index = 0; index < bytes; index++)
		if (text[index] == '\n' && (!index || text[index - 1] != '\r'))
			breaks++;
	if (!breaks)
		return mx_bstr_counted(text, bytes);
	wide = xmlMalloc(bytes + breaks);
	for (index = 0; index < bytes; index++)
	{
		if (text[index] == '\n' && (!index || text[index - 1] != '\r'))
			wide[to++] = '\r';
		wide[to++] = text[index];
	}
	out = mx_bstr_counted(wide, to);
	xmlFree(wide);
	return out;
}

static xmlChar *declaration_without_encoding(const xmlChar *content)
{
	const xmlChar *at = xmlStrstr(content, (const xmlChar *)"encoding"), *close = NULL, *from = at;
	xmlChar *out;
	while (from > content && from[-1] == ' ')
		from--;
	at += 8;
	while (*at == ' ' || *at == '=')
		at++;
	if (*at == '"' || *at == '\'')
		close = xmlStrchr(at + 1, *at);
	if (!close)
		return xmlStrdup(content);
	out = xmlStrndup(content, (int)(from - content));
	return xmlStrcat(out, close + 1);
}

static void write_escaped(xmlBufferPtr buffer, const xmlChar *text, BOOL attribute)
{
	const xmlChar *from = text, *at;
	if (!text)
		return;
	for (at = text; *at; at++)
	{
		const char *entity = NULL;
		if (*at == '&')
			entity = "&amp;";
		else if (*at == '<')
			entity = "&lt;";
		else if (*at == '>')
			entity = "&gt;";
		else if (*at == '"' && attribute)
			entity = "&quot;";
		else if (*at == '\r')
			entity = "&#13;";
		else if (*at == '\n')
			entity = attribute ? "&#10;" : "\r\n";
		else if (*at == '\t' && attribute)
			entity = "&#9;";
		if (entity)
		{
			xmlBufferAdd(buffer, from, (int)(at - from));
			xmlBufferCCat(buffer, entity);
			from = at + 1;
		}
	}
	xmlBufferAdd(buffer, from, (int)(at - from));
}

static void write_written_name(xmlBufferPtr buffer, xmlNodePtr node)
{
	if (node->ns && node->ns->prefix)
	{
		xmlBufferCat(buffer, node->ns->prefix);
		xmlBufferCCat(buffer, ":");
	}
	xmlBufferCat(buffer, node->name);
}

static void write_attribute(xmlBufferPtr buffer, xmlNodePtr node)
{
	xmlNodePtr at;
	write_written_name(buffer, node);
	xmlBufferCCat(buffer, "=\"");
	for (at = node->children; at; at = at->next)
		if (at->type == XML_ENTITY_REF_NODE)
		{
			xmlBufferCCat(buffer, "&");
			xmlBufferCat(buffer, at->name);
			xmlBufferCCat(buffer, ";");
		}
		else
			write_escaped(buffer, at->content, TRUE);
	xmlBufferCCat(buffer, "\"");
}

/* A declaration made only to hold a node's namespace is left out where the writing around it already says the same. */
static BOOL said_above(xmlNodePtr node, xmlNodePtr top, xmlNsPtr wanted)
{
	while (node != top && (node = node->parent) && node->type == XML_ELEMENT_NODE)
	{
		xmlNsPtr space;
		for (space = node->nsDef; space; space = space->next)
			if (xmlStrEqual(space->prefix, wanted->prefix))
				return xmlStrEqual(space->href, wanted->href);
	}
	return FALSE;
}

static void write_declarations(xmlBufferPtr buffer, xmlNodePtr node, xmlNodePtr top)
{
	xmlNsPtr space;
	for (space = node->nsDef; space; space = space->next)
	{
		xmlAttrPtr attribute;
		BOOL spelled = FALSE;
		if (space == node->nsDef && (node->extra & MX_IMPLIED_DECLARATION) && said_above(node, top, space))
			continue;
		/* The same declaration can also be there as an ordinary attribute someone set. */
		for (attribute = node->properties; attribute && !spelled; attribute = attribute->next)
			spelled = space->prefix && ((attribute->ns && attribute->ns->prefix && !xmlStrcmp(attribute->ns->prefix, (const xmlChar *)"xmlns") && xmlStrEqual(attribute->name, space->prefix)) ||
				(!xmlStrncmp(attribute->name, (const xmlChar *)"xmlns:", 6) && xmlStrEqual(attribute->name + 6, space->prefix)));
		if (spelled)
			continue;
		xmlBufferCCat(buffer, " xmlns");
		if (space->prefix)
		{
			xmlBufferCCat(buffer, ":");
			xmlBufferCat(buffer, space->prefix);
		}
		xmlBufferCCat(buffer, "=\"");
		write_escaped(buffer, space->href, TRUE);
		xmlBufferCCat(buffer, "\"");
	}
}

static void write_lines(xmlBufferPtr buffer, const xmlChar *text)
{
	const xmlChar *at;
	for (at = text; at && *at; at++)
	{
		if (*at == '\n')
			xmlBufferCCat(buffer, "\r");
		xmlBufferAdd(buffer, at, 1);
	}
}

/* Blanks left out of the tree come back as a line and indentation by depth, unless they had no line in them. */
static void write_blank(xmlBufferPtr buffer, xmlNodePtr blank, xmlNodePtr parent, xmlNodePtr top, int extra)
{
	int depth = extra;
	if (!blank)
		return;
	if (!xmlStrchr(blank->content, '\n'))
	{
		xmlBufferCat(buffer, blank->content);
		return;
	}
	xmlBufferCCat(buffer, "\r\n");
	for (; parent != top && parent && parent->type == XML_ELEMENT_NODE; parent = parent->parent)
		depth++;
	while (depth-- > 0)
		xmlBufferCCat(buffer, "\t");
}

static const xmlChar *kept_doctype(xmlDocPtr tree)
{
	xmlNodePtr at = tree && tree->psvi ? ((xmlNodePtr)tree->psvi)->children : NULL;
	for (; at; at = at->next)
		if (at->type == XML_COMMENT_NODE)
			return at->content;
	return NULL;
}

static void write_node(xmlBufferPtr buffer, xmlNodePtr node, xmlNodePtr top, BOOL whole_document)
{
	xmlNodePtr at;
	xmlAttrPtr attribute;
	switch (node->type)
	{
	case XML_ELEMENT_NODE:
		xmlBufferCCat(buffer, "<");
		write_written_name(buffer, node);
		if (!(node->extra & MX_ATTRIBUTES_FIRST))
			write_declarations(buffer, node, top);
		for (attribute = node->properties; attribute; attribute = attribute->next)
		{
			xmlBufferCCat(buffer, " ");
			write_attribute(buffer, (xmlNodePtr)attribute);
		}
		if (node->extra & MX_ATTRIBUTES_FIRST)
			write_declarations(buffer, node, top);
		if (!node->children && !mx_blank_at_end(node))
		{
			if (node->extra & MX_EXPLICIT_END)
			{
				xmlBufferCCat(buffer, "></");
				write_written_name(buffer, node);
				xmlBufferCCat(buffer, ">");
			}
			else
				xmlBufferCCat(buffer, "/>");
			break;
		}
		xmlBufferCCat(buffer, ">");
		for (at = node->children; at; at = at->next)
		{
			write_blank(buffer, mx_blank_before(at), node, top, 1);
			write_node(buffer, at, top, FALSE);
		}
		write_blank(buffer, mx_blank_at_end(node), node, top, 0);
		xmlBufferCCat(buffer, "</");
		write_written_name(buffer, node);
		xmlBufferCCat(buffer, ">");
		break;
	case XML_ATTRIBUTE_NODE:
		write_attribute(buffer, node);
		break;
	case XML_TEXT_NODE:
		write_escaped(buffer, node->content, FALSE);
		break;
	case XML_CDATA_SECTION_NODE:
		xmlBufferCCat(buffer, "<![CDATA[");
		xmlBufferCat(buffer, node->content);
		xmlBufferCCat(buffer, "]]>");
		break;
	case XML_COMMENT_NODE:
		xmlBufferCCat(buffer, "<!--");
		write_lines(buffer, node->content);
		xmlBufferCCat(buffer, "-->");
		break;
	case XML_ENTITY_REF_NODE:
		xmlBufferCCat(buffer, "&");
		xmlBufferCat(buffer, node->name);
		xmlBufferCCat(buffer, ";");
		break;
	case XML_PI_NODE:
	{
		/* The text is wide characters by now, so no other encoding it names still applies. */
		xmlChar *cut = NULL;
		const xmlChar *content = node->psvi && !xmlStrcmp(node->name, (const xmlChar *)"xml") ? ((xmlNodePtr)node->psvi)->content : node->content;
		if (whole_document && content && xmlStrstr(content, (const xmlChar *)"encoding") && !xmlStrstr(content, (const xmlChar *)"\"UTF-16\""))
			content = cut = declaration_without_encoding(content);
		xmlBufferCCat(buffer, "<?");
		xmlBufferCat(buffer, node->name);
		if (content && *content)
		{
			xmlBufferCCat(buffer, " ");
			write_lines(buffer, content);
		}
		xmlBufferCCat(buffer, "?>");
		xmlFree(cut);
		break;
	}
	case XML_DTD_NODE:
	{
		const xmlChar *kept = kept_doctype(node->doc);
		if (kept)
			xmlBufferCat(buffer, kept);
		else
			xmlNodeDump(buffer, node->doc, node, 0, 0);
		break;
	}
	case XML_DOCUMENT_NODE:
	case XML_DOCUMENT_FRAG_NODE:
		for (at = node->children; at; at = at->next)
		{
			write_node(buffer, at, at, node->type == XML_DOCUMENT_NODE);
			if (node->type == XML_DOCUMENT_NODE)
				xmlBufferCCat(buffer, "\n");
		}
		break;
	default:
		break;
	}
}

HRESULT mx_serialize(xmlNodePtr node, BSTR *out)
{
	xmlBufferPtr buffer = xmlBufferCreate();
	const xmlChar *text;
	int bytes;
	if (!buffer)
		return E_OUTOFMEMORY;
	write_node(buffer, node, node, FALSE);
	text = xmlBufferContent(buffer);
	bytes = xmlBufferLength(buffer);
	*out = node->type == XML_DOCUMENT_NODE || node->type == XML_DTD_NODE ? with_line_ends(text, bytes) : mx_bstr_counted(text, bytes);
	xmlBufferFree(buffer);
	return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI nd_get_xml(IXMLDOMNode *iface, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	return mx_serialize(mx_node_from(iface)->node, out);
}

HRESULT WINAPI nd_transformNode(IXMLDOMNode *iface, IXMLDOMNode *style, BSTR *out)
{
	if (!style || !out)
		return E_INVALIDARG;
	return mx_node_transform(mx_node_from(iface)->node, style, out, NULL, NULL);
}

HRESULT WINAPI nd_transformNodeToObject(IXMLDOMNode *iface, IXMLDOMNode *style, VARIANT target)
{
	if (!style)
		return E_INVALIDARG;
	return mx_node_transform_to_object(mx_node_from(iface)->node, style, target);
}

HRESULT WINAPI nd_selectNodes(IXMLDOMNode *iface, BSTR query, IXMLDOMNodeList **out)
{
	if (!out || !query)
		return E_INVALIDARG;
	*out = NULL;
	return mx_select_nodes(mx_node_from(iface)->node, query, out);
}

HRESULT WINAPI nd_selectSingleNode(IXMLDOMNode *iface, BSTR query, IXMLDOMNode **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!query)
		return E_INVALIDARG;
	return mx_select_single(mx_node_from(iface)->node, query, out);
}

HRESULT WINAPI nd_get_parsed(IXMLDOMNode *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = VARIANT_TRUE;
	return S_OK;
}

static BOOL is_namespace_attribute(xmlNodePtr node)
{
	return node->type == XML_ATTRIBUTE_NODE && (!xmlStrcmp(node->name, (const xmlChar *)"xmlns") || !xmlStrncmp(node->name, (const xmlChar *)"xmlns:", 6));
}

HRESULT WINAPI nd_get_namespaceURI(IXMLDOMNode *iface, BSTR *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (is_namespace_attribute(node))
		return mx_return_string(mx_tree_of(node->doc)->version >= MX_6 ? xmlns_namespace : ((xmlAttrPtr)node)->psvi != mx_declaration_mark && !xmlStrcmp(node->name, (const xmlChar *)"xmlns") ? node->name : (const xmlChar *)"", out);
	if (node->type == XML_ATTRIBUTE_NODE && node->ns && node->ns->prefix && !xmlStrcmp(node->ns->prefix, (const xmlChar *)"xmlns"))
		return mx_return_string(mx_tree_of(node->doc)->version >= MX_6 ? xmlns_namespace : (const xmlChar *)"", out);
	if ((node->type == XML_ELEMENT_NODE || node->type == XML_ATTRIBUTE_NODE) && node->ns && node->ns->href)
		return mx_return_string(node->ns->href, out);
	if (node->type == XML_ELEMENT_NODE || node->type == XML_ATTRIBUTE_NODE)
		return S_FALSE;
	if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE || node->type == XML_COMMENT_NODE || node->type == XML_PI_NODE)
		return mx_return_string((const xmlChar *)"", out);
	return S_FALSE;
}

HRESULT WINAPI nd_get_prefix(IXMLDOMNode *iface, BSTR *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (is_namespace_attribute(node))
		return mx_return_string((const xmlChar *)"xmlns", out);
	if ((node->type == XML_ELEMENT_NODE || node->type == XML_ATTRIBUTE_NODE) && node->ns && node->ns->prefix)
		return mx_return_string(node->ns->prefix, out);
	if (node->type == XML_ELEMENT_NODE && !node->ns && xmlStrchr(node->name, ':'))
	{
		/* A prefix without a known namespace stays in the plain name. */
		xmlChar *prefix = xmlStrndup(node->name, (int)(xmlStrchr(node->name, ':') - node->name));
		HRESULT status = mx_return_string(prefix, out);
		xmlFree(prefix);
		return status;
	}
	return S_FALSE;
}

HRESULT WINAPI nd_get_baseName(IXMLDOMNode *iface, BSTR *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	switch (node->type)
	{
	case XML_ELEMENT_NODE:
	case XML_ATTRIBUTE_NODE:
	case XML_ENTITY_REF_NODE:
	case XML_PI_NODE:
	case XML_DTD_NODE:
	case XML_ENTITY_DECL:
	case XML_NOTATION_NODE:
		if (is_namespace_attribute(node) && node->name[5] == ':')
			return mx_return_string(node->name + 6, out);
		return mx_return_string(node->name, out);
	default:
		return S_FALSE;
	}
}

/* --- Elements. --- */

static const xmlChar *split_qualified(const xmlChar *name, xmlChar **prefix)
{
	const xmlChar *colon = xmlStrchr(name, ':');
	*prefix = colon ? xmlStrndup(name, (int)(colon - name)) : NULL;
	return colon ? colon + 1 : name;
}

static xmlAttrPtr find_attribute(xmlNodePtr owner, const xmlChar *qualified)
{
	xmlChar *prefix;
	const xmlChar *local = split_qualified(qualified, &prefix);
	xmlAttrPtr at;
	for (at = owner->properties; at; at = at->next)
		if (!xmlStrcmp(at->name, local) && (prefix ? at->ns && at->ns->prefix && !xmlStrcmp(at->ns->prefix, prefix) : !at->ns))
			break;
	if (!at && !prefix)
		for (at = owner->properties; at; at = at->next)
			if (!xmlStrcmp(at->name, qualified) && !at->ns)
				break;
	xmlFree(prefix);
	return at;
}

static HRESULT WINAPI element_get_tagName(IXMLDOMElement *iface, BSTR *out)
{
	return nd_get_nodeName((IXMLDOMNode *)iface, out);
}

static HRESULT WINAPI element_getAttribute(IXMLDOMElement *iface, BSTR name, VARIANT *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *qualified, *value = NULL;
	xmlAttrPtr attribute;
	if (!name || !out)
		return E_INVALIDARG;
	V_VT(out) = VT_NULL;
	qualified = mx_to_utf8(name);
	if (xmlValidateQName(qualified, 0))
	{
		xmlFree(qualified);
		return E_FAIL;
	}
	attribute = find_attribute(node, qualified);
	if (attribute)
		value = xmlNodeGetContent((xmlNodePtr)attribute);
	else if (!xmlStrcmp(qualified, (const xmlChar *)"xmlns") || !xmlStrncmp(qualified, (const xmlChar *)"xmlns:", 6))
	{
		const xmlChar *wanted = qualified[5] ? qualified + 6 : NULL;
		xmlNsPtr space;
		for (space = (node->extra & MX_IMPLIED_DECLARATION) && node->nsDef ? node->nsDef->next : node->nsDef; space; space = space->next)
			if (wanted ? space->prefix && !xmlStrcmp(space->prefix, wanted) : !space->prefix)
				value = xmlStrdup(space->href ? space->href : (const xmlChar *)"");
	}
	xmlFree(qualified);
	if (!value)
		return S_FALSE;
	V_VT(out) = VT_BSTR;
	V_BSTR(out) = mx_bstr(value);
	xmlFree(value);
	return S_OK;
}

static HRESULT WINAPI element_setAttribute(IXMLDOMElement *iface, BSTR name, VARIANT value)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	VARIANT text;
	xmlChar *qualified, *utf8, *prefix;
	const xmlChar *local;
	HRESULT status = S_OK;
	if (!name || !*name)
		return E_INVALIDARG;
	if (V_VT(&value) == VT_NULL || V_VT(&value) == VT_EMPTY)
		return E_INVALIDARG;
	status = mx_variant_text(&value, &text);
	if (FAILED(status))
		return status;
	qualified = mx_to_utf8(name);
	utf8 = mx_to_utf8(V_BSTR(&text));
	VariantClear(&text);
	local = split_qualified(qualified, &prefix);
	if (!xmlStrcmp(qualified, (const xmlChar *)"xmlns") || (prefix && !xmlStrcmp(prefix, (const xmlChar *)"xmlns")))
	{
		const xmlChar *wanted = prefix ? local : NULL;
		xmlNsPtr space;
		for (space = node->nsDef; space; space = space->next)
			if (wanted ? space->prefix && !xmlStrcmp(space->prefix, wanted) : !space->prefix)
				break;
		if (space && !(space == node->nsDef && (node->extra & MX_IMPLIED_DECLARATION)))
			status = E_FAIL;
		else if (space)
		{
			if (xmlStrcmp(space->href, utf8))
				status = E_INVALIDARG;
			else
				node->extra &= ~MX_IMPLIED_DECLARATION;
		}
		else if (!wanted && node->ns && !node->ns->prefix && xmlStrcmp(node->ns->href, utf8))
			status = E_INVALIDARG;
		else if (!wanted && (!node->ns || xmlStrcmp(node->ns->href, utf8)))
			status = E_INVALIDARG;
		else if (!xmlNewNs(node, utf8, wanted))
			status = E_FAIL;
	}
	else if (prefix)
	{
		xmlNsPtr space = xmlSearchNs(node->doc, node, prefix);
		if (space)
			xmlSetNsProp(node, space, local, utf8);
		else
			xmlSetProp(node, qualified, utf8);
	}
	else
		xmlSetProp(node, qualified, utf8);
	xmlFree(prefix);
	xmlFree(qualified);
	xmlFree(utf8);
	return status;
}

/* A declaration still in use stays on the element, only no longer as an attribute. It is kept first, where the implied one lives. */
static HRESULT remove_declaration(xmlNodePtr node, const xmlChar *prefix)
{
	xmlNsPtr *link;
	for (link = &node->nsDef; *link; link = &(*link)->next)
	{
		xmlNsPtr space = *link;
		if (prefix ? !space->prefix || xmlStrcmp(space->prefix, prefix) : space->prefix != NULL)
			continue;
		if (space == node->nsDef && (node->extra & MX_IMPLIED_DECLARATION))
			return S_FALSE;
		*link = space->next;
		space->next = node->nsDef;
		node->nsDef = space;
		node->extra |= MX_IMPLIED_DECLARATION;
		node->extra &= ~MX_ATTRIBUTES_FIRST;
		return S_OK;
	}
	return S_FALSE;
}

static BOOL uses_space(xmlNodePtr node, xmlNsPtr space)
{
	xmlAttrPtr attribute;
	if (node->ns == space)
		return TRUE;
	for (attribute = node->properties; attribute; attribute = attribute->next)
		if (attribute->ns == space)
			return TRUE;
	return FALSE;
}

/* Hands a declaration over to the nearest elements below that use it, each of which then says it for itself. */
static void hand_down(xmlNodePtr parent, xmlNsPtr space)
{
	xmlNodePtr at;
	for (at = parent->children; at; at = at->next)
	{
		xmlNsPtr own;
		xmlAttrPtr attribute;
		if (at->type != XML_ELEMENT_NODE)
			continue;
		if (!uses_space(at, space))
		{
			hand_down(at, space);
			continue;
		}
		own = xmlNewNs(NULL, space->href, space->prefix);
		if (!own)
			continue;
		own->next = at->nsDef;
		at->nsDef = own;
		if (at->ns == space)
			at->ns = own;
		for (attribute = at->properties; attribute; attribute = attribute->next)
			if (attribute->ns == space)
				attribute->ns = own;
		hand_down(at, space);
	}
}

/* Once nothing on the element needs the declaration that stayed only for use, it moves down to whoever still does. */
static void settle_implied_declaration(xmlNodePtr node)
{
	xmlNsPtr space = node->nsDef;
	if (!(node->extra & MX_IMPLIED_DECLARATION) || !space || uses_space(node, space))
		return;
	hand_down(node, space);
	node->nsDef = space->next;
	node->extra &= ~MX_IMPLIED_DECLARATION;
	space->next = NULL;
	xmlFreeNs(space);
}

static HRESULT WINAPI element_removeAttribute(IXMLDOMElement *iface, BSTR name)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *qualified;
	xmlAttrPtr attribute;
	if (!name)
		return E_INVALIDARG;
	qualified = mx_to_utf8(name);
	attribute = find_attribute(node, qualified);
	if (!attribute && (!xmlStrcmp(qualified, (const xmlChar *)"xmlns") || !xmlStrncmp(qualified, (const xmlChar *)"xmlns:", 6)))
	{
		HRESULT status = remove_declaration(node, qualified[5] ? qualified + 6 : NULL);
		xmlFree(qualified);
		return status;
	}
	xmlFree(qualified);
	if (!attribute)
		return S_FALSE;
	xmlUnlinkNode((xmlNodePtr)attribute);
	mx_tree_orphan((xmlNodePtr)attribute);
	settle_implied_declaration(node);
	return S_OK;
}

static HRESULT WINAPI element_getAttributeNode(IXMLDOMElement *iface, BSTR name, IXMLDOMAttribute **out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *qualified;
	xmlAttrPtr attribute;
	if (out)
		*out = NULL;
	if (!name)
		return E_FAIL;
	qualified = mx_to_utf8(name);
	if (xmlValidateQName(qualified, 0))
	{
		xmlFree(qualified);
		return E_FAIL;
	}
	attribute = find_attribute(node, qualified);
	if (!attribute && out && (!xmlStrcmp(qualified, (const xmlChar *)"xmlns") || !xmlStrncmp(qualified, (const xmlChar *)"xmlns:", 6)))
	{
		const xmlChar *wanted = qualified[5] ? qualified + 6 : NULL;
		xmlNsPtr space;
		for (space = (node->extra & MX_IMPLIED_DECLARATION) && node->nsDef ? node->nsDef->next : node->nsDef; space; space = space->next)
			if (wanted ? space->prefix && !xmlStrcmp(space->prefix, wanted) : !space->prefix)
			{
				xmlFree(qualified);
				return mx_node_create(mx_declaration_attribute(node, space), (IXMLDOMNode **)out);
			}
	}
	xmlFree(qualified);
	if (!attribute)
		return S_FALSE;
	return out ? mx_node_create((xmlNodePtr)attribute, (IXMLDOMNode **)out) : S_OK;
}

static HRESULT WINAPI element_setAttributeNode(IXMLDOMElement *iface, IXMLDOMAttribute *attribute, IXMLDOMAttribute **out)
{
	xmlNodePtr node = mx_node_from(iface)->node, given = mx_node_get((IUnknown *)attribute);
	xmlAttrPtr existing;
	xmlChar *name;
	if (!attribute)
		return E_INVALIDARG;
	if (!given || given->type != XML_ATTRIBUTE_NODE || given->parent)
		return E_FAIL;
	if (out)
		*out = NULL;
	name = qualified_name(given);
	existing = find_attribute(node, name);
	xmlFree(name);
	if (existing)
	{
		xmlUnlinkNode((xmlNodePtr)existing);
		mx_tree_orphan((xmlNodePtr)existing);
		if (out)
			mx_node_create((xmlNodePtr)existing, (IXMLDOMNode **)out);
	}
	mx_tree_adopted(given);
	xmlAddChild(node, given);
	return S_OK;
}

static HRESULT WINAPI element_removeAttributeNode(IXMLDOMElement *iface, IXMLDOMAttribute *attribute, IXMLDOMAttribute **out)
{
	xmlNodePtr node = mx_node_from(iface)->node, given = mx_node_get((IUnknown *)attribute);
	if (!given || given->type != XML_ATTRIBUTE_NODE || given->parent != node)
		return E_INVALIDARG;
	if (out)
		*out = NULL;
	xmlUnlinkNode(given);
	mx_tree_orphan(given);
	if (out)
	{
		*out = attribute;
		IXMLDOMAttribute_AddRef(attribute);
	}
	return S_OK;
}

static HRESULT WINAPI element_getElementsByTagName(IXMLDOMElement *iface, BSTR name, IXMLDOMNodeList **out)
{
	if (!name || !out)
		return E_INVALIDARG;
	return mx_select_by_name(mx_node_from(iface)->node, name, out);
}

/* Adjacent text nodes become one and empty ones go, all the way down. */
static void normalize_children(xmlNodePtr parent)
{
	xmlNodePtr at = parent->children;
	while (at)
	{
		xmlNodePtr next = at->next;
		if (at->type == XML_ELEMENT_NODE)
			normalize_children(at);
		else if (at->type == XML_TEXT_NODE)
		{
			while (next && next->type == XML_TEXT_NODE)
			{
				xmlNodePtr after = next->next;
				at->content = xmlStrcat(at->content, next->content);
				xmlUnlinkNode(next);
				mx_tree_orphan(next);
				next = after;
			}
			if (!at->content || !*at->content)
			{
				xmlUnlinkNode(at);
				mx_tree_orphan(at);
			}
		}
		at = next;
	}
}

static HRESULT WINAPI element_normalize(IXMLDOMElement *iface)
{
	normalize_children(mx_node_from(iface)->node);
	return S_OK;
}

static const IXMLDOMElementVtbl element_table = {
	MX_NODE_TABLE,
	.get_tagName = element_get_tagName, .getAttribute = element_getAttribute, .setAttribute = element_setAttribute,
	.removeAttribute = element_removeAttribute, .getAttributeNode = element_getAttributeNode, .setAttributeNode = element_setAttributeNode,
	.removeAttributeNode = element_removeAttributeNode, .getElementsByTagName = element_getElementsByTagName, .normalize = element_normalize,
};

/* --- Attributes. --- */

static HRESULT WINAPI attribute_get_name(IXMLDOMAttribute *iface, BSTR *out)
{
	return nd_get_nodeName((IXMLDOMNode *)iface, out);
}

static HRESULT WINAPI attribute_get_value(IXMLDOMAttribute *iface, VARIANT *out)
{
	return nd_get_nodeValue((IXMLDOMNode *)iface, out);
}

static HRESULT WINAPI attribute_put_value(IXMLDOMAttribute *iface, VARIANT value)
{
	return nd_put_nodeValue((IXMLDOMNode *)iface, value);
}

static const IXMLDOMAttributeVtbl attribute_table = {
	MX_NODE_TABLE, .get_name = attribute_get_name, .get_value = attribute_get_value, .put_value = attribute_put_value,
};

/* --- Character data, by wide character positions. --- */

static HRESULT data_read(xmlNodePtr node, BSTR *out)
{
	xmlChar *text = xmlNodeGetContent(node);
	*out = text ? mx_bstr(text) : SysAllocStringLen(NULL, 0);
	xmlFree(text);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT data_write(xmlNodePtr node, const WCHAR *text, int length)
{
	xmlChar *utf8 = mx_to_utf8_counted(text ? text : L"", text ? length : 0);
	HRESULT status = set_content(node, utf8);
	xmlFree(utf8);
	return status;
}

static HRESULT WINAPI data_get_data(IXMLDOMCharacterData *iface, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	return data_read(mx_node_from(iface)->node, out);
}

static HRESULT WINAPI data_put_data(IXMLDOMCharacterData *iface, BSTR text)
{
	return data_write(mx_node_from(iface)->node, text, text ? lstrlenW(text) : 0);
}

static HRESULT WINAPI data_get_length(IXMLDOMCharacterData *iface, LONG *out)
{
	BSTR text;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	status = data_read(mx_node_from(iface)->node, &text);
	if (FAILED(status))
		return status;
	*out = SysStringLen(text);
	SysFreeString(text);
	return S_OK;
}

static HRESULT WINAPI data_substringData(IXMLDOMCharacterData *iface, LONG offset, LONG count, BSTR *out)
{
	BSTR text;
	LONG length;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (offset < 0 || count < 0)
		return E_INVALIDARG;
	if (!count)
		return S_FALSE;
	status = data_read(mx_node_from(iface)->node, &text);
	if (FAILED(status))
		return status;
	length = SysStringLen(text);
	if (offset > length)
		status = E_INVALIDARG;
	else if (offset == length)
		status = S_FALSE;
	else
	{
		*out = SysAllocStringLen(text + offset, min(count, length - offset));
		status = *out ? S_OK : E_OUTOFMEMORY;
	}
	SysFreeString(text);
	return status;
}

static HRESULT splice_data(xmlNodePtr node, LONG offset, LONG remove, const WCHAR *insert)
{
	BSTR text, joined;
	LONG length, added = insert ? lstrlenW(insert) : 0;
	HRESULT status = data_read(node, &text);
	if (FAILED(status))
		return status;
	length = SysStringLen(text);
	if (offset < 0 || remove < 0 || offset > length)
	{
		SysFreeString(text);
		return E_INVALIDARG;
	}
	if (remove > length - offset)
		remove = length - offset;
	joined = SysAllocStringLen(NULL, length - remove + added);
	memcpy(joined, text, offset * sizeof(WCHAR));
	if (added)
		memcpy(joined + offset, insert, added * sizeof(WCHAR));
	memcpy(joined + offset + added, text + offset + remove, (length - offset - remove) * sizeof(WCHAR));
	status = data_write(node, joined, length - remove + added);
	SysFreeString(joined);
	SysFreeString(text);
	return status;
}

static HRESULT WINAPI data_appendData(IXMLDOMCharacterData *iface, BSTR text)
{
	LONG length;
	if (!text || !*text)
		return S_OK;
	data_get_length(iface, &length);
	return splice_data(mx_node_from(iface)->node, length, 0, text);
}

static HRESULT WINAPI data_insertData(IXMLDOMCharacterData *iface, LONG offset, BSTR text)
{
	if (!text || !*text)
		return S_OK;
	return splice_data(mx_node_from(iface)->node, offset, 0, text);
}

static HRESULT WINAPI data_deleteData(IXMLDOMCharacterData *iface, LONG offset, LONG count)
{
	LONG length;
	data_get_length(iface, &length);
	if (offset < 0 || count < 0 || offset > length)
		return E_INVALIDARG;
	if (!count || !length)
		return S_OK;
	return splice_data(mx_node_from(iface)->node, offset, count, NULL);
}

static HRESULT WINAPI data_replaceData(IXMLDOMCharacterData *iface, LONG offset, LONG count, BSTR text)
{
	return splice_data(mx_node_from(iface)->node, offset, count, text);
}

static HRESULT WINAPI text_splitText(IXMLDOMText *iface, LONG offset, IXMLDOMText **out)
{
	xmlNodePtr node = mx_node_from(iface)->node, second;
	BSTR text;
	LONG length;
	HRESULT status;
	xmlChar *tail;
	if (!out || offset < 0)
		return E_INVALIDARG;
	*out = NULL;
	status = data_read(node, &text);
	if (FAILED(status))
		return status;
	length = SysStringLen(text);
	if (offset > length)
	{
		SysFreeString(text);
		return E_INVALIDARG;
	}
	if (offset == length)
	{
		SysFreeString(text);
		return S_FALSE;
	}
	tail = mx_to_utf8(text + offset);
	second = node->type == XML_CDATA_SECTION_NODE ? xmlNewCDataBlock(node->doc, tail, xmlStrlen(tail)) : xmlNewDocText(node->doc, tail);
	xmlFree(tail);
	data_write(node, text, offset);
	SysFreeString(text);
	if (!second)
		return E_OUTOFMEMORY;
	if (node->parent)
		splice(node->parent, second, node->next);
	else
		mx_tree_orphan(second);
	return out ? mx_node_create(second, (IXMLDOMNode **)out) : S_OK;
}

#define MX_DATA_TABLE \
	MX_NODE_TABLE, .get_data = (void *)data_get_data, .put_data = (void *)data_put_data, .get_length = (void *)data_get_length, \
	.substringData = (void *)data_substringData, .appendData = (void *)data_appendData, .insertData = (void *)data_insertData, \
	.deleteData = (void *)data_deleteData, .replaceData = (void *)data_replaceData

static const IXMLDOMTextVtbl text_table = { MX_DATA_TABLE, .splitText = text_splitText };
static const IXMLDOMCDATASectionVtbl cdata_table = { MX_DATA_TABLE, .splitText = (void *)text_splitText };
static const IXMLDOMCommentVtbl comment_table = { MX_DATA_TABLE };

/* --- Processing instructions. --- */

static HRESULT WINAPI pi_get_target(IXMLDOMProcessingInstruction *iface, BSTR *out)
{
	return mx_return_string(mx_node_from(iface)->node->name, out);
}

static HRESULT WINAPI pi_get_data(IXMLDOMProcessingInstruction *iface, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	return data_read(mx_node_from(iface)->node, out);
}

static HRESULT WINAPI pi_put_data(IXMLDOMProcessingInstruction *iface, BSTR text)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	/* The declaration at the top of a document is fixed once written. */
	if (!xmlStrcmp(node->name, (const xmlChar *)"xml"))
		return E_FAIL;
	return data_write(node, text, text ? lstrlenW(text) : 0);
}

static const IXMLDOMProcessingInstructionVtbl pi_table = {
	MX_NODE_TABLE, .get_target = pi_get_target, .get_data = pi_get_data, .put_data = pi_put_data,
};

/* --- The rest of the node kinds. --- */

static const IXMLDOMDocumentFragmentVtbl fragment_table = { MX_NODE_TABLE };
static const IXMLDOMEntityReferenceVtbl entity_ref_table = { MX_NODE_TABLE };

static HRESULT WINAPI doctype_get_name(IXMLDOMDocumentType *iface, BSTR *out)
{
	return nd_get_nodeName((IXMLDOMNode *)iface, out);
}

static HRESULT WINAPI doctype_get_entities(IXMLDOMDocumentType *iface, IXMLDOMNamedNodeMap **out)
{
	xmlDtdPtr type = (xmlDtdPtr)mx_node_from(iface)->node;
	if (!out)
		return E_INVALIDARG;
	return mx_dtd_map(type->doc, type->entities, out);
}

static HRESULT WINAPI doctype_get_notations(IXMLDOMDocumentType *iface, IXMLDOMNamedNodeMap **out)
{
	xmlDtdPtr type = (xmlDtdPtr)mx_node_from(iface)->node;
	if (!out)
		return E_INVALIDARG;
	return mx_dtd_map(type->doc, type->notations, out);
}

static const IXMLDOMDocumentTypeVtbl doctype_table = {
	MX_NODE_TABLE, .get_name = doctype_get_name, .get_entities = doctype_get_entities, .get_notations = doctype_get_notations,
};

static HRESULT return_id(const xmlChar *text, VARIANT *out)
{
	if (!out)
		return E_INVALIDARG;
	if (!text)
		return mx_return_null_variant(out);
	V_VT(out) = VT_BSTR;
	V_BSTR(out) = mx_bstr(text);
	return S_OK;
}

static HRESULT WINAPI entity_get_publicId(IXMLDOMEntity *iface, VARIANT *out)
{
	return return_id(((xmlEntityPtr)mx_node_from(iface)->node)->ExternalID, out);
}

static HRESULT WINAPI entity_get_systemId(IXMLDOMEntity *iface, VARIANT *out)
{
	return return_id(((xmlEntityPtr)mx_node_from(iface)->node)->SystemID, out);
}

static HRESULT WINAPI entity_get_notationName(IXMLDOMEntity *iface, BSTR *out)
{
	xmlEntityPtr entity = (xmlEntityPtr)mx_node_from(iface)->node;
	return mx_return_string(entity->etype == XML_EXTERNAL_GENERAL_UNPARSED_ENTITY ? entity->content : NULL, out);
}

static const IXMLDOMEntityVtbl entity_table = {
	MX_NODE_TABLE, .get_publicId = entity_get_publicId, .get_systemId = entity_get_systemId, .get_notationName = entity_get_notationName,
};

static HRESULT WINAPI notation_get_publicId(IXMLDOMNotation *iface, VARIANT *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *value = xmlGetProp(node, (const xmlChar *)"public");
	HRESULT status = return_id(value, out);
	xmlFree(value);
	return status;
}

static HRESULT WINAPI notation_get_systemId(IXMLDOMNotation *iface, VARIANT *out)
{
	xmlNodePtr node = mx_node_from(iface)->node;
	xmlChar *value = xmlGetProp(node, (const xmlChar *)"system");
	HRESULT status = return_id(value, out);
	xmlFree(value);
	return status;
}

static const IXMLDOMNotationVtbl notation_table = {
	MX_NODE_TABLE, .get_publicId = notation_get_publicId, .get_systemId = notation_get_systemId,
};

HRESULT mx_node_create(xmlNodePtr node, IXMLDOMNode **out)
{
	mx_node *self;
	const void *table;
	mx_type type;
	const IID *iid;
	if (node->type == XML_DOCUMENT_NODE)
		return mx_document_wrap((xmlDocPtr)node, &IID_IXMLDOMNode, (void **)out);
	switch (node->type)
	{
	case XML_ELEMENT_NODE: table = &element_table; type = MX_TYPE_ELEMENT; iid = &IID_IXMLDOMElement; break;
	case XML_ATTRIBUTE_NODE: table = &attribute_table; type = MX_TYPE_ATTRIBUTE; iid = &IID_IXMLDOMAttribute; break;
	case XML_TEXT_NODE: table = &text_table; type = MX_TYPE_TEXT; iid = &IID_IXMLDOMText; break;
	case XML_CDATA_SECTION_NODE: table = &cdata_table; type = MX_TYPE_CDATA; iid = &IID_IXMLDOMCDATASection; break;
	case XML_COMMENT_NODE: table = &comment_table; type = MX_TYPE_COMMENT; iid = &IID_IXMLDOMComment; break;
	case XML_PI_NODE: table = &pi_table; type = MX_TYPE_PI; iid = &IID_IXMLDOMProcessingInstruction; break;
	case XML_DOCUMENT_FRAG_NODE: table = &fragment_table; type = MX_TYPE_FRAGMENT; iid = &IID_IXMLDOMDocumentFragment; break;
	case XML_DTD_NODE: table = &doctype_table; type = MX_TYPE_DOCTYPE; iid = &IID_IXMLDOMDocumentType; break;
	case XML_ENTITY_REF_NODE: table = &entity_ref_table; type = MX_TYPE_ENTITY_REF; iid = &IID_IXMLDOMEntityReference; break;
	case XML_ENTITY_DECL: table = &entity_table; type = MX_TYPE_ENTITY; iid = &IID_IXMLDOMEntity; break;
	case XML_NOTATION_NODE: table = &notation_table; type = MX_TYPE_NOTATION; iid = &IID_IXMLDOMNotation; break;
	default:
		*out = NULL;
		return E_FAIL;
	}
	self = mx_zalloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	mx_node_init(self, table, node, type, iid);
	*out = (IXMLDOMNode *)self;
	return S_OK;
}
