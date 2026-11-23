#include <windows.h>

typedef void (*probe_function)(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);

__declspec(dllimport) void GetApplicationLanguages(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);
__declspec(dllimport) void GetSystemMetadataPathForPackage(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);
__declspec(dllimport) void ProbeOrdinal(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);
__declspec(dllimport) void RavenMissingImportOutsideProbe(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);

__declspec(dllexport) unsigned run_probe(unsigned index)
{
#ifdef OUTSIDE_PROBE
	probe_function target = RavenMissingImportOutsideProbe;
#else
	probe_function const probes[] = {GetApplicationLanguages, GetSystemMetadataPathForPackage, ProbeOrdinal};
	probe_function target = probes[index];
#endif
	target(0x111, 0x222, 0x333, 0x444);
	return index;
}
