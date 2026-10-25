/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windef.h>
#include <winbase.h>
#include <winuser.h>

/* The start of a Unicode program built without a C runtime start file: wWinMain with the command line after the program name. */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show);

void WINAPI raven_program_entry(void)
{
	LPWSTR line = GetCommandLineW();
	BOOL quoted = FALSE;
	while (*line && (quoted || (*line != L' ' && *line != L'\t'))) {
		if (*line == L'"')
			quoted = !quoted;
		++line;
	}
	while (*line == L' ' || *line == L'\t')
		++line;
	STARTUPINFOW startup = {sizeof startup};
	GetStartupInfoW(&startup);
	ExitProcess((UINT)wWinMain(GetModuleHandleW(NULL), NULL, line, startup.dwFlags & STARTF_USESHOWWINDOW ? startup.wShowWindow : SW_SHOWDEFAULT));
}
