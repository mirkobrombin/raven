/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "advpack.h"

#define S_ASYNC_STARTED ((HRESULT)0x000401E8)
#define INFO_MAX 2048

typedef HRESULT (WINAPI *register_function)(void);
typedef HRESULT (WINAPI *dll_install_function)(BOOL install, const WCHAR *line);

/* One running install: the open INF, what it installs and where its files come from. */
typedef struct
{
	HWND window;
	HINF inf;
	const WCHAR *section;
	WCHAR *source;
	DWORD flags;
} install_run;

HRESULT WINAPI SetPerUserSecValuesW(per_user_w *values);
BOOL WINAPI IsNTAdmin(DWORD reserved, DWORD *unused);

static HRESULT open_inf(const WCHAR *path, HINF *inf)
{
	HINF opened;
	opened = SetupOpenInfFileW(path, NULL, INF_STYLE_WIN4, NULL);
	if (opened == INVALID_HANDLE_VALUE)
		return adv_error(GetLastError());
	*inf = opened;
	return S_OK;
}

static BOOL section_value(HINF inf, const WCHAR *section, const WCHAR *key, WCHAR *value, DWORD size)
{
	return SetupGetLineTextW(NULL, inf, section, key, value, size, NULL);
}

static void strip_quotes(WCHAR *text)
{
	size_t length = wcslen(text);
	if (length >= 2 && text[0] == L'\'' && text[length - 1] == L'\'')
	{
		memmove(text, text + 1, (length - 2) * sizeof(WCHAR));
		text[length - 2] = 0;
	}
}

static HKEY root_key(const WCHAR *name)
{
	if (!lstrcmpiW(name, L"HKLM"))
		return HKEY_LOCAL_MACHINE;
	if (!lstrcmpiW(name, L"HKCU"))
		return HKEY_CURRENT_USER;
	if (!lstrcmpiW(name, L"HKCR"))
		return HKEY_CLASSES_ROOT;
	if (!lstrcmpiW(name, L"HKU"))
		return HKEY_USERS;
	return NULL;
}

static BOOL line_field(INFCONTEXT *line, DWORD index, WCHAR *value, DWORD size)
{
	value[0] = 0;
	if (!SetupGetStringFieldW(line, index, value, size, NULL))
		return FALSE;
	strip_quotes(value);
	return TRUE;
}

/* A destination line is root,key,value,prompt,default: the registry wins over the default. */
static BOOL destination_from_line(INFCONTEXT *line, WCHAR *path, DWORD size)
{
	WCHAR root[16], key[MAX_PATH], value[MAX_PATH], text[INFO_MAX];
	DWORD type, bytes = sizeof(text) - sizeof(WCHAR);
	HKEY handle, base;
	LONG status = ERROR_FILE_NOT_FOUND;
	line_field(line, 1, root, ARRAYSIZE(root));
	line_field(line, 2, key, ARRAYSIZE(key));
	line_field(line, 3, value, ARRAYSIZE(value));
	base = root_key(root);
	if (base && !RegOpenKeyExW(base, key, 0, KEY_READ, &handle))
	{
		status = RegQueryValueExW(handle, value, NULL, &type, (BYTE *)text, &bytes);
		RegCloseKey(handle);
		if (!status && type != REG_SZ && type != REG_EXPAND_SZ)
			status = ERROR_INVALID_DATA;
	}
	if (status)
	{
		if (!line_field(line, 5, text, ARRAYSIZE(text)) || !text[0])
			return FALSE;
		type = REG_SZ;
	}
	else
		text[bytes / sizeof(WCHAR)] = 0;
	if (type == REG_EXPAND_SZ)
		ExpandEnvironmentStringsW(text, path, size);
	else
		lstrcpynW(path, text, size);
	return TRUE;
}

/* A bare drive names its root directory. */
static void finish_destination(WCHAR *path)
{
	if (path[0] && path[1] == L':' && !path[2])
	{
		path[2] = L'\\';
		path[3] = 0;
	}
}

static void apply_custom_destinations(HINF inf, const WCHAR *install)
{
	WCHAR name[MAX_PATH], id_text[32], target[MAX_PATH], path[INFO_MAX];
	INFCONTEXT line, destination;
	if (!section_value(inf, install, L"CustomDestination", name, ARRAYSIZE(name)))
		return;
	if (!SetupFindFirstLineW(inf, name, NULL, &line))
		return;
	do
	{
		if (!line_field(&line, 0, id_text, ARRAYSIZE(id_text)) || !line_field(&line, 1, target, ARRAYSIZE(target)))
			continue;
		if (!SetupFindFirstLineW(inf, target, NULL, &destination))
			continue;
		do
		{
			if (!destination_from_line(&destination, path, ARRAYSIZE(path)))
				continue;
			finish_destination(path);
			SetupSetDirectoryIdW(inf, wcstol(id_text, NULL, 10), path);
			break;
		}
		while (SetupFindNextLine(&destination, &destination));
	}
	while (SetupFindNextLine(&line, &line));
}

static HRESULT register_library(const WCHAR *path, const WCHAR *mode, const WCHAR *arguments, BOOL install)
{
	BOOL call_install = FALSE, call_register = TRUE;
	dll_install_function dll_install;
	register_function function;
	HRESULT result = S_OK, com;
	HMODULE library;
	const WCHAR *flag;
	for (flag = mode; *flag; flag++)
	{
		if (*flag == L'i' || *flag == L'I')
			call_install = TRUE;
		else if (*flag == L'n' || *flag == L'N')
			call_register = FALSE;
	}
	com = CoInitialize(NULL);
	library = LoadLibraryW(path);
	if (!library)
		result = adv_error(GetLastError());
	else
	{
		dll_install = (dll_install_function)GetProcAddress(library, "DllInstall");
		function = (register_function)GetProcAddress(library, install ? "DllRegisterServer" : "DllUnregisterServer");
		if (!install && call_install && dll_install)
			result = dll_install(FALSE, arguments);
		if (SUCCEEDED(result) && call_register)
			result = function ? function() : E_FAIL;
		if (SUCCEEDED(result) && install && call_install)
			result = dll_install ? dll_install(TRUE, arguments) : E_FAIL;
		FreeLibrary(library);
	}
	if (SUCCEEDED(com))
		CoUninitialize();
	return result;
}

/* A library named with a leading @ registers at the next logon, like the delay flag asks. */
static HRESULT register_later(const WCHAR *path, const WCHAR *mode)
{
	static const WCHAR run_once[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
	WCHAR command[INFO_MAX];
	HKEY key;
	LONG status;
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return S_OK;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, run_once, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL))
		return S_OK;
	lstrcpyW(command, L"regsvr32.exe /s ");
	if (wcschr(mode, L'i') || wcschr(mode, L'I'))
		lstrcatW(command, L"/i ");
	if (wcschr(mode, L'n') || wcschr(mode, L'N'))
		lstrcatW(command, L"/n ");
	lstrcatW(command, L"\"");
	lstrcatW(command, path);
	lstrcatW(command, L"\"");
	status = RegSetValueExW(key, path, 0, REG_SZ, (const BYTE *)command, (lstrlenW(command) + 1) * sizeof(WCHAR));
	RegCloseKey(key);
	return HRESULT_FROM_WIN32(status);
}

static HRESULT process_libraries(install_run *run, const WCHAR *key, BOOL install)
{
	WCHAR list[INFO_MAX], *name, *next, file[MAX_PATH], mode[16], arguments[INFO_MAX], *path;
	const WCHAR *target;
	HRESULT result = S_OK, step;
	INFCONTEXT line;
	BOOL later;
	if (!section_value(run->inf, run->section, key, list, ARRAYSIZE(list)))
		return S_OK;
	for (name = list; name; name = next)
	{
		next = wcschr(name, L',');
		if (next)
			*next++ = 0;
		if (!SetupFindFirstLineW(run->inf, name, NULL, &line))
			continue;
		do
		{
			if (!line_field(&line, 1, file, ARRAYSIZE(file)) || !file[0])
				continue;
			line_field(&line, 2, mode, ARRAYSIZE(mode));
			line_field(&line, 3, arguments, ARRAYSIZE(arguments));
			target = file;
			later = file[0] == L'@' || (run->flags & INSTALL_DELAY_OCX);
			if (file[0] == L'@')
				target++;
			path = adv_join(run->source, target);
			if (path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
				target = path;
			if (later && install)
				step = register_later(target, mode);
			else
				step = register_library(target, mode, arguments, install);
			adv_free(path);
			if (FAILED(step))
				result = step;
		}
		while (SetupFindNextLine(&line, &line));
	}
	return result;
}

static HRESULT run_commands(install_run *run, const WCHAR *key)
{
	WCHAR list[INFO_MAX], *name, *next, *mode, command[INFO_MAX];
	HRESULT result;
	INFCONTEXT line;
	if (!section_value(run->inf, run->section, key, list, ARRAYSIZE(list)))
		return S_OK;
	for (name = list; name; name = next)
	{
		next = wcschr(name, L',');
		if (next)
			*next++ = 0;
		/* The section name can carry display options after a colon. */
		mode = wcschr(name, L':');
		if (mode)
			*mode = 0;
		if (!SetupFindFirstLineW(run->inf, name, NULL, &line))
			continue;
		do
		{
			if (!SetupGetLineTextW(&line, NULL, NULL, NULL, command, ARRAYSIZE(command), NULL))
				continue;
			result = adv_run_wait(command, run->source, (run->flags & INSTALL_QUIET) != 0);
			if (FAILED(result))
				return result;
		}
		while (SetupFindNextLine(&line, &line));
	}
	return S_OK;
}

static void wide_value(HINF inf, const WCHAR *section, const WCHAR *key, WCHAR *value, DWORD size)
{
	if (!section_value(inf, section, key, value, size))
		value[0] = 0;
}

static HRESULT apply_per_user(install_run *run)
{
	WCHAR name[MAX_PATH], installed[16];
	per_user_w *values;
	HRESULT result;
	if (!section_value(run->inf, run->section, L"PerUserInstall", name, ARRAYSIZE(name)))
		return S_OK;
	values = adv_alloc(sizeof(*values));
	if (!values)
		return E_OUTOFMEMORY;
	wide_value(run->inf, name, L"GUID", values->guid, ARRAYSIZE(values->guid));
	wide_value(run->inf, name, L"DisplayName", values->name, ARRAYSIZE(values->name));
	wide_value(run->inf, name, L"Locale", values->locale, ARRAYSIZE(values->locale));
	wide_value(run->inf, name, L"StubPath", values->stub, ARRAYSIZE(values->stub));
	wide_value(run->inf, name, L"Version", values->version, ARRAYSIZE(values->version));
	wide_value(run->inf, name, L"ComponentID", values->component, ARRAYSIZE(values->component));
	wide_value(run->inf, name, L"IsInstalled", installed, ARRAYSIZE(installed));
	values->installed = installed[0] ? wcstol(installed, NULL, 10) : 1;
	result = SetPerUserSecValuesW(values);
	adv_free(values);
	return result;
}

static HRESULT install_files(install_run *run)
{
	void *context;
	BOOL done;
	DWORD error;
	context = SetupInitDefaultQueueCallbackEx(run->window, INVALID_HANDLE_VALUE, 0, 0, NULL);
	done = SetupInstallFromInfSectionW(run->window, run->inf, run->section, SPINST_ALL, NULL, run->source, SP_COPY_NEWER,
		SetupDefaultQueueCallbackW, context, NULL, NULL);
	error = GetLastError();
	SetupTermDefaultQueueCallback(context);
	return done ? S_OK : adv_error(error);
}

/* Everything an advanced install section asks for, in the order setup programs expect. */
static HRESULT run_install(install_run *run)
{
	WCHAR value[16];
	HRESULT result, later;
	if (section_value(run->inf, run->section, L"CheckAdminRights", value, ARRAYSIZE(value)) && wcstol(value, NULL, 10) && !IsNTAdmin(0, NULL))
		return E_ACCESSDENIED;
	apply_custom_destinations(run->inf, run->section);
	result = run_commands(run, L"RunPreSetupCommands");
	if (FAILED(result))
		return result;
	result = install_files(run);
	if (FAILED(result))
		return result;
	result = process_libraries(run, L"RegisterOCXs", TRUE);
	later = process_libraries(run, L"UnregisterOCXs", FALSE);
	if (SUCCEEDED(result))
		result = later;
	if (FAILED(result))
		return result;
	result = apply_per_user(run);
	if (FAILED(result))
		return result;
	return run_commands(run, L"RunPostSetupCommands");
}

static HRESULT install_inf(HWND window, const WCHAR *path, const WCHAR *section, const WCHAR *directory, DWORD flags)
{
	install_run run = {0};
	HRESULT result;
	WCHAR *full = NULL;
	run.window = window;
	run.section = section && section[0] ? section : L"DefaultInstall";
	run.flags = flags;
	result = open_inf(path, &run.inf);
	if (FAILED(result))
		return result;
	if (directory && directory[0])
		run.source = adv_copy(directory);
	else
	{
		full = adv_full_path(path);
		run.source = adv_parent(full);
	}
	result = run.source ? run_install(&run) : E_OUTOFMEMORY;
	SetupCloseInfFile(run.inf);
	adv_free(run.source);
	adv_free(full);
	return result;
}

/* A bare INF name means the system's INF folders, never the current directory. */
static HRESULT install_named_inf(HWND window, const WCHAR *command, const WCHAR *section, const WCHAR *directory, DWORD flags)
{
	WCHAR folder[MAX_PATH], *path;
	HRESULT result;
	UINT index;
	if (wcschr(command, L'\\') || wcschr(command, L'/') || wcschr(command, L':'))
		return install_inf(window, command, section, directory, flags);
	for (index = 0; index < 2; index++)
	{
		if (index)
			GetSystemDirectoryW(folder, MAX_PATH);
		else
		{
			GetWindowsDirectoryW(folder, MAX_PATH - 4);
			lstrcatW(folder, L"\\inf");
		}
		path = adv_join(folder, command);
		if (!path)
			return E_OUTOFMEMORY;
		if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
		{
			result = install_inf(window, path, section, directory, flags);
			adv_free(path);
			return result;
		}
		adv_free(path);
	}
	return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT WINAPI RunSetupCommandW(HWND window, const WCHAR *command, const WCHAR *section, const WCHAR *directory, const WCHAR *title, HANDLE *process, DWORD flags, void *reserved)
{
	PROCESS_INFORMATION info;
	STARTUPINFOW startup;
	WCHAR *line;
	if (!command)
		return E_INVALIDARG;
	if (flags & 1)
	{
		if (!directory)
			return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
		return install_named_inf(window, command, section, directory, flags);
	}
	if (!directory)
		return E_INVALIDARG;
	if (process)
		*process = NULL;
	if (directory[0] && GetFileAttributesW(directory) == INVALID_FILE_ATTRIBUTES)
		return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
	line = adv_copy(command);
	if (!line)
		return E_OUTOFMEMORY;
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	if (flags & INSTALL_QUIET)
	{
		startup.dwFlags = STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_HIDE;
	}
	if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, directory[0] ? directory : NULL, &startup, &info))
	{
		DWORD error = GetLastError();
		adv_free(line);
		return HRESULT_FROM_WIN32(error);
	}
	adv_free(line);
	CloseHandle(info.hThread);
	if (process)
	{
		*process = info.hProcess;
		return S_ASYNC_STARTED;
	}
	WaitForSingleObject(info.hProcess, INFINITE);
	CloseHandle(info.hProcess);
	return S_OK;
}

HRESULT WINAPI RunSetupCommandA(HWND window, const char *command, const char *section, const char *directory, const char *title, HANDLE *process, DWORD flags, void *reserved)
{
	WCHAR *wide_command = adv_wide(command), *wide_section = adv_wide(section), *wide_directory = adv_wide(directory);
	HRESULT result = RunSetupCommandW(window, wide_command, wide_section, wide_directory, NULL, process, flags, reserved);
	adv_free(wide_command);
	adv_free(wide_section);
	adv_free(wide_directory);
	return result;
}

/* "inf,section,flags,smart reboot": success is 0 and failure 1, as rundll32 callers read them. */
INT WINAPI LaunchINFSectionW(HWND window, HINSTANCE instance, WCHAR *line, INT show)
{
	WCHAR *copy, *fields[4] = {NULL}, *full;
	HRESULT result;
	if (!line)
		return 1;
	copy = adv_copy(line);
	if (!copy)
		return 1;
	adv_split(copy, fields, 4);
	full = adv_full_path(fields[0] ? fields[0] : L"");
	result = full ? install_inf(window, full, fields[1], NULL, fields[2] ? wcstol(fields[2], NULL, 10) : 0) : E_OUTOFMEMORY;
	adv_free(full);
	adv_free(copy);
	return FAILED(result) ? 1 : 0;
}

INT WINAPI LaunchINFSectionA(HWND window, HINSTANCE instance, char *line, INT show)
{
	WCHAR *wide = adv_wide(line);
	INT result = LaunchINFSectionW(window, instance, wide, show);
	adv_free(wide);
	return result;
}

/* "inf,section,cab,flags,smart reboot": a cabinet that exists unpacks next to the INF. */
HRESULT WINAPI LaunchINFSectionExW(HWND window, HINSTANCE instance, WCHAR *line, INT show)
{
	WCHAR *copy, *fields[5] = {NULL}, *full = NULL, *parent = NULL;
	HRESULT result = E_INVALIDARG;
	if (!line)
		return E_INVALIDARG;
	copy = adv_copy(line);
	if (!copy)
		return E_OUTOFMEMORY;
	adv_split(copy, fields, 5);
	if (!fields[0] || !fields[0][0])
		goto done;
	/* A relative INF only makes sense inside a cabinet that can supply it. */
	if ((fields[0][1] != L':' && fields[0][0] != L'\\') && (!fields[2] || GetFileAttributesW(fields[2]) == INVALID_FILE_ATTRIBUTES) && GetFileAttributesW(fields[0]) == INVALID_FILE_ATTRIBUTES)
		goto done;
	full = adv_full_path(fields[0]);
	parent = adv_parent(full);
	if (!parent)
	{
		result = E_OUTOFMEMORY;
		goto done;
	}
	if (fields[2] && fields[2][0] && GetFileAttributesW(fields[2]) != INVALID_FILE_ATTRIBUTES)
	{
		result = adv_extract_cab(fields[2], parent, NULL);
		if (FAILED(result))
			goto done;
	}
	result = install_inf(window, full, fields[1], parent, fields[3] ? wcstol(fields[3], NULL, 10) : 0);
done:
	adv_free(parent);
	adv_free(full);
	adv_free(copy);
	return result;
}

HRESULT WINAPI LaunchINFSectionExA(HWND window, HINSTANCE instance, char *line, INT show)
{
	WCHAR *wide = adv_wide(line);
	HRESULT result = wide || !line ? LaunchINFSectionExW(window, instance, wide, show) : E_OUTOFMEMORY;
	adv_free(wide);
	return result;
}

HRESULT WINAPI ExecuteCabW(HWND window, cab_info_w *info, void *reserved)
{
	WCHAR *path;
	HRESULT result;
	if (!info || !info->inf)
		return E_INVALIDARG;
	if (info->cab && info->cab[0])
	{
		result = adv_extract_cab(info->cab, info->source, NULL);
		if (FAILED(result))
			return result;
	}
	path = adv_join(info->source, info->inf);
	if (!path)
		return E_OUTOFMEMORY;
	result = install_inf(window, path, info->section, info->source, info->flags);
	adv_free(path);
	return result;
}

HRESULT WINAPI ExecuteCabA(HWND window, cab_info_a *info, void *reserved)
{
	cab_info_w wide;
	HRESULT result;
	if (!info)
		return E_INVALIDARG;
	wide.cab = adv_wide(info->cab);
	wide.inf = adv_wide(info->inf);
	wide.section = adv_wide(info->section);
	wide.flags = info->flags;
	MultiByteToWideChar(CP_ACP, 0, info->source, -1, wide.source, MAX_PATH);
	result = ExecuteCabW(window, &wide, reserved);
	adv_free(wide.cab);
	adv_free(wide.inf);
	adv_free(wide.section);
	return result;
}

/* The layout setup programs hand DoInfInstall. */
typedef struct
{
	HWND window;
	const void *title;
	const void *inf;
	const void *directory;
	const void *section;
} inf_install_params;

HRESULT WINAPI DoInfInstallW(const inf_install_params *params)
{
	if (!params || !params->inf)
		return E_INVALIDARG;
	return install_inf(params->window, params->inf, params->section, params->directory, 0);
}

HRESULT WINAPI DoInfInstall(const inf_install_params *params)
{
	WCHAR *inf, *section, *directory;
	HRESULT result;
	if (!params || !params->inf)
		return E_INVALIDARG;
	inf = adv_wide(params->inf);
	section = adv_wide(params->section);
	directory = adv_wide(params->directory);
	result = install_inf(params->window, inf, section, directory, 0);
	adv_free(inf);
	adv_free(section);
	adv_free(directory);
	return result;
}

HRESULT WINAPI OpenINFEngineW(const WCHAR *path, const WCHAR *section, DWORD flags, HINF *inf, void *reserved)
{
	WCHAR *full;
	HRESULT result;
	if (!path || !inf)
		return E_INVALIDARG;
	if (!path[0])
		return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	full = adv_full_path(path);
	if (!full)
		return E_OUTOFMEMORY;
	result = open_inf(full, inf);
	adv_free(full);
	if (SUCCEEDED(result) && section)
		apply_custom_destinations(*inf, section);
	return result;
}

HRESULT WINAPI OpenINFEngineA(const char *path, const char *section, DWORD flags, HINF *inf, void *reserved)
{
	WCHAR *wide_path = adv_wide(path), *wide_section = adv_wide(section);
	HRESULT result = OpenINFEngineW(wide_path, wide_section, flags, inf, reserved);
	adv_free(wide_path);
	adv_free(wide_section);
	return result;
}

HRESULT WINAPI CloseINFEngine(HINF inf)
{
	if (!inf)
		return E_INVALIDARG;
	SetupCloseInfFile(inf);
	return S_OK;
}

HRESULT WINAPI TranslateInfStringExW(HINF inf, const WCHAR *path, const WCHAR *section, const WCHAR *key, WCHAR *buffer, DWORD size, DWORD *required, void *reserved)
{
	if (!inf || !path || !section || !key)
		return E_INVALIDARG;
	if (!SetupGetLineTextW(NULL, inf, section, key, buffer, size, required))
		return adv_error(GetLastError());
	return S_OK;
}

HRESULT WINAPI TranslateInfStringW(const WCHAR *path, const WCHAR *install, const WCHAR *section, const WCHAR *key, WCHAR *buffer, DWORD size, DWORD *required, void *reserved)
{
	HRESULT result;
	HINF inf;
	if (!path || !section || !key)
		return E_INVALIDARG;
	result = OpenINFEngineW(path, install, 0, &inf, NULL);
	if (FAILED(result))
		return result;
	result = TranslateInfStringExW(inf, path, section, key, buffer, size, required, reserved);
	SetupCloseInfFile(inf);
	return result;
}

/* The ANSI forms measure the converted text, so they translate to a wide buffer first. */
static HRESULT ansi_result(HRESULT result, const WCHAR *text, char *buffer, DWORD size, DWORD *required)
{
	int needed;
	if (FAILED(result))
		return result;
	needed = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
	if (required)
		*required = needed;
	if (!buffer)
		return S_OK;
	if ((DWORD)needed > size)
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	WideCharToMultiByte(CP_ACP, 0, text, -1, buffer, size, NULL, NULL);
	return S_OK;
}

HRESULT WINAPI TranslateInfStringExA(HINF inf, const char *path, const char *section, const char *key, char *buffer, DWORD size, DWORD *required, void *reserved)
{
	WCHAR *wide_path = adv_wide(path), *wide_section = adv_wide(section), *wide_key = adv_wide(key), *text = adv_alloc(MAX_INF_STRING_LENGTH * sizeof(WCHAR));
	HRESULT result = text ? TranslateInfStringExW(inf, wide_path, wide_section, wide_key, text, MAX_INF_STRING_LENGTH, NULL, reserved) : E_OUTOFMEMORY;
	result = ansi_result(result, text, buffer, size, required);
	adv_free(wide_path);
	adv_free(wide_section);
	adv_free(wide_key);
	adv_free(text);
	return result;
}

HRESULT WINAPI TranslateInfStringA(const char *path, const char *install, const char *section, const char *key, char *buffer, DWORD size, DWORD *required, void *reserved)
{
	WCHAR *wide_path = adv_wide(path), *wide_install = adv_wide(install), *wide_section = adv_wide(section), *wide_key = adv_wide(key), *text = adv_alloc(MAX_INF_STRING_LENGTH * sizeof(WCHAR));
	HRESULT result = text ? TranslateInfStringW(wide_path, wide_install, wide_section, wide_key, text, MAX_INF_STRING_LENGTH, NULL, reserved) : E_OUTOFMEMORY;
	result = ansi_result(result, text, buffer, size, required);
	adv_free(wide_path);
	adv_free(wide_install);
	adv_free(wide_section);
	adv_free(wide_key);
	adv_free(text);
	return result;
}

/* Appends to the UTF-16 text of the INF built for RegInstall. */
static BOOL write_text(HANDLE file, const WCHAR *text)
{
	DWORD written;
	return WriteFile(file, text, (DWORD)wcslen(text) * sizeof(WCHAR), &written, NULL);
}

static void write_string(HANDLE file, const WCHAR *name, const WCHAR *value)
{
	WCHAR one[2] = {0, 0};
	write_text(file, name);
	write_text(file, L"=\"");
	for (; *value; value++)
	{
		one[0] = *value;
		write_text(file, one);
		if (*value == L'"')
			write_text(file, one);
	}
	write_text(file, L"\"\r\n");
}

/* The system-relative form replaces the Windows directory, for REG_EXPAND_SZ values. */
static void write_module_strings(HANDLE file, HMODULE module)
{
	WCHAR path[MAX_PATH], windows[MAX_PATH], system_path[MAX_PATH + 16];
	UINT length;
	GetModuleFileNameW(module, path, MAX_PATH);
	write_string(file, L"_MOD_PATH", path);
	length = GetWindowsDirectoryW(windows, MAX_PATH);
	lstrcpyW(system_path, path);
	if (length && wcslen(path) > length && path[length] == L'\\')
	{
		system_path[length] = 0;
		if (!lstrcmpiW(system_path, windows))
		{
			lstrcpyW(system_path, L"%%SystemRoot%%");
			lstrcatW(system_path, path + length);
		}
		else
			lstrcpyW(system_path, path);
	}
	write_string(file, L"_SYS_MOD_PATH", system_path);
}

typedef void (*table_writer)(HANDLE file, const void *table);

static void write_wide_table(HANDLE file, const void *table)
{
	const struct { DWORD count; struct { WCHAR *name, *value; } *entries; } *strings = table;
	DWORD index;
	for (index = 0; index < strings->count; index++)
		write_string(file, strings->entries[index].name, strings->entries[index].value);
}

static void write_ansi_table(HANDLE file, const void *table)
{
	const struct { DWORD count; struct { char *name, *value; } *entries; } *strings = table;
	DWORD index;
	for (index = 0; index < strings->count; index++)
	{
		WCHAR *name = adv_wide(strings->entries[index].name), *value = adv_wide(strings->entries[index].value);
		if (name && value)
			write_string(file, name, value);
		adv_free(name);
		adv_free(value);
	}
}

/* The module's REGINST resource is an INF; caller strings arrive as an extra [Strings] part. */
static HRESULT reg_install(HMODULE module, const WCHAR *section, table_writer writer, const void *table)
{
	WCHAR directory[MAX_PATH], path[MAX_PATH], *text;
	DWORD size, written, length;
	const BYTE *data;
	HRESULT result;
	HANDLE file;
	HRSRC resource;
	if (!section)
		return E_INVALIDARG;
	resource = FindResourceW(module, L"REGINST", L"REGINST");
	if (!resource)
		return adv_error(GetLastError());
	size = SizeofResource(module, resource);
	data = LockResource(LoadResource(module, resource));
	if (!data || !GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"adv", 0, path))
		return adv_error(GetLastError());
	file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return adv_error(GetLastError());
	WriteFile(file, L"\xfeff", sizeof(WCHAR), &written, NULL);
	if (size >= 2 && data[0] == 0xff && data[1] == 0xfe)
		WriteFile(file, data + 2, size - 2, &written, NULL);
	else
	{
		length = MultiByteToWideChar(CP_ACP, 0, (const char *)data, size, NULL, 0);
		text = adv_alloc((length + 1) * sizeof(WCHAR));
		if (text)
		{
			MultiByteToWideChar(CP_ACP, 0, (const char *)data, size, text, length);
			WriteFile(file, text, length * sizeof(WCHAR), &written, NULL);
			adv_free(text);
		}
	}
	write_text(file, L"\r\n[Strings]\r\n");
	write_module_strings(file, module);
	if (table)
		writer(file, table);
	CloseHandle(file);
	result = install_inf(NULL, path, section, directory, INSTALL_QUIET);
	DeleteFileW(path);
	return result;
}

HRESULT WINAPI RegInstallW(HMODULE module, const WCHAR *section, const void *table)
{
	return reg_install(module, section, write_wide_table, table);
}

HRESULT WINAPI RegInstallA(HMODULE module, const char *section, const void *table)
{
	WCHAR *wide = adv_wide(section);
	HRESULT result = reg_install(module, wide, write_ansi_table, table);
	adv_free(wide);
	return result;
}

/* "library,mode,arguments" as rundll32 hands it over. */
HRESULT WINAPI RegisterOCXW(HWND window, HINSTANCE instance, const WCHAR *line, INT show)
{
	WCHAR *copy, *fields[3] = {NULL};
	HRESULT result;
	if (!line)
		return E_INVALIDARG;
	copy = adv_copy(line);
	if (!copy)
		return E_OUTOFMEMORY;
	adv_split(copy, fields, 3);
	result = fields[0] ? register_library(fields[0], fields[1] ? fields[1] : L"", fields[2] ? fields[2] : L"", TRUE) : E_INVALIDARG;
	adv_free(copy);
	return result;
}

HRESULT WINAPI RegisterOCX(HWND window, HINSTANCE instance, const char *line, INT show)
{
	WCHAR *wide = adv_wide(line);
	HRESULT result = RegisterOCXW(window, instance, wide, show);
	adv_free(wide);
	return result;
}
