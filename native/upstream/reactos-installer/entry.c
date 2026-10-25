/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windef.h>
#include <winbase.h>
#include <winuser.h>

/* The start of a program built without a C runtime start file: WinMain with the command line after the program name, as the runtime's
   start code would pass it. */
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show);

void WINAPI raven_program_entry(void)
{
	LPSTR line = GetCommandLineA();
	BOOL quoted = FALSE;
	while (*line && (quoted || (*line != ' ' && *line != '\t'))) {
		if (*line == '"')
			quoted = !quoted;
		++line;
	}
	while (*line == ' ' || *line == '\t')
		++line;
	STARTUPINFOA startup = {sizeof startup};
	GetStartupInfoA(&startup);
	ExitProcess((UINT)WinMain(GetModuleHandleA(NULL), NULL, line, startup.dwFlags & STARTF_USESHOWWINDOW ? startup.wShowWindow : SW_SHOWDEFAULT));
}
