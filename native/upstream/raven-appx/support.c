/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* What the readers share: strings, the footprint files parsed into trees by the XmlLite reader, block hashes through BCrypt, base64, and
 * the streams over a file's content held in memory. */
#include "appx.h"
/* The XmlLite reader's interface identifier is defined here; the import library has none. */
#include <initguid.h>
#include <xmllite.h>

BOOL appx_same(REFIID left, REFIID right)
{
	const DWORD *a = (const DWORD *)left, *b = (const DWORD *)right;
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

WCHAR *appx_strdup(const WCHAR *text)
{
	if (!text)
		return NULL;
	const SIZE_T length = lstrlenW(text);
	WCHAR *copy = ALLOC((length + 1) * sizeof(WCHAR));
	if (copy)
		memcpy(copy, text, length * sizeof(WCHAR));
	return copy;
}

WCHAR *appx_strdup_utf8(const char *text, int length)
{
	const int wide = length ? MultiByteToWideChar(CP_UTF8, 0, text, length, NULL, 0) : 0;
	if (length && !wide)
		return NULL;
	WCHAR *copy = ALLOC((wide + 1) * sizeof(WCHAR));
	if (copy && wide)
		MultiByteToWideChar(CP_UTF8, 0, text, length, copy, wide);
	return copy;
}

HRESULT appx_task_string(const WCHAR *text, LPWSTR *out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!text)
		return S_OK;
	const SIZE_T bytes = (lstrlenW(text) + 1) * sizeof(WCHAR);
	if (!(*out = CoTaskMemAlloc(bytes)))
		return E_OUTOFMEMORY;
	memcpy(*out, text, bytes);
	return S_OK;
}

BOOL appx_equal_nocase(const WCHAR *left, const WCHAR *right)
{
	return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

/* ---- Enumerators ---- */

/* Every enumerator of the packaging API has the same three methods after IUnknown's, with an interface pointer as the current item. */
typedef struct enumerator enumerator;
typedef struct enumerator_table {
	HRESULT(WINAPI *query)(enumerator *self, REFIID iid, void **out);
	ULONG(WINAPI *add_ref)(enumerator *self);
	ULONG(WINAPI *release)(enumerator *self);
	HRESULT(WINAPI *current)(enumerator *self, IUnknown **item);
	HRESULT(WINAPI *has_current)(enumerator *self, BOOL *has);
	HRESULT(WINAPI *move_next)(enumerator *self, BOOL *has);
} enumerator_table;

struct enumerator {
	const enumerator_table *table;
	LONG refs;
	IID iid;
	IUnknown **items;
	const WCHAR **strings; /* instead of items: strings that `owner` keeps */
	IUnknown *owner;
	UINT32 count, index;
};

static void enumerator_destroy(enumerator *self)
{
	for (UINT32 index = 0; self->items && index < self->count; ++index)
		IUnknown_Release(self->items[index]);
	FREE(self->items);
	FREE(self->strings);
	if (self->owner)
		IUnknown_Release(self->owner);
	FREE(self);
}

APPX_UNKNOWN(enumerator, enumerator, enumerator, appx_same(iid, &((enumerator *)iface)->iid), enumerator_destroy)

static HRESULT WINAPI enumerator_current(enumerator *self, IUnknown **item)
{
	if (!item)
		return E_POINTER;
	*item = NULL;
	if (self->index >= self->count)
		return E_BOUNDS;
	*item = self->items[self->index];
	IUnknown_AddRef(*item);
	return S_OK;
}

static HRESULT WINAPI enumerator_current_string(enumerator *self, IUnknown **item)
{
	if (!item)
		return E_POINTER;
	*item = NULL;
	if (self->index >= self->count)
		return E_BOUNDS;
	return appx_task_string(self->strings[self->index], (LPWSTR *)item);
}

static HRESULT WINAPI enumerator_has_current(enumerator *self, BOOL *has)
{
	if (!has)
		return E_POINTER;
	*has = self->index < self->count;
	return S_OK;
}

static HRESULT WINAPI enumerator_move_next(enumerator *self, BOOL *has)
{
	if (!has)
		return E_POINTER;
	if (self->index < self->count)
		++self->index;
	*has = self->index < self->count;
	return S_OK;
}

static const enumerator_table enumerator_methods = {enumerator_query, enumerator_add_ref, enumerator_release, enumerator_current,
	enumerator_has_current, enumerator_move_next};
static const enumerator_table string_methods = {enumerator_query, enumerator_add_ref, enumerator_release, enumerator_current_string,
	enumerator_has_current, enumerator_move_next};

HRESULT appx_enumerator(const IID *iid, IUnknown **items, UINT32 count, void **out)
{
	*out = NULL;
	enumerator *self = ALLOC(sizeof *self);
	if (self)
		self->items = ALLOC(sizeof *self->items * (count ? count : 1));
	if (!self || !self->items) {
		FREE(self);
		return E_OUTOFMEMORY;
	}
	self->table = &enumerator_methods;
	self->refs = 1;
	self->iid = *iid;
	for (UINT32 index = 0; index < count; ++index) {
		self->items[index] = items[index];
		IUnknown_AddRef(items[index]);
	}
	self->count = count;
	InterlockedIncrement(&appx_objects);
	*out = self;
	return S_OK;
}

HRESULT appx_string_enumerator(const IID *iid, IUnknown *owner, const WCHAR **strings, UINT32 count, void **out)
{
	*out = NULL;
	enumerator *self = ALLOC(sizeof *self);
	if (self)
		self->strings = ALLOC(sizeof *self->strings * (count ? count : 1));
	if (!self || !self->strings) {
		FREE(self);
		return E_OUTOFMEMORY;
	}
	self->table = &string_methods;
	self->refs = 1;
	self->iid = *iid;
	memcpy(self->strings, strings, sizeof *strings * count);
	self->count = count;
	self->owner = owner;
	IUnknown_AddRef(owner);
	InterlockedIncrement(&appx_objects);
	*out = self;
	return S_OK;
}

/* ---- XML ---- */

/* The tree's nodes and strings come from chunks freed together. */
typedef struct appx_chunk {
	struct appx_chunk *next;
	SIZE_T used, size;
	BYTE data[];
} appx_chunk;

static void *take(appx_document *document, SIZE_T size)
{
	size = (size + 7) & ~(SIZE_T)7;
	appx_chunk *chunk = document->chunks;
	if (!chunk || chunk->size - chunk->used < size) {
		const SIZE_T capacity = size > 16384 ? size : 16384;
		if (!(chunk = ALLOC(sizeof *chunk + capacity)))
			return NULL;
		chunk->size = capacity;
		chunk->next = document->chunks;
		document->chunks = chunk;
	}
	void *at = chunk->data + chunk->used;
	chunk->used += size;
	return at;
}

static const WCHAR *keep(appx_document *document, const WCHAR *text, UINT length)
{
	WCHAR *copy = take(document, (length + 1) * sizeof(WCHAR));
	if (copy) {
		memcpy(copy, text, length * sizeof(WCHAR));
		copy[length] = 0;
	}
	return copy;
}

void appx_document_free(appx_document *document)
{
	while (document->chunks) {
		appx_chunk *next = document->chunks->next;
		FREE(document->chunks);
		document->chunks = next;
	}
	document->root = NULL;
}

HRESULT appx_parse(const BYTE *data, UINT64 size, appx_document *document)
{
	ZeroMemory(document, sizeof *document);
	BYTE *copy = ALLOC(size ? size : 1);
	if (!copy)
		return E_OUTOFMEMORY;
	memcpy(copy, data, size);
	IStream *input = NULL;
	HRESULT result = appx_memory_stream(copy, size, &input);
	if (FAILED(result))
		return result;
	IXmlReader *reader = NULL;
	result = CreateXmlReader(&IID_IXmlReader, (void **)&reader, NULL);
	if (SUCCEEDED(result))
		result = IXmlReader_SetProperty(reader, XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
	if (SUCCEEDED(result))
		result = IXmlReader_SetInput(reader, (IUnknown *)input);
	appx_node *current = NULL;
	XmlNodeType type;
	while (SUCCEEDED(result) && (result = IXmlReader_Read(reader, &type)) == S_OK) {
		const WCHAR *name = NULL, *uri = NULL, *value = NULL;
		UINT name_length = 0, uri_length = 0, value_length = 0;
		if (type == XmlNodeType_Element) {
			appx_node *node = take(document, sizeof *node);
			if (!node) {
				result = E_OUTOFMEMORY;
				break;
			}
			ZeroMemory(node, sizeof *node);
			const BOOL empty = IXmlReader_IsEmptyElement(reader);
			if (FAILED(result = IXmlReader_GetLocalName(reader, &name, &name_length)) ||
				FAILED(result = IXmlReader_GetNamespaceUri(reader, &uri, &uri_length)))
				break;
			node->name = keep(document, name, name_length);
			node->uri = keep(document, uri, uri_length);
			appx_attribute **link = &node->attributes;
			for (HRESULT more = IXmlReader_MoveToFirstAttribute(reader); more == S_OK; more = IXmlReader_MoveToNextAttribute(reader)) {
				if (IXmlReader_IsDefault(reader))
					continue;
				appx_attribute *attribute = take(document, sizeof *attribute);
				if (!attribute || FAILED(result = IXmlReader_GetLocalName(reader, &name, &name_length)) ||
					FAILED(result = IXmlReader_GetNamespaceUri(reader, &uri, &uri_length)) ||
					FAILED(result = IXmlReader_GetValue(reader, &value, &value_length))) {
					if (!attribute)
						result = E_OUTOFMEMORY;
					break;
				}
				attribute->name = keep(document, name, name_length);
				attribute->uri = keep(document, uri, uri_length);
				attribute->value = keep(document, value, value_length);
				attribute->next = NULL;
				*link = attribute;
				link = &attribute->next;
			}
			if (FAILED(result))
				break;
			IXmlReader_MoveToElement(reader);
			node->parent = current;
			if (!current) {
				if (document->root) {
					result = WC_E_SYNTAX;
					break;
				}
				document->root = node;
			} else if (current->last)
				current->last = current->last->next = node;
			else
				current->children = current->last = node;
			if (!empty)
				current = node;
		} else if (type == XmlNodeType_EndElement) {
			if (current)
				current = current->parent;
		} else if ((type == XmlNodeType_Text || type == XmlNodeType_CDATA) && current) {
			if (FAILED(result = IXmlReader_GetValue(reader, &value, &value_length)))
				break;
			/* Character data split by comments or CDATA sections is one value. */
			const UINT before = current->text ? lstrlenW(current->text) : 0;
			WCHAR *joined = take(document, (before + value_length + 1) * sizeof(WCHAR));
			if (!joined) {
				result = E_OUTOFMEMORY;
				break;
			}
			if (before)
				memcpy(joined, current->text, before * sizeof(WCHAR));
			memcpy(joined + before, value, value_length * sizeof(WCHAR));
			joined[before + value_length] = 0;
			current->text = joined;
		}
	}
	if (result == S_FALSE)
		result = document->root ? S_OK : WC_E_SYNTAX;
	if (reader)
		IXmlReader_Release(reader);
	IStream_Release(input);
	if (FAILED(result))
		appx_document_free(document);
	return result;
}

const WCHAR *appx_attribute_value(const appx_node *node, const WCHAR *name)
{
	for (const appx_attribute *attribute = node ? node->attributes : NULL; attribute; attribute = attribute->next)
		if (!attribute->uri[0] && !lstrcmpW(attribute->name, name))
			return attribute->value;
	return NULL;
}

appx_node *appx_child(const appx_node *node, const WCHAR *name, const appx_node *after)
{
	for (appx_node *child = after ? after->next : node ? node->children : NULL; child; child = child->next)
		if (!lstrcmpW(child->name, name))
			return child;
	return NULL;
}

/* ---- Hashes ---- */

BOOL appx_hash_method(const WCHAR *uri, appx_hash *hash)
{
	static const struct {
		const WCHAR *uri;
		BCRYPT_ALG_HANDLE algorithm;
		UINT32 length;
	} methods[] = {{L"http://www.w3.org/2001/04/xmlenc#sha256", BCRYPT_SHA256_ALG_HANDLE, 32},
		{L"http://www.w3.org/2001/04/xmldsig-more#sha384", BCRYPT_SHA384_ALG_HANDLE, 48},
		{L"http://www.w3.org/2001/04/xmlenc#sha512", BCRYPT_SHA512_ALG_HANDLE, 64}};
	for (UINT32 index = 0; uri && index < ARRAYSIZE(methods); ++index)
		if (!lstrcmpW(uri, methods[index].uri)) {
			hash->algorithm = methods[index].algorithm;
			hash->length = methods[index].length;
			return TRUE;
		}
	return FALSE;
}

HRESULT appx_hash_data(const appx_hash *hash, const BYTE *data, UINT32 size, BYTE *digest)
{
	const NTSTATUS status = BCryptHash(hash->algorithm, NULL, 0, (BYTE *)data, size, digest, hash->length);
	return status ? HRESULT_FROM_NT(status) : S_OK;
}

BOOL appx_base64(const WCHAR *text, BYTE *out, UINT32 capacity, UINT32 *length)
{
	UINT32 bits = 0, count = 0, written = 0, padding = 0;
	for (; *text; ++text) {
		const WCHAR c = *text;
		int value;
		if (c >= 'A' && c <= 'Z')
			value = c - 'A';
		else if (c >= 'a' && c <= 'z')
			value = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			value = c - '0' + 52;
		else if (c == '+')
			value = 62;
		else if (c == '/')
			value = 63;
		else if (c == '=') {
			++padding;
			continue;
		} else if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			continue;
		else
			return FALSE;
		if (padding)
			return FALSE;
		bits = bits << 6 | (UINT32)value;
		if ((count += 6) >= 8) {
			count -= 8;
			if (written == capacity)
				return FALSE;
			out[written++] = (BYTE)(bits >> count);
		}
	}
	*length = written;
	return padding <= 2 && (bits & ((1u << count) - 1)) == 0;
}

/* ---- Streams over memory ---- */

typedef struct memory_stream {
	IStream iface;
	LONG refs;
	BYTE *data;
	UINT64 size, position;
} memory_stream;

static memory_stream *from_memory(IStream *iface) { return (memory_stream *)iface; }

static HRESULT WINAPI memory_query(IStream *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (appx_same(iid, &IID_IUnknown) || appx_same(iid, &IID_ISequentialStream) || appx_same(iid, &IID_IStream)) {
		*out = iface;
		IStream_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI memory_add_ref(IStream *iface) { return InterlockedIncrement(&from_memory(iface)->refs); }

static ULONG WINAPI memory_release(IStream *iface)
{
	memory_stream *self = from_memory(iface);
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		FREE(self->data);
		FREE(self);
		InterlockedDecrement(&appx_objects);
	}
	return refs;
}

static HRESULT WINAPI memory_read(IStream *iface, void *buffer, ULONG size, ULONG *done)
{
	memory_stream *self = from_memory(iface);
	if (!buffer)
		return STG_E_INVALIDPOINTER;
	const UINT64 left = self->position < self->size ? self->size - self->position : 0;
	const ULONG count = size < left ? size : (ULONG)left;
	memcpy(buffer, self->data + self->position, count);
	self->position += count;
	if (done)
		*done = count;
	return count < size ? S_FALSE : S_OK;
}

static HRESULT WINAPI read_only_write(IStream *iface, const void *buffer, ULONG size, ULONG *done)
{
	if (done)
		*done = 0;
	return STG_E_ACCESSDENIED;
}

/* The seek every read-only stream here shares: within its size, the position kept where `position` points. */
static HRESULT seek(UINT64 *position, UINT64 size, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *out)
{
	LONGLONG base;
	if (origin == STREAM_SEEK_SET)
		base = 0;
	else if (origin == STREAM_SEEK_CUR)
		base = (LONGLONG)*position;
	else if (origin == STREAM_SEEK_END)
		base = (LONGLONG)size;
	else
		return STG_E_INVALIDFUNCTION;
	if (move.QuadPart < 0 && -move.QuadPart > base)
		return STG_E_INVALIDFUNCTION;
	*position = (UINT64)(base + move.QuadPart);
	if (out)
		out->QuadPart = *position;
	return S_OK;
}

static HRESULT WINAPI memory_seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
	memory_stream *self = from_memory(iface);
	return seek(&self->position, self->size, move, origin, position);
}

static HRESULT WINAPI read_only_set_size(IStream *iface, ULARGE_INTEGER size) { return STG_E_ACCESSDENIED; }

/* Copies through the stream's own Read, as IStream::CopyTo is documented: from the current position, up to `size` bytes. */
static HRESULT WINAPI stream_copy_to(IStream *iface, IStream *target, ULARGE_INTEGER size, ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
	if (!target)
		return STG_E_INVALIDPOINTER;
	BYTE buffer[16384];
	UINT64 total_read = 0, total_written = 0;
	HRESULT result = S_OK;
	while (total_read < size.QuadPart) {
		const ULONG want = size.QuadPart - total_read < sizeof buffer ? (ULONG)(size.QuadPart - total_read) : sizeof buffer;
		ULONG got = 0, put = 0;
		result = IStream_Read(iface, buffer, want, &got);
		if (FAILED(result) || !got)
			break;
		total_read += got;
		result = IStream_Write(target, buffer, got, &put);
		total_written += put;
		if (FAILED(result))
			break;
	}
	if (read)
		read->QuadPart = total_read;
	if (written)
		written->QuadPart = total_written;
	return FAILED(result) ? result : S_OK;
}

static HRESULT WINAPI read_only_commit(IStream *iface, DWORD flags) { return S_OK; }
static HRESULT WINAPI read_only_revert(IStream *iface) { return S_OK; }
static HRESULT WINAPI read_only_lock(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type) { return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI read_only_unlock(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type) { return STG_E_INVALIDFUNCTION; }

static HRESULT stat_of(UINT64 size, STATSTG *stat)
{
	if (!stat)
		return STG_E_INVALIDPOINTER;
	ZeroMemory(stat, sizeof *stat);
	stat->type = STGTY_STREAM;
	stat->cbSize.QuadPart = size;
	stat->grfMode = STGM_READ;
	return S_OK;
}

static HRESULT WINAPI memory_stat(IStream *iface, STATSTG *stat, DWORD flags) { return stat_of(from_memory(iface)->size, stat); }

static HRESULT WINAPI memory_clone(IStream *iface, IStream **out)
{
	memory_stream *self = from_memory(iface);
	if (!out)
		return STG_E_INVALIDPOINTER;
	BYTE *copy = ALLOC(self->size ? self->size : 1);
	if (!copy)
		return E_OUTOFMEMORY;
	memcpy(copy, self->data, self->size);
	HRESULT result = appx_memory_stream(copy, self->size, out);
	if (SUCCEEDED(result))
		from_memory(*out)->position = self->position;
	return result;
}

static IStreamVtbl memory_table = {memory_query, memory_add_ref, memory_release, memory_read, read_only_write, memory_seek, read_only_set_size,
	stream_copy_to, read_only_commit, read_only_revert, read_only_lock, read_only_unlock, memory_stat, memory_clone};

HRESULT appx_memory_stream(BYTE *data, UINT64 size, IStream **out)
{
	*out = NULL;
	memory_stream *self = ALLOC(sizeof *self);
	if (!self) {
		FREE(data);
		return E_OUTOFMEMORY;
	}
	self->iface.lpVtbl = &memory_table;
	self->refs = 1;
	self->data = data;
	self->size = size;
	InterlockedIncrement(&appx_objects);
	*out = &self->iface;
	return S_OK;
}

HRESULT appx_read_stream(IStream *stream, BYTE **data, UINT64 *size)
{
	*data = NULL;
	*size = 0;
	if (!stream)
		return E_INVALIDARG;
	STATSTG stat;
	HRESULT result = IStream_Stat(stream, &stat, STATFLAG_NONAME);
	if (FAILED(result))
		return result;
	if (stat.cbSize.QuadPart > 0x7fffffff)
		return E_OUTOFMEMORY;
	LARGE_INTEGER start = {{0}};
	if (FAILED(result = IStream_Seek(stream, start, STREAM_SEEK_SET, NULL)))
		return result;
	const ULONG length = (ULONG)stat.cbSize.QuadPart;
	BYTE *buffer = ALLOC(length ? length : 1);
	if (!buffer)
		return E_OUTOFMEMORY;
	ULONG total = 0;
	while (total < length) {
		ULONG done = 0;
		result = IStream_Read(stream, buffer + total, length - total, &done);
		if (FAILED(result) || !done)
			break;
		total += done;
	}
	if (FAILED(result)) {
		FREE(buffer);
		return result;
	}
	*data = buffer;
	*size = total;
	return S_OK;
}

/* ---- Streams over a file of a package ---- */

/* A file the block map lists is read a block at a time: each block starts where the previous one's stored bytes end (a package writer
 * flushes the compressor at every block) and is checked against its hash once decompressed, as the stream reaches it. */
typedef struct file_stream {
	IStream iface;
	LONG refs;
	appx_archive *archive;
	appx_blockmap *map;
	appx_mapped_file *file;
	appx_entry *entry;
	UINT64 position;
	UINT64 *starts; /* where each block's stored bytes start in the entry's data */
	CRITICAL_SECTION lock;
	UINT32 cached; /* the block held in `block`, or MAXUINT32 */
	BYTE *block;
	BYTE *raw;
} file_stream;

static file_stream *from_file(IStream *iface) { return (file_stream *)iface; }

static ULONG WINAPI file_add_ref(IStream *iface) { return InterlockedIncrement(&from_file(iface)->refs); }

static ULONG WINAPI file_release(IStream *iface)
{
	file_stream *self = from_file(iface);
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		DeleteCriticalSection(&self->lock);
		FREE(self->starts);
		FREE(self->block);
		FREE(self->raw);
		appx_blockmap_release(self->map);
		appx_archive_release(self->archive);
		FREE(self);
		InterlockedDecrement(&appx_objects);
	}
	return refs;
}

static UINT32 block_size(const file_stream *self, UINT32 index)
{
	const UINT64 offset = (UINT64)index * APPX_BLOCK_SIZE;
	return self->file->size - offset < APPX_BLOCK_SIZE ? (UINT32)(self->file->size - offset) : APPX_BLOCK_SIZE;
}

/* Brings a block into the cache, decompressed and checked. */
static HRESULT load_block(file_stream *self, UINT32 index)
{
	if (self->cached == index)
		return S_OK;
	const appx_block *block = &self->file->blocks[index];
	const UINT32 size = block_size(self, index);
	const UINT32 stored = self->entry->method == 0 ? size : block->compressed;
	if (stored > APPX_BLOCK_SIZE + APPX_BLOCK_SIZE / 2)
		return APPX_E_CORRUPT_CONTENT;
	self->cached = MAXUINT32;
	HRESULT result = appx_archive_read(self->archive, self->entry, self->starts[index], self->entry->method == 0 ? self->block : self->raw, stored);
	if (SUCCEEDED(result) && self->entry->method != 0)
		result = appx_inflate(self->raw, stored, self->block, size);
	BYTE digest[64];
	if (SUCCEEDED(result))
		result = appx_hash_data(&self->map->hash, self->block, size, digest);
	if (SUCCEEDED(result) && memcmp(digest, block->hash, self->map->hash.length))
		result = APPX_E_CORRUPT_CONTENT;
	if (SUCCEEDED(result))
		self->cached = index;
	return result;
}

static HRESULT WINAPI file_read(IStream *iface, void *buffer, ULONG size, ULONG *done)
{
	file_stream *self = from_file(iface);
	if (done)
		*done = 0;
	if (!buffer)
		return STG_E_INVALIDPOINTER;
	EnterCriticalSection(&self->lock);
	HRESULT result = S_OK;
	ULONG total = 0;
	while (total < size && self->position < self->file->size) {
		const UINT32 index = (UINT32)(self->position / APPX_BLOCK_SIZE), within = (UINT32)(self->position % APPX_BLOCK_SIZE);
		if (FAILED(result = load_block(self, index)))
			break;
		const UINT32 left = block_size(self, index) - within;
		const ULONG count = size - total < left ? size - total : left;
		memcpy((BYTE *)buffer + total, self->block + within, count);
		total += count;
		self->position += count;
	}
	LeaveCriticalSection(&self->lock);
	if (done)
		*done = total;
	if (FAILED(result))
		return result;
	return total < size ? S_FALSE : S_OK;
}

static HRESULT WINAPI file_seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
	file_stream *self = from_file(iface);
	EnterCriticalSection(&self->lock);
	const HRESULT result = seek(&self->position, self->file->size, move, origin, position);
	LeaveCriticalSection(&self->lock);
	return result;
}

static HRESULT WINAPI file_stat(IStream *iface, STATSTG *stat, DWORD flags) { return stat_of(from_file(iface)->file->size, stat); }

static HRESULT WINAPI file_clone(IStream *iface, IStream **out)
{
	file_stream *self = from_file(iface);
	if (!out)
		return STG_E_INVALIDPOINTER;
	HRESULT result = appx_file_stream(self->archive, self->map, self->file, out);
	if (SUCCEEDED(result))
		from_file(*out)->position = self->position;
	return result;
}

static IStreamVtbl file_table = {memory_query, file_add_ref, file_release, file_read, read_only_write, file_seek, read_only_set_size, stream_copy_to,
	read_only_commit, read_only_revert, read_only_lock, read_only_unlock, file_stat, file_clone};

HRESULT appx_file_stream(appx_archive *archive, appx_blockmap *map, appx_mapped_file *file, IStream **out)
{
	*out = NULL;
	appx_entry *entry = file->entry;
	if (entry->method != 0 && entry->method != 8)
		return APPX_E_CORRUPT_CONTENT;
	/* The blocks a stored file is cut into are its size; a deflated one's are its compressed sizes, which add up to its data. */
	UINT64 *starts = ALLOC(sizeof *starts * (file->count ? file->count : 1));
	if (!starts)
		return E_OUTOFMEMORY;
	UINT64 at = 0;
	for (UINT32 index = 0; index < file->count; ++index) {
		starts[index] = at;
		const UINT64 offset = (UINT64)index * APPX_BLOCK_SIZE;
		at += entry->method == 0 ? (file->size - offset < APPX_BLOCK_SIZE ? file->size - offset : APPX_BLOCK_SIZE) : file->blocks[index].compressed;
	}
	if (at > entry->compressed) {
		FREE(starts);
		return APPX_E_INVALID_BLOCKMAP;
	}
	file_stream *self = ALLOC(sizeof *self);
	if (self) {
		self->block = ALLOC(APPX_BLOCK_SIZE);
		self->raw = entry->method ? ALLOC(APPX_BLOCK_SIZE + APPX_BLOCK_SIZE / 2) : NULL;
	}
	if (!self || !self->block || (entry->method && !self->raw)) {
		if (self) {
			FREE(self->block);
			FREE(self->raw);
		}
		FREE(self);
		FREE(starts);
		return E_OUTOFMEMORY;
	}
	self->iface.lpVtbl = &file_table;
	self->refs = 1;
	self->archive = archive;
	appx_archive_add_ref(archive);
	self->map = map;
	appx_blockmap_add_ref(map);
	self->file = file;
	self->entry = entry;
	self->starts = starts;
	self->cached = MAXUINT32;
	InitializeCriticalSection(&self->lock);
	InterlockedIncrement(&appx_objects);
	*out = &self->iface;
	return S_OK;
}
