#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <objbase.h>

static void zero(void *address, unsigned size)
{
	for (unsigned index = 0; index < size; ++index) ((BYTE *)address)[index] = 0;
}

static int same(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right) ++left, ++right;
	return *left == *right;
}

static int same_bytes(const char *left, const char *right, int length)
{
	for (int index = 0; index < length; ++index) if (left[index] != right[index]) return 0;
	return 1;
}

static int length_of(const char *text)
{
	int length = 0;
	while (text[length]) ++length;
	return length;
}

static SOCKET listener;

static int send_text(SOCKET peer, const char *text)
{
	return send(peer, text, length_of(text), 0) == length_of(text);
}

/* Answers two connections: a GET with a chunked body and a custom header, then a POST whose body comes back as it was sent. */
static DWORD WINAPI serve(void *unused)
{
	for (int round = 0; round < 2; ++round) {
		SOCKET peer = accept(listener, NULL, NULL);
		if (peer == INVALID_SOCKET) return 1;
		char request[2048];
		int size = 0, head = -1;
		while (head < 0 && size < (int)sizeof(request)) {
			const int got = recv(peer, request + size, sizeof(request) - size, 0);
			if (got <= 0) return 2;
			size += got;
			for (int at = 0; at + 3 < size; ++at)
				if (same_bytes(request + at, "\r\n\r\n", 4)) { head = at + 4; break; }
		}
		/* The request's own User-Agent takes the place of the session's agent: the first request carries it once. */
		int agents = 0, own = 0;
		for (int at = 0; at + 12 < head; ++at)
			if (same_bytes(request + at, "\nUser-Agent:", 12)) ++agents, own = same_bytes(request + at + 12, " own\r", 5);
		if (!round && (agents != 1 || !own)) {
			send_text(peer, "HTTP/1.1 500 Agents\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		} else if (!round) {
			send_text(peer, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-Raven: yes\r\nConnection: close\r\n\r\n"
				"5\r\nHello\r\n7\r\n, world\r\n0\r\n\r\n");
		} else {
			while (size - head < 10) {
				const int got = recv(peer, request + size, sizeof(request) - size, 0);
				if (got <= 0) return 3;
				size += got;
			}
			send_text(peer, "HTTP/1.1 201 Created\r\nContent-Length: 10\r\nConnection: close\r\n\r\n");
			send(peer, request + head, 10, 0);
		}
		closesocket(peer);
	}
	return 0;
}

static SOCKET kept_listener;

/* Reads one request head; 0 when the connection ends first. */
static int read_request(SOCKET peer)
{
	char request[2048];
	int size = 0;
	for (;;) {
		const int got = recv(peer, request + size, sizeof(request) - size, 0);
		if (got <= 0) return 0;
		size += got;
		for (int at = 0; at + 3 < size; ++at)
			if (same_bytes(request + at, "\r\n\r\n", 4)) return 1;
		if (size == (int)sizeof(request)) return 0;
	}
}

/* Answers three requests on one kept connection, then closes it and answers a fourth on a new one: a client that opens a connection for
 * each request has the second one refused. */
static DWORD WINAPI serve_kept(void *unused)
{
	SOCKET peer = accept(kept_listener, NULL, NULL);
	if (peer == INVALID_SOCKET) return 10;
	for (int round = 0; round < 3; ++round) {
		if (round) {
			fd_set readable;
			FD_ZERO(&readable);
			FD_SET(peer, &readable);
			TIMEVAL wait = {5, 0};
			if (select(0, &readable, NULL, NULL, &wait) != 1) {
				closesocket(kept_listener);
				return 11 + round;
			}
		}
		if (!read_request(peer)) return 14 + round;
		if (!send_text(peer, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nkept")) return 17;
	}
	closesocket(peer);
	peer = accept(kept_listener, NULL, NULL);
	if (peer == INVALID_SOCKET || !read_request(peer)) return 18;
	send_text(peer, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nfresh");
	closesocket(peer);
	return 0;
}

static SOCKET async_listener;

/* Answers one request with a short body, for the asynchronous client. */
static DWORD WINAPI serve_async(void *unused)
{
	SOCKET peer = accept(async_listener, NULL, NULL);
	if (peer == INVALID_SOCKET || !read_request(peer)) return 20;
	send_text(peer, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nasync!");
	closesocket(peer);
	return 0;
}

/* What the status callback saw: each notification in order, with the context and information it came with. */
static DWORD seen[16];
static int seen_count, wrong_context;
static DWORD last_length, error_api, error_code;
static HINTERNET closing;
static char *read_buffer;

static void CALLBACK status_callback(HINTERNET handle, DWORD_PTR context, DWORD status, void *information, DWORD length)
{
	if (context != 0x1234) ++wrong_context;
	if (seen_count < 16) seen[seen_count++] = status;
	if (status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE || status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE)
		last_length = *(DWORD *)information;
	else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
		last_length = information == read_buffer ? length : 0xffffffff;
	else if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
		closing = *(HINTERNET *)information;
	else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
		error_api = (DWORD)((WINHTTP_ASYNC_RESULT *)information)->dwResult;
		error_code = ((WINHTTP_ASYNC_RESULT *)information)->dwError;
	}
}

/* In the asynchronous mode every operation reports its completion to the callback, and a failure is a notification, not a result. */
static int asynchronous(USHORT port, USHORT closed_port)
{
	HINTERNET session = WinHttpOpen(L"raven-test", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, WINHTTP_FLAG_ASYNC);
	if (!session) return 90;
	const DWORD wanted = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES;
	if (WinHttpSetStatusCallback(session, status_callback, wanted, 0) != NULL ||
		WinHttpSetStatusCallback(session, status_callback, wanted, 0) != status_callback) return 91;
	HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
	HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", L"/async", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
	if (!request) return 92;
	if (!WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 0x1234) || !WinHttpReceiveResponse(request, NULL)) return 93;
	DWORD available = 0;
	if (!WinHttpQueryDataAvailable(request, &available) || available != 6 || last_length != 6) return 94;
	char body[8];
	read_buffer = body;
	if (!WinHttpReadData(request, body, 6, NULL) || last_length != 6 || !same_bytes(body, "async!", 6)) return 95;
	DWORD_PTR value = 0;
	DWORD size = sizeof(value);
	if (!WinHttpQueryOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &value, &size) || value != 0x1234) return 96;
	if (!WinHttpCloseHandle(request) || closing != request) return 97;
	const DWORD expected[] = {WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,
		WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING};
	if (seen_count != 5 || wrong_context) return 98;
	for (int index = 0; index < 5; ++index)
		if (seen[index] != expected[index]) return 99;
	WinHttpCloseHandle(connection);
	/* Nothing listens on the second port: the send still returns TRUE and the error arrives as a notification. */
	seen_count = 0;
	connection = WinHttpConnect(session, L"127.0.0.1", closed_port, 0);
	request = connection ? WinHttpOpenRequest(connection, L"GET", L"/none", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
	if (!request || !WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 0x1234)) return 110;
	if (seen_count != 1 || seen[0] != WINHTTP_CALLBACK_STATUS_REQUEST_ERROR || error_api != API_SEND_REQUEST || !error_code) return 111;
	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connection);
	WinHttpCloseHandle(session);
	return 0;
}

static int read_all(HINTERNET request, char *buffer, DWORD capacity, DWORD *total)
{
	*total = 0;
	for (;;) {
		DWORD available = 0, read = 0;
		if (!WinHttpQueryDataAvailable(request, &available)) return 0;
		if (!available) return 1;
		if (*total + available > capacity) return 0;
		if (!WinHttpReadData(request, buffer + *total, available, &read) || read != available) return 0;
		*total += read;
	}
}

static int exchange(USHORT port)
{
	HINTERNET session = WinHttpOpen(L"raven-test", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
	if (!session) return 30;
	HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
	if (!connection) return 31;
	/* A call that succeeds clears what an earlier one left as the last error. */
	SetLastError(ERROR_INSUFFICIENT_BUFFER);
	HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/greeting?x=1", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
	if (!request || GetLastError()) return 32;
	if (!WinHttpAddRequestHeaders(request, L"X-Asked: 1\r\nUser-Agent: own\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
		return 33;
	if (!WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 0) || !WinHttpReceiveResponse(request, NULL)) return 34;
	DWORD status = 0, size = sizeof(status);
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL) || status != 200) return 35;
	WCHAR value[16];
	size = 0;
	if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"x-raven", NULL, &size, NULL) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || size != 8) return 36;
	size = sizeof(value);
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"X-Raven", value, &size, NULL) || size != 6 || !same(value, L"yes")) return 37;
	if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"X-Absent", value, &size, NULL) || GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) return 38;
	DWORD type = 0;
	size = sizeof(type);
	if (!WinHttpQueryOption(request, WINHTTP_OPTION_HANDLE_TYPE, &type, &size) || type != WINHTTP_HANDLE_TYPE_REQUEST) return 39;
	char body[64];
	DWORD total = 0;
	if (!read_all(request, body, sizeof(body), &total) || total != 12 || !same_bytes(body, "Hello, world", 12)) return 40;
	WinHttpCloseHandle(request);
	/* A body that comes partly with the send and partly through WinHttpWriteData. */
	request = WinHttpOpenRequest(connection, L"POST", L"/echo", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
	if (!request) return 41;
	DWORD written = 0;
	if (!WinHttpSendRequest(request, L"Content-Type: text/plain", (DWORD)-1, "01234", 5, 10, 0) ||
		!WinHttpWriteData(request, "56789", 5, &written) || written != 5 || !WinHttpReceiveResponse(request, NULL)) return 42;
	size = sizeof(status);
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL) || status != 201) return 43;
	if (!read_all(request, body, sizeof(body), &total) || total != 10 || !same_bytes(body, "0123456789", 10)) return 44;
	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connection);
	if (!WinHttpCloseHandle(session) || WinHttpCloseHandle(session) || GetLastError() != ERROR_INVALID_HANDLE) return 45;
	return 0;
}

/* Requests to one server reuse its kept connection, and one the server closed meanwhile is replaced. */
static int kept(USHORT port)
{
	HINTERNET session = WinHttpOpen(L"raven-test", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
	HINTERNET connection = session ? WinHttpConnect(session, L"127.0.0.1", port, 0) : NULL;
	if (!connection) return 80;
	for (int round = 0; round < 4; ++round) {
		HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/range", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
		if (!request || !WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 0) || !WinHttpReceiveResponse(request, NULL)) return 81 + round;
		char body[16];
		DWORD total = 0;
		if (!read_all(request, body, sizeof(body), &total)) return 85;
		if (round < 3 ? total != 4 || !same_bytes(body, "kept", 4) : total != 5 || !same_bytes(body, "fresh", 5)) return 86 + round;
		WinHttpCloseHandle(request);
	}
	WinHttpCloseHandle(session);
	return 0;
}

static int cracked(void)
{
	const WCHAR *url = L"https://user:secret@example.com:8443/path/file.txt?query=1#top";
	URL_COMPONENTS parts;
	zero(&parts, sizeof(parts));
	parts.dwStructSize = sizeof(parts);
	parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwUserNameLength = (DWORD)-1;
	if (!WinHttpCrackUrl(url, 0, 0, &parts)) return 50;
	if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != 8443 || parts.lpszHostName != url + 20 || parts.dwHostNameLength != 11 ||
		parts.dwUrlPathLength != 14 || parts.dwExtraInfoLength != 12 || parts.dwUserNameLength != 4 || parts.lpszUserName != url + 8) return 51;
	WCHAR host[32], path[4];
	zero(&parts, sizeof(parts));
	parts.dwStructSize = sizeof(parts);
	parts.lpszHostName = host;
	parts.dwHostNameLength = 32;
	if (!WinHttpCrackUrl(L"http://raven.example/", 0, 0, &parts) || !same(host, L"raven.example") || parts.dwHostNameLength != 13 || parts.nPort != 80) return 52;
	parts.dwHostNameLength = 32;
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = 4;
	if (WinHttpCrackUrl(L"http://raven.example/long/path", 0, 0, &parts) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return 53;
	/* Without the extra information asked for, the query stays on the path. */
	zero(&parts, sizeof(parts));
	parts.dwStructSize = sizeof(parts);
	parts.dwUrlPathLength = (DWORD)-1;
	if (!WinHttpCrackUrl(L"https://raven.example/versions/latest?action=select", 0, 0, &parts) || parts.dwUrlPathLength != 30) return 55;
	if (WinHttpCrackUrl(L"ftp://raven.example/", 0, 0, &parts) || GetLastError() != ERROR_WINHTTP_UNRECOGNIZED_SCHEME) return 54;
	return 0;
}

static int names(void)
{
	WCHAR ascii[64], unicode[64];
	/* The example of RFC 3492's Punycode and its reverse. */
	const int length = IdnToAscii(0, L"B\x00fc" L"cher.example", -1, ascii, 64);
	if (length != 22 || !same(ascii, L"xn--bcher-kva.example")) return 60;
	if (IdnToUnicode(0, ascii, -1, unicode, 64) != 15 || !same(unicode, L"b\x00fc" L"cher.example")) return 61;
	if (IdnToAscii(0, L"plain.example", 13, ascii, 64) != 13) return 62;
	if (IdnToAscii(0, L"a..b", -1, ascii, 64) || GetLastError() != ERROR_INVALID_NAME) return 63;
	if (IdnToAscii(0, L"b\x00fc" L"cher", -1, NULL, 0) != 14 || IdnToAscii(0, L"b\x00fc" L"cher", -1, ascii, 4) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return 64;
	return 0;
}

static int allocator(void)
{
	IMalloc *task = NULL, *again = NULL;
	if (CoGetMalloc(MEMCTX_TASK, &task) != S_OK || !task) return 70;
	if (CoGetMalloc(2, &again) != E_INVALIDARG || again) return 71;
	if (task->lpVtbl->QueryInterface(task, &IID_IMalloc, (void **)&again) != S_OK || again != task) return 72;
	char *block = task->lpVtbl->Alloc(task, 32);
	if (!block) return 73;
	block[31] = 7;
	block = task->lpVtbl->Realloc(task, block, 64);
	if (!block || block[31] != 7) return 74;
	CoTaskMemFree(block);
	block = CoTaskMemAlloc(16);
	task->lpVtbl->Free(task, block);
	task->lpVtbl->Release(task);
	return 0;
}

int raven_entry(void)
{
	int result = names();
	if (result) return result;
	result = cracked();
	if (result) return result;
	result = allocator();
	if (result) return result;
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SOCKADDR_IN address;
	zero(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int address_size = sizeof(address);
	if (listener == INVALID_SOCKET || bind(listener, (SOCKADDR *)&address, sizeof(address)) || listen(listener, 2) ||
		getsockname(listener, (SOCKADDR *)&address, &address_size)) return 2;
	HANDLE server = CreateThread(NULL, 0, serve, NULL, 0, NULL);
	if (!server) return 3;
	result = exchange(ntohs(address.sin_port));
	if (result) return result;
	if (WaitForSingleObject(server, 10000) != WAIT_OBJECT_0) return 4;
	DWORD code = 1;
	if (!GetExitCodeThread(server, &code) || code) return 5;
	closesocket(listener);
	kept_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	address.sin_port = 0;
	address_size = sizeof(address);
	if (kept_listener == INVALID_SOCKET || bind(kept_listener, (SOCKADDR *)&address, sizeof(address)) || listen(kept_listener, 4) ||
		getsockname(kept_listener, (SOCKADDR *)&address, &address_size)) return 6;
	server = CreateThread(NULL, 0, serve_kept, NULL, 0, NULL);
	if (!server) return 7;
	const int reused = kept(ntohs(address.sin_port));
	if (WaitForSingleObject(server, 10000) != WAIT_OBJECT_0) return 8;
	code = 1;
	if (!GetExitCodeThread(server, &code) || code) return 100 + code;
	if (reused) return reused;
	closesocket(kept_listener);
	/* A port that was listening a moment ago and no longer is refuses the connection. */
	SOCKET gone = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	address.sin_port = 0;
	address_size = sizeof(address);
	if (gone == INVALID_SOCKET || bind(gone, (SOCKADDR *)&address, sizeof(address)) || getsockname(gone, (SOCKADDR *)&address, &address_size)) return 9;
	const USHORT closed_port = ntohs(address.sin_port);
	closesocket(gone);
	async_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	address.sin_port = 0;
	address_size = sizeof(address);
	if (async_listener == INVALID_SOCKET || bind(async_listener, (SOCKADDR *)&address, sizeof(address)) || listen(async_listener, 1) ||
		getsockname(async_listener, (SOCKADDR *)&address, &address_size)) return 10;
	server = CreateThread(NULL, 0, serve_async, NULL, 0, NULL);
	if (!server) return 11;
	result = asynchronous(ntohs(address.sin_port), closed_port);
	if (result) return result;
	if (WaitForSingleObject(server, 10000) != WAIT_OBJECT_0) return 12;
	code = 1;
	if (!GetExitCodeThread(server, &code) || code) return 120 + code;
	closesocket(async_listener);
	WSACleanup();
	return 0;
}
