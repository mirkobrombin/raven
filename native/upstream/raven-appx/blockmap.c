/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The block map of a package (AppxBlockMap.xml, the schema http://schemas.microsoft.com/appx/2010/blockmap): every payload file and the
 * manifest, each with its size, the size of its local header and the hash of each 64 KiB block of its content, hashed with the method
 * the map names. The signature signs the block map, and the block map vouches for the content. */
#include "appx.h"

static const WCHAR blockmap_namespace[] = L"http://schemas.microsoft.com/appx/2010/blockmap";

static BOOL parse_number(const WCHAR *text, UINT64 *value)
{
	if (!text || !*text)
		return FALSE;
	UINT64 result = 0;
	for (; *text; ++text) {
		if (*text < '0' || *text > '9' || result > (0xffffffffffffffffull - 9) / 10)
			return FALSE;
		result = result * 10 + (*text - '0');
	}
	*value = result;
	return TRUE;
}

/* The parts that describe the package rather than being its content, which the block map need not list: the block map, the signature,
 * the content types, and the metadata folder (the code integrity catalog, the content group map, a bundle's manifest). */
static BOOL footprint(const WCHAR *part)
{
	if (*part == '/')
		++part;
	static const WCHAR *const names[] = {L"AppxBlockMap.xml", L"AppxSignature.p7x", L"[Content_Types].xml"};
	for (UINT32 index = 0; index < ARRAYSIZE(names); ++index)
		if (appx_equal_nocase(part, names[index]))
			return TRUE;
	return lstrlenW(part) > 13 && CompareStringOrdinal(part, 13, L"AppxMetadata/", 13, TRUE) == CSTR_EQUAL;
}

HRESULT appx_blockmap_read(const BYTE *data, UINT64 size, appx_archive *archive, appx_blockmap **out)
{
	*out = NULL;
	appx_blockmap *map = ALLOC(sizeof *map);
	if (!map)
		return E_OUTOFMEMORY;
	map->refs = 1;
	HRESULT result = appx_parse(data, size, &map->document);
	if (FAILED(result)) {
		FREE(map);
		return APPX_E_INVALID_BLOCKMAP;
	}
	const appx_node *root = map->document.root;
	if (lstrcmpW(root->name, L"BlockMap") || lstrcmpW(root->uri, blockmap_namespace) ||
		!appx_hash_method(map->hash_method = appx_attribute_value(root, L"HashMethod"), &map->hash))
		result = APPX_E_INVALID_BLOCKMAP;
	UINT32 count = 0;
	for (appx_node *node = appx_child(root, L"File", NULL); node; node = appx_child(root, L"File", node))
		++count;
	if (SUCCEEDED(result) && !(map->files = ALLOC(sizeof *map->files * (count ? count : 1))))
		result = E_OUTOFMEMORY;
	for (appx_node *node = appx_child(root, L"File", NULL); SUCCEEDED(result) && node; node = appx_child(root, L"File", node)) {
		appx_mapped_file *file = &map->files[map->count];
		UINT64 header = 0;
		const WCHAR *name = appx_attribute_value(node, L"Name");
		if (!name || !*name || !parse_number(appx_attribute_value(node, L"Size"), &file->size) ||
			!parse_number(appx_attribute_value(node, L"LfhSize"), &header) || header > 0xffff) {
			result = APPX_E_INVALID_BLOCKMAP;
			break;
		}
		file->header = (UINT32)header;
		/* A name lists once, without regard to case, as the part names it stands for. */
		for (UINT32 index = 0; index < map->count; ++index)
			if (appx_equal_nocase(map->files[index].name, name))
				result = APPX_E_INVALID_BLOCKMAP;
		if (FAILED(result) || !(file->name = appx_strdup(name))) {
			if (SUCCEEDED(result))
				result = E_OUTOFMEMORY;
			break;
		}
		const UINT64 blocks = (file->size + APPX_BLOCK_SIZE - 1) / APPX_BLOCK_SIZE;
		UINT32 listed = 0;
		for (appx_node *block = appx_child(node, L"Block", NULL); block; block = appx_child(node, L"Block", block))
			++listed;
		if (listed != blocks) {
			result = APPX_E_INVALID_BLOCKMAP;
			break;
		}
		if (!(file->blocks = ALLOC(sizeof *file->blocks * (listed ? listed : 1)))) {
			result = E_OUTOFMEMORY;
			break;
		}
		for (appx_node *block = appx_child(node, L"Block", NULL); block; block = appx_child(node, L"Block", block)) {
			appx_block *entry = &file->blocks[file->count];
			UINT32 length = 0;
			const WCHAR *hash = appx_attribute_value(block, L"Hash");
			if (!hash || !appx_base64(hash, entry->hash, sizeof entry->hash, &length) || length != map->hash.length) {
				result = APPX_E_BLOCK_HASH_INVALID;
				break;
			}
			/* Size, the stored size of the block, is there only when the file is compressed. */
			const WCHAR *stored = appx_attribute_value(block, L"Size");
			UINT64 value = 0;
			if (stored && (!parse_number(stored, &value) || value > APPX_BLOCK_SIZE + APPX_BLOCK_SIZE / 2)) {
				result = APPX_E_INVALID_BLOCKMAP;
				break;
			}
			entry->compressed = (UINT32)value;
			file->count++;
		}
		if (FAILED(result))
			break;
		map->count++;
	}
	/* Within a package, each file the map lists is a part of that size, compressed when the map gives its blocks' stored sizes; and every
	 * part other than the footprint files is listed. */
	if (SUCCEEDED(result) && archive) {
		for (UINT32 index = 0; SUCCEEDED(result) && index < map->count; ++index) {
			appx_mapped_file *file = &map->files[index];
			if (!(file->entry = appx_archive_find(archive, file->name)))
				result = APPX_E_MISSING_REQUIRED_FILE;
			else if (file->entry->size != file->size || (file->entry->method != 0 && file->entry->method != 8))
				result = APPX_E_INVALID_BLOCKMAP;
		}
		for (UINT32 index = 0; SUCCEEDED(result) && index < archive->count; ++index) {
			const appx_entry *entry = &archive->entries[index];
			const WCHAR *name = entry->name;
			if (footprint(name) || (name[0] && name[lstrlenW(name) - 1] == '/'))
				continue;
			BOOL listed = FALSE;
			for (UINT32 file = 0; !listed && file < map->count; ++file)
				listed = map->files[file].entry == entry;
			if (!listed)
				result = APPX_E_INVALID_BLOCKMAP;
		}
		if (SUCCEEDED(result)) {
			map->archive = archive;
			appx_archive_add_ref(archive);
		}
	}
	if (FAILED(result)) {
		appx_blockmap_release(map);
		return result;
	}
	*out = map;
	return S_OK;
}

void appx_blockmap_add_ref(appx_blockmap *map) { InterlockedIncrement(&map->refs); }

void appx_blockmap_release(appx_blockmap *map)
{
	if (InterlockedDecrement(&map->refs))
		return;
	for (UINT32 index = 0; index < map->count; ++index) {
		FREE(map->files[index].name);
		FREE(map->files[index].blocks);
	}
	FREE(map->files);
	appx_document_free(&map->document);
	if (map->archive)
		appx_archive_release(map->archive);
	FREE(map);
}

appx_mapped_file *appx_blockmap_find(appx_blockmap *map, const WCHAR *name)
{
	if (!name)
		return NULL;
	for (UINT32 index = 0; index < map->count; ++index) {
		/* The names compare as the payload names they are, '\' or '/' separated and without case. */
		const WCHAR *a = map->files[index].name, *b = name;
		for (; *a && *b; ++a, ++b) {
			const WCHAR x = *a == '/' ? '\\' : *a, y = *b == '/' ? '\\' : *b;
			if (x != y && CompareStringOrdinal(&x, 1, &y, 1, TRUE) != CSTR_EQUAL)
				break;
		}
		if (!*a && !*b)
			return &map->files[index];
	}
	return NULL;
}

/* ---- The block map's objects ---- */

typedef struct block_object {
	IAppxBlockMapBlock iface;
	LONG refs;
	appx_blockmap *map;
	const appx_block *block;
} block_object;

static void block_destroy(block_object *self)
{
	appx_blockmap_release(self->map);
	FREE(self);
}

APPX_UNKNOWN(block, IAppxBlockMapBlock, block_object, appx_same(iid, &IID_IAppxBlockMapBlock), block_destroy)

static HRESULT WINAPI block_hash(IAppxBlockMapBlock *iface, UINT32 *size, BYTE **buffer)
{
	block_object *self = (block_object *)iface;
	if (!size || !buffer)
		return E_POINTER;
	if (!(*buffer = CoTaskMemAlloc(self->map->hash.length)))
		return E_OUTOFMEMORY;
	memcpy(*buffer, self->block->hash, self->map->hash.length);
	*size = self->map->hash.length;
	return S_OK;
}

static HRESULT WINAPI block_compressed_size(IAppxBlockMapBlock *iface, UINT32 *size)
{
	if (!size)
		return E_POINTER;
	*size = ((block_object *)iface)->block->compressed;
	return S_OK;
}

static const IAppxBlockMapBlockVtbl block_table = {block_query, block_add_ref, block_release, block_hash, block_compressed_size};

typedef struct file_object {
	IAppxBlockMapFile iface;
	LONG refs;
	appx_blockmap *map;
	appx_mapped_file *file;
} file_object;

static void file_destroy(file_object *self)
{
	appx_blockmap_release(self->map);
	FREE(self);
}

APPX_UNKNOWN(file, IAppxBlockMapFile, file_object, appx_same(iid, &IID_IAppxBlockMapFile), file_destroy)

static HRESULT WINAPI file_blocks(IAppxBlockMapFile *iface, IAppxBlockMapBlocksEnumerator **out)
{
	file_object *self = (file_object *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	IUnknown **items = ALLOC(sizeof *items * (self->file->count ? self->file->count : 1));
	if (!items)
		return E_OUTOFMEMORY;
	HRESULT result = S_OK;
	UINT32 made = 0;
	for (; made < self->file->count; ++made) {
		block_object *block = ALLOC(sizeof *block);
		if (!block) {
			result = E_OUTOFMEMORY;
			break;
		}
		block->iface.lpVtbl = &block_table;
		block->refs = 1;
		block->map = self->map;
		appx_blockmap_add_ref(self->map);
		block->block = &self->file->blocks[made];
		InterlockedIncrement(&appx_objects);
		items[made] = (IUnknown *)&block->iface;
	}
	if (SUCCEEDED(result))
		result = appx_enumerator(&IID_IAppxBlockMapBlocksEnumerator, items, made, (void **)out);
	for (UINT32 index = 0; index < made; ++index)
		IUnknown_Release(items[index]);
	FREE(items);
	return result;
}

static HRESULT WINAPI file_header_size(IAppxBlockMapFile *iface, UINT32 *size)
{
	if (!size)
		return E_POINTER;
	*size = ((file_object *)iface)->file->header;
	return S_OK;
}

static HRESULT WINAPI file_name(IAppxBlockMapFile *iface, LPWSTR *name) { return appx_task_string(((file_object *)iface)->file->name, name); }

static HRESULT WINAPI file_size(IAppxBlockMapFile *iface, UINT64 *size)
{
	if (!size)
		return E_POINTER;
	*size = ((file_object *)iface)->file->size;
	return S_OK;
}

/* Whether a stream's content, from its start, is the file the block map describes: its size and the hash of each block. */
static HRESULT WINAPI file_validate(IAppxBlockMapFile *iface, IStream *stream, BOOL *valid)
{
	file_object *self = (file_object *)iface;
	if (!stream || !valid)
		return E_POINTER;
	*valid = FALSE;
	LARGE_INTEGER start = {{0}};
	HRESULT result = IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
	if (FAILED(result))
		return result;
	BYTE *buffer = ALLOC(APPX_BLOCK_SIZE);
	if (!buffer)
		return E_OUTOFMEMORY;
	BOOL same = TRUE;
	UINT64 total = 0;
	for (UINT32 index = 0; same; ++index) {
		ULONG filled = 0;
		while (filled < APPX_BLOCK_SIZE) {
			ULONG done = 0;
			result = IStream_Read(stream, buffer + filled, APPX_BLOCK_SIZE - filled, &done);
			if (FAILED(result) || !done)
				break;
			filled += done;
		}
		if (FAILED(result))
			break;
		if (!filled)
			break;
		total += filled;
		BYTE digest[64];
		if (index >= self->file->count || FAILED(result = appx_hash_data(&self->map->hash, buffer, filled, digest)))
			same = FALSE;
		else
			same = !memcmp(digest, self->file->blocks[index].hash, self->map->hash.length);
	}
	FREE(buffer);
	if (FAILED(result))
		return result;
	*valid = same && total == self->file->size;
	return S_OK;
}

static const IAppxBlockMapFileVtbl file_table = {file_query, file_add_ref, file_release, file_blocks, file_header_size, file_name, file_size,
	file_validate};

static HRESULT make_file(appx_blockmap *map, appx_mapped_file *file, IAppxBlockMapFile **out)
{
	file_object *self = ALLOC(sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &file_table;
	self->refs = 1;
	self->map = map;
	appx_blockmap_add_ref(map);
	self->file = file;
	InterlockedIncrement(&appx_objects);
	*out = &self->iface;
	return S_OK;
}

typedef struct reader_object {
	IAppxBlockMapReader iface;
	LONG refs;
	appx_blockmap *map;
	IStream *stream;
} reader_object;

static void reader_destroy(reader_object *self)
{
	appx_blockmap_release(self->map);
	IStream_Release(self->stream);
	FREE(self);
}

APPX_UNKNOWN(reader, IAppxBlockMapReader, reader_object, appx_same(iid, &IID_IAppxBlockMapReader), reader_destroy)

static HRESULT WINAPI reader_file(IAppxBlockMapReader *iface, LPCWSTR name, IAppxBlockMapFile **out)
{
	reader_object *self = (reader_object *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!name || !*name)
		return E_INVALIDARG;
	appx_mapped_file *file = appx_blockmap_find(self->map, name);
	return file ? make_file(self->map, file, out) : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static HRESULT WINAPI reader_files(IAppxBlockMapReader *iface, IAppxBlockMapFilesEnumerator **out)
{
	reader_object *self = (reader_object *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	IUnknown **items = ALLOC(sizeof *items * (self->map->count ? self->map->count : 1));
	if (!items)
		return E_OUTOFMEMORY;
	HRESULT result = S_OK;
	UINT32 made = 0;
	for (; made < self->map->count && SUCCEEDED(result = make_file(self->map, &self->map->files[made], (IAppxBlockMapFile **)&items[made])); ++made)
		;
	if (SUCCEEDED(result))
		result = appx_enumerator(&IID_IAppxBlockMapFilesEnumerator, items, made, (void **)out);
	for (UINT32 index = 0; index < made; ++index)
		IUnknown_Release(items[index]);
	FREE(items);
	return result;
}

static HRESULT WINAPI reader_hash_method(IAppxBlockMapReader *iface, IUri **out)
{
	if (!out)
		return E_POINTER;
	return CreateUri(((reader_object *)iface)->map->hash_method, Uri_CREATE_CANONICALIZE, 0, out);
}

/* The block map as the package holds it, from its start. */
static HRESULT WINAPI reader_stream(IAppxBlockMapReader *iface, IStream **out)
{
	reader_object *self = (reader_object *)iface;
	if (!out)
		return E_POINTER;
	HRESULT result = IStream_Clone(self->stream, out);
	if (SUCCEEDED(result)) {
		LARGE_INTEGER start = {{0}};
		IStream_Seek(*out, start, STREAM_SEEK_SET, NULL);
	}
	return result;
}

static const IAppxBlockMapReaderVtbl reader_table = {reader_query, reader_add_ref, reader_release, reader_file, reader_files, reader_hash_method,
	reader_stream};

HRESULT appx_blockmap_reader(appx_blockmap *map, IStream *stream, IAppxBlockMapReader **out)
{
	*out = NULL;
	reader_object *self = ALLOC(sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &reader_table;
	self->refs = 1;
	self->map = map;
	appx_blockmap_add_ref(map);
	self->stream = stream;
	IStream_AddRef(stream);
	InterlockedIncrement(&appx_objects);
	*out = &self->iface;
	return S_OK;
}
