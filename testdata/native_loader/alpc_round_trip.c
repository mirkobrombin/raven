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

struct client_input { UNICODE_STRING *name; PSID server_sid; };

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
	const struct client_input *input = argument;
	HANDLE port = NULL;
	struct message message = {{sizeof(ULONG), sizeof(struct header) + sizeof(ULONG)}, 34};
	SIZE_T capacity = sizeof message;
	LARGE_INTEGER limit;
	limit.QuadPart = -100000000;
	if (connect(&port, input->name, NULL, NULL, 0x20000, input->server_sid, &message.header, &capacity, NULL, NULL, &limit) || !port)
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

union token_info { TOKEN_USER user; BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; };

static BOOL read_token_user(union token_info *info)
{
	HANDLE token = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return FALSE;
	DWORD length = 0;
	const BOOL read = GetTokenInformation(token, TokenUser, info, sizeof *info, &length);
	return CloseHandle(token) && read;
}

static HANDLE start_client_process(DWORD *process_id, DWORD *thread_id)
{
	WCHAR image[MAX_PATH], command[MAX_PATH + 32] = L"\"";
	const DWORD length = GetModuleFileNameW(NULL, image, ARRAYSIZE(image));
	if (!length || length == ARRAYSIZE(image))
		return NULL;
	lstrcatW(command, image);
	lstrcatW(command, L"\" --alpc-client");
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION process;
	if (!CreateProcessW(image, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
		return NULL;
	*process_id = process.dwProcessId;
	*thread_id = process.dwThreadId;
	CloseHandle(process.hThread);
	return process.hProcess;
}

static BOOL command_ends_with(const WCHAR *line, const WCHAR *suffix)
{
	const int length = lstrlenW(line), tail = lstrlenW(suffix);
	return length >= tail && !lstrcmpW(line + length - tail, suffix);
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
	const WCHAR *command = GetCommandLineW();
	const BOOL child = command_ends_with(command, L" --alpc-client");
	const BOOL cross_process = child || command_ends_with(command, L" --alpc-process");
	WCHAR server_process[] = L"\\RPC Control\\RavenProcessRoundTrip", server_thread[] = L"\\RPC Control\\RavenThreadRoundTrip";
	const USHORT name_length = cross_process ? sizeof server_process : sizeof server_thread;
	UNICODE_STRING name = {name_length - sizeof(WCHAR), name_length, cross_process ? server_process : server_thread};
	union token_info token_user;
	if (!read_token_user(&token_user))
		return 17;
	struct client_input input = {&name, token_user.user.User.Sid};
	if (child)
		return client(&input);
	OBJECT_ATTRIBUTES attributes = {.Length = sizeof attributes, .ObjectName = &name, .Attributes = 0x40};
	HANDLE listener = NULL, peer = NULL;
	if (create(&listener, &attributes, NULL) || !listener)
		return 2;
	struct message message;
	if (receive(listener, &message, 0) != 0x102)
		return 3;
	HANDLE restricted = NULL;
	if (!DuplicateHandle(GetCurrentProcess(), listener, GetCurrentProcess(), &restricted, READ_CONTROL, FALSE, 0) ||
		receive(restricted, &message, 0) != (NTSTATUS)0xc0000022 || !CloseHandle(restricted))
		return 18;
	BYTE different_sid[SECURITY_MAX_SID_SIZE];
	DWORD sid_size = sizeof different_sid;
	LARGE_INTEGER no_wait;
	no_wait.QuadPart = 0;
	if (!CreateWellKnownSid(WinNullSid, NULL, different_sid, &sid_size) ||
		connect(&peer, &name, NULL, NULL, 0x20000, different_sid, NULL, NULL, NULL, NULL, &no_wait) != (NTSTATUS)0xc00002a0 || peer ||
		receive(listener, &message, 0) != 0x102)
		return 16;
	DWORD peer_process = GetCurrentProcessId(), peer_thread = 0;
	HANDLE worker = cross_process ? start_client_process(&peer_process, &peer_thread) :
		CreateThread(NULL, 0, client, &input, 0, &peer_thread);
	if (!worker || receive(listener, &message, -100000000) || (message.header.type & 0xfff) != 10 ||
		!message.header.message_id || message.value != 34 || message.header.process != peer_process || message.header.thread != peer_thread)
		return 4;
	message.header.data_length = 0;
	message.header.total_length = sizeof message.header;
	if (accept(&peer, listener, 0, NULL, NULL, NULL, &message.header, NULL, TRUE) || !peer)
		return 5;
	if (receive(listener, &message, -30000000) || message.header.data_length != sizeof(ULONG) || message.value != 582 ||
		message.header.process != peer_process || message.header.thread != peer_thread || send(peer, 711))
		return 6;
	if (receive(listener, &message, -30000000) || !message.header.message_id || message.value != 921 ||
		message.header.process != peer_process || message.header.thread != peer_thread)
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
	if (WaitForSingleObject(worker, 4000) != WAIT_OBJECT_0 ||
		!(cross_process ? GetExitCodeProcess(worker, &result) : GetExitCodeThread(worker, &result)))
		return 14;
	return !CloseHandle(worker) || !CloseHandle(peer) || !CloseHandle(listener) ? 15 : result;
}
