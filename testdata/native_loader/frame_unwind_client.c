// A register a prolog saves with a move after setting up its frame register is found from the frame base, the frame register less its
// offset, even when the function has since moved the stack pointer, as code that allocates on the stack as it runs does. The .NET
// runtime unwinds such frames when its garbage collector walks a thread's stack, and reads each saved register where the unwinder says.
// The exit code names the first check that failed.
#include <windows.h>

static UINT64 stack[128];

static void copy(void *to, const void *from, SIZE_T size)
{
	for (SIZE_T at = 0; at < size; ++at)
		((BYTE *)to)[at] = ((const BYTE *)from)[at];
}

int raven_entry(void)
{
	BYTE *code = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!code)
		return 1;
	// push rbp (ends at 1); sub rsp, 0x40 (5); lea rbp, [rsp+0x20] (10); mov [rsp+8], rbx (15). The codes, last first: SAVE_NONVOL rbx
	// at 8, SET_FPREG, ALLOC_SMALL 0x40, PUSH_NONVOL rbp; frame register rbp at offset 0x20.
	static const BYTE info[] = {1, 15, 5, 0x25, 15, 0x34, 1, 0, 10, 0x03, 5, 0x72, 1, 0x50, 0, 0};
	RUNTIME_FUNCTION *table = (RUNTIME_FUNCTION *)(code + 0x200);
	table[0].BeginAddress = 0x000;
	table[0].EndAddress = 0x080;
	table[0].UnwindData = 0x100;
	copy(code + 0x100, info, sizeof(info));
	if (!RtlAddFunctionTable(table, 1, (DWORD64)code))
		return 2;

	// The frame base, then the saved rbx above it, the saved rbp at the top of the allocation, and the return address.
	const DWORD64 base = (DWORD64)&stack[96];
	stack[97] = 0x3333;
	stack[104] = 0x1111;
	stack[105] = 0x2222;
	DWORD64 image = 0;
	const DWORD64 pc = (DWORD64)code + 0x40;
	PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(pc, &image, NULL);
	if (!entry || image != (DWORD64)code)
		return 3;
	CONTEXT context;
	for (BYTE *byte = (BYTE *)&context; byte < (BYTE *)(&context + 1); ++byte)
		*byte = 0;
	context.Rip = pc;
	// The body has moved the stack pointer well below the frame.
	context.Rsp = base - 0x100;
	context.Rbp = base + 0x20;
	context.Rbx = 0x9999;
	KNONVOLATILE_CONTEXT_POINTERS pointers;
	for (BYTE *byte = (BYTE *)&pointers; byte < (BYTE *)(&pointers + 1); ++byte)
		*byte = 0;
	void *data = NULL;
	DWORD64 establisher = 0;
	RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, entry, &context, &data, &establisher, &pointers);
	if (establisher != base)
		return 4;
	if (context.Rbx != 0x3333 || pointers.IntegerContext[3] != &stack[97])
		return 5;
	if (context.Rbp != 0x1111 || pointers.IntegerContext[5] != &stack[104] || context.Rip != 0x2222 || context.Rsp != base + 0x50)
		return 6;
	RtlDeleteFunctionTable(table);
	return 0;
}
