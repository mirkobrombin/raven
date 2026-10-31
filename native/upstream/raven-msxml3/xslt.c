/* SPDX-License-Identifier: MIT */
#include "node.h"
#include <libxslt/xslt.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>

/* --- The transform itself. --- */

typedef struct parameter
{
	struct parameter *next;
	char *name, *value;
} parameter;

/* A style sheet parse takes the tree, so it reads a copy of the document the given node is in. */
static xsltStylesheetPtr compile_style(xmlNodePtr style)
{
	xmlDocPtr tree = style->doc;
	xmlDocPtr copy = xmlCopyDoc(tree, 1);
	xsltStylesheetPtr sheet;
	xmlNodePtr first;
	if (!copy)
		return NULL;
	/* The declaration node is ours, and to libxslt an unknown top level instruction. */
	first = copy->children;
	if (first && first->type == XML_PI_NODE && !xmlStrcmp(first->name, (const xmlChar *)"xml"))
	{
		xmlUnlinkNode(first);
		xmlFreeNode(first);
	}
	sheet = xsltParseStylesheetDoc(copy);
	if (!sheet)
		xmlFreeDoc(copy);
	return sheet;
}

/* A node other than the whole document is transformed as the root of a tree of its own. */
static xmlDocPtr source_tree(xmlNodePtr node, BOOL *owned)
{
	xmlDocPtr tree;
	xmlNodePtr copy;
	*owned = TRUE;
	tree = xmlNewDoc(NULL);
	if (!tree)
		return NULL;
	if (node->type == XML_DOCUMENT_NODE)
	{
		xmlNodePtr at;
		for (at = node->children; at; at = at->next)
		{
			if (at->type == XML_PI_NODE && !xmlStrcmp(at->name, (const xmlChar *)"xml"))
				continue;
			if (at->type == XML_DTD_NODE)
				continue;
			copy = xmlDocCopyNode(at, tree, 1);
			if (copy)
				xmlAddChild((xmlNodePtr)tree, copy);
		}
		return tree;
	}
	copy = xmlDocCopyNode(node, tree, 1);
	if (copy)
		xmlDocSetRootElement(tree, copy);
	return tree;
}

/* As text, without the declaration when it says it wants none. */
static HRESULT result_text(xmlDocPtr result, xsltStylesheetPtr sheet, BSTR *out)
{
	xmlChar *text = NULL;
	int size = 0;
	BOOL plain_text = sheet->method && !xmlStrcmp(sheet->method, (const xmlChar *)"text");
	const xmlChar *saved_encoding = sheet->encoding;
	sheet->encoding = (xmlChar *)"UTF-8";
	if (xsltSaveResultToString(&text, &size, result, sheet) < 0)
	{
		sheet->encoding = (xmlChar *)saved_encoding;
		return E_FAIL;
	}
	sheet->encoding = (xmlChar *)saved_encoding;
	/* The output layer ends a document with a line this library does not write. */
	if (text && size && text[size - 1] == '\n')
		size--;
	*out = text ? mx_bstr_counted(text, size) : SysAllocStringLen(NULL, 0);
	xmlFree(text);
	(void)plain_text;
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT run_transform(xmlNodePtr node, xsltStylesheetPtr sheet, const parameter *list, xmlDocPtr *result)
{
	const char *names[64];
	const parameter *at;
	unsigned count = 0;
	BOOL owned;
	xmlDocPtr tree = source_tree(node, &owned);
	if (!tree)
		return E_OUTOFMEMORY;
	for (at = list; at && count < ARRAYSIZE(names) - 2; at = at->next)
	{
		names[count++] = at->name;
		names[count++] = at->value;
	}
	names[count] = NULL;
	*result = xsltApplyStylesheet(sheet, tree, count ? names : NULL);
	if (owned)
		xmlFreeDoc(tree);
	return *result ? S_OK : E_FAIL;
}

HRESULT mx_node_transform(xmlNodePtr node, IXMLDOMNode *style, BSTR *out, IStream *stream, VARIANT *object)
{
	xmlNodePtr given = mx_node_get((IUnknown *)style);
	xsltStylesheetPtr sheet;
	xmlDocPtr result;
	HRESULT status;
	if (!given)
		return E_INVALIDARG;
	if (!xmlDocGetRootElement(given->doc))
		return E_FAIL;
	sheet = compile_style(given);
	if (!sheet)
		return E_FAIL;
	status = run_transform(node, sheet, NULL, &result);
	if (SUCCEEDED(status))
	{
		status = result_text(result, sheet, out);
		xmlFreeDoc(result);
	}
	xsltFreeStylesheet(sheet);
	return status;
}

HRESULT mx_node_transform_to_object(xmlNodePtr node, IXMLDOMNode *style, VARIANT target)
{
	BSTR text;
	HRESULT status = mx_node_transform(node, style, &text, NULL, NULL);
	if (FAILED(status))
		return status;
	if (V_VT(&target) == VT_UNKNOWN || V_VT(&target) == VT_DISPATCH)
	{
		IXMLDOMDocument *document;
		ISequentialStream *stream = NULL;
		/* A stream gets the text as it is held, with the mark that says so. */
		if (V_UNKNOWN(&target) && (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(&target), &IID_IStream, (void **)&stream)) ||
			SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(&target), &IID_ISequentialStream, (void **)&stream))))
		{
			static const WCHAR mark = 0xfeff;
			ULONG written;
			status = ISequentialStream_Write(stream, &mark, sizeof(mark), &written);
			if (SUCCEEDED(status))
				status = ISequentialStream_Write(stream, text, SysStringLen(text) * sizeof(WCHAR), &written);
			ISequentialStream_Release(stream);
		}
		else if (V_UNKNOWN(&target) && mx_is_document(V_UNKNOWN(&target)) && SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(&target), &IID_IXMLDOMDocument, (void **)&document)))
		{
			VARIANT_BOOL success;
			status = IXMLDOMDocument_loadXML(document, text, &success);
			IXMLDOMDocument_Release(document);
		}
		else
			status = E_INVALIDARG;
	}
	else
		status = E_INVALIDARG;
	SysFreeString(text);
	return status;
}

/* --- IXSLTemplate. --- */

typedef struct
{
	const IXSLTemplateVtbl *vtbl;
	mx_dispex dispex;
	LONG refs;
	IXMLDOMNode *style;
} template_object;

typedef struct
{
	const IXSLProcessorVtbl *vtbl;
	mx_dispex dispex;
	LONG refs;
	template_object *owner;
	IXMLDOMNode *input;
	VARIANT output;
	IUnknown *output_held;
	BSTR text;
	LONG state;
	parameter *parameters;
	BSTR start_mode, start_uri;
} processor_object;

static HRESULT WINAPI template_QueryInterface(IXSLTemplate *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &((template_object *)iface)->dispex;
	else if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IDispatch) && !IsEqualGUID(riid, &IID_IXSLTemplate))
		return E_NOINTERFACE;
	else
		*out = iface;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI template_AddRef(IXSLTemplate *iface)
{
	return InterlockedIncrement(&((template_object *)iface)->refs);
}

static ULONG WINAPI template_Release(IXSLTemplate *iface)
{
	template_object *self = (template_object *)iface;
	ULONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	if (self->style)
		IXMLDOMNode_Release(self->style);
	mx_free(self);
	return 0;
}

static HRESULT WINAPI template_GetTypeInfoCount(IXSLTemplate *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI template_GetTypeInfo(IXSLTemplate *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return index ? DISP_E_BADINDEX : mx_type_info(MX_TYPE_TEMPLATE, out);
}

static HRESULT WINAPI template_GetIDsOfNames(IXSLTemplate *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_TEMPLATE, riid, names, count, lcid, ids);
}

static HRESULT WINAPI template_Invoke(IXSLTemplate *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_TEMPLATE, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI template_putref_stylesheet(IXSLTemplate *iface, IXMLDOMNode *node)
{
	template_object *self = (template_object *)iface;
	if (node)
	{
		xmlNodePtr given = mx_node_get((IUnknown *)node);
		if (!given)
			return E_INVALIDARG;
		/* The whole document or its root, and it has to be able to run. */
		if (given->type != XML_DOCUMENT_NODE && given != xmlDocGetRootElement(given->doc))
			return E_INVALIDARG;
		IXMLDOMNode_AddRef(node);
	}
	if (self->style)
		IXMLDOMNode_Release(self->style);
	self->style = node;
	return S_OK;
}

static HRESULT WINAPI template_get_stylesheet(IXSLTemplate *iface, IXMLDOMNode **out)
{
	template_object *self = (template_object *)iface;
	if (!out)
		return E_INVALIDARG;
	*out = self->style;
	if (!self->style)
		return S_OK;
	IXMLDOMNode_AddRef(self->style);
	return S_OK;
}

static HRESULT processor_new(template_object *owner, IXSLProcessor **out);

static HRESULT WINAPI template_createProcessor(IXSLTemplate *iface, IXSLProcessor **out)
{
	template_object *self = (template_object *)iface;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!self->style)
		return E_UNEXPECTED;
	return processor_new(self, out);
}

static const IXSLTemplateVtbl template_table = {
	template_QueryInterface, template_AddRef, template_Release, template_GetTypeInfoCount, template_GetTypeInfo, template_GetIDsOfNames,
	template_Invoke, template_putref_stylesheet, template_get_stylesheet, template_createProcessor,
};

HRESULT mx_template_create(mx_version version, REFIID riid, void **out)
{
	template_object *self = mx_zalloc(sizeof(*self));
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &template_table;
	mx_dispex_init(&self->dispex, self, FALSE);
	self->refs = 1;
	status = template_QueryInterface((IXSLTemplate *)self, riid, out);
	template_Release((IXSLTemplate *)self);
	return status;
}

/* --- IXSLProcessor. --- */

static processor_object *processor_from(IXSLProcessor *iface)
{
	return (processor_object *)iface;
}

static HRESULT WINAPI processor_QueryInterface(IXSLProcessor *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &processor_from(iface)->dispex;
	else if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IDispatch) && !IsEqualGUID(riid, &IID_IXSLProcessor))
		return E_NOINTERFACE;
	else
		*out = iface;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI processor_AddRef(IXSLProcessor *iface)
{
	return InterlockedIncrement(&processor_from(iface)->refs);
}

static void parameters_clear(processor_object *self)
{
	parameter *at;
	while ((at = self->parameters))
	{
		self->parameters = at->next;
		mx_free(at->name);
		mx_free(at->value);
		mx_free(at);
	}
}

static ULONG WINAPI processor_Release(IXSLProcessor *iface)
{
	processor_object *self = processor_from(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	IXSLTemplate_Release((IXSLTemplate *)self->owner);
	if (self->input)
		IXMLDOMNode_Release(self->input);
	VariantClear(&self->output);
	if (self->output_held)
		IUnknown_Release(self->output_held);
	SysFreeString(self->text);
	SysFreeString(self->start_mode);
	SysFreeString(self->start_uri);
	parameters_clear(self);
	mx_free(self);
	return 0;
}

static HRESULT WINAPI processor_GetTypeInfoCount(IXSLProcessor *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI processor_GetTypeInfo(IXSLProcessor *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return index ? DISP_E_BADINDEX : mx_type_info(MX_TYPE_PROCESSOR, out);
}

static HRESULT WINAPI processor_GetIDsOfNames(IXSLProcessor *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_PROCESSOR, riid, names, count, lcid, ids);
}

static HRESULT WINAPI processor_Invoke(IXSLProcessor *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_PROCESSOR, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI processor_put_input(IXSLProcessor *iface, VARIANT input)
{
	processor_object *self = processor_from(iface);
	IXMLDOMNode *node = NULL;
	if (V_VT(&input) == VT_BSTR)
	{
		/* Text is parsed into a document of its own. */
		IXMLDOMDocument *document;
		VARIANT_BOOL success;
		HRESULT status = mx_document_create(MX_DEFAULT, &IID_IXMLDOMDocument, (void **)&document);
		if (FAILED(status))
			return status;
		status = IXMLDOMDocument_loadXML(document, V_BSTR(&input), &success);
		if (status != S_OK)
		{
			IXMLDOMDocument_Release(document);
			return E_INVALIDARG;
		}
		node = (IXMLDOMNode *)document;
	}
	else if ((V_VT(&input) == VT_UNKNOWN || V_VT(&input) == VT_DISPATCH) && V_UNKNOWN(&input))
	{
		if (FAILED(IUnknown_QueryInterface(V_UNKNOWN(&input), &IID_IXMLDOMNode, (void **)&node)))
			return E_INVALIDARG;
	}
	else
		return E_INVALIDARG;
	if (self->input)
		IXMLDOMNode_Release(self->input);
	self->input = node;
	return S_OK;
}

static HRESULT WINAPI processor_get_input(IXSLProcessor *iface, VARIANT *out)
{
	processor_object *self = processor_from(iface);
	if (!out)
		return E_INVALIDARG;
	V_VT(out) = VT_DISPATCH;
	V_DISPATCH(out) = (IDispatch *)self->input;
	if (self->input)
		IXMLDOMNode_AddRef(self->input);
	return S_OK;
}

static HRESULT WINAPI processor_get_ownerTemplate(IXSLProcessor *iface, IXSLTemplate **out)
{
	processor_object *self = processor_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = (IXSLTemplate *)self->owner;
	IXSLTemplate_AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI processor_setStartMode(IXSLProcessor *iface, BSTR mode, BSTR uri)
{
	processor_object *self = processor_from(iface);
	SysFreeString(self->start_mode);
	SysFreeString(self->start_uri);
	self->start_mode = SysAllocString(mode);
	self->start_uri = SysAllocString(uri);
	return S_OK;
}

static HRESULT WINAPI processor_get_startMode(IXSLProcessor *iface, BSTR *out)
{
	processor_object *self = processor_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = SysAllocString(self->start_mode);
	return S_OK;
}

static HRESULT WINAPI processor_get_startModeURI(IXSLProcessor *iface, BSTR *out)
{
	processor_object *self = processor_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = SysAllocString(self->start_uri);
	return S_OK;
}

static const GUID response_iid = {0xd97a6da0, 0xa864, 0x11cf, {0x83, 0xbe, 0x00, 0xa0, 0xc9, 0x0c, 0x2b, 0xd8}};

/* The response object takes its bytes through this slot of its table, as a one-dimensional array. */
enum { RESPONSE_BINARY_WRITE = 21 };

static HRESULT write_response(IUnknown *response, const WCHAR *text)
{
	typedef HRESULT (WINAPI *binary_write)(IUnknown *, VARIANT);
	binary_write write = (*(binary_write **)response)[RESPONSE_BINARY_WRITE];
	SAFEARRAY *bytes;
	void *data;
	VARIANT input;
	UINT size = lstrlenW(text) * sizeof(WCHAR) + sizeof(WCHAR);
	bytes = SafeArrayCreateVector(VT_UI1, 0, size);
	if (!bytes)
		return E_OUTOFMEMORY;
	SafeArrayAccessData(bytes, &data);
	*(WCHAR *)data = 0xfeff;
	memcpy((WCHAR *)data + 1, text, size - sizeof(WCHAR));
	SafeArrayUnaccessData(bytes);
	V_VT(&input) = VT_ARRAY | VT_UI1;
	V_ARRAY(&input) = bytes;
	write(response, input);
	SafeArrayDestroy(bytes);
	return S_OK;
}

static HRESULT WINAPI processor_put_output(IXSLProcessor *iface, VARIANT target)
{
	processor_object *self = processor_from(iface);
	switch (V_VT(&target))
	{
	case VT_EMPTY:
		if (self->output_held)
			IUnknown_Release(self->output_held);
		self->output_held = NULL;
		break;
	case VT_UNKNOWN:
	case VT_DISPATCH:
		{
			void *probe = NULL;
			if (!V_UNKNOWN(&target))
				break;
			/* Besides the documents, anything that takes bytes will do: a stream or an ASP response. */
			if (!mx_is_document(V_UNKNOWN(&target)))
			{
				if (FAILED(IUnknown_QueryInterface(V_UNKNOWN(&target), &IID_IStream, &probe)) &&
					FAILED(IUnknown_QueryInterface(V_UNKNOWN(&target), &IID_ISequentialStream, &probe)) &&
					FAILED(IUnknown_QueryInterface(V_UNKNOWN(&target), &response_iid, &probe)))
					return E_INVALIDARG;
			}
			else
				IUnknown_AddRef(V_UNKNOWN(&target));
			if (!probe)
				probe = V_UNKNOWN(&target);
			if (self->output_held)
				IUnknown_Release(self->output_held);
			self->output_held = probe;
		}
		break;
	default:
		return E_INVALIDARG;
	}
	VariantClear(&self->output);
	return VariantCopy(&self->output, &target);
}

static HRESULT WINAPI processor_get_output(IXSLProcessor *iface, VARIANT *out)
{
	processor_object *self = processor_from(iface);
	if (!out)
		return E_INVALIDARG;
	V_VT(out) = VT_EMPTY;
	if (V_VT(&self->output) != VT_EMPTY)
		return VariantCopy(out, &self->output);
	if (self->text)
	{
		V_VT(out) = VT_BSTR;
		V_BSTR(out) = SysAllocString(self->text);
	}
	return S_OK;
}

static HRESULT WINAPI processor_transform(IXSLProcessor *iface, VARIANT_BOOL *out)
{
	processor_object *self = processor_from(iface);
	xmlNodePtr node, style;
	xsltStylesheetPtr sheet;
	xmlDocPtr result;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	*out = VARIANT_FALSE;
	node = mx_node_get((IUnknown *)self->input);
	style = mx_node_get((IUnknown *)self->owner->style);
	if (!node || !style)
		return E_FAIL;
	self->state = READYSTATE_INTERACTIVE;
	sheet = compile_style(style);
	if (!sheet)
		return E_FAIL;
	status = run_transform(node, sheet, self->parameters, &result);
	if (SUCCEEDED(status))
	{
		SysFreeString(self->text);
		self->text = NULL;
		status = result_text(result, sheet, &self->text);
		xmlFreeDoc(result);
	}
	xsltFreeStylesheet(sheet);
	if (SUCCEEDED(status) && V_VT(&self->output) != VT_EMPTY && V_UNKNOWN(&self->output))
	{
		IXMLDOMDocument *document;
		ISequentialStream *stream;
		IUnknown *response;
		if (mx_is_document(V_UNKNOWN(&self->output)) && SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(&self->output), &IID_IXMLDOMDocument, (void **)&document)))
		{
			VARIANT_BOOL success;
			status = IXMLDOMDocument_loadXML(document, self->text, &success);
			IXMLDOMDocument_Release(document);
		}
		else if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(&self->output), &IID_IStream, (void **)&stream)) ||
			SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(&self->output), &IID_ISequentialStream, (void **)&stream)))
		{
			xmlChar *utf8 = mx_to_utf8(self->text);
			ULONG written;
			status = ISequentialStream_Write(stream, utf8, xmlStrlen(utf8), &written);
			xmlFree(utf8);
			ISequentialStream_Release(stream);
		}
		else if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(&self->output), &response_iid, (void **)&response)))
		{
			status = write_response(response, self->text);
			IUnknown_Release(response);
		}
	}
	self->state = READYSTATE_COMPLETE;
	if (SUCCEEDED(status))
		*out = VARIANT_TRUE;
	return status;
}

static HRESULT WINAPI processor_reset(IXSLProcessor *iface)
{
	processor_object *self = processor_from(iface);
	self->state = READYSTATE_INTERACTIVE;
	return S_OK;
}

static HRESULT WINAPI processor_get_readyState(IXSLProcessor *iface, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = processor_from(iface)->state;
	return S_OK;
}

/* A value enters the sheet as an XPath expression, so text goes in quotes. */
static char *quoted_value(const VARIANT *value)
{
	VARIANT text;
	xmlChar *utf8;
	char *out;
	int size;
	if (FAILED(mx_variant_text(value, &text)))
		return NULL;
	utf8 = mx_to_utf8(V_BSTR(&text));
	VariantClear(&text);
	size = xmlStrlen(utf8);
	out = mx_alloc(size + 3);
	if (out)
	{
		char quote = xmlStrchr(utf8, '\'') ? '"' : '\'';
		out[0] = quote;
		memcpy(out + 1, utf8, size);
		out[size + 1] = quote;
		out[size + 2] = 0;
	}
	xmlFree(utf8);
	return out;
}

static HRESULT WINAPI processor_addParameter(IXSLProcessor *iface, BSTR name, VARIANT value, BSTR uri)
{
	processor_object *self = processor_from(iface);
	parameter *at, **link;
	char *text;
	xmlChar *utf8;
	if (!name)
		return E_INVALIDARG;
	if (V_VT(&value) == VT_EMPTY || V_VT(&value) == VT_NULL || V_VT(&value) == VT_ERROR)
	{
		/* No value takes a set parameter off again. */
		utf8 = mx_to_utf8(name);
		for (link = &self->parameters; *link; link = &(*link)->next)
			if (!strcmp((*link)->name, (const char *)utf8))
			{
				at = *link;
				*link = at->next;
				mx_free(at->name);
				mx_free(at->value);
				mx_free(at);
				break;
			}
		xmlFree(utf8);
		return S_OK;
	}
	text = quoted_value(&value);
	if (!text)
		return E_INVALIDARG;
	utf8 = mx_to_utf8(name);
	for (at = self->parameters; at; at = at->next)
		if (!strcmp(at->name, (const char *)utf8))
		{
			mx_free(at->value);
			at->value = text;
			xmlFree(utf8);
			return S_OK;
		}
	at = mx_alloc(sizeof(*at));
	if (at)
	{
		int size = xmlStrlen(utf8);
		at->name = mx_alloc(size + 1);
		memcpy(at->name, utf8, size + 1);
		at->value = text;
		at->next = self->parameters;
		self->parameters = at;
	}
	xmlFree(utf8);
	return at ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI processor_addObject(IXSLProcessor *iface, IDispatch *object, BSTR uri)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI processor_get_stylesheet(IXSLProcessor *iface, IXMLDOMNode **out)
{
	processor_object *self = processor_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = self->owner->style;
	IXMLDOMNode_AddRef(*out);
	return S_OK;
}

static const IXSLProcessorVtbl processor_table = {
	processor_QueryInterface, processor_AddRef, processor_Release, processor_GetTypeInfoCount, processor_GetTypeInfo, processor_GetIDsOfNames,
	processor_Invoke, processor_put_input, processor_get_input, processor_get_ownerTemplate, processor_setStartMode, processor_get_startMode,
	processor_get_startModeURI, processor_put_output, processor_get_output, processor_transform, processor_reset, processor_get_readyState,
	processor_addParameter, processor_addObject, processor_get_stylesheet,
};

static HRESULT processor_new(template_object *owner, IXSLProcessor **out)
{
	processor_object *self = mx_zalloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &processor_table;
	mx_dispex_init(&self->dispex, self, FALSE);
	self->refs = 1;
	self->owner = owner;
	IXSLTemplate_AddRef((IXSLTemplate *)owner);
	*out = (IXSLProcessor *)self;
	return S_OK;
}
