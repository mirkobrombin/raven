/* SPDX-License-Identifier: MIT */
#include "msi_own.h"
#include <wtypes.h>

/* Custom actions, by the source and kind bits of their type. Libraries run in a host process of their own width; programs
 * run as programs. Script actions, VBScript and JScript, run on the Active Scripting engine of their language, which script.c hosts. */

enum {
	KIND_MASK = 0x07, KIND_DLL = 1, KIND_EXE = 2, KIND_TEXT = 3, KIND_JSCRIPT = 5, KIND_VBSCRIPT = 6, KIND_INSTALL = 7,
	SOURCE_MASK = 0x30, SOURCE_BINARY = 0x00, SOURCE_FILE = 0x10, SOURCE_DIRECTORY = 0x20, SOURCE_PROPERTY = 0x30,
	FLAG_CONTINUE = 0x40, FLAG_ASYNC = 0x80, FLAG_ROLLBACK = 0x100, FLAG_COMMIT = 0x200, FLAG_IN_SCRIPT = 0x400,
};

typedef struct {
	msi_package *package;
	HANDLE pipe;
} serve_context;

static LONG pipe_counter;

static WCHAR *extract_binary(msi_package *package, const WCHAR *key, const WCHAR *extension)
{
	WCHAR query[256], folder[MAX_PATH], path[MAX_PATH];
	wsprintfW(query, L"SELECT `Data` FROM `Binary` WHERE `Name` = '%s'", key);
	MSIHANDLE db = 0, view = 0, record = 0;
	object_ref(&package->db->base);
	db = handle_open(&package->db->base);
	WCHAR *result = NULL;
	if (!MsiDatabaseOpenViewW(db, query, &view) && !MsiViewExecute(view, 0) && !MsiViewFetch(view, &record)) {
		GetTempPathW(MAX_PATH, folder);
		// The name is reserved by an empty file; the library goes beside it under its own extension, and the placeholder goes.
		GetTempFileNameW(folder, L"msi", 0, path);
		DeleteFileW(path);
		lstrcatW(path, extension);
		HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (out != INVALID_HANDLE_VALUE) {
			char buffer[65536];
			DWORD size = sizeof buffer, put;
			while (!MsiRecordReadStream(record, 1, buffer, &size) && size) {
				WriteFile(out, buffer, size, &put, NULL);
				size = sizeof buffer;
			}
			CloseHandle(out);
			result = msi_copy(path);
		}
	}
	MsiCloseHandle(record);
	MsiCloseHandle(view);
	MsiCloseHandle(db);
	return result;
}

// A script stored in the Binary table, whole.
static BYTE *binary_data(msi_package *package, const WCHAR *key, SIZE_T *size)
{
	WCHAR query[256];
	wsprintfW(query, L"SELECT `Data` FROM `Binary` WHERE `Name` = '%s'", key);
	MSIHANDLE db = 0, view = 0, record = 0;
	object_ref(&package->db->base);
	db = handle_open(&package->db->base);
	BYTE *data = NULL;
	*size = 0;
	if (!MsiDatabaseOpenViewW(db, query, &view) && !MsiViewExecute(view, 0) && !MsiViewFetch(view, &record)) {
		DWORD length = MsiRecordDataSize(record, 1);
		data = msi_alloc(length + 1);
		if (!MsiRecordReadStream(record, 1, (char *)data, &length))
			*size = length;
	}
	MsiCloseHandle(record);
	MsiCloseHandle(view);
	MsiCloseHandle(db);
	return data;
}

static BYTE *file_data(const WCHAR *path, SIZE_T *size)
{
	*size = 0;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	DWORD length = GetFileSize(file, NULL), got = 0;
	BYTE *data = length < 64 * 1024 * 1024 ? msi_alloc(length + 1) : NULL;
	if (data && ReadFile(file, data, length, &got, NULL))
		*size = got;
	CloseHandle(file);
	return data;
}

// A script action, in VBScript or JScript: the script comes from the Binary table, an installed file, the action's target or a property.
static UINT run_script(msi_package *package, const WCHAR *language, const WCHAR *name, INT from, const WCHAR *source, const WCHAR *target)
{
	BYTE *data = NULL;
	SIZE_T size = 0;
	const WCHAR *entry = target;
	if (from == SOURCE_BINARY)
		data = binary_data(package, source, &size);
	else if (from == SOURCE_FILE) {
		tables_load(package);
		msi_file *file = file_find(package, source);
		WCHAR *path = file ? file_target(package, file) : NULL;
		if (path)
			data = file_data(path, &size);
		msi_free(path);
	} else {
		WCHAR *text = from == SOURCE_PROPERTY ? package_get(package, source) : msi_copy(target);
		if (from != SOURCE_PROPERTY)
			entry = NULL;
		if (text) {
			int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
			data = msi_alloc(length + 3);
			data[0] = 0xef, data[1] = 0xbb, data[2] = 0xbf;
			WideCharToMultiByte(CP_UTF8, 0, text, -1, (char *)data + 3, length, NULL, NULL);
			size = length + 2;
			msi_free(text);
		}
	}
	if (!data) {
		package_log(L"The script of %s is not there", name);
		return ERROR_INSTALL_FAILURE;
	}
	UINT status = script_action_run(package, package->session, language, data, size, entry, name);
	msi_free(data);
	return status;
}

// The library's own width picks the host: a 32-bit library runs in the system's 32-bit msiexec.
static WORD image_machine(const WCHAR *path)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	IMAGE_DOS_HEADER dos;
	IMAGE_NT_HEADERS32 nt;
	DWORD got;
	WORD machine = 0;
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	if (ReadFile(file, &dos, sizeof dos, &got, NULL) && got == sizeof dos && dos.e_magic == IMAGE_DOS_SIGNATURE &&
		SetFilePointer(file, dos.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER && ReadFile(file, &nt, sizeof nt, &got, NULL) && got == sizeof nt)
		machine = nt.FileHeader.Machine;
	CloseHandle(file);
	return machine;
}

static void host_program(WORD machine, WCHAR *path)
{
	GetWindowsDirectoryW(path, MAX_PATH);
	// A 32-bit installer asking for System32 lands in SysWOW64; Sysnative is how it reaches the 64-bit host a 64-bit action needs.
	if (sizeof(void *) == 8)
		lstrcatW(path, machine == IMAGE_FILE_MACHINE_I386 ? L"\\SysWOW64\\msiexec.exe" : L"\\System32\\msiexec.exe");
	else
		lstrcatW(path, machine == IMAGE_FILE_MACHINE_AMD64 ? L"\\Sysnative\\msiexec.exe" : L"\\System32\\msiexec.exe");
}

static UINT run_library(msi_package *package, const WCHAR *library, const WCHAR *entry)
{
	WCHAR pipe_name[96], host[MAX_PATH], line[MAX_PATH * 3];
	wsprintfW(pipe_name, L"\\\\.\\pipe\\raven-msi-%u-%d", GetCurrentProcessId(), InterlockedIncrement(&pipe_counter));
	HANDLE pipe = CreateNamedPipeW(pipe_name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, NULL);
	if (pipe == INVALID_HANDLE_VALUE)
		return ERROR_FUNCTION_FAILED;
	serve_context serve = { package, pipe };
	HANDLE thread = CreateThread(NULL, 0, remote_serve, &serve, 0, NULL);
	host_program(image_machine(library), host);
	wsprintfW(line, L"\"%s\" /raven-action \"%s\" \"%s\" \"%s\"", host, pipe_name, library, entry);
	STARTUPINFOW startup = { sizeof startup };
	PROCESS_INFORMATION process;
	UINT status = ERROR_INSTALL_FAILURE;
	if (CreateProcessW(host, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
		DWORD code = ERROR_INSTALL_FAILURE;
		WaitForSingleObject(process.hProcess, INFINITE);
		GetExitCodeProcess(process.hProcess, &code);
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);
		status = code;
	} else
		package_log(L"Cannot start the custom action host %s: %u", host, GetLastError());
	// A host that never connected leaves the server waiting: a client of our own releases it.
	HANDLE poke = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (poke != INVALID_HANDLE_VALUE)
		CloseHandle(poke);
	if (thread) {
		WaitForSingleObject(thread, 5000);
		CloseHandle(thread);
	}
	CloseHandle(pipe);
	return status;
}

static UINT run_program(const WCHAR *line, const WCHAR *directory, BOOL wait)
{
	STARTUPINFOW startup = { sizeof startup };
	PROCESS_INFORMATION process;
	WCHAR *writable = msi_copy(line);
	UINT status = ERROR_SUCCESS;
	if (!CreateProcessW(NULL, writable, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process))
		status = ERROR_INSTALL_FAILURE;
	else {
		if (wait) {
			DWORD code = 0;
			WaitForSingleObject(process.hProcess, INFINITE);
			GetExitCodeProcess(process.hProcess, &code);
			if (code)
				status = ERROR_INSTALL_FAILURE;
		}
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);
	}
	msi_free(writable);
	return status;
}

// A package kept as a storage inside this one, written out so it can be installed like any other.
static WCHAR *nested_package(msi_package *package, const WCHAR *name)
{
	WCHAR encoded[64], folder[MAX_PATH], path[MAX_PATH];
	IStorage *from = NULL, *to = NULL;
	name_encode(name, FALSE, encoded);
	if (!package->db->storage)
		return NULL;
	HRESULT hr = IStorage_OpenStorage(package->db->storage, encoded, NULL, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, NULL, 0, &from);
	if (FAILED(hr))
		hr = IStorage_OpenStorage(package->db->storage, name, NULL, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, NULL, 0, &from);
	if (FAILED(hr))
		return NULL;
	GetTempPathW(MAX_PATH, folder);
	GetTempFileNameW(folder, L"msi", 0, path);
	if (SUCCEEDED(hr = StgCreateDocfile(path, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &to))) {
		hr = IStorage_CopyTo(from, 0, NULL, NULL, to);
		IStorage_Commit(to, 0);
		IStorage_Release(to);
	}
	IStorage_Release(from);
	if (FAILED(hr)) {
		DeleteFileW(path);
		return NULL;
	}
	return msi_copy(path);
}

static UINT execute(msi_package *package, const WCHAR *name, INT type, const WCHAR *source, const WCHAR *target)
{
	INT kind = type & KIND_MASK, from = type & SOURCE_MASK;
	WCHAR *formatted = format_text(package, target, NULL);
	UINT status = ERROR_SUCCESS;
	switch (kind) {
	case KIND_DLL:
	case KIND_EXE: {
		WCHAR *image = NULL;
		BOOL temporary = FALSE;
		if (kind == KIND_DLL && from == SOURCE_BINARY) {
			image = extract_binary(package, source, L".dll");
			temporary = TRUE;
		} else if (from == SOURCE_BINARY) {
			image = extract_binary(package, source, L".exe");
			temporary = TRUE;
		} else if (from == SOURCE_FILE) {
			tables_load(package);
			msi_file *file = file_find(package, source);
			image = file ? file_target(package, file) : NULL;
		} else if (from == SOURCE_PROPERTY)
			image = package_get(package, source);
		if (kind == KIND_DLL)
			status = image ? run_library(package, image, formatted) : ERROR_INSTALL_FAILURE;
		else if (from == SOURCE_DIRECTORY) {
			tables_load(package);
			status = run_program(formatted, directory_target(package, source), !(type & FLAG_ASYNC));
		} else if (image) {
			SIZE_T room = lstrlenW(image) + lstrlenW(formatted) + 4;
			WCHAR *line = msi_alloc(room * sizeof(WCHAR));
			wsprintfW(line, L"\"%s\" %s", image, formatted);
			status = run_program(line, NULL, !(type & FLAG_ASYNC));
			msi_free(line);
		} else
			status = ERROR_INSTALL_FAILURE;
		if (temporary && image)
			DeleteFileW(image);
		msi_free(image);
		break;
	}
	case KIND_TEXT:
		if (from == SOURCE_PROPERTY)
			package_set(package, source, formatted);
		else if (from == SOURCE_DIRECTORY) {
			tables_load(package);
			package_set(package, source, formatted);
			directories_reset(package);
		} else if (from == SOURCE_FILE) {
			package_message(package, INSTALLMESSAGE_ERROR, formatted);
			status = ERROR_INSTALL_FAILURE;
		} else
			package_log(L"Unknown text action %s", name);
		break;
	case KIND_INSTALL: {
		// By its documented types: 7 a storage of this package, 23 a path below the source, 39 an advertised product's code.
		if (from == SOURCE_DIRECTORY) {
			status = MsiConfigureProductExW(source, INSTALLLEVEL_DEFAULT, INSTALLSTATE_DEFAULT, formatted);
			break;
		}
		WCHAR *path = NULL;
		BOOL temporary = FALSE;
		if (from == SOURCE_BINARY) {
			path = nested_package(package, source);
			temporary = path != NULL;
		} else if (from == SOURCE_FILE) {
			WCHAR *root = package_get(package, L"SourceDir");
			if (root) {
				path = msi_alloc((lstrlenW(root) + lstrlenW(source) + 1) * sizeof(WCHAR));
				lstrcpyW(path, root);
				lstrcatW(path, source);
				msi_free(root);
			}
		}
		if (path) {
			status = MsiInstallProductW(path, formatted);
			if (temporary)
				DeleteFileW(path);
			msi_free(path);
		} else {
			package_log(L"Nested installation from this source is not available: %s", name);
			status = ERROR_INSTALL_FAILURE;
		}
		break;
	}
	case KIND_VBSCRIPT:
	case KIND_JSCRIPT:
		status = run_script(package, kind == KIND_VBSCRIPT ? L"VBScript" : L"JScript", name, from, source, target);
		break;
	default:
		package_log(L"Script custom action is not available here: %s", name);
		status = ERROR_INSTALL_FAILURE;
		break;
	}
	msi_free(formatted);
	if (status == ERROR_NO_MORE_ITEMS || status == ERROR_FUNCTION_NOT_CALLED || status == ERROR_INSTALL_USEREXIT)
		return status;
	if (status && type & FLAG_CONTINUE)
		package_log(L"Custom action %s failed with %u; the package asks to go on", name, status);
	return type & FLAG_CONTINUE ? ERROR_SUCCESS : status;
}

UINT custom_scripted_run(msi_package *package, const script_entry *entry)
{
	if (entry->data)
		package_set(package, L"CustomActionData", entry->data);
	UINT status = execute(package, entry->name, entry->type, entry->source, entry->target);
	package_set(package, L"CustomActionData", NULL);
	return status;
}

typedef struct {
	INT type;
	WCHAR *source, *target;
	BOOL found;
} action_row;

static UINT custom_row(msi_record *row, void *context)
{
	action_row *found = context;
	WCHAR a[16], b[16];
	const WCHAR *source = record_text(row, 2, a), *target = record_text(row, 3, b);
	found->type = record_get_int(row, 1);
	found->source = msi_copy(source ? source : L"");
	found->target = msi_copy(target ? target : L"");
	found->found = TRUE;
	return ERROR_SUCCESS;
}

// A deferred action runs later, with the value of the property named after it as its CustomActionData.
UINT custom_action_run(msi_package *package, const WCHAR *name, const WCHAR *data, BOOL *handled)
{
	WCHAR query[256];
	action_row found = {0};
	wsprintfW(query, L"SELECT `Type`, `Source`, `Target` FROM `CustomAction` WHERE `Action` = '%s'", name);
	db_query_each(package->db, query, NULL, custom_row, &found);
	if (!found.found)
		return ERROR_SUCCESS;
	*handled = TRUE;
	UINT status = ERROR_SUCCESS;
	if (found.type & FLAG_IN_SCRIPT) {
		// A rollback or commit action only means something in the script, beside the changes it undoes or makes final.
		WCHAR *disabled = found.type & FLAG_ROLLBACK ? package_get(package, L"RollbackDisabled") : NULL;
		if ((found.type & (FLAG_ROLLBACK | FLAG_COMMIT)) && (!package->in_script || (disabled && *disabled)))
			package_log(L"Not scheduled, no script to undo or commit: %s", name);
		else if (package->in_script) {
			if (package->script_count == package->script_room) {
				package->script_room = package->script_room ? package->script_room * 2 : 64;
				package->script = msi_grow(package->script, package->script_room * sizeof(script_entry));
			}
			script_entry entry = { .name = msi_copy(name), .type = found.type, .source = msi_copy(found.source), .target = msi_copy(found.target) };
			entry.data = data ? msi_copy(data) : package_get(package, name);
			package->script[package->script_count++] = entry;
		} else
			status = execute(package, name, found.type, found.source, found.target);
		msi_free(disabled);
	} else
		status = execute(package, name, found.type, found.source, found.target);
	msi_free(found.source);
	msi_free(found.target);
	return status;
}

// The host's side, called by msiexec: connect back, load the library, run the entry with a proxy session.
UINT WINAPI RavenActionHost(const WCHAR *pipe_name, const WCHAR *library, const WCHAR *entry)
{
	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 200 && pipe == INVALID_HANDLE_VALUE; attempt++) {
		pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (pipe == INVALID_HANDLE_VALUE)
			Sleep(50);
	}
	if (pipe == INVALID_HANDLE_VALUE)
		return ERROR_INSTALL_FAILURE;
	HMODULE module = LoadLibraryW(library);
	char *narrow = msi_to_ansi(entry);
	UINT (WINAPI *call)(MSIHANDLE) = module ? (void *)GetProcAddress(module, narrow) : NULL;
	msi_free(narrow);
	if (!call) {
		CloseHandle(pipe);
		return ERROR_INSTALL_FAILURE;
	}
	msi_package *package = msi_alloc(sizeof *package);
	ZeroMemory(package, sizeof *package);
	object_init(&package->base, KIND_PACKAGE, NULL);
	package->remote = pipe;
	MSIHANDLE session = handle_open(&package->base);
	UINT status = call(session);
	CloseHandle(pipe);
	return status;
}
