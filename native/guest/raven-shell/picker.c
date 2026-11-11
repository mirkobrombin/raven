/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* raven-picker.exe: the file dialog of every program, run apart from it.
 *
 * The runtime trusts this program when it starts from the runtime's own folder: while the person chooses, it sees the prefix and every
 * host file, and what is chosen is granted to the program that asked, which sees nothing else of the host. The request arrives on the
 * standard input as the dialog's state and the answer leaves on the standard output; the exit code is 0 when the state was read. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "shell.h"

/* The runtime's private calls (ravenhost.dll). */
BOOL WINAPI RavenGrantPath(const WCHAR *path, DWORD write);

static BOOL transfer(HANDLE pipe, void *data, DWORD size, BOOL write)
{
	DWORD done = 0, count;
	while (done < size) {
		if (!(write ? WriteFile(pipe, (BYTE *)data + done, size - done, &count, NULL) : ReadFile(pipe, (BYTE *)data + done, size - done, &count, NULL)) || !count)
			return FALSE;
		done += count;
	}
	return TRUE;
}

/* The program asking cannot point the dialog at a host folder or name a host file for it: the dialog starts in the prefix, and the name
 * it suggests is a name, never a path. */
static void start_in_prefix(DialogState *state)
{
	WCHAR name[MAX_PATH];
	if (state->dir[0] && (state->dir[0] | 0x20) == L'z') {
		DWORD length = GetEnvironmentVariableW(L"USERPROFILE", state->dir, MAX_PATH);
		if (!length || length >= MAX_PATH - 11)
			lstrcpyW(state->dir, L"C:\\");
		else
			lstrcatW(state->dir, L"\\Documents");
	}
	copy_text(name, MAX_PATH, file_part(state->name));
	copy_text(state->name, MAX_PATH, name);
}

void WINAPI raven_program_entry(void)
{
	DialogState *state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
	UINT index;
	if (!state || !transfer(input, state, sizeof(*state), FALSE))
		ExitProcess(1);
	state->filter_count = state->filter_count > MAX_FILTERS ? MAX_FILTERS : state->filter_count;
	state->check_count = state->check_count > MAX_CHECKS ? MAX_CHECKS : state->check_count;
	for (index = 0; index < state->filter_count; ++index)
		state->filters[index].name[63] = state->filters[index].spec[255] = 0;
	state->title[127] = state->ok_label[31] = state->name_label[31] = state->dir[MAX_PATH - 1] = state->name[MAX_PATH - 1] = state->defext[15] = 0;
	start_in_prefix(state);
	if (show_dialog(NULL, state)) {
		/* What was chosen is the program's from now on: a file it opens to read, a file it saves and a folder it picks to write. */
		const DWORD write = state->save || (state->options & FOS_PICKFOLDERS) != 0;
		if (state->result_count > MAX_RESULTS)
			state->result_count = MAX_RESULTS;
		for (index = 0; index < state->result_count; ++index)
			RavenGrantPath(state->results[index], write);
		if (!state->result_count && state->result[0])
			RavenGrantPath(state->result, write);
	} else {
		state->has_result = FALSE;
	}
	ExitProcess(transfer(output, state, sizeof(*state), TRUE) ? 0 : 1);
}
