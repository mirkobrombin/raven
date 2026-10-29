/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* One request through WinINet: the request's method, URI, headers and body go out; the status, reason, version and headers of the
 * response come back, with the body left to read. Redirections are followed by WinINet unless the filter says not to, and the request
 * message is pointed at the URI they ended at, as Windows does. A failure is WinINet's error as an HRESULT (WININET_E_*). */
#include "web.h"

struct exchange {
	HINTERNET session, connection, request;
	BOOL has_length;
	UINT64 length;
};

struct exchange *exchange_open(void) { return WINRT_ALLOC(sizeof(struct exchange)); }

void exchange_close(struct exchange *exchange)
{
	if (!exchange)
		return;
	if (exchange->request)
		InternetCloseHandle(exchange->request);
	if (exchange->connection)
		InternetCloseHandle(exchange->connection);
	if (exchange->session)
		InternetCloseHandle(exchange->session);
	WINRT_FREE(exchange);
}

BOOL exchange_length(struct exchange *exchange, UINT64 *length)
{
	*length = exchange->length;
	return exchange->has_length;
}

HRESULT exchange_read(struct exchange *exchange, BYTE *into, UINT32 capacity, UINT32 *read)
{
	DWORD got = 0;
	*read = 0;
	if (!exchange->request)
		return S_OK;
	if (!InternetReadFile(exchange->request, into, capacity, &got))
		return web_error(GetLastError());
	*read = got;
	return S_OK;
}

/* A header of the request as a line for HttpAddRequestHeaders. */
struct head {
	WCHAR *text;
	UINT32 used, capacity;
};

static HRESULT add_head_line(void *context, HSTRING key, HSTRING value)
{
	struct head *head = context;
	UINT32 key_length, value_length;
	const WCHAR *key_text = winrt_text(key, &key_length), *value_text = winrt_text(value, &value_length);
	const UINT32 needed = head->used + key_length + value_length + 5;
	if (needed > head->capacity) {
		const UINT32 capacity = needed * 2;
		WCHAR *grown = head->text ? WINRT_REALLOC(head->text, capacity * sizeof(WCHAR)) : WINRT_ALLOC(capacity * sizeof(WCHAR));
		if (!grown)
			return E_OUTOFMEMORY;
		head->text = grown;
		head->capacity = capacity;
	}
	memcpy(head->text + head->used, key_text, key_length * sizeof(WCHAR));
	head->used += key_length;
	head->text[head->used++] = ':';
	head->text[head->used++] = ' ';
	memcpy(head->text + head->used, value_text, value_length * sizeof(WCHAR));
	head->used += value_length;
	head->text[head->used++] = '\r';
	head->text[head->used++] = '\n';
	head->text[head->used] = 0;
	return S_OK;
}

/* The headers of any header collection, through IIterable<IKeyValuePair<String, String>>. */
static HRESULT collect_headers(IInspectable *collection, struct head *head)
{
	__FIIterable_1___FIKeyValuePair_2_HSTRING_HSTRING *iterable = NULL;
	__FIIterator_1___FIKeyValuePair_2_HSTRING_HSTRING *iterator = NULL;
	boolean more = FALSE;
	HRESULT result = collection->lpVtbl->QueryInterface(collection, &IID___FIIterable_1___FIKeyValuePair_2_HSTRING_HSTRING, (void **)&iterable);
	if (SUCCEEDED(result))
		result = iterable->lpVtbl->First(iterable, &iterator);
	if (SUCCEEDED(result))
		result = iterator->lpVtbl->get_HasCurrent(iterator, &more);
	while (SUCCEEDED(result) && more) {
		__FIKeyValuePair_2_HSTRING_HSTRING *pair = NULL;
		HSTRING key = NULL, value = NULL;
		if (SUCCEEDED(result = iterator->lpVtbl->get_Current(iterator, &pair))) {
			if (SUCCEEDED(result = pair->lpVtbl->get_Key(pair, &key)) && SUCCEEDED(result = pair->lpVtbl->get_Value(pair, &value)))
				result = add_head_line(head, key, value);
			WindowsDeleteString(key);
			WindowsDeleteString(value);
			pair->lpVtbl->Release(pair);
		}
		if (SUCCEEDED(result))
			result = iterator->lpVtbl->MoveNext(iterator, &more);
	}
	winrt_put(iterator);
	winrt_put(iterable);
	return result;
}

/* A text answer of HttpQueryInfo, in a block the caller frees, or NULL when the head has none. */
static WCHAR *query_text(HINTERNET request, DWORD level, UINT32 *length)
{
	DWORD bytes = 0;
	WCHAR *text;
	HttpQueryInfoW(request, level, NULL, &bytes, NULL);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !(text = WINRT_ALLOC(bytes + sizeof(WCHAR))))
		return NULL;
	if (!HttpQueryInfoW(request, level, text, &bytes, NULL)) {
		WINRT_FREE(text);
		return NULL;
	}
	*length = bytes / sizeof(WCHAR);
	return text;
}

/* The response's headers, split between the message and its content, as the Windows classes keep them. */
static HRESULT read_headers(struct exchange *exchange, struct headers *message, struct headers *content)
{
	UINT32 length = 0;
	WCHAR *raw = query_text(exchange->request, HTTP_QUERY_RAW_HEADERS_CRLF, &length);
	if (!raw)
		return S_OK;
	HRESULT result = S_OK;
	UINT32 at = 0;
	/* The status line comes first. */
	while (at < length && raw[at] != '\n')
		++at;
	++at;
	while (at < length && SUCCEEDED(result)) {
		UINT32 end = at;
		while (end < length && raw[end] != '\r' && raw[end] != '\n')
			++end;
		UINT32 colon = at;
		while (colon < end && raw[colon] != ':')
			++colon;
		if (colon < end && colon > at) {
			UINT32 value = colon + 1;
			while (value < end && (raw[value] == ' ' || raw[value] == '\t'))
				++value;
			UINT32 value_end = end;
			while (value_end > value && (raw[value_end - 1] == ' ' || raw[value_end - 1] == '\t'))
				--value_end;
			struct headers *target = header_is_content(raw + at, colon - at) ? content : message;
			result = winrt_map_set(headers_map(target), raw + at, colon - at, raw + value, value_end - value, TRUE);
		}
		at = end;
		while (at < length && (raw[at] == '\r' || raw[at] == '\n'))
			++at;
	}
	WINRT_FREE(raw);
	return result;
}

static INT32 version_of(const WCHAR *text, UINT32 length)
{
	/* HttpVersion: None, Http10, Http11, Http20. */
	if (length >= 8 && text[5] == '1' && text[7] == '0')
		return 1;
	if (length >= 8 && text[5] == '1' && text[7] == '1')
		return 2;
	if (length >= 6 && text[5] == '2')
		return 3;
	return 0;
}

HRESULT exchange_send(struct exchange *exchange, const struct transport_settings *settings, request_message *request,
	response_message **out)
{
	HSTRING method = NULL, uri_string = NULL;
	uri_class *uri = NULL;
	http_content *content = NULL;
	IInspectable *request_headers = NULL, *content_headers = NULL;
	struct head head = {0};
	BYTE *body = NULL;
	UINT32 body_length = 0;
	struct headers *message_headers = NULL, *response_content_headers = NULL;
	response_message *response = NULL;
	http_content *response_content = NULL;
	*out = NULL;

	HRESULT result = request_parts(request, &method, &uri, &content);
	if (SUCCEEDED(result) && !uri)
		result = E_INVALIDARG;
	if (SUCCEEDED(result))
		result = uri_text(uri, &uri_string);
	if (SUCCEEDED(result))
		result = request->lpVtbl->get_Headers(request, (void *)&request_headers);
	if (SUCCEEDED(result))
		result = collect_headers(request_headers, &head);
	if (SUCCEEDED(result) && content) {
		result = content->lpVtbl->get_Headers(content, (void *)&content_headers);
		if (SUCCEEDED(result))
			result = collect_headers(content_headers, &head);
		if (SUCCEEDED(result))
			result = content_bytes(content, &body, &body_length);
	}

	/* The URI's parts. */
	WCHAR host[INTERNET_MAX_HOST_NAME_LENGTH + 1], user[256], password[256];
	UINT32 uri_length;
	const WCHAR *uri_text_buffer = winrt_text(uri_string, &uri_length);
	URL_COMPONENTSW parts = {sizeof(parts)};
	parts.lpszHostName = host;
	parts.dwHostNameLength = ARRAYSIZE(host);
	parts.lpszUserName = user;
	parts.dwUserNameLength = ARRAYSIZE(user);
	parts.lpszPassword = password;
	parts.dwPasswordLength = ARRAYSIZE(password);
	parts.dwUrlPathLength = 1;
	parts.dwExtraInfoLength = 1;
	if (SUCCEEDED(result) && !InternetCrackUrlW(uri_text_buffer, uri_length, 0, &parts))
		result = web_error(GetLastError());
	if (SUCCEEDED(result) && parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)
		result = E_INVALIDARG;

	WCHAR *object = NULL;
	if (SUCCEEDED(result)) {
		const DWORD path = parts.dwUrlPathLength, extra = parts.dwExtraInfoLength;
		if (!(object = WINRT_ALLOC(((size_t)path + extra + 2) * sizeof(WCHAR))))
			result = E_OUTOFMEMORY;
		else {
			memcpy(object, parts.lpszUrlPath, path * sizeof(WCHAR));
			memcpy(object + path, parts.lpszExtraInfo, extra * sizeof(WCHAR));
			if (!path && !extra)
				object[0] = '/';
		}
	}
	if (SUCCEEDED(result) && !(exchange->session = InternetOpenW(NULL, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0)))
		result = web_error(GetLastError());
	if (SUCCEEDED(result) && settings->decompress) {
		BOOL decode = TRUE;
		InternetSetOptionW(exchange->session, INTERNET_OPTION_HTTP_DECODING, &decode, sizeof(decode));
	}
	if (SUCCEEDED(result) && !(exchange->connection = InternetConnectW(exchange->session, host, parts.nPort,
		parts.dwUserNameLength ? user : NULL, parts.dwPasswordLength ? password : NULL, INTERNET_SERVICE_HTTP, 0, 0)))
		result = web_error(GetLastError());
	if (SUCCEEDED(result)) {
		DWORD flags = INTERNET_FLAG_KEEP_CONNECTION | settings->cache_flags;
		if (parts.nScheme == INTERNET_SCHEME_HTTPS)
			flags |= INTERNET_FLAG_SECURE;
		if (!settings->allow_redirect)
			flags |= INTERNET_FLAG_NO_AUTO_REDIRECT;
		if (!settings->use_cookies)
			flags |= INTERNET_FLAG_NO_COOKIES;
		if (!(exchange->request = HttpOpenRequestW(exchange->connection, winrt_text(method, NULL), object, NULL, NULL, NULL, flags, 0)))
			result = web_error(GetLastError());
	}
	if (SUCCEEDED(result) && head.used && !HttpAddRequestHeadersW(exchange->request, head.text, head.used, HTTP_ADDREQ_FLAG_ADD))
		result = web_error(GetLastError());
	if (SUCCEEDED(result) && !HttpSendRequestW(exchange->request, NULL, 0, body, body_length))
		result = web_error(GetLastError());

	/* The response's head. */
	DWORD status = 0, size = sizeof(status);
	if (SUCCEEDED(result) && !HttpQueryInfoW(exchange->request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL))
		result = web_error(GetLastError());
	if (SUCCEEDED(result))
		result = response_new((INT32)status, &response);
	if (SUCCEEDED(result))
		result = headers_new(HEADERS_RESPONSE, &message_headers);
	if (SUCCEEDED(result))
		result = headers_new(HEADERS_CONTENT, &response_content_headers);
	if (SUCCEEDED(result))
		result = read_headers(exchange, message_headers, response_content_headers);
	if (SUCCEEDED(result)) {
		UINT32 length = 0;
		WCHAR *reason = query_text(exchange->request, HTTP_QUERY_STATUS_TEXT, &length);
		WCHAR *version = query_text(exchange->request, HTTP_QUERY_VERSION, &(UINT32){0});
		UINT32 version_length = version ? lstrlenW(version) : 0;
		HSTRING reason_string = NULL;
		winrt_string_n(reason, reason ? length : 0, &reason_string);
		response_set_head(response, reason_string, version ? version_of(version, version_length) : 0, request, message_headers);
		WindowsDeleteString(reason_string);
		WINRT_FREE(reason);
		WINRT_FREE(version);
		/* Where redirections ended. */
		WCHAR *final_url = NULL;
		DWORD url_bytes = 0;
		InternetQueryOptionW(exchange->request, INTERNET_OPTION_URL, NULL, &url_bytes);
		if (url_bytes && (final_url = WINRT_ALLOC(url_bytes + sizeof(WCHAR))) &&
			InternetQueryOptionW(exchange->request, INTERNET_OPTION_URL, final_url, &url_bytes) && CompareStringOrdinal(final_url, -1,
			uri_text_buffer, (int)uri_length, FALSE) != CSTR_EQUAL) {
			uri_class *final_uri = NULL;
			if (SUCCEEDED(uri_create(final_url, lstrlenW(final_url), &final_uri))) {
				request_set_uri(request, final_uri);
				final_uri->lpVtbl->Release(final_uri);
			}
		}
		WINRT_FREE(final_url);
		UINT64 content_length = 0;
		DWORD length_size = sizeof(content_length);
		exchange->has_length = HttpQueryInfoW(exchange->request, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER64, &content_length,
			&length_size, NULL);
		exchange->length = exchange->has_length ? content_length : 0;
	}
	if (SUCCEEDED(result))
		result = content_new(exchange, response_content_headers, &response_content);
	if (SUCCEEDED(result)) {
		response_set_content(response, response_content);
		*out = response;
		response = NULL;
	}

	winrt_put(response_content);
	winrt_put(response);
	headers_release(message_headers);
	headers_release(response_content_headers);
	WINRT_FREE(object);
	WINRT_FREE(body);
	WINRT_FREE(head.text);
	winrt_put(content_headers);
	winrt_put(request_headers);
	winrt_put(content);
	winrt_put(uri);
	WindowsDeleteString(uri_string);
	WindowsDeleteString(method);
	return result;
}
