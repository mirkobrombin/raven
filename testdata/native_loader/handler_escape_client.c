#include <windows.h>

/* The .NET runtime leaves the handler of an exception by loading the registers of the code after the catch itself, without
   RtlRestoreContext, and the program goes on from that frame: the handler's call is over without returning. Here a vectored handler
   jumps back to the frame that raised the exception the same way. The program then raises again, continues, and returns. */
static void *resume[5];
static volatile int left, continued;

static LONG CALLBACK leave(EXCEPTION_POINTERS *pointers)
{
	if (pointers->ExceptionRecord->ExceptionCode != 0xe0001234)
		return EXCEPTION_CONTINUE_SEARCH;
	++left;
	__builtin_longjmp(resume, 1);
}

static LONG CALLBACK resume_after(EXCEPTION_POINTERS *pointers)
{
	if (pointers->ExceptionRecord->ExceptionCode != 0xe0005678)
		return EXCEPTION_CONTINUE_SEARCH;
	++continued;
	return EXCEPTION_CONTINUE_EXECUTION;
}

int raven_entry(void)
{
	void *first = AddVectoredExceptionHandler(1, leave);
	if (!__builtin_setjmp(resume))
		RaiseException(0xe0001234, 0, 0, NULL);
	if (left != 1)
		return 1;
	RemoveVectoredExceptionHandler(first);
	void *second = AddVectoredExceptionHandler(1, resume_after);
	RaiseException(0xe0005678, 0, 0, NULL);
	if (continued != 1)
		return 2;
	RemoveVectoredExceptionHandler(second);
	return 0;
}
