/* IXmlWriter: text goes out as UTF-16 into a buffer that is encoded and handed to the stream on Flush, or when it gets large. A start tag stays
   open until something else is written, so attributes can still join it and an empty element can close in place. */

#include "shared.h"

struct writer_output
{
	IUnknownVtbl *vtable;
	LONG references;
	IMalloc *allocator;
	IUnknown *stream;
	UINT code_page;
};

struct open_element
{
	WCHAR *qualified;
	UINT binding_mark;
	BOOL has_content, has_child;
};

struct declared_prefix
{
	WCHAR *prefix, *uri;
	BOOL written;
};

enum writer_phase { phase_none, phase_ready, phase_prolog, phase_document_started, phase_content, phase_finished, phase_failed };

struct writer
{
	IXmlWriter iface;
	LONG references;
	IMalloc *allocator;
	struct writer_output *output;
	ISequentialStream *stream;
	IUnknown *languages;
	LONG indent, byte_order_mark, omit_declaration, conformance;
	enum writer_phase phase;
	BOOL tag_open, in_attribute, wrote_bom, attribute_seen;
	UINT code_page;
	WCHAR *buffer;
	UINT buffered, capacity;
	struct open_element *elements;
	UINT element_count, element_capacity;
	struct declared_prefix *prefixes;
	UINT prefix_count, prefix_capacity;
};

static struct writer *writer_of(IXmlWriter *iface)
{
	return CONTAINING_RECORD(iface, struct writer, iface);
}

static HRESULT flush_buffer(struct writer *writer)
{
	if (!writer->stream)
		return E_UNEXPECTED;
	HRESULT result = S_OK;
	if (!writer->wrote_bom) {
		writer->wrote_bom = TRUE;
		if (writer->byte_order_mark && writer->code_page == 1200) {
			static const unsigned char mark[] = { 0xff, 0xfe };
			result = ISequentialStream_Write(writer->stream, mark, sizeof(mark), NULL);
			if (FAILED(result))
				return result;
		}
	}
	if (!writer->buffered)
		return S_OK;
	if (writer->code_page == 1200) {
		result = ISequentialStream_Write(writer->stream, writer->buffer, writer->buffered * sizeof(WCHAR), NULL);
	} else {
		int needed = WideCharToMultiByte(writer->code_page, 0, writer->buffer, (int)writer->buffered, NULL, 0, NULL, NULL);
		char *bytes = needed ? HeapAlloc(GetProcessHeap(), 0, (size_t)needed) : NULL;
		if (!bytes)
			return needed ? E_OUTOFMEMORY : MX_E_ENCODING;
		WideCharToMultiByte(writer->code_page, 0, writer->buffer, (int)writer->buffered, bytes, needed, NULL, NULL);
		result = ISequentialStream_Write(writer->stream, bytes, (ULONG)needed, NULL);
		HeapFree(GetProcessHeap(), 0, bytes);
	}
	writer->buffered = 0;
	return FAILED(result) ? result : S_OK;
}

static HRESULT put_text(struct writer *writer, const WCHAR *text, UINT length)
{
	if (writer->capacity - writer->buffered < length) {
		HRESULT result = flush_buffer(writer);
		if (FAILED(result))
			return result;
		if (writer->capacity < length) {
			UINT wider = length * 2;
			WCHAR *larger = memory_get(writer->allocator, wider * sizeof(WCHAR));
			if (!larger)
				return E_OUTOFMEMORY;
			memory_put(writer->allocator, writer->buffer);
			writer->buffer = larger;
			writer->capacity = wider;
		}
	}
	CopyMemory(writer->buffer + writer->buffered, text, length * sizeof(WCHAR));
	writer->buffered += length;
	return S_OK;
}

static HRESULT put_string(struct writer *writer, const WCHAR *text)
{
	return put_text(writer, text, (UINT)lstrlenW(text));
}

static HRESULT put_hex_entity(struct writer *writer, UINT value)
{
	WCHAR digits[16];
	UINT at = ARRAY_SIZE(digits);
	digits[--at] = 0;
	digits[--at] = ';';
	do {
		digits[--at] = L"0123456789abcdef"[value & 15];
		value >>= 4;
	} while (value);
	digits[--at] = 'x';
	digits[--at] = '#';
	digits[--at] = '&';
	return put_string(writer, digits + at);
}

static HRESULT put_escaped(struct writer *writer, const WCHAR *text, UINT length, BOOL in_attribute)
{
	HRESULT result = S_OK;
	for (UINT index = 0; index < length && SUCCEEDED(result); index++) {
		WCHAR ch = text[index];
		switch (ch) {
		case '<':
			result = put_string(writer, L"&lt;");
			break;
		case '>':
			result = put_string(writer, L"&gt;");
			break;
		case '&':
			result = put_string(writer, L"&amp;");
			break;
		case '"':
			result = in_attribute ? put_string(writer, L"&quot;") : put_text(writer, &ch, 1);
			break;
		case '\t':
		case '\n':
		case '\r':
			result = in_attribute ? put_hex_entity(writer, ch) : put_text(writer, &ch, 1);
			break;
		default:
			result = put_text(writer, &ch, 1);
			break;
		}
	}
	return result;
}

static HRESULT put_line_break(struct writer *writer)
{
	HRESULT result = put_string(writer, L"\r\n");
	for (UINT level = 0; level < writer->element_count && SUCCEEDED(result); level++)
		result = put_string(writer, L"  ");
	return result;
}

/* Comments, instructions and CDATA take a line of their own inside an element when indenting. */
static HRESULT break_inside_element(struct writer *writer)
{
	return writer->indent && writer->element_count ? put_line_break(writer) : S_OK;
}

static HRESULT put_declaration(struct writer *writer, XmlStandalone standalone)
{
	const WCHAR *encoding = writer->code_page == CP_UTF8 ? L"UTF-8" : writer->code_page == 1200 ? L"UTF-16" : name_of_code_page(writer->code_page);
	HRESULT result = put_string(writer, L"<?xml version=\"1.0\" encoding=\"");
	if (SUCCEEDED(result))
		result = put_string(writer, encoding ? encoding : L"UTF-8");
	if (SUCCEEDED(result))
		result = put_string(writer, L"\"");
	if (SUCCEEDED(result) && standalone != XmlStandalone_Omit)
		result = put_string(writer, standalone == XmlStandalone_Yes ? L" standalone=\"yes\"" : L" standalone=\"no\"");
	if (SUCCEEDED(result))
		result = put_string(writer, L"?>");
	if (SUCCEEDED(result) && writer->indent)
		result = put_string(writer, L"\r\n");
	return result;
}

static HRESULT put_namespace_attribute(struct writer *writer, const WCHAR *prefix, const WCHAR *uri);

/* Namespace declarations go last in a start tag, after the attributes that may still add to them. */
static HRESULT put_pending_declarations(struct writer *writer)
{
	for (UINT index = writer->element_count ? writer->elements[writer->element_count - 1].binding_mark : 0; index < writer->prefix_count; index++) {
		struct declared_prefix *declared = &writer->prefixes[index];
		if (declared->written)
			continue;
		declared->written = TRUE;
		HRESULT result = put_namespace_attribute(writer, declared->prefix, declared->uri);
		if (FAILED(result))
			return result;
	}
	return S_OK;
}

/* Whatever goes in an element ends the start tag it still has open. */
static HRESULT close_start_tag(struct writer *writer)
{
	if (!writer->tag_open)
		return S_OK;
	writer->tag_open = FALSE;
	HRESULT result = put_pending_declarations(writer);
	return FAILED(result) ? result : put_string(writer, L">");
}

static void note_child(struct writer *writer)
{
	if (writer->element_count)
		writer->elements[writer->element_count - 1].has_child = TRUE;
}

/* Everything that writes document content comes here first: the output must exist, and an unfinished attribute cannot be left open. */
static HRESULT prepare_content(struct writer *writer)
{
	if (!writer->stream)
		return E_UNEXPECTED;
	if (writer->phase == phase_failed || writer->phase == phase_finished)
		return WR_E_INVALIDACTION;
	if (writer->in_attribute)
		return WR_E_INVALIDACTION;
	if (writer->phase == phase_ready)
		writer->phase = phase_prolog;
	return close_start_tag(writer);
}

static BOOL is_declared(const struct writer *writer, const WCHAR *prefix, const WCHAR *uri)
{
	for (UINT index = writer->prefix_count; index--;) {
		if (!lstrcmpW(writer->prefixes[index].prefix, prefix))
			return !lstrcmpW(writer->prefixes[index].uri, uri);
	}
	return FALSE;
}

static HRESULT remember_prefix(struct writer *writer, const WCHAR *prefix, const WCHAR *uri)
{
	if (writer->prefix_count == writer->prefix_capacity) {
		UINT wider = writer->prefix_capacity ? writer->prefix_capacity * 2 : 16;
		struct declared_prefix *larger = memory_get(writer->allocator, wider * sizeof(*larger));
		if (!larger)
			return E_OUTOFMEMORY;
		if (writer->prefixes)
			CopyMemory(larger, writer->prefixes, writer->prefix_count * sizeof(*larger));
		memory_put(writer->allocator, writer->prefixes);
		writer->prefixes = larger;
		writer->prefix_capacity = wider;
	}
	WCHAR *prefix_copy = copy_text(writer->allocator, prefix, (UINT)lstrlenW(prefix));
	WCHAR *uri_copy = copy_text(writer->allocator, uri, (UINT)lstrlenW(uri));
	if (!prefix_copy || !uri_copy) {
		memory_put(writer->allocator, prefix_copy);
		memory_put(writer->allocator, uri_copy);
		return E_OUTOFMEMORY;
	}
	writer->prefixes[writer->prefix_count].prefix = prefix_copy;
	writer->prefixes[writer->prefix_count].uri = uri_copy;
	writer->prefixes[writer->prefix_count].written = FALSE;
	writer->prefix_count++;
	return S_OK;
}

static HRESULT put_namespace_attribute(struct writer *writer, const WCHAR *prefix, const WCHAR *uri)
{
	HRESULT result = put_string(writer, *prefix ? L" xmlns:" : L" xmlns");
	if (SUCCEEDED(result))
		result = put_string(writer, prefix);
	if (SUCCEEDED(result))
		result = put_string(writer, L"=\"");
	if (SUCCEEDED(result))
		result = put_escaped(writer, uri, (UINT)lstrlenW(uri), TRUE);
	if (SUCCEEDED(result))
		result = put_string(writer, L"\"");
	return result;
}

static HRESULT WINAPI writer_QueryInterface(IXmlWriter *iface, REFIID riid, void **object)
{
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IXmlWriter)) {
		*object = iface;
		IXmlWriter_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI writer_AddRef(IXmlWriter *iface)
{
	return InterlockedIncrement(&writer_of(iface)->references);
}

static void forget_elements(struct writer *writer)
{
	while (writer->element_count)
		memory_put(writer->allocator, writer->elements[--writer->element_count].qualified);
	while (writer->prefix_count) {
		struct declared_prefix *declared = &writer->prefixes[--writer->prefix_count];
		memory_put(writer->allocator, declared->prefix);
		memory_put(writer->allocator, declared->uri);
	}
}

static void drop_output(struct writer *writer)
{
	if (writer->stream) {
		flush_buffer(writer);
		ISequentialStream_Release(writer->stream);
		writer->stream = NULL;
	}
	if (writer->output) {
		IUnknown_Release((IUnknown *)writer->output);
		writer->output = NULL;
	}
	writer->buffered = 0;
}

static ULONG WINAPI writer_Release(IXmlWriter *iface)
{
	struct writer *writer = writer_of(iface);
	ULONG left = InterlockedDecrement(&writer->references);
	if (left)
		return left;
	IMalloc *allocator = writer->allocator;
	drop_output(writer);
	forget_elements(writer);
	if (writer->languages)
		IUnknown_Release(writer->languages);
	memory_put(allocator, writer->buffer);
	memory_put(allocator, writer->elements);
	memory_put(allocator, writer->prefixes);
	memory_put(allocator, writer);
	if (allocator)
		IMalloc_Release(allocator);
	return 0;
}

static ULONG WINAPI output_AddRef(IUnknown *iface)
{
	return InterlockedIncrement(&((struct writer_output *)iface)->references);
}

static HRESULT WINAPI output_QueryInterface(IUnknown *iface, REFIID riid, void **object)
{
	if (IsEqualGUID(riid, &IID_IUnknown)) {
		*object = iface;
		output_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI output_Release(IUnknown *iface)
{
	struct writer_output *output = (struct writer_output *)iface;
	ULONG left = InterlockedDecrement(&output->references);
	if (left)
		return left;
	IMalloc *allocator = output->allocator;
	IUnknown_Release(output->stream);
	memory_put(allocator, output);
	if (allocator)
		IMalloc_Release(allocator);
	return 0;
}

static IUnknownVtbl output_vtable = { output_QueryInterface, output_AddRef, output_Release };

static HRESULT WINAPI writer_SetOutput(IXmlWriter *iface, IUnknown *target)
{
	struct writer *writer = writer_of(iface);
	drop_output(writer);
	forget_elements(writer);
	writer->phase = phase_none;
	writer->tag_open = writer->in_attribute = writer->wrote_bom = FALSE;
	writer->code_page = CP_UTF8;
	if (!target)
		return S_OK;
	IUnknown *source = target;
	if (*(IUnknownVtbl **)target == &output_vtable) {
		writer->output = (struct writer_output *)target;
		IUnknown_AddRef(target);
		source = writer->output->stream;
		writer->code_page = writer->output->code_page;
	}
	if (FAILED(IUnknown_QueryInterface(source, &IID_ISequentialStream, (void **)&writer->stream))) {
		drop_output(writer);
		return E_NOINTERFACE;
	}
	writer->phase = phase_ready;
	return S_OK;
}

static HRESULT WINAPI writer_GetProperty(IXmlWriter *iface, UINT property, LONG_PTR *value)
{
	struct writer *writer = writer_of(iface);
	if (!value)
		return E_INVALIDARG;
	switch (property) {
	case XmlWriterProperty_MultiLanguage:
		*value = (LONG_PTR)writer->languages;
		return S_OK;
	case XmlWriterProperty_Indent:
		*value = writer->indent;
		return S_OK;
	case XmlWriterProperty_ByteOrderMark:
		*value = writer->byte_order_mark;
		return S_OK;
	case XmlWriterProperty_OmitXmlDeclaration:
		*value = writer->omit_declaration;
		return S_OK;
	case XmlWriterProperty_ConformanceLevel:
		*value = writer->conformance;
		return S_OK;
	default:
		return E_INVALIDARG;
	}
}

static HRESULT WINAPI writer_SetProperty(IXmlWriter *iface, UINT property, LONG_PTR value)
{
	struct writer *writer = writer_of(iface);
	switch (property) {
	case XmlWriterProperty_MultiLanguage:
		if (value)
			IUnknown_AddRef((IUnknown *)value);
		if (writer->languages)
			IUnknown_Release(writer->languages);
		writer->languages = (IUnknown *)value;
		return S_OK;
	case XmlWriterProperty_Indent:
		writer->indent = (LONG)value;
		return S_OK;
	case XmlWriterProperty_ByteOrderMark:
		writer->byte_order_mark = (LONG)value;
		return S_OK;
	case XmlWriterProperty_OmitXmlDeclaration:
		writer->omit_declaration = (LONG)value;
		return S_OK;
	case XmlWriterProperty_ConformanceLevel:
		writer->conformance = (LONG)value;
		return S_OK;
	default:
		return E_INVALIDARG;
	}
}

static HRESULT start_element(struct writer *writer, const WCHAR *prefix, const WCHAR *local, const WCHAR *uri)
{
	if (!local || !*local)
		return E_INVALIDARG;
	if (writer->phase == phase_finished)
		return WR_E_INVALIDACTION;
	HRESULT result = prepare_content(writer);
	if (FAILED(result))
		return result;
	if (writer->element_count == writer->element_capacity) {
		UINT wider = writer->element_capacity ? writer->element_capacity * 2 : 32;
		struct open_element *larger = memory_get(writer->allocator, wider * sizeof(*larger));
		if (!larger)
			return E_OUTOFMEMORY;
		if (writer->elements)
			CopyMemory(larger, writer->elements, writer->element_count * sizeof(*larger));
		memory_put(writer->allocator, writer->elements);
		writer->elements = larger;
		writer->element_capacity = wider;
	}
	if (writer->indent && (writer->element_count || writer->phase == phase_prolog) && writer->buffered + writer->wrote_bom) {
		if (FAILED(result = put_line_break(writer)))
			return result;
	}
	note_child(writer);
	if (!prefix)
		prefix = L"";
	UINT prefix_length = (UINT)lstrlenW(prefix), local_length = (UINT)lstrlenW(local);
	WCHAR *qualified = memory_get(writer->allocator, ((size_t)prefix_length + local_length + 2) * sizeof(WCHAR));
	if (!qualified)
		return E_OUTOFMEMORY;
	qualified[0] = 0;
	if (prefix_length) {
		lstrcpyW(qualified, prefix);
		lstrcatW(qualified, L":");
	}
	lstrcatW(qualified, local);
	struct open_element *element = &writer->elements[writer->element_count++];
	element->qualified = qualified;
	element->binding_mark = writer->prefix_count;
	element->has_content = element->has_child = FALSE;
	writer->phase = phase_content;
	writer->tag_open = TRUE;
	if (FAILED(result = put_string(writer, L"<")) || FAILED(result = put_string(writer, qualified)))
		return result;
	if (uri && (*uri || prefix_length) && !is_declared(writer, prefix, uri)) {
		if (FAILED(result = remember_prefix(writer, prefix, uri)))
			return result;
	}
	return S_OK;
}

static HRESULT WINAPI writer_WriteStartElement(IXmlWriter *iface, const WCHAR *prefix, const WCHAR *local, const WCHAR *uri)
{
	return start_element(writer_of(iface), prefix, local, uri);
}

static void pop_element(struct writer *writer)
{
	struct open_element *element = &writer->elements[--writer->element_count];
	while (writer->prefix_count > element->binding_mark) {
		struct declared_prefix *declared = &writer->prefixes[--writer->prefix_count];
		memory_put(writer->allocator, declared->prefix);
		memory_put(writer->allocator, declared->uri);
	}
	memory_put(writer->allocator, element->qualified);
	if (!writer->element_count)
		writer->phase = phase_prolog;
}

static HRESULT end_element(struct writer *writer, BOOL full)
{
	if (!writer->stream)
		return E_UNEXPECTED;
	if (!writer->element_count)
		return WR_E_INVALIDACTION;
	if (writer->in_attribute)
		return WR_E_INVALIDACTION;
	HRESULT result;
	if (writer->tag_open && !full) {
		writer->tag_open = FALSE;
		if (FAILED(result = put_pending_declarations(writer)))
			return result;
		result = put_string(writer, L" />");
		pop_element(writer);
		return result;
	}
	if (FAILED(result = close_start_tag(writer)))
		return result;
	struct open_element *element = &writer->elements[writer->element_count - 1];
	WCHAR *qualified = element->qualified;
	BOOL has_child = element->has_child;
	UINT level = writer->element_count;
	if (writer->indent && has_child) {
		writer->element_count = level - 1;
		result = put_line_break(writer);
		writer->element_count = level;
		if (FAILED(result))
			return result;
	}
	if (FAILED(result = put_string(writer, L"</")) || FAILED(result = put_string(writer, qualified)) || FAILED(result = put_string(writer, L">")))
		return result;
	pop_element(writer);
	return S_OK;
}

static HRESULT WINAPI writer_WriteEndElement(IXmlWriter *iface)
{
	return end_element(writer_of(iface), FALSE);
}

static HRESULT WINAPI writer_WriteFullEndElement(IXmlWriter *iface)
{
	return end_element(writer_of(iface), TRUE);
}

static HRESULT WINAPI writer_WriteAttributeString(IXmlWriter *iface, const WCHAR *prefix, const WCHAR *local, const WCHAR *uri, const WCHAR *value)
{
	struct writer *writer = writer_of(iface);
	if (!writer->stream)
		return E_UNEXPECTED;
	if (!local || !*local)
		return E_INVALIDARG;
	if (!writer->tag_open)
		return WR_E_INVALIDACTION;
	if (!prefix)
		prefix = L"";
	if (!value)
		value = L"";
	HRESULT result;
	/* An xmlns attribute is a declaration, however the caller spells it. */
	if (!lstrcmpW(prefix, L"xmlns") || (!*prefix && !lstrcmpW(local, L"xmlns"))) {
		const WCHAR *declared = *prefix ? local : L"";
		if (is_declared(writer, declared, value))
			return S_OK;
		return remember_prefix(writer, declared, value);
	}
	if (uri && *prefix && !is_declared(writer, prefix, uri) && lstrcmpW(prefix, L"xml") && FAILED(result = remember_prefix(writer, prefix, uri)))
		return result;
	if (FAILED(result = put_string(writer, L" ")))
		return result;
	if (*prefix && (FAILED(result = put_string(writer, prefix)) || FAILED(result = put_string(writer, L":"))))
		return result;
	if (FAILED(result = put_string(writer, local)) || FAILED(result = put_string(writer, L"=\"")) ||
	    FAILED(result = put_escaped(writer, value, (UINT)lstrlenW(value), TRUE)))
		return result;
	return put_string(writer, L"\"");
}

static HRESULT WINAPI writer_WriteAttributes(IXmlWriter *iface, IXmlReader *reader, BOOL defaults)
{
	if (!reader)
		return E_INVALIDARG;
	HRESULT result = IXmlReader_MoveToFirstAttribute(reader);
	while (result == S_OK) {
		if (defaults || !IXmlReader_IsDefault(reader)) {
			const WCHAR *prefix, *local, *uri, *value;
			IXmlReader_GetPrefix(reader, &prefix, NULL);
			IXmlReader_GetLocalName(reader, &local, NULL);
			IXmlReader_GetNamespaceUri(reader, &uri, NULL);
			IXmlReader_GetValue(reader, &value, NULL);
			HRESULT written = writer_WriteAttributeString(iface, prefix, local, uri, value);
			if (FAILED(written))
				return written;
		}
		result = IXmlReader_MoveToNextAttribute(reader);
	}
	return FAILED(result) ? result : S_OK;
}

static HRESULT write_text_content(struct writer *writer, const WCHAR *text, UINT length, BOOL escaped)
{
	HRESULT result = prepare_content(writer);
	if (FAILED(result))
		return result;
	if (writer->element_count)
		writer->elements[writer->element_count - 1].has_content = TRUE;
	return escaped ? put_escaped(writer, text, length, FALSE) : put_text(writer, text, length);
}

static HRESULT WINAPI writer_WriteCData(IXmlWriter *iface, const WCHAR *text)
{
	struct writer *writer = writer_of(iface);
	HRESULT result = prepare_content(writer);
	if (FAILED(result))
		return result;
	if (!text)
		text = L"";
	if (FAILED(result = break_inside_element(writer)) || FAILED(result = put_string(writer, L"<![CDATA[")))
		return result;
	/* A terminator in the text splits it into two sections. */
	for (const WCHAR *cursor = text; *cursor; cursor++) {
		if (cursor[0] == ']' && cursor[1] == ']' && cursor[2] == '>') {
			if (FAILED(result = put_string(writer, L"]]]]><![CDATA[>")))
				return result;
			cursor += 2;
			continue;
		}
		if (FAILED(result = put_text(writer, cursor, 1)))
			return result;
	}
	return put_string(writer, L"]]>");
}

static HRESULT WINAPI writer_WriteCharEntity(IXmlWriter *iface, WCHAR ch)
{
	struct writer *writer = writer_of(iface);
	HRESULT result = prepare_content(writer);
	return FAILED(result) ? result : put_hex_entity(writer, ch);
}

static HRESULT WINAPI writer_WriteChars(IXmlWriter *iface, const WCHAR *text, UINT length)
{
	if (!text && length)
		return E_INVALIDARG;
	return write_text_content(writer_of(iface), text, length, TRUE);
}

static HRESULT WINAPI writer_WriteComment(IXmlWriter *iface, const WCHAR *text)
{
	struct writer *writer = writer_of(iface);
	HRESULT result = prepare_content(writer);
	if (FAILED(result) || FAILED(result = break_inside_element(writer)) || FAILED(result = put_string(writer, L"<!--")))
		return result;
	note_child(writer);
	if (text) {
		/* A comment cannot hold a double dash or end in a dash. */
		for (const WCHAR *cursor = text; *cursor; cursor++) {
			if (FAILED(result = put_text(writer, cursor, 1)))
				return result;
			if (*cursor == '-' && (cursor[1] == '-' || !cursor[1]) && FAILED(result = put_string(writer, L" ")))
				return result;
		}
	}
	return put_string(writer, L"-->");
}

static HRESULT WINAPI writer_WriteDocType(IXmlWriter *iface, const WCHAR *name, const WCHAR *public_id, const WCHAR *system_id, const WCHAR *subset)
{
	struct writer *writer = writer_of(iface);
	if (!name || !*name)
		return E_INVALIDARG;
	if (writer->element_count)
		return WR_E_INVALIDACTION;
	HRESULT result = prepare_content(writer);
	if (FAILED(result) || FAILED(result = put_string(writer, L"<!DOCTYPE ")) || FAILED(result = put_string(writer, name)))
		return result;
	if (public_id) {
		if (FAILED(result = put_string(writer, L" PUBLIC \"")) || FAILED(result = put_string(writer, public_id)) ||
		    FAILED(result = put_string(writer, L"\" \"")) || FAILED(result = put_string(writer, system_id ? system_id : L"")) ||
		    FAILED(result = put_string(writer, L"\"")))
			return result;
	} else if (system_id) {
		if (FAILED(result = put_string(writer, L" SYSTEM \"")) || FAILED(result = put_string(writer, system_id)) || FAILED(result = put_string(writer, L"\"")))
			return result;
	}
	if (subset && (FAILED(result = put_string(writer, L" [")) || FAILED(result = put_string(writer, subset)) || FAILED(result = put_string(writer, L"]"))))
		return result;
	return put_string(writer, L">");
}

static HRESULT WINAPI writer_WriteString(IXmlWriter *iface, const WCHAR *text)
{
	return write_text_content(writer_of(iface), text ? text : L"", text ? (UINT)lstrlenW(text) : 0, TRUE);
}

static HRESULT WINAPI writer_WriteElementString(IXmlWriter *iface, const WCHAR *prefix, const WCHAR *local, const WCHAR *uri, const WCHAR *value)
{
	HRESULT result = writer_WriteStartElement(iface, prefix, local, uri);
	if (FAILED(result))
		return result;
	if (value && FAILED(result = writer_WriteString(iface, value)))
		return result;
	return value ? writer_WriteFullEndElement(iface) : writer_WriteEndElement(iface);
}

static HRESULT WINAPI writer_WriteEndDocument(IXmlWriter *iface)
{
	struct writer *writer = writer_of(iface);
	if (!writer->stream)
		return E_UNEXPECTED;
	while (writer->element_count) {
		HRESULT result = end_element(writer, FALSE);
		if (FAILED(result))
			return result;
	}
	if (writer->phase == phase_ready)
		return WR_E_INVALIDACTION;
	writer->phase = phase_finished;
	return flush_buffer(writer);
}

static HRESULT WINAPI writer_WriteEntityRef(IXmlWriter *iface, const WCHAR *name)
{
	struct writer *writer = writer_of(iface);
	if (!name || !*name)
		return E_INVALIDARG;
	HRESULT result = prepare_content(writer);
	if (FAILED(result) || FAILED(result = put_string(writer, L"&")) || FAILED(result = put_string(writer, name)))
		return result;
	return put_string(writer, L";");
}

static HRESULT WINAPI writer_WriteName(IXmlWriter *iface, const WCHAR *name)
{
	if (!name || !*name)
		return E_INVALIDARG;
	return write_text_content(writer_of(iface), name, (UINT)lstrlenW(name), FALSE);
}

static HRESULT WINAPI writer_WriteNmToken(IXmlWriter *iface, const WCHAR *token)
{
	return writer_WriteName(iface, token);
}

static HRESULT WINAPI writer_WriteProcessingInstruction(IXmlWriter *iface, const WCHAR *name, const WCHAR *text)
{
	struct writer *writer = writer_of(iface);
	if (!name || !*name)
		return E_INVALIDARG;
	if (!lstrcmpiW(name, L"xml"))
		return writer->phase == phase_ready && !writer->omit_declaration ? put_declaration(writer, XmlStandalone_Omit) : WR_E_INVALIDACTION;
	HRESULT result = prepare_content(writer);
	if (FAILED(result))
		return result;
	note_child(writer);
	if (FAILED(result = break_inside_element(writer)) || FAILED(result = put_string(writer, L"<?")) || FAILED(result = put_string(writer, name)))
		return result;
	if (text && *text && (FAILED(result = put_string(writer, L" ")) || FAILED(result = put_string(writer, text))))
		return result;
	return put_string(writer, L"?>");
}

static HRESULT WINAPI writer_WriteQualifiedName(IXmlWriter *iface, const WCHAR *local, const WCHAR *uri)
{
	struct writer *writer = writer_of(iface);
	if (!local || !*local)
		return E_INVALIDARG;
	HRESULT result = prepare_content(writer);
	if (FAILED(result))
		return result;
	if (uri && *uri) {
		for (UINT index = writer->prefix_count; index--;) {
			if (lstrcmpW(writer->prefixes[index].uri, uri))
				continue;
			if (*writer->prefixes[index].prefix) {
				if (FAILED(result = put_string(writer, writer->prefixes[index].prefix)) || FAILED(result = put_string(writer, L":")))
					return result;
			}
			return put_string(writer, local);
		}
		return WR_E_NAMESPACEUNDECLARED;
	}
	return put_string(writer, local);
}

static HRESULT WINAPI writer_WriteRaw(IXmlWriter *iface, const WCHAR *data)
{
	return write_text_content(writer_of(iface), data ? data : L"", data ? (UINT)lstrlenW(data) : 0, FALSE);
}

static HRESULT WINAPI writer_WriteRawChars(IXmlWriter *iface, const WCHAR *data, UINT length)
{
	if (!data && length)
		return E_INVALIDARG;
	return write_text_content(writer_of(iface), data, length, FALSE);
}

static HRESULT WINAPI writer_WriteStartDocument(IXmlWriter *iface, XmlStandalone standalone)
{
	struct writer *writer = writer_of(iface);
	if (!writer->stream)
		return E_UNEXPECTED;
	if (writer->phase != phase_ready)
		return WR_E_INVALIDACTION;
	writer->phase = phase_prolog;
	return writer->omit_declaration ? S_OK : put_declaration(writer, standalone);
}

static HRESULT WINAPI writer_WriteSurrogateCharEntity(IXmlWriter *iface, WCHAR low, WCHAR high)
{
	struct writer *writer = writer_of(iface);
	if (!IS_SURROGATE_PAIR(high, low))
		return WC_E_XMLCHARACTER;
	HRESULT result = prepare_content(writer);
	return FAILED(result) ? result : put_hex_entity(writer, 0x10000 + (((UINT)high - 0xd800) << 10) + ((UINT)low - 0xdc00));
}

static HRESULT WINAPI writer_WriteWhitespace(IXmlWriter *iface, const WCHAR *text)
{
	if (!text)
		return E_INVALIDARG;
	for (const WCHAR *cursor = text; *cursor; cursor++) {
		if (*cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n')
			return WR_E_NONWHITESPACE;
	}
	return write_text_content(writer_of(iface), text, (UINT)lstrlenW(text), FALSE);
}

static HRESULT WINAPI writer_Flush(IXmlWriter *iface)
{
	struct writer *writer = writer_of(iface);
	if (!writer->stream)
		return S_OK;
	return flush_buffer(writer);
}

/* One node of a reader, without going below it: elements stay open, which is what the deep variant then fills. */
static HRESULT write_reader_node(IXmlWriter *iface, IXmlReader *reader, BOOL defaults, XmlNodeType type)
{
	const WCHAR *prefix, *local, *uri, *value;
	HRESULT result = S_OK;
	switch (type) {
	case XmlNodeType_Element:
		IXmlReader_GetPrefix(reader, &prefix, NULL);
		IXmlReader_GetLocalName(reader, &local, NULL);
		IXmlReader_GetNamespaceUri(reader, &uri, NULL);
		if (FAILED(result = writer_WriteStartElement(iface, prefix, local, uri)) || FAILED(result = writer_WriteAttributes(iface, reader, defaults)))
			return result;
		IXmlReader_MoveToElement(reader);
		return IXmlReader_IsEmptyElement(reader) ? writer_WriteEndElement(iface) : S_OK;
	case XmlNodeType_EndElement:
		return writer_WriteFullEndElement(iface);
	case XmlNodeType_Text:
		IXmlReader_GetValue(reader, &value, NULL);
		return writer_WriteString(iface, value);
	case XmlNodeType_Whitespace:
		IXmlReader_GetValue(reader, &value, NULL);
		return writer_WriteWhitespace(iface, value);
	case XmlNodeType_CDATA:
		IXmlReader_GetValue(reader, &value, NULL);
		return writer_WriteCData(iface, value);
	case XmlNodeType_Comment:
		IXmlReader_GetValue(reader, &value, NULL);
		return writer_WriteComment(iface, value);
	case XmlNodeType_ProcessingInstruction:
		IXmlReader_GetLocalName(reader, &local, NULL);
		IXmlReader_GetValue(reader, &value, NULL);
		return writer_WriteProcessingInstruction(iface, local, value);
	case XmlNodeType_XmlDeclaration:
		return writer_WriteStartDocument(iface, XmlStandalone_Omit);
	default:
		return S_OK;
	}
}

static HRESULT WINAPI writer_WriteNodeShallow(IXmlWriter *iface, IXmlReader *reader, BOOL defaults)
{
	if (!reader)
		return E_INVALIDARG;
	XmlNodeType type;
	if (FAILED(IXmlReader_GetNodeType(reader, &type)))
		return E_INVALIDARG;
	return write_reader_node(iface, reader, defaults, type);
}

static HRESULT WINAPI writer_WriteNode(IXmlWriter *iface, IXmlReader *reader, BOOL defaults)
{
	if (!reader)
		return E_INVALIDARG;
	XmlNodeType type;
	UINT depth = 0;
	if (IXmlReader_GetNodeType(reader, &type) != S_OK && (FAILED(IXmlReader_Read(reader, &type)) || type == XmlNodeType_None))
		return E_INVALIDARG;
	HRESULT result;
	/* Starting on an element copies up to its end; starting anywhere else copies that one node. */
	do {
		if (FAILED(result = write_reader_node(iface, reader, defaults, type)))
			return result;
		if (type == XmlNodeType_Element && !IXmlReader_IsEmptyElement(reader))
			depth++;
		else if (type == XmlNodeType_EndElement)
			depth--;
		if (!depth)
			break;
		result = IXmlReader_Read(reader, &type);
		if (FAILED(result))
			return result;
	} while (result == S_OK);
	result = IXmlReader_Read(reader, &type);
	return FAILED(result) ? result : S_OK;
}

static const IXmlWriterVtbl writer_vtable = {
	writer_QueryInterface, writer_AddRef, writer_Release, writer_SetOutput, writer_GetProperty, writer_SetProperty, writer_WriteAttributes,
	writer_WriteAttributeString, writer_WriteCData, writer_WriteCharEntity, writer_WriteChars, writer_WriteComment, writer_WriteDocType,
	writer_WriteElementString, writer_WriteEndDocument, writer_WriteEndElement, writer_WriteEntityRef, writer_WriteFullEndElement, writer_WriteName,
	writer_WriteNmToken, writer_WriteNode, writer_WriteNodeShallow, writer_WriteProcessingInstruction, writer_WriteQualifiedName, writer_WriteRaw,
	writer_WriteRawChars, writer_WriteStartDocument, writer_WriteStartElement, writer_WriteString, writer_WriteSurrogateCharEntity, writer_WriteWhitespace,
	writer_Flush,
};

HRESULT WINAPI CreateXmlWriter(REFIID riid, void **object, IMalloc *allocator)
{
	*object = NULL;
	if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IXmlWriter))
		return E_NOINTERFACE;
	struct writer *writer = memory_get(allocator, sizeof(*writer));
	if (!writer)
		return E_OUTOFMEMORY;
	ZeroMemory(writer, sizeof(*writer));
	writer->iface.lpVtbl = &writer_vtable;
	writer->references = 1;
	writer->allocator = allocator;
	if (allocator)
		IMalloc_AddRef(allocator);
	writer->byte_order_mark = TRUE;
	writer->conformance = XmlConformanceLevel_Document;
	writer->code_page = CP_UTF8;
	writer->capacity = 4096;
	writer->buffer = memory_get(allocator, writer->capacity * sizeof(WCHAR));
	if (!writer->buffer) {
		memory_put(allocator, writer);
		if (allocator)
			IMalloc_Release(allocator);
		return E_OUTOFMEMORY;
	}
	*object = &writer->iface;
	return S_OK;
}

static HRESULT new_output(IUnknown *stream, IMalloc *allocator, UINT code_page, IUnknown **output)
{
	struct writer_output *created = memory_get(allocator, sizeof(*created));
	if (!created)
		return E_OUTOFMEMORY;
	created->vtable = &output_vtable;
	created->references = 1;
	created->allocator = allocator;
	if (allocator)
		IMalloc_AddRef(allocator);
	created->stream = stream;
	IUnknown_AddRef(stream);
	created->code_page = code_page;
	*output = (IUnknown *)created;
	return S_OK;
}

HRESULT WINAPI CreateXmlWriterOutputWithEncodingCodePage(IUnknown *stream, IMalloc *allocator, UINT code_page, IUnknown **output)
{
	if (!stream || !output)
		return E_INVALIDARG;
	return new_output(stream, allocator, code_page, output);
}

HRESULT WINAPI CreateXmlWriterOutputWithEncodingName(IUnknown *stream, IMalloc *allocator, const WCHAR *encoding, IUnknown **output)
{
	if (!stream || !output || !encoding)
		return E_INVALIDARG;
	UINT page = code_page_of_name(encoding);
	if (!page)
		return E_INVALIDARG;
	return new_output(stream, allocator, page, output);
}
