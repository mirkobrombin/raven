/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

/* The package's Directory, Feature, Component and File tables in memory, and what the session asks of them: target
 * paths and the states features and components are in and are going to. */

static WCHAR *row_copy(msi_record *row, UINT field)
{
	WCHAR scratch[16];
	const WCHAR *text = record_text(row, field, scratch);
	return text && *text ? msi_copy(text) : NULL;
}

static INT row_int(msi_record *row, UINT field)
{
	INT value = record_get_int(row, field);
	return value == MSI_NULL_INTEGER ? 0 : value;
}

#define GROW(array, count) ((array) = msi_grow((array), ((count) + 1) * sizeof(*(array))))

static UINT directory_row(msi_record *row, void *context)
{
	msi_package *package = context;
	GROW(package->directories, package->directory_count);
	msi_directory *entry = &package->directories[package->directory_count++];
	ZeroMemory(entry, sizeof *entry);
	entry->key = row_copy(row, 1);
	entry->parent = row_copy(row, 2);
	WCHAR *names = row_copy(row, 3);
	// DefaultDir is target:source, each of them short|long; only the long names matter here.
	if (names) {
		WCHAR *source = names;
		while (*source && *source != ':')
			source++;
		if (*source)
			*source++ = 0;
		else
			source = NULL;
		const WCHAR *target_name = names;
		for (const WCHAR *at = names; *at; at++)
			if (*at == '|')
				target_name = at + 1;
		entry->target_name = msi_copy(target_name);
		if (source) {
			const WCHAR *source_name = source;
			for (const WCHAR *at = source; *at; at++)
				if (*at == '|')
					source_name = at + 1;
			entry->source_name = msi_copy(source_name);
		}
		msi_free(names);
	}
	return ERROR_SUCCESS;
}

static UINT component_row(msi_record *row, void *context)
{
	msi_package *package = context;
	GROW(package->components, package->component_count);
	msi_component *entry = &package->components[package->component_count++];
	ZeroMemory(entry, sizeof *entry);
	entry->key = row_copy(row, 1);
	entry->guid = row_copy(row, 2);
	entry->directory = row_copy(row, 3);
	entry->attributes = row_int(row, 4);
	entry->condition = row_copy(row, 5);
	entry->key_path = row_copy(row, 6);
	entry->installed = entry->action = INSTALLSTATE_UNKNOWN;
	entry->enabled = TRUE;
	return ERROR_SUCCESS;
}

static UINT feature_row(msi_record *row, void *context)
{
	msi_package *package = context;
	GROW(package->features, package->feature_count);
	msi_feature *entry = &package->features[package->feature_count++];
	ZeroMemory(entry, sizeof *entry);
	entry->key = row_copy(row, 1);
	entry->parent = row_copy(row, 2);
	entry->title = row_copy(row, 3);
	entry->description = row_copy(row, 4);
	entry->display = row_int(row, 5);
	entry->level = row_int(row, 6);
	entry->directory = row_copy(row, 7);
	entry->attributes = row_int(row, 8);
	entry->installed = entry->action = INSTALLSTATE_UNKNOWN;
	return ERROR_SUCCESS;
}

static UINT feature_component_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	msi_feature *feature = feature_find(package, record_text(row, 1, a));
	msi_component *component = component_find(package, record_text(row, 2, b));
	if (feature && component) {
		GROW(feature->components, feature->component_count);
		feature->components[feature->component_count++] = component;
	}
	return ERROR_SUCCESS;
}

static UINT file_row(msi_record *row, void *context)
{
	msi_package *package = context;
	GROW(package->files, package->file_count);
	msi_file *entry = &package->files[package->file_count++];
	ZeroMemory(entry, sizeof *entry);
	WCHAR scratch[16];
	entry->key = row_copy(row, 1);
	entry->component = component_find(package, record_text(row, 2, scratch));
	WCHAR *names = row_copy(row, 3);
	if (names) {
		const WCHAR *name = names;
		for (const WCHAR *at = names; *at; at++)
			if (*at == '|')
				name = at + 1;
		entry->name = msi_copy(name);
		msi_free(names);
	}
	entry->size = row_int(row, 4);
	entry->version = row_copy(row, 5);
	entry->language = row_copy(row, 6);
	entry->attributes = row_int(row, 7);
	entry->sequence = row_int(row, 8);
	return ERROR_SUCCESS;
}

UINT tables_load(msi_package *package)
{
	if (package->tables_loaded || package->remote)
		return ERROR_SUCCESS;
	package->tables_loaded = TRUE;
	msi_db *db = package->db;
	db_query_each(db, L"SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`", NULL, directory_row, package);
	db_query_each(db, L"SELECT `Component`, `ComponentId`, `Directory_`, `Attributes`, `Condition`, `KeyPath` FROM `Component`", NULL, component_row, package);
	db_query_each(db, L"SELECT `Feature`, `Feature_Parent`, `Title`, `Description`, `Display`, `Level`, `Directory_`, `Attributes` FROM `Feature`", NULL, feature_row, package);
	db_query_each(db, L"SELECT `Feature_`, `Component_` FROM `FeatureComponents`", NULL, feature_component_row, package);
	db_query_each(db, L"SELECT `File`, `Component_`, `FileName`, `FileSize`, `Version`, `Language`, `Attributes`, `Sequence` FROM `File` ORDER BY `Sequence`", NULL, file_row, package);
	return ERROR_SUCCESS;
}

msi_directory *directory_find(msi_package *package, const WCHAR *key)
{
	for (UINT i = 0; key && i < package->directory_count; i++)
		if (!lstrcmpW(package->directories[i].key, key))
			return &package->directories[i];
	return NULL;
}

msi_feature *feature_find(msi_package *package, const WCHAR *key)
{
	for (UINT i = 0; key && i < package->feature_count; i++)
		if (!lstrcmpW(package->features[i].key, key))
			return &package->features[i];
	return NULL;
}

msi_component *component_find(msi_package *package, const WCHAR *key)
{
	for (UINT i = 0; key && i < package->component_count; i++)
		if (!lstrcmpW(package->components[i].key, key))
			return &package->components[i];
	return NULL;
}

msi_file *file_find(msi_package *package, const WCHAR *key)
{
	for (UINT i = 0; key && i < package->file_count; i++)
		if (!lstrcmpW(package->files[i].key, key))
			return &package->files[i];
	return NULL;
}

// Once costing is done every directory is also the property of its name, which formatted text and custom actions read.
void directories_publish(msi_package *package)
{
	for (UINT i = 0; i < package->directory_count; i++) {
		msi_directory *entry = &package->directories[i];
		WCHAR *chosen = package_get(package, entry->key);
		if (!chosen || !*chosen) {
			const WCHAR *target = directory_target(package, entry->key);
			if (target) {
				package_set(package, entry->key, target);
				entry->published = TRUE;
			}
		}
		msi_free(chosen);
	}
}

// A published value still equal to the target only followed it, and goes; a different one was chosen since, and stays.
void directories_reset(msi_package *package)
{
	for (UINT i = 0; i < package->directory_count; i++) {
		msi_directory *entry = &package->directories[i];
		if (entry->published) {
			WCHAR *current = package_get(package, entry->key);
			if (current && entry->target && !lstrcmpW(current, entry->target))
				package_set(package, entry->key, NULL);
			msi_free(current);
			entry->published = FALSE;
		}
		msi_free(entry->target);
		entry->target = NULL;
	}
	if (package->directories_published)
		directories_publish(package);
}

static WCHAR *with_backslash(WCHAR *path)
{
	UINT length = lstrlenW(path);
	if (length && path[length - 1] == '\\')
		return path;
	WCHAR *longer = msi_alloc((length + 2) * sizeof(WCHAR));
	lstrcpyW(longer, path);
	lstrcpyW(longer + length, L"\\");
	msi_free(path);
	return longer;
}

// A property named after the directory wins: that is how folders are chosen on a command line and standard ones found.
const WCHAR *directory_target(msi_package *package, const WCHAR *key)
{
	msi_directory *entry = directory_find(package, key);
	if (!entry)
		return NULL;
	if (entry->target)
		return entry->target;
	if (entry->resolving)
		return NULL;
	entry->resolving = TRUE;
	WCHAR *chosen = package_get(package, key);
	if (chosen && *chosen)
		entry->target = with_backslash(chosen);
	else {
		msi_free(chosen);
		const WCHAR *parent = entry->parent && lstrcmpW(entry->parent, entry->key) ? directory_target(package, entry->parent) : NULL;
		WCHAR *root = NULL;
		if (!parent) {
			root = package_get(package, L"TARGETDIR");
			if (!root || !*root) {
				msi_free(root);
				root = package_get(package, L"ROOTDRIVE");
			}
			parent = root ? root : L"C:\\";
		}
		const WCHAR *name = entry->target_name;
		UINT length = lstrlenW(parent) + (name ? lstrlenW(name) : 0) + 2;
		WCHAR *path = msi_alloc(length * sizeof(WCHAR));
		lstrcpyW(path, parent);
		if (name && lstrcmpW(name, L".") && lstrcmpW(entry->key, L"TARGETDIR"))
			lstrcatW(path, name);
		entry->target = with_backslash(path);
		msi_free(root);
	}
	entry->resolving = FALSE;
	return entry->target;
}

WCHAR *file_target(msi_package *package, const msi_file *file)
{
	const WCHAR *directory = file->component ? file->component->assembly_folder ? file->component->assembly_folder : directory_target(package, file->component->directory) : NULL;
	if (!directory || !file->name)
		return NULL;
	WCHAR *path = msi_alloc((lstrlenW(directory) + lstrlenW(file->name) + 1) * sizeof(WCHAR));
	lstrcpyW(path, directory);
	lstrcatW(path, file->name);
	return path;
}

static BOOL listed(const WCHAR *list, const WCHAR *key)
{
	if (!list)
		return FALSE;
	if (!lstrcmpW(list, L"ALL"))
		return TRUE;
	UINT length = lstrlenW(key);
	for (const WCHAR *at = list; *at;) {
		const WCHAR *end = at;
		while (*end && *end != ',')
			end++;
		if ((UINT)(end - at) == length && !memcmp(at, key, length * sizeof(WCHAR)))
			return TRUE;
		at = *end ? end + 1 : end;
	}
	return FALSE;
}

typedef struct {
	msi_package *package;
	INT install_level;
} level_context;

static UINT condition_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	msi_feature *feature = feature_find(package, record_text(row, 1, a));
	const WCHAR *condition = record_text(row, 3, b);
	if (feature && condition && condition_evaluate(package, condition) == MSICONDITION_TRUE)
		feature->level = record_get_int(row, 2);
	return ERROR_SUCCESS;
}

// Requested states, from the highest priority down: REMOVE, ADDLOCAL, ADDSOURCE, ADVERTISE, then what the install level selects.
void states_select(msi_package *package)
{
	tables_load(package);
	WCHAR *code = package_get(package, L"ProductCode");
	INT install_level = package_get_int(package, L"INSTALLLEVEL", 1);
	WCHAR *remove = package_get(package, L"REMOVE"), *add_local = package_get(package, L"ADDLOCAL");
	WCHAR *add_source = package_get(package, L"ADDSOURCE"), *advertise = package_get(package, L"ADVERTISE");
	WCHAR *reinstall = package_get(package, L"REINSTALL");
	BOOL explicit_request = add_local || add_source || advertise || remove || reinstall;
	package->removing = remove && !lstrcmpW(remove, L"ALL");
	db_query_each(package->db, L"SELECT `Feature_`, `Level`, `Condition` FROM `Condition`", NULL, condition_row, package);
	for (UINT i = 0; i < package->feature_count; i++) {
		msi_feature *feature = &package->features[i];
		feature->installed = code ? product_feature_installed(code, feature->key) : INSTALLSTATE_UNKNOWN;
		if (listed(remove, feature->key))
			feature->action = INSTALLSTATE_ABSENT;
		else if (listed(add_local, feature->key) || listed(add_source, feature->key))
			feature->action = INSTALLSTATE_LOCAL;
		else if (listed(advertise, feature->key))
			feature->action = INSTALLSTATE_ADVERTISED;
		else if (listed(reinstall, feature->key))
			feature->action = feature->installed == INSTALLSTATE_LOCAL ? INSTALLSTATE_LOCAL : INSTALLSTATE_UNKNOWN;
		else if (explicit_request)
			feature->action = INSTALLSTATE_UNKNOWN;
		else if (package->maintenance)
			feature->action = INSTALLSTATE_UNKNOWN;
		else if (feature->level > 0 && feature->level <= install_level)
			feature->action = INSTALLSTATE_LOCAL;
		else
			feature->action = INSTALLSTATE_ABSENT;
	}
	// A child feature cannot be installed without its parent.
	for (UINT i = 0; i < package->feature_count; i++) {
		msi_feature *feature = &package->features[i], *parent = feature_find(package, feature->parent);
		if (parent && parent->action == INSTALLSTATE_ABSENT)
			feature->action = INSTALLSTATE_ABSENT;
	}
	for (UINT i = 0; i < package->component_count; i++) {
		msi_component *component = &package->components[i];
		component->enabled = condition_true(package, component->condition);
		component->installed = code && component->guid ? product_component_installed(code, component->guid) : INSTALLSTATE_UNKNOWN;
		component->action = INSTALLSTATE_UNKNOWN;
	}
	// A component goes in when any feature wanting it does, and out only when all of them leave.
	for (UINT i = 0; i < package->feature_count; i++) {
		msi_feature *feature = &package->features[i];
		INSTALLSTATE wanted = feature->action == INSTALLSTATE_UNKNOWN ? feature->installed : feature->action;
		for (UINT j = 0; j < feature->component_count; j++) {
			msi_component *component = feature->components[j];
			if (wanted == INSTALLSTATE_LOCAL && component->enabled)
				component->action = feature->action == INSTALLSTATE_UNKNOWN ? component->action : INSTALLSTATE_LOCAL;
		}
	}
	for (UINT i = 0; i < package->feature_count; i++) {
		msi_feature *feature = &package->features[i];
		if (feature->action != INSTALLSTATE_ABSENT)
			continue;
		for (UINT j = 0; j < feature->component_count; j++) {
			msi_component *component = feature->components[j];
			if (component->action == INSTALLSTATE_UNKNOWN && component->installed == INSTALLSTATE_LOCAL)
				component->action = INSTALLSTATE_ABSENT;
		}
	}
	msi_free(code);
	msi_free(remove);
	msi_free(add_local);
	msi_free(add_source);
	msi_free(advertise);
	msi_free(reinstall);
	package->costed = TRUE;
}

UINT WINAPI MsiGetTargetPathW(MSIHANDLE handle, LPCWSTR folder, LPWSTR out, LPDWORD size)
{
	if (!folder)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_DIRECTORY;
	if (package->remote) {
		WCHAR *text = NULL;
		status = remote_call(package->remote, REMOTE_TARGET_PATH, folder, NULL, 0, &text, NULL);
		if (!status)
			status = out_text_w(text ? text : L"", text ? lstrlenW(text) : 0, out, size);
		msi_free(text);
	} else {
		tables_load(package);
		const WCHAR *target = directory_target(package, folder);
		if (target)
			status = out_text_w(target, lstrlenW(target), out, size);
	}
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiGetTargetPathA(MSIHANDLE handle, LPCSTR folder, LPSTR out, LPDWORD size)
{
	if (!folder)
		return ERROR_INVALID_PARAMETER;
	WCHAR *wide = msi_from_ansi(folder), path[MAX_PATH * 4];
	DWORD room = ARRAYSIZE(path);
	UINT status = MsiGetTargetPathW(handle, wide, path, &room);
	msi_free(wide);
	return status ? status : out_text_a(path, room, out, size);
}

UINT WINAPI MsiGetSourcePathW(MSIHANDLE handle, LPCWSTR folder, LPWSTR out, LPDWORD size)
{
	if (!folder)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	WCHAR *source = package_get(package, L"SourceDir");
	UINT status = source ? out_text_w(source, lstrlenW(source), out, size) : ERROR_DIRECTORY;
	msi_free(source);
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiGetSourcePathA(MSIHANDLE handle, LPCSTR folder, LPSTR out, LPDWORD size)
{
	if (!folder)
		return ERROR_INVALID_PARAMETER;
	WCHAR *wide = msi_from_ansi(folder), path[MAX_PATH * 4];
	DWORD room = ARRAYSIZE(path);
	UINT status = MsiGetSourcePathW(handle, wide, path, &room);
	msi_free(wide);
	return status ? status : out_text_a(path, room, out, size);
}

UINT WINAPI MsiSetTargetPathW(MSIHANDLE handle, LPCWSTR folder, LPCWSTR path)
{
	if (!folder || !path)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status;
	if (package->remote)
		status = remote_call(package->remote, REMOTE_SET_TARGET, folder, path, 0, NULL, NULL);
	else {
		tables_load(package);
		if (!directory_find(package, folder))
			status = ERROR_DIRECTORY;
		else {
			status = package_set(package, folder, path);
			directories_reset(package);
		}
	}
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiSetTargetPathA(MSIHANDLE handle, LPCSTR folder, LPCSTR path)
{
	WCHAR *wide_folder = folder ? msi_from_ansi(folder) : NULL, *wide_path = path ? msi_from_ansi(path) : NULL;
	UINT status = MsiSetTargetPathW(handle, wide_folder, wide_path);
	msi_free(wide_folder);
	msi_free(wide_path);
	return status;
}

UINT WINAPI MsiGetComponentStateW(MSIHANDLE handle, LPCWSTR key, INSTALLSTATE *installed, INSTALLSTATE *action)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_SUCCESS;
	if (package->remote) {
		INT first = 0, second = 0;
		status = remote_call(package->remote, REMOTE_COMPONENT_STATE, key, NULL, 0, NULL, &first);
		if (!status)
			status = remote_call(package->remote, REMOTE_COMPONENT_STATE, key, NULL, 1, NULL, &second);
		if (installed)
			*installed = first;
		if (action)
			*action = second;
	} else {
		tables_load(package);
		msi_component *component = component_find(package, key);
		if (!component)
			status = ERROR_UNKNOWN_COMPONENT;
		else {
			if (installed)
				*installed = component->installed;
			if (action)
				*action = component->action;
		}
	}
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiGetComponentStateA(MSIHANDLE handle, LPCSTR key, INSTALLSTATE *installed, INSTALLSTATE *action)
{
	WCHAR *wide = key ? msi_from_ansi(key) : NULL;
	UINT status = MsiGetComponentStateW(handle, wide, installed, action);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiGetFeatureStateW(MSIHANDLE handle, LPCWSTR key, INSTALLSTATE *installed, INSTALLSTATE *action)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_SUCCESS;
	if (package->remote) {
		INT first = 0, second = 0;
		status = remote_call(package->remote, REMOTE_FEATURE_STATE, key, NULL, 0, NULL, &first);
		if (!status)
			status = remote_call(package->remote, REMOTE_FEATURE_STATE, key, NULL, 1, NULL, &second);
		if (installed)
			*installed = first;
		if (action)
			*action = second;
	} else {
		tables_load(package);
		msi_feature *feature = feature_find(package, key);
		if (!feature)
			status = ERROR_UNKNOWN_FEATURE;
		else {
			if (installed)
				*installed = feature->installed;
			if (action)
				*action = feature->action;
		}
	}
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiGetFeatureStateA(MSIHANDLE handle, LPCSTR key, INSTALLSTATE *installed, INSTALLSTATE *action)
{
	WCHAR *wide = key ? msi_from_ansi(key) : NULL;
	UINT status = MsiGetFeatureStateW(handle, wide, installed, action);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiSetComponentStateW(MSIHANDLE handle, LPCWSTR key, INSTALLSTATE state)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_SUCCESS;
	if (package->remote)
		status = remote_call(package->remote, REMOTE_SET_COMPONENT, key, NULL, state, NULL, NULL);
	else {
		tables_load(package);
		msi_component *component = component_find(package, key);
		if (component)
			component->action = state;
		else
			status = ERROR_UNKNOWN_COMPONENT;
	}
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiSetComponentStateA(MSIHANDLE handle, LPCSTR key, INSTALLSTATE state)
{
	WCHAR *wide = key ? msi_from_ansi(key) : NULL;
	UINT status = MsiSetComponentStateW(handle, wide, state);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiSetFeatureStateW(MSIHANDLE handle, LPCWSTR key, INSTALLSTATE state)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_SUCCESS;
	if (package->remote)
		status = remote_call(package->remote, REMOTE_SET_FEATURE, key, NULL, state, NULL, NULL);
	else {
		tables_load(package);
		msi_feature *feature = feature_find(package, key);
		if (feature) {
			feature->action = state;
			for (UINT i = 0; i < feature->component_count; i++)
				if (feature->components[i]->enabled)
					feature->components[i]->action = state == INSTALLSTATE_LOCAL ? INSTALLSTATE_LOCAL : feature->components[i]->action;
		} else
			status = ERROR_UNKNOWN_FEATURE;
	}
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiSetFeatureStateA(MSIHANDLE handle, LPCSTR key, INSTALLSTATE state)
{
	WCHAR *wide = key ? msi_from_ansi(key) : NULL;
	UINT status = MsiSetFeatureStateW(handle, wide, state);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiSetInstallLevel(MSIHANDLE handle, int level)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	WCHAR text[16];
	wsprintfW(text, L"%d", level);
	UINT status = package_set(package, L"INSTALLLEVEL", text);
	if (!package->remote && package->costed)
		states_select(package);
	object_unref(&package->base);
	return status;
}
