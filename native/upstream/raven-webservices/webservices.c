/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven webservices.dll: the XML side of the Windows Web Services API, as programs use it to read documents into their own structures.
 *
 * A reader takes a whole text document from a buffer and splits it into a list of nodes at once: start elements with their resolved
 * namespaces and attributes, decoded text, and end elements. The reading functions move a cursor over that list. WsReadType fills
 * values from the type descriptions a program passes, allocating what they point to from a WS_HEAP. Element fields are matched by
 * name wherever they stand among the children of their element.
 *
 * Writers exist so that programs can create and free them; they write nothing yet. Channels, messages and services are not here:
 * creating one fails with E_NOTIMPL, as a service that cannot be reached, and the operations on them refuse what they are given.
 *
 * This is Raven code on the public Windows headers and the documentation of the API. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <webservices.h>

#define EXPORT __declspec(dllexport)

/* The header keeps the error codes of the API behind a condition; their values are public. */
#ifndef WS_E_INVALID_FORMAT
#define WS_E_INVALID_FORMAT ((HRESULT)0x803d0000)
#define WS_E_QUOTA_EXCEEDED ((HRESULT)0x803d0008)
#endif

/* Heaps: blocks of the process heap on a list, freed together. The header keeps what follows it aligned for any field. */
struct block {
	struct block *next;
	SIZE_T size;
};

struct _WS_HEAP {
	SIZE_T limit, used;
	struct block *blocks;
};

static void *heap_take(WS_HEAP *heap, SIZE_T size)
{
	if (size > heap->limit - heap->used)
		return NULL;
	SIZE_T header = (sizeof(struct block) + 15) & ~(SIZE_T)15;
	struct block *block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, header + (size ? size : 1));
	if (!block)
		return NULL;
	block->next = heap->blocks;
	block->size = size;
	heap->blocks = block;
	heap->used += size;
	return (BYTE *)block + header;
}

static void heap_release(WS_HEAP *heap)
{
	while (heap->blocks) {
		struct block *next = heap->blocks->next;
		HeapFree(GetProcessHeap(), 0, heap->blocks);
		heap->blocks = next;
	}
	heap->used = 0;
}

EXPORT HRESULT WINAPI WsCreateHeap(SIZE_T limit, SIZE_T trim, const WS_HEAP_PROPERTY *properties, ULONG count, WS_HEAP **out, WS_ERROR *error)
{
	if (!out)
		return E_INVALIDARG;
	WS_HEAP *heap = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*heap));
	if (!heap)
		return E_OUTOFMEMORY;
	heap->limit = limit;
	*out = heap;
	return S_OK;
}

EXPORT HRESULT WINAPI WsAlloc(WS_HEAP *heap, SIZE_T size, void **out, WS_ERROR *error)
{
	if (!heap || !out)
		return E_INVALIDARG;
	*out = heap_take(heap, size);
	return *out ? S_OK : WS_E_QUOTA_EXCEEDED;
}

EXPORT HRESULT WINAPI WsResetHeap(WS_HEAP *heap, WS_ERROR *error)
{
	if (!heap)
		return E_INVALIDARG;
	heap_release(heap);
	return S_OK;
}

EXPORT void WINAPI WsFreeHeap(WS_HEAP *heap)
{
	if (!heap)
		return;
	heap_release(heap);
	HeapFree(GetProcessHeap(), 0, heap);
}

/* Error objects: the strings that describe a failure, most recent first, and the code that started it. */
#define ERROR_STRINGS 16

struct _WS_ERROR {
	WS_STRING strings[ERROR_STRINGS];
	ULONG count;
	HRESULT original;
};

EXPORT HRESULT WINAPI WsCreateError(const WS_ERROR_PROPERTY *properties, ULONG count, WS_ERROR **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(**out));
	return *out ? S_OK : E_OUTOFMEMORY;
}

EXPORT HRESULT WINAPI WsResetError(WS_ERROR *error)
{
	if (!error)
		return E_INVALIDARG;
	for (ULONG index = 0; index < error->count; ++index)
		HeapFree(GetProcessHeap(), 0, error->strings[index].chars);
	error->count = 0;
	error->original = S_OK;
	return S_OK;
}

EXPORT void WINAPI WsFreeError(WS_ERROR *error)
{
	if (!error)
		return;
	WsResetError(error);
	HeapFree(GetProcessHeap(), 0, error);
}

EXPORT HRESULT WINAPI WsAddErrorString(WS_ERROR *error, const WS_STRING *string)
{
	if (!error || !string)
		return E_INVALIDARG;
	if (error->count == ERROR_STRINGS)
		return S_OK;
	WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, (string->length + 1) * sizeof(WCHAR));
	if (!copy)
		return E_OUTOFMEMORY;
	for (ULONG index = 0; index < string->length; ++index)
		copy[index] = string->chars[index];
	copy[string->length] = 0;
	for (ULONG index = error->count; index; --index)
		error->strings[index] = error->strings[index - 1];
	error->strings[0].length = string->length;
	error->strings[0].chars = copy;
	++error->count;
	return S_OK;
}

EXPORT HRESULT WINAPI WsGetErrorProperty(WS_ERROR *error, WS_ERROR_PROPERTY_ID id, void *buffer, ULONG size)
{
	if (!error || !buffer)
		return E_INVALIDARG;
	if (id == WS_ERROR_PROPERTY_STRING_COUNT && size == sizeof(ULONG))
		*(ULONG *)buffer = error->count;
	else if (id == WS_ERROR_PROPERTY_ORIGINAL_ERROR_CODE && size == sizeof(HRESULT))
		*(HRESULT *)buffer = error->original;
	else if (id == WS_ERROR_PROPERTY_LANGID && size == sizeof(LANGID))
		*(LANGID *)buffer = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
	else
		return E_INVALIDARG;
	return S_OK;
}

EXPORT HRESULT WINAPI WsGetErrorString(WS_ERROR *error, ULONG index, WS_STRING *string)
{
	if (!error || !string || index >= error->count)
		return E_INVALIDARG;
	*string = error->strings[index];
	return S_OK;
}

/* Records why a call failed when the program passed an error object. */
static HRESULT failed(WS_ERROR *error, HRESULT code, const WCHAR *text)
{
	if (error) {
		WS_STRING string = {0, (WCHAR *)text};
		while (text[string.length])
			++string.length;
		WsAddErrorString(error, &string);
		if (error->original == S_OK)
			error->original = code;
	}
	return code;
}

/* Readers. */
enum { NODE_START = 1, NODE_END, NODE_TEXT, NODE_EOF };

struct text {
	const char *bytes;
	ULONG length;
};

struct attribute {
	struct text prefix, local, ns, value;
};

struct node {
	int kind;
	/* A start element's end element and the start element every node stands in. */
	ULONG end, parent;
	struct text prefix, local, ns, value;
	ULONG first_attribute, attribute_count;
};

struct binding {
	struct text prefix, uri;
};

struct _WS_XML_READER {
	char *data, *decoded;
	ULONG size, used;
	struct node *nodes;
	ULONG count, cursor;
	struct attribute *attributes;
	ULONG attribute_count;
	struct binding *bindings;
	ULONG binding_count;
};

static const char xml_namespace[] = "http://www.w3.org/XML/1998/namespace";
static const char instance_namespace[] = "http://www.w3.org/2001/XMLSchema-instance";

static void reader_clear(WS_XML_READER *reader)
{
	HANDLE heap = GetProcessHeap();
	HeapFree(heap, 0, reader->data);
	HeapFree(heap, 0, reader->decoded);
	HeapFree(heap, 0, reader->nodes);
	HeapFree(heap, 0, reader->attributes);
	HeapFree(heap, 0, reader->bindings);
	reader->data = reader->decoded = NULL;
	reader->nodes = NULL;
	reader->attributes = NULL;
	reader->bindings = NULL;
	reader->size = reader->used = reader->count = reader->cursor = reader->attribute_count = reader->binding_count = 0;
}

EXPORT HRESULT WINAPI WsCreateReader(const WS_XML_READER_PROPERTY *properties, ULONG count, WS_XML_READER **out, WS_ERROR *error)
{
	if (!out)
		return E_INVALIDARG;
	*out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(**out));
	return *out ? S_OK : E_OUTOFMEMORY;
}

EXPORT void WINAPI WsFreeReader(WS_XML_READER *reader)
{
	if (!reader)
		return;
	reader_clear(reader);
	HeapFree(GetProcessHeap(), 0, reader);
}

static int same_text(struct text text, const char *bytes, ULONG length)
{
	if (text.length != length)
		return 0;
	for (ULONG index = 0; index < length; ++index)
		if (text.bytes[index] != bytes[index])
			return 0;
	return 1;
}

static int is_space(char character)
{
	return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

static int is_name(char character)
{
	return !is_space(character) && character != '>' && character != '/' && character != '=' && character != '<' && character && character != '"' &&
		character != '\'';
}

/* Appends a code point as UTF-8 to the decoded area. */
static void put_utf8(WS_XML_READER *reader, ULONG point)
{
	char *out = reader->decoded + reader->used;
	if (point < 0x80) {
		out[0] = (char)point;
		reader->used += 1;
	} else if (point < 0x800) {
		out[0] = (char)(0xc0 | point >> 6);
		out[1] = (char)(0x80 | (point & 0x3f));
		reader->used += 2;
	} else if (point < 0x10000) {
		out[0] = (char)(0xe0 | point >> 12);
		out[1] = (char)(0x80 | (point >> 6 & 0x3f));
		out[2] = (char)(0x80 | (point & 0x3f));
		reader->used += 3;
	} else {
		out[0] = (char)(0xf0 | point >> 18);
		out[1] = (char)(0x80 | (point >> 12 & 0x3f));
		out[2] = (char)(0x80 | (point >> 6 & 0x3f));
		out[3] = (char)(0x80 | (point & 0x3f));
		reader->used += 4;
	}
}

/* Text with its references replaced, kept in the decoded area; a reference is never shorter than what it stands for. */
static BOOL decode(WS_XML_READER *reader, const char *from, ULONG length, struct text *out)
{
	out->bytes = reader->decoded + reader->used;
	for (ULONG at = 0; at < length;) {
		if (from[at] != '&') {
			reader->decoded[reader->used++] = from[at++];
			continue;
		}
		ULONG end = at + 1;
		while (end < length && from[end] != ';')
			++end;
		if (end == length)
			return FALSE;
		const char *name = from + at + 1;
		const ULONG size = end - at - 1;
		struct text reference = {name, size};
		if (same_text(reference, "lt", 2))
			put_utf8(reader, '<');
		else if (same_text(reference, "gt", 2))
			put_utf8(reader, '>');
		else if (same_text(reference, "amp", 3))
			put_utf8(reader, '&');
		else if (same_text(reference, "quot", 4))
			put_utf8(reader, '"');
		else if (same_text(reference, "apos", 4))
			put_utf8(reader, '\'');
		else if (size > 1 && name[0] == '#') {
			ULONG point = 0, base = name[1] == 'x' ? 16 : 10;
			for (ULONG index = base == 16 ? 2 : 1; index < size; ++index) {
				const char digit = name[index];
				const ULONG value = digit >= '0' && digit <= '9' ? (ULONG)(digit - '0') : digit >= 'a' && digit <= 'f' ? (ULONG)(digit - 'a' + 10) :
					digit >= 'A' && digit <= 'F' ? (ULONG)(digit - 'A' + 10) : 99;
				if (value >= base || point > 0x10ffff)
					return FALSE;
				point = point * base + value;
			}
			if (!point || point > 0x10ffff)
				return FALSE;
			put_utf8(reader, point);
		} else {
			return FALSE;
		}
		at = end + 1;
	}
	out->length = (ULONG)(reader->decoded + reader->used - out->bytes);
	return TRUE;
}

static BOOL resolve(WS_XML_READER *reader, struct text prefix, struct text *uri)
{
	if (same_text(prefix, "xml", 3)) {
		uri->bytes = xml_namespace;
		uri->length = sizeof(xml_namespace) - 1;
		return TRUE;
	}
	for (ULONG index = reader->binding_count; index; --index)
		if (same_text(reader->bindings[index - 1].prefix, prefix.bytes, prefix.length)) {
			*uri = reader->bindings[index - 1].uri;
			return TRUE;
		}
	uri->bytes = "";
	uri->length = 0;
	return !prefix.length;
}

static void split_name(const char *name, ULONG length, struct text *prefix, struct text *local)
{
	ULONG colon = 0;
	while (colon < length && name[colon] != ':')
		++colon;
	if (colon == length) {
		prefix->bytes = "";
		prefix->length = 0;
		local->bytes = name;
		local->length = length;
	} else {
		prefix->bytes = name;
		prefix->length = colon;
		local->bytes = name + colon + 1;
		local->length = length - colon - 1;
	}
}

static BOOL starts(const char *at, const char *end, const char *with)
{
	while (*with) {
		if (at == end || *at != *with)
			return FALSE;
		++at;
		++with;
	}
	return TRUE;
}

static const char *find(const char *at, const char *end, const char *what)
{
	for (; at < end; ++at)
		if (starts(at, end, what))
			return at;
	return NULL;
}

/* Splits the document into nodes, resolving every prefix with the declarations in scope. */
static HRESULT parse(WS_XML_READER *reader)
{
	const char *at = reader->data, *end = reader->data + reader->size;
	ULONG opens = 0, equals = 0;
	for (const char *scan = at; scan < end; ++scan) {
		opens += *scan == '<';
		equals += *scan == '=';
	}
	HANDLE heap = GetProcessHeap();
	/* Each '<' makes at most two nodes, an empty element's start and end, and text may stand before each. */
	reader->nodes = HeapAlloc(heap, HEAP_ZERO_MEMORY, (opens * 3 + 4) * sizeof(struct node));
	reader->attributes = HeapAlloc(heap, HEAP_ZERO_MEMORY, (equals + 1) * sizeof(struct attribute));
	reader->bindings = HeapAlloc(heap, HEAP_ZERO_MEMORY, (equals + 1) * sizeof(struct binding));
	reader->decoded = HeapAlloc(heap, 0, reader->size + 1);
	/* The open elements and how many bindings were in scope before each. */
	ULONG *stack = HeapAlloc(heap, 0, (opens + 1) * sizeof(ULONG) * 2);
	if (!reader->nodes || !reader->attributes || !reader->bindings || !reader->decoded || !stack) {
		HeapFree(heap, 0, stack);
		return E_OUTOFMEMORY;
	}
	ULONG depth = 0;
	const ULONG none = (ULONG)-1;
	if (end - at >= 3 && (BYTE)at[0] == 0xef && (BYTE)at[1] == 0xbb && (BYTE)at[2] == 0xbf)
		at += 3;
	HRESULT result = S_OK;
	while (at < end) {
		if (*at != '<') {
			const char *text_end = at;
			while (text_end < end && *text_end != '<')
				++text_end;
			struct node *node = &reader->nodes[reader->count];
			if (!depth) {
				for (const char *scan = at; scan < text_end; ++scan)
					if (!is_space(*scan)) {
						result = WS_E_INVALID_FORMAT;
						goto done;
					}
				at = text_end;
				continue;
			}
			node->kind = NODE_TEXT;
			node->parent = stack[(depth - 1) * 2];
			if (!decode(reader, at, (ULONG)(text_end - at), &node->value)) {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			++reader->count;
			at = text_end;
			continue;
		}
		if (starts(at, end, "<!--")) {
			const char *close = find(at + 4, end, "-->");
			if (!close) {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			at = close + 3;
			continue;
		}
		if (starts(at, end, "<?")) {
			const char *close = find(at + 2, end, "?>");
			if (!close) {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			at = close + 2;
			continue;
		}
		if (starts(at, end, "<![CDATA[")) {
			const char *close = find(at + 9, end, "]]>");
			if (!close || !depth) {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			struct node *node = &reader->nodes[reader->count++];
			node->kind = NODE_TEXT;
			node->parent = stack[(depth - 1) * 2];
			node->value.bytes = at + 9;
			node->value.length = (ULONG)(close - at - 9);
			at = close + 3;
			continue;
		}
		if (starts(at, end, "<!")) {
			/* A document type declaration, with an internal subset in brackets. */
			int brackets = 0;
			for (at += 2; at < end && (brackets || *at != '>'); ++at)
				brackets += *at == '[' ? 1 : *at == ']' ? -1 : 0;
			++at;
			continue;
		}
		if (starts(at, end, "</")) {
			const char *name = at + 2, *name_end = name;
			while (name_end < end && is_name(*name_end))
				++name_end;
			const char *close = name_end;
			while (close < end && is_space(*close))
				++close;
			if (!depth || close == end || *close != '>') {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			const ULONG start = stack[(depth - 1) * 2];
			struct node *opening = &reader->nodes[start];
			const ULONG prefix_length = opening->prefix.length ? opening->prefix.length + 1 : 0;
			if ((ULONG)(name_end - name) != prefix_length + opening->local.length ||
				(prefix_length && !same_text(opening->prefix, name, opening->prefix.length)) ||
				!same_text(opening->local, name + prefix_length, opening->local.length)) {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			struct node *node = &reader->nodes[reader->count];
			node->kind = NODE_END;
			node->parent = start;
			opening->end = reader->count++;
			reader->binding_count = stack[(depth - 1) * 2 + 1];
			--depth;
			at = close + 1;
			continue;
		}
		/* A start element: its name, then its attributes, some of which declare namespaces. */
		const char *name = at + 1, *name_end = name;
		while (name_end < end && is_name(*name_end))
			++name_end;
		if (name_end == name) {
			result = WS_E_INVALID_FORMAT;
			goto done;
		}
		const ULONG index = reader->count++;
		struct node *node = &reader->nodes[index];
		node->kind = NODE_START;
		node->parent = depth ? stack[(depth - 1) * 2] : none;
		node->end = none;
		split_name(name, (ULONG)(name_end - name), &node->prefix, &node->local);
		node->first_attribute = reader->attribute_count;
		stack[depth * 2] = index;
		stack[depth * 2 + 1] = reader->binding_count;
		at = name_end;
		BOOL empty = FALSE;
		for (;;) {
			while (at < end && is_space(*at))
				++at;
			if (at == end) {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			if (*at == '>') {
				++at;
				break;
			}
			if (starts(at, end, "/>")) {
				at += 2;
				empty = TRUE;
				break;
			}
			const char *attribute = at;
			while (at < end && is_name(*at))
				++at;
			const ULONG attribute_length = (ULONG)(at - attribute);
			while (at < end && is_space(*at))
				++at;
			if (!attribute_length || at == end || *at != '=') {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			++at;
			while (at < end && is_space(*at))
				++at;
			if (at == end || (*at != '"' && *at != '\'')) {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			const char quote = *at++;
			const char *value = at;
			while (at < end && *at != quote)
				++at;
			if (at == end) {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			struct attribute *item = &reader->attributes[reader->attribute_count];
			split_name(attribute, attribute_length, &item->prefix, &item->local);
			if (!decode(reader, value, (ULONG)(at - value), &item->value)) {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
			++at;
			if (same_text(item->prefix, "xmlns", 5) || (!item->prefix.length && same_text(item->local, "xmlns", 5))) {
				struct binding *binding = &reader->bindings[reader->binding_count++];
				if (item->prefix.length)
					binding->prefix = item->local;
				else
					binding->prefix.bytes = "", binding->prefix.length = 0;
				binding->uri = item->value;
				continue;
			}
			++reader->attribute_count;
		}
		node->attribute_count = reader->attribute_count - node->first_attribute;
		if (!resolve(reader, node->prefix, &node->ns)) {
			result = WS_E_INVALID_FORMAT;
			goto done;
		}
		for (ULONG attribute = node->first_attribute; attribute < reader->attribute_count; ++attribute) {
			struct attribute *item = &reader->attributes[attribute];
			/* An attribute without a prefix is in no namespace. */
			if (!item->prefix.length)
				item->ns.bytes = "", item->ns.length = 0;
			else if (!resolve(reader, item->prefix, &item->ns)) {
				result = WS_E_INVALID_FORMAT;
				goto done;
			}
		}
		if (empty) {
			struct node *closing = &reader->nodes[reader->count];
			closing->kind = NODE_END;
			closing->parent = index;
			node->end = reader->count++;
			reader->binding_count = stack[depth * 2 + 1];
		} else {
			++depth;
		}
	}
	if (depth)
		result = WS_E_INVALID_FORMAT;
	reader->nodes[reader->count].kind = NODE_EOF;
	reader->nodes[reader->count].parent = none;
done:
	HeapFree(heap, 0, stack);
	return result;
}

/* UTF-16 input becomes UTF-8, which is what the reader works on. */
static char *from_utf16(const BYTE *data, ULONG size, BOOL big_endian, ULONG *length)
{
	char *out = HeapAlloc(GetProcessHeap(), 0, size / 2 * 3 + 1);
	if (!out)
		return NULL;
	ULONG used = 0;
	for (ULONG at = 0; at + 1 < size; at += 2) {
		ULONG unit = big_endian ? (ULONG)data[at] << 8 | data[at + 1] : (ULONG)data[at + 1] << 8 | data[at];
		if (unit >= 0xd800 && unit < 0xdc00 && at + 3 < size) {
			const ULONG low = big_endian ? (ULONG)data[at + 2] << 8 | data[at + 3] : (ULONG)data[at + 3] << 8 | data[at + 2];
			if (low >= 0xdc00 && low < 0xe000) {
				unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
				at += 2;
			}
		}
		if (unit < 0x80) {
			out[used++] = (char)unit;
		} else if (unit < 0x800) {
			out[used++] = (char)(0xc0 | unit >> 6);
			out[used++] = (char)(0x80 | (unit & 0x3f));
		} else if (unit < 0x10000) {
			out[used++] = (char)(0xe0 | unit >> 12);
			out[used++] = (char)(0x80 | (unit >> 6 & 0x3f));
			out[used++] = (char)(0x80 | (unit & 0x3f));
		} else {
			out[used++] = (char)(0xf0 | unit >> 18);
			out[used++] = (char)(0x80 | (unit >> 12 & 0x3f));
			out[used++] = (char)(0x80 | (unit >> 6 & 0x3f));
			out[used++] = (char)(0x80 | (unit & 0x3f));
		}
	}
	*length = used;
	return out;
}

EXPORT HRESULT WINAPI WsSetInput(WS_XML_READER *reader, const WS_XML_READER_ENCODING *encoding, const WS_XML_READER_INPUT *input,
	const WS_XML_READER_PROPERTY *properties, ULONG count, WS_ERROR *error)
{
	if (!reader)
		return E_INVALIDARG;
	reader_clear(reader);
	WS_CHARSET charset = WS_CHARSET_AUTO;
	if (encoding) {
		if (encoding->encodingType != WS_XML_READER_ENCODING_TYPE_TEXT)
			return failed(error, E_NOTIMPL, L"Only text XML can be read.");
		charset = ((const WS_XML_READER_TEXT_ENCODING *)encoding)->charSet;
	}
	if (!input) {
		reader->data = HeapAlloc(GetProcessHeap(), 0, 1);
		return reader->data ? parse(reader) : E_OUTOFMEMORY;
	}
	if (input->inputType != WS_XML_READER_INPUT_TYPE_BUFFER)
		return failed(error, E_NOTIMPL, L"Only buffers can be read.");
	const WS_XML_READER_BUFFER_INPUT *buffer = (const WS_XML_READER_BUFFER_INPUT *)input;
	const BYTE *bytes = buffer->encodedData;
	ULONG size = buffer->encodedDataSize;
	if (charset == WS_CHARSET_AUTO && size >= 2)
		charset = (bytes[0] == 0xff && bytes[1] == 0xfe) || (bytes[0] == '<' && !bytes[1]) ? WS_CHARSET_UTF16LE :
			(bytes[0] == 0xfe && bytes[1] == 0xff) || (!bytes[0] && bytes[1] == '<') ? WS_CHARSET_UTF16BE : WS_CHARSET_UTF8;
	if (charset == WS_CHARSET_UTF16LE || charset == WS_CHARSET_UTF16BE) {
		const BOOL big = charset == WS_CHARSET_UTF16BE;
		if (size >= 2 && ((big && bytes[0] == 0xfe && bytes[1] == 0xff) || (!big && bytes[0] == 0xff && bytes[1] == 0xfe)))
			bytes += 2, size -= 2;
		reader->data = from_utf16(bytes, size, big, &reader->size);
	} else {
		reader->data = HeapAlloc(GetProcessHeap(), 0, size + 1);
		if (reader->data) {
			for (ULONG index = 0; index < size; ++index)
				reader->data[index] = (char)bytes[index];
			reader->size = size;
		}
	}
	if (!reader->data)
		return E_OUTOFMEMORY;
	const HRESULT result = parse(reader);
	if (FAILED(result)) {
		reader_clear(reader);
		return failed(error, result, L"The input data was not in the expected format.");
	}
	return S_OK;
}

EXPORT HRESULT WINAPI WsFillReader(WS_XML_READER *reader, ULONG minimum, const WS_ASYNC_CONTEXT *context, WS_ERROR *error)
{
	return reader ? S_OK : E_INVALIDARG;
}

static struct node *current(WS_XML_READER *reader)
{
	return &reader->nodes[reader->cursor];
}

static void skip_text(WS_XML_READER *reader)
{
	while (current(reader)->kind == NODE_TEXT)
		++reader->cursor;
}

static int name_matches(const struct node *node, const WS_XML_STRING *local, const WS_XML_STRING *ns)
{
	if (local && !same_text(node->local, (const char *)local->bytes, local->length))
		return 0;
	return !ns || same_text(node->ns, (const char *)ns->bytes, ns->length);
}

EXPORT HRESULT WINAPI WsReadToStartElement(WS_XML_READER *reader, const WS_XML_STRING *local, const WS_XML_STRING *ns, BOOL *found,
	WS_ERROR *error)
{
	if (!reader || !reader->nodes)
		return E_INVALIDARG;
	skip_text(reader);
	const BOOL match = current(reader)->kind == NODE_START && name_matches(current(reader), local, ns);
	if (found) {
		*found = match;
		return S_OK;
	}
	return match ? S_OK : failed(error, WS_E_INVALID_FORMAT, L"The expected element was not found.");
}

EXPORT HRESULT WINAPI WsReadStartElement(WS_XML_READER *reader, WS_ERROR *error)
{
	if (!reader || !reader->nodes)
		return E_INVALIDARG;
	skip_text(reader);
	if (current(reader)->kind != NODE_START)
		return failed(error, WS_E_INVALID_FORMAT, L"The reader is not at a start element.");
	++reader->cursor;
	return S_OK;
}

EXPORT HRESULT WINAPI WsReadEndElement(WS_XML_READER *reader, WS_ERROR *error)
{
	if (!reader || !reader->nodes)
		return E_INVALIDARG;
	skip_text(reader);
	if (current(reader)->kind != NODE_END)
		return failed(error, WS_E_INVALID_FORMAT, L"The reader is not at an end element.");
	++reader->cursor;
	return S_OK;
}

EXPORT HRESULT WINAPI WsSkipNode(WS_XML_READER *reader, WS_ERROR *error)
{
	if (!reader || !reader->nodes)
		return E_INVALIDARG;
	const struct node *node = current(reader);
	if (node->kind == NODE_START)
		reader->cursor = node->end + 1;
	else if (node->kind != NODE_EOF)
		++reader->cursor;
	return S_OK;
}

/* Values. */
static ULONG type_size(WS_TYPE type, const void *description)
{
	switch (type) {
	case WS_BOOL_TYPE: return sizeof(BOOL);
	case WS_INT8_TYPE: case WS_UINT8_TYPE: return 1;
	case WS_INT16_TYPE: case WS_UINT16_TYPE: return 2;
	case WS_INT32_TYPE: case WS_UINT32_TYPE: case WS_ENUM_TYPE: case WS_FLOAT_TYPE: return 4;
	case WS_INT64_TYPE: case WS_UINT64_TYPE: case WS_DOUBLE_TYPE: case WS_TIMESPAN_TYPE: return 8;
	case WS_DATETIME_TYPE: return sizeof(WS_DATETIME);
	case WS_GUID_TYPE: return sizeof(GUID);
	case WS_STRING_TYPE: return sizeof(WS_STRING);
	case WS_WSZ_TYPE: return sizeof(WCHAR *);
	case WS_BYTES_TYPE: return sizeof(WS_BYTES);
	case WS_XML_STRING_TYPE: return sizeof(WS_XML_STRING);
	case WS_STRUCT_TYPE: return description ? ((const WS_STRUCT_DESCRIPTION *)description)->size : 0;
	default: return 0;
	}
}

/* The text an element holds directly, its child elements left out; the whole range when it is one piece. */
static struct text content(WS_XML_READER *reader, ULONG from, ULONG to, WS_HEAP *heap)
{
	struct text result = {"", 0};
	ULONG pieces = 0, total = 0;
	for (ULONG index = from; index < to; ++index) {
		const struct node *node = &reader->nodes[index];
		if (node->kind == NODE_START)
			index = node->end;
		else if (node->kind == NODE_TEXT && node->value.length) {
			if (!pieces)
				result = node->value;
			++pieces;
			total += node->value.length;
		}
	}
	if (pieces < 2)
		return result;
	char *joined = heap_take(heap, total);
	if (!joined)
		return result;
	ULONG used = 0;
	for (ULONG index = from; index < to; ++index) {
		const struct node *node = &reader->nodes[index];
		if (node->kind == NODE_START)
			index = node->end;
		else if (node->kind == NODE_TEXT)
			for (ULONG at = 0; at < node->value.length; ++at)
				joined[used++] = node->value.bytes[at];
	}
	result.bytes = joined;
	result.length = used;
	return result;
}

static struct text trimmed(struct text text)
{
	while (text.length && is_space(text.bytes[0]))
		++text.bytes, --text.length;
	while (text.length && is_space(text.bytes[text.length - 1]))
		--text.length;
	return text;
}

static HRESULT parse_integer(struct text text, BOOL is_signed, LONGLONG minimum, ULONGLONG maximum, ULONGLONG *out)
{
	text = trimmed(text);
	BOOL negative = FALSE;
	ULONG at = 0;
	if (at < text.length && (text.bytes[at] == '-' || text.bytes[at] == '+')) {
		negative = text.bytes[at] == '-';
		++at;
	}
	if (at == text.length || (negative && !is_signed))
		return WS_E_INVALID_FORMAT;
	ULONGLONG value = 0;
	for (; at < text.length; ++at) {
		const char digit = text.bytes[at];
		if (digit < '0' || digit > '9' || value > (~0ULL - (ULONGLONG)(digit - '0')) / 10)
			return WS_E_INVALID_FORMAT;
		value = value * 10 + (ULONGLONG)(digit - '0');
	}
	if (negative) {
		if (value > (ULONGLONG)0 - (ULONGLONG)minimum)
			return WS_E_INVALID_FORMAT;
		*out = (ULONGLONG)(-(LONGLONG)value);
		return S_OK;
	}
	if (value > maximum)
		return WS_E_INVALID_FORMAT;
	*out = value;
	return S_OK;
}

static HRESULT parse_double(struct text text, double *out)
{
	text = trimmed(text);
	if (same_text(text, "INF", 3)) {
		*out = __builtin_inf();
		return S_OK;
	}
	if (same_text(text, "-INF", 4)) {
		*out = -__builtin_inf();
		return S_OK;
	}
	if (same_text(text, "NaN", 3)) {
		*out = __builtin_nan("");
		return S_OK;
	}
	ULONG at = 0;
	BOOL negative = FALSE, digits = FALSE;
	if (at < text.length && (text.bytes[at] == '-' || text.bytes[at] == '+'))
		negative = text.bytes[at++] == '-';
	double value = 0, scale = 1;
	while (at < text.length && text.bytes[at] >= '0' && text.bytes[at] <= '9')
		value = value * 10 + (text.bytes[at++] - '0'), digits = TRUE;
	if (at < text.length && text.bytes[at] == '.')
		for (++at; at < text.length && text.bytes[at] >= '0' && text.bytes[at] <= '9'; ++at)
			scale /= 10, value += (text.bytes[at] - '0') * scale, digits = TRUE;
	if (!digits)
		return WS_E_INVALID_FORMAT;
	if (at < text.length && (text.bytes[at] == 'e' || text.bytes[at] == 'E')) {
		ULONGLONG exponent;
		struct text rest = {text.bytes + at + 1, text.length - at - 1};
		if (FAILED(parse_integer(rest, TRUE, -400, 400, &exponent)))
			return WS_E_INVALID_FORMAT;
		for (LONGLONG count = (LONGLONG)exponent; count > 0; --count)
			value *= 10;
		for (LONGLONG count = (LONGLONG)exponent; count < 0; ++count)
			value /= 10;
		at = text.length;
	}
	if (at != text.length)
		return WS_E_INVALID_FORMAT;
	*out = negative ? -value : value;
	return S_OK;
}

static int hex_value(char digit)
{
	return digit >= '0' && digit <= '9' ? digit - '0' : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1;
}

static HRESULT parse_guid(struct text text, GUID *out)
{
	text = trimmed(text);
	if (text.length != 36)
		return WS_E_INVALID_FORMAT;
	BYTE bytes[16];
	ULONG count = 0;
	for (ULONG at = 0; at < 36;) {
		if (at == 8 || at == 13 || at == 18 || at == 23) {
			if (text.bytes[at++] != '-')
				return WS_E_INVALID_FORMAT;
			continue;
		}
		const int high = hex_value(text.bytes[at]), low = hex_value(text.bytes[at + 1]);
		if (high < 0 || low < 0)
			return WS_E_INVALID_FORMAT;
		bytes[count++] = (BYTE)(high << 4 | low);
		at += 2;
	}
	out->Data1 = (ULONG)bytes[0] << 24 | (ULONG)bytes[1] << 16 | (ULONG)bytes[2] << 8 | bytes[3];
	out->Data2 = (USHORT)(bytes[4] << 8 | bytes[5]);
	out->Data3 = (USHORT)(bytes[6] << 8 | bytes[7]);
	for (int index = 0; index < 8; ++index)
		out->Data4[index] = bytes[8 + index];
	return S_OK;
}

/* xs:dateTime as 100-nanosecond ticks from the year 1; a time with a zone is taken to UTC. */
static HRESULT parse_datetime(struct text text, WS_DATETIME *out)
{
	text = trimmed(text);
	const char *bytes = text.bytes;
	if (text.length < 19 || bytes[4] != '-' || bytes[7] != '-' || bytes[10] != 'T' || bytes[13] != ':' || bytes[16] != ':')
		return WS_E_INVALID_FORMAT;
	ULONG fields[6];
	const ULONG starts_at[6] = {0, 5, 8, 11, 14, 17}, lengths[6] = {4, 2, 2, 2, 2, 2};
	for (int index = 0; index < 6; ++index) {
		fields[index] = 0;
		for (ULONG at = 0; at < lengths[index]; ++at) {
			const char digit = bytes[starts_at[index] + at];
			if (digit < '0' || digit > '9')
				return WS_E_INVALID_FORMAT;
			fields[index] = fields[index] * 10 + (ULONG)(digit - '0');
		}
	}
	const ULONG year = fields[0], month = fields[1], day = fields[2];
	if (!year || month < 1 || month > 12 || day < 1 || day > 31 || fields[3] > 24 || fields[4] > 59 || fields[5] > 59)
		return WS_E_INVALID_FORMAT;
	ULONG at = 19;
	ULONGLONG fraction = 0;
	if (at < text.length && bytes[at] == '.') {
		ULONG digits = 0;
		for (++at; at < text.length && bytes[at] >= '0' && bytes[at] <= '9'; ++at)
			if (digits < 7)
				fraction = fraction * 10 + (ULONGLONG)(bytes[at] - '0'), ++digits;
		while (digits++ < 7)
			fraction *= 10;
	}
	static const ULONG before_month[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	const ULONG years = year - 1;
	const BOOL leap = (year % 4 == 0 && year % 100) || year % 400 == 0;
	ULONGLONG days = (ULONGLONG)years * 365 + years / 4 - years / 100 + years / 400 + before_month[month - 1] + (leap && month > 2) + day - 1;
	LONGLONG seconds = (LONGLONG)(days * 86400 + fields[3] * 3600 + fields[4] * 60 + fields[5]);
	out->format = WS_DATETIME_FORMAT_NONE;
	if (at < text.length && bytes[at] == 'Z') {
		out->format = WS_DATETIME_FORMAT_UTC;
		++at;
	} else if (at + 6 == text.length && (bytes[at] == '+' || bytes[at] == '-') && bytes[at + 3] == ':') {
		const int hours = hex_value(bytes[at + 1]) * 10 + hex_value(bytes[at + 2]), minutes = hex_value(bytes[at + 4]) * 10 + hex_value(bytes[at + 5]);
		const LONGLONG offset = (LONGLONG)(hours * 3600 + minutes * 60);
		seconds += bytes[at] == '+' ? -offset : offset;
		out->format = WS_DATETIME_FORMAT_UTC;
		at += 6;
	}
	if (at != text.length)
		return WS_E_INVALID_FORMAT;
	out->ticks = (ULONGLONG)seconds * 10000000 + fraction;
	return S_OK;
}

static HRESULT parse_base64(struct text text, WS_HEAP *heap, WS_BYTES *out)
{
	BYTE *bytes = heap_take(heap, text.length / 4 * 3 + 3);
	if (!bytes)
		return WS_E_QUOTA_EXCEEDED;
	ULONG value = 0, bits = 0, used = 0;
	for (ULONG at = 0; at < text.length; ++at) {
		const char character = text.bytes[at];
		int digit = character >= 'A' && character <= 'Z' ? character - 'A' : character >= 'a' && character <= 'z' ? character - 'a' + 26 :
			character >= '0' && character <= '9' ? character - '0' + 52 : character == '+' ? 62 : character == '/' ? 63 : -1;
		if (character == '=' || is_space(character))
			continue;
		if (digit < 0)
			return WS_E_INVALID_FORMAT;
		value = value << 6 | (ULONG)digit;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			bytes[used++] = (BYTE)(value >> bits);
		}
	}
	out->bytes = used ? bytes : NULL;
	out->length = used;
	return S_OK;
}

/* The UTF-16 form of UTF-8 text in heap memory, with a terminator. */
static WCHAR *wide_copy(struct text text, WS_HEAP *heap, ULONG *length)
{
	WCHAR *out = heap_take(heap, (text.length + 1) * sizeof(WCHAR));
	if (!out)
		return NULL;
	ULONG used = 0;
	for (ULONG at = 0; at < text.length;) {
		const BYTE lead = (BYTE)text.bytes[at];
		ULONG point = lead, extra = 0;
		if (lead >= 0xf0)
			point = lead & 0x07, extra = 3;
		else if (lead >= 0xe0)
			point = lead & 0x0f, extra = 2;
		else if (lead >= 0xc0)
			point = lead & 0x1f, extra = 1;
		++at;
		for (; extra && at < text.length; --extra, ++at)
			point = point << 6 | ((BYTE)text.bytes[at] & 0x3f);
		if (point >= 0x10000) {
			point -= 0x10000;
			out[used++] = (WCHAR)(0xd800 + (point >> 10));
			out[used++] = (WCHAR)(0xdc00 + (point & 0x3ff));
		} else {
			out[used++] = (WCHAR)point;
		}
	}
	out[used] = 0;
	*length = used;
	return out;
}

/* A value of a type that is text, from that text. */
static HRESULT parse_value(struct text text, WS_TYPE type, const void *description, WS_HEAP *heap, void *out)
{
	ULONGLONG number;
	HRESULT result;
	switch (type) {
	case WS_BOOL_TYPE: {
		const struct text value = trimmed(text);
		if (same_text(value, "true", 4) || same_text(value, "1", 1))
			*(BOOL *)out = TRUE;
		else if (same_text(value, "false", 5) || same_text(value, "0", 1))
			*(BOOL *)out = FALSE;
		else
			return WS_E_INVALID_FORMAT;
		return S_OK;
	}
	case WS_INT8_TYPE:
		if (SUCCEEDED(result = parse_integer(text, TRUE, -128, 127, &number)))
			*(char *)out = (char)number;
		return result;
	case WS_INT16_TYPE:
		if (SUCCEEDED(result = parse_integer(text, TRUE, -32768, 32767, &number)))
			*(short *)out = (short)number;
		return result;
	case WS_INT32_TYPE:
		if (SUCCEEDED(result = parse_integer(text, TRUE, -2147483647LL - 1, 2147483647, &number)))
			*(int *)out = (int)number;
		return result;
	case WS_INT64_TYPE:
		if (SUCCEEDED(result = parse_integer(text, TRUE, -9223372036854775807LL - 1, 9223372036854775807ULL, &number)))
			*(LONGLONG *)out = (LONGLONG)number;
		return result;
	case WS_UINT8_TYPE:
		if (SUCCEEDED(result = parse_integer(text, FALSE, 0, 255, &number)))
			*(BYTE *)out = (BYTE)number;
		return result;
	case WS_UINT16_TYPE:
		if (SUCCEEDED(result = parse_integer(text, FALSE, 0, 65535, &number)))
			*(USHORT *)out = (USHORT)number;
		return result;
	case WS_UINT32_TYPE:
		if (SUCCEEDED(result = parse_integer(text, FALSE, 0, 4294967295ULL, &number)))
			*(ULONG *)out = (ULONG)number;
		return result;
	case WS_UINT64_TYPE:
		if (SUCCEEDED(result = parse_integer(text, FALSE, 0, ~0ULL, &number)))
			*(ULONGLONG *)out = number;
		return result;
	case WS_FLOAT_TYPE: {
		double value;
		if (SUCCEEDED(result = parse_double(text, &value)))
			*(float *)out = (float)value;
		return result;
	}
	case WS_DOUBLE_TYPE:
		return parse_double(text, out);
	case WS_GUID_TYPE:
		return parse_guid(text, out);
	case WS_DATETIME_TYPE:
		return parse_datetime(text, out);
	case WS_STRING_TYPE: {
		WS_STRING *string = out;
		string->chars = wide_copy(text, heap, &string->length);
		return string->chars ? S_OK : WS_E_QUOTA_EXCEEDED;
	}
	case WS_WSZ_TYPE: {
		ULONG length;
		*(WCHAR **)out = wide_copy(text, heap, &length);
		return *(WCHAR **)out ? S_OK : WS_E_QUOTA_EXCEEDED;
	}
	case WS_XML_STRING_TYPE: {
		WS_XML_STRING *string = out;
		BYTE *bytes = heap_take(heap, text.length);
		if (!bytes)
			return WS_E_QUOTA_EXCEEDED;
		for (ULONG at = 0; at < text.length; ++at)
			bytes[at] = (BYTE)text.bytes[at];
		string->length = text.length;
		string->bytes = bytes;
		string->dictionary = NULL;
		string->id = 0;
		return S_OK;
	}
	case WS_BYTES_TYPE:
		return parse_base64(trimmed(text), heap, out);
	case WS_ENUM_TYPE: {
		const WS_ENUM_DESCRIPTION *values = description;
		const struct text name = trimmed(text);
		if (!values)
			return E_INVALIDARG;
		for (ULONG index = 0; index < values->valueCount; ++index)
			if (same_text(name, (const char *)values->values[index].name->bytes, values->values[index].name->length)) {
				*(int *)out = values->values[index].value;
				return S_OK;
			}
		return WS_E_INVALID_FORMAT;
	}
	default:
		return E_NOTIMPL;
	}
}

static const struct attribute *find_attribute(WS_XML_READER *reader, ULONG element, const WS_XML_STRING *local, const WS_XML_STRING *ns)
{
	const struct node *node = &reader->nodes[element];
	for (ULONG index = 0; index < node->attribute_count; ++index) {
		const struct attribute *attribute = &reader->attributes[node->first_attribute + index];
		if (same_text(attribute->local, (const char *)local->bytes, local->length) &&
			(!ns || same_text(attribute->ns, (const char *)ns->bytes, ns->length)))
			return attribute;
	}
	return NULL;
}

static BOOL is_nil(WS_XML_READER *reader, ULONG element)
{
	const struct node *node = &reader->nodes[element];
	for (ULONG index = 0; index < node->attribute_count; ++index) {
		const struct attribute *attribute = &reader->attributes[node->first_attribute + index];
		if (same_text(attribute->local, "nil", 3) && same_text(attribute->ns, instance_namespace, sizeof(instance_namespace) - 1))
			return same_text(trimmed(attribute->value), "true", 4) || same_text(trimmed(attribute->value), "1", 1);
	}
	return FALSE;
}

static HRESULT read_content(WS_XML_READER *reader, ULONG element, ULONG from, ULONG to, WS_TYPE type, const void *description, WS_HEAP *heap,
	void *out);

/* The next child element at or after from, below to, with this name; -1 when there is none. */
static ULONG next_child(WS_XML_READER *reader, ULONG from, ULONG to, const WS_XML_STRING *local, const WS_XML_STRING *ns)
{
	for (ULONG index = from; index < to; ++index) {
		const struct node *node = &reader->nodes[index];
		if (node->kind != NODE_START)
			continue;
		if (name_matches(node, local, ns))
			return index;
		index = node->end;
	}
	return (ULONG)-1;
}

/* One value of a field: in place, or in heap memory that the field points to. */
static HRESULT read_item(WS_XML_READER *reader, ULONG element, WS_TYPE type, const void *description, BOOL pointer, BOOL nillable, WS_HEAP *heap,
	void *slot)
{
	if (nillable && is_nil(reader, element)) {
		if (pointer)
			*(void **)slot = NULL;
		return S_OK;
	}
	void *target = slot;
	if (pointer) {
		target = heap_take(heap, type_size(type, description));
		if (!target)
			return WS_E_QUOTA_EXCEEDED;
		*(void **)slot = target;
	}
	return read_content(reader, element, element + 1, reader->nodes[element].end, type, description, heap, target);
}

static void set_default(const WS_FIELD_DESCRIPTION *field, BYTE *slot, ULONG size)
{
	if (field->defaultValue && field->defaultValue->value && field->defaultValue->valueSize == size)
		for (ULONG at = 0; at < size; ++at)
			slot[at] = ((const BYTE *)field->defaultValue->value)[at];
	else
		for (ULONG at = 0; at < size; ++at)
			slot[at] = 0;
}

static HRESULT read_struct(WS_XML_READER *reader, ULONG element, ULONG from, ULONG to, const WS_STRUCT_DESCRIPTION *description, WS_HEAP *heap,
	BYTE *out)
{
	if (!description)
		return E_INVALIDARG;
	for (ULONG at = 0; at < description->size; ++at)
		out[at] = 0;
	for (ULONG index = 0; index < description->fieldCount; ++index) {
		const WS_FIELD_DESCRIPTION *field = description->fields[index];
		BYTE *slot = out + field->offset;
		const BOOL pointer = field->options & WS_FIELD_POINTER, optional = field->options & (WS_FIELD_OPTIONAL | WS_FIELD_NILLABLE);
		const ULONG size = pointer ? sizeof(void *) : type_size(field->type, field->typeDescription);
		HRESULT result = S_OK;
		switch (field->mapping) {
		case WS_ATTRIBUTE_FIELD_MAPPING: {
			const struct attribute *attribute = element == (ULONG)-1 || !field->localName ? NULL :
				find_attribute(reader, element, field->localName, field->ns);
			if (!attribute) {
				if (!optional)
					return WS_E_INVALID_FORMAT;
				set_default(field, slot, size);
				break;
			}
			void *target = slot;
			if (pointer && !(target = *(void **)slot = heap_take(heap, type_size(field->type, field->typeDescription))))
				return WS_E_QUOTA_EXCEEDED;
			result = parse_value(attribute->value, field->type, field->typeDescription, heap, target);
			break;
		}
		case WS_ELEMENT_FIELD_MAPPING: {
			const ULONG child = next_child(reader, from, to, field->localName, field->ns);
			if (child == (ULONG)-1) {
				if (!optional)
					return WS_E_INVALID_FORMAT;
				set_default(field, slot, size);
				break;
			}
			result = read_item(reader, child, field->type, field->typeDescription, pointer, field->options & WS_FIELD_NILLABLE, heap, slot);
			break;
		}
		case WS_REPEATING_ELEMENT_FIELD_MAPPING: {
			/* Items stand in a wrapper element when the field names one, and among the element's own children otherwise. */
			ULONG first = from, last = to;
			if (field->localName) {
				const ULONG wrapper = next_child(reader, from, to, field->localName, field->ns);
				if (wrapper == (ULONG)-1)
					first = last = 0;
				else
					first = wrapper + 1, last = reader->nodes[wrapper].end;
			}
			ULONG count = 0;
			for (ULONG child = next_child(reader, first, last, field->itemLocalName, field->itemNs); child != (ULONG)-1;
				child = next_child(reader, reader->nodes[child].end + 1, last, field->itemLocalName, field->itemNs))
				++count;
			BYTE *items = NULL;
			if (count) {
				items = heap_take(heap, (SIZE_T)size * count);
				if (!items)
					return WS_E_QUOTA_EXCEEDED;
			}
			ULONG item = 0;
			for (ULONG child = next_child(reader, first, last, field->itemLocalName, field->itemNs); child != (ULONG)-1 && SUCCEEDED(result);
				child = next_child(reader, reader->nodes[child].end + 1, last, field->itemLocalName, field->itemNs))
				result = read_item(reader, child, field->type, field->typeDescription, pointer, field->options & WS_FIELD_NILLABLE_ITEM, heap,
					items + (SIZE_T)size * item++);
			*(void **)slot = items;
			*(ULONG *)(out + field->countOffset) = count;
			break;
		}
		case WS_TEXT_FIELD_MAPPING: {
			void *target = slot;
			if (pointer && !(target = *(void **)slot = heap_take(heap, type_size(field->type, field->typeDescription))))
				return WS_E_QUOTA_EXCEEDED;
			result = parse_value(content(reader, from, to, heap), field->type, field->typeDescription, heap, target);
			break;
		}
		case WS_NO_FIELD_MAPPING:
			set_default(field, slot, size);
			break;
		case WS_TYPE_ATTRIBUTE_FIELD_MAPPING:
		case WS_XML_ATTRIBUTE_FIELD_MAPPING:
		case WS_ANY_ELEMENT_FIELD_MAPPING:
		case WS_REPEATING_ANY_ELEMENT_FIELD_MAPPING:
		case WS_ANY_CONTENT_FIELD_MAPPING:
		case WS_ANY_ATTRIBUTES_FIELD_MAPPING:
			/* What these hold is not kept: the fields stay empty. */
			break;
		default:
			return E_NOTIMPL;
		}
		if (FAILED(result))
			return result;
	}
	return S_OK;
}

static HRESULT read_content(WS_XML_READER *reader, ULONG element, ULONG from, ULONG to, WS_TYPE type, const void *description, WS_HEAP *heap,
	void *out)
{
	if (type == WS_STRUCT_TYPE)
		return read_struct(reader, element, from, to, description, heap, out);
	return parse_value(content(reader, from, to, heap), type, description, heap, out);
}

EXPORT HRESULT WINAPI WsReadType(WS_XML_READER *reader, WS_TYPE_MAPPING mapping, WS_TYPE type, const void *description, WS_READ_OPTION option,
	WS_HEAP *heap, void *value, ULONG value_size, WS_ERROR *error)
{
	if (!reader || !reader->nodes || !value)
		return E_INVALIDARG;
	const ULONG size = type_size(type, description);
	if (!size)
		return failed(error, E_NOTIMPL, L"This type cannot be read yet.");
	const BOOL pointer_option = option == WS_READ_REQUIRED_POINTER || option == WS_READ_OPTIONAL_POINTER || option == WS_READ_NILLABLE_POINTER;
	/* A string of WCHARs is a pointer already: the pointer options read it in place. */
	const BOOL pointer = pointer_option && type != WS_WSZ_TYPE;
	if (value_size != (pointer ? sizeof(void *) : size) || (!heap && (pointer || type == WS_STRUCT_TYPE || type == WS_STRING_TYPE ||
		type == WS_WSZ_TYPE || type == WS_XML_STRING_TYPE || type == WS_BYTES_TYPE)))
		return E_INVALIDARG;
	ULONG element, from, to;
	if (mapping == WS_ELEMENT_TYPE_MAPPING) {
		skip_text(reader);
		if (current(reader)->kind != NODE_START) {
			if (option == WS_READ_OPTIONAL_POINTER) {
				*(void **)value = NULL;
				return S_OK;
			}
			return failed(error, WS_E_INVALID_FORMAT, L"The reader is not at an element.");
		}
		element = reader->cursor;
		from = element + 1;
		to = reader->nodes[element].end;
	} else if (mapping == WS_ELEMENT_CONTENT_TYPE_MAPPING) {
		/* The rest of the element the reader stands in. */
		element = current(reader)->parent;
		from = reader->cursor;
		to = element == (ULONG)-1 ? reader->count : reader->nodes[element].end;
	} else {
		return failed(error, E_NOTIMPL, L"This type mapping cannot be read yet.");
	}
	HRESULT result;
	const BOOL nil = element != (ULONG)-1 && mapping == WS_ELEMENT_TYPE_MAPPING && is_nil(reader, element);
	if (nil && (option == WS_READ_NILLABLE_POINTER || option == WS_READ_NILLABLE_VALUE)) {
		if (pointer)
			*(void **)value = NULL;
		else
			for (ULONG at = 0; at < size; ++at)
				((BYTE *)value)[at] = 0;
		result = S_OK;
	} else if (pointer) {
		void *target = heap_take(heap, size);
		if (!target)
			return failed(error, WS_E_QUOTA_EXCEEDED, L"The heap is full.");
		result = read_content(reader, element, from, to, type, description, heap, target);
		if (SUCCEEDED(result))
			*(void **)value = target;
	} else {
		result = read_content(reader, element, from, to, type, description, heap, value);
	}
	if (FAILED(result))
		return failed(error, result, result == WS_E_INVALID_FORMAT ? L"The input data was not in the expected format." : L"The value could not be read.");
	reader->cursor = mapping == WS_ELEMENT_TYPE_MAPPING ? to + 1 : to;
	return S_OK;
}

EXPORT HRESULT WINAPI WsReadElement(WS_XML_READER *reader, const WS_ELEMENT_DESCRIPTION *element, WS_READ_OPTION option, WS_HEAP *heap,
	void *value, ULONG value_size, WS_ERROR *error)
{
	if (!reader || !element)
		return E_INVALIDARG;
	BOOL found = FALSE;
	const HRESULT result = WsReadToStartElement(reader, element->elementLocalName, element->elementNs, &found, error);
	if (FAILED(result))
		return result;
	if (!found) {
		if (option == WS_READ_OPTIONAL_POINTER && value && value_size == sizeof(void *)) {
			*(void **)value = NULL;
			return S_OK;
		}
		return failed(error, WS_E_INVALID_FORMAT, L"The expected element was not found.");
	}
	return WsReadType(reader, WS_ELEMENT_TYPE_MAPPING, element->type, element->typeDescription, option, heap, value, value_size, error);
}

/* Writers: created and freed; writing is not here yet. */
struct _WS_XML_WRITER {
	ULONG unused;
};

EXPORT HRESULT WINAPI WsCreateWriter(const WS_XML_WRITER_PROPERTY *properties, ULONG count, WS_XML_WRITER **out, WS_ERROR *error)
{
	if (!out)
		return E_INVALIDARG;
	*out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(**out));
	return *out ? S_OK : E_OUTOFMEMORY;
}

EXPORT void WINAPI WsFreeWriter(WS_XML_WRITER *writer)
{
	if (writer)
		HeapFree(GetProcessHeap(), 0, writer);
}

/* Times: a WS_DATETIME counts 100-nanosecond ticks from the start of the year 1, a FILETIME from the start of 1601. */
#define FILETIME_START 504911232000000000ULL
#define DATETIME_LAST 0x2bca2875f4373fffULL
#ifndef WS_E_NUMERIC_OVERFLOW
#define WS_E_NUMERIC_OVERFLOW ((HRESULT)0x803d0002)
#endif

EXPORT HRESULT WINAPI WsDateTimeToFileTime(const WS_DATETIME *time, FILETIME *out, WS_ERROR *error)
{
	ULONGLONG ticks;
	if (!time || !out)
		return E_INVALIDARG;
	if (time->ticks < FILETIME_START)
		return failed(error, WS_E_NUMERIC_OVERFLOW, L"The time is before the first time a FILETIME holds.");
	ticks = time->ticks - FILETIME_START;
	out->dwLowDateTime = (DWORD)ticks;
	out->dwHighDateTime = (DWORD)(ticks >> 32);
	return S_OK;
}

EXPORT HRESULT WINAPI WsFileTimeToDateTime(const FILETIME *time, WS_DATETIME *out, WS_ERROR *error)
{
	ULONGLONG ticks;
	if (!time || !out)
		return E_INVALIDARG;
	ticks = (ULONGLONG)time->dwHighDateTime << 32 | time->dwLowDateTime;
	if (ticks > DATETIME_LAST - FILETIME_START)
		return failed(error, WS_E_NUMERIC_OVERFLOW, L"The time is past the last time a WS_DATETIME holds.");
	out->ticks = ticks + FILETIME_START;
	out->format = WS_DATETIME_FORMAT_UTC;
	return S_OK;
}

/* Two XML strings are equal when their bytes are. */
EXPORT HRESULT WINAPI WsXmlStringEquals(const WS_XML_STRING *first, const WS_XML_STRING *second, WS_ERROR *error)
{
	if (!first || !second)
		return E_INVALIDARG;
	return same_text((struct text){(const char *)first->bytes, first->length}, (const char *)second->bytes, second->length) ? S_OK : S_FALSE;
}

/* Channels, messages and service proxies are not here: creating one fails, so no call ever reaches the operations on them, and
 * a program sees a service it cannot reach. Node by node reading and writing are not here either. */
static HRESULT unavailable(WS_ERROR *error)
{
	return failed(error, E_NOTIMPL, L"This part of the Web Services API is not available.");
}

EXPORT HRESULT WINAPI WsCreateServiceProxy(const WS_CHANNEL_TYPE type, const WS_CHANNEL_BINDING binding, const WS_SECURITY_DESCRIPTION *security,
	const WS_PROXY_PROPERTY *properties, const ULONG count, const WS_CHANNEL_PROPERTY *channel_properties, const ULONG channel_count,
	WS_SERVICE_PROXY **out, WS_ERROR *error)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return unavailable(error);
}

EXPORT HRESULT WINAPI WsCreateServiceProxyFromTemplate(WS_CHANNEL_TYPE type, const WS_PROXY_PROPERTY *properties, const ULONG count,
	WS_BINDING_TEMPLATE_TYPE template_type, void *template_value, ULONG template_size, const void *description, ULONG description_size,
	WS_SERVICE_PROXY **out, WS_ERROR *error)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return unavailable(error);
}

EXPORT HRESULT WINAPI WsCreateChannel(WS_CHANNEL_TYPE type, WS_CHANNEL_BINDING binding, const WS_CHANNEL_PROPERTY *properties, ULONG count,
	const WS_SECURITY_DESCRIPTION *security, WS_CHANNEL **out, WS_ERROR *error)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return unavailable(error);
}

EXPORT HRESULT WINAPI WsCreateMessageForChannel(WS_CHANNEL *channel, const WS_MESSAGE_PROPERTY *properties, ULONG count, WS_MESSAGE **out,
	WS_ERROR *error)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return channel ? unavailable(error) : E_INVALIDARG;
}

EXPORT void WINAPI WsFreeServiceProxy(WS_SERVICE_PROXY *proxy) {}
EXPORT void WINAPI WsFreeChannel(WS_CHANNEL *channel) {}
EXPORT void WINAPI WsFreeMessage(WS_MESSAGE *message) {}

/* No proxy, channel or message exists, so whatever these are handed was not made here. */
#define NO_OBJECT(object) return (object) ? unavailable(error) : E_INVALIDARG

EXPORT HRESULT WINAPI WsOpenServiceProxy(WS_SERVICE_PROXY *proxy, const WS_ENDPOINT_ADDRESS *address, const WS_ASYNC_CONTEXT *context, WS_ERROR *error)
{ NO_OBJECT(proxy); }
EXPORT HRESULT WINAPI WsCloseServiceProxy(WS_SERVICE_PROXY *proxy, const WS_ASYNC_CONTEXT *context, WS_ERROR *error) { NO_OBJECT(proxy); }
EXPORT HRESULT WINAPI WsAbortServiceProxy(WS_SERVICE_PROXY *proxy, WS_ERROR *error) { NO_OBJECT(proxy); }
EXPORT HRESULT WINAPI WsCall(WS_SERVICE_PROXY *proxy, const WS_OPERATION_DESCRIPTION *operation, const void **arguments, WS_HEAP *heap,
	const WS_CALL_PROPERTY *properties, const ULONG count, const WS_ASYNC_CONTEXT *context, WS_ERROR *error)
{ NO_OBJECT(proxy); }
EXPORT HRESULT WINAPI WsOpenChannel(WS_CHANNEL *channel, const WS_ENDPOINT_ADDRESS *address, const WS_ASYNC_CONTEXT *context, WS_ERROR *error)
{ NO_OBJECT(channel); }
EXPORT HRESULT WINAPI WsCloseChannel(WS_CHANNEL *channel, const WS_ASYNC_CONTEXT *context, WS_ERROR *error) { NO_OBJECT(channel); }
EXPORT HRESULT WINAPI WsAbortChannel(WS_CHANNEL *channel, WS_ERROR *error) { NO_OBJECT(channel); }
EXPORT HRESULT WINAPI WsResetChannel(WS_CHANNEL *channel, WS_ERROR *error) { NO_OBJECT(channel); }
EXPORT HRESULT WINAPI WsGetChannelProperty(WS_CHANNEL *channel, WS_CHANNEL_PROPERTY_ID id, void *value, ULONG size, WS_ERROR *error)
{ NO_OBJECT(channel); }
EXPORT HRESULT WINAPI WsInitializeMessage(WS_MESSAGE *message, WS_MESSAGE_INITIALIZATION initialization, WS_MESSAGE *source, WS_ERROR *error)
{ NO_OBJECT(message); }
EXPORT HRESULT WINAPI WsResetMessage(WS_MESSAGE *message, WS_ERROR *error) { NO_OBJECT(message); }
EXPORT HRESULT WINAPI WsGetMessageProperty(WS_MESSAGE *message, WS_MESSAGE_PROPERTY_ID id, void *value, ULONG size, WS_ERROR *error)
{ NO_OBJECT(message); }
EXPORT HRESULT WINAPI WsAddMappedHeader(WS_MESSAGE *message, const WS_XML_STRING *name, WS_TYPE type, WS_WRITE_OPTION option, const void *value,
	ULONG size, WS_ERROR *error)
{ NO_OBJECT(message); }
EXPORT HRESULT WINAPI WsGetMappedHeader(WS_MESSAGE *message, const WS_XML_STRING *name, WS_REPEATING_HEADER_OPTION repeating, ULONG index,
	WS_TYPE type, WS_READ_OPTION option, WS_HEAP *heap, void *value, ULONG size, WS_ERROR *error)
{ NO_OBJECT(message); }
EXPORT HRESULT WINAPI WsSetHeader(WS_MESSAGE *message, WS_HEADER_TYPE header, WS_TYPE type, WS_WRITE_OPTION option, const void *value, ULONG size,
	WS_ERROR *error)
{ NO_OBJECT(message); }
EXPORT HRESULT WINAPI WsAddCustomHeader(WS_MESSAGE *message, const WS_ELEMENT_DESCRIPTION *element, WS_WRITE_OPTION option, const void *value,
	ULONG size, ULONG attributes, WS_ERROR *error)
{ NO_OBJECT(message); }
EXPORT HRESULT WINAPI WsWriteMessageStart(WS_CHANNEL *channel, WS_MESSAGE *message, const WS_ASYNC_CONTEXT *context, WS_ERROR *error)
{ NO_OBJECT(channel && message); }
EXPORT HRESULT WINAPI WsWriteMessageEnd(WS_CHANNEL *channel, WS_MESSAGE *message, const WS_ASYNC_CONTEXT *context, WS_ERROR *error)
{ NO_OBJECT(channel && message); }
EXPORT HRESULT WINAPI WsReadMessageStart(WS_CHANNEL *channel, WS_MESSAGE *message, const WS_ASYNC_CONTEXT *context, WS_ERROR *error)
{ NO_OBJECT(channel && message); }
EXPORT HRESULT WINAPI WsReadMessageEnd(WS_CHANNEL *channel, WS_MESSAGE *message, const WS_ASYNC_CONTEXT *context, WS_ERROR *error)
{ NO_OBJECT(channel && message); }
EXPORT HRESULT WINAPI WsAbandonMessage(WS_CHANNEL *channel, WS_MESSAGE *message, WS_ERROR *error) { NO_OBJECT(channel && message); }

/* No call reaches a service, so no error holds a fault. */
EXPORT HRESULT WINAPI WsGetFaultErrorDetail(WS_ERROR *error, const WS_FAULT_DETAIL_DESCRIPTION *description, WS_READ_OPTION option,
	WS_HEAP *heap, void *value, ULONG size)
{
	return error && description && value ? unavailable(NULL) : E_INVALIDARG;
}

EXPORT HRESULT WINAPI WsReadNode(WS_XML_READER *reader, WS_ERROR *error) { NO_OBJECT(reader); }
EXPORT HRESULT WINAPI WsGetReaderNode(WS_XML_READER *reader, const WS_XML_NODE **node, WS_ERROR *error) { NO_OBJECT(reader && node); }
EXPORT HRESULT WINAPI WsReadChars(WS_XML_READER *reader, WCHAR *chars, ULONG count, ULONG *done, WS_ERROR *error) { NO_OBJECT(reader); }
EXPORT HRESULT WINAPI WsReadBytes(WS_XML_READER *reader, void *bytes, ULONG count, ULONG *done, WS_ERROR *error) { NO_OBJECT(reader); }
EXPORT HRESULT WINAPI WsFindAttribute(WS_XML_READER *reader, const WS_XML_STRING *local, const WS_XML_STRING *ns, WINBOOL required, ULONG *index,
	WS_ERROR *error)
{ NO_OBJECT(reader); }
EXPORT HRESULT WINAPI WsReadStartAttribute(WS_XML_READER *reader, ULONG index, WS_ERROR *error) { NO_OBJECT(reader); }
EXPORT HRESULT WINAPI WsReadEndAttribute(WS_XML_READER *reader, WS_ERROR *error) { NO_OBJECT(reader); }
EXPORT HRESULT WINAPI WsSetInputToBuffer(WS_XML_READER *reader, WS_XML_BUFFER *buffer, const WS_XML_READER_PROPERTY *properties, ULONG count,
	WS_ERROR *error)
{ NO_OBJECT(reader && buffer); }
EXPORT HRESULT WINAPI WsSetOutput(WS_XML_WRITER *writer, const WS_XML_WRITER_ENCODING *encoding, const WS_XML_WRITER_OUTPUT *output,
	const WS_XML_WRITER_PROPERTY *properties, ULONG count, WS_ERROR *error)
{ NO_OBJECT(writer); }
EXPORT HRESULT WINAPI WsGetWriterProperty(WS_XML_WRITER *writer, WS_XML_WRITER_PROPERTY_ID id, void *value, ULONG size, WS_ERROR *error)
{ NO_OBJECT(writer); }
EXPORT HRESULT WINAPI WsWriteStartElement(WS_XML_WRITER *writer, const WS_XML_STRING *prefix, const WS_XML_STRING *local, const WS_XML_STRING *ns,
	WS_ERROR *error)
{ NO_OBJECT(writer); }
EXPORT HRESULT WINAPI WsWriteEndElement(WS_XML_WRITER *writer, WS_ERROR *error) { NO_OBJECT(writer); }
EXPORT HRESULT WINAPI WsWriteStartAttribute(WS_XML_WRITER *writer, const WS_XML_STRING *prefix, const WS_XML_STRING *local, const WS_XML_STRING *ns,
	WINBOOL single_quote, WS_ERROR *error)
{ NO_OBJECT(writer); }
EXPORT HRESULT WINAPI WsWriteEndAttribute(WS_XML_WRITER *writer, WS_ERROR *error) { NO_OBJECT(writer); }
EXPORT HRESULT WINAPI WsWriteChars(WS_XML_WRITER *writer, const WCHAR *chars, ULONG count, WS_ERROR *error) { NO_OBJECT(writer); }
EXPORT HRESULT WINAPI WsWriteBytes(WS_XML_WRITER *writer, const void *bytes, ULONG count, WS_ERROR *error) { NO_OBJECT(writer); }
EXPORT HRESULT WINAPI WsPullBytes(WS_XML_WRITER *writer, void *callback, void *state, WS_ERROR *error) { NO_OBJECT(writer); }
EXPORT HRESULT WINAPI WsWriteType(WS_XML_WRITER *writer, WS_TYPE_MAPPING mapping, WS_TYPE type, const void *description, WS_WRITE_OPTION option,
	const void *value, ULONG size, WS_ERROR *error)
{ NO_OBJECT(writer); }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
