/* SPDX-License-Identifier: MIT */
#include "msi_own.h"
#include <fcntl.h>
#include <fdi.h>

/* The action sequences. Everything that changes the machine is only scheduled between InstallInitialize and
 * InstallFinalize, then runs from the script there, each change leaving an undo entry so a failure restores what was. */

static REGSAM reg_view;

void reg_set_view(REGSAM view)
{
	reg_view = view;
}

static void undo_push(msi_package *package, const undo_entry *entry)
{
	if (package->undo_count == package->undo_room) {
		package->undo_room = package->undo_room ? package->undo_room * 2 : 64;
		package->undo = msi_grow(package->undo, package->undo_room * sizeof(undo_entry));
	}
	package->undo[package->undo_count++] = *entry;
}

void undo_file(msi_package *package, const WCHAR *path, const WCHAR *backup)
{
	undo_entry entry = { .kind = UNDO_FILE, .path = msi_copy(path), .backup = backup ? msi_copy(backup) : NULL };
	undo_push(package, &entry);
}

void undo_directory(msi_package *package, const WCHAR *path)
{
	undo_entry entry = { .kind = UNDO_DIRECTORY, .path = msi_copy(path) };
	undo_push(package, &entry);
}

// Records what a value holds before it changes; a NULL name stands for the key itself.
void undo_registry(msi_package *package, HKEY root, const WCHAR *key, const WCHAR *name)
{
	undo_entry entry = { .kind = UNDO_REGISTRY, .root = root, .key = msi_copy(key), .name = name ? msi_copy(name) : NULL };
	HKEY handle;
	entry.size = 0;
	if (!RegOpenKeyExW(root, key, 0, KEY_READ | reg_view, &handle)) {
		if (name && !RegQueryValueExW(handle, name, NULL, &entry.type, NULL, &entry.size)) {
			entry.data = msi_alloc(entry.size + 2);
			if (!RegQueryValueExW(handle, name, NULL, &entry.type, entry.data, &entry.size))
				entry.existed = TRUE;
		} else if (!name)
			entry.existed = TRUE;
		RegCloseKey(handle);
	}
	undo_push(package, &entry);
}

static void delete_key_tree(HKEY root, const WCHAR *key)
{
	HKEY handle;
	WCHAR name[256];
	if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WRITE | reg_view, &handle))
		return;
	for (;;) {
		DWORD size = ARRAYSIZE(name);
		if (RegEnumKeyExW(handle, 0, name, &size, NULL, NULL, NULL, NULL))
			break;
		WCHAR *child = msi_alloc((lstrlenW(key) + size + 2) * sizeof(WCHAR));
		wsprintfW(child, L"%s\\%s", key, name);
		delete_key_tree(root, child);
		msi_free(child);
	}
	RegCloseKey(handle);
	RegDeleteKeyExW(root, key, reg_view, 0);
}

void script_entry_free(script_entry *entry)
{
	msi_free(entry->name);
	msi_free(entry->data);
	msi_free(entry->source);
	msi_free(entry->target);
}

static void undo_entry_free(undo_entry *entry)
{
	msi_free(entry->path);
	msi_free(entry->backup);
	msi_free(entry->key);
	msi_free(entry->name);
	msi_free(entry->data);
	script_entry_free(&entry->action);
}

void script_rollback(msi_package *package)
{
	package->rollback = TRUE;
	while (package->undo_count) {
		undo_entry *entry = &package->undo[--package->undo_count];
		switch (entry->kind) {
		case UNDO_ACTION: {
			UINT status = custom_scripted_run(package, &entry->action);
			if (status)
				package_log(L"Rollback action %s failed with %u", entry->action.name, status);
			break;
		}
		case UNDO_FILE:
			if (entry->backup) {
				SetFileAttributesW(entry->path, FILE_ATTRIBUTE_NORMAL);
				MoveFileExW(entry->backup, entry->path, MOVEFILE_REPLACE_EXISTING);
			} else {
				SetFileAttributesW(entry->path, FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(entry->path);
			}
			break;
		case UNDO_DIRECTORY:
			RemoveDirectoryW(entry->path);
			break;
		case UNDO_REGISTRY: {
			HKEY handle;
			if (entry->name && entry->existed) {
				if (!RegCreateKeyExW(entry->root, entry->key, 0, NULL, 0, KEY_WRITE | reg_view, NULL, &handle, NULL)) {
					RegSetValueExW(handle, entry->name, 0, entry->type, entry->data, entry->size);
					RegCloseKey(handle);
				}
			} else if (entry->name) {
				if (!RegOpenKeyExW(entry->root, entry->key, 0, KEY_READ | KEY_WRITE | reg_view, &handle)) {
					RegDeleteValueW(handle, entry->name);
					DWORD values = 0, keys = 0;
					RegQueryInfoKeyW(handle, NULL, NULL, NULL, &keys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
					RegCloseKey(handle);
					if (!values && !keys)
						RegDeleteKeyExW(entry->root, entry->key, reg_view, 0);
				}
			} else if (!entry->existed)
				delete_key_tree(entry->root, entry->key);
			break;
		}
		}
		undo_entry_free(entry);
	}
	package->rollback = FALSE;
	for (UINT i = 0; i < package->commit_count; i++)
		script_entry_free(&package->commits[i]);
	package->commit_count = 0;
}

// A success leaves nothing to restore: only the backups of replaced files need clearing.
static void script_commit(msi_package *package)
{
	for (UINT i = 0; i < package->undo_count; i++) {
		undo_entry *entry = &package->undo[i];
		if (entry->kind == UNDO_FILE && entry->backup) {
			SetFileAttributesW(entry->backup, FILE_ATTRIBUTE_NORMAL);
			DeleteFileW(entry->backup);
		}
		undo_entry_free(entry);
	}
	package->undo_count = 0;
	// The commit actions run once nothing can be rolled back any more; one that fails cannot undo the installation.
	package->commit = TRUE;
	for (UINT i = 0; i < package->commit_count; i++) {
		UINT status = custom_scripted_run(package, &package->commits[i]);
		if (status)
			package_log(L"Commit action %s failed with %u", package->commits[i].name, status);
		script_entry_free(&package->commits[i]);
	}
	package->commit_count = 0;
	package->commit = FALSE;
}

UINT reg_write(msi_package *package, HKEY root, const WCHAR *key, const WCHAR *name, DWORD type, const void *data, DWORD size)
{
	HKEY handle;
	undo_registry(package, root, key, name);
	LSTATUS status = RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE | reg_view, NULL, &handle, NULL);
	if (status)
		return status;
	status = RegSetValueExW(handle, name, 0, type, data, size);
	RegCloseKey(handle);
	return status;
}

UINT reg_write_text(msi_package *package, HKEY root, const WCHAR *key, const WCHAR *name, const WCHAR *text)
{
	return reg_write(package, root, key, name, REG_SZ, text, (lstrlenW(text) + 1) * sizeof(WCHAR));
}

UINT reg_write_number(msi_package *package, HKEY root, const WCHAR *key, const WCHAR *name, DWORD number)
{
	return reg_write(package, root, key, name, REG_DWORD, &number, sizeof number);
}

UINT create_directories(msi_package *package, const WCHAR *path)
{
	WCHAR *copy = msi_copy(path);
	UINT status = ERROR_SUCCESS;
	UINT length = lstrlenW(copy);
	while (length && copy[length - 1] == '\\')
		copy[--length] = 0;
	for (WCHAR *at = copy + 3; ; at++) {
		if (at > copy + length)
			break;
		if (*at == '\\' || !*at) {
			WCHAR kept = *at;
			*at = 0;
			if (CreateDirectoryW(copy, NULL))
				undo_directory(package, copy);
			else if (GetLastError() != ERROR_ALREADY_EXISTS && GetFileAttributesW(copy) == INVALID_FILE_ATTRIBUTES)
				status = GetLastError();
			*at = kept;
			if (!kept)
				break;
		}
	}
	msi_free(copy);
	return status;
}

/* Installing the files. */

typedef struct {
	msi_package *package;
	UINT status;
} extract_context;

static const extra_action *find_action(const extra_action *table, UINT count, const WCHAR *name);

static INT_PTR DIAMONDAPI fdi_open(char *name, int flags, int mode)
{
	(void)mode;
	WCHAR *wide = msi_from_ansi(name);
	DWORD access = (flags & 3) == _O_RDONLY ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
	HANDLE file = CreateFileW(wide, access, FILE_SHARE_READ, NULL, flags & _O_CREAT ? CREATE_ALWAYS : OPEN_EXISTING, 0, NULL);
	msi_free(wide);
	return file == INVALID_HANDLE_VALUE ? -1 : (INT_PTR)file;
}

static UINT DIAMONDAPI fdi_read(INT_PTR file, void *data, UINT size)
{
	DWORD got = 0;
	return ReadFile((HANDLE)file, data, size, &got, NULL) ? got : (UINT)-1;
}

static UINT DIAMONDAPI fdi_write(INT_PTR file, void *data, UINT size)
{
	DWORD put = 0;
	return WriteFile((HANDLE)file, data, size, &put, NULL) ? put : (UINT)-1;
}

static int DIAMONDAPI fdi_close(INT_PTR file)
{
	return CloseHandle((HANDLE)file) ? 0 : -1;
}

static LONG DIAMONDAPI fdi_seek(INT_PTR file, LONG offset, int origin)
{
	return SetFilePointer((HANDLE)file, offset, NULL, origin);
}

static void *DIAMONDAPI fdi_alloc(ULONG size)
{
	return msi_alloc(size);
}

static void DIAMONDAPI fdi_free(void *memory)
{
	msi_free(memory);
}

static UINT version_parts(const WCHAR *text, UINT64 *value)
{
	UINT parts = 0;
	UINT64 total = 0;
	for (const WCHAR *at = text; parts < 4;) {
		UINT number = 0;
		if (*at < '0' || *at > '9')
			break;
		while (*at >= '0' && *at <= '9')
			number = number * 10 + (*at++ - '0');
		total |= (UINT64)number << (48 - 16 * parts++);
		if (*at != '.')
			break;
		at++;
	}
	*value = total;
	return parts;
}

// The default REINSTALLMODE's rule: a versioned file replaces an older or unversioned one, an unversioned one a missing one.
static BOOL should_install(msi_package *package, const msi_file *file, const WCHAR *path)
{
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return TRUE;
	WCHAR *mode = package_get(package, L"REINSTALLMODE");
	BOOL always = FALSE;
	for (const WCHAR *at = mode; at && *at; at++)
		if (*at == 'a' || *at == 'A')
			always = TRUE;
	msi_free(mode);
	if (always)
		return TRUE;
	WCHAR existing[64];
	DWORD room = ARRAYSIZE(existing);
	if (MsiGetFileVersionW(path, existing, &room, NULL, NULL) || !file->version)
		return TRUE;
	UINT64 have, want;
	if (!version_parts(existing, &have) || !version_parts(file->version, &want))
		return TRUE;
	return want > have;
}

static BOOL backup_existing(msi_package *package, const WCHAR *path)
{
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
		undo_file(package, path, NULL);
		return TRUE;
	}
	WCHAR folder[MAX_PATH], backup[MAX_PATH];
	GetTempPathW(MAX_PATH, folder);
	if (!GetTempFileNameW(folder, L"msi", 0, backup))
		return FALSE;
	SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
	if (!MoveFileExW(path, backup, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
		return FALSE;
	undo_file(package, path, backup);
	return TRUE;
}

static BOOL file_wanted(const msi_file *file)
{
	return file->component && file->component->action == INSTALLSTATE_LOCAL;
}

static INT_PTR DIAMONDAPI fdi_notify(FDINOTIFICATIONTYPE kind, PFDINOTIFICATION info)
{
	extract_context *context = info->pv;
	msi_package *package = context->package;
	if (kind == fdintCOPY_FILE) {
		WCHAR *key = msi_from_ansi(info->psz1);
		msi_file *file = file_find(package, key);
		msi_free(key);
		if (!file || !file_wanted(file) || file->extracted)
			return 0;
		file->extracted = package->patched;
		if (file->has_delta) {
			WCHAR folder[MAX_PATH], name[MAX_PATH];
			GetTempPathW(MAX_PATH, folder);
			if (!GetTempFileNameW(folder, L"dlt", 0, name))
				return -1;
			HANDLE out = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (out == INVALID_HANDLE_VALUE)
				return -1;
			file->delta_path = msi_copy(name);
			return (INT_PTR)out;
		}
		WCHAR *path = file_target(package, file);
		if (!path)
			return 0;
		INT_PTR handle = 0;
		if (should_install(package, file, path)) {
			const WCHAR *folder = file->component->assembly_folder ? file->component->assembly_folder : directory_target(package, file->component->directory);
			if (create_directories(package, folder) || !backup_existing(package, path))
				context->status = ERROR_INSTALL_FAILURE;
			else {
				HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
				if (out == INVALID_HANDLE_VALUE) {
					package_log(L"Cannot create %s: %u", path, GetLastError());
					context->status = ERROR_INSTALL_FAILURE;
					handle = -1;
				} else
					handle = (INT_PTR)out;
			}
		}
		msi_free(path);
		return handle;
	}
	if (kind == fdintCLOSE_FILE_INFO) {
		FILETIME local, utc;
		if (DosDateTimeToFileTime(info->date, info->time, &local) && LocalFileTimeToFileTime(&local, &utc))
			SetFileTime((HANDLE)info->hf, &utc, NULL, &utc);
		CloseHandle((HANDLE)info->hf);
		return TRUE;
	}
	return 0;
}

static BOOL extract_cabinet(msi_package *package, const WCHAR *cabinet_path, extract_context *context)
{
	ERF error;
	HFDI fdi = FDICreate(fdi_alloc, fdi_free, fdi_open, fdi_read, fdi_write, fdi_close, fdi_seek, cpuUNKNOWN, &error);
	if (!fdi)
		return FALSE;
	char *narrow = msi_to_ansi(cabinet_path);
	char *name = narrow + lstrlenA(narrow);
	while (name > narrow && name[-1] != '\\')
		name--;
	char kept = *name, folder[MAX_PATH], file_part[MAX_PATH];
	lstrcpynA(file_part, name, MAX_PATH);
	*name = 0;
	lstrcpynA(folder, narrow, MAX_PATH);
	*name = kept;
	BOOL ok = FDICopy(fdi, file_part, folder, 0, fdi_notify, NULL, context);
	FDIDestroy(fdi);
	msi_free(narrow);
	(void)package;
	return ok;
}

static UINT media_cabinet(extract_context *context, const WCHAR *cabinet)
{
	msi_package *package = context->package;
	WCHAR path[MAX_PATH * 2];
	BOOL temporary = FALSE;
	if (cabinet[0] == '#') {
		WCHAR folder[MAX_PATH];
		GetTempPathW(MAX_PATH, folder);
		GetTempFileNameW(folder, L"cab", 0, path);
		HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		const BOOL copied = out != INVALID_HANDLE_VALUE && db_copy_named_stream(package->db, cabinet + 1, out);
		if (out != INVALID_HANDLE_VALUE)
			CloseHandle(out);
		if (!copied) {
			package_log(L"No embedded cabinet %s", cabinet);
			DeleteFileW(path);
			context->status = ERROR_INSTALL_FAILURE;
			return context->status;
		}
		temporary = TRUE;
	} else {
		WCHAR *source = package_get(package, L"SourceDir");
		wsprintfW(path, L"%s%s", source ? source : L"", cabinet);
		msi_free(source);
		// Maintenance runs from the cached package, away from the original media; a patched file comes from the patch.
		if (package->patched && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
			package_log(L"Cabinet %s is not here, its files stay as they are", path);
			return ERROR_SUCCESS;
		}
	}
	if (!extract_cabinet(package, path, context)) {
		package_log(L"Cannot extract %s: %u", path, GetLastError());
		context->status = ERROR_INSTALL_FAILURE;
	}
	if (temporary)
		DeleteFileW(path);
	return context->status;
}

static UINT media_row(msi_record *row, void *raw)
{
	WCHAR scratch[16];
	const WCHAR *cabinet = record_text(row, 1, scratch);
	return cabinet && *cabinet ? media_cabinet(raw, cabinet) : ERROR_SUCCESS;
}

typedef struct media_list {
	WCHAR **names;
	UINT count;
} media_list;

static UINT media_collect(msi_record *row, void *raw)
{
	media_list *list = raw;
	WCHAR scratch[16];
	const WCHAR *cabinet = record_text(row, 1, scratch);
	if (!cabinet || !*cabinet)
		return ERROR_SUCCESS;
	list->names = msi_grow(list->names, (list->count + 1) * sizeof(*list->names));
	list->names[list->count++] = msi_copy(cabinet);
	return ERROR_SUCCESS;
}

// Uncompressed payload sits below the source folder, along the directory tree's source names.
static void source_folder(msi_package *package, const WCHAR *key, WCHAR *out, UINT depth)
{
	msi_directory *entry = directory_find(package, key);
	if (!entry || !entry->parent || depth > 64) {
		WCHAR *source = package_get(package, L"SourceDir");
		lstrcpyW(out, source ? source : L"");
		msi_free(source);
		return;
	}
	source_folder(package, entry->parent, out, depth + 1);
	const WCHAR *name = entry->source_name ? entry->source_name : entry->target_name;
	if (name && lstrcmpW(name, L".")) {
		lstrcatW(out, name);
		lstrcatW(out, L"\\");
	}
}

static BOOL package_compressed(msi_package *package)
{
	MSIHANDLE summary = 0;
	INT words = 0;
	UINT type;
	if (MsiGetSummaryInformationW(0, package->db->path, 0, &summary))
		return FALSE;
	MsiSummaryInfoGetPropertyW(summary, PID_WORDCOUNT, &type, &words, NULL, NULL, NULL);
	MsiCloseHandle(summary);
	return (words & 2) != 0;
}

static UINT act_install_files(msi_package *package)
{
	BOOL compressed = package_compressed(package);
	BOOL any_cabinet = FALSE;
	// A patch's file sits in its cabinet whatever the base package says of it: the cabinets go first and mark what they gave.
	if (package->patched) {
		extract_context first = { package, ERROR_SUCCESS };
		media_list media = { NULL, 0 };
		UINT queried = db_query_each(package->db, L"SELECT `Cabinet` FROM `Media` ORDER BY `DiskId`", NULL, media_collect, &media);
		if (queried)
			first.status = queried;
		// A patch adds its media after the product's, and what a cabinet gave stays: the newest goes first.
		for (UINT i = media.count; i-- > 0;) {
			if (!first.status)
				media_cabinet(&first, media.names[i]);
			msi_free(media.names[i]);
		}
		msi_free(media.names);
		if (first.status)
			return first.status;
	}
	for (UINT i = 0; i < package->file_count; i++) {
		msi_file *file = &package->files[i];
		if (file->extracted)
			continue;
		BOOL in_cabinet = file->attributes & msidbFileAttributesCompressed ? TRUE : file->attributes & msidbFileAttributesNoncompressed ? FALSE : compressed;
		if (!file_wanted(file))
			continue;
		if (in_cabinet) {
			any_cabinet = TRUE;
			continue;
		}
		WCHAR source[MAX_PATH * 2];
		source_folder(package, file->component->directory, source, 0);
		lstrcatW(source, file->name);
		WCHAR *path = file_target(package, file);
		UINT status = ERROR_SUCCESS;
		// Maintenance may have no media: a file already in place then stays.
		BOOL unreachable = package->maintenance && path && GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
		// A package that keeps its own media among the files it installs, as the .NET Framework does in its setup cache, names the
		// installed file as its source once the source list points there: the file is already in place.
		WCHAR full_source[MAX_PATH * 2], full_path[MAX_PATH * 2];
		BOOL in_place = path && GetFullPathNameW(source, ARRAYSIZE(full_source), full_source, NULL) && GetFullPathNameW(path, ARRAYSIZE(full_path), full_path, NULL) &&
			!lstrcmpiW(full_source, full_path);
		if (unreachable)
			package_log(L"Source %s is not here, %s stays as it is", source, path);
		else if (in_place)
			package_log(L"%s is its own source and stays as it is", path);
		else if (path && should_install(package, file, path)) {
			if (create_directories(package, file->component->assembly_folder ? file->component->assembly_folder : directory_target(package, file->component->directory)) || !backup_existing(package, path) ||
				!CopyFileW(source, path, FALSE)) {
				package_log(L"Cannot copy %s to %s: %u", source, path, GetLastError());
				status = ERROR_INSTALL_FAILURE;
			}
		}
		msi_free(path);
		if (status)
			return status;
	}
	if (!any_cabinet || package->patched)
		return ERROR_SUCCESS;
	extract_context context = { package, ERROR_SUCCESS };
	db_query_each(package->db, L"SELECT `Cabinet` FROM `Media` ORDER BY `DiskId`", NULL, media_row, &context);
	return context.status;
}

static UINT act_remove_installed_files(msi_package *package)
{
	for (UINT i = 0; i < package->file_count; i++) {
		msi_file *file = &package->files[i];
		if (!file->component || file->component->action != INSTALLSTATE_ABSENT || (file->component->attributes & msidbComponentAttributesPermanent))
			continue;
		WCHAR *path = file_target(package, file);
		if (path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && backup_existing(package, path))
			package_log(L"Removed %s", path);
		msi_free(path);
	}
	// Folders the removal left empty go too, deepest first.
	for (UINT pass = 0; pass < package->directory_count && pass < 16; pass++)
		for (UINT i = 0; i < package->component_count; i++) {
			msi_component *component = &package->components[i];
			if (component->action != INSTALLSTATE_ABSENT)
				continue;
			const WCHAR *target = directory_target(package, component->directory);
			if (!target)
				continue;
			WCHAR *copy = msi_copy(target);
			UINT length = lstrlenW(copy);
			while (length > 3) {
				copy[--length] = 0;
				if (!RemoveDirectoryW(copy))
					break;
				while (length > 3 && copy[length - 1] != '\\')
					copy[--length] = 0;
			}
			msi_free(copy);
		}
	return ERROR_SUCCESS;
}

static UINT act_remove_files(msi_package *package)
{
	UINT status = act_remove_installed_files(package);
	const extra_action *listed = find_action(extra_actions, (UINT)-1, L"RemoveFiles2");
	return status ? status : listed->call(package);
}

static UINT act_install_initialize(msi_package *package)
{
	package->in_script = TRUE;
	return ERROR_SUCCESS;
}

static UINT act_cost_initialize(msi_package *package)
{
	tables_load(package);
	assemblies_load(package);
	return ERROR_SUCCESS;
}

static UINT patch_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR scratch[16];
	msi_file *file = file_find(package, record_text(row, 1, scratch));
	if (file)
		file->has_delta = TRUE;
	return ERROR_SUCCESS;
}

static UINT act_file_cost(msi_package *package)
{
	if (package->patched)
		db_query_each(package->db, L"SELECT `File_` FROM `Patch`", NULL, patch_row, package);
	return ERROR_SUCCESS;
}

static UINT act_cost_finalize(msi_package *package)
{
	states_select(package);
	package->directories_published = TRUE;
	directories_publish(package);
	costs_publish(package);
	return ERROR_SUCCESS;
}

static UINT act_none(msi_package *package)
{
	(void)package;
	return ERROR_SUCCESS;
}

static UINT launch_row(msi_record *row, void *context)
{
	msi_package *package = context;
	WCHAR a[16], b[16];
	const WCHAR *condition = record_text(row, 1, a), *description = record_text(row, 2, b);
	if (condition_true(package, condition))
		return ERROR_SUCCESS;
	WCHAR *text = format_text(package, description, NULL);
	package_message(package, INSTALLMESSAGE_ERROR, text);
	msi_free(text);
	return ERROR_INSTALL_FAILURE;
}

static UINT act_launch_conditions(msi_package *package)
{
	UINT status = db_query_each(package->db, L"SELECT `Condition`, `Description` FROM `LaunchCondition`", NULL, launch_row, package);
	return status == ERROR_INSTALL_FAILURE ? status : ERROR_SUCCESS;
}

static UINT act_install_execute(msi_package *package)
{
	UINT status = script_run(package);
	package->in_script = TRUE;
	return status;
}

static UINT act_install_finalize(msi_package *package)
{
	UINT status = script_run(package);
	package->in_script = FALSE;
	// Maintenance may skip PublishProduct; a patch still belongs to the product once the script has gone through.
	return status ? status : patches_register(package);
}

static UINT act_install_validate(msi_package *package)
{
	if (!package->costed)
		states_select(package);
	return ERROR_SUCCESS;
}

static const extra_action standard_actions[] = {
	{ L"CostInitialize", act_cost_initialize, FALSE }, { L"FileCost", act_file_cost, FALSE }, { L"CostFinalize", act_cost_finalize, FALSE },
	{ L"InstallValidate", act_install_validate, FALSE }, { L"InstallInitialize", act_install_initialize, FALSE },
	{ L"InstallFinalize", act_install_finalize, FALSE }, { L"InstallExecute", act_install_execute, FALSE },
	{ L"InstallExecuteAgain", act_install_execute, FALSE }, { L"LaunchConditions", act_launch_conditions, FALSE },
	{ L"InstallFiles", act_install_files, TRUE }, { L"PatchFiles", act_none, FALSE }, { L"RemoveFiles", act_remove_files, TRUE },
	{ L"InstallAdminPackage", act_none, FALSE }, { L"ValidateProductID", act_none, FALSE }, { L"ResolveSource", act_none, FALSE },
	{ L"AllocateRegistrySpace", act_none, FALSE }, { L"DisableRollback", act_none, FALSE }, { L"IsolateComponents", act_none, FALSE },
	{ L"CCPSearch", act_none, FALSE }, { L"RMCCPSearch", act_none, FALSE }, { L"SetODBCFolders", act_none, FALSE }, { L"InstallSFPCatalogFile", act_none, FALSE },
	{ L"InstallODBC", act_none, FALSE }, { L"RemoveODBC", act_none, FALSE }, { L"BindImage", act_none, FALSE }, { L"MsiPublishAssemblies", act_none, FALSE },
	{ L"MsiUnpublishAssemblies", act_none, FALSE }, { L"PublishComponents", act_none, FALSE }, { L"UnpublishComponents", act_none, FALSE },
	{ L"MsiConfigureServices", act_none, FALSE }, { L"RegisterComPlus", act_none, FALSE }, { L"UnregisterComPlus", act_none, FALSE },
	{ L"ForceReboot", act_none, FALSE }, { L"ScheduleReboot", act_none, FALSE },
};

static const extra_action *find_action(const extra_action *table, UINT count, const WCHAR *name)
{
	for (UINT i = 0; i < count && table[i].name; i++)
		if (!lstrcmpW(table[i].name, name))
			return &table[i];
	return NULL;
}

// Runs a standard action now, or schedules it; a custom action is told apart by not being one.
UINT action_run(msi_package *package, const WCHAR *name)
{
	UINT status = ERROR_SUCCESS;
	BOOL handled = FALSE;
	WCHAR *previous = package->current_action;
	package->current_action = msi_copy(name);
	package_log(L"Action start: %s", name);
	ui_deliver(INSTALLMESSAGE_ACTIONSTART, name, 0);
	ui_action_started(package, name);
	if (ui_action(package, name, &status)) {
		package_log(L"Action ended: %s, %u", name, status);
		msi_free(package->current_action);
		package->current_action = previous;
		return status;
	}
	const extra_action *action = find_action(more_actions, (UINT)-1, name);
	if (!action)
		action = find_action(standard_actions, ARRAYSIZE(standard_actions), name);
	if (!action)
		action = find_action(extra_actions, (UINT)-1, name);
	if (action) {
		handled = TRUE;
		if (action->scheduled && package->in_script) {
			if (package->script_count == package->script_room) {
				package->script_room = package->script_room ? package->script_room * 2 : 64;
				package->script = msi_grow(package->script, package->script_room * sizeof(script_entry));
			}
			script_entry entry = { .action = action, .name = msi_copy(name) };
			package->script[package->script_count++] = entry;
		} else
			status = action->call(package);
	}
	if (!handled)
		status = custom_action_run(package, name, NULL, &handled);
	if (!handled)
		package_log(L"Skipped, neither a standard nor a custom action: %s", name);
	package_log(L"Action ended: %s, %u", name, status);
	msi_free(package->current_action);
	package->current_action = previous;
	return status;
}

// Runs what was scheduled. A failure restores the machine.
UINT script_run(msi_package *package)
{
	UINT status = ERROR_SUCCESS;
	package->in_script = FALSE;
	package->scheduled = TRUE;
	for (UINT i = 0; i < package->script_count && !status; i++) {
		script_entry *entry = &package->script[i];
		if (!ui_progress(package, i, package->script_count)) {
			status = ERROR_INSTALL_USEREXIT;
			break;
		}
		package_log(L"Script: %s", entry->name);
		if (entry->action)
			status = ((const extra_action *)entry->action)->call(package);
		else if (entry->type & 0x100) {
			// Passing a rollback action arms it; the entry now belongs to the undo list.
			undo_entry undo = { .kind = UNDO_ACTION, .action = *entry };
			undo_push(package, &undo);
			*entry = (script_entry){0};
		} else if (entry->type & 0x200) {
			package->commits = msi_grow(package->commits, (package->commit_count + 1) * sizeof(script_entry));
			package->commits[package->commit_count++] = *entry;
			*entry = (script_entry){0};
		} else
			status = custom_scripted_run(package, entry);
	}
	package->scheduled = FALSE;
	for (UINT i = 0; i < package->script_count; i++)
		script_entry_free(&package->script[i]);
	package->script_count = 0;
	if (status) {
		package_log(L"Rolling back: %u", status);
		script_rollback(package);
		return status == ERROR_INSTALL_USEREXIT ? status : ERROR_INSTALL_FAILURE;
	}
	return ERROR_SUCCESS;
}

typedef struct {
	WCHAR *action, *condition;
	INT sequence;
} sequence_row;

typedef struct {
	sequence_row *rows;
	UINT count;
} sequence_list;

static UINT collect_row(msi_record *row, void *context)
{
	sequence_list *list = context;
	WCHAR a[16], b[16];
	list->rows = msi_grow(list->rows, (list->count + 1) * sizeof(sequence_row));
	const WCHAR *action = record_text(row, 1, a), *condition = record_text(row, 2, b);
	list->rows[list->count].action = msi_copy(action ? action : L"");
	list->rows[list->count].condition = condition ? msi_copy(condition) : NULL;
	list->rows[list->count].sequence = record_get_int(row, 3);
	list->count++;
	return ERROR_SUCCESS;
}

static void run_terminal(msi_package *package, sequence_list *list, INT sequence)
{
	for (UINT i = 0; i < list->count; i++)
		if (list->rows[i].sequence == sequence && condition_true(package, list->rows[i].condition))
			action_run(package, list->rows[i].action);
}

UINT sequence_run(msi_package *package, const WCHAR *table)
{
	WCHAR query[128];
	sequence_list list = {0};
	wsprintfW(query, L"SELECT `Action`, `Condition`, `Sequence` FROM `%s` ORDER BY `Sequence`", table);
	UINT status = db_query_each(package->db, query, NULL, collect_row, &list);
	if (status)
		return status == ERROR_BAD_QUERY_SYNTAX || status == ERROR_INVALID_TABLE ? ERROR_SUCCESS : status;
	status = ERROR_SUCCESS;
	for (UINT i = 0; i < list.count && !status; i++) {
		if (list.rows[i].sequence <= 0 || !condition_true(package, list.rows[i].condition))
			continue;
		status = action_run(package, list.rows[i].action);
		// ERROR_NO_MORE_ITEMS from an action means stop here, successfully; ERROR_FUNCTION_NOT_CALLED, go on.
		if (status == ERROR_FUNCTION_NOT_CALLED)
			status = ERROR_SUCCESS;
		else if (status == ERROR_NO_MORE_ITEMS) {
			status = ERROR_SUCCESS;
			break;
		}
	}
	if (status) {
		// Whatever was scheduled and never ran has nothing to undo; what did run is undone by now.
		package->script_count = 0;
		package->in_script = FALSE;
		script_rollback(package);
		run_terminal(package, &list, status == ERROR_INSTALL_USEREXIT ? -2 : -3);
	} else {
		script_commit(package);
		run_terminal(package, &list, -1);
	}
	for (UINT i = 0; i < list.count; i++) {
		msi_free(list.rows[i].action);
		msi_free(list.rows[i].condition);
	}
	msi_free(list.rows);
	return status;
}

UINT WINAPI MsiDoActionW(MSIHANDLE handle, LPCWSTR action)
{
	if (!action)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status;
	if (package->remote)
		status = remote_call(package->remote, REMOTE_DO_ACTION, action, NULL, 0, NULL, NULL);
	else if (!lstrcmpW(action, L"INSTALL"))
		status = sequence_run(package, L"InstallExecuteSequence");
	else
		status = action_run(package, action);
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiDoActionA(MSIHANDLE handle, LPCSTR action)
{
	WCHAR *wide = action ? msi_from_ansi(action) : NULL;
	UINT status = MsiDoActionW(handle, wide);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiSequenceW(MSIHANDLE handle, LPCWSTR table, INT mode)
{
	if (!table)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status = package->remote ? remote_call(package->remote, REMOTE_SEQUENCE, table, NULL, mode, NULL, NULL) : sequence_run(package, table);
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiSequenceA(MSIHANDLE handle, LPCSTR table, INT mode)
{
	WCHAR *wide = table ? msi_from_ansi(table) : NULL;
	UINT status = MsiSequenceW(handle, wide, mode);
	msi_free(wide);
	return status;
}

// The installation, behind the package's own dialogs when the user interface level asks for them.
UINT install_run(msi_package *package)
{
	UINT prepared = package_prepare(package);
	if (prepared)
		return prepared;
	// Packages test the level in their conditions; the default one is the basic level.
	WCHAR level[8];
	const INSTALLUILEVEL now = ui_internal_level() & 7;
	wsprintfW(level, L"%u", now == INSTALLUILEVEL_DEFAULT || now == INSTALLUILEVEL_NOCHANGE ? INSTALLUILEVEL_BASIC : now);
	package_set(package, L"UILevel", level);
	WCHAR *action = package_get(package, L"ACTION");
	const WCHAR *table = L"InstallExecuteSequence";
	if (action && !lstrcmpW(action, L"ADMIN"))
		table = L"AdminExecuteSequence";
	else if (action && !lstrcmpW(action, L"ADVERTISE"))
		table = L"AdvtExecuteSequence";
	const WCHAR *dialogs = action && !lstrcmpW(action, L"ADMIN") ? L"AdminUISequence" : action && !lstrcmpW(action, L"ADVERTISE") ? NULL
		: L"InstallUISequence";
	msi_free(action);
	if (dialogs && ui_wanted(package, dialogs))
		return ui_run(package, dialogs, table);
	return sequence_run(package, table);
}
