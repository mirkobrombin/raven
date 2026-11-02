/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Patches: a .msp holds transforms and cabinets for the products its summary names. Applying one changes the package in
 * memory and reinstalls the product from the result, and the product keeps the patch so later maintenance sees it too. */
#include "msi_own.h"

enum { PID_PATCH_TARGETS = 7, PID_PATCH_TRANSFORMS = 8, PID_PATCH_CODE = 9 };

static WCHAR *summary_text(const WCHAR *path, UINT property)
{
	MSIHANDLE summary = 0;
	if (MsiGetSummaryInformationW(0, path, 0, &summary))
		return NULL;
	WCHAR *text = NULL;
	UINT type;
	INT number;
	FILETIME time;
	WCHAR small[1];
	DWORD size = 0;
	if (MsiSummaryInfoGetPropertyW(summary, property, &type, &number, &time, small, &size) == ERROR_MORE_DATA) {
		text = msi_alloc((++size) * sizeof(WCHAR));
		if (MsiSummaryInfoGetPropertyW(summary, property, &type, &number, &time, text, &size)) {
			msi_free(text);
			text = NULL;
		}
	}
	MsiCloseHandle(summary);
	return text;
}

// Finds an item in a list split by semicolons, ignoring case as GUIDs ask.
static BOOL list_has(const WCHAR *list, const WCHAR *item)
{
	UINT length = lstrlenW(item);
	for (const WCHAR *at = list; at && *at;) {
		const WCHAR *end = at;
		while (*end && *end != ';')
			end++;
		if ((UINT)(end - at) == length && CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, at, length, item, length) == CSTR_EQUAL)
			return TRUE;
		at = *end ? end + 1 : end;
	}
	return FALSE;
}

// A patch's code is the first GUID of its revision; superseded ones may follow.
static BOOL patch_code(const WCHAR *path, WCHAR out[39])
{
	WCHAR *revision = summary_text(path, PID_PATCH_CODE);
	BOOL good = revision && lstrlenW(revision) >= 38;
	if (good)
		lstrcpynW(out, revision, 39);
	msi_free(revision);
	return good;
}

// A transform names the products it applies to in its summary's revision; one naming no product applies to any.
static BOOL transform_applies(IStorage *storage, const WCHAR *product)
{
	WCHAR encoded[64];
	IStream *stream = NULL;
	name_encode(L"\005SummaryInformation", FALSE, encoded);
	if (FAILED(IStorage_OpenStream(storage, encoded, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream)))
		return TRUE;
	STATSTG stat;
	BOOL named = FALSE, found = FALSE;
	if (SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME)) && stat.cbSize.LowPart) {
		BYTE *data = msi_alloc(stat.cbSize.LowPart);
		ULONG got = 0;
		char *narrow = msi_to_ansi(product);
		if (SUCCEEDED(IStream_Read(stream, data, stat.cbSize.LowPart, &got)))
			for (ULONG at = 0; at + 38 <= got && !found; at++) {
				if (data[at] != '{' || data[at + 37] != '}')
					continue;
				named = TRUE;
				found = CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, (char *)data + at, 38, narrow, 38) == CSTR_EQUAL;
			}
		msi_free(narrow);
		msi_free(data);
	}
	IStream_Release(stream);
	return found || !named;
}

// Cabinets the patch brings become streams of the package in memory, where the Media rows of its transforms look for them.
static void take_streams(msi_package *package, IStorage *storage)
{
	IEnumSTATSTG *items = NULL;
	if (FAILED(IStorage_EnumElements(storage, 0, NULL, 0, &items)))
		return;
	STATSTG stat;
	while (IEnumSTATSTG_Next(items, 1, &stat, NULL) == S_OK) {
		WCHAR name[128];
		if (stat.type == STGTY_STREAM && lstrlenW(stat.pwcsName) < 64 && !name_decode(stat.pwcsName, name) && name[0] != L'\005' && name[0] != '_' && !stream_find(package->db, name)) {
			IStream *stream = NULL;
			if (SUCCEEDED(IStorage_OpenStream(storage, stat.pwcsName, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream))) {
				BYTE *data = msi_alloc(stat.cbSize.LowPart ? stat.cbSize.LowPart : 1);
				ULONG got = 0;
				IStream_Read(stream, data, stat.cbSize.LowPart, &got);
				msi_blob *blob = blob_new(data, got);
				stream_put(package->db, name, blob);
				blob_unref(blob);
				msi_free(data);
				IStream_Release(stream);
			}
		}
		CoTaskMemFree(stat.pwcsName);
	}
	IEnumSTATSTG_Release(items);
}

static UINT apply_one_patch(msi_package *package, const WCHAR *path, const WCHAR *product)
{
	WCHAR *targets = summary_text(path, PID_PATCH_TARGETS), *transforms = summary_text(path, PID_PATCH_TRANSFORMS);
	UINT status = ERROR_SUCCESS;
	IStorage *storage = NULL;
	if (!targets || !transforms || !list_has(targets, product)) {
		package_log(L"Patch %s does not target %s", path, product);
		status = ERROR_PATCH_TARGET_NOT_FOUND;
	} else if (FAILED(StgOpenStorage(path, NULL, STGM_DIRECT | STGM_READ | STGM_SHARE_DENY_WRITE, NULL, 0, &storage)))
		status = ERROR_PATCH_PACKAGE_OPEN_FAILED;
	else {
		take_streams(package, storage);
		for (const WCHAR *at = transforms; *at && !status;) {
			const WCHAR *end = at;
			while (*end && *end != ';')
				end++;
			if (*at == ':' && end - at < 62) {
				WCHAR name[64], encoded[64];
				lstrcpynW(name, at + 1, (int)(end - at));
				name_encode(name, FALSE, encoded);
				IStorage *transform = NULL;
				HRESULT hr = IStorage_OpenStorage(storage, encoded, NULL, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, NULL, 0, &transform);
				if (FAILED(hr))
					hr = IStorage_OpenStorage(storage, name, NULL, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, NULL, 0, &transform);
				if (SUCCEEDED(hr)) {
					BOOL applies = transform_applies(transform, product);
					if (applies)
						status = db_apply_storage(package->db, transform, 0);
					package_log(L"Patch transform %s: %s, status %u", name, applies ? L"applied" : L"not for this product", status);
					IStorage_Release(transform);
				} else
					status = ERROR_PATCH_PACKAGE_INVALID;
			}
			at = *end ? end + 1 : end;
		}
		IStorage_Release(storage);
	}
	msi_free(targets);
	msi_free(transforms);
	return status;
}

// The product's registered patches, with their cached packages, as a list split by semicolons.
static WCHAR *registered_patches(msi_package *package, const WCHAR *removing)
{
	WCHAR *code = package_get(package, L"ProductCode");
	WCHAR squashed[33], rest[128], key[256];
	WCHAR *list = NULL;
	if (code && guid_squash(code, squashed)) {
		wsprintfW(rest, L"Products\\%s\\Patches", squashed);
		user_data_key(key, package_is_machine(package), rest);
		HKEY handle;
		if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle)) {
			for (DWORD i = 0;; i++) {
				WCHAR name[40], guid[39], local[MAX_PATH], local_key[256];
				DWORD size = ARRAYSIZE(name);
				if (RegEnumKeyExW(handle, i, name, &size, NULL, NULL, NULL, NULL))
					break;
				if (!guid_unsquash(name, guid) || (removing && list_has(removing, guid)))
					continue;
				wsprintfW(rest, L"Patches\\%s", name);
				user_data_key(local_key, package_is_machine(package), rest);
				size = sizeof local;
				if (RegGetValueW(HKEY_LOCAL_MACHINE, local_key, L"LocalPackage", RRF_RT_REG_SZ, NULL, local, &size))
					continue;
				UINT old = list ? lstrlenW(list) : 0;
				WCHAR *longer = msi_alloc((old + lstrlenW(local) + 2) * sizeof(WCHAR));
				longer[0] = 0;
				if (list) {
					lstrcpyW(longer, list);
					lstrcatW(longer, L";");
				}
				lstrcatW(longer, local);
				msi_free(list);
				list = longer;
			}
			RegCloseKey(handle);
		}
	}
	msi_free(code);
	return list;
}

// Applies the package's transforms and patches, once, before its sequence runs; properties they bring beat the table's but
// not the command line's.
UINT package_prepare(msi_package *package)
{
	if (package->prepared)
		return ERROR_SUCCESS;
	package->prepared = TRUE;
	UINT status = ERROR_SUCCESS;
	WCHAR *code = package_get(package, L"ProductCode");
	// Running from the cached copy, the media are where the source list says they were.
	if (code && package->maintenance) {
		WCHAR source[MAX_PATH * 2];
		DWORD size = ARRAYSIZE(source);
		if (!MsiSourceListGetInfoW(code, NULL, package_is_machine(package) ? MSIINSTALLCONTEXT_MACHINE : MSIINSTALLCONTEXT_USERUNMANAGED, MSICODE_PRODUCT, INSTALLPROPERTY_LASTUSEDSOURCE, source, &size) && *source)
			package_set(package, L"SourceDir", source);
	}
	WCHAR *transforms = package_get(package, L"TRANSFORMS"), *patches = package_get(package, L"PATCH");
	WCHAR *removing = package_get(package, L"MSIPATCHREMOVE");
	WCHAR cached[MAX_PATH];
	WCHAR *kept = code && product_local_package(code, cached, MAX_PATH) ? registered_patches(package, removing) : NULL;
	BOOL changed = FALSE;
	for (const WCHAR *at = transforms; at && *at && !status;) {
		const WCHAR *end = at;
		while (*end && *end != ';')
			end++;
		WCHAR name[MAX_PATH];
		// @ and | only say where a transform is kept.
		const WCHAR *start = *at == '@' || *at == '|' ? at + 1 : at;
		lstrcpynW(name, start, min(MAX_PATH, (int)(end - start) + 1));
		if (*name) {
			if (db_apply_transform(package->db, name, 0))
				package_log(L"Cannot apply transform %s", name);
			changed = TRUE;
		}
		at = *end ? end + 1 : end;
	}
	for (int pass = 0; pass < 2 && !status; pass++) {
		const WCHAR *list = pass ? patches : kept;
		for (const WCHAR *at = list; at && *at && !status;) {
			const WCHAR *end = at;
			while (*end && *end != ';')
				end++;
			WCHAR path[MAX_PATH];
			lstrcpynW(path, at, min(MAX_PATH, (int)(end - at) + 1));
			if (*path && code) {
				status = apply_one_patch(package, path, code);
				changed = package->patched = TRUE;
			}
			at = *end ? end + 1 : end;
		}
	}
	if (changed && !status)
		package_reload_properties(package);
	if (removing && code) {
		// The product goes back over every file the removed patches changed: the reinstall compares versions.
		package_set(package, L"REINSTALLMODE", L"amus");
	}
	msi_free(code);
	msi_free(transforms);
	msi_free(patches);
	msi_free(removing);
	msi_free(kept);
	return status;
}

// The product's published patches: one value per patch, and the list of them all.
static void published_list_rebuild(msi_package *package, HKEY root, const WCHAR *key)
{
	HKEY handle;
	if (RegOpenKeyExW(root, key, 0, KEY_READ, &handle))
		return;
	WCHAR list[1024];
	DWORD used = 0;
	for (DWORD i = 0;; i++) {
		WCHAR name[40];
		DWORD length = ARRAYSIZE(name);
		if (RegEnumValueW(handle, i, name, &length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
			break;
		if (!lstrcmpW(name, L"Patches") || used + length + 2 >= ARRAYSIZE(list))
			continue;
		lstrcpyW(list + used, name);
		used += length + 1;
	}
	RegCloseKey(handle);
	list[used++] = 0;
	reg_write(package, root, key, L"Patches", REG_MULTI_SZ, list, used * sizeof(WCHAR));
}

// The patches of this install become part of the product, each with a cached copy.
UINT patches_register(msi_package *package)
{
	WCHAR *code = package_get(package, L"ProductCode"), *patches = package_get(package, L"PATCH");
	WCHAR *removing = package_get(package, L"MSIPATCHREMOVE");
	BOOL machine = package_is_machine(package);
	HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	WCHAR squashed[33], rest[128], key[256], published[256];
	if (!code || !guid_squash(code, squashed)) {
		msi_free(code);
		msi_free(patches);
		msi_free(removing);
		return ERROR_SUCCESS;
	}
	wsprintfW(rest, L"Products\\%s\\Patches", squashed);
	published_key(published, machine, rest);
	for (const WCHAR *at = patches; at && *at;) {
		const WCHAR *end = at;
		while (*end && *end != ';')
			end++;
		WCHAR path[MAX_PATH], guid[39], cached[MAX_PATH], folder[MAX_PATH], one[33];
		lstrcpynW(path, at, min(MAX_PATH, (int)(end - at) + 1));
		at = *end ? end + 1 : end;
		if (!*path || !patch_code(path, guid) || !guid_squash(guid, one))
			continue;
		GetWindowsDirectoryW(folder, MAX_PATH);
		lstrcatW(folder, L"\\Installer");
		create_directories(package, folder);
		UUID name;
		UuidCreate(&name);
		wsprintfW(cached, L"%s\\%08x%04x.msp", folder, name.Data1, name.Data2);
		undo_file(package, cached, NULL);
		if (!CopyFileW(path, cached, FALSE))
			continue;
		wsprintfW(rest, L"Patches\\%s", one);
		user_data_key(key, machine, rest);
		reg_write_text(package, HKEY_LOCAL_MACHINE, key, L"LocalPackage", cached);
		WCHAR *transforms = summary_text(path, PID_PATCH_TRANSFORMS);
		reg_write_text(package, root, published, one, transforms ? transforms : L"");
		msi_free(transforms);
		wsprintfW(rest, L"Products\\%s\\Patches\\%s", squashed, one);
		user_data_key(key, machine, rest);
		reg_write_number(package, HKEY_LOCAL_MACHINE, key, L"State", MSIPATCHSTATE_APPLIED);
		reg_write_number(package, HKEY_LOCAL_MACHINE, key, L"Uninstallable", 1);
		reg_write_number(package, HKEY_LOCAL_MACHINE, key, L"LUAEnabled", 0);
		WCHAR *title = summary_text(path, 2);
		reg_write_text(package, HKEY_LOCAL_MACHINE, key, L"DisplayName", title ? title : L"");
		msi_free(title);
		SYSTEMTIME now;
		WCHAR date[16];
		GetLocalTime(&now);
		wsprintfW(date, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
		reg_write_text(package, HKEY_LOCAL_MACHINE, key, L"Installed", date);
	}
	for (const WCHAR *at = removing; at && *at;) {
		const WCHAR *end = at;
		while (*end && *end != ';')
			end++;
		WCHAR guid[39], one[33];
		lstrcpynW(guid, at, min(39, (int)(end - at) + 1));
		at = *end ? end + 1 : end;
		if (!guid_squash(guid, one))
			continue;
		HKEY handle;
		if (!RegOpenKeyExW(root, published, 0, KEY_SET_VALUE, &handle)) {
			RegDeleteValueW(handle, one);
			RegCloseKey(handle);
		}
		wsprintfW(rest, L"Products\\%s\\Patches\\%s", squashed, one);
		user_data_key(key, machine, rest);
		RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
		wsprintfW(rest, L"Patches\\%s", one);
		user_data_key(key, machine, rest);
		WCHAR local[MAX_PATH];
		DWORD size = sizeof local;
		if (!RegGetValueW(HKEY_LOCAL_MACHINE, key, L"LocalPackage", RRF_RT_REG_SZ, NULL, local, &size))
			DeleteFileW(local);
		RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
	}
	if ((patches && *patches) || (removing && *removing))
		published_list_rebuild(package, root, published);
	msi_free(code);
	msi_free(patches);
	msi_free(removing);
	return ERROR_SUCCESS;
}

// A product going away takes its patches and their cached copies.
void patches_forget(msi_package *package)
{
	WCHAR *code = package_get(package, L"ProductCode");
	WCHAR squashed[33], rest[128], key[256];
	BOOL machine = package_is_machine(package);
	if (code && guid_squash(code, squashed)) {
		wsprintfW(rest, L"Products\\%s\\Patches", squashed);
		user_data_key(key, machine, rest);
		HKEY handle;
		if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle)) {
			for (DWORD i = 0;; i++) {
				WCHAR name[40], one[256], local[MAX_PATH];
				DWORD size = ARRAYSIZE(name);
				if (RegEnumKeyExW(handle, i, name, &size, NULL, NULL, NULL, NULL))
					break;
				wsprintfW(rest, L"Patches\\%s", name);
				user_data_key(one, machine, rest);
				size = sizeof local;
				if (!RegGetValueW(HKEY_LOCAL_MACHINE, one, L"LocalPackage", RRF_RT_REG_SZ, NULL, local, &size))
					DeleteFileW(local);
				RegDeleteTreeW(HKEY_LOCAL_MACHINE, one);
			}
			RegCloseKey(handle);
		}
		wsprintfW(rest, L"Products\\%s\\Patches", squashed);
		published_key(key, machine, rest);
		RegDeleteTreeW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key);
	}
	msi_free(code);
}

static BOOL product_installed_any(const WCHAR *product)
{
	return product_state(product) == INSTALLSTATE_DEFAULT;
}

static UINT apply_list(LPCWSTR patches, LPCWSTR product, LPCWSTR command_line)
{
	UINT length = lstrlenW(patches) + (command_line ? lstrlenW(command_line) : 0) + 64;
	WCHAR *line = msi_alloc(length * sizeof(WCHAR));
	wsprintfW(line, L"PATCH=\"%s\" REINSTALLMODE=omus %s", patches, command_line ? command_line : L"");
	UINT status = MsiConfigureProductExW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_DEFAULT, line);
	msi_free(line);
	return status;
}

UINT WINAPI MsiApplyPatchW(LPCWSTR patch, LPCWSTR install_package, INSTALLTYPE type, LPCWSTR command_line)
{
	if (!patch || !*patch)
		return ERROR_INVALID_PARAMETER;
	if (type != INSTALLTYPE_DEFAULT && type != INSTALLTYPE_NETWORK_IMAGE && type != INSTALLTYPE_SINGLE_INSTANCE)
		return ERROR_INVALID_PARAMETER;
	if (GetFileAttributesW(patch) == INVALID_FILE_ATTRIBUTES)
		return ERROR_PATCH_PACKAGE_OPEN_FAILED;
	WCHAR *targets = summary_text(patch, PID_PATCH_TARGETS);
	if (!targets)
		return ERROR_PATCH_PACKAGE_INVALID;
	UINT status = ERROR_PATCH_TARGET_NOT_FOUND;
	if (type == INSTALLTYPE_NETWORK_IMAGE && install_package) {
		// An administrative image takes the patch through a normal install of its package.
		UINT length = lstrlenW(patch) + (command_line ? lstrlenW(command_line) : 0) + 32;
		WCHAR *line = msi_alloc(length * sizeof(WCHAR));
		wsprintfW(line, L"PATCH=\"%s\" %s", patch, command_line ? command_line : L"");
		status = MsiInstallProductW(install_package, line);
		msi_free(line);
	} else {
		for (const WCHAR *at = targets; *at;) {
			const WCHAR *end = at;
			while (*end && *end != ';')
				end++;
			WCHAR product[39];
			if (end - at == 38) {
				lstrcpynW(product, at, 39);
				if (product_installed_any(product)) {
					status = apply_list(patch, product, command_line);
					break;
				}
			}
			at = *end ? end + 1 : end;
		}
	}
	msi_free(targets);
	return status;
}

UINT WINAPI MsiApplyPatchA(LPCSTR patch, LPCSTR install_package, INSTALLTYPE type, LPCSTR command_line)
{
	WCHAR *a = msi_from_ansi(patch), *b = msi_from_ansi(install_package), *c = msi_from_ansi(command_line);
	UINT status = MsiApplyPatchW(a, b, type, c);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	return status;
}

UINT WINAPI MsiApplyMultiplePatchesW(LPCWSTR patches, LPCWSTR product, LPCWSTR properties)
{
	if (!patches || !*patches)
		return ERROR_INVALID_PARAMETER;
	if (product)
		return apply_list(patches, product, properties);
	UINT status = ERROR_SUCCESS;
	for (const WCHAR *at = patches; *at && !status;) {
		const WCHAR *end = at;
		while (*end && *end != ';')
			end++;
		WCHAR path[MAX_PATH];
		lstrcpynW(path, at, min(MAX_PATH, (int)(end - at) + 1));
		status = MsiApplyPatchW(path, NULL, INSTALLTYPE_DEFAULT, properties);
		at = *end ? end + 1 : end;
	}
	return status;
}

UINT WINAPI MsiApplyMultiplePatchesA(LPCSTR patches, LPCSTR product, LPCSTR properties)
{
	WCHAR *a = msi_from_ansi(patches), *b = msi_from_ansi(product), *c = msi_from_ansi(properties);
	UINT status = MsiApplyMultiplePatchesW(a, b, c);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	return status;
}

UINT WINAPI MsiRemovePatchesW(LPCWSTR patches, LPCWSTR product, INSTALLTYPE type, LPCWSTR command_line)
{
	if (!patches || !*patches || !product || type != INSTALLTYPE_SINGLE_INSTANCE)
		return ERROR_INVALID_PARAMETER;
	UINT length = lstrlenW(patches) + (command_line ? lstrlenW(command_line) : 0) + 40;
	WCHAR *line = msi_alloc(length * sizeof(WCHAR));
	wsprintfW(line, L"MSIPATCHREMOVE=\"%s\" %s", patches, command_line ? command_line : L"");
	UINT status = MsiConfigureProductExW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_DEFAULT, line);
	msi_free(line);
	return status;
}

UINT WINAPI MsiRemovePatchesA(LPCSTR patches, LPCSTR product, INSTALLTYPE type, LPCSTR command_line)
{
	WCHAR *a = msi_from_ansi(patches), *b = msi_from_ansi(product), *c = msi_from_ansi(command_line);
	UINT status = MsiRemovePatchesW(a, b, type, c);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	return status;
}

UINT WINAPI MsiEnumPatchesW(LPCWSTR product, DWORD index, LPWSTR patch, LPWSTR transforms, LPDWORD size)
{
	WCHAR squashed[33], rest[128], key[256];
	if (!product || !guid_squash(product, squashed))
		return ERROR_INVALID_PARAMETER;
	if (!patch || !transforms || !size)
		return ERROR_INVALID_PARAMETER;
	if (product_state(product) == INSTALLSTATE_UNKNOWN)
		return ERROR_UNKNOWN_PRODUCT;
	wsprintfW(rest, L"Products\\%s\\Patches", squashed);
	for (int pass = 0; pass < 2; pass++) {
		published_key(key, pass == 0, rest);
		HKEY handle;
		if (RegOpenKeyExW(pass == 0 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key, 0, KEY_READ, &handle))
			continue;
		WCHAR name[40], data[1024];
		LONG result = ERROR_NO_MORE_ITEMS;
		for (DWORD i = 0, seen = 0;; i++) {
			DWORD length = ARRAYSIZE(name), bytes = sizeof data;
			result = RegEnumValueW(handle, i, name, &length, NULL, NULL, (BYTE *)data, &bytes);
			if (result || (lstrcmpW(name, L"Patches") && seen++ == index))
				break;
		}
		RegCloseKey(handle);
		if (result)
			return ERROR_NO_MORE_ITEMS;
		if (!guid_unsquash(name, patch))
			return ERROR_BAD_CONFIGURATION;
		DWORD need = lstrlenW(data);
		if (*size <= need) {
			*size = need;
			return ERROR_MORE_DATA;
		}
		lstrcpyW(transforms, data);
		*size = need;
		return ERROR_SUCCESS;
	}
	return ERROR_NO_MORE_ITEMS;
}

UINT WINAPI MsiEnumPatchesA(LPCSTR product, DWORD index, LPSTR patch, LPSTR transforms, LPDWORD size)
{
	if (!patch || !transforms || !size)
		return ERROR_INVALID_PARAMETER;
	WCHAR *wide = msi_from_ansi(product), guid[39], list[1024];
	DWORD room = ARRAYSIZE(list);
	UINT status = MsiEnumPatchesW(wide, index, guid, list, &room);
	msi_free(wide);
	if (status)
		return status;
	WideCharToMultiByte(CP_ACP, 0, guid, -1, patch, 39, NULL, NULL);
	DWORD need = WideCharToMultiByte(CP_ACP, 0, list, -1, NULL, 0, NULL, NULL) - 1;
	if (*size <= need) {
		*size = need;
		return ERROR_MORE_DATA;
	}
	WideCharToMultiByte(CP_ACP, 0, list, -1, transforms, *size, NULL, NULL);
	*size = need;
	return ERROR_SUCCESS;
}

static UINT out_text(const WCHAR *text, LPWSTR out, LPDWORD size)
{
	DWORD need = lstrlenW(text);
	if (out && *size > need)
		lstrcpyW(out, text);
	UINT status = !out || *size > need ? ERROR_SUCCESS : ERROR_MORE_DATA;
	*size = need;
	return status;
}

// Answers per installed product in the context asked; a patch is always published for a machine or the current user.
UINT WINAPI MsiGetPatchInfoExW(LPCWSTR patch, LPCWSTR product, LPCWSTR user, MSIINSTALLCONTEXT context, LPCWSTR property, LPWSTR out, LPDWORD size)
{
	WCHAR one[33], target[33], rest[128], key[256];
	if (!patch || !product || !property || !guid_squash(patch, one) || !guid_squash(product, target))
		return ERROR_INVALID_PARAMETER;
	if (out && !size)
		return ERROR_INVALID_PARAMETER;
	if (context == MSIINSTALLCONTEXT_MACHINE && user)
		return ERROR_INVALID_PARAMETER;
	DWORD unused = 0;
	if (!size)
		size = &unused;
	BOOL machine = context == MSIINSTALLCONTEXT_MACHINE;
	if (!lstrcmpW(property, INSTALLPROPERTY_LOCALPACKAGE)) {
		wsprintfW(rest, L"Patches\\%s", one);
		user_data_key(key, machine, rest);
		WCHAR local[MAX_PATH];
		DWORD bytes = sizeof local;
		if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"LocalPackage", RRF_RT_REG_SZ, NULL, local, &bytes))
			return ERROR_UNKNOWN_PATCH;
		return out_text(local, out, size);
	}
	if (!lstrcmpW(property, INSTALLPROPERTY_TRANSFORMS)) {
		wsprintfW(rest, L"Products\\%s\\Patches", target);
		published_key(key, machine, rest);
		WCHAR data[1024];
		DWORD bytes = sizeof data;
		if (RegGetValueW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key, one, RRF_RT_REG_SZ, NULL, data, &bytes))
			return ERROR_UNKNOWN_PATCH;
		return out_text(data, out, size);
	}
	const WCHAR *value = !lstrcmpW(property, INSTALLPROPERTY_DISPLAYNAME) ? L"DisplayName"
		: !lstrcmpW(property, INSTALLPROPERTY_MOREINFOURL) ? L"MoreInfoURL"
		: !lstrcmpW(property, INSTALLPROPERTY_INSTALLDATE) ? L"Installed"
		: !lstrcmpW(property, INSTALLPROPERTY_UNINSTALLABLE) ? L"Uninstallable"
		: !lstrcmpW(property, INSTALLPROPERTY_PATCHSTATE) ? L"State"
		: !lstrcmpW(property, INSTALLPROPERTY_LUAENABLED) ? L"LUAEnabled" : NULL;
	if (!value)
		return ERROR_UNKNOWN_PROPERTY;
	wsprintfW(rest, L"Products\\%s\\Patches\\%s", target, one);
	user_data_key(key, machine, rest);
	BYTE data[1024];
	DWORD bytes = sizeof data, type = 0;
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ | RRF_RT_REG_DWORD, &type, data, &bytes))
		return bytes && !lstrcmpW(value, L"MoreInfoURL") ? out_text(L"", out, size) : ERROR_UNKNOWN_PATCH;
	if (type == REG_DWORD) {
		WCHAR number[16];
		wsprintfW(number, L"%u", *(DWORD *)data);
		return out_text(number, out, size);
	}
	return out_text((WCHAR *)data, out, size);
}

static UINT narrow_out(UINT status, const WCHAR *wide, LPSTR out, LPDWORD size)
{
	if (status && status != ERROR_MORE_DATA)
		return status;
	DWORD need = WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL) - 1;
	UINT result = !out || *size > need ? ERROR_SUCCESS : ERROR_MORE_DATA;
	if (out && *size)
		WideCharToMultiByte(CP_ACP, 0, wide, -1, out, *size, NULL, NULL);
	*size = need;
	return result;
}

UINT WINAPI MsiGetPatchInfoExA(LPCSTR patch, LPCSTR product, LPCSTR user, MSIINSTALLCONTEXT context, LPCSTR property, LPSTR out, LPDWORD size)
{
	WCHAR *a = msi_from_ansi(patch), *b = msi_from_ansi(product), *c = msi_from_ansi(user), *d = msi_from_ansi(property), wide[1024];
	DWORD room = ARRAYSIZE(wide), unused = 0;
	UINT status = MsiGetPatchInfoExW(a, b, c, context, d, wide, &room);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	msi_free(d);
	return narrow_out(status, wide, out, size ? size : &unused);
}

UINT WINAPI MsiGetPatchInfoW(LPCWSTR patch, LPCWSTR attribute, LPWSTR out, LPDWORD size)
{
	if (!patch || !attribute || (out && !size))
		return ERROR_INVALID_PARAMETER;
	if (lstrcmpW(attribute, INSTALLPROPERTY_LOCALPACKAGE))
		return ERROR_UNKNOWN_PROPERTY;
	WCHAR one[33], rest[128], key[256], local[MAX_PATH];
	if (!guid_squash(patch, one))
		return ERROR_INVALID_PARAMETER;
	wsprintfW(rest, L"Patches\\%s", one);
	DWORD unused = 0;
	for (int machine = 1; machine >= 0; machine--) {
		user_data_key(key, machine, rest);
		DWORD bytes = sizeof local;
		if (!RegGetValueW(HKEY_LOCAL_MACHINE, key, L"LocalPackage", RRF_RT_REG_SZ, NULL, local, &bytes))
			return out_text(local, out, size ? size : &unused);
	}
	return ERROR_UNKNOWN_PRODUCT;
}

UINT WINAPI MsiGetPatchInfoA(LPCSTR patch, LPCSTR attribute, LPSTR out, LPDWORD size)
{
	WCHAR *a = msi_from_ansi(patch), *b = msi_from_ansi(attribute), wide[MAX_PATH];
	DWORD room = ARRAYSIZE(wide), unused = 0;
	UINT status = MsiGetPatchInfoW(a, b, wide, &room);
	msi_free(a);
	msi_free(b);
	return narrow_out(status, wide, out, size ? size : &unused);
}

UINT WINAPI MsiEnumPatchesExW(LPCWSTR product, LPCWSTR user, DWORD contexts, DWORD filter, DWORD index, WCHAR patch[39], WCHAR target[39], MSIINSTALLCONTEXT *context_out, LPWSTR sid, LPDWORD sid_size)
{
	(void)user;
	if (!(filter & MSIPATCHSTATE_ALL) || !(contexts & MSIINSTALLCONTEXT_ALL) || (sid && !sid_size))
		return ERROR_INVALID_PARAMETER;
	DWORD seen = 0;
	for (DWORD p = 0;; p++) {
		WCHAR code[39];
		if (product)
			lstrcpynW(code, product, 39);
		else if (MsiEnumProductsW(p, code))
			return ERROR_NO_MORE_ITEMS;
		WCHAR guid[39], list[1024];
		for (DWORD i = 0;; i++) {
			DWORD room = ARRAYSIZE(list);
			if (MsiEnumPatchesW(code, i, guid, list, &room))
				break;
			if (seen++ == index) {
				if (patch)
					lstrcpyW(patch, guid);
				if (target)
					lstrcpyW(target, code);
				if (context_out)
					*context_out = MSIINSTALLCONTEXT_MACHINE;
				if (sid_size) {
					if (sid && *sid_size)
						*sid = 0;
					*sid_size = 0;
				}
				return ERROR_SUCCESS;
			}
		}
		if (product)
			return ERROR_NO_MORE_ITEMS;
	}
}

UINT WINAPI MsiEnumPatchesExA(LPCSTR product, LPCSTR user, DWORD contexts, DWORD filter, DWORD index, char patch[39], char target[39], MSIINSTALLCONTEXT *context_out, LPSTR sid, LPDWORD sid_size)
{
	WCHAR *a = msi_from_ansi(product), *b = msi_from_ansi(user), guid[39], code[39];
	UINT status = MsiEnumPatchesExW(a, b, contexts, filter, index, guid, code, context_out, NULL, sid_size);
	msi_free(a);
	msi_free(b);
	if (status)
		return status;
	if (patch)
		WideCharToMultiByte(CP_ACP, 0, guid, -1, patch, 39, NULL, NULL);
	if (target)
		WideCharToMultiByte(CP_ACP, 0, code, -1, target, 39, NULL, NULL);
	if (sid && sid_size && *sid_size)
		*sid = 0;
	return ERROR_SUCCESS;
}

// Order of application for a product: patch files that target it, as listed.
static UINT sequence_for(const WCHAR *product, DWORD count, PMSIPATCHSEQUENCEINFOW info)
{
	DWORD next = 0;
	for (DWORD i = 0; i < count; i++) {
		info[i].dwOrder = (DWORD)-1;
		info[i].uStatus = ERROR_PATCH_TARGET_NOT_FOUND;
		if (info[i].ePatchDataType != MSIPATCH_DATATYPE_PATCHFILE) {
			info[i].uStatus = ERROR_PATCH_NO_SEQUENCE;
			continue;
		}
		WCHAR *targets = summary_text(info[i].szPatchData, PID_PATCH_TARGETS);
		if (targets && list_has(targets, product)) {
			info[i].dwOrder = next++;
			info[i].uStatus = ERROR_SUCCESS;
		}
		msi_free(targets);
	}
	return ERROR_SUCCESS;
}

UINT WINAPI MsiDetermineApplicablePatchesW(LPCWSTR package_path, DWORD count, PMSIPATCHSEQUENCEINFOW info)
{
	if (!package_path || !*package_path || !count || !info)
		return ERROR_INVALID_PARAMETER;
	msi_package *package;
	if (package_open(package_path, &package))
		return ERROR_PATCH_PACKAGE_OPEN_FAILED;
	WCHAR *code = package_get(package, L"ProductCode");
	UINT status = code ? sequence_for(code, count, info) : ERROR_INSTALL_PACKAGE_INVALID;
	msi_free(code);
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiDeterminePatchSequenceW(LPCWSTR product, LPCWSTR user, MSIINSTALLCONTEXT context, DWORD count, PMSIPATCHSEQUENCEINFOW info)
{
	if (!product || !count || !info || (context == MSIINSTALLCONTEXT_MACHINE && user))
		return ERROR_INVALID_PARAMETER;
	return sequence_for(product, count, info);
}

static PMSIPATCHSEQUENCEINFOW wide_infos(DWORD count, const MSIPATCHSEQUENCEINFOA *info)
{
	PMSIPATCHSEQUENCEINFOW wide = msi_alloc(count * sizeof *wide);
	for (DWORD i = 0; i < count; i++) {
		wide[i].szPatchData = msi_from_ansi(info[i].szPatchData);
		wide[i].ePatchDataType = info[i].ePatchDataType;
		wide[i].dwOrder = info[i].dwOrder;
		wide[i].uStatus = info[i].uStatus;
	}
	return wide;
}

static void wide_infos_finish(DWORD count, PMSIPATCHSEQUENCEINFOW wide, PMSIPATCHSEQUENCEINFOA info)
{
	for (DWORD i = 0; i < count; i++) {
		info[i].dwOrder = wide[i].dwOrder;
		info[i].uStatus = wide[i].uStatus;
		msi_free((WCHAR *)wide[i].szPatchData);
	}
	msi_free(wide);
}

UINT WINAPI MsiDetermineApplicablePatchesA(LPCSTR package_path, DWORD count, PMSIPATCHSEQUENCEINFOA info)
{
	if (!package_path || !count || !info)
		return ERROR_INVALID_PARAMETER;
	WCHAR *path = msi_from_ansi(package_path);
	PMSIPATCHSEQUENCEINFOW wide = wide_infos(count, info);
	UINT status = MsiDetermineApplicablePatchesW(path, count, wide);
	wide_infos_finish(count, wide, info);
	msi_free(path);
	return status;
}

UINT WINAPI MsiDeterminePatchSequenceA(LPCSTR product, LPCSTR user, MSIINSTALLCONTEXT context, DWORD count, PMSIPATCHSEQUENCEINFOA info)
{
	if (!product || !count || !info)
		return ERROR_INVALID_PARAMETER;
	WCHAR *a = msi_from_ansi(product), *b = msi_from_ansi(user);
	PMSIPATCHSEQUENCEINFOW wide = wide_infos(count, info);
	UINT status = MsiDeterminePatchSequenceW(a, b, context, count, wide);
	wide_infos_finish(count, wide, info);
	msi_free(a);
	msi_free(b);
	return status;
}

// The patch's identity, in the shape of the applicability XML, from what its summary says.
UINT WINAPI MsiExtractPatchXMLDataW(LPCWSTR patch, DWORD reserved, LPWSTR out, LPDWORD size)
{
	if (!patch || !*patch || reserved || (out && !size))
		return ERROR_INVALID_PARAMETER;
	WCHAR *targets = summary_text(patch, PID_PATCH_TARGETS);
	WCHAR guid[39];
	if (!targets || !patch_code(patch, guid)) {
		msi_free(targets);
		return ERROR_PATCH_PACKAGE_OPEN_FAILED;
	}
	UINT room = lstrlenW(targets) * 4 + 256;
	WCHAR *xml = msi_alloc(room * sizeof(WCHAR));
	wsprintfW(xml, L"<MsiPatch SchemaVersion=\"1.0.0.0\" PatchGUID=\"%s\" MinMsiVersion=\"3.0\">", guid);
	for (const WCHAR *at = targets; *at;) {
		const WCHAR *end = at;
		while (*end && *end != ';')
			end++;
		lstrcatW(xml, L"<TargetProduct><TargetProductCode>");
		UINT used = lstrlenW(xml);
		lstrcpynW(xml + used, at, (int)(end - at) + 1);
		lstrcatW(xml, L"</TargetProductCode></TargetProduct>");
		at = *end ? end + 1 : end;
	}
	lstrcatW(xml, L"</MsiPatch>");
	DWORD unused = 0;
	UINT status = out_text(xml, out, size ? size : &unused);
	msi_free(xml);
	msi_free(targets);
	return status;
}

UINT WINAPI MsiExtractPatchXMLDataA(LPCSTR patch, DWORD reserved, LPSTR out, LPDWORD size)
{
	WCHAR *path = msi_from_ansi(patch);
	DWORD room = 0;
	UINT status = MsiExtractPatchXMLDataW(path, reserved, NULL, &room);
	if (status) {
		msi_free(path);
		return status;
	}
	WCHAR *wide = msi_alloc((++room) * sizeof(WCHAR));
	status = MsiExtractPatchXMLDataW(path, reserved, wide, &room);
	DWORD unused = 0;
	status = narrow_out(status, wide, out, size ? size : &unused);
	msi_free(wide);
	msi_free(path);
	return status;
}
