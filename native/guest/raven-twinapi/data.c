/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Storage.Streams.DataReader and DataWriter: values read from and written to bytes in a byte order (big-endian unless set) and
 * strings in an encoding (UTF-8 unless set).
 *
 * - A writer keeps what is written until StoreAsync writes it to its output stream, or DetachBuffer hands it out as a buffer.
 *   WriteString and MeasureString answer the bytes the string takes.
 * - A reader reads what LoadAsync brought from its input stream, or the bytes of the buffer it was made from. Reading past what it holds
 *   answers E_BOUNDS; ReadString counts code units of its encoding (bytes for UTF-8, characters for UTF-16).
 * - Closing either closes its stream, unless DetachStream took it first.
 *
 * StoreAsync, FlushAsync and LoadAsync run on threads of their own through raven-winrt and wait there for the stream's own operation.
 * A reader or writer is used by one caller at a time, as on Windows; a lock still keeps its bytes whole. */
#include "object.h"
#include "../raven-winrt/winrt.h"

static const IID iid_data_reader = {0xe2b50029, 0xb4c1, 0x4314, {0xa4, 0xb8, 0xfb, 0x81, 0x3a, 0x2f, 0x27, 0x5e}};
static const IID iid_data_reader_factory = {0xd7527847, 0x57da, 0x4e15, {0x91, 0x4c, 0x06, 0x80, 0x66, 0x99, 0xa0, 0x98}};
static const IID iid_data_reader_statics = {0x11fcbfc8, 0xf93a, 0x471b, {0xb1, 0x21, 0xf3, 0x79, 0xe3, 0x49, 0x31, 0x3c}};
static const IID iid_data_writer = {0x64b89265, 0xd341, 0x4922, {0xb3, 0x8a, 0xdd, 0x4a, 0xf8, 0x80, 0x8c, 0x4e}};
static const IID iid_data_writer_factory = {0x338c67c2, 0x8b84, 0x4c2b, {0x9c, 0x50, 0x7b, 0x87, 0x67, 0x84, 0x7a, 0x1f}};
static const IID iid_closable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
/* IAsyncOperation<UInt32>: DataWriterStoreOperation and DataReaderLoadOperation answer it. */
static const IID iid_count_operation = {0xef60385f, 0xbe78, 0x584b, {0xaa, 0xef, 0x78, 0x29, 0xad, 0xa2, 0xb0, 0xde}};

typedef __x_ABI_CWindows_CStorage_CStreams_CIBuffer buffer_type;
typedef __x_ABI_CWindows_CStorage_CStreams_CIInputStream input_type;
typedef __x_ABI_CWindows_CStorage_CStreams_CIOutputStream output_type;

/* UnicodeEncoding and ByteOrder. */
#define ENCODING_UTF8 0
#define ENCODING_UTF16LE 1
#define ENCODING_UTF16BE 2
#define LITTLE_ENDIAN 0
#define BIG_ENDIAN 1

#define CLOSED ((HRESULT)0x80000013L)

/* Bytes that grow at the end and are taken from the front. */
struct bytes {
	BYTE *data;
	UINT32 start, end, capacity;
};

static UINT32 held(const struct bytes *bytes) { return bytes->end - bytes->start; }

static HRESULT room(struct bytes *bytes, UINT32 more)
{
	if (more > 0xffffffffu - held(bytes))
		return E_OUTOFMEMORY;
	if (bytes->end + more <= bytes->capacity)
		return S_OK;
	/* What was taken from the front goes first, then the block grows. */
	if (bytes->start) {
		memmove(bytes->data, bytes->data + bytes->start, held(bytes));
		bytes->end -= bytes->start;
		bytes->start = 0;
		if (bytes->end + more <= bytes->capacity)
			return S_OK;
	}
	UINT32 capacity = bytes->capacity ? bytes->capacity : 64;
	while (capacity < bytes->end + more)
		capacity = capacity > 0x7fffffff ? bytes->end + more : capacity * 2;
	BYTE *data = bytes->data ? HeapReAlloc(GetProcessHeap(), 0, bytes->data, capacity) : ALLOC(capacity);
	if (!data)
		return E_OUTOFMEMORY;
	bytes->data = data;
	bytes->capacity = capacity;
	return S_OK;
}

static HRESULT append(struct bytes *bytes, const void *data, UINT32 length)
{
	const HRESULT result = room(bytes, length);
	if (SUCCEEDED(result) && length) {
		memcpy(bytes->data + bytes->end, data, length);
		bytes->end += length;
	}
	return result;
}

static void clear(struct bytes *bytes) { bytes->start = bytes->end = 0; }

/* A number of size bytes in the byte order, to or from its value in memory. */
static void order(void *value, UINT32 size, INT32 byte_order)
{
	if (byte_order != BIG_ENDIAN)
		return;
	BYTE *at = value;
	for (UINT32 low = 0, high = size - 1; low < high; ++low, --high) {
		const BYTE swap = at[low];
		at[low] = at[high];
		at[high] = swap;
	}
}

/* What both keep: the bytes, the stream, how values are laid out. */
struct data {
	struct object base;
	SRWLOCK lock;
	struct bytes bytes;
	IUnknown *stream;
	INT32 encoding, byte_order, options;
};

static struct data *data_of(void *iface) { return (struct data *)owner(iface); }

static void destroy_data(struct object *base)
{
	struct data *self = (struct data *)base;
	release(self->stream);
	FREE(self->bytes.data);
}

static HRESULT WINAPI data_encoding(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = data_of(iface)->encoding;
	return S_OK;
}

static HRESULT WINAPI data_set_encoding(void *iface, INT32 encoding)
{
	if (encoding < ENCODING_UTF8 || encoding > ENCODING_UTF16BE)
		return E_INVALIDARG;
	data_of(iface)->encoding = encoding;
	return S_OK;
}

static HRESULT WINAPI data_byte_order(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = data_of(iface)->byte_order;
	return S_OK;
}

static HRESULT WINAPI data_set_byte_order(void *iface, INT32 byte_order)
{
	if (byte_order != LITTLE_ENDIAN && byte_order != BIG_ENDIAN)
		return E_INVALIDARG;
	data_of(iface)->byte_order = byte_order;
	return S_OK;
}

static HRESULT WINAPI data_held(void *iface, UINT32 *out)
{
	struct data *self = data_of(iface);
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	*out = held(&self->bytes);
	ReleaseSRWLockShared(&self->lock);
	return S_OK;
}

/* The bytes held, handed out as a buffer; the reader or writer is left empty. */
static HRESULT WINAPI data_detach_buffer(void *iface, void **out)
{
	struct data *self = data_of(iface);
	if (!out)
		return E_POINTER;
	AcquireSRWLockExclusive(&self->lock);
	const UINT32 length = held(&self->bytes);
	const HRESULT result = winrt_buffer(self->bytes.data ? self->bytes.data + self->bytes.start : NULL, length, length, (buffer_type **)out);
	if (SUCCEEDED(result))
		clear(&self->bytes);
	ReleaseSRWLockExclusive(&self->lock);
	return result;
}

static HRESULT WINAPI data_detach_stream(void *iface, void **out)
{
	struct data *self = data_of(iface);
	if (!out)
		return E_POINTER;
	*out = InterlockedExchangePointer((void **)&self->stream, NULL);
	return S_OK;
}

static HRESULT WINAPI data_close(void *iface)
{
	IUnknown *stream = InterlockedExchangePointer((void **)&data_of(iface)->stream, NULL), *closable = NULL;
	if (stream && SUCCEEDED(stream->lpVtbl->QueryInterface(stream, &iid_closable, (void **)&closable))) {
		((HRESULT (WINAPI *)(void *))method(closable, 6))(closable);
		release(closable);
	}
	release(stream);
	return S_OK;
}

/* The stream, with a reference, for an operation to use on its thread. */
static IUnknown *stream_of(struct data *self)
{
	AcquireSRWLockShared(&self->lock);
	IUnknown *stream = self->stream;
	reference(stream);
	ReleaseSRWLockShared(&self->lock);
	return stream;
}

/* An operation over the reader or writer, which it holds until it has run. */
struct job {
	struct data *data;
	UINT32 count;
};

static void free_job(void *context)
{
	struct job *job = context;
	release(face_of(&job->data->base, 0));
	FREE(job);
}

static HRESULT start(struct data *self, UINT32 count, const IID *operation, enum winrt_result kind, winrt_work work, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!self->stream)
		return E_ILLEGAL_METHOD_CALL;
	struct job *job = ALLOC(sizeof *job);
	if (!job)
		return E_OUTOFMEMORY;
	reference(face_of(&self->base, 0));
	job->data = self;
	job->count = count;
	return winrt_async_start(operation, FALSE, kind, work, job, free_job, out);
}

/* Windows.Storage.Streams.DataWriter. */
static HRESULT put(void *iface, const void *value, UINT32 size)
{
	struct data *self = data_of(iface);
	AcquireSRWLockExclusive(&self->lock);
	const HRESULT result = append(&self->bytes, value, size);
	ReleaseSRWLockExclusive(&self->lock);
	return result;
}

static HRESULT put_number(void *iface, void *value, UINT32 size)
{
	order(value, size, data_of(iface)->byte_order);
	return put(iface, value, size);
}

static HRESULT WINAPI writer_byte(void *iface, BYTE value) { return put(iface, &value, 1); }

static HRESULT WINAPI writer_bytes(void *iface, UINT32 count, const BYTE *value)
{
	if (count && !value)
		return E_POINTER;
	return put(iface, value, count);
}

static HRESULT WINAPI writer_buffer_range(void *iface, buffer_type *buffer, UINT32 start, UINT32 count)
{
	BYTE *bytes;
	UINT32 length;
	if (!buffer)
		return E_INVALIDARG;
	const HRESULT result = winrt_buffer_bytes(buffer, &bytes, &length);
	if (FAILED(result))
		return result;
	if (start > length || count > length - start)
		return E_BOUNDS;
	return put(iface, bytes + start, count);
}

static HRESULT WINAPI writer_buffer(void *iface, buffer_type *buffer)
{
	UINT32 length = 0;
	if (!buffer)
		return E_INVALIDARG;
	const HRESULT result = buffer->lpVtbl->get_Length(buffer, &length);
	return FAILED(result) ? result : writer_buffer_range(iface, buffer, 0, length);
}

static HRESULT WINAPI writer_boolean(void *iface, boolean value)
{
	const BYTE byte = value ? 1 : 0;
	return put(iface, &byte, 1);
}

/* A GUID is its three numbers in the byte order, then its eight bytes as they are. */
static HRESULT WINAPI writer_guid(void *iface, GUID value)
{
	order(&value.Data1, 4, data_of(iface)->byte_order);
	order(&value.Data2, 2, data_of(iface)->byte_order);
	order(&value.Data3, 2, data_of(iface)->byte_order);
	return put(iface, &value, sizeof value);
}

static HRESULT WINAPI writer_int16(void *iface, INT16 value) { return put_number(iface, &value, 2); }
static HRESULT WINAPI writer_int32(void *iface, INT32 value) { return put_number(iface, &value, 4); }
static HRESULT WINAPI writer_int64(void *iface, INT64 value) { return put_number(iface, &value, 8); }
static HRESULT WINAPI writer_single(void *iface, float value) { return put_number(iface, &value, 4); }
static HRESULT WINAPI writer_double(void *iface, double value) { return put_number(iface, &value, 8); }

/* The bytes a string takes in the encoding, into a block the caller frees when bytes is given. */
static HRESULT encode(HSTRING string, INT32 encoding, BYTE **bytes, UINT32 *size)
{
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(string, &length);
	if (bytes)
		*bytes = NULL;
	if (encoding == ENCODING_UTF8) {
		const int needed = length ? WideCharToMultiByte(CP_UTF8, 0, text, (int)length, NULL, 0, NULL, NULL) : 0;
		if (length && !needed)
			return HRESULT_FROM_WIN32(GetLastError());
		*size = (UINT32)needed;
		if (!bytes || !needed)
			return S_OK;
		if (!(*bytes = ALLOC((size_t)needed)))
			return E_OUTOFMEMORY;
		WideCharToMultiByte(CP_UTF8, 0, text, (int)length, (char *)*bytes, needed, NULL, NULL);
		return S_OK;
	}
	if (length > 0x7fffffff)
		return E_OUTOFMEMORY;
	*size = length * 2;
	if (!bytes || !length)
		return S_OK;
	if (!(*bytes = ALLOC((size_t)length * 2)))
		return E_OUTOFMEMORY;
	for (UINT32 at = 0; at < length; ++at) {
		(*bytes)[2 * at + (encoding == ENCODING_UTF16LE ? 0 : 1)] = (BYTE)text[at];
		(*bytes)[2 * at + (encoding == ENCODING_UTF16LE ? 1 : 0)] = (BYTE)(text[at] >> 8);
	}
	return S_OK;
}

static HRESULT WINAPI writer_string(void *iface, HSTRING value, UINT32 *out)
{
	BYTE *bytes;
	UINT32 size = 0;
	if (!out)
		return E_POINTER;
	HRESULT result = encode(value, data_of(iface)->encoding, &bytes, &size);
	if (SUCCEEDED(result))
		result = put(iface, bytes, size);
	FREE(bytes);
	*out = SUCCEEDED(result) ? size : 0;
	return result;
}

static HRESULT WINAPI writer_measure(void *iface, HSTRING value, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	return encode(value, data_of(iface)->encoding, NULL, out);
}

/* Writes what is held to the stream, waiting there for the stream's write; the result is how many bytes went. */
static HRESULT run_store(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct job *job = context;
	struct data *self = job->data;
	IUnknown *stream = stream_of(self);
	buffer_type *buffer = NULL;
	if (!stream)
		return CLOSED;
	AcquireSRWLockExclusive(&self->lock);
	const UINT32 length = held(&self->bytes);
	HRESULT outcome = winrt_buffer(self->bytes.data ? self->bytes.data + self->bytes.start : NULL, length, length, &buffer);
	if (SUCCEEDED(outcome))
		clear(&self->bytes);
	ReleaseSRWLockExclusive(&self->lock);
	void *write = NULL;
	UINT32 written = 0;
	if (SUCCEEDED(outcome))
		outcome = ((output_type *)stream)->lpVtbl->WriteAsync((output_type *)stream, buffer, (void *)&write);
	if (SUCCEEDED(outcome))
		outcome = winrt_wait(write, TRUE, &written);
	release(write);
	winrt_put(buffer);
	release(stream);
	result->uint32 = written;
	return outcome;
}

static HRESULT WINAPI writer_store(void *iface, void **out)
{
	return start(data_of(iface), 0, &iid_count_operation, WINRT_RESULT_UINT32, run_store, out);
}

static HRESULT run_flush(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct job *job = context;
	IUnknown *stream = stream_of(job->data);
	void *flush = NULL;
	boolean flushed = FALSE;
	if (!stream)
		return CLOSED;
	HRESULT outcome = ((output_type *)stream)->lpVtbl->FlushAsync((output_type *)stream, (void *)&flush);
	if (SUCCEEDED(outcome))
		outcome = winrt_wait(flush, FALSE, &flushed);
	release(flush);
	release(stream);
	result->boolean = flushed;
	return outcome;
}

static HRESULT WINAPI writer_flush(void *iface, void **out)
{
	return start(data_of(iface), 0, &IID___FIAsyncOperation_1_boolean, WINRT_RESULT_BOOLEAN, run_flush, out);
}

/* Windows.Storage.Streams.DataReader. */
static HRESULT WINAPI reader_options(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = data_of(iface)->options;
	return S_OK;
}

static HRESULT WINAPI reader_set_options(void *iface, INT32 options)
{
	data_of(iface)->options = options;
	return S_OK;
}

/* Takes size bytes from the front, or answers E_BOUNDS and takes nothing. */
static HRESULT take(void *iface, void *into, UINT32 size)
{
	struct data *self = data_of(iface);
	HRESULT result = S_OK;
	AcquireSRWLockExclusive(&self->lock);
	if (held(&self->bytes) < size)
		result = E_BOUNDS;
	else if (size) {
		if (into)
			memcpy(into, self->bytes.data + self->bytes.start, size);
		self->bytes.start += size;
	}
	ReleaseSRWLockExclusive(&self->lock);
	return result;
}

static HRESULT take_number(void *iface, void *into, UINT32 size)
{
	if (!into)
		return E_POINTER;
	const HRESULT result = take(iface, into, size);
	if (SUCCEEDED(result))
		order(into, size, data_of(iface)->byte_order);
	return result;
}

static HRESULT WINAPI reader_byte(void *iface, BYTE *out) { return out ? take(iface, out, 1) : E_POINTER; }

static HRESULT WINAPI reader_bytes(void *iface, UINT32 count, BYTE *out)
{
	if (count && !out)
		return E_POINTER;
	return take(iface, out, count);
}

static HRESULT WINAPI reader_buffer(void *iface, UINT32 length, void **out)
{
	struct data *self = data_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	AcquireSRWLockExclusive(&self->lock);
	HRESULT result = held(&self->bytes) < length ? E_BOUNDS :
		winrt_buffer(self->bytes.data ? self->bytes.data + self->bytes.start : NULL, length, length, (buffer_type **)out);
	if (SUCCEEDED(result))
		self->bytes.start += length;
	ReleaseSRWLockExclusive(&self->lock);
	return result;
}

static HRESULT WINAPI reader_boolean(void *iface, boolean *out)
{
	BYTE byte;
	if (!out)
		return E_POINTER;
	const HRESULT result = take(iface, &byte, 1);
	if (SUCCEEDED(result))
		*out = byte != 0;
	return result;
}

static HRESULT WINAPI reader_guid(void *iface, GUID *out)
{
	if (!out)
		return E_POINTER;
	const HRESULT result = take(iface, out, sizeof *out);
	if (SUCCEEDED(result)) {
		order(&out->Data1, 4, data_of(iface)->byte_order);
		order(&out->Data2, 2, data_of(iface)->byte_order);
		order(&out->Data3, 2, data_of(iface)->byte_order);
	}
	return result;
}

static HRESULT WINAPI reader_two(void *iface, INT16 *out) { return take_number(iface, out, 2); }
static HRESULT WINAPI reader_four(void *iface, INT32 *out) { return take_number(iface, out, 4); }
static HRESULT WINAPI reader_eight(void *iface, INT64 *out) { return take_number(iface, out, 8); }

/* count code units of the encoding: bytes for UTF-8, characters for UTF-16. */
static HRESULT WINAPI reader_string(void *iface, UINT32 count, HSTRING *out)
{
	struct data *self = data_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	const INT32 encoding = self->encoding;
	if (encoding != ENCODING_UTF8 && count > 0x7fffffff)
		return E_BOUNDS;
	const UINT32 size = encoding == ENCODING_UTF8 ? count : count * 2;
	BYTE *bytes = size ? ALLOC(size) : NULL;
	if (size && !bytes)
		return E_OUTOFMEMORY;
	HRESULT result = take(iface, bytes, size);
	WCHAR *text = NULL;
	int length = 0;
	if (SUCCEEDED(result) && size) {
		length = encoding == ENCODING_UTF8 ? MultiByteToWideChar(CP_UTF8, 0, (const char *)bytes, (int)size, NULL, 0) : (int)count;
		if (!length || !(text = ALLOC((size_t)length * sizeof *text)))
			result = length ? E_OUTOFMEMORY : HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
	}
	if (SUCCEEDED(result) && size) {
		if (encoding == ENCODING_UTF8)
			MultiByteToWideChar(CP_UTF8, 0, (const char *)bytes, (int)size, text, length);
		else
			for (UINT32 at = 0; at < count; ++at)
				text[at] = encoding == ENCODING_UTF16LE ? (WCHAR)(bytes[2 * at] | bytes[2 * at + 1] << 8) :
					(WCHAR)(bytes[2 * at] << 8 | bytes[2 * at + 1]);
	}
	if (SUCCEEDED(result))
		result = WindowsCreateString(text, (UINT32)length, out);
	FREE(text);
	FREE(bytes);
	return result;
}

/* Reads up to count bytes from the stream into what is held, as one read of the stream gives them; the result is how many came. */
static HRESULT run_load(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct job *job = context;
	struct data *self = job->data;
	IUnknown *stream = stream_of(self);
	buffer_type *buffer = NULL, *filled = NULL;
	void *read = NULL;
	if (!stream)
		return CLOSED;
	HRESULT outcome = winrt_buffer(NULL, 0, job->count, &buffer);
	if (SUCCEEDED(outcome))
		outcome = ((input_type *)stream)->lpVtbl->ReadAsync((input_type *)stream, buffer, job->count, self->options, (void *)&read);
	if (SUCCEEDED(outcome))
		outcome = winrt_wait(read, TRUE, &filled);
	BYTE *bytes;
	UINT32 length = 0;
	if (SUCCEEDED(outcome))
		outcome = winrt_buffer_bytes(filled, &bytes, &length);
	if (SUCCEEDED(outcome)) {
		AcquireSRWLockExclusive(&self->lock);
		outcome = append(&self->bytes, bytes, length);
		ReleaseSRWLockExclusive(&self->lock);
	}
	winrt_put(filled);
	release(read);
	winrt_put(buffer);
	release(stream);
	result->uint32 = SUCCEEDED(outcome) ? length : 0;
	return outcome;
}

static HRESULT WINAPI reader_load(void *iface, UINT32 count, void **out)
{
	return start(data_of(iface), count, &iid_count_operation, WINRT_RESULT_UINT32, run_load, out);
}

TABLE(reader_table, 0, INSPECTABLE, (const void *)data_held, (const void *)data_encoding, (const void *)data_set_encoding,
	(const void *)data_byte_order, (const void *)data_set_byte_order, (const void *)reader_options, (const void *)reader_set_options,
	(const void *)reader_byte, (const void *)reader_bytes, (const void *)reader_buffer, (const void *)reader_boolean,
	(const void *)reader_guid, (const void *)reader_two, (const void *)reader_four, (const void *)reader_eight, (const void *)reader_two,
	(const void *)reader_four, (const void *)reader_eight, (const void *)reader_four, (const void *)reader_eight, (const void *)reader_string,
	(const void *)reader_eight, (const void *)reader_eight, (const void *)reader_load, (const void *)data_detach_buffer,
	(const void *)data_detach_stream);
TABLE(writer_table, 0, INSPECTABLE, (const void *)data_held, (const void *)data_encoding, (const void *)data_set_encoding,
	(const void *)data_byte_order, (const void *)data_set_byte_order, (const void *)writer_byte, (const void *)writer_bytes,
	(const void *)writer_buffer, (const void *)writer_buffer_range, (const void *)writer_boolean, (const void *)writer_guid,
	(const void *)writer_int16, (const void *)writer_int32, (const void *)writer_int64, (const void *)writer_int16,
	(const void *)writer_int32, (const void *)writer_int64, (const void *)writer_single, (const void *)writer_double,
	(const void *)writer_int64, (const void *)writer_int64, (const void *)writer_string, (const void *)writer_measure,
	(const void *)writer_store, (const void *)writer_flush, (const void *)data_detach_buffer, (const void *)data_detach_stream);
TABLE(data_closable_table, 1, INSPECTABLE, (const void *)data_close);
static const struct face_iid reader_iids[] = {{&iid_data_reader, 0}, {&iid_closable, 1}, {NULL, 0}};
static const struct face_iid writer_iids[] = {{&iid_data_writer, 0}, {&iid_closable, 1}, {NULL, 0}};
static const struct kind reader_kind = {"DataReader", L"Windows.Storage.Streams.DataReader", reader_iids, destroy_data};
static const struct kind writer_kind = {"DataWriter", L"Windows.Storage.Streams.DataWriter", writer_iids, destroy_data};

/* A reader over a stream (with stream_iid the interface it must answer) or a writer, starting with the bytes of a buffer. */
static HRESULT make_data(BOOL reader, IUnknown *stream, const IID *stream_iid, buffer_type *buffer, void **out)
{
	static const void *const *const reader_tables[] = {reader_table.slots, data_closable_table.slots};
	static const void *const *const writer_tables[] = {writer_table.slots, data_closable_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	IUnknown *kept = NULL;
	if (stream && FAILED(stream->lpVtbl->QueryInterface(stream, stream_iid, (void **)&kept)))
		return E_INVALIDARG;
	struct data *self = make(sizeof *self, reader ? &reader_kind : &writer_kind, reader ? reader_tables : writer_tables, 2);
	if (!self) {
		release(kept);
		return E_OUTOFMEMORY;
	}
	InitializeSRWLock(&self->lock);
	self->stream = kept;
	self->encoding = ENCODING_UTF8;
	self->byte_order = BIG_ENDIAN;
	HRESULT result = S_OK;
	if (buffer) {
		BYTE *bytes;
		UINT32 length;
		if (SUCCEEDED(result = winrt_buffer_bytes(buffer, &bytes, &length)))
			result = append(&self->bytes, bytes, length);
	}
	if (FAILED(result)) {
		release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI reader_create(void *iface, IUnknown *stream, void **out)
{
	if (!stream) {
		answer_null(out);
		return E_INVALIDARG;
	}
	return make_data(TRUE, stream, &IID___x_ABI_CWindows_CStorage_CStreams_CIInputStream, NULL, out);
}

static HRESULT WINAPI reader_from_buffer(void *iface, buffer_type *buffer, void **out)
{
	if (!buffer) {
		answer_null(out);
		return E_INVALIDARG;
	}
	return make_data(TRUE, NULL, NULL, buffer, out);
}

static HRESULT WINAPI writer_create(void *iface, IUnknown *stream, void **out)
{
	if (!stream) {
		answer_null(out);
		return E_INVALIDARG;
	}
	return make_data(FALSE, stream, &IID___x_ABI_CWindows_CStorage_CStreams_CIOutputStream, NULL, out);
}

/* new DataWriter(): a writer with no stream, whose bytes DetachBuffer hands out. */
static HRESULT WINAPI writer_activate(void *iface, IInspectable **out) { return make_data(FALSE, NULL, NULL, NULL, (void **)out); }

TABLE(reader_factory_table, 0, FACTORY);
TABLE(reader_create_table, 1, INSPECTABLE, (const void *)reader_create);
TABLE(reader_statics_table, 2, INSPECTABLE, (const void *)reader_from_buffer);
static const struct face_iid reader_class_iids[] = {{&iid_factory, 0}, {&iid_data_reader_factory, 1}, {&iid_data_reader_statics, 2},
	{NULL, 0}};
static const struct kind reader_class_kind = {"DataReader", L"Windows.Storage.Streams.DataReader", reader_class_iids, NULL};
struct object data_reader_class = {{reader_factory_table.slots, reader_create_table.slots, reader_statics_table.slots}, &reader_class_kind, 1};

TABLE(writer_factory_table, 0, INSPECTABLE, (const void *)writer_activate);
TABLE(writer_create_table, 1, INSPECTABLE, (const void *)writer_create);
static const struct face_iid writer_class_iids[] = {{&iid_factory, 0}, {&iid_data_writer_factory, 1}, {NULL, 0}};
static const struct kind writer_class_kind = {"DataWriter", L"Windows.Storage.Streams.DataWriter", writer_class_iids, NULL};
struct object data_writer_class = {{writer_factory_table.slots, writer_create_table.slots}, &writer_class_kind, 1};
