#include <windows.h>

/* A program whose file is larger than a gigabyte, as an installer that carries its payload after the image is: it starts, and reads the
   last bytes of its own file, where large_image.py wrote the marker. */

int raven_entry(void)
{
	static const char marker[16] = "raven large end";
	WCHAR path[MAX_PATH];
	if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return 1;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE) return 2;
	LARGE_INTEGER size, at;
	if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0x40000000) return 3;
	at.QuadPart = size.QuadPart - sizeof marker;
	if (!SetFilePointerEx(file, at, NULL, FILE_BEGIN)) return 4;
	char tail[sizeof marker];
	DWORD got = 0;
	if (!ReadFile(file, tail, sizeof tail, &got, NULL) || got != sizeof tail) return 5;
	CloseHandle(file);
	for (unsigned i = 0; i < sizeof marker; ++i)
		if (tail[i] != marker[i]) return 6;
	return 0;
}
