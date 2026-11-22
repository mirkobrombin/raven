#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

struct header {
	USHORT data_length, total_length, type, data_offset;
	ULONG_PTR process, thread;
	ULONG message_id;
	ULONG_PTR view_size;
};
struct message {
	struct header header;
	ULONG value;
};
_Static_assert(sizeof(struct header) == (sizeof(void *) == 8 ? 40 : 24), "PORT_MESSAGE size");

typedef NTSTATUS (NTAPI *create_port)(HANDLE *, OBJECT_ATTRIBUTES *, const void *);
typedef NTSTATUS (NTAPI *connect_port)(HANDLE *, UNICODE_STRING *, OBJECT_ATTRIBUTES *, const void *, ULONG, PSID,
	struct header *, SIZE_T *, void *, void *, LARGE_INTEGER *);
typedef NTSTATUS (NTAPI *accept_port)(HANDLE *, HANDLE, ULONG, OBJECT_ATTRIBUTES *, const void *, void *, struct header *, void *, BOOLEAN);
typedef NTSTATUS (NTAPI *exchange)(HANDLE, ULONG, struct header *, void *, struct header *, SIZE_T *, void *, LARGE_INTEGER *);

static connect_port connect;
static exchange send_receive;

static NTSTATUS receive(HANDLE port, struct message *message, LONGLONG interval)
{
	LARGE_INTEGER limit;
	limit.QuadPart = interval;
	SIZE_T capacity = sizeof *message;
	return send_receive(port, 0, NULL, NULL, &message->header, &capacity, NULL, &limit);
}

static NTSTATUS send(HANDLE port, ULONG value)
{
	struct message message = {{sizeof value, sizeof(struct header) + sizeof value}, value};
	return send_receive(port, 0, &message.header, NULL, NULL, NULL, NULL, NULL);
}

static DWORD WINAPI client(void *argument)
{
	UNICODE_STRING *name = argument;
	HANDLE port = NULL;
	struct message message = {{sizeof(ULONG), sizeof(struct header) + sizeof(ULONG)}, 34};
	SIZE_T capacity = sizeof message;
	LARGE_INTEGER limit;
	limit.QuadPart = -30000000;
	if (connect(&port, name, NULL, NULL, 0x20000, NULL, &message.header, &capacity, NULL, NULL, &limit) || !port)
		return 10;
	if (send(port, 582) || receive(port, &message, -30000000) || message.header.data_length != sizeof(ULONG) || message.value != 711)
		return 11;
	struct message request = {{sizeof(ULONG), sizeof(struct header) + sizeof(ULONG)}, 921};
	capacity = sizeof message;
	if (send_receive(port, 0x20000, &request.header, NULL, &message.header, &capacity, NULL, &limit) ||
		message.header.data_length != sizeof(ULONG) || message.value != 425)
		return 12;
	return CloseHandle(port) ? 0 : 13;
}

int raven_entry(void)
{
	const HMODULE native = GetModuleHandleW(L"ntdll.dll");
	const create_port create = (create_port)GetProcAddress(native, "NtAlpcCreatePort");
	const accept_port accept = (accept_port)GetProcAddress(native, "NtAlpcAcceptConnectPort");
	connect = (connect_port)GetProcAddress(native, "NtAlpcConnectPort");
	send_receive = (exchange)GetProcAddress(native, "NtAlpcSendWaitReceivePort");
	if (!create || !accept || !connect || !send_receive)
		return 1;
	WCHAR address[] = L"\\RPC Control\\RavenRoundTrip";
	UNICODE_STRING name = {sizeof address - sizeof(WCHAR), sizeof address, address};
	OBJECT_ATTRIBUTES attributes = {.Length = sizeof attributes, .ObjectName = &name, .Attributes = 0x40};
	HANDLE listener = NULL, peer = NULL;
	if (create(&listener, &attributes, NULL) || !listener)
		return 2;
	struct message message;
	if (receive(listener, &message, 0) != 0x102)
		return 3;
	HANDLE worker = CreateThread(NULL, 0, client, &name, 0, NULL);
	if (!worker || receive(listener, &message, -30000000) || (message.header.type & 0xfff) != 10 ||
		!message.header.message_id || message.value != 34)
		return 4;
	message.header.data_length = 0;
	message.header.total_length = sizeof message.header;
	if (accept(&peer, listener, 0, NULL, NULL, NULL, &message.header, NULL, TRUE) || !peer)
		return 5;
	if (receive(listener, &message, -30000000) || message.header.data_length != sizeof(ULONG) || message.value != 582 || send(peer, 711))
		return 6;
	if (receive(listener, &message, -30000000) || !message.header.message_id || message.value != 921)
		return 7;
	const ULONG request_id = message.header.message_id;
	message.header.message_id = request_id ^ 0x80000000;
	if (send_receive(listener, 1, &message.header, NULL, NULL, NULL, NULL, NULL) >= 0)
		return 8;
	message.header.message_id = request_id;
	message.value = 425;
	if (send_receive(listener, 1, &message.header, NULL, NULL, NULL, NULL, NULL))
		return 9;
	DWORD result = 0;
	if (WaitForSingleObject(worker, 4000) != WAIT_OBJECT_0 || !GetExitCodeThread(worker, &result))
		return 14;
	return !CloseHandle(worker) || !CloseHandle(peer) || !CloseHandle(listener) ? 15 : result;
}
