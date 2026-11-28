/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Storage.Streams.RandomAccessStream and RandomAccessStreamReference:
 *
 * - CopyAsync copies an input stream into an output stream, all of it or a number of bytes, in reads of 64 KiB; CopyAndCloseAsync then
 *   flushes the output and closes both. A cancel stops between two reads.
 * - A reference opens what it names for reading, with a content type: a storage file (as the file itself opens), a stream of
 *   raven-winrt (from its start, as application/octet-stream), or a URI of the package (ms-appx:///) or of a file (file:///). Other
 *   URIs are named in Raven's trace. */
#include "object.h"
#include "../raven-winrt/winrt.h"
#include <appmodel.h>

static const IID iid_stream_statics = {0x524cedcf, 0x6e29, 0x4ce5, {0x95, 0x73, 0x6b, 0x75, 0x3d, 0xb6, 0x6c, 0x3a}};
static const IID iid_reference_statics = {0x857309dc, 0x3fbf, 0x4e7d, {0x98, 0x6f, 0xef, 0x3b, 0x1a, 0x07, 0xa9, 0x64}};
static const IID iid_stream_reference = {0x33ee3134, 0x1dd6, 0x4e3a, {0x80, 0x67, 0xd1, 0xc1, 0x62, 0xe8, 0x64, 0x2b}};
static const IID iid_closable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
/* IAsyncOperation<IRandomAccessStreamWithContentType>. */
static const IID iid_typed_stream_operation = {0xc4a57c5e, 0x32b0, 0x55b3, {0xad, 0x13, 0xce, 0x1c, 0x23, 0x04, 0x1e, 0xd6}};

typedef __x_ABI_CWindows_CStorage_CStreams_CIBuffer buffer_type;
typedef __x_ABI_CWindows_CStorage_CStreams_CIInputStream input_type;
typedef __x_ABI_CWindows_CStorage_CStreams_CIOutputStream output_type;
typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClass uri_type;

#define COPY_PIECE 65536

static void close_any(IUnknown *stream)
{
	IUnknown *closable = NULL;
	if (stream && SUCCEEDED(stream->lpVtbl->QueryInterface(stream, &iid_closable, (void **)&closable))) {
		((HRESULT (WINAPI *)(void *))method(closable, 6))(closable);
		release(closable);
	}
}

/* A copy, holding both streams until it has run. limit is the most bytes it copies, or all with UINT64 all ones. */
struct copy {
	input_type *source;
	output_type *target;
	UINT64 limit;
	BOOL close;
};

static void free_copy(void *context)
{
	struct copy *copy = context;
	release(copy->source);
	release(copy->target);
	FREE(copy);
}

static HRESULT run_copy(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct copy *copy = context;
	buffer_type *buffer = NULL;
	UINT64 total = 0;
	HRESULT outcome = winrt_buffer(NULL, 0, COPY_PIECE, &buffer);
	while (SUCCEEDED(outcome) && total < copy->limit) {
		if (winrt_async_canceled(operation)) {
			outcome = E_ABORT;
			break;
		}
		const UINT32 wanted = copy->limit - total < COPY_PIECE ? (UINT32)(copy->limit - total) : COPY_PIECE;
		void *read = NULL, *write = NULL;
		buffer_type *filled = NULL;
		UINT32 length = 0, written = 0;
		outcome = copy->source->lpVtbl->ReadAsync(copy->source, buffer, wanted, InputStreamOptions_None, (void *)&read);
		if (SUCCEEDED(outcome))
			outcome = winrt_wait(read, TRUE, &filled);
		release(read);
		if (SUCCEEDED(outcome))
			outcome = filled->lpVtbl->get_Length(filled, &length);
		/* An empty read is the end of the source. */
		if (SUCCEEDED(outcome) && length) {
			outcome = copy->target->lpVtbl->WriteAsync(copy->target, filled, (void *)&write);
			if (SUCCEEDED(outcome))
				outcome = winrt_wait(write, TRUE, &written);
			release(write);
			total += written;
		}
		winrt_put(filled);
		if (SUCCEEDED(outcome) && !length)
			break;
	}
	winrt_put(buffer);
	if (SUCCEEDED(outcome) && copy->close) {
		void *flush = NULL;
		boolean flushed = FALSE;
		outcome = copy->target->lpVtbl->FlushAsync(copy->target, (void *)&flush);
		if (SUCCEEDED(outcome))
			outcome = winrt_wait(flush, FALSE, &flushed);
		release(flush);
		close_any((IUnknown *)copy->source);
		close_any((IUnknown *)copy->target);
	}
	result->uint64 = total;
	return outcome;
}

static HRESULT start_copy(IUnknown *source, IUnknown *target, UINT64 limit, BOOL close, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!source || !target)
		return E_INVALIDARG;
	struct copy *copy = ALLOC(sizeof *copy);
	if (!copy)
		return E_OUTOFMEMORY;
	copy->limit = limit;
	copy->close = close;
	HRESULT result = source->lpVtbl->QueryInterface(source, &IID___x_ABI_CWindows_CStorage_CStreams_CIInputStream, (void **)&copy->source);
	if (SUCCEEDED(result))
		result = target->lpVtbl->QueryInterface(target, &IID___x_ABI_CWindows_CStorage_CStreams_CIOutputStream, (void **)&copy->target);
	if (FAILED(result)) {
		free_copy(copy);
		return E_INVALIDARG;
	}
	return winrt_async_start(&IID___FIAsyncOperationWithProgress_2_UINT64_UINT64, TRUE, WINRT_RESULT_UINT64, run_copy, copy, free_copy, out);
}

static HRESULT WINAPI stream_copy(void *iface, IUnknown *source, IUnknown *target, void **out)
{
	return start_copy(source, target, ~(UINT64)0, FALSE, out);
}

static HRESULT WINAPI stream_copy_size(void *iface, IUnknown *source, IUnknown *target, UINT64 size, void **out)
{
	return start_copy(source, target, size, FALSE, out);
}

static HRESULT WINAPI stream_copy_close(void *iface, IUnknown *source, IUnknown *target, void **out)
{
	return start_copy(source, target, ~(UINT64)0, TRUE, out);
}

TABLE(stream_factory_table, 0, FACTORY);
TABLE(stream_statics_table, 1, INSPECTABLE, (const void *)stream_copy, (const void *)stream_copy_size, (const void *)stream_copy_close);
static const struct face_iid stream_class_iids[] = {{&iid_factory, 0}, {&iid_stream_statics, 1}, {NULL, 0}};
static const struct kind stream_class_kind = {"RandomAccessStream", L"Windows.Storage.Streams.RandomAccessStream", stream_class_iids, NULL};
struct object random_access_stream_class = {{stream_factory_table.slots, stream_statics_table.slots}, &stream_class_kind, 1};

/* RandomAccessStreamReference: what it names is a file's reference, a stream, or a path. */
struct stream_ref {
	struct object base;
	IUnknown *file, *stream;
	WCHAR path[MAX_PATH];
};

static void destroy_reference(struct object *base)
{
	struct stream_ref *self = (struct stream_ref *)base;
	release(self->file);
	release(self->stream);
}

static HRESULT run_open(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct stream_ref *self = context;
	if (self->stream)
		return winrt_stream_typed(self->stream, L"application/octet-stream", &result->object);
	return open_typed_stream(self->path, &result->object);
}

static void free_open(void *context) { release(face_of(&((struct stream_ref *)context)->base, 0)); }

static HRESULT WINAPI reference_open(void *iface, void **out)
{
	struct stream_ref *self = (struct stream_ref *)owner(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	/* A file opens itself. */
	if (self->file)
		return ((HRESULT (WINAPI *)(void *, void **))method(self->file, 6))(self->file, out);
	reference(face_of(&self->base, 0));
	return winrt_async_start(&iid_typed_stream_operation, FALSE, WINRT_RESULT_OBJECT, run_open, self, free_open, out);
}

TABLE(reference_table, 0, INSPECTABLE, (const void *)reference_open);
static const struct face_iid reference_iids[] = {{&iid_stream_reference, 0}, {NULL, 0}};
static const struct kind reference_kind = {"RandomAccessStreamReference", L"Windows.Storage.Streams.RandomAccessStreamReference",
	reference_iids, destroy_reference};

static struct stream_ref *new_reference(void)
{
	static const void *const *const tables[] = {reference_table.slots};
	return make(sizeof(struct stream_ref), &reference_kind, tables, 1);
}

static HRESULT WINAPI reference_from_file(void *iface, IUnknown *file, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	IUnknown *opener = NULL;
	if (!file || FAILED(file->lpVtbl->QueryInterface(file, &iid_stream_reference, (void **)&opener)))
		return E_INVALIDARG;
	struct stream_ref *self = new_reference();
	if (!self) {
		release(opener);
		return E_OUTOFMEMORY;
	}
	self->file = opener;
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI reference_from_stream(void *iface, IUnknown *stream, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!stream)
		return E_INVALIDARG;
	struct stream_ref *self = new_reference();
	if (!self)
		return E_OUTOFMEMORY;
	reference(self->stream = stream);
	*out = face_of(&self->base, 0);
	return S_OK;
}

HRESULT stream_reference_make(IUnknown *stream, void **out) { return reference_from_stream(NULL, stream, out); }

static int hex(WCHAR digit)
{
	return digit >= L'0' && digit <= L'9' ? digit - L'0' : digit >= L'a' && digit <= L'f' ? digit - L'a' + 10 :
		digit >= L'A' && digit <= L'F' ? digit - L'A' + 10 : -1;
}

/* The path of a URI's escaped path, its %XX sequences read as UTF-8 and its slashes made backslashes, after base. */
static HRESULT path_of_uri(const WCHAR *base, HSTRING escaped, WCHAR *path)
{
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(escaped, &length);
	char *bytes = ALLOC((size_t)length * 3 + 1);
	if (!bytes)
		return E_OUTOFMEMORY;
	UINT32 count = 0;
	for (UINT32 at = 0; at < length; ++at) {
		int high, low;
		if (text[at] == L'%' && at + 2 < length && (high = hex(text[at + 1])) >= 0 && (low = hex(text[at + 2])) >= 0) {
			bytes[count++] = (char)(high << 4 | low);
			at += 2;
		} else
			count += (UINT32)WideCharToMultiByte(CP_UTF8, 0, &text[at], 1, bytes + count, 3, NULL, NULL);
	}
	WCHAR decoded[MAX_PATH];
	const int made = count ? MultiByteToWideChar(CP_UTF8, 0, bytes, (int)count, decoded, MAX_PATH - 1) : 0;
	FREE(bytes);
	if (count && !made)
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	decoded[made] = 0;
	const WCHAR *rest = decoded;
	/* file:///C:/x has the path /C:/x. */
	if (!*base && rest[0] == L'/' && rest[1] && rest[2] == L':')
		++rest;
	while (*base && *rest == L'/')
		++rest;
	if ((size_t)lstrlenW(base) + 1 + lstrlenW(rest) >= MAX_PATH)
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	lstrcpyW(path, base);
	if (*base && path[lstrlenW(path) - 1] != L'\\')
		lstrcatW(path, L"\\");
	lstrcatW(path, rest);
	for (WCHAR *at = path; *at; ++at)
		if (*at == L'/')
			*at = L'\\';
	return S_OK;
}

static HRESULT WINAPI reference_from_uri(void *iface, uri_type *uri, void **out)
{
	HSTRING scheme = NULL, escaped = NULL;
	WCHAR base[MAX_PATH] = L"";
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!uri)
		return E_INVALIDARG;
	HRESULT result = uri->lpVtbl->get_SchemeName(uri, &scheme);
	if (SUCCEEDED(result))
		result = uri->lpVtbl->get_Path(uri, &escaped);
	UINT32 length = 0;
	const WCHAR *name = SUCCEEDED(result) ? WindowsGetStringRawBuffer(scheme, &length) : NULL;
	if (SUCCEEDED(result)) {
		if (CompareStringOrdinal(name, (int)length, L"ms-appx", -1, TRUE) == CSTR_EQUAL) {
			UINT32 size = MAX_PATH;
			const LONG error = GetCurrentPackagePath(&size, base);
			if (error)
				result = HRESULT_FROM_WIN32(error);
		} else if (CompareStringOrdinal(name, (int)length, L"file", -1, TRUE) != CSTR_EQUAL)
			result = missing("RandomAccessStreamReference.CreateFromUri for this scheme");
	}
	struct stream_ref *self = NULL;
	if (SUCCEEDED(result) && !(self = new_reference()))
		result = E_OUTOFMEMORY;
	if (SUCCEEDED(result))
		result = path_of_uri(base, escaped, self->path);
	if (FAILED(result) && self)
		release(face_of(&self->base, 0));
	else if (SUCCEEDED(result))
		*out = face_of(&self->base, 0);
	WindowsDeleteString(scheme);
	WindowsDeleteString(escaped);
	return result;
}

TABLE(reference_factory_table, 0, FACTORY);
TABLE(reference_statics_table, 1, INSPECTABLE, (const void *)reference_from_file, (const void *)reference_from_uri,
	(const void *)reference_from_stream);
static const struct face_iid reference_class_iids[] = {{&iid_factory, 0}, {&iid_reference_statics, 1}, {NULL, 0}};
static const struct kind reference_class_kind = {"RandomAccessStreamReference", L"Windows.Storage.Streams.RandomAccessStreamReference",
	reference_class_iids, NULL};
struct object stream_reference_class = {{reference_factory_table.slots, reference_statics_table.slots}, &reference_class_kind, 1};
