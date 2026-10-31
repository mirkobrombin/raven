#define INITGUID
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <winstring.h>
#include <roapi.h>
#include <robuffer.h>
#include "windows.web.http.h"

typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClass uri_class;
typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClassFactory uri_factory;
typedef __x_ABI_CWindows_CWeb_CHttp_CIHttpClient http_client;
typedef __x_ABI_CWindows_CWeb_CHttp_CIHttpResponseMessage response_message;
typedef __x_ABI_CWindows_CWeb_CHttp_CIHttpRequestMessage request_message;
typedef __x_ABI_CWindows_CWeb_CHttp_CIHttpContent http_content;
typedef __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpRequestHeaderCollection request_headers;
typedef __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpResponseHeaderCollection response_headers;
typedef __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpContentHeaderCollection content_headers;

static int same_bytes(const char *left, const char *right, int length)
{
	for (int index = 0; index < length; ++index)
		if (left[index] != right[index])
			return 0;
	return 1;
}

static int length_of(const char *text)
{
	int length = 0;
	while (text[length])
		++length;
	return length;
}

static int contains(const char *text, int length, const char *part)
{
	const int wanted = length_of(part);
	for (int at = 0; at + wanted <= length; ++at)
		if (same_bytes(text + at, part, wanted))
			return 1;
	return 0;
}

static SOCKET listener;

/* Answers one request per connection, by its path: what a package source's CDN does, and a little more. */
static DWORD WINAPI serve(void *unused)
{
	for (;;) {
		SOCKET peer = accept(listener, NULL, NULL);
		if (peer == INVALID_SOCKET)
			return 0;
		char request[4096];
		int size = 0, head = -1;
		while (head < 0 && size < (int)sizeof(request)) {
			const int got = recv(peer, request + size, sizeof(request) - size, 0);
			if (got <= 0)
				break;
			size += got;
			for (int at = 0; at + 3 < size; ++at)
				if (same_bytes(request + at, "\r\n\r\n", 4)) {
					head = at + 4;
					break;
				}
		}
		const char *answer = "HTTP/1.1 400 Bad\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		/* The client's default headers go out with every request; the cache control asks for the most recent copy. */
		const int defaults = contains(request, head, "\r\nUser-Agent: raven-test/1.0 (probe)\r\n") &&
			contains(request, head, "\r\nConnection: close\r\n");
		if (head < 0 || !defaults)
			answer = "HTTP/1.1 500 Defaults\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		else if (same_bytes(request, "HEAD /index ", 12))
			answer = "HTTP/1.1 200 OK\r\nx-ms-meta-sourceversion: 1.2.3\r\nContent-Length: 1234\r\nContent-Type: application/msix\r\n"
				"Last-Modified: Sun, 06 Nov 1994 08:49:37 GMT\r\nConnection: close\r\n\r\n";
		else if (same_bytes(request, "GET /redirect ", 14))
			answer = "HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		else if (same_bytes(request, "GET /final ", 11))
			answer = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 15\r\nConnection: close\r\n\r\n"
				"Hello, w\xc3\xb6rld!!";
		else if (same_bytes(request, "GET /busy ", 10))
			answer = "HTTP/1.1 503 Busy\r\nRetry-After: 120\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		else if (same_bytes(request, "GET /range ", 11) && contains(request, head, "\r\nRange: bytes=1-3\r\n"))
			answer = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 1-3/12\r\nContent-Length: 3\r\nConnection: close\r\n\r\nell";
		else if (same_bytes(request, "GET /missing ", 13))
			answer = "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnone";
		send(peer, answer, length_of(answer), 0);
		closesocket(peer);
	}
}

static HSTRING text(const WCHAR *value)
{
	HSTRING string = NULL;
	WindowsCreateString(value, lstrlenW(value), &string);
	return string;
}

static int equals(HSTRING string, const WCHAR *expected)
{
	UINT32 length;
	const WCHAR *characters = WindowsGetStringRawBuffer(string, &length);
	return (int)length == lstrlenW(expected) && CompareStringOrdinal(characters, (int)length, expected, -1, FALSE) == CSTR_EQUAL;
}

/* Waits for an operation through IAsyncInfo, as a program polling its status does, and answers its status. */
static AsyncStatus wait(void *operation, HRESULT *error)
{
	IAsyncInfo *info = NULL;
	AsyncStatus status = Started;
	if (FAILED(IUnknown_QueryInterface((IUnknown *)operation, &IID_IAsyncInfo, (void **)&info)))
		return Error;
	for (int round = 0; round < 1000 && status == Started; ++round) {
		IAsyncInfo_get_Status(info, &status);
		if (status == Started)
			Sleep(10);
	}
	IAsyncInfo_get_ErrorCode(info, error);
	IAsyncInfo_Release(info);
	return status;
}

static uri_factory *uris;

static uri_class *make_uri(USHORT port, const WCHAR *path)
{
	WCHAR address[64] = L"http://127.0.0.1:";
	WCHAR digits[8];
	int count = 0;
	do {
		digits[count++] = (WCHAR)('0' + port % 10);
		port /= 10;
	} while (port);
	int at = lstrlenW(address);
	while (count)
		address[at++] = digits[--count];
	address[at] = 0;
	lstrcatW(address, path);
	HSTRING string = text(address);
	uri_class *uri = NULL;
	__x_ABI_CWindows_CFoundation_CIUriRuntimeClassFactory_CreateUri(uris, string, &uri);
	WindowsDeleteString(string);
	return uri;
}

/* Windows.Web.Http as winget uses it: a base protocol filter with its cache control, a client over it with default headers, a HEAD
 * request whose response headers are read, a redirection the request message follows, content read as a string and a buffer, a status
 * that fails GetStringAsync, Retry-After, and a range request; and Windows.Foundation.Uri. */
int raven_entry(void)
{
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data))
		return 1;
	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int length = sizeof(address);
	if (listener == INVALID_SOCKET || bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 8) ||
		getsockname(listener, (struct sockaddr *)&address, &length))
		return 2;
	const USHORT port = ntohs(address.sin_port);
	if (!CreateThread(NULL, 0, serve, NULL, 0, NULL))
		return 3;
	/* The activation of the Windows Runtime is imported, as C++/WinRT programs import it, and nothing else of COM. */
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED)))
		return 4;

	/* Uri */
	HSTRING name = text(L"Windows.Foundation.Uri");
	HRESULT result = RoGetActivationFactory(name, &IID___x_ABI_CWindows_CFoundation_CIUriRuntimeClassFactory, (void **)&uris);
	WindowsDeleteString(name);
	if (FAILED(result))
		return 10;
	HSTRING value = NULL, wanted = text(L"https://Example.COM/a/b.txt?x=1#top");
	uri_class *uri = NULL;
	if (FAILED(__x_ABI_CWindows_CFoundation_CIUriRuntimeClassFactory_CreateUri(uris, wanted, &uri)))
		return 11;
	WindowsDeleteString(wanted);
	INT32 number = 0;
	if (FAILED(uri->lpVtbl->get_Host(uri, &value)) || !equals(value, L"example.com"))
		return 12;
	WindowsDeleteString(value);
	if (FAILED(uri->lpVtbl->get_Path(uri, &value)) || !equals(value, L"/a/b.txt"))
		return 13;
	WindowsDeleteString(value);
	if (FAILED(uri->lpVtbl->get_Query(uri, &value)) || !equals(value, L"?x=1"))
		return 14;
	WindowsDeleteString(value);
	if (FAILED(uri->lpVtbl->get_Extension(uri, &value)) || !equals(value, L".txt"))
		return 15;
	WindowsDeleteString(value);
	if (FAILED(uri->lpVtbl->get_Port(uri, &number)) || number != 443)
		return 16;
	uri->lpVtbl->Release(uri);
	uri = NULL;
	wanted = text(L"not a uri");
	if (__x_ABI_CWindows_CFoundation_CIUriRuntimeClassFactory_CreateUri(uris, wanted, &uri) != E_INVALIDARG || uri)
		return 17;
	WindowsDeleteString(wanted);

	/* The filter, its cache control and the client. */
	IInspectable *object = NULL;
	name = text(L"Windows.Web.Http.Filters.HttpBaseProtocolFilter");
	result = RoActivateInstance(name, &object);
	WindowsDeleteString(name);
	if (FAILED(result))
		return 20;
	__x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter *filter = NULL;
	__x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpCacheControl *cache = NULL;
	__x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpFilter *http_filter = NULL;
	boolean flag = FALSE;
	if (FAILED(object->lpVtbl->QueryInterface(object, &IID___x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter, (void **)&filter)) ||
		FAILED(filter->lpVtbl->get_AllowAutoRedirect(filter, &flag)) || !flag || FAILED(filter->lpVtbl->get_CacheControl(filter, &cache)) ||
		FAILED(cache->lpVtbl->put_ReadBehavior(cache, HttpCacheReadBehavior_MostRecent)))
		return 21;
	if (FAILED(object->lpVtbl->QueryInterface(object, &IID___x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpFilter, (void **)&http_filter)))
		return 22;
	__x_ABI_CWindows_CWeb_CHttp_CIHttpClientFactory *clients = NULL;
	name = text(L"Windows.Web.Http.HttpClient");
	result = RoGetActivationFactory(name, &IID___x_ABI_CWindows_CWeb_CHttp_CIHttpClientFactory, (void **)&clients);
	WindowsDeleteString(name);
	http_client *client = NULL;
	if (FAILED(result) || FAILED(clients->lpVtbl->Create(clients, http_filter, &client)))
		return 23;
	request_headers *defaults = NULL;
	__x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpConnectionOptionHeaderValueCollection *connection = NULL;
	__x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpProductInfoHeaderValueCollection *agent = NULL;
	__FIVector_1_Windows__CWeb__CHttp__CHeaders__CHttpConnectionOptionHeaderValue *options = NULL;
	if (FAILED(client->lpVtbl->get_DefaultRequestHeaders(client, &defaults)) || FAILED(defaults->lpVtbl->get_Connection(defaults, &connection)) ||
		FAILED(connection->lpVtbl->QueryInterface(connection, &IID___FIVector_1_Windows__CWeb__CHttp__CHeaders__CHttpConnectionOptionHeaderValue,
			(void **)&options)) || FAILED(options->lpVtbl->Clear(options)))
		return 24;
	HSTRING header = text(L"Connection"), header_value = text(L"close");
	if (FAILED(defaults->lpVtbl->Append(defaults, header, header_value)))
		return 25;
	WindowsDeleteString(header);
	WindowsDeleteString(header_value);
	header_value = text(L"raven-test/1.0 (probe)");
	UINT32 count = 0;
	if (FAILED(defaults->lpVtbl->get_UserAgent(defaults, &agent)) || FAILED(agent->lpVtbl->ParseAdd(agent, header_value)) ||
		FAILED(options->lpVtbl->get_Size(options, &count)) || count != 1)
		return 26;
	WindowsDeleteString(header_value);
	header = text(L"Bad Name");
	if (defaults->lpVtbl->Append(defaults, header, header) != E_INVALIDARG)
		return 27;
	WindowsDeleteString(header);

	/* HEAD, with the response's headers split between the message and its content. */
	__x_ABI_CWindows_CWeb_CHttp_CIHttpMethodStatics *methods = NULL;
	__x_ABI_CWindows_CWeb_CHttp_CIHttpRequestMessageFactory *requests = NULL;
	__x_ABI_CWindows_CWeb_CHttp_CIHttpMethod *head = NULL;
	name = text(L"Windows.Web.Http.HttpMethod");
	result = RoGetActivationFactory(name, &IID___x_ABI_CWindows_CWeb_CHttp_CIHttpMethodStatics, (void **)&methods);
	WindowsDeleteString(name);
	name = text(L"Windows.Web.Http.HttpRequestMessage");
	if (SUCCEEDED(result))
		result = RoGetActivationFactory(name, &IID___x_ABI_CWindows_CWeb_CHttp_CIHttpRequestMessageFactory, (void **)&requests);
	WindowsDeleteString(name);
	request_message *request = NULL;
	if (FAILED(result) || FAILED(methods->lpVtbl->get_Head(methods, &head)) ||
		FAILED(requests->lpVtbl->Create(requests, head, make_uri(port, L"/index"), &request)))
		return 30;
	__FIAsyncOperationWithProgress_2_Windows__CWeb__CHttp__CHttpResponseMessage_HttpProgress *sending = NULL;
	response_message *response = NULL;
	HRESULT error = S_OK;
	if (FAILED(client->lpVtbl->SendRequestWithOptionAsync(client, request, HttpCompletionOption_ResponseHeadersRead, &sending)) ||
		wait(sending, &error) != Completed || FAILED(sending->lpVtbl->GetResults(sending, &response)))
		return 31;
	__x_ABI_CWindows_CWeb_CHttp_CHttpStatusCode status = 0;
	response_headers *message_headers = NULL;
	__FIMap_2_HSTRING_HSTRING *map = NULL;
	if (FAILED(response->lpVtbl->get_StatusCode(response, &status)) || status != 200)
		return 100 + status / 100;
	header = text(L"X-MS-Meta-SourceVersion");
	if (FAILED(response->lpVtbl->get_Headers(response, &message_headers)) ||
		FAILED(message_headers->lpVtbl->QueryInterface(message_headers, &IID___FIMap_2_HSTRING_HSTRING, (void **)&map)) ||
		FAILED(map->lpVtbl->Lookup(map, header, &value)) || !equals(value, L"1.2.3"))
		return 32;
	WindowsDeleteString(value);
	WindowsDeleteString(header);
	http_content *content = NULL;
	content_headers *body_headers = NULL;
	__FIReference_1_UINT64 *content_length = NULL;
	__FIReference_1_DateTime *modified = NULL;
	UINT64 size = 0;
	__x_ABI_CWindows_CFoundation_CDateTime when = {0};
	if (FAILED(response->lpVtbl->get_Content(response, &content)) || FAILED(content->lpVtbl->get_Headers(content, &body_headers)) ||
		FAILED(body_headers->lpVtbl->get_ContentLength(body_headers, &content_length)) || !content_length ||
		FAILED(content_length->lpVtbl->get_Value(content_length, &size)) || size != 1234)
		return 33;
	/* Sun, 06 Nov 1994 08:49:37 GMT, in 100-nanosecond intervals since 1601. */
	if (FAILED(body_headers->lpVtbl->get_LastModified(body_headers, &modified)) || !modified ||
		FAILED(modified->lpVtbl->get_Value(modified, &when)) || when.UniversalTime != 0x01b98d5354912680LL)
		return 34;
	header = text(L"Content-Length");
	boolean present = TRUE;
	if (FAILED(map->lpVtbl->HasKey(map, header, &present)) || present)
		return 35;
	WindowsDeleteString(header);

	/* A redirection, which the request message follows; the body read as a string. */
	__FIAsyncOperationWithProgress_2_HSTRING_HttpProgress *reading = NULL;
	if (FAILED(client->lpVtbl->GetAsync(client, make_uri(port, L"/redirect"), &sending)) || wait(sending, &error) != Completed ||
		FAILED(sending->lpVtbl->GetResults(sending, &response)) || FAILED(response->lpVtbl->get_StatusCode(response, &status)) || status != 200)
		return 40;
	if (FAILED(response->lpVtbl->get_RequestMessage(response, &request)) || FAILED(request->lpVtbl->get_RequestUri(request, &uri)) ||
		FAILED(uri->lpVtbl->get_Path(uri, &value)) || !equals(value, L"/final"))
		return 41;
	WindowsDeleteString(value);
	if (FAILED(client->lpVtbl->GetStringAsync(client, make_uri(port, L"/final"), &reading)) || wait(reading, &error) != Completed ||
		FAILED(reading->lpVtbl->GetResults(reading, &value)) || !equals(value, L"Hello, w\x00f6rld!!"))
		return 42;
	WindowsDeleteString(value);

	/* A status outside 2xx is a response, but fails GetStringAsync with its HRESULT. */
	if (FAILED(client->lpVtbl->GetAsync(client, make_uri(port, L"/missing"), &sending)) || wait(sending, &error) != Completed ||
		FAILED(sending->lpVtbl->GetResults(sending, &response)) || FAILED(response->lpVtbl->get_StatusCode(response, &status)) || status != 404)
		return 50;
	if (FAILED(client->lpVtbl->GetStringAsync(client, make_uri(port, L"/missing"), &reading)) || wait(reading, &error) != Error ||
		error != (HRESULT)0x80190194)
		return 51;

	/* Retry-After as a delay. */
	__x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpDateOrDeltaHeaderValue *retry = NULL;
	__FIReference_1_TimeSpan *delta = NULL;
	__x_ABI_CWindows_CFoundation_CTimeSpan span = {0};
	if (FAILED(client->lpVtbl->GetAsync(client, make_uri(port, L"/busy"), &sending)) || wait(sending, &error) != Completed ||
		FAILED(sending->lpVtbl->GetResults(sending, &response)) || FAILED(response->lpVtbl->get_Headers(response, &message_headers)) ||
		FAILED(message_headers->lpVtbl->get_RetryAfter(message_headers, &retry)) || !retry || FAILED(retry->lpVtbl->get_Delta(retry, &delta)) ||
		!delta || FAILED(delta->lpVtbl->get_Value(delta, &span)) || span.Duration != 1200000000LL)
		return 60;

	/* A range request, the body read as a buffer. */
	__x_ABI_CWindows_CWeb_CHttp_CIHttpMethod *get = NULL;
	request_headers *range_headers = NULL;
	if (FAILED(methods->lpVtbl->get_Get(methods, &get)) || FAILED(requests->lpVtbl->Create(requests, get, make_uri(port, L"/range"), &request)) ||
		FAILED(request->lpVtbl->get_Headers(request, &range_headers)))
		return 70;
	header = text(L"Range");
	header_value = text(L"bytes=1-3");
	if (FAILED(range_headers->lpVtbl->Append(range_headers, header, header_value)))
		return 71;
	__FIAsyncOperationWithProgress_2_Windows__CStorage__CStreams__CIBuffer_UINT64 *buffering = NULL;
	__x_ABI_CWindows_CStorage_CStreams_CIBuffer *buffer = NULL;
	__x_Windows_CStorage_CStreams_CIBufferByteAccess *access = NULL;
	BYTE *bytes = NULL;
	UINT32 got = 0;
	if (FAILED(client->lpVtbl->SendRequestAsync(client, request, &sending)) || wait(sending, &error) != Completed ||
		FAILED(sending->lpVtbl->GetResults(sending, &response)) || FAILED(response->lpVtbl->get_StatusCode(response, &status)) || status != 206)
		return 72;
	if (FAILED(response->lpVtbl->get_Content(response, &content)) || FAILED(content->lpVtbl->ReadAsBufferAsync(content, &buffering)) ||
		wait(buffering, &error) != Completed || FAILED(buffering->lpVtbl->GetResults(buffering, &buffer)) ||
		FAILED(buffer->lpVtbl->get_Length(buffer, &got)) || got != 3 ||
		FAILED(buffer->lpVtbl->QueryInterface(buffer, &IID___x_Windows_CStorage_CStreams_CIBufferByteAccess, (void **)&access)) ||
		FAILED(access->lpVtbl->Buffer(access, &bytes)) || !same_bytes((char *)bytes, "ell", 3))
		return 73;
	closesocket(listener);
	return 0;
}
