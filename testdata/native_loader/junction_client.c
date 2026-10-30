#include <windows.h>
#include <winioctl.h>

// Reparse points through DeviceIoControl: an empty directory becomes a mount point (a junction) to another directory, whose file is
// then reached through it; FSCTL_GET_REPARSE_POINT reads the junction back, FSCTL_DELETE_REPARSE_POINT leaves an empty directory, and
// a plain file is not a reparse point. Returns 0, or the number of the check that failed.
static void append(WCHAR *to, const WCHAR *text)
{
	while (*to)
		++to;
	while ((*to++ = *text++))
		;
}

static int ends_with(const WCHAR *text, unsigned length, const WCHAR *tail)
{
	unsigned count = 0;
	while (tail[count])
		++count;
	if (length < count)
		return 0;
	for (unsigned index = 0; index < count; ++index)
		if (text[length - count + index] != tail[index])
			return 0;
	return 1;
}

int raven_entry(void)
{
	static WCHAR root[MAX_PATH], target[MAX_PATH + 32], link[MAX_PATH + 32], inside[MAX_PATH + 64], file[MAX_PATH + 64];
	static __attribute__((aligned(8))) BYTE buffer[1024];
	if (!GetTempPathW(MAX_PATH, root))
		return 1;
	append(target, root);
	append(target, L"raven-junction-target");
	append(link, root);
	append(link, L"raven-junction-link");
	append(file, target);
	append(file, L"\\inside.txt");
	CreateDirectoryW(target, NULL);
	RemoveDirectoryW(link);
	if (!CreateDirectoryW(link, NULL))
		return 2;
	HANDLE created = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written;
	if (created == INVALID_HANDLE_VALUE || !WriteFile(created, "hi", 2, &written, NULL))
		return 3;
	CloseHandle(created);
	// REPARSE_DATA_BUFFER for a mount point: the substitute name \??\<target>, then an empty print name.
	static WCHAR substitute[MAX_PATH + 40];
	append(substitute, L"\\??\\");
	append(substitute, target);
	USHORT length = 0;
	while (substitute[length])
		++length;
	DWORD *tag = (DWORD *)buffer;
	USHORT *words = (USHORT *)(buffer + 4);
	tag[0] = IO_REPARSE_TAG_MOUNT_POINT;
	words[0] = (USHORT)(8 + (length + 2) * 2);
	words[1] = 0;
	words[2] = 0;
	words[3] = (USHORT)(length * 2);
	words[4] = (USHORT)((length + 1) * 2);
	words[5] = 0;
	WCHAR *names = (WCHAR *)(buffer + 16);
	for (USHORT index = 0; index < length; ++index)
		names[index] = substitute[index];
	names[length] = names[length + 1] = 0;
	HANDLE directory = CreateFileW(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (directory == INVALID_HANDLE_VALUE)
		return 4;
	DWORD returned = 0;
	if (!DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer, 8 + words[0], NULL, 0, &returned, NULL))
		return 5;
	CloseHandle(directory);
	if (!(GetFileAttributesW(link) & FILE_ATTRIBUTE_REPARSE_POINT))
		return 6;
	append(inside, link);
	append(inside, L"\\inside.txt");
	HANDLE through = CreateFileW(inside, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	char read[4] = {0};
	if (through == INVALID_HANDLE_VALUE || !ReadFile(through, read, sizeof read, &returned, NULL) || returned != 2 || read[0] != 'h')
		return 7;
	CloseHandle(through);
	directory = CreateFileW(link, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (directory == INVALID_HANDLE_VALUE)
		return 8;
	if (!DeviceIoControl(directory, FSCTL_GET_REPARSE_POINT, NULL, 0, buffer, sizeof buffer, &returned, NULL) || tag[0] != IO_REPARSE_TAG_MOUNT_POINT)
		return 9;
	if (!ends_with((WCHAR *)(buffer + 16 + words[2]), words[3] / 2, L"raven-junction-target"))
		return 10;
	tag[0] = IO_REPARSE_TAG_MOUNT_POINT;
	words[0] = 0;
	if (!DeviceIoControl(directory, FSCTL_DELETE_REPARSE_POINT, buffer, 8, NULL, 0, &returned, NULL))
		return 11;
	CloseHandle(directory);
	if (GetFileAttributesW(link) & FILE_ATTRIBUTE_REPARSE_POINT)
		return 12;
	HANDLE plain = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (plain == INVALID_HANDLE_VALUE)
		return 13;
	if (DeviceIoControl(plain, FSCTL_GET_REPARSE_POINT, NULL, 0, buffer, sizeof buffer, &returned, NULL) || GetLastError() != ERROR_NOT_A_REPARSE_POINT)
		return 14;
	CloseHandle(plain);
	DeleteFileW(file);
	RemoveDirectoryW(link);
	RemoveDirectoryW(target);
	return 0;
}
