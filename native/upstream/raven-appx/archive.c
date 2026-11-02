/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The ZIP archive a package is (PKWARE's APPNOTE.TXT): its central directory read through the program's stream, ZIP64 sizes and offsets
 * included, and the entries' data, stored or deflated (RFC 1951, with zlib). A package's parts are read through one stream, so each read
 * seeks and reads under a lock. */
#include "appx.h"
#include <zlib.h>

static UINT16 u16(const BYTE *at) { return (UINT16)(at[0] | at[1] << 8); }
static UINT32 u32(const BYTE *at) { return (UINT32)at[0] | (UINT32)at[1] << 8 | (UINT32)at[2] << 16 | (UINT32)at[3] << 24; }
static UINT64 u64(const BYTE *at) { return u32(at) | (UINT64)u32(at + 4) << 32; }

/* Reads exactly `size` bytes at an offset of the stream; a short read is a corrupt package. */
static HRESULT read_at(IStream *stream, UINT64 offset, void *buffer, UINT32 size)
{
	LARGE_INTEGER position;
	position.QuadPart = (LONGLONG)offset;
	HRESULT result = IStream_Seek(stream, position, STREAM_SEEK_SET, NULL);
	if (FAILED(result))
		return result;
	BYTE *at = buffer;
	while (size) {
		ULONG done = 0;
		result = IStream_Read(stream, at, size, &done);
		if (FAILED(result))
			return result;
		if (!done)
			return APPX_E_CORRUPT_CONTENT;
		at += done;
		size -= done;
	}
	return S_OK;
}

HRESULT appx_archive_open(IStream *stream, appx_archive **out)
{
	*out = NULL;
	STATSTG stat;
	HRESULT result = IStream_Stat(stream, &stat, STATFLAG_NONAME);
	if (FAILED(result))
		return result;
	const UINT64 total = stat.cbSize.QuadPart;
	/* The end of central directory record is the last 22 bytes, unless a comment of up to 65535 bytes follows it. */
	if (total < 22)
		return APPX_E_CORRUPT_CONTENT;
	const UINT32 tail = total < 22 + 65535 ? (UINT32)total : 22 + 65535;
	BYTE *end = ALLOC(tail);
	if (!end)
		return E_OUTOFMEMORY;
	result = read_at(stream, total - tail, end, tail);
	INT64 record = -1;
	for (INT64 at = (INT64)tail - 22; SUCCEEDED(result) && at >= 0; --at)
		if (u32(end + at) == 0x06054b50 && at + 22 + u16(end + at + 20) == tail) {
			record = at;
			break;
		}
	if (SUCCEEDED(result) && record < 0)
		result = APPX_E_CORRUPT_CONTENT;
	UINT64 count = 0, directory = 0, directory_size = 0;
	if (SUCCEEDED(result)) {
		const BYTE *eocd = end + record;
		count = u16(eocd + 10);
		directory_size = u32(eocd + 12);
		directory = u32(eocd + 16);
		/* ZIP64: the locator just before the record points at the ZIP64 end of central directory, which holds the real values. */
		if ((count == 0xffff || directory_size == 0xffffffff || directory == 0xffffffff) && record >= 20 && u32(eocd - 20) == 0x07064b50) {
			BYTE zip64[56];
			result = read_at(stream, u64(eocd - 20 + 8), zip64, sizeof zip64);
			if (SUCCEEDED(result) && u32(zip64) != 0x06064b50)
				result = APPX_E_CORRUPT_CONTENT;
			if (SUCCEEDED(result)) {
				count = u64(zip64 + 32);
				directory_size = u64(zip64 + 40);
				directory = u64(zip64 + 48);
			}
		}
	}
	FREE(end);
	if (SUCCEEDED(result) && (directory > total || directory_size > total - directory || directory_size > 0x7fffffff || count > directory_size / 46))
		result = APPX_E_CORRUPT_CONTENT;
	if (FAILED(result))
		return result;
	appx_archive *archive = ALLOC(sizeof *archive);
	BYTE *bytes = ALLOC(directory_size ? directory_size : 1);
	if (archive)
		archive->entries = ALLOC(sizeof *archive->entries * (count ? count : 1));
	if (!archive || !bytes || !archive->entries) {
		if (archive)
			FREE(archive->entries);
		FREE(archive);
		FREE(bytes);
		return E_OUTOFMEMORY;
	}
	archive->refs = 1;
	archive->stream = stream;
	IStream_AddRef(stream);
	InitializeCriticalSection(&archive->lock);
	result = read_at(stream, directory, bytes, (UINT32)directory_size);
	UINT64 at = 0;
	for (UINT64 index = 0; SUCCEEDED(result) && index < count; ++index) {
		const BYTE *header = bytes + at;
		if (directory_size - at < 46 || u32(header) != 0x02014b50) {
			result = APPX_E_CORRUPT_CONTENT;
			break;
		}
		const UINT16 name_length = u16(header + 28), extra_length = u16(header + 30), comment_length = u16(header + 32);
		if (directory_size - at - 46 < (UINT64)name_length + extra_length + comment_length) {
			result = APPX_E_CORRUPT_CONTENT;
			break;
		}
		appx_entry *entry = &archive->entries[archive->count];
		entry->method = u16(header + 10);
		entry->compressed = u32(header + 20);
		entry->size = u32(header + 24);
		entry->header = u32(header + 42);
		/* The ZIP64 extra field holds, in this order, each of these that the fixed fields mark as 0xffffffff. */
		const BYTE *extra = header + 46 + name_length, *extra_end = extra + extra_length;
		while (extra_end - extra >= 4) {
			const UINT16 id = u16(extra), length = u16(extra + 2);
			if (extra_end - extra - 4 < length)
				break;
			if (id == 0x0001) {
				const BYTE *field = extra + 4, *field_end = field + length;
				UINT64 *values[] = {&entry->size, &entry->compressed, &entry->header};
				for (int value = 0; value < 3; ++value)
					if (*values[value] == 0xffffffff) {
						if (field_end - field < 8) {
							result = APPX_E_CORRUPT_CONTENT;
							break;
						}
						*values[value] = u64(field);
						field += 8;
					}
			}
			extra += 4 + length;
		}
		/* The part names of OPC are ASCII once percent-encoded, so UTF-8 (general purpose bit 11) and code page 437 read the same. */
		entry->name = appx_strdup_utf8((const char *)header + 46, name_length);
		if (!entry->name && SUCCEEDED(result))
			result = E_OUTOFMEMORY;
		if (SUCCEEDED(result) && (entry->header > total || entry->compressed > total))
			result = APPX_E_CORRUPT_CONTENT;
		if (FAILED(result))
			break;
		archive->count++;
		at += 46 + (UINT64)name_length + extra_length + comment_length;
	}
	FREE(bytes);
	if (FAILED(result)) {
		appx_archive_release(archive);
		return result;
	}
	*out = archive;
	return S_OK;
}

void appx_archive_add_ref(appx_archive *archive) { InterlockedIncrement(&archive->refs); }

void appx_archive_release(appx_archive *archive)
{
	if (InterlockedDecrement(&archive->refs))
		return;
	for (UINT32 index = 0; index < archive->count; ++index)
		FREE(archive->entries[index].name);
	FREE(archive->entries);
	DeleteCriticalSection(&archive->lock);
	IStream_Release(archive->stream);
	FREE(archive);
}

static int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* A name as UTF-8 with '/' separators and ASCII letters in lower case, its percent-encoding decoded when it is a part name; the length, or
 * -1 when it does not fit. */
static int comparable(const WCHAR *name, BOOL encoded, char *out, int capacity)
{
	char raw[1024];
	const int length = WideCharToMultiByte(CP_UTF8, 0, name, -1, raw, sizeof raw, NULL, NULL);
	if (length <= 0)
		return -1;
	int written = 0;
	for (int at = 0; at < length - 1; ++at) {
		char c = raw[at];
		if (encoded && c == '%' && at + 2 < length - 1 && hex_digit(raw[at + 1]) >= 0 && hex_digit(raw[at + 2]) >= 0) {
			c = (char)(hex_digit(raw[at + 1]) << 4 | hex_digit(raw[at + 2]));
			at += 2;
		}
		if (written == capacity)
			return -1;
		out[written++] = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
	}
	return written;
}

BOOL appx_part_matches(const WCHAR *part, const WCHAR *name)
{
	/* A leading '/' names the same part. */
	if (*part == '/')
		++part;
	if (*name == '/' || *name == '\\')
		++name;
	char left[1024], right[1024];
	const int left_length = comparable(part, TRUE, left, sizeof left), right_length = comparable(name, FALSE, right, sizeof right);
	return left_length >= 0 && left_length == right_length && !memcmp(left, right, left_length);
}

appx_entry *appx_archive_find(appx_archive *archive, const WCHAR *name)
{
	for (UINT32 index = 0; index < archive->count; ++index)
		if (appx_part_matches(archive->entries[index].name, name))
			return &archive->entries[index];
	return NULL;
}

/* Where an entry's data starts: after its local header, whose name and extra field may differ in length from the central directory's. */
static HRESULT data_offset(appx_archive *archive, appx_entry *entry)
{
	if (entry->data)
		return S_OK;
	BYTE header[30];
	HRESULT result = read_at(archive->stream, entry->header, header, sizeof header);
	if (FAILED(result))
		return result;
	if (u32(header) != 0x04034b50)
		return APPX_E_CORRUPT_CONTENT;
	entry->data = entry->header + 30 + u16(header + 26) + u16(header + 28);
	return S_OK;
}

HRESULT appx_archive_read(appx_archive *archive, appx_entry *entry, UINT64 offset, void *buffer, UINT32 size)
{
	if (offset > entry->compressed || size > entry->compressed - offset)
		return APPX_E_CORRUPT_CONTENT;
	EnterCriticalSection(&archive->lock);
	HRESULT result = data_offset(archive, entry);
	if (SUCCEEDED(result))
		result = read_at(archive->stream, entry->data + offset, buffer, size);
	LeaveCriticalSection(&archive->lock);
	return result;
}

static voidpf zlib_alloc(voidpf opaque, uInt items, uInt size) { return HeapAlloc(GetProcessHeap(), 0, (SIZE_T)items * size); }
static void zlib_free(voidpf opaque, voidpf address) { HeapFree(GetProcessHeap(), 0, address); }

HRESULT appx_inflate(const BYTE *input, UINT64 input_size, BYTE *output, UINT64 output_size)
{
	z_stream inflater;
	ZeroMemory(&inflater, sizeof inflater);
	inflater.zalloc = zlib_alloc;
	inflater.zfree = zlib_free;
	if (inflateInit2(&inflater, -MAX_WBITS) != Z_OK)
		return E_OUTOFMEMORY;
	HRESULT result = S_OK;
	UINT64 consumed = 0, produced = 0;
	for (;;) {
		/* zlib counts in 32 bits: large data goes through in slices. */
		inflater.next_in = (Bytef *)input + consumed;
		inflater.avail_in = (uInt)(input_size - consumed > 0x40000000 ? 0x40000000 : input_size - consumed);
		inflater.next_out = output + produced;
		inflater.avail_out = (uInt)(output_size - produced > 0x40000000 ? 0x40000000 : output_size - produced);
		const uInt in = inflater.avail_in, out = inflater.avail_out;
		const int status = inflate(&inflater, Z_SYNC_FLUSH);
		consumed += in - inflater.avail_in;
		produced += out - inflater.avail_out;
		if (status == Z_STREAM_END || produced == output_size)
			break;
		if (status != Z_OK || (in == inflater.avail_in && out == inflater.avail_out)) {
			result = APPX_E_CORRUPT_CONTENT;
			break;
		}
	}
	/* A block of a package ends with a flush rather than the end of the stream; what counts is that it gives exactly its size. */
	if (SUCCEEDED(result) && produced != output_size)
		result = APPX_E_CORRUPT_CONTENT;
	inflateEnd(&inflater);
	return result;
}

HRESULT appx_archive_extract(appx_archive *archive, appx_entry *entry, BYTE **data, UINT64 *size)
{
	*data = NULL;
	*size = 0;
	if (entry->method != 0 && entry->method != 8)
		return APPX_E_CORRUPT_CONTENT;
	if (entry->compressed > 0x7fffffff || entry->size > 0x7fffffff)
		return E_OUTOFMEMORY;
	BYTE *raw = ALLOC(entry->compressed ? entry->compressed : 1);
	if (!raw)
		return E_OUTOFMEMORY;
	HRESULT result = appx_archive_read(archive, entry, 0, raw, (UINT32)entry->compressed);
	if (SUCCEEDED(result) && entry->method == 0) {
		if (entry->compressed != entry->size)
			result = APPX_E_CORRUPT_CONTENT;
		else {
			*data = raw;
			*size = entry->size;
			return S_OK;
		}
	}
	BYTE *content = NULL;
	if (SUCCEEDED(result) && !(content = ALLOC(entry->size ? entry->size : 1)))
		result = E_OUTOFMEMORY;
	if (SUCCEEDED(result))
		result = appx_inflate(raw, entry->compressed, content, entry->size);
	FREE(raw);
	if (FAILED(result)) {
		FREE(content);
		return result;
	}
	*data = content;
	*size = entry->size;
	return S_OK;
}
