#include <windows.h>

// Each drive is one volume device, the same for QueryDosDevice and for the name NtQueryObject gives an open file: drive C is
// \Device\HarddiskVolume1 and Z, after it in letter order, \Device\HarddiskVolume24. Returns 0, or the number of the check that failed.
typedef LONG NTSTATUS;
typedef struct {
	USHORT Length, MaximumLength;
	PWSTR Buffer;
} Text;
__declspec(dllimport) NTSTATUS NTAPI NtQueryObject(HANDLE, int, void *, ULONG, ULONG *);

static int same(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right)
		++left, ++right;
	return *left == *right;
}

int raven_entry(void)
{
	static WCHAR device[MAX_PATH], folder[MAX_PATH];
	if (!QueryDosDeviceW(L"C:", device, MAX_PATH) || !same(device, L"\\Device\\HarddiskVolume1"))
		return 1;
	if (!QueryDosDeviceW(L"Z:", device, MAX_PATH) || !same(device, L"\\Device\\HarddiskVolume24"))
		return 2;
	const DWORD length = GetTempPathW(MAX_PATH, folder);
	if (!length || folder[0] != L'C')
		return 3;
	HANDLE directory = CreateFileW(folder, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (directory == INVALID_HANDLE_VALUE)
		return 4;
	// The name is the volume device, then the path after the drive (without the closing backslash).
	static unsigned char name[1024];
	if (NtQueryObject(directory, 1, name, sizeof name, NULL) != 0)
		return 5;
	const Text *text = (const Text *)name;
	static const WCHAR prefix[] = L"\\Device\\HarddiskVolume1\\";
	for (int index = 0; prefix[index]; ++index)
		if (index >= text->Length / 2 || text->Buffer[index] != prefix[index])
			return 6;
	if (text->Length / 2 != 23 + length - 3 || text->Buffer[22 + length - 3] == L'\\')
		return 7;
	CloseHandle(directory);
	return 0;
}
