#include <windows.h>

/* A start served by a warm host: it says READY, reads one line from its standard input, writes it back after GOT, and returns 23, so
   whoever runs it sees the streams and the exit status the start had, wherever it ran. */

int raven_entry(void)
{
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
	char line[256] = "GOT ";
	DWORD count = 0, done = 0;
	WriteFile(output, "READY\n", 6, &done, NULL);
	while (count < sizeof line - 5) {
		DWORD got = 0;
		if (!ReadFile(input, line + 4 + count, 1, &got, NULL) || !got)
			break;
		if (line[4 + count++] == '\n')
			break;
	}
	WriteFile(output, line, 4 + count, &done, NULL);
	return 23;
}
