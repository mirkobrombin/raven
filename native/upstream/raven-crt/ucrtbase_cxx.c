/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* What programs built against the system msvcrt.dll import for C++: exit handlers kept by a library, and, for 32-bit programs, the
 * exceptions of the Microsoft C++ compiler. A throw is dispatched here, in the program's own process, along the chain of exception
 * frames the thread registered: each frame's handler is asked in turn, and the C++ frame handler finds the catch block that takes the
 * object, unwinds the frames above it, runs the block and resumes the function after it. Included by ucrtbase.c. */

/* The handlers' type is the headers' _onexit_t, which returns an int nobody reads. */
typedef int (__cdecl *exit_function)(void);

CRT exit_function _onexit(exit_function function) { return _crt_atexit((void (__cdecl *)(void))function) ? NULL : function; }

/* A library's own table of exit handlers, from *begin to *end, grows by one. */
CRT exit_function __dllonexit(exit_function function, exit_function **begin, exit_function **end)
{
	if (!function || !begin || !end)
		return NULL;
	const size_t count = *begin ? (size_t)(*end - *begin) : 0;
	exit_function *table = realloc(*begin, (count + 1) * sizeof *table);
	if (!table)
		return NULL;
	table[count] = function;
	*begin = table;
	*end = table + count + 1;
	return function;
}

/* Member functions take the object in ECX on 32 bits; on 64 bits they are ordinary functions. */
#ifdef __i386__
#define THISCALL __attribute__((thiscall))
#else
#define THISCALL
#endif

#define CXX_EXCEPTION 0xe06d7363
#define CXX_MAGIC_FIRST 0x19930520
#define CXX_MAGIC_LAST 0x19930522
#define FUNCTION_SYNCHRONOUS 1
#define CATCH_CONST 1
#define CATCH_VOLATILE 2
#define CATCH_REFERENCE 8
#define TYPE_SIMPLE 1
#define TYPE_VIRTUAL_BASE 4
#define UNWINDING (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)

/* A type's descriptor, and where a base class lies in an object of a derived one, as both widths lay them out. */
typedef struct { const void *vtable; char *name; char mangled[1]; } cxx_type;
typedef struct { int this_offset, base_offset, base_entry; } cxx_offsets;

static void *adjusted(const cxx_offsets *offsets, void *object)
{
	char *at = object;
	if (!at)
		return NULL;
	if (offsets->base_offset >= 0) {
		at += offsets->base_offset;
		at += *(int *)(*(char **)at + offsets->base_entry);
	}
	return at + offsets->this_offset;
}

#ifdef __i386__

/* The tables the compiler emits, as 32-bit programs lay them out. */
typedef struct { unsigned flags; const cxx_type *type; cxx_offsets offsets; unsigned size; void *copy; } cxx_catchable;
typedef struct { int count; const cxx_catchable *types[1]; } cxx_catchables;
typedef struct { unsigned flags; void *destructor; void *forward; const cxx_catchables *catchable; } cxx_throw_info;
typedef struct { int previous; void *handler; } cxx_unwind;
typedef struct { unsigned flags; const cxx_type *type; int offset; void *handler; } cxx_catch;
typedef struct { int start, end, catch_level, count; const cxx_catch *catches; } cxx_try;
typedef struct { unsigned magic, unwind_count; const cxx_unwind *unwind; unsigned try_count; const cxx_try *tries; unsigned ip_count;
	const void *ip_map; const void *expected; unsigned flags; } cxx_function;

/* The exception frame a C++ function registers: the link and handler, its current state, and the function's frame pointer, which is
 * the address of the saved one. The stack pointer the function keeps after its prologue sits just below. */
typedef struct cxx_registration { struct cxx_registration *next; void *handler; } cxx_registration;
typedef struct { cxx_registration link; int state; unsigned ebp; } cxx_frame;
typedef int (__cdecl *frame_handler)(EXCEPTION_RECORD *, void *, CONTEXT *, void *);

static cxx_registration *frame_list(void)
{
	cxx_registration *head;
	__asm__ volatile ("movl %%fs:0, %0" : "=r"(head));
	return head;
}

static void set_frame_list(cxx_registration *head)
{
	__asm__ volatile ("movl %0, %%fs:0" : : "r"(head) : "memory");
}

/* Runs a part of a function, a catch block or an unwind action, with the function's frame pointer; returns what the part returns. */
__asm__(".text\n"
	"_raven_call_with_frame:\n\t"
	"pushl %ebp\n\tpushl %ebx\n\tpushl %esi\n\tpushl %edi\n\t"
	"movl 24(%esp), %ebp\n\t"
	"call *20(%esp)\n\t"
	"popl %edi\n\tpopl %esi\n\tpopl %ebx\n\tpopl %ebp\n\t"
	"ret\n");
void *raven_call_with_frame(void *code, void *frame_pointer);

/* Leaves the catch block for good: the function's stack and frame pointers come back and it goes on at the address the block gave. */
__asm__(".text\n"
	"_raven_resume_function:\n\t"
	"movl 4(%esp), %edx\n\t"
	"movl 8(%esp), %eax\n\t"
	"movl -4(%edx), %esp\n\t"
	"leal 12(%edx), %ebp\n\t"
	"jmp *%eax\n");
void raven_resume_function(cxx_frame *frame, void *address) __attribute__((noreturn));

/* The catch blocks running on each thread, innermost first: the frame each belongs to, the stack pointer its function had before any
 * of its catch blocks ran, and the exception, which a rethrow sends on. */
typedef struct cxx_active {
	cxx_frame *frame;
	void *stack;
	EXCEPTION_RECORD *record;
	struct cxx_active *outer;
} cxx_active;
static DWORD current_slot = TLS_OUT_OF_INDEXES;

static cxx_active *current_catch(void)
{
	return current_slot == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue(current_slot);
}

static EXCEPTION_RECORD *current_exception(void)
{
	const cxx_active *active = current_catch();
	return active ? active->record : NULL;
}

static void set_current_catch(cxx_active *active)
{
	if (current_slot == TLS_OUT_OF_INDEXES) {
		DWORD slot = TlsAlloc();
		if (InterlockedCompareExchange((LONG *)&current_slot, (LONG)slot, (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES)
			TlsFree(slot);
	}
	TlsSetValue(current_slot, active);
}

/* Calls the unwind actions of a function from its current state back to the state asked for, destroying what was built since. */
static void unwind_function(cxx_frame *frame, const cxx_function *function, int target)
{
	int state = frame->state;
	while (state > target && state >= 0 && (unsigned)state < function->unwind_count) {
		const cxx_unwind *entry = &function->unwind[state];
		frame->state = entry->previous;
		if (entry->handler)
			raven_call_with_frame(entry->handler, &frame->ebp);
		state = entry->previous;
	}
	frame->state = target;
}

/* The dispatcher's own frame says that an exception raised while a handler runs is a nested one. */
static int __cdecl nested_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context, void *dispatcher)
{
	return record->ExceptionFlags & UNWINDING ? ExceptionContinueSearch : ExceptionNestedException;
}

/* Calls a frame's handler as Windows does: with a frame of the dispatcher's own above it, which the C++ runtime's unwinding takes off
 * and links again, and which leaves the chain when the handler returns. */
static int call_handler(cxx_registration *frame, EXCEPTION_RECORD *record, CONTEXT *context, void *dispatcher)
{
	cxx_registration own = {frame_list(), (void *)nested_handler};
	set_frame_list(&own);
	const int disposition = ((frame_handler)frame->handler)(record, frame, context, dispatcher);
	set_frame_list(own.next);
	return disposition;
}

/* Every frame registered above the target hears that it is being unwound and leaves the chain. */
static void unwind_frames(cxx_registration *target, EXCEPTION_RECORD *record)
{
	CONTEXT context = {0};
	void *dispatcher = NULL;
	record->ExceptionFlags |= EXCEPTION_UNWINDING;
	for (cxx_registration *frame = frame_list(); frame && frame != target && frame != (cxx_registration *)-1;) {
		call_handler(frame, record, &context, &dispatcher);
		cxx_registration *next = frame->next;
		set_frame_list(next);
		frame = next;
	}
	record->ExceptionFlags &= ~EXCEPTION_UNWINDING;
}

/* The type of the thrown object that a catch block takes, by name as the compiler decorates it. */
static const cxx_catchable *caught_type(const cxx_throw_info *info, const cxx_catch *block)
{
	for (int index = 0; index < info->catchable->count; ++index) {
		const cxx_catchable *type = info->catchable->types[index];
		if (block->type != type->type && strcmp(block->type->mangled, type->type->mangled))
			continue;
		if (((info->flags & CATCH_CONST) && !(block->flags & CATCH_CONST)) || ((info->flags & CATCH_VOLATILE) && !(block->flags & CATCH_VOLATILE)))
			continue;
		return type;
	}
	return NULL;
}

/* The catch block's own copy of the object, or a reference to it, where the block finds it in the function's frame. */
static void copy_object(void *object, cxx_frame *frame, const cxx_catch *block, const cxx_catchable *type)
{
	char *to = (char *)&frame->ebp + block->offset;
	if (block->flags & CATCH_REFERENCE) {
		*(void **)to = adjusted(&type->offsets, object);
	} else if (type->flags & TYPE_SIMPLE) {
		memmove(to, object, type->size);
		if (type->size == sizeof(void *))
			*(void **)to = adjusted(&type->offsets, *(void **)to);
	} else if (type->copy) {
		if (type->flags & TYPE_VIRTUAL_BASE)
			((void (THISCALL *)(void *, void *, int))type->copy)(to, adjusted(&type->offsets, object), 1);
		else
			((void (THISCALL *)(void *, void *))type->copy)(to, adjusted(&type->offsets, object));
	} else {
		memmove(to, adjusted(&type->offsets, object), type->size);
	}
}

/* A catch block takes the exception: the frames above are unwound, then the function's own actions back to the try block, the block
 * runs with the exception current, the object is destroyed and the function goes on after the block. */
static void run_catch(EXCEPTION_RECORD *record, cxx_frame *frame, const cxx_function *function, const cxx_try *try_block,
	const cxx_catch *block, const cxx_catchable *type)
{
	const cxx_throw_info *info = type ? (const cxx_throw_info *)record->ExceptionInformation[2] : NULL;
	void *object = type ? (void *)record->ExceptionInformation[1] : NULL;
	if (type && block->offset && block->type && block->type->mangled[0])
		copy_object(object, frame, block, type);
	unwind_frames(&frame->link, record);
	unwind_function(frame, function, try_block->start);
	frame->state = try_block->end + 1;
	/* Catch blocks of this frame and of the frames below it, which the unwind abandoned, are over. The block may keep its own stack
	 * pointer where the function keeps its own: the function's, from before its first catch block, comes back before it goes on. */
	cxx_active *outer = current_catch();
	void *stack = ((void **)frame)[-1];
	for (; outer && (char *)outer->frame <= (char *)frame; outer = outer->outer)
		if (outer->frame == frame)
			stack = outer->stack;
	cxx_active active = {frame, stack, record, outer};
	set_current_catch(&active);
	void *resume = raven_call_with_frame(block->handler, &frame->ebp);
	((void **)frame)[-1] = stack;
	set_current_catch(outer);
	if (info && info->destructor && object)
		((void (THISCALL *)(void *))info->destructor)(object);
	raven_resume_function(frame, resume);
}

__attribute__((used)) int raven_cxx_frame_search(EXCEPTION_RECORD *record, cxx_frame *frame, CONTEXT *context, void *dispatcher, const cxx_function *function)
{
	const unsigned magic = function->magic & 0x1fffffff;
	if (magic < CXX_MAGIC_FIRST || magic > CXX_MAGIC_LAST)
		return ExceptionContinueSearch;
	if (record->ExceptionFlags & UNWINDING) {
		if (function->unwind_count)
			unwind_function(frame, function, -1);
		return ExceptionContinueSearch;
	}
	const BOOL cxx = record->ExceptionCode == CXX_EXCEPTION && record->NumberParameters >= 3 &&
		record->ExceptionInformation[0] >= CXX_MAGIC_FIRST && record->ExceptionInformation[0] <= CXX_MAGIC_LAST;
	/* Functions compiled for synchronous exceptions only catch what C++ throws. */
	if (!cxx && magic >= CXX_MAGIC_LAST && (function->flags & FUNCTION_SYNCHRONOUS))
		return ExceptionContinueSearch;
	const cxx_throw_info *info = cxx ? (const cxx_throw_info *)record->ExceptionInformation[2] : NULL;
	for (unsigned index = 0; index < function->try_count; ++index) {
		const cxx_try *try_block = &function->tries[index];
		if (frame->state < try_block->start || frame->state > try_block->end)
			continue;
		for (int choice = 0; choice < try_block->count; ++choice) {
			const cxx_catch *block = &try_block->catches[choice];
			const BOOL any = !block->type || !block->type->mangled[0];
			const cxx_catchable *type = NULL;
			if (!any) {
				if (!info || !info->catchable || !(type = caught_type(info, block)))
					continue;
			}
			run_catch(record, frame, function, try_block, block, info ? (type ? type : info->catchable->types[0]) : NULL);
		}
	}
	return ExceptionContinueSearch;
}

/* The handler compiled functions register takes its function's tables in EAX. */
__asm__(".text\n"
	".globl _raven_cxx_frame_handler\n"
	"_raven_cxx_frame_handler:\n\t"
	"pushl %eax\n\t"
	"pushl 20(%esp)\n\t"
	"pushl 20(%esp)\n\t"
	"pushl 20(%esp)\n\t"
	"pushl 20(%esp)\n\t"
	"call _raven_cxx_frame_search\n\t"
	"addl $20, %esp\n\t"
	"ret\n");

/* Ends the process for an exception no frame took, as Windows does after the last chance. */
static void unhandled(EXCEPTION_RECORD *record, CONTEXT *context)
{
	EXCEPTION_POINTERS pointers = {record, context};
	UnhandledExceptionFilter(&pointers);
	ExitProcess(record->ExceptionCode);
}


/* Vectored handlers: AddVectoredExceptionHandler and AddVectoredContinueHandler put a handler first or last on their list, and the
 * cookie that removes it is its entry. Exception handlers hear of an exception before any frame of the stack; continue handlers when
 * execution is about to go on. */
typedef struct vectored {
	struct vectored *next;
	PVECTORED_EXCEPTION_HANDLER handler;
	BOOL after;
} vectored;
static vectored *volatile vectored_handlers;
static volatile LONG vectored_lock;

static void lock_vectored(void)
{
	while (InterlockedExchange(&vectored_lock, 1))
		Sleep(0);
}

static void unlock_vectored(void)
{
	InterlockedExchange(&vectored_lock, 0);
}

static void *add_vectored(ULONG first, PVECTORED_EXCEPTION_HANDLER handler, BOOL after)
{
	vectored *entry = HeapAlloc(GetProcessHeap(), 0, sizeof *entry);
	if (!entry)
		return NULL;
	entry->handler = handler;
	entry->after = after;
	entry->next = NULL;
	lock_vectored();
	vectored *volatile *at = &vectored_handlers;
	if (!first)
		while (*at)
			at = &(*at)->next;
	entry->next = *at;
	*at = entry;
	unlock_vectored();
	return entry;
}

void *__stdcall raven_add_vectored_exception(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)
{
	return add_vectored(first, handler, FALSE);
}

void *__stdcall raven_add_vectored_continue(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)
{
	return add_vectored(first, handler, TRUE);
}

ULONG __stdcall raven_remove_vectored(void *cookie)
{
	lock_vectored();
	for (vectored *volatile *at = &vectored_handlers; *at; at = &(*at)->next)
		if (*at == cookie) {
			vectored *entry = *at;
			*at = entry->next;
			unlock_vectored();
			HeapFree(GetProcessHeap(), 0, entry);
			return 1;
		}
	unlock_vectored();
	return 0;
}

/* Runs the handlers of one list in order; TRUE when one of them continues execution. The list may change while a handler runs, so each
 * step takes the next entry under the lock. */
static BOOL call_vectored(EXCEPTION_RECORD *record, CONTEXT *context, BOOL after)
{
	EXCEPTION_POINTERS pointers = {record, context};
	lock_vectored();
	vectored *entry = vectored_handlers;
	while (entry) {
		if (entry->after != after) {
			entry = entry->next;
			continue;
		}
		PVECTORED_EXCEPTION_HANDLER handler = entry->handler;
		unlock_vectored();
		if (handler(&pointers) == EXCEPTION_CONTINUE_EXECUTION)
			return TRUE;
		lock_vectored();
		vectored *next = vectored_handlers;
		while (next && next != entry)
			next = next->next;
		entry = next ? next->next : NULL;
	}
	unlock_vectored();
	return FALSE;
}

/* Raises the exception: the vectored handlers first, then along the thread's frames; a frame that takes it never returns here, and a
 * handler that continues execution of a continuable exception returns to the caller after the continue handlers ran. */
static void dispatch(EXCEPTION_RECORD *record, CONTEXT *context)
{
	void *dispatcher = NULL;
	const BOOL continuable = !(record->ExceptionFlags & EXCEPTION_NONCONTINUABLE);
	if (call_vectored(record, context, FALSE) && continuable) {
		call_vectored(record, context, TRUE);
		return;
	}
	for (cxx_registration *frame = frame_list(); frame && frame != (cxx_registration *)-1; frame = frame->next) {
		const int disposition = call_handler(frame, record, context, &dispatcher);
		if (disposition == ExceptionContinueExecution && continuable) {
			call_vectored(record, context, TRUE);
			return;
		}
	}
	unhandled(record, context);
}

/* The caller's control registers, from this function's frame: its return address, frame pointer and stack after the arguments. */
#define CALLER_CONTEXT(context, arguments) do { \
		(context).ContextFlags = CONTEXT_CONTROL; \
		(context).Eip = (DWORD)__builtin_return_address(0); \
		(context).Ebp = *(DWORD *)__builtin_frame_address(0); \
		(context).Esp = (DWORD)__builtin_frame_address(0) + 8 + (arguments); \
	} while (0)

/* RaiseException for 32-bit programs: the runtime sends the program's call here, with its arguments, so the handlers of the frames run
 * in the program as they do on Windows. */
void __stdcall raven_raise_exception(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments)
{
	EXCEPTION_RECORD record = {0};
	CONTEXT context = {0};
	CALLER_CONTEXT(context, 16);
	record.ExceptionCode = code;
	record.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
	record.ExceptionAddress = __builtin_return_address(0);
	record.NumberParameters = count > EXCEPTION_MAXIMUM_PARAMETERS ? EXCEPTION_MAXIMUM_PARAMETERS : count;
	for (DWORD index = 0; arguments && index < record.NumberParameters; ++index)
		record.ExceptionInformation[index] = arguments[index];
	dispatch(&record, &context);
}

/* RtlUnwind for 32-bit programs: the frames above the target hear that they are being unwound and leave the chain. The compiled code
 * that calls it continues right after the call, which is where it returns. */
void __stdcall raven_unwind(cxx_registration *target, void *target_ip, EXCEPTION_RECORD *record, void *value)
{
	EXCEPTION_RECORD local = {0};
	if (!record) {
		local.ExceptionCode = 0xc0000027; /* STATUS_UNWIND */
		local.ExceptionAddress = __builtin_return_address(0);
		record = &local;
	}
	if (!target)
		record->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;
	unwind_frames(target, record);
}

/* throw: the object and its type's description; with neither, the exception the current catch block handles goes on. */
void __stdcall raven_cxx_throw(void *object, const cxx_throw_info *info)
{
	EXCEPTION_RECORD record = {0};
	record.ExceptionCode = CXX_EXCEPTION;
	record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
	record.ExceptionAddress = __builtin_return_address(0);
	record.NumberParameters = 3;
	record.ExceptionInformation[0] = CXX_MAGIC_FIRST;
	record.ExceptionInformation[1] = (ULONG_PTR)object;
	record.ExceptionInformation[2] = (ULONG_PTR)info;
	if (!object && !info) {
		const EXCEPTION_RECORD *current = current_exception();
		if (!current)
			abort();
		record.ExceptionInformation[1] = current->ExceptionInformation[1];
		record.ExceptionInformation[2] = current->ExceptionInformation[2];
	}
	CONTEXT context = {0};
	CALLER_CONTEXT(context, 8);
	dispatch(&record, &context);
	abort();
}

#endif

#ifdef __x86_64__

/* The tables the compiler emits for 64-bit programs: the same tables with 32-bit offsets from the image's base in place of pointers. */
typedef struct { unsigned flags, type; cxx_offsets offsets; unsigned size, copy; } cxx_catchable;
typedef struct { int count; unsigned types[1]; } cxx_catchables;
typedef struct { unsigned flags, destructor, forward, catchable; } cxx_throw_info;
typedef struct { int previous; unsigned action; } cxx_unwind;
typedef struct { unsigned flags, type; int offset; unsigned handler, frame; } cxx_catch;
typedef struct { int start, end, catch_level, count; unsigned catches; } cxx_try;
typedef struct { unsigned ip; int state; } cxx_ip_state;
typedef struct { unsigned magic; int unwind_count; unsigned unwind; unsigned try_count; unsigned tries; unsigned ip_count; unsigned ip_map;
	int unwind_help; unsigned expected; unsigned flags; } cxx_function;

#define EXCEPTION_TARGET_UNWIND 0x20
#define STATUS_UNWIND_CONSOLIDATE 0x80000029

/* What a catch is entered with, in the parameters of the consolidating unwind: the callback, the function's frame, the catch block, the
 * state the function is unwound to, and the exception: its object, its type's description and the base of the image that describes it. */
enum { CATCH_CALLBACK, CATCH_FRAME, CATCH_BLOCK, CATCH_STATE, CATCH_OBJECT, CATCH_INFO, CATCH_BASE, CATCH_PARAMETERS };

/* The catch blocks running on each thread, innermost first: the frame of the function each belongs to, the state that function was
 * unwound to, and the exception, which a rethrow sends on. While its catch block runs, the function's frame is reached only through the
 * block, and it is in that state whatever address it was left at. */
typedef struct cxx_active {
	ULONG64 frame;
	int state;
	void *object;
	const cxx_throw_info *info;
	ULONG_PTR base;
	struct cxx_active *outer;
} cxx_active;
static DWORD current_slot = TLS_OUT_OF_INDEXES;

static cxx_active *current_exception(void)
{
	return current_slot == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue(current_slot);
}

static void set_current_exception(cxx_active *exception)
{
	if (current_slot == TLS_OUT_OF_INDEXES) {
		DWORD slot = TlsAlloc();
		if (InterlockedCompareExchange((LONG *)&current_slot, (LONG)slot, (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES)
			TlsFree(slot);
	}
	TlsSetValue(current_slot, exception);
}

static const cxx_active *active_catch(ULONG64 frame)
{
	for (const cxx_active *item = current_exception(); item; item = item->outer)
		if (item->frame == frame)
			return item;
	return NULL;
}

/* A part of a function the compiler moved out of it, an unwind action or a catch block, runs with the function's frame in RDX. */
typedef void *(*funclet)(ULONG64, ULONG64);

/* The state of a function at an address of its code: the last entry of its map at or before the address. */
static int state_at(const cxx_function *function, ULONG64 base, ULONG64 pc)
{
	const cxx_ip_state *map = (const cxx_ip_state *)(base + function->ip_map);
	int state = -1;
	for (unsigned index = 0; index < function->ip_count && base + map[index].ip <= pc; ++index)
		state = map[index].state;
	return state;
}

/* Calls the unwind actions of a function from its current state back to the state asked for, destroying what was built since. */
static void unwind_function(ULONG64 frame, ULONG64 base, const cxx_function *function, int state, int target)
{
	const cxx_unwind *map = (const cxx_unwind *)(base + function->unwind);
	while (state > target && state >= 0 && state < function->unwind_count) {
		const cxx_unwind *entry = &map[state];
		state = entry->previous;
		if (entry->action)
			((funclet)(base + entry->action))(0, frame);
	}
}

/* The type of the thrown object that a catch block takes, by name as the compiler decorates it. */
static const cxx_catchable *caught_type(const cxx_throw_info *info, ULONG64 thrown_base, const cxx_type *wanted, unsigned catch_flags)
{
	const cxx_catchables *types = (const cxx_catchables *)(thrown_base + info->catchable);
	for (int index = 0; index < types->count; ++index) {
		const cxx_catchable *type = (const cxx_catchable *)(thrown_base + types->types[index]);
		const cxx_type *described = (const cxx_type *)(thrown_base + type->type);
		if (wanted != described && strcmp(wanted->mangled, described->mangled))
			continue;
		if (((info->flags & CATCH_CONST) && !(catch_flags & CATCH_CONST)) || ((info->flags & CATCH_VOLATILE) && !(catch_flags & CATCH_VOLATILE)))
			continue;
		return type;
	}
	return NULL;
}

/* The catch block's own copy of the object, or a reference to it, where the block finds it in the function's frame. */
static void copy_object(void *object, ULONG64 frame, const cxx_catch *block, const cxx_catchable *type, ULONG64 thrown_base)
{
	char *to = (char *)(frame + block->offset);
	if (block->flags & CATCH_REFERENCE) {
		*(void **)to = adjusted(&type->offsets, object);
	} else if (type->flags & TYPE_SIMPLE) {
		memmove(to, object, type->size);
		if (type->size == sizeof(void *))
			*(void **)to = adjusted(&type->offsets, *(void **)to);
	} else if (type->copy) {
		if (type->flags & TYPE_VIRTUAL_BASE)
			((void (*)(void *, void *, int))(thrown_base + type->copy))(to, adjusted(&type->offsets, object), 1);
		else
			((void (*)(void *, void *))(thrown_base + type->copy))(to, adjusted(&type->offsets, object));
	} else {
		memmove(to, adjusted(&type->offsets, object), type->size);
	}
}

/* Runs on the catching function's stack once the frames above it are gone: the block runs with the exception current, then the object
 * is destroyed and the function goes on where the block says. */
static ULONG64 run_catch(EXCEPTION_RECORD *record)
{
	const ULONG_PTR *parameters = record->ExceptionInformation;
	/* Catch blocks of this frame and of the frames below it, which the unwind abandoned, are over. */
	cxx_active *outer = current_exception();
	while (outer && outer->frame <= parameters[CATCH_FRAME])
		outer = outer->outer;
	cxx_active exception = {parameters[CATCH_FRAME], (int)parameters[CATCH_STATE], (void *)parameters[CATCH_OBJECT],
		(const cxx_throw_info *)parameters[CATCH_INFO], parameters[CATCH_BASE], outer};
	set_current_exception(&exception);
	void *resume = ((funclet)parameters[CATCH_BLOCK])(0, parameters[CATCH_FRAME]);
	set_current_exception(exception.outer);
	if (exception.info && exception.info->destructor && exception.object)
		((void (*)(void *))(exception.base + exception.info->destructor))(exception.object);
	return (ULONG64)resume;
}

EXCEPTION_DISPOSITION raven_cxx_frame_handler(EXCEPTION_RECORD *record, ULONG64 frame, CONTEXT *context, DISPATCHER_CONTEXT *dispatch)
{
	const ULONG64 base = dispatch->ImageBase;
	const cxx_function *function = (const cxx_function *)(base + *(const unsigned *)dispatch->HandlerData);
	const unsigned magic = function->magic & 0x1fffffff;
	if (magic < CXX_MAGIC_FIRST || magic > CXX_MAGIC_LAST)
		return ExceptionContinueSearch;
	int state = state_at(function, base, dispatch->ControlPc);
	const cxx_try *tries = (const cxx_try *)(base + function->tries);
	/* A catch block is a part of its function with a frame of its own, which keeps the function's frame where the block's description
	 * says; what it built is its own, and what the function built before is the function's. */
	const cxx_try *running = NULL;
	for (unsigned index = 0; index < function->try_count && !running; ++index) {
		const cxx_catch *catches = (const cxx_catch *)(base + tries[index].catches);
		for (int choice = 0; choice < tries[index].count; ++choice)
			if (catches[choice].handler == dispatch->FunctionEntry->BeginAddress) {
				frame = *(const ULONG64 *)(frame + catches[choice].frame);
				running = &tries[index];
				break;
			}
	}
	const cxx_active *active = running ? NULL : active_catch(frame);
	if (active)
		state = active->state;
	if (record->ExceptionFlags & UNWINDING) {
		/* The catching function keeps what it built before the try block; every other function loses all it built, and a catch block
		 * what it built itself. */
		int target = running ? running->end : -1;
		if (!running && (record->ExceptionFlags & EXCEPTION_TARGET_UNWIND) && record->ExceptionCode == STATUS_UNWIND_CONSOLIDATE &&
			record->NumberParameters >= CATCH_PARAMETERS && record->ExceptionInformation[CATCH_CALLBACK] == (ULONG_PTR)run_catch)
			target = (int)record->ExceptionInformation[CATCH_STATE];
		unwind_function(frame, base, function, state, target);
		return ExceptionContinueSearch;
	}
	/* The catch blocks of a function whose own catch block is running were offered to the exception from that block's frame. */
	if (active)
		return ExceptionContinueSearch;
	const BOOL cxx = record->ExceptionCode == CXX_EXCEPTION && record->NumberParameters >= 4 &&
		record->ExceptionInformation[0] >= CXX_MAGIC_FIRST && record->ExceptionInformation[0] <= CXX_MAGIC_LAST;
	/* Functions compiled for synchronous exceptions only catch what C++ throws. */
	if (!cxx && magic >= CXX_MAGIC_LAST && (function->flags & FUNCTION_SYNCHRONOUS))
		return ExceptionContinueSearch;
	void *object = cxx ? (void *)record->ExceptionInformation[1] : NULL;
	const cxx_throw_info *info = cxx ? (const cxx_throw_info *)record->ExceptionInformation[2] : NULL;
	const ULONG64 thrown_base = cxx ? record->ExceptionInformation[3] : 0;
	for (unsigned index = 0; index < function->try_count; ++index) {
		const cxx_try *try_block = &tries[index];
		if (state < try_block->start || state > try_block->end)
			continue;
		const cxx_catch *catches = (const cxx_catch *)(base + try_block->catches);
		for (int choice = 0; choice < try_block->count; ++choice) {
			const cxx_catch *block = &catches[choice];
			const cxx_type *wanted = block->type ? (const cxx_type *)(base + block->type) : NULL;
			const cxx_catchable *type = NULL;
			if (wanted && wanted->mangled[0]) {
				if (!info || !info->catchable || !(type = caught_type(info, thrown_base, wanted, block->flags)))
					continue;
				if (block->offset)
					copy_object(object, frame, block, type, thrown_base);
			}
			/* The frames above go, the function's own actions back to the try block run, and the catch block runs on its stack. */
			EXCEPTION_RECORD consolidate = {0};
			consolidate.ExceptionCode = STATUS_UNWIND_CONSOLIDATE;
			consolidate.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
			consolidate.NumberParameters = CATCH_PARAMETERS;
			consolidate.ExceptionInformation[CATCH_CALLBACK] = (ULONG_PTR)run_catch;
			consolidate.ExceptionInformation[CATCH_FRAME] = frame;
			consolidate.ExceptionInformation[CATCH_BLOCK] = base + block->handler;
			consolidate.ExceptionInformation[CATCH_STATE] = (ULONG_PTR)(LONG_PTR)try_block->start;
			consolidate.ExceptionInformation[CATCH_OBJECT] = (ULONG_PTR)object;
			consolidate.ExceptionInformation[CATCH_INFO] = (ULONG_PTR)info;
			consolidate.ExceptionInformation[CATCH_BASE] = thrown_base;
			RtlUnwindEx((void *)frame, (void *)dispatch->ControlPc, &consolidate, NULL, dispatch->ContextRecord, dispatch->HistoryTable);
		}
	}
	return ExceptionContinueSearch;
}

/* throw: the object, its type's description and the image that holds it; with neither, the exception the current catch block handles
 * goes on. */
void raven_cxx_throw(void *object, const cxx_throw_info *info)
{
	ULONG_PTR parameters[4] = {CXX_MAGIC_FIRST, (ULONG_PTR)object, (ULONG_PTR)info, 0};
	if (!object && !info) {
		const cxx_active *current = current_exception();
		if (!current)
			abort();
		parameters[1] = (ULONG_PTR)current->object;
		parameters[2] = (ULONG_PTR)current->info;
		parameters[3] = current->base;
	} else {
		void *image = NULL;
		RtlPcToFileHeader((void *)info, &image);
		parameters[3] = (ULONG_PTR)image;
	}
	RaiseException(CXX_EXCEPTION, EXCEPTION_NONCONTINUABLE, 4, parameters);
	abort();
}

/* The names 64-bit programs import these by, with the members of type_info and std::exception below. */
__asm__(".section .drectve\n"
	".ascii \" -export:_CxxThrowException=raven_cxx_throw -export:__CxxFrameHandler=raven_cxx_frame_handler\"\n"
	".ascii \" -export:__CxxFrameHandler2=raven_cxx_frame_handler -export:__CxxFrameHandler3=raven_cxx_frame_handler\"\n"
	".ascii \" -export:??1type_info@@UEAA@XZ=raven_type_info_destroy -export:?name@type_info@@QEBAPEBDXZ=raven_type_info_name\"\n"
	".ascii \" -export:?raw_name@type_info@@QEBAPEBDXZ=raven_type_info_raw_name -export:??8type_info@@QEBAHAEBV0@@Z=raven_type_info_equal\"\n"
	".ascii \" -export:??9type_info@@QEBAHAEBV0@@Z=raven_type_info_unequal -export:?before@type_info@@QEBAHAEBV0@@Z=raven_type_info_before\"\n"
	".ascii \" -export:??0exception@@QEAA@XZ=raven_exception_construct -export:??0exception@@QEAA@AEBQEBD@Z=raven_exception_construct_message\"\n"
	".ascii \" -export:??0exception@@QEAA@AEBQEBDH@Z=raven_exception_construct_message_shared -export:??0exception@@QEAA@AEBV0@@Z=raven_exception_copy\"\n"
	".ascii \" -export:??1exception@@UEAA@XZ=raven_exception_destroy -export:??4exception@@QEAAAEAV0@AEBV0@@Z=raven_exception_assign\"\n"
	".ascii \" -export:?what@exception@@UEBAPEBDXZ=raven_exception_what -export:??_Eexception@@UEAAPEAXI@Z=raven_exception_delete\"\n"
	".ascii \" -export:??_7exception@@6B@=raven_exception_vtable,DATA -export:?terminate@@YAXXZ=terminate\"\n"
	".ascii \" -export:??_7type_info@@6B@=raven_type_info_vtable,DATA\"\n"
	".text");

#endif

/* type_info: its decorated name, and the readable one made from it on demand. */
typedef struct { const void *vtable; char *name; char mangled[1]; } type_info;

void THISCALL raven_type_info_destroy(type_info *self) { free(self->name); }

/* The deleting destructor of type_info, the one entry of its table of virtual functions, which every type descriptor points to. */
void *THISCALL raven_type_info_delete(type_info *self, unsigned flags)
{
	raven_type_info_destroy(self);
	if (flags & 1)
		free(self);
	return self;
}

const void *const raven_type_info_vtable[] = {(const void *)raven_type_info_delete};
const char *THISCALL raven_type_info_raw_name(const type_info *self) { return self->mangled; }
const char *THISCALL raven_type_info_name(type_info *self) { return self->mangled; }
int THISCALL raven_type_info_equal(const type_info *self, const type_info *other) { return !strcmp(self->mangled, other->mangled); }
int THISCALL raven_type_info_unequal(const type_info *self, const type_info *other) { return strcmp(self->mangled, other->mangled) != 0; }
int THISCALL raven_type_info_before(const type_info *self, const type_info *other) { return strcmp(self->mangled, other->mangled) < 0; }

/* std::exception as msvcrt has it: a table of virtual functions, the message and whether the object owns it. */
typedef struct { const void *vtable; const char *message; int owned; } exception_object;

const char *THISCALL raven_exception_what(const exception_object *self) { return self->message ? self->message : "Unknown exception"; }
void *THISCALL raven_exception_delete(exception_object *self, unsigned flags);
const void *const raven_exception_vtable[] = {(const void *)raven_exception_delete, (const void *)raven_exception_what};

static void set_message(exception_object *self, const char *message)
{
	self->vtable = raven_exception_vtable;
	self->message = NULL;
	self->owned = 0;
	if (message) {
		char *copy = malloc(strlen(message) + 1);
		if (copy) {
			strcpy(copy, message);
			self->message = copy;
			self->owned = 1;
		}
	}
}

exception_object *THISCALL raven_exception_construct(exception_object *self)
{
	set_message(self, NULL);
	return self;
}

exception_object *THISCALL raven_exception_construct_message(exception_object *self, const char *const *message)
{
	set_message(self, *message);
	return self;
}

exception_object *THISCALL raven_exception_construct_message_shared(exception_object *self, const char *const *message, int unused)
{
	self->vtable = raven_exception_vtable;
	self->message = *message;
	self->owned = 0;
	return self;
}

exception_object *THISCALL raven_exception_copy(exception_object *self, const exception_object *other)
{
	if (other->owned)
		set_message(self, other->message);
	else {
		self->vtable = raven_exception_vtable;
		self->message = other->message;
		self->owned = 0;
	}
	return self;
}

void THISCALL raven_exception_destroy(exception_object *self)
{
	self->vtable = raven_exception_vtable;
	if (self->owned)
		free((void *)self->message);
}

exception_object *THISCALL raven_exception_assign(exception_object *self, const exception_object *other)
{
	if (self != other) {
		raven_exception_destroy(self);
		raven_exception_copy(self, other);
	}
	return self;
}

/* The deleting destructor: flag 1 frees the object, flag 2 an array whose count precedes it. */
void *THISCALL raven_exception_delete(exception_object *self, unsigned flags)
{
	if (flags & 2) {
		int *count = (int *)self - 1;
		for (int index = 0; index < *count; ++index)
			raven_exception_destroy(self + index);
		if (flags & 1)
			free(count);
		return count;
	}
	raven_exception_destroy(self);
	if (flags & 1)
		free(self);
	return self;
}

/* A pure virtual function called through an object under construction or destruction ends the process. */
CRT int _purecall(void)
{
	ExitProcess(255);
	return 0;
}
