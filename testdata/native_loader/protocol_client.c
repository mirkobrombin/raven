#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0a00
#include <winsock2.h>
#include <ws2tcpip.h>

/* getprotobyname and getprotobynumber answer from the protocols database into one PROTOENT that the following call reuses, as Winsock
   reuses its per-thread one. Both spellings of a protocol name the same record, and a protocol the database does not list is refused.
   The socket forms of the byte order conversions follow. */
static int same(const char *left, const char *right)
{
	if (!left || !right)
		return 0;
	for (unsigned at = 0;; ++at) {
		char a = left[at], b = right[at];
		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (a != b)
			return 0;
		if (!a)
			return 1;
	}
}

/* GetAddrInfoEx answers with ADDRINFOEX records, also when the caller passes an OVERLAPPED: the lookup completes in the call, its
   result is in the OVERLAPPED too, and no lookup is left to cancel. */
static int extended_lookup(void)
{
	ADDRINFOEXW hints = {0}, *found = NULL;
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (GetAddrInfoExW(L"127.0.0.1", L"80", NS_DNS, NULL, &hints, &found, NULL, NULL, NULL, NULL) || !found)
		return 1;
	const struct sockaddr_in *address = (const struct sockaddr_in *)found->ai_addr;
	if (found->ai_family != AF_INET || found->ai_addrlen != sizeof(*address) || found->ai_next || found->ai_blob || found->ai_provider)
		return 2;
	if (address->sin_family != AF_INET || address->sin_port != htons(80) || address->sin_addr.s_addr != htonl(INADDR_LOOPBACK))
		return 3;
	FreeAddrInfoExW(found);
	OVERLAPPED overlapped = {0};
	HANDLE cancel = (HANDLE)1;
	found = NULL;
	if (GetAddrInfoExW(L"127.0.0.1", NULL, NS_DNS, NULL, &hints, &found, NULL, &overlapped, NULL, &cancel) || !found)
		return 4;
	INT (WSAAPI *overlapped_result)(LPOVERLAPPED) = (INT (WSAAPI *)(LPOVERLAPPED))GetProcAddress(GetModuleHandleW(L"ws2_32.dll"), "GetAddrInfoExOverlappedResult");
	if (!overlapped_result || overlapped_result(&overlapped) || cancel)
		return 5;
	FreeAddrInfoExW(found);
	found = NULL;
	if (!GetAddrInfoExW(L"not an address", NULL, NS_DNS, NULL, &hints, &found, NULL, NULL, NULL, NULL) || found)
		return 6;
	return 0;
}

int raven_entry(void)
{
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data))
		return 1;
	struct protoent *tcp = getprotobyname("tcp");
	if (!tcp || tcp->p_proto != 6 || !same(tcp->p_name, "tcp"))
		return 2;
	struct protoent *by_number = getprotobynumber(6);
	if (!by_number || by_number->p_proto != 6 || !same(by_number->p_name, "tcp"))
		return 3;
	// The record is the one the earlier call answered with, and it has no aliases.
	if (tcp != by_number || !by_number->p_aliases || by_number->p_aliases[0])
		return 4;
	struct protoent *udp = getprotobynumber(17);
	if (!udp || udp->p_proto != 17 || !same(udp->p_name, "udp"))
		return 5;
	// A protocol the database does not list has no record.
	SetLastError(0xdead);
	if (getprotobyname("no-such-protocol") || getprotobynumber(200))
		return 6;
	const int extended = extended_lookup();
	if (extended)
		return 10 + extended;
	/* The socket forms of the byte order conversions store the converted value and answer 0 (Qt's socket engine reads addresses with
	   them); a handle that is no socket and a missing place to store are refused. */
	SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	u_long wide = 0;
	u_short narrow = 0;
	if (socket_handle == INVALID_SOCKET || WSAHtonl(socket_handle, 0x01020304, &wide) || wide != 0x04030201 ||
		WSANtohl(socket_handle, 0x04030201, &wide) || wide != 0x01020304 || WSAHtons(socket_handle, 0x0102, &narrow) || narrow != 0x0201 ||
		WSANtohs(socket_handle, 0x0201, &narrow) || narrow != 0x0102)
		return 30;
	if (WSANtohl(socket_handle, 1, NULL) != SOCKET_ERROR || WSAGetLastError() != WSAEFAULT ||
		WSAHtons((SOCKET)0x7ff0, 1, &narrow) != SOCKET_ERROR || WSAGetLastError() != WSAENOTSOCK)
		return 31;
	closesocket(socket_handle);
	WSACleanup();
	return 0;
}
