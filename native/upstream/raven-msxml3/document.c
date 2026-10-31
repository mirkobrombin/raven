/* SPDX-License-Identifier: MIT */
#include "node.h"

/* Script handlers and the document's own connection points. */
enum { EVENT_READY, EVENT_DATA, EVENT_TRANSFORM, EVENT_COUNT };
enum { POINT_DISPATCH, POINT_NOTIFY, POINT_EVENTS, POINT_COUNT };
enum { DISPID_READY_CHANGE = -609 };

static const GUID document_events_iid = {0x3efaa427, 0x272f, 0x11d2, {0x83, 0x6f, 0x00, 0x00, 0xf8, 0x7a, 0x77, 0x82}};

typedef struct mx_document mx_document;

/* One kind of event: sinks are kept by slot and the cookie is the slot plus one. */
typedef struct
{
	const IConnectionPointVtbl *vtbl;
	mx_document *owner;
	const IID *iid;
	IUnknown **sinks;
	DWORD count;
} mx_point;

struct mx_document
{
	mx_node base;
	const IPersistStreamInitVtbl *persist_vtbl;
	const IObjectWithSiteVtbl *site_vtbl;
	const IObjectSafetyVtbl *safety_vtbl;
	const IConnectionPointContainerVtbl *points_vtbl;
	mx_point points[POINT_COUNT];
	mx_error_info error;
	IDispatch *handlers[EVENT_COUNT];
	IUnknown *site;
	IXMLDOMSchemaCollection *namespaces;
	xmlChar *snapshot;
	int snapshot_size;
	/* What streams have written so far, and how many of them still have to finish. */
	char *pending;
	ULONG pending_size;
	LONG writers;
	VARIANT_BOOL async;
	LONG ready;
	DWORD safety;
	BOOL modified;
};

static mx_document *doc_from(IXMLDOMDocument3 *iface)
{
	return (mx_document *)iface;
}

static xmlDocPtr tree_of(mx_document *self)
{
	return (xmlDocPtr)self->base.node;
}

static const IXMLDOMDocument3Vtbl document_table;

/* --- Reading text in from wherever it lives. --- */

HRESULT mx_read_stream(IStream *stream, char **data, ULONG *size)
{
	ULONG room = 4096, used = 0, got = 0;
	char *buffer = mx_alloc(room);
	HRESULT status;
	if (!buffer)
		return E_OUTOFMEMORY;
	for (;;)
	{
		status = IStream_Read(stream, buffer + used, room - used, &got);
		if (FAILED(status))
		{
			mx_free(buffer);
			return status;
		}
		if (!got)
			break;
		used += got;
		if (used == room)
		{
			char *grown = mx_realloc(buffer, room * 2);
			if (!grown)
			{
				mx_free(buffer);
				return E_OUTOFMEMORY;
			}
			buffer = grown;
			room *= 2;
		}
	}
	*data = buffer;
	*size = used;
	return S_OK;
}

static BOOL is_web_location(const WCHAR *url)
{
	return !_wcsnicmp(url, L"http:", 5) || !_wcsnicmp(url, L"https:", 6) || !_wcsnicmp(url, L"ftp:", 4) || !_wcsnicmp(url, L"res:", 4);
}

HRESULT mx_read_url(const WCHAR *url, char **data, ULONG *size)
{
	WCHAR local[MAX_PATH * 2];
	const WCHAR *path = url;
	HANDLE file;
	DWORD length, got = 0;
	char *buffer;
	if (is_web_location(url))
	{
		IStream *stream;
		HRESULT status = URLOpenBlockingStreamW(NULL, url, &stream, 0, NULL);
		if (FAILED(status))
			return status;
		status = mx_read_stream(stream, data, size);
		IStream_Release(stream);
		return status;
	}
	if (!_wcsnicmp(url, L"file:", 5))
	{
		DWORD count = ARRAYSIZE(local);
		const WCHAR *at = url + 5;
		const WCHAR *from;
		WCHAR *to = local;
		while (*at == L'/')
			at++;
		for (from = at; *from && to < local + count - 1; from++)
		{
			if (*from == L'%' && from[1] && from[2])
			{
				WCHAR digits[3] = {from[1], from[2], 0};
				*to++ = (WCHAR)wcstol(digits, NULL, 16);
				from += 2;
			}
			else
				*to++ = *from == L'/' ? L'\\' : *from;
		}
		*to = 0;
		path = local;
	}
	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE && path == url && wcschr(url, L'%'))
	{
		/* A plain path can spell its characters the way a location does. */
		const WCHAR *from;
		WCHAR *to = local;
		for (from = url; *from && to < local + ARRAYSIZE(local) - 1; from++)
		{
			if (*from == L'%' && from[1] && from[2])
			{
				WCHAR digits[3] = {from[1], from[2], 0};
				*to++ = (WCHAR)wcstol(digits, NULL, 16);
				from += 2;
			}
			else
				*to++ = *from;
		}
		*to = 0;
		file = CreateFileW(local, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	}
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());
	length = GetFileSize(file, NULL);
	buffer = mx_alloc(length + 1);
	if (!buffer || !ReadFile(file, buffer, length, &got, NULL))
	{
		mx_free(buffer);
		CloseHandle(file);
		return E_FAIL;
	}
	CloseHandle(file);
	*data = buffer;
	*size = got;
	return S_OK;
}

/* --- Parsing. --- */

/* Whether the text starts with an XML declaration, and how far it runs. libxml takes the
 * declaration off into the document's fields; this library shows it as the first node. */
static const char *find_declaration(const char *data, int size, int *length)
{
	const char *at = data, *end = data + size, *close;
	if (size >= 3 && (BYTE)at[0] == 0xef && (BYTE)at[1] == 0xbb && (BYTE)at[2] == 0xbf)
		at += 3;
	if (end - at < 6 || memcmp(at, "<?xml", 5) || !(at[5] == ' ' || at[5] == '\t' || at[5] == '\r' || at[5] == '\n'))
		return NULL;
	for (close = at + 5; close + 1 < end; close++)
		if (close[0] == '?' && close[1] == '>')
		{
			*length = (int)(close - at);
			return at;
		}
	return NULL;
}

static void double_quotes(xmlChar *text)
{
	for (; *text; text++)
		if (*text == '\'')
			*text = '"';
}

static void show_declaration(xmlDocPtr tree, const char *declaration, int length)
{
	const char *from = declaration + 5, *to = declaration + length;
	xmlChar *content;
	xmlNodePtr node;
	while (from < to && (*from == ' ' || *from == '\t' || *from == '\r' || *from == '\n'))
		from++;
	while (to > from && (to[-1] == ' ' || to[-1] == '\t' || to[-1] == '\r' || to[-1] == '\n'))
		to--;
	content = xmlStrndup((const xmlChar *)from, (int)(to - from));
	double_quotes(content);
	node = xmlNewDocPI(tree, (const xmlChar *)"xml", content);
	xmlFree(content);
	if (!node)
		return;
	node->parent = (xmlNodePtr)tree;
	node->next = tree->children;
	if (tree->children)
		tree->children->prev = node;
	else
		tree->last = node;
	tree->children = node;
}

static const struct { const char *name; UINT page; } code_pages[] = {
	{"utf-8", CP_UTF8}, {"utf8", CP_UTF8}, {"utf-7", CP_UTF7}, {"us-ascii", 20127}, {"ascii", 20127},
	{"windows-1250", 1250}, {"windows-1251", 1251}, {"windows-1252", 1252}, {"windows-1253", 1253}, {"windows-1254", 1254},
	{"windows-1255", 1255}, {"windows-1256", 1256}, {"windows-1257", 1257}, {"windows-1258", 1258},
	{"iso-8859-1", 28591}, {"iso8859-1", 28591}, {"latin1", 28591}, {"iso-8859-2", 28592}, {"iso-8859-3", 28593}, {"iso-8859-4", 28594},
	{"iso-8859-5", 28595}, {"iso-8859-6", 28596}, {"iso-8859-7", 28597}, {"iso-8859-8", 28598}, {"iso-8859-9", 28599}, {"iso-8859-13", 28603},
	{"iso-8859-15", 28605}, {"shift_jis", 932}, {"shift-jis", 932}, {"sjis", 932}, {"euc-jp", 20932}, {"iso-2022-jp", 50220}, {"gb2312", 936},
	{"gbk", 936}, {"big5", 950}, {"euc-kr", 949}, {"ks_c_5601-1987", 949}, {"koi8-r", 20866}, {"koi8-u", 21866},
};

BOOL mx_page_of_name(const xmlChar *name, UINT *page)
{
	unsigned index;
	/* The dash after the family is optional when it is written. */
	if (!xmlStrncasecmp(name, (const xmlChar *)"iso8859-", 8))
	{
		*page = 28590 + atoi((const char *)name + 8);
		return TRUE;
	}
	for (index = 0; index < ARRAYSIZE(code_pages); index++)
		if (!xmlStrcasecmp(name, (const xmlChar *)code_pages[index].name))
		{
			*page = code_pages[index].page;
			return TRUE;
		}
	return FALSE;
}

/* The value of encoding="..." in a declaration, as a new string. */
xmlChar *mx_declared_encoding(const char *text, int size)
{
	const char *end = text + size, *at;
	for (at = text; at + 8 < end && !(at[0] == '?' && at[1] == '>'); at++)
		if (!memcmp(at, "encoding", 8))
		{
			at += 8;
			while (at < end && (*at == ' ' || *at == '=' || *at == '\t' || *at == '\r' || *at == '\n'))
				at++;
			if (at < end && (*at == '"' || *at == '\''))
			{
				const char *close = memchr(at + 1, *at, end - at - 1);
				if (close)
					return xmlStrndup((const xmlChar *)at + 1, (int)(close - at - 1));
			}
			return NULL;
		}
	return NULL;
}

/* Everything the parser reads is UTF-8 by then: the system's code pages do the decoding, not a library's own tables. */
char *mx_decode_input(const char *data, int size, int *out_size, BOOL *unknown)
{
	const WCHAR *wide = NULL;
	int wide_length = 0;
	WCHAR *swapped = NULL, *converted = NULL;
	char *result;
	int bytes;
	UINT page = 0;
	*unknown = FALSE;
	if (size >= 4 && !(size % 4) && ((data[0] == '<' && !data[1] && !data[2] && !data[3]) || (!data[0] && !data[1] && !data[2] && data[3] == '<')))
	{
		/* Four bytes a character with nothing to say which way round but the first one. */
		BOOL little = data[0] == '<';
		int index, used = 0;
		swapped = mx_alloc((size / 4 * 2 + 1) * sizeof(WCHAR));
		if (!swapped)
			return NULL;
		for (index = 0; index < size / 4; index++)
		{
			const BYTE *at = (const BYTE *)data + index * 4;
			DWORD code = little ? at[0] | at[1] << 8 | at[2] << 16 | (DWORD)at[3] << 24 : (DWORD)at[0] << 24 | at[1] << 16 | at[2] << 8 | at[3];
			if (code >= 0x10000)
			{
				code -= 0x10000;
				swapped[used++] = (WCHAR)(0xd800 + (code >> 10));
				swapped[used++] = (WCHAR)(0xdc00 + (code & 0x3ff));
			}
			else
				swapped[used++] = (WCHAR)code;
		}
		wide = swapped;
		wide_length = used;
	}
	else if (size >= 2 && (BYTE)data[0] == 0xff && (BYTE)data[1] == 0xfe)
	{
		wide = (const WCHAR *)(data + 2);
		wide_length = (size - 2) / 2;
	}
	else if (size >= 2 && (BYTE)data[0] == 0xfe && (BYTE)data[1] == 0xff)
	{
		int index;
		wide_length = (size - 2) / 2;
		swapped = mx_alloc((wide_length + 1) * sizeof(WCHAR));
		if (!swapped)
			return NULL;
		for (index = 0; index < wide_length; index++)
			swapped[index] = ((BYTE)data[2 + index * 2] << 8) | (BYTE)data[3 + index * 2];
		wide = swapped;
	}
	else if (size >= 4 && data[0] == '<' && !data[1] && data[2] && !data[3])
	{
		wide = (const WCHAR *)data;
		wide_length = size / 2;
	}
	else
	{
		xmlChar *name = mx_declared_encoding(data, size);
		if (name)
		{
			if (!xmlStrcasecmp(name, (const xmlChar *)"utf-16") || !xmlStrcasecmp(name, (const xmlChar *)"ucs-2"))
				*unknown = TRUE;
			else if (!mx_page_of_name(name, &page))
				*unknown = TRUE;
			xmlFree(name);
			if (*unknown)
				return NULL;
		}
		if (!page || page == CP_UTF8)
		{
			int skip = size >= 3 && (BYTE)data[0] == 0xef && (BYTE)data[1] == 0xbb && (BYTE)data[2] == 0xbf ? 3 : 0;
			result = mx_alloc(size - skip + 1);
			if (!result)
				return NULL;
			memcpy(result, data + skip, size - skip);
			*out_size = size - skip;
			return result;
		}
		if (page == 28591 || page == 20127)
		{
			/* Each byte is its own character, and plain ASCII has no use for the high bit. */
			int index;
			converted = mx_alloc((size + 1) * sizeof(WCHAR));
			if (!converted)
				return NULL;
			for (index = 0; index < size; index++)
				converted[index] = page == 20127 ? (BYTE)data[index] & 0x7f : (BYTE)data[index];
			wide = converted;
			wide_length = size;
		}
		else
		{
		wide_length = MultiByteToWideChar(page, 0, data, size, NULL, 0);
		if (!wide_length)
		{
			/* A code page the system lacks still reads plain ASCII the same way. */
			int index;
			for (index = 0; index < size; index++)
				if ((BYTE)data[index] >= 0x80)
				{
					*unknown = TRUE;
					return NULL;
				}
			result = mx_alloc(size + 1);
			if (result)
				memcpy(result, data, size);
			*out_size = size;
			return result;
		}
		converted = mx_alloc((wide_length + 1) * sizeof(WCHAR));
		if (!converted)
			return NULL;
		MultiByteToWideChar(page, 0, data, size, converted, wide_length);
		wide = converted;
		}
	}
	bytes = wide_length ? WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, NULL, 0, NULL, NULL) : 0;
	result = mx_alloc(bytes + 1);
	if (result && bytes)
		WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, result, bytes, NULL, NULL);
	*out_size = bytes;
	mx_free(swapped);
	mx_free(converted);
	return result;
}

/* Text nodes of nothing but blanks are not part of the tree outside preserved space. The text of an element still reads one
 * space where they were, so the place is noted on what follows them, or on the parent at its end. */
static void strip_blanks(xmlNodePtr parent)
{
	xmlNodePtr at = parent->children;
	while (at)
	{
		xmlNodePtr next = at->next;
		if (at->type == XML_ELEMENT_NODE)
			strip_blanks(at);
		else if (at->type == XML_TEXT_NODE && parent->type == XML_ELEMENT_NODE && xmlIsBlankNode(at) && at->content && *at->content &&
			xmlNodeGetSpacePreserve(parent) != 1)
		{
			xmlNodePtr earlier = mx_blank_before(at);
			if (earlier)
			{
				xmlChar *joined = xmlStrdup(earlier->content);
				joined = xmlStrcat(joined, at->content);
				xmlNodeSetContent(at, joined);
				xmlFree(joined);
			}
			at->extra = 0;
			at->psvi = NULL;
			xmlUnlinkNode(at);
			mx_tree_hold(parent->doc, at);
			if (next)
			{
				next->extra |= MX_BLANK_BEFORE;
				next->psvi = at;
			}
			else
			{
				if (parent->extra & MX_BLANK_BEFORE)
					((xmlNodePtr)parent->psvi)->psvi = at;
				else
					parent->psvi = at;
				parent->extra |= MX_BLANK_AT_END;
			}
		}
		at = next;
	}
}

/* libxml reads both spellings of an empty element the same way, and declarations apart from attributes. Both show in the source. */
typedef struct
{
	LONG depth;
	BOOL exceeded;
} parse_limits;

/* The value as the source spells it, since libxml turns every blank in one into a space. Only line ends are settled, as anywhere. */
static void keep_blanks_in_values(xmlParserCtxtPtr parser, const xmlChar *at)
{
	xmlNodePtr node = parser->node;
	const xmlChar *end = parser->input->cur;
	while (at < end)
	{
		const xmlChar *name, *value;
		xmlChar quote;
		while (at < end && (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n'))
			at++;
		name = at;
		while (at < end && *at != '=' && *at != ' ' && *at != '\t' && *at != '\r' && *at != '\n')
			at++;
		const xmlChar *name_end = at;
		while (at < end && *at != '=')
			at++;
		at++;
		while (at < end && *at != '"' && *at != '\'')
			at++;
		if (at >= end)
			return;
		quote = *at++;
		value = at;
		while (at < end && *at != quote)
			at++;
		if (at >= end)
			return;
		if (memchr(value, '\n', at - value) || memchr(value, '\r', at - value) || memchr(value, '\t', at - value))
		{
			xmlAttrPtr attribute;
			const xmlChar *colon = NULL, *scan;
			for (scan = name; scan < name_end; scan++)
				if (*scan == ':')
					colon = scan;
			for (attribute = node->properties; attribute; attribute = attribute->next)
				if (!memchr(value, '&', at - value) && !xmlStrncmp(attribute->name, colon ? colon + 1 : name, (int)(name_end - (colon ? colon + 1 : name))) &&
					!attribute->name[name_end - (colon ? colon + 1 : name)])
				{
					xmlChar *kept = xmlStrndup(value, (int)(at - value));
					mx_normalize_lines(kept);
					if (attribute->children)
						xmlNodeSetContent((xmlNodePtr)attribute->children, kept);
					xmlFree(kept);
					break;
				}
		}
		at++;
	}
}

static void on_start_element(void *context, const xmlChar *name, const xmlChar *prefix, const xmlChar *uri, int spaces, const xmlChar **declared,
	int attributes, int defaulted, const xmlChar **values)
{
	xmlParserCtxtPtr parser = context;
	parse_limits *limits = parser->_private;
	xmlSAX2StartElementNs(context, name, prefix, uri, spaces, declared, attributes, defaulted, values);
	if (limits->depth && parser->nodeNr > limits->depth)
	{
		limits->exceeded = TRUE;
		xmlStopParser(parser);
		return;
	}
	if (attributes - defaulted > 0 && parser->node)
	{
		const xmlChar *at = parser->input->cur;
		while (at > parser->input->base && *at != '<')
			at--;
		if (*at == '<')
		{
			while (*at && *at != ' ' && *at != '\t' && *at != '\r' && *at != '\n')
				at++;
			keep_blanks_in_values(parser, at);
			while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')
				at++;
			if (spaces && (xmlStrncmp(at, (const xmlChar *)"xmlns", 5) || (at[5] != '=' && at[5] != ':' && at[5] != ' ')))
				parser->node->extra |= MX_ATTRIBUTES_FIRST;
		}
	}
}

static void on_end_element(void *context, const xmlChar *name, const xmlChar *prefix, const xmlChar *uri)
{
	xmlParserCtxtPtr parser = context;
	xmlNodePtr node = parser->node;
	const xmlChar *cur = parser->input->cur;
	if (node && !node->children && !(cur - parser->input->base >= 2 && cur[-2] == '/' && cur[-1] == '>'))
		node->extra |= MX_EXPLICIT_END;
	xmlSAX2EndElementNs(context, name, prefix, uri);
}

/* The document type reads back as it was written, apart from its line ends. */
static void keep_doctype(xmlDocPtr tree, const char *data, int size)
{
	const char *end = data + size, *from = data, *at;
	char quote = 0;
	int depth = 0, to = 0;
	xmlChar *text;
	xmlNodePtr held;
	while (from + 9 <= end && memcmp(from, "<!DOCTYPE", 9))
		from++;
	if (from + 9 > end)
		return;
	for (at = from; at < end; at++)
	{
		if (quote)
			quote = *at == quote ? 0 : quote;
		else if (*at == '"' || *at == '\'')
			quote = *at;
		else if (*at == '[')
			depth++;
		else if (*at == ']')
			depth--;
		else if (*at == '>' && !depth)
			break;
	}
	if (at == end)
		return;
	text = xmlStrndup((const xmlChar *)from, (int)(at + 1 - from));
	for (from = (const char *)text; *from; from++)
		if (*from != '\r' || from[1] != '\n')
			text[to++] = *from;
	text[to] = 0;
	held = xmlNewDocComment(tree, text);
	xmlFree(text);
	if (held)
		mx_tree_hold(tree, held);
}

HRESULT mx_parse_memory(const char *data, int size, const char *encoding, mx_version version,
	BOOL preserve, BOOL resolve, BOOL prohibit, const WCHAR *url, LONG max_depth, xmlDocPtr *out, mx_error_info *error)
{
	xmlParserCtxtPtr context = xmlNewParserCtxt();
	parse_limits limits = {max_depth, FALSE};
	xmlDocPtr tree;
	int options = XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET | XML_PARSE_BIG_LINES;
	int length = 0;
	const char *declaration;
	xmlChar *location = url ? mx_to_utf8(url) : NULL;
	char *decoded = NULL;
	*out = NULL;
	if (!context)
		return E_OUTOFMEMORY;
	if (!encoding)
	{
		BOOL unknown;
		int decoded_size = 0;
		decoded = mx_decode_input(data, size, &decoded_size, &unknown);
		if (!decoded)
		{
			xmlFree(location);
			xmlFreeParserCtxt(context);
			if (error)
			{
				mx_error_clear(error);
				error->code = (LONG)0xC00CE50E;
				error->reason = SysAllocString(unknown ? L"System does not support the specified encoding.\r\n" : L"Out of memory.\r\n");
				if (url)
					error->url = SysAllocString(url);
			}
			return S_FALSE;
		}
		data = decoded;
		size = decoded_size;
	}
	encoding = "UTF-8";
	/* Entity references are kept only in the tree nodes that name them; values are read through them. */
	options |= XML_PARSE_NOENT;
	if (resolve)
		options |= XML_PARSE_DTDATTR | XML_PARSE_DTDLOAD;
	options |= XML_PARSE_IGNORE_ENC;
	declaration = find_declaration(data, size, &length);
	context->_private = &limits;
	context->sax->startElementNs = on_start_element;
	context->sax->endElementNs = on_end_element;
	tree = xmlCtxtReadMemory(context, data, size, (const char *)location, encoding, options);
	xmlFree(location);
	if (tree && !preserve)
		strip_blanks((xmlNodePtr)tree);
	if (tree && xmlGetIntSubset(tree))
		keep_doctype(tree, data, size);
	if (tree && prohibit && xmlGetIntSubset(tree))
	{
		mx_tree_free_doc(tree);
		tree = NULL;
		if (error)
		{
			mx_error_clear(error);
			error->code = (LONG)0xC00CE5A5;
			error->reason = SysAllocString(L"DTD is prohibited.\r\n");
			if (url)
				error->url = SysAllocString(url);
		}
		xmlFreeParserCtxt(context);
		mx_free(decoded);
		return S_FALSE;
	}
	if (limits.exceeded)
	{
		if (error)
		{
			mx_error_clear(error);
			error->code = (LONG)E_ABORT;
			error->reason = SysAllocString(L"The maximum element depth was exceeded.\r\n");
			if (url)
				error->url = SysAllocString(url);
		}
		if (tree)
			mx_tree_free_doc(tree);
		xmlFreeParserCtxt(context);
		mx_free(decoded);
		return S_FALSE;
	}
	if (!tree || !context->wellFormed)
	{
		if (error)
			mx_error_from_libxml(error, xmlCtxtGetLastError(context), url);
		if (tree)
			mx_tree_free_doc(tree);
		xmlFreeParserCtxt(context);
		mx_free(decoded);
		return S_FALSE;
	}
	xmlFreeParserCtxt(context);
	if (declaration)
		show_declaration(tree, declaration, length);
	mx_free(decoded);
	/* A tree without a root is no document, whatever else it held. */
	if (!xmlDocGetRootElement(tree))
	{
		xmlFreeDoc(tree);
		if (error)
		{
			mx_error_clear(error);
			error->code = (LONG)0xC00CE558;
			error->reason = SysAllocString(L"XML document must have a top level element.\r\n");
		}
		return S_FALSE;
	}
	*out = tree;
	return S_OK;
}

/* The tree the object shows is replaced; other objects over the old one keep it alive. */
static void replace_tree(mx_document *self, xmlDocPtr tree)
{
	xmlDocPtr old = tree_of(self);
	mx_tree_attach(tree, mx_tree_of(old)->version);
	mx_tree_share_properties(tree, old);
	self->base.node = (xmlNodePtr)tree;
	self->base.held = tree;
	mx_tree_ref(tree);
	mx_tree_unref(old);
	if (self->namespaces)
	{
		IXMLDOMSchemaCollection_Release(self->namespaces);
		self->namespaces = NULL;
	}
}

static void fire_handler(mx_document *self, unsigned which)
{
	DISPPARAMS none = {NULL, NULL, 0, 0};
	VARIANT result;
	if (!self->handlers[which])
		return;
	VariantInit(&result);
	IDispatch_Invoke(self->handlers[which], 0, &IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD, &none, &result, NULL, NULL);
	VariantClear(&result);
}

static void fire_points(mx_document *self)
{
	DISPPARAMS none = {NULL, NULL, 0, 0};
	unsigned kind;
	DWORD slot;
	for (kind = 0; kind < POINT_COUNT; kind++)
		for (slot = 0; slot < self->points[kind].count; slot++)
		{
			IUnknown *sink = self->points[kind].sinks[slot];
			if (!sink)
				continue;
			if (kind == POINT_NOTIFY)
				IPropertyNotifySink_OnChanged((IPropertyNotifySink *)sink, DISPID_READY_CHANGE);
			else
				IDispatch_Invoke((IDispatch *)sink, DISPID_READY_CHANGE, &IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD, &none, NULL, NULL, NULL);
		}
}

static void set_ready(mx_document *self, LONG state)
{
	self->ready = state;
	fire_handler(self, EVENT_READY);
	fire_points(self);
}

static HRESULT document_bytes(mx_document *self, xmlChar **bytes, int *size);

static HRESULT load_text(mx_document *self, const char *data, int size, const char *encoding, const WCHAR *url, VARIANT_BOOL *success)
{
	mx_tree *settings = mx_tree_of(tree_of(self));
	xmlDocPtr tree = NULL;
	HRESULT status;
	mx_error_clear(&self->error);
	set_ready(self, READYSTATE_LOADING);
	status = mx_parse_memory(data, size, encoding, settings->version, settings->props->preserve_space, settings->props->resolve_externals, settings->props->prohibit_dtd, url, settings->props->max_element_depth, &tree, &self->error);
	if (status == S_OK && mx_tree_of(tree_of(self))->props->schemas)
	{
		xmlNodePtr root = xmlDocGetRootElement(tree);
		mx_tree_attach(tree, settings->version);
		mx_tree_ref(tree);
		status = settings->props->validate_parse && mx_schema_covers(mx_tree_of(tree_of(self))->props->schemas, root) ? mx_schema_validate(mx_tree_of(tree_of(self))->props->schemas, tree, root, &self->error) : S_OK;
		if (status != S_OK)
		{
			mx_tree_unref(tree);
			tree = NULL;
			status = S_FALSE;
		}
		else
		{
			mx_tree_detach(tree);
		}
	}
	if (status == S_OK)
	{
		replace_tree(self, tree);
		if (url)
		{
			SysFreeString(mx_tree_of(tree)->url);
			mx_tree_of(tree)->url = SysAllocString(url);
		}
	}
	else
	{
		xmlDocPtr empty = xmlNewDoc(NULL);
		if (empty)
			replace_tree(self, empty);
	}
	self->modified = FALSE;
	xmlFree(self->snapshot);
	self->snapshot = NULL;
	self->snapshot_size = 0;
	if (status == S_OK)
		document_bytes(self, &self->snapshot, &self->snapshot_size);
	if (success)
		*success = status == S_OK ? VARIANT_TRUE : VARIANT_FALSE;
	set_ready(self, READYSTATE_COMPLETE);
	return status == S_OK ? S_OK : S_FALSE;
}

/* --- IXMLDOMDocument. --- */

static HRESULT WINAPI document_get_doctype(IXMLDOMDocument3 *iface, IXMLDOMDocumentType **out)
{
	xmlDtdPtr type;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	type = xmlGetIntSubset(tree_of(doc_from(iface)));
	return type ? mx_node_create((xmlNodePtr)type, (IXMLDOMNode **)out) : S_FALSE;
}

static HRESULT WINAPI document_get_implementation(IXMLDOMDocument3 *iface, IXMLDOMImplementation **out)
{
	if (!out)
		return E_INVALIDARG;
	return mx_implementation_create(out);
}

static HRESULT WINAPI document_get_documentElement(IXMLDOMDocument3 *iface, IXMLDOMElement **out)
{
	xmlNodePtr root;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	root = xmlDocGetRootElement(tree_of(doc_from(iface)));
	return root ? mx_node_create(root, (IXMLDOMNode **)out) : S_FALSE;
}

static HRESULT WINAPI document_putref_documentElement(IXMLDOMDocument3 *iface, IXMLDOMElement *element)
{
	xmlDocPtr tree = tree_of(doc_from(iface));
	xmlNodePtr given, old;
	if (!element)
		return E_INVALIDARG;
	given = mx_node_get((IUnknown *)element);
	if (!given || given->type != XML_ELEMENT_NODE)
		return E_FAIL;
	if (given->doc != tree)
		return E_FAIL;
	old = xmlDocGetRootElement(tree);
	if (old == given)
		return S_OK;
	mx_tree_adopted(given);
	xmlUnlinkNode(given);
	if (old)
	{
		xmlReplaceNode(old, given);
		mx_tree_orphan(old);
	}
	else
		mx_link_before((xmlNodePtr)tree, given, NULL);
	return S_OK;
}

static HRESULT new_orphan(xmlNodePtr node, IXMLDOMNode **out)
{
	if (!node)
		return E_OUTOFMEMORY;
	mx_tree_orphan(node);
	return mx_node_create(node, out);
}

static HRESULT new_named_element(xmlDocPtr tree, const WCHAR *name, const xmlChar *uri, IXMLDOMNode **out)
{
	xmlChar *qualified = mx_to_utf8(name), *prefix = NULL;
	const xmlChar *colon = xmlStrchr(qualified, ':');
	xmlNodePtr node;
	if (colon)
		prefix = xmlStrndup(qualified, (int)(colon - qualified));
	node = xmlNewDocNode(tree, NULL, colon ? colon + 1 : qualified, NULL);
	if (node && uri && *uri)
	{
		xmlSetNs(node, xmlNewNs(node, uri, prefix));
		node->extra |= MX_IMPLIED_DECLARATION;
	}
	else if (node && colon && !uri)
	{
		/* No namespace known yet: the prefix stays part of the plain name. */
		xmlNodeSetName(node, qualified);
	}
	xmlFree(prefix);
	xmlFree(qualified);
	return new_orphan(node, out);
}

static HRESULT WINAPI document_createElement(IXMLDOMDocument3 *iface, BSTR name, IXMLDOMElement **out)
{
	if (!out || !name)
		return E_INVALIDARG;
	if (!*name)
		return E_FAIL;
	return new_named_element(tree_of(doc_from(iface)), name, NULL, (IXMLDOMNode **)out);
}

static HRESULT WINAPI document_createDocumentFragment(IXMLDOMDocument3 *iface, IXMLDOMDocumentFragment **out)
{
	if (!out)
		return E_INVALIDARG;
	return new_orphan(xmlNewDocFragment(tree_of(doc_from(iface))), (IXMLDOMNode **)out);
}

static HRESULT new_character_node(xmlDocPtr tree, DOMNodeType kind, const WCHAR *text, IXMLDOMNode **out)
{
	xmlChar *utf8 = mx_to_utf8(text ? text : L"");
	xmlNodePtr node = NULL;
	if (kind == NODE_COMMENT && xmlStrstr(utf8, (const xmlChar *)"-->"))
	{
		xmlFree(utf8);
		return E_INVALIDARG;
	}
	mx_normalize_lines(utf8);
	switch (kind)
	{
	case NODE_TEXT: node = xmlNewDocText(tree, utf8); break;
	case NODE_COMMENT: node = xmlNewDocComment(tree, utf8); break;
	case NODE_CDATA_SECTION: node = xmlNewCDataBlock(tree, utf8, xmlStrlen(utf8)); break;
	default: break;
	}
	xmlFree(utf8);
	return new_orphan(node, out);
}

static HRESULT WINAPI document_createTextNode(IXMLDOMDocument3 *iface, BSTR text, IXMLDOMText **out)
{
	if (!out)
		return E_INVALIDARG;
	return new_character_node(tree_of(doc_from(iface)), NODE_TEXT, text, (IXMLDOMNode **)out);
}

static HRESULT WINAPI document_createComment(IXMLDOMDocument3 *iface, BSTR text, IXMLDOMComment **out)
{
	if (!out)
		return E_INVALIDARG;
	return new_character_node(tree_of(doc_from(iface)), NODE_COMMENT, text, (IXMLDOMNode **)out);
}

static HRESULT WINAPI document_createCDATASection(IXMLDOMDocument3 *iface, BSTR text, IXMLDOMCDATASection **out)
{
	if (!out)
		return E_INVALIDARG;
	return new_character_node(tree_of(doc_from(iface)), NODE_CDATA_SECTION, text, (IXMLDOMNode **)out);
}

/* The text of a declaration written by hand: known names, in quotes that match. */
static HRESULT declaration_check(const WCHAR *text)
{
	const WCHAR *at = text;
	if (!text || !*text)
		return 0xC00CE507;
	while (*at)
	{
		const WCHAR *name = at, *close;
		size_t length;
		WCHAR quote;
		while (*at && *at != L'=' && *at != L' ')
			at++;
		length = at - name;
		while (*at == L' ')
			at++;
		if (*at != L'=')
			return 0xC00CE507;
		at++;
		while (*at == L' ')
			at++;
		quote = *at;
		if (quote != L'"' && quote != L'\'')
			return 0xC00CE502;
		close = wcschr(at + 1, quote);
		if (!close)
			return 0xC00CE506;
		if (!(length == 7 && !wcsncmp(name, L"version", 7)) && !(length == 8 && !wcsncmp(name, L"encoding", 8)) && !(length == 10 && !wcsncmp(name, L"standalone", 10)))
			return 0xC00CE56C;
		at = close + 1;
		while (*at == L' ')
			at++;
	}
	return S_OK;
}

static HRESULT WINAPI document_createProcessingInstruction(IXMLDOMDocument3 *iface, BSTR target, BSTR data, IXMLDOMProcessingInstruction **out)
{
	xmlChar *name, *content;
	xmlNodePtr node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!target || !*target)
		return E_FAIL;
	if (!lstrcmpW(target, L"xml"))
	{
		HRESULT problem = declaration_check(data);
		if (FAILED(problem))
			return problem;
	}
	name = mx_to_utf8(target);
	content = mx_to_utf8(data ? data : L"");
	if (xmlStrstr(content, (const xmlChar *)"?>"))
	{
		xmlFree(name);
		xmlFree(content);
		return E_INVALIDARG;
	}
	mx_normalize_lines(content);
	if (!xmlStrcmp(name, (const xmlChar *)"xml"))
		double_quotes(content);
	node = xmlNewDocPI(tree_of(doc_from(iface)), name, content);
	xmlFree(name);
	xmlFree(content);
	return new_orphan(node, (IXMLDOMNode **)out);
}

static HRESULT new_attribute(xmlDocPtr tree, const WCHAR *name, const xmlChar *uri, IXMLDOMNode **out)
{
	xmlChar *qualified = mx_to_utf8(name);
	const xmlChar *colon = xmlStrchr(qualified, ':');
	xmlAttrPtr attribute;
	attribute = xmlNewDocProp(tree, uri && *uri && colon ? colon + 1 : qualified, NULL);
	if (attribute && uri && *uri)
	{
		/* Held on a holder node until it joins an element: an attribute cannot carry its own declaration. */
		xmlNodePtr holder = xmlNewDocNode(tree, NULL, (const xmlChar *)"namespace-holder", NULL);
		xmlChar *prefix = colon ? xmlStrndup(qualified, (int)(colon - qualified)) : NULL;
		if (holder)
		{
			attribute->ns = xmlNewNs(holder, uri, prefix);
			mx_tree_orphan(holder);
		}
		xmlFree(prefix);
	}
	xmlFree(qualified);
	return new_orphan((xmlNodePtr)attribute, out);
}

static HRESULT WINAPI document_createAttribute(IXMLDOMDocument3 *iface, BSTR name, IXMLDOMAttribute **out)
{
	if (!out || !name)
		return E_INVALIDARG;
	if (!*name)
		return E_FAIL;
	return new_attribute(tree_of(doc_from(iface)), name, NULL, (IXMLDOMNode **)out);
}

static HRESULT WINAPI document_createEntityReference(IXMLDOMDocument3 *iface, BSTR name, IXMLDOMEntityReference **out)
{
	xmlChar *utf8;
	xmlNodePtr node;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!name || !*name)
		return E_FAIL;
	if (wcschr(name, L':'))
		return E_FAIL;
	utf8 = mx_to_utf8(name);
	node = xmlNewReference(tree_of(doc_from(iface)), utf8);
	xmlFree(utf8);
	return new_orphan(node, (IXMLDOMNode **)out);
}

static HRESULT WINAPI document_getElementsByTagName(IXMLDOMDocument3 *iface, BSTR name, IXMLDOMNodeList **out)
{
	if (!name || !out)
		return E_INVALIDARG;
	return mx_select_by_name(doc_from(iface)->base.node, name, out);
}

static HRESULT WINAPI document_createNode(IXMLDOMDocument3 *iface, VARIANT kind, BSTR name, BSTR uri, IXMLDOMNode **out)
{
	mx_document *self = doc_from(iface);
	xmlDocPtr tree = tree_of(self);
	VARIANT number;
	DOMNodeType type;
	xmlChar *space = NULL;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	VariantInit(&number);
	if (V_VT(&kind) == VT_BSTR)
	{
		static const struct { const WCHAR *name; DOMNodeType type; } names[] = {
			{L"element", NODE_ELEMENT}, {L"attribute", NODE_ATTRIBUTE}, {L"text", NODE_TEXT}, {L"cdatasection", NODE_CDATA_SECTION},
			{L"entityreference", NODE_ENTITY_REFERENCE}, {L"processinginstruction", NODE_PROCESSING_INSTRUCTION}, {L"comment", NODE_COMMENT},
			{L"documentfragment", NODE_DOCUMENT_FRAGMENT},
		};
		unsigned index;
		type = NODE_INVALID;
		for (index = 0; index < ARRAYSIZE(names); index++)
			if (!lstrcmpiW(V_BSTR(&kind), names[index].name))
				type = names[index].type;
		if (type == NODE_INVALID)
		{
			status = VariantChangeType(&number, &kind, 0, VT_I4);
			if (FAILED(status))
				return E_INVALIDARG;
			type = V_I4(&number);
		}
	}
	else
	{
		status = VariantChangeType(&number, &kind, 0, VT_I4);
		if (FAILED(status))
			return E_INVALIDARG;
		type = V_I4(&number);
	}
	if ((type == NODE_ELEMENT || type == NODE_ATTRIBUTE || type == NODE_ENTITY_REFERENCE || type == NODE_PROCESSING_INSTRUCTION) && (!name || !*name))
		return E_FAIL;
	if (uri)
		space = mx_to_utf8(uri);
	switch (type)
	{
	case NODE_ELEMENT:
		status = new_named_element(tree, name, space, out);
		break;
	case NODE_ATTRIBUTE:
		status = new_attribute(tree, name, space, out);
		break;
	case NODE_TEXT:
	case NODE_COMMENT:
	case NODE_CDATA_SECTION:
		status = new_character_node(tree, type, L"", out);
		break;
	case NODE_ENTITY_REFERENCE:
		if (space && *space)
		{
			status = E_FAIL;
			break;
		}
		status = document_createEntityReference(iface, name, (IXMLDOMEntityReference **)out);
		break;
	case NODE_PROCESSING_INSTRUCTION:
		status = document_createProcessingInstruction(iface, name, NULL, (IXMLDOMProcessingInstruction **)out);
		break;
	case NODE_DOCUMENT_FRAGMENT:
		status = document_createDocumentFragment(iface, (IXMLDOMDocumentFragment **)out);
		break;
	default:
		status = E_INVALIDARG;
		break;
	}
	xmlFree(space);
	return status;
}

static HRESULT WINAPI document_nodeFromID(IXMLDOMDocument3 *iface, BSTR id, IXMLDOMNode **out)
{
	xmlChar *utf8;
	xmlAttrPtr attribute;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!id)
		return E_INVALIDARG;
	utf8 = mx_to_utf8(id);
	attribute = xmlGetID(tree_of(doc_from(iface)), utf8);
	xmlFree(utf8);
	return attribute ? mx_node_create(attribute->parent, out) : S_FALSE;
}

static HRESULT load_bytes(mx_document *self, const BYTE *bytes, ULONG size, VARIANT_BOOL *success)
{
	return load_text(self, (const char *)bytes, (int)size, NULL, NULL, success);
}

static HRESULT WINAPI document_load(IXMLDOMDocument3 *iface, VARIANT source, VARIANT_BOOL *success)
{
	mx_document *self = doc_from(iface);
	VARIANT *from = &source;
	HRESULT status;
	if (success)
		*success = VARIANT_FALSE;
	while (V_VT(from) == (VT_VARIANT | VT_BYREF) && V_VARIANTREF(from))
		from = V_VARIANTREF(from);
	switch (V_VT(from))
	{
	case VT_BSTR:
	case VT_BSTR | VT_BYREF:
		{
			const WCHAR *url;
			char *data = NULL;
			ULONG size = 0;
			if (V_VT(from) != VT_BSTR && !V_BSTRREF(from))
				return E_INVALIDARG;
			url = V_VT(from) == VT_BSTR ? V_BSTR(from) : *V_BSTRREF(from);
			mx_error_clear(&self->error);
			status = url && *url ? mx_read_url(url, &data, &size) : E_FAIL;
			if (FAILED(status))
			{
				self->error.code = (LONG)status;
				self->error.reason = SysAllocString(L"The system cannot locate the object specified.\r\n");
				if (url)
					self->error.url = SysAllocString(url);
				set_ready(self, READYSTATE_COMPLETE);
				return S_FALSE;
			}
			status = load_text(self, data, (int)size, NULL, url, success);
			mx_free(data);
			return status;
		}
	case VT_ARRAY | VT_UI1:
		{
			void *data;
			LONG low = 0, high = -1;
			if (SafeArrayGetDim(V_ARRAY(from)) != 1)
				return E_INVALIDARG;
			SafeArrayGetLBound(V_ARRAY(from), 1, &low);
			SafeArrayGetUBound(V_ARRAY(from), 1, &high);
			status = SafeArrayAccessData(V_ARRAY(from), &data);
			if (FAILED(status))
				return status;
			status = load_bytes(self, data, (ULONG)(high - low + 1), success);
			SafeArrayUnaccessData(V_ARRAY(from));
			return status;
		}
	case VT_UNKNOWN:
	case VT_DISPATCH:
		{
			IUnknown *object = V_UNKNOWN(from);
			IPersistStream *persist;
			IXMLDOMDocument *other;
			ISequentialStream *sequential;
			if (!object)
				return E_INVALIDARG;
			/* Another document: read its markup. */
			if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IXMLDOMDocument, (void **)&other)))
			{
				BSTR text;
				status = IXMLDOMDocument_get_xml(other, &text);
				IXMLDOMDocument_Release(other);
				if (SUCCEEDED(status))
				{
					xmlChar *utf8 = mx_to_utf8(text);
					status = load_text(self, (const char *)utf8, xmlStrlen(utf8), "UTF-8", NULL, success);
					xmlFree(utf8);
					SysFreeString(text);
				}
				return status;
			}
			if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IStream, (void **)&sequential)) ||
				SUCCEEDED(IUnknown_QueryInterface(object, &IID_ISequentialStream, (void **)&sequential)))
			{
				char *data;
				ULONG size;
				status = mx_read_stream((IStream *)sequential, &data, &size);
				ISequentialStream_Release(sequential);
				if (FAILED(status))
					return status;
				status = load_bytes(self, (const BYTE *)data, size, success);
				mx_free(data);
				return status;
			}
			if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IPersistStream, (void **)&persist)))
			{
				IStream *stream;
				status = CreateStreamOnHGlobal(NULL, TRUE, &stream);
				if (SUCCEEDED(status))
				{
					LARGE_INTEGER start = {{0}};
					status = IPersistStream_Save(persist, stream, TRUE);
					IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
					if (SUCCEEDED(status))
					{
						char *data;
						ULONG size;
						status = mx_read_stream(stream, &data, &size);
						if (SUCCEEDED(status))
						{
							status = load_bytes(self, (const BYTE *)data, size, success);
							mx_free(data);
						}
					}
					IStream_Release(stream);
				}
				IPersistStream_Release(persist);
				return status;
			}
			return E_FAIL;
		}
	default:
		return E_INVALIDARG;
	}
}

static HRESULT WINAPI document_get_readyState(IXMLDOMDocument3 *iface, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = doc_from(iface)->ready;
	return S_OK;
}

static HRESULT WINAPI document_get_parseError(IXMLDOMDocument3 *iface, IXMLDOMParseError **out)
{
	if (!out)
		return E_INVALIDARG;
	return mx_parse_error_create(&doc_from(iface)->error, out);
}

static HRESULT WINAPI document_get_url(IXMLDOMDocument3 *iface, BSTR *out)
{
	BSTR url = mx_tree_of(tree_of(doc_from(iface)))->url;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!url)
		return S_FALSE;
	*out = SysAllocString(url);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI document_get_async(IXMLDOMDocument3 *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = doc_from(iface)->async;
	return S_OK;
}

static HRESULT WINAPI document_put_async(IXMLDOMDocument3 *iface, VARIANT_BOOL value)
{
	doc_from(iface)->async = value;
	return S_OK;
}

static HRESULT WINAPI document_abort(IXMLDOMDocument3 *iface)
{
	return S_OK;
}

/* Text handed in never counts its own encoding: it is already wide characters. Leading white space
 * is forgiven, which a parser given the same bytes would not do. */
static BOOL self_version_old(mx_document *self)
{
	mx_version version = mx_tree_of(tree_of(self))->version;
	return version == MX_1 || version == MX_26;
}

static HRESULT WINAPI document_loadXML(IXMLDOMDocument3 *iface, BSTR text, VARIANT_BOOL *success)
{
	mx_document *self = doc_from(iface);
	const WCHAR *from = text;
	xmlChar *utf8;
	HRESULT status;
	if (success)
		*success = VARIANT_FALSE;
	if (from)
		while (*from == L' ' || *from == L'\t' || *from == L'\r' || *from == L'\n')
			from++;
	if (!from || !*from)
	{
		xmlDocPtr empty = xmlNewDoc(NULL);
		mx_error_clear(&self->error);
		self->error.code = (LONG)0xC00CE558;
		self->error.reason = SysAllocString(L"XML document must have a top level element.\r\n");
		if (empty)
			replace_tree(self, empty);
		set_ready(self, READYSTATE_COMPLETE);
		return S_FALSE;
	}
	/* Only the older classes let blanks stand before a declaration. */
	if (self_version_old(self))
		text = (BSTR)from;
	utf8 = mx_to_utf8(text);
	status = load_text(self, (const char *)utf8, xmlStrlen(utf8), "UTF-8", NULL, success);
	xmlFree(utf8);
	return status;
}

/* The text to store: the encoding the declaration names, or plain UTF-8 without any. */
static HRESULT document_bytes(mx_document *self, xmlChar **bytes, int *size)
{
	xmlDocPtr tree = tree_of(self);
	xmlBufferPtr buffer = xmlBufferCreate();
	xmlSaveCtxtPtr writer;
	xmlChar *encoding = NULL;
	xmlNodePtr first = tree->children;
	if (!buffer)
		return E_OUTOFMEMORY;
	if (first && first->type == XML_PI_NODE && !xmlStrcmp(first->name, (const xmlChar *)"xml") && first->content)
	{
		const xmlChar *at = xmlStrstr(first->content, (const xmlChar *)"encoding");
		if (at)
		{
			at += 8;
			while (*at == ' ' || *at == '=')
				at++;
			if (*at == '"' || *at == '\'')
			{
				const xmlChar *close = xmlStrchr(at + 1, *at);
				if (close)
					encoding = xmlStrndup(at + 1, (int)(close - at - 1));
			}
		}
	}
	writer = xmlSaveToBuffer(buffer, "UTF-8", XML_SAVE_NO_DECL | XML_SAVE_AS_XML);
	if (!writer)
	{
		xmlFree(encoding);
		xmlBufferFree(buffer);
		return E_FAIL;
	}
	xmlSaveDoc(writer, tree);
	xmlSaveClose(writer);
	{
		int length = xmlBufferLength(buffer), index, breaks = 0, to = 0;
		const xmlChar *text = xmlBufferContent(buffer);
		xmlChar *ends;
		for (index = 0; index < length; index++)
			if (text[index] == '\n' && (!index || text[index - 1] != '\r'))
				breaks++;
		ends = xmlMalloc(length + breaks + 1);
		for (index = 0; index < length; index++)
		{
			if (text[index] == '\n' && (!index || text[index - 1] != '\r'))
				ends[to++] = '\r';
			ends[to++] = text[index];
		}
		*bytes = ends;
		*size = to;
	}
	xmlBufferFree(buffer);
	if (encoding && (!xmlStrcasecmp(encoding, (const xmlChar *)"utf-16") || !xmlStrcasecmp(encoding, (const xmlChar *)"ucs-2")))
	{
		int wide_length = MultiByteToWideChar(CP_UTF8, 0, (const char *)*bytes, *size, NULL, 0);
		xmlChar *out = xmlMalloc((wide_length + 1) * 2);
		out[0] = 0xff;
		out[1] = 0xfe;
		MultiByteToWideChar(CP_UTF8, 0, (const char *)*bytes, *size, (WCHAR *)(out + 2), wide_length);
		xmlFree(*bytes);
		*bytes = out;
		*size = (wide_length + 1) * 2;
	}
	else
	{
		UINT page;
		if (encoding && mx_page_of_name(encoding, &page) && page != CP_UTF8)
		{
			int wide_length = MultiByteToWideChar(CP_UTF8, 0, (const char *)*bytes, *size, NULL, 0), narrow;
			WCHAR *wide = mx_alloc((wide_length + 1) * sizeof(WCHAR));
			xmlChar *out;
			MultiByteToWideChar(CP_UTF8, 0, (const char *)*bytes, *size, wide, wide_length);
			narrow = WideCharToMultiByte(page, 0, wide, wide_length, NULL, 0, NULL, NULL);
			out = xmlMalloc(narrow + 1);
			WideCharToMultiByte(page, 0, wide, wide_length, (char *)out, narrow, NULL, NULL);
			mx_free(wide);
			xmlFree(*bytes);
			*bytes = out;
			*size = narrow;
		}
	}
	xmlFree(encoding);
	return S_OK;
}

static HRESULT WINAPI document_save(IXMLDOMDocument3 *iface, VARIANT target)
{
	mx_document *self = doc_from(iface);
	xmlChar *bytes;
	int size;
	HRESULT status;
	if (!xmlDocGetRootElement(tree_of(self)) && !tree_of(self)->children)
		return (HRESULT)0xC00CE00F;
	if (V_VT(&target) == (VT_BSTR | VT_BYREF))
	{
		VARIANT direct;
		V_VT(&direct) = VT_BSTR;
		V_BSTR(&direct) = *V_BSTRREF(&target);
		return document_save(iface, direct);
	}
	switch (V_VT(&target))
	{
	case VT_BSTR:
		{
			HANDLE file;
			DWORD written;
			status = document_bytes(self, &bytes, &size);
			if (FAILED(status))
				return status;
			file = CreateFileW(V_BSTR(&target), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (file == INVALID_HANDLE_VALUE)
			{
				xmlFree(bytes);
				return HRESULT_FROM_WIN32(GetLastError());
			}
			status = WriteFile(file, bytes, size, &written, NULL) ? S_OK : E_FAIL;
			CloseHandle(file);
			xmlFree(bytes);
			return status;
		}
	case VT_UNKNOWN:
	case VT_DISPATCH:
		{
			IXMLDOMDocument *other;
			IStream *stream;
			if (!V_UNKNOWN(&target))
				return E_INVALIDARG;
			if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(&target), &IID_IXMLDOMDocument, (void **)&other)))
			{
				BSTR text;
				VARIANT_BOOL ok;
				status = mx_serialize(self->base.node, &text);
				if (SUCCEEDED(status))
				{
					status = IXMLDOMDocument_loadXML(other, text, &ok);
					SysFreeString(text);
				}
				IXMLDOMDocument_Release(other);
				return status;
			}
			if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(&target), &IID_IStream, (void **)&stream)))
			{
				ULONG written;
				status = document_bytes(self, &bytes, &size);
				if (SUCCEEDED(status))
				{
					status = IStream_Write(stream, bytes, size, &written);
					xmlFree(bytes);
				}
				IStream_Release(stream);
				return status;
			}
			return E_FAIL;
		}
	default:
		return E_INVALIDARG;
	}
}

#define BOOL_PROPERTY(Name, field) \
	static HRESULT WINAPI document_get_##Name(IXMLDOMDocument3 *iface, VARIANT_BOOL *out) \
	{ \
		if (!out) \
			return E_INVALIDARG; \
		*out = mx_tree_of(tree_of(doc_from(iface)))->props->field ? VARIANT_TRUE : VARIANT_FALSE; \
		return S_OK; \
	} \
	static HRESULT WINAPI document_put_##Name(IXMLDOMDocument3 *iface, VARIANT_BOOL value) \
	{ \
		mx_tree_of(tree_of(doc_from(iface)))->props->field = value != VARIANT_FALSE; \
		return S_OK; \
	}

BOOL_PROPERTY(validateOnParse, validate_parse)
BOOL_PROPERTY(resolveExternals, resolve_externals)
BOOL_PROPERTY(preserveWhiteSpace, preserve_space)

static HRESULT set_handler(mx_document *self, unsigned which, VARIANT value)
{
	IDispatch *handler = NULL;
	if (V_VT(&value) == VT_DISPATCH)
		handler = V_DISPATCH(&value);
	else if (V_VT(&value) == (VT_DISPATCH | VT_BYREF))
		handler = *V_DISPATCHREF(&value);
	else if (V_VT(&value) == VT_UNKNOWN)
	{
		if (V_UNKNOWN(&value))
		{
			HRESULT status = IUnknown_QueryInterface(V_UNKNOWN(&value), &IID_IDispatch, (void **)&handler);
			if (FAILED(status))
				return E_INVALIDARG;
			IDispatch_Release(handler);
		}
	}
	else
		return DISP_E_TYPEMISMATCH;
	if (handler)
		IDispatch_AddRef(handler);
	if (self->handlers[which])
		IDispatch_Release(self->handlers[which]);
	self->handlers[which] = handler;
	return S_OK;
}

static HRESULT WINAPI document_put_onreadystatechange(IXMLDOMDocument3 *iface, VARIANT value)
{
	return set_handler(doc_from(iface), EVENT_READY, value);
}

static HRESULT WINAPI document_put_ondataavailable(IXMLDOMDocument3 *iface, VARIANT value)
{
	return set_handler(doc_from(iface), EVENT_DATA, value);
}

static HRESULT WINAPI document_put_ontransformnode(IXMLDOMDocument3 *iface, VARIANT value)
{
	return set_handler(doc_from(iface), EVENT_TRANSFORM, value);
}

/* --- IXMLDOMDocument2. --- */

static HRESULT WINAPI document_get_namespaces(IXMLDOMDocument3 *iface, IXMLDOMSchemaCollection **out)
{
	mx_document *self = doc_from(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	/* One view for each loaded tree: this document keeps it until the next one. */
	if (!self->namespaces)
	{
		HRESULT status = mx_namespace_view_create(tree_of(self), &self->namespaces);
		if (FAILED(status))
			return status;
	}
	*out = self->namespaces;
	IXMLDOMSchemaCollection_AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI document_get_schemas(IXMLDOMDocument3 *iface, VARIANT *out)
{
	mx_document *self = doc_from(iface);
	if (!out)
		return E_INVALIDARG;
	if (!mx_tree_of(tree_of(self))->props->schemas)
	{
		V_VT(out) = VT_NULL;
		V_DISPATCH(out) = NULL;
		return S_FALSE;
	}
	V_VT(out) = VT_DISPATCH;
	return IUnknown_QueryInterface(mx_tree_of(tree_of(self))->props->schemas, &IID_IDispatch, (void **)&V_DISPATCH(out));
}

static HRESULT WINAPI document_putref_schemas(IXMLDOMDocument3 *iface, VARIANT value)
{
	mx_document *self = doc_from(iface);
	IXMLDOMSchemaCollection *cache = NULL;
	switch (V_VT(&value))
	{
	case VT_UNKNOWN:
	case VT_DISPATCH:
		if (V_UNKNOWN(&value) && FAILED(IUnknown_QueryInterface(V_UNKNOWN(&value), &IID_IXMLDOMSchemaCollection, (void **)&cache)))
			return E_FAIL;
		break;
	case VT_NULL:
	case VT_EMPTY:
		break;
	default:
		return E_FAIL;
	}
	if (mx_tree_of(tree_of(self))->props->schemas)
		IUnknown_Release(mx_tree_of(tree_of(self))->props->schemas);
	mx_tree_of(tree_of(self))->props->schemas = (IUnknown *)cache;
	return S_OK;
}

static HRESULT validate_node(mx_document *self, xmlNodePtr node, IXMLDOMParseError **out)
{
	mx_error_info error = {0};
	HRESULT status = S_OK;
	xmlDtdPtr type = xmlGetIntSubset(tree_of(self));
	if (out)
		*out = NULL;
	if (mx_tree_of(tree_of(self))->props->schemas)
		status = mx_schema_validate(mx_tree_of(tree_of(self))->props->schemas, tree_of(self), node, &error);
	else if (type)
	{
		xmlValidCtxtPtr check = xmlNewValidCtxt();
		int good = node->type == XML_DOCUMENT_NODE ? xmlValidateDocument(check, tree_of(self)) : xmlValidateElement(check, tree_of(self), node);
		xmlFreeValidCtxt(check);
		if (!good)
		{
			const xmlError *last = xmlGetLastError();
			mx_error_from_libxml(&error, last, NULL);
			status = S_FALSE;
		}
	}
	else
	{
		error.code = (LONG)0xC00CE223;
		error.reason = SysAllocString(L"Validate failed because the document does not contain exactly one root node.\r\n");
		if (xmlDocGetRootElement(tree_of(self)))
		{
			SysFreeString(error.reason);
			error.code = (LONG)0xC00CE224;
			error.reason = SysAllocString(L"Validate failed because a DTD or schema was not specified in the document.\r\n");
		}
		status = S_FALSE;
	}
	if (out)
		mx_parse_error_create(&error, out);
	mx_error_clear(&error);
	return status;
}

static HRESULT WINAPI document_validate(IXMLDOMDocument3 *iface, IXMLDOMParseError **out)
{
	mx_document *self = doc_from(iface);
	return validate_node(self, self->base.node, out);
}

static BOOL variant_truth(const VARIANT *value)
{
	VARIANT flag;
	VariantInit(&flag);
	if (FAILED(VariantChangeType(&flag, (VARIANT *)value, 0, VT_BOOL)))
		return FALSE;
	return V_BOOL(&flag) != VARIANT_FALSE;
}

void mx_tree_set_namespaces(mx_tree *data, const xmlChar *text)
{
	mx_select_ns *space;
	const xmlChar *at = text;
	while ((space = data->props->namespaces))
	{
		data->props->namespaces = space->next;
		xmlFree(space->prefix);
		xmlFree(space->uri);
		mx_free(space);
	}
	xmlFree(data->props->selection_namespaces);
	data->props->selection_namespaces = xmlStrdup(text);
	while (at && *at)
	{
		const xmlChar *equal, *close;
		xmlChar quote;
		while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')
			at++;
		if (xmlStrncmp(at, (const xmlChar *)"xmlns", 5))
			break;
		at += 5;
		space = mx_zalloc(sizeof(*space));
		if (*at == ':')
		{
			equal = xmlStrchr(++at, '=');
			if (!equal)
			{
				mx_free(space);
				break;
			}
			space->prefix = xmlStrndup(at, (int)(equal - at));
		}
		else
			equal = xmlStrchr(at, '=');
		if (!equal || (equal[1] != '"' && equal[1] != '\''))
		{
			xmlFree(space->prefix);
			mx_free(space);
			break;
		}
		quote = equal[1];
		close = xmlStrchr(equal + 2, quote);
		if (!close)
		{
			xmlFree(space->prefix);
			mx_free(space);
			break;
		}
		space->uri = xmlStrndup(equal + 2, (int)(close - equal - 2));
		space->next = data->props->namespaces;
		data->props->namespaces = space;
		at = close + 1;
	}
}

static BOOL namespaces_well_formed(const WCHAR *text)
{
	const WCHAR *at = text;
	while (*at)
	{
		const WCHAR *close;
		WCHAR quote;
		while (*at == L' ' || *at == L'\t' || *at == L'\r' || *at == L'\n')
			at++;
		if (!*at)
			return TRUE;
		if (wcsncmp(at, L"xmlns", 5))
			return FALSE;
		at += 5;
		if (*at == L':')
			while (*at && *at != L'=' && *at != L' ')
				at++;
		if (*at != L'=')
			return FALSE;
		quote = at[1];
		if (quote != L'"' && quote != L'\'')
			return FALSE;
		close = wcschr(at + 2, quote);
		if (!close)
			return FALSE;
		at = close + 1;
	}
	return TRUE;
}

static HRESULT WINAPI document_setProperty(IXMLDOMDocument3 *iface, BSTR name, VARIANT value)
{
	mx_tree *data = mx_tree_of(tree_of(doc_from(iface)));
	if (!name)
		return E_INVALIDARG;
	if (!lstrcmpiW(name, L"SelectionLanguage"))
	{
		VARIANT *from = V_VT(&value) == (VT_BSTR | VT_BYREF) ? NULL : &value;
		const WCHAR *text = from ? (V_VT(from) == VT_BSTR ? V_BSTR(from) : NULL) : *V_BSTRREF(&value);
		if (!text)
			return E_FAIL;
		if (!lstrcmpiW(text, L"XPath"))
			data->props->xpath = TRUE;
		else if (!lstrcmpiW(text, L"XSLPattern"))
			data->props->xpath = FALSE;
		else
			return E_FAIL;
		return S_OK;
	}
	if (!lstrcmpiW(name, L"SelectionNamespaces"))
	{
		const WCHAR *text = V_VT(&value) == VT_BSTR ? V_BSTR(&value) : V_VT(&value) == (VT_BSTR | VT_BYREF) ? *V_BSTRREF(&value) : NULL;
		xmlChar *utf8;
		if (V_VT(&value) != VT_BSTR && V_VT(&value) != (VT_BSTR | VT_BYREF))
			return E_FAIL;
		utf8 = mx_to_utf8(text ? text : L"");
		/* The rejected text still reads back; the prefixes known so far stay. */
		if (text && !namespaces_well_formed(text))
		{
			xmlFree(data->props->selection_namespaces);
			data->props->selection_namespaces = utf8;
			return E_FAIL;
		}
		mx_tree_set_namespaces(data, utf8);
		xmlFree(utf8);
		return S_OK;
	}
	if (!lstrcmpiW(name, L"ProhibitDTD")) { data->props->prohibit_dtd = variant_truth(&value); return S_OK; }
	if (!lstrcmpiW(name, L"NewParser")) { data->props->new_parser = variant_truth(&value); return S_OK; }
	if (!lstrcmpiW(name, L"MultipleErrorMessages")) { data->props->multiple_errors = variant_truth(&value); return S_OK; }
	if (!wcscmp(name, L"MaxElementDepth"))
	{
		VARIANT number;
		HRESULT status;
		VariantInit(&number);
		status = VariantChangeType(&number, &value, 0, VT_I4);
		if (FAILED(status) || V_I4(&number) < 0)
			return E_INVALIDARG;
		data->props->max_element_depth = V_I4(&number);
		return S_OK;
	}
	if (!lstrcmpiW(name, L"ValidateOnParse") && data->version >= MX_4) { data->props->validate_parse = variant_truth(&value); return S_OK; }
	if (!lstrcmpiW(name, L"ResolveExternals")) { data->props->resolve_externals = variant_truth(&value); return S_OK; }
	if (!lstrcmpiW(name, L"AllowDocumentFunction")) { data->props->allow_document_function = variant_truth(&value); return S_OK; }
	if (!lstrcmpiW(name, L"AllowXsltScript")) { data->props->allow_xslt_script = variant_truth(&value); return S_OK; }
	if (!lstrcmpiW(name, L"NormalizeAttributeValues")) { data->props->normalize_attributes = variant_truth(&value); return S_OK; }
	if (!lstrcmpiW(name, L"MaxElementDepth") || !lstrcmpiW(name, L"MaxXMLSize") || !lstrcmpiW(name, L"ForcedResync") ||
		!lstrcmpiW(name, L"UseInlineSchema") || !lstrcmpiW(name, L"ServerHTTPRequest"))
		return S_OK;
	return E_FAIL;
}

static HRESULT WINAPI document_getProperty(IXMLDOMDocument3 *iface, BSTR name, VARIANT *out)
{
	mx_tree *data = mx_tree_of(tree_of(doc_from(iface)));
	if (!name || !out)
		return E_INVALIDARG;
	if (!wcscmp(name, L"MaxElementDepth"))
	{
		V_VT(out) = VT_I4;
		V_I4(out) = data->props->max_element_depth;
		return S_OK;
	}
	if (!lstrcmpiW(name, L"SelectionLanguage"))
	{
		V_VT(out) = VT_BSTR;
		V_BSTR(out) = SysAllocString(data->props->xpath ? L"XPath" : L"XSLPattern");
		return S_OK;
	}
	if (!lstrcmpiW(name, L"SelectionNamespaces"))
	{
		V_VT(out) = VT_BSTR;
		V_BSTR(out) = data->props->selection_namespaces ? mx_bstr(data->props->selection_namespaces) : SysAllocString(L"");
		return S_OK;
	}
#define BOOL_VALUE(text, field) \
	if (!lstrcmpiW(name, text)) \
	{ \
		V_VT(out) = VT_BOOL; \
		V_BOOL(out) = data->props->field ? VARIANT_TRUE : VARIANT_FALSE; \
		return S_OK; \
	}
	BOOL_VALUE(L"ProhibitDTD", prohibit_dtd)
	BOOL_VALUE(L"NewParser", new_parser)
	BOOL_VALUE(L"MultipleErrorMessages", multiple_errors)
	if (data->version >= MX_4)
		BOOL_VALUE(L"ValidateOnParse", validate_parse)
	BOOL_VALUE(L"ResolveExternals", resolve_externals)
	BOOL_VALUE(L"AllowDocumentFunction", allow_document_function)
	BOOL_VALUE(L"AllowXsltScript", allow_xslt_script)
	BOOL_VALUE(L"NormalizeAttributeValues", normalize_attributes)
#undef BOOL_VALUE
	return E_FAIL;
}

/* --- IXMLDOMDocument3. --- */

static HRESULT WINAPI document_validateNode(IXMLDOMDocument3 *iface, IXMLDOMNode *node, IXMLDOMParseError **out)
{
	xmlNodePtr given;
	if (!node)
		return E_INVALIDARG;
	given = mx_node_get((IUnknown *)node);
	if (!given || given->doc != tree_of(doc_from(iface)))
		return E_FAIL;
	return validate_node(doc_from(iface), given, out);
}

static HRESULT WINAPI document_importNode(IXMLDOMDocument3 *iface, IXMLDOMNode *node, VARIANT_BOOL deep, IXMLDOMNode **out)
{
	xmlDocPtr tree = tree_of(doc_from(iface));
	xmlNodePtr given, copy;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!node)
		return E_INVALIDARG;
	given = mx_node_get((IUnknown *)node);
	if (!given)
		return E_INVALIDARG;
	if (given->type == XML_DOCUMENT_NODE || given->type == XML_DTD_NODE)
		return E_FAIL;
	copy = xmlDocCopyNode(given, tree, deep ? 1 : 2);
	if (!copy)
		return E_FAIL;
	return new_orphan(copy, out);
}

/* --- The extra interfaces. --- */

#define OTHER_FACE(Name, Iface, field) \
	static mx_document *Name##_self(Iface *iface) { return CONTAINING_RECORD(iface, mx_document, field); } \
	static HRESULT WINAPI Name##_QueryInterface(Iface *iface, REFIID riid, void **out) \
	{ return nd_QueryInterface((IXMLDOMNode *)Name##_self(iface), riid, out); } \
	static ULONG WINAPI Name##_AddRef(Iface *iface) { return nd_AddRef((IXMLDOMNode *)Name##_self(iface)); } \
	static ULONG WINAPI Name##_Release(Iface *iface) { return nd_Release((IXMLDOMNode *)Name##_self(iface)); }

OTHER_FACE(persist, IPersistStreamInit, persist_vtbl)
OTHER_FACE(site, IObjectWithSite, site_vtbl)
OTHER_FACE(safety, IObjectSafety, safety_vtbl)
OTHER_FACE(points, IConnectionPointContainer, points_vtbl)

static const GUID class_of_document = {0x2933bf90, 0x7b36, 0x11d2, {0xb2, 0x0e, 0x00, 0xc0, 0x4f, 0x98, 0x3e, 0x60}};

static HRESULT WINAPI persist_GetClassID(IPersistStreamInit *iface, CLSID *out)
{
	if (!out)
		return E_POINTER;
	switch (mx_tree_of(tree_of(persist_self(iface)))->version)
	{
	case MX_1: *out = class_of_document; break;
	case MX_26: *out = CLSID_DOMDocument26; break;
	case MX_3: *out = CLSID_DOMDocument30; break;
	case MX_4: *out = CLSID_DOMDocument40; break;
	case MX_6: *out = CLSID_DOMDocument60; break;
	default: *out = CLSID_DOMDocument; break;
	}
	return S_OK;
}

static HRESULT WINAPI persist_IsDirty(IPersistStreamInit *iface)
{
	return persist_self(iface)->modified ? S_OK : S_FALSE;
}

static HRESULT WINAPI persist_Load(IPersistStreamInit *iface, IStream *stream)
{
	mx_document *self = persist_self(iface);
	char *data;
	ULONG size;
	VARIANT_BOOL success;
	HRESULT status;
	if (!stream)
		return E_INVALIDARG;
	status = mx_read_stream(stream, &data, &size);
	if (FAILED(status))
		return status;
	status = load_bytes(self, (const BYTE *)data, size, &success);
	mx_free(data);
	return status == S_OK ? S_OK : E_FAIL;
}

static HRESULT WINAPI persist_Save(IPersistStreamInit *iface, IStream *stream, BOOL clear_dirty)
{
	mx_document *self = persist_self(iface);
	xmlChar *bytes;
	int size;
	ULONG written;
	HRESULT status;
	if (!stream)
		return E_INVALIDARG;
	status = document_bytes(self, &bytes, &size);
	if (FAILED(status))
		return status;
	if (size < 5 || memcmp(bytes, "<?xml", 5))
		status = IStream_Write(stream, "<?xml version=\"1.0\"?>\r\n", 23, &written);
	if (SUCCEEDED(status))
		status = IStream_Write(stream, bytes, size, &written);
	xmlFree(bytes);
	if (SUCCEEDED(status) && clear_dirty)
		self->modified = FALSE;
	return status;
}

static HRESULT WINAPI persist_GetSizeMax(IPersistStreamInit *iface, ULARGE_INTEGER *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI persist_InitNew(IPersistStreamInit *iface)
{
	mx_document *self = persist_self(iface);
	xmlDocPtr empty = xmlNewDoc(NULL);
	if (!empty)
		return E_OUTOFMEMORY;
	replace_tree(self, empty);
	return S_OK;
}

static const IPersistStreamInitVtbl persist_table = {
	persist_QueryInterface, persist_AddRef, persist_Release, persist_GetClassID,
	persist_IsDirty, persist_Load, persist_Save, persist_GetSizeMax, persist_InitNew,
};

static HRESULT WINAPI site_SetSite(IObjectWithSite *iface, IUnknown *site)
{
	mx_document *self = site_self(iface);
	if (site)
	{
		/* The hosting page is what a security check would ask for: look it up the way hosts expect. */
		static const GUID html_document2 = {0x332c4425, 0x26cb, 0x11d0, {0xb4, 0x83, 0x00, 0xc0, 0x4f, 0xd9, 0x01, 0x19}};
		static const GUID container_dispatch = {0xb722be00, 0x4e68, 0x101b, {0xa2, 0xbc, 0x00, 0xaa, 0x00, 0x40, 0x47, 0x70}};
		IUnknown *page = NULL;
		IServiceProvider *services = NULL;
		if (SUCCEEDED(IUnknown_QueryInterface(site, &html_document2, (void **)&page)) && page)
			IUnknown_Release(page);
		if (SUCCEEDED(IUnknown_QueryInterface(site, &IID_IServiceProvider, (void **)&services)) && services)
		{
			page = NULL;
			if (SUCCEEDED(IServiceProvider_QueryService(services, &container_dispatch, &html_document2, (void **)&page)) && page)
				IUnknown_Release(page);
			IServiceProvider_Release(services);
		}
		IUnknown_AddRef(site);
	}
	if (self->site)
		IUnknown_Release(self->site);
	self->site = site;
	return S_OK;
}

static HRESULT WINAPI site_GetSite(IObjectWithSite *iface, REFIID riid, void **out)
{
	mx_document *self = site_self(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!self->site)
		return E_FAIL;
	return IUnknown_QueryInterface(self->site, riid, out);
}

static const IObjectWithSiteVtbl site_table = {
	site_QueryInterface, site_AddRef, site_Release, site_SetSite, site_GetSite,
};

#define SAFETY_OPTIONS (INTERFACESAFE_FOR_UNTRUSTED_CALLER | INTERFACESAFE_FOR_UNTRUSTED_DATA | INTERFACE_USES_SECURITY_MANAGER)

static HRESULT WINAPI safety_GetInterfaceSafetyOptions(IObjectSafety *iface, REFIID riid, DWORD *supported, DWORD *enabled)
{
	if (!supported || !enabled)
		return E_POINTER;
	*supported = SAFETY_OPTIONS;
	*enabled = safety_self(iface)->safety;
	return S_OK;
}

static HRESULT WINAPI safety_SetInterfaceSafetyOptions(IObjectSafety *iface, REFIID riid, DWORD mask, DWORD enabled)
{
	mx_document *self = safety_self(iface);
	if (mask & ~SAFETY_OPTIONS)
		return E_FAIL;
	self->safety = (self->safety & ~mask) | (enabled & mask);
	return S_OK;
}

static const IObjectSafetyVtbl safety_table = {
	safety_QueryInterface, safety_AddRef, safety_Release, safety_GetInterfaceSafetyOptions, safety_SetInterfaceSafetyOptions,
};

static HRESULT WINAPI point_QueryInterface(IConnectionPoint *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IConnectionPoint))
	{
		*out = NULL;
		return E_NOINTERFACE;
	}
	*out = iface;
	IConnectionPoint_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI point_AddRef(IConnectionPoint *iface)
{
	return nd_AddRef((IXMLDOMNode *)((mx_point *)iface)->owner);
}

static ULONG WINAPI point_Release(IConnectionPoint *iface)
{
	return nd_Release((IXMLDOMNode *)((mx_point *)iface)->owner);
}

static HRESULT WINAPI point_GetConnectionInterface(IConnectionPoint *iface, IID *out)
{
	if (!out)
		return E_POINTER;
	*out = *((mx_point *)iface)->iid;
	return S_OK;
}

static HRESULT WINAPI point_GetConnectionPointContainer(IConnectionPoint *iface, IConnectionPointContainer **out)
{
	if (!out)
		return E_POINTER;
	*out = (IConnectionPointContainer *)&((mx_point *)iface)->owner->points_vtbl;
	IConnectionPointContainer_AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI point_Advise(IConnectionPoint *iface, IUnknown *sink, DWORD *cookie)
{
	mx_point *self = (mx_point *)iface;
	IUnknown *held = NULL, **room;
	DWORD slot;
	if (!sink || !cookie)
		return E_POINTER;
	if (FAILED(IUnknown_QueryInterface(sink, self->iid, (void **)&held)) && FAILED(IUnknown_QueryInterface(sink, &IID_IDispatch, (void **)&held)))
		return CONNECT_E_CANNOTCONNECT;
	for (slot = 0; slot < self->count && self->sinks[slot]; slot++)
		;
	if (slot == self->count)
	{
		room = mx_alloc((self->count + 1) * sizeof(*room));
		if (!room)
		{
			IUnknown_Release(held);
			return E_OUTOFMEMORY;
		}
		if (self->count)
			memcpy(room, self->sinks, self->count * sizeof(*room));
		mx_free(self->sinks);
		self->sinks = room;
		self->count++;
	}
	self->sinks[slot] = held;
	*cookie = slot + 1;
	return S_OK;
}

static HRESULT WINAPI point_Unadvise(IConnectionPoint *iface, DWORD cookie)
{
	mx_point *self = (mx_point *)iface;
	if (!cookie || cookie > self->count || !self->sinks[cookie - 1])
		return CONNECT_E_NOCONNECTION;
	IUnknown_Release(self->sinks[cookie - 1]);
	self->sinks[cookie - 1] = NULL;
	return S_OK;
}

static HRESULT WINAPI point_EnumConnections(IConnectionPoint *iface, IEnumConnections **out)
{
	return E_NOTIMPL;
}

static const IConnectionPointVtbl point_table = {
	point_QueryInterface, point_AddRef, point_Release, point_GetConnectionInterface, point_GetConnectionPointContainer,
	point_Advise, point_Unadvise, point_EnumConnections,
};

/* --- The document as a stream: the markup as it was when loaded, whatever happened to the tree since. --- */

typedef struct
{
	const IStreamVtbl *vtbl;
	LONG refs;
	mx_document *owner;
	ULONG position;
	BOOL writing;
} document_stream;

static HRESULT WINAPI stream_QueryInterface(IStream *iface, REFIID riid, void **out)
{
	document_stream *self = (document_stream *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown))
		return nd_QueryInterface((IXMLDOMNode *)self->owner, riid, out);
	if (!IsEqualGUID(riid, &IID_IStream) && !IsEqualGUID(riid, &IID_ISequentialStream))
		return E_NOINTERFACE;
	*out = iface;
	IStream_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI stream_AddRef(IStream *iface)
{
	return InterlockedIncrement(&((document_stream *)iface)->refs);
}

static ULONG WINAPI stream_Release(IStream *iface)
{
	document_stream *self = (document_stream *)iface;
	LONG refs = InterlockedDecrement(&self->refs);
	if (!refs)
	{
		mx_document *owner = self->owner;
		if (self->writing && !InterlockedDecrement(&owner->writers))
		{
			char *data = owner->pending;
			ULONG size = owner->pending_size;
			VARIANT_BOOL success;
			owner->pending = NULL;
			owner->pending_size = 0;
			if (data)
			{
				/* The parse happens when the last writer lets go. */
				load_text(owner, data, (int)size, NULL, NULL, &success);
				mx_free(data);
			}
		}
		nd_Release((IXMLDOMNode *)owner);
		mx_free(self);
	}
	return refs;
}

static HRESULT WINAPI stream_Read(IStream *iface, void *buffer, ULONG wanted, ULONG *got)
{
	document_stream *self = (document_stream *)iface;
	ULONG size, count;
	if (self->owner->writers)
		return E_FAIL;
	size = (ULONG)self->owner->snapshot_size;
	count = self->position < size ? min(wanted, size - self->position) : 0;
	if (count)
		memcpy(buffer, self->owner->snapshot + self->position, count);
	self->position += count;
	if (got)
		*got = count;
	return S_OK;
}

static HRESULT WINAPI stream_Write(IStream *iface, const void *buffer, ULONG size, ULONG *written)
{
	document_stream *self = (document_stream *)iface;
	mx_document *owner = self->owner;
	char *room;
	/* The first write drops what the document held. */
	if (!owner->writers)
	{
		xmlDocPtr empty = xmlNewDoc(NULL);
		if (empty)
			replace_tree(owner, empty);
		xmlFree(owner->snapshot);
		owner->snapshot = NULL;
		owner->snapshot_size = 0;
	}
	if (!self->writing)
	{
		self->writing = TRUE;
		InterlockedIncrement(&owner->writers);
	}
	room = mx_alloc(owner->pending_size + size + 1);
	if (!room)
		return E_OUTOFMEMORY;
	if (owner->pending_size)
		memcpy(room, owner->pending, owner->pending_size);
	memcpy(room + owner->pending_size, buffer, size);
	mx_free(owner->pending);
	owner->pending = room;
	owner->pending_size += size;
	if (written)
		*written = size;
	return S_OK;
}

static HRESULT WINAPI stream_Seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *out)
{
	document_stream *self = (document_stream *)iface;
	LONGLONG at;
	/* While written to there is only going back to the start, which changes nothing. */
	if (self->owner->writers)
		return origin == STREAM_SEEK_SET && !move.QuadPart ? S_OK : E_NOTIMPL;
	at = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? self->position : self->owner->snapshot_size;
	at += move.QuadPart;
	if (at < 0)
		return STG_E_INVALIDFUNCTION;
	self->position = (ULONG)at;
	if (out)
		out->QuadPart = self->position;
	return S_OK;
}

static HRESULT WINAPI stream_SetSize(IStream *iface, ULARGE_INTEGER size)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_CopyTo(IStream *iface, IStream *to, ULARGE_INTEGER size, ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_Commit(IStream *iface, DWORD flags)
{
	return S_OK;
}

static HRESULT WINAPI stream_Revert(IStream *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_LockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_UnlockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_Stat(IStream *iface, STATSTG *out, DWORD flags)
{
	if (!out)
		return STG_E_INVALIDPOINTER;
	memset(out, 0, sizeof(*out));
	out->type = STGTY_STREAM;
	out->cbSize.QuadPart = ((document_stream *)iface)->owner->snapshot_size;
	return S_OK;
}

static HRESULT WINAPI stream_Clone(IStream *iface, IStream **out)
{
	return E_NOTIMPL;
}

static const IStreamVtbl stream_table = {
	stream_QueryInterface, stream_AddRef, stream_Release, stream_Read, stream_Write, stream_Seek, stream_SetSize, stream_CopyTo,
	stream_Commit, stream_Revert, stream_LockRegion, stream_UnlockRegion, stream_Stat, stream_Clone,
};

static HRESULT document_stream_create(mx_document *owner, void **out)
{
	document_stream *self = mx_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &stream_table;
	self->refs = 1;
	self->owner = owner;
	self->position = 0;
	self->writing = FALSE;
	nd_AddRef((IXMLDOMNode *)owner);
	*out = self;
	return S_OK;
}

static HRESULT WINAPI points_EnumConnectionPoints(IConnectionPointContainer *iface, IEnumConnectionPoints **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI points_FindConnectionPoint(IConnectionPointContainer *iface, REFIID riid, IConnectionPoint **out)
{
	mx_document *self = points_self(iface);
	unsigned kind;
	if (!riid || !out)
		return E_POINTER;
	*out = NULL;
	for (kind = 0; kind < POINT_COUNT; kind++)
		if (IsEqualGUID(riid, self->points[kind].iid))
		{
			*out = (IConnectionPoint *)&self->points[kind];
			IConnectionPoint_AddRef(*out);
			return S_OK;
		}
	return CONNECT_E_NOCONNECTION;
}

static const IConnectionPointContainerVtbl points_table = {
	points_QueryInterface, points_AddRef, points_Release, points_EnumConnectionPoints, points_FindConnectionPoint,
};

/* --- Life. --- */

static HRESULT document_query(mx_node *base, REFIID riid, void **out)
{
	mx_document *self = (mx_document *)base;
	if (IsEqualGUID(riid, &IID_IXMLDOMDocument) || IsEqualGUID(riid, &IID_IXMLDOMDocument2) || IsEqualGUID(riid, &IID_IXMLDOMDocument3))
		*out = self;
	else if (IsEqualGUID(riid, &IID_IPersistStream) || IsEqualGUID(riid, &IID_IPersistStreamInit))
		*out = &self->persist_vtbl;
	else if (IsEqualGUID(riid, &IID_IStream))
		return document_stream_create(self, out);
	else if (IsEqualGUID(riid, &IID_IObjectWithSite))
		*out = &self->site_vtbl;
	else if (IsEqualGUID(riid, &IID_IObjectSafety))
		*out = &self->safety_vtbl;
	else if (IsEqualGUID(riid, &IID_IConnectionPointContainer))
		*out = &self->points_vtbl;
	else
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static void document_destroy(mx_node *base)
{
	mx_document *self = (mx_document *)base;
	mx_properties *props;
	unsigned index;
	DWORD slot;
	for (index = 0; index < EVENT_COUNT; index++)
		if (self->handlers[index])
			IDispatch_Release(self->handlers[index]);
	for (index = 0; index < POINT_COUNT; index++)
	{
		for (slot = 0; slot < self->points[index].count; slot++)
			if (self->points[index].sinks[slot])
				IUnknown_Release(self->points[index].sinks[slot]);
		mx_free(self->points[index].sinks);
	}
	if (self->site)
		IUnknown_Release(self->site);
	if (self->namespaces)
		IXMLDOMSchemaCollection_Release(self->namespaces);
	/* A cache can pin the very tree whose properties name it. Without a document object nothing loads or validates any
	 * more, so the reference goes here rather than with the tree. */
	props = mx_tree_of(tree_of(self))->props;
	if (props->refs == 1 && props->schemas)
	{
		IUnknown_Release(props->schemas);
		props->schemas = NULL;
	}
	xmlFree(self->snapshot);
	mx_free(self->pending);
	mx_error_clear(&self->error);
}

HRESULT mx_document_wrap(xmlDocPtr tree, REFIID riid, void **out)
{
	mx_document *self;
	HRESULT status;
	unsigned index;
	if (!tree->_private)
		mx_tree_attach(tree, MX_DEFAULT);
	self = mx_zalloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	mx_node_init(&self->base, &document_table, (xmlNodePtr)tree, MX_TYPE_DOCUMENT3, &IID_IXMLDOMDocument3);
	self->base.destroy = document_destroy;
	self->base.query = document_query;
	self->persist_vtbl = &persist_table;
	self->site_vtbl = &site_table;
	self->safety_vtbl = &safety_table;
	self->points_vtbl = &points_table;
	self->points[POINT_DISPATCH].iid = &IID_IDispatch;
	self->points[POINT_NOTIFY].iid = &IID_IPropertyNotifySink;
	self->points[POINT_EVENTS].iid = &document_events_iid;
	for (index = 0; index < POINT_COUNT; index++)
	{
		self->points[index].vtbl = &point_table;
		self->points[index].owner = self;
	}
	self->async = VARIANT_TRUE;
	self->ready = READYSTATE_COMPLETE;
	status = nd_QueryInterface((IXMLDOMNode *)self, riid, out);
	nd_Release((IXMLDOMNode *)self);
	return status;
}

HRESULT mx_document_create(mx_version version, REFIID riid, void **out)
{
	xmlDocPtr tree = mx_tree_new_document(version);
	if (!tree)
		return E_OUTOFMEMORY;
	return mx_document_wrap(tree, riid, out);
}

#define DOCUMENT_FIELDS \
	MX_NODE_TABLE, \
	.get_doctype = document_get_doctype, .get_implementation = document_get_implementation, \
	.get_documentElement = document_get_documentElement, .putref_documentElement = document_putref_documentElement, \
	.createElement = document_createElement, .createDocumentFragment = document_createDocumentFragment, \
	.createTextNode = document_createTextNode, .createComment = document_createComment, .createCDATASection = document_createCDATASection, \
	.createProcessingInstruction = document_createProcessingInstruction, .createAttribute = document_createAttribute, \
	.createEntityReference = document_createEntityReference, .getElementsByTagName = document_getElementsByTagName, \
	.createNode = document_createNode, .nodeFromID = document_nodeFromID, .load = document_load, .get_readyState = document_get_readyState, \
	.get_parseError = document_get_parseError, .get_url = document_get_url, .get_async = document_get_async, .put_async = document_put_async, \
	.abort = document_abort, .loadXML = document_loadXML, .save = document_save, \
	.get_validateOnParse = document_get_validateOnParse, .put_validateOnParse = document_put_validateOnParse, \
	.get_resolveExternals = document_get_resolveExternals, .put_resolveExternals = document_put_resolveExternals, \
	.get_preserveWhiteSpace = document_get_preserveWhiteSpace, .put_preserveWhiteSpace = document_put_preserveWhiteSpace, \
	.put_onreadystatechange = document_put_onreadystatechange, .put_ondataavailable = document_put_ondataavailable, \
	.put_ontransformnode = document_put_ontransformnode, \
	.get_namespaces = document_get_namespaces, .get_schemas = document_get_schemas, .putref_schemas = document_putref_schemas, \
	.validate = document_validate, .setProperty = document_setProperty, .getProperty = document_getProperty, \
	.validateNode = document_validateNode, .importNode = document_importNode

static const IXMLDOMDocument3Vtbl document_table = { DOCUMENT_FIELDS };

BOOL mx_is_document(IUnknown *object)
{
	return *(const void **)object == &document_table;
}
