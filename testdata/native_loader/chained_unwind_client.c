// A function split in two keeps its frame in the first part: the second part's unwind data only chains to it. Unwinding from the
// second part, past every prolog, gives the frame register less its offset as the establisher frame, as it does in the first part.
#include <windows.h>

static UINT64 stack[64];

static void copy(void *to, const void *from, SIZE_T size)
{
	for (SIZE_T at = 0; at < size; ++at)
		((BYTE *)to)[at] = ((const BYTE *)from)[at];
}

static int unwind(DWORD64 base, DWORD64 pc, DWORD64 frame, DWORD64 rsp, DWORD64 *establisher, CONTEXT *context)
{
	DWORD64 image = 0;
	PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(pc, &image, NULL);
	if (!entry || image != base)
		return 0;
	for (BYTE *byte = (BYTE *)context; byte < (BYTE *)(context + 1); ++byte)
		*byte = 0;
	context->Rip = pc;
	context->Rsp = rsp;
	context->Rbp = frame;
	void *data = NULL;
	RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, entry, context, &data, establisher, NULL);
	return 1;
}

int raven_entry(void)
{
	BYTE *code = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!code)
		return 1;
	// push rbp; sub rsp, 0x40; lea rbp, [rsp+0x40]: three codes, last first, then the frame register rbp at offset 0x40.
	static const BYTE parent[] = {1, 10, 3, 0x45, 10, 0x03, 5, 0x72, 1, 0x50, 0, 0};
	RUNTIME_FUNCTION *table = (RUNTIME_FUNCTION *)(code + 0x200);
	table[0].BeginAddress = 0x000;
	table[0].EndAddress = 0x040;
	table[0].UnwindData = 0x100;
	table[1].BeginAddress = 0x080;
	table[1].EndAddress = 0x0c0;
	table[1].UnwindData = 0x140;
	copy(code + 0x100, parent, sizeof(parent));
	// The second part: no prolog and no codes of its own, chained to the first part's entry.
	BYTE *chained = code + 0x140;
	chained[0] = 1 | (UNW_FLAG_CHAININFO << 3);
	chained[1] = 0;
	chained[2] = 0;
	chained[3] = 0x45;
	copy(chained + 4, &table[0], sizeof(table[0]));
	if (!RtlAddFunctionTable(table, 2, (DWORD64)code))
		return 2;

	// The frame register points at the saved rbp, with the return address above it.
	const DWORD64 frame = (DWORD64)&stack[48];
	stack[48] = 0x1111;
	stack[49] = 0x2222;
	const DWORD64 rsp = frame - 0x40 - 0x30;
	DWORD64 establisher = 0;
	CONTEXT context;
	if (!unwind((DWORD64)code, (DWORD64)code + 0x20, frame, frame - 0x40, &establisher, &context))
		return 3;
	if (establisher != frame - 0x40 || context.Rbp != 0x1111 || context.Rip != 0x2222 || context.Rsp != frame + 16)
		return 4;
	if (!unwind((DWORD64)code, (DWORD64)code + 0x90, frame, rsp, &establisher, &context))
		return 5;
	if (establisher != frame - 0x40)
		return 6;
	if (context.Rbp != 0x1111 || context.Rip != 0x2222 || context.Rsp != frame + 16)
		return 7;
	RtlDeleteFunctionTable(table);
	return 0;
}
