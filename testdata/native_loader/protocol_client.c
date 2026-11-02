#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

/* getprotobyname and getprotobynumber answer from the protocols database into one PROTOENT that the following call reuses, as Winsock
   reuses its per-thread one. Both spellings of a protocol name the same record, and a protocol the database does not list is refused. */
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
	WSACleanup();
	return 0;
}
