/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The content of a response (Windows' HttpStreamContent): the body as it arrives from the exchange, kept as it is read so that it can
 * be read again, whole as a buffer or a string, or in pieces as a stream. A string is decoded as the charset of Content-Type says,
 * UTF-8 otherwise, after any byte order mark. Closing the content ends the exchange; reads after it fail with RO_E_CLOSED. */
#include "web.h"

#ifndef RO_E_CLOSED
#define RO_E_CLOSED ((HRESULT)0x80000013L)
#endif

typedef __x_ABI_CWindows_CStorage_CStreams_CInputStreamOptions stream_options;

struct content {
	struct winrt_object base;
	struct winrt_interface content, closable, stringable;
	struct headers *headers;
	CRITICAL_SECTION lock;
	struct exchange *exchange;
	BYTE *bytes;
	UINT32 length, capacity;
	BOOL complete, closed;
};

static struct content *content_of(void *iface) { return (struct content *)winrt_object_of(iface); }

static void content_destroy(struct winrt_object *object)
{
	struct content *self = (struct content *)object;
	exchange_close(self->exchange);
	headers_release(self->headers);
	WINRT_FREE(self->bytes);
	DeleteCriticalSection(&self->lock);
}

/* Reads more of the body into the content: at least wanted bytes past what it holds, or to its end. Under the lock. */
static HRESULT read_more(struct content *self, UINT32 wanted)
{
	if (self->closed)
		return RO_E_CLOSED;
	const UINT32 goal = self->length + wanted < self->length ? ~0u : self->length + wanted;
	while (!self->complete && self->length < goal) {
		if (self->capacity - self->length < 65536) {
			const UINT32 capacity = self->capacity ? self->capacity * 2 : 65536 * 2;
			BYTE *grown = self->bytes ? WINRT_REALLOC(self->bytes, capacity) : WINRT_ALLOC(capacity);
			if (!grown)
				return E_OUTOFMEMORY;
			self->bytes = grown;
			self->capacity = capacity;
		}
		UINT32 read;
		const HRESULT result = exchange_read(self->exchange, self->bytes + self->length, self->capacity - self->length, &read);
		if (FAILED(result))
			return result;
		self->length += read;
		if (!read) {
			self->complete = TRUE;
			exchange_close(self->exchange);
			self->exchange = NULL;
		}
	}
	return S_OK;
}

static HRESULT read_all(struct content *self)
{
	EnterCriticalSection(&self->lock);
	const HRESULT result = read_more(self, ~0u);
	LeaveCriticalSection(&self->lock);
	return result;
}

/* The work of the asynchronous reads; the context is the content, with a reference the operation releases. */
static void release_content(void *context) { winrt_release(&((struct content *)context)->content); }

static HRESULT buffer_all_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct content *self = context;
	const HRESULT outcome = read_all(self);
	if (SUCCEEDED(outcome))
		result->uint64 = self->length;
	return outcome;
}

static HRESULT read_buffer_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct content *self = context;
	HRESULT outcome = read_all(self);
	if (SUCCEEDED(outcome))
		outcome = winrt_buffer(self->bytes, self->length, self->length, (buffer_class **)&result->object);
	return outcome;
}

/* The charset parameter of Content-Type names UTF-16 (RFC 2781): the bytes are UTF-16 little-endian. */
static BOOL utf16_charset(struct headers *headers)
{
	static const WCHAR *names[] = {L"utf-16", L"utf-16le", L"unicode"};
	HSTRING type = NULL;
	BOOL utf16 = FALSE;
	if (SUCCEEDED(winrt_map_get(headers_map(headers), L"Content-Type", &type))) {
		UINT32 length;
		const WCHAR *text = winrt_text(type, &length);
		for (UINT32 at = 0; at + 8 <= length; ++at)
			if (winrt_equal_ascii(text + at, 8, L"charset=", 8)) {
				UINT32 start = at + 8, end;
				if (start < length && text[start] == '"')
					++start;
				for (end = start; end < length && text[end] != ';' && text[end] != '"' && text[end] != ' '; ++end)
					;
				for (UINT32 index = 0; index < ARRAYSIZE(names); ++index)
					if (winrt_equal_ascii(text + start, end - start, names[index], lstrlenW(names[index])))
						utf16 = TRUE;
				break;
			}
	}
	WindowsDeleteString(type);
	return utf16;
}

static HRESULT read_string_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct content *self = context;
	HRESULT outcome = read_all(self);
	if (FAILED(outcome))
		return outcome;
	const BYTE *bytes = self->bytes;
	UINT32 length = self->length;
	BOOL utf16 = utf16_charset(self->headers);
	if (length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
		bytes += 3;
		length -= 3;
		utf16 = FALSE;
	} else if (length >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
		bytes += 2;
		length -= 2;
		utf16 = TRUE;
	}
	if (utf16)
		return winrt_string_n((const WCHAR *)bytes, length / 2, &result->string);
	UINT32 wide;
	WCHAR *text = winrt_utf16((const char *)bytes, length, &wide);
	if (!text)
		return E_OUTOFMEMORY;
	outcome = winrt_string_n(text, wide, &result->string);
	WINRT_FREE(text);
	return outcome;
}

/* A stream over the content: reads go on from where the last one ended. */
struct stream {
	struct winrt_object base;
	struct winrt_interface stream, closable;
	struct content *content;
	UINT32 position;
	BOOL closed;
};

struct stream_read {
	struct stream *stream;
	buffer_class *buffer;
	UINT32 count;
	BOOL partial;
};

static void stream_destroy(struct winrt_object *object) { winrt_release(&((struct stream *)object)->content->content); }

static void free_stream_read(void *context)
{
	struct stream_read *read = context;
	winrt_put(read->buffer);
	winrt_release(&read->stream->stream);
	WINRT_FREE(read);
}

static HRESULT stream_read_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct stream_read *read = context;
	struct content *content = read->stream->content;
	BYTE *into;
	UINT32 capacity, length;
	HRESULT outcome = read->stream->closed ? RO_E_CLOSED : winrt_buffer_bytes(read->buffer, &into, &length);
	if (SUCCEEDED(outcome))
		outcome = read->buffer->lpVtbl->get_Capacity(read->buffer, &capacity);
	if (FAILED(outcome))
		return outcome;
	const UINT32 count = read->count < capacity ? read->count : capacity;
	EnterCriticalSection(&content->lock);
	/* A partial read answers with what one read of the body gives; otherwise it waits for count bytes or the end. */
	if (content->length - read->stream->position < count)
		outcome = read_more(content, read->partial && content->length > read->stream->position ? 0 :
			read->partial ? 1 : count - (content->length - read->stream->position));
	UINT32 taken = 0;
	if (SUCCEEDED(outcome)) {
		taken = content->length - read->stream->position;
		if (taken > count)
			taken = count;
		memcpy(into, content->bytes + read->stream->position, taken);
		read->stream->position += taken;
	}
	LeaveCriticalSection(&content->lock);
	if (SUCCEEDED(outcome))
		outcome = read->buffer->lpVtbl->put_Length(read->buffer, taken);
	if (SUCCEEDED(outcome)) {
		read->buffer->lpVtbl->AddRef(read->buffer);
		result->object = read->buffer;
	}
	return outcome;
}

static HRESULT STDMETHODCALLTYPE stream_read_async(void *iface, buffer_class *buffer, UINT32 count, stream_options options,
	__FIAsyncOperationWithProgress_2_Windows__CStorage__CStreams__CIBuffer_UINT32 **out)
{
	struct stream *self = (struct stream *)winrt_object_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!buffer)
		return E_INVALIDARG;
	if (self->closed)
		return RO_E_CLOSED;
	struct stream_read *read = WINRT_ALLOC(sizeof(*read));
	if (!read)
		return E_OUTOFMEMORY;
	winrt_add_ref(&self->stream);
	buffer->lpVtbl->AddRef(buffer);
	read->stream = self;
	read->buffer = buffer;
	read->count = count;
	read->partial = (options & InputStreamOptions_Partial) != 0;
	return winrt_async_start(&IID___FIAsyncOperationWithProgress_2_Windows__CStorage__CStreams__CIBuffer_UINT32, TRUE, WINRT_RESULT_OBJECT,
		stream_read_work, read, free_stream_read, (void **)out);
}

static HRESULT STDMETHODCALLTYPE stream_close(void *iface)
{
	((struct stream *)winrt_object_of(iface))->closed = TRUE;
	return S_OK;
}

static const __x_ABI_CWindows_CStorage_CStreams_CIInputStreamVtbl stream_table = {WINRT_INSPECTABLE, (void *)stream_read_async};
static const __x_ABI_CWindows_CFoundation_CIClosableVtbl stream_closable_table = {WINRT_INSPECTABLE, (void *)stream_close};
static const struct winrt_entry stream_entries[] = {
	WINRT_ENTRY(struct stream, stream, IID___x_ABI_CWindows_CStorage_CStreams_CIInputStream),
	WINRT_ENTRY(struct stream, closable, IID___x_ABI_CWindows_CFoundation_CIClosable), WINRT_END};
static const struct winrt_class stream_kind = {L"Windows.Storage.Streams.IInputStream", stream_entries, stream_destroy};

static HRESULT read_stream_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct content *content = context;
	if (content->closed)
		return RO_E_CLOSED;
	struct stream *self = winrt_new(&stream_kind, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->stream.table = &stream_table;
	self->closable.table = &stream_closable_table;
	winrt_add_ref(&content->content);
	self->content = content;
	result->object = &self->stream;
	return S_OK;
}

/* Starts one of the reads, holding the content until it has run. */
static HRESULT start_read(void *iface, const IID *operation_iid, enum winrt_result kind, winrt_work work, void **out)
{
	struct content *self = content_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->closed)
		return RO_E_CLOSED;
	winrt_add_ref(&self->content);
	return winrt_async_start(operation_iid, TRUE, kind, work, self, release_content, out);
}

static HRESULT STDMETHODCALLTYPE content_headers(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = headers_interface(content_of(iface)->headers);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE content_buffer_all(void *iface, void **out)
{
	return start_read(iface, &IID___FIAsyncOperationWithProgress_2_UINT64_UINT64, WINRT_RESULT_UINT64, buffer_all_work, out);
}

static HRESULT STDMETHODCALLTYPE content_read_buffer(void *iface, void **out)
{
	return start_read(iface, &IID___FIAsyncOperationWithProgress_2_Windows__CStorage__CStreams__CIBuffer_UINT64, WINRT_RESULT_OBJECT,
		read_buffer_work, out);
}

static HRESULT STDMETHODCALLTYPE content_read_stream(void *iface, void **out)
{
	return start_read(iface, &IID___FIAsyncOperationWithProgress_2_Windows__CStorage__CStreams__CIInputStream_UINT64, WINRT_RESULT_OBJECT,
		read_stream_work, out);
}

static HRESULT STDMETHODCALLTYPE content_read_string(void *iface, void **out)
{
	return start_read(iface, &IID___FIAsyncOperationWithProgress_2_HSTRING_UINT64, WINRT_RESULT_STRING, read_string_work, out);
}

static HRESULT STDMETHODCALLTYPE content_length(void *iface, UINT64 *length, boolean *known)
{
	struct content *self = content_of(iface);
	if (!length || !known)
		return E_POINTER;
	EnterCriticalSection(&self->lock);
	*known = self->complete || (self->exchange && exchange_length(self->exchange, length));
	if (self->complete)
		*length = self->length;
	else if (!*known)
		*length = 0;
	LeaveCriticalSection(&self->lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE content_write(void *iface, void *stream, void **out)
{
	/* Writing the content to an output stream is not provided yet. */
	if (!out)
		return E_POINTER;
	*out = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE content_close(void *iface)
{
	struct content *self = content_of(iface);
	EnterCriticalSection(&self->lock);
	self->closed = TRUE;
	exchange_close(self->exchange);
	self->exchange = NULL;
	LeaveCriticalSection(&self->lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE content_string(void *iface, HSTRING *out) { return winrt_class_name(iface, out); }

static const __x_ABI_CWindows_CWeb_CHttp_CIHttpContentVtbl content_table = {WINRT_INSPECTABLE, (void *)content_headers,
	(void *)content_buffer_all, (void *)content_read_buffer, (void *)content_read_stream, (void *)content_read_string,
	(void *)content_length, (void *)content_write};
static const __x_ABI_CWindows_CFoundation_CIClosableVtbl content_closable_table = {WINRT_INSPECTABLE, (void *)content_close};
static const __x_ABI_CWindows_CFoundation_CIStringableVtbl content_stringable_table = {WINRT_INSPECTABLE, (void *)content_string};
static const struct winrt_entry content_entries[] = {
	WINRT_ENTRY(struct content, content, IID___x_ABI_CWindows_CWeb_CHttp_CIHttpContent),
	WINRT_ENTRY(struct content, closable, IID___x_ABI_CWindows_CFoundation_CIClosable),
	WINRT_ENTRY(struct content, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_class content_kind = {L"Windows.Web.Http.HttpStreamContent", content_entries, content_destroy};

HRESULT content_new(struct exchange *exchange, struct headers *headers, http_content **out)
{
	struct content *self = winrt_new(&content_kind, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->content.table = &content_table;
	self->closable.table = &content_closable_table;
	self->stringable.table = &content_stringable_table;
	InitializeCriticalSection(&self->lock);
	/* The content holds the headers: headers_interface adds the reference content_destroy gives back. */
	headers_interface(headers);
	self->headers = headers;
	self->exchange = exchange;
	*out = (void *)&self->content;
	return S_OK;
}

HRESULT content_bytes(http_content *content, BYTE **bytes, UINT32 *length)
{
	__FIAsyncOperationWithProgress_2_Windows__CStorage__CStreams__CIBuffer_UINT64 *operation = NULL;
	buffer_class *buffer = NULL;
	BYTE *data;
	*bytes = NULL;
	*length = 0;
	HRESULT result = content->lpVtbl->ReadAsBufferAsync(content, &operation);
	if (SUCCEEDED(result))
		result = winrt_wait(operation, TRUE, &buffer);
	if (SUCCEEDED(result))
		result = winrt_buffer_bytes(buffer, &data, length);
	if (SUCCEEDED(result) && !(*bytes = WINRT_ALLOC(*length ? *length : 1)))
		result = E_OUTOFMEMORY;
	if (SUCCEEDED(result))
		memcpy(*bytes, data, *length);
	winrt_put(buffer);
	winrt_put(operation);
	return result;
}
