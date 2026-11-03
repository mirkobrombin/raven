#include <windows.h>
#include <imagehlp.h>

/* The PE checksum: four bytes that are no image sum to their 16-bit words plus their length; an image's sum leaves out its own CheckSum
   field, so writing the computed value there changes nothing and the header then holds it. Given a path, the program checks that
   file's stored checksum instead, as the linkers of shipped libraries wrote it. */

static BYTE *read_whole(const WCHAR *path, DWORD *length)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	*length = GetFileSize(file, NULL);
	BYTE *data = HeapAlloc(GetProcessHeap(), 0, *length);
	DWORD got = 0;
	if (data && (!ReadFile(file, data, *length, &got, NULL) || got != *length))
		data = NULL;
	CloseHandle(file);
	return data;
}

int raven_entry(void)
{
	BYTE plain[4] = {1, 0, 2, 0};
	DWORD stored = 1, sum = 0;
	if (CheckSumMappedFile(plain, sizeof plain, &stored, &sum) || stored || sum != 7) return 1;

	WCHAR *line = GetCommandLineW(), path[MAX_PATH];
	WCHAR *argument = line;
	if (*argument == L'"') {
		for (++argument; *argument && *argument != L'"'; ++argument)
			;
		if (*argument)
			++argument;
	} else
		while (*argument && *argument != L' ')
			++argument;
	while (*argument == L' ')
		++argument;
	DWORD length = 0;
	BYTE *image;
	if (*argument) {
		image = read_whole(argument, &length);
		if (!image) return 2;
		IMAGE_NT_HEADERS *headers = CheckSumMappedFile(image, length, &stored, &sum);
		return !headers ? 3 : stored != sum ? 4 : 0;
	}
	GetModuleFileNameW(NULL, path, MAX_PATH);
	image = read_whole(path, &length);
	if (!image) return 5;
	IMAGE_NT_HEADERS *headers = CheckSumMappedFile(image, length, &stored, &sum);
	if (!headers || (BYTE *)headers != image + ((IMAGE_DOS_HEADER *)image)->e_lfanew) return 6;
	headers->OptionalHeader.CheckSum = sum;
	DWORD again = 0;
	if (CheckSumMappedFile(image, length, &stored, &again) != headers || again != sum || stored != sum) return 7;
	return 0;
}
