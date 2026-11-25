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
struct context_attribute {
	void *port_context, *message_context;
	ULONG sequence, message_id, callback_id;
};
struct security_attribute {
	ULONG flags;
	SECURITY_QUALITY_OF_SERVICE *quality;
	void *context;
};
struct port_attributes {
	ULONG flags;
	SECURITY_QUALITY_OF_SERVICE quality;
	SIZE_T limits[6];
	ULONG duplicable, reserved;
};
struct message_attributes {
	ULONG allocated, valid;
	BYTE storage[200];
};

typedef NTSTATUS (NTAPI *create_port)(HANDLE *, OBJECT_ATTRIBUTES *, const void *);
typedef NTSTATUS (NTAPI *impersonate_client)(HANDLE, struct header *, void *);
typedef NTSTATUS (NTAPI *connect_port)(HANDLE *, UNICODE_STRING *, OBJECT_ATTRIBUTES *, const void *, ULONG, PSID,
	struct header *, SIZE_T *, void *, void *, LARGE_INTEGER *);
typedef NTSTATUS (NTAPI *accept_port)(HANDLE *, HANDLE, ULONG, OBJECT_ATTRIBUTES *, const void *, void *, struct header *, void *, BOOLEAN);
typedef NTSTATUS (NTAPI *exchange)(HANDLE, ULONG, struct header *, void *, struct header *, SIZE_T *, void *, LARGE_INTEGER *);
typedef NTSTATUS (NTAPI *create_packet)(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *);
typedef NTSTATUS (NTAPI *associate_packet)(HANDLE, HANDLE, HANDLE, void *, void *, NTSTATUS, ULONG_PTR, BOOLEAN *);
typedef NTSTATUS (NTAPI *initialize_attributes)(ULONG, struct message_attributes *, SIZE_T, SIZE_T *);
typedef void *(NTAPI *get_attribute)(const struct message_attributes *, ULONG);

enum { security = 0x80000000, view = 0x40000000, context = 0x20000000, work_on_behalf = 0x02000000 };

static connect_port connect;
static exchange send_receive;
static initialize_attributes initialize;
static get_attribute get;

static NTSTATUS receive(HANDLE port, struct message *message, void *attributes, LONGLONG interval)
{
	LARGE_INTEGER limit;
	limit.QuadPart = interval;
	SIZE_T capacity = sizeof *message;
	return send_receive(port, 0, NULL, NULL, &message->header, &capacity, attributes, &limit);
}

static DWORD WINAPI client(void *go)
{
	WCHAR text[] = L"\\RPC Control\\RavenAlpcWait";
	UNICODE_STRING name = {sizeof text - sizeof(WCHAR), sizeof text, text};
	struct message message = {{sizeof(ULONG), sizeof(struct header) + sizeof(ULONG)}, 31};
	SIZE_T capacity = sizeof message;
	LARGE_INTEGER limit;
	limit.QuadPart = -100000000;
	HANDLE port = NULL;
	// The client port is nameless, and the connection message carries a security attribute without a quality of service, as
	// CoreMessaging sends it: the port attributes' stays in force, and lets the server identify it.
	OBJECT_ATTRIBUTES object = {.Length = sizeof object};
	struct port_attributes allowed = {0x10000, {sizeof(SECURITY_QUALITY_OF_SERVICE), SecurityIdentification, SECURITY_STATIC_TRACKING, TRUE}};
	struct message_attributes sent, received;
	SIZE_T needed = 0;
	if (initialize(security, &sent, sizeof sent, &needed) || initialize(context, &received, sizeof received, &needed))
		return 43;
	struct security_attribute *about = get(&sent, security);
	about->flags = 0;
	about->quality = NULL;
	about->context = (void *)(LONG_PTR)-2;
	sent.valid = security;
	if (connect(&port, &name, &object, &allowed, 0x20000, NULL, &message.header, &capacity, &sent, &received, &limit) || !port)
		return 40;
	if (WaitForSingleObject(go, 10000))
		return 41;
	// A sender may name a context and a work-on-behalf ticket as valid, as CoreMessaging does with each request: the receiver's
	// context still comes from its connection.
	struct message datagram = {{sizeof(ULONG), sizeof(struct header) + sizeof(ULONG)}, 77};
	struct message_attributes along;
	if (initialize(security | view | context | work_on_behalf, &along, sizeof along, &needed))
		return 44;
	struct context_attribute *sent_context = get(&along, context);
	sent_context->port_context = sent_context->message_context = NULL;
	sent_context->sequence = sent_context->message_id = sent_context->callback_id = 0;
	along.valid = context | work_on_behalf;
	return send_receive(port, 0, &datagram.header, &along, NULL, NULL, NULL, NULL) ? 42 : 0;
}

int raven_entry(void)
{
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	const create_port create = (create_port)GetProcAddress(ntdll, "NtAlpcCreatePort");
	const accept_port accept = (accept_port)GetProcAddress(ntdll, "NtAlpcAcceptConnectPort");
	const impersonate_client impersonate = (impersonate_client)GetProcAddress(ntdll, "NtAlpcImpersonateClientOfPort");
	const create_packet create_wait = (create_packet)GetProcAddress(ntdll, "NtCreateWaitCompletionPacket");
	const associate_packet associate = (associate_packet)GetProcAddress(ntdll, "NtAssociateWaitCompletionPacket");
	initialize = (initialize_attributes)GetProcAddress(ntdll, "AlpcInitializeMessageAttribute");
	get = (get_attribute)GetProcAddress(ntdll, "AlpcGetMessageAttribute");
	connect = (connect_port)GetProcAddress(ntdll, "NtAlpcConnectPort");
	send_receive = (exchange)GetProcAddress(ntdll, "NtAlpcSendWaitReceivePort");
	if (!create || !accept || !impersonate || !create_wait || !associate || !initialize || !get || !connect || !send_receive)
		return 1;
	WCHAR text[] = L"\\RPC Control\\RavenAlpcWait";
	UNICODE_STRING name = {sizeof text - sizeof(WCHAR), sizeof text, text};
	OBJECT_ATTRIBUTES object = {.Length = sizeof object, .ObjectName = &name};
	HANDLE listener = NULL, packet = NULL;
	const HANDLE completion = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	const HANDLE go = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (create(&listener, &object, NULL) || !listener || !completion || !go || create_wait(&packet, 1, NULL) || !packet)
		return 2;
	// An empty port is not signaled: a wait on it times out, and the packet tied to it waits.
	BOOLEAN signaled = TRUE;
	if (WaitForSingleObject(listener, 0) != WAIT_TIMEOUT || associate(packet, completion, listener, (void *)17, (void *)19, 0, 23, &signaled) ||
		signaled)
		return 3;
	HANDLE worker = CreateThread(NULL, 0, client, go, 0, NULL);
	struct message message;
	if (!worker || receive(listener, &message, NULL, -100000000) || (message.header.type & 0xfff) != 10)
		return 4;
	// The connection request was what woke the packet: it was queued once, with the key and context it was tied with.
	DWORD bytes = 0;
	ULONG_PTR key = 0;
	OVERLAPPED *overlapped = NULL;
	if (!GetQueuedCompletionStatus(completion, &bytes, &key, &overlapped, 10000) || bytes != 23 || key != 17 || (ULONG_PTR)overlapped != 19 ||
		GetQueuedCompletionStatus(completion, &bytes, &key, &overlapped, 0) || GetLastError() != WAIT_TIMEOUT)
		return 5;
	// The server identifies who asks before it accepts, through the connection port and the request.
	if (impersonate(listener, &message.header, NULL) || !RevertToSelf())
		return 16;
	HANDLE peer = NULL;
	void *const port_context = (void *)(ULONG_PTR)0x5eed;
	if (accept(&peer, listener, 0, NULL, NULL, port_context, &message.header, NULL, TRUE) || !peer)
		return 6;
	if (associate(packet, completion, listener, (void *)18, (void *)20, 0, 24, &signaled) || signaled || !SetEvent(go))
		return 7;
	if (!GetQueuedCompletionStatus(completion, &bytes, &key, &overlapped, 10000) || bytes != 24 || key != 18 || WaitForSingleObject(listener, 0))
		return 8;
	// The receiver's attributes name what came with the message: only its context, which holds the server's port context and its id.
	struct message_attributes attributes;
	SIZE_T needed = 0;
	if (initialize(security | view | context | work_on_behalf, &attributes, sizeof attributes, &needed))
		return 9;
	if (receive(listener, &message, &attributes, -30000000) || message.value != 77 || (message.header.type & 0xfff) != 3)
		return 10;
	const struct context_attribute *about = get(&attributes, context);
	if (attributes.allocated != (security | view | context | work_on_behalf) || attributes.valid != context || !about)
		return 11;
	if (about->port_context != port_context || about->message_id != message.header.message_id || about->message_context)
		return 12;
	if (WaitForSingleObject(worker, 10000) || !GetExitCodeThread(worker, &bytes) || bytes)
		return 13;
	if (WaitForSingleObject(listener, 0) != WAIT_TIMEOUT)
		return 14;
	return CloseHandle(peer) && CloseHandle(listener) ? 0 : 15;
}
