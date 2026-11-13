// The AVX part of a thread's state in its CONTEXT: the system says the YMM registers are saved, a context initialized for extended
// state has room for their upper halves, and they travel with GetThreadContext, SetThreadContext and RtlRestoreContext, as the .NET
// runtime carries them when it interrupts a thread. The exit code names the first check that failed; a processor without AVX passes.
#define _WIN32_WINNT 0x0a00
#include <windows.h>

#ifndef XSTATE_AVX
#define XSTATE_AVX 2
#endif
#ifndef XSTATE_MASK_AVX
#define XSTATE_MASK_AVX (1ull << XSTATE_AVX)
#endif
#ifndef CONTEXT_XSTATE
#define CONTEXT_XSTATE (CONTEXT_AMD64 | 0x40)
#endif

static volatile LONG stage;
static BYTE first[32], second_half[16], seen[32], restored[32];

static DWORD WINAPI holder(void *unused)
{
	/* YMM6 holds the pattern while the thread spins with no call that could spoil it. */
	__asm__ volatile("vmovdqu %0, %%ymm6" : : "m"(first) : "xmm6");
	stage = 1;
	for (;;) {
		__asm__ volatile("vmovdqu %%ymm6, %0" : "=m"(seen));
		if (seen[16] == second_half[0] && seen[31] == second_half[15])
			break;
	}
	stage = 2;
	return 0;
}

static BOOL has_avx(void)
{
	unsigned eax = 1, ebx, ecx = 0, edx;
	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
	return (ecx & (1u << 27)) && (ecx & (1u << 28));
}

int raven_entry(void)
{
	if (!has_avx())
		return 0;
	if (!(GetEnabledXStateFeatures() & XSTATE_MASK_AVX))
		return 1;
	DWORD length = 0;
	if (InitializeContext(NULL, CONTEXT_ALL | CONTEXT_XSTATE, NULL, &length) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
		length < sizeof(CONTEXT) + 24 + 64 + 256)
		return 2;
	void *buffer = VirtualAlloc(NULL, length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	CONTEXT *context = NULL;
	if (!buffer || !InitializeContext(buffer, CONTEXT_ALL | CONTEXT_XSTATE, &context, &length) || !context)
		return 3;
	DWORD64 mask = 99;
	DWORD size = 0;
	if (!(context->ContextFlags & 0x40) || LocateXStateFeature(context, XSTATE_AVX, &size) == NULL || size != 256)
		return 4;
	if (!SetXStateFeaturesMask(context, XSTATE_MASK_AVX) || !GetXStateFeaturesMask(context, &mask) || !(mask & XSTATE_MASK_AVX))
		return 5;
	BYTE *halves = LocateXStateFeature(context, XSTATE_AVX, NULL);
	if (((ULONG_PTR)halves & 15) || halves < (BYTE *)(context + 1))
		return 6;

	for (int index = 0; index < 32; ++index)
		first[index] = (BYTE)(0x40 + index);
	for (int index = 0; index < 16; ++index)
		second_half[index] = (BYTE)(0xb0 + index);
	HANDLE thread = CreateThread(NULL, 0, holder, NULL, 0, NULL);
	if (!thread)
		return 7;
	while (stage != 1)
		Sleep(1);
	if (SuspendThread(thread) == (DWORD)-1 || !GetThreadContext(thread, context))
		return 8;
	/* The upper half of YMM6 is the seventh of the sixteen halves; its lower half is XMM6 in the record. */
	for (int index = 0; index < 16; ++index)
		if (halves[6 * 16 + index] != first[16 + index] || ((BYTE *)&context->Xmm6)[index] != first[index])
			return 9;
	for (int index = 0; index < 16; ++index)
		halves[6 * 16 + index] = second_half[index];
	if (!SetThreadContext(thread, context) || ResumeThread(thread) == (DWORD)-1)
		return 10;
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || stage != 2)
		return 11;
	for (int index = 0; index < 16; ++index)
		if (seen[index] != first[index])
			return 12;
	CloseHandle(thread);

	/* A context restored brings its halves back to the thread that restores it. */
	static volatile LONG returned;
	RtlCaptureContext(context);
	if (!returned) {
		returned = 1;
		context->ContextFlags |= CONTEXT_XSTATE;
		if (!SetXStateFeaturesMask(context, XSTATE_MASK_AVX))
			return 13;
		for (int index = 0; index < 256; ++index)
			halves[index] = (BYTE)(0x70 + index % 16);
		RtlRestoreContext(context, NULL);
		return 14;
	}
	__asm__ volatile("vmovdqu %%ymm7, %0" : "=m"(restored));
	for (int index = 0; index < 16; ++index)
		if (restored[16 + index] != (BYTE)(0x70 + index))
			return 15;
	return 0;
}
