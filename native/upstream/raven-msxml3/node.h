/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_MSXML_NODE_H
#define RAVEN_MSXML_NODE_H

#include "mx.h"

/* One object answers for every kind of node. Each DOM interface extends IXMLDOMNode, so the object
 * has a single interface pointer whose table is the widest one for its kind, and the same pointer
 * serves for IXMLDOMNode and for the wider interface alike. */
typedef struct mx_node
{
	const void *vtbl;
	mx_dispex dispex;
	LONG refs;
	xmlNodePtr node;
	xmlDocPtr held;
	mx_type type;
	const IID *iid;
	void (*destroy)(struct mx_node *self);
	HRESULT (*query)(struct mx_node *self, REFIID riid, void **out);
} mx_node;

static inline mx_node *mx_node_from(void *iface)
{
	return (mx_node *)iface;
}

HRESULT WINAPI nd_QueryInterface(IXMLDOMNode *iface, REFIID riid, void **out);
ULONG WINAPI nd_AddRef(IXMLDOMNode *iface);
ULONG WINAPI nd_Release(IXMLDOMNode *iface);
HRESULT WINAPI nd_GetTypeInfoCount(IXMLDOMNode *iface, UINT *out);
HRESULT WINAPI nd_GetTypeInfo(IXMLDOMNode *iface, UINT index, LCID lcid, ITypeInfo **out);
HRESULT WINAPI nd_GetIDsOfNames(IXMLDOMNode *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids);
HRESULT WINAPI nd_Invoke(IXMLDOMNode *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err);
HRESULT WINAPI nd_get_nodeName(IXMLDOMNode *iface, BSTR *out);
HRESULT WINAPI nd_get_nodeValue(IXMLDOMNode *iface, VARIANT *out);
HRESULT WINAPI nd_put_nodeValue(IXMLDOMNode *iface, VARIANT value);
HRESULT WINAPI nd_get_nodeType(IXMLDOMNode *iface, DOMNodeType *out);
HRESULT WINAPI nd_get_parentNode(IXMLDOMNode *iface, IXMLDOMNode **out);
HRESULT WINAPI nd_get_childNodes(IXMLDOMNode *iface, IXMLDOMNodeList **out);
HRESULT WINAPI nd_get_firstChild(IXMLDOMNode *iface, IXMLDOMNode **out);
HRESULT WINAPI nd_get_lastChild(IXMLDOMNode *iface, IXMLDOMNode **out);
HRESULT WINAPI nd_get_previousSibling(IXMLDOMNode *iface, IXMLDOMNode **out);
HRESULT WINAPI nd_get_nextSibling(IXMLDOMNode *iface, IXMLDOMNode **out);
HRESULT WINAPI nd_get_attributes(IXMLDOMNode *iface, IXMLDOMNamedNodeMap **out);
HRESULT WINAPI nd_insertBefore(IXMLDOMNode *iface, IXMLDOMNode *child, VARIANT before, IXMLDOMNode **out);
HRESULT WINAPI nd_replaceChild(IXMLDOMNode *iface, IXMLDOMNode *child, IXMLDOMNode *old, IXMLDOMNode **out);
HRESULT WINAPI nd_removeChild(IXMLDOMNode *iface, IXMLDOMNode *child, IXMLDOMNode **out);
HRESULT WINAPI nd_appendChild(IXMLDOMNode *iface, IXMLDOMNode *child, IXMLDOMNode **out);
HRESULT WINAPI nd_hasChildNodes(IXMLDOMNode *iface, VARIANT_BOOL *out);
HRESULT WINAPI nd_get_ownerDocument(IXMLDOMNode *iface, IXMLDOMDocument **out);
HRESULT WINAPI nd_cloneNode(IXMLDOMNode *iface, VARIANT_BOOL deep, IXMLDOMNode **out);
HRESULT WINAPI nd_get_nodeTypeString(IXMLDOMNode *iface, BSTR *out);
HRESULT WINAPI nd_get_text(IXMLDOMNode *iface, BSTR *out);
HRESULT WINAPI nd_put_text(IXMLDOMNode *iface, BSTR text);
HRESULT WINAPI nd_get_specified(IXMLDOMNode *iface, VARIANT_BOOL *out);
HRESULT WINAPI nd_get_definition(IXMLDOMNode *iface, IXMLDOMNode **out);
HRESULT WINAPI nd_get_nodeTypedValue(IXMLDOMNode *iface, VARIANT *out);
HRESULT WINAPI nd_put_nodeTypedValue(IXMLDOMNode *iface, VARIANT value);
HRESULT WINAPI nd_get_dataType(IXMLDOMNode *iface, VARIANT *out);
HRESULT WINAPI nd_put_dataType(IXMLDOMNode *iface, BSTR type);
HRESULT WINAPI nd_get_xml(IXMLDOMNode *iface, BSTR *out);
HRESULT WINAPI nd_transformNode(IXMLDOMNode *iface, IXMLDOMNode *style, BSTR *out);
HRESULT WINAPI nd_selectNodes(IXMLDOMNode *iface, BSTR query, IXMLDOMNodeList **out);
HRESULT WINAPI nd_selectSingleNode(IXMLDOMNode *iface, BSTR query, IXMLDOMNode **out);
HRESULT WINAPI nd_get_parsed(IXMLDOMNode *iface, VARIANT_BOOL *out);
HRESULT WINAPI nd_get_namespaceURI(IXMLDOMNode *iface, BSTR *out);
HRESULT WINAPI nd_get_prefix(IXMLDOMNode *iface, BSTR *out);
HRESULT WINAPI nd_get_baseName(IXMLDOMNode *iface, BSTR *out);
HRESULT WINAPI nd_transformNodeToObject(IXMLDOMNode *iface, IXMLDOMNode *style, VARIANT target);

/* The start of every table: the first parts of any IXMLDOM*Vtbl are the same functions. */
#define MX_NODE_TABLE \
	.QueryInterface = (void *)nd_QueryInterface, .AddRef = (void *)nd_AddRef, .Release = (void *)nd_Release, \
	.GetTypeInfoCount = (void *)nd_GetTypeInfoCount, .GetTypeInfo = (void *)nd_GetTypeInfo, \
	.GetIDsOfNames = (void *)nd_GetIDsOfNames, .Invoke = (void *)nd_Invoke, \
	.get_nodeName = (void *)nd_get_nodeName, .get_nodeValue = (void *)nd_get_nodeValue, .put_nodeValue = (void *)nd_put_nodeValue, \
	.get_nodeType = (void *)nd_get_nodeType, .get_parentNode = (void *)nd_get_parentNode, .get_childNodes = (void *)nd_get_childNodes, \
	.get_firstChild = (void *)nd_get_firstChild, .get_lastChild = (void *)nd_get_lastChild, \
	.get_previousSibling = (void *)nd_get_previousSibling, .get_nextSibling = (void *)nd_get_nextSibling, \
	.get_attributes = (void *)nd_get_attributes, .insertBefore = (void *)nd_insertBefore, .replaceChild = (void *)nd_replaceChild, \
	.removeChild = (void *)nd_removeChild, .appendChild = (void *)nd_appendChild, .hasChildNodes = (void *)nd_hasChildNodes, \
	.get_ownerDocument = (void *)nd_get_ownerDocument, .cloneNode = (void *)nd_cloneNode, \
	.get_nodeTypeString = (void *)nd_get_nodeTypeString, .get_text = (void *)nd_get_text, .put_text = (void *)nd_put_text, \
	.get_specified = (void *)nd_get_specified, .get_definition = (void *)nd_get_definition, \
	.get_nodeTypedValue = (void *)nd_get_nodeTypedValue, .put_nodeTypedValue = (void *)nd_put_nodeTypedValue, \
	.get_dataType = (void *)nd_get_dataType, .put_dataType = (void *)nd_put_dataType, .get_xml = (void *)nd_get_xml, \
	.transformNode = (void *)nd_transformNode, .selectNodes = (void *)nd_selectNodes, .selectSingleNode = (void *)nd_selectSingleNode, \
	.get_parsed = (void *)nd_get_parsed, .get_namespaceURI = (void *)nd_get_namespaceURI, .get_prefix = (void *)nd_get_prefix, \
	.get_baseName = (void *)nd_get_baseName, .transformNodeToObject = (void *)nd_transformNodeToObject

void mx_node_init(mx_node *self, const void *vtbl, xmlNodePtr node, mx_type type, const IID *iid);
xmlNodePtr mx_link_children_of_fragment(xmlNodePtr fragment);
HRESULT mx_link_before(xmlNodePtr parent, xmlNodePtr child, xmlNodePtr before);

#endif
