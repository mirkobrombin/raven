/* SPDX-License-Identifier: MIT */
#include "rpc_own.h"

typedef struct server_interface
{
	RPC_SERVER_INTERFACE *spec;
	UUID manager_type;
	RPC_MGR_EPV *manager;
	UINT flags;
	RPC_IF_CALLBACK_FN *callback;
	LONG running;
	struct server_interface *next;
} server_interface;

typedef struct object_type
{
	UUID object, type;
	struct object_type *next;
} object_type;

typedef struct server_endpoint
{
	rpc_listener listener;
	WCHAR *protseq, *endpoint;
	HANDLE thread;
	struct server_endpoint *next;
} server_endpoint;

static CRITICAL_SECTION server_lock;
static CRITICAL_SECTION_DEBUG server_lock_debug = {0, 0, &server_lock, {&server_lock_debug.ProcessLocksList, &server_lock_debug.ProcessLocksList}, 0, 0, 0, 0, 0, 0, 0};
static CRITICAL_SECTION server_lock = {&server_lock_debug, -1, 0, 0, 0, 0};

static server_interface *interfaces;
static object_type *object_types;
static server_endpoint *endpoints;
static BOOL listening, stopping;
static BOOL listen_open;		// from RpcServerListen until somebody has seen it end
static LONG auto_listen;
static HANDLE stopped_event;
static LONG active_calls;
static DWORD current_slot = TLS_OUT_OF_INDEXES;
static ULONG endpoint_counter;

static DWORD call_slot(void)
{
	if (current_slot == TLS_OUT_OF_INDEXES) {
		DWORD slot = TlsAlloc();
		if (InterlockedCompareExchange((LONG *)&current_slot, (LONG)slot, (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES)
			TlsFree(slot);
	}
	return current_slot;
}

static BOOL syntax_equal(const RPC_SYNTAX_IDENTIFIER *a, const RPC_SYNTAX_IDENTIFIER *b)
{
	return !memcmp(a, b, sizeof(*a));
}

static const RPC_SYNTAX_IDENTIFIER ndr_syntax = {{0x8a885d04, 0x1ceb, 0x11c9, {0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60}}, {2, 0}};

// An spec with this identity, the major version exact and the minor at least what was asked.
static server_interface *interface_for(const RPC_SYNTAX_IDENTIFIER *wanted)
{
	for (server_interface *at = interfaces; at; at = at->next) {
		const RPC_SYNTAX_IDENTIFIER *id = &at->spec->InterfaceId;
		if (!memcmp(&id->SyntaxGUID, &wanted->SyntaxGUID, sizeof(GUID)) && id->SyntaxVersion.MajorVersion == wanted->SyntaxVersion.MajorVersion && id->SyntaxVersion.MinorVersion >= wanted->SyntaxVersion.MinorVersion)
			return at;
	}
	return NULL;
}

// ---- one connection ----

#define MAX_CONTEXTS 32

typedef struct connection_state
{
	rpc_stream stream;
	rpc_binding binding;
	struct
	{
		USHORT id;
		RPC_SYNTAX_IDENTIFIER abstract;
	} contexts[MAX_CONTEXTS];
	INT context_count;
	WCHAR *protseq, *endpoint;
} connection_state;

#pragma pack(push, 1)
typedef struct syntax_result
{
	USHORT result, reason;
	RPC_SYNTAX_IDENTIFIER transfer;
} syntax_result;
#pragma pack(pop)

static BOOL reply_bind(connection_state *state, const rpc_pdu *pdu)
{
	BYTE body[256];
	memset(body, 0, sizeof(body));
	*(USHORT *)body = PDU_FRAGMENT_SIZE;
	*(USHORT *)(body + 2) = PDU_FRAGMENT_SIZE;
	*(ULONG *)(body + 4) = 1;
	// No secondary address is needed on a connection that already exists; it stays one NUL long.
	*(USHORT *)(body + 8) = 1;
	ULONG at = 12;
	ULONG size = pdu->frag_length - 16;
	if (size < 12)
		return FALSE;
	BYTE count = pdu->body[8];
	body[at] = count;
	at += 4;
	const BYTE *item = pdu->body + 12, *end = pdu->body + size;
	for (BYTE i = 0; i < count; i++) {
		if (item + 4 + sizeof(RPC_SYNTAX_IDENTIFIER) > end || at + sizeof(syntax_result) > sizeof(body))
			return FALSE;
		USHORT id = *(USHORT *)item;
		BYTE transfers = item[2];
		const RPC_SYNTAX_IDENTIFIER *abstract = (const RPC_SYNTAX_IDENTIFIER *)(item + 4);
		const BYTE *transfer = item + 4 + sizeof(*abstract);
		if (transfer + transfers * sizeof(RPC_SYNTAX_IDENTIFIER) > end)
			return FALSE;
		BOOL has_ndr = FALSE;
		for (BYTE t = 0; t < transfers; t++)
			if (syntax_equal((const RPC_SYNTAX_IDENTIFIER *)(transfer + t * sizeof(RPC_SYNTAX_IDENTIFIER)), &ndr_syntax))
				has_ndr = TRUE;
		EnterCriticalSection(&server_lock);
		BOOL known = interface_for(abstract) != NULL;
		LeaveCriticalSection(&server_lock);
		syntax_result result;
		memset(&result, 0, sizeof(result));
		if (!known) {
			result.result = 2;
			result.reason = 1;
		} else if (!has_ndr) {
			result.result = 2;
			result.reason = 2;
		} else {
			result.transfer = ndr_syntax;
			if (state->context_count < MAX_CONTEXTS) {
				state->contexts[state->context_count].id = id;
				state->contexts[state->context_count].abstract = *abstract;
				state->context_count++;
			}
		}
		memcpy(body + at, &result, sizeof(result));
		at += sizeof(result);
		item = transfer + transfers * sizeof(RPC_SYNTAX_IDENTIFIER);
	}
	return pdu_write(&state->stream, pdu->type == PDU_BIND ? PDU_BIND_ACK : PDU_ALTER_CONTEXT_RESP, PDU_FIRST | PDU_LAST, pdu->call_id, body, at, NULL, 0);
}

static BOOL send_fault(connection_state *state, ULONG call_id, USHORT context, RPC_STATUS status)
{
	BYTE head[16];
	memset(head, 0, sizeof(head));
	*(USHORT *)(head + 4) = context;
	*(ULONG *)(head + 8) = status;
	return pdu_write(&state->stream, PDU_FAULT, PDU_FIRST | PDU_LAST, call_id, head, sizeof(head), NULL, 0);
}

static BOOL send_reply(connection_state *state, ULONG call_id, USHORT context, const BYTE *data, ULONG length)
{
	ULONG head[2];
	BYTE flags = PDU_FIRST;
	ULONG room = PDU_FRAGMENT_SIZE - 16 - sizeof(head), sent = 0, left = length;
	do {
		ULONG piece = left > room ? room : left;
		BYTE fragment = flags | (piece == left ? PDU_LAST : 0);
		head[0] = left;
		head[1] = context;
		if (!pdu_write(&state->stream, PDU_RESPONSE, fragment, call_id, head, sizeof(head), data + sent, piece))
			return FALSE;
		sent += piece;
		left -= piece;
		flags = 0;
	} while (left);
	return TRUE;
}

RPC_STATUS rpc_server_get_buffer(RPC_MESSAGE *message)
{
	message->Buffer = HeapAlloc(GetProcessHeap(), 0, message->BufferLength ? message->BufferLength : 1);
	return message->Buffer ? RPC_S_OK : RPC_S_OUT_OF_MEMORY;
}

typedef struct dispatch_call
{
	RPC_DISPATCH_FUNCTION function;
	RPC_MESSAGE *message;
} dispatch_call;

static void run_dispatch(void *arg)
{
	dispatch_call *call = arg;
	call->function(call->message);
}

static const UUID *type_of_object(const UUID *object)
{
	for (object_type *at = object_types; at; at = at->next)
		if (!memcmp(&at->object, object, sizeof(UUID)))
			return &at->type;
	return NULL;
}

static BOOL handle_call(connection_state *state, BYTE *data, ULONG length, ULONG call_id, USHORT context, USHORT opnum, const UUID *object, ULONG drep, BOOL no_reply)
{
	const RPC_SYNTAX_IDENTIFIER *abstract = NULL;
	for (INT i = 0; i < state->context_count; i++)
		if (state->contexts[i].id == context)
			abstract = &state->contexts[i].abstract;
	if (!abstract)
		return send_fault(state, call_id, context, RPC_S_UNKNOWN_IF);
	EnterCriticalSection(&server_lock);
	server_interface *entry = interface_for(abstract);
	RPC_SERVER_INTERFACE *spec = entry ? entry->spec : NULL;
	RPC_IF_CALLBACK_FN *callback = entry ? entry->callback : NULL;
	RPC_MGR_EPV *manager = entry ? entry->manager : NULL;
	if (entry)
		InterlockedIncrement(&entry->running);
	if (object && !UuidIsNil((UUID *)object, &(RPC_STATUS){0})) {
		const UUID *type = type_of_object(object);
		if (type && entry && memcmp(type, &entry->manager_type, sizeof(UUID)))
			manager = NULL;
	}
	LeaveCriticalSection(&server_lock);
	if (!entry)
		return send_fault(state, call_id, context, RPC_S_UNKNOWN_IF);
	InterlockedIncrement(&active_calls);
	BOOL alive = TRUE;
	RPC_DISPATCH_TABLE *table = spec->DispatchTable;
	if (!table || opnum >= table->DispatchTableCount) {
		alive = send_fault(state, call_id, context, RPC_S_PROCNUM_OUT_OF_RANGE);
		goto done;
	}
	state->binding.object = object ? *object : (UUID){0};
	state->binding.has_object = object != NULL;
	RPC_MESSAGE message;
	memset(&message, 0, sizeof(message));
	message.Handle = &state->binding;
	message.DataRepresentation = drep;
	message.Buffer = data;
	message.BufferLength = length;
	state->binding.request = data;
	state->binding.request_size = length;
	message.ProcNum = opnum;
	message.TransferSyntax = (RPC_SYNTAX_IDENTIFIER *)&ndr_syntax;
	message.RpcInterfaceInformation = spec;
	message.ManagerEpv = manager;
	message.RpcFlags = RPC_BUFFER_COMPLETE;
	TlsSetValue(call_slot(), &state->binding);
	if (callback && callback(spec, &state->binding) != RPC_S_OK) {
		alive = send_fault(state, call_id, context, RPC_S_ACCESS_DENIED);
		goto done;
	}
	dispatch_call call = {table->DispatchTable[opnum], &message};
	LONG code = rpc_guard(run_dispatch, &call);
	TlsSetValue(call_slot(), NULL);
	if (code) {
		alive = no_reply || send_fault(state, call_id, context, (RPC_STATUS)code);
	} else if (!no_reply) {
		BOOL replied = message.Buffer != data;
		alive = send_reply(state, call_id, context, replied ? message.Buffer : NULL, replied ? message.BufferLength : 0);
	}
	if (message.Buffer != data)
		rpc_free(message.Buffer);
done:
	InterlockedDecrement(&entry->running);
	InterlockedDecrement(&active_calls);
	return alive;
}

// A request arrives in fragments, which pile up here until the last.
static DWORD WINAPI connection_thread(void *arg)
{
	connection_state *state = arg;
	BYTE *data = NULL;
	ULONG used = 0, capacity = 0;
	UUID object;
	for (;;) {
		rpc_pdu pdu;
		if (!pdu_read(&state->stream, &pdu))
			break;
		BOOL alive = TRUE;
		ULONG size = pdu.frag_length - 16;
		switch (pdu.type) {
		case PDU_BIND:
		case PDU_ALTER_CONTEXT:
			alive = reply_bind(state, &pdu);
			break;
		case PDU_REQUEST: {
			ULONG head = 8 + (pdu.flags & PDU_OBJECT_UUID ? sizeof(UUID) : 0);
			if (size < head) {
				alive = FALSE;
				break;
			}
			if (pdu.flags & PDU_OBJECT_UUID)
				memcpy(&object, pdu.body + 8, sizeof(UUID));
			ULONG piece = size - head;
			if (used + piece > capacity) {
				ULONG wanted = used + piece + *(ULONG *)pdu.body;
				BYTE *grown = data ? HeapReAlloc(GetProcessHeap(), 0, data, wanted) : HeapAlloc(GetProcessHeap(), 0, wanted ? wanted : 1);
				if (!grown) {
					alive = FALSE;
					break;
				}
				data = grown;
				capacity = wanted;
			}
			memcpy(data + used, pdu.body + head, piece);
			used += piece;
			if (pdu.flags & PDU_LAST) {
				if (!data)
					data = HeapAlloc(GetProcessHeap(), 0, 1);
				ULONG drep = pdu.drep[0] | pdu.drep[1] << 8 | pdu.drep[2] << 16 | (ULONG)pdu.drep[3] << 24;
				alive = handle_call(state, data, used, pdu.call_id, *(USHORT *)(pdu.body + 4), *(USHORT *)(pdu.body + 6), pdu.flags & PDU_OBJECT_UUID ? &object : NULL, drep, (pdu.flags & 0x20) != 0);
				used = 0;
			}
			break;
		}
		case PDU_SHUTDOWN:
		case PDU_ORPHANED:
		case PDU_CANCEL:
			break;
		default:
			alive = FALSE;
			break;
		}
		rpc_free(pdu.body);
		if (!alive)
			break;
	}
	rpc_free(data);
	stream_close(&state->stream);
	rpc_free(state);
	return 0;
}

static DWORD WINAPI listener_thread(void *arg)
{
	server_endpoint *endpoint = arg;
	for (;;) {
		rpc_stream stream;
		BOOL accepted = listener_accept(&endpoint->listener, &stream);
		if (stopping && !(auto_listen > 0)) {
			if (accepted)
				stream_close(&stream);
			break;
		}
		if (!accepted)
			continue;
		connection_state *state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state) {
			stream_close(&stream);
			continue;
		}
		state->stream = stream;
		state->binding.magic = RPC_BINDING_MAGIC;
		state->binding.server = TRUE;
		state->binding.protseq = endpoint->protseq;
		state->binding.endpoint = endpoint->endpoint;
		InitializeSRWLock(&state->binding.lock);
		HANDLE thread = CreateThread(NULL, 0, connection_thread, state, 0, NULL);
		if (thread) {
			CloseHandle(thread);
		} else {
			stream_close(&stream);
			rpc_free(state);
		}
	}
	return 0;
}

static void start_endpoints(void)
{
	for (server_endpoint *at = endpoints; at; at = at->next)
		if (!at->thread) {
			listener_resume(&at->listener);
			at->thread = CreateThread(NULL, 0, listener_thread, at, 0, NULL);
		}
}

static void stop_endpoints(void)
{
	stopping = TRUE;
	for (server_endpoint *at = endpoints; at; at = at->next) {
		if (at->thread) {
			listener_wake(&at->listener);
			WaitForSingleObject(at->thread, INFINITE);
			CloseHandle(at->thread);
			at->thread = NULL;
			listener_pause(&at->listener);
		}
	}
	stopping = FALSE;
}

// ---- addresses ----

static RPC_STATUS use_protseq(const WCHAR *protseq, const WCHAR *endpoint)
{
	if (!protseq || !*protseq)
		return RPC_S_INVALID_RPC_PROTSEQ;
	if (wcsncmp(protseq, L"nca", 3))
		return RPC_S_INVALID_RPC_PROTSEQ;
	if (!protseq_supported(protseq))
		return RPC_S_PROTSEQ_NOT_SUPPORTED;
	EnterCriticalSection(&server_lock);
	RPC_STATUS status = RPC_S_OK;
	for (server_endpoint *at = endpoints; at; at = at->next) {
		// The same address asked for again is the same endpoint; without one, any on this sequence will do.
		if (!lstrcmpW(at->protseq, protseq) && (!endpoint || !lstrcmpW(at->endpoint, endpoint))) {
			LeaveCriticalSection(&server_lock);
			return RPC_S_OK;
		}
	}
	server_endpoint *entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry));
	if (!entry) {
		LeaveCriticalSection(&server_lock);
		return RPC_S_OUT_OF_MEMORY;
	}
	WCHAR generated[64];
	if (!endpoint && lstrcmpW(protseq, L"ncacn_ip_tcp")) {
		wsprintfW(generated, !lstrcmpW(protseq, L"ncalrpc") ? L"LRPC%08lx.%08lx" : L"\\pipe\\raven%08lx.%03lx", GetCurrentProcessId(), InterlockedIncrement((LONG *)&endpoint_counter));
		endpoint = generated;
	}
	WCHAR *resolved = NULL;
	status = listener_open(&entry->listener, protseq, endpoint, &resolved);
	if (status != RPC_S_OK) {
		rpc_free(entry);
		LeaveCriticalSection(&server_lock);
		return status;
	}
	entry->protseq = rpc_copy(protseq);
	entry->endpoint = resolved;
	entry->next = endpoints;
	endpoints = entry;
	if (listening || auto_listen > 0)
		start_endpoints();
	LeaveCriticalSection(&server_lock);
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcServerUseProtseqEpExW(RPC_WSTR protseq, UINT calls, RPC_WSTR endpoint, LPVOID security, PRPC_POLICY policy)
{
	(void)calls; (void)security; (void)policy;
	return use_protseq(protseq, endpoint);
}

RPC_STATUS RPC_ENTRY RpcServerUseProtseqEpW(RPC_WSTR protseq, UINT calls, RPC_WSTR endpoint, LPVOID security)
{
	return RpcServerUseProtseqEpExW(protseq, calls, endpoint, security, NULL);
}

RPC_STATUS RPC_ENTRY RpcServerUseProtseqEpExA(RPC_CSTR protseq, UINT calls, RPC_CSTR endpoint, LPVOID security, PRPC_POLICY policy)
{
	WCHAR *wide_protseq = rpc_widen((const char *)protseq), *wide_endpoint = rpc_widen((const char *)endpoint);
	RPC_STATUS status = RpcServerUseProtseqEpExW(wide_protseq, calls, wide_endpoint, security, policy);
	rpc_free(wide_protseq);
	rpc_free(wide_endpoint);
	return status;
}

RPC_STATUS RPC_ENTRY RpcServerUseProtseqEpA(RPC_CSTR protseq, UINT calls, RPC_CSTR endpoint, LPVOID security)
{
	return RpcServerUseProtseqEpExA(protseq, calls, endpoint, security, NULL);
}

RPC_STATUS RPC_ENTRY RpcServerUseProtseqW(RPC_WSTR protseq, unsigned int calls, void *security)
{
	(void)calls; (void)security;
	return use_protseq(protseq, NULL);
}

RPC_STATUS RPC_ENTRY RpcServerUseProtseqA(RPC_CSTR protseq, unsigned int calls, void *security)
{
	WCHAR *wide = rpc_widen((const char *)protseq);
	RPC_STATUS status = RpcServerUseProtseqW(wide, calls, security);
	rpc_free(wide);
	return status;
}

RPC_STATUS RPC_ENTRY RpcServerInqBindings(RPC_BINDING_VECTOR **out)
{
	EnterCriticalSection(&server_lock);
	ULONG count = 0;
	for (server_endpoint *at = endpoints; at; at = at->next)
		count++;
	if (!count) {
		*out = NULL;
		LeaveCriticalSection(&server_lock);
		return RPC_S_NO_BINDINGS;
	}
	RPC_BINDING_VECTOR *vector = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, offsetof(RPC_BINDING_VECTOR, BindingH) + count * sizeof(RPC_BINDING_HANDLE));
	if (!vector) {
		LeaveCriticalSection(&server_lock);
		return RPC_S_OUT_OF_MEMORY;
	}
	WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1];
	DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
	if (!GetComputerNameW(computer, &length))
		computer[0] = 0;
	ULONG index = 0;
	for (server_endpoint *at = endpoints; at; at = at->next) {
		RPC_WSTR text = NULL;
		if (RpcStringBindingComposeW(NULL, at->protseq, computer, at->endpoint, NULL, &text) == RPC_S_OK) {
			if (RpcBindingFromStringBindingW(text, &vector->BindingH[index]) == RPC_S_OK)
				index++;
			rpc_free(text);
		}
	}
	vector->Count = index;
	*out = vector;
	LeaveCriticalSection(&server_lock);
	return RPC_S_OK;
}

// ---- interfaces ----

static RPC_STATUS register_interface(RPC_IF_HANDLE handle, UUID *type, RPC_MGR_EPV *manager, UINT flags, RPC_IF_CALLBACK_FN *callback)
{
	RPC_SERVER_INTERFACE *spec = handle;
	server_interface *entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry));
	if (!entry)
		return RPC_S_OUT_OF_MEMORY;
	entry->spec = spec;
	if (type)
		entry->manager_type = *type;
	entry->manager = manager ? manager : spec->DefaultManagerEpv;
	entry->flags = flags;
	entry->callback = callback;
	EnterCriticalSection(&server_lock);
	entry->next = interfaces;
	interfaces = entry;
	if (flags & RPC_IF_AUTOLISTEN) {
		auto_listen++;
		start_endpoints();
	}
	LeaveCriticalSection(&server_lock);
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcServerRegisterIf3(RPC_IF_HANDLE handle, UUID *type, RPC_MGR_EPV *manager, UINT flags, UINT calls, UINT size, RPC_IF_CALLBACK_FN *callback, void *security)
{
	(void)calls; (void)size; (void)security;
	return register_interface(handle, type, manager, flags, callback);
}

RPC_STATUS RPC_ENTRY RpcServerRegisterIf2(RPC_IF_HANDLE handle, UUID *type, RPC_MGR_EPV *manager, UINT flags, UINT calls, UINT size, RPC_IF_CALLBACK_FN *callback)
{
	return register_interface(handle, type, manager, flags, callback);
}

RPC_STATUS RPC_ENTRY RpcServerRegisterIfEx(RPC_IF_HANDLE handle, UUID *type, RPC_MGR_EPV *manager, UINT flags, UINT calls, RPC_IF_CALLBACK_FN *callback)
{
	(void)calls;
	return register_interface(handle, type, manager, flags, callback);
}

RPC_STATUS RPC_ENTRY RpcServerRegisterIf(RPC_IF_HANDLE handle, UUID *type, RPC_MGR_EPV *manager)
{
	return register_interface(handle, type, manager, 0, NULL);
}

RPC_STATUS RPC_ENTRY RpcServerUnregisterIf(RPC_IF_HANDLE handle, UUID *type, UINT wait)
{
	RPC_STATUS status = handle ? RPC_S_UNKNOWN_IF : RPC_S_OK;
	BOOL kept = FALSE, removed = FALSE;
	UUID nil = {0};
	EnterCriticalSection(&server_lock);
	server_interface **at = &interfaces;
	while (*at) {
		server_interface *entry = *at;
		BOOL same_handle = !handle || entry->spec == (RPC_SERVER_INTERFACE *)handle;
		BOOL same_type = !handle || !type || !memcmp(type, &entry->manager_type, sizeof(UUID)) || (!type && !memcmp(&nil, &entry->manager_type, sizeof(UUID)));
		// "All of them" leaves alone an interface that keeps its own listening going.
		if (!handle && entry->flags & RPC_IF_AUTOLISTEN)
			kept = TRUE;
		if (!same_handle || !same_type || (!handle && entry->flags & RPC_IF_AUTOLISTEN)) {
			at = &entry->next;
			continue;
		}
		removed = TRUE;
		*at = entry->next;
		if (entry->flags & RPC_IF_AUTOLISTEN)
			auto_listen--;
		LeaveCriticalSection(&server_lock);
		while (wait && entry->running)
			Sleep(10);
		// A call still running holds a reference to the entry, so an unwaited one stays allocated.
		if (!entry->running)
			rpc_free(entry);
		EnterCriticalSection(&server_lock);
		at = &interfaces;
		status = RPC_S_OK;
	}
	if (kept && !removed)
		status = RPC_S_UNKNOWN_MGR_TYPE;
	if (!auto_listen && !listening)
		stop_endpoints();
	LeaveCriticalSection(&server_lock);
	return status;
}

RPC_STATUS RPC_ENTRY RpcServerUnregisterIfEx(RPC_IF_HANDLE handle, UUID *type, int rundown)
{
	(void)rundown;
	return RpcServerUnregisterIf(handle, type, TRUE);
}

RPC_STATUS RPC_ENTRY RpcObjectSetType(UUID *object, UUID *type)
{
	RPC_STATUS ignored;
	if (UuidIsNil(object, &ignored))
		return RPC_S_INVALID_OBJECT;
	EnterCriticalSection(&server_lock);
	object_type **at = &object_types;
	while (*at && memcmp(&(*at)->object, object, sizeof(UUID)))
		at = &(*at)->next;
	RPC_STATUS status = RPC_S_OK;
	if (!type || UuidIsNil(type, &ignored)) {
		if (*at) {
			object_type *old = *at;
			*at = old->next;
			rpc_free(old);
		} else {
			status = RPC_S_OBJECT_NOT_FOUND;
		}
	} else if (*at) {
		status = RPC_S_ALREADY_REGISTERED;
	} else {
		object_type *entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry));
		if (entry) {
			entry->object = *object;
			entry->type = *type;
			*at = entry;
		} else {
			status = RPC_S_OUT_OF_MEMORY;
		}
	}
	LeaveCriticalSection(&server_lock);
	return status;
}

// ---- listening ----

RPC_STATUS RPC_ENTRY RpcMgmtWaitServerListen(void)
{
	EnterCriticalSection(&server_lock);
	HANDLE event = stopped_event;
	BOOL open = listen_open;
	LeaveCriticalSection(&server_lock);
	if (!open)
		return RPC_S_NOT_LISTENING;
	WaitForSingleObject(event, INFINITE);
	while (active_calls)
		Sleep(10);
	EnterCriticalSection(&server_lock);
	if (!listening)
		listen_open = FALSE;
	LeaveCriticalSection(&server_lock);
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcServerListen(UINT min_calls, UINT max_calls, UINT dont_wait)
{
	(void)max_calls;
	if (!min_calls)
		return RPC_S_MAX_CALLS_TOO_SMALL;
	EnterCriticalSection(&server_lock);
	if (listening) {
		LeaveCriticalSection(&server_lock);
		return RPC_S_ALREADY_LISTENING;
	}
	if (!endpoints) {
		LeaveCriticalSection(&server_lock);
		return RPC_S_NO_PROTSEQS_REGISTERED;
	}
	listening = TRUE;
	listen_open = TRUE;
	if (stopped_event)
		ResetEvent(stopped_event);
	else
		stopped_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	start_endpoints();
	LeaveCriticalSection(&server_lock);
	return dont_wait ? RPC_S_OK : RpcMgmtWaitServerListen();
}

RPC_STATUS RPC_ENTRY RpcMgmtStopServerListening(RPC_BINDING_HANDLE handle)
{
	if (handle)
		return RPC_S_WRONG_KIND_OF_BINDING;
	EnterCriticalSection(&server_lock);
	if (!listen_open) {
		LeaveCriticalSection(&server_lock);
		return RPC_S_NOT_LISTENING;
	}
	if (listening) {
		listening = FALSE;
		if (!auto_listen)
			stop_endpoints();
		SetEvent(stopped_event);
	}
	LeaveCriticalSection(&server_lock);
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcMgmtIsServerListening(RPC_BINDING_HANDLE handle)
{
	if (handle) {
		rpc_binding *binding = rpc_binding_of(handle);
		if (!binding)
			return RPC_S_INVALID_BINDING;
		rpc_stream stream;
		if (stream_connect(&stream, binding->protseq, binding->network, binding->endpoint) != RPC_S_OK)
			return RPC_S_NOT_LISTENING;
		stream_close(&stream);
		return RPC_S_OK;
	}
	return listening || auto_listen > 0 ? RPC_S_OK : RPC_S_NOT_LISTENING;
}

RPC_STATUS RPC_ENTRY RpcMgmtSetServerStackSize(ULONG size)
{
	(void)size;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcMgmtSetAuthorizationFn(RPC_MGMT_AUTHORIZATION_FN function)
{
	(void)function;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcMgmtInqIfIds(RPC_BINDING_HANDLE handle, RPC_IF_ID_VECTOR **out)
{
	(void)handle; (void)out;
	return RPC_S_CANNOT_SUPPORT;
}

RPC_STATUS RPC_ENTRY RpcMgmtInqStats(RPC_BINDING_HANDLE handle, RPC_STATS_VECTOR **out)
{
	(void)handle;
	RPC_STATS_VECTOR *vector = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, offsetof(RPC_STATS_VECTOR, Stats) + 4 * sizeof(ULONG));
	if (!vector)
		return RPC_S_OUT_OF_MEMORY;
	vector->Count = 4;
	*out = vector;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcMgmtStatsVectorFree(RPC_STATS_VECTOR **vector)
{
	rpc_free(*vector);
	*vector = NULL;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY I_RpcServerStartListening(HWND window)
{
	(void)window;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY I_RpcServerStopListening(void)
{
	return RPC_S_OK;
}

UINT RPC_ENTRY I_RpcWindowProc(void *window, UINT message, UINT wparam, ULONG lparam)
{
	(void)window; (void)message; (void)wparam; (void)lparam;
	return 0;
}

// ---- who is calling ----

BOOL rpc_in_request(const void *pointer)
{
	rpc_binding *binding = TlsGetValue(call_slot());
	if (!binding || !binding->request)
		return FALSE;
	return (const BYTE *)pointer >= binding->request && (const BYTE *)pointer < binding->request + binding->request_size;
}

RPC_BINDING_HANDLE RPC_ENTRY I_RpcGetCurrentCallHandle(void)
{
	return TlsGetValue(call_slot());
}

RPC_STATUS RPC_ENTRY I_RpcBindingInqLocalClientPID(RPC_BINDING_HANDLE handle, ULONG *pid)
{
	rpc_binding *binding = rpc_binding_of(handle ? handle : I_RpcGetCurrentCallHandle());
	if (!binding)
		return RPC_S_NO_CALL_ACTIVE;
	if (!binding->server || lstrcmpW(binding->protseq, L"ncalrpc"))
		return RPC_S_INVALID_BINDING;
	connection_state *state = CONTAINING_RECORD(binding, connection_state, binding);
	ULONG client = 0;
	if (!GetNamedPipeClientProcessId(state->stream.pipe, &client))
		return RPC_S_INVALID_BINDING;
	*pid = client;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcImpersonateClient(RPC_BINDING_HANDLE handle)
{
	return rpc_binding_of(handle ? handle : I_RpcGetCurrentCallHandle()) ? RPC_S_OK : RPC_S_NO_CALL_ACTIVE;
}

RPC_STATUS RPC_ENTRY RpcRevertToSelfEx(RPC_BINDING_HANDLE handle)
{
	(void)handle;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcRevertToSelf(void)
{
	return RPC_S_OK;
}

// No provider verifies anybody, so no client ever arrives authenticated.
RPC_STATUS RPC_ENTRY RpcBindingInqAuthClientExW(RPC_BINDING_HANDLE handle, RPC_AUTHZ_HANDLE *privileges, RPC_WSTR *principal, ULONG *level, ULONG *service, ULONG *authz, ULONG flags)
{
	(void)privileges; (void)principal; (void)level; (void)service; (void)authz; (void)flags;
	return rpc_binding_of(handle ? handle : I_RpcGetCurrentCallHandle()) ? RPC_S_BINDING_HAS_NO_AUTH : RPC_S_NO_CALL_ACTIVE;
}

RPC_STATUS RPC_ENTRY RpcBindingInqAuthClientExA(RPC_BINDING_HANDLE handle, RPC_AUTHZ_HANDLE *privileges, RPC_CSTR *principal, ULONG *level, ULONG *service, ULONG *authz, ULONG flags)
{
	return RpcBindingInqAuthClientExW(handle, privileges, (RPC_WSTR *)principal, level, service, authz, flags);
}

RPC_STATUS RPC_ENTRY RpcBindingInqAuthClientW(RPC_BINDING_HANDLE handle, RPC_AUTHZ_HANDLE *privileges, RPC_WSTR *principal, ULONG *level, ULONG *service, ULONG *authz)
{
	return RpcBindingInqAuthClientExW(handle, privileges, principal, level, service, authz, 0);
}

RPC_STATUS RPC_ENTRY RpcBindingInqAuthClientA(RPC_BINDING_HANDLE handle, RPC_AUTHZ_HANDLE *privileges, RPC_CSTR *principal, ULONG *level, ULONG *service, ULONG *authz)
{
	return RpcBindingInqAuthClientExA(handle, privileges, principal, level, service, authz, 0);
}

RPC_STATUS RPC_ENTRY RpcGetAuthorizationContextForClient(RPC_BINDING_HANDLE handle, BOOL impersonate, void *reserved1, PLARGE_INTEGER expiration, LUID reserved2, DWORD reserved3, void *reserved4, void **out)
{
	(void)handle; (void)impersonate; (void)reserved1; (void)expiration; (void)reserved2; (void)reserved3; (void)reserved4; (void)out;
	return RPC_S_BINDING_HAS_NO_AUTH;
}

RPC_STATUS RPC_ENTRY RpcServerRegisterAuthInfoW(RPC_WSTR principal, ULONG service, RPC_AUTH_KEY_RETRIEVAL_FN retrieve, void *arg)
{
	(void)principal; (void)retrieve; (void)arg;
	return service == RPC_C_AUTHN_NONE || service == RPC_C_AUTHN_WINNT || service == RPC_C_AUTHN_GSS_NEGOTIATE || service == RPC_C_AUTHN_GSS_KERBEROS || service == RPC_C_AUTHN_DEFAULT ? RPC_S_OK : RPC_S_UNKNOWN_AUTHN_SERVICE;
}

RPC_STATUS RPC_ENTRY RpcServerRegisterAuthInfoA(RPC_CSTR principal, ULONG service, RPC_AUTH_KEY_RETRIEVAL_FN retrieve, void *arg)
{
	(void)principal;
	return RpcServerRegisterAuthInfoW(NULL, service, retrieve, arg);
}

RPC_STATUS RPC_ENTRY RpcServerInqDefaultPrincNameW(ULONG service, RPC_WSTR *out)
{
	if (service != RPC_C_AUTHN_WINNT && service != RPC_C_AUTHN_GSS_NEGOTIATE && service != RPC_C_AUTHN_GSS_KERBEROS)
		return RPC_S_UNKNOWN_AUTHN_SERVICE;
	WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1], user[256];
	DWORD computer_length = MAX_COMPUTERNAME_LENGTH + 1, user_length = 256;
	if (!GetComputerNameW(computer, &computer_length) || !GetEnvironmentVariableW(L"USERNAME", user, user_length))
		return RPC_S_INTERNAL_ERROR;
	WCHAR *name = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(computer) + lstrlenW(user) + 2) * sizeof(WCHAR));
	if (!name)
		return RPC_S_OUT_OF_MEMORY;
	wsprintfW(name, L"%s\\%s", computer, user);
	*out = name;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcServerInqDefaultPrincNameA(ULONG service, RPC_CSTR *out)
{
	RPC_WSTR wide = NULL;
	RPC_STATUS status = RpcServerInqDefaultPrincNameW(service, &wide);
	if (status == RPC_S_OK) {
		*out = (RPC_CSTR)rpc_narrow(wide);
		rpc_free(wide);
	}
	return status;
}
