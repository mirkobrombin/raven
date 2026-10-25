/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven DISM: the part of the Deployment Image Servicing and Management tool that turns on the .NET Framework 3.5 feature of the running
 * Windows from a local package, as `dism /Online /Enable-Feature /FeatureName:NetFx3 /Source:<package>` does with the Windows media.
 *
 * The source is the .NET Framework 3.5 SP1 full package Microsoft publishes (dotnetfx35.exe), or a folder it was unpacked into. The
 * package's own setup refuses to run on a Windows that has the feature built in, so the tool installs its parts the way that setup does
 * on Windows XP and 2003: the .NET Framework 2.0 SP2 package with the patches of its service pack, the software rasterizer, the 3.0 SP2
 * package with its patches, then the 3.5 SP1 package. Windows Installer does the work, through msiexec. Get-FeatureInfo and
 * Get-Features report the feature as enabled once the 3.5 package is registered, as programs see it. */
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <windows.h>
/* The FDI structures are packed to four bytes, as in the Windows SDK and the cabinet library; MinGW's header leaves them aligned. */
#include <pshpack4.h>
#include <fdi.h>
#include <poppack.h>

/* FDI seeks from the start, the position or the end, as the C library's fseek does. */
enum { SEEK_SET, SEEK_CUR, SEEK_END };

#define ERROR_SOURCE_MISSING 0x800f081fU
#define ERROR_FEATURE_UNKNOWN 0x800f080cU

static HANDLE output, log_file = INVALID_HANDLE_VALUE;

/* What the tool reports goes to its console and, as with Windows, to Logs\DISM\dism.log in the Windows folder. */
static void say(const WCHAR *text)
{
	DWORD written;
	char bytes[1024];
	int length = WideCharToMultiByte(CP_OEMCP, 0, text, -1, bytes, sizeof(bytes), NULL, NULL);
	if (length <= 1)
		return;
	WriteFile(output, bytes, (DWORD)(length - 1), &written, NULL);
	if (log_file == INVALID_HANDLE_VALUE) {
		WCHAR path[MAX_PATH];
		GetWindowsDirectoryW(path, MAX_PATH);
		wcscat(path, L"\\Logs");
		CreateDirectoryW(path, NULL);
		wcscat(path, L"\\DISM");
		CreateDirectoryW(path, NULL);
		wcscat(path, L"\\dism.log");
		log_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	}
	if (log_file != INVALID_HANDLE_VALUE)
		WriteFile(log_file, bytes, (DWORD)(length - 1), &written, NULL);
}

static WCHAR upper(WCHAR c) { return c >= L'a' && c <= L'z' ? (WCHAR)(c - 32) : c; }

static BOOL starts_with(const WCHAR *text, const WCHAR *prefix)
{
	while (*prefix && upper(*text) == upper(*prefix))
		++text, ++prefix;
	return !*prefix;
}

static BOOL same(const WCHAR *a, const WCHAR *b)
{
	return starts_with(a, b) && wcslen(a) == wcslen(b);
}

static BOOL exists(const WCHAR *path)
{
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL is_folder(const WCHAR *path)
{
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void join(WCHAR *path, const WCHAR *folder, const WCHAR *name)
{
	wcscpy(path, folder);
	if (path[0] && path[wcslen(path) - 1] != L'\\')
		wcscat(path, L"\\");
	wcscat(path, name);
}

/* ---- the cabinet inside a self-extracting package: FDI reads it at its offset in the program ---- */
static HMODULE cabinet;
static HFDI(DIAMONDAPI *fdi_create)(PFNALLOC, PFNFREE, PFNOPEN, PFNREAD, PFNWRITE, PFNCLOSE, PFNSEEK, int, PERF);
static BOOL(DIAMONDAPI *fdi_copy)(HFDI, char *, char *, int, PFNFDINOTIFY, PFNFDIDECRYPT, void *);
static BOOL(DIAMONDAPI *fdi_destroy)(HFDI);
static HANDLE cabinet_file;
static LONG cabinet_offset;
static const WCHAR *unpack_folder;

static void *DIAMONDAPI fdi_alloc(ULONG size) { return HeapAlloc(GetProcessHeap(), 0, size); }
static void DIAMONDAPI fdi_free(void *block) { HeapFree(GetProcessHeap(), 0, block); }

static INT_PTR DIAMONDAPI fdi_open(char *name, int flags, int mode)
{
	WCHAR path[MAX_PATH];
	HANDLE file;
	MultiByteToWideChar(CP_ACP, 0, name, -1, path, MAX_PATH);
	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return -1;
	/* The program the cabinet is in: reads start at the cabinet's first byte. */
	cabinet_file = file;
	SetFilePointer(file, cabinet_offset, NULL, FILE_BEGIN);
	return (INT_PTR)file;
}

static UINT DIAMONDAPI fdi_read(INT_PTR file, void *buffer, UINT size)
{
	DWORD done = 0;
	return ReadFile((HANDLE)file, buffer, size, &done, NULL) ? done : (UINT)-1;
}

static UINT DIAMONDAPI fdi_write(INT_PTR file, void *buffer, UINT size)
{
	DWORD done = 0;
	return WriteFile((HANDLE)file, buffer, size, &done, NULL) ? done : (UINT)-1;
}

static int DIAMONDAPI fdi_close(INT_PTR file) { return CloseHandle((HANDLE)file) ? 0 : -1; }

static LONG DIAMONDAPI fdi_seek(INT_PTR file, LONG distance, int origin)
{
	const BOOL inside = (HANDLE)file == cabinet_file;
	if (origin == SEEK_SET && inside)
		distance += cabinet_offset;
	distance = (LONG)SetFilePointer((HANDLE)file, distance, NULL, origin == SEEK_SET ? FILE_BEGIN : origin == SEEK_CUR ? FILE_CURRENT : FILE_END);
	return inside && distance != (LONG)INVALID_SET_FILE_POINTER ? distance - cabinet_offset : distance;
}

/* Makes the folders of a path inside the unpack folder; the cabinet's names hold "." steps, which are left out. */
static BOOL unpacked_path(const char *name, WCHAR *path)
{
	WCHAR relative[MAX_PATH], *step, *next;
	MultiByteToWideChar(CP_ACP, 0, name, -1, relative, MAX_PATH);
	wcscpy(path, unpack_folder);
	for (step = relative; step && *step; step = next) {
		next = wcschr(step, L'\\');
		if (!next)
			next = wcschr(step, L'/');
		if (next)
			*next++ = 0;
		if (!wcscmp(step, L".") || !*step)
			continue;
		wcscat(path, L"\\");
		wcscat(path, step);
		if (next)
			CreateDirectoryW(path, NULL);
	}
	return TRUE;
}

static INT_PTR DIAMONDAPI fdi_notify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION notification)
{
	WCHAR path[MAX_PATH];
	HANDLE file;
	switch (type) {
	case fdintCOPY_FILE:
		unpacked_path(notification->psz1, path);
		file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		return file == INVALID_HANDLE_VALUE ? -1 : (INT_PTR)file;
	case fdintCLOSE_FILE_INFO:
		CloseHandle((HANDLE)notification->hf);
		return TRUE;
	default:
		return 0;
	}
}

/* Where the cabinet starts in a self-extracting program: its signature, with a size that fits in the file. */
static LONG find_cabinet(const WCHAR *program)
{
	HANDLE file = CreateFileW(program, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	const DWORD total = file == INVALID_HANDLE_VALUE ? 0 : GetFileSize(file, NULL);
	static BYTE window[1 << 20];
	DWORD done = 0, index;
	LONG found = -1;
	if (file == INVALID_HANDLE_VALUE)
		return -1;
	ReadFile(file, window, sizeof(window), &done, NULL);
	CloseHandle(file);
	for (index = 0; index + 12 <= done && found < 0; ++index)
		if (window[index] == 'M' && window[index + 1] == 'S' && window[index + 2] == 'C' && window[index + 3] == 'F' && !window[index + 4] &&
			!window[index + 5] && !window[index + 6] && !window[index + 7]) {
			const DWORD size = window[index + 8] | window[index + 9] << 8 | window[index + 10] << 16 | (DWORD)window[index + 11] << 24;
			if (size && index + size <= total)
				found = (LONG)index;
		}
	return found;
}

static BOOL unpack(const WCHAR *program, const WCHAR *folder)
{
	char name[MAX_PATH], path[MAX_PATH];
	ERF error;
	HFDI context;
	BOOL done;
	const WCHAR *slash = wcsrchr(program, L'\\');
	if (!cabinet && !(cabinet = LoadLibraryW(L"cabinet.dll")))
		return FALSE;
	fdi_create = (void *)GetProcAddress(cabinet, "FDICreate");
	fdi_copy = (void *)GetProcAddress(cabinet, "FDICopy");
	fdi_destroy = (void *)GetProcAddress(cabinet, "FDIDestroy");
	if (!fdi_create || !fdi_copy || !fdi_destroy || (cabinet_offset = find_cabinet(program)) < 0)
		return FALSE;
	unpack_folder = folder;
	CreateDirectoryW(folder, NULL);
	WideCharToMultiByte(CP_ACP, 0, slash ? slash + 1 : program, -1, name, MAX_PATH, NULL, NULL);
	WideCharToMultiByte(CP_ACP, 0, program, -1, path, MAX_PATH, NULL, NULL);
	path[slash ? slash - program + 1 : 0] = 0;
	if (!(context = fdi_create(fdi_alloc, fdi_free, fdi_open, fdi_read, fdi_write, fdi_close, fdi_seek, cpuUNKNOWN, &error)))
		return FALSE;
	done = fdi_copy(context, name, path, 0, fdi_notify, NULL, NULL);
	fdi_destroy(context);
	cabinet_file = NULL;
	return done;
}

/* ---- the packages, installed through msiexec ---- */
static DWORD run(const WCHAR *command)
{
	WCHAR line[8192];
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION process;
	DWORD code = 1;
	wcscpy(line, command);
	if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
		return GetLastError();
	WaitForSingleObject(process.hProcess, INFINITE);
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return code == ERROR_SUCCESS_REBOOT_REQUIRED ? 0 : code;
}

/* Installs a package with every patch of its service pack, the .msp files of its folder whose names `wanted` accepts. The framework
 * packages refuse a direct installation: VSEXTUI=1 is what the package's own setup passes them. */
static DWORD install(const WCHAR *folder, const WCHAR *package, BOOL (*wanted)(const WCHAR *), const WCHAR *properties)
{
	static WCHAR command[8192];
	WCHAR system[MAX_PATH], pattern[MAX_PATH], patch[MAX_PATH], log[MAX_PATH];
	WIN32_FIND_DATAW found;
	HANDLE search;
	BOOL first = TRUE;
	GetSystemDirectoryW(system, MAX_PATH);
	wcscpy(command, L"\"");
	wcscat(command, system);
	wcscat(command, L"\\msiexec.exe\" /i \"");
	wcscat(command, folder);
	wcscat(command, L"\\");
	wcscat(command, package);
	wcscat(command, L"\" /qn REBOOT=ReallySuppress /l*v \"");
	/* Windows Installer's own log of each package goes beside the tool's, as Logs\DISM\<package>.log. */
	GetWindowsDirectoryW(log, MAX_PATH);
	wcscat(command, log);
	wcscat(command, L"\\Logs\\DISM\\");
	wcscat(command, package);
	wcscat(command, L".log\"");
	if (properties) {
		wcscat(command, L" ");
		wcscat(command, properties);
	}
	join(pattern, folder, L"*.msp");
	if (wanted && (search = FindFirstFileW(pattern, &found)) != INVALID_HANDLE_VALUE) {
		do {
			if (!wanted(found.cFileName))
				continue;
			join(patch, folder, found.cFileName);
			wcscat(command, first ? L" PATCH=\"" : L";");
			wcscat(command, patch);
			first = FALSE;
		} while (FindNextFileW(search, &found));
		FindClose(search);
		if (!first)
			wcscat(command, L"\"");
	}
	return run(command);
}

/* A 64-bit Windows takes the patches of both widths, a 32-bit one those without the 64-bit suffix. */
static BOOL windows_64;
static BOOL patch_for_width(const WCHAR *name)
{
	return windows_64 || !wcsstr(name, L"_64");
}

static BOOL enabled(void)
{
	HKEY key;
	DWORD value = 0, size = sizeof(value), type = 0;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP\\v3.5", 0, KEY_READ | KEY_WOW64_64KEY, &key))
		return FALSE;
	if (RegQueryValueExW(key, L"Install", NULL, &type, (BYTE *)&value, &size) || type != REG_DWORD)
		value = 0;
	RegCloseKey(key);
	return value == 1;
}

/* The folder that holds the dotNetFX20, dotNetFX30 and dotNetFX35 folders of the package, unpacked when the source is the program. */
static BOOL payload_folder(const WCHAR *source, WCHAR *payload)
{
	static const WCHAR *const inside[] = {L"", L"wcu\\dotNetFramework", L"dotNetFramework"};
	WCHAR temporary[MAX_PATH], probe[MAX_PATH];
	size_t index;
	if (!is_folder(source)) {
		if (!exists(source))
			return FALSE;
		GetWindowsDirectoryW(temporary, MAX_PATH);
		wcscat(temporary, L"\\Temp");
		CreateDirectoryW(temporary, NULL);
		wcscat(temporary, L"\\NetFx3");
		if (!unpack(source, temporary))
			return FALSE;
		source = temporary;
	}
	for (index = 0; index < sizeof(inside) / sizeof(inside[0]); ++index) {
		join(payload, source, inside[index]);
		join(probe, payload, L"dotNetFX20");
		if (is_folder(probe))
			return TRUE;
	}
	return FALSE;
}

static DWORD enable_netfx3(const WCHAR *source)
{
	WCHAR payload[MAX_PATH], folder[MAX_PATH], netfx35[MAX_PATH], unpacked[MAX_PATH];
	DWORD code;
	say(L"Enabling feature(s)\r\n");
	if (enabled()) {
		say(L"[==========================100.0%==========================]\r\nThe operation completed successfully.\r\n");
		return 0;
	}
	if (!source || !payload_folder(source, payload)) {
		say(L"\r\nError: 0x800f081f\r\n\r\nThe source files could not be found.\r\nUse the \"Source\" option to specify the location of the files that "
			L"are required to restore the feature.\r\n");
		return ERROR_SOURCE_MISSING;
	}
	/* 2.0 SP2 with its patches, the rasterizer the 3.0 framework draws with, 3.0 SP2 with its patches, then 3.5 SP1. MSXML 6 is part of
	 * Windows 10, and its package in the folder declines to install there. The packages are those of Windows XP and 2003, whose
	 * components and custom actions decide by the Windows version; they are installed under the Windows Server 2003 compatibility
	 * layer, as Windows runs an old installer, so that every part of them sees the system they were made for. */
	SetEnvironmentVariableW(L"__COMPAT_LAYER", windows_64 ? L"WinSrv03Sp1" : L"WinXPSp3");
	join(folder, payload, L"dotNetFX20");
	if ((code = install(folder, windows_64 ? L"Netfx20a_x64.msi" : L"Netfx20a_x86.msi", patch_for_width, L"VSEXTUI=1")))
		return code;
	say(L"[==========================50.0%                          ]\r\n");
	join(folder, payload, L"dotNetFX30");
	if ((code = install(folder, windows_64 ? L"RGB9RAST_x64.msi" : L"RGB9RAST_x86.msi", NULL, NULL)))
		return code;
	if ((code = install(folder, windows_64 ? L"Netfx30a_x64.msi" : L"Netfx30a_x86.msi", patch_for_width, L"VSEXTUI=1")))
		return code;
	say(L"[==========================75.0%=============             ]\r\n");
	join(netfx35, payload, windows_64 ? L"dotNetFX35\\x64\\netfx35_x64.exe" : L"dotNetFX35\\x86\\netfx35_x86.exe");
	join(unpacked, payload, L"dotNetFX35\\setup");
	if (!unpack(netfx35, unpacked))
		return ERROR_SOURCE_MISSING;
	if ((code = install(unpacked, L"vs_setup.msi", NULL, L"VSEXTUI=1")))
		return code;
	SetEnvironmentVariableW(L"__COMPAT_LAYER", NULL);
	say(L"[==========================100.0%==========================]\r\nThe operation completed successfully.\r\n");
	return 0;
}

static void feature_info(void)
{
	say(L"\r\nFeature Information:\r\n\r\nFeature Name : NetFx3\r\nDisplay Name : .NET Framework 3.5 (includes .NET 2.0 and 3.0)\r\n");
	say(L"Description : .NET Framework 3.5 (includes .NET 2.0 and 3.0)\r\nRestart Required : Possible\r\n");
	say(enabled() ? L"State : Enabled\r\n" : L"State : Disabled with Payload Removed\r\n");
	say(L"\r\nThe operation completed successfully.\r\n");
}

/* The arguments after the program name, each on its own, quotes removed. */
static int split(WCHAR *line, WCHAR **arguments, int capacity)
{
	int count = 0;
	while (*line && count < capacity) {
		WCHAR *write;
		BOOL quoted = FALSE;
		while (*line == L' ' || *line == L'\t')
			++line;
		if (!*line)
			break;
		arguments[count++] = write = line;
		while (*line && (quoted || (*line != L' ' && *line != L'\t'))) {
			if (*line == L'"')
				quoted = !quoted;
			else
				*write++ = *line;
			++line;
		}
		if (*line)
			++line;
		*write = 0;
	}
	return count;
}

static DWORD dism(WCHAR *line)
{
	WCHAR *arguments[32];
	const WCHAR *feature = NULL, *source = NULL;
	BOOL online = FALSE, enable = FALSE, info = FALSE, list = FALSE;
	const int count = split(line, arguments, 32);
	int index;
	BOOL wow64 = FALSE;
	windows_64 = sizeof(void *) == 8 || (IsWow64Process(GetCurrentProcess(), &wow64) && wow64);
	say(L"\r\nDeployment Image Servicing and Management tool\r\nVersion: 10.0.19041.844\r\n\r\n");
	for (index = 0; index < count; ++index) {
		const WCHAR *argument = arguments[index];
		if (same(argument, L"/Online"))
			online = TRUE;
		else if (same(argument, L"/Enable-Feature"))
			enable = TRUE;
		else if (same(argument, L"/Get-FeatureInfo"))
			info = TRUE;
		else if (same(argument, L"/Get-Features"))
			list = TRUE;
		else if (starts_with(argument, L"/FeatureName:"))
			feature = argument + 13;
		else if (starts_with(argument, L"/Source:"))
			source = argument + 8;
	}
	if (!online || (!enable && !info && !list)) {
		say(L"Error: 87\r\n\r\nOnly /Online with /Enable-Feature, /Get-FeatureInfo or /Get-Features is known here.\r\n");
		return ERROR_INVALID_PARAMETER;
	}
	say(L"Image Version: 10.0.19045.0\r\n\r\n");
	if (list) {
		say(L"Features listing for package : Microsoft-Windows-Foundation-Package~31bf3856ad364e35~amd64~~10.0.19041.1\r\n\r\n");
		say(enabled() ? L"Feature Name : NetFx3\r\nState : Enabled\r\n\r\n" : L"Feature Name : NetFx3\r\nState : Disabled with Payload Removed\r\n\r\n");
		say(L"The operation completed successfully.\r\n");
		return 0;
	}
	if (!feature || !same(feature, L"NetFx3")) {
		say(L"Error: 0x800f080c\r\n\r\nFeature name is unknown.\r\n");
		return ERROR_FEATURE_UNKNOWN;
	}
	if (info) {
		feature_info();
		return 0;
	}
	return enable_netfx3(source);
}

/* The start of a program built without a C runtime start file: the command line after the program name. */
void WINAPI raven_program_entry(void)
{
	LPWSTR line = GetCommandLineW();
	BOOL quoted = FALSE;
	output = GetStdHandle(STD_OUTPUT_HANDLE);
	while (*line && (quoted || (*line != L' ' && *line != L'\t'))) {
		if (*line == L'"')
			quoted = !quoted;
		++line;
	}
	ExitProcess(dism(line));
}
