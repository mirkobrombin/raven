/* SPDX-License-Identifier: MIT */
#include "rpc_own.h"

rpc_binding *rpc_binding_of(RPC_BINDING_HANDLE handle)
{
	rpc_binding *binding = handle;
	return binding && binding->magic == RPC_BINDING_MAGIC ? binding : NULL;
}

// ---- string bindings: [object@]protseq:[network][endpoint,option=value,...] ----

static SIZE_T text_length(const WCHAR *text)
{
	return text ? lstrlenW(text) : 0;
}

static WCHAR *append(WCHAR *at, const WCHAR *text)
{
	if (text)
		while (*text)
			*at++ = *text++;
	return at;
}

// A backslash takes whatever follows it literally, so one meant as text goes out doubled.
static WCHAR *append_escaped(WCHAR *at, const WCHAR *text)
{
	if (text)
		for (; *text; text++) {
			if (*text == L'\\')
				*at++ = L'\\';
			*at++ = *text;
		}
	return at;
}

RPC_STATUS RPC_ENTRY RpcStringBindingComposeW(RPC_WSTR object, RPC_WSTR protseq, RPC_WSTR network, RPC_WSTR endpoint, RPC_WSTR options, RPC_WSTR *out)
{
	SIZE_T size = text_length(object) + text_length(protseq) + 2 * (text_length(network) + text_length(endpoint) + text_length(options)) + 6;
	WCHAR *text = HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR));
	if (!text)
		return RPC_S_OUT_OF_MEMORY;
	WCHAR *at = text;
	if (object && *object) {
		at = append(at, object);
		*at++ = L'@';
	}
	if (protseq && *protseq) {
		at = append(at, protseq);
		*at++ = L':';
	}
	at = append_escaped(at, network);
	if ((endpoint && *endpoint) || (options && *options)) {
		*at++ = L'[';
		at = append_escaped(at, endpoint);
		if (options && *options) {
			*at++ = L',';
			at = append_escaped(at, options);
		}
		*at++ = L']';
	}
	*at = 0;
	*out = text;
	return RPC_S_OK;
}

#define WITH_WIDE(name, source) WCHAR *name = rpc_widen((const char *)(source))

RPC_STATUS RPC_ENTRY RpcStringBindingComposeA(RPC_CSTR object, RPC_CSTR protseq, RPC_CSTR network, RPC_CSTR endpoint, RPC_CSTR options, RPC_CSTR *out)
{
	WITH_WIDE(wide_object, object);
	WITH_WIDE(wide_protseq, protseq);
	WITH_WIDE(wide_network, network);
	WITH_WIDE(wide_endpoint, endpoint);
	WITH_WIDE(wide_options, options);
	RPC_WSTR composed = NULL;
	RPC_STATUS status = RpcStringBindingComposeW(wide_object, wide_protseq, wide_network, wide_endpoint, wide_options, &composed);
	if (status == RPC_S_OK) {
		*out = (RPC_CSTR)rpc_narrow(composed);
		rpc_free(composed);
	}
	rpc_free(wide_object);
	rpc_free(wide_protseq);
	rpc_free(wide_network);
	rpc_free(wide_endpoint);
	rpc_free(wide_options);
	return status;
}

static const WCHAR *find_char(const WCHAR *text, WCHAR c)
{
	for (; *text; text++) {
		if (*text == L'\\' && text[1])
			text++;
		else if (*text == c)
			return text;
	}
	return NULL;
}

static void unescape(WCHAR *text)
{
	WCHAR *to = text;
	if (!text)
		return;
	for (; *text; text++) {
		if (*text == L'\\' && text[1])
			text++;
		*to++ = *text;
	}
	*to = 0;
}

static WCHAR *copy_range(const WCHAR *start, const WCHAR *end)
{
	SIZE_T length = end - start;
	WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
	if (copy) {
		memcpy(copy, start, length * sizeof(WCHAR));
		copy[length] = 0;
	}
	return copy;
}

RPC_STATUS RPC_ENTRY RpcStringBindingParseW(RPC_WSTR binding, RPC_WSTR *object, RPC_WSTR *protseq, RPC_WSTR *network, RPC_WSTR *endpoint, RPC_WSTR *options)
{
	WCHAR *found[5] = {NULL, NULL, NULL, NULL, NULL};
	const WCHAR *at = binding;
	RPC_STATUS status = RPC_S_INVALID_STRING_BINDING;
	RPC_WSTR *targets[5] = {object, protseq, network, endpoint, options};
	for (INT i = 0; i < 5; i++)
		if (targets[i])
			*targets[i] = NULL;
	const WCHAR *mark = find_char(at, L'@');
	if (mark) {
		found[0] = copy_range(at, mark);
		UUID check;
		if (UuidFromStringW(found[0], &check) != RPC_S_OK) {
			status = RPC_S_INVALID_STRING_UUID;
			goto fail;
		}
		at = mark + 1;
	}
	mark = find_char(at, L':');
	if (!mark)
		goto fail;
	found[1] = copy_range(at, mark);
	at = mark + 1;
	mark = find_char(at, L'[');
	if (mark) {
		found[2] = copy_range(at, mark);
		const WCHAR *close = find_char(mark, L']');
		if (!close || close[1])
			goto fail;
		const WCHAR *inner = mark + 1, *comma = find_char(inner, L',');
		if (comma && comma < close) {
			found[3] = copy_range(inner, comma);
			found[4] = copy_range(comma + 1, close);
		} else {
			found[3] = copy_range(inner, close);
		}
		// "endpoint=value" names what is otherwise the first thing in the brackets.
		static const WCHAR tag[] = L"endpoint=";
		if (found[3] && !wcsncmp(found[3], tag, 9)) {
			WCHAR *plain = rpc_copy(found[3] + 9);
			rpc_free(found[3]);
			found[3] = plain;
		}
	} else if (*at) {
		found[2] = rpc_copy(at);
	}
	for (INT i = 2; i < 5; i++)
		unescape(found[i]);
	if (!found[4])
		found[4] = rpc_copy(L"");
	#define HAND_OUT(index, target) do { if (target) { *(target) = found[index]; found[index] = NULL; } else { rpc_free(found[index]); found[index] = NULL; } } while (0)
	HAND_OUT(0, object);
	HAND_OUT(1, protseq);
	HAND_OUT(2, network);
	HAND_OUT(3, endpoint);
	HAND_OUT(4, options);
	#undef HAND_OUT
	return RPC_S_OK;
fail:
	for (INT i = 0; i < 5; i++)
		rpc_free(found[i]);
	return status;
}

RPC_STATUS RPC_ENTRY RpcStringBindingParseA(RPC_CSTR binding, RPC_CSTR *object, RPC_CSTR *protseq, RPC_CSTR *network, RPC_CSTR *endpoint, RPC_CSTR *options)
{
	WITH_WIDE(wide, binding);
	RPC_WSTR parts[5] = {NULL, NULL, NULL, NULL, NULL};
	RPC_STATUS status = RpcStringBindingParseW(wide, object ? &parts[0] : NULL, protseq ? &parts[1] : NULL, network ? &parts[2] : NULL, endpoint ? &parts[3] : NULL, options ? &parts[4] : NULL);
	rpc_free(wide);
	RPC_CSTR *targets[5] = {object, protseq, network, endpoint, options};
	for (INT i = 0; i < 5; i++) {
		if (targets[i])
			*targets[i] = (RPC_CSTR)rpc_narrow(parts[i]);
		rpc_free(parts[i]);
	}
	return status;
}

// ---- binding handles ----

static rpc_binding *binding_new(void)
{
	rpc_binding *binding = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*binding));
	if (binding) {
		binding->magic = RPC_BINDING_MAGIC;
		InitializeSRWLock(&binding->lock);
		binding->auth_level = RPC_C_AUTHN_LEVEL_NONE;
	}
	return binding;
}

RPC_STATUS RPC_ENTRY RpcBindingFromStringBindingW(RPC_WSTR text, RPC_BINDING_HANDLE *out)
{
	RPC_WSTR object = NULL, protseq = NULL, network = NULL, endpoint = NULL, options = NULL;
	RPC_STATUS status = RpcStringBindingParseW(text, &object, &protseq, &network, &endpoint, &options);
	if (status != RPC_S_OK)
		return status;
	if (!protseq || !*protseq) {
		status = RPC_S_INVALID_RPC_PROTSEQ;
	} else if (!protseq_supported(protseq)) {
		status = RPC_S_PROTSEQ_NOT_SUPPORTED;
	} else {
		rpc_binding *binding = binding_new();
		if (!binding) {
			status = RPC_S_OUT_OF_MEMORY;
		} else {
			binding->protseq = protseq;
			binding->network = network;
			binding->endpoint = endpoint;
			binding->options = options;
			protseq = network = endpoint = options = NULL;
			if (object && *object) {
				status = UuidFromStringW(object, &binding->object);
				binding->has_object = TRUE;
			}
			if (status == RPC_S_OK) {
				*out = binding;
			} else {
				RPC_BINDING_HANDLE handle = binding;
				RpcBindingFree(&handle);
			}
		}
	}
	rpc_free(object);
	rpc_free(protseq);
	rpc_free(network);
	rpc_free(endpoint);
	rpc_free(options);
	return status;
}

RPC_STATUS RPC_ENTRY RpcBindingFromStringBindingA(RPC_CSTR text, RPC_BINDING_HANDLE *out)
{
	WITH_WIDE(wide, text);
	RPC_STATUS status = RpcBindingFromStringBindingW(wide, out);
	rpc_free(wide);
	return status;
}

RPC_STATUS RPC_ENTRY RpcBindingToStringBindingW(RPC_BINDING_HANDLE handle, RPC_WSTR *out)
{
	rpc_binding *binding = rpc_binding_of(handle);
	if (!binding)
		return RPC_S_INVALID_BINDING;
	RPC_WSTR object = NULL;
	if (binding->has_object)
		UuidToStringW(&binding->object, &object);
	RPC_STATUS status = RpcStringBindingComposeW(object, binding->protseq, binding->network, binding->endpoint, binding->options, out);
	rpc_free(object);
	return status;
}

RPC_STATUS RPC_ENTRY RpcBindingToStringBindingA(RPC_BINDING_HANDLE handle, RPC_CSTR *out)
{
	RPC_WSTR wide = NULL;
	RPC_STATUS status = RpcBindingToStringBindingW(handle, &wide);
	if (status == RPC_S_OK) {
		*out = (RPC_CSTR)rpc_narrow(wide);
		rpc_free(wide);
	}
	return status;
}

static void connection_close(rpc_connection *connection)
{
	stream_close(&connection->stream);
	while (connection->contexts) {
		rpc_bound_context *next = connection->contexts->next;
		rpc_free(connection->contexts);
		connection->contexts = next;
	}
	rpc_free(connection);
}

RPC_STATUS RPC_ENTRY RpcBindingFree(RPC_BINDING_HANDLE *handle)
{
	rpc_binding *binding = handle ? rpc_binding_of(*handle) : NULL;
	if (!binding)
		return RPC_S_INVALID_BINDING;
	while (binding->idle) {
		rpc_connection *next = binding->idle->next;
		connection_close(binding->idle);
		binding->idle = next;
	}
	rpc_free(binding->protseq);
	rpc_free(binding->network);
	rpc_free(binding->endpoint);
	rpc_free(binding->options);
	rpc_free(binding->principal);
	binding->magic = 0;
	rpc_free(binding);
	*handle = NULL;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcBindingCopy(RPC_BINDING_HANDLE source, RPC_BINDING_HANDLE *out)
{
	rpc_binding *from = rpc_binding_of(source);
	if (!from)
		return RPC_S_INVALID_BINDING;
	rpc_binding *copy = binding_new();
	if (!copy)
		return RPC_S_OUT_OF_MEMORY;
	copy->protseq = rpc_copy(from->protseq);
	copy->network = rpc_copy(from->network);
	copy->endpoint = rpc_copy(from->endpoint);
	copy->options = rpc_copy(from->options);
	copy->object = from->object;
	copy->has_object = from->has_object;
	copy->auth_level = from->auth_level;
	copy->auth_service = from->auth_service;
	copy->authz_service = from->authz_service;
	copy->principal = rpc_copy(from->principal);
	copy->identity = from->identity;
	*out = copy;
	return RPC_S_OK;
}

// A handle turned into a client handle from the server side of a call: it sees the same address, and no call.
RPC_STATUS RPC_ENTRY RpcBindingServerFromClient(RPC_BINDING_HANDLE client, RPC_BINDING_HANDLE *server)
{
	RPC_STATUS status = RpcBindingCopy(client ? client : I_RpcGetCurrentCallHandle(), server);
	if (status != RPC_S_OK)
		return status;
	rpc_binding *copy = *server;
	copy->server = FALSE;
	if (!copy->network)
		copy->network = rpc_copy(L".");
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcBindingReset(RPC_BINDING_HANDLE handle)
{
	rpc_binding *binding = rpc_binding_of(handle);
	if (!binding)
		return RPC_S_INVALID_BINDING;
	rpc_free(binding->endpoint);
	binding->endpoint = NULL;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcBindingSetObject(RPC_BINDING_HANDLE handle, UUID *object)
{
	rpc_binding *binding = rpc_binding_of(handle);
	if (!binding)
		return RPC_S_INVALID_BINDING;
	if (object)
		binding->object = *object;
	else
		memset(&binding->object, 0, sizeof(UUID));
	RPC_STATUS ignored;
	binding->has_object = !UuidIsNil(&binding->object, &ignored);
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcBindingInqObject(RPC_BINDING_HANDLE handle, UUID *object)
{
	rpc_binding *binding = rpc_binding_of(handle);
	if (!binding)
		return RPC_S_INVALID_BINDING;
	*object = binding->object;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcBindingVectorFree(RPC_BINDING_VECTOR **vector)
{
	if (!vector || !*vector)
		return RPC_S_INVALID_ARG;
	for (ULONG i = 0; i < (*vector)->Count; i++)
		RpcBindingFree(&(*vector)->BindingH[i]);
	rpc_free(*vector);
	*vector = NULL;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcBindingSetOption(RPC_BINDING_HANDLE handle, ULONG option, ULONG_PTR value)
{
	(void)option; (void)value;
	return rpc_binding_of(handle) ? RPC_S_OK : RPC_S_INVALID_BINDING;
}

// There is no security provider: the settings are remembered so they read back, and a call runs unauthenticated.
RPC_STATUS RPC_ENTRY RpcBindingSetAuthInfoExW(RPC_BINDING_HANDLE handle, RPC_WSTR principal, ULONG level, ULONG service, RPC_AUTH_IDENTITY_HANDLE identity, ULONG authz, RPC_SECURITY_QOS *qos)
{
	(void)qos;
	rpc_binding *binding = rpc_binding_of(handle);
	if (!binding)
		return RPC_S_INVALID_BINDING;
	if (level == RPC_C_AUTHN_LEVEL_DEFAULT)
		level = RPC_C_AUTHN_LEVEL_NONE;
	if (service == RPC_C_AUTHN_DEFAULT)
		service = level == RPC_C_AUTHN_LEVEL_NONE ? RPC_C_AUTHN_NONE : RPC_C_AUTHN_WINNT;
	if (service != RPC_C_AUTHN_NONE && service != RPC_C_AUTHN_WINNT && service != RPC_C_AUTHN_GSS_NEGOTIATE && service != RPC_C_AUTHN_GSS_KERBEROS)
		return RPC_S_UNKNOWN_AUTHN_SERVICE;
	binding->auth_level = service == RPC_C_AUTHN_NONE ? RPC_C_AUTHN_LEVEL_NONE : level;
	binding->auth_service = service;
	binding->authz_service = authz;
	binding->identity = identity;
	rpc_free(binding->principal);
	binding->principal = rpc_copy(principal);
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcBindingSetAuthInfoExA(RPC_BINDING_HANDLE handle, RPC_CSTR principal, ULONG level, ULONG service, RPC_AUTH_IDENTITY_HANDLE identity, ULONG authz, RPC_SECURITY_QOS *qos)
{
	WITH_WIDE(wide, principal);
	RPC_STATUS status = RpcBindingSetAuthInfoExW(handle, wide, level, service, identity, authz, qos);
	rpc_free(wide);
	return status;
}

RPC_STATUS RPC_ENTRY RpcBindingSetAuthInfoW(RPC_BINDING_HANDLE handle, RPC_WSTR principal, ULONG level, ULONG service, RPC_AUTH_IDENTITY_HANDLE identity, ULONG authz)
{
	return RpcBindingSetAuthInfoExW(handle, principal, level, service, identity, authz, NULL);
}

RPC_STATUS RPC_ENTRY RpcBindingSetAuthInfoA(RPC_BINDING_HANDLE handle, RPC_CSTR principal, ULONG level, ULONG service, RPC_AUTH_IDENTITY_HANDLE identity, ULONG authz)
{
	return RpcBindingSetAuthInfoExA(handle, principal, level, service, identity, authz, NULL);
}

RPC_STATUS RPC_ENTRY RpcBindingInqAuthInfoExW(RPC_BINDING_HANDLE handle, RPC_WSTR *principal, ULONG *level, ULONG *service, RPC_AUTH_IDENTITY_HANDLE *identity, ULONG *authz, ULONG version, RPC_SECURITY_QOS *qos)
{
	(void)version; (void)qos;
	rpc_binding *binding = rpc_binding_of(handle);
	if (!binding)
		return RPC_S_INVALID_BINDING;
	if (binding->auth_service == RPC_C_AUTHN_NONE || binding->auth_level == RPC_C_AUTHN_LEVEL_NONE)
		return RPC_S_BINDING_HAS_NO_AUTH;
	if (principal)
		*principal = rpc_copy(binding->principal);
	if (level)
		*level = binding->auth_level;
	if (service)
		*service = binding->auth_service;
	if (identity)
		*identity = binding->identity;
	if (authz)
		*authz = binding->authz_service;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcBindingInqAuthInfoExA(RPC_BINDING_HANDLE handle, RPC_CSTR *principal, ULONG *level, ULONG *service, RPC_AUTH_IDENTITY_HANDLE *identity, ULONG *authz, ULONG version, RPC_SECURITY_QOS *qos)
{
	RPC_WSTR wide = NULL;
	RPC_STATUS status = RpcBindingInqAuthInfoExW(handle, principal ? &wide : NULL, level, service, identity, authz, version, qos);
	if (status == RPC_S_OK && principal) {
		*principal = (RPC_CSTR)rpc_narrow(wide);
		rpc_free(wide);
	}
	return status;
}

RPC_STATUS RPC_ENTRY RpcBindingInqAuthInfoW(RPC_BINDING_HANDLE handle, RPC_WSTR *principal, ULONG *level, ULONG *service, RPC_AUTH_IDENTITY_HANDLE *identity, ULONG *authz)
{
	return RpcBindingInqAuthInfoExW(handle, principal, level, service, identity, authz, 0, NULL);
}

RPC_STATUS RPC_ENTRY RpcBindingInqAuthInfoA(RPC_BINDING_HANDLE handle, RPC_CSTR *principal, ULONG *level, ULONG *service, RPC_AUTH_IDENTITY_HANDLE *identity, ULONG *authz)
{
	return RpcBindingInqAuthInfoExA(handle, principal, level, service, identity, authz, 0, NULL);
}

// ---- the client side of a call ----

static const RPC_SYNTAX_IDENTIFIER ndr_syntax = {{0x8a885d04, 0x1ceb, 0x11c9, {0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60}}, {2, 0}};

#pragma pack(push, 1)
typedef struct bind_body
{
	USHORT max_xmit, max_recv;
	ULONG assoc_group;
	BYTE contexts, pad[3];
	USHORT context_id;
	BYTE transfers, pad2;
	RPC_SYNTAX_IDENTIFIER abstract, transfer;
} bind_body;

typedef struct request_head
{
	ULONG alloc_hint;
	USHORT context, opnum;
} request_head;

typedef struct request_head_object
{
	request_head head;
	UUID object;
} request_head_object;

typedef struct response_head
{
	ULONG alloc_hint;
	USHORT context;
	BYTE cancels, pad;
} response_head;
#pragma pack(pop)

static RPC_STATUS negotiate(rpc_connection *connection, const RPC_SYNTAX_IDENTIFIER *abstract, ULONG call_id, USHORT *id)
{
	for (rpc_bound_context *context = connection->contexts; context; context = context->next) {
		if (!memcmp(&context->abstract, abstract, sizeof(*abstract))) {
			*id = context->id;
			return RPC_S_OK;
		}
	}
	bind_body body;
	memset(&body, 0, sizeof(body));
	body.max_xmit = body.max_recv = PDU_FRAGMENT_SIZE;
	body.contexts = 1;
	body.context_id = connection->next_context;
	body.transfers = 1;
	body.abstract = *abstract;
	body.transfer = ndr_syntax;
	BYTE type = connection->contexts ? PDU_ALTER_CONTEXT : PDU_BIND;
	if (!pdu_write(&connection->stream, type, PDU_FIRST | PDU_LAST, call_id, &body, sizeof(body), NULL, 0))
		return RPC_S_SERVER_UNAVAILABLE;
	rpc_pdu reply;
	if (!pdu_read(&connection->stream, &reply))
		return RPC_S_SERVER_UNAVAILABLE;
	RPC_STATUS status = RPC_S_OK;
	if (reply.type == PDU_BIND_NAK) {
		status = RPC_S_SERVER_UNAVAILABLE;
	} else if (reply.type != PDU_BIND_ACK && reply.type != PDU_ALTER_CONTEXT_RESP) {
		status = RPC_S_PROTOCOL_ERROR;
	} else {
		// The body is 8 bytes of limits, the secondary address with its padding, then the results.
		ULONG size = reply.frag_length - 16;
		if (size < 10) {
			status = RPC_S_PROTOCOL_ERROR;
		} else {
			USHORT address = *(USHORT *)(reply.body + 8);
			ULONG results = (10 + address + 3) & ~3u;
			if (results + 8 > size || !reply.body[results]) {
				status = RPC_S_PROTOCOL_ERROR;
			} else if (*(USHORT *)(reply.body + results + 4)) {
				status = RPC_S_UNKNOWN_IF;
			}
		}
	}
	rpc_free(reply.body);
	if (status != RPC_S_OK)
		return status;
	rpc_bound_context *context = HeapAlloc(GetProcessHeap(), 0, sizeof(*context));
	if (!context)
		return RPC_S_OUT_OF_MEMORY;
	context->abstract = *abstract;
	context->id = connection->next_context++;
	context->next = connection->contexts;
	connection->contexts = context;
	*id = context->id;
	return RPC_S_OK;
}

static rpc_connection *connection_take(rpc_binding *binding, RPC_STATUS *status)
{
	AcquireSRWLockExclusive(&binding->lock);
	rpc_connection *connection = binding->idle;
	if (connection)
		binding->idle = connection->next;
	ReleaseSRWLockExclusive(&binding->lock);
	*status = RPC_S_OK;
	if (connection)
		return connection;
	connection = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*connection));
	if (!connection) {
		*status = RPC_S_OUT_OF_MEMORY;
		return NULL;
	}
	*status = stream_connect(&connection->stream, binding->protseq, binding->network, binding->endpoint);
	if (*status != RPC_S_OK) {
		rpc_free(connection);
		return NULL;
	}
	return connection;
}

static void connection_give_back(rpc_binding *binding, rpc_connection *connection)
{
	AcquireSRWLockExclusive(&binding->lock);
	connection->next = binding->idle;
	binding->idle = connection;
	ReleaseSRWLockExclusive(&binding->lock);
}

RPC_STATUS RPC_ENTRY I_RpcNegotiateTransferSyntax(RPC_MESSAGE *message)
{
	message->TransferSyntax = (RPC_SYNTAX_IDENTIFIER *)&ndr_syntax;
	return RPC_S_OK;
}

// What a client call holds between getting its buffer and the reply.
typedef struct client_call
{
	rpc_connection *connection;
	USHORT context;
	ULONG call_id;
} client_call;

RPC_STATUS rpc_client_get_buffer(RPC_MESSAGE *message)
{
	rpc_binding *binding = rpc_binding_of(message->Handle);
	if (!binding)
		return RPC_S_INVALID_BINDING;
	client_call *call = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*call));
	if (!call)
		return RPC_S_OUT_OF_MEMORY;
	RPC_STATUS status;
	call->connection = connection_take(binding, &status);
	if (!call->connection) {
		rpc_free(call);
		return status;
	}
	call->call_id = (ULONG)InterlockedIncrement((LONG *)&binding->call_id);
	const RPC_CLIENT_INTERFACE *spec = message->RpcInterfaceInformation;
	if (!spec) {
		connection_give_back(binding, call->connection);
		rpc_free(call);
		return RPC_S_INTERFACE_NOT_FOUND;
	}
	status = negotiate(call->connection, &spec->InterfaceId, call->call_id, &call->context);
	if (status != RPC_S_OK) {
		connection_close(call->connection);
		rpc_free(call);
		return status;
	}
	message->TransferSyntax = (RPC_SYNTAX_IDENTIFIER *)&ndr_syntax;
	message->ReservedForRuntime = call;
	message->Buffer = HeapAlloc(GetProcessHeap(), 0, message->BufferLength ? message->BufferLength : 1);
	if (!message->Buffer) {
		connection_give_back(binding, call->connection);
		rpc_free(call);
		message->ReservedForRuntime = NULL;
		return RPC_S_OUT_OF_MEMORY;
	}
	return RPC_S_OK;
}

static RPC_STATUS send_request(rpc_binding *binding, client_call *call, RPC_MESSAGE *message)
{
	request_head_object head;
	head.head.alloc_hint = message->BufferLength;
	head.head.context = call->context;
	head.head.opnum = (USHORT)(message->ProcNum & 0x7fff);
	head.object = binding->object;
	ULONG head_size = binding->has_object ? sizeof(head) : sizeof(request_head);
	ULONG room = PDU_FRAGMENT_SIZE - 16 - head_size, sent = 0, left = message->BufferLength;
	BYTE flags = PDU_FIRST | (binding->has_object ? PDU_OBJECT_UUID : 0);
	do {
		ULONG piece = left > room ? room : left;
		BYTE fragment = flags;
		if (piece == left)
			fragment |= PDU_LAST;
		if (!pdu_write(&call->connection->stream, PDU_REQUEST, fragment, call->call_id, &head, head_size, (BYTE *)message->Buffer + sent, piece))
			return RPC_S_CALL_FAILED;
		sent += piece;
		left -= piece;
		head.head.alloc_hint = left;
		flags &= (BYTE)~PDU_FIRST;
	} while (left);
	return RPC_S_OK;
}

static RPC_STATUS receive_reply(client_call *call, RPC_MESSAGE *message)
{
	BYTE *data = NULL;
	ULONG used = 0, capacity = 0;
	for (;;) {
		rpc_pdu reply;
		if (!pdu_read(&call->connection->stream, &reply)) {
			rpc_free(data);
			return RPC_S_CALL_FAILED;
		}
		ULONG size = reply.frag_length - 16;
		if (reply.type == PDU_FAULT) {
			RPC_STATUS fault = size >= 12 ? *(ULONG *)(reply.body + 8) : RPC_S_CALL_FAILED;
			rpc_free(reply.body);
			rpc_free(data);
			return fault ? fault : RPC_S_CALL_FAILED;
		}
		if (reply.type != PDU_RESPONSE || size < sizeof(response_head) || reply.call_id != call->call_id) {
			rpc_free(reply.body);
			rpc_free(data);
			return RPC_S_PROTOCOL_ERROR;
		}
		ULONG piece = size - sizeof(response_head), hint = ((response_head *)reply.body)->alloc_hint;
		if (used + piece > capacity) {
			ULONG wanted = used + (hint > piece ? hint : piece);
			BYTE *grown = data ? HeapReAlloc(GetProcessHeap(), 0, data, wanted) : HeapAlloc(GetProcessHeap(), 0, wanted ? wanted : 1);
			if (!grown) {
				rpc_free(reply.body);
				rpc_free(data);
				return RPC_S_OUT_OF_MEMORY;
			}
			data = grown;
			capacity = wanted;
		}
		memcpy(data + used, reply.body + sizeof(response_head), piece);
		used += piece;
		BYTE flags = reply.flags;
		message->DataRepresentation = reply.drep[0] | reply.drep[1] << 8 | reply.drep[2] << 16 | (ULONG)reply.drep[3] << 24;
		rpc_free(reply.body);
		if (flags & PDU_LAST)
			break;
	}
	message->Buffer = data ? data : HeapAlloc(GetProcessHeap(), 0, 1);
	message->BufferLength = used;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY I_RpcSend(RPC_MESSAGE *message)
{
	rpc_binding *binding = rpc_binding_of(message->Handle);
	client_call *call = message->ReservedForRuntime;
	if (!binding || !call)
		return RPC_S_INVALID_BINDING;
	RPC_STATUS status = send_request(binding, call, message);
	if (status == RPC_S_OK)
		rpc_free(message->Buffer);
	else {
		connection_close(call->connection);
		call->connection = NULL;
	}
	if (status == RPC_S_OK)
		message->Buffer = NULL;
	return status;
}

RPC_STATUS RPC_ENTRY I_RpcReceive(RPC_MESSAGE *message)
{
	rpc_binding *binding = rpc_binding_of(message->Handle);
	client_call *call = message->ReservedForRuntime;
	if (!binding || !call || !call->connection)
		return RPC_S_INVALID_BINDING;
	RPC_STATUS status = receive_reply(call, message);
	// A fault from the far end leaves the connection in good order; anything else leaves it in an unknown state.
	if (status == RPC_S_CALL_FAILED || status == RPC_S_PROTOCOL_ERROR) {
		connection_close(call->connection);
		call->connection = NULL;
	}
	if (status != RPC_S_OK) {
		if (call->connection)
			connection_give_back(binding, call->connection);
		call->connection = NULL;
		message->ReservedForRuntime = call;
		message->Buffer = NULL;
	}
	return status;
}

RPC_STATUS RPC_ENTRY I_RpcSendReceive(RPC_MESSAGE *message)
{
	RPC_STATUS status = I_RpcSend(message);
	return status == RPC_S_OK ? I_RpcReceive(message) : status;
}

RPC_STATUS RPC_ENTRY I_RpcGetBuffer(RPC_MESSAGE *message)
{
	rpc_binding *binding = rpc_binding_of(message->Handle);
	if (!binding)
		return RPC_S_INVALID_BINDING;
	return binding->server ? rpc_server_get_buffer(message) : rpc_client_get_buffer(message);
}

RPC_STATUS RPC_ENTRY I_RpcFreeBuffer(RPC_MESSAGE *message)
{
	rpc_binding *binding = rpc_binding_of(message->Handle);
	rpc_free(message->Buffer);
	if (binding && !binding->server) {
		client_call *call = message->ReservedForRuntime;
		if (call) {
			if (call->connection)
				connection_give_back(binding, call->connection);
			rpc_free(call);
			message->ReservedForRuntime = NULL;
		}
	}
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY I_RpcBindingSetAsync(RPC_BINDING_HANDLE handle, void *routine, ULONG thread)
{
	(void)routine; (void)thread;
	return rpc_binding_of(handle) ? RPC_S_OK : RPC_S_INVALID_BINDING;
}

RPC_STATUS RPC_ENTRY I_RpcBindingInqTransportType(RPC_BINDING_HANDLE handle, unsigned int *type)
{
	rpc_binding *binding = rpc_binding_of(handle);
	if (!binding)
		return RPC_S_INVALID_BINDING;
	*type = !lstrcmpW(binding->protseq, L"ncalrpc") ? TRANSPORT_TYPE_LPC : TRANSPORT_TYPE_CN;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcIfInqId(RPC_IF_HANDLE handle, RPC_IF_ID *id)
{
	const RPC_CLIENT_INTERFACE *spec = handle;
	id->Uuid = spec->InterfaceId.SyntaxGUID;
	id->VersMajor = spec->InterfaceId.SyntaxVersion.MajorVersion;
	id->VersMinor = spec->InterfaceId.SyntaxVersion.MinorVersion;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcMgmtSetComTimeout(RPC_BINDING_HANDLE handle, unsigned int timeout)
{
	(void)timeout;
	return rpc_binding_of(handle) ? RPC_S_OK : RPC_S_INVALID_BINDING;
}

RPC_STATUS RPC_ENTRY RpcMgmtSetCancelTimeout(LONG timeout)
{
	(void)timeout;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcMgmtEnableIdleCleanup(void)
{
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcCancelThread(void *thread)
{
	(void)thread;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcCancelThreadEx(void *thread, LONG timeout)
{
	(void)thread; (void)timeout;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcNetworkIsProtseqValidW(RPC_WSTR protseq)
{
	if (protseq_supported(protseq))
		return RPC_S_OK;
	return wcsncmp(protseq, L"nca", 3) ? RPC_S_INVALID_RPC_PROTSEQ : RPC_S_PROTSEQ_NOT_SUPPORTED;
}

RPC_STATUS RPC_ENTRY RpcNetworkIsProtseqValidA(RPC_CSTR protseq)
{
	WITH_WIDE(wide, protseq);
	RPC_STATUS status = RpcNetworkIsProtseqValidW(wide);
	rpc_free(wide);
	return status;
}

static const WCHAR *const protseq_names[] = {L"ncalrpc", L"ncacn_np", L"ncacn_ip_tcp"};

RPC_STATUS RPC_ENTRY RpcNetworkInqProtseqsW(RPC_PROTSEQ_VECTORW **out)
{
	const ULONG count = ARRAYSIZE(protseq_names);
	RPC_PROTSEQ_VECTORW *vector = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, offsetof(RPC_PROTSEQ_VECTORW, Protseq) + count * sizeof(RPC_WSTR));
	if (!vector)
		return RPC_S_OUT_OF_MEMORY;
	vector->Count = count;
	for (ULONG i = 0; i < count; i++)
		vector->Protseq[i] = rpc_copy(protseq_names[i]);
	*out = vector;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcNetworkInqProtseqsA(RPC_PROTSEQ_VECTORA **out)
{
	const ULONG count = ARRAYSIZE(protseq_names);
	RPC_PROTSEQ_VECTORA *vector = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, offsetof(RPC_PROTSEQ_VECTORA, Protseq) + count * sizeof(RPC_CSTR));
	if (!vector)
		return RPC_S_OUT_OF_MEMORY;
	vector->Count = count;
	for (ULONG i = 0; i < count; i++)
		vector->Protseq[i] = (RPC_CSTR)rpc_narrow(protseq_names[i]);
	*out = vector;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcProtseqVectorFreeW(RPC_PROTSEQ_VECTORW **vector)
{
	if (!vector)
		return RPC_S_INVALID_ARG;
	if (*vector) {
		for (ULONG i = 0; i < (*vector)->Count; i++)
			rpc_free((*vector)->Protseq[i]);
		rpc_free(*vector);
		*vector = NULL;
	}
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcProtseqVectorFreeA(RPC_PROTSEQ_VECTORA **vector)
{
	if (!vector)
		return RPC_S_INVALID_ARG;
	if (*vector) {
		for (ULONG i = 0; i < (*vector)->Count; i++)
			rpc_free((*vector)->Protseq[i]);
		rpc_free(*vector);
		*vector = NULL;
	}
	return RPC_S_OK;
}
