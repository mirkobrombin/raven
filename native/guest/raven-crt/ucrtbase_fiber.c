/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Fibers for 32-bit programs: the runtime routes the kernel32 fiber calls here, because switching fibers only swaps registers and
 * thread block fields, which the program's own thread does without the runtime. The current fiber is the thread block's FiberData
 * (fs:0x10), whose first word is the fiber's parameter, as GetFiberData reads it. A switch keeps the callee-saved registers and the
 * floating point control words on the stack it leaves, and the exception list and stack bounds of the thread block in the fiber.
 * Included by ucrtbase.c. */
#ifdef __i386__

typedef struct raven_fiber {
	void *data;
	void *exception_list;
	void *stack_base;
	void *stack_limit;
	void *deallocation_stack;
	void *stack_pointer;
	LPFIBER_START_ROUTINE start;
	/* The stack the fiber was created with, freed with it; null for a thread converted to a fiber. */
	void *allocation;
} raven_fiber;

/* SameTebFlags (TEB 0xfca) bit HasFiberData, which IsThreadAFiber reads. */
#define FIBER_FLAGS_OFFSET 0xfca
#define HAS_FIBER_DATA 0x4

static raven_fiber *current_fiber(void)
{
	raven_fiber *fiber;
	__asm__ volatile ("movl %%fs:0x10, %0" : "=r"(fiber));
	return fiber;
}

static int thread_is_fiber(void)
{
	unsigned short flags;
	__asm__ volatile ("movw %%fs:0xfca, %0" : "=r"(flags));
	return (flags & HAS_FIBER_DATA) != 0;
}

static void set_fiber_state(raven_fiber *fiber)
{
	unsigned short flags;
	__asm__ volatile ("movw %%fs:0xfca, %0" : "=r"(flags));
	flags = fiber ? flags | HAS_FIBER_DATA : flags & ~HAS_FIBER_DATA;
	__asm__ volatile ("movl %0, %%fs:0x10\n\tmovw %1, %%fs:0xfca" : : "r"(fiber), "r"(flags) : "memory");
}

/* SwitchToFiber: saves the running fiber where it stands and resumes the other one where it left off. */
__asm__(".text\n"
	".globl _raven_switch_to_fiber@4\n"
	"_raven_switch_to_fiber@4:\n\t"
	"pushl %ebp\n\tpushl %ebx\n\tpushl %esi\n\tpushl %edi\n\t"
	"subl $8, %esp\n\t"
	"fnstcw (%esp)\n\t"
	"stmxcsr 4(%esp)\n\t"
	"movl %fs:0x10, %eax\n\t"
	"movl %esp, 20(%eax)\n\t"
	"movl %fs:0, %ecx\n\tmovl %ecx, 4(%eax)\n\t"
	"movl %fs:4, %ecx\n\tmovl %ecx, 8(%eax)\n\t"
	"movl %fs:8, %ecx\n\tmovl %ecx, 12(%eax)\n\t"
	"movl %fs:0xe0c, %ecx\n\tmovl %ecx, 16(%eax)\n\t"
	"movl 28(%esp), %edx\n\t"
	"movl %edx, %fs:0x10\n\t"
	"movl 4(%edx), %ecx\n\tmovl %ecx, %fs:0\n\t"
	"movl 8(%edx), %ecx\n\tmovl %ecx, %fs:4\n\t"
	"movl 12(%edx), %ecx\n\tmovl %ecx, %fs:8\n\t"
	"movl 16(%edx), %ecx\n\tmovl %ecx, %fs:0xe0c\n\t"
	"movl 20(%edx), %esp\n\t"
	"fldcw (%esp)\n\t"
	"ldmxcsr 4(%esp)\n\t"
	"addl $8, %esp\n\t"
	"popl %edi\n\tpopl %esi\n\tpopl %ebx\n\tpopl %ebp\n\t"
	"ret $4\n");
void __stdcall raven_switch_to_fiber(void *fiber);

/* Where a new fiber first runs: its start routine with its parameter. A fiber that returns ends its thread, as on Windows. */
__asm__(".text\n"
	"_raven_fiber_start:\n\t"
	"movl %fs:0x10, %eax\n\t"
	"pushl (%eax)\n\t"
	"call *24(%eax)\n\t"
	"pushl $0\n\t"
	"call _ExitThread@4\n");
void raven_fiber_start(void);

void *__stdcall raven_create_fiber_ex(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE start, void *parameter)
{
	raven_fiber *fiber;
	SIZE_T size = reserve > commit ? reserve : commit;
	DWORD *top;
	(void)flags;
	if (!start) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	if (!size)
		size = 1024 * 1024;
	size = (size + 0xffff) & ~(SIZE_T)0xffff;
	fiber = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*fiber));
	if (!fiber) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	fiber->allocation = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!fiber->allocation) {
		HeapFree(GetProcessHeap(), 0, fiber);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	fiber->data = parameter;
	fiber->start = start;
	fiber->exception_list = (void *)~(ULONG_PTR)0;
	fiber->stack_base = (char *)fiber->allocation + size;
	fiber->stack_limit = fiber->allocation;
	fiber->deallocation_stack = fiber->allocation;
	/* The frame a switch resumes from: the control words, the four saved registers, the return into the start and the argument word
	 * the switch's `ret $4` drops, placed so the start calls the routine on a 16-byte aligned stack. */
	top = (DWORD *)fiber->stack_base - 3;
	top[-1] = 0;
	top[-2] = (DWORD)raven_fiber_start;
	top[-3] = top[-4] = top[-5] = top[-6] = 0;
	top[-7] = 0x1f80;
	top[-8] = 0x27f;
	fiber->stack_pointer = top - 8;
	return fiber;
}

void *__stdcall raven_create_fiber(SIZE_T size, LPFIBER_START_ROUTINE start, void *parameter)
{
	return raven_create_fiber_ex(size, 0, 0, start, parameter);
}

void *__stdcall raven_convert_thread_to_fiber_ex(void *parameter, DWORD flags)
{
	raven_fiber *fiber;
	(void)flags;
	if (thread_is_fiber()) {
		SetLastError(ERROR_ALREADY_FIBER);
		return NULL;
	}
	fiber = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*fiber));
	if (!fiber) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	fiber->data = parameter;
	set_fiber_state(fiber);
	return fiber;
}

void *__stdcall raven_convert_thread_to_fiber(void *parameter)
{
	return raven_convert_thread_to_fiber_ex(parameter, 0);
}

BOOL __stdcall raven_convert_fiber_to_thread(void)
{
	raven_fiber *fiber;
	if (!thread_is_fiber()) {
		SetLastError(ERROR_ALREADY_THREAD);
		return FALSE;
	}
	fiber = current_fiber();
	set_fiber_state(NULL);
	/* A thread runs on in whatever fiber it is in; only the record of a converted thread is its own to free. */
	if (fiber && !fiber->allocation)
		HeapFree(GetProcessHeap(), 0, fiber);
	return TRUE;
}

void __stdcall raven_delete_fiber(void *handle)
{
	raven_fiber *fiber = handle;
	if (!fiber)
		return;
	/* Deleting the running fiber ends the thread, whose stack that is. */
	if (fiber == current_fiber())
		ExitThread(1);
	if (fiber->allocation)
		VirtualFree(fiber->allocation, 0, MEM_RELEASE);
	HeapFree(GetProcessHeap(), 0, fiber);
}

BOOL __stdcall raven_is_thread_a_fiber(void)
{
	return thread_is_fiber();
}

#endif
