/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "advpack.h"

#define WRAPPER_COMMAND L"rundll32.exe advpack.dll,UserInstStubWrapper "

static const WCHAR components_key[] = L"SOFTWARE\\Microsoft\\Active Setup\\Installed Components";
static const WCHAR session_key[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}

void *adv_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void adv_free(void *memory)
{
	if (memory)
		HeapFree(GetProcessHeap(), 0, memory);
}

WCHAR *adv_wide(const char *text)
{
	WCHAR *wide;
	int length;
	if (!text)
		return NULL;
	length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	wide = adv_alloc(length * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

/* A list of strings ended by an empty one. */
WCHAR *adv_wide_list(const char *list)
{
	const char *end = list;
	WCHAR *wide;
	int length;
	if (!list)
		return NULL;
	while (*end)
		end += strlen(end) + 1;
	length = MultiByteToWideChar(CP_ACP, 0, list, (int)(end - list) + 1, NULL, 0);
	wide = adv_alloc(length * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, list, (int)(end - list) + 1, wide, length);
	return wide;
}

WCHAR *adv_copy(const WCHAR *text)
{
	WCHAR *copy;
	if (!text)
		return NULL;
	copy = adv_alloc((wcslen(text) + 1) * sizeof(WCHAR));
	if (copy)
		lstrcpyW(copy, text);
	return copy;
}

WCHAR *adv_join(const WCHAR *left, const WCHAR *right)
{
	size_t length = wcslen(left);
	WCHAR *joined = adv_alloc((length + wcslen(right) + 2) * sizeof(WCHAR));
	if (!joined)
		return NULL;
	lstrcpyW(joined, left);
	if (length && left[length - 1] != L'\\' && left[length - 1] != L'/')
		lstrcatW(joined, L"\\");
	lstrcatW(joined, right);
	return joined;
}

WCHAR *adv_full_path(const WCHAR *path)
{
	DWORD length;
	WCHAR *full;
	if (!path[0])
		return adv_copy(path);
	length = GetFullPathNameW(path, 0, NULL, NULL);
	if (!length)
		return adv_copy(path);
	full = adv_alloc((length + 1) * sizeof(WCHAR));
	if (full)
		GetFullPathNameW(path, length + 1, full, NULL);
	return full;
}

WCHAR *adv_parent(const WCHAR *path)
{
	WCHAR *parent, *slash;
	if (!path)
		return NULL;
	parent = adv_copy(path);
	if (!parent)
		return NULL;
	slash = wcsrchr(parent, L'\\');
	if (slash)
		slash[slash > parent && slash[-1] == L':' ? 1 : 0] = 0;
	else
		parent[0] = 0;
	return parent;
}

HRESULT adv_error(DWORD error)
{
	if ((error & 0xE0000000) == 0xE0000000)
		return (HRESULT)(0x800F0000 | (error & 0xFFFF));
	return HRESULT_FROM_WIN32(error);
}

HRESULT adv_run_wait(const WCHAR *command, const WCHAR *directory, BOOL hidden)
{
	PROCESS_INFORMATION info;
	STARTUPINFOW startup;
	WCHAR *line = adv_copy(command);
	DWORD error;
	if (!line)
		return E_OUTOFMEMORY;
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	if (hidden)
	{
		startup.dwFlags = STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_HIDE;
	}
	if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, directory && directory[0] ? directory : NULL, &startup, &info))
	{
		error = GetLastError();
		adv_free(line);
		return HRESULT_FROM_WIN32(error);
	}
	adv_free(line);
	WaitForSingleObject(info.hProcess, INFINITE);
	CloseHandle(info.hProcess);
	CloseHandle(info.hThread);
	return S_OK;
}

DWORD adv_split(WCHAR *text, WCHAR **fields, DWORD count)
{
	WCHAR *read = text, *write = text;
	DWORD used = 0;
	BOOL quoted;
	while (used < count)
	{
		while (*read == L' ' || *read == L'\t')
			read++;
		fields[used++] = write;
		quoted = FALSE;
		while (*read && (quoted || *read != L','))
		{
			if (*read == L'"')
				quoted = !quoted;
			else
				*write++ = *read;
			read++;
		}
		if (!*read)
		{
			*write = 0;
			return used;
		}
		read++;
		*write++ = 0;
	}
	return used;
}

HRESULT WINAPI GetVersionFromFileExW(const WCHAR *path, DWORD *major, DWORD *minor, BOOL file_version)
{
	struct { WORD language, code_page; } *translation;
	VS_FIXEDFILEINFO *fixed;
	DWORD size, unused;
	void *data;
	UINT length;
	if (!major || !minor)
		return E_INVALIDARG;
	*major = *minor = 0;
	if (!path)
		return S_OK;
	size = GetFileVersionInfoSizeW(path, &unused);
	if (!size)
		return S_OK;
	data = adv_alloc(size);
	if (!data)
		return E_OUTOFMEMORY;
	if (GetFileVersionInfoW(path, 0, size, data))
	{
		if (file_version)
		{
			if (VerQueryValueW(data, L"\\", (void **)&fixed, &length))
			{
				*major = fixed->dwFileVersionMS;
				*minor = fixed->dwFileVersionLS;
			}
		}
		else if (VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void **)&translation, &length) && length)
		{
			*major = translation->language;
			*minor = translation->code_page;
		}
	}
	adv_free(data);
	return S_OK;
}

HRESULT WINAPI GetVersionFromFileExA(const char *path, DWORD *major, DWORD *minor, BOOL file_version)
{
	WCHAR *wide = adv_wide(path);
	HRESULT result = GetVersionFromFileExW(wide, major, minor, file_version);
	adv_free(wide);
	return result;
}

HRESULT WINAPI GetVersionFromFileW(const WCHAR *path, DWORD *major, DWORD *minor, BOOL file_version)
{
	return GetVersionFromFileExW(path, major, minor, file_version);
}

HRESULT WINAPI GetVersionFromFileA(const char *path, DWORD *major, DWORD *minor, BOOL file_version)
{
	return GetVersionFromFileExA(path, major, minor, file_version);
}

BOOL WINAPI IsNTAdmin(DWORD reserved, DWORD *unused)
{
	SID_IDENTIFIER_AUTHORITY authority = {SECURITY_NT_AUTHORITY};
	BOOL member = FALSE;
	PSID administrators;
	if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators))
		return FALSE;
	if (!CheckTokenMembership(NULL, administrators, &member))
		member = FALSE;
	FreeSid(administrators);
	return member;
}

/* Pending renames are what a reboot would finish; their size tells when more arrive. */
static DWORD pending_renames(void)
{
	DWORD size = 0;
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, session_key, 0, KEY_READ, &key))
		return 0;
	if (RegQueryValueExW(key, L"PendingFileRenameOperations", NULL, NULL, NULL, &size))
		size = 0;
	RegCloseKey(key);
	return size;
}

DWORD WINAPI NeedRebootInit(void)
{
	return pending_renames();
}

BOOL WINAPI NeedReboot(DWORD check)
{
	return pending_renames() != check;
}

HRESULT WINAPI RebootCheckOnInstallW(HWND window, const WCHAR *inf, const WCHAR *section, DWORD reserved)
{
	return pending_renames() ? S_OK : S_FALSE;
}

HRESULT WINAPI RebootCheckOnInstallA(HWND window, const char *inf, const char *section, DWORD reserved)
{
	return pending_renames() ? S_OK : S_FALSE;
}

static void set_text(HKEY key, const WCHAR *name, const WCHAR *value)
{
	RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)(wcslen(value) + 1) * sizeof(WCHAR));
}

/* Moves a current value aside under its Old name. */
static void keep_old(HKEY key, const WCHAR *name, const WCHAR *old_name)
{
	WCHAR value[MAX_PATH * 4];
	DWORD size = sizeof(value) - sizeof(WCHAR), type;
	if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &size) || type != REG_SZ)
		return;
	value[size / sizeof(WCHAR)] = 0;
	set_text(key, old_name, value);
}

HRESULT WINAPI SetPerUserSecValuesW(per_user_w *values)
{
	WCHAR *path, *wrapper;
	HRESULT result = S_OK;
	HKEY key;
	LONG status;
	if (!values)
		return E_INVALIDARG;
	if (!values->guid[0])
		return S_OK;
	path = adv_join(components_key, values->guid);
	if (!path)
		return E_OUTOFMEMORY;
	status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL);
	adv_free(path);
	if (status)
		return HRESULT_FROM_WIN32(status);
	if (values->rollback)
	{
		keep_old(key, NULL, L"OldDisplayName");
		keep_old(key, L"Locale", L"OldLocale");
		keep_old(key, L"StubPath", L"OldStubPath");
		keep_old(key, L"Version", L"OldVersion");
	}
	if (values->name[0])
		set_text(key, NULL, values->name);
	if (values->locale[0])
		set_text(key, L"Locale", values->locale);
	if (values->version[0])
		set_text(key, L"Version", values->version);
	if (values->component[0])
		set_text(key, L"ComponentID", values->component);
	if (values->stub[0])
	{
		if (values->rollback)
		{
			set_text(key, L"RealStubPath", values->stub);
			wrapper = adv_alloc((wcslen(WRAPPER_COMMAND) + wcslen(values->guid) + 1) * sizeof(WCHAR));
			if (wrapper)
			{
				lstrcpyW(wrapper, WRAPPER_COMMAND);
				lstrcatW(wrapper, values->guid);
				set_text(key, L"StubPath", wrapper);
				adv_free(wrapper);
			}
			else
				result = E_OUTOFMEMORY;
		}
		else
			set_text(key, L"StubPath", values->stub);
	}
	RegSetValueExW(key, L"IsInstalled", 0, REG_DWORD, (const BYTE *)&values->installed, sizeof(DWORD));
	RegCloseKey(key);
	return result;
}

static void narrow_field(const char *source, WCHAR *target, int size)
{
	MultiByteToWideChar(CP_ACP, 0, source, -1, target, size);
	target[size - 1] = 0;
}

HRESULT WINAPI SetPerUserSecValuesA(per_user_a *values)
{
	per_user_w *wide;
	HRESULT result;
	if (!values)
		return E_INVALIDARG;
	wide = adv_alloc(sizeof(*wide));
	if (!wide)
		return E_OUTOFMEMORY;
	narrow_field(values->guid, wide->guid, ARRAYSIZE(wide->guid));
	narrow_field(values->name, wide->name, ARRAYSIZE(wide->name));
	narrow_field(values->locale, wide->locale, ARRAYSIZE(wide->locale));
	narrow_field(values->stub, wide->stub, ARRAYSIZE(wide->stub));
	narrow_field(values->version, wide->version, ARRAYSIZE(wide->version));
	narrow_field(values->component, wide->component, ARRAYSIZE(wide->component));
	wide->installed = values->installed;
	wide->rollback = values->rollback;
	result = SetPerUserSecValuesW(wide);
	adv_free(wide);
	return result;
}

/* Runs the real stub kept beside a wrapper, reading it from the component's key. */
static HRESULT run_real_stub(const WCHAR *guid)
{
	WCHAR command[MAX_PATH * 4];
	DWORD size = sizeof(command) - sizeof(WCHAR), type;
	WCHAR *path;
	HKEY key;
	LONG status;
	if (!guid || !guid[0])
		return E_INVALIDARG;
	path = adv_join(components_key, guid);
	if (!path)
		return E_OUTOFMEMORY;
	status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key);
	adv_free(path);
	if (status)
		return S_OK;
	status = RegQueryValueExW(key, L"RealStubPath", NULL, &type, (BYTE *)command, &size);
	RegCloseKey(key);
	if (status || !size)
		return S_OK;
	command[size / sizeof(WCHAR)] = 0;
	return adv_run_wait(command, NULL, FALSE);
}

HRESULT WINAPI UserInstStubWrapperW(HWND window, HINSTANCE instance, const WCHAR *guid, INT show)
{
	return run_real_stub(guid);
}

HRESULT WINAPI UserInstStubWrapperA(HWND window, HINSTANCE instance, const char *guid, INT show)
{
	WCHAR *wide = adv_wide(guid);
	HRESULT result = run_real_stub(wide);
	adv_free(wide);
	return result;
}

/* An uninstall stub lives under the component's .Restore key. */
HRESULT WINAPI UserUnInstStubWrapperW(HWND window, HINSTANCE instance, const WCHAR *guid, INT show)
{
	WCHAR *name;
	HRESULT result;
	if (!guid || !guid[0])
		return E_INVALIDARG;
	name = adv_alloc((wcslen(guid) + 9) * sizeof(WCHAR));
	if (!name)
		return E_OUTOFMEMORY;
	lstrcpyW(name, guid);
	lstrcatW(name, L".Restore");
	result = run_real_stub(name);
	adv_free(name);
	return result;
}

HRESULT WINAPI UserUnInstStubWrapperA(HWND window, HINSTANCE instance, const char *guid, INT show)
{
	WCHAR *wide = adv_wide(guid);
	HRESULT result = UserUnInstStubWrapperW(window, instance, wide, show);
	adv_free(wide);
	return result;
}

/* Backups of registry values and files for rollback are not kept: callers hear so plainly. */
HRESULT WINAPI RegSaveRestoreA(HWND window, const char *title, HKEY backup, const char *root, const char *subkey, const char *value, DWORD flags)
{
	return E_NOTIMPL;
}

HRESULT WINAPI RegSaveRestoreW(HWND window, const WCHAR *title, HKEY backup, const WCHAR *root, const WCHAR *subkey, const WCHAR *value, DWORD flags)
{
	return E_NOTIMPL;
}

HRESULT WINAPI RegSaveRestoreOnINFA(HWND window, const char *title, const char *inf, const char *section, HKEY machine, HKEY user, DWORD flags)
{
	return E_NOTIMPL;
}

HRESULT WINAPI RegSaveRestoreOnINFW(HWND window, const WCHAR *title, const WCHAR *inf, const WCHAR *section, HKEY machine, HKEY user, DWORD flags)
{
	return E_NOTIMPL;
}

HRESULT WINAPI RegRestoreAllA(HWND window, const char *title, HKEY backup)
{
	return E_NOTIMPL;
}

HRESULT WINAPI RegRestoreAllW(HWND window, const WCHAR *title, HKEY backup)
{
	return E_NOTIMPL;
}

HRESULT WINAPI FileSaveRestoreA(HWND window, const char *list, const char *directory, const char *base, DWORD flags)
{
	return E_NOTIMPL;
}

HRESULT WINAPI FileSaveRestoreW(HWND window, const WCHAR *list, const WCHAR *directory, const WCHAR *base, DWORD flags)
{
	return E_NOTIMPL;
}

HRESULT WINAPI FileSaveRestoreOnINFA(HWND window, const char *title, const char *inf, const char *section, const char *directory, const char *base, DWORD flags)
{
	return E_NOTIMPL;
}

HRESULT WINAPI FileSaveRestoreOnINFW(HWND window, const WCHAR *title, const WCHAR *inf, const WCHAR *section, const WCHAR *directory, const WCHAR *base, DWORD flags)
{
	return E_NOTIMPL;
}
