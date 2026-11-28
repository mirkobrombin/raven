#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

/* A crash reporter's walk of its own stack: three nested calls capture their context and StackWalk64 goes up through each caller, frame
   by frame, then stops at the end of the stack. The exit code names the first check that failed. */

static int walk(void);

static __declspec(noinline) int innermost(void) { return walk(); }
static __declspec(noinline) int middle(void) { return innermost() + 1; }
static __declspec(noinline) int outer(void) { return middle() + 1; }

static BOOL inside(DWORD64 address, void *function)
{
	DWORD64 base = 0;
	PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(address, &base, NULL);
	return entry && base + entry->BeginAddress == (DWORD64)function;
}

static __declspec(noinline) int walk(void)
{
	CONTEXT context;
	RtlCaptureContext(&context);
	STACKFRAME64 frame;
	SecureZeroMemory(&frame, sizeof frame);
	frame.AddrPC.Offset = context.Rip;
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrStack.Offset = context.Rsp;
	frame.AddrStack.Mode = AddrModeFlat;
	frame.AddrFrame.Offset = context.Rbp;
	frame.AddrFrame.Mode = AddrModeFlat;
	HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
	DWORD64 pcs[64], stacks[64];
	int count = 0;
	while (count < 64 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, NULL, SymFunctionTableAccess64,
			SymGetModuleBase64, NULL)) {
		if (!frame.AddrPC.Offset || frame.AddrPC.Mode != AddrModeFlat || frame.AddrStack.Offset != context.Rsp)
			return 100 + count;
		pcs[count] = frame.AddrPC.Offset;
		stacks[count] = frame.AddrStack.Offset;
		++count;
	}
	/* The walk ends, and the frames are those of the calls, in order, each further up the stack. */
	if (count < 5 || count == 64)
		return 2;
	if (!inside(pcs[0], walk) || !inside(pcs[1], innermost) || !inside(pcs[2], middle) || !inside(pcs[3], outer))
		return 3;
	for (int index = 1; index < count; ++index)
		if (stacks[index] <= stacks[index - 1])
			return 4;
	/* Another machine is refused. */
	SecureZeroMemory(&frame, sizeof frame);
	RtlCaptureContext(&context);
	if (StackWalk64(IMAGE_FILE_MACHINE_I386, process, thread, &frame, &context, NULL, NULL, NULL, NULL))
		return 5;
	return 0;
}

int raven_entry(void)
{
	SymInitialize(GetCurrentProcess(), NULL, TRUE);
	return outer() - 2;
}
