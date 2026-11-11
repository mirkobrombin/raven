/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* raven-host.exe: a Linux program as the programs of the prefix see it.
 *
 * The runtime copies it to C:\Program Files\Raven\Host\NAME.exe for each Linux program the prefix exposes, and a program starts it as it
 * would start that program: the files and addresses on its command line go to the Linux program, as far as the program that started the
 * bridge reaches them itself. The exit code is 0 when the Linux program was started. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The runtime's private calls (ravenhost.dll). */
BOOL WINAPI RavenHostOpen(const WCHAR *targets);

/* The arguments after the program's own name, split as the C runtime splits them, into a list of strings ended by an empty one. */
static WCHAR *split(const WCHAR *line)
{
	WCHAR *list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (lstrlenW(line) + 2) * sizeof(WCHAR)), *out = list;
	BOOL first = TRUE;
	if (!list)
		return NULL;
	while (*line) {
		BOOL quoted = FALSE;
		while (*line == L' ' || *line == L'\t')
			++line;
		if (!*line)
			break;
		for (; *line && (quoted || (*line != L' ' && *line != L'\t')); ++line) {
			if (*line == L'"')
				quoted = !quoted;
			else if (!first)
				*out++ = *line;
		}
		if (!first)
			*out++ = 0;
		first = FALSE;
	}
	*out = 0;
	return list;
}

void WINAPI raven_program_entry(void)
{
	WCHAR *targets = split(GetCommandLineW());
	ExitProcess(targets && RavenHostOpen(targets) ? 0 : 1);
}
