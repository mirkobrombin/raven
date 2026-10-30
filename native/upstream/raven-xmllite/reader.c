/* IXmlReader: a pull parser over one document, written from the XML 1.0 and Namespaces in XML specifications and the public XmlLite documentation.
   The whole input is read and turned to UTF-16 on the first Read; names and values are built per node and stay valid until the next Read. */

#include "shared.h"

/* Marks the input objects CreateXmlReaderInputWith... hand out, which only this reader can unwrap. */
struct reader_input
{
	IUnknownVtbl *vtable;
	LONG references;
	IMalloc *allocator;
	IUnknown *stream;
	UINT code_page;
	BOOL hint_only;
	WCHAR *base_uri;
};

struct attribute
{
	WCHAR *prefix, *local, *qualified, *uri, *value;
	UINT prefix_length, local_length, qualified_length, uri_length, value_length, line, position;
};

struct open_element
{
	WCHAR *qualified;
	UINT qualified_length, binding_mark;
};

struct binding
{
	WCHAR *prefix, *uri;
	UINT uri_length;
};

/* A block of the per-node arena; blocks stay until the next Read so returned pointers do not move. */
struct arena_block
{
	struct arena_block *next;
	size_t used, size;
	WCHAR text[1];
};

struct reader
{
	IXmlReader iface;
	LONG references;
	IMalloc *allocator;
	struct reader_input *input;
	IUnknown *resolver, *languages;
	LONG conformance, dtd, max_depth, max_expansion, random_access;
	XmlReadState state;
	HRESULT failure;
	WCHAR *document;
	const WCHAR *cursor, *end;
	UINT line;
	const WCHAR *line_start;
	struct arena_block *arena;
	XmlNodeType type;
	WCHAR *prefix, *local, *qualified, *uri, *value;
	UINT prefix_length, local_length, qualified_length, uri_length, value_length, value_read, node_line, node_position;
	struct attribute *attributes;
	UINT attribute_count, attribute_capacity;
	int current_attribute;
	struct open_element *elements;
	UINT element_count, element_capacity, depth;
	struct binding *bindings;
	UINT binding_count, binding_capacity;
	BOOL is_empty, pending_pop, root_seen, root_done, started;
};

static const WCHAR uri_xml[] = L"http://www.w3.org/XML/1998/namespace";
static const WCHAR uri_xmlns[] = L"http://www.w3.org/2000/xmlns/";

static struct reader *reader_of(IXmlReader *iface)
{
	return CONTAINING_RECORD(iface, struct reader, iface);
}

/* The first block stays for the next node: heap calls per node would dominate the cost of parsing. */
static void arena_reset(struct reader *reader)
{
	if (!reader->arena)
		return;
	while (reader->arena->next) {
		struct arena_block *next = reader->arena->next;
		memory_put(reader->allocator, reader->arena);
		reader->arena = next;
	}
	reader->arena->used = 0;
}

static void arena_clear(struct reader *reader)
{
	arena_reset(reader);
	memory_put(reader->allocator, reader->arena);
	reader->arena = NULL;
}

static WCHAR *arena_get(struct reader *reader, size_t length)
{
	struct arena_block *block = reader->arena;
	if (!block || block->size - block->used < length) {
		size_t size = length > 4000 ? length : 4000;
		block = memory_get(reader->allocator, offsetof(struct arena_block, text) + size * sizeof(WCHAR));
		if (!block)
			return NULL;
		block->size = size;
		block->used = 0;
		block->next = reader->arena;
		reader->arena = block;
	}
	WCHAR *text = block->text + block->used;
	block->used += length;
	return text;
}

static WCHAR *arena_copy(struct reader *reader, const WCHAR *text, UINT length)
{
	WCHAR *copy = arena_get(reader, (size_t)length + 1);
	if (!copy)
		return NULL;
	CopyMemory(copy, text, length * sizeof(WCHAR));
	copy[length] = 0;
	return copy;
}

static BOOL is_space(WCHAR ch)
{
	return ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r';
}

static BOOL is_name_start(WCHAR ch)
{
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == ':' || (ch >= 0xc0 && ch != 0xd7 && ch != 0xf7 && ch != 0x2000 && ch != 0xfffe && ch != 0xffff);
}

static BOOL is_name_char(WCHAR ch)
{
	return is_name_start(ch) || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == 0xb7;
}

static BOOL is_xml_char(UINT ch)
{
	return ch == 9 || ch == 10 || ch == 13 || (ch >= 0x20 && ch <= 0xd7ff) || (ch >= 0xe000 && ch <= 0xfffd) || (ch >= 0x10000 && ch <= 0x10ffff);
}

static BOOL at_text(const struct reader *reader, const WCHAR *text)
{
	const WCHAR *cursor = reader->cursor;
	for (; *text; text++, cursor++) {
		if (cursor >= reader->end || *cursor != *text)
			return FALSE;
	}
	return TRUE;
}

/* Moves on, keeping the line and column the node getters report. */
static void advance(struct reader *reader, size_t count)
{
	while (count-- && reader->cursor < reader->end) {
		if (*reader->cursor++ == '\n') {
			reader->line++;
			reader->line_start = reader->cursor;
		}
	}
}

static void skip_space(struct reader *reader)
{
	while (reader->cursor < reader->end && is_space(*reader->cursor))
		advance(reader, 1);
}

/* A node's position is where its name or content starts, past the markup that opens it. */
static void mark_node(struct reader *reader, UINT opening)
{
	reader->node_line = reader->line;
	reader->node_position = (UINT)(reader->cursor - reader->line_start) + 1 + opening;
}

static HRESULT fail_with(struct reader *reader, HRESULT failure)
{
	reader->failure = failure;
	reader->state = XmlReadState_Error;
	reader->type = XmlNodeType_None;
	reader->attribute_count = 0;
	reader->current_attribute = -1;
	return failure;
}

static HRESULT read_name(struct reader *reader, const WCHAR **name, UINT *length)
{
	const WCHAR *start = reader->cursor, *cursor = start;
	if (cursor >= reader->end || !is_name_start(*cursor))
		return cursor >= reader->end ? MX_E_INPUTEND : WC_E_NAMECHARACTER;
	while (cursor < reader->end && is_name_char(*cursor))
		cursor++;
	*name = start;
	*length = (UINT)(cursor - start);
	advance(reader, *length);
	return S_OK;
}

/* Splits at the colon a qualified name may have, which may be neither first nor last nor doubled. */
static HRESULT split_name(const WCHAR *name, UINT length, UINT *prefix_length)
{
	*prefix_length = 0;
	for (UINT index = 0; index < length; index++) {
		if (name[index] != ':')
			continue;
		if (*prefix_length || !index || index + 1 == length)
			return NC_E_QNAMECOLON;
		*prefix_length = index;
	}
	return S_OK;
}

static const struct binding *find_binding(const struct reader *reader, const WCHAR *prefix, UINT length)
{
	for (UINT index = reader->binding_count; index--;) {
		const struct binding *binding = &reader->bindings[index];
		if ((UINT)lstrlenW(binding->prefix) == length && !memcmp(binding->prefix, prefix, length * sizeof(WCHAR)))
			return binding;
	}
	return NULL;
}

static HRESULT add_binding(struct reader *reader, const WCHAR *prefix, UINT prefix_length, const WCHAR *uri, UINT uri_length)
{
	if (reader->binding_count == reader->binding_capacity) {
		UINT wider = reader->binding_capacity ? reader->binding_capacity * 2 : 16;
		struct binding *larger = memory_get(reader->allocator, wider * sizeof(*larger));
		if (!larger)
			return E_OUTOFMEMORY;
		if (reader->bindings)
			CopyMemory(larger, reader->bindings, reader->binding_count * sizeof(*larger));
		memory_put(reader->allocator, reader->bindings);
		reader->bindings = larger;
		reader->binding_capacity = wider;
	}
	struct binding *binding = &reader->bindings[reader->binding_count];
	binding->prefix = copy_text(reader->allocator, prefix, prefix_length);
	binding->uri = copy_text(reader->allocator, uri, uri_length);
	if (!binding->prefix || !binding->uri) {
		memory_put(reader->allocator, binding->prefix);
		memory_put(reader->allocator, binding->uri);
		return E_OUTOFMEMORY;
	}
	binding->uri_length = uri_length;
	reader->binding_count++;
	return S_OK;
}

static void drop_bindings(struct reader *reader, UINT mark)
{
	while (reader->binding_count > mark) {
		struct binding *binding = &reader->bindings[--reader->binding_count];
		memory_put(reader->allocator, binding->prefix);
		memory_put(reader->allocator, binding->uri);
	}
}

/* Reads a reference at `&`, leaving the code point in *value. */
static HRESULT read_reference(struct reader *reader, UINT *value)
{
	advance(reader, 1);
	if (at_text(reader, L"#")) {
		advance(reader, 1);
		BOOL hex = at_text(reader, L"x");
		if (hex)
			advance(reader, 1);
		UINT total = 0, digits = 0;
		while (reader->cursor < reader->end && *reader->cursor != ';') {
			WCHAR ch = *reader->cursor;
			UINT digit;
			if (ch >= '0' && ch <= '9')
				digit = ch - '0';
			else if (hex && ch >= 'a' && ch <= 'f')
				digit = ch - 'a' + 10;
			else if (hex && ch >= 'A' && ch <= 'F')
				digit = ch - 'A' + 10;
			else
				return hex ? WC_E_HEXDIGIT : WC_E_DIGIT;
			if (total <= 0x10ffff)
				total = total * (hex ? 16 : 10) + digit;
			digits++;
			advance(reader, 1);
		}
		if (reader->cursor >= reader->end)
			return MX_E_INPUTEND;
		if (!digits)
			return hex ? WC_E_HEXDIGIT : WC_E_DIGIT;
		if (!is_xml_char(total))
			return WC_E_XMLCHARACTER;
		advance(reader, 1);
		*value = total;
		return S_OK;
	}
	static const struct { const WCHAR *name; WCHAR value; } predefined[] = {
		{ L"lt;", '<' }, { L"gt;", '>' }, { L"amp;", '&' }, { L"apos;", '\'' }, { L"quot;", '"' },
	};
	for (size_t index = 0; index < ARRAY_SIZE(predefined); index++) {
		if (at_text(reader, predefined[index].name)) {
			advance(reader, (size_t)lstrlenW(predefined[index].name));
			*value = predefined[index].value;
			return S_OK;
		}
	}
	const WCHAR *name;
	UINT length;
	HRESULT failure = read_name(reader, &name, &length);
	if (FAILED(failure))
		return failure;
	return reader->cursor < reader->end && *reader->cursor == ';' ? WC_E_UNDECLAREDENTITY : WC_E_SEMICOLON;
}

static void store_code_point(WCHAR *target, UINT *length, UINT value)
{
	if (value >= 0x10000) {
		value -= 0x10000;
		target[(*length)++] = (WCHAR)(0xd800 | value >> 10);
		target[(*length)++] = (WCHAR)(0xdc00 | (value & 0x3ff));
	} else {
		target[(*length)++] = (WCHAR)value;
	}
}

/* An attribute value: references resolved and white space turned to spaces, which a character reference escapes. */
static HRESULT read_attribute_value(struct reader *reader, WCHAR **value, UINT *length)
{
	if (reader->cursor >= reader->end)
		return MX_E_INPUTEND;
	WCHAR quote = *reader->cursor;
	if (quote != '"' && quote != '\'')
		return WC_E_QUOTE;
	advance(reader, 1);
	const WCHAR *scan = reader->cursor;
	while (scan < reader->end && *scan != quote)
		scan++;
	if (scan >= reader->end)
		return MX_E_INPUTEND;
	WCHAR *target = arena_get(reader, (size_t)(scan - reader->cursor) + 1);
	if (!target)
		return E_OUTOFMEMORY;
	UINT used = 0;
	while (*reader->cursor != quote) {
		WCHAR ch = *reader->cursor;
		if (ch == '<')
			return WC_E_LESSTHAN;
		if (ch == '&') {
			UINT code;
			HRESULT failure = read_reference(reader, &code);
			if (FAILED(failure))
				return failure;
			store_code_point(target, &used, code);
			continue;
		}
		if (!is_xml_char(ch) && !(ch >= 0xd800 && ch <= 0xdfff))
			return WC_E_XMLCHARACTER;
		target[used++] = is_space(ch) ? ' ' : ch;
		advance(reader, 1);
	}
	advance(reader, 1);
	target[used] = 0;
	*value = target;
	*length = used;
	return S_OK;
}

static HRESULT add_attribute(struct reader *reader, struct attribute **added)
{
	if (reader->attribute_count == reader->attribute_capacity) {
		UINT wider = reader->attribute_capacity ? reader->attribute_capacity * 2 : 16;
		struct attribute *larger = memory_get(reader->allocator, wider * sizeof(*larger));
		if (!larger)
			return E_OUTOFMEMORY;
		if (reader->attributes)
			CopyMemory(larger, reader->attributes, reader->attribute_count * sizeof(*larger));
		memory_put(reader->allocator, reader->attributes);
		reader->attributes = larger;
		reader->attribute_capacity = wider;
	}
	*added = &reader->attributes[reader->attribute_count++];
	ZeroMemory(*added, sizeof(**added));
	return S_OK;
}

static void set_node_name(struct reader *reader, WCHAR *qualified, UINT length, UINT prefix_length)
{
	reader->qualified = qualified;
	reader->qualified_length = length;
	reader->prefix_length = prefix_length;
	reader->prefix = arena_copy(reader, qualified, prefix_length);
	reader->local = prefix_length ? qualified + prefix_length + 1 : qualified;
	reader->local_length = prefix_length ? length - prefix_length - 1 : length;
}

static void set_blank_node(struct reader *reader, XmlNodeType type)
{
	static WCHAR nothing[] = L"";
	reader->type = type;
	reader->prefix = reader->local = reader->qualified = reader->uri = reader->value = nothing;
	reader->prefix_length = reader->local_length = reader->qualified_length = reader->uri_length = reader->value_length = 0;
	reader->value_read = 0;
}

static HRESULT read_declaration(struct reader *reader)
{
	mark_node(reader, 2);
	advance(reader, 5);
	set_blank_node(reader, XmlNodeType_XmlDeclaration);
	reader->qualified = reader->local = arena_copy(reader, L"xml", 3);
	reader->qualified_length = reader->local_length = 3;
	for (;;) {
		BOOL spaced = reader->cursor < reader->end && is_space(*reader->cursor);
		skip_space(reader);
		if (at_text(reader, L"?>")) {
			advance(reader, 2);
			return S_OK;
		}
		if (reader->cursor >= reader->end)
			return MX_E_INPUTEND;
		if (!spaced)
			return WC_E_WHITESPACE;
		struct attribute *attribute;
		const WCHAR *name;
		UINT length, line = reader->line, position = (UINT)(reader->cursor - reader->line_start) + 1;
		HRESULT failure = read_name(reader, &name, &length);
		if (FAILED(failure))
			return failure;
		WCHAR *text = arena_copy(reader, name, length);
		skip_space(reader);
		if (!at_text(reader, L"="))
			return WC_E_EQUAL;
		advance(reader, 1);
		skip_space(reader);
		WCHAR *value;
		UINT value_length;
		if (FAILED(failure = read_attribute_value(reader, &value, &value_length)) || FAILED(failure = add_attribute(reader, &attribute)))
			return failure;
		attribute->prefix = attribute->uri = (WCHAR *)L"";
		attribute->qualified = attribute->local = text;
		attribute->qualified_length = attribute->local_length = length;
		attribute->value = value;
		attribute->value_length = value_length;
		attribute->line = line;
		attribute->position = position;
	}
}

static HRESULT read_comment(struct reader *reader)
{
	mark_node(reader, 4);
	advance(reader, 4);
	const WCHAR *start = reader->cursor;
	for (;;) {
		if (reader->cursor >= reader->end)
			return MX_E_INPUTEND;
		if (at_text(reader, L"--")) {
			if (!at_text(reader, L"-->"))
				return WC_E_COMMENT;
			break;
		}
		if (!is_xml_char(*reader->cursor) && !(*reader->cursor >= 0xd800 && *reader->cursor <= 0xdfff))
			return WC_E_XMLCHARACTER;
		advance(reader, 1);
	}
	set_blank_node(reader, XmlNodeType_Comment);
	reader->value_length = (UINT)(reader->cursor - start);
	reader->value = arena_copy(reader, start, reader->value_length);
	advance(reader, 3);
	return reader->value ? S_OK : E_OUTOFMEMORY;
}

static HRESULT read_cdata(struct reader *reader)
{
	mark_node(reader, 9);
	advance(reader, 9);
	const WCHAR *start = reader->cursor;
	while (!at_text(reader, L"]]>")) {
		if (reader->cursor >= reader->end)
			return MX_E_INPUTEND;
		advance(reader, 1);
	}
	set_blank_node(reader, XmlNodeType_CDATA);
	reader->value_length = (UINT)(reader->cursor - start);
	reader->value = arena_copy(reader, start, reader->value_length);
	advance(reader, 3);
	return reader->value ? S_OK : E_OUTOFMEMORY;
}

static HRESULT read_instruction(struct reader *reader)
{
	mark_node(reader, 2);
	advance(reader, 2);
	const WCHAR *name;
	UINT length;
	HRESULT failure = read_name(reader, &name, &length);
	if (FAILED(failure))
		return failure;
	if (length == 3 && (name[0] | 32) == 'x' && (name[1] | 32) == 'm' && (name[2] | 32) == 'l')
		return WC_E_PI;
	set_blank_node(reader, XmlNodeType_ProcessingInstruction);
	reader->qualified = reader->local = arena_copy(reader, name, length);
	reader->qualified_length = reader->local_length = length;
	if (!at_text(reader, L"?>") && !(reader->cursor < reader->end && is_space(*reader->cursor)))
		return reader->cursor >= reader->end ? MX_E_INPUTEND : WC_E_WHITESPACE;
	skip_space(reader);
	const WCHAR *start = reader->cursor;
	while (!at_text(reader, L"?>")) {
		if (reader->cursor >= reader->end)
			return MX_E_INPUTEND;
		advance(reader, 1);
	}
	reader->value_length = (UINT)(reader->cursor - start);
	reader->value = arena_copy(reader, start, reader->value_length);
	advance(reader, 2);
	return S_OK;
}

/* Keeps the name and steps over the internal subset: nothing a document declares there is applied. */
static HRESULT read_doctype(struct reader *reader)
{
	if (reader->dtd == DtdProcessing_Prohibit)
		return WC_E_DTDPROHIBITED;
	mark_node(reader, 2);
	advance(reader, 9);
	skip_space(reader);
	const WCHAR *name;
	UINT length;
	HRESULT failure = read_name(reader, &name, &length);
	if (FAILED(failure))
		return failure;
	set_blank_node(reader, XmlNodeType_DocumentType);
	reader->qualified = reader->local = arena_copy(reader, name, length);
	reader->qualified_length = reader->local_length = length;
	UINT nesting = 0;
	WCHAR quote = 0;
	while (reader->cursor < reader->end) {
		WCHAR ch = *reader->cursor;
		if (quote) {
			quote = ch == quote ? 0 : quote;
		} else if (ch == '"' || ch == '\'') {
			quote = ch;
		} else if (ch == '[') {
			nesting++;
		} else if (ch == ']' && nesting) {
			nesting--;
		} else if (ch == '>' && !nesting) {
			advance(reader, 1);
			return S_OK;
		}
		advance(reader, 1);
	}
	return MX_E_INPUTEND;
}

static HRESULT push_element(struct reader *reader, WCHAR *qualified, UINT length, UINT mark)
{
	if (reader->element_count == reader->element_capacity) {
		UINT wider = reader->element_capacity ? reader->element_capacity * 2 : 32;
		struct open_element *larger = memory_get(reader->allocator, wider * sizeof(*larger));
		if (!larger)
			return E_OUTOFMEMORY;
		if (reader->elements)
			CopyMemory(larger, reader->elements, reader->element_count * sizeof(*larger));
		memory_put(reader->allocator, reader->elements);
		reader->elements = larger;
		reader->element_capacity = wider;
	}
	WCHAR *copy = copy_text(reader->allocator, qualified, length);
	if (!copy)
		return E_OUTOFMEMORY;
	reader->elements[reader->element_count].qualified = copy;
	reader->elements[reader->element_count].qualified_length = length;
	reader->elements[reader->element_count].binding_mark = mark;
	reader->element_count++;
	return S_OK;
}

static HRESULT resolve_attribute_names(struct reader *reader)
{
	for (UINT index = 0; index < reader->attribute_count; index++) {
		struct attribute *attribute = &reader->attributes[index];
		if (attribute->prefix_length == 5 && !memcmp(attribute->prefix, L"xmlns", 5 * sizeof(WCHAR))) {
			attribute->uri = (WCHAR *)uri_xmlns;
			attribute->uri_length = ARRAY_SIZE(uri_xmlns) - 1;
		} else if (!attribute->prefix_length && attribute->qualified_length == 5 && !memcmp(attribute->qualified, L"xmlns", 5 * sizeof(WCHAR))) {
			attribute->uri = (WCHAR *)uri_xmlns;
			attribute->uri_length = ARRAY_SIZE(uri_xmlns) - 1;
		} else if (attribute->prefix_length == 3 && !memcmp(attribute->prefix, L"xml", 3 * sizeof(WCHAR))) {
			attribute->uri = (WCHAR *)uri_xml;
			attribute->uri_length = ARRAY_SIZE(uri_xml) - 1;
		} else if (attribute->prefix_length) {
			const struct binding *binding = find_binding(reader, attribute->prefix, attribute->prefix_length);
			if (!binding)
				return NC_E_UNDECLAREDPREFIX;
			attribute->uri = binding->uri;
			attribute->uri_length = binding->uri_length;
		} else {
			attribute->uri = (WCHAR *)L"";
		}
		for (UINT other = 0; other < index; other++) {
			const struct attribute *before = &reader->attributes[other];
			if (before->local_length == attribute->local_length && before->uri_length == attribute->uri_length &&
			    !lstrcmpW(before->local, attribute->local) && !lstrcmpW(before->uri, attribute->uri))
				return WC_E_UNIQUEATTRIBUTE;
		}
	}
	return S_OK;
}

static HRESULT read_element(struct reader *reader)
{
	if (reader->root_done && reader->conformance != XmlConformanceLevel_Fragment)
		return WC_E_SYNTAX;
	mark_node(reader, 1);
	advance(reader, 1);
	const WCHAR *name;
	UINT length, prefix_length, mark = reader->binding_count;
	HRESULT failure = read_name(reader, &name, &length);
	if (FAILED(failure) || FAILED(failure = split_name(name, length, &prefix_length)))
		return failure;
	WCHAR *qualified = arena_copy(reader, name, length);
	if (!qualified)
		return E_OUTOFMEMORY;
	set_blank_node(reader, XmlNodeType_Element);
	set_node_name(reader, qualified, length, prefix_length);

	for (;;) {
		BOOL spaced = reader->cursor < reader->end && is_space(*reader->cursor);
		skip_space(reader);
		if (reader->cursor >= reader->end)
			return MX_E_INPUTEND;
		if (*reader->cursor == '>' || at_text(reader, L"/>"))
			break;
		if (!spaced)
			return WC_E_WHITESPACE;
		struct attribute *attribute;
		const WCHAR *attribute_name;
		UINT attribute_length, attribute_prefix, line = reader->line, position = (UINT)(reader->cursor - reader->line_start) + 1;
		if (FAILED(failure = read_name(reader, &attribute_name, &attribute_length)) || FAILED(failure = split_name(attribute_name, attribute_length, &attribute_prefix)))
			return failure;
		WCHAR *attribute_text = arena_copy(reader, attribute_name, attribute_length);
		if (!attribute_text)
			return E_OUTOFMEMORY;
		skip_space(reader);
		if (!at_text(reader, L"="))
			return reader->cursor >= reader->end ? MX_E_INPUTEND : WC_E_EQUAL;
		advance(reader, 1);
		skip_space(reader);
		WCHAR *value;
		UINT value_length;
		if (FAILED(failure = read_attribute_value(reader, &value, &value_length)) || FAILED(failure = add_attribute(reader, &attribute)))
			return failure;
		attribute->qualified = attribute_text;
		attribute->qualified_length = attribute_length;
		attribute->prefix_length = attribute_prefix;
		attribute->prefix = arena_copy(reader, attribute_text, attribute_prefix);
		attribute->local = attribute_prefix ? attribute_text + attribute_prefix + 1 : attribute_text;
		attribute->local_length = attribute_prefix ? attribute_length - attribute_prefix - 1 : attribute_length;
		attribute->value = value;
		attribute->value_length = value_length;
		attribute->line = line;
		attribute->position = position;
		/* Bindings apply to the whole tag, including names before them, so they all go in before anything resolves. */
		if (attribute_prefix == 5 && !memcmp(attribute_text, L"xmlns", 5 * sizeof(WCHAR))) {
			if (!value_length)
				return NC_E_EMPTYURI;
			if (FAILED(failure = add_binding(reader, attribute->local, attribute->local_length, value, value_length)))
				return failure;
		} else if (!attribute_prefix && attribute_length == 5 && !memcmp(attribute_text, L"xmlns", 5 * sizeof(WCHAR))) {
			if (FAILED(failure = add_binding(reader, L"", 0, value, value_length)))
				return failure;
		}
	}
	if (FAILED(failure = resolve_attribute_names(reader)))
		return failure;
	if (prefix_length == 3 && !memcmp(name, L"xml", 3 * sizeof(WCHAR))) {
		reader->uri = (WCHAR *)uri_xml;
		reader->uri_length = ARRAY_SIZE(uri_xml) - 1;
	} else {
		const struct binding *binding = find_binding(reader, name, prefix_length);
		if (!binding && prefix_length)
			return NC_E_UNDECLAREDPREFIX;
		if (binding) {
			reader->uri = binding->uri;
			reader->uri_length = binding->uri_length;
		}
	}
	reader->root_seen = TRUE;
	reader->is_empty = *reader->cursor == '/';
	advance(reader, reader->is_empty ? 2 : 1);
	if (reader->is_empty) {
		reader->pending_pop = TRUE;
		if (!reader->element_count)
			reader->root_done = TRUE;
		return push_element(reader, qualified, length, mark);
	}
	return push_element(reader, qualified, length, mark);
}

static HRESULT read_end_element(struct reader *reader)
{
	mark_node(reader, 2);
	advance(reader, 2);
	const WCHAR *name;
	UINT length, prefix_length;
	HRESULT failure = read_name(reader, &name, &length);
	if (FAILED(failure))
		return failure;
	if (!reader->element_count)
		return WC_E_ELEMENTMATCH;
	const struct open_element *open = &reader->elements[reader->element_count - 1];
	if (open->qualified_length != length || memcmp(open->qualified, name, length * sizeof(WCHAR)))
		return WC_E_ELEMENTMATCH;
	skip_space(reader);
	if (!at_text(reader, L">"))
		return reader->cursor >= reader->end ? MX_E_INPUTEND : WC_E_GREATERTHAN;
	advance(reader, 1);
	if (FAILED(failure = split_name(name, length, &prefix_length)))
		return failure;
	WCHAR *qualified = arena_copy(reader, name, length);
	if (!qualified)
		return E_OUTOFMEMORY;
	set_blank_node(reader, XmlNodeType_EndElement);
	set_node_name(reader, qualified, length, prefix_length);
	if (prefix_length == 3 && !memcmp(name, L"xml", 3 * sizeof(WCHAR))) {
		reader->uri = (WCHAR *)uri_xml;
		reader->uri_length = ARRAY_SIZE(uri_xml) - 1;
	} else {
		const struct binding *binding = find_binding(reader, name, prefix_length);
		if (binding) {
			reader->uri = binding->uri;
			reader->uri_length = binding->uri_length;
		}
	}
	reader->pending_pop = TRUE;
	if (reader->element_count == 1)
		reader->root_done = TRUE;
	return S_OK;
}

static HRESULT read_text(struct reader *reader)
{
	mark_node(reader, 0);
	const WCHAR *scan = reader->cursor;
	while (scan < reader->end && *scan != '<')
		scan++;
	WCHAR *target = arena_get(reader, (size_t)(scan - reader->cursor) + 1);
	if (!target)
		return E_OUTOFMEMORY;
	UINT used = 0;
	BOOL only_space = TRUE;
	while (reader->cursor < scan) {
		WCHAR ch = *reader->cursor;
		if (ch == '&') {
			UINT code;
			HRESULT failure = read_reference(reader, &code);
			if (FAILED(failure))
				return failure;
			store_code_point(target, &used, code);
			only_space = FALSE;
			continue;
		}
		if (at_text(reader, L"]]>"))
			return WC_E_CDSECTEND;
		if (!is_xml_char(ch) && !(ch >= 0xd800 && ch <= 0xdfff))
			return WC_E_XMLCHARACTER;
		only_space = only_space && is_space(ch);
		target[used++] = ch;
		advance(reader, 1);
	}
	target[used] = 0;
	set_blank_node(reader, only_space ? XmlNodeType_Whitespace : XmlNodeType_Text);
	if (!only_space && !reader->element_count && reader->conformance != XmlConformanceLevel_Fragment)
		return WC_E_SYNTAX;
	reader->value = target;
	reader->value_length = used;
	return S_OK;
}

/* Everything up to the first node: gets the bytes and turns them into normalized UTF-16. */
static UINT sniff_declared_encoding(const unsigned char *bytes, size_t size)
{
	static const char signature[] = "<?xml";
	if (size < 6 || memcmp(bytes, signature, 5))
		return 0;
	for (size_t index = 5; index + 9 < size && bytes[index] != '>'; index++) {
		if (memcmp(bytes + index, "encoding", 8))
			continue;
		index += 8;
		while (index < size && bytes[index] != '"' && bytes[index] != '\'' && bytes[index] != '>')
			index++;
		if (index >= size || bytes[index] == '>')
			return 0;
		unsigned char quote = bytes[index++];
		WCHAR name[32];
		UINT used = 0;
		while (index < size && bytes[index] != quote && used + 1 < ARRAY_SIZE(name))
			name[used++] = bytes[index++];
		name[used] = 0;
		UINT page = code_page_of_name(name);
		return page ? page : (UINT)-1;
	}
	return 0;
}

static HRESULT load_document(struct reader *reader)
{
	IUnknown *source = reader->input->stream;
	ISequentialStream *stream = NULL;
	if (FAILED(IUnknown_QueryInterface(source, &IID_ISequentialStream, (void **)&stream)))
		return E_NOINTERFACE;
	size_t size = 0, capacity = 65536;
	unsigned char *bytes = HeapAlloc(GetProcessHeap(), 0, capacity);
	HRESULT result = bytes ? S_OK : E_OUTOFMEMORY;
	while (SUCCEEDED(result)) {
		if (size == capacity) {
			unsigned char *larger = HeapReAlloc(GetProcessHeap(), 0, bytes, capacity * 2);
			if (!larger) {
				result = E_OUTOFMEMORY;
				break;
			}
			bytes = larger;
			capacity *= 2;
		}
		ULONG got = 0;
		HRESULT status = ISequentialStream_Read(stream, bytes + size, (ULONG)(capacity - size), &got);
		if (FAILED(status)) {
			result = status;
			break;
		}
		size += got;
		if (status != S_OK || !got)
			break;
	}
	ISequentialStream_Release(stream);
	if (FAILED(result))
		goto done;

	const unsigned char *data = bytes;
	size_t data_size = size;
	UINT page = reader->input->code_page;
	if (size >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
		page = 1200;
		data += 2;
		data_size -= 2;
	} else if (size >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) {
		page = 1201;
		data += 2;
		data_size -= 2;
	} else if (size >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
		page = CP_UTF8;
		data += 3;
		data_size -= 3;
	} else if (size >= 2 && bytes[0] == '<' && !bytes[1]) {
		page = 1200;
	} else if (size >= 2 && !bytes[0] && bytes[1] == '<') {
		page = 1201;
	} else if (!page || reader->input->hint_only) {
		UINT declared = sniff_declared_encoding(bytes, size);
		if (declared == (UINT)-1) {
			result = WC_E_ENCNAME;
			goto done;
		}
		page = declared ? declared : CP_UTF8;
	}

	size_t length;
	WCHAR *text;
	if (page == 1200 || page == 1201) {
		length = data_size / 2;
		text = memory_get(reader->allocator, (length + 1) * sizeof(WCHAR));
		if (!text) {
			result = E_OUTOFMEMORY;
			goto done;
		}
		for (size_t index = 0; index < length; index++)
			text[index] = page == 1200 ? (WCHAR)(data[index * 2] | data[index * 2 + 1] << 8) : (WCHAR)(data[index * 2] << 8 | data[index * 2 + 1]);
	} else if (page == 28591 || page == 20127) {
		/* These two map byte for byte and need no table from the system. */
		length = data_size;
		text = memory_get(reader->allocator, (length + 1) * sizeof(WCHAR));
		if (!text) {
			result = E_OUTOFMEMORY;
			goto done;
		}
		for (size_t index = 0; index < length; index++)
			text[index] = data[index];
	} else {
		int needed = data_size ? MultiByteToWideChar(page, 0, (const char *)data, (int)data_size, NULL, 0) : 0;
		if (data_size && !needed) {
			result = MX_E_ENCODING;
			goto done;
		}
		length = (size_t)needed;
		text = memory_get(reader->allocator, (length + 1) * sizeof(WCHAR));
		if (!text) {
			result = E_OUTOFMEMORY;
			goto done;
		}
		if (needed)
			MultiByteToWideChar(page, 0, (const char *)data, (int)data_size, text, needed);
	}
	/* Line ends turn to a single line feed before anything looks at them (XML 1.0, 2.11). */
	size_t kept = 0;
	for (size_t index = 0; index < length; index++) {
		if (text[index] == '\r') {
			text[kept++] = '\n';
			if (index + 1 < length && text[index + 1] == '\n')
				index++;
		} else {
			text[kept++] = text[index];
		}
	}
	text[kept] = 0;
	reader->document = text;
	reader->cursor = text;
	reader->end = text + kept;
	reader->line_start = text;
	reader->line = 1;
done:
	HeapFree(GetProcessHeap(), 0, bytes);
	return result;
}

static void forget_document(struct reader *reader)
{
	drop_bindings(reader, 0);
	while (reader->element_count)
		memory_put(reader->allocator, reader->elements[--reader->element_count].qualified);
	memory_put(reader->allocator, reader->document);
	reader->document = NULL;
	reader->cursor = reader->end = reader->line_start = NULL;
	arena_clear(reader);
	reader->attribute_count = 0;
	reader->current_attribute = -1;
	reader->depth = 0;
	reader->failure = S_OK;
	reader->is_empty = reader->pending_pop = reader->root_seen = reader->root_done = reader->started = FALSE;
	set_blank_node(reader, XmlNodeType_None);
}

static HRESULT read_node(struct reader *reader)
{
	if (reader->cursor >= reader->end) {
		if (reader->element_count || (!reader->root_seen && reader->conformance != XmlConformanceLevel_Fragment))
			return MX_E_INPUTEND;
		reader->state = XmlReadState_EndOfFile;
		set_blank_node(reader, XmlNodeType_None);
		return S_FALSE;
	}
	if (at_text(reader, L"<?xml") && reader->cursor + 5 < reader->end && is_space(reader->cursor[5]))
		return reader->cursor == reader->document ? read_declaration(reader) : WC_E_LEADINGXML;
	if (at_text(reader, L"<?"))
		return read_instruction(reader);
	if (at_text(reader, L"<!--"))
		return read_comment(reader);
	if (at_text(reader, L"<![CDATA[")) {
		if (!reader->element_count)
			return WC_E_SYNTAX;
		return read_cdata(reader);
	}
	if (at_text(reader, L"<!DOCTYPE"))
		return reader->root_seen ? WC_E_SYNTAX : read_doctype(reader);
	if (at_text(reader, L"</"))
		return read_end_element(reader);
	if (*reader->cursor == '<')
		return read_element(reader);
	return read_text(reader);
}

static HRESULT WINAPI reader_QueryInterface(IXmlReader *iface, REFIID riid, void **object)
{
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IXmlReader)) {
		*object = iface;
		IXmlReader_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI reader_AddRef(IXmlReader *iface)
{
	return InterlockedIncrement(&reader_of(iface)->references);
}

static void release_input(struct reader *reader)
{
	if (reader->input) {
		IUnknown_Release((IUnknown *)reader->input);
		reader->input = NULL;
	}
}

static ULONG WINAPI reader_Release(IXmlReader *iface)
{
	struct reader *reader = reader_of(iface);
	ULONG left = InterlockedDecrement(&reader->references);
	if (left)
		return left;
	IMalloc *allocator = reader->allocator;
	forget_document(reader);
	release_input(reader);
	if (reader->resolver)
		IUnknown_Release(reader->resolver);
	if (reader->languages)
		IUnknown_Release(reader->languages);
	memory_put(allocator, reader->attributes);
	memory_put(allocator, reader->elements);
	memory_put(allocator, reader->bindings);
	memory_put(allocator, reader);
	if (allocator)
		IMalloc_Release(allocator);
	return 0;
}

static ULONG WINAPI input_AddRef(IUnknown *iface)
{
	return InterlockedIncrement(&((struct reader_input *)iface)->references);
}

static HRESULT WINAPI input_QueryInterface(IUnknown *iface, REFIID riid, void **object)
{
	if (IsEqualGUID(riid, &IID_IUnknown)) {
		*object = iface;
		input_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI input_Release(IUnknown *iface)
{
	struct reader_input *input = (struct reader_input *)iface;
	ULONG left = InterlockedDecrement(&input->references);
	if (left)
		return left;
	IMalloc *allocator = input->allocator;
	if (input->stream)
		IUnknown_Release(input->stream);
	memory_put(allocator, input->base_uri);
	memory_put(allocator, input);
	if (allocator)
		IMalloc_Release(allocator);
	return 0;
}

static IUnknownVtbl input_vtable = { input_QueryInterface, input_AddRef, input_Release };

static struct reader_input *new_input(IMalloc *allocator, IUnknown *stream, UINT code_page, BOOL hint_only, const WCHAR *base_uri)
{
	struct reader_input *input = memory_get(allocator, sizeof(*input));
	if (!input)
		return NULL;
	input->vtable = &input_vtable;
	input->references = 1;
	input->allocator = allocator;
	if (allocator)
		IMalloc_AddRef(allocator);
	input->stream = stream;
	IUnknown_AddRef(stream);
	input->code_page = code_page;
	input->hint_only = hint_only;
	input->base_uri = base_uri ? copy_text(allocator, base_uri, (UINT)lstrlenW(base_uri)) : NULL;
	return input;
}

static HRESULT WINAPI reader_SetInput(IXmlReader *iface, IUnknown *source)
{
	struct reader *reader = reader_of(iface);
	forget_document(reader);
	release_input(reader);
	reader->state = XmlReadState_Closed;
	if (!source)
		return S_OK;
	if (*(IUnknownVtbl **)source == &input_vtable) {
		reader->input = (struct reader_input *)source;
		IUnknown_AddRef(source);
	} else {
		ISequentialStream *stream = NULL;
		if (FAILED(IUnknown_QueryInterface(source, &IID_ISequentialStream, (void **)&stream)))
			return E_NOINTERFACE;
		ISequentialStream_Release(stream);
		if (!(reader->input = new_input(reader->allocator, source, 0, TRUE, NULL)))
			return E_OUTOFMEMORY;
	}
	reader->state = XmlReadState_Initial;
	return S_OK;
}

static HRESULT WINAPI reader_GetProperty(IXmlReader *iface, UINT property, LONG_PTR *value)
{
	struct reader *reader = reader_of(iface);
	switch (property) {
	case XmlReaderProperty_MultiLanguage:
		*value = (LONG_PTR)reader->languages;
		if (reader->languages)
			IUnknown_AddRef(reader->languages);
		return S_OK;
	case XmlReaderProperty_ConformanceLevel:
		*value = reader->conformance;
		return S_OK;
	case XmlReaderProperty_RandomAccess:
		*value = reader->random_access;
		return S_OK;
	case XmlReaderProperty_XmlResolver:
		*value = (LONG_PTR)reader->resolver;
		if (reader->resolver)
			IUnknown_AddRef(reader->resolver);
		return S_OK;
	case XmlReaderProperty_DtdProcessing:
		*value = reader->dtd;
		return S_OK;
	case XmlReaderProperty_ReadState:
		*value = reader->state;
		return S_OK;
	case XmlReaderProperty_MaxElementDepth:
		*value = reader->max_depth;
		return S_OK;
	case XmlReaderProperty_MaxEntityExpansion:
		*value = reader->max_expansion;
		return S_OK;
	default:
		return E_INVALIDARG;
	}
}

static void swap_interface(IUnknown **slot, IUnknown *value)
{
	if (value)
		IUnknown_AddRef(value);
	if (*slot)
		IUnknown_Release(*slot);
	*slot = value;
}

static HRESULT WINAPI reader_SetProperty(IXmlReader *iface, UINT property, LONG_PTR value)
{
	struct reader *reader = reader_of(iface);
	switch (property) {
	case XmlReaderProperty_MultiLanguage:
		swap_interface(&reader->languages, (IUnknown *)value);
		return S_OK;
	case XmlReaderProperty_ConformanceLevel:
		if (value < XmlConformanceLevel_Auto || value > XmlConformanceLevel_Document)
			return E_INVALIDARG;
		reader->conformance = (LONG)value;
		return S_OK;
	case XmlReaderProperty_RandomAccess:
		reader->random_access = (LONG)value;
		return S_OK;
	case XmlReaderProperty_XmlResolver:
		swap_interface(&reader->resolver, (IUnknown *)value);
		return S_OK;
	case XmlReaderProperty_DtdProcessing:
		if (value < DtdProcessing_Prohibit || value > DtdProcessing_Parse)
			return E_INVALIDARG;
		reader->dtd = (LONG)value;
		return S_OK;
	case XmlReaderProperty_MaxElementDepth:
		reader->max_depth = (LONG)value;
		return S_OK;
	case XmlReaderProperty_MaxEntityExpansion:
		reader->max_expansion = (LONG)value;
		return S_OK;
	case XmlReaderProperty_ReadState:
		return E_INVALIDARG;
	default:
		return E_INVALIDARG;
	}
}

static HRESULT WINAPI reader_Read(IXmlReader *iface, XmlNodeType *type)
{
	struct reader *reader = reader_of(iface);
	if (type)
		*type = XmlNodeType_None;
	if (reader->failure)
		return reader->failure;
	if (reader->state == XmlReadState_Closed || !reader->input)
		return S_FALSE;
	if (reader->state == XmlReadState_EndOfFile)
		return S_FALSE;
	if (!reader->started) {
		HRESULT failure = load_document(reader);
		if (FAILED(failure))
			return fail_with(reader, failure);
		reader->started = TRUE;
		reader->state = XmlReadState_Interactive;
	}
	arena_reset(reader);
	reader->attribute_count = 0;
	reader->current_attribute = -1;
	reader->is_empty = FALSE;
	if (reader->pending_pop) {
		struct open_element *open = &reader->elements[--reader->element_count];
		drop_bindings(reader, open->binding_mark);
		memory_put(reader->allocator, open->qualified);
		reader->pending_pop = FALSE;
	}
	HRESULT result = read_node(reader);
	if (FAILED(result))
		return fail_with(reader, result);
	if (result == S_OK) {
		/* An end tag still counts as inside its element; only the start tag sits at the level of what surrounds it. */
		reader->depth = reader->element_count - (reader->type == XmlNodeType_Element ? 1 : 0);
		if (type)
			*type = reader->type;
	}
	return result;
}

static HRESULT WINAPI reader_GetNodeType(IXmlReader *iface, XmlNodeType *type)
{
	struct reader *reader = reader_of(iface);
	if (reader->failure) {
		*type = XmlNodeType_None;
		return S_FALSE;
	}
	*type = reader->current_attribute >= 0 ? XmlNodeType_Attribute : reader->type;
	return *type == XmlNodeType_None ? S_FALSE : S_OK;
}

static HRESULT WINAPI reader_MoveToFirstAttribute(IXmlReader *iface)
{
	struct reader *reader = reader_of(iface);
	if (!reader->attribute_count)
		return S_FALSE;
	reader->current_attribute = 0;
	reader->value_read = 0;
	return S_OK;
}

static HRESULT WINAPI reader_MoveToNextAttribute(IXmlReader *iface)
{
	struct reader *reader = reader_of(iface);
	if (!reader->attribute_count)
		return S_FALSE;
	if ((UINT)(reader->current_attribute + 1) >= reader->attribute_count)
		return S_FALSE;
	reader->current_attribute++;
	reader->value_read = 0;
	return S_OK;
}

static HRESULT WINAPI reader_MoveToAttributeByName(IXmlReader *iface, const WCHAR *local, const WCHAR *uri)
{
	struct reader *reader = reader_of(iface);
	if (!local)
		return E_INVALIDARG;
	if (!uri)
		uri = L"";
	for (UINT index = 0; index < reader->attribute_count; index++) {
		const struct attribute *attribute = &reader->attributes[index];
		if (!lstrcmpW(attribute->local, local) && !lstrcmpW(attribute->uri, uri)) {
			reader->current_attribute = (int)index;
			reader->value_read = 0;
			return S_OK;
		}
	}
	return S_FALSE;
}

static HRESULT WINAPI reader_MoveToElement(IXmlReader *iface)
{
	struct reader *reader = reader_of(iface);
	if (reader->current_attribute < 0)
		return S_FALSE;
	reader->current_attribute = -1;
	reader->value_read = 0;
	return S_OK;
}

static HRESULT give_text(const WCHAR *text, UINT length, const WCHAR **out, UINT *out_length)
{
	if (!out)
		return E_INVALIDARG;
	*out = text;
	if (out_length)
		*out_length = length;
	return S_OK;
}

static HRESULT WINAPI reader_GetQualifiedName(IXmlReader *iface, const WCHAR **name, UINT *length)
{
	struct reader *reader = reader_of(iface);
	if (reader->current_attribute >= 0) {
		const struct attribute *attribute = &reader->attributes[reader->current_attribute];
		return give_text(attribute->qualified, attribute->qualified_length, name, length);
	}
	return give_text(reader->qualified, reader->qualified_length, name, length);
}

static HRESULT WINAPI reader_GetNamespaceUri(IXmlReader *iface, const WCHAR **uri, UINT *length)
{
	struct reader *reader = reader_of(iface);
	if (reader->current_attribute >= 0) {
		const struct attribute *attribute = &reader->attributes[reader->current_attribute];
		return give_text(attribute->uri, attribute->uri_length, uri, length);
	}
	return give_text(reader->uri, reader->uri_length, uri, length);
}

static HRESULT WINAPI reader_GetLocalName(IXmlReader *iface, const WCHAR **name, UINT *length)
{
	struct reader *reader = reader_of(iface);
	if (reader->current_attribute >= 0) {
		const struct attribute *attribute = &reader->attributes[reader->current_attribute];
		return give_text(attribute->local, attribute->local_length, name, length);
	}
	return give_text(reader->local, reader->local_length, name, length);
}

static HRESULT WINAPI reader_GetPrefix(IXmlReader *iface, const WCHAR **prefix, UINT *length)
{
	struct reader *reader = reader_of(iface);
	if (reader->current_attribute >= 0) {
		const struct attribute *attribute = &reader->attributes[reader->current_attribute];
		return give_text(attribute->prefix, attribute->prefix_length, prefix, length);
	}
	return give_text(reader->prefix, reader->prefix_length, prefix, length);
}

static HRESULT WINAPI reader_GetValue(IXmlReader *iface, const WCHAR **value, UINT *length)
{
	struct reader *reader = reader_of(iface);
	if (value)
		*value = NULL;
	if (length)
		*length = 0;
	if (reader->failure)
		return reader->failure;
	if (reader->current_attribute >= 0) {
		const struct attribute *attribute = &reader->attributes[reader->current_attribute];
		return give_text(attribute->value, attribute->value_length, value, length);
	}
	return give_text(reader->value + reader->value_read, reader->value_length - reader->value_read, value, length);
}

static HRESULT WINAPI reader_ReadValueChunk(IXmlReader *iface, WCHAR *buffer, UINT wanted, UINT *read)
{
	struct reader *reader = reader_of(iface);
	const WCHAR *text = reader->value;
	UINT length = reader->value_length;
	if (reader->current_attribute >= 0) {
		text = reader->attributes[reader->current_attribute].value;
		length = reader->attributes[reader->current_attribute].value_length;
	}
	UINT left = length - reader->value_read;
	UINT run = wanted < left ? wanted : left;
	if (read)
		*read = run;
	if (!run)
		return S_FALSE;
	CopyMemory(buffer, text + reader->value_read, run * sizeof(WCHAR));
	reader->value_read += run;
	return S_OK;
}

static HRESULT WINAPI reader_GetBaseUri(IXmlReader *iface, const WCHAR **uri, UINT *length)
{
	struct reader *reader = reader_of(iface);
	const WCHAR *base = reader->input && reader->input->base_uri ? reader->input->base_uri : L"";
	if (!uri)
		return E_INVALIDARG;
	*uri = base;
	if (length)
		*length = (UINT)lstrlenW(base);
	return S_OK;
}

static BOOL WINAPI reader_IsDefault(IXmlReader *iface)
{
	(void)iface;
	return FALSE;
}

static BOOL WINAPI reader_IsEmptyElement(IXmlReader *iface)
{
	return reader_of(iface)->is_empty;
}

static HRESULT WINAPI reader_GetLineNumber(IXmlReader *iface, UINT *line)
{
	struct reader *reader = reader_of(iface);
	if (!line)
		return E_INVALIDARG;
	*line = reader->current_attribute >= 0 ? reader->attributes[reader->current_attribute].line : reader->node_line;
	return S_OK;
}

static HRESULT WINAPI reader_GetLinePosition(IXmlReader *iface, UINT *position)
{
	struct reader *reader = reader_of(iface);
	if (!position)
		return E_INVALIDARG;
	*position = reader->current_attribute >= 0 ? reader->attributes[reader->current_attribute].position : reader->node_position;
	return S_OK;
}

static HRESULT WINAPI reader_GetAttributeCount(IXmlReader *iface, UINT *count)
{
	if (!count)
		return E_INVALIDARG;
	*count = reader_of(iface)->attribute_count;
	return S_OK;
}

static HRESULT WINAPI reader_GetDepth(IXmlReader *iface, UINT *depth)
{
	struct reader *reader = reader_of(iface);
	if (!depth)
		return E_INVALIDARG;
	*depth = reader->depth + (reader->current_attribute >= 0 ? 1 : 0);
	return S_OK;
}

static BOOL WINAPI reader_IsEOF(IXmlReader *iface)
{
	return reader_of(iface)->state == XmlReadState_EndOfFile;
}

static const IXmlReaderVtbl reader_vtable = {
	reader_QueryInterface, reader_AddRef, reader_Release, reader_SetInput, reader_GetProperty, reader_SetProperty, reader_Read, reader_GetNodeType,
	reader_MoveToFirstAttribute, reader_MoveToNextAttribute, reader_MoveToAttributeByName, reader_MoveToElement, reader_GetQualifiedName,
	reader_GetNamespaceUri, reader_GetLocalName, reader_GetPrefix, reader_GetValue, reader_ReadValueChunk, reader_GetBaseUri, reader_IsDefault,
	reader_IsEmptyElement, reader_GetLineNumber, reader_GetLinePosition, reader_GetAttributeCount, reader_GetDepth, reader_IsEOF,
};

HRESULT WINAPI CreateXmlReader(REFIID riid, void **object, IMalloc *allocator)
{
	if (!IsEqualGUID(riid, &IID_IXmlReader)) {
		*object = NULL;
		return E_FAIL;
	}
	struct reader *reader = memory_get(allocator, sizeof(*reader));
	if (!reader)
		return E_OUTOFMEMORY;
	ZeroMemory(reader, sizeof(*reader));
	reader->iface.lpVtbl = &reader_vtable;
	reader->references = 1;
	reader->allocator = allocator;
	if (allocator)
		IMalloc_AddRef(allocator);
	reader->state = XmlReadState_Closed;
	reader->conformance = XmlConformanceLevel_Document;
	reader->dtd = DtdProcessing_Prohibit;
	reader->current_attribute = -1;
	set_blank_node(reader, XmlNodeType_None);
	*object = &reader->iface;
	return S_OK;
}

HRESULT WINAPI CreateXmlReaderInputWithEncodingCodePage(IUnknown *stream, IMalloc *allocator, UINT code_page, BOOL hint, const WCHAR *base_uri, IUnknown **input)
{
	if (!stream || !input)
		return E_INVALIDARG;
	*input = (IUnknown *)new_input(allocator, stream, code_page, hint, base_uri);
	return *input ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI CreateXmlReaderInputWithEncodingName(IUnknown *stream, IMalloc *allocator, const WCHAR *encoding, BOOL hint, const WCHAR *base_uri, IUnknown **input)
{
	if (!stream || !input)
		return E_INVALIDARG;
	UINT page = encoding ? code_page_of_name(encoding) : 0;
	if (encoding && !page)
		return E_INVALIDARG;
	*input = (IUnknown *)new_input(allocator, stream, page, hint, base_uri);
	return *input ? S_OK : E_OUTOFMEMORY;
}

/* The two widely used Unicode encodings plus the single-byte pages XML files name in practice. */
static const struct { const WCHAR *name; UINT code_page; } encodings[] = {
	{ L"utf-8", CP_UTF8 }, { L"utf-16", 1200 }, { L"utf-16le", 1200 }, { L"unicode", 1200 }, { L"utf-16be", 1201 },
	{ L"us-ascii", 20127 }, { L"ascii", 20127 }, { L"iso-8859-1", 28591 }, { L"iso-8859-2", 28592 }, { L"iso-8859-15", 28605 },
	{ L"windows-1250", 1250 }, { L"windows-1251", 1251 }, { L"windows-1252", 1252 }, { L"windows-1253", 1253 }, { L"windows-1254", 1254 },
	{ L"windows-1255", 1255 }, { L"windows-1256", 1256 }, { L"windows-1257", 1257 }, { L"windows-1258", 1258 },
};

UINT code_page_of_name(const WCHAR *name)
{
	for (size_t index = 0; index < ARRAY_SIZE(encodings); index++) {
		if (!lstrcmpiW(encodings[index].name, name))
			return encodings[index].code_page;
	}
	return 0;
}

const WCHAR *name_of_code_page(UINT code_page)
{
	for (size_t index = 0; index < ARRAY_SIZE(encodings); index++) {
		if (encodings[index].code_page == code_page)
			return encodings[index].name;
	}
	return NULL;
}

WCHAR *copy_text(IMalloc *allocator, const WCHAR *text, UINT length)
{
	WCHAR *copy = memory_get(allocator, ((size_t)length + 1) * sizeof(WCHAR));
	if (!copy)
		return NULL;
	CopyMemory(copy, text, length * sizeof(WCHAR));
	copy[length] = 0;
	return copy;
}
