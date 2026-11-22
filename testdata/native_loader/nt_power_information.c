#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef LONG (WINAPI *power_information)(POWER_INFORMATION_LEVEL, void *, ULONG, void *, ULONG);

int raven_entry(void)
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	power_information query = (power_information)GetProcAddress(ntdll, "NtPowerInformation");
	power_information query_zw = (power_information)GetProcAddress(ntdll, "ZwPowerInformation");
	if (!query || !query_zw)
		return 1;
	BYTE output[] = {0xff, 0xaa};
	if (query(PlatformInformation, NULL, 0, output, sizeof output) || output[0] != 0 || output[1] != 0xaa)
		return 2;
	output[0] = 0xff;
	if (query_zw(PlatformInformation, NULL, 0, output, 1) || output[0] != 0 || output[1] != 0xaa)
		return 3;
	if (query(PlatformInformation, NULL, 0, output, 0) != (LONG)0xc0000023 || output[1] != 0xaa)
		return 4;
	if (query(PlatformInformation, output, 1, output, sizeof output) != (LONG)0xc000000d ||
		query(PlatformInformation, NULL, 0, NULL, 0) != (LONG)0xc000000d)
		return 5;
	return 0;
}
