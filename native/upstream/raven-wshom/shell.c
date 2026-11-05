/* SPDX-License-Identifier: MIT */
#include "wshom.h"
#include <shellapi.h>
#include <shlobj.h>

/* WScript.Shell: programs started through the shell or with their streams on pipes, message boxes, the registry, the environments, the
 * special folders, shortcuts, the event log, the current directory and keys typed into other windows. */

enum {
	SHELL_RUN = 1, SHELL_EXEC, SHELL_POPUP, SHELL_REGREAD, SHELL_REGWRITE, SHELL_REGDELETE, SHELL_ENVIRONMENT, SHELL_EXPAND, SHELL_SPECIAL,
	SHELL_SHORTCUT, SHELL_LOGEVENT, SHELL_ACTIVATE, SHELL_SENDKEYS, SHELL_DIRECTORY,
};

static const sr_member members[] = {
	{ L"Run", SHELL_RUN, 3 }, { L"Exec", SHELL_EXEC, 1 }, { L"Popup", SHELL_POPUP, 4 }, { L"RegRead", SHELL_REGREAD, 1 },
	{ L"RegWrite", SHELL_REGWRITE, 3 }, { L"RegDelete", SHELL_REGDELETE, 1 }, { L"Environment", SHELL_ENVIRONMENT, 1 },
	{ L"ExpandEnvironmentStrings", SHELL_EXPAND, 1 }, { L"SpecialFolders", SHELL_SPECIAL, 0 }, { L"CreateShortcut", SHELL_SHORTCUT, 1 },
	{ L"LogEvent", SHELL_LOGEVENT, 3 }, { L"AppActivate", SHELL_ACTIVATE, 2 }, { L"SendKeys", SHELL_SENDKEYS, 2 },
	{ L"CurrentDirectory", SHELL_DIRECTORY, 0 }, { NULL },
};

static HRESULT last_error(void) { return HRESULT_FROM_WIN32(GetLastError()); }

static WCHAR *expand(const WCHAR *text)
{
	DWORD size = ExpandEnvironmentStringsW(text, NULL, 0);
	WCHAR *expanded = sr_alloc((size + 1) * sizeof(WCHAR));
	if (!size || !ExpandEnvironmentStringsW(text, expanded, size))
		lstrcpyW(expanded, text);
	return expanded;
}

/* Run: the command with its variables expanded, given to the shell as the program or document it starts with and the rest. */
static HRESULT run(const WCHAR *command, LONG style, BOOL wait, VARIANT *result)
{
	WCHAR *line = expand(command), *file = line, *parameters;
	while (*file == L' ' || *file == L'\t')
		file++;
	if (*file == L'"') {
		file++;
		parameters = file;
		while (*parameters && *parameters != L'"')
			parameters++;
	} else {
		parameters = file;
		while (*parameters && *parameters != L' ' && *parameters != L'\t')
			parameters++;
	}
	if (*parameters)
		*parameters++ = 0;
	while (*parameters == L' ' || *parameters == L'\t')
		parameters++;
	SHELLEXECUTEINFOW info = { sizeof(info) };
	info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
	info.lpFile = file;
	info.lpParameters = *parameters ? parameters : NULL;
	info.nShow = style;
	HRESULT hr = S_OK;
	LONG code = 0;
	if (!ShellExecuteExW(&info))
		hr = last_error();
	else if (info.hProcess) {
		if (wait) {
			DWORD exit = 0;
			WaitForSingleObject(info.hProcess, INFINITE);
			GetExitCodeProcess(info.hProcess, &exit);
			code = (LONG)exit;
		}
		CloseHandle(info.hProcess);
	}
	sr_free(line);
	return SUCCEEDED(hr) ? sr_return_long(result, code) : hr;
}

/* Popup: a message box that closes by itself after the seconds given, answering -1 then. */
static HRESULT popup(const WCHAR *text, LONG seconds, const WCHAR *title, LONG type, VARIANT *result)
{
	typedef int (WINAPI *timed_box)(HWND, LPCWSTR, LPCWSTR, UINT, WORD, DWORD);
	static const UINT timed_out = 32000;
	timed_box timed = (timed_box)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "MessageBoxTimeoutW");
	int answer;
	if (seconds > 0 && timed) {
		answer = timed(NULL, text, title, (UINT)type, 0, (DWORD)seconds * 1000);
		if ((UINT)answer == timed_out)
			answer = -1;
	} else
		answer = MessageBoxW(NULL, text, title, (UINT)type);
	return sr_return_long(result, answer);
}

/* The special folders by the names scripts give them, in the order the collection numbers them. */
static const struct {
	const WCHAR *name;
	int csidl;
} special_folders[] = {
	{ L"AllUsersDesktop", CSIDL_COMMON_DESKTOPDIRECTORY }, { L"AllUsersStartMenu", CSIDL_COMMON_STARTMENU },
	{ L"AllUsersPrograms", CSIDL_COMMON_PROGRAMS }, { L"AllUsersStartup", CSIDL_COMMON_STARTUP }, { L"Desktop", CSIDL_DESKTOPDIRECTORY },
	{ L"Favorites", CSIDL_FAVORITES }, { L"Fonts", CSIDL_FONTS }, { L"MyDocuments", CSIDL_PERSONAL }, { L"NetHood", CSIDL_NETHOOD },
	{ L"PrintHood", CSIDL_PRINTHOOD }, { L"Programs", CSIDL_PROGRAMS }, { L"Recent", CSIDL_RECENT }, { L"SendTo", CSIDL_SENDTO },
	{ L"StartMenu", CSIDL_STARTMENU }, { L"Startup", CSIDL_STARTUP }, { L"Templates", CSIDL_TEMPLATES },
};

static HRESULT special(VARIANT *result)
{
	VARIANT items[ARRAYSIZE(special_folders)];
	const WCHAR *names[ARRAYSIZE(special_folders)];
	for (UINT i = 0; i < ARRAYSIZE(special_folders); i++) {
		WCHAR path[MAX_PATH] = L"";
		if (FAILED(SHGetFolderPathW(NULL, special_folders[i].csidl, NULL, SHGFP_TYPE_CURRENT, path)))
			path[0] = 0;
		V_VT(&items[i]) = VT_BSTR;
		V_BSTR(&items[i]) = SysAllocString(path);
		names[i] = special_folders[i].name;
	}
	IDispatch *folders = wsh_collection_new(items, names, ARRAYSIZE(special_folders));
	for (UINT i = 0; i < ARRAYSIZE(special_folders); i++)
		VariantClear(&items[i]);
	return sr_return_object(result, folders);
}

/* AppActivate: the window of a process, or the one whose title is the text, starts with it or ends with it, in that order. */
typedef struct {
	const WCHAR *title;
	DWORD process;
	HWND found[3];
} activation;

static BOOL CALLBACK find_window(HWND window, LPARAM parameter)
{
	activation *a = (activation *)parameter;
	if (!IsWindowVisible(window))
		return TRUE;
	if (!a->title) {
		DWORD process = 0;
		GetWindowThreadProcessId(window, &process);
		if (process == a->process) {
			a->found[0] = window;
			return FALSE;
		}
		return TRUE;
	}
	WCHAR text[512];
	int length = GetWindowTextW(window, text, ARRAYSIZE(text)), wanted = lstrlenW(a->title);
	if (!length || length < wanted)
		return TRUE;
	if (CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, text, length, a->title, wanted) == CSTR_EQUAL) {
		a->found[0] = window;
		return FALSE;
	}
	if (!a->found[1] && CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, text, wanted, a->title, wanted) == CSTR_EQUAL)
		a->found[1] = window;
	if (!a->found[2] && CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, text + length - wanted, wanted, a->title, wanted) == CSTR_EQUAL)
		a->found[2] = window;
	return TRUE;
}

static HRESULT activate(VARIANT *which, VARIANT *result)
{
	activation a = { 0 };
	BSTR title = NULL;
	HRESULT hr;
	if (!which)
		return DISP_E_PARAMNOTFOUND;
	if (V_VT(which) == VT_BSTR)
		hr = sr_text(which, &title);
	else
		hr = sr_long(which, (LONG *)&a.process);
	if (FAILED(hr))
		return hr;
	a.title = title;
	EnumWindows(find_window, (LPARAM)&a);
	HWND window = a.found[0] ? a.found[0] : a.found[1] ? a.found[1] : a.found[2];
	SysFreeString(title);
	if (window) {
		if (IsIconic(window))
			ShowWindow(window, SW_RESTORE);
		SetForegroundWindow(window);
	}
	return sr_return_bool(result, window != NULL);
}

/* LogEvent: the types are the event log's own, success 0, error 1, warning 2, information 4, audits 8 and 16. */
static HRESULT log_event(LONG type, const WCHAR *message, const WCHAR *target, VARIANT *result)
{
	HANDLE source = RegisterEventSourceW(target && *target ? target : NULL, L"WSH");
	BOOL logged = FALSE;
	if (source) {
		logged = ReportEventW(source, (WORD)type, 0, 0, NULL, 1, 0, &message, NULL);
		DeregisterEventSource(source);
	}
	return sr_return_bool(result, logged);
}

static HRESULT shell_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	BOOL put = sr_is_put(flags);
	BSTR first = NULL, second = NULL;
	LONG number = 0, other = 0;
	BOOL flag = FALSE;
	HRESULT hr = S_OK;
	if (put && id != SHELL_DIRECTORY)
		return DISP_E_MEMBERNOTFOUND;
	switch (id) {
	case SHELL_RUN:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &first)) && SUCCEEDED(hr = sr_long_or(sr_arg(params, flags, 1), SW_SHOWNORMAL, &number)) &&
			SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 2), FALSE, &flag)))
			hr = run(first, number, flag, result);
		break;
	case SHELL_EXEC:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &first)))
			hr = wsh_exec(first, result);
		break;
	case SHELL_POPUP:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &first)) && SUCCEEDED(hr = sr_long_or(sr_arg(params, flags, 1), 0, &number)) &&
			SUCCEEDED(hr = sr_long_or(sr_arg(params, flags, 3), 0, &other))) {
			VARIANT *title = sr_arg(params, flags, 2);
			if (!title || SUCCEEDED(hr = sr_text(title, &second)))
				hr = popup(first, number, second ? second : L"Windows Script Host", other, result);
		}
		break;
	case SHELL_REGREAD:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &first)))
			hr = wsh_reg_read(first, result);
		break;
	case SHELL_REGWRITE:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &first))) {
			VARIANT *type = sr_arg(params, flags, 2);
			if (!type || SUCCEEDED(hr = sr_text(type, &second)))
				hr = wsh_reg_write(first, sr_arg(params, flags, 1), second);
		}
		break;
	case SHELL_REGDELETE:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &first)))
			hr = wsh_reg_delete(first);
		break;
	case SHELL_ENVIRONMENT: {
		VARIANT *kind = sr_arg(params, flags, 0);
		if (!kind || SUCCEEDED(hr = sr_text(kind, &first)))
			hr = wsh_environment_new(first, result);
		break;
	}
	case SHELL_EXPAND:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &first))) {
			WCHAR *expanded = expand(first);
			hr = sr_return_text(result, expanded);
			sr_free(expanded);
		}
		break;
	case SHELL_SPECIAL:
		return special(result);
	case SHELL_SHORTCUT:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &first)))
			hr = wsh_shortcut_new(first, result);
		break;
	case SHELL_LOGEVENT:
		if (SUCCEEDED(hr = sr_long(sr_arg(params, flags, 0), &number)) && SUCCEEDED(hr = sr_text(sr_arg(params, flags, 1), &first))) {
			VARIANT *target = sr_arg(params, flags, 2);
			if (!target || SUCCEEDED(hr = sr_text(target, &second)))
				hr = log_event(number, first, second, result);
		}
		break;
	case SHELL_ACTIVATE:
		return activate(sr_arg(params, flags, 0), result);
	case SHELL_SENDKEYS:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &first)))
			hr = wsh_send_keys(first);
		break;
	case SHELL_DIRECTORY:
		if (put) {
			if (SUCCEEDED(hr = sr_text(sr_put_value(params), &first)) && !SetCurrentDirectoryW(first))
				hr = last_error();
		} else {
			DWORD size = GetCurrentDirectoryW(0, NULL);
			WCHAR *directory = sr_alloc((size + 1) * sizeof(WCHAR));
			GetCurrentDirectoryW(size, directory);
			hr = sr_return_text(result, directory);
			sr_free(directory);
		}
		break;
	default:
		return DISP_E_MEMBERNOTFOUND;
	}
	SysFreeString(first);
	SysFreeString(second);
	return hr;
}

static const sr_class shell_class = { members, shell_invoke, NULL };

IDispatch *wsh_shell_new(void) { return sr_dispatch(sr_object_new(&shell_class, sizeof(sr_object))); }
