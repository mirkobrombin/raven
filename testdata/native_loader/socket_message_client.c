#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <ws2tcpip.h>

static void zero(void *address, unsigned size)
{
	for (unsigned index = 0; index < size; ++index) ((BYTE *)address)[index] = 0;
}

static int received(SOCKET socket, int length)
{
	char bytes[32];
	const char expected[] = "first-second";
	if (recv(socket, bytes, sizeof(bytes), 0) != length) return 0;
	for (int index = 0; index < length; ++index) if (bytes[index] != expected[index]) return 0;
	return 1;
}

static int rejected(LPFN_WSASENDMSG send_message, SOCKET socket, WSAMSG *message, DWORD flags, int error)
{
	DWORD sent = 0xcccccccc;
	return send_message(socket, message, flags, &sent, NULL, NULL) == SOCKET_ERROR &&
		WSAGetLastError() == error && sent == 0xcccccccc;
}

static int datagrams(int family)
{
	SOCKET receiver = socket(family, SOCK_DGRAM, IPPROTO_UDP);
	SOCKET sender = WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_STORAGE address;
	int address_size = family == AF_INET ? sizeof(SOCKADDR_IN) : sizeof(SOCKADDR_IN6);
	zero(&address, sizeof(address));
	address.ss_family = (USHORT)family;
	if (family == AF_INET) ((SOCKADDR_IN *)&address)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	else ((SOCKADDR_IN6 *)&address)->sin6_addr.s6_addr[15] = 1;
	if (receiver == INVALID_SOCKET || sender == INVALID_SOCKET ||
		bind(receiver, (SOCKADDR *)&address, address_size) || getsockname(receiver, (SOCKADDR *)&address, &address_size)) return 10;
	LPFN_WSASENDMSG send_message = NULL;
	GUID guid = WSAID_WSASENDMSG;
	DWORD returned = 0;
	if (WSAIoctl(sender, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &send_message,
		sizeof(send_message), &returned, NULL, NULL) || !send_message || returned != sizeof(send_message) ||
		(FARPROC)send_message != GetProcAddress(GetModuleHandleW(L"ws2_32.dll"), "WSASendMsg")) return 11;
	WSABUF buffers[2] = {{6, "first-"}, {6, "second"}};
	WSAMSG message;
	zero(&message, sizeof(message));
	message.name = (SOCKADDR *)&address;
	message.namelen = address_size;
	message.lpBuffers = buffers;
	message.dwBufferCount = 2;
	message.dwFlags = 0xdeadbeef;
	DWORD sent = 0;
	if (send_message(sender, &message, 0, &sent, NULL, NULL) || sent != 12 ||
		message.dwFlags != 0xdeadbeef || !received(receiver, 12)) return 12;
	LPFN_WSARECVMSG receive_message = NULL;
	GUID receive_guid = WSAID_WSARECVMSG;
	char bytes[16];
	SOCKADDR_STORAGE source;
	WSABUF receive_buffer = {sizeof(bytes), bytes};
	WSAMSG incoming;
	zero(&incoming, sizeof(incoming));
	incoming.name = (SOCKADDR *)&source;
	incoming.namelen = sizeof(source);
	incoming.lpBuffers = &receive_buffer;
	incoming.dwBufferCount = 1;
	DWORD read = 0;
	if (WSAIoctl(receiver, SIO_GET_EXTENSION_FUNCTION_POINTER, &receive_guid, sizeof(receive_guid), &receive_message,
		sizeof(receive_message), &returned, NULL, NULL) || !receive_message ||
		send_message(sender, &message, 0, &sent, NULL, NULL) || receive_message(receiver, &incoming, &read, NULL, NULL) ||
		read != 12 || bytes[6] != 's' || source.ss_family != family || incoming.Control.len) return 33;
	union { WSACMSGHDR aligned; char bytes[128]; } control;
	zero(&control, sizeof(control));
	message.Control.buf = control.bytes;
	const unsigned packet_size = family == AF_INET ? sizeof(IN_PKTINFO) : sizeof(IN6_PKTINFO);
	message.Control.len = WSA_CMSG_SPACE(packet_size);
	WSACMSGHDR *header = WSA_CMSG_FIRSTHDR(&message);
	header->cmsg_len = WSA_CMSG_LEN(packet_size);
	header->cmsg_level = family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
	header->cmsg_type = family == AF_INET ? IP_PKTINFO : IPV6_PKTINFO;
	if (family == AF_INET) ((IN_PKTINFO *)WSA_CMSG_DATA(header))->ipi_addr = ((SOCKADDR_IN *)&address)->sin_addr;
	else ((IN6_PKTINFO *)WSA_CMSG_DATA(header))->ipi6_addr = ((SOCKADDR_IN6 *)&address)->sin6_addr;
	if (send_message(sender, &message, 0, &sent, NULL, NULL) || sent != 12 || !received(receiver, 12)) return 13;
	zero(&control, sizeof(control));
	message.Control.len = WSA_CMSG_SPACE(sizeof(INT));
	header->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
	header->cmsg_level = family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
	header->cmsg_type = family == AF_INET ? IP_ECN : IPV6_ECN;
	*(INT *)WSA_CMSG_DATA(header) = 2;
	if (send_message(sender, &message, 0, &sent, NULL, NULL) || !received(receiver, 12)) return 14;
	message.Control.len = 0;
	message.Control.buf = NULL;
	OVERLAPPED overlapped;
	zero(&overlapped, sizeof(overlapped));
	overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE port = CreateIoCompletionPort((HANDLE)sender, NULL, 0x1234, 0);
	if (!overlapped.hEvent || !port || send_message(sender, &message, 0, &sent, &overlapped, NULL) ||
		sent != 12 || WaitForSingleObject(overlapped.hEvent, 1000) != WAIT_OBJECT_0 || !received(receiver, 12)) return 15;
	ULONG_PTR key = 0;
	OVERLAPPED *completion = NULL;
	DWORD flags = 99;
	if (!GetQueuedCompletionStatus(port, &sent, &key, &completion, 1000) || sent != 12 || key != 0x1234 ||
		completion != &overlapped || !WSAGetOverlappedResult(sender, &overlapped, &sent, FALSE, &flags) || sent != 12 || flags) return 16;
	CloseHandle(overlapped.hEvent);
	CloseHandle(port);
	if (connect(sender, (SOCKADDR *)&address, address_size)) return 17;
	message.name = NULL;
	message.namelen = 0;
	if (send_message(sender, &message, 0, &sent, NULL, NULL) || !received(receiver, 12)) return 18;
	buffers[0].len = 0;
	message.dwBufferCount = 1;
	if (send_message(sender, &message, 0, &sent, NULL, NULL) || sent || !received(receiver, 0)) return 19;
	buffers[0].len = 6;
	message.dwBufferCount = 2;
	if (!rejected(send_message, INVALID_SOCKET, &message, 0, WSAENOTSOCK) ||
		!rejected(send_message, sender, (WSAMSG *)1, 0, WSAEFAULT) ||
		!rejected(send_message, sender, &message, MSG_PEEK, WSAEOPNOTSUPP)) return 20;
	message.dwBufferCount = 65;
	if (!rejected(send_message, sender, &message, 0, WSAENOBUFS)) return 21;
	message.dwBufferCount = 2;
	buffers[0].buf = (char *)1;
	if (!rejected(send_message, sender, &message, 0, WSAEFAULT)) return 22;
	buffers[0].buf = "first-";
	message.Control.len = 1;
	if (!rejected(send_message, sender, &message, 0, WSAEFAULT)) return 23;
	message.Control.buf = control.bytes;
	message.Control.len = sizeof(WSACMSGHDR);
	header->cmsg_len = sizeof(WSACMSGHDR) - 1;
	if (!rejected(send_message, sender, &message, 0, WSAEINVAL)) return 24;
	header->cmsg_len = sizeof(WSACMSGHDR);
	header->cmsg_type = 0x1234;
	if (!rejected(send_message, sender, &message, 0, WSAENOPROTOOPT)) return 25;
	message.Control.len = 0;
	if (send_message(sender, &message, 0, NULL, NULL, NULL) != SOCKET_ERROR || WSAGetLastError() != WSAEFAULT) return 26;
	message.name = NULL;
	message.namelen = address_size;
	if (!rejected(send_message, sender, &message, 0, WSAEFAULT)) return 27;
	message.namelen = 0;
	if (send_message(sender, &message, 0, &sent, NULL, (LPWSAOVERLAPPED_COMPLETION_ROUTINE)(ULONG_PTR)1) != SOCKET_ERROR ||
		WSAGetLastError() != WSAEOPNOTSUPP) return 29;
	SOCKET stream = socket(family, SOCK_STREAM, IPPROTO_TCP);
	if (stream == INVALID_SOCKET || !rejected(send_message, stream, &message, 0, WSAEOPNOTSUPP)) return 30;
	closesocket(stream);
	DWORD *protected_out = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), protection;
	if (!protected_out) return 31;
	*protected_out = 0xcccccccc;
	if (!VirtualProtect(protected_out, 4096, PAGE_READONLY, &protection) ||
		send_message(sender, &message, 0, protected_out, NULL, NULL) != SOCKET_ERROR || WSAGetLastError() != WSAEFAULT ||
		*protected_out != 0xcccccccc || !VirtualFree(protected_out, 0, MEM_RELEASE)) return 32;
	ULONG nonblocking = 1;
	char byte;
	if (ioctlsocket(receiver, FIONBIO, &nonblocking) || recv(receiver, &byte, 1, 0) != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK) return 28;
	closesocket(sender);
	closesocket(receiver);
	return 0;
}

static int network_event(SOCKET socket, HANDLE event, long expected)
{
	WSANETWORKEVENTS events;
	if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0 || WSAEnumNetworkEvents(socket, event, &events) ||
		events.lNetworkEvents != expected) return 0;
	for (unsigned index = 0; index < FD_MAX_EVENTS; ++index)
		if (events.iErrorCode[index]) return 0;
	return 1;
}

static int selected_events(void)
{
	SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP), client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SOCKADDR_IN address;
	zero(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int size = sizeof(address);
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!event || listener == INVALID_SOCKET || client == INVALID_SOCKET || bind(listener, (SOCKADDR *)&address, size) ||
		getsockname(listener, (SOCKADDR *)&address, &size) || listen(listener, 2) || WSAEventSelect(listener, event, FD_ACCEPT) ||
		connect(client, (SOCKADDR *)&address, size) || !network_event(listener, event, FD_ACCEPT)) return 40;
	SOCKET server = accept(listener, NULL, NULL);
	closesocket(listener);
	if (server == INVALID_SOCKET || WSAEventSelect(server, event, FD_READ | FD_WRITE | FD_CLOSE) ||
		!network_event(server, event, FD_WRITE)) return 41;
	WSANETWORKEVENTS events;
	for (unsigned index = 0; index < 1000; ++index) {
		Sleep(0);
		if (WSAEnumNetworkEvents(server, event, &events) || events.lNetworkEvents ||
			WaitForSingleObject(event, 0) != WAIT_TIMEOUT) return 42;
	}
	if (send(client, "ab", 2, 0) != 2 || !network_event(server, event, FD_READ) ||
		WSAEnumNetworkEvents(server, event, &events) || events.lNetworkEvents) return 43;
	char byte;
	if (recv(server, &byte, 1, 0) != 1 || byte != 'a' || !network_event(server, event, FD_READ) ||
		recv(server, &byte, 1, 0) != 1 || byte != 'b') return 44;
	if (WSAEventSelect(server, NULL, 0) || send(client, "c", 1, 0) != 1) return 45;
	Sleep(10);
	if (WaitForSingleObject(event, 0) != WAIT_TIMEOUT || WSAEnumNetworkEvents(server, event, &events) ||
		events.lNetworkEvents || WSAEventSelect(server, event, FD_READ | FD_CLOSE) ||
		!network_event(server, event, FD_READ) || recv(server, &byte, 1, 0) != 1 || byte != 'c') return 46;
	if (closesocket(client) || !network_event(server, event, FD_CLOSE) ||
		WSAEnumNetworkEvents(server, event, &events) || events.lNetworkEvents ||
		WaitForSingleObject(event, 0) != WAIT_TIMEOUT) return 47;
	closesocket(server);
	CloseHandle(event);
	return 0;
}

static int selected_batch(void)
{
	SOCKET sockets[8];
	HANDLE events[8];
	SOCKADDR_IN addresses[8];
	for (unsigned index = 0; index < 8; ++index) {
		zero(&addresses[index], sizeof(addresses[index]));
		addresses[index].sin_family = AF_INET;
		addresses[index].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		int size = sizeof(addresses[index]);
		sockets[index] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		events[index] = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (sockets[index] == INVALID_SOCKET || !events[index] ||
			bind(sockets[index], (SOCKADDR *)&addresses[index], size) ||
			getsockname(sockets[index], (SOCKADDR *)&addresses[index], &size) ||
			WSAEventSelect(sockets[index], events[index], FD_READ)) return 50;
	}
	for (unsigned round = 0; round < 3; ++round) {
		for (unsigned turn = 0; turn < 256; ++turn) Sleep(0);
		for (unsigned index = round; index < 8; ++index)
			if (WaitForSingleObject(events[index], 0) != WAIT_TIMEOUT) return 55;
		for (unsigned index = round; index < 8; ++index)
			if (sendto(sockets[index], "x", 1, 0, (SOCKADDR *)&addresses[index], sizeof(addresses[index])) != 1) return 51;
		if (WaitForMultipleObjects(8 - round, events + round, TRUE, 1000) != WAIT_OBJECT_0) return 52;
		for (unsigned index = round; index < 8; ++index) {
			WSANETWORKEVENTS found;
			char byte;
			if (WSAEnumNetworkEvents(sockets[index], events[index], &found) || found.lNetworkEvents != FD_READ ||
				found.iErrorCode[FD_READ_BIT] || recv(sockets[index], &byte, 1, 0) != 1 || byte != 'x' ||
				WSAEnumNetworkEvents(sockets[index], events[index], &found) || found.lNetworkEvents ||
				WaitForSingleObject(events[index], 0) != WAIT_TIMEOUT) return 53;
		}
		if (WSAEventSelect(sockets[round], NULL, 0) || closesocket(sockets[round]) || !CloseHandle(events[round])) return 54;
	}
	for (unsigned index = 3; index < 8; ++index) {
		closesocket(sockets[index]);
		CloseHandle(events[index]);
	}
	return 0;
}

#ifdef _WIN64
static HANDLE callback_done;
static volatile DWORD callback_error = 0xcccccccc, callback_bytes;
static LPOVERLAPPED volatile callback_operation;

static VOID CALLBACK operation_finished(DWORD error, DWORD bytes, LPOVERLAPPED operation)
{
	callback_error = error;
	callback_bytes = bytes;
	callback_operation = operation;
	SetEvent(callback_done);
}
#endif

/* A server the way Analysis Services is one: an exclusive listening port, connections taken with AcceptEx together with their first
 * data, finished through a completion port. */
static int accepted_with_data(void)
{
	GUID accept_id = WSAID_ACCEPTEX, addresses_id = WSAID_GETACCEPTEXSOCKADDRS;
	LPFN_ACCEPTEX accept_ex = NULL;
	LPFN_GETACCEPTEXSOCKADDRS accept_addresses = NULL;
	SOCKET listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKET accepted = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SOCKADDR_IN address;
	int address_size = sizeof(address);
	DWORD done = 0, exclusive = 1, connect_time = 0;
	int length = sizeof(connect_time);
	if (listener == INVALID_SOCKET || accepted == INVALID_SOCKET || client == INVALID_SOCKET) return 60;
	if (WSAIoctl(listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_id, sizeof(accept_id), &accept_ex, sizeof(accept_ex), &done, NULL, NULL) ||
		WSAIoctl(listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &addresses_id, sizeof(addresses_id), &accept_addresses, sizeof(accept_addresses),
			&done, NULL, NULL) || !accept_ex || !accept_addresses) return 61;
	zero(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive)) ||
		bind(listener, (SOCKADDR *)&address, sizeof(address)) || getsockname(listener, (SOCKADDR *)&address, &address_size) ||
		listen(listener, SOMAXCONN)) return 62;
	if (getsockopt(accepted, SOL_SOCKET, SO_CONNECT_TIME, (char *)&connect_time, &length) || connect_time != 0xffffffff) return 63;
	HANDLE port = CreateIoCompletionPort((HANDLE)listener, NULL, 7, 0);
	char buffer[5 + 2 * (sizeof(SOCKADDR_IN) + 16)];
	OVERLAPPED operation;
	zero(&operation, sizeof(operation));
	if (!port) return 64;
	if (accept_ex(listener, accepted, buffer, 5, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &done, &operation) ||
		WSAGetLastError() != ERROR_IO_PENDING) return 65;
	if (WSAConnect(client, (SOCKADDR *)&address, sizeof(address), NULL, NULL, NULL, NULL)) return 66;
	/* Connected, the operation still waits for the first data. */
	DWORD bytes = 0;
	ULONG_PTR key = 0;
	LPOVERLAPPED finished = NULL;
	if (GetQueuedCompletionStatus(port, &bytes, &key, &finished, 200) || GetLastError() != WAIT_TIMEOUT) return 67;
	if (send(client, "hello", 5, 0) != 5 || !GetQueuedCompletionStatus(port, &bytes, &key, &finished, 5000) || bytes != 5 || key != 7 ||
		finished != &operation || buffer[0] != 'h' || buffer[4] != 'o') return 68;
	/* The accepted socket takes the listener's context, as a server does before it answers, and then answers. */
	char reply[3] = {0};
	if (setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *)&listener, sizeof(listener)) ||
		send(accepted, "ok", 2, 0) != 2 || recv(client, reply, 2, 0) != 2 || reply[0] != 'o' || reply[1] != 'k') return 76;
	SOCKADDR *local, *remote;
	int local_size = 0, remote_size = 0;
	accept_addresses(buffer, 5, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &local, &local_size, &remote, &remote_size);
	if (local->sa_family != AF_INET || remote->sa_family != AF_INET || ((SOCKADDR_IN *)local)->sin_port != address.sin_port) return 69;
	char text[64];
	DWORD text_size = sizeof(text);
	if (WSAAddressToStringA(local, local_size, NULL, text, &text_size) || text[0] != '1' || text[3] != '.' || text_size < 12) return 70;
	DWORD small = 2;
	if (WSAAddressToStringA(local, local_size, NULL, text, &small) != SOCKET_ERROR || WSAGetLastError() != WSAEFAULT || small != text_size)
		return 71;
	closesocket(client);
	closesocket(accepted);
	closesocket(listener);
	CloseHandle(port);
#ifdef _WIN64
	/* The same through the thread pool: a callback bound to the listening socket runs on one of its threads. Its threads are those of
	 * the guest RTL, which 32-bit programs do not have. */
	listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	accepted = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	address.sin_port = 0;
	address_size = sizeof(address);
	callback_done = CreateEventW(NULL, TRUE, FALSE, NULL);
	zero(&operation, sizeof(operation));
	if (bind(listener, (SOCKADDR *)&address, sizeof(address)) || getsockname(listener, (SOCKADDR *)&address, &address_size) ||
		listen(listener, SOMAXCONN) || !BindIoCompletionCallback((HANDLE)listener, operation_finished, 0)) return 72;
	if (accept_ex(listener, accepted, buffer, 5, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &done, &operation) ||
		WSAGetLastError() != ERROR_IO_PENDING) return 73;
	if (connect(client, (SOCKADDR *)&address, sizeof(address)) || send(client, "again", 5, 0) != 5) return 74;
	if (WaitForSingleObject(callback_done, 5000) || callback_error || callback_bytes != 5 || callback_operation != &operation ||
		buffer[0] != 'a') return 75;
	closesocket(client);
	closesocket(accepted);
	closesocket(listener);
	CloseHandle(callback_done);
#endif
	return 0;
}

int raven_entry(void)
{
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
	int result = datagrams(AF_INET);
	if (result) return result;
	result = datagrams(AF_INET6);
	if (result) return result + 100;
	result = selected_events();
	if (result) return result;
	result = selected_batch();
	if (result) return result;
	result = accepted_with_data();
	if (result) return result;
	WSACleanup();
	return 0;
}
