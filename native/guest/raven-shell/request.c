/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: the file dialog a program opens, shown by the runtime's file picker.
 *
 * The picker is a program of its own, started from the runtime's folder, which the runtime trusts: it sees every host file while the
 * person chooses, and grants the program only what was chosen. The dialog's state goes to it on its standard input and comes back on its
 * standard output; meanwhile the program's windows keep painting. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "shell.h"

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

BOOL run_dialog(HWND owner, DialogState *state)
{
	SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
	HANDLE input_read, input_write, output_read, output_write;
	STARTUPINFOW startup = {sizeof(startup)};
	PROCESS_INFORMATION picker;
	WCHAR line[MAX_PATH + 32];
	DialogState *answer;
	DWORD code = 1;
	UINT length;
	BOOL accepted = FALSE;
	if (!CreatePipe(&input_read, &input_write, &inherit, 0))
		return show_dialog(owner, state);
	if (!CreatePipe(&output_read, &output_write, &inherit, 0)) {
		CloseHandle(input_read);
		CloseHandle(input_write);
		return show_dialog(owner, state);
	}
	SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
	line[0] = L'"';
	length = GetSystemDirectoryW(line + 1, MAX_PATH);
	lstrcpyW(line + 1 + length, L"\\raven-picker.exe\"");
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = input_read;
	startup.hStdOutput = output_write;
	startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	if (!state->dir[0])
		GetCurrentDirectoryW(MAX_PATH, state->dir);
	if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &picker)) {
		CloseHandle(input_read);
		CloseHandle(input_write);
		CloseHandle(output_read);
		CloseHandle(output_write);
		return show_dialog(owner, state);
	}
	CloseHandle(input_read);
	CloseHandle(output_write);
	CloseHandle(picker.hThread);
	transfer(input_write, state, sizeof(*state), TRUE);
	CloseHandle(input_write);
	/* The owner waits as it would behind a modal dialog: disabled, but answering its messages. */
	if (owner)
		EnableWindow(owner, FALSE);
	for (;;) {
		MSG message;
		if (MsgWaitForMultipleObjects(1, &picker.hProcess, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0)
			break;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	if (owner) {
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner);
	}
	answer = HeapAlloc(GetProcessHeap(), 0, sizeof(*answer));
	if (GetExitCodeProcess(picker.hProcess, &code) && code == 0 && answer && transfer(output_read, answer, sizeof(*answer), FALSE) &&
		answer->has_result) {
		*state = *answer;
		accepted = TRUE;
	}
	if (answer)
		HeapFree(GetProcessHeap(), 0, answer);
	CloseHandle(output_read);
	CloseHandle(picker.hProcess);
	state->has_result = accepted;
	return accepted;
}
