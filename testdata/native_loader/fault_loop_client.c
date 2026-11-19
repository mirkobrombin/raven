// A vectored handler that continues execution without advancing past the faulting instruction must not spin forever: the runtime
// ends the run with the fault after a bounded number of rounds.
#include <windows.h>

static LONG CALLBACK loop_handler(EXCEPTION_POINTERS *pointers)
{
	if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION)
		return EXCEPTION_CONTINUE_SEARCH;
	return EXCEPTION_CONTINUE_EXECUTION;
}

int raven_entry(void)
{
	if (!AddVectoredExceptionHandler(1, loop_handler))
		return 1;
	__asm__ volatile("ud2" : : : "memory");
	return 0;
}
