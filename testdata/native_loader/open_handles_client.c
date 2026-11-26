/* Keeps hundreds of files and pipes open at once, as a download client does, and returns 0 or the number of the first failing check. */
#include <windows.h>

#define FILES 300
#define PIPES 150

unsigned int raven_entry(void)
{
	static HANDLE files[FILES], reads[PIPES], writes[PIPES];
	HANDLE first = CreateFileA("C:\\open-handles.bin", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (first == INVALID_HANDLE_VALUE)
		return 1;
	CloseHandle(first);
	for (int index = 0; index < FILES; ++index) {
		files[index] = CreateFileA("C:\\open-handles.bin", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, NULL);
		if (files[index] == INVALID_HANDLE_VALUE)
			return 2;
	}
	for (int index = 0; index < PIPES; ++index)
		if (!CreatePipe(&reads[index], &writes[index], NULL, 0))
			return 3;
	/* Every pipe still carries data from end to end. */
	for (int index = 0; index < PIPES; ++index) {
		DWORD done = 0;
		char byte = (char)index, back = 0;
		if (!WriteFile(writes[index], &byte, 1, &done, NULL) || done != 1)
			return 4;
		if (!ReadFile(reads[index], &back, 1, &done, NULL) || done != 1 || back != byte)
			return 5;
	}
	for (int index = 0; index < FILES; ++index)
		CloseHandle(files[index]);
	for (int index = 0; index < PIPES; ++index) {
		CloseHandle(reads[index]);
		CloseHandle(writes[index]);
	}
	return 0;
}
