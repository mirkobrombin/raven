/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven fondue.exe: Windows Features on Demand, which the .NET Framework's shim starts when a program needs the .NET Framework 3.5 and the
 * feature is off (`fondue.exe /enable-feature:NETFX3 /caller-name:mscoreei.dll`).
 *
 * As on Windows it tells the user which feature the program needs and offers to install it. Windows downloads the feature; here it comes
 * from the local package Microsoft publishes, dotnetfx35.exe: the one HKLM\Software\Raven\Features names in NetFx3Source, or the one
 * the user picks. dism.exe installs it. The exit code is 0 once the feature is on, 1 when the user skips it or it could not be installed. */
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commdlg.h>

static const WCHAR title[] = L"Windows Features";

static WCHAR upper(WCHAR c) { return c >= L'a' && c <= L'z' ? (WCHAR)(c - 32) : c; }

/* Whether the command line asks for the .NET Framework 3.5 feature: /enable-feature:NETFX3, in any case. */
static BOOL asks_netfx3(const WCHAR *line)
{
	static const WCHAR wanted[] = L"/ENABLE-FEATURE:NETFX3";
	for (; *line; ++line) {
		size_t index = 0;
		while (wanted[index] && upper(line[index]) == wanted[index])
			++index;
		if (!wanted[index])
			return TRUE;
	}
	return FALSE;
}

static BOOL configured_source(WCHAR *path, DWORD size)
{
	DWORD bytes = size * sizeof(WCHAR);
	return RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Raven\\Features", L"NetFx3Source", RRF_RT_REG_SZ, NULL, path, &bytes) == ERROR_SUCCESS &&
		GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL chosen_source(WCHAR *path, DWORD size)
{
	OPENFILENAMEW request = {sizeof request};
	path[0] = 0;
	request.lpstrFilter = L".NET Framework 3.5 package (dotnetfx35.exe)\0dotnetfx35.exe\0Programs\0*.exe\0";
	request.lpstrFile = path;
	request.nMaxFile = size;
	request.lpstrTitle = L"Choose the .NET Framework 3.5 package";
	request.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	return GetOpenFileNameW(&request);
}

static DWORD enable(const WCHAR *source)
{
	WCHAR command[MAX_PATH * 2 + 128], system[MAX_PATH];
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION process;
	DWORD code = 1;
	GetSystemDirectoryW(system, MAX_PATH);
	lstrcpyW(command, L"\"");
	lstrcatW(command, system);
	lstrcatW(command, L"\\dism.exe\" /Online /Enable-Feature /FeatureName:NetFx3 /All /LimitAccess /Source:\"");
	lstrcatW(command, source);
	lstrcatW(command, L"\"");
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process))
		return 1;
	WaitForSingleObject(process.hProcess, INFINITE);
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return code;
}

static DWORD fondue(const WCHAR *line)
{
	WCHAR source[MAX_PATH];
	if (!asks_netfx3(line)) {
		MessageBoxW(NULL, L"The requested feature is not known.", title, MB_OK | MB_ICONERROR);
		return 1;
	}
	if (MessageBoxW(NULL, L"An app on your PC needs the following Windows feature:\n\n.NET Framework 3.5 (includes .NET 2.0 and 3.0)\n\n"
			L"Install this feature now? It is installed from the .NET Framework 3.5 package on this PC.", title,
			MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) != IDYES)
		return 1;
	if (!configured_source(source, MAX_PATH) && !chosen_source(source, MAX_PATH))
		return 1;
	if (enable(source)) {
		MessageBoxW(NULL, L"The following feature couldn't be installed:\n.NET Framework 3.5 (includes .NET 2.0 and 3.0)\n\n"
			L"The source files could not be found.", title, MB_OK | MB_ICONERROR);
		return 1;
	}
	MessageBoxW(NULL, L"The following feature was successfully installed:\n.NET Framework 3.5 (includes .NET 2.0 and 3.0)", title,
		MB_OK | MB_ICONINFORMATION);
	return 0;
}

/* The start of a program built without a C runtime start file: the command line after the program name. */
void WINAPI raven_program_entry(void)
{
	LPWSTR line = GetCommandLineW();
	BOOL quoted = FALSE;
	while (*line && (quoted || (*line != L' ' && *line != L'\t'))) {
		if (*line == L'"')
			quoted = !quoted;
		++line;
	}
	ExitProcess(fondue(line));
}
