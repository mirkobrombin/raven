#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <dsound.h>

typedef DWORD(WINAPI *marker_function)(void);
typedef HRESULT(WINAPI *get_class_function)(REFCLSID, REFIID, void **);

/* The DirectSound component makes DirectSound's classes; the runtime's own dsound.dll does not. */
static BOOL component(HMODULE library)
{
	get_class_function get_class = (get_class_function)GetProcAddress(library, "DllGetClassObject");
	IClassFactory *factory = NULL;
	if (!get_class || get_class(&CLSID_DirectSound8, &IID_IClassFactory, (void **)&factory) != S_OK || !factory)
		return FALSE;
	IClassFactory_Release(factory);
	return TRUE;
}

int raven_entry(void)
{
#ifndef EXPECT_LOCAL_DSOUND
	/* A search of the system directory alone finds the component. */
	HMODULE searched = LoadLibraryExA("dsound.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!searched || !component(searched) || !FreeLibrary(searched))
		return 9;
#endif
	HMODULE first = LoadLibraryA("dsound.dll");
	if (!first)
		return 1;
	marker_function marker = (marker_function)GetProcAddress(first, "RavenLocalDsoundMarker");
#ifdef EXPECT_LOCAL_DSOUND
	if (!marker || marker() != 0x5241564eu)
		return 2;
#else
	if (!GetProcAddress(first, "DirectSoundCreate8") || !component(first))
		return 3;
#endif
	if (!FreeLibrary(first))
		return 4;
	HMODULE second = LoadLibraryA("dsound.dll");
	if (!second)
		return 5;
	marker = (marker_function)GetProcAddress(second, "RavenLocalDsoundMarker");
#ifdef EXPECT_LOCAL_DSOUND
	if (!marker || marker() != 0x5241564eu)
		return 6;
#else
	if (!GetProcAddress(second, "DirectSoundCreate8"))
		return 7;
#endif
	/* The system's dsound.dll by its path, as COM loads a class's library: the component, or the runtime's own beside the program's,
	 * which the program's reaches without coming back to itself. */
	HMODULE system = LoadLibraryExA("C:\\Windows\\System32\\dsound.dll", NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#ifdef EXPECT_LOCAL_DSOUND
	if (!system || system == second || GetProcAddress(system, "RavenLocalDsoundMarker"))
		return 11;
	IDirectSound8 *sound = NULL;
	const HRESULT created = DirectSoundCreate8(NULL, &sound, NULL);
	if (created == E_FAIL)
		return 12;
	if (SUCCEEDED(created))
		IDirectSound8_Release(sound);
#else
	if (system != second)
		return 11;
#endif
	if (!FreeLibrary(system))
		return 13;
	return FreeLibrary(second) ? 0 : 8;
}
