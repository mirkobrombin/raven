#define COBJMACROS
#include <windows.h>
#include <urlmon.h>

// Flags tune how a URL is looked at; a local file stays in the local zone whichever are passed.
int main(void)
{
	static const DWORD cases[] = {0, MUTZ_NOSAVEDFILECHECK, MUTZ_ISFILE, MUTZ_NOSAVEDFILECHECK | MUTZ_ISFILE, MUTZ_DONT_UNESCAPE};
	IInternetSecurityManager *manager = NULL;
	DWORD index, zone;
	if (FAILED(CoInternetCreateSecurityManager(NULL, &manager, 0)))
		return 1;
	for (index = 0; index < ARRAYSIZE(cases); index++)
	{
		zone = URLZONE_INVALID;
		if (IInternetSecurityManager_MapUrlToZone(manager, L"C:\\Users\\raven\\Documents\\file.docx", &zone, cases[index]) != S_OK)
			return 10 + index;
		if (zone != URLZONE_LOCAL_MACHINE)
			return 20 + index;
	}
	zone = URLZONE_INVALID;
	if (IInternetSecurityManager_MapUrlToZone(manager, L"https://example.com/", &zone, MUTZ_NOSAVEDFILECHECK) != S_OK || zone != URLZONE_INTERNET)
		return 30;
	IInternetSecurityManager_Release(manager);
	return 0;
}
