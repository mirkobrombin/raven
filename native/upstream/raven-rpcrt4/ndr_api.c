/* SPDX-License-Identifier: MIT */
#include "rpc_own.h"

// Where memory ends up when unmarshalling: what the caller passed, a new block, or the buffer itself for a server
// reading flat data that needs no copy.
static void unmarshal_top(MIDL_STUB_MESSAGE *m, BYTE **memory, const BYTE *format, UCHAR must_alloc)
{
	// A string of fixed capacity fits what the caller already has.
	if ((*format == FC_CSTRING || *format == FC_WSTRING) && *memory)
		must_alloc = 0;
	if (must_alloc || !*memory) {
		BYTE *in_place = !must_alloc && !m->IsClient ? ndr_flat_start(m, format) : NULL;
		if (in_place) {
			*memory = in_place;
		} else {
			SIZE_T size = ndr_memory_size(m, format);
			*memory = NdrAllocate(m, size);
			memset(*memory, 0, size);
		}
	}
	ndr_run(m, NDR_UNMARSHAL, *memory, format);
}

static ULONG size_of_memory(MIDL_STUB_MESSAGE *m, const BYTE *format)
{
	ndr_run(m, NDR_MEMSIZE, NULL, format);
	return m->MemorySize;
}

#define NDR_MARSHAL_FAMILY(name) \
	unsigned char *RPC_ENTRY name##Marshall(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format) \
	{ \
		ndr_run(m, NDR_MARSHAL, memory, format); \
		return NULL; \
	} \
	void RPC_ENTRY name##BufferSize(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format) \
	{ \
		ndr_run(m, NDR_SIZE, memory, format); \
	} \
	ULONG RPC_ENTRY name##MemorySize(PMIDL_STUB_MESSAGE m, PFORMAT_STRING format) \
	{ \
		return size_of_memory(m, format); \
	} \
	unsigned char *RPC_ENTRY name##Unmarshall(PMIDL_STUB_MESSAGE m, unsigned char **memory, PFORMAT_STRING format, unsigned char must_alloc) \
	{ \
		unmarshal_top(m, memory, format, must_alloc); \
		return NULL; \
	}

#define NDR_FAMILY(name) \
	NDR_MARSHAL_FAMILY(name) \
	void RPC_ENTRY name##Free(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format) \
	{ \
		ndr_run(m, NDR_FREE, memory, format); \
	}

NDR_FAMILY(NdrSimpleStruct)
NDR_FAMILY(NdrConformantStruct)
NDR_FAMILY(NdrConformantVaryingStruct)
NDR_FAMILY(NdrComplexStruct)
NDR_FAMILY(NdrFixedArray)
NDR_FAMILY(NdrVaryingArray)
NDR_FAMILY(NdrConformantArray)
NDR_FAMILY(NdrConformantVaryingArray)
NDR_FAMILY(NdrComplexArray)
NDR_FAMILY(NdrEncapsulatedUnion)
NDR_FAMILY(NdrNonEncapsulatedUnion)
NDR_FAMILY(NdrXmitOrRepAs)
NDR_FAMILY(NdrUserMarshal)
NDR_MARSHAL_FAMILY(NdrConformantString)
NDR_MARSHAL_FAMILY(NdrNonConformantString)

// A pointer's memory argument is what it points at, while the walk works on the slot holding it.
unsigned char *RPC_ENTRY NdrPointerMarshall(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format)
{
	void *slot = memory;
	if (format[0] == FC_RP && !memory)
		RpcRaiseException(RPC_X_NULL_REF_POINTER);
	ndr_run_pointer(m, NDR_MARSHAL, &slot, format);
	return NULL;
}

void RPC_ENTRY NdrPointerBufferSize(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format)
{
	void *slot = memory;
	ndr_run_pointer(m, NDR_SIZE, &slot, format);
}

ULONG RPC_ENTRY NdrPointerMemorySize(PMIDL_STUB_MESSAGE m, PFORMAT_STRING format)
{
	void *slot = NULL;
	ndr_run_pointer(m, NDR_MEMSIZE, &slot, format);
	return m->MemorySize;
}

unsigned char *RPC_ENTRY NdrPointerUnmarshall(PMIDL_STUB_MESSAGE m, unsigned char **memory, PFORMAT_STRING format, unsigned char must_alloc)
{
	// Whatever must_alloc says, a pointer with a target already in place keeps it.
	(void)must_alloc;
	ndr_run_pointer(m, NDR_UNMARSHAL, (void **)memory, format);
	return NULL;
}

void RPC_ENTRY NdrPointerFree(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format)
{
	void *slot = memory;
	ndr_run_pointer(m, NDR_FREE, &slot, format);
}

unsigned char *RPC_ENTRY NdrInterfacePointerMarshall(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format)
{
	ndr_run(m, NDR_MARSHAL, (BYTE *)&memory, format);
	return NULL;
}

void RPC_ENTRY NdrInterfacePointerBufferSize(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format)
{
	ndr_run(m, NDR_SIZE, (BYTE *)&memory, format);
}

ULONG RPC_ENTRY NdrInterfacePointerMemorySize(PMIDL_STUB_MESSAGE m, PFORMAT_STRING format)
{
	return size_of_memory(m, format);
}

unsigned char *RPC_ENTRY NdrInterfacePointerUnmarshall(PMIDL_STUB_MESSAGE m, unsigned char **memory, PFORMAT_STRING format, unsigned char must_alloc)
{
	(void)must_alloc;
	// On the client an [in, out] pointer being replaced must not leak; a server's slot means nothing.
	if (m->IsClient && *memory)
		IUnknown_Release((IUnknown *)*memory);
	*memory = NULL;
	ndr_run(m, NDR_UNMARSHAL, (BYTE *)memory, format);
	return NULL;
}

void RPC_ENTRY NdrInterfacePointerFree(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format)
{
	ndr_run(m, NDR_FREE, (BYTE *)&memory, format);
}

void RPC_ENTRY NdrSimpleTypeMarshall(PMIDL_STUB_MESSAGE m, unsigned char *memory, unsigned char type)
{
	const BYTE format[2] = {type, FC_PAD};
	ndr_run(m, NDR_MARSHAL, memory, format);
}

void RPC_ENTRY NdrSimpleTypeUnmarshall(PMIDL_STUB_MESSAGE m, unsigned char *memory, unsigned char type)
{
	const BYTE format[2] = {type, FC_PAD};
	ndr_run(m, NDR_UNMARSHAL, memory, format);
}

void RPC_ENTRY NdrByteCountPointerBufferSize(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format)
{
	(void)m; (void)memory; (void)format;
	RpcRaiseException(RPC_X_BAD_STUB_DATA);
}

unsigned char *RPC_ENTRY NdrByteCountPointerMarshall(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format)
{
	(void)m; (void)memory; (void)format;
	RpcRaiseException(RPC_X_BAD_STUB_DATA);
	return NULL;
}

unsigned char *RPC_ENTRY NdrByteCountPointerUnmarshall(PMIDL_STUB_MESSAGE m, unsigned char **memory, PFORMAT_STRING format, unsigned char must_alloc)
{
	(void)m; (void)memory; (void)format; (void)must_alloc;
	RpcRaiseException(RPC_X_BAD_STUB_DATA);
	return NULL;
}

void RPC_ENTRY NdrByteCountPointerFree(PMIDL_STUB_MESSAGE m, unsigned char *memory, PFORMAT_STRING format)
{
	(void)m; (void)memory; (void)format;
}

unsigned char *RPC_ENTRY NdrRangeUnmarshall(PMIDL_STUB_MESSAGE m, unsigned char **memory, PFORMAT_STRING format, unsigned char must_alloc)
{
	if (must_alloc || !*memory)
		*memory = NdrAllocate(m, sizeof(ULONGLONG));
	ndr_run(m, NDR_UNMARSHAL, *memory, format);
	return NULL;
}

void RPC_ENTRY NdrCorrelationInitialize(PMIDL_STUB_MESSAGE m, void *cache, ULONG size, ULONG flags)
{
	(void)cache; (void)size; (void)flags;
	m->fHasNewCorrDesc = TRUE;
	if (!m->CorrDespIncrement)
		m->CorrDespIncrement = 2;
}

void RPC_ENTRY NdrCorrelationPass(PMIDL_STUB_MESSAGE m)
{
	(void)m;
}

void RPC_ENTRY NdrCorrelationFree(PMIDL_STUB_MESSAGE m)
{
	(void)m;
}

void RPC_ENTRY NdrConvert(PMIDL_STUB_MESSAGE m, PFORMAT_STRING format)
{
	(void)m; (void)format;
}

void RPC_ENTRY NdrConvert2(PMIDL_STUB_MESSAGE m, PFORMAT_STRING format, LONG count)
{
	(void)m; (void)format; (void)count;
}

// ---- memory handed out by the stub allocator ----

typedef struct memory_link
{
	DWORD magic;
	DWORD size;
	DWORD unknown;
	struct memory_link *next;
} memory_link;

static const DWORD memory_link_magic = 'M' << 24 | 'E' << 16 | 'M' << 8 | 'L';

// Every block carries a link behind it, so what a server stub never freed can go when the call ends.
void *WINAPI NdrAllocate(PMIDL_STUB_MESSAGE m, size_t size)
{
	SIZE_T aligned = (size + 7) & ~(SIZE_T)7;
	if (aligned < size || aligned + sizeof(memory_link) < aligned)
		RpcRaiseException(RPC_X_BAD_STUB_DATA);
	BYTE *block = m->pfnAllocate(aligned + sizeof(memory_link));
	if (!block)
		RpcRaiseException(ERROR_OUTOFMEMORY);
	memory_link *link = (memory_link *)(block + aligned);
	link->magic = memory_link_magic;
	link->size = (DWORD)aligned;
	link->unknown = 0;
	link->next = m->pMemoryList;
	m->pMemoryList = link;
	return block;
}

// Only a call run by the interpreter owns its list: elsewhere the caller frees blocks behind our back.
void ndr_release_block(MIDL_STUB_MESSAGE *m, void *block)
{
	if (m->Unused != NDR_TRACKS_MEMORY) {
		m->pfnFree(block);
		return;
	}
	for (memory_link **at = (memory_link **)&m->pMemoryList; *at; at = &(*at)->next) {
		if ((BYTE *)*at - (*at)->size == block) {
			*at = (*at)->next;
			break;
		}
	}
	m->pfnFree(block);
}

void ndr_release_all(MIDL_STUB_MESSAGE *m)
{
	memory_link *link = m->pMemoryList;
	m->pMemoryList = NULL;
	while (link) {
		memory_link *next = link->next;
		m->pfnFree((BYTE *)link - link->size);
		link = next;
	}
}

void RPC_ENTRY NdrClearOutParameters(PMIDL_STUB_MESSAGE m, PFORMAT_STRING format, void *memory)
{
	if (!m->IsClient || !memory)
		return;
	SIZE_T size = 0;
	switch (*format) {
	case FC_RP:
	case FC_UP:
	case FC_OP:
	case FC_FP: {
		// Only the target of an out pointer gets cleared; the pointer belongs to the caller.
		const BYTE *target = format[1] & FC_SIMPLE_POINTER ? format + 2 : format + 2 + *(const SHORT *)(format + 2);
		ndr_run(m, NDR_FREE, memory, target);
		if (*target == FC_RP || *target == FC_UP || *target == FC_OP || *target == FC_FP || *target == FC_IP)
			*(void **)memory = NULL;
		return;
	}
	default:
		ndr_run(m, NDR_FREE, memory, format);
		size = ndr_memory_size(m, format);
		memset(memory, 0, size);
		return;
	}
}

// ---- marshalling info for user routines ----

RPC_STATUS RPC_ENTRY NdrGetUserMarshalInfo(ULONG *flags, ULONG level, NDR_USER_MARSHAL_INFO *info)
{
	if (level != 1)
		return RPC_S_INVALID_ARG;
	USER_MARSHAL_CB *cb = CONTAINING_RECORD(flags, USER_MARSHAL_CB, Flags);
	if (cb->Signature != USER_MARSHAL_CB_SIGNATURE)
		return RPC_S_INVALID_ARG;
	MIDL_STUB_MESSAGE *m = cb->pStubMsg;
	memset(info, 0, sizeof(*info));
	info->InformationLevel = level;
	info->Level1.pfnAllocate = m->pfnAllocate;
	info->Level1.pfnFree = m->pfnFree;
	info->Level1.pRpcChannelBuffer = m->pRpcChannelBuffer;
	if (cb->CBType == USER_MARSHAL_CB_MARSHALL || cb->CBType == USER_MARSHAL_CB_UNMARSHALL) {
		RPC_MESSAGE *message = m->RpcMsg;
		info->Level1.Buffer = m->Buffer;
		BYTE *end = (BYTE *)message->Buffer + message->BufferLength;
		if (m->Buffer > end)
			return ERROR_INVALID_USER_BUFFER;
		info->Level1.BufferSize = (ULONG)(end - m->Buffer);
	}
	return RPC_S_OK;
}

// ---- status codes for the [comm_status] and [fault_status] attributes ----

RPC_STATUS RPC_ENTRY NdrMapCommAndFaultStatus(PMIDL_STUB_MESSAGE m, ULONG *comm, ULONG *fault, RPC_STATUS status)
{
	(void)m;
	*comm = *fault = 0;
	switch (status) {
	case ERROR_INVALID_HANDLE:
	case RPC_S_INVALID_BINDING:
	case RPC_S_UNKNOWN_IF:
	case RPC_S_SERVER_UNAVAILABLE:
	case RPC_S_SERVER_TOO_BUSY:
	case RPC_S_CALL_FAILED_DNE:
	case RPC_S_PROTOCOL_ERROR:
	case RPC_S_UNSUPPORTED_TRANS_SYN:
	case RPC_S_UNSUPPORTED_TYPE:
	case RPC_S_PROCNUM_OUT_OF_RANGE:
	case EPT_S_NOT_REGISTERED:
	case RPC_S_COMM_FAILURE:
		*comm = status;
		break;
	default:
		*fault = status;
		break;
	}
	return RPC_S_OK;
}

// ---- full pointers ----

// Identity of a pointer across one call. Ids come from a counter for pointers seen here, and from the wire otherwise.

typedef struct full_entry
{
	void *pointer;
	ULONG id;
	UCHAR state;
	struct full_entry *next_by_pointer;
} full_entry;

typedef struct full_table
{
	FULL_PTR_XLAT_TABLES tables;
	full_entry **by_pointer;
	ULONG buckets;
	full_entry **by_id;
	ULONG ids;
	ULONG next_id;
} full_table;

static ULONG pointer_bucket(const full_table *table, const void *pointer)
{
	ULONG_PTR value = (ULONG_PTR)pointer;
	return (ULONG)((value ^ value >> 16) % table->buckets);
}

static full_entry *entry_by_pointer(const full_table *table, const void *pointer)
{
	for (full_entry *entry = table->by_pointer[pointer_bucket(table, pointer)]; entry; entry = entry->next_by_pointer)
		if (entry->pointer == pointer)
			return entry;
	return NULL;
}

static full_entry *entry_for_id(full_table *table, ULONG id)
{
	if (id >= table->ids) {
		ULONG grown = table->ids * 2;
		while (grown <= id)
			grown *= 2;
		full_entry **list = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, table->by_id, grown * sizeof(*list));
		if (!list)
			RpcRaiseException(ERROR_OUTOFMEMORY);
		table->by_id = list;
		table->ids = grown;
	}
	if (!table->by_id[id]) {
		full_entry *entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry));
		if (!entry)
			RpcRaiseException(ERROR_OUTOFMEMORY);
		entry->id = id;
		table->by_id[id] = entry;
	}
	// Ids handed out from here on stay clear of the ones the other side has used.
	if (id >= table->next_id)
		table->next_id = id + 1;
	return table->by_id[id];
}

static void entry_attach(full_table *table, full_entry *entry, void *pointer)
{
	entry->pointer = pointer;
	ULONG bucket = pointer_bucket(table, pointer);
	entry->next_by_pointer = table->by_pointer[bucket];
	table->by_pointer[bucket] = entry;
}

PFULL_PTR_XLAT_TABLES RPC_ENTRY NdrFullPointerXlatInit(ULONG hint, XLAT_SIDE side)
{
	(void)side;
	full_table *table = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*table));
	if (!table)
		RpcRaiseException(ERROR_OUTOFMEMORY);
	table->buckets = hint < 61 ? 61 : hint | 1;
	table->ids = table->buckets;
	table->by_pointer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, table->buckets * sizeof(*table->by_pointer));
	table->by_id = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, table->ids * sizeof(*table->by_id));
	if (!table->by_pointer || !table->by_id)
		RpcRaiseException(ERROR_OUTOFMEMORY);
	table->next_id = 1;
	return &table->tables;
}

void RPC_ENTRY NdrFullPointerXlatFree(PFULL_PTR_XLAT_TABLES tables)
{
	full_table *table = CONTAINING_RECORD(tables, full_table, tables);
	for (ULONG i = 0; i < table->ids; i++)
		if (table->by_id[i])
			HeapFree(GetProcessHeap(), 0, table->by_id[i]);
	HeapFree(GetProcessHeap(), 0, table->by_id);
	HeapFree(GetProcessHeap(), 0, table->by_pointer);
	HeapFree(GetProcessHeap(), 0, table);
}

int RPC_ENTRY NdrFullPointerQueryPointer(PFULL_PTR_XLAT_TABLES tables, void *pointer, unsigned char query, ULONG *id)
{
	full_table *table = CONTAINING_RECORD(tables, full_table, tables);
	if (!pointer) {
		*id = 0;
		return 1;
	}
	full_entry *entry = entry_by_pointer(table, pointer);
	if (!entry) {
		entry = entry_for_id(table, table->next_id);
		entry_attach(table, entry, pointer);
	}
	*id = entry->id;
	BOOL seen = query && entry->state & query;
	entry->state |= query;
	return seen;
}

int RPC_ENTRY NdrFullPointerQueryRefId(PFULL_PTR_XLAT_TABLES tables, ULONG id, unsigned char query, void **pointer)
{
	full_table *table = CONTAINING_RECORD(tables, full_table, tables);
	if (!id)
		return 1;
	full_entry *entry = entry_for_id(table, id);
	*pointer = entry->pointer;
	BOOL seen = query && entry->state & query;
	entry->state |= query;
	return seen;
}

void RPC_ENTRY NdrFullPointerInsertRefId(PFULL_PTR_XLAT_TABLES tables, ULONG id, void *pointer)
{
	full_table *table = CONTAINING_RECORD(tables, full_table, tables);
	entry_attach(table, entry_for_id(table, id), pointer);
}

int RPC_ENTRY NdrFullPointerFree(PFULL_PTR_XLAT_TABLES tables, void *pointer)
{
	full_table *table = CONTAINING_RECORD(tables, full_table, tables);
	if (!pointer)
		return 0;
	full_entry *entry = entry_by_pointer(table, pointer);
	if (!entry) {
		ULONG id;
		NdrFullPointerQueryPointer(tables, pointer, 0, &id);
		entry = entry_by_pointer(table, pointer);
	}
	if (entry->state & 0x20)
		return 0;
	entry->state |= 0x20;
	return 1;
}
