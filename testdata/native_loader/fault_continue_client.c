// A read through a null pointer is an access violation the program's handlers hear, and one that changes the registers and continues
// execution resumes where it says, as the .NET runtime turns the null check of compiled code into a NullReferenceException; a 32-bit
// program also hears it in the frames it registers. The exit code names the first check that failed.
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
	return 0;
}
