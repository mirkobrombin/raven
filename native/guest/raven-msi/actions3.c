/* SPDX-License-Identifier: MIT */
#include "msi_own.h"
#include <oleauto.h>

/* The standard actions for what fewer packages carry: the machine searches, published components, assemblies, ODBC, type
 * libraries, MIME types, moved files, the removal side of what actions2.c writes, and administrative images. */

static WCHAR *row_text(msi_package *package, msi_record *row, UINT field)
{
	WCHAR scratch[16];
	const WCHAR *text = record_text(row, field, scratch);
	return text ? format_text(package, text, NULL) : NULL;
}

// A name column may hold "short|long"; the long one is the name.
static const WCHAR *long_name(const WCHAR *name)
{
	const WCHAR *result = name;
	for (const WCHAR *at = name; at && *at; at++)
		if (*at == '|')
			result = at + 1;
	return result;
}

static BOOL component_is(msi_package *package, msi_record *row, UINT field, INSTALLSTATE state)
{
	WCHAR scratch[16];
	msi_component *component = component_find(package, record_text(row, field, scratch));
	return component && component->action == state;
}

static WCHAR *with_backslash(const WCHAR *path)
{
	UINT length = lstrlenW(path);
	WCHAR *copy = msi_alloc((length + 2) * sizeof(WCHAR));
	lstrcpyW(copy, path);
	if (length && copy[length - 1] != '\\')
		lstrcatW(copy, L"\\");
	return copy;
}

/* Searching the machine: CompLocator, then RegLocator, IniLocator and DrLocator, each result checked against Signature. */

typedef struct {
	const WCHAR *key;
	BOOL present;
	WCHAR *file, *min_version, *max_version, *languages;
	DWORD min_size, max_size, min_date, max_date;
} signature;

static DWORD null_is_zero(msi_record *row, UINT field)
{
	return record_is_null(row, field) ? 0 : (DWORD)record_get_int(row, field);
}

static UINT signature_row(msi_record *row, void *context)
{
	signature *sig = context;
	WCHAR a[16], b[16], c[16], d[16], e[16];
	if (sig->present || lstrcmpW(record_text(row, 1, a), sig->key))
		return ERROR_SUCCESS;
	const WCHAR *file = record_text(row, 2, b), *low = record_text(row, 3, c), *high = record_text(row, 4, d), *lang = record_text(row, 9, e);
	sig->present = TRUE;
	sig->file = file ? msi_copy(long_name(file)) : NULL;
	sig->min_version = low ? msi_copy(low) : NULL;
	sig->max_version = high ? msi_copy(high) : NULL;
	sig->languages = lang ? msi_copy(lang) : NULL;
	sig->min_size = null_is_zero(row, 5);
	sig->max_size = null_is_zero(row, 6);
	sig->min_date = null_is_zero(row, 7);
	sig->max_date = null_is_zero(row, 8);
	return ERROR_SUCCESS;
}

static void signature_free(signature *sig)
{
	msi_free(sig->file);
	msi_free(sig->min_version);
	msi_free(sig->max_version);
	msi_free(sig->languages);
}

static UINT64 version_number(const WCHAR *text)
{
	UINT64 result = 0;
	const WCHAR *at = text;
	for (int part = 0; part < 4; part++) {
		UINT value = 0;
		while (at && *at >= '0' && *at <= '9')
			value = value * 10 + (*at++ - '0');
		result = (result << 16) | (value & 0xffff);
		if (at && *at == '.')
			at++;
	}
	return result;
}

static BOOL languages_match(const WCHAR *wanted, const WCHAR *have)
{
	for (const WCHAR *at = wanted; *at;) {
		INT number = 0;
		while (*at >= '0' && *at <= '9')
			number = number * 10 + (*at++ - '0');
		for (const WCHAR *other = have; *other;) {
			INT value = 0;
			while (*other >= '0' && *other <= '9')
				value = value * 10 + (*other++ - '0');
			if (value == number)
				return TRUE;
			while (*other && (*other < '0' || *other > '9'))
				other++;
		}
		while (*at && (*at < '0' || *at > '9'))
			at++;
	}
	return FALSE;
}

// A date in the Signature table is packed as MS-DOS keeps one: date in the high word, time in the low one.
static BOOL file_matches(const signature *sig, const WCHAR *path)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		return FALSE;
	if (sig->min_size && data.nFileSizeLow < sig->min_size)
		return FALSE;
	if (sig->max_size && data.nFileSizeLow > sig->max_size)
		return FALSE;
	if (sig->min_date || sig->max_date) {
		FILETIME local;
		WORD date = 0, time = 0;
		FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
		FileTimeToDosDateTime(&local, &date, &time);
		DWORD packed = ((DWORD)date << 16) | time;
		if ((sig->min_date && packed < sig->min_date) || (sig->max_date && packed > sig->max_date))
			return FALSE;
	}
	if (sig->min_version || sig->max_version || sig->languages) {
		WCHAR version[64], language[256];
		DWORD version_room = ARRAYSIZE(version), language_room = ARRAYSIZE(language);
		if (MsiGetFileVersionW(path, version, &version_room, language, &language_room))
			return FALSE;
		UINT64 have = version_number(version);
		if (sig->min_version && have < version_number(sig->min_version))
			return FALSE;
		if (sig->max_version && have > version_number(sig->max_version))
			return FALSE;
		if (sig->languages && !languages_match(sig->languages, language))
			return FALSE;
	}
	return TRUE;
}

// Looks for what the signature describes below a folder, as deep as asked. Finds the file, or the folder itself.
static WCHAR *search_folder(const signature *sig, const WCHAR *folder, INT depth)
{
	WCHAR *base = with_backslash(folder), *found = NULL;
	DWORD attributes = GetFileAttributesW(base);
	if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		msi_free(base);
		return NULL;
	}
	if (!sig->file) {
		found = base;
		return found;
	}
	UINT length = lstrlenW(base);
	WCHAR *path = msi_alloc((length + lstrlenW(sig->file) + 4) * sizeof(WCHAR));
	lstrcpyW(path, base);
	lstrcatW(path, sig->file);
	if (file_matches(sig, path))
		found = path;
	else
		msi_free(path);
	if (!found && depth > 0) {
		WCHAR *pattern = msi_alloc((length + 4) * sizeof(WCHAR));
		wsprintfW(pattern, L"%s*", base);
		WIN32_FIND_DATAW item;
		HANDLE find = FindFirstFileW(pattern, &item);
		msi_free(pattern);
		if (find != INVALID_HANDLE_VALUE) {
			do {
				if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !lstrcmpW(item.cFileName, L".") || !lstrcmpW(item.cFileName, L".."))
					continue;
				WCHAR *below = msi_alloc((length + lstrlenW(item.cFileName) + 2) * sizeof(WCHAR));
				wsprintfW(below, L"%s%s", base, item.cFileName);
				found = search_folder(sig, below, depth - 1);
				msi_free(below);
			} while (!found && FindNextFileW(find, &item));
			FindClose(find);
		}
	}
	msi_free(base);
	return found;
}

// Type 0 of a locator means a folder, 1 a file, 2 the raw value.
static WCHAR *check_value(const signature *sig, const WCHAR *value, INT type)
{
	if (!value || !*value)
		return NULL;
	if ((type & 0x0f) == 2)
		return msi_copy(value);
	if ((type & 0x0f) == 1) {
		signature bare = *sig;
		return file_matches(&bare, value) ? msi_copy(value) : NULL;
	}
	return search_folder(sig, value, 0);
}

typedef struct search_state {
	msi_package *package;
	const WCHAR *key;
	const signature *sig;
	WCHAR *found;
	UINT depth;
} search_state;

static WCHAR *search_signature(msi_package *package, const WCHAR *key, UINT depth);

static UINT comp_locator_row(msi_record *row, void *context)
{
	search_state *search = context;
	WCHAR a[16], b[16];
	if (search->found || lstrcmpW(record_text(row, 1, a), search->key))
		return ERROR_SUCCESS;
	const WCHAR *guid = record_text(row, 2, b);
	INT type = record_is_null(row, 3) ? 1 : record_get_int(row, 3);
	WCHAR path[MAX_PATH * 2];
	DWORD room = ARRAYSIZE(path);
	if (!guid || MsiLocateComponentW(guid, path, &room) != INSTALLSTATE_LOCAL)
		return ERROR_SUCCESS;
	if (!(type & 0x0f)) {
		// The folder of the component's key file.
		WCHAR *cut = path + lstrlenW(path);
		while (cut > path && cut[-1] != '\\')
			cut--;
		*cut = 0;
	}
	search->found = check_value(search->sig, path, type);
	return ERROR_SUCCESS;
}

// A raw registry value reads as the installer's text forms: #number, #xhex, and the list marker.
static WCHAR *registry_text(DWORD type, const BYTE *data, DWORD size)
{
	if (type == REG_DWORD) {
		WCHAR *text = msi_alloc(16 * sizeof(WCHAR));
		wsprintfW(text, L"#%d", *(const INT *)data);
		return text;
	}
	if (type == REG_EXPAND_SZ) {
		DWORD need = ExpandEnvironmentStringsW((const WCHAR *)data, NULL, 0);
		WCHAR *text = msi_alloc((need + 1) * sizeof(WCHAR));
		ExpandEnvironmentStringsW((const WCHAR *)data, text, need + 1);
		return text;
	}
	if (type == REG_SZ)
		return msi_copy((const WCHAR *)data);
	if (type == REG_MULTI_SZ) {
		WCHAR *text = msi_alloc((size / sizeof(WCHAR) + 8) * sizeof(WCHAR));
		WCHAR *out = text;
		for (const WCHAR *at = (const WCHAR *)data; *at; at += lstrlenW(at) + 1) {
			lstrcpyW(out, at);
			out += lstrlenW(out);
			lstrcpyW(out, L"[~]");
			out += 3;
		}
		*out = 0;
		return text;
	}
	WCHAR *text = msi_alloc((size * 2 + 3) * sizeof(WCHAR));
	lstrcpyW(text, L"#x");
	for (DWORD i = 0; i < size; i++)
		wsprintfW(text + 2 + i * 2, L"%02X", data[i]);
	return text;
}

static UINT reg_locator_row(msi_record *row, void *context)
{
	search_state *search = context;
	msi_package *package = search->package;
	WCHAR a[16];
	if (search->found || lstrcmpW(record_text(row, 1, a), search->key))
		return ERROR_SUCCESS;
	INT root_code = record_get_int(row, 2), type = record_is_null(row, 5) ? 0 : record_get_int(row, 5);
	HKEY root = root_code == 0 ? HKEY_CLASSES_ROOT : root_code == 1 ? HKEY_CURRENT_USER : root_code == 2 ? HKEY_LOCAL_MACHINE : root_code == 3 ? HKEY_USERS
		: package_is_machine(package) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	WCHAR *key = row_text(package, row, 3), *name = row_text(package, row, 4);
	REGSAM view = type & 0x10 ? KEY_WOW64_64KEY : sizeof(void *) == 8 ? KEY_WOW64_32KEY : 0;
	HKEY handle;
	if (key && !RegOpenKeyExW(root, key, 0, KEY_READ | view, &handle)) {
		DWORD kind = 0, size = 0;
		if (!RegQueryValueExW(handle, name, NULL, &kind, NULL, &size) && size) {
			BYTE *data = msi_alloc(size + 4);
			RegQueryValueExW(handle, name, NULL, &kind, data, &size);
			data[size] = data[size + 1] = data[size + 2] = data[size + 3] = 0;
			WCHAR *text = registry_text(kind, data, size);
			search->found = check_value(search->sig, text, type);
			msi_free(text);
			msi_free(data);
		}
		RegCloseKey(handle);
	}
	msi_free(key);
	msi_free(name);
	return ERROR_SUCCESS;
}

static UINT ini_locator_row(msi_record *row, void *context)
{
	search_state *search = context;
	WCHAR a[16], b[16], c[16], d[16];
	if (search->found || lstrcmpW(record_text(row, 1, a), search->key))
		return ERROR_SUCCESS;
	const WCHAR *file = record_text(row, 2, b), *section = record_text(row, 3, c), *key = record_text(row, 4, d);
	INT field = record_is_null(row, 5) ? 0 : record_get_int(row, 5), type = record_is_null(row, 6) ? 1 : record_get_int(row, 6);
	if (!file || !section || !key)
		return ERROR_SUCCESS;
	WCHAR value[1024];
	if (!GetPrivateProfileStringW(section, key, L"", value, ARRAYSIZE(value), long_name(file)))
		return ERROR_SUCCESS;
	// Field 0 is the whole value; others count from 1 along its commas.
	const WCHAR *start = value;
	WCHAR *end = NULL;
	for (INT i = 1; i < field && start; i++) {
		while (*start && *start != ',')
			start++;
		start = *start ? start + 1 : NULL;
	}
	if (start && field) {
		for (end = (WCHAR *)start; *end && *end != ','; end++)
			;
		*end = 0;
	}
	if (start)
		search->found = check_value(search->sig, start, type);
	return ERROR_SUCCESS;
}

static UINT dr_locator_row(msi_record *row, void *context)
{
	search_state *search = context;
	msi_package *package = search->package;
	WCHAR a[16], b[16];
	if (search->found || lstrcmpW(record_text(row, 1, a), search->key))
		return ERROR_SUCCESS;
	const WCHAR *parent_key = record_text(row, 2, b);
	WCHAR *path = row_text(package, row, 3);
	INT depth = record_is_null(row, 4) ? 0 : record_get_int(row, 4);
	WCHAR *parent = parent_key && *parent_key ? search_signature(package, parent_key, search->depth + 1) : NULL;
	if (parent_key && *parent_key && !parent) {
		msi_free(path);
		return ERROR_SUCCESS;
	}
	BOOL absolute = path && path[0] && (path[1] == ':' || (path[0] == '\\' && path[1] == '\\'));
	if (parent) {
		WCHAR *cut = parent + lstrlenW(parent);
		DWORD attributes = GetFileAttributesW(parent);
		if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
			while (cut > parent && cut[-1] != '\\')
				cut--;
			*cut = 0;
		}
		WCHAR *base = with_backslash(parent);
		WCHAR *full = msi_alloc((lstrlenW(base) + (path ? lstrlenW(path) : 0) + 1) * sizeof(WCHAR));
		lstrcpyW(full, absolute ? path : base);
		if (path && !absolute)
			lstrcatW(full, path);
		search->found = search_folder(search->sig, full, depth);
		msi_free(full);
		msi_free(base);
		msi_free(parent);
	} else if (absolute)
		search->found = search_folder(search->sig, path, depth);
	else {
		// No anchor: every fixed drive.
		for (WCHAR letter = 'C'; letter <= 'Z' && !search->found; letter++) {
			WCHAR root[4] = { letter, ':', '\\', 0 };
			if (GetDriveTypeW(root) != DRIVE_FIXED)
				continue;
			WCHAR *full = msi_alloc((4 + (path ? lstrlenW(path) : 0)) * sizeof(WCHAR));
			lstrcpyW(full, root);
			if (path)
				lstrcatW(full, path[0] == '\\' ? path + 1 : path);
			search->found = search_folder(search->sig, full, depth);
			msi_free(full);
		}
	}
	msi_free(path);
	return ERROR_SUCCESS;
}

static WCHAR *search_signature(msi_package *package, const WCHAR *key, UINT depth)
{
	if (depth > 16)
		return NULL;
	signature sig = { .key = key };
	db_query_each(package->db, L"SELECT `Signature`, `FileName`, `MinVersion`, `MaxVersion`, `MinSize`, `MaxSize`, `MinDate`, `MaxDate`, `Languages` FROM `Signature`", NULL, signature_row, &sig);
	search_state search = { package, key, &sig, NULL, depth };
	db_query_each(package->db, L"SELECT `Signature_`, `ComponentId`, `Type` FROM `CompLocator`", NULL, comp_locator_row, &search);
	if (!search.found)
		db_query_each(package->db, L"SELECT `Signature_`, `Root`, `Key`, `Name`, `Type` FROM `RegLocator`", NULL, reg_locator_row, &search);
	if (!search.found)
		db_query_each(package->db, L"SELECT `Signature_`, `FileName`, `Section`, `Key`, `Field`, `Type` FROM `IniLocator`", NULL, ini_locator_row, &search);
	if (!search.found)
		db_query_each(package->db, L"SELECT `Signature_`, `Parent`, `Path`, `Depth` FROM `DrLocator`", NULL, dr_locator_row, &search);
	signature_free(&sig);
	return search.found;
}

static UINT app_search_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	const WCHAR *property = record_text(row, 1, a), *key = record_text(row, 2, b);
	if (!property || !key)
		return ERROR_SUCCESS;
	WCHAR *found = search_signature(package, key, 0);
	if (found) {
		package_log(L"AppSearch: %s = %s", property, found);
		package_set(package, property, found);
		msi_free(found);
	}
	return ERROR_SUCCESS;
}

static UINT act_app_search(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Property`, `Signature_` FROM `AppSearch`", NULL, app_search_row, package);
	return ERROR_SUCCESS;
}

/* Published components: a category's qualifiers, each naming the feature and component that answer it. */

static void components_key(WCHAR *key, BOOL machine, const WCHAR *squashed)
{
	WCHAR rest[64];
	wsprintfW(rest, L"Components\\%s", squashed);
	published_key(key, machine, rest);
}

static UINT publish_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16], d[16], squashed[33], key[256];
	const WCHAR *category = record_text(row, 1, a), *qualifier = record_text(row, 2, b), *feature = record_text(row, 5, c);
	msi_component *component = component_find(package, record_text(row, 3, d));
	if (!category || !qualifier || !feature || !component || !component->guid || !guid_squash(category, squashed))
		return ERROR_SUCCESS;
	BOOL machine = package_is_machine(package);
	HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	components_key(key, machine, squashed);
	if (component->action == INSTALLSTATE_ABSENT) {
		undo_registry(package, root, key, qualifier);
		HKEY handle;
		if (!RegOpenKeyExW(root, key, 0, KEY_SET_VALUE, &handle)) {
			RegDeleteValueW(handle, qualifier);
			RegCloseKey(handle);
		}
		return ERROR_SUCCESS;
	}
	if (component->action != INSTALLSTATE_LOCAL)
		return ERROR_SUCCESS;
	WCHAR *code = package_get(package, L"ProductCode"), *data = row_text(package, row, 4);
	if (code) {
		SIZE_T length = lstrlenW(code) + lstrlenW(feature) + 40 + (data ? lstrlenW(data) : 0) + 2;
		WCHAR *value = msi_alloc(length * sizeof(WCHAR));
		wsprintfW(value, L"%s%s>%s%s", code, feature, component->guid, data ? data : L"");
		UINT used = lstrlenW(value) + 1;
		value[used] = 0;
		reg_write(package, root, key, qualifier, REG_MULTI_SZ, value, (used + 1) * sizeof(WCHAR));
		msi_free(value);
	}
	msi_free(code);
	msi_free(data);
	return ERROR_SUCCESS;
}

static UINT act_publish_components(msi_package *package)
{
	db_query_each(package->db, L"SELECT `ComponentId`, `Qualifier`, `Component_`, `AppData`, `Feature_` FROM `PublishComponent`", NULL, publish_row, package);
	return ERROR_SUCCESS;
}

/* Assemblies. One meant for the global cache has no place in the Directory table: its files land in a scratch folder and
 * the cache of its kind takes them from the manifest. */

// The cache interface as fusion and sxs export it; only these slots are called.
typedef struct assembly_cache assembly_cache;
typedef struct {
	HRESULT (WINAPI *QueryInterface)(assembly_cache *, REFIID, void **);
	ULONG (WINAPI *AddRef)(assembly_cache *);
	ULONG (WINAPI *Release)(assembly_cache *);
	HRESULT (WINAPI *UninstallAssembly)(assembly_cache *, DWORD, const WCHAR *, void *, ULONG *);
	HRESULT (WINAPI *QueryAssemblyInfo)(assembly_cache *, DWORD, const WCHAR *, void *);
	HRESULT (WINAPI *CreateAssemblyCacheItem)(assembly_cache *, DWORD, void *, void **, const WCHAR *);
	HRESULT (WINAPI *CreateAssemblyScavenger)(assembly_cache *, IUnknown **);
	HRESULT (WINAPI *InstallAssembly)(assembly_cache *, DWORD, const WCHAR *, void *);
} assembly_cache_vtbl;
struct assembly_cache { const assembly_cache_vtbl *vtbl; };

// A .NET assembly is taken by the newest framework's fusion, whose folder holds it since the framework installed.
static assembly_cache *cache_open(INT kind)
{
	HRESULT (WINAPI *create)(assembly_cache **, DWORD) = NULL;
	if (kind == msidbAssemblyAttributesWin32) {
		HMODULE sxs = LoadLibraryW(L"sxs.dll");
		if (sxs)
			create = (void *)GetProcAddress(sxs, "CreateAssemblyCache");
	} else {
		static const WCHAR *const versions[] = { L"v4.0.30319", L"v2.0.50727" };
		WCHAR windows[MAX_PATH], path[MAX_PATH * 2];
		GetWindowsDirectoryW(windows, MAX_PATH);
		for (UINT i = 0; i < ARRAYSIZE(versions) && !create; i++) {
			wsprintfW(path, L"%s\\Microsoft.NET\\%s\\%s\\fusion.dll", windows, sizeof(void *) == 8 ? L"Framework64" : L"Framework", versions[i]);
			HMODULE fusion = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (fusion)
				create = (void *)GetProcAddress(fusion, "CreateAssemblyCache");
		}
	}
	assembly_cache *cache = NULL;
	if (create && FAILED(create(&cache, 0)))
		cache = NULL;
	return cache;
}

static UINT assembly_load_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16];
	msi_component *component = component_find(package, record_text(row, 1, a));
	// A row naming an application keeps the assembly private to it, an ordinary file.
	if (!component || !record_is_null(row, 3))
		return ERROR_SUCCESS;
	WCHAR folder[MAX_PATH], name[MAX_PATH];
	GetTempPathW(MAX_PATH, folder);
	if (!GetTempFileNameW(folder, L"asm", 0, name))
		return ERROR_SUCCESS;
	DeleteFileW(name);
	component->assembly_folder = with_backslash(name);
	component->assembly_kind = record_is_null(row, 2) ? 0 : record_get_int(row, 2);
	return ERROR_SUCCESS;
}

void assemblies_load(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Component_`, `Attributes`, `File_Application` FROM `MsiAssembly`", NULL, assembly_load_row, package);
}

static void folder_remove(const WCHAR *folder)
{
	WCHAR pattern[MAX_PATH * 2];
	wsprintfW(pattern, L"%s*", folder);
	WIN32_FIND_DATAW item;
	HANDLE find = FindFirstFileW(pattern, &item);
	if (find != INVALID_HANDLE_VALUE) {
		do {
			wsprintfW(pattern, L"%s%s", folder, item.cFileName);
			if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				DeleteFileW(pattern);
		} while (FindNextFileW(find, &item));
		FindClose(find);
	}
	RemoveDirectoryW(folder);
}

static UINT assembly_publish_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	msi_component *component = component_find(package, record_text(row, 1, a));
	msi_file *manifest = file_find(package, record_text(row, 2, b));
	if (!component || !component->assembly_folder || component->action != INSTALLSTATE_LOCAL || !manifest)
		return ERROR_SUCCESS;
	assembly_cache *cache = cache_open(component->assembly_kind);
	if (!cache) {
		package_log(L"No assembly cache for %s", component->key);
		return ERROR_INSTALL_FAILURE;
	}
	WCHAR *path = file_target(package, manifest);
	HRESULT hr = path ? cache->vtbl->InstallAssembly(cache, 2, path, NULL) : E_FAIL;
	package_log(L"Assembly %s into the cache: %08x", path ? path : L"", hr);
	cache->vtbl->Release(cache);
	msi_free(path);
	folder_remove(component->assembly_folder);
	return SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

static UINT act_publish_assemblies(msi_package *package)
{
	return db_query_each(package->db, L"SELECT `Component_`, `File_Manifest` FROM `MsiAssembly`", NULL, assembly_publish_row, package) == ERROR_INSTALL_FAILURE ? ERROR_INSTALL_FAILURE : ERROR_SUCCESS;
}

typedef struct {
	const WCHAR *component;
	WCHAR name[512];
} assembly_name;

// The display name: the Name value, then each other one as key=value.
static UINT assembly_name_row(msi_record *row, void *context)
{
	assembly_name *out = context;
	WCHAR a[16], b[16], c[16];
	const WCHAR *component = record_text(row, 1, a), *name = record_text(row, 2, b), *value = record_text(row, 3, c);
	if (lstrcmpW(component, out->component) || !name || !value)
		return ERROR_SUCCESS;
	UINT used = lstrlenW(out->name);
	if (used + lstrlenW(name) + lstrlenW(value) + 3 >= ARRAYSIZE(out->name))
		return ERROR_SUCCESS;
	if (!lstrcmpiW(name, L"Name")) {
		WCHAR rest[512];
		lstrcpyW(rest, out->name);
		wsprintfW(out->name, L"%s%s", value, rest);
	} else
		wsprintfW(out->name + used, L",%s=%s", name, value);
	return ERROR_SUCCESS;
}

static UINT assembly_unpublish_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16];
	msi_component *component = component_find(package, record_text(row, 1, a));
	if (!component || !component->assembly_folder || component->action != INSTALLSTATE_ABSENT)
		return ERROR_SUCCESS;
	assembly_name name = { component->key, { 0 } };
	db_query_each(package->db, L"SELECT `Component_`, `Name`, `Value` FROM `MsiAssemblyName`", NULL, assembly_name_row, &name);
	assembly_cache *cache = cache_open(component->assembly_kind);
	if (cache) {
		ULONG disposition = 0;
		HRESULT hr = cache->vtbl->UninstallAssembly(cache, 0, name.name, NULL, &disposition);
		package_log(L"Assembly %s out of the cache: %08x", name.name, hr);
		cache->vtbl->Release(cache);
	}
	return ERROR_SUCCESS;
}

static UINT act_unpublish_assemblies(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Component_` FROM `MsiAssembly`", NULL, assembly_unpublish_row, package);
	return ERROR_SUCCESS;
}

/* Costs, in the units of 512 bytes the API speaks. */

UINT component_cost(msi_package *package, const msi_component *component)
{
	UINT64 bytes = 0;
	for (UINT i = 0; i < package->file_count; i++)
		if (package->files[i].component == component)
			bytes += (UINT)package->files[i].size;
	return (UINT)((bytes + 511) / 512);
}

// What CostFinalize leaves for conditions and dialogs: whether the install fits on the volume it goes to.
void costs_publish(msi_package *package)
{
	UINT64 required = 0;
	for (UINT i = 0; i < package->component_count; i++) {
		const msi_component *component = &package->components[i];
		if (component->action == INSTALLSTATE_LOCAL && component->installed != INSTALLSTATE_LOCAL)
			required += component_cost(package, component);
	}
	WCHAR *root = package_get(package, L"ROOTDRIVE"), volume[MAX_PATH], text[32];
	lstrcpynW(volume, root && *root ? root : L"C:\\", MAX_PATH);
	msi_free(root);
	ULARGE_INTEGER free_bytes;
	UINT64 available = GetDiskFreeSpaceExW(volume, &free_bytes, NULL, NULL) ? free_bytes.QuadPart / 512 : required;
	package_set(package, L"OutOfDiskSpace", required > available ? L"1" : L"0");
	package_set(package, L"OutOfNoRbDiskSpace", required > available ? L"1" : L"0");
	package_set(package, L"PrimaryVolumePath", volume);
	wsprintfW(text, L"%I64u", available);
	package_set(package, L"PrimaryVolumeSpaceAvailable", text);
	wsprintfW(text, L"%I64u", required);
	package_set(package, L"PrimaryVolumeSpaceRequired", text);
	wsprintfW(text, L"%I64u", available > required ? available - required : 0);
	package_set(package, L"PrimaryVolumeSpaceRemaining", text);
}

/* ODBC: what the driver manager reads from the registry, as its own setup functions would leave it. */

#define ODBC_INST L"Software\\ODBC\\ODBCINST.INI"
#define ODBC_SOURCES L"Software\\ODBC\\ODBC.INI"

typedef struct {
	msi_package *package;
	const WCHAR *owner;
	HKEY root;
	const WCHAR *key;
} odbc_attributes;

static UINT odbc_attribute_row(msi_record *row, void *context)
{
	odbc_attributes *attributes = context;
	WCHAR a[16], b[16];
	const WCHAR *attribute = record_text(row, 2, b);
	if (lstrcmpW(record_text(row, 1, a), attributes->owner) || !attribute)
		return ERROR_SUCCESS;
	WCHAR *value = row_text(attributes->package, row, 3);
	reg_write_text(attributes->package, attributes->root, attributes->key, attribute, value ? value : L"");
	msi_free(value);
	return ERROR_SUCCESS;
}

static UINT odbc_driver_row_in(msi_package *package, msi_record *row, const WCHAR *list, BOOL driver)
{
	WCHAR a[16], b[16], c[16], d[16], e[16], key[512], listing[256];
	const WCHAR *owner = record_text(row, 1, a), *description = record_text(row, 3, c);
	msi_component *component = component_find(package, record_text(row, 2, b));
	msi_file *file = file_find(package, record_text(row, 4, d)), *setup = file_find(package, record_text(row, 5, e));
	if (!component || !description)
		return ERROR_SUCCESS;
	wsprintfW(key, ODBC_INST L"\\%s", description);
	wsprintfW(listing, ODBC_INST L"\\%s", list);
	HKEY root = HKEY_LOCAL_MACHINE;
	if (component->action == INSTALLSTATE_ABSENT) {
		RegDeleteTreeW(root, key);
		HKEY handle;
		if (!RegOpenKeyExW(root, listing, 0, KEY_SET_VALUE, &handle)) {
			RegDeleteValueW(handle, description);
			RegCloseKey(handle);
		}
		return ERROR_SUCCESS;
	}
	if (component->action != INSTALLSTATE_LOCAL)
		return ERROR_SUCCESS;
	WCHAR *path = file ? file_target(package, file) : NULL, *setup_path = setup ? file_target(package, setup) : NULL;
	reg_write_text(package, root, key, driver ? L"Driver" : L"Translator", path ? path : L"");
	if (setup_path)
		reg_write_text(package, root, key, L"Setup", setup_path);
	reg_write_number(package, root, key, L"UsageCount", 1);
	reg_write_text(package, root, listing, description, L"Installed");
	if (driver) {
		odbc_attributes attributes = { package, owner, root, key };
		db_query_each(package->db, L"SELECT `Driver_`, `Attribute`, `Value` FROM `ODBCAttribute`", NULL, odbc_attribute_row, &attributes);
	}
	msi_free(path);
	msi_free(setup_path);
	return ERROR_SUCCESS;
}

static UINT odbc_driver_row(msi_record *row, void *context)
{
	return odbc_driver_row_in(context, row, L"ODBC Drivers", TRUE);
}

static UINT odbc_translator_row(msi_record *row, void *context)
{
	return odbc_driver_row_in(context, row, L"ODBC Translators", FALSE);
}

static UINT odbc_source_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16], d[16], key[512], listing[256];
	const WCHAR *owner = record_text(row, 1, a), *description = record_text(row, 3, c), *driver = record_text(row, 4, d);
	msi_component *component = component_find(package, record_text(row, 2, b));
	HKEY root = record_get_int(row, 5) == 1 ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
	if (!component || !description)
		return ERROR_SUCCESS;
	wsprintfW(key, ODBC_SOURCES L"\\%s", description);
	lstrcpyW(listing, ODBC_SOURCES L"\\ODBC Data Sources");
	if (component->action == INSTALLSTATE_ABSENT) {
		RegDeleteTreeW(root, key);
		HKEY handle;
		if (!RegOpenKeyExW(root, listing, 0, KEY_SET_VALUE, &handle)) {
			RegDeleteValueW(handle, description);
			RegCloseKey(handle);
		}
		return ERROR_SUCCESS;
	}
	if (component->action != INSTALLSTATE_LOCAL)
		return ERROR_SUCCESS;
	reg_write_text(package, root, listing, description, driver ? driver : L"");
	WCHAR driver_key[512], library[MAX_PATH * 2];
	DWORD size = sizeof library;
	wsprintfW(driver_key, ODBC_INST L"\\%s", driver ? driver : L"");
	if (!RegGetValueW(HKEY_LOCAL_MACHINE, driver_key, L"Driver", RRF_RT_REG_SZ, NULL, library, &size))
		reg_write_text(package, root, key, L"Driver", library);
	odbc_attributes attributes = { package, owner, root, key };
	db_query_each(package->db, L"SELECT `DataSource_`, `Attribute`, `Value` FROM `ODBCSourceAttribute`", NULL, odbc_attribute_row, &attributes);
	return ERROR_SUCCESS;
}

static UINT act_odbc(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Driver`, `Component_`, `Description`, `File_`, `File_Setup` FROM `ODBCDriver`", NULL, odbc_driver_row, package);
	db_query_each(package->db, L"SELECT `Translator`, `Component_`, `Description`, `File_`, `File_Setup` FROM `ODBCTranslator`", NULL, odbc_translator_row, package);
	db_query_each(package->db, L"SELECT `DataSource`, `Component_`, `Description`, `DriverDescription`, `Registration` FROM `ODBCDataSource`", NULL, odbc_source_row, package);
	return ERROR_SUCCESS;
}

// A driver already registered keeps its folder: the new files go where the driver manager expects them.
static UINT odbc_folder_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], key[512], library[MAX_PATH * 2];
	const WCHAR *description = record_text(row, 1, a);
	msi_component *component = component_find(package, record_text(row, 2, b));
	DWORD size = sizeof library;
	if (!description || !component)
		return ERROR_SUCCESS;
	wsprintfW(key, ODBC_INST L"\\%s", description);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"Driver", RRF_RT_REG_SZ, NULL, library, &size))
		return ERROR_SUCCESS;
	WCHAR *cut = library + lstrlenW(library);
	while (cut > library && cut[-1] != '\\')
		cut--;
	*cut = 0;
	msi_directory *entry = directory_find(package, component->directory);
	if (entry && *library) {
		msi_free(entry->target);
		entry->target = msi_copy(library);
	}
	return ERROR_SUCCESS;
}

static UINT act_set_odbc_folders(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Description`, `Component_` FROM `ODBCDriver`", NULL, odbc_folder_row, package);
	return ERROR_SUCCESS;
}

/* Type libraries register from the component's key file, which a Directory_ column may point past. */

typedef HRESULT (WINAPI *load_typelib_ex)(LPCOLESTR, REGKIND, ITypeLib **);
typedef HRESULT (WINAPI *unregister_typelib)(REFGUID, WORD, WORD, LCID, SYSKIND);

static UINT typelib_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	const WCHAR *library_id = record_text(row, 1, a);
	msi_component *component = component_find(package, record_text(row, 3, b));
	if (!library_id || !component)
		return ERROR_SUCCESS;
	HMODULE oleaut = LoadLibraryW(L"oleaut32.dll");
	if (!oleaut)
		return ERROR_SUCCESS;
	if (component->action == INSTALLSTATE_LOCAL) {
		msi_file *file = file_find(package, component->key_path);
		load_typelib_ex load = (load_typelib_ex)GetProcAddress(oleaut, "LoadTypeLibEx");
		WCHAR *path = file ? file_target(package, file) : NULL;
		if (load && path) {
			ITypeLib *library = NULL;
			HRESULT hr = load(path, REGKIND_REGISTER, &library);
			package_log(L"Type library %s: %08x", path, hr);
			if (SUCCEEDED(hr))
				ITypeLib_Release(library);
		}
		msi_free(path);
	} else if (component->action == INSTALLSTATE_ABSENT) {
		unregister_typelib unregister = (unregister_typelib)GetProcAddress(oleaut, "UnRegisterTypeLib");
		GUID guid;
		INT version = record_is_null(row, 4) ? 0 : record_get_int(row, 4);
		if (unregister && SUCCEEDED(CLSIDFromString(library_id, &guid)))
			unregister(&guid, (WORD)(version >> 8), (WORD)(version & 0xff), (LCID)record_get_int(row, 2), sizeof(void *) == 8 ? SYS_WIN64 : SYS_WIN32);
	}
	return ERROR_SUCCESS;
}

static UINT act_typelibs(msi_package *package)
{
	db_query_each(package->db, L"SELECT `LibID`, `Language`, `Component_`, `Version` FROM `TypeLib`", NULL, typelib_row, package);
	return ERROR_SUCCESS;
}

/* MIME types hang off the extension they name. */

static UINT mime_extension_row(msi_record *row, void *context)
{
	struct { const WCHAR *extension; WCHAR *component; } *lookup = context;
	WCHAR a[16], b[16];
	if (!lstrcmpW(record_text(row, 1, a), lookup->extension) && !lookup->component) {
		const WCHAR *component = record_text(row, 2, b);
		lookup->component = component ? msi_copy(component) : NULL;
	}
	return ERROR_SUCCESS;
}

static UINT mime_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16], key[320], dotted[280];
	const WCHAR *type = record_text(row, 1, a), *extension = record_text(row, 2, b), *clsid = record_text(row, 3, c);
	if (!type || !extension)
		return ERROR_SUCCESS;
	struct { const WCHAR *extension; WCHAR *component; } lookup = { extension, NULL };
	db_query_each(package->db, L"SELECT `Extension`, `Component_` FROM `Extension`", NULL, mime_extension_row, &lookup);
	msi_component *component = component_find(package, lookup.component);
	msi_free(lookup.component);
	if (!component)
		return ERROR_SUCCESS;
	wsprintfW(key, L"MIME\\Database\\Content Type\\%s", type);
	if (component->action == INSTALLSTATE_ABSENT)
		RegDeleteTreeW(HKEY_CLASSES_ROOT, key);
	else if (component->action == INSTALLSTATE_LOCAL) {
		wsprintfW(dotted, L".%s", extension);
		reg_write_text(package, HKEY_CLASSES_ROOT, key, L"Extension", dotted);
		if (clsid)
			reg_write_text(package, HKEY_CLASSES_ROOT, key, L"CLSID", clsid);
	}
	return ERROR_SUCCESS;
}

static UINT act_mime(msi_package *package)
{
	db_query_each(package->db, L"SELECT `ContentType`, `Extension_`, `CLSID` FROM `MIME`", NULL, mime_row, package);
	return ERROR_SUCCESS;
}

/* Moved and duplicated files. */

static UINT move_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16], d[16];
	if (!component_is(package, row, 2, INSTALLSTATE_LOCAL))
		return ERROR_SUCCESS;
	const WCHAR *source_name = record_text(row, 3, a), *dest_name = record_text(row, 4, b), *source_key = record_text(row, 5, c), *dest_key = record_text(row, 6, d);
	WCHAR *source_folder = source_key ? package_get(package, source_key) : NULL, *dest_folder = dest_key ? package_get(package, dest_key) : NULL;
	BOOL move = record_get_int(row, 7) == 1;
	if (source_folder && dest_folder && source_name) {
		WCHAR *from = with_backslash(source_folder), *to = with_backslash(dest_folder);
		WCHAR pattern[MAX_PATH * 2], source[MAX_PATH * 2], target[MAX_PATH * 2];
		wsprintfW(pattern, L"%s%s", from, long_name(source_name));
		create_directories(package, to);
		WIN32_FIND_DATAW item;
		HANDLE find = FindFirstFileW(pattern, &item);
		if (find != INVALID_HANDLE_VALUE) {
			do {
				if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;
				wsprintfW(source, L"%s%s", from, item.cFileName);
				wsprintfW(target, L"%s%s", to, dest_name && *dest_name ? long_name(dest_name) : item.cFileName);
				undo_file(package, target, NULL);
				if (move)
					MoveFileExW(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
				else
					CopyFileW(source, target, FALSE);
			} while (FindNextFileW(find, &item));
			FindClose(find);
		}
		msi_free(from);
		msi_free(to);
	}
	msi_free(source_folder);
	msi_free(dest_folder);
	return ERROR_SUCCESS;
}

static UINT act_move_files(msi_package *package)
{
	db_query_each(package->db, L"SELECT `FileKey`, `Component_`, `SourceName`, `DestName`, `SourceFolder`, `DestFolder`, `Options` FROM `MoveFile`", NULL, move_row, package);
	return ERROR_SUCCESS;
}

static UINT duplicate_remove_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16], c[16];
	if (!component_is(package, row, 1, INSTALLSTATE_ABSENT))
		return ERROR_SUCCESS;
	msi_file *file = file_find(package, record_text(row, 2, a));
	const WCHAR *name = record_text(row, 3, b), *folder_key = record_text(row, 4, c);
	WCHAR *property = folder_key ? package_get(package, folder_key) : NULL;
	const WCHAR *folder = property ? property : folder_key ? directory_target(package, folder_key) : file ? directory_target(package, file->component->directory) : NULL;
	const WCHAR *leaf = name && *name ? long_name(name) : file ? file->name : NULL;
	if (folder && leaf) {
		WCHAR *base = with_backslash(folder), path[MAX_PATH * 2];
		wsprintfW(path, L"%s%s", base, leaf);
		DeleteFileW(path);
		msi_free(base);
	}
	msi_free(property);
	return ERROR_SUCCESS;
}

static UINT act_remove_duplicates(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Component_`, `File_`, `DestName`, `DestFolder` FROM `DuplicateFile`", NULL, duplicate_remove_row, package);
	return ERROR_SUCCESS;
}

/* The removal side of INI files, program ids, extensions and fonts. */

static void ini_path(msi_package *package, msi_record *row, WCHAR *path)
{
	WCHAR a[16], b[16], windows[MAX_PATH];
	const WCHAR *name = record_text(row, 1, a), *folder_key = record_text(row, 2, b);
	WCHAR *property = folder_key ? package_get(package, folder_key) : NULL;
	const WCHAR *folder = property ? property : folder_key ? directory_target(package, folder_key) : NULL;
	if (!folder) {
		GetWindowsDirectoryW(windows, MAX_PATH);
		lstrcatW(windows, L"\\");
		folder = windows;
	}
	wsprintfW(path, L"%s%s", folder, name ? long_name(name) : L"");
	msi_free(property);
}

static UINT ini_undo_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR path[MAX_PATH * 2];
	if (!component_is(package, row, 5, INSTALLSTATE_ABSENT))
		return ERROR_SUCCESS;
	ini_path(package, row, path);
	WCHAR *section = row_text(package, row, 3), *key = row_text(package, row, 4);
	WritePrivateProfileStringW(section, key, NULL, path);
	msi_free(section);
	msi_free(key);
	return ERROR_SUCCESS;
}

// A RemoveIniFile row takes a key away when its component goes in; action 4 asks only for one value of a list to go, and
// that is the whole value when the key holds no list.
static UINT ini_remove_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR path[MAX_PATH * 2];
	if (!component_is(package, row, 5, INSTALLSTATE_LOCAL))
		return ERROR_SUCCESS;
	ini_path(package, row, path);
	WCHAR *section = row_text(package, row, 3), *key = row_text(package, row, 4), *wanted = row_text(package, row, 6);
	INT action = record_get_int(row, 7);
	if (section && key) {
		WCHAR current[1024];
		GetPrivateProfileStringW(section, key, L"", current, ARRAYSIZE(current), path);
		if (action == 2 || (action == 4 && wanted && !lstrcmpiW(current, wanted)))
			WritePrivateProfileStringW(section, key, NULL, path);
		else if (action == 4 && wanted) {
			// Drop the matching tag from the comma list.
			WCHAR kept[1024];
			kept[0] = 0;
			for (WCHAR *tag = current; tag && *tag;) {
				WCHAR *end = tag;
				while (*end && *end != ',')
					end++;
				WCHAR saved = *end;
				*end = 0;
				if (lstrcmpiW(tag, wanted)) {
					if (*kept)
						lstrcatW(kept, L",");
					lstrcatW(kept, tag);
				}
				tag = saved ? end + 1 : end;
			}
			WritePrivateProfileStringW(section, key, kept, path);
		}
	}
	msi_free(section);
	msi_free(key);
	msi_free(wanted);
	return ERROR_SUCCESS;
}

static UINT act_remove_ini(msi_package *package)
{
	db_query_each(package->db, L"SELECT `FileName`, `DirProperty`, `Section`, `Key`, `Component_` FROM `IniFile`", NULL, ini_undo_row, package);
	db_query_each(package->db, L"SELECT `FileName`, `DirProperty`, `Section`, `Key`, `Component_`, `Value`, `Action` FROM `RemoveIniFile`", NULL, ini_remove_row, package);
	return ERROR_SUCCESS;
}

static UINT progid_remove_row(msi_record *row, void *context)
{
	WCHAR a[16];
	const WCHAR *progid = record_text(row, 1, a);
	(void)context;
	if (progid)
		RegDeleteTreeW(HKEY_CLASSES_ROOT, progid);
	return ERROR_SUCCESS;
}

static UINT act_unregister_progids(msi_package *package)
{
	if (package->removing)
		db_query_each(package->db, L"SELECT `ProgId` FROM `ProgId`", NULL, progid_remove_row, package);
	return ERROR_SUCCESS;
}

static UINT extension_remove_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], key[280];
	const WCHAR *extension = record_text(row, 1, a);
	if (extension && component_is(package, row, 2, INSTALLSTATE_ABSENT)) {
		wsprintfW(key, L".%s", extension);
		RegDeleteTreeW(HKEY_CLASSES_ROOT, key);
	}
	return ERROR_SUCCESS;
}

static UINT act_unregister_extensions(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Extension`, `Component_` FROM `Extension`", NULL, extension_remove_row, package);
	return ERROR_SUCCESS;
}

static UINT font_remove_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	msi_file *file = file_find(package, record_text(row, 1, a));
	const WCHAR *title = record_text(row, 2, b);
	if (!file || !file->component || file->component->action != INSTALLSTATE_ABSENT)
		return ERROR_SUCCESS;
	WCHAR *path = file_target(package, file);
	if (path) {
		BOOL (WINAPI *remove)(const WCHAR *) = NULL;
		HMODULE gdi = LoadLibraryW(L"gdi32.dll");
		if (gdi)
			remove = (void *)GetProcAddress(gdi, "RemoveFontResourceW");
		if (remove)
			remove(path);
	}
	msi_free(path);
	if (title) {
		HKEY fonts;
		if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_SET_VALUE, &fonts)) {
			RegDeleteValueW(fonts, title);
			RegCloseKey(fonts);
		}
	}
	return ERROR_SUCCESS;
}

static UINT act_unregister_fonts(msi_package *package)
{
	db_query_each(package->db, L"SELECT `File_`, `FontTitle` FROM `Font`", NULL, font_remove_row, package);
	return ERROR_SUCCESS;
}

/* The rest. */

static UINT act_register_user(msi_package *package)
{
	WCHAR *code = package_get(package, L"ProductCode");
	WCHAR squashed[33], rest[128], key[256];
	if (!package->removing && code && guid_squash(code, squashed)) {
		wsprintfW(rest, L"Products\\%s\\InstallProperties", squashed);
		user_data_key(key, package_is_machine(package), rest);
		static const struct { const WCHAR *value, *property; } names[] = {
			{ L"RegOwner", L"USERNAME" }, { L"RegCompany", L"COMPANYNAME" }, { L"ProductID", L"ProductID" },
		};
		for (UINT i = 0; i < ARRAYSIZE(names); i++) {
			WCHAR *value = package_get(package, names[i].property);
			if (value)
				reg_write_text(package, HKEY_LOCAL_MACHINE, key, names[i].value, value);
			msi_free(value);
		}
	}
	msi_free(code);
	return ERROR_SUCCESS;
}

// An upgrade asking for it leaves out what the user had left out of the product it replaces.
static UINT migrate_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16];
	const WCHAR *property = record_text(row, 1, a);
	if (!property || !(record_get_int(row, 2) & msidbUpgradeAttributesMigrateFeatures))
		return ERROR_SUCCESS;
	WCHAR *list = package_get(package, property);
	for (WCHAR *at = list; at && *at;) {
		WCHAR *end = at;
		while (*end && *end != ';')
			end++;
		WCHAR saved = *end;
		*end = 0;
		for (UINT i = 0; i < package->feature_count; i++)
			if (product_feature_installed(at, package->features[i].key) == INSTALLSTATE_ABSENT)
				package->features[i].level = 0;
		at = saved ? end + 1 : end;
	}
	msi_free(list);
	return ERROR_SUCCESS;
}

static UINT act_migrate_features(msi_package *package)
{
	if (!package->maintenance) {
		db_query_each(package->db, L"SELECT `ActionProperty`, `Attributes` FROM `Upgrade`", NULL, migrate_row, package);
		states_select(package);
	}
	return ERROR_SUCCESS;
}

static UINT bind_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16];
	msi_file *file = file_find(package, record_text(row, 1, a));
	if (!file || !file->component || file->component->action != INSTALLSTATE_LOCAL)
		return ERROR_SUCCESS;
	BOOL (WINAPI *bind)(DWORD, const char *, const char *, const char *, void *) = NULL;
	HMODULE library = LoadLibraryW(L"imagehlp.dll");
	if (library)
		bind = (void *)GetProcAddress(library, "BindImageEx");
	WCHAR *path = file_target(package, file), *search = row_text(package, row, 2);
	if (bind && path) {
		char *narrow = msi_to_ansi(path), *folders = search ? msi_to_ansi(search) : NULL;
		bind(0, narrow, folders, NULL, NULL);
		msi_free(narrow);
		msi_free(folders);
	}
	msi_free(path);
	msi_free(search);
	return ERROR_SUCCESS;
}

static UINT act_bind_image(msi_package *package)
{
	db_query_each(package->db, L"SELECT `File_`, `Path` FROM `BindImage`", NULL, bind_row, package);
	return ERROR_SUCCESS;
}

/* An administrative image: the database in the target folder, its files uncompressed along the tree. */

static UINT admin_file_row(msi_record *row, void *context)
{
	WCHAR a[16];
	const WCHAR *key = record_text(row, 1, a);
	INT attributes = record_is_null(row, 2) ? 0 : record_get_int(row, 2);
	attributes = (attributes & ~msidbFileAttributesCompressed) | msidbFileAttributesNoncompressed;
	if (!key)
		return ERROR_SUCCESS;
	WCHAR query[256];
	wsprintfW(query, L"UPDATE `File` SET `Attributes` = %d WHERE `File` = '%s'", attributes, key);
	MSIHANDLE view = 0;
	if (!MsiDatabaseOpenViewW(*(MSIHANDLE *)context, query, &view)) {
		MsiViewExecute(view, 0);
		MsiCloseHandle(view);
	}
	return ERROR_SUCCESS;
}

// An image keeps the source layout below its root: no folder of it stands for a folder of this machine.
static void admin_folder(msi_package *package, const WCHAR *key, const WCHAR *root, WCHAR *out, UINT depth)
{
	msi_directory *entry = directory_find(package, key);
	if (!entry || !entry->parent || depth > 64) {
		lstrcpyW(out, root);
		return;
	}
	admin_folder(package, entry->parent, root, out, depth + 1);
	const WCHAR *name = entry->source_name ? entry->source_name : entry->target_name;
	if (name && lstrcmpW(name, L".")) {
		lstrcatW(out, name);
		lstrcatW(out, L"\\");
	}
}

static UINT act_install_admin(msi_package *package)
{
	WCHAR *target = package_get(package, L"TARGETDIR");
	if (!target || !*target) {
		msi_free(target);
		package_log(L"An administrative image needs TARGETDIR");
		return ERROR_INSTALL_FAILURE;
	}
	WCHAR *folder = with_backslash(target), path[MAX_PATH * 2];
	const WCHAR *leaf = package->path;
	for (const WCHAR *at = package->path; *at; at++)
		if (*at == '\\' || *at == '/')
			leaf = at + 1;
	wsprintfW(path, L"%s%s", folder, leaf);
	UINT status = create_directories(package, folder);
	if (!status && !CopyFileW(package->path, path, FALSE))
		status = ERROR_INSTALL_FAILURE;
	if (!status) {
		undo_file(package, path, NULL);
		SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
		MSIHANDLE database = 0, summary = 0;
		if (!MsiOpenDatabaseW(path, MSIDBOPEN_TRANSACT, &database)) {
			db_query_each(package->db, L"SELECT `File`, `Attributes` FROM `File`", NULL, admin_file_row, &database);
			if (!MsiGetSummaryInformationW(database, NULL, 1, &summary)) {
				INT words = 0;
				UINT type;
				FILETIME time;
				DWORD room = 0;
				MsiSummaryInfoGetPropertyW(summary, PID_WORDCOUNT, &type, &words, &time, NULL, &room);
				// Bit 1 said compressed; bit 2 says this is an administrative image.
				MsiSummaryInfoSetPropertyW(summary, PID_WORDCOUNT, VT_I4, (words & ~2) | 4, NULL, NULL);
				MsiSummaryInfoPersist(summary);
				MsiCloseHandle(summary);
			}
			MsiDatabaseCommit(database);
			MsiCloseHandle(database);
		}
	}
	for (UINT i = 0; i < package->component_count; i++)
		package->components[i].action = INSTALLSTATE_LOCAL;
	for (UINT i = 0; i < package->directory_count; i++) {
		WCHAR place[MAX_PATH * 2];
		admin_folder(package, package->directories[i].key, folder, place, 0);
		msi_free(package->directories[i].target);
		package->directories[i].target = msi_copy(place);
	}
	msi_free(folder);
	msi_free(target);
	return status;
}

/* Delta files: the patch's cabinet gave a difference beside each one, which turns the installed file into the new one. */

static UINT act_patch_files(msi_package *package)
{
	BOOL (WINAPI *apply)(const WCHAR *, const WCHAR *, const WCHAR *, ULONG) = NULL;
	for (UINT i = 0; i < package->file_count; i++) {
		msi_file *file = &package->files[i];
		if (!file->delta_path || !file->component || file->component->action != INSTALLSTATE_LOCAL)
			continue;
		if (!apply) {
			HMODULE library = LoadLibraryW(L"mspatcha.dll");
			if (library)
				apply = (void *)GetProcAddress(library, "ApplyPatchToFileW");
		}
		WCHAR *path = file_target(package, file), scratch[MAX_PATH * 2];
		BOOL done = FALSE;
		if (apply && path) {
			wsprintfW(scratch, L"%s.new", path);
			done = apply(file->delta_path, path, scratch, 0);
			if (done) {
				undo_file(package, scratch, NULL);
				done = MoveFileExW(scratch, path, MOVEFILE_REPLACE_EXISTING);
			}
		}
		package_log(L"Delta for %s: %u", file->key, done);
		DeleteFileW(file->delta_path);
		msi_free(path);
		if (!done)
			return ERROR_INSTALL_FAILURE;
	}
	return ERROR_SUCCESS;
}

const extra_action more_actions[] = {
	{ L"AppSearch", act_app_search, FALSE },
	{ L"PublishComponents", act_publish_components, TRUE }, { L"UnpublishComponents", act_publish_components, TRUE },
	{ L"MsiPublishAssemblies", act_publish_assemblies, TRUE }, { L"MsiUnpublishAssemblies", act_unpublish_assemblies, TRUE },
	{ L"InstallODBC", act_odbc, TRUE }, { L"RemoveODBC", act_odbc, TRUE }, { L"SetODBCFolders", act_set_odbc_folders, FALSE },
	{ L"RegisterTypeLibraries", act_typelibs, TRUE }, { L"UnregisterTypeLibraries", act_typelibs, TRUE },
	{ L"RegisterMIMEInfo", act_mime, TRUE }, { L"UnregisterMIMEInfo", act_mime, TRUE },
	{ L"MoveFiles", act_move_files, TRUE }, { L"RemoveDuplicateFiles", act_remove_duplicates, TRUE },
	{ L"RemoveIniValues", act_remove_ini, TRUE }, { L"UnregisterProgIdInfo", act_unregister_progids, TRUE },
	{ L"UnregisterExtensionInfo", act_unregister_extensions, TRUE }, { L"UnregisterFonts", act_unregister_fonts, TRUE },
	{ L"RegisterUser", act_register_user, TRUE }, { L"MigrateFeatureStates", act_migrate_features, FALSE },
	{ L"BindImage", act_bind_image, TRUE }, { L"InstallAdminPackage", act_install_admin, FALSE },
	{ L"PatchFiles", act_patch_files, TRUE },
	{ NULL, NULL, FALSE },
};
