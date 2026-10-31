/* SPDX-License-Identifier: MIT */
#include "rpc_own.h"

// One walk over a format string serves every operation: what differs is what a leaf does with its bytes.
// Pointers met inside a constructed type only leave their id in place; their targets queue up and follow the type.

// What has already been done to a full pointer, so that each thing happens once per call.
#define FULL_SEEN_MARSHAL 0x01
#define FULL_SEEN_SIZE 0x02
#define FULL_SEEN_UNMARSHAL 0x04
#define FULL_SEEN_MEMSIZE 0x08

typedef struct deferred
{
	void **slot;
	const BYTE *format;
	ULONG id;
	BYTE *memory;		// the structure a correlation in the target refers to
} deferred;

typedef struct walk_frame
{
	deferred *items;
	INT count, capacity;
	struct walk_frame *outer;
} walk_frame;

static void walk(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format);
static void pointer_target(MIDL_STUB_MESSAGE *m, ndr_op op, void **slot, const BYTE *format, ULONG id);
static void top_pointer(MIDL_STUB_MESSAGE *m, ndr_op op, void **slot, const BYTE *format);

static SHORT read_short(const BYTE *at)
{
	return (SHORT)(at[0] | at[1] << 8);
}

static const BYTE *relative(const BYTE *at)
{
	return at + read_short(at);
}

static ULONG read_long(const BYTE *at)
{
	return at[0] | at[1] << 8 | at[2] << 16 | (ULONG)at[3] << 24;
}

static void buffer_check(MIDL_STUB_MESSAGE *m, SIZE_T size)
{
	if (m->BufferEnd && (m->Buffer > m->BufferEnd || size > (SIZE_T)(m->BufferEnd - m->Buffer)))
		RpcRaiseException(RPC_X_BAD_STUB_DATA);
}

static void buffer_align(MIDL_STUB_MESSAGE *m, ndr_op op, ULONG alignment)
{
	if (!alignment)
		return;
	if (op == NDR_SIZE) {
		m->BufferLength = (m->BufferLength + alignment - 1) & ~(alignment - 1);
		return;
	}
	if (op == NDR_FREE)
		return;
	ULONG_PTR at = (ULONG_PTR)m->Buffer, aligned = (at + alignment - 1) & ~(ULONG_PTR)(alignment - 1);
	if (op == NDR_MARSHAL)
		memset(m->Buffer, 0, aligned - at);
	m->Buffer = (BYTE *)aligned;
}

// The bytes of a leaf, in the direction of the operation.
static void transfer(MIDL_STUB_MESSAGE *m, ndr_op op, void *memory, SIZE_T size)
{
	switch (op) {
	case NDR_SIZE:
		m->BufferLength += (ULONG)size;
		break;
	case NDR_MARSHAL:
		memcpy(m->Buffer, memory, size);
		m->Buffer += size;
		break;
	case NDR_UNMARSHAL:
		buffer_check(m, size);
		memcpy(memory, m->Buffer, size);
		m->Buffer += size;
		break;
	case NDR_MEMSIZE:
		m->MemorySize += (ULONG)size;
		/* fall through */
	case NDR_SKIP:
		buffer_check(m, size);
		m->Buffer += size;
		break;
	case NDR_FREE:
		break;
	}
}

static ULONG count_put(MIDL_STUB_MESSAGE *m, ndr_op op, ULONG value)
{
	ndr_op plain = op == NDR_MEMSIZE ? NDR_SKIP : op;
	buffer_align(m, plain, 4);
	if (plain == NDR_FREE)
		return value;
	transfer(m, plain, &value, 4);
	if (plain == NDR_SKIP) {
		memcpy(&value, m->Buffer - 4, 4);
		return value;
	}
	return value;
}

// ---- base types ----

typedef struct
{
	BYTE wire, memory;
} base_size;

static base_size base_sizes(BYTE type)
{
	base_size size = {0, 0};
	switch (type) {
	case FC_BYTE: case FC_CHAR: case FC_SMALL: case FC_USMALL:
		size.wire = size.memory = 1;
		break;
	case FC_WCHAR: case FC_SHORT: case FC_USHORT:
		size.wire = size.memory = 2;
		break;
	case FC_ENUM16:
		size.wire = 2;
		size.memory = 4;
		break;
	case FC_LONG: case FC_ULONG: case FC_FLOAT: case FC_ENUM32: case FC_ERROR_STATUS_T:
		size.wire = size.memory = 4;
		break;
	case FC_HYPER: case FC_DOUBLE:
		size.wire = size.memory = 8;
		break;
	case FC_INT3264: case FC_UINT3264: case FC_IGNORE:
		size.wire = 4;
		size.memory = sizeof(void *);
		break;
	default:
		RpcRaiseException(RPC_X_BAD_STUB_DATA);
	}
	return size;
}

static void base_type(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, BYTE type)
{
	base_size size = base_sizes(type);
	if (op == NDR_FREE)
		return;
	buffer_align(m, op, size.wire);
	if (size.wire == size.memory) {
		transfer(m, op, memory, size.wire);
		return;
	}
	if (op == NDR_SIZE) {
		m->BufferLength += size.wire;
		return;
	}
	if (op == NDR_MEMSIZE || op == NDR_SKIP) {
		buffer_check(m, size.wire);
		m->Buffer += size.wire;
		if (op == NDR_MEMSIZE)
			m->MemorySize += size.memory;
		return;
	}
	if (op == NDR_MARSHAL) {
		if (type == FC_ENUM16) {
			INT value = *(INT *)memory;
			if (value < 0 || value > 0x7fff)
				RpcRaiseException(RPC_X_ENUM_VALUE_OUT_OF_RANGE);
			*(USHORT *)m->Buffer = (USHORT)value;
		} else if (type == FC_IGNORE) {
			*(ULONG *)m->Buffer = 0;
		} else {
			*(ULONG *)m->Buffer = (ULONG)*(ULONG_PTR *)memory;
		}
		m->Buffer += size.wire;
		return;
	}
	buffer_check(m, size.wire);
	if (type == FC_ENUM16)
		*(INT *)memory = *(USHORT *)m->Buffer;
	else if (type == FC_INT3264)
		*(LONG_PTR *)memory = *(LONG *)m->Buffer;
	else if (type == FC_IGNORE)
		*(ULONG_PTR *)memory = 0;
	else
		*(ULONG_PTR *)memory = *(ULONG *)m->Buffer;
	m->Buffer += size.wire;
}

// ---- correlation ----

static SIZE_T correlation_size(const MIDL_STUB_MESSAGE *m)
{
	return (SIZE_T)4 + m->CorrDespIncrement;
}

// What a conformance, variance or switch descriptor says, found in the stack or in the type around it.
static ULONG_PTR correlate(MIDL_STUB_MESSAGE *m, BYTE *memory, const BYTE *desc, ULONG_PTR fallback)
{
	BYTE kind = desc[0], operation = desc[1];
	if (kind == 0xff && operation == 0xff)
		return fallback;
	BYTE *base;
	switch (kind & 0xf0) {
	case FC_CONSTANT_CONFORMANCE:
		return (ULONG_PTR)(desc[1] << 16 | (USHORT)read_short(desc + 2));
	case FC_TOP_LEVEL_CONFORMANCE:
		base = m->StackTop;
		break;
	case FC_TOP_LEVEL_MULTID_CONFORMANCE:
		return fallback;
	case FC_POINTER_CONFORMANCE:
		base = m->Memory;
		break;
	default:
		base = memory;
		break;
	}
	if (!base)
		return fallback;
	if (operation == FC_CALLBACK) {
		BYTE *saved = m->StackTop;
		ULONG_PTR saved_count = m->MaxCount;
		m->StackTop = base;
		m->StubDesc->apfnExprEval[(USHORT)read_short(desc + 2)](m);
		ULONG_PTR value = m->MaxCount;
		m->StackTop = saved;
		m->MaxCount = saved_count;
		return value;
	}
	BYTE *at = base + read_short(desc + 2);
	if (operation == FC_DEREFERENCE)
		at = *(BYTE **)at;
	LONG_PTR value;
	switch (kind & 0x0f) {
	case FC_LONG: value = *(LONG *)at; break;
	case FC_ULONG: value = *(ULONG *)at; break;
	case FC_SHORT: value = *(SHORT *)at; break;
	case FC_USHORT: value = *(USHORT *)at; break;
	case FC_SMALL: value = *(signed char *)at; break;
	case FC_USMALL: case FC_BYTE: case FC_CHAR: value = *at; break;
	case FC_HYPER: value = (LONG_PTR)*(LONGLONG *)at; break;
	default:
		return fallback;
	}
	switch (operation) {
	case FC_DIV_2: value /= 2; break;
	case FC_MULT_2: value *= 2; break;
	case FC_ADD_1: value += 1; break;
	case FC_SUB_1: value -= 1; break;
	}
	return (ULONG_PTR)value;
}

// Counts on the wire: known from memory when writing, read from the buffer otherwise.
static void conformance(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *desc)
{
	if (op == NDR_SIZE || op == NDR_MARSHAL || op == NDR_FREE)
		m->MaxCount = correlate(m, memory, desc, m->MaxCount);
	ULONG value = count_put(m, op, (ULONG)m->MaxCount);
	if (op != NDR_SIZE && op != NDR_MARSHAL && op != NDR_FREE)
		m->MaxCount = value;
}

static void variance(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *desc, ULONG capacity)
{
	if (op == NDR_SIZE || op == NDR_MARSHAL || op == NDR_FREE) {
		m->Offset = 0;
		m->ActualCount = (ULONG)correlate(m, memory, desc, capacity);
	}
	ULONG offset = count_put(m, op, m->Offset), actual = count_put(m, op, m->ActualCount);
	if (op != NDR_SIZE && op != NDR_MARSHAL && op != NDR_FREE) {
		m->Offset = offset;
		m->ActualCount = actual;
		if (op == NDR_UNMARSHAL && ((ULONGLONG)offset + actual > capacity))
			RpcRaiseException(RPC_S_INVALID_BOUND);
	}
}

// ---- pointers ----

static BOOL inside_buffer(const MIDL_STUB_MESSAGE *m, const void *pointer)
{
	if (m->BufferStart && (const BYTE *)pointer >= m->BufferStart && (const BYTE *)pointer < m->BufferEnd)
		return TRUE;
	// Once the reply has its buffer, in parameters can still point into the request.
	return !m->IsClient && rpc_in_request(pointer);
}

static const BYTE *pointee_format(const BYTE *format)
{
	return format[1] & FC_SIMPLE_POINTER ? format + 2 : relative(format + 2);
}

// How much memory the target about to be read needs, without moving through the buffer for good.
static SIZE_T target_size(MIDL_STUB_MESSAGE *m, const BYTE *format)
{
	BYTE *buffer = m->Buffer;
	ULONG memory = m->MemorySize;
	INT ignore = m->IgnoreEmbeddedPointers;
	ULONG_PTR count = m->MaxCount;
	ULONG offset = m->Offset, actual = m->ActualCount;
	m->MemorySize = 0;
	m->IgnoreEmbeddedPointers = 1;
	walk(m, NDR_MEMSIZE, NULL, format);
	SIZE_T size = m->MemorySize;
	m->Buffer = buffer;
	m->MemorySize = memory;
	m->IgnoreEmbeddedPointers = ignore;
	m->MaxCount = count;
	m->Offset = offset;
	m->ActualCount = actual;
	return size;
}

SIZE_T ndr_memory_size(MIDL_STUB_MESSAGE *m, const BYTE *format)
{
	return target_size(m, format);
}

static void pointer_target(MIDL_STUB_MESSAGE *m, ndr_op op, void **slot, const BYTE *format, ULONG id)
{
	BYTE type = format[0];
	const BYTE *target = pointee_format(format);
	// A pointer to a pointer that is not marked indirect: both stand for the same address.
	if (!(format[1] & FC_POINTER_DEREF) && (*target == FC_RP || *target == FC_UP || *target == FC_OP || *target == FC_FP)) {
		void *inner = *slot;
		top_pointer(m, op, &inner, target);
		*slot = inner;
		return;
	}
	if (op == NDR_UNMARSHAL) {
		if (type != FC_RP && !id) {
			*slot = NULL;
			return;
		}
		// What a server finds in a slot says nothing, unless it is the stack space of an indirect pointer.
		if (!m->IsClient && (format[1] & (FC_ALLOCED_ON_STACK | FC_POINTER_DEREF)) != (FC_ALLOCED_ON_STACK | FC_POINTER_DEREF))
			*slot = NULL;
		if (type == FC_FP) {
			void *known = NULL;
			if (NdrFullPointerQueryRefId(m->FullPtrXlatTables, id, FULL_SEEN_UNMARSHAL, &known) || known) {
				*slot = known;
				return;
			}
		}
		if (format[1] & FC_POINTER_DEREF) {
			if (!*slot) {
				*slot = NdrAllocate(m, sizeof(void *));
				*(void **)*slot = NULL;
			}
			top_pointer(m, op, (void **)*slot, target);
			return;
		}
		// Only a pointer on its own may stand for bytes in the buffer: an embedded one owns a block.
		if (!*slot && !m->IsClient && !m->PointerLength)
			*slot = ndr_flat_start(m, target);
		if (!*slot) {
			SIZE_T size = target_size(m, target);
			*slot = NdrAllocate(m, size);
			memset(*slot, 0, size);
		}
		if (type == FC_FP)
			NdrFullPointerInsertRefId(m->FullPtrXlatTables, id, *slot);
		walk(m, op, *slot, target);
		return;
	}
	if (op == NDR_MEMSIZE || op == NDR_SKIP) {
		if (type != FC_RP && !id)
			return;
		// A target starts where its alignment says, counted from what is already added up.
		if (format[1] & FC_POINTER_DEREF) {
			m->MemorySize = (m->MemorySize + sizeof(void *) - 1) & ~(ULONG)(sizeof(void *) - 1);
			m->MemorySize += sizeof(void *);
		}
		{
			ULONG alignment = *target == FC_HYPER || *target == FC_DOUBLE ? 8 : sizeof(void *);
			m->MemorySize = (m->MemorySize + alignment - 1) & ~(alignment - 1);
		}
		if (format[1] & FC_POINTER_DEREF) {
			static void *nothing;
			top_pointer(m, NDR_MEMSIZE, &nothing, target);
			return;
		}
		if (type == FC_FP) {
			void *known = NULL;
			if (NdrFullPointerQueryRefId(m->FullPtrXlatTables, id, FULL_SEEN_MEMSIZE, &known) || known)
				return;
		}
		walk(m, NDR_MEMSIZE, NULL, target);
		return;
	}
	void *pointer = *slot;
	if (op == NDR_FREE) {
		if (!pointer)
			return;
		if (type == FC_FP && !NdrFullPointerFree(m->FullPtrXlatTables, pointer))
			return;
		if (format[1] & FC_POINTER_DEREF) {
			top_pointer(m, op, (void **)pointer, target);
			if (!inside_buffer(m, pointer) && !(format[1] & (FC_DONT_FREE | FC_ALLOCED_ON_STACK)))
				ndr_release_block(m, pointer);
			return;
		}
		walk(m, NDR_FREE, pointer, target);
		if (inside_buffer(m, pointer) || format[1] & (FC_DONT_FREE | FC_ALLOCED_ON_STACK))
			return;
		ndr_release_block(m, pointer);
		return;
	}
	if (!pointer) {
		if (type == FC_RP)
			RpcRaiseException(RPC_X_NULL_REF_POINTER);
		return;
	}
	if (format[1] & FC_POINTER_DEREF) {
		top_pointer(m, op, (void **)pointer, target);
		return;
	}
	if (type == FC_FP) {
		ULONG full_id;
		if (NdrFullPointerQueryPointer(m->FullPtrXlatTables, pointer, op == NDR_SIZE ? FULL_SEEN_SIZE : FULL_SEEN_MARSHAL, &full_id))
			return;
	}
	walk(m, op, pointer, target);
}

// What goes on the wire for the pointer itself.
static ULONG pointer_id(MIDL_STUB_MESSAGE *m, ndr_op op, const void *pointer, BYTE type)
{
	if (type == FC_FP && op == NDR_MARSHAL) {
		ULONG id = 0;
		NdrFullPointerQueryPointer(m->FullPtrXlatTables, (void *)pointer, 0, &id);
		return id;
	}
	// Referent ids only have to differ; these are the values the protocol documents use.
	return pointer ? 0x20000 + 4 * m->UniquePtrCount++ : 0;
}

static void frame_push(MIDL_STUB_MESSAGE *m, ndr_op op, void **slot, const BYTE *format, ULONG id)
{
	walk_frame *frame = (walk_frame *)m->pPointerQueueState;
	if (!frame) {
		pointer_target(m, op, slot, format, id);
		return;
	}
	if (frame->count == frame->capacity) {
		INT capacity = frame->capacity ? frame->capacity * 2 : 16;
		deferred *grown = frame->items ? HeapReAlloc(GetProcessHeap(), 0, frame->items, capacity * sizeof(*grown)) : HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(*grown));
		if (!grown)
			RpcRaiseException(RPC_S_OUT_OF_MEMORY);
		frame->items = grown;
		frame->capacity = capacity;
	}
	deferred *item = &frame->items[frame->count++];
	item->slot = slot;
	item->format = format;
	item->id = id;
	item->memory = m->Memory;
}

// Each target can queue targets of its own, which follow it before the next one of this frame.
static void frame_drain(MIDL_STUB_MESSAGE *m, ndr_op op, walk_frame *frame)
{
	for (INT i = 0; i < frame->count; i++) {
		deferred item = frame->items[i];
		walk_frame inner = {NULL, 0, 0, frame};
		BYTE *memory = m->Memory;
		ULONG embedded = m->PointerLength;
		m->pPointerQueueState = (void *)&inner;
		m->Memory = item.memory;
		m->PointerLength = 1;
		pointer_target(m, op, item.slot, item.format, item.id);
		frame_drain(m, op, &inner);
		m->PointerLength = embedded;
		m->Memory = memory;
		m->pPointerQueueState = (void *)frame;
	}
	if (frame->items)
		HeapFree(GetProcessHeap(), 0, frame->items);
	frame->items = NULL;
}

static void frame_run(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format)
{
	walk_frame frame = {NULL, 0, 0, (walk_frame *)m->pPointerQueueState};
	m->pPointerQueueState = (void *)&frame;
	walk(m, op, memory, format);
	frame_drain(m, op, &frame);
	m->pPointerQueueState = (void *)frame.outer;
}

// A pointer inside something bigger: the id here, the target later.
static void embedded_pointer(MIDL_STUB_MESSAGE *m, ndr_op op, void **slot, const BYTE *format)
{
	if (op == NDR_FREE) {
		if (!m->IgnoreEmbeddedPointers)
			pointer_target(m, op, slot, format, 0);
		return;
	}
	ndr_op plain = op == NDR_MEMSIZE ? NDR_SKIP : op;
	if (op == NDR_UNMARSHAL && !m->IsClient)
		*slot = NULL;
	ULONG id = op == NDR_MARSHAL ? pointer_id(m, op, *slot, format[0]) : 0;
	buffer_align(m, plain, 4);
	transfer(m, plain, &id, 4);
	if (plain == NDR_SKIP)
		memcpy(&id, m->Buffer - 4, 4);
	if (m->IgnoreEmbeddedPointers)
		return;
	if (op == NDR_SIZE || op == NDR_MARSHAL) {
		if (format[0] == FC_RP && !*slot)
			RpcRaiseException(RPC_X_NULL_REF_POINTER);
	}
	if (op == NDR_UNMARSHAL && format[0] != FC_RP && !id) {
		*slot = NULL;
		return;
	}
	frame_push(m, op, slot, format, id);
}

// A pointer on its own: id and target together; a reference pointer has no id at all.
static void top_pointer(MIDL_STUB_MESSAGE *m, ndr_op op, void **slot, const BYTE *format)
{
	ULONG id = 0;
	if (format[0] != FC_RP && op != NDR_FREE) {
		ndr_op plain = op == NDR_MEMSIZE ? NDR_SKIP : op;
		if (op == NDR_MARSHAL)
			id = pointer_id(m, op, *slot, format[0]);
		buffer_align(m, plain, 4);
		transfer(m, plain, &id, 4);
		if (plain == NDR_SKIP)
			memcpy(&id, m->Buffer - 4, 4);
	}
	pointer_target(m, op, slot, format, id);
}

// A layout says where the pointers of a flat block are, in memory and in its image on the wire.
static void layout_pointer(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, BYTE *image, const BYTE *saved, const BYTE *item)
{
	void **slot = memory ? (void **)(memory + read_short(item)) : NULL;
	BYTE *wire = image ? image + read_short(item + 2) : NULL;
	const BYTE *format = item + 4;
	ULONG id = 0;
	if (op == NDR_MARSHAL) {
		id = pointer_id(m, op, *slot, format[0]);
		memcpy(wire, &id, 4);
	} else if (op == NDR_UNMARSHAL || op == NDR_SKIP || op == NDR_MEMSIZE) {
		memcpy(&id, wire, 4);
	}
	if (op == NDR_UNMARSHAL && format[0] != FC_RP && !id) {
		*slot = NULL;
		return;
	}
	// The block was just overwritten with the wire's image: a client's own pointer is where the reply goes.
	if (op == NDR_UNMARSHAL)
		*slot = saved ? *(void **)(saved + read_short(item)) : NULL;
	if (op == NDR_SIZE || op == NDR_MARSHAL || op == NDR_FREE) {
		if (!*slot && format[0] == FC_RP && op != NDR_FREE)
			RpcRaiseException(RPC_X_NULL_REF_POINTER);
	}
	static void *memsize_slot;
	frame_push(m, op, slot ? slot : &memsize_slot, format, id);
}

static void pointer_layout(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, BYTE *image, const BYTE *saved, const BYTE *layout)
{
	if (m->IgnoreEmbeddedPointers || *layout != FC_PP)
		return;
	const BYTE *at = layout + 2;
	while (*at != FC_END) {
		BYTE kind = at[0];
		if (kind == FC_NO_REPEAT) {
			layout_pointer(m, op, memory, image, saved, at + 2);
			at += 10;
			continue;
		}
		ULONG times, increment, to_array, pointers;
		const BYTE *items;
		if (kind == FC_FIXED_REPEAT) {
			times = (USHORT)read_short(at + 2);
			increment = (USHORT)read_short(at + 4);
			to_array = (USHORT)read_short(at + 6);
			pointers = (USHORT)read_short(at + 8);
			items = at + 10;
		} else {
			times = at[1] == FC_VARIABLE_OFFSET ? m->ActualCount : (ULONG)m->MaxCount;
			increment = (USHORT)read_short(at + 2);
			to_array = (USHORT)read_short(at + 4);
			pointers = (USHORT)read_short(at + 6);
			items = at + 8;
		}
		for (ULONG i = 0; i < times; i++) {
			BYTE *repeat_memory = memory ? memory + to_array + i * increment : NULL;
			BYTE *repeat_image = image ? image + to_array + i * increment : NULL;
			const BYTE *repeat_saved = saved ? saved + to_array + i * increment : NULL;
			for (ULONG p = 0; p < pointers; p++)
				layout_pointer(m, op, repeat_memory, repeat_image, repeat_saved, items + p * 8);
		}
		at = items + pointers * 8;
	}
}

// ---- flat data ----

// A block of plain bytes, with pointers described by a layout if it has any.
// What a client's block holds before the reply lands on it.
static BYTE *block_saved(const MIDL_STUB_MESSAGE *m, ndr_op op, const BYTE *memory, SIZE_T size, const BYTE *layout)
{
	if (op != NDR_UNMARSHAL || !m->IsClient || !layout || !memory || !size)
		return NULL;
	BYTE *saved = HeapAlloc(GetProcessHeap(), 0, size);
	if (!saved)
		RpcRaiseException(RPC_S_OUT_OF_MEMORY);
	memcpy(saved, memory, size);
	return saved;
}

static void flat_block(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, SIZE_T size, const BYTE *layout)
{
	if (op == NDR_FREE) {
		if (layout)
			pointer_layout(m, op, memory, NULL, NULL, layout);
		return;
	}
	BYTE *image = m->Buffer;
	BYTE *saved = block_saved(m, op, memory, size, layout);
	transfer(m, op, memory, size);
	if (layout && op != NDR_SIZE)
		pointer_layout(m, op, memory, image, saved, layout);
	else if (layout)
		pointer_layout(m, op, memory, NULL, NULL, layout);
	if (saved)
		HeapFree(GetProcessHeap(), 0, saved);
}

static const BYTE *layout_after(const BYTE *at)
{
	return *at == FC_PP ? at : NULL;
}

// ---- strings ----

static void string_type(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format)
{
	BYTE type = format[0];
	ULONG unit = type == FC_C_WSTRING || type == FC_WSTRING ? 2 : 1;
	BOOL conformant = type == FC_C_CSTRING || type == FC_C_WSTRING;
	if (op == NDR_FREE)
		return;
	if (op == NDR_SIZE || op == NDR_MARSHAL) {
		ULONG length;
		if (unit == 2)
			length = lstrlenW((const WCHAR *)memory) + 1;
		else
			length = lstrlenA((const char *)memory) + 1;
		m->ActualCount = length;
		m->Offset = 0;
		if (conformant) {
			m->MaxCount = format[1] == FC_STRING_SIZED ? correlate(m, memory, format + 2, length) : length;
			if (format[1] == FC_STRING_SIZED && m->MaxCount < length)
				RpcRaiseException(RPC_S_INVALID_BOUND);
		} else {
			ULONG capacity = (USHORT)read_short(format + 2);
			if (length > capacity)
				RpcRaiseException(RPC_S_INVALID_BOUND);
		}
	}
	if (conformant) {
		ULONG value = count_put(m, op, (ULONG)m->MaxCount);
		if (op != NDR_SIZE && op != NDR_MARSHAL)
			m->MaxCount = value;
	}
	ULONG offset = count_put(m, op, m->Offset), actual = count_put(m, op, m->ActualCount);
	if (op != NDR_SIZE && op != NDR_MARSHAL) {
		m->Offset = offset;
		m->ActualCount = actual;
		ULONG capacity = conformant ? (ULONG)m->MaxCount : (ULONG)(USHORT)read_short(format + 2);
		if (offset || actual > capacity || !actual)
			RpcRaiseException(RPC_S_INVALID_BOUND);
		buffer_check(m, (SIZE_T)actual * unit);
		// A string on the wire must end where it says it does.
		const BYTE *last = m->Buffer + (SIZE_T)(actual - 1) * unit;
		for (ULONG i = 0; i < unit; i++)
			if (last[i])
				RpcRaiseException(RPC_S_INVALID_BOUND);
	}
	if (op == NDR_MEMSIZE) {
		m->MemorySize += (conformant ? (ULONG)m->MaxCount : (ULONG)(USHORT)read_short(format + 2)) * unit;
		buffer_check(m, (SIZE_T)m->ActualCount * unit);
		m->Buffer += (SIZE_T)m->ActualCount * unit;
		return;
	}
	transfer(m, op, memory, (SIZE_T)m->ActualCount * unit);
}

// ---- arrays ----

static BYTE memory_alignment(BYTE format_alignment)
{
	return (BYTE)(format_alignment + 1);
}

// Elements of an array or struct that are not flat, one by one.
static SIZE_T complex_member_size(MIDL_STUB_MESSAGE *m, const BYTE *format);

static const BYTE *skip_embedded(const BYTE *format, const BYTE **target)
{
	if (*format == FC_EMBEDDED_COMPLEX) {
		*target = relative(format + 2);
		return format + 4;
	}
	*target = format;
	return format + 1;
}

static void array_elements(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *element, ULONG count, ULONG offset)
{
	const BYTE *type;
	skip_embedded(element, &type);
	if (*element == FC_RP || *element == FC_UP || *element == FC_FP || *element == FC_OP) {
		for (ULONG i = 0; i < count; i++) {
			void **slot = memory ? (void **)memory + offset + i : NULL;
			static void *nothing;
			if (op == NDR_UNMARSHAL && slot)
				*slot = NULL;
			embedded_pointer(m, op, slot ? slot : &nothing, element);
		}
		return;
	}
	SIZE_T size = complex_member_size(m, type);
	for (ULONG i = 0; i < count; i++) {
		BYTE *at = memory ? memory + (offset + i) * size : NULL;
		ndr_op inner = op == NDR_MEMSIZE ? NDR_SKIP : op;
		walk(m, inner, at, type);
	}
}

static void array_type(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format)
{
	BYTE type = format[0], alignment = format[1] + 1;
	SIZE_T desc = correlation_size(m);
	switch (type) {
	case FC_SMFARRAY:
	case FC_LGFARRAY: {
		SIZE_T total = type == FC_SMFARRAY ? (USHORT)read_short(format + 2) : read_long(format + 2);
		const BYTE *after = format + (type == FC_SMFARRAY ? 4 : 6);
		buffer_align(m, op, alignment);
		flat_block(m, op, memory, total, layout_after(after));
		return;
	}
	case FC_SMVARRAY:
	case FC_LGVARRAY: {
		BOOL small = type == FC_SMVARRAY;
		ULONG total = small ? (USHORT)read_short(format + 2) : read_long(format + 2);
		ULONG elements = small ? (USHORT)read_short(format + 4) : read_long(format + 6);
		ULONG size = (USHORT)read_short(format + (small ? 6 : 10));
		const BYTE *var = format + (small ? 8 : 12);
		variance(m, op, memory, var, elements);
		buffer_align(m, op == NDR_MEMSIZE ? NDR_SKIP : op, alignment);
		const BYTE *layout = layout_after(var + desc);
		if (op == NDR_MEMSIZE)
			m->MemorySize += total;
		ndr_op wire = op == NDR_MEMSIZE ? NDR_SKIP : op;
		flat_block(m, wire, memory ? memory + (SIZE_T)m->Offset * size : NULL, (SIZE_T)m->ActualCount * size, layout);
		return;
	}
	case FC_CARRAY:
	case FC_CVARRAY: {
		ULONG size = (USHORT)read_short(format + 2);
		const BYTE *conf = format + 4;
		conformance(m, op, memory, conf);
		const BYTE *after = conf + desc;
		if (type == FC_CVARRAY) {
			variance(m, op, memory, after, (ULONG)m->MaxCount);
			after += desc;
		} else {
			m->Offset = 0;
			m->ActualCount = (ULONG)m->MaxCount;
		}
		buffer_align(m, op == NDR_MEMSIZE ? NDR_SKIP : op, alignment);
		if (op == NDR_MEMSIZE)
			m->MemorySize += (ULONG)m->MaxCount * size;
		ndr_op wire = op == NDR_MEMSIZE ? NDR_SKIP : op;
		flat_block(m, wire, memory ? memory + (SIZE_T)m->Offset * size : NULL, (SIZE_T)m->ActualCount * size, layout_after(after));
		return;
	}
	case FC_BOGUS_ARRAY: {
		ULONG dimension = (USHORT)read_short(format + 2);
		const BYTE *conf = format + 4, *var = conf + desc, *element = var + desc;
		BOOL has_conf = !(conf[0] == 0xff && conf[1] == 0xff), has_var = !(var[0] == 0xff && var[1] == 0xff);
		if (has_conf)
			conformance(m, op, memory, conf);
		else
			m->MaxCount = dimension;
		ULONG capacity = (ULONG)m->MaxCount;
		if (has_var) {
			variance(m, op, memory, var, capacity);
		} else {
			m->Offset = 0;
			m->ActualCount = capacity;
		}
		ULONG offset = m->Offset, actual = m->ActualCount;
		buffer_align(m, op == NDR_MEMSIZE ? NDR_SKIP : op, alignment);
		if (op == NDR_MEMSIZE) {
			const BYTE *target;
			skip_embedded(element, &target);
			SIZE_T size = *element == FC_RP || *element == FC_UP || *element == FC_FP || *element == FC_OP ? sizeof(void *) : complex_member_size(m, target);
			m->MemorySize += (ULONG)(capacity * size);
		}
		array_elements(m, op, memory, element, actual, offset);
		return;
	}
	}
}

// ---- structures ----

static SIZE_T complex_member_size(MIDL_STUB_MESSAGE *m, const BYTE *format)
{
	switch (*format) {
	case FC_STRUCT: case FC_PSTRUCT: case FC_CSTRUCT: case FC_CPSTRUCT: case FC_CVSTRUCT: case FC_BOGUS_STRUCT:
		return (USHORT)read_short(format + 2);
	case FC_SMFARRAY:
		return (USHORT)read_short(format + 2);
	case FC_LGFARRAY:
		return read_long(format + 2);
	case FC_SMVARRAY:
		return (USHORT)read_short(format + 2);
	case FC_LGVARRAY:
		return read_long(format + 2);
	case FC_TRANSMIT_AS: case FC_REPRESENT_AS:
		return (USHORT)read_short(format + 4);
	case FC_USER_MARSHAL:
		return (USHORT)read_short(format + 4);
	case FC_ENCAPSULATED_UNION:
		return (USHORT)read_short(format + 2);
	case FC_NON_ENCAPSULATED_UNION: {
		const BYTE *arms = format + 2 + correlation_size(m);
		return (USHORT)read_short(relative(arms));
	}
	case FC_RANGE:
		return base_sizes(format[1] & 0x0f).memory;
	case FC_IP: case FC_RP: case FC_UP: case FC_OP: case FC_FP:
		return sizeof(void *);
	default:
		return base_sizes(*format).memory;
	}
}

// The members of a complex struct, in memory order with explicit padding.
static BYTE *struct_members(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *members, const BYTE **layout)
{
	static void *nothing;
	ndr_op inner = op == NDR_MEMSIZE ? NDR_SKIP : op;
	BYTE *start = memory;
	for (const BYTE *at = members; *at != FC_END; ) {
		switch (*at) {
		case FC_PAD:
			at++;
			break;
		case FC_ALIGNM2:
		case FC_ALIGNM4:
		case FC_ALIGNM8: {
			// From the start of the structure: where it lies in memory may not be aligned at all.
			ULONG_PTR mask = *at == FC_ALIGNM2 ? 1 : *at == FC_ALIGNM4 ? 3 : 7;
			if (memory)
				memory = start + (((ULONG_PTR)(memory - start) + mask) & ~mask);
			at++;
			break;
		}
		case FC_STRUCTPAD1: case FC_STRUCTPAD2: case FC_STRUCTPAD3: case FC_STRUCTPAD4:
		case FC_STRUCTPAD5: case FC_STRUCTPAD6: case FC_STRUCTPAD7:
			if (memory)
				memory += *at - FC_STRUCTPAD1 + 1;
			at++;
			break;
		case FC_POINTER: {
			void **slot = memory ? (void **)memory : &nothing;
			embedded_pointer(m, inner, slot, *layout);
			*layout += 4;
			if (memory)
				memory += sizeof(void *);
			at++;
			break;
		}
		case FC_EMBEDDED_COMPLEX: {
			if (memory)
				memory += at[1];
			const BYTE *target = relative(at + 2);
			walk(m, inner, memory, target);
			if (memory)
				memory += complex_member_size(m, target);
			at += 4;
			break;
		}
		case FC_UP: case FC_RP: case FC_OP: case FC_FP: {
			void **slot = memory ? (void **)memory : &nothing;
			embedded_pointer(m, inner, slot, at);
			if (memory)
				memory += sizeof(void *);
			at += 4;
			break;
		}
		default:
			base_type(m, inner, memory, *at);
			if (memory)
				memory += base_sizes(*at).memory;
			at++;
			break;
		}
	}
	return memory;
}

static void struct_type(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format)
{
	BYTE type = format[0], alignment = format[1] + 1;
	ULONG size = (USHORT)read_short(format + 2);
	BYTE *outer = m->Memory;
	m->Memory = memory ? memory : outer;
	if (op == NDR_MEMSIZE)
		m->MemorySize += size;
	ndr_op body = op == NDR_MEMSIZE ? NDR_SKIP : op;
	switch (type) {
	case FC_STRUCT:
	case FC_PSTRUCT:
		buffer_align(m, body, alignment);
		flat_block(m, body, memory, size, type == FC_PSTRUCT ? layout_after(format + 4) : NULL);
		break;
	case FC_CSTRUCT:
	case FC_CPSTRUCT: {
		const BYTE *array = relative(format + 4);
		const BYTE *after = format + 6;
		conformance(m, op == NDR_MEMSIZE ? NDR_MEMSIZE : op, memory ? memory + size : NULL, array + 4);
		ULONG element = (USHORT)read_short(array + 2);
		buffer_align(m, body, alignment);
		m->ActualCount = (ULONG)m->MaxCount;
		m->Offset = 0;
		if (op == NDR_MEMSIZE)
			m->MemorySize += (ULONG)m->MaxCount * element;
		flat_block(m, body, memory, size + (SIZE_T)m->MaxCount * element, type == FC_CPSTRUCT ? layout_after(after) : NULL);
		break;
	}
	case FC_CVSTRUCT: {
		const BYTE *array = relative(format + 4);
		const BYTE *after = format + 6;
		BYTE array_kind = array[0];
		BYTE *tail = memory ? memory + size : NULL;
		ULONG unit;
		if (array_kind == FC_CVARRAY) {
			unit = (USHORT)read_short(array + 2);
			conformance(m, op, tail, array + 4);
		} else {
			unit = array_kind == FC_C_WSTRING ? 2 : 1;
			if (op == NDR_SIZE || op == NDR_MARSHAL) {
				ULONG length = unit == 2 ? lstrlenW((WCHAR *)tail) + 1 : lstrlenA((char *)tail) + 1;
				m->MaxCount = array[1] == FC_STRING_SIZED ? correlate(m, tail, array + 2, length) : length;
				m->ActualCount = length;
			}
			ULONG value = op == NDR_FREE ? 0 : count_put(m, op, (ULONG)m->MaxCount);
			if (op != NDR_SIZE && op != NDR_MARSHAL)
				m->MaxCount = value;
		}
		buffer_align(m, body, alignment);
		BYTE *image = m->Buffer;
		BYTE *saved = *after == FC_PP ? block_saved(m, op, memory, size + (SIZE_T)m->MaxCount * unit, after) : NULL;
		transfer(m, body, memory, size);
		if (array_kind == FC_CVARRAY) {
			variance(m, op, tail, array + 4 + correlation_size(m), (ULONG)m->MaxCount);
			buffer_align(m, body, array[1] + 1);
		} else if (op != NDR_FREE) {
			m->Offset = count_put(m, op, 0);
			m->ActualCount = count_put(m, op, m->ActualCount);
		}
		if (op == NDR_MEMSIZE)
			m->MemorySize += (ULONG)m->MaxCount * unit;
		transfer(m, body, tail, (SIZE_T)m->ActualCount * unit);
		if (*after == FC_PP)
			pointer_layout(m, body, memory, op == NDR_SIZE || op == NDR_FREE ? NULL : image, saved, after);
		if (saved)
			HeapFree(GetProcessHeap(), 0, saved);
		break;
	}
	case FC_BOGUS_STRUCT: {
		const BYTE *array = read_short(format + 4) ? relative(format + 4) : NULL;
		const BYTE *layout = read_short(format + 6) ? relative(format + 6) : NULL;
		const BYTE *members = format + 8;
		BYTE *saved_mark = m->ConformanceMark;
		// A conformant tail has its size up front, ahead of the members.
		if (array && (*array == FC_CARRAY || *array == FC_CVARRAY || *array == FC_BOGUS_ARRAY)) {
			conformance(m, op == NDR_MEMSIZE ? NDR_SKIP : op, memory ? memory + size : NULL, array + 4);
		}
		if (array && (*array == FC_C_CSTRING || *array == FC_C_WSTRING) && op != NDR_FREE) {
			ULONG unit = *array == FC_C_WSTRING ? 2 : 1;
			BYTE *tail = memory ? memory + size : NULL;
			if (op == NDR_SIZE || op == NDR_MARSHAL)
				m->MaxCount = unit == 2 ? lstrlenW((WCHAR *)tail) + 1 : lstrlenA((char *)tail) + 1;
			ULONG value = count_put(m, op, (ULONG)m->MaxCount);
			if (op != NDR_SIZE && op != NDR_MARSHAL)
				m->MaxCount = value;
		}
		ULONG_PTR held = m->MaxCount;
		buffer_align(m, body, alignment);
		struct_members(m, op, memory, members, &layout);
		m->MaxCount = held;
		if (array) {
			BYTE *tail = memory ? memory + size : NULL;
			if (*array == FC_CARRAY || *array == FC_CVARRAY || *array == FC_BOGUS_ARRAY) {
				ULONG unit = *array == FC_BOGUS_ARRAY ? 0 : (USHORT)read_short(array + 2);
				if (*array == FC_CARRAY) {
					m->Offset = 0;
					m->ActualCount = (ULONG)held;
					buffer_align(m, body, array[1] + 1);
					if (op == NDR_MEMSIZE)
						m->MemorySize += (ULONG)held * unit;
					flat_block(m, body, tail, (SIZE_T)held * unit, layout_after(array + 4 + correlation_size(m)));
				} else if (*array == FC_CVARRAY) {
					variance(m, op, tail, array + 4 + correlation_size(m), (ULONG)held);
					buffer_align(m, body, array[1] + 1);
					if (op == NDR_MEMSIZE)
						m->MemorySize += (ULONG)held * unit;
					flat_block(m, body, tail, (SIZE_T)m->ActualCount * unit, layout_after(array + 4 + 2 * correlation_size(m)));
				} else {
					const BYTE *var = array + 4 + correlation_size(m), *element = var + correlation_size(m);
					BOOL has_var = !(var[0] == 0xff && var[1] == 0xff);
					if (has_var) {
						variance(m, op, tail, var, (ULONG)held);
					} else {
						m->Offset = 0;
						m->ActualCount = (ULONG)held;
					}
					buffer_align(m, body, array[1] + 1);
					if (op == NDR_MEMSIZE) {
						const BYTE *target;
						skip_embedded(element, &target);
						m->MemorySize += (ULONG)(held * complex_member_size(m, target));
					}
					array_elements(m, op, tail, element, m->ActualCount, m->Offset);
				}
			} else if (*array == FC_C_CSTRING || *array == FC_C_WSTRING) {
				ULONG unit = *array == FC_C_WSTRING ? 2 : 1;
				if (op == NDR_SIZE || op == NDR_MARSHAL)
					m->ActualCount = (ULONG)held;
				ULONG offset = op == NDR_FREE ? 0 : count_put(m, op, 0);
				ULONG actual = op == NDR_FREE ? 0 : count_put(m, op, m->ActualCount);
				(void)offset;
				if (op != NDR_SIZE && op != NDR_MARSHAL)
					m->ActualCount = actual;
				if (op == NDR_MEMSIZE)
					m->MemorySize += (ULONG)held * unit;
				transfer(m, body, tail, (SIZE_T)m->ActualCount * unit);
			}
		}
		m->ConformanceMark = saved_mark;
		break;
	}
	}
	m->Memory = outer;
}

// ---- unions ----

static void union_arm(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *arms, LONG_PTR value)
{
	USHORT header = (USHORT)read_short(arms);
	ULONG count = header & 0x0fff, alignment = header >> 12;
	const BYTE *entry = arms + 2;
	const BYTE *arm = NULL;
	if (alignment)
		buffer_align(m, op == NDR_MEMSIZE ? NDR_SKIP : op, alignment);
	for (ULONG i = 0; i < count && !arm; i++, entry += 6)
		if ((LONG)read_long(entry) == (LONG)value)
			arm = entry + 4;
	if (!arm)
		arm = entry;
	if (arm == entry && (USHORT)read_short(arm) == 0xffff && count && (LONG)read_long(entry - 6) != (LONG)value)
		RpcRaiseException(RPC_S_INVALID_TAG);
	USHORT arm_value = (USHORT)read_short(arm);
	ndr_op inner = op == NDR_MEMSIZE ? NDR_SKIP : op;
	if ((arm_value & 0xff00) == 0x8000) {
		base_type(m, inner, memory, (BYTE)(arm_value & 0xff));
		return;
	}
	if (!arm_value)
		return;
	const BYTE *target = arm + (SHORT)arm_value;
	if (*target == FC_RP || *target == FC_UP || *target == FC_OP || *target == FC_FP) {
		static void *nothing;
		void **slot = memory ? (void **)memory : &nothing;
		embedded_pointer(m, inner, slot, target);
		return;
	}
	walk(m, inner, memory, target);
}

static LONG_PTR switch_value(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE type, BYTE *in_memory, LONG_PTR computed)
{
	BYTE wire[8] = {0};
	base_size size = base_sizes(type);
	ndr_op inner = op == NDR_MEMSIZE ? NDR_SKIP : op;
	if (op == NDR_FREE)
		return computed;
	if (op == NDR_SIZE || op == NDR_MARSHAL) {
		memcpy(wire, &computed, size.memory > sizeof(computed) ? sizeof(computed) : size.memory);
		base_type(m, inner, wire, type);
		return computed;
	}
	buffer_align(m, inner, size.wire);
	buffer_check(m, size.wire);
	memcpy(wire, m->Buffer, size.wire);
	m->Buffer += size.wire;
	LONG_PTR value;
	switch (type) {
	case FC_CHAR: case FC_USMALL: case FC_BYTE: value = wire[0]; break;
	case FC_SMALL: value = *(signed char *)wire; break;
	case FC_SHORT: value = *(SHORT *)wire; break;
	case FC_WCHAR: case FC_USHORT: case FC_ENUM16: value = *(USHORT *)wire; break;
	default: value = *(LONG *)wire; break;
	}
	if (in_memory)
		memcpy(in_memory, &value, size.memory > sizeof(value) ? sizeof(value) : size.memory);
	return value;
}

static void union_type(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format)
{
	if (format[0] == FC_ENCAPSULATED_UNION) {
		BYTE description = format[1], type = description & 0x0f;
		ULONG increment = description >> 4;
		ULONG size = (USHORT)read_short(format + 2);
		if (op == NDR_MEMSIZE)
			m->MemorySize += size;
		LONG_PTR value = 0;
		if (memory && op != NDR_UNMARSHAL) {
			switch (base_sizes(type).memory) {
			case 1: value = type == FC_SMALL ? *(signed char *)memory : *memory; break;
			case 2: value = type == FC_SHORT ? *(SHORT *)memory : *(USHORT *)memory; break;
			default: value = *(LONG *)memory; break;
			}
		}
		value = switch_value(m, op, type, op == NDR_UNMARSHAL ? memory : NULL, value);
		union_arm(m, op, memory ? memory + increment : NULL, format + 4, value);
		return;
	}
	BYTE type = format[1];
	const BYTE *desc = format + 2, *arms = relative(desc + correlation_size(m));
	if (op == NDR_MEMSIZE)
		m->MemorySize += (USHORT)read_short(arms);
	LONG_PTR value = 0;
	if (op == NDR_SIZE || op == NDR_MARSHAL || op == NDR_FREE)
		value = (LONG_PTR)correlate(m, memory ? memory : m->Memory, desc, m->MaxCount);
	value = switch_value(m, op, type, NULL, value);
	union_arm(m, op, memory, arms + 2, value);
}

// ---- things with their own rules ----

static const USER_MARSHAL_ROUTINE_QUADRUPLE *user_routines(const MIDL_STUB_MESSAGE *m, const BYTE *format)
{
	return &m->StubDesc->aUserMarshalQuadruple[(USHORT)read_short(format + 2)];
}

static ULONG user_flags(const MIDL_STUB_MESSAGE *m)
{
	return MAKELONG(m->dwDestContext, m->RpcMsg ? m->RpcMsg->DataRepresentation : NDR_LOCAL_DATA_REPRESENTATION);
}

static void user_marshal(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format)
{
	BYTE flags = format[1];
	USER_MARSHAL_CB cb;
	memset(&cb, 0, sizeof(cb));
	cb.Flags = user_flags(m);
	cb.pStubMsg = m;
	cb.pReserve = NULL;
	cb.Signature = USER_MARSHAL_CB_SIGNATURE;
	cb.CBType = op == NDR_SIZE ? USER_MARSHAL_CB_BUFFER_SIZE : op == NDR_MARSHAL ? USER_MARSHAL_CB_MARSHALL : op == NDR_FREE ? USER_MARSHAL_CB_FREE : USER_MARSHAL_CB_UNMARSHALL;
	cb.pFormat = format;
	cb.pTypeFormat = relative(format + 8);
	const USER_MARSHAL_ROUTINE_QUADRUPLE *routines = user_routines(m, format);
	ULONG alignment = (flags & 0x0f) + 1;
	USHORT memory_size = (USHORT)read_short(format + 4), wire_size = (USHORT)read_short(format + 6);
	if (flags & USER_MARSHAL_POINTER && op != NDR_FREE) {
		ULONG id = 0x20000;
		buffer_align(m, op == NDR_MEMSIZE ? NDR_SKIP : op, 4);
		transfer(m, op == NDR_MEMSIZE ? NDR_SKIP : op, &id, 4);
	}
	switch (op) {
	case NDR_SIZE:
		buffer_align(m, op, alignment);
		m->BufferLength = routines->pfnBufferSize(&cb.Flags, m->BufferLength, memory);
		break;
	case NDR_MARSHAL:
		buffer_align(m, op, alignment);
		m->Buffer = routines->pfnMarshall(&cb.Flags, m->Buffer, memory);
		break;
	case NDR_UNMARSHAL:
		buffer_align(m, op, alignment);
		m->Buffer = routines->pfnUnmarshall(&cb.Flags, m->Buffer, memory);
		break;
	case NDR_FREE:
		routines->pfnFree(&cb.Flags, memory);
		break;
	case NDR_MEMSIZE:
		m->MemorySize += memory_size;
		/* fall through */
	case NDR_SKIP:
		buffer_align(m, NDR_SKIP, alignment);
		if (wire_size) {
			m->Buffer += wire_size;
		} else {
			// A size unknown ahead of time is found by reading the value into scratch.
			void *scratch = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, memory_size ? memory_size : 1);
			m->Buffer = routines->pfnUnmarshall(&cb.Flags, m->Buffer, scratch);
			routines->pfnFree(&cb.Flags, scratch);
			HeapFree(GetProcessHeap(), 0, scratch);
		}
		break;
	}
}

static void transmit_as(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format)
{
	const XMIT_ROUTINE_QUINTUPLE *routines = &m->StubDesc->aXmitQuintuple[(USHORT)read_short(format + 2)];
	USHORT present = (USHORT)read_short(format + 4), transmitted = (USHORT)read_short(format + 6);
	const BYTE *target = relative(format + 8);
	BYTE *saved_present = m->pPresentedType, *saved_transmit = m->pTransmitType;
	switch (op) {
	case NDR_SIZE:
	case NDR_MARSHAL:
		m->pPresentedType = memory;
		m->pTransmitType = NULL;
		routines->pfnTranslateToXmit(m);
		walk(m, op, m->pTransmitType, target);
		routines->pfnFreeXmit(m);
		break;
	case NDR_UNMARSHAL: {
		BYTE *transmit = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, transmitted ? transmitted : 1);
		m->pPresentedType = memory;
		m->pTransmitType = transmit;
		walk(m, op, transmit, target);
		routines->pfnTranslateFromXmit(m);
		routines->pfnFreeXmit(m);
		HeapFree(GetProcessHeap(), 0, transmit);
		break;
	}
	case NDR_FREE:
		m->pPresentedType = memory;
		routines->pfnFreeInst(m);
		break;
	case NDR_MEMSIZE:
		m->MemorySize += present;
		/* fall through */
	case NDR_SKIP:
		walk(m, NDR_SKIP, NULL, target);
		break;
	}
	m->pPresentedType = saved_present;
	m->pTransmitType = saved_transmit;
}

static void interface_pointer(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format)
{
	const IID *iid = NULL;
	if (format[1] == FC_CONSTANT_IID)
		iid = (const IID *)(format + 2);
	else if (op != NDR_FREE && op != NDR_MEMSIZE && op != NDR_SKIP)
		iid = (const IID *)correlate(m, m->Memory ? m->Memory : memory, format + 2, 0);
	ndr_interface(m, op, memory, iid);
}

static void walk(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format)
{
	switch (*format) {
	case FC_STRUCT: case FC_PSTRUCT: case FC_CSTRUCT: case FC_CPSTRUCT: case FC_CVSTRUCT: case FC_BOGUS_STRUCT:
		struct_type(m, op, memory, format);
		break;
	case FC_CARRAY: case FC_CVARRAY: case FC_SMFARRAY: case FC_LGFARRAY: case FC_SMVARRAY: case FC_LGVARRAY: case FC_BOGUS_ARRAY:
		array_type(m, op, memory, format);
		break;
	case FC_C_CSTRING: case FC_C_WSTRING: case FC_CSTRING: case FC_WSTRING:
		string_type(m, op, memory, format);
		break;
	case FC_ENCAPSULATED_UNION: case FC_NON_ENCAPSULATED_UNION:
		union_type(m, op, memory, format);
		break;
	case FC_RP: case FC_UP: case FC_OP: case FC_FP: {
		static void *nothing;
		void **slot = memory ? (void **)memory : &nothing;
		if (op == NDR_MEMSIZE)
			m->MemorySize += sizeof(void *);
		top_pointer(m, op == NDR_MEMSIZE ? NDR_SKIP : op, slot, format);
		break;
	}
	case FC_USER_MARSHAL:
		user_marshal(m, op, memory, format);
		break;
	case FC_TRANSMIT_AS: case FC_REPRESENT_AS:
		transmit_as(m, op, memory, format);
		break;
	case FC_IP:
		interface_pointer(m, op, memory, format);
		break;
	case FC_RANGE: {
		BYTE type = format[1] & 0x0f;
		LONG low = (LONG)read_long(format + 2), high = (LONG)read_long(format + 6);
		base_type(m, op, memory, type);
		if (op == NDR_UNMARSHAL) {
			LONG value = 0;
			memcpy(&value, memory, base_sizes(type).memory > 4 ? 4 : base_sizes(type).memory);
			if (value < low || value > high)
				RpcRaiseException(RPC_X_INVALID_BOUND);
		}
		break;
	}
	default:
		base_type(m, op, memory, *format);
		break;
	}
}

// Flat data a server can use where it lies in the buffer; NULL if the type needs a block of its own.
BYTE *ndr_flat_start(MIDL_STUB_MESSAGE *m, const BYTE *format)
{
	ULONG_PTR at = (ULONG_PTR)m->Buffer;
	ULONG alignment = (ULONG)format[1] + 1;
	switch (*format) {
	case FC_C_CSTRING:
	case FC_C_WSTRING:
		at = ((at + 3) & ~(ULONG_PTR)3) + 12;
		break;
	case FC_STRUCT:
	case FC_PSTRUCT:
	case FC_SMFARRAY:
	case FC_LGFARRAY:
		at = (at + alignment - 1) & ~(ULONG_PTR)(alignment - 1);
		break;
	case FC_CARRAY:
		at = ((at + 3) & ~(ULONG_PTR)3) + 4;
		at = (at + alignment - 1) & ~(ULONG_PTR)(alignment - 1);
		break;
	case FC_CSTRUCT:
		at = ((at + 3) & ~(ULONG_PTR)3) + 4;
		at = (at + alignment - 1) & ~(ULONG_PTR)(alignment - 1);
		break;
	case FC_BYTE: case FC_CHAR: case FC_SMALL: case FC_USMALL: case FC_WCHAR: case FC_SHORT: case FC_USHORT:
	case FC_LONG: case FC_ULONG: case FC_FLOAT: case FC_ENUM32: case FC_ERROR_STATUS_T: case FC_HYPER: case FC_DOUBLE:
		alignment = base_sizes(*format).wire;
		at = (at + alignment - 1) & ~(ULONG_PTR)(alignment - 1);
		break;
	default:
		return NULL;
	}
	return (BYTE *)at;
}

SIZE_T ndr_fixed_size(MIDL_STUB_MESSAGE *m, const BYTE *format)
{
	return complex_member_size(m, format);
}

// How much an out parameter's block must hold, sized from the in parameters that are already on the stack.
SIZE_T ndr_out_size(MIDL_STUB_MESSAGE *m, BYTE *args, const BYTE *format)
{
	BYTE *saved = m->StackTop;
	SIZE_T size;
	m->StackTop = args;
	switch (*format) {
	case FC_CARRAY:
	case FC_CVARRAY:
		size = (SIZE_T)(USHORT)read_short(format + 2) * correlate(m, NULL, format + 4, 0);
		break;
	case FC_C_CSTRING:
		size = correlate(m, NULL, format + 2, 0);
		break;
	case FC_C_WSTRING:
		size = 2 * correlate(m, NULL, format + 2, 0);
		break;
	case FC_BOGUS_ARRAY: {
		const BYTE *element, *target;
		element = format + 4 + 2 * correlation_size(m);
		skip_embedded(element, &target);
		SIZE_T count = format[4] == 0xff ? (USHORT)read_short(format + 2) : correlate(m, NULL, format + 4, 0);
		size = count * complex_member_size(m, target);
		break;
	}
	case FC_CSTRING:
		size = (USHORT)read_short(format + 2);
		break;
	case FC_WSTRING:
		size = 2 * (SIZE_T)(USHORT)read_short(format + 2);
		break;
	default:
		size = complex_member_size(m, format);
		break;
	}
	m->StackTop = saved;
	return size ? size : sizeof(void *);
}

void ndr_run(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format)
{
	frame_run(m, op, memory, format);
}

void ndr_run_pointer(MIDL_STUB_MESSAGE *m, ndr_op op, void **slot, const BYTE *format)
{
	walk_frame frame = {NULL, 0, 0, (walk_frame *)m->pPointerQueueState};
	m->pPointerQueueState = (void *)&frame;
	top_pointer(m, op, slot, format);
	frame_drain(m, op, &frame);
	m->pPointerQueueState = (void *)frame.outer;
}
