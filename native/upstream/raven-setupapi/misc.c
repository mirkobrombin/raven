/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "setup.h"
#include <objbase.h>
#include <string.h>

/* Memory and small helpers callers reach through documented or long established exports. */

void *WINAPI MyMalloc(DWORD size)
{
	return setup_alloc(size);
}

void *WINAPI MyRealloc(void *memory, DWORD size)
{
	return setup_realloc(memory, size);
}

void WINAPI MyFree(void *memory)
{
	setup_free(memory);
}

void *WINAPI pSetupMalloc(DWORD size)
{
	return setup_alloc(size);
}

void *WINAPI pSetupRealloc(void *memory, DWORD size)
{
	return setup_realloc(memory, size);
}

void WINAPI pSetupFree(void *memory)
{
	setup_free(memory);
}

WCHAR *WINAPI pSetupDuplicateString(const WCHAR *text)
{
	return setup_copy_wide(text);
}

WCHAR *WINAPI pSetupMultiByteToUnicode(const char *text, UINT code_page)
{
	int length;
	WCHAR *wide;

	if (!text)
		return NULL;
	length = MultiByteToWideChar(code_page, 0, text, -1, NULL, 0);
	wide = setup_alloc((SIZE_T)length * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(code_page, 0, text, -1, wide, length);
	return wide;
}

char *WINAPI pSetupUnicodeToMultiByte(const WCHAR *text, UINT code_page)
{
	int length;
	char *ansi;

	if (!text)
		return NULL;
	length = WideCharToMultiByte(code_page, 0, text, -1, NULL, 0, NULL, NULL);
	ansi = setup_alloc(length);
	if (ansi)
		WideCharToMultiByte(code_page, 0, text, -1, ansi, length, NULL, NULL);
	return ansi;
}

char *WINAPI UnicodeToMultiByte(const WCHAR *text, UINT code_page)
{
	return pSetupUnicodeToMultiByte(text, code_page);
}

DWORD WINAPI pSetupCaptureAndConvertAnsiArg(const char *text, WCHAR **wide)
{
	if (!wide)
		return ERROR_INVALID_PARAMETER;
	if (!text) {
		*wide = NULL;
		return NO_ERROR;
	}
	*wide = setup_to_wide(text);
	return *wide ? NO_ERROR : ERROR_NOT_ENOUGH_MEMORY;
}

BOOL WINAPI pSetupConcatenatePaths(WCHAR *target, const WCHAR *addition, DWORD size, DWORD *required)
{
	WCHAR *joined = setup_join_path(target, addition);
	BOOL result;

	if (!joined)
		return FALSE;
	result = setup_give_wide(joined, target, size, required);
	setup_free(joined);
	return result;
}

const WCHAR *WINAPI pSetupGetFileTitle(const WCHAR *path)
{
	const WCHAR *at, *title = path;

	for (at = path; *at; at++) {
		if (*at == '\\' || *at == '/' || *at == ':')
			title = at + 1;
	}
	return title;
}

BOOL WINAPI pSetupIsGuidNull(const GUID *guid)
{
	static const GUID null_guid;

	return !memcmp(guid, &null_guid, sizeof(*guid));
}

DWORD WINAPI pSetupStringFromGuid(const GUID *guid, WCHAR *buffer, DWORD size)
{
	if (size < 39)
		return ERROR_INSUFFICIENT_BUFFER;
	StringFromGUID2(guid, buffer, size);
	return NO_ERROR;
}

DWORD WINAPI pSetupGuidFromString(const WCHAR *text, GUID *guid)
{
	return SUCCEEDED(CLSIDFromString((LPOLESTR)text, guid)) ? NO_ERROR : RPC_S_INVALID_STRING_UUID;
}

/* This runtime runs programs as the machine's administrator, so a privilege check always passes. */
BOOL WINAPI IsUserAdmin(void)
{
	return TRUE;
}

BOOL WINAPI pSetupIsUserAdmin(void)
{
	return TRUE;
}

BOOL WINAPI DoesUserHavePrivilege(const WCHAR *privilege)
{
	return TRUE;
}

BOOL WINAPI pSetupDoesUserHavePrivilege(const WCHAR *privilege)
{
	return TRUE;
}

BOOL WINAPI pSetupEnablePrivilege(const WCHAR *privilege, BOOL enable)
{
	return TRUE;
}

static DWORD global_flags;

DWORD WINAPI pSetupGetGlobalFlags(void)
{
	return global_flags;
}

void WINAPI pSetupSetGlobalFlags(DWORD flags)
{
	global_flags = flags;
}

void WINAPI pSetupModifyGlobalFlags(DWORD mask, DWORD flags)
{
	global_flags = (global_flags & ~mask) | (flags & mask);
}

DWORD WINAPI pSetupGetQueueFlags(HSPFILEQ queue)
{
	DWORD flags = 0;

	SetupGetFileQueueFlags(queue, &flags);
	return flags;
}

void WINAPI pSetupSetQueueFlags(HSPFILEQ queue, DWORD flags)
{
	SetupSetFileQueueFlags(queue, 0xffffffff, flags);
}

BOOL WINAPI SetupGetNonInteractiveMode(void);
static BOOL non_interactive;

BOOL WINAPI SetupGetNonInteractiveMode(void)
{
	return non_interactive;
}

BOOL WINAPI SetupSetNonInteractiveMode(BOOL value)
{
	BOOL previous = non_interactive;

	non_interactive = value;
	return previous;
}

BOOL WINAPI pSetupMakeSurePathExists(const WCHAR *path)
{
	WCHAR *directory = setup_copy_wide(path), *slash;
	BOOL result = TRUE;

	if (!directory)
		return FALSE;
	slash = wcsrchr(directory, '\\');
	if (slash) {
		*slash = 0;
		result = setup_make_directories(directory);
	}
	setup_free(directory);
	return result;
}

/* Disk prompts: there is nobody to ask here, so each answers for itself. */

UINT WINAPI SetupPromptForDiskW(HWND owner, PCWSTR title, PCWSTR disk, PCWSTR path, PCWSTR file, PCWSTR tag, DWORD style, PWSTR buffer, DWORD size, PDWORD required)
{
	WCHAR *full = setup_join_path(path, file ? file : tag);
	BOOL present = full && GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES;

	setup_free(full);
	if (!present)
		return DPROMPT_CANCEL;
	if (!path)
		return DPROMPT_CANCEL;
	if (!setup_give_wide(path, buffer, size, required))
		return DPROMPT_BUFFERTOOSMALL;
	return DPROMPT_SUCCESS;
}

UINT WINAPI SetupPromptForDiskA(HWND owner, PCSTR title, PCSTR disk, PCSTR path, PCSTR file, PCSTR tag, DWORD style, PSTR buffer, DWORD size, PDWORD required)
{
	WCHAR *a = setup_to_wide(path), *b = setup_to_wide(file), *c = setup_to_wide(tag);
	WCHAR text[MAX_PATH];
	UINT result = SetupPromptForDiskW(owner, NULL, NULL, a, b, c, style, text, MAX_PATH, NULL);

	setup_free(a);
	setup_free(b);
	setup_free(c);
	if (result == DPROMPT_SUCCESS && !setup_wide_to_ansi_result(TRUE, text, buffer, size, required))
		return DPROMPT_BUFFERTOOSMALL;
	return result;
}

#define ERROR_ANSWER(name, parameters, answer) UINT WINAPI name parameters { return answer; }

ERROR_ANSWER(SetupCopyErrorW, (HWND o, PCWSTR t, PCWSTR d, PCWSTR p, PCWSTR f, PCWSTR g, UINT w, DWORD s, PWSTR b, DWORD z, PDWORD r), DPROMPT_SKIPFILE)
ERROR_ANSWER(SetupCopyErrorA, (HWND o, PCSTR t, PCSTR d, PCSTR p, PCSTR f, PCSTR g, UINT w, DWORD s, PSTR b, DWORD z, PDWORD r), DPROMPT_SKIPFILE)
ERROR_ANSWER(SetupDeleteErrorW, (HWND o, PCWSTR t, PCWSTR f, UINT w, DWORD s), DPROMPT_SKIPFILE)
ERROR_ANSWER(SetupDeleteErrorA, (HWND o, PCSTR t, PCSTR f, UINT w, DWORD s), DPROMPT_SKIPFILE)
ERROR_ANSWER(SetupRenameErrorW, (HWND o, PCWSTR t, PCWSTR a, PCWSTR b, UINT w, DWORD s), DPROMPT_SKIPFILE)
ERROR_ANSWER(SetupRenameErrorA, (HWND o, PCSTR t, PCSTR a, PCSTR b, UINT w, DWORD s), DPROMPT_SKIPFILE)
ERROR_ANSWER(SetupBackupErrorW, (HWND o, PCWSTR t, PCWSTR a, PCWSTR b, UINT w, DWORD s), DPROMPT_SKIPFILE)
ERROR_ANSWER(SetupBackupErrorA, (HWND o, PCSTR t, PCSTR a, PCSTR b, UINT w, DWORD s), DPROMPT_SKIPFILE)

INT WINAPI SetupPromptReboot(HSPFILEQ queue, HWND owner, BOOL scan_only)
{
	return 0;
}

/* The source list: a process wide list, which the documentation allows when no system list is asked for. */

static WCHAR **source_list;
static UINT source_count;

BOOL WINAPI SetupAddToSourceListW(DWORD flags, PCWSTR source)
{
	WCHAR **grown = setup_realloc(source_list, (source_count + 1) * sizeof(WCHAR *));

	if (!grown)
		return FALSE;
	source_list = grown;
	source_list[source_count++] = setup_copy_wide(source);
	return TRUE;
}

BOOL WINAPI SetupAddToSourceListA(DWORD flags, PCSTR source)
{
	WCHAR *wide = setup_to_wide(source);
	BOOL result = SetupAddToSourceListW(flags, wide);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupRemoveFromSourceListW(DWORD flags, PCWSTR source)
{
	UINT at;

	for (at = 0; at < source_count; at++) {
		if (!lstrcmpiW(source_list[at], source)) {
			setup_free(source_list[at]);
			source_list[at] = source_list[--source_count];
			return TRUE;
		}
	}
	return FALSE;
}

BOOL WINAPI SetupRemoveFromSourceListA(DWORD flags, PCSTR source)
{
	WCHAR *wide = setup_to_wide(source);
	BOOL result = SetupRemoveFromSourceListW(flags, wide);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupSetSourceListW(DWORD flags, PCWSTR *sources, UINT count)
{
	UINT at;

	for (at = 0; at < count; at++) {
		if (!SetupAddToSourceListW(flags, sources[at]))
			return FALSE;
	}
	return TRUE;
}

BOOL WINAPI SetupSetSourceListA(DWORD flags, PCSTR *sources, UINT count)
{
	UINT at;

	for (at = 0; at < count; at++) {
		if (!SetupAddToSourceListA(flags, sources[at]))
			return FALSE;
	}
	return TRUE;
}

BOOL WINAPI SetupCancelTemporarySourceList(void)
{
	return TRUE;
}

BOOL WINAPI SetupQuerySourceListW(DWORD flags, PCWSTR **list, PUINT count)
{
	PCWSTR *copy = setup_alloc((source_count + 1) * sizeof(WCHAR *));
	UINT at;

	if (!copy)
		return FALSE;
	for (at = 0; at < source_count; at++)
		copy[at] = setup_copy_wide(source_list[at]);
	*list = copy;
	*count = source_count;
	return TRUE;
}

BOOL WINAPI SetupQuerySourceListA(DWORD flags, PCSTR **list, PUINT count)
{
	PCSTR *copy = setup_alloc((source_count + 1) * sizeof(char *));
	UINT at;

	if (!copy)
		return FALSE;
	for (at = 0; at < source_count; at++)
		copy[at] = setup_to_ansi(source_list[at]);
	*list = copy;
	*count = source_count;
	return TRUE;
}

BOOL WINAPI SetupFreeSourceListW(PCWSTR **list, UINT count)
{
	UINT at;

	for (at = 0; at < count; at++)
		setup_free((void *)(*list)[at]);
	setup_free((void *)*list);
	*list = NULL;
	return TRUE;
}

BOOL WINAPI SetupFreeSourceListA(PCSTR **list, UINT count)
{
	UINT at;

	for (at = 0; at < count; at++)
		setup_free((void *)(*list)[at]);
	setup_free((void *)*list);
	*list = NULL;
	return TRUE;
}

/* The action log. */

static HANDLE log_file = INVALID_HANDLE_VALUE;

BOOL WINAPI SetupOpenLog(BOOL erase)
{
	WCHAR path[MAX_PATH];

	if (log_file != INVALID_HANDLE_VALUE)
		return TRUE;
	GetWindowsDirectoryW(path, MAX_PATH);
	lstrcatW(path, L"\\setupact.log");
	log_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, erase ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	return log_file != INVALID_HANDLE_VALUE;
}

void WINAPI SetupCloseLog(void)
{
	if (log_file != INVALID_HANDLE_VALUE)
		CloseHandle(log_file);
	log_file = INVALID_HANDLE_VALUE;
}

BOOL WINAPI SetupLogErrorA(LPCSTR message, LogSeverity severity)
{
	DWORD put;

	if (log_file == INVALID_HANDLE_VALUE) {
		SetLastError(ERROR_FILE_INVALID);
		return FALSE;
	}
	if (severity >= LogSevMaximum)
		return FALSE;
	return !message || WriteFile(log_file, message, lstrlenA(message), &put, NULL);
}

BOOL WINAPI SetupLogErrorW(LPCWSTR message, LogSeverity severity)
{
	char *ansi = setup_to_ansi(message);
	BOOL result = SetupLogErrorA(ansi, severity);

	setup_free(ansi);
	return result;
}

/* Disk space lists. One entry per path, sized in whole clusters: what the last operation on that
 * path adds to its drive, measured against the file on disk when the list was told about it. */

#define SPACE_CLUSTER 4096

struct space_entry {
	struct space_entry *next;
	WCHAR *path;
	LONGLONG change;
	UINT operation;
	BOOL adjustment;
};

struct space_list {
	struct space_entry *entries;
	UINT flags;
};

#define SPACE_FLAGS_KNOWN (SPDSL_IGNORE_DISK | SPDSL_DISALLOW_NEGATIVE_ADJUST)

static LONGLONG whole_clusters(LONGLONG size)
{
	return (size + SPACE_CLUSTER - 1) / SPACE_CLUSTER * SPACE_CLUSTER;
}

HDSKSPC WINAPI SetupCreateDiskSpaceListW(PVOID reserved1, DWORD reserved2, UINT flags)
{
	struct space_list *list;

	if (reserved1 || reserved2 || (flags & ~SPACE_FLAGS_KNOWN)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	list = setup_alloc(sizeof(*list));
	if (list)
		list->flags = flags;
	return list;
}

HDSKSPC WINAPI SetupCreateDiskSpaceListA(PVOID reserved1, DWORD reserved2, UINT flags)
{
	return SetupCreateDiskSpaceListW(reserved1, reserved2, flags);
}

BOOL WINAPI SetupDestroyDiskSpaceList(HDSKSPC handle)
{
	struct space_list *list = handle;

	if (!list) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	while (list->entries) {
		struct space_entry *next = list->entries->next;
		setup_free(list->entries->path);
		setup_free(list->entries);
		list->entries = next;
	}
	setup_free(list);
	return TRUE;
}

HDSKSPC WINAPI SetupDuplicateDiskSpaceListW(HDSKSPC handle, PVOID reserved1, DWORD reserved2, UINT flags)
{
	struct space_list *list = handle, *copy;
	struct space_entry *entry, **tail;

	if (reserved1 || reserved2 || flags) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	if (!list) {
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	copy = setup_alloc(sizeof(*copy));
	if (!copy)
		return NULL;
	copy->flags = list->flags;
	tail = &copy->entries;
	for (entry = list->entries; entry; entry = entry->next) {
		struct space_entry *item = setup_alloc(sizeof(*item));
		if (!item)
			break;
		*item = *entry;
		item->path = setup_copy_wide(entry->path);
		item->next = NULL;
		*tail = item;
		tail = &item->next;
	}
	return copy;
}

HDSKSPC WINAPI SetupDuplicateDiskSpaceListA(HDSKSPC handle, PVOID reserved1, DWORD reserved2, UINT flags)
{
	return SetupDuplicateDiskSpaceListW(handle, reserved1, reserved2, flags);
}

static BOOL valid_space_path(const WCHAR *path, UINT operation, DWORD *error)
{
	SIZE_T length = lstrlenW(path);

	if (operation != FILEOP_COPY && operation != FILEOP_DELETE) {
		*error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}
	if (!length) {
		*error = ERROR_INVALID_NAME;
		return FALSE;
	}
	if ((path[0] == '\\' && path[1] == '\\') || path[length - 1] == '\\' || path[length - 1] == '/') {
		*error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}
	return TRUE;
}

static BOOL same_drive(const WCHAR *path, const WCHAR *drive)
{
	return (path[0] | 0x20) == (drive[0] | 0x20) && path[1] == ':';
}

static LONGLONG size_on_disk(const WCHAR *path)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	ULARGE_INTEGER size;

	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data))
		return 0;
	size.LowPart = data.nFileSizeLow;
	size.HighPart = data.nFileSizeHigh;
	return whole_clusters((LONGLONG)size.QuadPart);
}

BOOL WINAPI SetupAddToDiskSpaceListW(HDSKSPC handle, PCWSTR target, LONGLONG size, UINT operation, PVOID reserved1, UINT reserved2)
{
	struct space_list *list = handle;
	WCHAR full[MAX_PATH * 2];
	struct space_entry *entry, **tail;
	LONGLONG existing;
	DWORD error;

	if (!list) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!target || reserved1 || reserved2) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!valid_space_path(target, operation, &error)) {
		SetLastError(error);
		return FALSE;
	}
	if (!GetFullPathNameW(target, ARRAYSIZE(full), full, NULL))
		return FALSE;
	tail = &list->entries;
	for (entry = list->entries; entry; tail = &entry->next, entry = entry->next) {
		if (!entry->adjustment && !lstrcmpiW(entry->path, full))
			break;
	}
	/* Deleting what the list already copies changes nothing. */
	if (entry && entry->operation == FILEOP_COPY && operation == FILEOP_DELETE) {
		SetLastError(ERROR_SUCCESS);
		return TRUE;
	}
	if (!entry) {
		entry = setup_alloc(sizeof(*entry));
		if (!entry)
			return FALSE;
		entry->path = setup_copy_wide(full);
		*tail = entry;
	}
	entry->operation = operation;
	existing = list->flags & SPDSL_IGNORE_DISK ? 0 : size_on_disk(full);
	entry->change = operation == FILEOP_COPY ? whole_clusters(size) - existing : -existing;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI SetupAddToDiskSpaceListA(HDSKSPC handle, PCSTR target, LONGLONG size, UINT operation, PVOID reserved1, UINT reserved2)
{
	WCHAR *wide = setup_to_wide(target);
	BOOL result = SetupAddToDiskSpaceListW(handle, wide, size, operation, reserved1, reserved2);
	DWORD error = GetLastError();

	setup_free(wide);
	SetLastError(error);
	return result;
}

BOOL WINAPI SetupRemoveFromDiskSpaceListW(HDSKSPC handle, PCWSTR target, UINT operation, PVOID reserved1, UINT reserved2)
{
	struct space_list *list = handle;
	WCHAR full[MAX_PATH * 2];
	struct space_entry **at;

	if (!list) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!target || (operation != FILEOP_COPY && operation != FILEOP_DELETE) || reserved1 || reserved2) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	GetFullPathNameW(target, ARRAYSIZE(full), full, NULL);
	for (at = &list->entries; *at; at = &(*at)->next) {
		struct space_entry *entry = *at;
		if (!entry->adjustment && entry->operation == operation && !lstrcmpiW(entry->path, full)) {
			*at = entry->next;
			setup_free(entry->path);
			setup_free(entry);
			return TRUE;
		}
	}
	SetLastError(ERROR_INVALID_DRIVE);
	return FALSE;
}

BOOL WINAPI SetupRemoveFromDiskSpaceListA(HDSKSPC handle, PCSTR target, UINT operation, PVOID reserved1, UINT reserved2)
{
	WCHAR *wide = setup_to_wide(target);
	BOOL result = SetupRemoveFromDiskSpaceListW(handle, wide, operation, reserved1, reserved2);

	setup_free(wide);
	return result;
}

static BOOL is_drive_name(const WCHAR *drive)
{
	return ((drive[0] >= 'a' && drive[0] <= 'z') || (drive[0] >= 'A' && drive[0] <= 'Z')) && drive[1] == ':' && !drive[2];
}

BOOL WINAPI SetupAdjustDiskSpaceListW(HDSKSPC handle, LPCWSTR root, LONGLONG amount, PVOID reserved1, UINT reserved2)
{
	struct space_list *list = handle;
	struct space_entry *entry, **tail;

	if (!list) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!root || reserved1 || reserved2) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!is_drive_name(root)) {
		SetLastError(ERROR_INVALID_DRIVE);
		return FALSE;
	}
	entry = setup_alloc(sizeof(*entry));
	if (!entry)
		return FALSE;
	entry->path = setup_copy_wide(root);
	entry->change = amount;
	entry->adjustment = TRUE;
	for (tail = &list->entries; *tail; tail = &(*tail)->next)
		;
	*tail = entry;
	return TRUE;
}

BOOL WINAPI SetupAdjustDiskSpaceListA(HDSKSPC handle, LPCSTR root, LONGLONG amount, PVOID reserved1, UINT reserved2)
{
	WCHAR *wide = setup_to_wide(root);
	BOOL result = SetupAdjustDiskSpaceListW(handle, wide, amount, reserved1, reserved2);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupQuerySpaceRequiredOnDriveW(HDSKSPC handle, LPCWSTR drive, LONGLONG *space, PVOID reserved1, UINT reserved2)
{
	struct space_list *list = handle;
	struct space_entry *entry;
	LONGLONG total = 0;
	BOOL found = FALSE;

	if (!list) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!drive) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!is_drive_name(drive)) {
		SetLastError(ERROR_INVALID_DRIVE);
		return FALSE;
	}
	if (!space || reserved1 || reserved2) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	for (entry = list->entries; entry; entry = entry->next) {
		if (same_drive(entry->path, drive)) {
			found = TRUE;
			total += entry->change;
		}
	}
	if (!found) {
		SetLastError(ERROR_INVALID_DRIVE);
		return FALSE;
	}
	*space = total;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI SetupQuerySpaceRequiredOnDriveA(HDSKSPC handle, LPCSTR drive, LONGLONG *space, PVOID reserved1, UINT reserved2)
{
	WCHAR *wide;
	BOOL result;
	DWORD error;

	if (!drive) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	wide = setup_to_wide(drive);
	result = SetupQuerySpaceRequiredOnDriveW(handle, wide, space, reserved1, reserved2);
	error = GetLastError();

	setup_free(wide);
	SetLastError(error);
	return result;
}

BOOL WINAPI SetupQueryDrivesInDiskSpaceListW(HDSKSPC handle, PWSTR buffer, DWORD size, PDWORD required)
{
	struct space_list *list = handle;
	struct space_entry *entry, *other;
	DWORD needed = 1, used = 0;

	if (!list) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	for (entry = list->entries; entry; entry = entry->next) {
		BOOL seen = FALSE;
		for (other = list->entries; other != entry; other = other->next)
			seen |= same_drive(other->path, entry->path);
		if (seen)
			continue;
		if (buffer && used + 3 < size) {
			buffer[used] = entry->path[0];
			buffer[used + 1] = ':';
			buffer[used + 2] = 0;
		}
		used += 3;
		needed += 3;
	}
	if (required)
		*required = needed;
	if (buffer) {
		if (size < needed) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		buffer[used] = 0;
	}
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI SetupQueryDrivesInDiskSpaceListA(HDSKSPC handle, PSTR buffer, DWORD size, PDWORD required)
{
	WCHAR text[128];
	DWORD chars = 0, at;

	if (!SetupQueryDrivesInDiskSpaceListW(handle, text, ARRAYSIZE(text), &chars))
		return FALSE;
	if (required)
		*required = chars;
	if (buffer) {
		if (size < chars) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		for (at = 0; at < chars; at++)
			buffer[at] = (char)text[at];
	}
	return TRUE;
}

/* Sections added to a list: their copies with real sizes when a layout gives them. */

static BOOL space_from_section(HDSKSPC handle, HINF hinf, HINF layout, const WCHAR *section, UINT operation, BOOL removing)
{
	INFCONTEXT context;
	WCHAR *directory = setup_section_target(hinf, section);
	BOOL result = TRUE;

	if (!directory)
		return FALSE;
	if (SetupFindFirstLineW(hinf, section, NULL, &context)) {
		do {
			const WCHAR *file = pSetupGetField(&context, 1);
			WCHAR *full;
			DWORD size = 0;

			if (!file || !*file)
				continue;
			full = setup_join_path(directory, file);
			SetupGetSourceFileSizeW(layout ? layout : hinf, NULL, file, NULL, &size, 0);
			if (removing)
				SetupRemoveFromDiskSpaceListW(handle, full, operation, NULL, 0);
			else
				SetupAddToDiskSpaceListW(handle, full, size, operation, NULL, 0);
			setup_free(full);
		} while (SetupFindNextLine(&context, &context));
	} else {
		result = FALSE;
	}
	setup_free(directory);
	return result;
}

BOOL WINAPI SetupAddSectionToDiskSpaceListW(HDSKSPC handle, HINF hinf, HINF layout, PCWSTR section, UINT operation, PVOID reserved1, UINT reserved2)
{
	return space_from_section(handle, hinf, layout, section, operation, FALSE);
}

BOOL WINAPI SetupAddSectionToDiskSpaceListA(HDSKSPC handle, HINF hinf, HINF layout, PCSTR section, UINT operation, PVOID reserved1, UINT reserved2)
{
	WCHAR *wide = setup_to_wide(section);
	BOOL result = space_from_section(handle, hinf, layout, wide, operation, FALSE);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupRemoveSectionFromDiskSpaceListW(HDSKSPC handle, HINF hinf, HINF layout, PCWSTR section, UINT operation, PVOID reserved1, UINT reserved2)
{
	return space_from_section(handle, hinf, layout, section, operation, TRUE);
}

BOOL WINAPI SetupRemoveSectionFromDiskSpaceListA(HDSKSPC handle, HINF hinf, HINF layout, PCSTR section, UINT operation, PVOID reserved1, UINT reserved2)
{
	WCHAR *wide = setup_to_wide(section);
	BOOL result = space_from_section(handle, hinf, layout, wide, operation, TRUE);

	setup_free(wide);
	return result;
}

static BOOL space_from_install(HDSKSPC handle, HINF hinf, HINF layout, const WCHAR *install, BOOL removing)
{
	static const struct { const WCHAR *directive; UINT operation; } kinds[] = { { L"CopyFiles", FILEOP_COPY }, { L"DelFiles", FILEOP_DELETE } };
	UINT kind;

	for (kind = 0; kind < ARRAYSIZE(kinds); kind++) {
		INFCONTEXT context;
		if (!SetupFindFirstLineW(hinf, install, kinds[kind].directive, &context))
			continue;
		do {
			DWORD count = SetupGetFieldCount(&context), at;
			for (at = 1; at <= count; at++) {
				const WCHAR *name = pSetupGetField(&context, at);
				if (name && *name && name[0] != '@')
					space_from_section(handle, hinf, layout, name, kinds[kind].operation, removing);
			}
		} while (SetupFindNextMatchLineW(&context, kinds[kind].directive, &context));
	}
	return TRUE;
}

BOOL WINAPI SetupAddInstallSectionToDiskSpaceListW(HDSKSPC handle, HINF hinf, HINF layout, PCWSTR section, PVOID reserved1, UINT reserved2)
{
	return space_from_install(handle, hinf, layout, section, FALSE);
}

BOOL WINAPI SetupAddInstallSectionToDiskSpaceListA(HDSKSPC handle, HINF hinf, HINF layout, PCSTR section, PVOID reserved1, UINT reserved2)
{
	WCHAR *wide = setup_to_wide(section);
	BOOL result = space_from_install(handle, hinf, layout, wide, FALSE);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupRemoveInstallSectionFromDiskSpaceListW(HDSKSPC handle, HINF hinf, HINF layout, PCWSTR section, PVOID reserved1, UINT reserved2)
{
	return space_from_install(handle, hinf, layout, section, TRUE);
}

BOOL WINAPI SetupRemoveInstallSectionFromDiskSpaceListA(HDSKSPC handle, HINF hinf, HINF layout, PCSTR section, PVOID reserved1, UINT reserved2)
{
	WCHAR *wide = setup_to_wide(section);
	BOOL result = space_from_install(handle, hinf, layout, wide, TRUE);

	setup_free(wide);
	return result;
}

/* OEM INF copies: %windir%\inf\oemN.inf, reusing an existing identical file. */

static BOOL same_file_content(const WCHAR *left, const WCHAR *right)
{
	HANDLE a = CreateFileW(left, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	HANDLE b = a == INVALID_HANDLE_VALUE ? INVALID_HANDLE_VALUE : CreateFileW(right, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	BOOL same = FALSE;

	if (b != INVALID_HANDLE_VALUE) {
		DWORD size = GetFileSize(a, NULL);
		if (size == GetFileSize(b, NULL)) {
			BYTE *one = setup_alloc(size ? size : 1), *two = setup_alloc(size ? size : 1);
			DWORD got1 = 0, got2 = 0;
			if (one && two && ReadFile(a, one, size, &got1, NULL) && ReadFile(b, two, size, &got2, NULL))
				same = got1 == size && got2 == size && !memcmp(one, two, size);
			setup_free(one);
			setup_free(two);
		}
		CloseHandle(b);
	}
	if (a != INVALID_HANDLE_VALUE)
		CloseHandle(a);
	return same;
}

BOOL WINAPI SetupCopyOEMInfW(PCWSTR source, PCWSTR location, DWORD media_type, DWORD style, PWSTR buffer, DWORD size, PDWORD required, PWSTR *name_part)
{
	WCHAR directory[MAX_PATH], pattern[MAX_PATH + 16], target[MAX_PATH + 16];
	WIN32_FIND_DATAW found;
	HANDLE search;
	BOOL have = FALSE;
	UINT number;
	const WCHAR *slash;

	if (!source) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (style & ~(SP_COPY_DELETESOURCE | SP_COPY_REPLACEONLY | SP_COPY_NOOVERWRITE | SP_COPY_OEMINF_CATALOG_ONLY)) {
		SetLastError(ERROR_INVALID_FLAGS);
		return FALSE;
	}
	if (GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES) {
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	if (!(slash = wcschr(source, '\\')) && !wcschr(source, '/')) {
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	GetWindowsDirectoryW(directory, MAX_PATH);
	lstrcatW(directory, L"\\inf");
	CreateDirectoryW(directory, NULL);
	lstrcpyW(pattern, directory);
	lstrcatW(pattern, L"\\oem*.inf");
	search = FindFirstFileW(pattern, &found);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			lstrcpyW(target, directory);
			lstrcatW(target, L"\\");
			lstrcatW(target, found.cFileName);
			if (same_file_content(source, target)) {
				have = TRUE;
				break;
			}
		} while (FindNextFileW(search, &found));
		FindClose(search);
	}
	if (have && (style & SP_COPY_NOOVERWRITE)) {
		setup_give_wide(target, buffer, size, required);
		if (name_part && buffer)
			*name_part = wcsrchr(buffer, '\\') + 1;
		SetLastError(ERROR_FILE_EXISTS);
		return FALSE;
	}
	if (!have) {
		if (style & SP_COPY_REPLACEONLY) {
			SetLastError(ERROR_FILE_NOT_FOUND);
			return FALSE;
		}
		for (number = 0; number < 100000; number++) {
			wsprintfW(target, L"%s\\oem%u.inf", directory, number);
			if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES)
				break;
		}
		if (!CopyFileW(source, target, TRUE))
			return FALSE;
	}
	if (style & SP_COPY_DELETESOURCE)
		DeleteFileW(source);
	if (required)
		*required = lstrlenW(target) + 1;
	if (buffer) {
		if (size < (DWORD)lstrlenW(target) + 1) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		lstrcpyW(buffer, target);
		if (name_part)
			*name_part = wcsrchr(buffer, '\\') + 1;
	}
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI SetupCopyOEMInfA(PCSTR source, PCSTR location, DWORD media_type, DWORD style, PSTR buffer, DWORD size, PDWORD required, PSTR *name_part)
{
	WCHAR *wide_source = setup_to_wide(source), *wide_location = setup_to_wide(location);
	WCHAR text[MAX_PATH + 16];
	BOOL result = SetupCopyOEMInfW(wide_source, wide_location, media_type, style, text, ARRAYSIZE(text), NULL, NULL);
	DWORD error = GetLastError();

	setup_free(wide_source);
	setup_free(wide_location);
	if (!result) {
		SetLastError(error);
		return FALSE;
	}
	if (!setup_wide_to_ansi_result(TRUE, text, buffer, size, required))
		return FALSE;
	if (name_part && buffer)
		*name_part = strrchr(buffer, '\\') + 1;
	SetLastError(error);
	return TRUE;
}

BOOL WINAPI SetupUninstallOEMInfW(PCWSTR name, DWORD flags, PVOID reserved)
{
	WCHAR directory[MAX_PATH], *path;
	BOOL result;

	if (!name) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	GetWindowsDirectoryW(directory, MAX_PATH);
	lstrcatW(directory, L"\\inf");
	path = setup_join_path(directory, name);
	if (!*name || !path || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
		setup_free(path);
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	result = DeleteFileW(path);
	setup_free(path);
	return result;
}

BOOL WINAPI SetupUninstallOEMInfA(PCSTR name, DWORD flags, PVOID reserved)
{
	WCHAR *wide = setup_to_wide(name);
	BOOL result = SetupUninstallOEMInfW(wide, flags, reserved);
	DWORD error = GetLastError();

	setup_free(wide);
	SetLastError(error);
	return result;
}

/* String tables: strings with stable ids, optional fixed size extra data. An id is the position
 * plus one, so that 0 and -1 stay free for "nothing". */

#define ST_CASE_SENSITIVE 0x00000001

struct string_entry {
	WCHAR *text;
	BYTE *extra;
};

struct string_table {
	struct string_entry *entries;
	DWORD count;
	DWORD capacity;
	DWORD extra_size;
};

typedef struct string_table *string_table_handle;

string_table_handle WINAPI pSetupStringTableInitializeEx(DWORD extra_size, DWORD reserved)
{
	struct string_table *table = setup_alloc(sizeof(*table));

	if (table)
		table->extra_size = extra_size;
	return table;
}

string_table_handle WINAPI pSetupStringTableInitialize(void)
{
	return pSetupStringTableInitializeEx(0, 0);
}

void WINAPI pSetupStringTableDestroy(string_table_handle table)
{
	DWORD at;

	if (!table)
		return;
	for (at = 0; at < table->count; at++) {
		setup_free(table->entries[at].text);
		setup_free(table->entries[at].extra);
	}
	setup_free(table->entries);
	setup_free(table);
}

DWORD WINAPI pSetupStringTableLookUpStringEx(string_table_handle table, const WCHAR *text, DWORD flags, void *extra, DWORD extra_size)
{
	DWORD at;

	if (!table)
		return (DWORD)-1;
	for (at = 0; at < table->count; at++) {
		const WCHAR *entry = table->entries[at].text;
		if (flags & ST_CASE_SENSITIVE ? !lstrcmpW(entry, text) : !lstrcmpiW(entry, text)) {
			if (extra && extra_size && table->entries[at].extra)
				memcpy(extra, table->entries[at].extra, min(extra_size, table->extra_size));
			return at + 1;
		}
	}
	return (DWORD)-1;
}

DWORD WINAPI pSetupStringTableLookUpString(string_table_handle table, const WCHAR *text, DWORD flags)
{
	return pSetupStringTableLookUpStringEx(table, text, flags, NULL, 0);
}

DWORD WINAPI pSetupStringTableAddStringEx(string_table_handle table, const WCHAR *text, DWORD flags, void *extra, DWORD extra_size)
{
	DWORD found, at;

	if (!table)
		return (DWORD)-1;
	found = pSetupStringTableLookUpStringEx(table, text, flags, NULL, 0);
	if (found != (DWORD)-1)
		return found;
	if (table->count == table->capacity) {
		DWORD capacity = table->capacity ? table->capacity * 2 : 16;
		struct string_entry *grown = setup_realloc(table->entries, capacity * sizeof(*grown));
		if (!grown)
			return (DWORD)-1;
		table->entries = grown;
		table->capacity = capacity;
	}
	at = table->count;
	table->entries[at].text = setup_copy_wide(text);
	table->entries[at].extra = NULL;
	if (table->extra_size) {
		table->entries[at].extra = setup_alloc(table->extra_size);
		if (table->entries[at].extra && extra)
			memcpy(table->entries[at].extra, extra, min(extra_size, table->extra_size));
	}
	table->count++;
	return at + 1;
}

DWORD WINAPI pSetupStringTableAddString(string_table_handle table, const WCHAR *text, DWORD flags)
{
	return pSetupStringTableAddStringEx(table, text, flags, NULL, 0);
}

string_table_handle WINAPI pSetupStringTableDuplicate(string_table_handle table)
{
	struct string_table *copy;
	DWORD at;

	if (!table)
		return NULL;
	copy = pSetupStringTableInitializeEx(table->extra_size, 0);
	if (!copy)
		return NULL;
	for (at = 0; at < table->count; at++)
		pSetupStringTableAddStringEx(copy, table->entries[at].text, ST_CASE_SENSITIVE, table->entries[at].extra, table->extra_size);
	return copy;
}

WCHAR *WINAPI pSetupStringTableStringFromId(string_table_handle table, DWORD id)
{
	if (!table || !id || id > table->count)
		return NULL;
	return table->entries[id - 1].text;
}

BOOL WINAPI pSetupStringTableStringFromIdEx(string_table_handle table, DWORD id, WCHAR *buffer, DWORD *size)
{
	const WCHAR *text = pSetupStringTableStringFromId(table, id);

	if (!text) {
		if (size)
			*size = 0;
		return FALSE;
	}
	return setup_give_wide(text, buffer, size ? *size : 0, size);
}

BOOL WINAPI pSetupStringTableGetExtraData(string_table_handle table, DWORD id, void *extra, DWORD extra_size)
{
	if (!table || !id || id > table->count || extra_size > table->extra_size)
		return FALSE;
	if (table->entries[id - 1].extra)
		memcpy(extra, table->entries[id - 1].extra, extra_size);
	return TRUE;
}

BOOL WINAPI pSetupStringTableSetExtraData(string_table_handle table, DWORD id, void *extra, DWORD extra_size)
{
	if (!table || !id || id > table->count || extra_size > table->extra_size)
		return FALSE;
	if (table->entries[id - 1].extra)
		memcpy(table->entries[id - 1].extra, extra, extra_size);
	return TRUE;
}

/* The device version answer callers use to tell the plug and play manager's generation. */
WORD WINAPI CM_Get_Version(void)
{
	return 0x0400;
}

WORD WINAPI CM_Get_Version_Ex(HANDLE machine)
{
	return 0x0400;
}
