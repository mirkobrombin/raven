/* A library loaded after start, as PDF-XChange Editor loads its core: it imports the directory services by number, the HTTP server, audio
   compression and color management libraries by name, ntdll's keyed events and, in 64-bit processes, the path function that allocates its
   result. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllimport) HRESULT WINAPI ADsGetObject(LPCWSTR, REFIID, void **);
__declspec(dllimport) HRESULT WINAPI ADsGetLastError(DWORD *, WCHAR *, DWORD, WCHAR *, DWORD);
__declspec(dllimport) ULONG WINAPI HttpInitialize(ULONG, ULONG, void *);
__declspec(dllimport) UINT WINAPI acmMetrics(HANDLE, UINT, void *);
__declspec(dllimport) HANDLE WINAPI OpenColorProfileW(void *, DWORD, DWORD, DWORD);
__declspec(dllimport) LONG WINAPI NtCreateKeyedEvent(HANDLE *, ACCESS_MASK, void *, ULONG);
#ifdef _WIN64
__declspec(dllimport) HRESULT WINAPI PathAllocCanonicalize(const WCHAR *, ULONG, WCHAR **);
#endif

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	return TRUE;
}

/* The addresses the loader bound those imports to, in that order. */
__declspec(dllexport) void late_imports(void **addresses)
{
	addresses[0] = (void *)ADsGetObject;
	addresses[1] = (void *)ADsGetLastError;
	addresses[2] = (void *)HttpInitialize;
	addresses[3] = (void *)acmMetrics;
	addresses[4] = (void *)OpenColorProfileW;
	addresses[5] = (void *)NtCreateKeyedEvent;
#ifdef _WIN64
	addresses[6] = (void *)PathAllocCanonicalize;
#endif
}
