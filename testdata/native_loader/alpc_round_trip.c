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

typedef NTSTATUS (NTAPI *query_port)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef NTSTATUS (NTAPI *impersonate_port)(HANDLE, struct header *, ULONG);

struct server_session { ULONG session, process; };
struct client_input { UNICODE_STRING *name; PSID server_sid; BOOL impersonation; };
struct port_attributes {
	ULONG flags;
	SECURITY_QUALITY_OF_SERVICE qos;
	SIZE_T message_limit, bandwidth, pool_limit, section_limit, view_limit, total_section_limit;
	ULONG object_types;
};
_Static_assert(sizeof(struct port_attributes) == (sizeof(void *) == 8 ? 72 : 44), "ALPC_PORT_ATTRIBUTES size");

enum { allow_impersonation = 0x10000, impersonation_level = SecurityImpersonation << 2 };

static connect_port connect;
static exchange send_receive;
static query_port query;

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

static BOOL set_client_integrity(WELL_KNOWN_SID_TYPE label_type)
{
	BYTE sid[SECURITY_MAX_SID_SIZE];
	DWORD size = sizeof sid;
	if (!CreateWellKnownSid(label_type, NULL, sid, &size))
		return FALSE;
	HANDLE token;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_DEFAULT, &token))
		return FALSE;
	TOKEN_MANDATORY_LABEL label = {{sid, SE_GROUP_INTEGRITY}};
	const BOOL lowered = SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof label + size);
	return CloseHandle(token) && lowered;
}

static BOOL check_impersonation(impersonate_port impersonate, HANDLE port, struct header *message)
{
	const ULONG id = message->message_id;
	message->message_id = 0;
	const NTSTATUS invalid_message = impersonate(port, message, impersonation_level);
	message->message_id = id;
	const ULONG_PTR process = message->process;
	message->process = GetCurrentProcessId();
	const NTSTATUS wrong_sender = impersonate(port, message, impersonation_level);
	message->process = process;
	HANDLE token;
	if (invalid_message >= 0 || wrong_sender >= 0 || OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) ||
		GetLastError() != ERROR_NO_TOKEN)
		return FALSE;
	if (impersonate(port, message, impersonation_level) || !OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
		return FALSE;
	union { TOKEN_MANDATORY_LABEL label; BYTE bytes[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]; } integrity;
	SECURITY_IMPERSONATION_LEVEL level;
	DWORD size;
	const BOOL client = GetTokenInformation(token, TokenIntegrityLevel, &integrity, sizeof integrity, &size) &&
		*GetSidSubAuthority(integrity.label.Label.Sid, 0) == SECURITY_MANDATORY_LOW_RID &&
		GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof level, &size) && level == SecurityImpersonation;
	return CloseHandle(token) && RevertToSelf() && client &&
		!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) && GetLastError() == ERROR_NO_TOKEN;
}

static DWORD WINAPI client(void *argument)
{
	const struct client_input *input = argument;
	const struct port_attributes port_attrs = {allow_impersonation, {sizeof(SECURITY_QUALITY_OF_SERVICE), SecurityImpersonation,
		SECURITY_STATIC_TRACKING, FALSE}, .message_limit = 512};
	if (input->impersonation && !set_client_integrity(WinLowLabelSid))
		return 35;
	HANDLE port = NULL;
	struct message message = {{sizeof(ULONG), sizeof(struct header) + sizeof(ULONG)}, 34};
	SIZE_T capacity = sizeof message;
	LARGE_INTEGER limit;
	limit.QuadPart = -100000000;
	if (connect(&port, input->name, NULL, input->impersonation ? &port_attrs : NULL, 0x20000, input->server_sid,
		&message.header, &capacity, NULL, NULL, &limit) || !port)
		return 10;
	if (input->impersonation && !set_client_integrity(WinUntrustedLabelSid))
		return 36;
	if (send(port, 582) || receive(port, &message, -30000000) || message.header.data_length != sizeof(ULONG) || !message.value)
		return 11;
	struct server_session server = {0};
	DWORD session = 0;
	ULONG needed = 0;
	if (query(port, 12, &server, sizeof server - 1, &needed) >= 0 || server.process || server.session)
		return 24;
	if (!ProcessIdToSessionId(message.value, &session))
		return 28;
	if (query(port, 12, &server, sizeof server, &needed))
		return 25;
	if (server.process != message.value)
		return 29;
	if (server.session != session)
		return 30;
	struct message request = {{sizeof(ULONG), sizeof(struct header) + sizeof(ULONG)}, 921};
	capacity = sizeof message;
	if (send_receive(port, 0x20000, &request.header, NULL, &message.header, &capacity, NULL, &limit) ||
		message.header.data_length != sizeof(ULONG) || message.value != 425)
		return 12;
	request.value = 123;
	capacity = sizeof message;
	limit.QuadPart = -10000000;
	if (send_receive(port, 0x20000, &request.header, NULL, &message.header, &capacity, NULL, &limit) != 0x102)
		return 19;
	if (send(port, 456) || receive(port, &message, -30000000) || message.header.data_length != sizeof(ULONG) || message.value != 789)
		return 20;
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

static HANDLE start_client_process(DWORD *process_id, DWORD *thread_id, BOOL impersonation)
{
	WCHAR image[MAX_PATH], command[MAX_PATH + 32] = L"\"";
	const DWORD length = GetModuleFileNameW(NULL, image, ARRAYSIZE(image));
	if (!length || length == ARRAYSIZE(image))
		return NULL;
	lstrcatW(command, image);
	lstrcatW(command, impersonation ? L"\" --alpc-token-client" : L"\" --alpc-client");
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
	query = (query_port)GetProcAddress(native, "NtAlpcQueryInformation");
	if (!query)
		return 23;
	const WCHAR *command = GetCommandLineW();
	const BOOL token_child = command_ends_with(command, L" --alpc-token-client");
	const BOOL impersonation = token_child || command_ends_with(command, L" --alpc-impersonation");
	const BOOL child = token_child || command_ends_with(command, L" --alpc-client");
	const BOOL cross_process = child || impersonation || command_ends_with(command, L" --alpc-process");
	const impersonate_port impersonate = impersonation ?
		(impersonate_port)GetProcAddress(native, "NtAlpcImpersonateClientOfPort") : NULL;
	if (impersonation && !impersonate)
		return 32;
	DWORD invalid_session = MAXDWORD;
	if (ProcessIdToSessionId(MAXDWORD, &invalid_session) || invalid_session != MAXDWORD ||
		ProcessIdToSessionId(GetCurrentProcessId(), NULL))
		return 31;
	WCHAR server_process[] = L"\\RPC Control\\RavenProcessRoundTrip", server_thread[] = L"\\RPC Control\\RavenThreadRoundTrip";
	WCHAR server_token[] = L"\\RPC Control\\RavenImpersonationRoundTrip";
	const USHORT name_length = impersonation ? sizeof server_token : cross_process ? sizeof server_process : sizeof server_thread;
	UNICODE_STRING name = {name_length - sizeof(WCHAR), name_length, impersonation ? server_token : cross_process ? server_process : server_thread};
	union token_info token_user;
	if (!read_token_user(&token_user))
		return 17;
	struct client_input input = {&name, token_user.user.User.Sid, impersonation};
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
	HANDLE worker = cross_process ? start_client_process(&peer_process, &peer_thread, impersonation) :
		CreateThread(NULL, 0, client, &input, 0, &peer_thread);
	if (!worker || receive(listener, &message, -100000000) || (message.header.type & 0xfff) != 10 ||
		!message.header.message_id || message.value != 34 || message.header.process != peer_process || message.header.thread != peer_thread)
		return 4;
	message.header.data_length = 0;
	message.header.total_length = sizeof message.header;
	if (accept(&peer, listener, 0, NULL, NULL, NULL, &message.header, NULL, TRUE) || !peer)
		return 5;
	if (receive(listener, &message, -30000000) || message.header.data_length != sizeof(ULONG) || message.value != 582 ||
		message.header.process != peer_process || message.header.thread != peer_thread || send(peer, GetCurrentProcessId()))
		return 6;
	if (receive(listener, &message, -30000000)) {
		DWORD result = 0;
		const BOOL read = cross_process ? GetExitCodeProcess(worker, &result) : GetExitCodeThread(worker, &result);
		if (read && result && result != STILL_ACTIVE)
			return result;
		return 7;
	}
	if (!message.header.message_id || message.value != 921)
		return 26;
	if (message.header.process != peer_process || message.header.thread != peer_thread)
		return 27;
	if (impersonation && !check_impersonation(impersonate, peer, &message.header))
		return 33;
	const ULONG request_id = message.header.message_id;
	message.header.message_id = request_id ^ 0x80000000;
	if (send_receive(listener, 1, &message.header, NULL, NULL, NULL, NULL, NULL) >= 0)
		return 8;
	message.header.message_id = request_id;
	message.value = 425;
	if (send_receive(listener, 1, &message.header, NULL, NULL, NULL, NULL, NULL))
		return 9;
	if (impersonation && impersonate(peer, &message.header, impersonation_level) >= 0)
		return 37;
	struct message timed_out;
	if (receive(listener, &timed_out, -30000000) || (timed_out.header.type & 0xfff) != 1 || timed_out.value != 123 ||
		receive(listener, &message, -30000000) || (message.header.type & 0xfff) != 3 || message.value != 456)
		return 21;
	if (send_receive(listener, 1, &timed_out.header, NULL, NULL, NULL, NULL, NULL) >= 0 || send(peer, 789))
		return 22;
	DWORD result = 0;
	if (WaitForSingleObject(worker, 4000) != WAIT_OBJECT_0 ||
		!(cross_process ? GetExitCodeProcess(worker, &result) : GetExitCodeThread(worker, &result)))
		return 14;
	return !CloseHandle(worker) || !CloseHandle(peer) || !CloseHandle(listener) ? 15 : result;
}
