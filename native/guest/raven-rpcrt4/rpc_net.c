/* SPDX-License-Identifier: MIT */
#include "rpc_own.h"
#include <winsock2.h>
#include <ws2tcpip.h>

// A connection is a stream of bytes: a named pipe for local and named pipe sequences, a socket for TCP.
// The connection-oriented PDUs of DCE RPC run over all of them, as C706 and MS-RPCE describe.

typedef struct socket_api
{
	int (WSAAPI *wsa_startup)(WORD, WSADATA *);
	SOCKET (WSAAPI *open)(int, int, int);
	int (WSAAPI *connect)(SOCKET, const struct sockaddr *, int);
	int (WSAAPI *close)(SOCKET);
	int (WSAAPI *send)(SOCKET, const char *, int, int);
	int (WSAAPI *recv)(SOCKET, char *, int, int);
	int (WSAAPI *getaddrinfo)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
	void (WSAAPI *freeaddrinfo)(struct addrinfo *);
	int (WSAAPI *bind)(SOCKET, const struct sockaddr *, int);
	int (WSAAPI *listen)(SOCKET, int);
	SOCKET (WSAAPI *accept)(SOCKET, struct sockaddr *, int *);
	int (WSAAPI *getsockname)(SOCKET, struct sockaddr *, int *);
} socket_api;

static const socket_api *sockets(void)
{
	static socket_api api;
	static LONG ready;
	if (ready)
		return &api;
	HMODULE library = LoadLibraryW(L"ws2_32.dll");
	if (!library)
		return NULL;
	api.wsa_startup = (void *)GetProcAddress(library, "WSAStartup");
	api.open = (void *)GetProcAddress(library, "socket");
	api.connect = (void *)GetProcAddress(library, "connect");
	api.close = (void *)GetProcAddress(library, "closesocket");
	api.send = (void *)GetProcAddress(library, "send");
	api.recv = (void *)GetProcAddress(library, "recv");
	api.getaddrinfo = (void *)GetProcAddress(library, "getaddrinfo");
	api.freeaddrinfo = (void *)GetProcAddress(library, "freeaddrinfo");
	api.bind = (void *)GetProcAddress(library, "bind");
	api.listen = (void *)GetProcAddress(library, "listen");
	api.accept = (void *)GetProcAddress(library, "accept");
	api.getsockname = (void *)GetProcAddress(library, "getsockname");
	if (!api.wsa_startup || !api.open || !api.connect || !api.close || !api.send || !api.recv || !api.getaddrinfo || !api.freeaddrinfo || !api.bind || !api.listen || !api.accept || !api.getsockname)
		return NULL;
	WSADATA data;
	if (api.wsa_startup(MAKEWORD(2, 2), &data))
		return NULL;
	InterlockedExchange(&ready, 1);
	return &api;
}

BOOL stream_read(rpc_stream *stream, void *data, ULONG size)
{
	BYTE *at = data;
	while (size) {
		DWORD got = 0;
		if (stream->is_socket) {
			int count = sockets()->recv((SOCKET)stream->socket, (char *)at, size > 0x10000 ? 0x10000 : (int)size, 0);
			if (count <= 0)
				return FALSE;
			got = (DWORD)count;
		} else if (!ReadFile(stream->pipe, at, size, &got, NULL) && GetLastError() != ERROR_MORE_DATA) {
			return FALSE;
		}
		if (!got)
			return FALSE;
		at += got;
		size -= got;
	}
	return TRUE;
}

BOOL stream_write(rpc_stream *stream, const void *data, ULONG size)
{
	const BYTE *at = data;
	while (size) {
		DWORD put = 0;
		if (stream->is_socket) {
			int count = sockets()->send((SOCKET)stream->socket, (const char *)at, size > 0x10000 ? 0x10000 : (int)size, 0);
			if (count <= 0)
				return FALSE;
			put = (DWORD)count;
		} else if (!WriteFile(stream->pipe, at, size, &put, NULL)) {
			return FALSE;
		}
		at += put;
		size -= put;
	}
	return TRUE;
}

void stream_close(rpc_stream *stream)
{
	if (stream->is_socket)
		sockets()->close((SOCKET)stream->socket);
	else if (stream->pipe)
		CloseHandle(stream->pipe);
	stream->pipe = NULL;
	stream->socket = 0;
}

BOOL protseq_supported(const WCHAR *protseq)
{
	return !lstrcmpW(protseq, L"ncalrpc") || !lstrcmpW(protseq, L"ncacn_np") || !lstrcmpW(protseq, L"ncacn_ip_tcp");
}

static RPC_STATUS connect_pipe(rpc_stream *stream, const WCHAR *name)
{
	for (INT attempt = 0; ; attempt++) {
		HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (pipe != INVALID_HANDLE_VALUE) {
			stream->pipe = pipe;
			stream->is_socket = FALSE;
			return RPC_S_OK;
		}
		DWORD error = GetLastError();
		if (error != ERROR_PIPE_BUSY || attempt == 8)
			return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_PIPE_BUSY ? RPC_S_SERVER_UNAVAILABLE : (error == ERROR_ACCESS_DENIED ? RPC_S_ACCESS_DENIED : RPC_S_SERVER_UNAVAILABLE);
		WaitNamedPipeW(name, 250);
	}
}

static void rpc_pipe_name(WCHAR *out, SIZE_T capacity, const WCHAR *protseq, const WCHAR *network, const WCHAR *endpoint)
{
	static const WCHAR local_prefix[] = L"\\\\.\\pipe\\lrpc\\";
	SIZE_T used = 0;
	#define APPEND(text) do { const WCHAR *from = (text); while (*from && used + 1 < capacity) out[used++] = *from++; } while (0)
	if (!lstrcmpW(protseq, L"ncalrpc")) {
		APPEND(local_prefix);
		APPEND(endpoint);
	} else {
		// The server part may come with its own leading backslashes.
		const WCHAR *server = network && *network ? network : L".";
		while (*server == L'\\')
			server++;
		APPEND(L"\\\\");
		APPEND(server);
		APPEND(endpoint);
	}
	#undef APPEND
	out[used] = 0;
}

RPC_STATUS stream_connect(rpc_stream *stream, const WCHAR *protseq, const WCHAR *network, const WCHAR *endpoint)
{
	memset(stream, 0, sizeof(*stream));
	if (!protseq_supported(protseq))
		return RPC_S_PROTSEQ_NOT_SUPPORTED;
	if (!endpoint || !*endpoint)
		return RPC_S_SERVER_UNAVAILABLE;
	if (lstrcmpW(protseq, L"ncacn_ip_tcp")) {
		WCHAR name[MAX_PATH];
		rpc_pipe_name(name, MAX_PATH, protseq, network, endpoint);
		return connect_pipe(stream, name);
	}
	const socket_api *api = sockets();
	if (!api)
		return RPC_S_PROTSEQ_NOT_SUPPORTED;
	char *host = rpc_narrow(network && *network ? network : L"localhost"), *port = rpc_narrow(endpoint);
	struct addrinfo hints, *found = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	RPC_STATUS status = RPC_S_SERVER_UNAVAILABLE;
	if (!api->getaddrinfo(host, port, &hints, &found)) {
		for (struct addrinfo *at = found; at; at = at->ai_next) {
			SOCKET handle = api->open(at->ai_family, at->ai_socktype, at->ai_protocol);
			if (handle == INVALID_SOCKET)
				continue;
			if (!api->connect(handle, at->ai_addr, (int)at->ai_addrlen)) {
				stream->socket = handle;
				stream->is_socket = TRUE;
				status = RPC_S_OK;
				break;
			}
			api->close(handle);
		}
		api->freeaddrinfo(found);
	}
	rpc_free(host);
	rpc_free(port);
	return status;
}

// ---- the listening side ----

static HANDLE pipe_instance(const WCHAR *name)
{
	return CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 0x10000, 0x10000, 5000, NULL);
}

RPC_STATUS listener_open(rpc_listener *listener, const WCHAR *protseq, const WCHAR *endpoint, WCHAR **resolved)
{
	memset(listener, 0, sizeof(*listener));
	if (lstrcmpW(protseq, L"ncacn_ip_tcp")) {
		rpc_pipe_name(listener->pipe_name, MAX_PATH, protseq, NULL, endpoint);
		listener->pending = pipe_instance(listener->pipe_name);
		if (listener->pending == INVALID_HANDLE_VALUE)
			return GetLastError() == ERROR_ACCESS_DENIED ? RPC_S_DUPLICATE_ENDPOINT : RPC_S_CANT_CREATE_ENDPOINT;
		*resolved = rpc_copy(endpoint);
		return RPC_S_OK;
	}
	const socket_api *api = sockets();
	if (!api)
		return RPC_S_PROTSEQ_NOT_SUPPORTED;
	char *port = rpc_narrow(endpoint ? endpoint : L"0");
	struct addrinfo hints, *found = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;
	INT failed = api->getaddrinfo(NULL, port, &hints, &found);
	rpc_free(port);
	if (failed)
		return RPC_S_INVALID_ENDPOINT_FORMAT;
	SOCKET handle = api->open(found->ai_family, found->ai_socktype, found->ai_protocol);
	RPC_STATUS status = RPC_S_OK;
	if (handle == INVALID_SOCKET)
		status = RPC_S_CANT_CREATE_ENDPOINT;
	else if (api->bind(handle, found->ai_addr, (int)found->ai_addrlen) || api->listen(handle, SOMAXCONN)) {
		api->close(handle);
		status = RPC_S_DUPLICATE_ENDPOINT;
	}
	api->freeaddrinfo(found);
	if (status != RPC_S_OK)
		return status;
	struct sockaddr_in address;
	int size = sizeof(address);
	if (api->getsockname(handle, (struct sockaddr *)&address, &size)) {
		api->close(handle);
		return RPC_S_CANT_CREATE_ENDPOINT;
	}
	WCHAR number[8];
	wsprintfW(number, L"%u", (UINT)(address.sin_port >> 8 | (address.sin_port & 0xff) << 8));
	listener->is_socket = TRUE;
	listener->socket = handle;
	*resolved = rpc_copy(number);
	return RPC_S_OK;
}

BOOL listener_accept(rpc_listener *listener, rpc_stream *stream)
{
	memset(stream, 0, sizeof(*stream));
	if (listener->is_socket) {
		SOCKET handle = sockets()->accept((SOCKET)listener->socket, NULL, NULL);
		if (handle == INVALID_SOCKET)
			return FALSE;
		stream->is_socket = TRUE;
		stream->socket = handle;
		return TRUE;
	}
	HANDLE pipe = listener->pending;
	if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
		return FALSE;
	stream->pipe = pipe;
	// The next client finds an instance waiting before this one starts its work.
	listener->pending = pipe_instance(listener->pipe_name);
	return TRUE;
}

// A blocked accept only comes back for a client, so it is given one that goes away at once.
void listener_wake(rpc_listener *listener)
{
	if (listener->is_socket) {
		struct sockaddr_in address;
		int size = sizeof(address);
		const socket_api *api = sockets();
		if (api->getsockname((SOCKET)listener->socket, (struct sockaddr *)&address, &size))
			return;
		address.sin_addr.s_addr = 0x0100007f;
		SOCKET handle = api->open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (handle != INVALID_SOCKET) {
			api->connect(handle, (struct sockaddr *)&address, size);
			api->close(handle);
		}
		return;
	}
	HANDLE pipe = CreateFileW(listener->pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (pipe != INVALID_HANDLE_VALUE)
		CloseHandle(pipe);
}

// Nobody is listening now, so no instance of the pipe should answer a client.
void listener_pause(rpc_listener *listener)
{
	if (!listener->is_socket && listener->pending && listener->pending != INVALID_HANDLE_VALUE) {
		CloseHandle(listener->pending);
		listener->pending = NULL;
	}
}

void listener_resume(rpc_listener *listener)
{
	if (!listener->is_socket && !listener->pending)
		listener->pending = pipe_instance(listener->pipe_name);
}

// ---- PDUs ----

#pragma pack(push, 1)
typedef struct pdu_header
{
	BYTE version, minor, type, flags, drep[4];
	USHORT frag_length, auth_length;
	ULONG call_id;
} pdu_header;
#pragma pack(pop)

BOOL pdu_read(rpc_stream *stream, rpc_pdu *pdu)
{
	pdu_header header;
	if (!stream_read(stream, &header, sizeof(header)))
		return FALSE;
	if (header.version != 5 || header.frag_length < sizeof(header))
		return FALSE;
	pdu->type = header.type;
	pdu->flags = header.flags;
	memcpy(pdu->drep, header.drep, 4);
	pdu->frag_length = header.frag_length;
	pdu->auth_length = header.auth_length;
	pdu->call_id = header.call_id;
	ULONG size = header.frag_length - sizeof(header);
	pdu->body = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
	if (!pdu->body)
		return FALSE;
	if (!stream_read(stream, pdu->body, size)) {
		HeapFree(GetProcessHeap(), 0, pdu->body);
		return FALSE;
	}
	return TRUE;
}

BOOL pdu_write(rpc_stream *stream, BYTE type, BYTE flags, ULONG call_id, const void *head, ULONG head_size, const void *data, ULONG data_size)
{
	ULONG total = sizeof(pdu_header) + head_size + data_size;
	BYTE *packet = HeapAlloc(GetProcessHeap(), 0, total);
	if (!packet)
		return FALSE;
	pdu_header *header = (pdu_header *)packet;
	header->version = 5;
	header->minor = 0;
	header->type = type;
	header->flags = flags;
	header->drep[0] = 0x10;
	header->drep[1] = header->drep[2] = header->drep[3] = 0;
	header->frag_length = (USHORT)total;
	header->auth_length = 0;
	header->call_id = call_id;
	if (head_size)
		memcpy(packet + sizeof(*header), head, head_size);
	if (data_size)
		memcpy(packet + sizeof(*header) + head_size, data, data_size);
	BOOL written = stream_write(stream, packet, total);
	HeapFree(GetProcessHeap(), 0, packet);
	return written;
}
