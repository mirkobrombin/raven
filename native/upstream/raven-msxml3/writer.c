/* SPDX-License-Identifier: MIT */
#include "mx.h"

/* SAX events in, markup out: into a string held here, a stream, or a document once it has ended.
   The bytes wait in a buffer in the encoding in force when each piece was written. */

#define FLUSH_THRESHOLD 4096

typedef struct
{
	const IMXWriterVtbl *vtbl;
	const ISAXContentHandlerVtbl *content_vtbl;
	const ISAXLexicalHandlerVtbl *lexical_vtbl;
	const ISAXDeclHandlerVtbl *decl_vtbl;
	const ISAXDTDHandlerVtbl *dtd_vtbl;
	const ISAXErrorHandlerVtbl *error_vtbl;
	const IVBSAXContentHandlerVtbl *vb_content_vtbl;
	const IVBSAXLexicalHandlerVtbl *vb_lexical_vtbl;
	const IVBSAXDeclHandlerVtbl *vb_decl_vtbl;
	const IVBSAXDTDHandlerVtbl *vb_dtd_vtbl;
	const IVBSAXErrorHandlerVtbl *vb_error_vtbl;
	mx_dispex dispex;
	LONG refs;
	mx_version version;

	VARIANT_BOOL bom, indent, standalone, omit_declaration, no_escaping;
	BSTR encoding, xml_version;

	IStream *stream;
	IXMLDOMDocument *document;
	BYTE *bytes;
	SIZE_T used, room;

	BOOL open_tag;		/* A start tag waits for its end: ">" or "/>" depends on what comes next. */
	BOOL after_text;	/* Text leaves the next tag on the same line. */
	BOOL line_start;	/* Nothing has been written on the current line. */
	BOOL seen_element;
	BOOL in_cdata;
	BOOL wrote_mark;
	int depth;
} mx_writer;

static BOOL is_utf16(BSTR name)
{
	return !lstrcmpiW(name, L"UTF-16") || !lstrcmpiW(name, L"UCS-2");
}

static BOOL wide_output(const mx_writer *self)
{
	return !self->stream || is_utf16(self->encoding);
}

static BOOL reserve(mx_writer *self, SIZE_T more)
{
	if (self->used + more > self->room)
	{
		SIZE_T room = (self->used + more) * 2 + 256;
		BYTE *grown = mx_realloc(self->bytes, room);
		if (!grown)
			return FALSE;
		self->bytes = grown;
		self->room = room;
	}
	return TRUE;
}

static void put_text(mx_writer *self, const WCHAR *text, int length)
{
	if (length <= 0)
		return;
	if (wide_output(self))
	{
		if (reserve(self, length * sizeof(WCHAR)))
		{
			memcpy(self->bytes + self->used, text, length * sizeof(WCHAR));
			self->used += length * sizeof(WCHAR);
		}
	}
	else
	{
		xmlChar *name = mx_to_utf8(self->encoding);
		UINT page = CP_UTF8;
		int size;
		mx_page_of_name(name, &page);
		xmlFree(name);
		size = WideCharToMultiByte(page, 0, text, length, NULL, 0, NULL, NULL);
		if (size && reserve(self, size))
			self->used += WideCharToMultiByte(page, 0, text, length, (char *)self->bytes + self->used, size, NULL, NULL);
		else if (!size && reserve(self, length))
		{
			/* A code page the system lacks still writes its ASCII range. */
			int index;
			for (index = 0; index < length; index++)
				self->bytes[self->used++] = text[index] < 0x80 ? (BYTE)text[index] : '?';
		}
	}
}

static void put_literal(mx_writer *self, const WCHAR *text)
{
	put_text(self, text, lstrlenW(text));
}

static int count_of(const WCHAR *text, int length)
{
	return length < 0 ? lstrlenW(text) : length;
}

/* Every line break leaves as the pair, whichever way it came in. */
static void put_lines(mx_writer *self, const WCHAR *text, int length)
{
	int from = 0, index;
	for (index = 0; index < length; index++)
		if (text[index] == '\r' || text[index] == '\n')
		{
			put_text(self, text + from, index - from);
			put_literal(self, L"\r\n");
			if (text[index] == '\r' && index + 1 < length && text[index + 1] == '\n')
				index++;
			from = index + 1;
		}
	put_text(self, text + from, length - from);
}

static void put_escaped(mx_writer *self, const WCHAR *text, int length, BOOL attribute)
{
	int from = 0, index;
	for (index = 0; index < length; index++)
	{
		const WCHAR *entity = NULL;
		switch (text[index])
		{
		case '<': entity = L"&lt;"; break;
		case '>': entity = L"&gt;"; break;
		case '&': entity = L"&amp;"; break;
		case '"': entity = attribute ? L"&quot;" : NULL; break;
		default: break;
		}
		if (entity)
		{
			put_lines(self, text + from, index - from);
			put_literal(self, entity);
			from = index + 1;
		}
	}
	put_lines(self, text + from, length - from);
}

static HRESULT flush_bytes(mx_writer *self)
{
	HRESULT status = S_OK;
	if (self->stream && self->used)
	{
		ULONG written = 0;
		status = IStream_Write(self->stream, self->bytes, (ULONG)self->used, &written);
		self->used = 0;
	}
	return status;
}

static void flush_if_full(mx_writer *self)
{
	if (self->stream && self->used >= FLUSH_THRESHOLD)
		flush_bytes(self);
}

static void close_tag(mx_writer *self)
{
	if (self->open_tag)
		put_literal(self, L">");
	self->open_tag = FALSE;
}

/* With indentation on, a tag starts on its own line unless text came right before it. */
static void put_indent_mark(mx_writer *self)
{
	int level;
	if (self->indent != VARIANT_TRUE || self->after_text)
		return;
	if (!self->line_start)
		put_literal(self, L"\r\n");
	for (level = 0; level < self->depth; level++)
		put_literal(self, L"\t");
	self->line_start = FALSE;
}

/* Without indentation, comments and instructions take a line each until the first element shows up. */
static void end_misc_line(mx_writer *self)
{
	if (self->indent != VARIANT_TRUE && !self->seen_element)
	{
		put_literal(self, L"\r\n");
		self->line_start = TRUE;
	}
}

static void reset_output(mx_writer *self)
{
	self->used = 0;
	self->open_tag = FALSE;
	self->after_text = FALSE;
	self->line_start = TRUE;
	self->seen_element = FALSE;
	self->in_cdata = FALSE;
	self->wrote_mark = FALSE;
	self->depth = 0;
}

static void drop_output(mx_writer *self)
{
	if (self->stream)
		IStream_Release(self->stream);
	if (self->document)
		IXMLDOMDocument_Release(self->document);
	self->stream = NULL;
	self->document = NULL;
}

/* --- Event bodies, shared by the native faces and the ones for scripts. --- */

static HRESULT do_start_document(mx_writer *self)
{
	if (self->stream && self->bom == VARIANT_TRUE && is_utf16(self->encoding) && !self->wrote_mark)
	{
		static const BYTE mark[2] = {0xff, 0xfe};
		self->wrote_mark = TRUE;
		IStream_Write(self->stream, mark, sizeof(mark), NULL);
	}
	if (self->omit_declaration == VARIANT_TRUE)
		return S_OK;
	put_literal(self, L"<?xml version=\"");
	put_text(self, self->xml_version, SysStringLen(self->xml_version));
	put_literal(self, L"\" encoding=\"");
	if (self->stream)
		put_text(self, self->encoding, SysStringLen(self->encoding));
	else
		put_literal(self, L"UTF-16");
	put_literal(self, L"\" standalone=\"");
	put_literal(self, self->standalone == VARIANT_TRUE ? L"yes" : L"no");
	put_literal(self, L"\"?>\r\n");
	self->line_start = TRUE;
	return S_OK;
}

static HRESULT do_end_document(mx_writer *self)
{
	close_tag(self);
	if (self->document)
	{
		BSTR markup = SysAllocStringLen((const WCHAR *)self->bytes, (UINT)(self->used / sizeof(WCHAR)));
		VARIANT_BOOL loaded;
		HRESULT status = IXMLDOMDocument_loadXML(self->document, markup, &loaded);
		SysFreeString(markup);
		return status;
	}
	return flush_bytes(self);
}

static HRESULT do_start_element(mx_writer *self, const WCHAR *uri, const WCHAR *local, const WCHAR *qname, int qname_length, ISAXAttributes *attributes, IVBSAXAttributes *vb_attributes)
{
	int count = 0, index;
	if (!uri || !local || !qname)
		return E_INVALIDARG;
	close_tag(self);
	put_indent_mark(self);
	self->after_text = FALSE;
	self->seen_element = TRUE;
	put_literal(self, L"<");
	put_text(self, qname, count_of(qname, qname_length));
	self->open_tag = TRUE;
	self->depth++;
	if (attributes)
		ISAXAttributes_getLength(attributes, &count);
	else if (vb_attributes)
		IVBSAXAttributes_get_length(vb_attributes, &count);
	for (index = 0; index < count; index++)
	{
		const WCHAR *name = NULL, *value = NULL;
		BSTR vb_name = NULL, vb_value = NULL;
		int name_length = 0, value_length = 0;
		HRESULT status;
		if (attributes)
		{
			status = ISAXAttributes_getQName(attributes, index, &name, &name_length);
			if (SUCCEEDED(status))
				status = ISAXAttributes_getValue(attributes, index, &value, &value_length);
		}
		else
		{
			status = IVBSAXAttributes_getQName(vb_attributes, index, &vb_name);
			if (SUCCEEDED(status))
				status = IVBSAXAttributes_getValue(vb_attributes, index, &vb_value);
			name = vb_name;
			name_length = SysStringLen(vb_name);
			value = vb_value;
			value_length = SysStringLen(vb_value);
		}
		if (SUCCEEDED(status))
		{
			put_literal(self, L" ");
			put_text(self, name, name_length);
			put_literal(self, L"=\"");
			if (self->no_escaping == VARIANT_TRUE)
				put_lines(self, value, value_length);
			else
				put_escaped(self, value, value_length, TRUE);
			put_literal(self, L"\"");
		}
		SysFreeString(vb_name);
		SysFreeString(vb_value);
		if (FAILED(status))
			return status;
	}
	flush_if_full(self);
	return S_OK;
}

static HRESULT do_end_element(mx_writer *self, const WCHAR *uri, const WCHAR *local, const WCHAR *qname, int qname_length)
{
	if (!uri || !local || !qname)
		return E_INVALIDARG;
	if (self->depth > 0)
		self->depth--;
	if (self->open_tag)
	{
		put_literal(self, L"/>");
		self->open_tag = FALSE;
	}
	else
	{
		put_indent_mark(self);
		put_literal(self, L"</");
		put_text(self, qname, count_of(qname, qname_length));
		put_literal(self, L">");
	}
	self->after_text = FALSE;
	flush_if_full(self);
	return S_OK;
}

static HRESULT do_characters(mx_writer *self, const WCHAR *text, int length)
{
	if (!text)
		return E_INVALIDARG;
	close_tag(self);
	length = count_of(text, length);
	if (self->in_cdata || self->no_escaping == VARIANT_TRUE)
		put_lines(self, text, length);
	else
		put_escaped(self, text, length, FALSE);
	self->after_text = TRUE;
	self->line_start = FALSE;
	flush_if_full(self);
	return S_OK;
}

static HRESULT do_ignorable(mx_writer *self, const WCHAR *text, int length)
{
	if (!text)
		return E_INVALIDARG;
	close_tag(self);
	put_text(self, text, count_of(text, length));
	flush_if_full(self);
	return S_OK;
}

static HRESULT do_instruction(mx_writer *self, const WCHAR *target, int target_length, const WCHAR *data, int data_length)
{
	if (!target)
		return E_INVALIDARG;
	close_tag(self);
	put_indent_mark(self);
	put_literal(self, L"<?");
	put_text(self, target, count_of(target, target_length));
	if (data && data_length)
	{
		put_literal(self, L" ");
		put_text(self, data, count_of(data, data_length));
	}
	put_literal(self, L"?>");
	end_misc_line(self);
	flush_if_full(self);
	return S_OK;
}

static HRESULT do_skipped(mx_writer *self, const WCHAR *name, int length)
{
	if (!name)
		return E_INVALIDARG;
	close_tag(self);
	put_literal(self, L"&");
	put_text(self, name, count_of(name, length));
	put_literal(self, L";");
	return S_OK;
}

static HRESULT do_comment(mx_writer *self, const WCHAR *text, int length)
{
	if (!text)
		return E_INVALIDARG;
	close_tag(self);
	put_indent_mark(self);
	put_literal(self, L"<!--");
	put_text(self, text, count_of(text, length));
	put_literal(self, L"-->");
	end_misc_line(self);
	flush_if_full(self);
	return S_OK;
}

static HRESULT do_start_cdata(mx_writer *self)
{
	close_tag(self);
	put_literal(self, L"<![CDATA[");
	self->in_cdata = TRUE;
	return S_OK;
}

static HRESULT do_end_cdata(mx_writer *self)
{
	put_literal(self, L"]]>");
	self->in_cdata = FALSE;
	return S_OK;
}

static void put_quoted(mx_writer *self, const WCHAR *text, int length)
{
	put_literal(self, L"\"");
	put_text(self, text, count_of(text, length));
	put_literal(self, L"\"");
}

/* The identifiers of a declaration: what is missing comes out only once the fault is found, as it always did. */
static HRESULT put_identifiers(mx_writer *self, const WCHAR *public_id, int public_length, const WCHAR *system_id, int system_length)
{
	BOOL has_public = public_id && public_length;
	BOOL has_system = system_id && system_length;
	if (has_public)
	{
		put_literal(self, L" PUBLIC ");
		put_quoted(self, public_id, public_length);
		if (has_system)
		{
			put_literal(self, L" ");
			put_quoted(self, system_id, system_length);
		}
	}
	else if (has_system)
	{
		put_literal(self, L" SYSTEM ");
		put_quoted(self, system_id, system_length);
	}
	return S_OK;
}

static HRESULT do_start_dtd(mx_writer *self, const WCHAR *name, int name_length, const WCHAR *public_id, int public_length, const WCHAR *system_id, int system_length)
{
	if (!name)
		return E_INVALIDARG;
	close_tag(self);
	put_literal(self, L"<!DOCTYPE ");
	put_text(self, name, count_of(name, name_length));
	put_identifiers(self, public_id, public_length, system_id, system_length);
	if (public_id && public_length && !(system_id && system_length))
		return E_INVALIDARG;
	put_literal(self, L" [\r\n");
	self->line_start = TRUE;
	return S_OK;
}

static HRESULT do_end_dtd(mx_writer *self)
{
	put_literal(self, L"]>\r\n");
	self->line_start = TRUE;
	return S_OK;
}

static HRESULT do_element_decl(mx_writer *self, const WCHAR *name, int name_length, const WCHAR *model, int model_length)
{
	if (!name || !model)
		return E_INVALIDARG;
	put_literal(self, L"<!ELEMENT ");
	put_text(self, name, count_of(name, name_length));
	put_literal(self, L" ");
	put_text(self, model, count_of(model, model_length));
	put_literal(self, L">\r\n");
	return S_OK;
}

static HRESULT do_attribute_decl(mx_writer *self, const WCHAR *element, int element_length, const WCHAR *name, int name_length, const WCHAR *type, int type_length, const WCHAR *fallback, int fallback_length, const WCHAR *value, int value_length)
{
	if (!element || !name || !type)
		return E_INVALIDARG;
	put_literal(self, L"<!ATTLIST ");
	put_text(self, element, count_of(element, element_length));
	put_literal(self, L" ");
	put_text(self, name, count_of(name, name_length));
	put_literal(self, L" ");
	put_text(self, type, count_of(type, type_length));
	if (fallback && fallback_length)
	{
		put_literal(self, L" ");
		put_text(self, fallback, count_of(fallback, fallback_length));
	}
	if (value && value_length)
	{
		put_literal(self, L" ");
		put_quoted(self, value, value_length);
	}
	put_literal(self, L">\r\n");
	return S_OK;
}

static HRESULT do_internal_entity(mx_writer *self, const WCHAR *name, int name_length, const WCHAR *value, int value_length)
{
	if (!name || !value)
		return E_INVALIDARG;
	put_literal(self, L"<!ENTITY ");
	put_text(self, name, count_of(name, name_length));
	put_literal(self, L" ");
	put_quoted(self, value, value_length);
	put_literal(self, L">\r\n");
	return S_OK;
}

static HRESULT do_external_entity(mx_writer *self, const WCHAR *name, int name_length, const WCHAR *public_id, int public_length, const WCHAR *system_id, int system_length)
{
	if (!name || name_length <= 0 || !system_id)
		return E_INVALIDARG;
	put_literal(self, L"<!ENTITY ");
	put_text(self, name, name_length);
	put_identifiers(self, public_id, public_length, system_id, system_length);
	put_literal(self, L">\r\n");
	return S_OK;
}

static HRESULT do_notation(mx_writer *self, const WCHAR *name, int name_length, const WCHAR *public_id, int public_length, const WCHAR *system_id, int system_length)
{
	if (!name)
		return E_INVALIDARG;
	put_literal(self, L"<!NOTATION ");
	put_text(self, name, count_of(name, name_length));
	if (!(public_id && public_length) && !(system_id && system_length))
		return E_INVALIDARG;
	put_identifiers(self, public_id, public_length, system_id, system_length);
	put_literal(self, L">\r\n");
	return S_OK;
}

static HRESULT do_unparsed(mx_writer *self, const WCHAR *name, int name_length, const WCHAR *public_id, int public_length, const WCHAR *system_id, int system_length, const WCHAR *notation, int notation_length)
{
	if (!name || !notation)
		return E_INVALIDARG;
	put_literal(self, L"<!ENTITY ");
	put_text(self, name, count_of(name, name_length));
	put_identifiers(self, public_id, public_length, system_id, system_length);
	put_literal(self, L" NDATA ");
	put_text(self, notation, count_of(notation, notation_length));
	put_literal(self, L">\r\n");
	return S_OK;
}

/* --- The writer's own face. --- */

static HRESULT WINAPI writer_QueryInterface(IMXWriter *iface, REFIID riid, void **out)
{
	mx_writer *self = (mx_writer *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IMXWriter))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_IDispatchEx))
		*out = &self->dispex;
	else if (IsEqualGUID(riid, &IID_ISAXContentHandler))
		*out = &self->content_vtbl;
	else if (IsEqualGUID(riid, &IID_ISAXLexicalHandler))
		*out = &self->lexical_vtbl;
	else if (IsEqualGUID(riid, &IID_ISAXDeclHandler))
		*out = &self->decl_vtbl;
	else if (IsEqualGUID(riid, &IID_ISAXDTDHandler))
		*out = &self->dtd_vtbl;
	else if (IsEqualGUID(riid, &IID_ISAXErrorHandler))
		*out = &self->error_vtbl;
	else if (IsEqualGUID(riid, &IID_IVBSAXContentHandler))
		*out = &self->vb_content_vtbl;
	else if (IsEqualGUID(riid, &IID_IVBSAXLexicalHandler))
		*out = &self->vb_lexical_vtbl;
	else if (IsEqualGUID(riid, &IID_IVBSAXDeclHandler))
		*out = &self->vb_decl_vtbl;
	else if (IsEqualGUID(riid, &IID_IVBSAXDTDHandler))
		*out = &self->vb_dtd_vtbl;
	else if (IsEqualGUID(riid, &IID_IVBSAXErrorHandler))
		*out = &self->vb_error_vtbl;
	else
		return E_NOINTERFACE;
	IMXWriter_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI writer_AddRef(IMXWriter *iface)
{
	return InterlockedIncrement(&((mx_writer *)iface)->refs);
}

static ULONG WINAPI writer_Release(IMXWriter *iface)
{
	mx_writer *self = (mx_writer *)iface;
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		/* What the stream hears last is a write even with nothing left, except in wide text. */
		if (self->stream)
		{
			if (self->used || !is_utf16(self->encoding))
			{
				ULONG written = 0;
				static const BYTE nothing = 0;
				IStream_Write(self->stream, self->used ? self->bytes : &nothing, (ULONG)self->used, &written);
			}
			self->used = 0;
		}
		drop_output(self);
		SysFreeString(self->encoding);
		SysFreeString(self->xml_version);
		mx_free(self->bytes);
		mx_free(self);
	}
	return left;
}

static HRESULT WINAPI writer_GetTypeInfoCount(IMXWriter *iface, UINT *count)
{
	*count = 1;
	return S_OK;
}

static HRESULT WINAPI writer_GetTypeInfo(IMXWriter *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	return mx_type_info(MX_TYPE_WRITER, out);
}

static HRESULT WINAPI writer_GetIDsOfNames(IMXWriter *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(MX_TYPE_WRITER, riid, names, count, lcid, ids);
}

static HRESULT WINAPI writer_Invoke(IMXWriter *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(MX_TYPE_WRITER, iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

static HRESULT WINAPI writer_put_output(IMXWriter *iface, VARIANT target)
{
	mx_writer *self = (mx_writer *)iface;
	const VARIANT *from = &target;
	IStream *stream = NULL;
	IXMLDOMDocument *document = NULL;
	while (V_VT(from) == (VT_VARIANT | VT_BYREF))
		from = V_VARIANTREF(from);
	if (V_VT(from) == VT_UNKNOWN || V_VT(from) == VT_DISPATCH)
	{
		if (!V_UNKNOWN(from))
			return E_INVALIDARG;
		if (FAILED(IUnknown_QueryInterface(V_UNKNOWN(from), &IID_IStream, (void **)&stream))
			&& FAILED(IUnknown_QueryInterface(V_UNKNOWN(from), &IID_ISequentialStream, (void **)&stream))
			&& FAILED(IUnknown_QueryInterface(V_UNKNOWN(from), &IID_IXMLDOMDocument, (void **)&document)))
			return E_INVALIDARG;
	}
	else if (V_VT(from) != VT_EMPTY && V_VT(from) != VT_BSTR)
		return E_INVALIDARG;
	flush_bytes(self);
	drop_output(self);
	reset_output(self);
	self->stream = stream;
	self->document = document;
	return S_OK;
}

static HRESULT WINAPI writer_get_output(IMXWriter *iface, VARIANT *out)
{
	mx_writer *self = (mx_writer *)iface;
	if (!out)
		return E_POINTER;
	if (self->stream || self->document)
	{
		V_VT(out) = VT_UNKNOWN;
		V_UNKNOWN(out) = self->stream ? (IUnknown *)self->stream : (IUnknown *)self->document;
		IUnknown_AddRef(V_UNKNOWN(out));
		return S_OK;
	}
	/* Reading settles the tag still open: whatever ends it later writes a full end tag. */
	close_tag(self);
	V_VT(out) = VT_BSTR;
	V_BSTR(out) = SysAllocStringLen((const WCHAR *)self->bytes, (UINT)(self->used / sizeof(WCHAR)));
	if (!V_BSTR(out))
		V_BSTR(out) = SysAllocString(L"");
	return S_OK;
}

static HRESULT WINAPI writer_put_encoding(IMXWriter *iface, BSTR encoding)
{
	mx_writer *self = (mx_writer *)iface;
	xmlChar *name;
	UINT page;
	BOOL known;
	BSTR copy;
	if (!encoding)
		return E_INVALIDARG;
	name = mx_to_utf8(encoding);
	known = is_utf16(encoding) || mx_page_of_name(name, &page);
	xmlFree(name);
	if (!known)
		return E_INVALIDARG;
	copy = SysAllocString(encoding);
	if (!copy)
		return E_OUTOFMEMORY;
	SysFreeString(self->encoding);
	self->encoding = copy;
	return S_OK;
}

static HRESULT WINAPI writer_get_encoding(IMXWriter *iface, BSTR *out)
{
	if (!out)
		return E_POINTER;
	*out = SysAllocString(((mx_writer *)iface)->encoding);
	return *out ? S_OK : E_OUTOFMEMORY;
}

#define WRITER_FLAG(put, get, field) \
	static HRESULT WINAPI writer_##put(IMXWriter *iface, VARIANT_BOOL value) \
	{ \
		((mx_writer *)iface)->field = value; \
		return S_OK; \
	} \
	static HRESULT WINAPI writer_##get(IMXWriter *iface, VARIANT_BOOL *out) \
	{ \
		if (!out) \
			return E_POINTER; \
		*out = ((mx_writer *)iface)->field; \
		return S_OK; \
	}

WRITER_FLAG(put_byteOrderMark, get_byteOrderMark, bom)
WRITER_FLAG(put_indent, get_indent, indent)
WRITER_FLAG(put_standalone, get_standalone, standalone)
WRITER_FLAG(put_omitXMLDeclaration, get_omitXMLDeclaration, omit_declaration)
WRITER_FLAG(put_disableOutputEscaping, get_disableOutputEscaping, no_escaping)

static HRESULT WINAPI writer_put_version(IMXWriter *iface, BSTR version)
{
	mx_writer *self = (mx_writer *)iface;
	BSTR copy;
	if (!version)
		return E_INVALIDARG;
	copy = SysAllocString(version);
	if (!copy)
		return E_OUTOFMEMORY;
	SysFreeString(self->xml_version);
	self->xml_version = copy;
	return S_OK;
}

static HRESULT WINAPI writer_get_version(IMXWriter *iface, BSTR *out)
{
	if (!out)
		return E_POINTER;
	*out = SysAllocString(((mx_writer *)iface)->xml_version);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI writer_flush(IMXWriter *iface)
{
	return flush_bytes((mx_writer *)iface);
}

static const IMXWriterVtbl writer_table = {
	writer_QueryInterface, writer_AddRef, writer_Release, writer_GetTypeInfoCount, writer_GetTypeInfo, writer_GetIDsOfNames, writer_Invoke,
	writer_put_output, writer_get_output, writer_put_encoding, writer_get_encoding, writer_put_byteOrderMark, writer_get_byteOrderMark,
	writer_put_indent, writer_get_indent, writer_put_standalone, writer_get_standalone, writer_put_omitXMLDeclaration, writer_get_omitXMLDeclaration,
	writer_put_version, writer_get_version, writer_put_disableOutputEscaping, writer_get_disableOutputEscaping, writer_flush,
};

/* --- Every other face leads back to the writer. --- */

#define OF(iface, field) CONTAINING_RECORD(iface, mx_writer, field)

#define FACE_UNKNOWN(prefix, type, field) \
	static HRESULT WINAPI prefix##_QueryInterface(type *iface, REFIID riid, void **out) \
	{ \
		return writer_QueryInterface((IMXWriter *)OF(iface, field), riid, out); \
	} \
	static ULONG WINAPI prefix##_AddRef(type *iface) \
	{ \
		return writer_AddRef((IMXWriter *)OF(iface, field)); \
	} \
	static ULONG WINAPI prefix##_Release(type *iface) \
	{ \
		return writer_Release((IMXWriter *)OF(iface, field)); \
	}

/* The script faces are what a reader calls directly. Late binding has no use for them, the writer object answers it. */
#define FACE_DISPATCH(prefix, type) \
	static HRESULT WINAPI prefix##_GetTypeInfoCount(type *iface, UINT *count) \
	{ \
		*count = 0; \
		return S_OK; \
	} \
	static HRESULT WINAPI prefix##_GetTypeInfo(type *iface, UINT index, LCID lcid, ITypeInfo **out) \
	{ \
		return E_NOTIMPL; \
	} \
	static HRESULT WINAPI prefix##_GetIDsOfNames(type *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids) \
	{ \
		return E_NOTIMPL; \
	} \
	static HRESULT WINAPI prefix##_Invoke(type *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err) \
	{ \
		return E_NOTIMPL; \
	}

#define VB_FACE_HEAD(prefix) \
	prefix##_QueryInterface, prefix##_AddRef, prefix##_Release, prefix##_GetTypeInfoCount, prefix##_GetTypeInfo, prefix##_GetIDsOfNames, prefix##_Invoke

/* Native content events. */

FACE_UNKNOWN(content, ISAXContentHandler, content_vtbl)

static HRESULT WINAPI content_putDocumentLocator(ISAXContentHandler *iface, ISAXLocator *locator)
{
	return S_OK;
}

static HRESULT WINAPI content_startDocument(ISAXContentHandler *iface)
{
	return do_start_document(OF(iface, content_vtbl));
}

static HRESULT WINAPI content_endDocument(ISAXContentHandler *iface)
{
	return do_end_document(OF(iface, content_vtbl));
}

static HRESULT WINAPI content_startPrefixMapping(ISAXContentHandler *iface, const WCHAR *prefix, int prefix_length, const WCHAR *uri, int uri_length)
{
	return S_OK;
}

static HRESULT WINAPI content_endPrefixMapping(ISAXContentHandler *iface, const WCHAR *prefix, int length)
{
	return S_OK;
}

static HRESULT WINAPI content_startElement(ISAXContentHandler *iface, const WCHAR *uri, int uri_length, const WCHAR *local, int local_length, const WCHAR *qname, int qname_length, ISAXAttributes *attributes)
{
	return do_start_element(OF(iface, content_vtbl), uri, local, qname, qname_length, attributes, NULL);
}

static HRESULT WINAPI content_endElement(ISAXContentHandler *iface, const WCHAR *uri, int uri_length, const WCHAR *local, int local_length, const WCHAR *qname, int qname_length)
{
	return do_end_element(OF(iface, content_vtbl), uri, local, qname, qname_length);
}

static HRESULT WINAPI content_characters(ISAXContentHandler *iface, const WCHAR *text, int length)
{
	return do_characters(OF(iface, content_vtbl), text, length);
}

static HRESULT WINAPI content_ignorableWhitespace(ISAXContentHandler *iface, const WCHAR *text, int length)
{
	return do_ignorable(OF(iface, content_vtbl), text, length);
}

static HRESULT WINAPI content_processingInstruction(ISAXContentHandler *iface, const WCHAR *target, int target_length, const WCHAR *data, int data_length)
{
	return do_instruction(OF(iface, content_vtbl), target, target_length, data, data_length);
}

static HRESULT WINAPI content_skippedEntity(ISAXContentHandler *iface, const WCHAR *name, int length)
{
	return do_skipped(OF(iface, content_vtbl), name, length);
}

static const ISAXContentHandlerVtbl content_table = {
	content_QueryInterface, content_AddRef, content_Release, content_putDocumentLocator, content_startDocument, content_endDocument,
	content_startPrefixMapping, content_endPrefixMapping, content_startElement, content_endElement, content_characters,
	content_ignorableWhitespace, content_processingInstruction, content_skippedEntity,
};

/* Native lexical events. */

FACE_UNKNOWN(lexical, ISAXLexicalHandler, lexical_vtbl)

static HRESULT WINAPI lexical_startDTD(ISAXLexicalHandler *iface, const WCHAR *name, int name_length, const WCHAR *public_id, int public_length, const WCHAR *system_id, int system_length)
{
	return do_start_dtd(OF(iface, lexical_vtbl), name, name_length, public_id, public_length, system_id, system_length);
}

static HRESULT WINAPI lexical_endDTD(ISAXLexicalHandler *iface)
{
	return do_end_dtd(OF(iface, lexical_vtbl));
}

static HRESULT WINAPI lexical_startEntity(ISAXLexicalHandler *iface, const WCHAR *name, int length)
{
	return S_OK;
}

static HRESULT WINAPI lexical_endEntity(ISAXLexicalHandler *iface, const WCHAR *name, int length)
{
	return S_OK;
}

static HRESULT WINAPI lexical_startCDATA(ISAXLexicalHandler *iface)
{
	return do_start_cdata(OF(iface, lexical_vtbl));
}

static HRESULT WINAPI lexical_endCDATA(ISAXLexicalHandler *iface)
{
	return do_end_cdata(OF(iface, lexical_vtbl));
}

static HRESULT WINAPI lexical_comment(ISAXLexicalHandler *iface, const WCHAR *text, int length)
{
	return do_comment(OF(iface, lexical_vtbl), text, length);
}

static const ISAXLexicalHandlerVtbl lexical_table = {
	lexical_QueryInterface, lexical_AddRef, lexical_Release, lexical_startDTD, lexical_endDTD, lexical_startEntity, lexical_endEntity,
	lexical_startCDATA, lexical_endCDATA, lexical_comment,
};

/* Native declaration events. */

FACE_UNKNOWN(decl, ISAXDeclHandler, decl_vtbl)

static HRESULT WINAPI decl_elementDecl(ISAXDeclHandler *iface, const WCHAR *name, int name_length, const WCHAR *model, int model_length)
{
	return do_element_decl(OF(iface, decl_vtbl), name, name_length, model, model_length);
}

static HRESULT WINAPI decl_attributeDecl(ISAXDeclHandler *iface, const WCHAR *element, int element_length, const WCHAR *name, int name_length, const WCHAR *type, int type_length, const WCHAR *fallback, int fallback_length, const WCHAR *value, int value_length)
{
	return do_attribute_decl(OF(iface, decl_vtbl), element, element_length, name, name_length, type, type_length, fallback, fallback_length, value, value_length);
}

static HRESULT WINAPI decl_internalEntityDecl(ISAXDeclHandler *iface, const WCHAR *name, int name_length, const WCHAR *value, int value_length)
{
	return do_internal_entity(OF(iface, decl_vtbl), name, name_length, value, value_length);
}

static HRESULT WINAPI decl_externalEntityDecl(ISAXDeclHandler *iface, const WCHAR *name, int name_length, const WCHAR *public_id, int public_length, const WCHAR *system_id, int system_length)
{
	return do_external_entity(OF(iface, decl_vtbl), name, name_length, public_id, public_length, system_id, system_length);
}

static const ISAXDeclHandlerVtbl decl_table = {
	decl_QueryInterface, decl_AddRef, decl_Release, decl_elementDecl, decl_attributeDecl, decl_internalEntityDecl, decl_externalEntityDecl,
};

/* Native notation events. */

FACE_UNKNOWN(dtd, ISAXDTDHandler, dtd_vtbl)

static HRESULT WINAPI dtd_notationDecl(ISAXDTDHandler *iface, const WCHAR *name, int name_length, const WCHAR *public_id, int public_length, const WCHAR *system_id, int system_length)
{
	return do_notation(OF(iface, dtd_vtbl), name, name_length, public_id, public_length, system_id, system_length);
}

static HRESULT WINAPI dtd_unparsedEntityDecl(ISAXDTDHandler *iface, const WCHAR *name, int name_length, const WCHAR *public_id, int public_length, const WCHAR *system_id, int system_length, const WCHAR *notation, int notation_length)
{
	return do_unparsed(OF(iface, dtd_vtbl), name, name_length, public_id, public_length, system_id, system_length, notation, notation_length);
}

static const ISAXDTDHandlerVtbl dtd_table = {
	dtd_QueryInterface, dtd_AddRef, dtd_Release, dtd_notationDecl, dtd_unparsedEntityDecl,
};

/* Errors go nowhere: a writer has nothing to say about them. */

FACE_UNKNOWN(errors, ISAXErrorHandler, error_vtbl)

static HRESULT WINAPI errors_error(ISAXErrorHandler *iface, ISAXLocator *locator, const WCHAR *message, HRESULT code)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI errors_fatalError(ISAXErrorHandler *iface, ISAXLocator *locator, const WCHAR *message, HRESULT code)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI errors_ignorableWarning(ISAXErrorHandler *iface, ISAXLocator *locator, const WCHAR *message, HRESULT code)
{
	return E_NOTIMPL;
}

static const ISAXErrorHandlerVtbl errors_table = {
	errors_QueryInterface, errors_AddRef, errors_Release, errors_error, errors_fatalError, errors_ignorableWarning,
};

/* The same events for scripts: strings arrive by reference and know their own lengths. */

#define TEXT_LENGTH(pointer) SysStringLen(*(pointer))

FACE_UNKNOWN(vb_content, IVBSAXContentHandler, vb_content_vtbl)
FACE_DISPATCH(vb_content, IVBSAXContentHandler)

static HRESULT WINAPI vb_content_putref_documentLocator(IVBSAXContentHandler *iface, IVBSAXLocator *locator)
{
	return S_OK;
}

static HRESULT WINAPI vb_content_startDocument(IVBSAXContentHandler *iface)
{
	return do_start_document(OF(iface, vb_content_vtbl));
}

static HRESULT WINAPI vb_content_endDocument(IVBSAXContentHandler *iface)
{
	return do_end_document(OF(iface, vb_content_vtbl));
}

static HRESULT WINAPI vb_content_startPrefixMapping(IVBSAXContentHandler *iface, BSTR *prefix, BSTR *uri)
{
	return prefix && uri ? S_OK : E_POINTER;
}

static HRESULT WINAPI vb_content_endPrefixMapping(IVBSAXContentHandler *iface, BSTR *prefix)
{
	return prefix ? S_OK : E_POINTER;
}

static HRESULT WINAPI vb_content_startElement(IVBSAXContentHandler *iface, BSTR *uri, BSTR *local, BSTR *qname, IVBSAXAttributes *attributes)
{
	if (!uri || !local || !qname)
		return E_POINTER;
	return do_start_element(OF(iface, vb_content_vtbl), *uri, *local, *qname, TEXT_LENGTH(qname), NULL, attributes);
}

static HRESULT WINAPI vb_content_endElement(IVBSAXContentHandler *iface, BSTR *uri, BSTR *local, BSTR *qname)
{
	if (!uri || !local || !qname)
		return E_POINTER;
	return do_end_element(OF(iface, vb_content_vtbl), *uri, *local, *qname, TEXT_LENGTH(qname));
}

static HRESULT WINAPI vb_content_characters(IVBSAXContentHandler *iface, BSTR *text)
{
	if (!text)
		return E_POINTER;
	return do_characters(OF(iface, vb_content_vtbl), *text, TEXT_LENGTH(text));
}

static HRESULT WINAPI vb_content_ignorableWhitespace(IVBSAXContentHandler *iface, BSTR *text)
{
	if (!text)
		return E_POINTER;
	return do_ignorable(OF(iface, vb_content_vtbl), *text, TEXT_LENGTH(text));
}

static HRESULT WINAPI vb_content_processingInstruction(IVBSAXContentHandler *iface, BSTR *target, BSTR *data)
{
	if (!target || !data)
		return E_POINTER;
	return do_instruction(OF(iface, vb_content_vtbl), *target, TEXT_LENGTH(target), *data, TEXT_LENGTH(data));
}

static HRESULT WINAPI vb_content_skippedEntity(IVBSAXContentHandler *iface, BSTR *name)
{
	if (!name)
		return E_POINTER;
	return do_skipped(OF(iface, vb_content_vtbl), *name, TEXT_LENGTH(name));
}

static const IVBSAXContentHandlerVtbl vb_content_table = {
	VB_FACE_HEAD(vb_content), vb_content_putref_documentLocator, vb_content_startDocument, vb_content_endDocument,
	vb_content_startPrefixMapping, vb_content_endPrefixMapping, vb_content_startElement, vb_content_endElement, vb_content_characters,
	vb_content_ignorableWhitespace, vb_content_processingInstruction, vb_content_skippedEntity,
};

FACE_UNKNOWN(vb_lexical, IVBSAXLexicalHandler, vb_lexical_vtbl)
FACE_DISPATCH(vb_lexical, IVBSAXLexicalHandler)

static HRESULT WINAPI vb_lexical_startDTD(IVBSAXLexicalHandler *iface, BSTR *name, BSTR *public_id, BSTR *system_id)
{
	if (!name || !public_id || !system_id)
		return E_POINTER;
	return do_start_dtd(OF(iface, vb_lexical_vtbl), *name, TEXT_LENGTH(name), *public_id, TEXT_LENGTH(public_id), *system_id, TEXT_LENGTH(system_id));
}

static HRESULT WINAPI vb_lexical_endDTD(IVBSAXLexicalHandler *iface)
{
	return do_end_dtd(OF(iface, vb_lexical_vtbl));
}

static HRESULT WINAPI vb_lexical_startEntity(IVBSAXLexicalHandler *iface, BSTR *name)
{
	return name ? S_OK : E_POINTER;
}

static HRESULT WINAPI vb_lexical_endEntity(IVBSAXLexicalHandler *iface, BSTR *name)
{
	return name ? S_OK : E_POINTER;
}

static HRESULT WINAPI vb_lexical_startCDATA(IVBSAXLexicalHandler *iface)
{
	return do_start_cdata(OF(iface, vb_lexical_vtbl));
}

static HRESULT WINAPI vb_lexical_endCDATA(IVBSAXLexicalHandler *iface)
{
	return do_end_cdata(OF(iface, vb_lexical_vtbl));
}

static HRESULT WINAPI vb_lexical_comment(IVBSAXLexicalHandler *iface, BSTR *text)
{
	if (!text)
		return E_POINTER;
	return do_comment(OF(iface, vb_lexical_vtbl), *text, TEXT_LENGTH(text));
}

static const IVBSAXLexicalHandlerVtbl vb_lexical_table = {
	VB_FACE_HEAD(vb_lexical), vb_lexical_startDTD, vb_lexical_endDTD, vb_lexical_startEntity, vb_lexical_endEntity,
	vb_lexical_startCDATA, vb_lexical_endCDATA, vb_lexical_comment,
};

FACE_UNKNOWN(vb_decl, IVBSAXDeclHandler, vb_decl_vtbl)
FACE_DISPATCH(vb_decl, IVBSAXDeclHandler)

static HRESULT WINAPI vb_decl_elementDecl(IVBSAXDeclHandler *iface, BSTR *name, BSTR *model)
{
	if (!name || !model)
		return E_POINTER;
	return do_element_decl(OF(iface, vb_decl_vtbl), *name, TEXT_LENGTH(name), *model, TEXT_LENGTH(model));
}

static HRESULT WINAPI vb_decl_attributeDecl(IVBSAXDeclHandler *iface, BSTR *element, BSTR *name, BSTR *type, BSTR *fallback, BSTR *value)
{
	if (!element || !name || !type || !fallback || !value)
		return E_POINTER;
	return do_attribute_decl(OF(iface, vb_decl_vtbl), *element, TEXT_LENGTH(element), *name, TEXT_LENGTH(name), *type, TEXT_LENGTH(type), *fallback, TEXT_LENGTH(fallback), *value, TEXT_LENGTH(value));
}

static HRESULT WINAPI vb_decl_internalEntityDecl(IVBSAXDeclHandler *iface, BSTR *name, BSTR *value)
{
	if (!name || !value)
		return E_POINTER;
	return do_internal_entity(OF(iface, vb_decl_vtbl), *name, TEXT_LENGTH(name), *value, TEXT_LENGTH(value));
}

static HRESULT WINAPI vb_decl_externalEntityDecl(IVBSAXDeclHandler *iface, BSTR *name, BSTR *public_id, BSTR *system_id)
{
	if (!name || !public_id || !system_id)
		return E_POINTER;
	return do_external_entity(OF(iface, vb_decl_vtbl), *name, TEXT_LENGTH(name), *public_id, TEXT_LENGTH(public_id), *system_id, TEXT_LENGTH(system_id));
}

static const IVBSAXDeclHandlerVtbl vb_decl_table = {
	VB_FACE_HEAD(vb_decl), vb_decl_elementDecl, vb_decl_attributeDecl, vb_decl_internalEntityDecl, vb_decl_externalEntityDecl,
};

FACE_UNKNOWN(vb_dtd, IVBSAXDTDHandler, vb_dtd_vtbl)
FACE_DISPATCH(vb_dtd, IVBSAXDTDHandler)

static HRESULT WINAPI vb_dtd_notationDecl(IVBSAXDTDHandler *iface, BSTR *name, BSTR *public_id, BSTR *system_id)
{
	if (!name || !public_id || !system_id)
		return E_POINTER;
	return do_notation(OF(iface, vb_dtd_vtbl), *name, TEXT_LENGTH(name), *public_id, TEXT_LENGTH(public_id), *system_id, TEXT_LENGTH(system_id));
}

static HRESULT WINAPI vb_dtd_unparsedEntityDecl(IVBSAXDTDHandler *iface, BSTR *name, BSTR *public_id, BSTR *system_id, BSTR *notation)
{
	if (!name || !public_id || !system_id || !notation)
		return E_POINTER;
	return do_unparsed(OF(iface, vb_dtd_vtbl), *name, TEXT_LENGTH(name), *public_id, TEXT_LENGTH(public_id), *system_id, TEXT_LENGTH(system_id), *notation, TEXT_LENGTH(notation));
}

static const IVBSAXDTDHandlerVtbl vb_dtd_table = {
	VB_FACE_HEAD(vb_dtd), vb_dtd_notationDecl, vb_dtd_unparsedEntityDecl,
};

FACE_UNKNOWN(vb_errors, IVBSAXErrorHandler, vb_error_vtbl)
FACE_DISPATCH(vb_errors, IVBSAXErrorHandler)

static HRESULT WINAPI vb_errors_error(IVBSAXErrorHandler *iface, IVBSAXLocator *locator, BSTR *message, LONG code)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI vb_errors_fatalError(IVBSAXErrorHandler *iface, IVBSAXLocator *locator, BSTR *message, LONG code)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI vb_errors_ignorableWarning(IVBSAXErrorHandler *iface, IVBSAXLocator *locator, BSTR *message, LONG code)
{
	return E_NOTIMPL;
}

static const IVBSAXErrorHandlerVtbl vb_errors_table = {
	VB_FACE_HEAD(vb_errors), vb_errors_error, vb_errors_fatalError, vb_errors_ignorableWarning,
};

HRESULT mx_writer_create(mx_version version, REFIID riid, void **out)
{
	mx_writer *self = mx_zalloc(sizeof(*self));
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &writer_table;
	self->content_vtbl = &content_table;
	self->lexical_vtbl = &lexical_table;
	self->decl_vtbl = &decl_table;
	self->dtd_vtbl = &dtd_table;
	self->error_vtbl = &errors_table;
	self->vb_content_vtbl = &vb_content_table;
	self->vb_lexical_vtbl = &vb_lexical_table;
	self->vb_decl_vtbl = &vb_decl_table;
	self->vb_dtd_vtbl = &vb_dtd_table;
	self->vb_error_vtbl = &vb_errors_table;
	mx_dispex_init(&self->dispex, self, FALSE);
	self->refs = 1;
	self->version = version;
	self->bom = VARIANT_TRUE;
	self->indent = VARIANT_FALSE;
	self->standalone = VARIANT_FALSE;
	self->omit_declaration = VARIANT_FALSE;
	self->no_escaping = VARIANT_FALSE;
	self->encoding = SysAllocString(L"UTF-16");
	self->xml_version = SysAllocString(L"1.0");
	self->line_start = TRUE;
	status = IMXWriter_QueryInterface((IMXWriter *)self, riid, out);
	IMXWriter_Release((IMXWriter *)self);
	return status;
}
