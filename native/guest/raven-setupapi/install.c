/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "setup.h"
#include <objbase.h>
#include <shlobj.h>
#include <shlguid.h>
#include <string.h>

/* Install sections are directive lines: each names further sections to process with a handler.
 * The sections are listed in the directive's fields, and a directive may repeat. */

typedef BOOL (*section_handler)(HINF hinf, const WCHAR *section, void *argument);

static BOOL for_each_directive(HINF hinf, const WCHAR *install, const WCHAR *directive, section_handler handler, void *argument)
{
	INFCONTEXT context;
	BOOL ok;

	if (!SetupFindFirstLineW(hinf, install, directive, &context))
		return TRUE;
	do {
		DWORD count = SetupGetFieldCount(&context), at;
		for (at = 1; at <= count; at++) {
			const WCHAR *name = pSetupGetField(&context, at);
			if (name && *name && !handler(hinf, name, argument))
				return FALSE;
		}
		ok = SetupFindNextMatchLineW(&context, directive, &context);
	} while (ok);
	return TRUE;
}

/* Registry. */

static HKEY root_key_of(const WCHAR *name, HKEY relative)
{
	if (!lstrcmpiW(name, L"HKCR") || !lstrcmpiW(name, L"HKEY_CLASSES_ROOT"))
		return HKEY_CLASSES_ROOT;
	if (!lstrcmpiW(name, L"HKCU") || !lstrcmpiW(name, L"HKEY_CURRENT_USER"))
		return HKEY_CURRENT_USER;
	if (!lstrcmpiW(name, L"HKLM") || !lstrcmpiW(name, L"HKEY_LOCAL_MACHINE"))
		return HKEY_LOCAL_MACHINE;
	if (!lstrcmpiW(name, L"HKU") || !lstrcmpiW(name, L"HKEY_USERS"))
		return HKEY_USERS;
	if (!lstrcmpiW(name, L"HKR"))
		return relative;
	return NULL;
}

#define REG_FLAG(flags, flag) (((flags) & (flag)) == (flag))

static BYTE *binary_from_fields(INFCONTEXT *context, DWORD first, DWORD *size)
{
	DWORD count = SetupGetFieldCount(context), wanted = count >= first ? count - first + 1 : 0;
	BYTE *data = setup_alloc(wanted ? wanted : 1);

	if (data && wanted && !SetupGetBinaryField(context, first, data, wanted, NULL)) {
		/* Bad values do not stop the rest of the section. */
		wanted = 0;
	}
	*size = wanted;
	return data;
}

/* A multi string value may be appended to; entries already there stay and duplicates do not repeat. */
static BOOL append_multi_sz(HKEY key, const WCHAR *value, const WCHAR *addition)
{
	DWORD type, size = 0, used, length = lstrlenW(addition) + 1;
	WCHAR *text, *at;
	BOOL result;

	if (RegQueryValueExW(key, value, NULL, &type, NULL, &size) != ERROR_SUCCESS) {
		size = 0;
	} else if (type != REG_MULTI_SZ) {
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	text = setup_alloc(size + (SIZE_T)length * sizeof(WCHAR) + 2 * sizeof(WCHAR));
	if (!text)
		return FALSE;
	if (size && RegQueryValueExW(key, value, NULL, &type, (BYTE *)text, &size) != ERROR_SUCCESS)
		size = 0;
	for (at = text; *at; at += lstrlenW(at) + 1) {
		if (!lstrcmpiW(at, addition)) {
			setup_free(text);
			return TRUE;
		}
	}
	used = (DWORD)(at - text);
	memcpy(text + used, addition, length * sizeof(WCHAR));
	text[used + length] = 0;
	result = RegSetValueExW(key, value, 0, REG_MULTI_SZ, (BYTE *)text, (used + length + 1) * sizeof(WCHAR)) == ERROR_SUCCESS;
	setup_free(text);
	return result;
}

static BOOL write_value(HKEY key, const WCHAR *value, DWORD flags, INFCONTEXT *context)
{
	DWORD base_type = flags & FLG_ADDREG_TYPE_MASK;

	if (flags & FLG_ADDREG_KEYONLY)
		return TRUE;
	if ((flags & FLG_ADDREG_NOCLOBBER) && RegQueryValueExW(key, value, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
		return TRUE;
	if ((flags & FLG_ADDREG_OVERWRITEONLY) && RegQueryValueExW(key, value, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
		return TRUE;
	if (flags & FLG_ADDREG_APPEND) {
		DWORD count = SetupGetFieldCount(context), at;
		for (at = 5; at <= count; at++) {
			const WCHAR *text = pSetupGetField(context, at);
			if (text && *text && !append_multi_sz(key, value, text))
				return FALSE;
		}
		return TRUE;
	}
	if (flags & FLG_ADDREG_BINVALUETYPE) {
		DWORD size = 0, type;
		BYTE *data = binary_from_fields(context, 5, &size);

		type = base_type == FLG_ADDREG_TYPE_DWORD && size == sizeof(DWORD) ? REG_DWORD : (base_type == FLG_ADDREG_TYPE_NONE ? REG_NONE : REG_BINARY);
		if (base_type != FLG_ADDREG_TYPE_DWORD && base_type != FLG_ADDREG_TYPE_NONE && (flags >> 16) > 1)
			type = flags >> 16;
		if (base_type == FLG_ADDREG_TYPE_DWORD && SetupGetFieldCount(context) == 5) {
			INT number = 0;
			setup_free(data);
			data = setup_alloc(sizeof(DWORD));
			SetupGetIntField(context, 5, &number);
			if (data)
				memcpy(data, &number, sizeof(number));
			size = sizeof(DWORD);
			type = REG_DWORD;
		}
		if (data)
			RegSetValueExW(key, value, 0, type, data, size);
		setup_free(data);
		return TRUE;
	}
	if (base_type == FLG_ADDREG_TYPE_MULTI_SZ) {
		DWORD chars = 0;
		WCHAR *text;
		if (SetupGetFieldCount(context) >= 5 && SetupGetMultiSzFieldW(context, 5, NULL, 0, &chars)) {
			text = setup_alloc((SIZE_T)chars * sizeof(WCHAR));
			if (text && SetupGetMultiSzFieldW(context, 5, text, chars, NULL))
				RegSetValueExW(key, value, 0, REG_MULTI_SZ, (BYTE *)text, chars * sizeof(WCHAR));
			setup_free(text);
		} else {
			static const WCHAR empty[2] = { 0, 0 };
			RegSetValueExW(key, value, 0, REG_MULTI_SZ, (const BYTE *)empty, sizeof(empty));
		}
		return TRUE;
	}
	{
		const WCHAR *text = SetupGetFieldCount(context) >= 5 ? pSetupGetField(context, 5) : L"";
		DWORD type = base_type == FLG_ADDREG_TYPE_EXPAND_SZ ? REG_EXPAND_SZ : REG_SZ;
		if (base_type == FLG_ADDREG_TYPE_NONE) {
			RegSetValueExW(key, value, 0, REG_NONE, NULL, 0);
			return TRUE;
		}
		if (!text)
			text = L"";
		if (base_type == FLG_ADDREG_TYPE_DWORD) {
			INT number = 0;
			SetupGetIntField(context, 5, &number);
			RegSetValueExW(key, value, 0, REG_DWORD, (BYTE *)&number, sizeof(number));
			return TRUE;
		}
		RegSetValueExW(key, value, 0, type, (const BYTE *)text, (lstrlenW(text) + 1) * sizeof(WCHAR));
	}
	return TRUE;
}

static BOOL add_registry_section(HINF hinf, const WCHAR *section, void *argument)
{
	HKEY relative = argument;
	INFCONTEXT context;

	if (!SetupFindFirstLineW(hinf, section, NULL, &context))
		return TRUE;
	do {
		const WCHAR *root_name = pSetupGetField(&context, 1), *subkey = pSetupGetField(&context, 2), *value = SetupGetFieldCount(&context) >= 3 ? pSetupGetField(&context, 3) : NULL;
		INT flags = 0;
		HKEY root, key;
		REGSAM view = 0;

		if (!root_name || !(root = root_key_of(root_name, relative)))
			continue;
		if (SetupGetFieldCount(&context) >= 4)
			SetupGetIntField(&context, 4, &flags);
		if (flags & FLG_ADDREG_64BITKEY)
			view = KEY_WOW64_64KEY;
		else if (flags & FLG_ADDREG_32BITKEY)
			view = KEY_WOW64_32KEY;
		if (flags & FLG_ADDREG_DELVAL) {
			if (RegOpenKeyExW(root, subkey ? subkey : L"", 0, KEY_ALL_ACCESS | view, &key) == ERROR_SUCCESS) {
				RegDeleteValueW(key, value);
				RegCloseKey(key);
			}
			continue;
		}
		if (flags & FLG_ADDREG_OVERWRITEONLY) {
			if (RegOpenKeyExW(root, subkey ? subkey : L"", 0, KEY_ALL_ACCESS | view, &key) != ERROR_SUCCESS)
				continue;
		} else if (RegCreateKeyExW(root, subkey ? subkey : L"", 0, NULL, 0, KEY_ALL_ACCESS | view, NULL, &key, NULL) != ERROR_SUCCESS) {
			continue;
		}
		if (!write_value(key, value && *value ? value : NULL, (DWORD)flags, &context)) {
			DWORD error = GetLastError();
			RegCloseKey(key);
			SetLastError(error);
			return FALSE;
		}
		RegCloseKey(key);
	} while (SetupFindNextLine(&context, &context));
	return TRUE;
}

static void delete_from_multi_sz(HKEY key, const WCHAR *value, const WCHAR *entry)
{
	DWORD type, size = 0;
	WCHAR *text, *at, *out;

	if (RegQueryValueExW(key, value, NULL, &type, NULL, &size) != ERROR_SUCCESS || type != REG_MULTI_SZ)
		return;
	text = setup_alloc(size + 2 * sizeof(WCHAR));
	if (!text)
		return;
	if (RegQueryValueExW(key, value, NULL, &type, (BYTE *)text, &size) == ERROR_SUCCESS) {
		for (at = out = text; *at;) {
			DWORD length = lstrlenW(at) + 1;
			if (lstrcmpiW(at, entry)) {
				memmove(out, at, length * sizeof(WCHAR));
				out += length;
			}
			at += length;
		}
		*out = 0;
		RegSetValueExW(key, value, 0, REG_MULTI_SZ, (BYTE *)text, (DWORD)(out - text + 1) * sizeof(WCHAR));
	}
	setup_free(text);
}

static BOOL delete_registry_section(HINF hinf, const WCHAR *section, void *argument)
{
	HKEY relative = argument;
	INFCONTEXT context;

	if (!SetupFindFirstLineW(hinf, section, NULL, &context))
		return TRUE;
	do {
		const WCHAR *root_name = pSetupGetField(&context, 1), *subkey = pSetupGetField(&context, 2);
		const WCHAR *value = SetupGetFieldCount(&context) >= 3 ? pSetupGetField(&context, 3) : NULL;
		INT flags = 0;
		HKEY root, key;

		if (!root_name || !(root = root_key_of(root_name, relative)))
			continue;
		if (SetupGetFieldCount(&context) >= 4)
			SetupGetIntField(&context, 4, &flags);
		if (flags & FLG_DELREG_KEYONLY_COMMON) {
			RegDeleteKeyW(root, subkey ? subkey : L"");
			continue;
		}
		if (!value || !*value) {
			if (flags & FLG_ADDREG_DELVAL || SetupGetFieldCount(&context) < 3) {
				RegDeleteTreeW(root, subkey ? subkey : NULL);
				RegDeleteKeyW(root, subkey ? subkey : L"");
				continue;
			}
		}
		if (RegOpenKeyExW(root, subkey ? subkey : L"", 0, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS)
			continue;
		if ((flags & FLG_DELREG_MULTI_SZ_DELSTRING) == FLG_DELREG_MULTI_SZ_DELSTRING && SetupGetFieldCount(&context) >= 5)
			delete_from_multi_sz(key, value, pSetupGetField(&context, 5));
		else
			RegDeleteValueW(key, value);
		RegCloseKey(key);
	} while (SetupFindNextLine(&context, &context));
	return TRUE;
}

static BOOL bit_registry_section(HINF hinf, const WCHAR *section, void *argument)
{
	HKEY relative = argument;
	INFCONTEXT context;

	if (!SetupFindFirstLineW(hinf, section, NULL, &context))
		return TRUE;
	do {
		const WCHAR *root_name = pSetupGetField(&context, 1), *subkey = pSetupGetField(&context, 2), *value = pSetupGetField(&context, 3);
		INT flags = 0, mask = 0, offset = 0;
		HKEY root, key;
		BYTE data[256];
		DWORD size = sizeof(data), type;

		if (!root_name || !(root = root_key_of(root_name, relative)))
			continue;
		SetupGetIntField(&context, 4, &flags);
		SetupGetIntField(&context, 5, &mask);
		SetupGetIntField(&context, 6, &offset);
		if (RegOpenKeyExW(root, subkey ? subkey : L"", 0, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS)
			continue;
		if (RegQueryValueExW(key, value, NULL, &type, data, &size) == ERROR_SUCCESS && (DWORD)offset < size) {
			if (flags & FLG_BITREG_SETBITS)
				data[offset] |= (BYTE)mask;
			else
				data[offset] &= (BYTE)~mask;
			RegSetValueExW(key, value, 0, type, data, size);
		}
		RegCloseKey(key);
	} while (SetupFindNextLine(&context, &context));
	return TRUE;
}

/* INI files. */

static BOOL update_ini_section(HINF hinf, const WCHAR *section, void *argument)
{
	INFCONTEXT context;

	if (!SetupFindFirstLineW(hinf, section, NULL, &context))
		return TRUE;
	do {
		const WCHAR *file = pSetupGetField(&context, 1), *group = pSetupGetField(&context, 2);
		const WCHAR *old_entry = SetupGetFieldCount(&context) >= 3 ? pSetupGetField(&context, 3) : NULL;
		const WCHAR *new_entry = SetupGetFieldCount(&context) >= 4 ? pSetupGetField(&context, 4) : NULL;
		INT flags = 0;
		WCHAR buffer[MAX_PATH];

		if (!file || !group)
			continue;
		if (SetupGetFieldCount(&context) >= 5)
			SetupGetIntField(&context, 5, &flags);
		if (old_entry && *old_entry) {
			WCHAR name[256];
			const WCHAR *equals = wcschr(old_entry, '=');
			lstrcpynW(name, old_entry, equals ? (int)(equals - old_entry + 1) : (int)ARRAYSIZE(name));
			if (!new_entry || !*new_entry) {
				WritePrivateProfileStringW(group, name, NULL, file);
				continue;
			}
			if ((flags & 1) && !GetPrivateProfileStringW(group, name, L"", buffer, ARRAYSIZE(buffer), file))
				continue;
			if (equals)
				WritePrivateProfileStringW(group, name, NULL, file);
		}
		if (new_entry && *new_entry) {
			WCHAR name[256];
			const WCHAR *equals = wcschr(new_entry, '=');
			lstrcpynW(name, new_entry, equals ? (int)(equals - new_entry + 1) : (int)ARRAYSIZE(name));
			WritePrivateProfileStringW(group, name, equals ? equals + 1 : L"", file);
		}
	} while (SetupFindNextLine(&context, &context));
	return TRUE;
}

/* Copies, deletes and renames. */

struct files_argument {
	HSPFILEQ queue;
	const WCHAR *root;
	HINF layout;
	DWORD style;
};

static BOOL copy_files_section(HINF hinf, const WCHAR *section, void *argument)
{
	struct files_argument *files = argument;

	return SetupQueueCopySectionW(files->queue, files->root, hinf, files->layout, section, files->style);
}

static BOOL delete_files_section(HINF hinf, const WCHAR *section, void *argument)
{
	struct files_argument *files = argument;

	return SetupQueueDeleteSectionW(files->queue, hinf, files->layout, section);
}

static BOOL rename_files_section(HINF hinf, const WCHAR *section, void *argument)
{
	struct files_argument *files = argument;

	return SetupQueueRenameSectionW(files->queue, hinf, files->layout, section);
}

BOOL WINAPI SetupInstallFilesFromInfSectionW(HINF hinf, HINF layout, HSPFILEQ queue, PCWSTR section, PCWSTR root, UINT flags)
{
	struct files_argument files = { queue, root, layout, flags };

	return for_each_directive(hinf, section, L"CopyFiles", copy_files_section, &files) &&
			for_each_directive(hinf, section, L"DelFiles", delete_files_section, &files) &&
			for_each_directive(hinf, section, L"RenFiles", rename_files_section, &files);
}

BOOL WINAPI SetupInstallFilesFromInfSectionA(HINF hinf, HINF layout, HSPFILEQ queue, PCSTR section, PCSTR root, UINT flags)
{
	WCHAR *wide_section = setup_to_wide(section), *wide_root = setup_to_wide(root);
	BOOL result = SetupInstallFilesFromInfSectionW(hinf, layout, queue, wide_section, wide_root, flags);

	setup_free(wide_section);
	setup_free(wide_root);
	return result;
}

/* Self registration. */

static void call_dll_export(HMODULE module, const char *name)
{
	HRESULT (WINAPI *entry)(void) = (void *)GetProcAddress(module, name);

	if (entry)
		entry();
}

static BOOL register_section(HINF hinf, const WCHAR *section, void *argument)
{
	BOOL registering = argument != NULL;
	INFCONTEXT context;

	if (!SetupFindFirstLineW(hinf, section, NULL, &context))
		return TRUE;
	do {
		WCHAR *directory = setup_directory_from_line(hinf, &context, 1), *full;
		const WCHAR *file = pSetupGetField(&context, 3);
		const WCHAR *arguments = SetupGetFieldCount(&context) >= 6 ? pSetupGetField(&context, 6) : NULL;
		INT flags = 0, timeout = 60;
		const WCHAR *dot;

		if (!directory || !file) {
			setup_free(directory);
			continue;
		}
		if (SetupGetFieldCount(&context) >= 4)
			SetupGetIntField(&context, 4, &flags);
		if (SetupGetFieldCount(&context) >= 5)
			SetupGetIntField(&context, 5, &timeout);
		full = setup_join_path(directory, file);
		setup_free(directory);
		dot = full ? wcsrchr(full, '.') : NULL;
		if (dot && !lstrcmpiW(dot, L".exe")) {
			if (flags & FLG_REGSVR_DLLREGISTER) {
				SIZE_T length = lstrlenW(full) + 32;
				WCHAR *command = setup_alloc(length * sizeof(WCHAR));
				if (command) {
					STARTUPINFOW startup = { sizeof(startup) };
					PROCESS_INFORMATION process;
					wsprintfW(command, L"\"%s\" %s", full, registering ? L"/RegServer" : L"/UnregServer");
					if (CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
						WaitForSingleObject(process.hProcess, timeout ? (DWORD)timeout * 1000 : INFINITE);
						CloseHandle(process.hThread);
						CloseHandle(process.hProcess);
					}
					setup_free(command);
				}
			}
		} else if (full) {
			HMODULE module = LoadLibraryExW(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (module) {
				HRESULT init = CoInitialize(NULL);
				if (flags & FLG_REGSVR_DLLREGISTER)
					call_dll_export(module, registering ? "DllRegisterServer" : "DllUnregisterServer");
				if (flags & FLG_REGSVR_DLLINSTALL) {
					HRESULT (WINAPI *install)(BOOL, LPCWSTR) = (void *)GetProcAddress(module, "DllInstall");
					if (install)
						install(registering, arguments);
				}
				if (SUCCEEDED(init))
					CoUninitialize();
				FreeLibrary(module);
			}
		}
		setup_free(full);
	} while (SetupFindNextLine(&context, &context));
	return TRUE;
}

/* Start menu entries. */

static WCHAR *field_directory(HINF hinf, const WCHAR *section, const WCHAR *key, const WCHAR *file_default, BOOL *found)
{
	INFCONTEXT context;
	WCHAR *directory, *full;
	const WCHAR *file;

	*found = FALSE;
	if (!SetupFindFirstLineW(hinf, section, key, &context))
		return NULL;
	*found = TRUE;
	directory = setup_directory_from_line(hinf, &context, 1);
	if (!directory)
		return NULL;
	/* The subdirectory field is optional: with two fields the second is the file name. */
	if (SetupGetFieldCount(&context) == 2) {
		setup_free(directory);
		{
			INT id = 0;
			WCHAR *base;
			SetupGetIntField(&context, 1, &id);
			base = setup_dir_id_path(hinf, id);
			file = pSetupGetField(&context, 2);
			full = base && file ? setup_join_path(base, file) : NULL;
			setup_free(base);
			return full;
		}
	}
	file = SetupGetFieldCount(&context) >= 3 ? pSetupGetField(&context, 3) : file_default;
	if (!file)
		return directory;
	full = setup_join_path(directory, file);
	setup_free(directory);
	return full;
}

static void make_link(HINF hinf, const WCHAR *item, const WCHAR *name, const WCHAR *link_path)
{
	IShellLinkW *link = NULL;
	IPersistFile *file = NULL;
	BOOL found;
	WCHAR *target = field_directory(hinf, item, L"CmdLine", NULL, &found);
	INFCONTEXT context;

	if (!target)
		return;
	if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) {
		WCHAR *other;

		IShellLinkW_SetPath(link, target);
		other = field_directory(hinf, item, L"WorkingDir", NULL, &found);
		if (other) {
			IShellLinkW_SetWorkingDirectory(link, other);
			setup_free(other);
		}
		other = field_directory(hinf, item, L"IconPath", NULL, &found);
		if (other) {
			INT index = 0;
			if (SetupFindFirstLineW(hinf, item, L"IconIndex", &context))
				SetupGetIntField(&context, 1, &index);
			IShellLinkW_SetIconLocation(link, other, index);
			setup_free(other);
		}
		if (SetupFindFirstLineW(hinf, item, L"InfoTip", &context)) {
			const WCHAR *tip = pSetupGetField(&context, 1);
			if (tip)
				IShellLinkW_SetDescription(link, tip);
		}
		if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
			IPersistFile_Save(file, link_path, TRUE);
			IPersistFile_Release(file);
		}
		IShellLinkW_Release(link);
	}
	setup_free(target);
}

/* The flags of a Name line: for the current user, remove, a program group, a folder in field 3. */
#define PROFILE_CURRENT_USER 1
#define PROFILE_DELETE 2
#define PROFILE_GROUP 4
#define PROFILE_CSIDL 8

static BOOL profile_items_section(HINF hinf, const WCHAR *item, void *argument)
{
	INFCONTEXT context;
	WCHAR folder[MAX_PATH], *name, *directory = NULL, *link = NULL;
	INT flags = 0, csidl;

	if (!SetupFindFirstLineW(hinf, item, L"Name", &context))
		return TRUE;
	name = setup_copy_wide(pSetupGetField(&context, 1));
	if (!name)
		return TRUE;
	if (SetupGetFieldCount(&context) >= 2)
		SetupGetIntField(&context, 2, &flags);
	csidl = flags & PROFILE_CURRENT_USER ? CSIDL_PROGRAMS : CSIDL_COMMON_PROGRAMS;
	if ((flags & PROFILE_CSIDL) && SetupGetFieldCount(&context) >= 3)
		SetupGetIntField(&context, 3, &csidl);
	if (FAILED(SHGetFolderPathW(NULL, csidl | CSIDL_FLAG_CREATE, NULL, 0, folder))) {
		setup_free(name);
		return TRUE;
	}
	if (flags & PROFILE_GROUP) {
		directory = setup_join_path(folder, name);
		if (directory) {
			if (flags & PROFILE_DELETE)
				RemoveDirectoryW(directory);
			else
				setup_make_directories(directory);
		}
	} else {
		const WCHAR *sub = NULL;
		WCHAR *file = setup_alloc((lstrlenW(name) + 5) * sizeof(WCHAR));

		if (SetupFindFirstLineW(hinf, item, L"SubDir", &context))
			sub = pSetupGetField(&context, 1);
		directory = sub ? setup_join_path(folder, sub) : setup_copy_wide(folder);
		if (file) {
			lstrcpyW(file, name);
			lstrcatW(file, L".lnk");
			link = directory ? setup_join_path(directory, file) : NULL;
			setup_free(file);
		}
		if (link && (flags & PROFILE_DELETE)) {
			DeleteFileW(link);
		} else if (link) {
			HRESULT init = CoInitialize(NULL);
			setup_make_directories(directory);
			make_link(hinf, item, name, link);
			if (SUCCEEDED(init))
				CoUninitialize();
		}
	}
	setup_free(link);
	setup_free(directory);
	setup_free(name);
	return TRUE;
}

BOOL WINAPI SetupInstallFromInfSectionW(HWND owner, HINF hinf, PCWSTR section, UINT flags, HKEY key_root, PCWSTR source_root, UINT copy_flags, PSP_FILE_CALLBACK_W callback, PVOID context, HDEVINFO devinfo, PSP_DEVINFO_DATA device)
{
	HKEY relative = key_root;

	if (!hinf || hinf == INVALID_HANDLE_VALUE || !section) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (SetupGetLineCountW(hinf, section) < 0)
		return FALSE;
	if (flags & SPINST_FILES) {
		HSPFILEQ queue = SetupOpenFileQueue();
		BOOL result;

		if (queue == INVALID_HANDLE_VALUE)
			return FALSE;
		if (!callback) {
			SetupCloseFileQueue(queue);
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		result = SetupInstallFilesFromInfSectionW(hinf, NULL, queue, section, source_root, copy_flags) &&
				SetupCommitFileQueueW(owner, queue, callback, context);
		{
			DWORD error = GetLastError();
			SetupCloseFileQueue(queue);
			if (!result) {
				SetLastError(error);
				return FALSE;
			}
		}
	}
	if (flags & SPINST_INIFILES)
		for_each_directive(hinf, section, L"UpdateInis", update_ini_section, NULL);
	if (flags & SPINST_REGISTRY) {
		for_each_directive(hinf, section, L"DelReg", delete_registry_section, relative);
		if (!for_each_directive(hinf, section, L"AddReg", add_registry_section, relative))
			return FALSE;
	}
	if (flags & SPINST_BITREG)
		for_each_directive(hinf, section, L"BitReg", bit_registry_section, relative);
	if (flags & SPINST_UNREGSVR)
		for_each_directive(hinf, section, L"UnregisterDlls", register_section, NULL);
	if (flags & SPINST_REGSVR)
		for_each_directive(hinf, section, L"RegisterDlls", register_section, (void *)1);
	if (flags & SPINST_PROFILEITEMS)
		for_each_directive(hinf, section, L"ProfileItems", profile_items_section, NULL);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI SetupInstallFromInfSectionA(HWND owner, HINF hinf, PCSTR section, UINT flags, HKEY key_root, PCSTR source_root, UINT copy_flags, PSP_FILE_CALLBACK_A callback, PVOID context, HDEVINFO devinfo, PSP_DEVINFO_DATA device)
{
	WCHAR *wide_section = setup_to_wide(section), *wide_root = setup_to_wide(source_root);
	BOOL result;

	if (flags & SPINST_FILES) {
		/* The callback stays ANSI: the queue commit converts for it. */
		HSPFILEQ queue = SetupOpenFileQueue();
		if (queue == INVALID_HANDLE_VALUE) {
			result = FALSE;
			goto done;
		}
		if (!callback) {
			SetupCloseFileQueue(queue);
			SetLastError(ERROR_INVALID_PARAMETER);
			result = FALSE;
			goto done;
		}
		result = SetupInstallFilesFromInfSectionW(hinf, NULL, queue, wide_section, wide_root, copy_flags) &&
				SetupCommitFileQueueA(owner, queue, callback, context);
		{
			DWORD error = GetLastError();
			SetupCloseFileQueue(queue);
			SetLastError(error);
		}
		if (!result)
			goto done;
	}
	result = SetupInstallFromInfSectionW(owner, hinf, wide_section, flags & ~SPINST_FILES, key_root, NULL, 0, NULL, NULL, devinfo, device);
done:
	{
		DWORD error = GetLastError();
		setup_free(wide_section);
		setup_free(wide_root);
		SetLastError(error);
	}
	return result;
}

/* Services. */

static WCHAR *section_string(HINF hinf, const WCHAR *section, const WCHAR *key)
{
	INFCONTEXT context;
	const WCHAR *value;

	if (!SetupFindFirstLineW(hinf, section, key, &context))
		return NULL;
	value = pSetupGetField(&context, 1);
	return value ? setup_copy_wide(value) : NULL;
}

static DWORD section_number(HINF hinf, const WCHAR *section, const WCHAR *key, DWORD fallback)
{
	INFCONTEXT context;
	INT value;

	if (!SetupFindFirstLineW(hinf, section, key, &context) || !SetupGetIntField(&context, 1, &value))
		return fallback;
	return (DWORD)value;
}

static WCHAR *section_multi(HINF hinf, const WCHAR *section, const WCHAR *key)
{
	INFCONTEXT context;
	DWORD chars = 0;
	WCHAR *text;

	if (!SetupFindFirstLineW(hinf, section, key, &context) || !SetupGetMultiSzFieldW(&context, 1, NULL, 0, &chars))
		return NULL;
	text = setup_alloc((SIZE_T)chars * sizeof(WCHAR));
	if (text)
		SetupGetMultiSzFieldW(&context, 1, text, chars, NULL);
	return text;
}

static BOOL add_service(HINF hinf, SC_HANDLE manager, const WCHAR *name, const WCHAR *section, DWORD flags)
{
	WCHAR *display = section_string(hinf, section, L"DisplayName"), *binary = section_string(hinf, section, L"ServiceBinary");
	WCHAR *group = section_string(hinf, section, L"LoadOrderGroup"), *account = section_string(hinf, section, L"StartName");
	WCHAR *dependencies = section_multi(hinf, section, L"Dependencies"), *description = section_string(hinf, section, L"Description");
	DWORD type = section_number(hinf, section, L"ServiceType", 0), start = section_number(hinf, section, L"StartType", 0);
	DWORD error_control = section_number(hinf, section, L"ErrorControl", 0);
	SC_HANDLE service = NULL;
	BOOL result = FALSE;
	INFCONTEXT line;

	/* These four are what a service section cannot do without. */
	if (!binary || !SetupFindFirstLineW(hinf, section, L"ServiceType", &line) || !SetupFindFirstLineW(hinf, section, L"StartType", &line) ||
			!SetupFindFirstLineW(hinf, section, L"ErrorControl", &line)) {
		SetLastError(ERROR_BAD_SERVICE_INSTALLSECT);
		goto done;
	}
	service = OpenServiceW(manager, name, SERVICE_ALL_ACCESS);
	if (service) {
		QUERY_SERVICE_CONFIGW *config = NULL;
		DWORD size = 0;

		QueryServiceConfigW(service, NULL, 0, &size);
		config = size ? setup_alloc(size) : NULL;
		if (config && QueryServiceConfigW(service, config, size, &size)) {
			if (flags & SPSVCINST_NOCLOBBER_DISPLAYNAME) {
				setup_free(display);
				display = setup_copy_wide(config->lpDisplayName);
			}
			if (flags & SPSVCINST_NOCLOBBER_STARTTYPE)
				start = config->dwStartType;
			if (flags & SPSVCINST_NOCLOBBER_ERRORCONTROL)
				error_control = config->dwErrorControl;
			if (flags & SPSVCINST_NOCLOBBER_LOADORDERGROUP) {
				setup_free(group);
				group = setup_copy_wide(config->lpLoadOrderGroup);
			}
			if (flags & SPSVCINST_NOCLOBBER_DEPENDENCIES) {
				setup_free(dependencies);
				dependencies = NULL;
			}
		}
		setup_free(config);
		result = ChangeServiceConfigW(service, type, start, error_control, binary, group, NULL, dependencies, account, NULL, display);
	} else if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
		service = CreateServiceW(manager, name, display ? display : name, SERVICE_ALL_ACCESS, type, start, error_control, binary, group, NULL, dependencies, account, NULL);
		result = service != NULL;
	}
	if (result && description && !(flags & SPSVCINST_NOCLOBBER_DESCRIPTION)) {
		SERVICE_DESCRIPTIONW text = { description };
		ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &text);
	}
	if (result && (flags & SETUP_SVC_START))
		StartServiceW(service, 0, NULL);
done:
	if (service)
		CloseServiceHandle(service);
	setup_free(display);
	setup_free(binary);
	setup_free(group);
	setup_free(account);
	setup_free(dependencies);
	setup_free(description);
	return result;
}

static BOOL delete_service(SC_HANDLE manager, const WCHAR *name, DWORD flags)
{
	SC_HANDLE service = OpenServiceW(manager, name, SERVICE_ALL_ACCESS);
	BOOL result;

	if (!service)
		return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
	if (flags & SPSVCINST_STOPSERVICE) {
		SERVICE_STATUS status;
		ControlService(service, SERVICE_CONTROL_STOP, &status);
	}
	result = DeleteService(service);
	CloseServiceHandle(service);
	return result;
}

BOOL WINAPI SetupInstallServicesFromInfSectionExW(HINF hinf, PCWSTR section, DWORD flags, HDEVINFO devinfo, PSP_DEVINFO_DATA device, PVOID reserved1, PVOID reserved2)
{
	SC_HANDLE manager;
	INFCONTEXT context;
	BOOL result = TRUE;

	if (reserved1 || reserved2 || (devinfo && devinfo != INVALID_HANDLE_VALUE && !device)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (flags & ~(SETUP_SVC_START | SPSVCINST_NOCLOBBER_DISPLAYNAME | SPSVCINST_NOCLOBBER_STARTTYPE | SPSVCINST_NOCLOBBER_ERRORCONTROL |
			SPSVCINST_NOCLOBBER_LOADORDERGROUP | SPSVCINST_NOCLOBBER_DEPENDENCIES | SPSVCINST_NOCLOBBER_DESCRIPTION | SPSVCINST_STOPSERVICE |
			SPSVCINST_CLOBBER_SECURITY | SPSVCINST_DELETEEVENTLOGENTRY | SPSVCINST_TAGTOFRONT | SPSVCINST_ASSOCSERVICE | 0x400)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!hinf || hinf == INVALID_HANDLE_VALUE || !section) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (SetupGetLineCountW(hinf, section) <= 0) {
		SetLastError(ERROR_SECTION_NOT_FOUND);
		return FALSE;
	}
	manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (!manager)
		return FALSE;
	if (SetupFindFirstLineW(hinf, section, L"AddService", &context)) {
		do {
			const WCHAR *name = pSetupGetField(&context, 1), *install = pSetupGetField(&context, 3);
			INT line_flags = 0;
			if (!name || !*name)
				continue;
			SetupGetIntField(&context, 2, &line_flags);
			if (!install || !add_service(hinf, manager, name, install, flags | (DWORD)line_flags)) {
				if (!install)
					SetLastError(ERROR_BAD_SERVICE_INSTALLSECT);
				result = FALSE;
				break;
			}
		} while (SetupFindNextMatchLineW(&context, L"AddService", &context));
	}
	if (result && SetupFindFirstLineW(hinf, section, L"DelService", &context)) {
		do {
			const WCHAR *name = pSetupGetField(&context, 1);
			INT line_flags = 0;
			if (!name)
				continue;
			SetupGetIntField(&context, 2, &line_flags);
			if (!delete_service(manager, name, flags | (DWORD)line_flags)) {
				result = FALSE;
				break;
			}
		} while (SetupFindNextMatchLineW(&context, L"DelService", &context));
	}
	{
		DWORD error = GetLastError();
		CloseServiceHandle(manager);
		SetLastError(result ? ERROR_SUCCESS : error);
	}
	return result;
}

BOOL WINAPI SetupInstallServicesFromInfSectionExA(HINF hinf, PCSTR section, DWORD flags, HDEVINFO devinfo, PSP_DEVINFO_DATA device, PVOID reserved1, PVOID reserved2)
{
	WCHAR *wide = setup_to_wide(section);
	BOOL result = SetupInstallServicesFromInfSectionExW(hinf, wide, flags, devinfo, device, reserved1, reserved2);
	DWORD error = GetLastError();

	setup_free(wide);
	SetLastError(error);
	return result;
}

BOOL WINAPI SetupInstallServicesFromInfSectionW(HINF hinf, PCWSTR section, DWORD flags)
{
	return SetupInstallServicesFromInfSectionExW(hinf, section, flags, INVALID_HANDLE_VALUE, NULL, NULL, NULL);
}

BOOL WINAPI SetupInstallServicesFromInfSectionA(HINF hinf, PCSTR section, DWORD flags)
{
	return SetupInstallServicesFromInfSectionExA(hinf, section, flags, INVALID_HANDLE_VALUE, NULL, NULL, NULL);
}

/* rundll32 setupapi,InstallHinfSection: "section mode path". */

static BOOL install_with_services(HWND window, HINF hinf, const WCHAR *section, void *context)
{
	WCHAR services[256 + 10];

	if (!SetupInstallFromInfSectionW(window, hinf, section, SPINST_ALL, NULL, NULL, SP_COPY_NEWER, SetupDefaultQueueCallbackW, context, NULL, NULL))
		return FALSE;
	lstrcpynW(services, section, 256);
	lstrcatW(services, L".Services");
	SetupInstallServicesFromInfSectionW(hinf, services, 0);
	return TRUE;
}

/* Only this entry point follows Include and Needs; the plain section install ignores them. */
static BOOL include_section(HINF hinf, const WCHAR *file, void *argument)
{
	SetupOpenAppendInfFileW(file, hinf, NULL);
	return TRUE;
}

static BOOL needed_section(HINF hinf, const WCHAR *section, void *context)
{
	install_with_services(NULL, hinf, section, context);
	return TRUE;
}

void WINAPI InstallHinfSectionW(HWND window, HINSTANCE instance, PCWSTR command, INT show)
{
	WCHAR *text = setup_copy_wide(command), *section, *mode, *path, *cut;
	HINF hinf;
	void *context;

	if (!text)
		return;
	section = text;
	cut = wcschr(section, ' ');
	if (!cut)
		goto done;
	*cut = 0;
	mode = cut + 1;
	cut = wcschr(mode, ' ');
	if (!cut)
		goto done;
	*cut = 0;
	path = cut + 1;
	hinf = SetupOpenInfFileW(path, NULL, INF_STYLE_WIN4, NULL);
	if (hinf == INVALID_HANDLE_VALUE)
		goto done;
	{
		WCHAR actual[256];
		DWORD length;
		if (!SetupDiGetActualSectionToInstallW(hinf, section, actual, ARRAYSIZE(actual), &length, NULL))
			lstrcpynW(actual, section, ARRAYSIZE(actual));
		context = SetupInitDefaultQueueCallback(window);
		for_each_directive(hinf, actual, L"Include", include_section, NULL);
		for_each_directive(hinf, actual, L"Needs", needed_section, context);
		install_with_services(window, hinf, actual, context);
		SetupTermDefaultQueueCallback(context);
	}
	SetupCloseInfFile(hinf);
done:
	setup_free(text);
}

void WINAPI InstallHinfSectionA(HWND window, HINSTANCE instance, PCSTR command, INT show)
{
	WCHAR *wide = setup_to_wide(command);

	if (wide)
		InstallHinfSectionW(window, instance, wide, show);
	setup_free(wide);
}

/* Which decorated form of a section this machine installs. */

BOOL WINAPI SetupDiGetActualSectionToInstallExW(HINF hinf, PCWSTR section, PSP_ALTPLATFORM_INFO platform, PWSTR buffer, DWORD size, PDWORD required, PWSTR *extension, PVOID reserved)
{
	static const WCHAR *const suffixes[] = {
#if defined(_M_AMD64) || defined(__x86_64__)
		L".NTamd64",
#else
		L".NTx86",
#endif
		L".NT", L"" };
	WCHAR name[256 + 16];
	UINT at;

	if (!hinf || hinf == INVALID_HANDLE_VALUE || !section || reserved) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	for (at = 0; at < ARRAYSIZE(suffixes); at++) {
		lstrcpynW(name, section, 256);
		lstrcatW(name, suffixes[at]);
		if (SetupGetLineCountW(hinf, name) >= 0 || !suffixes[at][0]) {
			if (!setup_give_wide(name, buffer, size, required))
				return FALSE;
			if (extension)
				*extension = buffer && suffixes[at][0] ? buffer + lstrlenW(section) : NULL;
			SetLastError(ERROR_SUCCESS);
			return TRUE;
		}
	}
	return FALSE;
}

BOOL WINAPI SetupDiGetActualSectionToInstallW(HINF hinf, PCWSTR section, PWSTR buffer, DWORD size, PDWORD required, PWSTR *extension)
{
	return SetupDiGetActualSectionToInstallExW(hinf, section, NULL, buffer, size, required, extension, NULL);
}

BOOL WINAPI SetupDiGetActualSectionToInstallExA(HINF hinf, PCSTR section, PSP_ALTPLATFORM_INFO platform, PSTR buffer, DWORD size, PDWORD required, PSTR *extension, PVOID reserved)
{
	WCHAR *wide = setup_to_wide(section), text[280], *wide_extension = NULL;
	BOOL result = SetupDiGetActualSectionToInstallExW(hinf, wide, platform, text, ARRAYSIZE(text), NULL, &wide_extension, reserved);

	setup_free(wide);
	if (!setup_wide_to_ansi_result(result, text, buffer, size, required))
		return FALSE;
	if (extension)
		*extension = buffer && wide_extension ? buffer + (wide_extension - text) : NULL;
	return TRUE;
}

BOOL WINAPI SetupDiGetActualSectionToInstallA(HINF hinf, PCSTR section, PSTR buffer, DWORD size, PDWORD required, PSTR *extension)
{
	return SetupDiGetActualSectionToInstallExA(hinf, section, NULL, buffer, size, required, extension, NULL);
}
