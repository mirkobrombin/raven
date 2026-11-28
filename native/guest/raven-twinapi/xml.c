/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Data.Xml.Dom: XmlDocument and its nodes, node lists and attribute maps, over the DOM of Raven's msxml3 in its 6.0 mode (XPath
 * selection, DTDs refused unless the load settings allow them), as the Windows classes are a view of MSXML's DOM. Each node of the
 * document is handed out as an object that answers IXmlNode, IXmlNodeSelector and IXmlNodeSerializer and the interfaces of its type;
 * lists and maps are also IVectorView<IXmlNode>. */
#include "object.h"
#include "../raven-winrt/winrt.h"
#include <initguid.h>
#include <msxml2.h>

static const IID iid_node = {0x1c741d59, 0x2122, 0x47d5, {0xa8, 0x56, 0x83, 0xf3, 0xd4, 0x21, 0x48, 0x75}};
static const IID iid_selector = {0x63dbba8b, 0xd0db, 0x4fe1, {0xb7, 0x45, 0xf9, 0x43, 0x3a, 0xfd, 0xc2, 0x5b}};
static const IID iid_serializer = {0x5cc5b382, 0xe6dd, 0x4991, {0xab, 0xef, 0x06, 0xd8, 0xd2, 0xe7, 0xbd, 0x0c}};
static const IID iid_element = {0x2dfb8a1f, 0x6b10, 0x4ef8, {0x9f, 0x83, 0xef, 0xcc, 0xe8, 0xfa, 0xec, 0x37}};
static const IID iid_attribute = {0xac144aa4, 0xb4f1, 0x4db6, {0xb2, 0x06, 0x8a, 0x22, 0xc3, 0x08, 0xdb, 0x0a}};
static const IID iid_text = {0xf931a4cb, 0x308d, 0x4760, {0xa1, 0xd5, 0x43, 0xb6, 0x74, 0x50, 0xac, 0x7e}};
static const IID iid_character_data = {0x132e42ab, 0x4e36, 0x4df6, {0xb1, 0xc8, 0x0c, 0xe6, 0x2f, 0xd8, 0x8b, 0x26}};
static const IID iid_cdata = {0x4d04b46f, 0xc8bd, 0x45b4, {0x88, 0x99, 0x04, 0x00, 0xd7, 0xc2, 0xc6, 0x0f}};
static const IID iid_entity_reference = {0x2e2f47bc, 0xc3d0, 0x4ccf, {0xbb, 0x86, 0x0a, 0xb8, 0xc3, 0x6a, 0x61, 0xcf}};
static const IID iid_dtd_entity = {0x6a0b5ffc, 0x63b4, 0x480f, {0x9e, 0x6a, 0x8a, 0x92, 0x81, 0x6a, 0xad, 0xe4}};
static const IID iid_instruction = {0x2707fd1e, 0x1e92, 0x4ece, {0xb6, 0xf4, 0x26, 0xf0, 0x69, 0x07, 0x8d, 0xdc}};
static const IID iid_comment = {0xbca474d5, 0xb61f, 0x4611, {0x9c, 0xac, 0x2e, 0x92, 0xe3, 0x47, 0x6d, 0x47}};
static const IID iid_document = {0xf7f3a506, 0x1e87, 0x42d6, {0xbc, 0xfb, 0xb8, 0xc8, 0x09, 0xfa, 0x54, 0x94}};
static const IID iid_document_io = {0x6cd0e74e, 0xee65, 0x4489, {0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37}};
static const IID iid_document_io2 = {0x5d034661, 0x7bd8, 0x4ad5, {0x9e, 0xbf, 0x81, 0xe6, 0x34, 0x72, 0x63, 0xb1}};
static const IID iid_document_type = {0xf7342425, 0x9781, 0x4964, {0x8e, 0x94, 0x9b, 0x1c, 0x6d, 0xfc, 0x9b, 0xc7}};
static const IID iid_fragment = {0xe2ea6a96, 0x0c21, 0x44a5, {0x8b, 0xc9, 0x9e, 0x4a, 0x26, 0x27, 0x08, 0xec}};
static const IID iid_dtd_notation = {0x8cb4e04d, 0x6d46, 0x4edb, {0xab, 0x73, 0xdf, 0x83, 0xc5, 0x1a, 0xd3, 0x97}};
static const IID iid_node_list = {0x8c60ad77, 0x83a4, 0x4ec1, {0x9c, 0x54, 0x7b, 0xa4, 0x29, 0xe1, 0x3d, 0xa6}};
static const IID iid_node_map = {0xb3a69eb0, 0xaab0, 0x4b82, {0xa6, 0xfa, 0xb1, 0x45, 0x3f, 0x7c, 0x02, 0x1b}};
static const IID iid_implementation = {0x6de58132, 0xf11d, 0x4fbb, {0x8c, 0xc6, 0x58, 0x3c, 0xba, 0x93, 0x11, 0x2f}};
static const IID iid_load_settings = {0x58aa07a8, 0xfed6, 0x46f7, {0xb4, 0xc5, 0xfb, 0x1b, 0xa7, 0x21, 0x08, 0xd6}};
static const IID iid_document_statics = {0x5543d254, 0xd757, 0x4b79, {0x95, 0x39, 0x23, 0x2b, 0x18, 0xf5, 0x0b, 0xf1}};
/* IVectorView, IIterable and IIterator of Windows.Data.Xml.Dom.IXmlNode, and IAsyncOperation<XmlDocument>. */
static const IID iid_node_view = {0x139d959e, 0xe7b5, 0x5cb6, {0xa5, 0x96, 0x4b, 0x54, 0x44, 0x78, 0xda, 0x9b}};
static const IID iid_node_iterable = {0xf1146ffc, 0x8c92, 0x56e8, {0x93, 0xf1, 0x71, 0x1f, 0x86, 0x72, 0x26, 0x33}};
static const IID iid_node_iterator = {0x3833a35e, 0x2c61, 0x56bd, {0xb0, 0x93, 0x36, 0x94, 0x16, 0x5f, 0x88, 0x98}};
static const IID iid_document_operation = {0xf858e239, 0x1896, 0x5982, {0x84, 0x95, 0x14, 0x31, 0x68, 0x47, 0x8e, 0xb8}};
/* MSXML 6.0's free-threaded DOM document, which any thread may use as the Windows Runtime's objects may be. */
static const CLSID clsid_free_document60 = {0x88d96a06, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}};

static const struct winrt_items_kind node_items = {&iid_node_view, &iid_node_iterable, &iid_node_iterator, FALSE, NULL};

/* The places of a node's interfaces: the three every node has, then those of its type from OWN_FACE on. */
enum { NODE_FACE, SELECTOR_FACE, SERIALIZER_FACE, OWN_FACE };
#define STRING_TYPE 12

/* Strings between the Windows Runtime and MSXML. */
static BSTR bstr_of(HSTRING string)
{
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(string, &length);
	return SysAllocStringLen(text, length);
}

/* A BSTR MSXML handed out, as an HSTRING; the BSTR is freed. */
static HRESULT answer_bstr(HRESULT result, BSTR *given, HSTRING *out)
{
	BSTR text = *given;
	if (SUCCEEDED(result))
		result = WindowsCreateString(text, SysStringLen(text), out);
	SysFreeString(text);
	return result;
}

/* A string as Object, as namespace URIs and names are: a boxed string, or null for none. */
static HRESULT answer_boxed(HRESULT result, BSTR *given, void **out)
{
	BSTR text = *given;
	HSTRING string = NULL;
	*out = NULL;
	if (SUCCEEDED(result) && text && *text && SUCCEEDED(result = WindowsCreateString(text, SysStringLen(text), &string)))
		result = property_value_make(STRING_TYPE, &string, out);
	WindowsDeleteString(string);
	SysFreeString(text);
	return FAILED(result) ? result : S_OK;
}

static HRESULT answer_variant_boxed(HRESULT result, VARIANT *value, void **out)
{
	if (SUCCEEDED(result) && value->vt == VT_BSTR)
		return answer_boxed(S_OK, &value->bstrVal, out);
	VariantClear(value);
	*out = NULL;
	return FAILED(result) ? result : S_OK;
}

/* The string in an Object argument as a BSTR, NULL for null; E_INVALIDARG for anything but a string. */
static HRESULT unbox(IInspectable *value, BSTR *out)
{
	HSTRING string = NULL;
	*out = NULL;
	if (!value)
		return S_OK;
	if (FAILED(property_value_string((IUnknown *)value, &string)))
		return E_INVALIDARG;
	*out = bstr_of(string);
	WindowsDeleteString(string);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* A node: one of MSXML's, with the kind of its type. */
struct node {
	struct object base;
	IXMLDOMNode *node;
};

/* A list of nodes, or a map of them: what both hand out by index. */
struct collection {
	struct object base;
	IXMLDOMNodeList *list;
	IXMLDOMNamedNodeMap *map;
};

struct implementation {
	struct object base;
	IXMLDOMImplementation *implementation;
};

struct settings {
	struct object base;
	UINT32 depth;
	boolean prohibit_dtd, resolve_externals, validate, white_space;
};

static void destroy_node(struct object *base) { IXMLDOMNode_Release(((struct node *)base)->node); }

static void destroy_collection(struct object *base)
{
	struct collection *self = (struct collection *)base;
	if (self->list)
		IXMLDOMNodeList_Release(self->list);
	if (self->map)
		IXMLDOMNamedNodeMap_Release(self->map);
}

static void destroy_implementation(struct object *base)
{
	IXMLDOMImplementation_Release(((struct implementation *)base)->implementation);
}

static struct node *node_of(void *iface) { return (struct node *)owner(iface); }
static IXMLDOMNode *dom(void *iface) { return node_of(iface)->node; }

/* The MSXML node of a node of ours, without a reference; NULL for anything else. */
static IXMLDOMNode *dom_of(void *iface)
{
	if (!iface || method(iface, 0) != (void *)object_query || owner(iface)->kind->destroy != destroy_node)
		return NULL;
	return dom(iface);
}

/* The node as another of MSXML's interfaces, with a reference. */
static HRESULT dom_as(void *iface, REFIID iid, void *out) { return IXMLDOMNode_QueryInterface(dom(iface), iid, (void **)out); }

static HRESULT wrap(IUnknown *dom_node, size_t face, void **out);
static HRESULT wrap_list(IXMLDOMNodeList *list, void **out);
static HRESULT wrap_map(IXMLDOMNamedNodeMap *map, void **out);

/* An answer that is a node MSXML handed out (with its reference, which is taken over), at a face of ours. */
static HRESULT answer_node(HRESULT result, void *given, size_t face, void **out)
{
	IUnknown *dom_node = *(IUnknown **)given;
	*out = NULL;
	if (SUCCEEDED(result) && dom_node)
		result = wrap(dom_node, face, out);
	if (dom_node)
		dom_node->lpVtbl->Release(dom_node);
	return FAILED(result) ? result : S_OK;
}

static HRESULT answer_list(HRESULT result, IXMLDOMNodeList **given, void **out)
{
	IXMLDOMNodeList *list = *given;
	*out = NULL;
	if (SUCCEEDED(result) && list)
		result = wrap_list(list, out);
	if (list)
		IXMLDOMNodeList_Release(list);
	return FAILED(result) ? result : S_OK;
}

static HRESULT answer_map(HRESULT result, IXMLDOMNamedNodeMap **given, void **out)
{
	IXMLDOMNamedNodeMap *map = *given;
	*out = NULL;
	if (SUCCEEDED(result) && map)
		result = wrap_map(map, out);
	if (map)
		IXMLDOMNamedNodeMap_Release(map);
	return FAILED(result) ? result : S_OK;
}

/* IXmlNode. */
static HRESULT WINAPI node_value(void *iface, void **out)
{
	VARIANT value;
	if (!out)
		return E_POINTER;
	VariantInit(&value);
	return answer_variant_boxed(IXMLDOMNode_get_nodeValue(dom(iface), &value), &value, out);
}

static HRESULT WINAPI node_set_value(void *iface, IInspectable *value)
{
	VARIANT text;
	VariantInit(&text);
	HRESULT result = unbox(value, &text.bstrVal);
	if (FAILED(result))
		return result;
	text.vt = text.bstrVal ? VT_BSTR : VT_NULL;
	result = IXMLDOMNode_put_nodeValue(dom(iface), text);
	VariantClear(&text);
	return result;
}

static HRESULT WINAPI node_type(void *iface, INT32 *out)
{
	DOMNodeType type = NODE_INVALID;
	if (!out)
		return E_POINTER;
	const HRESULT result = IXMLDOMNode_get_nodeType(dom(iface), &type);
	*out = type;
	return result;
}

static HRESULT WINAPI node_name(void *iface, HSTRING *out)
{
	BSTR name = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	return answer_bstr(IXMLDOMNode_get_nodeName(dom(iface), &name), &name, out);
}

#define NODE_GETTER(function, getter) \
	static HRESULT WINAPI function(void *iface, void **out) \
	{ \
		IXMLDOMNode *found = NULL; \
		if (!out) \
			return E_POINTER; \
		return answer_node(getter(dom(iface), &found), &found, NODE_FACE, out); \
	}

NODE_GETTER(node_parent, IXMLDOMNode_get_parentNode)
NODE_GETTER(node_first, IXMLDOMNode_get_firstChild)
NODE_GETTER(node_last, IXMLDOMNode_get_lastChild)
NODE_GETTER(node_previous, IXMLDOMNode_get_previousSibling)
NODE_GETTER(node_next, IXMLDOMNode_get_nextSibling)

static HRESULT WINAPI node_children(void *iface, void **out)
{
	IXMLDOMNodeList *list = NULL;
	if (!out)
		return E_POINTER;
	return answer_list(IXMLDOMNode_get_childNodes(dom(iface), &list), &list, out);
}

static HRESULT WINAPI node_attributes(void *iface, void **out)
{
	IXMLDOMNamedNodeMap *map = NULL;
	if (!out)
		return E_POINTER;
	return answer_map(IXMLDOMNode_get_attributes(dom(iface), &map), &map, out);
}

static HRESULT WINAPI node_has_children(void *iface, boolean *out)
{
	VARIANT_BOOL has = VARIANT_FALSE;
	if (!out)
		return E_POINTER;
	const HRESULT result = IXMLDOMNode_hasChildNodes(dom(iface), &has);
	*out = has != VARIANT_FALSE;
	return FAILED(result) ? result : S_OK;
}

static HRESULT WINAPI node_owner(void *iface, void **out)
{
	IXMLDOMDocument *document = NULL;
	if (!out)
		return E_POINTER;
	return answer_node(IXMLDOMNode_get_ownerDocument(dom(iface), &document), &document, OWN_FACE, out);
}

/* The MSXML node of a node argument, which must be one of ours. */
#define ARGUMENT(name, iface) \
	IXMLDOMNode *name = dom_of(iface); \
	if (!name) \
		return iface ? E_INVALIDARG : E_POINTER

static HRESULT WINAPI node_insert_before(void *iface, void *child, void *reference, void **out)
{
	VARIANT before;
	IXMLDOMNode *inserted = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	ARGUMENT(new_child, child);
	VariantInit(&before);
	if (reference) {
		if (!(before.punkVal = (IUnknown *)dom_of(reference)))
			return E_INVALIDARG;
		before.vt = VT_UNKNOWN;
	} else
		before.vt = VT_NULL;
	return answer_node(IXMLDOMNode_insertBefore(dom(iface), new_child, before, &inserted), &inserted, NODE_FACE, out);
}

static HRESULT WINAPI node_replace(void *iface, void *child, void *reference, void **out)
{
	IXMLDOMNode *replaced = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	ARGUMENT(new_child, child);
	ARGUMENT(old_child, reference);
	return answer_node(IXMLDOMNode_replaceChild(dom(iface), new_child, old_child, &replaced), &replaced, NODE_FACE, out);
}

static HRESULT WINAPI node_remove(void *iface, void *child, void **out)
{
	IXMLDOMNode *removed = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	ARGUMENT(old_child, child);
	return answer_node(IXMLDOMNode_removeChild(dom(iface), old_child, &removed), &removed, NODE_FACE, out);
}

static HRESULT WINAPI node_append(void *iface, void *child, void **out)
{
	IXMLDOMNode *appended = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	ARGUMENT(new_child, child);
	return answer_node(IXMLDOMNode_appendChild(dom(iface), new_child, &appended), &appended, NODE_FACE, out);
}

static HRESULT WINAPI node_clone(void *iface, boolean deep, void **out)
{
	IXMLDOMNode *copy = NULL;
	if (!out)
		return E_POINTER;
	return answer_node(IXMLDOMNode_cloneNode(dom(iface), deep ? VARIANT_TRUE : VARIANT_FALSE, &copy), &copy, NODE_FACE, out);
}

static HRESULT WINAPI node_namespace(void *iface, void **out)
{
	BSTR text = NULL;
	return out ? answer_boxed(IXMLDOMNode_get_namespaceURI(dom(iface), &text), &text, out) : E_POINTER;
}

/* The local name of an element or an attribute; other nodes have none. */
static HRESULT WINAPI node_local_name(void *iface, void **out)
{
	DOMNodeType type = NODE_INVALID;
	BSTR text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = IXMLDOMNode_get_nodeType(dom(iface), &type);
	if (FAILED(result) || (type != NODE_ELEMENT && type != NODE_ATTRIBUTE))
		return result;
	return answer_boxed(IXMLDOMNode_get_baseName(dom(iface), &text), &text, out);
}

static HRESULT WINAPI node_prefix(void *iface, void **out)
{
	BSTR text = NULL;
	return out ? answer_boxed(IXMLDOMNode_get_prefix(dom(iface), &text), &text, out) : E_POINTER;
}

/* Normalize: adjacent text under the node merged. MSXML normalizes an element and what is under it, so a document normalizes its
 * root. */
static HRESULT WINAPI node_normalize(void *iface)
{
	IXMLDOMElement *element = NULL;
	IXMLDOMDocument *document = NULL;
	HRESULT result = S_OK;
	if (SUCCEEDED(dom_as(iface, &IID_IXMLDOMDocument, &document))) {
		result = IXMLDOMDocument_get_documentElement(document, &element);
		IXMLDOMDocument_Release(document);
	} else if (FAILED(dom_as(iface, &IID_IXMLDOMElement, &element)))
		return S_OK;
	if (SUCCEEDED(result) && element)
		result = IXMLDOMElement_normalize(element);
	if (element)
		IXMLDOMElement_Release(element);
	return FAILED(result) ? result : S_OK;
}

/* MSXML's DOM gives a node its prefix when it is made and never changes it. */
static HRESULT WINAPI node_set_prefix(void *iface, IInspectable *value) { return missing("IXmlNode.put_Prefix"); }

/* IXmlNodeSelector: XPath, with the namespaces of the expression's prefixes given as xmlns declarations for the NS forms. */
static HRESULT select_with(void *iface, HSTRING xpath, IInspectable *namespaces, BOOL many, void **out)
{
	IXMLDOMDocument2 *document = NULL;
	IXMLDOMDocument *owner_document = NULL;
	VARIANT kept, given;
	BSTR expression = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	VariantInit(&kept);
	VariantInit(&given);
	HRESULT result = S_OK;
	if (namespaces) {
		if (FAILED(result = unbox(namespaces, &given.bstrVal)))
			return result;
		given.vt = VT_BSTR;
		if (FAILED(dom_as(iface, &IID_IXMLDOMDocument2, &document)) &&
			SUCCEEDED(result = IXMLDOMNode_get_ownerDocument(dom(iface), &owner_document)) && owner_document)
			result = IXMLDOMDocument_QueryInterface(owner_document, &IID_IXMLDOMDocument2, (void **)&document);
		if (owner_document)
			IXMLDOMDocument_Release(owner_document);
		if (SUCCEEDED(result) && document) {
			BSTR property = SysAllocString(L"SelectionNamespaces");
			IXMLDOMDocument2_getProperty(document, property, &kept);
			result = IXMLDOMDocument2_setProperty(document, property, given);
			SysFreeString(property);
		}
	}
	if (SUCCEEDED(result) && !(expression = bstr_of(xpath)))
		result = E_OUTOFMEMORY;
	if (SUCCEEDED(result)) {
		if (many) {
			IXMLDOMNodeList *list = NULL;
			result = answer_list(IXMLDOMNode_selectNodes(dom(iface), expression, &list), &list, out);
		} else {
			IXMLDOMNode *found = NULL;
			result = answer_node(IXMLDOMNode_selectSingleNode(dom(iface), expression, &found), &found, NODE_FACE, out);
		}
	}
	if (document) {
		BSTR property = SysAllocString(L"SelectionNamespaces");
		if (kept.vt == VT_BSTR)
			IXMLDOMDocument2_setProperty(document, property, kept);
		SysFreeString(property);
		IXMLDOMDocument2_Release(document);
	}
	SysFreeString(expression);
	VariantClear(&kept);
	VariantClear(&given);
	return result;
}

static HRESULT WINAPI select_one(void *iface, HSTRING xpath, void **out) { return select_with(iface, xpath, NULL, FALSE, out); }
static HRESULT WINAPI select_many(void *iface, HSTRING xpath, void **out) { return select_with(iface, xpath, NULL, TRUE, out); }

static HRESULT WINAPI select_one_ns(void *iface, HSTRING xpath, IInspectable *namespaces, void **out)
{
	return select_with(iface, xpath, namespaces, FALSE, out);
}

static HRESULT WINAPI select_many_ns(void *iface, HSTRING xpath, IInspectable *namespaces, void **out)
{
	return select_with(iface, xpath, namespaces, TRUE, out);
}

/* IXmlNodeSerializer. */
static HRESULT WINAPI node_xml(void *iface, HSTRING *out)
{
	BSTR text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	return answer_bstr(IXMLDOMNode_get_xml(dom(iface), &text), &text, out);
}

static HRESULT WINAPI node_text(void *iface, HSTRING *out)
{
	BSTR text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	return answer_bstr(IXMLDOMNode_get_text(dom(iface), &text), &text, out);
}

static HRESULT WINAPI node_set_text(void *iface, HSTRING value)
{
	BSTR text = bstr_of(value);
	if (!text && WindowsGetStringLen(value))
		return E_OUTOFMEMORY;
	const HRESULT result = IXMLDOMNode_put_text(dom(iface), text);
	SysFreeString(text);
	return result;
}

TABLE(node_table, NODE_FACE, INSPECTABLE, (const void *)node_value, (const void *)node_set_value, (const void *)node_type,
	(const void *)node_name, (const void *)node_parent, (const void *)node_children, (const void *)node_first, (const void *)node_last,
	(const void *)node_previous, (const void *)node_next, (const void *)node_attributes, (const void *)node_has_children,
	(const void *)node_owner, (const void *)node_insert_before, (const void *)node_replace, (const void *)node_remove,
	(const void *)node_append, (const void *)node_clone, (const void *)node_namespace, (const void *)node_local_name,
	(const void *)node_prefix, (const void *)node_normalize, (const void *)node_set_prefix);
TABLE(selector_table, SELECTOR_FACE, INSPECTABLE, (const void *)select_one, (const void *)select_many, (const void *)select_one_ns,
	(const void *)select_many_ns);
TABLE(serializer_table, SERIALIZER_FACE, INSPECTABLE, (const void *)node_xml, (const void *)node_text, (const void *)node_set_text);

/* IXmlElement. */
static HRESULT with_element(void *iface, IXMLDOMElement **out) { return dom_as(iface, &IID_IXMLDOMElement, out); }

static HRESULT WINAPI element_tag(void *iface, HSTRING *out)
{
	IXMLDOMElement *element;
	BSTR text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_element(iface, &element);
	if (FAILED(result))
		return result;
	result = answer_bstr(IXMLDOMElement_get_tagName(element, &text), &text, out);
	IXMLDOMElement_Release(element);
	return result;
}

/* The value of an attribute, the empty string when the element has none of the name. */
static HRESULT WINAPI element_attribute(void *iface, HSTRING name, HSTRING *out)
{
	IXMLDOMElement *element;
	VARIANT value;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_element(iface, &element);
	if (FAILED(result))
		return result;
	BSTR key = bstr_of(name);
	VariantInit(&value);
	result = IXMLDOMElement_getAttribute(element, key, &value);
	SysFreeString(key);
	IXMLDOMElement_Release(element);
	if (SUCCEEDED(result) && value.vt == VT_BSTR)
		return answer_bstr(S_OK, &value.bstrVal, out);
	VariantClear(&value);
	return FAILED(result) ? result : S_OK;
}

static HRESULT WINAPI element_set_attribute(void *iface, HSTRING name, HSTRING value)
{
	IXMLDOMElement *element;
	VARIANT text;
	HRESULT result = with_element(iface, &element);
	if (FAILED(result))
		return result;
	BSTR key = bstr_of(name);
	VariantInit(&text);
	text.vt = VT_BSTR;
	text.bstrVal = bstr_of(value);
	result = IXMLDOMElement_setAttribute(element, key, text);
	SysFreeString(key);
	VariantClear(&text);
	IXMLDOMElement_Release(element);
	return result;
}

static HRESULT WINAPI element_remove_attribute(void *iface, HSTRING name)
{
	IXMLDOMElement *element;
	HRESULT result = with_element(iface, &element);
	if (FAILED(result))
		return result;
	BSTR key = bstr_of(name);
	result = IXMLDOMElement_removeAttribute(element, key);
	SysFreeString(key);
	IXMLDOMElement_Release(element);
	return FAILED(result) ? result : S_OK;
}

static HRESULT WINAPI element_attribute_node(void *iface, HSTRING name, void **out)
{
	IXMLDOMElement *element;
	IXMLDOMAttribute *found = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_element(iface, &element);
	if (FAILED(result))
		return result;
	BSTR key = bstr_of(name);
	result = IXMLDOMElement_getAttributeNode(element, key, &found);
	SysFreeString(key);
	IXMLDOMElement_Release(element);
	return answer_node(result, &found, OWN_FACE, out);
}

/* setAttributeNode and removeAttributeNode, with an attribute of ours. */
static HRESULT change_attribute_node(void *iface, void *attribute, BOOL set, void **out)
{
	IXMLDOMElement *element;
	IXMLDOMAttribute *given = NULL, *answer = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	ARGUMENT(node, attribute);
	if (FAILED(IXMLDOMNode_QueryInterface(node, &IID_IXMLDOMAttribute, (void **)&given)))
		return E_INVALIDARG;
	HRESULT result = with_element(iface, &element);
	if (SUCCEEDED(result)) {
		result = set ? IXMLDOMElement_setAttributeNode(element, given, &answer) : IXMLDOMElement_removeAttributeNode(element, given, &answer);
		IXMLDOMElement_Release(element);
	}
	IXMLDOMAttribute_Release(given);
	return answer_node(result, &answer, OWN_FACE, out);
}

static HRESULT WINAPI element_set_attribute_node(void *iface, void *attribute, void **out)
{
	return change_attribute_node(iface, attribute, TRUE, out);
}

static HRESULT WINAPI element_remove_attribute_node(void *iface, void *attribute, void **out)
{
	return change_attribute_node(iface, attribute, FALSE, out);
}

static HRESULT WINAPI element_by_tag(void *iface, HSTRING name, void **out)
{
	IXMLDOMElement *element;
	IXMLDOMNodeList *list = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_element(iface, &element);
	if (FAILED(result))
		return result;
	BSTR key = bstr_of(name);
	result = IXMLDOMElement_getElementsByTagName(element, key, &list);
	SysFreeString(key);
	IXMLDOMElement_Release(element);
	return answer_list(result, &list, out);
}

/* The attribute of a namespace and a local name, through the element's attribute map. */
static HRESULT qualified_attribute(void *iface, IInspectable *namespace_uri, HSTRING name, IXMLDOMNode **out)
{
	IXMLDOMNamedNodeMap *map = NULL;
	BSTR uri = NULL;
	*out = NULL;
	HRESULT result = unbox(namespace_uri, &uri);
	if (SUCCEEDED(result))
		result = IXMLDOMNode_get_attributes(dom(iface), &map);
	if (SUCCEEDED(result) && map) {
		BSTR key = bstr_of(name);
		result = IXMLDOMNamedNodeMap_getQualifiedItem(map, key, uri, out);
		SysFreeString(key);
	}
	if (map)
		IXMLDOMNamedNodeMap_Release(map);
	SysFreeString(uri);
	return FAILED(result) ? result : S_OK;
}

/* A new attribute of the node's document, in a namespace. */
static HRESULT make_qualified(IXMLDOMNode *context, DOMNodeType type, IInspectable *namespace_uri, HSTRING name, IXMLDOMNode **out)
{
	IXMLDOMDocument *document = NULL;
	BSTR uri = NULL;
	VARIANT kind;
	*out = NULL;
	HRESULT result = unbox(namespace_uri, &uri);
	if (SUCCEEDED(result) && FAILED(IXMLDOMNode_QueryInterface(context, &IID_IXMLDOMDocument, (void **)&document)))
		result = IXMLDOMNode_get_ownerDocument(context, &document);
	if (SUCCEEDED(result) && document) {
		BSTR key = bstr_of(name);
		VariantInit(&kind);
		kind.vt = VT_I4;
		kind.lVal = type;
		result = IXMLDOMDocument_createNode(document, kind, key, uri ? uri : (BSTR)L"", out);
		SysFreeString(key);
	}
	if (document)
		IXMLDOMDocument_Release(document);
	SysFreeString(uri);
	return result;
}

static HRESULT WINAPI element_set_attribute_ns(void *iface, IInspectable *namespace_uri, HSTRING name, HSTRING value)
{
	IXMLDOMNode *attribute = NULL;
	IXMLDOMElement *element = NULL;
	IXMLDOMAttribute *as_attribute = NULL, *replaced = NULL;
	HRESULT result = make_qualified(dom(iface), NODE_ATTRIBUTE, namespace_uri, name, &attribute);
	if (SUCCEEDED(result)) {
		BSTR text = bstr_of(value);
		result = IXMLDOMNode_put_text(attribute, text);
		SysFreeString(text);
	}
	if (SUCCEEDED(result))
		result = IXMLDOMNode_QueryInterface(attribute, &IID_IXMLDOMAttribute, (void **)&as_attribute);
	if (SUCCEEDED(result))
		result = with_element(iface, &element);
	if (SUCCEEDED(result))
		result = IXMLDOMElement_setAttributeNode(element, as_attribute, &replaced);
	if (replaced)
		IXMLDOMAttribute_Release(replaced);
	if (element)
		IXMLDOMElement_Release(element);
	if (as_attribute)
		IXMLDOMAttribute_Release(as_attribute);
	if (attribute)
		IXMLDOMNode_Release(attribute);
	return FAILED(result) ? result : S_OK;
}

static HRESULT WINAPI element_attribute_ns(void *iface, IInspectable *namespace_uri, HSTRING name, HSTRING *out)
{
	IXMLDOMNode *attribute = NULL;
	BSTR text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = qualified_attribute(iface, namespace_uri, name, &attribute);
	if (FAILED(result) || !attribute)
		return result;
	result = answer_bstr(IXMLDOMNode_get_text(attribute, &text), &text, out);
	IXMLDOMNode_Release(attribute);
	return result;
}

static HRESULT WINAPI element_remove_attribute_ns(void *iface, IInspectable *namespace_uri, HSTRING name)
{
	IXMLDOMNamedNodeMap *map = NULL;
	IXMLDOMNode *removed = NULL;
	BSTR uri = NULL;
	HRESULT result = unbox(namespace_uri, &uri);
	if (SUCCEEDED(result))
		result = IXMLDOMNode_get_attributes(dom(iface), &map);
	if (SUCCEEDED(result) && map) {
		BSTR key = bstr_of(name);
		result = IXMLDOMNamedNodeMap_removeQualifiedItem(map, key, uri, &removed);
		SysFreeString(key);
		IXMLDOMNamedNodeMap_Release(map);
	}
	if (removed)
		IXMLDOMNode_Release(removed);
	SysFreeString(uri);
	return FAILED(result) ? result : S_OK;
}

static HRESULT WINAPI element_attribute_node_ns(void *iface, IInspectable *namespace_uri, HSTRING name, void **out)
{
	IXMLDOMNode *attribute = NULL;
	if (!out)
		return E_POINTER;
	return answer_node(qualified_attribute(iface, namespace_uri, name, &attribute), &attribute, OWN_FACE, out);
}

TABLE(element_table, OWN_FACE, INSPECTABLE, (const void *)element_tag, (const void *)element_attribute, (const void *)element_set_attribute,
	(const void *)element_remove_attribute, (const void *)element_attribute_node, (const void *)element_set_attribute_node,
	(const void *)element_remove_attribute_node, (const void *)element_by_tag, (const void *)element_set_attribute_ns,
	(const void *)element_attribute_ns, (const void *)element_remove_attribute_ns, (const void *)element_set_attribute_node,
	(const void *)element_attribute_node_ns);

/* IXmlAttribute. */
static HRESULT WINAPI attribute_name(void *iface, HSTRING *out) { return node_name(iface, out); }

static HRESULT WINAPI attribute_specified(void *iface, boolean *out)
{
	VARIANT_BOOL specified = VARIANT_TRUE;
	if (!out)
		return E_POINTER;
	const HRESULT result = IXMLDOMNode_get_specified(dom(iface), &specified);
	*out = specified != VARIANT_FALSE;
	return FAILED(result) ? result : S_OK;
}

static HRESULT WINAPI attribute_value(void *iface, HSTRING *out) { return node_text(iface, out); }
static HRESULT WINAPI attribute_set_value(void *iface, HSTRING value) { return node_set_text(iface, value); }

TABLE(attribute_table, OWN_FACE, INSPECTABLE, (const void *)attribute_name, (const void *)attribute_specified,
	(const void *)attribute_value, (const void *)attribute_set_value);

/* IXmlCharacterData, of text, CDATA sections and comments. */
static HRESULT with_data(void *iface, IXMLDOMCharacterData **out) { return dom_as(iface, &IID_IXMLDOMCharacterData, out); }

static HRESULT WINAPI data_get(void *iface, HSTRING *out)
{
	IXMLDOMCharacterData *data;
	BSTR text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_data(iface, &data);
	if (FAILED(result))
		return result;
	result = answer_bstr(IXMLDOMCharacterData_get_data(data, &text), &text, out);
	IXMLDOMCharacterData_Release(data);
	return result;
}

static HRESULT WINAPI data_set(void *iface, HSTRING value)
{
	IXMLDOMCharacterData *data;
	HRESULT result = with_data(iface, &data);
	if (FAILED(result))
		return result;
	BSTR text = bstr_of(value);
	result = IXMLDOMCharacterData_put_data(data, text);
	SysFreeString(text);
	IXMLDOMCharacterData_Release(data);
	return result;
}

static HRESULT WINAPI data_length(void *iface, UINT32 *out)
{
	IXMLDOMCharacterData *data;
	LONG length = 0;
	if (!out)
		return E_POINTER;
	HRESULT result = with_data(iface, &data);
	if (FAILED(result))
		return result;
	result = IXMLDOMCharacterData_get_length(data, &length);
	IXMLDOMCharacterData_Release(data);
	*out = (UINT32)length;
	return result;
}

static HRESULT WINAPI data_substring(void *iface, UINT32 offset, UINT32 count, HSTRING *out)
{
	IXMLDOMCharacterData *data;
	BSTR text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_data(iface, &data);
	if (FAILED(result))
		return result;
	result = answer_bstr(IXMLDOMCharacterData_substringData(data, (LONG)offset, (LONG)count, &text), &text, out);
	IXMLDOMCharacterData_Release(data);
	return result;
}

static HRESULT WINAPI data_append(void *iface, HSTRING value)
{
	IXMLDOMCharacterData *data;
	HRESULT result = with_data(iface, &data);
	if (FAILED(result))
		return result;
	BSTR text = bstr_of(value);
	result = IXMLDOMCharacterData_appendData(data, text);
	SysFreeString(text);
	IXMLDOMCharacterData_Release(data);
	return result;
}

static HRESULT WINAPI data_insert(void *iface, UINT32 offset, HSTRING value)
{
	IXMLDOMCharacterData *data;
	HRESULT result = with_data(iface, &data);
	if (FAILED(result))
		return result;
	BSTR text = bstr_of(value);
	result = IXMLDOMCharacterData_insertData(data, (LONG)offset, text);
	SysFreeString(text);
	IXMLDOMCharacterData_Release(data);
	return result;
}

static HRESULT WINAPI data_delete(void *iface, UINT32 offset, UINT32 count)
{
	IXMLDOMCharacterData *data;
	HRESULT result = with_data(iface, &data);
	if (FAILED(result))
		return result;
	result = IXMLDOMCharacterData_deleteData(data, (LONG)offset, (LONG)count);
	IXMLDOMCharacterData_Release(data);
	return result;
}

static HRESULT WINAPI data_replace(void *iface, UINT32 offset, UINT32 count, HSTRING value)
{
	IXMLDOMCharacterData *data;
	HRESULT result = with_data(iface, &data);
	if (FAILED(result))
		return result;
	BSTR text = bstr_of(value);
	result = IXMLDOMCharacterData_replaceData(data, (LONG)offset, (LONG)count, text);
	SysFreeString(text);
	IXMLDOMCharacterData_Release(data);
	return result;
}

#define DATA_TABLE(name, place) \
	TABLE(name, place, INSPECTABLE, (const void *)data_get, (const void *)data_set, (const void *)data_length, (const void *)data_substring, \
		(const void *)data_append, (const void *)data_insert, (const void *)data_delete, (const void *)data_replace)
DATA_TABLE(text_data_table, OWN_FACE + 1);
DATA_TABLE(cdata_data_table, OWN_FACE + 2);
DATA_TABLE(comment_data_table, OWN_FACE + 1);

/* IXmlText: the second part of a split is text of the same type as the first, a CDATA section after a CDATA section. */
static size_t text_face(IXMLDOMNode *node)
{
	DOMNodeType type = NODE_TEXT;
	IXMLDOMNode_get_nodeType(node, &type);
	return type == NODE_CDATA_SECTION ? OWN_FACE + 1 : OWN_FACE;
}

static HRESULT WINAPI text_split(void *iface, UINT32 offset, void **out)
{
	IXMLDOMText *text, *rest = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = dom_as(iface, &IID_IXMLDOMText, &text);
	if (FAILED(result))
		return result;
	result = IXMLDOMText_splitText(text, (LONG)offset, &rest);
	IXMLDOMText_Release(text);
	return answer_node(result, &rest, rest ? text_face((IXMLDOMNode *)rest) : OWN_FACE, out);
}

TABLE(text_table, OWN_FACE, INSPECTABLE, (const void *)text_split);
TABLE(cdata_text_table, OWN_FACE + 1, INSPECTABLE, (const void *)text_split);
/* IXmlCDataSection, IXmlComment, IXmlEntityReference and IXmlDocumentFragment have no members of their own. */
TABLE(cdata_table, OWN_FACE, INSPECTABLE);
TABLE(comment_table, OWN_FACE, INSPECTABLE);
TABLE(entity_reference_table, OWN_FACE, INSPECTABLE);
TABLE(fragment_table, OWN_FACE, INSPECTABLE);

/* IXmlProcessingInstruction. */
static HRESULT WINAPI instruction_target(void *iface, HSTRING *out)
{
	IXMLDOMProcessingInstruction *instruction;
	BSTR text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = dom_as(iface, &IID_IXMLDOMProcessingInstruction, &instruction);
	if (FAILED(result))
		return result;
	result = answer_bstr(IXMLDOMProcessingInstruction_get_target(instruction, &text), &text, out);
	IXMLDOMProcessingInstruction_Release(instruction);
	return result;
}

static HRESULT WINAPI instruction_data(void *iface, HSTRING *out)
{
	IXMLDOMProcessingInstruction *instruction;
	BSTR text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = dom_as(iface, &IID_IXMLDOMProcessingInstruction, &instruction);
	if (FAILED(result))
		return result;
	result = answer_bstr(IXMLDOMProcessingInstruction_get_data(instruction, &text), &text, out);
	IXMLDOMProcessingInstruction_Release(instruction);
	return result;
}

static HRESULT WINAPI instruction_set_data(void *iface, HSTRING value)
{
	IXMLDOMProcessingInstruction *instruction;
	HRESULT result = dom_as(iface, &IID_IXMLDOMProcessingInstruction, &instruction);
	if (FAILED(result))
		return result;
	BSTR text = bstr_of(value);
	result = IXMLDOMProcessingInstruction_put_data(instruction, text);
	SysFreeString(text);
	IXMLDOMProcessingInstruction_Release(instruction);
	return result;
}

TABLE(instruction_table, OWN_FACE, INSPECTABLE, (const void *)instruction_target, (const void *)instruction_data,
	(const void *)instruction_set_data);

/* IXmlDocumentType, IDtdEntity and IDtdNotation. */
static HRESULT WINAPI doctype_name(void *iface, HSTRING *out) { return node_name(iface, out); }

static HRESULT doctype_map(void *iface, BOOL entities, void **out)
{
	IXMLDOMDocumentType *doctype;
	IXMLDOMNamedNodeMap *map = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = dom_as(iface, &IID_IXMLDOMDocumentType, &doctype);
	if (FAILED(result))
		return result;
	result = entities ? IXMLDOMDocumentType_get_entities(doctype, &map) : IXMLDOMDocumentType_get_notations(doctype, &map);
	IXMLDOMDocumentType_Release(doctype);
	return answer_map(result, &map, out);
}

static HRESULT WINAPI doctype_entities(void *iface, void **out) { return doctype_map(iface, TRUE, out); }
static HRESULT WINAPI doctype_notations(void *iface, void **out) { return doctype_map(iface, FALSE, out); }

TABLE(doctype_table, OWN_FACE, INSPECTABLE, (const void *)doctype_name, (const void *)doctype_entities, (const void *)doctype_notations);

enum { PUBLIC_ID, SYSTEM_ID, NOTATION_NAME };

static HRESULT dtd_text(void *iface, int which, void **out)
{
	IXMLDOMEntity *entity = NULL;
	IXMLDOMNotation *notation = NULL;
	VARIANT value;
	if (!out)
		return E_POINTER;
	*out = NULL;
	VariantInit(&value);
	if (SUCCEEDED(dom_as(iface, &IID_IXMLDOMEntity, &entity))) {
		HRESULT result;
		if (which == NOTATION_NAME) {
			BSTR text = NULL;
			result = answer_boxed(IXMLDOMEntity_get_notationName(entity, &text), &text, out);
		} else
			result = answer_variant_boxed(which == PUBLIC_ID ? IXMLDOMEntity_get_publicId(entity, &value) :
				IXMLDOMEntity_get_systemId(entity, &value), &value, out);
		IXMLDOMEntity_Release(entity);
		return result;
	}
	HRESULT result = dom_as(iface, &IID_IXMLDOMNotation, &notation);
	if (FAILED(result))
		return result;
	result = answer_variant_boxed(which == PUBLIC_ID ? IXMLDOMNotation_get_publicId(notation, &value) :
		IXMLDOMNotation_get_systemId(notation, &value), &value, out);
	IXMLDOMNotation_Release(notation);
	return result;
}

static HRESULT WINAPI dtd_public_id(void *iface, void **out) { return dtd_text(iface, PUBLIC_ID, out); }
static HRESULT WINAPI dtd_system_id(void *iface, void **out) { return dtd_text(iface, SYSTEM_ID, out); }
static HRESULT WINAPI dtd_notation_name(void *iface, void **out) { return dtd_text(iface, NOTATION_NAME, out); }

TABLE(dtd_entity_table, OWN_FACE, INSPECTABLE, (const void *)dtd_public_id, (const void *)dtd_system_id, (const void *)dtd_notation_name);
TABLE(dtd_notation_table, OWN_FACE, INSPECTABLE, (const void *)dtd_public_id, (const void *)dtd_system_id);

/* IXmlDocument. */
static HRESULT with_document(void *iface, IXMLDOMDocument3 **out) { return dom_as(iface, &IID_IXMLDOMDocument3, out); }

#define DOCUMENT_GETTER(function, getter, type) \
	static HRESULT WINAPI function(void *iface, void **out) \
	{ \
		IXMLDOMDocument3 *document; \
		type *found = NULL; \
		if (!out) \
			return E_POINTER; \
		*out = NULL; \
		HRESULT result = with_document(iface, &document); \
		if (FAILED(result)) \
			return result; \
		result = getter(document, &found); \
		IXMLDOMDocument3_Release(document); \
		return answer_node(result, &found, OWN_FACE, out); \
	}

DOCUMENT_GETTER(document_doctype, IXMLDOMDocument3_get_doctype, IXMLDOMDocumentType)
DOCUMENT_GETTER(document_element, IXMLDOMDocument3_get_documentElement, IXMLDOMElement)
DOCUMENT_GETTER(document_fragment, IXMLDOMDocument3_createDocumentFragment, IXMLDOMDocumentFragment)

static HRESULT make_implementation(IXMLDOMImplementation *implementation, void **out);

static HRESULT WINAPI document_implementation(void *iface, void **out)
{
	IXMLDOMDocument3 *document;
	IXMLDOMImplementation *implementation = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_document(iface, &document);
	if (FAILED(result))
		return result;
	result = IXMLDOMDocument3_get_implementation(document, &implementation);
	IXMLDOMDocument3_Release(document);
	if (SUCCEEDED(result) && implementation)
		result = make_implementation(implementation, out);
	if (implementation)
		IXMLDOMImplementation_Release(implementation);
	return result;
}

/* The Create methods that take one string: the new node MSXML makes of it, through its class's default interface. */
#define DOCUMENT_CREATE(function, creator, type) \
	static HRESULT WINAPI function(void *iface, HSTRING value, void **out) \
	{ \
		IXMLDOMDocument3 *document; \
		type *made = NULL; \
		if (!out) \
			return E_POINTER; \
		*out = NULL; \
		HRESULT result = with_document(iface, &document); \
		if (FAILED(result)) \
			return result; \
		BSTR text = bstr_of(value); \
		result = creator(document, text, &made); \
		SysFreeString(text); \
		IXMLDOMDocument3_Release(document); \
		return answer_node(result, &made, OWN_FACE, out); \
	}

DOCUMENT_CREATE(document_create_element, IXMLDOMDocument3_createElement, IXMLDOMElement)
DOCUMENT_CREATE(document_create_text, IXMLDOMDocument3_createTextNode, IXMLDOMText)
DOCUMENT_CREATE(document_create_comment, IXMLDOMDocument3_createComment, IXMLDOMComment)
DOCUMENT_CREATE(document_create_attribute, IXMLDOMDocument3_createAttribute, IXMLDOMAttribute)
DOCUMENT_CREATE(document_create_reference, IXMLDOMDocument3_createEntityReference, IXMLDOMEntityReference)
DOCUMENT_CREATE(document_create_cdata, IXMLDOMDocument3_createCDATASection, IXMLDOMCDATASection)

static HRESULT WINAPI document_create_instruction(void *iface, HSTRING target, HSTRING data, void **out)
{
	IXMLDOMDocument3 *document;
	IXMLDOMProcessingInstruction *made = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_document(iface, &document);
	if (FAILED(result))
		return result;
	BSTR name = bstr_of(target), text = bstr_of(data);
	result = IXMLDOMDocument3_createProcessingInstruction(document, name, text, &made);
	SysFreeString(name);
	SysFreeString(text);
	IXMLDOMDocument3_Release(document);
	return answer_node(result, &made, OWN_FACE, out);
}

static HRESULT WINAPI document_by_tag(void *iface, HSTRING name, void **out)
{
	IXMLDOMDocument3 *document;
	IXMLDOMNodeList *list = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_document(iface, &document);
	if (FAILED(result))
		return result;
	BSTR key = bstr_of(name);
	result = IXMLDOMDocument3_getElementsByTagName(document, key, &list);
	SysFreeString(key);
	IXMLDOMDocument3_Release(document);
	return answer_list(result, &list, out);
}

/* The URL the document was loaded from, empty for one loaded from text. */
static HRESULT WINAPI document_uri(void *iface, HSTRING *out)
{
	IXMLDOMDocument3 *document;
	BSTR url = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_document(iface, &document);
	if (FAILED(result))
		return result;
	result = answer_bstr(IXMLDOMDocument3_get_url(document, &url), &url, out);
	IXMLDOMDocument3_Release(document);
	return result;
}

static HRESULT WINAPI document_create_attribute_ns(void *iface, IInspectable *namespace_uri, HSTRING name, void **out)
{
	IXMLDOMNode *made = NULL;
	if (!out)
		return E_POINTER;
	return answer_node(make_qualified(dom(iface), NODE_ATTRIBUTE, namespace_uri, name, &made), &made, OWN_FACE, out);
}

static HRESULT WINAPI document_create_element_ns(void *iface, IInspectable *namespace_uri, HSTRING name, void **out)
{
	IXMLDOMNode *made = NULL;
	if (!out)
		return E_POINTER;
	return answer_node(make_qualified(dom(iface), NODE_ELEMENT, namespace_uri, name, &made), &made, OWN_FACE, out);
}

static HRESULT WINAPI document_by_id(void *iface, HSTRING id, void **out)
{
	IXMLDOMDocument3 *document;
	IXMLDOMNode *found = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = with_document(iface, &document);
	if (FAILED(result))
		return result;
	BSTR key = bstr_of(id);
	result = IXMLDOMDocument3_nodeFromID(document, key, &found);
	SysFreeString(key);
	IXMLDOMDocument3_Release(document);
	return answer_node(result, &found, OWN_FACE, out);
}

static HRESULT WINAPI document_import(void *iface, void *node, boolean deep, void **out)
{
	IXMLDOMDocument3 *document;
	IXMLDOMNode *imported = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	ARGUMENT(source, node);
	HRESULT result = with_document(iface, &document);
	if (FAILED(result))
		return result;
	result = IXMLDOMDocument3_importNode(document, source, deep ? VARIANT_TRUE : VARIANT_FALSE, &imported);
	IXMLDOMDocument3_Release(document);
	return answer_node(result, &imported, NODE_FACE, out);
}

TABLE(document_table, OWN_FACE, INSPECTABLE, (const void *)document_doctype, (const void *)document_implementation,
	(const void *)document_element, (const void *)document_create_element, (const void *)document_fragment,
	(const void *)document_create_text, (const void *)document_create_comment, (const void *)document_create_instruction,
	(const void *)document_create_attribute, (const void *)document_create_reference, (const void *)document_by_tag,
	(const void *)document_create_cdata, (const void *)document_uri, (const void *)document_create_attribute_ns,
	(const void *)document_create_element_ns, (const void *)document_by_id, (const void *)document_import);

/* Loading: the settings a load takes, applied to the MSXML document before it parses. */
static const struct settings default_settings = {.depth = 256, .prohibit_dtd = TRUE};

static struct settings *settings_of(void *iface);

static HRESULT set_property(IXMLDOMDocument3 *document, const WCHAR *name, VARIANT value)
{
	BSTR key = SysAllocString(name);
	const HRESULT result = IXMLDOMDocument3_setProperty(document, key, value);
	SysFreeString(key);
	return result;
}

static HRESULT apply_settings(IXMLDOMDocument3 *document, const struct settings *settings)
{
	VARIANT value;
	VariantInit(&value);
	value.vt = VT_BOOL;
	value.boolVal = settings->prohibit_dtd ? VARIANT_TRUE : VARIANT_FALSE;
	HRESULT result = set_property(document, L"ProhibitDTD", value);
	value.vt = VT_I4;
	value.lVal = (LONG)settings->depth;
	if (SUCCEEDED(result))
		result = set_property(document, L"MaxElementDepth", value);
	if (SUCCEEDED(result))
		result = IXMLDOMDocument3_put_resolveExternals(document, settings->resolve_externals ? VARIANT_TRUE : VARIANT_FALSE);
	if (SUCCEEDED(result))
		result = IXMLDOMDocument3_put_validateOnParse(document, settings->validate ? VARIANT_TRUE : VARIANT_FALSE);
	if (SUCCEEDED(result))
		result = IXMLDOMDocument3_put_preserveWhiteSpace(document, settings->white_space ? VARIANT_TRUE : VARIANT_FALSE);
	return result;
}

/* A load that MSXML refused: the code of its parse error, which is what the Windows Runtime reports. */
static HRESULT load_failure(IXMLDOMDocument3 *document, HRESULT result, VARIANT_BOOL loaded)
{
	IXMLDOMParseError *error = NULL;
	LONG code = 0;
	if (FAILED(result) || loaded != VARIANT_FALSE)
		return FAILED(result) ? result : S_OK;
	if (SUCCEEDED(IXMLDOMDocument3_get_parseError(document, &error)) && error) {
		IXMLDOMParseError_get_errorCode(error, &code);
		IXMLDOMParseError_Release(error);
	}
	return code < 0 ? (HRESULT)code : E_FAIL;
}

static HRESULT load_text(void *iface, HSTRING xml, void *settings_iface)
{
	IXMLDOMDocument3 *document;
	VARIANT_BOOL loaded = VARIANT_FALSE;
	HRESULT result = with_document(iface, &document);
	if (FAILED(result))
		return result;
	result = apply_settings(document, settings_iface ? settings_of(settings_iface) : &default_settings);
	if (SUCCEEDED(result)) {
		BSTR text = bstr_of(xml);
		result = IXMLDOMDocument3_loadXML(document, text, &loaded);
		result = load_failure(document, result, loaded);
		SysFreeString(text);
	}
	IXMLDOMDocument3_Release(document);
	return result;
}

/* Loads what a VARIANT names: a path or a URL, or bytes in an array. */
static HRESULT load_source(IXMLDOMDocument3 *document, VARIANT source)
{
	VARIANT_BOOL loaded = VARIANT_FALSE;
	const HRESULT result = IXMLDOMDocument3_load(document, source, &loaded);
	return load_failure(document, result, loaded);
}

static HRESULT load_bytes(void *iface, __x_ABI_CWindows_CStorage_CStreams_CIBuffer *buffer, void *settings_iface)
{
	IXMLDOMDocument3 *document;
	BYTE *bytes;
	UINT32 length;
	VARIANT source;
	if (!buffer)
		return E_INVALIDARG;
	HRESULT result = winrt_buffer_bytes(buffer, &bytes, &length);
	if (FAILED(result))
		return result;
	SAFEARRAY *array = SafeArrayCreateVector(VT_UI1, 0, length);
	if (!array)
		return E_OUTOFMEMORY;
	CopyMemory(array->pvData, bytes, length);
	VariantInit(&source);
	source.vt = VT_ARRAY | VT_UI1;
	source.parray = array;
	if (SUCCEEDED(result = with_document(iface, &document))) {
		result = apply_settings(document, settings_iface ? settings_of(settings_iface) : &default_settings);
		if (SUCCEEDED(result))
			result = load_source(document, source);
		IXMLDOMDocument3_Release(document);
	}
	VariantClear(&source);
	return result;
}

static HRESULT WINAPI document_load_xml(void *iface, HSTRING xml) { return load_text(iface, xml, NULL); }

static HRESULT WINAPI document_load_xml_with(void *iface, HSTRING xml, void *settings)
{
	return settings ? load_text(iface, xml, settings) : E_INVALIDARG;
}

/* SaveToFileAsync: the document written to the file's path on a thread of its own. */
struct save {
	IXMLDOMDocument3 *document;
	WCHAR path[MAX_PATH];
};

static void free_save(void *context)
{
	struct save *save = context;
	IXMLDOMDocument3_Release(save->document);
	FREE(save);
}

static HRESULT save_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct save *save = context;
	VARIANT destination;
	VariantInit(&destination);
	destination.vt = VT_BSTR;
	if (!(destination.bstrVal = SysAllocString(save->path)))
		return E_OUTOFMEMORY;
	const HRESULT status = IXMLDOMDocument3_save(save->document, destination);
	VariantClear(&destination);
	return status;
}

static HRESULT WINAPI document_save(void *iface, void *file, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!file)
		return E_INVALIDARG;
	struct save *save = ALLOC(sizeof *save);
	if (!save)
		return E_OUTOFMEMORY;
	HRESULT result = path_of_any(file, save->path);
	if (SUCCEEDED(result))
		result = with_document(iface, &save->document);
	if (FAILED(result)) {
		FREE(save);
		return result;
	}
	return winrt_action_start(save_work, save, free_save, out);
}

TABLE(document_io_table, OWN_FACE + 1, INSPECTABLE, (const void *)document_load_xml, (const void *)document_load_xml_with,
	(const void *)document_save);

static HRESULT WINAPI document_load_buffer(void *iface, void *buffer) { return load_bytes(iface, buffer, NULL); }

static HRESULT WINAPI document_load_buffer_with(void *iface, void *buffer, void *settings)
{
	return settings ? load_bytes(iface, buffer, settings) : E_INVALIDARG;
}

TABLE(document_io2_table, OWN_FACE + 2, INSPECTABLE, (const void *)document_load_buffer, (const void *)document_load_buffer_with);

/* The kinds of the nodes, one for each DOMNodeType from 1 to 12: the class and the interfaces of its type after the three of every node. */
struct node_kind {
	struct kind kind;
	struct face_iid iids[FACES + 1];
	const void *const *tables[FACES];
	size_t count;
};

#define NODE_KIND(name, ...) \
	{{name, L"Windows.Data.Xml.Dom." name, NULL, destroy_node}, {{&iid_node, 0}, {&iid_selector, 1}, {&iid_serializer, 2}}, \
		{node_table.slots, selector_table.slots, serializer_table.slots}, 0}
static struct node_kind node_kinds[NODE_NOTATION + 1] = {
	[NODE_ELEMENT] = NODE_KIND("XmlElement"),
	[NODE_ATTRIBUTE] = NODE_KIND("XmlAttribute"),
	[NODE_TEXT] = NODE_KIND("XmlText"),
	[NODE_CDATA_SECTION] = NODE_KIND("XmlCDataSection"),
	[NODE_ENTITY_REFERENCE] = NODE_KIND("XmlEntityReference"),
	[NODE_ENTITY] = NODE_KIND("DtdEntity"),
	[NODE_PROCESSING_INSTRUCTION] = NODE_KIND("XmlProcessingInstruction"),
	[NODE_COMMENT] = NODE_KIND("XmlComment"),
	[NODE_DOCUMENT] = NODE_KIND("XmlDocument"),
	[NODE_DOCUMENT_TYPE] = NODE_KIND("XmlDocumentType"),
	[NODE_DOCUMENT_FRAGMENT] = NODE_KIND("XmlDocumentFragment"),
	[NODE_NOTATION] = NODE_KIND("DtdNotation"),
};
static INIT_ONCE kinds_once = INIT_ONCE_STATIC_INIT;

/* Adds the interfaces of a type, in place order from OWN_FACE. */
static void own_faces(DOMNodeType type, const IID *const *iids, const void *const *const *tables, size_t count)
{
	struct node_kind *entry = &node_kinds[type];
	for (size_t at = 0; at < count; ++at) {
		entry->iids[OWN_FACE + at] = (struct face_iid){iids[at], OWN_FACE + at};
		entry->tables[OWN_FACE + at] = tables[at];
	}
	entry->iids[OWN_FACE + count] = (struct face_iid){NULL, 0};
	entry->count = OWN_FACE + count;
	entry->kind.iids = entry->iids;
}

#define OWN(type, ...) \
	own_faces(type, (const IID *const[]){__VA_ARGS__}, tables, sizeof((const IID *const[]){__VA_ARGS__}) / sizeof(IID *))

static BOOL CALLBACK make_kinds(INIT_ONCE *once, void *parameter, void **context)
{
	{
		const void *const *const tables[] = {element_table.slots};
		OWN(NODE_ELEMENT, &iid_element);
	}
	{
		const void *const *const tables[] = {attribute_table.slots};
		OWN(NODE_ATTRIBUTE, &iid_attribute);
	}
	{
		const void *const *const tables[] = {text_table.slots, text_data_table.slots};
		OWN(NODE_TEXT, &iid_text, &iid_character_data);
	}
	{
		const void *const *const tables[] = {cdata_table.slots, cdata_text_table.slots, cdata_data_table.slots};
		OWN(NODE_CDATA_SECTION, &iid_cdata, &iid_text, &iid_character_data);
	}
	{
		const void *const *const tables[] = {entity_reference_table.slots};
		OWN(NODE_ENTITY_REFERENCE, &iid_entity_reference);
	}
	{
		const void *const *const tables[] = {dtd_entity_table.slots};
		OWN(NODE_ENTITY, &iid_dtd_entity);
	}
	{
		const void *const *const tables[] = {instruction_table.slots};
		OWN(NODE_PROCESSING_INSTRUCTION, &iid_instruction);
	}
	{
		const void *const *const tables[] = {comment_table.slots, comment_data_table.slots};
		OWN(NODE_COMMENT, &iid_comment, &iid_character_data);
	}
	{
		const void *const *const tables[] = {document_table.slots, document_io_table.slots, document_io2_table.slots};
		OWN(NODE_DOCUMENT, &iid_document, &iid_document_io, &iid_document_io2);
	}
	{
		const void *const *const tables[] = {doctype_table.slots};
		OWN(NODE_DOCUMENT_TYPE, &iid_document_type);
	}
	{
		const void *const *const tables[] = {fragment_table.slots};
		OWN(NODE_DOCUMENT_FRAGMENT, &iid_fragment);
	}
	{
		const void *const *const tables[] = {dtd_notation_table.slots};
		OWN(NODE_NOTATION, &iid_dtd_notation);
	}
	return TRUE;
}

/* A node of ours for one of MSXML's, through one of its faces; the MSXML node gets a reference of its own. */
static HRESULT wrap(IUnknown *dom_node, size_t face, void **out)
{
	IXMLDOMNode *node = NULL;
	DOMNodeType type = NODE_INVALID;
	*out = NULL;
	InitOnceExecuteOnce(&kinds_once, make_kinds, NULL, NULL);
	HRESULT result = dom_node->lpVtbl->QueryInterface(dom_node, &IID_IXMLDOMNode, (void **)&node);
	if (FAILED(result))
		return result;
	if (FAILED(result = IXMLDOMNode_get_nodeType(node, &type)) || type < NODE_ELEMENT || type > NODE_NOTATION) {
		IXMLDOMNode_Release(node);
		return FAILED(result) ? result : E_UNEXPECTED;
	}
	const struct node_kind *kind = &node_kinds[type];
	struct node *self = make(sizeof *self, &kind->kind, kind->tables, kind->count);
	if (!self) {
		IXMLDOMNode_Release(node);
		return E_OUTOFMEMORY;
	}
	self->node = node;
	*out = face_of(&self->base, face < kind->count ? face : NODE_FACE);
	return S_OK;
}

/* XmlNodeList and XmlNamedNodeMap: IXmlNodeList or IXmlNamedNodeMap at place 0, then IVectorView<IXmlNode> and IIterable<IXmlNode>. */
static struct collection *collection_of(void *iface) { return (struct collection *)owner(iface); }

static HRESULT collection_length(struct collection *self, UINT32 *out)
{
	LONG length = 0;
	const HRESULT result = self->list ? IXMLDOMNodeList_get_length(self->list, &length) : IXMLDOMNamedNodeMap_get_length(self->map, &length);
	*out = SUCCEEDED(result) && length > 0 ? (UINT32)length : 0;
	return result;
}

/* The node at an index, with a reference; NULL past the end. */
static HRESULT collection_item(struct collection *self, UINT32 index, IXMLDOMNode **out)
{
	*out = NULL;
	const HRESULT result = self->list ? IXMLDOMNodeList_get_item(self->list, (LONG)index, out) :
		IXMLDOMNamedNodeMap_get_item(self->map, (LONG)index, out);
	return FAILED(result) ? result : S_OK;
}

static HRESULT WINAPI collection_get_length(void *iface, UINT32 *out) { return out ? collection_length(collection_of(iface), out) : E_POINTER; }

static HRESULT WINAPI collection_get_item(void *iface, UINT32 index, void **out)
{
	IXMLDOMNode *found = NULL;
	if (!out)
		return E_POINTER;
	return answer_node(collection_item(collection_of(iface), index, &found), &found, NODE_FACE, out);
}

static HRESULT WINAPI view_at(void *iface, UINT32 index, void **out)
{
	UINT32 length = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = collection_length(collection_of(iface), &length);
	if (SUCCEEDED(result) && index >= length)
		result = E_BOUNDS;
	return SUCCEEDED(result) ? collection_get_item(iface, index, out) : result;
}

static HRESULT WINAPI view_size(void *iface, UINT32 *out) { return collection_get_length(iface, out); }

/* Nodes are the same when MSXML's nodes are the same object. */
static HRESULT WINAPI view_index(void *iface, void *value, UINT32 *index, boolean *found)
{
	struct collection *self = collection_of(iface);
	IUnknown *wanted = NULL;
	UINT32 length = 0;
	if (!index || !found)
		return E_POINTER;
	*index = 0;
	*found = FALSE;
	IXMLDOMNode *node = dom_of(value);
	if (!node || FAILED(IXMLDOMNode_QueryInterface(node, &IID_IUnknown, (void **)&wanted)))
		return S_OK;
	HRESULT result = collection_length(self, &length);
	for (UINT32 at = 0; SUCCEEDED(result) && at < length && !*found; ++at) {
		IXMLDOMNode *item = NULL;
		IUnknown *identity = NULL;
		if (SUCCEEDED(result = collection_item(self, at, &item)) && item &&
			SUCCEEDED(IXMLDOMNode_QueryInterface(item, &IID_IUnknown, (void **)&identity))) {
			if (identity == wanted) {
				*index = at;
				*found = TRUE;
			}
			identity->lpVtbl->Release(identity);
		}
		if (item)
			IXMLDOMNode_Release(item);
	}
	wanted->lpVtbl->Release(wanted);
	return FAILED(result) ? result : S_OK;
}

static HRESULT WINAPI view_many(void *iface, UINT32 start, UINT32 capacity, void **items, UINT32 *count)
{
	UINT32 length = 0;
	if (!count || (capacity && !items))
		return E_POINTER;
	*count = 0;
	HRESULT result = collection_length(collection_of(iface), &length);
	if (SUCCEEDED(result) && start > length)
		result = E_BOUNDS;
	for (UINT32 at = start; SUCCEEDED(result) && at < length && *count < capacity; ++at)
		if (SUCCEEDED(result = collection_get_item(iface, at, &items[*count])))
			++*count;
	if (FAILED(result)) {
		for (UINT32 at = 0; at < *count; ++at)
			release(items[at]);
		*count = 0;
	}
	return result;
}

/* First: an iterator over the nodes as they are now. */
static HRESULT WINAPI view_first(void *iface, void **out)
{
	UINT32 length = 0, count = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = collection_length(collection_of(iface), &length);
	void **items = SUCCEEDED(result) && length ? ALLOC(length * sizeof *items) : NULL;
	if (SUCCEEDED(result) && length && !items)
		result = E_OUTOFMEMORY;
	if (SUCCEEDED(result))
		result = view_many(iface, 0, length, items, &count);
	if (SUCCEEDED(result))
		result = winrt_items(&node_items, (IInspectable **)items, count, TRUE, out);
	for (UINT32 at = 0; at < count; ++at)
		release(items[at]);
	if (items)
		FREE(items);
	return result;
}

TABLE(list_table, 0, INSPECTABLE, (const void *)collection_get_length, (const void *)collection_get_item);
TABLE(view_table, 1, INSPECTABLE, (const void *)view_at, (const void *)view_size, (const void *)view_index, (const void *)view_many);
TABLE(iterable_table, 2, INSPECTABLE, (const void *)view_first);
static const struct face_iid list_iids[] = {{&iid_node_list, 0}, {&iid_node_view, 1}, {&iid_node_iterable, 2}, {NULL, 0}};
static const struct kind list_kind = {"XmlNodeList", L"Windows.Data.Xml.Dom.XmlNodeList", list_iids, destroy_collection};

/* IXmlNamedNodeMap. */
static HRESULT WINAPI map_named(void *iface, HSTRING name, void **out)
{
	IXMLDOMNode *found = NULL;
	if (!out)
		return E_POINTER;
	BSTR key = bstr_of(name);
	const HRESULT result = IXMLDOMNamedNodeMap_getNamedItem(collection_of(iface)->map, key, &found);
	SysFreeString(key);
	return answer_node(result, &found, NODE_FACE, out);
}

static HRESULT WINAPI map_set(void *iface, void *node, void **out)
{
	IXMLDOMNode *replaced = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	ARGUMENT(item, node);
	return answer_node(IXMLDOMNamedNodeMap_setNamedItem(collection_of(iface)->map, item, &replaced), &replaced, NODE_FACE, out);
}

static HRESULT WINAPI map_remove(void *iface, HSTRING name, void **out)
{
	IXMLDOMNode *removed = NULL;
	if (!out)
		return E_POINTER;
	BSTR key = bstr_of(name);
	const HRESULT result = IXMLDOMNamedNodeMap_removeNamedItem(collection_of(iface)->map, key, &removed);
	SysFreeString(key);
	return answer_node(result, &removed, NODE_FACE, out);
}

static HRESULT WINAPI map_named_ns(void *iface, IInspectable *namespace_uri, HSTRING name, void **out)
{
	IXMLDOMNode *found = NULL;
	BSTR uri = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = unbox(namespace_uri, &uri);
	if (FAILED(result))
		return result;
	BSTR key = bstr_of(name);
	result = IXMLDOMNamedNodeMap_getQualifiedItem(collection_of(iface)->map, key, uri, &found);
	SysFreeString(key);
	SysFreeString(uri);
	return answer_node(result, &found, NODE_FACE, out);
}

static HRESULT WINAPI map_remove_ns(void *iface, IInspectable *namespace_uri, HSTRING name, void **out)
{
	IXMLDOMNode *removed = NULL;
	BSTR uri = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = unbox(namespace_uri, &uri);
	if (FAILED(result))
		return result;
	BSTR key = bstr_of(name);
	result = IXMLDOMNamedNodeMap_removeQualifiedItem(collection_of(iface)->map, key, uri, &removed);
	SysFreeString(key);
	SysFreeString(uri);
	return answer_node(result, &removed, NODE_FACE, out);
}

TABLE(map_table, 0, INSPECTABLE, (const void *)collection_get_length, (const void *)collection_get_item, (const void *)map_named,
	(const void *)map_set, (const void *)map_remove, (const void *)map_named_ns, (const void *)map_remove_ns, (const void *)map_set);
static const struct face_iid map_iids[] = {{&iid_node_map, 0}, {&iid_node_view, 1}, {&iid_node_iterable, 2}, {NULL, 0}};
static const struct kind map_kind = {"XmlNamedNodeMap", L"Windows.Data.Xml.Dom.XmlNamedNodeMap", map_iids, destroy_collection};

static HRESULT wrap_collection(IXMLDOMNodeList *list, IXMLDOMNamedNodeMap *map, void **out)
{
	static const void *const *const list_tables[] = {list_table.slots, view_table.slots, iterable_table.slots};
	static const void *const *const map_tables[] = {map_table.slots, view_table.slots, iterable_table.slots};
	struct collection *self = make(sizeof *self, list ? &list_kind : &map_kind, list ? list_tables : map_tables, 3);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	if ((self->list = list))
		IXMLDOMNodeList_AddRef(list);
	if ((self->map = map))
		IXMLDOMNamedNodeMap_AddRef(map);
	return S_OK;
}

static HRESULT wrap_list(IXMLDOMNodeList *list, void **out) { return wrap_collection(list, NULL, out); }
static HRESULT wrap_map(IXMLDOMNamedNodeMap *map, void **out) { return wrap_collection(NULL, map, out); }

/* XmlDomImplementation. */
static HRESULT WINAPI implementation_has_feature(void *iface, HSTRING feature, IInspectable *version, boolean *out)
{
	VARIANT_BOOL has = VARIANT_FALSE;
	BSTR level = NULL;
	if (!out)
		return E_POINTER;
	*out = FALSE;
	HRESULT result = unbox(version, &level);
	if (FAILED(result))
		return result;
	BSTR name = bstr_of(feature);
	result = IXMLDOMImplementation_hasFeature(((struct implementation *)owner(iface))->implementation, name, level ? level : (BSTR)L"", &has);
	SysFreeString(name);
	SysFreeString(level);
	*out = has != VARIANT_FALSE;
	return FAILED(result) ? result : S_OK;
}

TABLE(implementation_table, 0, INSPECTABLE, (const void *)implementation_has_feature);
static const struct face_iid implementation_iids[] = {{&iid_implementation, 0}, {NULL, 0}};
static const struct kind implementation_kind = {"XmlDomImplementation", L"Windows.Data.Xml.Dom.XmlDomImplementation", implementation_iids,
	destroy_implementation};

static HRESULT make_implementation(IXMLDOMImplementation *implementation, void **out)
{
	static const void *const *const tables[] = {implementation_table.slots};
	struct implementation *self = make(sizeof *self, &implementation_kind, tables, 1);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	self->implementation = implementation;
	IXMLDOMImplementation_AddRef(implementation);
	return S_OK;
}

/* XmlLoadSettings. */
static struct settings *settings_of(void *iface) { return (struct settings *)owner(iface); }

static HRESULT WINAPI settings_depth(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = settings_of(iface)->depth;
	return S_OK;
}

static HRESULT WINAPI settings_set_depth(void *iface, UINT32 value)
{
	settings_of(iface)->depth = value;
	return S_OK;
}

#define SETTINGS_FLAG(getter, setter, field) \
	static HRESULT WINAPI getter(void *iface, boolean *out) { return answer_flag(out, settings_of(iface)->field); } \
	static HRESULT WINAPI setter(void *iface, boolean value) \
	{ \
		settings_of(iface)->field = value != 0; \
		return S_OK; \
	}

SETTINGS_FLAG(settings_prohibit_dtd, settings_set_prohibit_dtd, prohibit_dtd)
SETTINGS_FLAG(settings_resolve_externals, settings_set_resolve_externals, resolve_externals)
SETTINGS_FLAG(settings_validate, settings_set_validate, validate)
SETTINGS_FLAG(settings_white_space, settings_set_white_space, white_space)

TABLE(settings_table, 0, INSPECTABLE, (const void *)settings_depth, (const void *)settings_set_depth, (const void *)settings_prohibit_dtd,
	(const void *)settings_set_prohibit_dtd, (const void *)settings_resolve_externals, (const void *)settings_set_resolve_externals,
	(const void *)settings_validate, (const void *)settings_set_validate, (const void *)settings_white_space,
	(const void *)settings_set_white_space);
static const struct face_iid settings_iids[] = {{&iid_load_settings, 0}, {NULL, 0}};
static const struct kind settings_kind = {"XmlLoadSettings", L"Windows.Data.Xml.Dom.XmlLoadSettings", settings_iids, NULL};

static HRESULT WINAPI settings_activate(void *iface, IInspectable **out)
{
	static const void *const *const tables[] = {settings_table.slots};
	if (!out)
		return E_POINTER;
	struct settings *self = make(sizeof *self, &settings_kind, tables, 1);
	if (!self)
		return *out = NULL, E_OUTOFMEMORY;
	self->depth = default_settings.depth;
	self->prohibit_dtd = default_settings.prohibit_dtd;
	*out = face_of(&self->base, 0);
	return S_OK;
}

/* new XmlDocument(): an empty MSXML 6.0 document with the default settings, loading in one call. */
static HRESULT new_document(IXMLDOMDocument3 **out)
{
	HRESULT result = CoCreateInstance(&clsid_free_document60, NULL, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument3, (void **)out);
	if (FAILED(result))
		return result;
	result = IXMLDOMDocument3_put_async(*out, VARIANT_FALSE);
	if (SUCCEEDED(result))
		result = apply_settings(*out, &default_settings);
	if (FAILED(result)) {
		IXMLDOMDocument3_Release(*out);
		*out = NULL;
	}
	return result;
}

static HRESULT WINAPI document_activate(void *iface, IInspectable **out)
{
	IXMLDOMDocument3 *document;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = new_document(&document);
	if (SUCCEEDED(result))
		result = answer_node(S_OK, &document, OWN_FACE, (void **)out);
	return result;
}

/* LoadFromFileAsync and LoadFromUriAsync: a new document loaded on a thread of its own from a path or a URL. */
struct load {
	IXMLDOMDocument3 *document;
	BSTR source;
};

static void free_load(void *context)
{
	struct load *load = context;
	IXMLDOMDocument3_Release(load->document);
	SysFreeString(load->source);
	FREE(load);
}

static HRESULT load_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct load *load = context;
	VARIANT source;
	VariantInit(&source);
	source.vt = VT_BSTR;
	source.bstrVal = load->source;
	HRESULT status = load_source(load->document, source);
	if (SUCCEEDED(status))
		status = wrap((IUnknown *)load->document, OWN_FACE, &result->object);
	return status;
}

static HRESULT load_async(const WCHAR *source, void *settings, void **out)
{
	struct load *load = ALLOC(sizeof *load);
	if (!load)
		return E_OUTOFMEMORY;
	HRESULT result = new_document(&load->document);
	if (SUCCEEDED(result) && settings)
		result = apply_settings(load->document, settings_of(settings));
	if (SUCCEEDED(result) && !(load->source = SysAllocString(source)))
		result = E_OUTOFMEMORY;
	if (FAILED(result)) {
		if (load->document)
			IXMLDOMDocument3_Release(load->document);
		FREE(load);
		return result;
	}
	return winrt_async_start(&iid_document_operation, FALSE, WINRT_RESULT_OBJECT, load_work, load, free_load, out);
}

static HRESULT load_uri(__x_ABI_CWindows_CFoundation_CIUriRuntimeClass *uri, void *settings, void **out)
{
	WCHAR path[MAX_PATH];
	HSTRING absolute = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!uri)
		return E_INVALIDARG;
	HRESULT result = uri_file_path(uri, path);
	if (result == S_OK)
		return load_async(path, settings, out);
	if (FAILED(result) || FAILED(result = uri->lpVtbl->get_AbsoluteUri(uri, &absolute)))
		return result;
	BSTR url = bstr_of(absolute);
	WindowsDeleteString(absolute);
	result = url ? load_async(url, settings, out) : E_OUTOFMEMORY;
	SysFreeString(url);
	return result;
}

static HRESULT WINAPI document_from_uri(void *iface, void *uri, void **out) { return load_uri(uri, NULL, out); }

static HRESULT WINAPI document_from_uri_with(void *iface, void *uri, void *settings, void **out)
{
	return settings ? load_uri(uri, settings, out) : E_INVALIDARG;
}

static HRESULT load_file(void *file, void *settings, void **out)
{
	WCHAR path[MAX_PATH];
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!file)
		return E_INVALIDARG;
	const HRESULT result = path_of_any(file, path);
	return FAILED(result) ? result : load_async(path, settings, out);
}

static HRESULT WINAPI document_from_file(void *iface, void *file, void **out) { return load_file(file, NULL, out); }

static HRESULT WINAPI document_from_file_with(void *iface, void *file, void *settings, void **out)
{
	return settings ? load_file(file, settings, out) : E_INVALIDARG;
}

TABLE(document_factory_table, 0, INSPECTABLE, (const void *)document_activate);
TABLE(document_statics_table, 1, INSPECTABLE, (const void *)document_from_uri, (const void *)document_from_uri_with,
	(const void *)document_from_file, (const void *)document_from_file_with);
static const struct face_iid document_class_iids[] = {{&iid_factory, 0}, {&iid_document_statics, 1}, {NULL, 0}};
static const struct kind document_class_kind = {"XmlDocument", L"Windows.Data.Xml.Dom.XmlDocument", document_class_iids, NULL};
struct object xml_document_class = {{document_factory_table.slots, document_statics_table.slots}, &document_class_kind, 1};

TABLE(settings_factory_table, 0, INSPECTABLE, (const void *)settings_activate);
static const struct face_iid settings_class_iids[] = {{&iid_factory, 0}, {NULL, 0}};
static const struct kind settings_class_kind = {"XmlLoadSettings", L"Windows.Data.Xml.Dom.XmlLoadSettings", settings_class_iids, NULL};
struct object xml_load_settings_class = {{settings_factory_table.slots}, &settings_class_kind, 1};
