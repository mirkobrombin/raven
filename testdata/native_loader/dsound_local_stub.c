#include <windows.h>

typedef HRESULT(WINAPI *create_function)(const GUID *, void **, void *);

static HMODULE self;

__declspec(dllexport) DWORD WINAPI RavenLocalDsoundMarker(void)
{
	return 0x5241564eu;
}

/* A wrapper's DirectSoundCreate8, which goes on to the system's dsound.dll by its path; given itself back, it would call itself. */
__declspec(dllexport) HRESULT WINAPI DirectSoundCreate8(const GUID *device, void **sound, void *outer)
{
	HMODULE system = LoadLibraryA("C:\\Windows\\System32\\dsound.dll");
	if (!system || system == self)
		return E_FAIL;
	create_function create = (create_function)GetProcAddress(system, "DirectSoundCreate8");
	return create ? create(device, sound, outer) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		self = instance;
	return TRUE;
}
