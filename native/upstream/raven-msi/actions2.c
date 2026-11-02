/* SPDX-License-Identifier: MIT */
#include "msi_own.h"
#include <shlobj.h>
#include <objidl.h>

/* The standard actions that write the rest of the machine: folders, registry, shortcuts, environment, INI files, class
 * registration, services, self-registration; and those reading it before the installation: searches and related products. */

static UINT act_none_extra(msi_package *package)
{
	(void)package;
	return ERROR_SUCCESS;
}

static WCHAR *row_format(msi_package *package, msi_record *row, UINT field)
{
	WCHAR scratch[16];
	const WCHAR *text = record_text(row, field, scratch);
	return text ? format_text(package, text, NULL) : NULL;
}

static HKEY registry_root(msi_package *package, INT code)
{
	switch (code) {
	case 0: return HKEY_CLASSES_ROOT;
	case 1: return HKEY_CURRENT_USER;
	case 2: return HKEY_LOCAL_MACHINE;
	case 3: return HKEY_USERS;
	default: return package_is_machine(package) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	}
}

// A 64-bit process writes for a 32-bit component through the 32-bit view, as the installer service does.
static void view_for(const msi_component *component)
{
	reg_set_view(sizeof(void *) == 8 && !(component->attributes & msidbComponentAttributes64bit) ? KEY_WOW64_32KEY : 0);
}

static UINT create_folder_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	const WCHAR *folder = record_text(row, 1, a);
	msi_component *component = component_find(package, record_text(row, 2, b));
	if (component && component->action == INSTALLSTATE_LOCAL)
		return create_directories(package, directory_target(package, folder));
	return ERROR_SUCCESS;
}

static UINT act_create_folders(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Directory_`, `Component_` FROM `CreateFolder`", NULL, create_folder_row, package);
	return ERROR_SUCCESS;
}

static UINT remove_folder_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	const WCHAR *folder = record_text(row, 1, a);
	msi_component *component = component_find(package, record_text(row, 2, b));
	if (component && component->action == INSTALLSTATE_ABSENT) {
		const WCHAR *target = directory_target(package, folder);
		if (target)
			RemoveDirectoryW(target);
	}
	return ERROR_SUCCESS;
}

static UINT act_remove_folders(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Directory_`, `Component_` FROM `CreateFolder`", NULL, remove_folder_row, package);
	return ERROR_SUCCESS;
}

static BYTE hex_digit(WCHAR c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
}

// "#123" a number, "#xAB" bytes, "#%text" an expandable string, [~] in a text separates a multi-string; "#" escapes itself.
static UINT write_value(msi_package *package, HKEY root, const WCHAR *key, const WCHAR *name, const WCHAR *value)
{
	if (value && value[0] == '#' && value[1] != '#') {
		if (value[1] == 'x' || value[1] == 'X') {
			UINT digits = lstrlenW(value + 2);
			BYTE *bytes = msi_alloc(digits / 2 + 1);
			for (UINT i = 0; i + 1 < digits; i += 2)
				bytes[i / 2] = hex_digit(value[2 + i]) << 4 | hex_digit(value[3 + i]);
			UINT status = reg_write(package, root, key, name, REG_BINARY, bytes, digits / 2);
			msi_free(bytes);
			return status;
		}
		if (value[1] == '%')
			return reg_write(package, root, key, name, REG_EXPAND_SZ, value + 2, (lstrlenW(value + 2) + 1) * sizeof(WCHAR));
		INT total = 0;
		BOOL negative = value[1] == '-';
		for (const WCHAR *at = value + 1 + negative; *at >= '0' && *at <= '9'; at++)
			total = total * 10 + (*at - '0');
		return reg_write_number(package, root, key, name, negative ? -total : total);
	}
	return reg_write_text(package, root, key, name, value ? (value[0] == '#' ? value + 1 : value) : L"");
}

static UINT write_registry_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16];
	msi_component *component = component_find(package, record_text(row, 5, a));
	if (!component || component->action != INSTALLSTATE_LOCAL)
		return ERROR_SUCCESS;
	view_for(component);
	HKEY root = registry_root(package, record_get_int(row, 1));
	WCHAR *key = row_format(package, row, 2), *name = row_format(package, row, 3), *value = row_format(package, row, 4);
	UINT status = ERROR_SUCCESS;
	if (name && (!lstrcmpW(name, L"+") || !lstrcmpW(name, L"*") || !lstrcmpW(name, L"-"))) {
		if (name[0] != '-') {
			HKEY handle;
			undo_registry(package, root, key, NULL);
			if (!RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE, NULL, &handle, NULL))
				RegCloseKey(handle);
		}
	} else if (key)
		status = write_value(package, root, key, name && *name ? name : NULL, value);
	reg_set_view(0);
	msi_free(key);
	msi_free(name);
	msi_free(value);
	return status ? ERROR_INSTALL_FAILURE : ERROR_SUCCESS;
}

static UINT act_write_registry(msi_package *package)
{
	UINT status = db_query_each(package->db, L"SELECT `Root`, `Key`, `Name`, `Value`, `Component_` FROM `Registry`", NULL, write_registry_row, package);
	return status == ERROR_INSTALL_FAILURE ? status : ERROR_SUCCESS;
}

static void delete_value_and_empty_key(msi_package *package, HKEY root, const WCHAR *key, const WCHAR *name)
{
	HKEY handle;
	undo_registry(package, root, key, name);
	if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WRITE, &handle))
		return;
	if (name)
		RegDeleteValueW(handle, name);
	DWORD values = 0, keys = 0;
	RegQueryInfoKeyW(handle, NULL, NULL, NULL, &keys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
	RegCloseKey(handle);
	if (!values && !keys)
		RegDeleteKeyW(root, key);
}

static UINT remove_registry_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16];
	msi_component *component = component_find(package, record_text(row, 5, a));
	if (!component || component->action != INSTALLSTATE_ABSENT || (component->attributes & msidbComponentAttributesPermanent))
		return ERROR_SUCCESS;
	view_for(component);
	HKEY root = registry_root(package, record_get_int(row, 1));
	WCHAR *key = row_format(package, row, 2), *name = row_format(package, row, 3);
	if (key) {
		BOOL key_only = name && (!lstrcmpW(name, L"+") || !lstrcmpW(name, L"*") || !lstrcmpW(name, L"-"));
		delete_value_and_empty_key(package, root, key, key_only ? NULL : name && *name ? name : L"");
	}
	reg_set_view(0);
	msi_free(key);
	msi_free(name);
	return ERROR_SUCCESS;
}

static UINT act_remove_registry(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Root`, `Key`, `Name`, `Value`, `Component_` FROM `Registry`", NULL, remove_registry_row, package);
	return ERROR_SUCCESS;
}

static UINT shortcut_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16];
	msi_component *component = component_find(package, record_text(row, 3, a));
	if (!component)
		return ERROR_SUCCESS;
	const WCHAR *folder = directory_target(package, record_text(row, 1, b));
	const WCHAR *raw_name = record_text(row, 2, c);
	if (!folder || !raw_name)
		return ERROR_SUCCESS;
	const WCHAR *name = raw_name;
	for (const WCHAR *at = raw_name; *at; at++)
		if (*at == '|')
			name = at + 1;
	WCHAR path[MAX_PATH * 2];
	wsprintfW(path, L"%s%s.lnk", folder, name);
	if (component->action == INSTALLSTATE_ABSENT) {
		DeleteFileW(path);
		return ERROR_SUCCESS;
	}
	if (component->action != INSTALLSTATE_LOCAL)
		return ERROR_SUCCESS;
	WCHAR *target = row_format(package, row, 4), *arguments = row_format(package, row, 5), *description = row_format(package, row, 6), *directory = row_format(package, row, 7);
	// An advertised shortcut names a feature; it starts the component's key file instead.
	WCHAR *resolved = NULL;
	if (target && !wcschr(target, L'\\') && component->key_path) {
		msi_file *file = file_find(package, component->key_path);
		resolved = file ? file_target(package, file) : NULL;
	}
	IShellLinkW *link = NULL;
	if (create_directories(package, folder) == ERROR_SUCCESS && SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) {
		IPersistFile *persist = NULL;
		IShellLinkW_SetPath(link, resolved ? resolved : target ? target : L"");
		if (arguments)
			IShellLinkW_SetArguments(link, arguments);
		if (description)
			IShellLinkW_SetDescription(link, description);
		const WCHAR *working = directory ? directory_target(package, directory) : NULL;
		if (working)
			IShellLinkW_SetWorkingDirectory(link, working);
		if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&persist))) {
			undo_file(package, path, NULL);
			IPersistFile_Save(persist, path, TRUE);
			IPersistFile_Release(persist);
		}
		IShellLinkW_Release(link);
	}
	msi_free(resolved);
	msi_free(target);
	msi_free(arguments);
	msi_free(description);
	msi_free(directory);
	return ERROR_SUCCESS;
}

static UINT act_shortcuts(msi_package *package)
{
	HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	db_query_each(package->db, L"SELECT `Directory_`, `Name`, `Component_`, `Target`, `Arguments`, `Description`, `WkDir` FROM `Shortcut`", NULL, shortcut_row, package);
	if (SUCCEEDED(com))
		CoUninitialize();
	return ERROR_SUCCESS;
}

static UINT remove_file_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16];
	msi_component *component = component_find(package, record_text(row, 1, a));
	INT mode = record_get_int(row, 4);
	if (!component)
		return ERROR_SUCCESS;
	BOOL install = component->action == INSTALLSTATE_LOCAL && (mode & 1), remove = component->action == INSTALLSTATE_ABSENT && (mode & 2);
	if (!install && !remove)
		return ERROR_SUCCESS;
	const WCHAR *raw_name = record_text(row, 2, b);
	WCHAR *folder = package_get(package, record_text(row, 3, c));
	const WCHAR *target = folder ? folder : directory_target(package, record_text(row, 3, c));
	if (target && raw_name) {
		const WCHAR *name = raw_name;
		for (const WCHAR *at = raw_name; *at; at++)
			if (*at == '|')
				name = at + 1;
		WCHAR path[MAX_PATH * 2];
		wsprintfW(path, L"%s%s", target, name);
		DeleteFileW(path);
	} else if (target)
		RemoveDirectoryW(target);
	msi_free(folder);
	return ERROR_SUCCESS;
}

static UINT act_remove_listed_files(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Component_`, `FileName`, `DirProperty`, `InstallMode` FROM `RemoveFile`", NULL, remove_file_row, package);
	return ERROR_SUCCESS;
}

static UINT duplicate_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16], d[16];
	msi_component *component = component_find(package, record_text(row, 1, a));
	msi_file *file = file_find(package, record_text(row, 2, b));
	if (!component || !file || component->action != INSTALLSTATE_LOCAL)
		return ERROR_SUCCESS;
	const WCHAR *name = record_text(row, 3, c);
	WCHAR *folder_property = record_text(row, 4, d) ? package_get(package, d) : NULL;
	const WCHAR *folder = folder_property ? folder_property : directory_target(package, component->directory);
	WCHAR *source = file_target(package, file);
	if (source && folder) {
		const WCHAR *long_name = name ? name : file->name;
		for (const WCHAR *at = long_name; *at; at++)
			if (*at == '|')
				long_name = at + 1;
		WCHAR target[MAX_PATH * 2];
		wsprintfW(target, L"%s%s", folder, long_name);
		undo_file(package, target, NULL);
		create_directories(package, folder);
		CopyFileW(source, target, FALSE);
	}
	msi_free(source);
	msi_free(folder_property);
	return ERROR_SUCCESS;
}

static UINT act_duplicate_files(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Component_`, `File_`, `DestName`, `DestFolder` FROM `DuplicateFile`", NULL, duplicate_row, package);
	return ERROR_SUCCESS;
}

static UINT ini_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16];
	msi_component *component = component_find(package, record_text(row, 6, a));
	if (!component || component->action != INSTALLSTATE_LOCAL)
		return ERROR_SUCCESS;
	const WCHAR *raw_name = record_text(row, 1, b);
	WCHAR *directory = record_text(row, 2, c) ? package_get(package, c) : NULL;
	const WCHAR *folder = directory ? directory : NULL;
	WCHAR windows[MAX_PATH], path[MAX_PATH * 2];
	if (!folder && record_text(row, 2, c))
		folder = directory_target(package, c);
	if (!folder) {
		GetWindowsDirectoryW(windows, MAX_PATH);
		lstrcatW(windows, L"\\");
		folder = windows;
	}
	const WCHAR *name = raw_name;
	for (const WCHAR *at = raw_name; at && *at; at++)
		if (*at == '|')
			name = at + 1;
	wsprintfW(path, L"%s%s", folder, name ? name : L"");
	WCHAR *section = row_format(package, row, 3), *key = row_format(package, row, 4), *value = row_format(package, row, 5);
	WritePrivateProfileStringW(section, key, value, path);
	msi_free(section);
	msi_free(key);
	msi_free(value);
	msi_free(directory);
	return ERROR_SUCCESS;
}

static UINT act_ini(msi_package *package)
{
	db_query_each(package->db, L"SELECT `FileName`, `DirProperty`, `Section`, `Key`, `Value`, `Component_` FROM `IniFile`", NULL, ini_row, package);
	return ERROR_SUCCESS;
}

static UINT environment_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	msi_component *component = component_find(package, record_text(row, 3, a));
	if (!component || (component->action != INSTALLSTATE_LOCAL && component->action != INSTALLSTATE_ABSENT))
		return ERROR_SUCCESS;
	const WCHAR *raw = record_text(row, 1, b);
	if (!raw)
		return ERROR_SUCCESS;
	// The name's prefix characters: = set, + create, - remove with the component, ! remove on install, * machine.
	BOOL machine = FALSE;
	while (*raw == '=' || *raw == '+' || *raw == '-' || *raw == '!' || *raw == '*') {
		if (*raw == '*')
			machine = TRUE;
		raw++;
	}
	HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	const WCHAR *key = machine ? L"System\\CurrentControlSet\\Control\\Session Manager\\Environment" : L"Environment";
	if (component->action == INSTALLSTATE_ABSENT) {
		delete_value_and_empty_key(package, root, key, raw);
		return ERROR_SUCCESS;
	}
	WCHAR *value = row_format(package, row, 2);
	if (value) {
		// [~] marks where the value already there goes.
		WCHAR *mark = wcsstr(value, L"[~]");
		WCHAR *existing = NULL;
		if (mark) {
			HKEY handle;
			if (!RegOpenKeyExW(root, key, 0, KEY_READ, &handle)) {
				DWORD size = 0;
				if (!RegQueryValueExW(handle, raw, NULL, NULL, NULL, &size) && size) {
					existing = msi_alloc(size + 2);
					RegQueryValueExW(handle, raw, NULL, NULL, (BYTE *)existing, &size);
				}
				RegCloseKey(handle);
			}
		}
		reg_write_text(package, root, key, raw, existing ? existing : value);
		msi_free(existing);
	}
	msi_free(value);
	return ERROR_SUCCESS;
}

static UINT act_environment(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Name`, `Value`, `Component_` FROM `Environment`", NULL, environment_row, package);
	return ERROR_SUCCESS;
}

static UINT self_reg_row(msi_record *row, void *context)
{
	WCHAR a[16];
	struct { msi_package *package; BOOL install; } *pass = context;
	msi_file *file = file_find(pass->package, record_text(row, 1, a));
	if (!file || !file->component || file->component->action != (pass->install ? INSTALLSTATE_LOCAL : INSTALLSTATE_ABSENT))
		return ERROR_SUCCESS;
	WCHAR *path = file_target(pass->package, file);
	HMODULE module = path ? LoadLibraryW(path) : NULL;
	if (module) {
		HRESULT (WINAPI *call)(void) = (void *)GetProcAddress(module, pass->install ? "DllRegisterServer" : "DllUnregisterServer");
		if (call)
			call();
		FreeLibrary(module);
	}
	msi_free(path);
	return ERROR_SUCCESS;
}

static UINT act_self_register(msi_package *package)
{
	struct { msi_package *package; BOOL install; } pass = { package, TRUE };
	db_query_each(package->db, L"SELECT `File_`, `Cost` FROM `SelfReg`", NULL, self_reg_row, &pass);
	return ERROR_SUCCESS;
}

static UINT act_self_unregister(msi_package *package)
{
	struct { msi_package *package; BOOL install; } pass = { package, FALSE };
	db_query_each(package->db, L"SELECT `File_`, `Cost` FROM `SelfReg`", NULL, self_reg_row, &pass);
	return ERROR_SUCCESS;
}

static UINT class_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16], d[16], key[256];
	msi_component *component = component_find(package, record_text(row, 3, a));
	const WCHAR *clsid = record_text(row, 1, b), *server_context = record_text(row, 2, c);
	if (!component || !clsid || !server_context)
		return ERROR_SUCCESS;
	view_for(component);
	wsprintfW(key, L"Software\\Classes\\CLSID\\%s", clsid);
	HKEY root = package_is_machine(package) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	if (component->action == INSTALLSTATE_LOCAL) {
		const WCHAR *description = record_text(row, 4, d);
		if (description)
			reg_write_text(package, root, key, NULL, description);
		msi_file *file = component->key_path ? file_find(package, component->key_path) : NULL;
		WCHAR *path = file ? file_target(package, file) : NULL;
		if (path) {
			WCHAR sub[300];
			wsprintfW(sub, L"%s\\%s", key, server_context);
			reg_write_text(package, root, sub, NULL, path);
			msi_free(path);
		}
	} else if (component->action == INSTALLSTATE_ABSENT) {
		WCHAR sub[300];
		wsprintfW(sub, L"%s\\%s", key, server_context);
		RegDeleteKeyW(root, sub);
		RegDeleteKeyW(root, key);
	}
	reg_set_view(0);
	return ERROR_SUCCESS;
}

static UINT act_classes(msi_package *package)
{
	db_query_each(package->db, L"SELECT `CLSID`, `Context`, `Component_`, `Description` FROM `Class`", NULL, class_row, package);
	return ERROR_SUCCESS;
}

static UINT progid_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16], d[16], key[256], sub[300];
	const WCHAR *progid = record_text(row, 1, a), *clsid = record_text(row, 2, b), *description = record_text(row, 3, c);
	WCHAR query[256];
	(void)d;
	(void)query;
	if (!progid)
		return ERROR_SUCCESS;
	HKEY root = package_is_machine(package) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	wsprintfW(key, L"Software\\Classes\\%s", progid);
	if (description)
		reg_write_text(package, root, key, NULL, description);
	if (clsid) {
		wsprintfW(sub, L"%s\\CLSID", key);
		reg_write_text(package, root, sub, NULL, clsid);
	}
	return ERROR_SUCCESS;
}

static UINT act_progids(msi_package *package)
{
	if (package->removing)
		return ERROR_SUCCESS;
	db_query_each(package->db, L"SELECT `ProgId`, `Class_`, `Description` FROM `ProgId`", NULL, progid_row, package);
	return ERROR_SUCCESS;
}

static UINT extension_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16], key[256];
	const WCHAR *extension = record_text(row, 1, a), *progid = record_text(row, 3, c);
	msi_component *component = component_find(package, record_text(row, 2, b));
	if (!extension || !component || component->action != INSTALLSTATE_LOCAL || !progid)
		return ERROR_SUCCESS;
	wsprintfW(key, L"Software\\Classes\\.%s", extension);
	reg_write_text(package, package_is_machine(package) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key, NULL, progid);
	return ERROR_SUCCESS;
}

static UINT act_extensions(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Extension`, `Component_`, `ProgId_` FROM `Extension`", NULL, extension_row, package);
	return ERROR_SUCCESS;
}

static UINT font_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	msi_file *file = file_find(package, record_text(row, 1, a));
	const WCHAR *title = record_text(row, 2, b);
	if (file && file->component && file->component->action == INSTALLSTATE_LOCAL && file->name)
		reg_write_text(package, HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", title ? title : file->name, file->name);
	return ERROR_SUCCESS;
}

static UINT act_fonts(msi_package *package)
{
	db_query_each(package->db, L"SELECT `File_`, `FontTitle` FROM `Font`", NULL, font_row, package);
	return ERROR_SUCCESS;
}

static UINT service_install_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16];
	msi_component *component = component_find(package, record_text(row, 7, a));
	if (!component || component->action != INSTALLSTATE_LOCAL || !component->key_path)
		return ERROR_SUCCESS;
	msi_file *file = file_find(package, component->key_path);
	WCHAR *name = row_format(package, row, 1), *display = row_format(package, row, 2), *arguments = row_format(package, row, 6);
	WCHAR *image = file ? file_target(package, file) : NULL;
	UINT status = ERROR_SUCCESS;
	SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (manager && image && name) {
		WCHAR line[MAX_PATH * 3];
		wsprintfW(line, arguments ? L"\"%s\" %s" : L"\"%s\"", image, arguments);
		SC_HANDLE service = CreateServiceW(manager, name, display ? display : name, SERVICE_ALL_ACCESS, record_get_int(row, 3), record_get_int(row, 4), record_get_int(row, 5),
			line, NULL, NULL, NULL, NULL, NULL);
		if (service)
			CloseServiceHandle(service);
		else if (GetLastError() != ERROR_SERVICE_EXISTS)
			status = ERROR_INSTALL_FAILURE;
	}
	if (manager)
		CloseServiceHandle(manager);
	msi_free(name);
	msi_free(display);
	msi_free(arguments);
	msi_free(image);
	return status;
}

static UINT act_install_services(msi_package *package)
{
	UINT status = db_query_each(package->db, L"SELECT `Name`, `DisplayName`, `ServiceType`, `StartType`, `ErrorControl`, `Arguments`, `Component_` FROM `ServiceInstall`", NULL, service_install_row, package);
	return status == ERROR_INSTALL_FAILURE ? status : ERROR_SUCCESS;
}

static UINT service_control_row(msi_record *row, void *context)
{
	struct { msi_package *package; INT mask; } *pass = context;
	WCHAR a[16];
	msi_component *component = component_find(pass->package, record_text(row, 3, a));
	INT event = record_get_int(row, 2);
	if (!component)
		return ERROR_SUCCESS;
	// The low nibble's bits act on install, the high one's on removal.
	INT bits = component->action == INSTALLSTATE_LOCAL ? event & 0xf : component->action == INSTALLSTATE_ABSENT ? event >> 4 & 0xf : 0;
	if (!(bits & pass->mask))
		return ERROR_SUCCESS;
	WCHAR *name = row_format(pass->package, row, 1);
	SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS), service = manager && name ? OpenServiceW(manager, name, SERVICE_ALL_ACCESS) : NULL;
	if (service) {
		SERVICE_STATUS state;
		if (pass->mask == 1)
			StartServiceW(service, 0, NULL);
		else if (pass->mask == 2)
			ControlService(service, SERVICE_CONTROL_STOP, &state);
		else
			DeleteService(service);
		CloseServiceHandle(service);
	}
	if (manager)
		CloseServiceHandle(manager);
	msi_free(name);
	return ERROR_SUCCESS;
}

static UINT service_control(msi_package *package, INT mask)
{
	struct { msi_package *package; INT mask; } pass = { package, mask };
	db_query_each(package->db, L"SELECT `Name`, `Event`, `Component_` FROM `ServiceControl`", NULL, service_control_row, &pass);
	return ERROR_SUCCESS;
}

static UINT act_start_services(msi_package *package) { return service_control(package, 1); }
static UINT act_stop_services(msi_package *package) { return service_control(package, 2); }
static UINT act_delete_services(msi_package *package) { return service_control(package, 8); }

static UINT act_process_components(msi_package *package)
{
	return product_publish_components(package);
}

// Removal drops the registration; the installation writes it, and its undo entries take it away on a failure.
static UINT act_register_product(msi_package *package)
{
	if (package->removing)
		patches_forget(package);
	return package->removing ? product_unregister(package) : product_register(package);
}

static UINT act_publish_product(msi_package *package)
{
	return package->removing ? ERROR_SUCCESS : product_publish(package);
}

static UINT act_publish_features(msi_package *package)
{
	return package->removing ? ERROR_SUCCESS : product_publish_features(package);
}

static UINT act_unpublish_features(msi_package *package)
{
	return package->removing ? product_unregister(package) : ERROR_SUCCESS;
}

/* Related products. */

static UINT upgrade_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	const WCHAR *upgrade = record_text(row, 1, a), *property = record_text(row, 2, b);
	INT attributes = record_get_int(row, 3);
	WCHAR product[39], *code = package_get(package, L"ProductCode");
	if (upgrade && property) {
		for (DWORD i = 0; !MsiEnumRelatedProductsW(upgrade, 0, i, product); i++) {
			if (code && !lstrcmpW(code, product))
				continue;
			(void)attributes;
			WCHAR *existing = package_get(package, property);
			if (existing) {
				SIZE_T room = lstrlenW(existing) + 40;
				WCHAR *list = msi_alloc(room * sizeof(WCHAR));
				wsprintfW(list, L"%s;%s", existing, product);
				package_set(package, property, list);
				msi_free(list);
				msi_free(existing);
			} else
				package_set(package, property, product);
		}
	}
	msi_free(code);
	return ERROR_SUCCESS;
}

static UINT act_find_related(msi_package *package)
{
	db_query_each(package->db, L"SELECT `UpgradeCode`, `ActionProperty`, `Attributes` FROM `Upgrade`", NULL, upgrade_row, package);
	return ERROR_SUCCESS;
}

static UINT remove_existing_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16];
	const WCHAR *property = record_text(row, 1, a);
	INT attributes = record_get_int(row, 2);
	WCHAR *list = property && !(attributes & msidbUpgradeAttributesOnlyDetect) ? package_get(package, property) : NULL;
	UINT status = ERROR_SUCCESS;
	for (WCHAR *at = list; at && *at && !status;) {
		WCHAR *end = at;
		while (*end && *end != ';')
			end++;
		WCHAR kept = *end;
		*end = 0;
		status = MsiConfigureProductW(at, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT);
		*end = kept;
		at = kept ? end + 1 : end;
	}
	msi_free(list);
	return status;
}

static UINT act_remove_existing(msi_package *package)
{
	UINT status = db_query_each(package->db, L"SELECT `ActionProperty`, `Attributes` FROM `Upgrade`", NULL, remove_existing_row, package);
	return status == ERROR_BAD_QUERY_SYNTAX || status == ERROR_INVALID_TABLE ? ERROR_SUCCESS : status;
}

const extra_action extra_actions[] = {
	{ L"CreateFolders", act_create_folders, TRUE }, { L"RemoveFolders", act_remove_folders, TRUE },
	{ L"WriteRegistryValues", act_write_registry, TRUE }, { L"RemoveRegistryValues", act_remove_registry, TRUE },
	{ L"CreateShortcuts", act_shortcuts, TRUE }, { L"RemoveShortcuts", act_shortcuts, TRUE },
	{ L"DuplicateFiles", act_duplicate_files, TRUE }, { L"RemoveDuplicateFiles", act_none_extra, FALSE }, { L"MoveFiles", act_none_extra, FALSE },
	{ L"WriteIniValues", act_ini, TRUE }, { L"RemoveIniValues", act_none_extra, FALSE },
	{ L"WriteEnvironmentStrings", act_environment, TRUE }, { L"RemoveEnvironmentStrings", act_environment, TRUE },
	{ L"SelfRegModules", act_self_register, TRUE }, { L"SelfUnregModules", act_self_unregister, TRUE },
	{ L"RegisterClassInfo", act_classes, TRUE }, { L"UnregisterClassInfo", act_classes, TRUE },
	{ L"RegisterProgIdInfo", act_progids, TRUE }, { L"UnregisterProgIdInfo", act_none_extra, FALSE },
	{ L"RegisterExtensionInfo", act_extensions, TRUE }, { L"UnregisterExtensionInfo", act_none_extra, FALSE },
	{ L"RegisterMIMEInfo", act_none_extra, FALSE }, { L"UnregisterMIMEInfo", act_none_extra, FALSE },
	{ L"RegisterTypeLibraries", act_none_extra, FALSE }, { L"UnregisterTypeLibraries", act_none_extra, FALSE },
	{ L"RegisterFonts", act_fonts, TRUE }, { L"UnregisterFonts", act_none_extra, FALSE },
	{ L"InstallServices", act_install_services, TRUE }, { L"StartServices", act_start_services, TRUE }, { L"StopServices", act_stop_services, TRUE }, { L"DeleteServices", act_delete_services, TRUE },
	{ L"RemoveFiles2", act_remove_listed_files, TRUE },
	{ L"ProcessComponents", act_process_components, TRUE }, { L"RegisterProduct", act_register_product, TRUE }, { L"RegisterUser", act_none_extra, FALSE },
	{ L"PublishProduct", act_publish_product, TRUE }, { L"PublishFeatures", act_publish_features, TRUE }, { L"UnpublishFeatures", act_unpublish_features, TRUE },
	{ L"FindRelatedProducts", act_find_related, FALSE }, { L"MigrateFeatureStates", act_none_extra, FALSE },
	{ L"RemoveExistingProducts", act_remove_existing, FALSE },
	{ NULL, NULL, FALSE },
};
