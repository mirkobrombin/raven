/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "raven_rtl.h"

/* The guest heap. Small blocks live in segments, with a chunk header before each and the free ones in size bins found through a bitmap; a block too
 * large for a segment gets its own mapping. Segments commit from the bottom up as the unused top is reached. Nothing here keeps a pointer into a
 * chunk's payload while it is in use, so programs that write past a block damage the neighbour's header and nothing is hidden from them. */

#define HEAP_MAGIC 0x52485031u
#define UNIT 16u
#define MIN_UNITS 2u
#define EXACT_BINS 64u
#define BIN_COUNT 128u
#define SEGMENT_MINIMUM ((SIZE_T)1024 * 1024)
#define SEGMENT_MAXIMUM ((SIZE_T)64 * 1024 * 1024)
#define COMMIT_STEP ((SIZE_T)64 * 1024)
/* Blocks from this size up are mapped on their own and return to the system when freed. */
#define LARGE_MINIMUM ((SIZE_T)512 * 1024)

enum chunk_state { state_free = 0x5a, state_busy = 0xb5, state_large = 0xc3 };

typedef struct chunk {
	ULONG units, previous_units;
	/* Bytes between the end of the caller's block and the end of the chunk. */
	ULONG slack;
	UCHAR state;
	UCHAR reserved[3];
} chunk;

C_ASSERT(sizeof(chunk) == UNIT);

/* A free chunk's payload links it into its bin. */
typedef struct free_links {
	struct free_links *next, *previous;
} free_links;

typedef struct segment {
	struct segment *next;
	PUCHAR base, top, committed, end;
	/* The size of the chunk just below the top, which the next chunk carved will record as its neighbour. */
	ULONG last_units;
	/* A segment over caller-supplied memory has no mapping to give back. */
	BOOLEAN owned;
} segment;

typedef struct large_block {
	struct large_block *next, *previous;
	SIZE_T user_size, mapped;
	chunk header;
} large_block;

typedef struct heap {
	ULONG magic, flags;
	PVOID lock_word;
	ULONG protection;
	BOOLEAN growable;
	segment *segments;
	large_block *large;
	SIZE_T next_segment_size;
	ULONG64 bin_bits[BIN_COUNT / 64];
	free_links bins[BIN_COUNT];
} heap;

#define LAST_ERROR (*(volatile ULONG *)((PUCHAR)NtCurrentTeb() + 0x68))

static PVOID process_heaps_lock;
static BOOLEAN initialized;

/* The lock is a word as an SRW lock is: the thread that releases it and asks again gets it back rather than queueing behind a waiter. */
static void heap_lock(heap *owner, ULONG flags)
{
	if ((flags | owner->flags) & HEAP_NO_SERIALIZE)
		return;
	if (InterlockedCompareExchangePointer(&owner->lock_word, (PVOID)1, NULL))
		RtlAcquireSRWLockExclusive((PRTL_SRWLOCK)&owner->lock_word);
}

static void heap_unlock(heap *owner, ULONG flags)
{
	if ((flags | owner->flags) & HEAP_NO_SERIALIZE)
		return;
	InterlockedExchangePointer(&owner->lock_word, NULL);
}

static unsigned bin_of(SIZE_T units)
{
	if (units < EXACT_BINS)
		return (unsigned)units;
	unsigned high = 63 - (unsigned)__builtin_clzll(units);
	unsigned index = EXACT_BINS + (high - 6) * 4 + (unsigned)((units >> (high - 2)) & 3);
	return index < BIN_COUNT ? index : BIN_COUNT - 1;
}

static void bin_insert(heap *owner, chunk *block)
{
	unsigned bin = bin_of(block->units);
	free_links *head = &owner->bins[bin], *links = (free_links *)(block + 1);
	links->next = head->next;
	links->previous = head;
	head->next->previous = links;
	head->next = links;
	owner->bin_bits[bin / 64] |= 1ull << (bin % 64);
}

static void bin_remove(heap *owner, chunk *block)
{
	free_links *links = (free_links *)(block + 1);
	links->previous->next = links->next;
	links->next->previous = links->previous;
	unsigned bin = bin_of(block->units);
	if (owner->bins[bin].next == &owner->bins[bin])
		owner->bin_bits[bin / 64] &= ~(1ull << (bin % 64));
}

static chunk *next_chunk(chunk *block)
{
	return block + block->units;
}

static segment *segment_of(heap *owner, const void *address)
{
	for (segment *item = owner->segments; item; item = item->next) {
		if ((PUCHAR)address >= item->base && (PUCHAR)address < item->top)
			return item;
	}
	return NULL;
}

static chunk *chunk_of(heap *owner, void *block)
{
	if ((ULONG_PTR)block & (UNIT - 1) || !segment_of(owner, block))
		return NULL;
	chunk *header = (chunk *)block - 1;
	return header->state == state_busy ? header : NULL;
}

static large_block *large_of(heap *owner, void *block)
{
	/* The block of a large mapping sits at one offset in its page, which spares the walk for every other bad pointer. */
	if (((ULONG_PTR)block & 0xfff) != sizeof(large_block))
		return NULL;
	for (large_block *item = owner->large; item; item = item->next) {
		if (item + 1 == (large_block *)block)
			return item;
	}
	return NULL;
}

static BOOLEAN commit_up_to(heap *owner, segment *item, PUCHAR wanted)
{
	if (wanted <= item->committed)
		return TRUE;
	if (!item->owned)
		return FALSE;
	SIZE_T size = (SIZE_T)(wanted - item->committed);
	size = (size + COMMIT_STEP - 1) & ~(COMMIT_STEP - 1);
	if (size > (SIZE_T)(item->end - item->committed))
		size = (SIZE_T)(item->end - item->committed);
	PVOID base = item->committed;
	if (!NT_SUCCESS(ZwAllocateVirtualMemory(CURRENT_PROCESS, &base, 0, &size, MEM_COMMIT, owner->protection)))
		return FALSE;
	item->committed += size;
	return TRUE;
}

static segment *add_segment(heap *owner, SIZE_T needed)
{
	SIZE_T size = owner->next_segment_size;
	/* Room for the segment record and the block, with the mapping rounded to the allocation granularity. */
	needed = needed + sizeof(segment) + UNIT * 2;
	if (size < needed)
		size = needed;
	size = (size + 0xffff) & ~(SIZE_T)0xffff;
	PVOID base = NULL;
	if (!NT_SUCCESS(ZwAllocateVirtualMemory(CURRENT_PROCESS, &base, 0, &size, MEM_RESERVE, owner->protection)))
		return NULL;
	SIZE_T first = COMMIT_STEP;
	PVOID commit = base;
	if (!NT_SUCCESS(ZwAllocateVirtualMemory(CURRENT_PROCESS, &commit, 0, &first, MEM_COMMIT, owner->protection))) {
		SIZE_T none = 0;
		ZwFreeVirtualMemory(CURRENT_PROCESS, &base, &none, MEM_RELEASE);
		return NULL;
	}
	segment *item = base;
	item->base = (PUCHAR)base + ((sizeof(segment) + UNIT - 1) & ~(SIZE_T)(UNIT - 1));
	item->top = item->base;
	item->committed = (PUCHAR)base + first;
	item->end = (PUCHAR)base + size;
	item->owned = TRUE;
	item->next = owner->segments;
	owner->segments = item;
	if (owner->next_segment_size < SEGMENT_MAXIMUM)
		owner->next_segment_size *= 2;
	return item;
}

static chunk *carve_from_top(heap *owner, segment *item, SIZE_T units)
{
	SIZE_T bytes = units * UNIT;
	if ((SIZE_T)(item->end - item->top) < bytes || !commit_up_to(owner, item, item->top + bytes))
		return NULL;
	chunk *block = (chunk *)item->top;
	block->units = (ULONG)units;
	block->previous_units = item->last_units;
	block->slack = 0;
	block->state = state_busy;
	item->top += bytes;
	item->last_units = (ULONG)units;
	return block;
}

/* Tells whatever follows a chunk that the chunk's size changed. */
static void note_size(segment *item, chunk *block)
{
	chunk *after = next_chunk(block);
	if ((PUCHAR)after < item->top)
		after->previous_units = block->units;
	else
		item->last_units = block->units;
}

/* Takes what the block needs and gives the rest back to the bins, joined with a free neighbour so two free chunks never touch. */
static void trim_chunk(heap *owner, segment *item, chunk *block, SIZE_T units)
{
	if (block->units - units < MIN_UNITS)
		return;
	chunk *rest = block + units;
	rest->units = block->units - (ULONG)units;
	rest->previous_units = (ULONG)units;
	rest->slack = 0;
	rest->state = state_free;
	block->units = (ULONG)units;
	chunk *after = next_chunk(rest);
	if ((PUCHAR)after < item->top && after->state == state_free) {
		bin_remove(owner, after);
		rest->units += after->units;
	}
	note_size(item, rest);
	bin_insert(owner, rest);
}

static chunk *take_from_bins(heap *owner, SIZE_T units)
{
	unsigned bin = bin_of(units);
	/* The bin a size falls in can also hold smaller chunks once sizes stop being exact: only the first few are looked at, then larger bins. */
	if (bin >= EXACT_BINS) {
		unsigned looked = 0;
		for (free_links *links = owner->bins[bin].next; links != &owner->bins[bin] && looked < 16; links = links->next, looked++) {
			chunk *block = (chunk *)links - 1;
			if (block->units >= units) {
				bin_remove(owner, block);
				return block;
			}
		}
		bin++;
	}
	for (unsigned word = bin / 64; word < BIN_COUNT / 64; word++) {
		ULONG64 bits = owner->bin_bits[word];
		if (word == bin / 64)
			bits &= ~0ull << (bin % 64);
		if (!bits)
			continue;
		unsigned found = word * 64 + (unsigned)__builtin_ctzll(bits);
		chunk *block = (chunk *)owner->bins[found].next - 1;
		bin_remove(owner, block);
		return block;
	}
	return NULL;
}

static void *allocate_large(heap *owner, SIZE_T size)
{
	SIZE_T mapped = (size + sizeof(large_block) + 0xfff) & ~(SIZE_T)0xfff;
	if (mapped < size)
		return NULL;
	PVOID base = NULL;
	if (!NT_SUCCESS(ZwAllocateVirtualMemory(CURRENT_PROCESS, &base, 0, &mapped, MEM_RESERVE | MEM_COMMIT, owner->protection)))
		return NULL;
	large_block *item = base;
	item->user_size = size;
	item->mapped = mapped;
	item->header.state = state_large;
	item->previous = NULL;
	item->next = owner->large;
	if (item->next)
		item->next->previous = item;
	owner->large = item;
	return item + 1;
}

static void free_large(heap *owner, large_block *item)
{
	if (item->previous)
		item->previous->next = item->next;
	else
		owner->large = item->next;
	if (item->next)
		item->next->previous = item->previous;
	PVOID base = item;
	SIZE_T none = 0;
	ZwFreeVirtualMemory(CURRENT_PROCESS, &base, &none, MEM_RELEASE);
}

static void *allocate_small(heap *owner, SIZE_T size)
{
	SIZE_T units = (size + sizeof(chunk) + UNIT - 1) / UNIT;
	if (units < MIN_UNITS)
		units = MIN_UNITS;
	chunk *block = take_from_bins(owner, units);
	segment *item = NULL;
	if (block) {
		block->state = state_busy;
		item = segment_of(owner, block);
	} else {
		for (item = owner->segments; item; item = item->next) {
			if ((block = carve_from_top(owner, item, units)))
				break;
		}
		if (!block) {
			if (!owner->growable || !(item = add_segment(owner, units * UNIT)))
				return NULL;
			block = carve_from_top(owner, item, units);
			if (!block)
				return NULL;
		}
	}
	trim_chunk(owner, item, block, units);
	block->slack = (ULONG)(block->units * UNIT - sizeof(chunk) - size);
	return block + 1;
}

static void free_small(heap *owner, chunk *block)
{
	segment *item = segment_of(owner, block);
	block->state = state_free;
	block->slack = 0;
	if ((PUCHAR)block > item->base) {
		chunk *before = block - block->previous_units;
		if (before->state == state_free) {
			bin_remove(owner, before);
			before->units += block->units;
			block = before;
		}
	}
	chunk *after = next_chunk(block);
	if ((PUCHAR)after < item->top && after->state == state_free) {
		bin_remove(owner, after);
		block->units += after->units;
		after = next_chunk(block);
	}
	if ((PUCHAR)after >= item->top) {
		/* The last chunk goes back to the top, where it can serve a block of any size. */
		item->top = (PUCHAR)block;
		item->last_units = block->previous_units;
		return;
	}
	after->previous_units = block->units;
	bin_insert(owner, block);
}

static void *fail_block(heap *owner, ULONG flags)
{
	if ((flags | owner->flags) & HEAP_GENERATE_EXCEPTIONS)
		RtlRaiseStatus(STATUS_NO_MEMORY);
	return NULL;
}

static heap *heap_of(PVOID handle)
{
	heap *owner = handle;
	return owner && owner->magic == HEAP_MAGIC ? owner : NULL;
}

PVOID NTAPI RtlAllocateHeap(PVOID handle, ULONG flags, SIZE_T size)
{
	heap *owner = heap_of(handle);
	if (!owner)
		return NULL;
	if (size > (SIZE_T)0x7fffffff0000ull)
		return fail_block(owner, flags);
	void *block;
	heap_lock(owner, flags);
	block = size >= LARGE_MINIMUM && owner->growable ? allocate_large(owner, size) : allocate_small(owner, size);
	heap_unlock(owner, flags);
	if (!block)
		return fail_block(owner, flags);
	if ((flags | owner->flags) & HEAP_ZERO_MEMORY)
		RtlZeroMemory(block, size);
	return block;
}

BOOLEAN NTAPI RtlFreeHeap(PVOID handle, ULONG flags, PVOID block)
{
	heap *owner = heap_of(handle);
	if (!block)
		return TRUE;
	if (!owner)
		return FALSE;
	BOOLEAN result = TRUE;
	heap_lock(owner, flags);
	chunk *small = chunk_of(owner, block);
	large_block *large = small ? NULL : large_of(owner, block);
	if (small)
		free_small(owner, small);
	else if (large)
		free_large(owner, large);
	else
		result = FALSE;
	heap_unlock(owner, flags);
	return result;
}

SIZE_T NTAPI RtlSizeHeap(PVOID handle, ULONG flags, PVOID block)
{
	heap *owner = heap_of(handle);
	if (!owner || !block)
		return (SIZE_T)-1;
	SIZE_T size = (SIZE_T)-1;
	heap_lock(owner, flags);
	chunk *small = chunk_of(owner, block);
	large_block *large = small ? NULL : large_of(owner, block);
	if (small)
		size = small->units * (SIZE_T)UNIT - sizeof(chunk) - small->slack;
	else if (large)
		size = large->user_size;
	heap_unlock(owner, flags);
	return size;
}

/* Grows a chunk in place into a free neighbour or the unused top. */
static BOOLEAN grow_in_place(heap *owner, segment *item, chunk *block, SIZE_T units)
{
	chunk *after = next_chunk(block);
	if ((PUCHAR)after < item->top && after->state == state_free) {
		if (block->units + after->units < units)
			return FALSE;
		bin_remove(owner, after);
		block->units += after->units;
		note_size(item, block);
		trim_chunk(owner, item, block, units);
		return TRUE;
	}
	if ((PUCHAR)after != item->top)
		return FALSE;
	SIZE_T extra = (units - block->units) * UNIT;
	if ((SIZE_T)(item->end - item->top) < extra || !commit_up_to(owner, item, item->top + extra))
		return FALSE;
	item->top += extra;
	block->units = (ULONG)units;
	item->last_units = (ULONG)units;
	return TRUE;
}

PVOID NTAPI RtlReAllocateHeap(PVOID handle, ULONG flags, PVOID block, SIZE_T size)
{
	heap *owner = heap_of(handle);
	if (!owner || !block)
		return NULL;
	if (size > (SIZE_T)0x7fffffff0000ull)
		return fail_block(owner, flags);
	ULONG all = flags | owner->flags;
	void *result = NULL;
	SIZE_T old_size = 0;
	heap_lock(owner, flags);
	chunk *small = chunk_of(owner, block);
	large_block *large = small ? NULL : large_of(owner, block);
	if (small) {
		SIZE_T capacity = small->units * (SIZE_T)UNIT - sizeof(chunk);
		old_size = capacity - small->slack;
		SIZE_T units = (size + sizeof(chunk) + UNIT - 1) / UNIT;
		if (units < MIN_UNITS)
			units = MIN_UNITS;
		segment *item = segment_of(owner, small);
		if (units <= small->units) {
			trim_chunk(owner, item, small, units);
			result = block;
		} else if (size < LARGE_MINIMUM && grow_in_place(owner, item, small, units)) {
			result = block;
		}
		if (result)
			small->slack = (ULONG)(small->units * UNIT - sizeof(chunk) - size);
	} else if (large) {
		old_size = large->user_size;
		if (size + sizeof(large_block) <= large->mapped && size >= LARGE_MINIMUM / 2) {
			large->user_size = size;
			result = block;
		}
	} else {
		heap_unlock(owner, flags);
		return NULL;
	}
	if (!result && !(all & HEAP_REALLOC_IN_PLACE_ONLY)) {
		result = size >= LARGE_MINIMUM && owner->growable ? allocate_large(owner, size) : allocate_small(owner, size);
		if (result) {
			RtlMoveMemory(result, block, old_size < size ? old_size : size);
			if (small)
				free_small(owner, small);
			else
				free_large(owner, large);
		}
	}
	heap_unlock(owner, flags);
	if (!result)
		return fail_block(owner, flags);
	if ((all & HEAP_ZERO_MEMORY) && size > old_size)
		RtlZeroMemory((PUCHAR)result + old_size, size - old_size);
	return result;
}

static BOOLEAN register_heap(PVOID handle)
{
	BOOLEAN done = FALSE;
	if (InterlockedCompareExchangePointer(&process_heaps_lock, (PVOID)1, NULL))
		RtlAcquireSRWLockExclusive((PRTL_SRWLOCK)&process_heaps_lock);
	if (PEB_NUMBER_OF_HEAPS < PEB_MAXIMUM_HEAPS) {
		PEB_PROCESS_HEAPS[PEB_NUMBER_OF_HEAPS++] = handle;
		done = TRUE;
	}
	InterlockedExchangePointer(&process_heaps_lock, NULL);
	return done;
}

static void unregister_heap(PVOID handle)
{
	if (InterlockedCompareExchangePointer(&process_heaps_lock, (PVOID)1, NULL))
		RtlAcquireSRWLockExclusive((PRTL_SRWLOCK)&process_heaps_lock);
	for (ULONG index = 0; index < PEB_NUMBER_OF_HEAPS; index++) {
		if (PEB_PROCESS_HEAPS[index] != handle)
			continue;
		PEB_PROCESS_HEAPS[index] = PEB_PROCESS_HEAPS[--PEB_NUMBER_OF_HEAPS];
		PEB_PROCESS_HEAPS[PEB_NUMBER_OF_HEAPS] = NULL;
		break;
	}
	InterlockedExchangePointer(&process_heaps_lock, NULL);
}

PVOID NTAPI RtlCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PRTL_HEAP_PARAMETERS parameters)
{
	(void)lock;
	(void)parameters;
	/* HEAP_NO_SERIALIZE, HEAP_GROWABLE, HEAP_GENERATE_EXCEPTIONS and HEAP_CREATE_ENABLE_EXECUTE, whose pages are committed executable as the .NET
	   runtime's compiler needs for the stubs it keeps there. */
	if (!initialized || (flags & ~(7U | HEAP_CREATE_ENABLE_EXECUTE))) {
		LAST_ERROR = ERROR_NOT_SUPPORTED;
		return NULL;
	}
	ULONG protection = flags & HEAP_CREATE_ENABLE_EXECUTE ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
	BOOLEAN growable = !reserve || (flags & HEAP_GROWABLE);
	SIZE_T size = reserve > commit ? reserve : commit;
	SIZE_T record = (sizeof(heap) + sizeof(segment) + UNIT - 1) & ~(SIZE_T)(UNIT - 1);
	if (size < record + UNIT * 4)
		size = SEGMENT_MINIMUM;
	size = (size + 0xffff) & ~(SIZE_T)0xffff;
	PVOID memory = base;
	SIZE_T committed = record + COMMIT_STEP;
	committed = (committed + 0xfff) & ~(SIZE_T)0xfff;
	if (committed > size)
		committed = size;
	if (!base) {
		if (!NT_SUCCESS(ZwAllocateVirtualMemory(CURRENT_PROCESS, &memory, 0, &size, MEM_RESERVE, protection)))
			goto no_memory;
		PVOID start = memory;
		if (!NT_SUCCESS(ZwAllocateVirtualMemory(CURRENT_PROCESS, &start, 0, &committed, MEM_COMMIT, protection))) {
			SIZE_T none = 0;
			ZwFreeVirtualMemory(CURRENT_PROCESS, &memory, &none, MEM_RELEASE);
			goto no_memory;
		}
	} else {
		/* Caller-supplied memory is taken as committed for what the caller says. */
		committed = commit > record + UNIT * 4 ? commit : size;
	}
	heap *owner = memory;
	RtlZeroMemory(owner, sizeof(*owner));
	owner->magic = HEAP_MAGIC;
	owner->flags = flags;
	owner->protection = protection;
	owner->growable = growable;
	owner->next_segment_size = SEGMENT_MINIMUM * 4;
	for (unsigned index = 0; index < BIN_COUNT; index++)
		owner->bins[index].next = owner->bins[index].previous = &owner->bins[index];
	segment *first = (segment *)(owner + 1);
	first->base = (PUCHAR)memory + record;
	first->top = first->base;
	first->committed = (PUCHAR)memory + committed;
	first->end = (PUCHAR)memory + size;
	first->owned = !base;
	first->next = NULL;
	owner->segments = first;
	if (!register_heap(owner)) {
		if (!base) {
			SIZE_T none = 0;
			ZwFreeVirtualMemory(CURRENT_PROCESS, &memory, &none, MEM_RELEASE);
		}
		goto no_memory;
	}
	return owner;

no_memory:
	LAST_ERROR = ERROR_NOT_ENOUGH_MEMORY;
	if (flags & HEAP_GENERATE_EXCEPTIONS)
		RtlRaiseStatus(STATUS_NO_MEMORY);
	return NULL;
}

/* NULL on success, as the DDK has it: the handle back says it was not destroyed. */
PVOID NTAPI RtlDestroyHeap(PVOID handle)
{
	heap *owner = heap_of(handle);
	if (!owner || handle == PEB_PROCESS_HEAP)
		return handle;
	unregister_heap(handle);
	owner->magic = 0;
	while (owner->large) {
		large_block *item = owner->large;
		owner->large = item->next;
		PVOID base = item;
		SIZE_T none = 0;
		ZwFreeVirtualMemory(CURRENT_PROCESS, &base, &none, MEM_RELEASE);
	}
	/* The first segment lives inside the heap's own mapping, which goes last. */
	segment *first = (segment *)(owner + 1);
	for (segment *item = owner->segments; item;) {
		segment *next = item->next;
		if (item != first) {
			PVOID base = item;
			SIZE_T none = 0;
			ZwFreeVirtualMemory(CURRENT_PROCESS, &base, &none, MEM_RELEASE);
		}
		item = next;
	}
	if (first->owned) {
		PVOID base = owner;
		SIZE_T none = 0;
		ZwFreeVirtualMemory(CURRENT_PROCESS, &base, &none, MEM_RELEASE);
	}
	return NULL;
}

NTSTATUS NTAPI RavenInitializeRTL(PVOID *heaps, ULONG capacity)
{
	if (initialized || !heaps || !capacity || capacity > 512 || PEB_PROCESS_HEAPS != heaps || PEB_MAXIMUM_HEAPS != capacity ||
	    PEB_NUMBER_OF_HEAPS || PEB_PROCESS_HEAP || PEB_NT_GLOBAL_FLAG)
		return STATUS_INVALID_PARAMETER;
	initialized = TRUE;
	return STATUS_SUCCESS;
}
