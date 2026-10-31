/* SPDX-License-Identifier: MIT */
#include "rpc_own.h"

// ---- context handles: 20 bytes on the wire, a word of attributes and a uuid ----

#define CLIENT_CONTEXT_MAGIC 0x54584343

typedef struct client_context
{
	DWORD magic;
	RPC_BINDING_HANDLE binding;
	UUID uuid;
	ULONG attributes;
} client_context;

typedef struct wire_context
{
	ULONG attributes;
	UUID uuid;
} wire_context;

RPC_BINDING_HANDLE RPC_ENTRY NDRCContextBinding(NDR_CCONTEXT handle)
{
	client_context *context = handle;
	if (!context || context->magic != CLIENT_CONTEXT_MAGIC)
		RpcRaiseException(RPC_X_SS_CONTEXT_MISMATCH);
	return context->binding;
}

void RPC_ENTRY NDRCContextMarshall(NDR_CCONTEXT handle, void *buffer)
{
	wire_context *wire = buffer;
	client_context *context = handle;
	if (!context) {
		memset(wire, 0, sizeof(*wire));
		return;
	}
	if (context->magic != CLIENT_CONTEXT_MAGIC)
		RpcRaiseException(RPC_X_SS_CONTEXT_MISMATCH);
	wire->attributes = context->attributes;
	wire->uuid = context->uuid;
}

void RPC_ENTRY NDRCContextUnmarshall(NDR_CCONTEXT *handle, RPC_BINDING_HANDLE binding, void *buffer, ULONG representation)
{
	(void)representation;
	const wire_context *wire = buffer;
	RPC_STATUS ignored;
	client_context *context = *handle;
	if (UuidIsNil((UUID *)&wire->uuid, &ignored)) {
		if (context) {
			RpcBindingFree(&context->binding);
			context->magic = 0;
			rpc_free(context);
			*handle = NULL;
		}
		return;
	}
	if (context) {
		context->uuid = wire->uuid;
		context->attributes = wire->attributes;
		return;
	}
	context = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*context));
	if (!context)
		RpcRaiseException(RPC_S_OUT_OF_MEMORY);
	context->magic = CLIENT_CONTEXT_MAGIC;
	context->uuid = wire->uuid;
	context->attributes = wire->attributes;
	RPC_STATUS status = RpcBindingCopy(binding, &context->binding);
	if (status != RPC_S_OK)
		RpcRaiseException(status);
	*handle = context;
}

RPC_STATUS RPC_ENTRY RpcSmDestroyClientContext(void **handle)
{
	client_context *context = *handle;
	if (!context || context->magic != CLIENT_CONTEXT_MAGIC)
		return RPC_X_SS_CONTEXT_MISMATCH;
	RpcBindingFree(&context->binding);
	context->magic = 0;
	rpc_free(context);
	*handle = NULL;
	return RPC_S_OK;
}

void RPC_ENTRY RpcSsDestroyClientContext(void **handle)
{
	RPC_STATUS status = RpcSmDestroyClientContext(handle);
	if (status != RPC_S_OK)
		RpcRaiseException(status);
}

void RPC_ENTRY RpcSsDontSerializeContext(void)
{
}

// The public header opens the server's structure with two reserved words and the user's value; ours follows on.
typedef struct server_context
{
	struct _NDR_SCONTEXT public_part;
	UUID uuid;
	NDR_RUNDOWN rundown;
	struct server_context *next;
} server_context;

static SRWLOCK contexts_lock = SRWLOCK_INIT;
static server_context *contexts;

NDR_SCONTEXT RPC_ENTRY NDRSContextUnmarshall2(RPC_BINDING_HANDLE binding, void *buffer, ULONG representation, void *syntax, ULONG flags)
{
	(void)binding; (void)representation; (void)syntax; (void)flags;
	const wire_context *wire = buffer;
	RPC_STATUS ignored;
	if (wire && !UuidIsNil((UUID *)&wire->uuid, &ignored)) {
		AcquireSRWLockShared(&contexts_lock);
		server_context *found = contexts;
		while (found && memcmp(&found->uuid, &wire->uuid, sizeof(UUID)))
			found = found->next;
		ReleaseSRWLockShared(&contexts_lock);
		if (!found)
			RpcRaiseException(RPC_X_SS_CONTEXT_MISMATCH);
		return (NDR_SCONTEXT)found;
	}
	server_context *context = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*context));
	if (!context)
		RpcRaiseException(RPC_S_OUT_OF_MEMORY);
	return (NDR_SCONTEXT)context;
}

NDR_SCONTEXT RPC_ENTRY NDRSContextUnmarshallEx(RPC_BINDING_HANDLE binding, void *buffer, ULONG representation)
{
	return NDRSContextUnmarshall2(binding, buffer, representation, NULL, 0);
}

NDR_SCONTEXT RPC_ENTRY NDRSContextUnmarshall(void *buffer, ULONG representation)
{
	return NDRSContextUnmarshall2(I_RpcGetCurrentCallHandle(), buffer, representation, NULL, 0);
}

// A value left NULL ends the context; a new value gets its identity when first sent out.
void RPC_ENTRY NDRSContextMarshall2(RPC_BINDING_HANDLE binding, NDR_SCONTEXT handle, void *buffer, NDR_RUNDOWN rundown, void *syntax, ULONG flags)
{
	(void)binding; (void)syntax; (void)flags;
	server_context *context = (server_context *)handle;
	wire_context *wire = buffer;
	RPC_STATUS ignored;
	memset(wire, 0, sizeof(*wire));
	if (!context->public_part.userContext) {
		AcquireSRWLockExclusive(&contexts_lock);
		for (server_context **at = &contexts; *at; at = &(*at)->next) {
			if (*at == context) {
				*at = context->next;
				break;
			}
		}
		ReleaseSRWLockExclusive(&contexts_lock);
		rpc_free(context);
		return;
	}
	if (UuidIsNil(&context->uuid, &ignored)) {
		UuidCreate(&context->uuid);
		context->rundown = rundown;
		AcquireSRWLockExclusive(&contexts_lock);
		context->next = contexts;
		contexts = context;
		ReleaseSRWLockExclusive(&contexts_lock);
	}
	wire->uuid = context->uuid;
}

void RPC_ENTRY NDRSContextMarshallEx(RPC_BINDING_HANDLE binding, NDR_SCONTEXT handle, void *buffer, NDR_RUNDOWN rundown)
{
	NDRSContextMarshall2(binding, handle, buffer, rundown, NULL, 0);
}

void RPC_ENTRY NDRSContextMarshall(NDR_SCONTEXT handle, void *buffer, NDR_RUNDOWN rundown)
{
	NDRSContextMarshall2(I_RpcGetCurrentCallHandle(), handle, buffer, rundown, NULL, 0);
}

void RPC_ENTRY NdrClientContextMarshall(PMIDL_STUB_MESSAGE m, NDR_CCONTEXT context, int check)
{
	if (check && !context)
		RpcRaiseException(RPC_X_SS_IN_NULL_CONTEXT);
	m->Buffer = (BYTE *)(((ULONG_PTR)m->Buffer + 3) & ~(ULONG_PTR)3);
	NDRCContextMarshall(context, m->Buffer);
	m->Buffer += sizeof(wire_context);
}

void RPC_ENTRY NdrClientContextUnmarshall(PMIDL_STUB_MESSAGE m, NDR_CCONTEXT *context, RPC_BINDING_HANDLE binding)
{
	m->Buffer = (BYTE *)(((ULONG_PTR)m->Buffer + 3) & ~(ULONG_PTR)3);
	if (m->Buffer + sizeof(wire_context) > m->BufferEnd)
		RpcRaiseException(RPC_X_BAD_STUB_DATA);
	NDRCContextUnmarshall(context, binding, m->Buffer, m->RpcMsg->DataRepresentation);
	m->Buffer += sizeof(wire_context);
}

void RPC_ENTRY NdrServerContextMarshall(PMIDL_STUB_MESSAGE m, NDR_SCONTEXT context, NDR_RUNDOWN rundown)
{
	m->Buffer = (BYTE *)(((ULONG_PTR)m->Buffer + 3) & ~(ULONG_PTR)3);
	NDRSContextMarshall2(m->RpcMsg->Handle, context, m->Buffer, rundown, NULL, 0);
	m->Buffer += sizeof(wire_context);
}

NDR_SCONTEXT RPC_ENTRY NdrServerContextUnmarshall(PMIDL_STUB_MESSAGE m)
{
	m->Buffer = (BYTE *)(((ULONG_PTR)m->Buffer + 3) & ~(ULONG_PTR)3);
	if (m->Buffer + sizeof(wire_context) > m->BufferEnd)
		RpcRaiseException(RPC_X_BAD_STUB_DATA);
	NDR_SCONTEXT context = NDRSContextUnmarshall2(m->RpcMsg->Handle, m->Buffer, m->RpcMsg->DataRepresentation, NULL, 0);
	m->Buffer += sizeof(wire_context);
	return context;
}

void RPC_ENTRY NdrContextHandleSize(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format)
{
	(void)memory; (void)format;
	m->BufferLength = (m->BufferLength + 3) & ~3u;
	m->BufferLength += sizeof(wire_context);
}

NDR_SCONTEXT RPC_ENTRY NdrContextHandleInitialize(PMIDL_STUB_MESSAGE m, PFORMAT_STRING format)
{
	(void)format;
	return NDRSContextUnmarshall2(m->RpcMsg->Handle, NULL, m->RpcMsg->DataRepresentation, NULL, 0);
}

void RPC_ENTRY NdrServerContextNewMarshall(PMIDL_STUB_MESSAGE m, NDR_SCONTEXT context, NDR_RUNDOWN rundown, PFORMAT_STRING format)
{
	(void)format;
	NdrServerContextMarshall(m, context, rundown);
}

NDR_SCONTEXT RPC_ENTRY NdrServerContextNewUnmarshall(PMIDL_STUB_MESSAGE m, PFORMAT_STRING format)
{
	(void)format;
	return NdrServerContextUnmarshall(m);
}

// ---- asynchronous calls ----

RPC_STATUS rpc_async_complete(RPC_ASYNC_STATE *state, void *reply);
RPC_STATUS rpc_async_status(RPC_ASYNC_STATE *state);

RPC_STATUS RPC_ENTRY RpcAsyncInitializeHandle(PRPC_ASYNC_STATE state, unsigned int size)
{
	if (size != sizeof(*state))
		return ERROR_INVALID_PARAMETER;
	state->Size = size;
	state->Signature = 0x43595341;
	state->Lock = 0;
	state->Flags = 0;
	state->StubInfo = NULL;
	state->RuntimeInfo = NULL;
	memset(state->Reserved, 0, sizeof(state->Reserved));
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcAsyncGetCallStatus(PRPC_ASYNC_STATE state)
{
	return rpc_async_status(state);
}

RPC_STATUS RPC_ENTRY RpcAsyncCompleteCall(PRPC_ASYNC_STATE state, void *reply)
{
	return rpc_async_complete(state, reply);
}

RPC_STATUS RPC_ENTRY RpcAsyncAbortCall(PRPC_ASYNC_STATE state, ULONG code)
{
	(void)state; (void)code;
	return RPC_S_CALL_IN_PROGRESS;
}

RPC_STATUS RPC_ENTRY RpcAsyncCancelCall(PRPC_ASYNC_STATE state, BOOL abort)
{
	(void)state; (void)abort;
	return RPC_S_OK;
}

// ---- endpoint map, shared between the processes of the machine ----

#define ENDPOINT_SLOTS 128

typedef struct endpoint_slot
{
	BOOL used;
	RPC_SYNTAX_IDENTIFIER spec;
	UUID object;
	WCHAR protseq[32], endpoint[128], annotation[64];
} endpoint_slot;

static endpoint_slot *endpoint_map(HANDLE *lock)
{
	static endpoint_slot *view;
	static HANDLE mutex;
	if (!view) {
		mutex = CreateMutexW(NULL, FALSE, L"Local\\raven_rpc_endpoint_map_lock");
		HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, ENDPOINT_SLOTS * sizeof(endpoint_slot), L"Local\\raven_rpc_endpoint_map");
		view = mapping ? MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
	}
	*lock = mutex;
	return view;
}

static void slot_store(endpoint_slot *slot, const RPC_SERVER_INTERFACE *spec, const UUID *object, const rpc_binding *binding, const WCHAR *annotation)
{
	slot->used = TRUE;
	slot->spec = spec->InterfaceId;
	if (object)
		slot->object = *object;
	else
		memset(&slot->object, 0, sizeof(UUID));
	lstrcpynW(slot->protseq, binding->protseq, ARRAYSIZE(slot->protseq));
	lstrcpynW(slot->endpoint, binding->endpoint ? binding->endpoint : L"", ARRAYSIZE(slot->endpoint));
	lstrcpynW(slot->annotation, annotation ? annotation : L"", ARRAYSIZE(slot->annotation));
}

static BOOL slot_matches(const endpoint_slot *slot, const RPC_SERVER_INTERFACE *spec, const UUID *object, const WCHAR *protseq)
{
	if (!slot->used || memcmp(&slot->spec, &spec->InterfaceId, sizeof(slot->spec)))
		return FALSE;
	if (protseq && lstrcmpW(slot->protseq, protseq))
		return FALSE;
	static const UUID nil;
	return !memcmp(&slot->object, object ? object : &nil, sizeof(UUID));
}

static RPC_STATUS register_endpoints(RPC_IF_HANDLE handle, RPC_BINDING_VECTOR *bindings, UUID_VECTOR *objects, const WCHAR *annotation)
{
	if (!bindings)
		return RPC_S_NO_BINDINGS;
	HANDLE lock;
	endpoint_slot *map = endpoint_map(&lock);
	if (!map)
		return EPT_S_CANT_PERFORM_OP;
	const RPC_SERVER_INTERFACE *spec = handle;
	ULONG object_count = objects && objects->Count ? objects->Count : 1;
	WaitForSingleObject(lock, INFINITE);
	RPC_STATUS status = RPC_S_OK;
	for (ULONG b = 0; b < bindings->Count && status == RPC_S_OK; b++) {
		const rpc_binding *binding = rpc_binding_of(bindings->BindingH[b]);
		if (!binding)
			continue;
		for (ULONG o = 0; o < object_count && status == RPC_S_OK; o++) {
			const UUID *object = objects && objects->Count ? objects->Uuid[o] : NULL;
			INT empty = -1, same = -1;
			for (INT s = 0; s < ENDPOINT_SLOTS; s++) {
				if (!map[s].used && empty < 0)
					empty = s;
				if (slot_matches(&map[s], spec, object, binding->protseq))
					same = s;
			}
			INT use = same >= 0 ? same : empty;
			if (use < 0)
				status = EPT_S_CANT_CREATE;
			else
				slot_store(&map[use], spec, object, binding, annotation);
		}
	}
	ReleaseMutex(lock);
	return status;
}

RPC_STATUS RPC_ENTRY RpcEpRegisterW(RPC_IF_HANDLE handle, RPC_BINDING_VECTOR *bindings, UUID_VECTOR *objects, RPC_WSTR annotation)
{
	return register_endpoints(handle, bindings, objects, annotation);
}

RPC_STATUS RPC_ENTRY RpcEpRegisterA(RPC_IF_HANDLE handle, RPC_BINDING_VECTOR *bindings, UUID_VECTOR *objects, RPC_CSTR annotation)
{
	WCHAR *wide = rpc_widen((const char *)annotation);
	RPC_STATUS status = register_endpoints(handle, bindings, objects, wide);
	rpc_free(wide);
	return status;
}

RPC_STATUS RPC_ENTRY RpcEpRegisterNoReplaceW(RPC_IF_HANDLE handle, RPC_BINDING_VECTOR *bindings, UUID_VECTOR *objects, RPC_WSTR annotation)
{
	return register_endpoints(handle, bindings, objects, annotation);
}

RPC_STATUS RPC_ENTRY RpcEpRegisterNoReplaceA(RPC_IF_HANDLE handle, RPC_BINDING_VECTOR *bindings, UUID_VECTOR *objects, RPC_CSTR annotation)
{
	return RpcEpRegisterA(handle, bindings, objects, annotation);
}

RPC_STATUS RPC_ENTRY RpcEpUnregister(RPC_IF_HANDLE handle, RPC_BINDING_VECTOR *bindings, UUID_VECTOR *objects)
{
	if (!bindings)
		return RPC_S_NO_BINDINGS;
	HANDLE lock;
	endpoint_slot *map = endpoint_map(&lock);
	if (!map)
		return EPT_S_CANT_PERFORM_OP;
	const RPC_SERVER_INTERFACE *spec = handle;
	ULONG object_count = objects && objects->Count ? objects->Count : 1;
	WaitForSingleObject(lock, INFINITE);
	for (ULONG b = 0; b < bindings->Count; b++) {
		const rpc_binding *binding = rpc_binding_of(bindings->BindingH[b]);
		if (!binding)
			continue;
		for (ULONG o = 0; o < object_count; o++)
			for (INT s = 0; s < ENDPOINT_SLOTS; s++)
				if (slot_matches(&map[s], spec, objects && objects->Count ? objects->Uuid[o] : NULL, binding->protseq))
					map[s].used = FALSE;
	}
	ReleaseMutex(lock);
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcEpResolveBinding(RPC_BINDING_HANDLE handle, RPC_IF_HANDLE interface_handle)
{
	rpc_binding *binding = rpc_binding_of(handle);
	if (!binding)
		return RPC_S_INVALID_BINDING;
	if (binding->endpoint && *binding->endpoint)
		return RPC_S_OK;
	HANDLE lock;
	endpoint_slot *map = endpoint_map(&lock);
	if (!map)
		return EPT_S_NOT_REGISTERED;
	const RPC_SERVER_INTERFACE *spec = interface_handle;
	RPC_STATUS status = EPT_S_NOT_REGISTERED;
	WaitForSingleObject(lock, INFINITE);
	for (INT s = 0; s < ENDPOINT_SLOTS; s++) {
		if (slot_matches(&map[s], spec, binding->has_object ? &binding->object : NULL, binding->protseq)) {
			rpc_free(binding->endpoint);
			binding->endpoint = rpc_copy(map[s].endpoint);
			status = RPC_S_OK;
			break;
		}
	}
	ReleaseMutex(lock);
	return status;
}

RPC_STATUS RPC_ENTRY RpcMgmtEpEltInqBegin(RPC_BINDING_HANDLE handle, ULONG type, RPC_IF_ID *id, ULONG version, UUID *object, RPC_EP_INQ_HANDLE *out)
{
	(void)handle; (void)type; (void)id; (void)version; (void)object; (void)out;
	return RPC_S_CANNOT_SUPPORT;
}

// ---- towers, the way the endpoint mapper protocol of C706 and MS-RPCE spells an address ----

#define PROTOCOL_UUID 0x0d
#define PROTOCOL_NCACN 0x0b
#define PROTOCOL_LRPC 0x0c
#define PROTOCOL_TCP 0x07
#define PROTOCOL_IP 0x09
#define PROTOCOL_SMB 0x0f
#define PROTOCOL_NETBIOS 0x11
#define PROTOCOL_PIPE 0x10

typedef struct twr_t
{
	ULONG tower_length;
	BYTE tower_octet_string[1];
} twr_t;

#pragma pack(push, 1)
typedef struct syntax_floor
{
	USHORT lhs_size;
	BYTE protocol;
	UUID uuid;
	USHORT major;
	USHORT rhs_size;
	USHORT minor;
} syntax_floor;
#pragma pack(pop)

static BYTE *put_floor(BYTE *at, const void *lhs, USHORT lhs_size, const void *rhs, USHORT rhs_size)
{
	*(USHORT *)at = lhs_size;
	memcpy(at + 2, lhs, lhs_size);
	at += 2 + lhs_size;
	*(USHORT *)at = rhs_size;
	memcpy(at + 2, rhs, rhs_size);
	return at + 2 + rhs_size;
}

// Builds floors into out, and returns how many bytes they take; out may be NULL.
static ULONG address_floors(const char *protseq, const char *endpoint, const char *address, BYTE *out, BYTE *floors, RPC_STATUS *status)
{
	BYTE scratch[1024];
	BYTE *at = out ? out : scratch;
	BYTE *start = at;
	*status = RPC_S_OK;
	if (!lstrcmpA(protseq, "ncalrpc")) {
		const BYTE protocol = PROTOCOL_LRPC;
		const char *ep = endpoint ? endpoint : "";
		at = put_floor(at, &protocol, 1, ep, (USHORT)(strlen(ep) + 1));
		*floors = 1;
	} else if (!lstrcmpA(protseq, "ncacn_np")) {
		const BYTE smb = PROTOCOL_SMB, netbios = PROTOCOL_NETBIOS;
		const char *ep = endpoint ? endpoint : "", *host = address ? address : "";
		at = put_floor(at, &smb, 1, ep, (USHORT)(strlen(ep) + 1));
		at = put_floor(at, &netbios, 1, host, (USHORT)(strlen(host) + 1));
		*floors = 2;
	} else if (!lstrcmpA(protseq, "ncacn_ip_tcp")) {
		const BYTE tcp = PROTOCOL_TCP, ip = PROTOCOL_IP;
		USHORT port = 0;
		for (const char *c = endpoint; c && *c; c++)
			port = (USHORT)(port * 10 + (*c - '0'));
		port = (USHORT)(port >> 8 | port << 8);
		ULONG host = 0;
		INT shift = 0;
		ULONG part = 0;
		BOOL dotted = address && *address;
		for (const char *c = address; c && *c; c++) {
			if (*c == '.') {
				host |= part << shift;
				shift += 8;
				part = 0;
			} else if (*c >= '0' && *c <= '9') {
				part = part * 10 + (*c - '0');
			} else {
				dotted = FALSE;
				break;
			}
		}
		if (dotted)
			host |= part << shift;
		else
			host = 0;
		at = put_floor(at, &tcp, 1, &port, 2);
		at = put_floor(at, &ip, 1, &host, 4);
		*floors = 2;
	} else {
		*status = EPT_S_NOT_REGISTERED;
		return 0;
	}
	return (ULONG)(at - start);
}

RPC_STATUS WINAPI TowerConstruct(const RPC_SYNTAX_IDENTIFIER *object, const RPC_SYNTAX_IDENTIFIER *syntax, const char *protseq, const char *endpoint, const char *address, twr_t **out)
{
	RPC_STATUS status;
	BYTE floors = 0;
	*out = NULL;
	ULONG tail = address_floors(protseq, endpoint, address, NULL, &floors, &status);
	if (status != RPC_S_OK)
		return status;
	ULONG size = 2 + 2 * sizeof(syntax_floor) + (2 + 1 + 2 + 2) + tail;
	twr_t *tower = I_RpcAllocate(offsetof(twr_t, tower_octet_string) + size);
	if (!tower)
		return RPC_S_OUT_OF_RESOURCES;
	tower->tower_length = size;
	BYTE *at = tower->tower_octet_string;
	*(USHORT *)at = (USHORT)(floors + 3);
	at += 2;
	const RPC_SYNTAX_IDENTIFIER *ids[2] = {object, syntax};
	for (INT i = 0; i < 2; i++) {
		syntax_floor floor;
		floor.lhs_size = 1 + sizeof(UUID) + 2;
		floor.protocol = PROTOCOL_UUID;
		floor.uuid = ids[i]->SyntaxGUID;
		floor.major = ids[i]->SyntaxVersion.MajorVersion;
		floor.rhs_size = 2;
		floor.minor = ids[i]->SyntaxVersion.MinorVersion;
		memcpy(at, &floor, sizeof(floor));
		at += sizeof(floor);
	}
	const BYTE family = !lstrcmpA(protseq, "ncalrpc") ? PROTOCOL_LRPC : PROTOCOL_NCACN;
	const USHORT minor = 0;
	if (family == PROTOCOL_LRPC) {
		*(USHORT *)at = 1;
		at[2] = 0x0c;
		*(USHORT *)(at + 3) = 2;
		*(USHORT *)(at + 5) = minor;
		at += 7;
	} else {
		at = put_floor(at, &family, 1, &minor, 2);
	}
	address_floors(protseq, endpoint, address, at, &floors, &status);
	*out = tower;
	return RPC_S_OK;
}

RPC_STATUS WINAPI TowerExplode(const twr_t *tower, RPC_SYNTAX_IDENTIFIER *object, RPC_SYNTAX_IDENTIFIER *syntax, char **protseq, char **endpoint, char **address)
{
	const BYTE *at = tower->tower_octet_string, *end = at + tower->tower_length;
	if (protseq)
		*protseq = NULL;
	if (endpoint)
		*endpoint = NULL;
	if (address)
		*address = NULL;
	if (tower->tower_length < 2 + 2 * sizeof(syntax_floor))
		return EPT_S_NOT_REGISTERED;
	USHORT floors = *(const USHORT *)at;
	at += 2;
	if (floors < 4)
		return EPT_S_NOT_REGISTERED;
	for (INT i = 0; i < 2; i++) {
		syntax_floor floor;
		memcpy(&floor, at, sizeof(floor));
		if (floor.protocol != PROTOCOL_UUID || floor.lhs_size != 1 + sizeof(UUID) + 2 || floor.rhs_size != 2)
			return EPT_S_NOT_REGISTERED;
		RPC_SYNTAX_IDENTIFIER *target = i ? syntax : object;
		if (target) {
			target->SyntaxGUID = floor.uuid;
			target->SyntaxVersion.MajorVersion = floor.major;
			target->SyntaxVersion.MinorVersion = floor.minor;
		}
		at += sizeof(floor);
	}
	const BYTE *floor_values[3][2];
	INT count = 0;
	while (at + 2 <= end && count < 3 && count < floors - 2) {
		USHORT lhs = *(const USHORT *)at;
		if (at + 2 + lhs + 2 > end)
			return EPT_S_NOT_REGISTERED;
		USHORT rhs = *(const USHORT *)(at + 2 + lhs);
		if (at + 2 + lhs + 2 + rhs > end)
			return EPT_S_NOT_REGISTERED;
		floor_values[count][0] = at + 2;
		floor_values[count][1] = at + 2 + lhs + 2;
		at += 2 + lhs + 2 + rhs;
		count++;
	}
	if (count < 2)
		return EPT_S_NOT_REGISTERED;
	const char *name;
	const char *found_endpoint = "", *found_address = "";
	char number[16], dotted[20];
	if (floor_values[0][0][0] == PROTOCOL_LRPC) {
		name = "ncalrpc";
		found_endpoint = (const char *)floor_values[1][1];
	} else if (floor_values[1][0][0] == PROTOCOL_SMB && count > 2) {
		name = "ncacn_np";
		found_endpoint = (const char *)floor_values[1][1];
		found_address = (const char *)floor_values[2][1];
	} else if (floor_values[1][0][0] == PROTOCOL_TCP && count > 2) {
		name = "ncacn_ip_tcp";
		USHORT port = *(const USHORT *)floor_values[1][1];
		wsprintfA(number, "%u", (UINT)(USHORT)(port >> 8 | port << 8));
		found_endpoint = number;
		const BYTE *host = floor_values[2][1];
		wsprintfA(dotted, "%u.%u.%u.%u", host[0], host[1], host[2], host[3]);
		found_address = dotted;
	} else {
		return EPT_S_NOT_REGISTERED;
	}
	#define HAND_OUT(target, text) if (target) { *(target) = I_RpcAllocate((unsigned int)strlen(text) + 1); if (*(target)) memcpy(*(target), (text), strlen(text) + 1); }
	HAND_OUT(protseq, name);
	HAND_OUT(endpoint, found_endpoint);
	HAND_OUT(address, found_address);
	#undef HAND_OUT
	return RPC_S_OK;
}

// ---- serialization handles ----

typedef struct mes_state
{
	MIDL_ES_CODE operation;
	MIDL_ES_HANDLE_STYLE style;
	char *fixed;
	char **dynamic;
	ULONG size;
	ULONG *encoded;
	MIDL_STUB_MESSAGE stub;
	RPC_MESSAGE rpc;
	void *user;
	MIDL_ES_ALLOC alloc;
	MIDL_ES_WRITE write;
	MIDL_ES_READ read;
} mes_state;

static RPC_STATUS mes_new(handle_t *out, mes_state **state)
{
	*state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(**state));
	if (!*state)
		return RPC_S_OUT_OF_MEMORY;
	*out = *state;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY MesEncodeFixedBufferHandleCreate(char *buffer, ULONG size, ULONG *encoded, handle_t *out)
{
	if (!out)
		return RPC_S_INVALID_ARG;
	if (!buffer)
		return RPC_S_INVALID_ARG;
	if ((ULONG_PTR)buffer & 7)
		return RPC_X_INVALID_BUFFER;
	if (!encoded)
		return RPC_S_INVALID_ARG;
	if (!size)
		return RPC_S_INVALID_ARG;
	mes_state *state;
	RPC_STATUS status = mes_new(out, &state);
	if (status != RPC_S_OK)
		return status;
	state->operation = MES_ENCODE;
	state->style = MES_FIXED_BUFFER_HANDLE;
	state->fixed = buffer;
	state->size = size;
	state->encoded = encoded;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY MesEncodeDynBufferHandleCreate(char **buffer, ULONG *encoded, handle_t *out)
{
	if (!buffer || !encoded || !out)
		return RPC_S_INVALID_ARG;
	mes_state *state;
	RPC_STATUS status = mes_new(out, &state);
	if (status != RPC_S_OK)
		return status;
	state->operation = MES_ENCODE;
	state->style = MES_DYNAMIC_BUFFER_HANDLE;
	state->dynamic = buffer;
	state->encoded = encoded;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY MesDecodeBufferHandleCreate(char *buffer, ULONG size, handle_t *out)
{
	if (!buffer || !out)
		return RPC_S_INVALID_ARG;
	if ((ULONG_PTR)buffer & 7)
		return RPC_X_INVALID_BUFFER;
	mes_state *state;
	RPC_STATUS status = mes_new(out, &state);
	if (status != RPC_S_OK)
		return status;
	state->operation = MES_DECODE;
	state->style = MES_FIXED_BUFFER_HANDLE;
	state->fixed = buffer;
	state->size = size;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY MesEncodeIncrementalHandleCreate(void *user, MIDL_ES_ALLOC alloc, MIDL_ES_WRITE write, handle_t *out)
{
	if (!alloc || !write || !out)
		return RPC_S_INVALID_ARG;
	mes_state *state;
	RPC_STATUS status = mes_new(out, &state);
	if (status != RPC_S_OK)
		return status;
	state->operation = MES_ENCODE;
	state->style = MES_INCREMENTAL_HANDLE;
	state->user = user;
	state->alloc = alloc;
	state->write = write;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY MesDecodeIncrementalHandleCreate(void *user, MIDL_ES_READ read, handle_t *out)
{
	if (!read || !out)
		return RPC_S_INVALID_ARG;
	mes_state *state;
	RPC_STATUS status = mes_new(out, &state);
	if (status != RPC_S_OK)
		return status;
	state->operation = MES_DECODE;
	state->style = MES_INCREMENTAL_HANDLE;
	state->user = user;
	state->read = read;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY MesIncrementalHandleReset(handle_t handle, void *user, MIDL_ES_ALLOC alloc, MIDL_ES_WRITE write, MIDL_ES_READ read, MIDL_ES_CODE operation)
{
	mes_state *state = handle;
	if (!state)
		return RPC_S_INVALID_ARG;
	state->style = MES_INCREMENTAL_HANDLE;
	state->operation = operation;
	if (user)
		state->user = user;
	if (alloc)
		state->alloc = alloc;
	if (write)
		state->write = write;
	if (read)
		state->read = read;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY MesBufferHandleReset(handle_t handle, ULONG style, MIDL_ES_CODE operation, char **buffer, ULONG size, ULONG *encoded)
{
	mes_state *state = handle;
	if (!state || !buffer || (!*buffer && style != MES_DYNAMIC_BUFFER_HANDLE))
		return RPC_S_INVALID_ARG;
	if (style != MES_FIXED_BUFFER_HANDLE && style != MES_DYNAMIC_BUFFER_HANDLE)
		return RPC_S_INVALID_ARG;
	if (operation == MES_ENCODE && !encoded)
		return RPC_S_INVALID_ARG;
	state->style = style;
	state->operation = operation;
	if (style == MES_FIXED_BUFFER_HANDLE) {
		state->fixed = *buffer;
		state->dynamic = NULL;
	} else {
		state->dynamic = buffer;
		state->fixed = NULL;
	}
	state->size = size;
	state->encoded = encoded;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY MesHandleFree(handle_t handle)
{
	rpc_free(handle);
	return RPC_S_OK;
}

// Type serialization has a fixed 16 byte head: version, endianness, its length, filler, the object's size, filler.
#define MES_HEAD_SIZE 16

static void mes_stub_setup(mes_state *state, const MIDL_STUB_DESC *desc)
{
	NdrClientInitializeNew(&state->rpc, &state->stub, (PMIDL_STUB_DESC)desc, 0);
	state->stub.RpcMsg = &state->rpc;
	state->rpc.DataRepresentation = NDR_LOCAL_DATA_REPRESENTATION;
	state->stub.IsClient = TRUE;
}

void RPC_ENTRY NdrMesTypeEncode2(handle_t handle, const MIDL_TYPE_PICKLING_INFO *info, const MIDL_STUB_DESC *desc, PFORMAT_STRING format, const void *object)
{
	(void)info;
	mes_state *state = handle;
	mes_stub_setup(state, desc);
	state->stub.BufferLength = 0;
	void *slot = (void *)object;
	ndr_run(&state->stub, NDR_SIZE, (BYTE *)slot, format);
	ULONG data = state->stub.BufferLength, total = MES_HEAD_SIZE + ((data + 7) & ~7u);
	char *target;
	switch (state->style) {
	case MES_FIXED_BUFFER_HANDLE:
		if (total > state->size)
			RpcRaiseException(RPC_S_BUFFER_TOO_SMALL);
		target = state->fixed;
		break;
	case MES_DYNAMIC_BUFFER_HANDLE:
		target = desc->pfnAllocate(total);
		if (!target)
			RpcRaiseException(RPC_S_OUT_OF_MEMORY);
		*state->dynamic = target;
		break;
	case MES_INCREMENTAL_HANDLE: {
		ULONG got = total;
		target = NULL;
		state->alloc(state->user, &target, &got);
		if (!target || got < total)
			RpcRaiseException(RPC_S_BUFFER_TOO_SMALL);
		break;
	}
	default:
		RpcRaiseException(RPC_S_INVALID_ARG);
	}
	BYTE *head = (BYTE *)target;
	head[0] = 1;
	head[1] = 0x10;
	*(USHORT *)(head + 2) = 8;
	*(ULONG *)(head + 4) = 0xcccccccc;
	*(ULONG *)(head + 8) = (data + 7) & ~7u;
	*(ULONG *)(head + 12) = 0;
	state->stub.Buffer = head + MES_HEAD_SIZE;
	ndr_run(&state->stub, NDR_MARSHAL, (BYTE *)slot, format);
	ULONG produced = (ULONG)(state->stub.Buffer - head);
	if (state->encoded)
		*state->encoded = total > produced ? total : produced;
	if (state->style == MES_INCREMENTAL_HANDLE)
		state->write(state->user, target, total);
}

void RPC_ENTRY NdrMesTypeDecode2(handle_t handle, const MIDL_TYPE_PICKLING_INFO *info, const MIDL_STUB_DESC *desc, PFORMAT_STRING format, void *object)
{
	(void)info;
	mes_state *state = handle;
	mes_stub_setup(state, desc);
	char *source = state->fixed;
	ULONG size = state->size;
	if (state->style == MES_INCREMENTAL_HANDLE) {
		size = MES_HEAD_SIZE;
		state->read(state->user, &source, &size);
		if (!source)
			RpcRaiseException(RPC_X_INVALID_ES_ACTION);
	}
	if (!source || size < MES_HEAD_SIZE || ((BYTE *)source)[0] != 1)
		RpcRaiseException(RPC_X_INVALID_ES_ACTION);
	ULONG data = *(ULONG *)(source + 8);
	if (state->style == MES_INCREMENTAL_HANDLE) {
		size = data;
		state->read(state->user, &source, &size);
		if (!source || size < data)
			RpcRaiseException(RPC_X_INVALID_ES_ACTION);
		state->stub.Buffer = state->stub.BufferStart = (BYTE *)source;
	} else {
		if (data > size - MES_HEAD_SIZE)
			RpcRaiseException(RPC_X_INVALID_ES_ACTION);
		state->stub.Buffer = state->stub.BufferStart = (BYTE *)source + MES_HEAD_SIZE;
	}
	state->stub.BufferEnd = state->stub.Buffer + data;
	ndr_run(&state->stub, NDR_UNMARSHAL, object, format);
}

void RPC_ENTRY NdrMesTypeFree2(handle_t handle, const MIDL_TYPE_PICKLING_INFO *info, const MIDL_STUB_DESC *desc, PFORMAT_STRING format, void *object)
{
	(void)info;
	mes_state *state = handle;
	mes_stub_setup(state, desc);
	ndr_run(&state->stub, NDR_FREE, object, format);
}

void RPC_VAR_ENTRY NdrMesProcEncodeDecode(handle_t handle, const MIDL_STUB_DESC *desc, PFORMAT_STRING format, ...)
{
	(void)handle; (void)desc; (void)format;
	RpcRaiseException(RPC_S_CANNOT_SUPPORT);
}
