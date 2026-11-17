// Calls Windows answers in user mode, which a program makes many times over: locating the parts of a context's processor state, and
// telling the system the new end of a code generator's function table. They are answered in the process and never reach the runtime,
// as the profile of the runtime's calls shows. The exit code names the first check that failed.
#include <windows.h>

NTSYSAPI VOID NTAPI RtlGrowFunctionTable(PVOID table, DWORD count);

int raven_entry(void)
{
	static CONTEXT context;
	context.ContextFlags = CONTEXT_FULL;
	for (int round = 0; round < 300; ++round) {
		DWORD length = 0;
		BYTE *legacy = LocateXStateFeature(&context, 0, &length);
		if (legacy != (BYTE *)&context.FltSave || length != 512)
			return 1;
		BYTE *vectors = LocateXStateFeature(&context, 1, &length);
		if (vectors != (BYTE *)&context.FltSave + 160 || length != 256)
			return 2;
		// A context without extended state has no place for AVX.
		if (LocateXStateFeature(&context, 2, &length))
			return 3;
		RtlGrowFunctionTable((PVOID)0x7010, (DWORD)round);
	}
	return 0;
}
