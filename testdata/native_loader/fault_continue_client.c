// A read through a null pointer is an access violation the program's handlers hear, and one that changes the registers and continues
// execution resumes where it says, as the .NET runtime turns the null check of compiled code into a NullReferenceException; a 32-bit
// program also hears it in the frames it registers. An instruction user mode may not run, an int3, and an illegal instruction are the
// exceptions Windows raises for them, PRIV_INSTRUCTION, BREAKPOINT and ILLEGAL_INSTRUCTION, all naming the instruction itself, and
// changing general registers and flags in the handler takes effect on resume. The exit code names the first check that failed.
#include <windows.h>

static DWORD handled;

/* Reads through a null pointer with an instruction of two bytes; esi and edi are given values that must come back unchanged. */
static int load_null(DWORD *kept_first, DWORD *kept_second)
{
	int value;
	DWORD first = *kept_first, second = *kept_second;
#ifdef __x86_64__
	__asm__ volatile("xor %%ecx, %%ecx\n\tmovl (%%rcx), %%eax" : "=a"(value), "+S"(first), "+D"(second) : : "rcx", "memory");
#else
	__asm__ volatile("xor %%ecx, %%ecx\n\tmovl (%%ecx), %%eax" : "=a"(value), "+S"(first), "+D"(second) : : "ecx", "memory");
#endif
	*kept_first = first;
	*kept_second = second;
	return value;
}

static BOOL skip_load(EXCEPTION_RECORD *record, CONTEXT *context, DWORD answer)
{
	if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[1] != 0)
		return FALSE;
#ifdef __x86_64__
	if ((ULONG_PTR)record->ExceptionAddress != context->Rip)
		return FALSE;
	context->Rip += 2;
	context->Rax = answer;
#else
	if ((ULONG_PTR)record->ExceptionAddress != context->Eip)
		return FALSE;
	context->Eip += 2;
	context->Eax = answer;
#endif
	++handled;
	return TRUE;
}

static LONG CALLBACK vectored(EXCEPTION_POINTERS *pointers)
{
	return skip_load(pointers->ExceptionRecord, pointers->ContextRecord, 0x1234) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

/* Skips the faulting instruction of `length` bytes when the exception is `code` and both the record and the context name it.
 * When `rewrite_state` is set, also updates the general registers and flags to verify resume applies handler edits. */
static DWORD expected_code, expected_length;
static ULONG_PTR expected_address;
static BOOL rewrite_state;

static LONG CALLBACK skip_instruction(EXCEPTION_POINTERS *pointers)
{
	EXCEPTION_RECORD *record = pointers->ExceptionRecord;
	CONTEXT *context = pointers->ContextRecord;
#ifdef __x86_64__
	ULONG_PTR *ip = (ULONG_PTR *)&context->Rip;
#else
	DWORD *ip = &context->Eip;
#endif
	if (record->ExceptionCode != expected_code || (ULONG_PTR)record->ExceptionAddress != expected_address || *ip != expected_address)
		return EXCEPTION_CONTINUE_SEARCH;
	*ip += expected_length;
	if (rewrite_state) {
#ifdef __x86_64__
		context->Rax = 0x10101010;
		context->Rbx = 0x20202020;
		context->Rcx = 0x30303030;
		context->Rdx = 0x40404040;
		context->Rsi = 0x50505050;
		context->Rdi = 0x60606060;
#else
		context->Eax = 0x10101010;
		context->Ebx = 0x20202020;
		context->Ecx = 0x30303030;
		context->Edx = 0x40404040;
		context->Esi = 0x50505050;
		context->Edi = 0x60606060;
#endif
		context->EFlags |= 0x41; /* CF | ZF */
	}
	++handled;
	return EXCEPTION_CONTINUE_EXECUTION;
}

/* Runs `in eax, dx`, `int3`, or `ud2` at a label whose address the handler checks against. */
static void port_read(void)
{
	__asm__ volatile(".globl port_read_at\nport_read_at:\n\tin %%dx, %%eax" : : "d"(0x5658) : "eax", "memory");
}

static void breakpoint(void)
{
	__asm__ volatile(".globl breakpoint_at\nbreakpoint_at:\n\tint3" : : : "memory");
}

static DWORD resumed_state[7] __attribute__((used));

static void illegal_instruction(void)
{
#ifdef __x86_64__
	__asm__ volatile(
		"movl $1, %%eax\n\t"
		"testl %%eax, %%eax\n\t"
		"clc\n\t"
		".globl illegal_at\n"
		"illegal_at:\n\t"
		"ud2\n\t"
		"movl %%eax, resumed_state+0(%%rip)\n\t"
		"movl %%ebx, resumed_state+4(%%rip)\n\t"
		"movl %%ecx, resumed_state+8(%%rip)\n\t"
		"movl %%edx, resumed_state+12(%%rip)\n\t"
		"movl %%esi, resumed_state+16(%%rip)\n\t"
		"movl %%edi, resumed_state+20(%%rip)\n\t"
		"pushfq\n\t"
		"popq %%rax\n\t"
		"movl %%eax, resumed_state+24(%%rip)"
		:
		:
		: "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "cc", "memory");
#else
	__asm__ volatile(
		"movl $1, %%eax\n\t"
		"testl %%eax, %%eax\n\t"
		"clc\n\t"
		".globl illegal_at\n"
		"illegal_at:\n\t"
		"ud2\n\t"
		"movl %%eax, _resumed_state+0\n\t"
		"movl %%ebx, _resumed_state+4\n\t"
		"movl %%ecx, _resumed_state+8\n\t"
		"movl %%edx, _resumed_state+12\n\t"
		"movl %%esi, _resumed_state+16\n\t"
		"movl %%edi, _resumed_state+20\n\t"
		"pushfl\n\t"
		"popl %%eax\n\t"
		"movl %%eax, _resumed_state+24"
		:
		:
		: "eax", "ebx", "ecx", "edx", "esi", "edi", "cc", "memory");
#endif
}

#ifdef __x86_64__
extern char port_read_at[], breakpoint_at[], illegal_at[];
#else
extern char port_read_at[] __asm__("port_read_at"), breakpoint_at[] __asm__("breakpoint_at"), illegal_at[] __asm__("illegal_at");
#endif

#ifndef __x86_64__
typedef struct registration {
	struct registration *next;
	void *handler;
} registration;

static EXCEPTION_DISPOSITION __cdecl frame_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context, void *dispatcher)
{
	(void)frame;
	(void)dispatcher;
	return skip_load(record, context, 0x5678) ? ExceptionContinueExecution : ExceptionContinueSearch;
}
#endif

int raven_entry(void)
{
	void *cookie = AddVectoredExceptionHandler(1, vectored);
	if (!cookie)
		return 1;
	DWORD first = 0x11111111, second = 0x22222222;
	if (load_null(&first, &second) != 0x1234 || handled != 1)
		return 2;
	if (first != 0x11111111 || second != 0x22222222)
		return 3;
	RemoveVectoredExceptionHandler(cookie);
#ifndef __x86_64__
	registration own = {0, (void *)frame_handler};
	__asm__ volatile("movl %%fs:0, %0" : "=r"(own.next));
	__asm__ volatile("movl %0, %%fs:0" : : "r"(&own) : "memory");
	const int value = load_null(&first, &second);
	__asm__ volatile("movl %0, %%fs:0" : : "r"(own.next) : "memory");
	if (value != 0x5678 || handled != 2)
		return 4;
	if (first != 0x11111111 || second != 0x22222222)
		return 5;
#endif
	cookie = AddVectoredExceptionHandler(1, skip_instruction);
	if (!cookie)
		return 6;
	const DWORD before = handled;
	expected_code = EXCEPTION_PRIV_INSTRUCTION;
	expected_address = (ULONG_PTR)port_read_at;
	expected_length = 1;
	port_read();
	if (handled != before + 1)
		return 7;
	expected_code = EXCEPTION_BREAKPOINT;
	expected_address = (ULONG_PTR)breakpoint_at;
	expected_length = 1;
	breakpoint();
	if (handled != before + 2)
		return 8;
	expected_code = EXCEPTION_ILLEGAL_INSTRUCTION;
	expected_address = (ULONG_PTR)illegal_at;
	expected_length = 2;
	rewrite_state = TRUE;
	illegal_instruction();
	rewrite_state = FALSE;
	if (handled != before + 3)
		return 9;
	if (resumed_state[0] != 0x10101010 || resumed_state[1] != 0x20202020 || resumed_state[2] != 0x30303030 ||
		resumed_state[3] != 0x40404040 || resumed_state[4] != 0x50505050 || resumed_state[5] != 0x60606060 ||
		(resumed_state[6] & 0x41) != 0x41)
		return 10;
	RemoveVectoredExceptionHandler(cookie);
	return 0;
}
