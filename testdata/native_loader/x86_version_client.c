#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>

static int same(const WCHAR *left, const WCHAR *right, UINT count)
{
	for (UINT index = 0; index < count; ++index)
		if (left[index] != right[index]) return 0;
	return 1;
}

// A 32-bit program reads the version resource of a system library through the wide and the narrow functions.
int raven_entry(void)
{
	DWORD handle = 0;
	const DWORD size = GetFileVersionInfoSizeW(L"kernel32.dll", &handle);
	if (!size) return 1;
	BYTE *data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
	if (!data) return 2;
	if (!GetFileVersionInfoW(L"kernel32.dll", 0, size, data)) return 3;
	VS_FIXEDFILEINFO *fixed = 0;
	UINT length = 0;
	if (!VerQueryValueW(data, L"\\", (LPVOID *)&fixed, &length)) return 4;
	if (length != sizeof *fixed || fixed->dwSignature != 0xFEEF04BD) return 5;
	// The pointer comes back as a 32-bit one, inside the block.
	if ((BYTE *)fixed < data || (BYTE *)fixed >= data + size) return 6;
	if (HIWORD(fixed->dwFileVersionMS) != 10) return 7;

	const DWORD size_ansi = GetFileVersionInfoSizeA("kernel32.dll", &handle);
	if (size_ansi != size) return 8;
	BYTE *data_ansi = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size_ansi);
	if (!data_ansi || !GetFileVersionInfoA("kernel32.dll", 0, size_ansi, data_ansi)) return 9;
	VS_FIXEDFILEINFO *fixed_ansi = 0;
	if (!VerQueryValueA(data_ansi, "\\", (LPVOID *)&fixed_ansi, &length)) return 10;
	if (length != sizeof *fixed_ansi || fixed_ansi->dwFileVersionMS != fixed->dwFileVersionMS || fixed_ansi->dwFileVersionLS != fixed->dwFileVersionLS) return 11;

	WORD *translation = 0;
	if (!VerQueryValueW(data, L"\\VarFileInfo\\Translation", (LPVOID *)&translation, &length)) return 12;
	if (length != 4 || translation[0] != 0x0409 || translation[1] != 0x04b0) return 13;
	WCHAR *product = 0;
	if (!VerQueryValueW(data, L"\\StringFileInfo\\040904b0\\InternalName", (LPVOID *)&product, &length)) return 14;
	if (length != 13 || !same(product, L"kernel32.dll", 12) || product[12]) return 15;

	// The narrow query for a string converts it to ANSI, outside the wide block.
	char *product_ansi = 0;
	SetLastError(0);
	if (!VerQueryValueA(data_ansi, "\\StringFileInfo\\040904b0\\InternalName", (LPVOID *)&product_ansi, &length)) return 16;
	if (length != 13 || lstrcmpA(product_ansi, "kernel32.dll")) return 17;
	if ((BYTE *)product_ansi < data_ansi + size_ansi / 2 || (BYTE *)product_ansi >= data_ansi + size_ansi) return 20;
	// A name that is not there.
	if (GetFileVersionInfoSizeA("no-such-file.dll", &handle)) return 18;
	if (GetFileVersionInfoSizeW(L"no-such-file.dll", &handle)) return 19;
	return 0;
}
