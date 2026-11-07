/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The parts of the packaging component the readers share: the package's ZIP archive, the parsed XML of its footprint files, the block map,
 * and the streams over a file's content. */
#ifndef RAVEN_APPX_H
#define RAVEN_APPX_H

#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <bcrypt.h>
#include "appxpackaging.h"

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

extern LONG appx_objects;

BOOL appx_same(REFIID left, REFIID right);

/* The IUnknown methods of an object whose interface pointer is its first member and whose count of references follows: it answers
 * IUnknown and the interfaces `answers` accepts (an expression of `iid`), and `destroy` frees it when the last reference goes. */
#define APPX_UNKNOWN(prefix, interface, type, answers, destroy) \
	static HRESULT WINAPI prefix##_query(interface *iface, REFIID iid, void **out) \
	{ \
		if (!out) \
			return E_POINTER; \
		if (appx_same(iid, &IID_IUnknown) || (answers)) { \
			InterlockedIncrement(&((type *)iface)->refs); \
			*out = iface; \
			return S_OK; \
		} \
		*out = NULL; \
		return E_NOINTERFACE; \
	} \
	static ULONG WINAPI prefix##_add_ref(interface *iface) { return InterlockedIncrement(&((type *)iface)->refs); } \
	static ULONG WINAPI prefix##_release(interface *iface) \
	{ \
		const LONG refs = InterlockedDecrement(&((type *)iface)->refs); \
		if (!refs) { \
			destroy((type *)iface); \
			InterlockedDecrement(&appx_objects); \
		} \
		return refs; \
	}

/* An enumerator over objects (IAppxFilesEnumerator, IAppxBlockMapBlocksEnumerator and the others of that shape): it holds a reference to
 * each item, and answers the interface it is made for. */
HRESULT appx_enumerator(const IID *iid, IUnknown **items, UINT32 count, void **out);
/* The same over strings (IAppxManifestResourcesEnumerator and the like), which the owner keeps alive; each item is returned as a copy. */
HRESULT appx_string_enumerator(const IID *iid, IUnknown *owner, const WCHAR **strings, UINT32 count, void **out);
/* A copy of a string the caller frees with CoTaskMemFree, as every string the readers return; NULL gives NULL. */
HRESULT appx_task_string(const WCHAR *text, LPWSTR *out);
WCHAR *appx_strdup(const WCHAR *text);
WCHAR *appx_strdup_utf8(const char *text, int length);
BOOL appx_equal_nocase(const WCHAR *left, const WCHAR *right);

/* ---- The ZIP archive (APPNOTE.TXT): the central directory, with ZIP64 sizes and offsets. ---- */
typedef struct appx_entry {
	WCHAR *name; /* the part name as the archive spells it, '/' separated and percent-encoded */
	UINT64 header; /* the offset of its local header */
	UINT64 data; /* the offset of its data, once the local header was read (0 before) */
	UINT64 compressed, size;
	WORD method; /* 0 stored, 8 deflated */
} appx_entry;

typedef struct appx_archive {
	LONG refs;
	IStream *stream;
	CRITICAL_SECTION lock;
	appx_entry *entries;
	UINT32 count;
} appx_archive;

HRESULT appx_archive_open(IStream *stream, appx_archive **out);
void appx_archive_add_ref(appx_archive *archive);
void appx_archive_release(appx_archive *archive);
/* Whether a part name, percent-encoded and '/' separated (OPC, ECMA-376 part 2), names a file, '\' or '/' separated; ASCII letters
 * compare without case. */
BOOL appx_part_matches(const WCHAR *part, const WCHAR *name);
/* The entry of a part name, '\' or '/' separated, compared without case as OPC part names are; NULL when the archive has none. */
appx_entry *appx_archive_find(appx_archive *archive, const WCHAR *name);
/* Bytes of an entry's raw (still compressed) data, at an offset within it. */
HRESULT appx_archive_read(appx_archive *archive, appx_entry *entry, UINT64 offset, void *buffer, UINT32 size);
/* An entry's whole content, decompressed, in a buffer the caller frees. */
HRESULT appx_archive_extract(appx_archive *archive, appx_entry *entry, BYTE **data, UINT64 *size);
/* Decompresses raw deflate data into a buffer of exactly the expected size. */
HRESULT appx_inflate(const BYTE *input, UINT64 input_size, BYTE *output, UINT64 output_size);

/* ---- XML: a footprint file parsed into a tree with the XmlLite reader. ---- */
typedef struct appx_attribute {
	const WCHAR *name, *uri, *value;
	struct appx_attribute *next;
} appx_attribute;

typedef struct appx_node {
	const WCHAR *name, *uri; /* local name and namespace */
	const WCHAR *text; /* the element's character data, or NULL */
	appx_attribute *attributes;
	struct appx_node *parent, *children, *last, *next;
} appx_node;

typedef struct appx_document {
	appx_node *root;
	struct appx_chunk *chunks;
} appx_document;

HRESULT appx_parse(const BYTE *data, UINT64 size, appx_document *document);
void appx_document_free(appx_document *document);
/* The value of an unqualified attribute, or NULL. */
const WCHAR *appx_attribute_value(const appx_node *node, const WCHAR *name);
/* The first child element with this local name (after a given one, when `after` is not NULL), or NULL. */
appx_node *appx_child(const appx_node *node, const WCHAR *name, const appx_node *after);

/* ---- Hashes: SHA-256, SHA-384 and SHA-512 through BCrypt, named as the block map's HashMethod. ---- */
typedef struct appx_hash {
	BCRYPT_ALG_HANDLE algorithm;
	UINT32 length;
} appx_hash;

BOOL appx_hash_method(const WCHAR *uri, appx_hash *hash);
HRESULT appx_hash_data(const appx_hash *hash, const BYTE *data, UINT32 size, BYTE *digest);
/* Base64 (RFC 4648) into a buffer of at most `capacity` bytes; FALSE for text that is not base64 or does not fit. */
BOOL appx_base64(const WCHAR *text, BYTE *out, UINT32 capacity, UINT32 *length);

/* ---- The block map (AppxBlockMap.xml): each payload file's size and the hashes of its 64 KiB blocks. ---- */
#define APPX_BLOCK_SIZE 65536u

typedef struct appx_block {
	UINT32 compressed; /* the block's stored size; its uncompressed size when the file is stored */
	BYTE hash[64];
} appx_block;

typedef struct appx_mapped_file {
	WCHAR *name; /* '\' separated, as the block map and the payload names spell it */
	UINT64 size;
	UINT32 header; /* LfhSize: the size of the file's local header */
	appx_block *blocks;
	UINT32 count;
	appx_entry *entry;
} appx_mapped_file;

typedef struct appx_blockmap {
	LONG refs;
	appx_document document;
	appx_hash hash;
	const WCHAR *hash_method;
	appx_mapped_file *files;
	UINT32 count;
	appx_archive *archive; /* the package the block map describes, or NULL for one read on its own */
} appx_blockmap;

/* Reads a block map; with an archive, checks that every file it lists is there with that size and every payload part is listed. */
HRESULT appx_blockmap_read(const BYTE *data, UINT64 size, appx_archive *archive, appx_blockmap **out);
void appx_blockmap_add_ref(appx_blockmap *map);
void appx_blockmap_release(appx_blockmap *map);
appx_mapped_file *appx_blockmap_find(appx_blockmap *map, const WCHAR *name);
HRESULT appx_blockmap_reader(appx_blockmap *map, IStream *stream, IAppxBlockMapReader **out);

/* ---- Streams: a read-only IStream over a file of a package, its blocks checked against the block map as they are read, or over bytes held
 * in memory. ---- */
HRESULT appx_file_stream(appx_archive *archive, appx_blockmap *map, appx_mapped_file *file, IStream **out);
/* Takes ownership of the buffer. */
HRESULT appx_memory_stream(BYTE *data, UINT64 size, IStream **out);
/* The whole content of a stream from its start, in a buffer the caller frees. */
HRESULT appx_read_stream(IStream *stream, BYTE **data, UINT64 *size);

/* ---- The manifest (AppxManifest.xml). ---- */
HRESULT appx_manifest_reader(BYTE *data, UINT64 size, IAppxManifestReader **out);
/* The publisher ID of a publisher name: the first 64 bits of the SHA-256 of its UTF-16 text in Crockford's base32, 13 characters. */
HRESULT appx_publisher_id(const WCHAR *publisher, WCHAR id[14]);

/* ---- The readers the factories make. ---- */
HRESULT appx_package_reader(IStream *stream, IAppxPackageReader **out);
HRESULT appx_bundle_reader(IStream *stream, IAppxBundleReader **out);

#endif
