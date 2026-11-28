/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Storage.Streams.FileRandomAccessStream: a stream over a file handle. Each stream keeps its own position, and reads and writes
 * go to the file at that position, so the streams cloned from one or taken at a position share the file but not where they are. The
 * file is closed when the last stream over it goes. Reads, writes and flushes run as operations on threads of their own; a stream
 * that was closed answers RO_E_CLOSED. */
#include "winrt.h"

#ifndef RO_E_CLOSED
#define RO_E_CLOSED ((HRESULT)0x80000013L)
#endif

static const IID iid_random_access_stream = {0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
static const IID iid_stream_with_type = {0xcc254827, 0x4b3d, 0x438f, {0x92, 0x32, 0x10, 0xc7, 0x6b, 0xc7, 0xe0, 0x38}};
static const IID iid_content_type_provider = {0x97d098a5, 0x3b99, 0x4de9, {0x88, 0xa5, 0xe1, 0x1d, 0x2f, 0x50, 0xc7, 0x95}};

typedef __x_ABI_CWindows_CStorage_CStreams_CIBuffer buffer_type;

/* The file the streams share. */
struct shared_file {
	LONG refs;
	HANDLE handle;
	/* Held exclusively while the size changes, shared for reads and writes. */
	SRWLOCK lock;
};

static void put_file(struct shared_file *file)
{
	if (file && !InterlockedDecrement(&file->refs)) {
		CloseHandle(file->handle);
		WINRT_FREE(file);
	}
}

struct stream {
	struct winrt_object base;
	struct winrt_interface random, input, output, closable, with_type, type_provider;
	struct shared_file *file;
	/* Guards the position, held through a whole read or write so that they follow one another. */
	SRWLOCK lock;
	UINT64 position;
	BOOL can_read, can_write;
	LONG closed;
	HSTRING content_type;
};

static struct stream *stream_of(void *iface) { return (struct stream *)winrt_object_of(iface); }

static BOOL closed(struct stream *self) { return InterlockedCompareExchange(&self->closed, 0, 0) != 0; }

static void stream_destroy(struct winrt_object *object)
{
	struct stream *self = (struct stream *)object;
	put_file(self->file);
	WindowsDeleteString(self->content_type);
}

static HRESULT last_error(void)
{
	const DWORD error = GetLastError();
	return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

/* The parts a stream answers: all of them, all of them with a content type, or only reading or only writing. */
enum { WHOLE, TYPED, INPUT_PART, OUTPUT_PART };

static HRESULT new_stream(struct shared_file *file, BOOL can_read, BOOL can_write, HSTRING content_type, UINT64 position, int part,
	struct stream **out);

/* IRandomAccessStream. */
static HRESULT STDMETHODCALLTYPE stream_size(void *iface, UINT64 *out)
{
	struct stream *self = stream_of(iface);
	LARGE_INTEGER size;
	if (!out)
		return E_POINTER;
	if (closed(self))
		return RO_E_CLOSED;
	if (!GetFileSizeEx(self->file->handle, &size))
		return last_error();
	*out = (UINT64)size.QuadPart;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_set_size(void *iface, UINT64 size)
{
	struct stream *self = stream_of(iface);
	if (closed(self))
		return RO_E_CLOSED;
	if (!self->can_write)
		return E_ACCESSDENIED;
	if (size > (UINT64)MAXLONGLONG)
		return E_INVALIDARG;
	AcquireSRWLockExclusive(&self->file->lock);
	LARGE_INTEGER at = {.QuadPart = (LONGLONG)size};
	const HRESULT result = SetFilePointerEx(self->file->handle, at, NULL, FILE_BEGIN) && SetEndOfFile(self->file->handle) ? S_OK : last_error();
	ReleaseSRWLockExclusive(&self->file->lock);
	return result;
}


static HRESULT part_at(void *iface, UINT64 position, int part, void **out)
{
	struct stream *self = stream_of(iface), *made;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (closed(self))
		return RO_E_CLOSED;
	if (part == INPUT_PART ? !self->can_read : !self->can_write)
		return E_ACCESSDENIED;
	const HRESULT result = new_stream(self->file, self->can_read, self->can_write, NULL, position, part, &made);
	if (SUCCEEDED(result))
		*out = part == INPUT_PART ? &made->input : &made->output;
	return result;
}

static HRESULT STDMETHODCALLTYPE stream_input_at(void *iface, UINT64 position, void **out) { return part_at(iface, position, INPUT_PART, out); }
static HRESULT STDMETHODCALLTYPE stream_output_at(void *iface, UINT64 position, void **out) { return part_at(iface, position, OUTPUT_PART, out); }

static HRESULT STDMETHODCALLTYPE stream_position(void *iface, UINT64 *out)
{
	struct stream *self = stream_of(iface);
	if (!out)
		return E_POINTER;
	if (closed(self))
		return RO_E_CLOSED;
	AcquireSRWLockShared(&self->lock);
	*out = self->position;
	ReleaseSRWLockShared(&self->lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_seek(void *iface, UINT64 position)
{
	struct stream *self = stream_of(iface);
	if (closed(self))
		return RO_E_CLOSED;
	if (position > (UINT64)MAXLONGLONG)
		return E_INVALIDARG;
	AcquireSRWLockExclusive(&self->lock);
	self->position = position;
	ReleaseSRWLockExclusive(&self->lock);
	return S_OK;
}

/* A clone starts at the beginning of the file. */
static HRESULT STDMETHODCALLTYPE stream_clone(void *iface, void **out)
{
	struct stream *self = stream_of(iface), *made;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (closed(self))
		return RO_E_CLOSED;
	const HRESULT result = new_stream(self->file, self->can_read, self->can_write, NULL, 0, WHOLE, &made);
	if (SUCCEEDED(result))
		*out = &made->random;
	return result;
}

static HRESULT STDMETHODCALLTYPE stream_can_read(void *iface, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = stream_of(iface)->can_read;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_can_write(void *iface, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = stream_of(iface)->can_write;
	return S_OK;
}

/* Reads and writes, with the stream and the buffer held until they have run. */
struct transfer {
	struct stream *stream;
	buffer_type *buffer;
	UINT32 count;
};

static void free_transfer(void *context)
{
	struct transfer *transfer = context;
	winrt_put(transfer->buffer);
	winrt_release(&transfer->stream->closable);
	WINRT_FREE(transfer);
}

static HRESULT start_transfer(struct stream *self, buffer_type *buffer, UINT32 count, const IID *operation, enum winrt_result kind,
	BOOL progress, winrt_work work, void **out)
{
	struct transfer *transfer = WINRT_ALLOC(sizeof(*transfer));
	if (!transfer)
		return E_OUTOFMEMORY;
	winrt_add_ref(&self->closable);
	if (buffer)
		buffer->lpVtbl->AddRef(buffer);
	transfer->stream = self;
	transfer->buffer = buffer;
	transfer->count = count;
	return winrt_async_start(operation, progress, kind, work, transfer, free_transfer, out);
}

/* One positioned transfer of the file, at most count bytes. */
static HRESULT file_io(struct shared_file *file, BOOL write, UINT64 position, BYTE *bytes, UINT32 count, UINT32 *done)
{
	OVERLAPPED at = {.Offset = (DWORD)position, .OffsetHigh = (DWORD)(position >> 32)};
	DWORD moved = 0;
	*done = 0;
	AcquireSRWLockShared(&file->lock);
	const BOOL ok = write ? WriteFile(file->handle, bytes, count, &moved, &at) : ReadFile(file->handle, bytes, count, &moved, &at);
	const DWORD error = ok ? 0 : GetLastError();
	ReleaseSRWLockShared(&file->lock);
	/* Reading at or past the end gives nothing, which is not an error. */
	if (!ok && !(error == ERROR_HANDLE_EOF && !write))
		return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
	*done = moved;
	return S_OK;
}

/* Reads up to count bytes (no more than the buffer holds) at the stream's position into the buffer, which is the result. */
static HRESULT read_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct transfer *transfer = context;
	struct stream *self = transfer->stream;
	BYTE *bytes;
	UINT32 length, capacity, done = 0;
	HRESULT outcome = closed(self) ? RO_E_CLOSED : winrt_buffer_bytes(transfer->buffer, &bytes, &length);
	if (SUCCEEDED(outcome))
		outcome = transfer->buffer->lpVtbl->get_Capacity(transfer->buffer, &capacity);
	if (FAILED(outcome))
		return outcome;
	const UINT32 count = transfer->count < capacity ? transfer->count : capacity;
	AcquireSRWLockExclusive(&self->lock);
	outcome = file_io(self->file, FALSE, self->position, bytes, count, &done);
	if (SUCCEEDED(outcome))
		self->position += done;
	ReleaseSRWLockExclusive(&self->lock);
	if (SUCCEEDED(outcome))
		outcome = transfer->buffer->lpVtbl->put_Length(transfer->buffer, done);
	if (SUCCEEDED(outcome)) {
		transfer->buffer->lpVtbl->AddRef(transfer->buffer);
		result->object = transfer->buffer;
	}
	return outcome;
}

static HRESULT STDMETHODCALLTYPE stream_read(void *iface, buffer_type *buffer, UINT32 count,
	__x_ABI_CWindows_CStorage_CStreams_CInputStreamOptions options, void **out)
{
	struct stream *self = stream_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!buffer)
		return E_INVALIDARG;
	if (closed(self))
		return RO_E_CLOSED;
	if (!self->can_read)
		return E_ACCESSDENIED;
	return start_transfer(self, buffer, count, &IID___FIAsyncOperationWithProgress_2_Windows__CStorage__CStreams__CIBuffer_UINT32,
		WINRT_RESULT_OBJECT, TRUE, read_work, out);
}

/* Writes the buffer's bytes at the stream's position; the result is how many were written. */
static HRESULT write_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct transfer *transfer = context;
	struct stream *self = transfer->stream;
	BYTE *bytes;
	UINT32 length, done = 0;
	HRESULT outcome = closed(self) ? RO_E_CLOSED : winrt_buffer_bytes(transfer->buffer, &bytes, &length);
	if (FAILED(outcome))
		return outcome;
	AcquireSRWLockExclusive(&self->lock);
	outcome = file_io(self->file, TRUE, self->position, bytes, length, &done);
	if (SUCCEEDED(outcome))
		self->position += done;
	ReleaseSRWLockExclusive(&self->lock);
	result->uint32 = done;
	return outcome;
}

static HRESULT STDMETHODCALLTYPE stream_write(void *iface, buffer_type *buffer, void **out)
{
	struct stream *self = stream_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!buffer)
		return E_INVALIDARG;
	if (closed(self))
		return RO_E_CLOSED;
	if (!self->can_write)
		return E_ACCESSDENIED;
	return start_transfer(self, buffer, 0, &IID___FIAsyncOperationWithProgress_2_UINT32_UINT32, WINRT_RESULT_UINT32, TRUE, write_work, out);
}

static HRESULT flush_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct transfer *transfer = context;
	if (closed(transfer->stream))
		return RO_E_CLOSED;
	if (!FlushFileBuffers(transfer->stream->file->handle))
		return last_error();
	result->boolean = TRUE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_flush(void *iface, void **out)
{
	struct stream *self = stream_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (closed(self))
		return RO_E_CLOSED;
	if (!self->can_write)
		return E_ACCESSDENIED;
	return start_transfer(self, NULL, 0, &IID___FIAsyncOperation_1_boolean, WINRT_RESULT_BOOLEAN, FALSE, flush_work, out);
}

/* Closing a stream ends it, not the file the others hold. */
static HRESULT STDMETHODCALLTYPE stream_close(void *iface)
{
	InterlockedExchange(&stream_of(iface)->closed, 1);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_content_type(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(stream_of(iface)->content_type, out);
}

static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *size, *set_size, *input_at, *output_at, *position, *seek, *clone, *can_read,
		*can_write;
} random_table = {WINRT_INSPECTABLE, stream_size, stream_set_size, stream_input_at, stream_output_at, stream_position, stream_seek,
	stream_clone, stream_can_read, stream_can_write};
static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *read;
} input_table = {WINRT_INSPECTABLE, stream_read};
static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *write, *flush;
} output_table = {WINRT_INSPECTABLE, stream_write, stream_flush};
static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *close;
} closable_table = {WINRT_INSPECTABLE, stream_close};
static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust;
} with_type_table = {WINRT_INSPECTABLE};
static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *content_type;
} type_provider_table = {WINRT_INSPECTABLE, stream_content_type};

#define STREAM_ENTRY(field, iid) WINRT_ENTRY(struct stream, field, iid)
static const struct winrt_entry whole_entries[] = {STREAM_ENTRY(random, iid_random_access_stream),
	STREAM_ENTRY(input, IID___x_ABI_CWindows_CStorage_CStreams_CIInputStream),
	STREAM_ENTRY(output, IID___x_ABI_CWindows_CStorage_CStreams_CIOutputStream),
	STREAM_ENTRY(closable, IID___x_ABI_CWindows_CFoundation_CIClosable), WINRT_END};
static const struct winrt_entry typed_entries[] = {STREAM_ENTRY(with_type, iid_stream_with_type), STREAM_ENTRY(random, iid_random_access_stream),
	STREAM_ENTRY(input, IID___x_ABI_CWindows_CStorage_CStreams_CIInputStream),
	STREAM_ENTRY(output, IID___x_ABI_CWindows_CStorage_CStreams_CIOutputStream),
	STREAM_ENTRY(closable, IID___x_ABI_CWindows_CFoundation_CIClosable), STREAM_ENTRY(type_provider, iid_content_type_provider), WINRT_END};
static const struct winrt_entry input_entries[] = {STREAM_ENTRY(input, IID___x_ABI_CWindows_CStorage_CStreams_CIInputStream),
	STREAM_ENTRY(closable, IID___x_ABI_CWindows_CFoundation_CIClosable), WINRT_END};
static const struct winrt_entry output_entries[] = {STREAM_ENTRY(output, IID___x_ABI_CWindows_CStorage_CStreams_CIOutputStream),
	STREAM_ENTRY(closable, IID___x_ABI_CWindows_CFoundation_CIClosable), WINRT_END};
static const struct winrt_class whole_class = {L"Windows.Storage.Streams.FileRandomAccessStream", whole_entries, stream_destroy};
static const struct winrt_class typed_class = {L"Windows.Storage.Streams.FileRandomAccessStream", typed_entries, stream_destroy};
static const struct winrt_class input_class = {L"Windows.Storage.Streams.FileInputStream", input_entries, stream_destroy};
static const struct winrt_class output_class = {L"Windows.Storage.Streams.FileOutputStream", output_entries, stream_destroy};

static HRESULT new_stream(struct shared_file *file, BOOL can_read, BOOL can_write, HSTRING content_type, UINT64 position, int part,
	struct stream **out)
{
	const struct winrt_class *kind = part == INPUT_PART ? &input_class : part == OUTPUT_PART ? &output_class :
		part == TYPED ? &typed_class : &whole_class;
	*out = NULL;
	struct stream *self = winrt_new(kind, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->random.table = &random_table;
	self->input.table = &input_table;
	self->output.table = &output_table;
	self->closable.table = &closable_table;
	self->with_type.table = &with_type_table;
	self->type_provider.table = &type_provider_table;
	InitializeSRWLock(&self->lock);
	self->position = position;
	self->can_read = can_read;
	self->can_write = can_write;
	const HRESULT result = WindowsDuplicateString(content_type, &self->content_type);
	if (FAILED(result)) {
		winrt_release(&self->closable);
		return result;
	}
	InterlockedIncrement(&file->refs);
	self->file = file;
	*out = self;
	return S_OK;
}

HRESULT winrt_file_stream(HANDLE handle, BOOL can_read, BOOL can_write, const WCHAR *content_type, void **out)
{
	HSTRING type = NULL;
	struct stream *self = NULL;
	if (!out) {
		CloseHandle(handle);
		return E_POINTER;
	}
	*out = NULL;
	struct shared_file *file = WINRT_ALLOC(sizeof(*file));
	HRESULT result = file ? S_OK : E_OUTOFMEMORY;
	if (SUCCEEDED(result) && content_type)
		result = WindowsCreateString(content_type, (UINT32)lstrlenW(content_type), &type);
	if (FAILED(result)) {
		WINRT_FREE(file);
		CloseHandle(handle);
		return result;
	}
	file->handle = handle;
	InitializeSRWLock(&file->lock);
	result = new_stream(file, can_read, can_write, type, 0, content_type ? TYPED : WHOLE, &self);
	WindowsDeleteString(type);
	if (FAILED(result)) {
		CloseHandle(handle);
		WINRT_FREE(file);
		return result;
	}
	*out = content_type ? &self->with_type : &self->random;
	return S_OK;
}
