/* SPDX-License-Identifier: MIT */
#include "msi_own.h"
#include <sddl.h>
#include <rpc.h>

/* What the installer records about products, where Windows records it, and every question that can be asked of it. A
 * Click-to-Run product is never registered: its package's manifests answer instead (clicktorun.c). */

BOOL raven_clicktorun_component(const WCHAR *component, const WCHAR *product, DWORD index, WCHAR *product_code, WCHAR *path);
BOOL raven_clicktorun_qualifier(const WCHAR *category, DWORD index, WCHAR *qualifier, WCHAR *data);
BOOL raven_clicktorun_published(const WCHAR *category, const WCHAR *qualifier, WCHAR *component);
BOOL raven_clicktorun_product(const WCHAR *product);
BOOL raven_clicktorun_feature(const WCHAR *product, const WCHAR *feature);

#define INSTALLER_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Installer"
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
#define MACHINE_SID L"S-1-5-18"

void product_user_sid(WCHAR out[80])
{
	HANDLE token;
	BYTE buffer[256];
	DWORD size;
	WCHAR *text = NULL;
	lstrcpyW(out, L"S-1-5-21-0-0-0-1000");
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return;
	if (GetTokenInformation(token, TokenUser, buffer, sizeof buffer, &size) && ConvertSidToStringSidW(((TOKEN_USER *)buffer)->User.Sid, &text)) {
		lstrcpynW(out, text, 80);
		LocalFree(text);
	}
	CloseHandle(token);
}

BOOL package_is_machine(msi_package *package)
{
	WCHAR *all_users = package_get(package, L"ALLUSERS");
	BOOL machine = all_users && *all_users;
	msi_free(all_users);
	return machine;
}

void user_data_key(WCHAR *key, BOOL machine, const WCHAR *rest)
{
	WCHAR sid[80];
	if (machine)
		lstrcpyW(sid, MACHINE_SID);
	else
		product_user_sid(sid);
	wsprintfW(key, INSTALLER_KEY L"\\UserData\\%s\\%s", sid, rest);
}

void published_key(WCHAR *key, BOOL machine, const WCHAR *rest)
{
	wsprintfW(key, machine ? L"Software\\Classes\\Installer\\%s" : L"Software\\Microsoft\\Installer\\%s", rest);
}

static void write_product_info(msi_package *package, HKEY root, const WCHAR *key)
{
	static const struct { const WCHAR *value, *property; } texts[] = {
		{ L"DisplayName", L"ProductName" }, { L"DisplayVersion", L"ProductVersion" }, { L"Publisher", L"Manufacturer" },
		{ L"HelpLink", L"ARPHELPLINK" }, { L"URLInfoAbout", L"ARPURLINFOABOUT" }, { L"URLUpdateInfo", L"ARPURLUPDATEINFO" },
		{ L"Comments", L"ARPCOMMENTS" }, { L"Contact", L"ARPCONTACT" }, { L"Readme", L"ARPREADME" },
		{ L"InstallLocation", L"ARPINSTALLLOCATION" }, { L"InstallSource", L"SourceDir" }, { L"HelpTelephone", L"ARPHELPTELEPHONE" },
	};
	for (UINT i = 0; i < ARRAYSIZE(texts); i++) {
		WCHAR *value = package_get(package, texts[i].property);
		if (value)
			reg_write_text(package, root, key, texts[i].value, value);
		msi_free(value);
	}
	reg_write_number(package, root, key, L"WindowsInstaller", 1);
	reg_write_number(package, root, key, L"Language", package_get_int(package, L"ProductLanguage", 0));
	WCHAR *version = package_get(package, L"ProductVersion");
	if (version) {
		UINT major = 0, minor = 0, build = 0;
		const WCHAR *at = version;
		while (*at >= '0' && *at <= '9') major = major * 10 + (*at++ - '0');
		if (*at == '.') at++;
		while (*at >= '0' && *at <= '9') minor = minor * 10 + (*at++ - '0');
		if (*at == '.') at++;
		while (*at >= '0' && *at <= '9') build = build * 10 + (*at++ - '0');
		reg_write_number(package, root, key, L"Version", major << 24 | minor << 16 | build);
		reg_write_number(package, root, key, L"VersionMajor", major);
		reg_write_number(package, root, key, L"VersionMinor", minor);
		msi_free(version);
	}
	SYSTEMTIME now;
	WCHAR date[16];
	GetLocalTime(&now);
	wsprintfW(date, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
	reg_write_text(package, root, key, L"InstallDate", date);
}

// The package stays cached so the product can be configured and removed without its source.
static BOOL cache_package(msi_package *package, WCHAR *cached)
{
	WCHAR folder[MAX_PATH];
	GetWindowsDirectoryW(folder, MAX_PATH);
	lstrcatW(folder, L"\\Installer");
	create_directories(package, folder);
	UUID name;
	UuidCreate(&name);
	wsprintfW(cached, L"%s\\%08x%04x.msi", folder, name.Data1, name.Data2);
	undo_file(package, cached, NULL);
	return CopyFileW(package->path, cached, FALSE);
}

UINT product_register(msi_package *package)
{
	WCHAR *code = package_get(package, L"ProductCode");
	if (!code)
		return ERROR_INSTALL_PACKAGE_INVALID;
	BOOL machine = package_is_machine(package);
	HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	WCHAR squashed[33], rest[128], key[256], cached[MAX_PATH];
	guid_squash(code, squashed);

	wsprintfW(rest, L"Products\\%s\\InstallProperties", squashed);
	user_data_key(key, machine, rest);
	HKEY user_data_root = HKEY_LOCAL_MACHINE;
	write_product_info(package, user_data_root, key);
	if (cache_package(package, cached))
		reg_write_text(package, user_data_root, key, L"LocalPackage", cached);

	wsprintfW(key, UNINSTALL_KEY L"\\%s", code);
	write_product_info(package, root, key);
	WCHAR command[96];
	wsprintfW(command, L"MsiExec.exe /X%s", code);
	reg_write_text(package, root, key, L"UninstallString", command);
	reg_write_text(package, root, key, L"ModifyPath", command);
	msi_free(code);
	return ERROR_SUCCESS;
}

UINT product_publish(msi_package *package)
{
	WCHAR *code = package_get(package, L"ProductCode");
	if (!code)
		return ERROR_INSTALL_PACKAGE_INVALID;
	BOOL machine = package_is_machine(package);
	HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	WCHAR squashed[33], rest[128], key[256];
	guid_squash(code, squashed);
	wsprintfW(rest, L"Products\\%s", squashed);
	published_key(key, machine, rest);
	WCHAR *name = package_get(package, L"ProductName");
	if (name)
		reg_write_text(package, root, key, L"ProductName", name);
	msi_free(name);
	reg_write_number(package, root, key, L"Language", package_get_int(package, L"ProductLanguage", 0));
	reg_write_number(package, root, key, L"Assignment", machine);
	WCHAR *transforms = package_get(package, L"TRANSFORMS");
	if (transforms)
		reg_write_text(package, root, key, L"Transforms", transforms);
	msi_free(transforms);
	// Where the package came from: an advertised product has no cached copy and installs from here later.
	const WCHAR *base = package->path;
	for (const WCHAR *at = package->path; *at; at++)
		if (*at == L'\\' || *at == L'/')
			base = at + 1;
	WCHAR list[300], net[300], folder[MAX_PATH];
	lstrcpynW(folder, package->path, min(MAX_PATH, (int)(base - package->path) + 1));
	wsprintfW(list, L"%s\\SourceList", key);
	wsprintfW(net, L"%s\\Net", list);
	// Maintenance runs from the cached copy; the source stays what the installation came from.
	if (!package->maintenance) {
		reg_write_text(package, root, list, L"PackageName", base);
		reg_write_text(package, root, net, L"1", folder);
	}
	WCHAR *upgrade = package_get(package, L"UpgradeCode");
	if (upgrade) {
		WCHAR squashed_upgrade[33];
		if (guid_squash(upgrade, squashed_upgrade)) {
			wsprintfW(rest, L"UpgradeCodes\\%s", squashed_upgrade);
			published_key(key, machine, rest);
			reg_write_text(package, root, key, squashed, L"");
		}
	}
	msi_free(upgrade);
	msi_free(code);
	return ERROR_SUCCESS;
}

UINT product_publish_features(msi_package *package)
{
	WCHAR *code = package_get(package, L"ProductCode");
	if (!code)
		return ERROR_INSTALL_PACKAGE_INVALID;
	BOOL machine = package_is_machine(package);
	WCHAR squashed[33], rest[128], key[256];
	guid_squash(code, squashed);
	wsprintfW(rest, L"Products\\%s\\Features", squashed);
	user_data_key(key, machine, rest);
	for (UINT i = 0; i < package->feature_count; i++) {
		msi_feature *feature = &package->features[i];
		INSTALLSTATE state = feature->action == INSTALLSTATE_UNKNOWN ? feature->installed : feature->action;
		if (state == INSTALLSTATE_LOCAL || state == INSTALLSTATE_ADVERTISED)
			reg_write_text(package, HKEY_LOCAL_MACHINE, key, feature->key, state == INSTALLSTATE_LOCAL ? L"" : L"\x06");
		else if (state == INSTALLSTATE_ABSENT) {
			HKEY handle;
			if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_SET_VALUE, &handle)) {
				undo_registry(package, HKEY_LOCAL_MACHINE, key, feature->key);
				RegDeleteValueW(handle, feature->key);
				RegCloseKey(handle);
			}
		}
	}
	msi_free(code);
	return ERROR_SUCCESS;
}

static WCHAR *component_key_path(msi_package *package, const msi_component *component)
{
	const WCHAR *folder = directory_target(package, component->directory);
	if (!component->key_path)
		return folder ? msi_copy(folder) : NULL;
	if (component->attributes & msidbComponentAttributesRegistryKeyPath) {
		WCHAR *result = msi_copy(L"");
		return result;
	}
	msi_file *file = file_find(package, component->key_path);
	return file ? file_target(package, file) : (folder ? msi_copy(folder) : NULL);
}

UINT product_publish_components(msi_package *package)
{
	WCHAR *code = package_get(package, L"ProductCode");
	if (!code)
		return ERROR_INSTALL_PACKAGE_INVALID;
	BOOL machine = package_is_machine(package);
	WCHAR squashed[33];
	guid_squash(code, squashed);
	for (UINT i = 0; i < package->component_count; i++) {
		msi_component *component = &package->components[i];
		WCHAR squashed_component[33], rest[128], key[256];
		if (!component->guid || !guid_squash(component->guid, squashed_component))
			continue;
		wsprintfW(rest, L"Components\\%s", squashed_component);
		user_data_key(key, machine, rest);
		if (component->action == INSTALLSTATE_LOCAL) {
			WCHAR *path = component_key_path(package, component);
			if (path)
				reg_write_text(package, HKEY_LOCAL_MACHINE, key, squashed, path);
			msi_free(path);
		} else if (component->action == INSTALLSTATE_ABSENT) {
			HKEY handle;
			if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ | KEY_SET_VALUE, &handle)) {
				undo_registry(package, HKEY_LOCAL_MACHINE, key, squashed);
				RegDeleteValueW(handle, squashed);
				DWORD values = 0;
				RegQueryInfoKeyW(handle, NULL, NULL, NULL, NULL, NULL, NULL, &values, NULL, NULL, NULL, NULL);
				RegCloseKey(handle);
				if (!values)
					RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
			}
		}
	}
	msi_free(code);
	return ERROR_SUCCESS;
}

static void delete_tree(HKEY root, const WCHAR *key)
{
	HKEY handle;
	WCHAR name[256];
	if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WRITE, &handle))
		return;
	for (;;) {
		DWORD size = ARRAYSIZE(name);
		if (RegEnumKeyExW(handle, 0, name, &size, NULL, NULL, NULL, NULL))
			break;
		WCHAR child[512];
		wsprintfW(child, L"%s\\%s", key, name);
		delete_tree(root, child);
	}
	RegCloseKey(handle);
	RegDeleteKeyW(root, key);
}

UINT product_unregister(msi_package *package)
{
	WCHAR *code = package_get(package, L"ProductCode");
	if (!code)
		return ERROR_INSTALL_PACKAGE_INVALID;
	BOOL machine = package_is_machine(package);
	HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	WCHAR squashed[33], rest[128], key[256], cached[MAX_PATH];
	guid_squash(code, squashed);
	if (product_local_package(code, cached, MAX_PATH)) {
		SetFileAttributesW(cached, FILE_ATTRIBUTE_NORMAL);
		DeleteFileW(cached);
	}
	wsprintfW(rest, L"Products\\%s", squashed);
	user_data_key(key, machine, rest);
	delete_tree(HKEY_LOCAL_MACHINE, key);
	// The user's feature usage, which a product installed for the machine keeps under the user's own key: it goes, and so does that
	// key once nothing else is in it, so that the product is no longer listed.
	if (machine) {
		user_data_key(key, FALSE, rest);
		WCHAR usage[300];
		wsprintfW(usage, L"%s\\Usage", key);
		RegDeleteKeyW(HKEY_LOCAL_MACHINE, usage);
		RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
	}
	published_key(key, machine, rest);
	delete_tree(root, key);
	wsprintfW(key, UNINSTALL_KEY L"\\%s", code);
	delete_tree(root, key);
	WCHAR *upgrade = package_get(package, L"UpgradeCode");
	WCHAR squashed_upgrade[33];
	if (upgrade && guid_squash(upgrade, squashed_upgrade)) {
		HKEY handle;
		wsprintfW(rest, L"UpgradeCodes\\%s", squashed_upgrade);
		published_key(key, machine, rest);
		if (!RegOpenKeyExW(root, key, 0, KEY_READ | KEY_SET_VALUE, &handle)) {
			RegDeleteValueW(handle, squashed);
			DWORD values = 0;
			RegQueryInfoKeyW(handle, NULL, NULL, NULL, NULL, NULL, NULL, &values, NULL, NULL, NULL, NULL);
			RegCloseKey(handle);
			if (!values)
				RegDeleteKeyW(root, key);
		}
	}
	msi_free(upgrade);
	msi_free(code);
	return ERROR_SUCCESS;
}

/* The questions. */

static BOOL open_properties(const WCHAR *product, HKEY *result, BOOL *machine)
{
	WCHAR squashed[33], rest[128], key[256];
	if (!guid_squash(product, squashed))
		return FALSE;
	wsprintfW(rest, L"Products\\%s\\InstallProperties", squashed);
	for (int pass = 0; pass < 2; pass++) {
		user_data_key(key, pass == 0, rest);
		if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, result)) {
			if (machine)
				*machine = pass == 0;
			return TRUE;
		}
	}
	return FALSE;
}

// The published key of a product, machine first.
static BOOL open_published(const WCHAR *product, HKEY *handle)
{
	WCHAR squashed[33], rest[128], key[256];
	if (!guid_squash(product, squashed))
		return FALSE;
	wsprintfW(rest, L"Products\\%s", squashed);
	for (int pass = 0; pass < 2; pass++) {
		published_key(key, pass == 0, rest);
		if (!RegOpenKeyExW(pass == 0 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key, 0, KEY_READ, handle))
			return TRUE;
	}
	return FALSE;
}

INSTALLSTATE product_state(const WCHAR *product)
{
	HKEY key;
	if (open_properties(product, &key, NULL)) {
		RegCloseKey(key);
		return INSTALLSTATE_DEFAULT;
	}
	if (raven_clicktorun_product(product))
		return INSTALLSTATE_DEFAULT;
	if (open_published(product, &key)) {
		RegCloseKey(key);
		return INSTALLSTATE_ADVERTISED;
	}
	return INSTALLSTATE_UNKNOWN;
}

static BOOL advertised_package(const WCHAR *product, WCHAR *path, DWORD room)
{
	HKEY key;
	if (product_state(product) != INSTALLSTATE_ADVERTISED || !open_published(product, &key))
		return FALSE;
	WCHAR name[MAX_PATH], folder[MAX_PATH];
	DWORD name_size = sizeof name, folder_size = sizeof folder;
	BOOL found = !RegGetValueW(key, L"SourceList", L"PackageName", RRF_RT_REG_SZ, NULL, name, &name_size) &&
		!RegGetValueW(key, L"SourceList\\Net", L"1", RRF_RT_REG_SZ, NULL, folder, &folder_size) &&
		(DWORD)lstrlenW(name) + lstrlenW(folder) < room;
	RegCloseKey(key);
	if (found) {
		lstrcpyW(path, folder);
		lstrcatW(path, name);
	}
	return found;
}

BOOL product_local_package(const WCHAR *product, WCHAR *path, DWORD room)
{
	HKEY key;
	if (!open_properties(product, &key, NULL))
		return FALSE;
	DWORD size = room * sizeof(WCHAR), type;
	BOOL found = !RegQueryValueExW(key, L"LocalPackage", NULL, &type, (BYTE *)path, &size) && type == REG_SZ;
	RegCloseKey(key);
	return found;
}

// A component's recorded key path for a product; TRUE even when the path is empty.
static BOOL component_recorded(const WCHAR *product, const WCHAR *component, WCHAR *path, DWORD room)
{
	WCHAR squashed[33], squashed_component[33], rest[128], key[256];
	if (!guid_squash(product, squashed) || !guid_squash(component, squashed_component))
		return FALSE;
	wsprintfW(rest, L"Components\\%s", squashed_component);
	for (int pass = 0; pass < 2; pass++) {
		HKEY handle;
		user_data_key(key, pass == 0, rest);
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle))
			continue;
		DWORD size = room * sizeof(WCHAR), type;
		LSTATUS status = RegQueryValueExW(handle, squashed, NULL, &type, (BYTE *)path, &size);
		RegCloseKey(handle);
		if (!status)
			return TRUE;
	}
	return FALSE;
}

INSTALLSTATE product_component_installed(const WCHAR *product, const WCHAR *component)
{
	WCHAR path[MAX_PATH * 2];
	if (!component_recorded(product, component, path, ARRAYSIZE(path)))
		return INSTALLSTATE_UNKNOWN;
	return INSTALLSTATE_LOCAL;
}

INSTALLSTATE product_feature_installed(const WCHAR *product, const WCHAR *feature)
{
	WCHAR squashed[33], rest[128], key[256], text[8];
	if (raven_clicktorun_feature(product, feature))
		return INSTALLSTATE_LOCAL;
	if (!guid_squash(product, squashed))
		return INSTALLSTATE_UNKNOWN;
	wsprintfW(rest, L"Products\\%s\\Features", squashed);
	for (int pass = 0; pass < 2; pass++) {
		HKEY handle;
		user_data_key(key, pass == 0, rest);
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle))
			continue;
		DWORD size = sizeof text, type;
		text[0] = 0;
		LSTATUS status = RegQueryValueExW(handle, feature, NULL, &type, (BYTE *)text, &size);
		RegCloseKey(handle);
		if (!status)
			return text[0] == 6 ? INSTALLSTATE_ADVERTISED : INSTALLSTATE_LOCAL;
	}
	return INSTALLSTATE_UNKNOWN;
}

INSTALLSTATE WINAPI MsiQueryProductStateW(LPCWSTR product)
{
	if (!product || lstrlenW(product) != 38)
		return INSTALLSTATE_INVALIDARG;
	return product_state(product);
}

INSTALLSTATE WINAPI MsiQueryFeatureStateW(LPCWSTR product, LPCWSTR feature)
{
	if (!product || !feature || lstrlenW(product) != 38)
		return INSTALLSTATE_INVALIDARG;
	if (product_state(product) == INSTALLSTATE_UNKNOWN)
		return INSTALLSTATE_UNKNOWN;
	return product_feature_installed(product, feature);
}

INSTALLSTATE WINAPI MsiGetComponentPathExW(LPCWSTR product, LPCWSTR component, LPCWSTR user, MSIINSTALLCONTEXT context, LPWSTR out, LPDWORD size)
{
	(void)user;
	(void)context;
	if (!product || !component || lstrlenW(component) != 38)
		return INSTALLSTATE_INVALIDARG;
	WCHAR path[MAX_PATH * 2];
	if (component_recorded(product, component, path, ARRAYSIZE(path))) {
		INSTALLSTATE state = !path[0] || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES ? INSTALLSTATE_LOCAL : INSTALLSTATE_ABSENT;
		return out_text_w(path, lstrlenW(path), out, size) == ERROR_MORE_DATA ? INSTALLSTATE_MOREDATA : state;
	}
	if (raven_clicktorun_component(component, product, 0, NULL, path)) {
		INSTALLSTATE state = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES ? INSTALLSTATE_LOCAL : INSTALLSTATE_ABSENT;
		return out_text_w(path, lstrlenW(path), out, size) == ERROR_MORE_DATA ? INSTALLSTATE_MOREDATA : state;
	}
	return INSTALLSTATE_UNKNOWN;
}

INSTALLSTATE WINAPI MsiGetComponentPathW(LPCWSTR product, LPCWSTR component, LPWSTR out, LPDWORD size)
{
	return MsiGetComponentPathExW(product, component, NULL, MSIINSTALLCONTEXT_ALL, out, size);
}

INSTALLSTATE WINAPI MsiLocateComponentW(LPCWSTR component, LPWSTR out, LPDWORD size)
{
	WCHAR product[39];
	if (MsiGetProductCodeW(component, product))
		return INSTALLSTATE_UNKNOWN;
	return MsiGetComponentPathW(product, component, out, size);
}

UINT WINAPI MsiGetProductCodeW(LPCWSTR component, LPWSTR out)
{
	WCHAR squashed_component[33], key[256], rest[128];
	if (!component || !out || !guid_squash(component, squashed_component))
		return ERROR_INVALID_PARAMETER;
	wsprintfW(rest, L"Components\\%s", squashed_component);
	for (int pass = 0; pass < 2; pass++) {
		HKEY handle;
		user_data_key(key, pass == 0, rest);
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle))
			continue;
		WCHAR name[40];
		DWORD size = ARRAYSIZE(name);
		LSTATUS status = RegEnumValueW(handle, 0, name, &size, NULL, NULL, NULL, NULL);
		RegCloseKey(handle);
		if (!status && guid_unsquash(name, out))
			return ERROR_SUCCESS;
	}
	return raven_clicktorun_component(component, NULL, 0, out, NULL) ? ERROR_SUCCESS : ERROR_UNKNOWN_COMPONENT;
}

UINT WINAPI MsiEnumClientsW(LPCWSTR component, DWORD index, LPWSTR out)
{
	WCHAR squashed_component[33], key[256], rest[128];
	if (!component || !out || !guid_squash(component, squashed_component))
		return ERROR_INVALID_PARAMETER;
	wsprintfW(rest, L"Components\\%s", squashed_component);
	DWORD seen = 0;
	BOOL any = FALSE;
	for (int pass = 0; pass < 2; pass++) {
		HKEY handle;
		user_data_key(key, pass == 0, rest);
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle))
			continue;
		any = TRUE;
		for (DWORD i = 0;; i++, seen++) {
			WCHAR name[40];
			DWORD size = ARRAYSIZE(name);
			if (RegEnumValueW(handle, i, name, &size, NULL, NULL, NULL, NULL))
				break;
			if (seen == index) {
				RegCloseKey(handle);
				return guid_unsquash(name, out) ? ERROR_SUCCESS : ERROR_BAD_CONFIGURATION;
			}
		}
		RegCloseKey(handle);
	}
	if (any)
		return ERROR_NO_MORE_ITEMS;
	if (raven_clicktorun_component(component, NULL, index, out, NULL))
		return ERROR_SUCCESS;
	if (index && raven_clicktorun_component(component, NULL, 0, NULL, NULL))
		return ERROR_NO_MORE_ITEMS;
	return ERROR_UNKNOWN_COMPONENT;
}

UINT WINAPI MsiEnumProductsW(DWORD index, LPWSTR out)
{
	if (!out)
		return ERROR_INVALID_PARAMETER;
	WCHAR key[256];
	DWORD seen = 0;
	for (int pass = 0; pass < 2; pass++) {
		HKEY handle;
		user_data_key(key, pass == 0, L"Products");
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle))
			continue;
		for (DWORD i = 0;; i++, seen++) {
			WCHAR name[40];
			DWORD size = ARRAYSIZE(name);
			if (RegEnumKeyExW(handle, i, name, &size, NULL, NULL, NULL, NULL))
				break;
			if (seen == index) {
				RegCloseKey(handle);
				return guid_unsquash(name, out) ? ERROR_SUCCESS : ERROR_BAD_CONFIGURATION;
			}
		}
		RegCloseKey(handle);
	}
	return ERROR_NO_MORE_ITEMS;
}

UINT WINAPI MsiEnumRelatedProductsW(LPCWSTR upgrade, DWORD reserved, DWORD index, LPWSTR out)
{
	WCHAR squashed[33], rest[128], key[256];
	if (reserved || !out || !guid_squash(upgrade, squashed))
		return ERROR_INVALID_PARAMETER;
	wsprintfW(rest, L"UpgradeCodes\\%s", squashed);
	DWORD seen = 0;
	for (int pass = 0; pass < 2; pass++) {
		HKEY handle, root = pass == 0 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
		published_key(key, pass == 0, rest);
		if (RegOpenKeyExW(root, key, 0, KEY_READ, &handle))
			continue;
		for (DWORD i = 0;; i++, seen++) {
			WCHAR name[40];
			DWORD size = ARRAYSIZE(name);
			if (RegEnumValueW(handle, i, name, &size, NULL, NULL, NULL, NULL))
				break;
			if (seen == index) {
				RegCloseKey(handle);
				return guid_unsquash(name, out) ? ERROR_SUCCESS : ERROR_BAD_CONFIGURATION;
			}
		}
		RegCloseKey(handle);
	}
	return ERROR_NO_MORE_ITEMS;
}

// Installed-product attributes live under InstallProperties, with Windows' two-name scheme.
UINT WINAPI MsiGetProductInfoW(LPCWSTR product, LPCWSTR attribute, LPWSTR out, LPDWORD size)
{
	static const struct { const WCHAR *attribute, *value; } names[] = {
		{ L"ProductName", L"DisplayName" }, { L"InstalledProductName", L"DisplayName" }, { L"VersionString", L"DisplayVersion" },
		{ L"Publisher", L"Publisher" }, { L"HelpLink", L"HelpLink" }, { L"HelpTelephone", L"HelpTelephone" }, { L"InstallDate", L"InstallDate" },
		{ L"InstallLocation", L"InstallLocation" }, { L"InstallSource", L"InstallSource" }, { L"LocalPackage", L"LocalPackage" },
		{ L"URLInfoAbout", L"URLInfoAbout" }, { L"URLUpdateInfo", L"URLUpdateInfo" }, { L"Language", L"Language" }, { L"Version", L"Version" },
		{ L"VersionMinor", L"VersionMinor" }, { L"VersionMajor", L"VersionMajor" },
	};
	if (!product || !attribute || lstrlenW(product) != 38)
		return ERROR_INVALID_PARAMETER;
	HKEY key;
	if (!open_properties(product, &key, NULL))
		return raven_clicktorun_product(product) ? out_text_w(L"", 0, out, size) : ERROR_UNKNOWN_PRODUCT;
	const WCHAR *value = NULL;
	for (UINT i = 0; i < ARRAYSIZE(names); i++)
		if (!lstrcmpW(names[i].attribute, attribute))
			value = names[i].value;
	UINT status;
	if (!value)
		status = ERROR_UNKNOWN_PROPERTY;
	else {
		BYTE data[4096];
		DWORD room = sizeof data - 2, type = 0;
		if (RegQueryValueExW(key, value, NULL, &type, data, &room))
			status = out_text_w(L"", 0, out, size);
		else if (type == REG_DWORD) {
			WCHAR number[16];
			wsprintfW(number, L"%u", *(DWORD *)data);
			status = out_text_w(number, lstrlenW(number), out, size);
		} else {
			data[room] = data[room + 1] = 0;
			status = out_text_w((WCHAR *)data, lstrlenW((WCHAR *)data), out, size);
		}
	}
	RegCloseKey(key);
	return status;
}

UINT WINAPI MsiGetProductInfoExW(LPCWSTR product, LPCWSTR user, MSIINSTALLCONTEXT context, LPCWSTR attribute, LPWSTR out, LPDWORD size)
{
	(void)user;
	(void)context;
	return MsiGetProductInfoW(product, attribute, out, size);
}

// PublishComponents' value: a descriptor, and the application's data behind it.
static BOOL qualifier_lookup(const WCHAR *category, const WCHAR *qualifier, const WCHAR *product, WCHAR owner[39], WCHAR component[39])
{
	WCHAR squashed[33], rest[64], key[256], data[1024];
	if (!guid_squash(category, squashed))
		return FALSE;
	wsprintfW(rest, L"Components\\%s", squashed);
	for (int machine = 1; machine >= 0; machine--) {
		DWORD size = sizeof data;
		published_key(key, machine, rest);
		if (RegGetValueW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key, qualifier, RRF_RT_REG_MULTI_SZ, NULL, data, &size))
			continue;
		WCHAR feature[MAX_FEATURE_CHARS + 1];
		DWORD used = 0;
		if (MsiDecomposeDescriptorW(data, owner, feature, component, &used))
			continue;
		if (!product || !lstrcmpiW(product, owner))
			return TRUE;
	}
	return FALSE;
}

UINT WINAPI MsiProvideQualifiedComponentExW(LPCWSTR category, LPCWSTR qualifier, DWORD mode, LPCWSTR product, DWORD unused1, DWORD unused2, LPWSTR out, LPDWORD size)
{
	(void)mode;
	(void)unused1;
	(void)unused2;
	WCHAR component[39], owner[39];
	if (!category || !qualifier)
		return ERROR_INVALID_PARAMETER;
	if (!qualifier_lookup(category, qualifier, product, owner, component)
		&& (!raven_clicktorun_published(category, qualifier, component) || !raven_clicktorun_component(component, product, 0, owner, NULL)))
		return ERROR_UNKNOWN_COMPONENT;
	INSTALLSTATE state = MsiGetComponentPathExW(owner, component, NULL, MSIINSTALLCONTEXT_ALL, out, size);
	if (state == INSTALLSTATE_MOREDATA)
		return ERROR_MORE_DATA;
	return state == INSTALLSTATE_LOCAL ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

UINT WINAPI MsiProvideQualifiedComponentW(LPCWSTR category, LPCWSTR qualifier, DWORD mode, LPWSTR out, LPDWORD size)
{
	return MsiProvideQualifiedComponentExW(category, qualifier, mode, NULL, 0, 0, out, size);
}

UINT WINAPI MsiEnumComponentQualifiersW(LPCWSTR category, DWORD index, LPWSTR qualifier, LPDWORD qualifier_size, LPWSTR data, LPDWORD data_size)
{
	WCHAR found[MAX_PATH], text[MAX_PATH];
	if (!category)
		return ERROR_INVALID_PARAMETER;
	if (raven_clicktorun_qualifier(category, index, found, text)) {
		UINT status = out_text_w(found, lstrlenW(found), qualifier, qualifier_size);
		UINT other = out_text_w(text, lstrlenW(text), data, data_size);
		return other ? other : status;
	}
	if (index && raven_clicktorun_qualifier(category, 0, found, text))
		return ERROR_NO_MORE_ITEMS;
	return ERROR_UNKNOWN_COMPONENT;
}

UINT WINAPI MsiProvideComponentW(LPCWSTR product, LPCWSTR feature, LPCWSTR component, DWORD mode, LPWSTR out, LPDWORD size)
{
	(void)feature;
	(void)mode;
	INSTALLSTATE state = MsiGetComponentPathW(product, component, out, size);
	if (state == INSTALLSTATE_MOREDATA)
		return ERROR_MORE_DATA;
	if (state == INSTALLSTATE_LOCAL)
		return ERROR_SUCCESS;
	return state == INSTALLSTATE_UNKNOWN ? ERROR_UNKNOWN_COMPONENT : ERROR_FILE_NOT_FOUND;
}

/* Installing and removing, as programs ask for it. */

#ifdef __i386__
static BOOL package_is_64_bit(const WCHAR *path)
{
	MSIHANDLE summary;
	WCHAR text[64];
	DWORD size = ARRAYSIZE(text);
	UINT type;
	INT number;
	FILETIME time;
	BOOL wide = FALSE;
	if (MsiGetSummaryInformationW(0, path, 0, &summary))
		return FALSE;
	if (!MsiSummaryInfoGetPropertyW(summary, PID_TEMPLATE, &type, &number, &time, text, &size))
		wide = !wcsncmp(text, L"x64", 3) || !wcsncmp(text, L"Intel64", 7) || !wcsncmp(text, L"AMD64", 5) || !wcsncmp(text, L"Arm64", 5);
	MsiCloseHandle(summary);
	return wide;
}

// On Windows the installer service does the work, in a 64-bit process whatever the caller is. A 32-bit engine would see the 32-bit
// folders and registry where a 64-bit package wants the others, so such a package goes to the 64-bit msiexec, silently like the caller.
static BOOL install_in_64_bit_engine(const WCHAR *path, const WCHAR *command_line, UINT *status)
{
	BOOL wow64 = FALSE;
	if (!IsWow64Process(GetCurrentProcess(), &wow64) || !wow64 || !package_is_64_bit(path))
		return FALSE;
	const WCHAR *log = package_log_release();
	const size_t size = lstrlenW(path) + lstrlenW(log) + (command_line ? lstrlenW(command_line) : 0) + 128;
	WCHAR *line = msi_alloc(size * sizeof(WCHAR));
	if (!line)
		return FALSE;
	wsprintfW(line, L"msiexec.exe /i \"%s\" /qn", path);
	if (log[0]) {
		lstrcatW(line, L" /l*v \"");
		lstrcatW(line, log);
		lstrcatW(line, L"\"");
	}
	if (command_line && command_line[0]) {
		lstrcatW(line, L" ");
		lstrcatW(line, command_line);
	}
	STARTUPINFOW startup = { sizeof(startup) };
	PROCESS_INFORMATION process;
	BOOL started = CreateProcessW(L"C:\\Windows\\Sysnative\\msiexec.exe", line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
	msi_free(line);
	if (!started)
		return FALSE;
	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD code = ERROR_INSTALL_FAILURE;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	*status = code;
	return TRUE;
}
#endif

UINT WINAPI MsiInstallProductW(LPCWSTR path, LPCWSTR command_line)
{
	if (!path)
		return ERROR_INVALID_PARAMETER;
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return ERROR_FILE_NOT_FOUND;
#ifdef __i386__
	UINT forwarded;
	if (install_in_64_bit_engine(path, command_line, &forwarded))
		return forwarded;
#endif
	msi_package *package;
	if (package_open(path, &package))
		return ERROR_INSTALL_PACKAGE_INVALID;
	package->session = handle_open(&package->base);
	object_ref(&package->base);
	package_command_line(package, command_line);
	// The command line may ask for another action, as advertising does.
	WCHAR *action = package_get(package, L"ACTION");
	if (!action || !action[0])
		package_set(package, L"ACTION", L"INSTALL");
	msi_free(action);
	UINT status = install_run(package);
	MsiCloseHandle(package->session);
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiConfigureProductExW(LPCWSTR product, int level, INSTALLSTATE state, LPCWSTR command_line)
{
	WCHAR cached[MAX_PATH];
	if (!product || lstrlenW(product) != 38)
		return ERROR_INVALID_PARAMETER;
	const BOOL installed = product_local_package(product, cached, MAX_PATH);
	if (!installed && !advertised_package(product, cached, MAX_PATH))
		return product_state(product) == INSTALLSTATE_DEFAULT ? ERROR_SUCCESS : ERROR_UNKNOWN_PRODUCT;
	msi_package *package;
	if (package_open(cached, &package))
		return ERROR_INSTALL_PACKAGE_INVALID;
	package->session = handle_open(&package->base);
	object_ref(&package->base);
	package_command_line(package, command_line);
	package_set(package, L"ACTION", L"INSTALL");
	if (state == INSTALLSTATE_ABSENT)
		package_set(package, L"REMOVE", L"ALL");
	else if (installed && (state == INSTALLSTATE_LOCAL || state == INSTALLSTATE_DEFAULT))
		package_set(package, L"REINSTALL", L"ALL");
	if (level && level != INSTALLLEVEL_DEFAULT) {
		WCHAR text[16];
		wsprintfW(text, L"%d", level);
		package_set(package, L"INSTALLLEVEL", text);
	}
	UINT status = install_run(package);
	MsiCloseHandle(package->session);
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiConfigureProductW(LPCWSTR product, int level, INSTALLSTATE state)
{
	return MsiConfigureProductExW(product, level, state, NULL);
}

UINT WINAPI MsiOpenProductW(LPCWSTR product, MSIHANDLE *handle)
{
	WCHAR cached[MAX_PATH];
	if (!product || !handle || lstrlenW(product) != 38)
		return ERROR_INVALID_PARAMETER;
	if (!product_local_package(product, cached, MAX_PATH))
		return ERROR_UNKNOWN_PRODUCT;
	return MsiOpenPackageExW(cached, 0, handle);
}

UINT WINAPI MsiGetFileVersionW(LPCWSTR path, LPWSTR version, LPDWORD version_size, LPWSTR language, LPDWORD language_size)
{
	if (!path)
		return ERROR_INVALID_PARAMETER;
	DWORD handle, size = GetFileVersionInfoSizeW(path, &handle);
	if (!size) {
		DWORD error = GetLastError();
		return GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES ? ERROR_FILE_NOT_FOUND : error == ERROR_FILE_INVALID ? ERROR_FILE_INVALID : ERROR_FILE_INVALID;
	}
	BYTE *data = msi_alloc(size);
	UINT status = ERROR_FILE_INVALID;
	VS_FIXEDFILEINFO *fixed;
	UINT fixed_size;
	if (GetFileVersionInfoW(path, 0, size, data) && VerQueryValueW(data, L"\\", (void **)&fixed, &fixed_size)) {
		WCHAR text[64];
		wsprintfW(text, L"%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
		status = version || version_size ? out_text_w(text, lstrlenW(text), version, version_size) : ERROR_SUCCESS;
		WORD *translation;
		UINT translation_size;
		if (language_size && VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void **)&translation, &translation_size) && translation_size >= 4) {
			wsprintfW(text, L"%u", translation[0]);
			UINT other = out_text_w(text, lstrlenW(text), language, language_size);
			status = status ? status : other;
		} else if (language_size)
			*language_size = 0;
	}
	msi_free(data);
	return status;
}
