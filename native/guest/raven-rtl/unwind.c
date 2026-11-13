/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "raven_rtl.h"

/* RtlVirtualUnwind, run in the guest: it moves a context from a function's frame to its caller's by the function's unwind data, as
 * the x64 exception handling of Windows documents it, and reads nothing but the program's own memory. A runtime that walks the
 * stacks of all its threads for each collection, as .NET does, calls it thousands of times a second. */

#define UNW_FLAG_CHAININFO 4
#define UWOP_PUSH_NONVOL 0
#define UWOP_ALLOC_LARGE 1
#define UWOP_ALLOC_SMALL 2
#define UWOP_SET_FPREG 3
#define UWOP_SAVE_NONVOL 4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_EPILOG 6
#define UWOP_SPARE 7
#define UWOP_SAVE_XMM128 8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME 10

struct function_entry {
	ULONG begin, end, unwind;
};

/* KNONVOLATILE_CONTEXT_POINTERS: where XMM0 to XMM15, then RAX to R15, were saved. */
struct saved_places {
	PVOID xmm[16];
	PVOID integer[16];
};

static ULONG64 *integer_register(CONTEXT *context, unsigned number)
{
	return &context->Rax + number;
}

static void pop_register(CONTEXT *context, unsigned number, struct saved_places *places)
{
	*integer_register(context, number) = *(ULONG64 *)context->Rsp;
	if (places)
		places->integer[number] = (PVOID)context->Rsp;
	context->Rsp += 8;
}

static void leave_leaf(CONTEXT *context)
{
	context->Rip = *(ULONG64 *)context->Rsp;
	context->Rsp += 8;
}

/* The slots an unwind code takes with its operands. */
static unsigned code_slots(UCHAR operation, UCHAR information)
{
	switch (operation) {
	case UWOP_ALLOC_LARGE:
		return information ? 3 : 2;
	case UWOP_SAVE_NONVOL:
	case UWOP_EPILOG:
	case UWOP_SAVE_XMM128:
		return 2;
	case UWOP_SAVE_NONVOL_FAR:
	case UWOP_SPARE:
	case UWOP_SAVE_XMM128_FAR:
		return 3;
	default:
		return 1;
	}
}

/* The rest of an epilogue when the address is inside one: an add or lea to the stack pointer, pops, then a return or a tail jump. */
static BOOL leave_epilogue(const UCHAR *code, CONTEXT *context, struct saved_places *places)
{
	struct {
		enum { STEP_ADJUST, STEP_LOAD, STEP_POP } kind;
		LONG64 value;
		unsigned number;
	} steps[12];
	unsigned count = 0, at = 0, index;
	UCHAR last;
	BOOL tail, plain;
	if (code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xc4) {
		steps[count].kind = STEP_ADJUST;
		steps[count++].value = (signed char)code[3];
		at = 4;
	} else if (code[0] == 0x48 && code[1] == 0x81 && code[2] == 0xc4) {
		steps[count].kind = STEP_ADJUST;
		steps[count++].value = *(const LONG *)(code + 3);
		at = 7;
	}
	if (!at && (code[0] == 0x48 || code[0] == 0x4c) && code[1] == 0x8d && ((code[2] & 0xc0) == 0x40 || (code[2] & 0xc0) == 0x80) &&
		((code[2] >> 3) & 7) == 4) {
		const BOOL wide = (code[2] & 0xc0) == 0x80;
		steps[count].kind = STEP_LOAD;
		steps[count].number = (code[2] & 7) | ((code[0] & 1) ? 8 : 0);
		steps[count++].value = wide ? *(const LONG *)(code + 3) : (signed char)code[3];
		at = wide ? 7 : 4;
	}
	for (;;) {
		if (at >= 15 || count >= 11)
			return FALSE;
		if (code[at] == 0x41 && (code[at + 1] & 0xf8) == 0x58) {
			steps[count].kind = STEP_POP;
			steps[count++].number = 8 + (code[at + 1] & 7);
			at += 2;
		} else if ((code[at] & 0xf8) == 0x58) {
			steps[count].kind = STEP_POP;
			steps[count++].number = code[at] & 7;
			++at;
		} else {
			break;
		}
	}
	last = code[at];
	tail = last == 0xe9 || last == 0xeb || (last == 0x48 && at + 2 < 16 && code[at + 1] == 0xff && (code[at + 2] & 0xf8) == 0xe0) ||
		(last == 0xff && at + 1 < 16 && (code[at + 1] & 0xf8) == 0x20);
	plain = last == 0xc3 || (last == 0xf3 && at + 1 < 16 && code[at + 1] == 0xc3);
	/* An instruction stream that merely starts like an epilogue is one only when it reaches a return. */
	if ((!plain && !tail) || (!count && !plain))
		return FALSE;
	for (index = 0; index < count; ++index) {
		if (steps[index].kind == STEP_ADJUST)
			context->Rsp += (ULONG64)steps[index].value;
		else if (steps[index].kind == STEP_LOAD)
			context->Rsp = *integer_register(context, steps[index].number) + (ULONG64)steps[index].value;
		else
			pop_register(context, steps[index].number, places);
	}
	leave_leaf(context);
	return TRUE;
}

PVOID NTAPI RavenVirtualUnwind(ULONG handler_type, ULONG64 base, ULONG64 pc, struct function_entry *function, CONTEXT *context, PVOID *data,
	ULONG64 *establisher, struct saved_places *places)
{
	const UCHAR *first = (const UCHAR *)(base + function->unwind), *info = first;
	const unsigned frame_register = first[3] & 15, frame_offset = (first[3] >> 4) * 16u;
	const ULONG64 prolog_offset = pc - (base + function->begin);
	const BOOL in_prolog = prolog_offset < first[1];
	ULONG64 limit = in_prolog ? prolog_offset : ~(ULONG64)0, set_frame_at = ~(ULONG64)0, frame_base;
	PVOID handler = NULL;
	BOOL machine_frame = FALSE;
	unsigned slot, depth;
	if (data)
		*data = NULL;
	if (!in_prolog) {
		const ULONG64 frame = frame_register ? *integer_register(context, frame_register) - frame_offset : context->Rsp;
		if (leave_epilogue((const UCHAR *)pc, context, places)) {
			if (establisher)
				*establisher = frame;
			return NULL;
		}
	}
	/* Registers saved with a move are found from the frame base: the frame register less its offset once the prolog has set it, as a
	   chained part always has, the stack pointer before that. */
	for (slot = 0; slot < first[2];) {
		const UCHAR operation = first[4 + slot * 2 + 1] & 15, information = first[4 + slot * 2 + 1] >> 4;
		if (operation == UWOP_SET_FPREG) {
			set_frame_at = first[4 + slot * 2];
			break;
		}
		slot += code_slots(operation, information);
	}
	frame_base = frame_register && (((first[0] >> 3) & UNW_FLAG_CHAININFO) || !in_prolog || prolog_offset >= set_frame_at) ?
		*integer_register(context, frame_register) - frame_offset : context->Rsp;
	for (depth = 0; depth < 32; ++depth) {
		const UCHAR flags = info[0] >> 3, count = info[2];
		const unsigned padded = (count + 1u) & ~1u, part_register = info[3] & 15, part_offset = (info[3] >> 4) * 16u;
		const UCHAR *codes = info + 4, *tail = codes + padded * 2;
		for (slot = 0; slot < count;) {
			const UCHAR offset = codes[slot * 2], operation = codes[slot * 2 + 1] & 15, information = codes[slot * 2 + 1] >> 4;
			const USHORT *operands = (const USHORT *)(codes + (slot + 1) * 2);
			if (offset <= limit) {
				switch (operation) {
				case UWOP_PUSH_NONVOL:
					pop_register(context, information, places);
					break;
				case UWOP_ALLOC_LARGE:
					context->Rsp += information ? *(const ULONG *)operands : (ULONG64)operands[0] * 8;
					break;
				case UWOP_ALLOC_SMALL:
					context->Rsp += information * 8 + 8;
					break;
				case UWOP_SET_FPREG:
					context->Rsp = *integer_register(context, part_register) - part_offset;
					break;
				case UWOP_SAVE_NONVOL:
				case UWOP_SAVE_NONVOL_FAR: {
					const ULONG64 place = frame_base + (operation == UWOP_SAVE_NONVOL ? (ULONG64)operands[0] * 8 : *(const ULONG *)operands);
					*integer_register(context, information) = *(ULONG64 *)place;
					if (places)
						places->integer[information] = (PVOID)place;
					break;
				}
				case UWOP_SAVE_XMM128:
				case UWOP_SAVE_XMM128_FAR: {
					const ULONG64 place = frame_base + (operation == UWOP_SAVE_XMM128 ? (ULONG64)operands[0] * 16 : *(const ULONG *)operands);
					(&context->Xmm0)[information] = *(M128A *)place;
					if (places)
						places->xmm[information] = (PVOID)place;
					break;
				}
				case UWOP_PUSH_MACHFRAME: {
					/* A frame the processor pushed for an interrupt: the return address and the stack pointer are in it. */
					const ULONG64 frame = context->Rsp + (information ? 8 : 0);
					context->Rip = *(ULONG64 *)frame;
					context->Rsp = *(ULONG64 *)(frame + 24);
					machine_frame = TRUE;
					break;
				}
				default:
					break;
				}
			}
			slot += code_slots(operation, information);
		}
		if (flags & UNW_FLAG_CHAININFO) {
			/* Chained unwind data goes on in the parent function's record, whose prolog has always run. An entry with the low bit of
			   its last field set stands for another entry. */
			const struct function_entry *parent = (const struct function_entry *)tail;
			unsigned hops;
			for (hops = 0; hops < 4 && (parent->unwind & 1); ++hops)
				parent = (const struct function_entry *)(base + (parent->unwind & ~1u));
			info = (const UCHAR *)(base + parent->unwind);
			limit = ~(ULONG64)0;
			continue;
		}
		if (flags & handler_type) {
			handler = (PVOID)(base + *(const ULONG *)tail);
			if (data)
				*data = (PVOID)(tail + 4);
		}
		break;
	}
	if (establisher)
		*establisher = frame_base;
	if (!machine_frame)
		leave_leaf(context);
	return handler;
}

/* On this processor there is no instruction cache to flush: the processor sees what was written. */
BOOL WINAPI RavenFlushInstructionCache(HANDLE process, const VOID *address, SIZE_T size)
{
	(void)process;
	(void)address;
	(void)size;
	return TRUE;
}
