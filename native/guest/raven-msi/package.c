/* SPDX-License-Identifier: MIT */
#include "msi_own.h"
#include <shlobj.h>
#include <sddl.h>

static CRITICAL_SECTION ui_lock;
static INSTALLUI_HANDLERW ui_handler_w;
static INSTALLUI_HANDLERA ui_handler_a;
static INSTALLUI_HANDLER_RECORD ui_handler_record;
static DWORD ui_filter;
static void *ui_context;
static INSTALLUILEVEL ui_level = INSTALLUILEVEL_DEFAULT;
static HWND ui_window;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static DWORD log_mode;
static WCHAR log_path[MAX_PATH];

void package_startup(void)
{
	InitializeCriticalSection(&ui_lock);
}

void package_log(const WCHAR *format, ...)
{
	if (log_file == INVALID_HANDLE_VALUE)
		return;
	WCHAR text[1024];
	va_list args;
	va_start(args, format);
	int length = wvsprintfW(text, format, args);
	va_end(args);
	char *narrow = msi_to_ansi(text);
	DWORD put;
	if (narrow && length > 0) {
		WriteFile(log_file, narrow, lstrlenA(narrow), &put, NULL);
		WriteFile(log_file, "\r\n", 2, &put, NULL);
	}
	msi_free(narrow);
}

UINT ui_deliver(INSTALLMESSAGE type, const WCHAR *text, UINT field_one)
{
	UINT answer = 0;
	(void)field_one;
	EnterCriticalSection(&ui_lock);
	DWORD bit = 1u << (((DWORD)type >> 24) & 0x1f);
	INSTALLUI_HANDLERW wide = (ui_filter & bit) ? ui_handler_w : NULL;
	INSTALLUI_HANDLERA narrow = (ui_filter & bit) ? ui_handler_a : NULL;
	INSTALLUI_HANDLER_RECORD by_record = (ui_filter & bit) ? ui_handler_record : NULL;
	void *context = ui_context;
	LeaveCriticalSection(&ui_lock);
	if (by_record) {
		msi_record *record = record_new(1);
		if (record) {
			record_set_text(record, 0, text);
			MSIHANDLE handle = handle_open(&record->base);
			answer = by_record(context, type, handle);
			MsiCloseHandle(handle);
		}
	} else if (wide)
		answer = wide(context, type, text);
	else if (narrow) {
		char *ansi = msi_to_ansi(text);
		answer = narrow(context, type, ansi);
		msi_free(ansi);
	}
	return answer;
}

void package_message(msi_package *package, INSTALLMESSAGE type, const WCHAR *text)
{
	(void)package;
	package_log(L"%s", text ? text : L"");
	ui_deliver(type, text ? text : L"", 0);
}

// Where each digit of a squashed code sits in the braced form: the first three groups reversed, the rest swapped in pairs.
static const signed char squash_order[32] = { 8, 7, 6, 5, 4, 3, 2, 1, 13, 12, 11, 10, 18, 17, 16, 15, 21, 20, 23, 22, 26, 25, 28, 27, 30, 29, 32, 31, 34, 33, 36, 35 };

BOOL guid_squash(const WCHAR *guid, WCHAR out[33])
{
	if (!guid || lstrlenW(guid) != 38 || guid[0] != '{' || guid[37] != '}')
		return FALSE;
	for (int i = 0; i < 32; i++)
		out[i] = guid[squash_order[i]];
	out[32] = 0;
	return TRUE;
}

BOOL guid_unsquash(const WCHAR *squashed, WCHAR out[39])
{
	if (!squashed || lstrlenW(squashed) != 32)
		return FALSE;
	lstrcpyW(out, L"{00000000-0000-0000-0000-000000000000}");
	for (int i = 0; i < 32; i++)
		out[squash_order[i]] = squashed[i];
	return TRUE;
}

static void package_destroy(msi_object *object)
{
	msi_package *package = (msi_package *)object;
	msi_free(package->command_line);
	for (UINT i = 0; i < package->property_count; i++) {
		msi_free(package->properties[i].name);
		msi_free(package->properties[i].value);
	}
	msi_free(package->properties);
	msi_free(package->path);
	if (package->db)
		object_unref(&package->db->base);
	if (package->remote)
		CloseHandle(package->remote);
	DeleteCriticalSection(&package->lock);
}

static msi_property *property_find(msi_package *package, const WCHAR *name)
{
	for (UINT i = 0; i < package->property_count; i++)
		if (!lstrcmpW(package->properties[i].name, name))
			return &package->properties[i];
	return NULL;
}

WCHAR *package_get(msi_package *package, const WCHAR *name)
{
	if (package->remote) {
		WCHAR *text = NULL;
		remote_call(package->remote, REMOTE_GET_PROPERTY, name, NULL, 0, &text, NULL);
		return text;
	}
	EnterCriticalSection(&package->lock);
	msi_property *found = property_find(package, name);
	WCHAR *value = found ? msi_copy(found->value) : NULL;
	LeaveCriticalSection(&package->lock);
	return value;
}

INT package_get_int(msi_package *package, const WCHAR *name, INT fallback)
{
	WCHAR *value = package_get(package, name);
	INT number = fallback;
	if (value && *value) {
		BOOL negative = *value == '-';
		INT total = 0;
		const WCHAR *at = value + negative;
		for (; *at >= '0' && *at <= '9'; at++)
			total = total * 10 + (*at - '0');
		if (!*at)
			number = negative ? -total : total;
	}
	msi_free(value);
	return number;
}

BOOL package_is(msi_package *package, const WCHAR *name, const WCHAR *expected)
{
	WCHAR *value = package_get(package, name);
	BOOL same = value && !lstrcmpW(value, expected);
	msi_free(value);
	return same;
}

// An empty value removes the property, as MsiSetProperty does.
UINT package_set(msi_package *package, const WCHAR *name, const WCHAR *value)
{
	if (package->remote)
		return remote_call(package->remote, REMOTE_SET_PROPERTY, name, value ? value : L"", 0, NULL, NULL);
	EnterCriticalSection(&package->lock);
	msi_property *found = property_find(package, name);
	if (!value || !*value) {
		if (found) {
			msi_free(found->name);
			msi_free(found->value);
			*found = package->properties[--package->property_count];
		}
	} else if (found) {
		msi_free(found->value);
		found->value = msi_copy(value);
	} else {
		if (package->property_count == package->property_room) {
			package->property_room = package->property_room ? package->property_room * 2 : 128;
			package->properties = msi_grow(package->properties, package->property_room * sizeof(msi_property));
		}
		package->properties[package->property_count].name = msi_copy(name);
		package->properties[package->property_count].value = msi_copy(value);
		package->property_count++;
	}
	LeaveCriticalSection(&package->lock);
	return ERROR_SUCCESS;
}

static void set_number(msi_package *package, const WCHAR *name, INT value)
{
	WCHAR text[16];
	wsprintfW(text, L"%d", value);
	package_set(package, name, text);
}

static void set_folder(msi_package *package, const WCHAR *name, int csidl)
{
	WCHAR path[MAX_PATH];
	if (FAILED(SHGetFolderPathW(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, path)))
		return;
	UINT length = lstrlenW(path);
	if (length && path[length - 1] != '\\' && length + 1 < MAX_PATH)
		lstrcpyW(path + length, L"\\");
	package_set(package, name, path);
}

static void set_directory(msi_package *package, const WCHAR *name, const WCHAR *path)
{
	WCHAR text[MAX_PATH + 1];
	lstrcpynW(text, path, MAX_PATH);
	UINT length = lstrlenW(text);
	if (length && text[length - 1] != '\\')
		lstrcpyW(text + length, L"\\");
	package_set(package, name, text);
}

static void set_system_properties(msi_package *package)
{
	WCHAR path[MAX_PATH], combined[MAX_PATH + 32];
	BOOL wow64 = FALSE;
	IsWow64Process(GetCurrentProcess(), &wow64);
	BOOL is64 = wow64 || sizeof(void *) == 8;

	GetWindowsDirectoryW(path, MAX_PATH);
	set_directory(package, L"WindowsFolder", path);
	WCHAR drive[4] = { path[0], ':', '\\', 0 };
	package_set(package, L"WindowsVolume", drive);
	package_set(package, L"ROOTDRIVE", drive);
	wsprintfW(combined, L"%s\\Fonts", path);
	set_directory(package, L"FontsFolder", combined);
	GetSystemDirectoryW(path, MAX_PATH);
	if (is64) {
		set_directory(package, L"System64Folder", path);
		GetWindowsDirectoryW(path, MAX_PATH);
		wsprintfW(combined, L"%s\\SysWOW64", path);
		set_directory(package, L"SystemFolder", combined);
		set_number(package, L"VersionNT64", 603);
		set_number(package, L"Msix64", 603);
		package_set(package, L"MsiAMD64", L"603");
	} else
		set_directory(package, L"SystemFolder", path);
	set_number(package, L"Intel", 6);

	WCHAR base[MAX_PATH];
	lstrcpyW(base, L"C:\\Program Files");
	ExpandEnvironmentStringsW(L"%ProgramW6432%", path, MAX_PATH);
	if (path[0] != '%')
		lstrcpyW(base, path);
	else if (GetEnvironmentVariableW(L"ProgramFiles", path, MAX_PATH))
		lstrcpyW(base, path);
	if (is64) {
		set_directory(package, L"ProgramFiles64Folder", base);
		wsprintfW(combined, L"%s\\Common Files", base);
		set_directory(package, L"CommonFiles64Folder", combined);
		wsprintfW(combined, L"%s (x86)", base);
		set_directory(package, L"ProgramFilesFolder", combined);
		wsprintfW(combined, L"%s (x86)\\Common Files", base);
		set_directory(package, L"CommonFilesFolder", combined);
	} else {
		set_directory(package, L"ProgramFilesFolder", base);
		wsprintfW(combined, L"%s\\Common Files", base);
		set_directory(package, L"CommonFilesFolder", combined);
	}
	static const struct { const WCHAR *name; int csidl; } folders[] = {
		{ L"AppDataFolder", CSIDL_APPDATA }, { L"LocalAppDataFolder", CSIDL_LOCAL_APPDATA }, { L"PersonalFolder", CSIDL_PERSONAL },
		{ L"MyPicturesFolder", CSIDL_MYPICTURES }, { L"NetHoodFolder", CSIDL_NETHOOD }, { L"PrintHoodFolder", CSIDL_PRINTHOOD },
		{ L"RecentFolder", CSIDL_RECENT }, { L"SendToFolder", CSIDL_SENDTO }, { L"TemplateFolder", CSIDL_TEMPLATES },
		{ L"CommonAppDataFolder", CSIDL_COMMON_APPDATA }, { L"AdminToolsFolder", CSIDL_COMMON_ADMINTOOLS }, { L"FavoritesFolder", CSIDL_FAVORITES },
		{ L"DesktopFolder", CSIDL_COMMON_DESKTOPDIRECTORY }, { L"ProgramMenuFolder", CSIDL_COMMON_PROGRAMS }, { L"StartMenuFolder", CSIDL_COMMON_STARTMENU },
		{ L"StartupFolder", CSIDL_COMMON_STARTUP },
	};
	for (UINT i = 0; i < ARRAYSIZE(folders); i++)
		set_folder(package, folders[i].name, folders[i].csidl);
	GetTempPathW(MAX_PATH, path);
	set_directory(package, L"TempFolder", path);

	set_number(package, L"VersionNT", 603);
	set_number(package, L"WindowsBuild", 9600);
	set_number(package, L"ServicePackLevel", 0);
	set_number(package, L"ServicePackLevelMinor", 0);
	set_number(package, L"VersionMsi", 500);
	set_number(package, L"MsiNTProductType", 1);
	set_number(package, L"Privileged", 1);
	set_number(package, L"AdminUser", 1);
	set_number(package, L"SystemLanguageID", GetSystemDefaultLangID());
	set_number(package, L"UserLanguageID", GetUserDefaultLangID());
	set_number(package, L"ScreenX", GetSystemMetrics(SM_CXSCREEN));
	set_number(package, L"ScreenY", GetSystemMetrics(SM_CYSCREEN));
	set_number(package, L"ColorBits", 32);
	set_number(package, L"TextHeight", 16);
	MEMORYSTATUSEX memory = { sizeof memory };
	if (GlobalMemoryStatusEx(&memory))
		set_number(package, L"PhysicalMemory", (INT)(memory.ullTotalPhys >> 20));
	DWORD size = MAX_PATH;
	if (GetComputerNameW(path, &size))
		package_set(package, L"ComputerName", path);
	size = MAX_PATH;
	if (GetUserNameW(path, &size))
		package_set(package, L"LogonUser", path);
	WCHAR sid[80];
	product_user_sid(sid);
	package_set(package, L"UserSID", sid);
	SYSTEMTIME now;
	GetLocalTime(&now);
	wsprintfW(combined, L"%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
	package_set(package, L"Time", combined);
	wsprintfW(combined, L"%u/%u/%u", now.wMonth, now.wDay, now.wYear);
	package_set(package, L"Date", combined);
	package_set(package, L"Installed", NULL);
}

static UINT property_row(msi_record *row, void *context)
{
	WCHAR a[16], b[16];
	const WCHAR *name = record_text(row, 1, a), *value = record_text(row, 2, b);
	if (name)
		package_set(context, name, value);
	return ERROR_SUCCESS;
}

UINT package_open_db(msi_db *db, const WCHAR *path, msi_package **result)
{
	msi_package *package = msi_alloc(sizeof *package);
	if (!package)
		return ERROR_OUTOFMEMORY;
	ZeroMemory(package, sizeof *package);
	object_init(&package->base, KIND_PACKAGE, package_destroy);
	InitializeCriticalSection(&package->lock);
	object_ref(&db->base);
	package->db = db;
	package->path = msi_copy(path ? path : L"");
	set_system_properties(package);
	db_query_each(db, L"SELECT `Property`, `Value` FROM `Property`", NULL, property_row, package);
	if (path && *path) {
		package_set(package, L"OriginalDatabase", path);
		package_set(package, L"DATABASE", path);
		WCHAR source[MAX_PATH + 1];
		lstrcpynW(source, path, MAX_PATH);
		WCHAR *cut = source + lstrlenW(source);
		while (cut > source && cut[-1] != '\\' && cut[-1] != '/')
			cut--;
		*cut = 0;
		if (*source)
			package_set(package, L"SourceDir", source);
		else {
			GetCurrentDirectoryW(MAX_PATH, source);
			set_directory(package, L"SourceDir", source);
		}
	}
	WCHAR *code = package_get(package, L"ProductCode");
	if (code) {
		guid_squash(code, package->squashed);
		INSTALLSTATE state = product_state(code);
		set_number(package, L"ProductState", state);
		if (state == INSTALLSTATE_DEFAULT) {
			package_set(package, L"Installed", L"1");
			package->maintenance = TRUE;
		}
		msi_free(code);
	}
	*result = package;
	return ERROR_SUCCESS;
}

UINT package_open(const WCHAR *path, msi_package **result)
{
	msi_db *db;
	UINT status = db_open(path, (const WCHAR *)MSIDBOPEN_READONLY, &db);
	if (status)
		return status;
	status = package_open_db(db, path, result);
	object_unref(&db->base);
	return status;
}

msi_package *package_from_handle(MSIHANDLE handle)
{
	return (msi_package *)handle_lookup(handle, KIND_PACKAGE);
}

// PROPERTY=value pairs, where a value may be quoted and "" stands for one quote.
void package_reload_properties(msi_package *package)
{
	db_query_each(package->db, L"SELECT `Property`, `Value` FROM `Property`", NULL, property_row, package);
	WCHAR *line = package->command_line ? msi_copy(package->command_line) : NULL;
	package_command_line(package, line);
	msi_free(line);
}

UINT package_command_line(msi_package *package, const WCHAR *line)
{
	if (!line)
		return ERROR_SUCCESS;
	if (line != package->command_line) {
		msi_free(package->command_line);
		package->command_line = msi_copy(line);
	}
	const WCHAR *at = line;
	for (;;) {
		while (*at == ' ' || *at == '\t')
			at++;
		if (!*at)
			break;
		const WCHAR *start = at;
		while (*at && *at != '=' && *at != ' ' && *at != '\t')
			at++;
		if (*at != '=')
			return ERROR_INVALID_COMMAND_LINE;
		WCHAR *name = msi_copy_n(start, at - start);
		SIZE_T room = lstrlenW(at) + 1, length = 0;
		WCHAR *value = msi_alloc(room * sizeof(WCHAR));
		BOOL quoted = FALSE;
		for (at++; *at && (quoted || (*at != ' ' && *at != '\t')); at++) {
			if (*at == '"') {
				if (quoted && at[1] == '"') {
					value[length++] = '"';
					at++;
				} else
					quoted = !quoted;
			} else
				value[length++] = *at;
		}
		value[length] = 0;
		CharUpperW(name);
		package_set(package, name, value);
		msi_free(name);
		msi_free(value);
	}
	return ERROR_SUCCESS;
}

UINT WINAPI MsiOpenPackageExW(LPCWSTR path, DWORD options, MSIHANDLE *handle)
{
	(void)options;
	if (!path || !handle)
		return ERROR_INVALID_PARAMETER;
	msi_package *package;
	UINT status;
	if (path[0] == '#') {
		const WCHAR *digits = path + 1;
		MSIHANDLE database = 0;
		while (*digits >= '0' && *digits <= '9')
			database = database * 10 + (*digits++ - '0');
		msi_db *db = (msi_db *)handle_lookup(database, KIND_DATABASE);
		if (!db)
			return ERROR_INVALID_HANDLE;
		status = package_open_db(db, db->path, &package);
		object_unref(&db->base);
	} else {
		if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
			return ERROR_FILE_NOT_FOUND;
		status = package_open(path, &package);
		if (status)
			status = ERROR_INSTALL_PACKAGE_INVALID;
	}
	if (status)
		return status;
	*handle = handle_open(&package->base);
	package->session = *handle;
	return *handle ? ERROR_SUCCESS : ERROR_OUTOFMEMORY;
}

UINT WINAPI MsiOpenPackageW(LPCWSTR path, MSIHANDLE *handle)
{
	return MsiOpenPackageExW(path, 0, handle);
}

UINT WINAPI MsiOpenPackageExA(LPCSTR path, DWORD options, MSIHANDLE *handle)
{
	WCHAR *wide = msi_from_ansi(path);
	UINT status = MsiOpenPackageExW(wide, options, handle);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiOpenPackageA(LPCSTR path, MSIHANDLE *handle)
{
	return MsiOpenPackageExA(path, 0, handle);
}

UINT WINAPI MsiGetPropertyW(MSIHANDLE handle, LPCWSTR name, LPWSTR out, LPDWORD size)
{
	if (!name)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	WCHAR *value = package_get(package, name);
	UINT status = out_text_w(value ? value : L"", value ? lstrlenW(value) : 0, out, size);
	msi_free(value);
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiGetPropertyA(MSIHANDLE handle, LPCSTR name, LPSTR out, LPDWORD size)
{
	if (!name)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	WCHAR *wide = msi_from_ansi(name), *value = package_get(package, wide);
	UINT status = out_text_a(value ? value : L"", value ? lstrlenW(value) : 0, out, size);
	msi_free(value);
	msi_free(wide);
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiSetPropertyW(MSIHANDLE handle, LPCWSTR name, LPCWSTR value)
{
	if (!name || !*name)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status = package_set(package, name, value);
	if (!package->remote && package->tables_loaded)
		directories_reset(package);
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiSetPropertyA(MSIHANDLE handle, LPCSTR name, LPCSTR value)
{
	if (!name)
		return ERROR_INVALID_PARAMETER;
	WCHAR *wide_name = msi_from_ansi(name), *wide_value = value ? msi_from_ansi(value) : NULL;
	UINT status = MsiSetPropertyW(handle, wide_name, wide_value);
	msi_free(wide_name);
	msi_free(wide_value);
	return status;
}

BOOL WINAPI MsiGetMode(MSIHANDLE handle, MSIRUNMODE mode)
{
	msi_package *package = package_from_handle(handle);
	BOOL result = FALSE;
	if (!package)
		return FALSE;
	if (package->remote) {
		INT answer = 0;
		remote_call(package->remote, REMOTE_MODE, NULL, NULL, mode, NULL, &answer);
		result = answer;
	} else
		switch (mode) {
		case MSIRUNMODE_SCHEDULED: result = package->scheduled; break;
		case MSIRUNMODE_ROLLBACK: result = package->rollback; break;
		case MSIRUNMODE_COMMIT: result = package->commit; break;
		case MSIRUNMODE_MAINTENANCE: result = package->maintenance; break;
		case MSIRUNMODE_REBOOTATEND: result = package->reboot_at_end; break;
		case MSIRUNMODE_REBOOTNOW: result = package->reboot_now; break;
		case MSIRUNMODE_WINDOWS9X: result = FALSE; break;
		case MSIRUNMODE_LOGENABLED: result = log_file != INVALID_HANDLE_VALUE; break;
		case MSIRUNMODE_SOURCESHORTNAMES: case MSIRUNMODE_TARGETSHORTNAMES: result = FALSE; break;
		default: result = FALSE; break;
		}
	object_unref(&package->base);
	return result;
}

UINT WINAPI MsiSetMode(MSIHANDLE handle, MSIRUNMODE mode, BOOL state)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_SUCCESS;
	if (package->remote)
		status = remote_call(package->remote, REMOTE_SET_MODE, NULL, NULL, mode | (state << 16), NULL, NULL);
	else if (mode == MSIRUNMODE_REBOOTATEND)
		package->reboot_at_end = state;
	else if (mode == MSIRUNMODE_REBOOTNOW)
		package->reboot_now = state;
	else
		status = ERROR_ACCESS_DENIED;
	object_unref(&package->base);
	return status;
}

LANGID WINAPI MsiGetLanguage(MSIHANDLE handle)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return 0;
	INT language = 0;
	if (package->remote)
		remote_call(package->remote, REMOTE_LANGUAGE, NULL, NULL, 0, NULL, &language);
	else
		language = package_get_int(package, L"ProductLanguage", 0);
	object_unref(&package->base);
	return (LANGID)language;
}

MSIDBERROR WINAPI MsiViewGetErrorW(MSIHANDLE view, LPWSTR column, LPDWORD size);

MSIHANDLE WINAPI MsiGetActiveDatabase(MSIHANDLE handle)
{
	msi_package *package = package_from_handle(handle);
	MSIHANDLE result = 0;
	if (!package)
		return 0;
	if (package->remote) {
		// One database for the whole action, which asks for it again for every row it works on.
		if (!package->db) {
			WCHAR *path = NULL;
			remote_call(package->remote, REMOTE_DATABASE_PATH, NULL, NULL, 0, &path, NULL);
			if (path && db_open(path, (const WCHAR *)MSIDBOPEN_READONLY, &package->db))
				package->db = NULL;
			if (package->db)
				package->db->remote = package->remote;
			msi_free(path);
		}
		if (package->db) {
			object_ref(&package->db->base);
			result = handle_open(&package->db->base);
		}
	} else {
		object_ref(&package->db->base);
		result = handle_open(&package->db->base);
	}
	object_unref(&package->base);
	return result;
}

INT WINAPI MsiProcessMessage(MSIHANDLE handle, INSTALLMESSAGE type, MSIHANDLE record_handle)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return -1;
	msi_record *record = (msi_record *)handle_lookup(record_handle, KIND_RECORD);
	if (!record) {
		object_unref(&package->base);
		return -1;
	}
	WCHAR *text = format_record(package, record);
	INT answer = 1;
	if (package->remote)
		remote_call(package->remote, REMOTE_MESSAGE, text, NULL, type, NULL, &answer);
	else
		package_message(package, type, text);
	msi_free(text);
	object_unref(&record->base);
	object_unref(&package->base);
	return answer;
}

MSIHANDLE WINAPI MsiGetLastErrorRecord(void)
{
	return 0;
}

INSTALLUILEVEL ui_internal_level(void)
{
	return ui_level;
}

INSTALLUILEVEL WINAPI MsiSetInternalUI(INSTALLUILEVEL level, HWND *window)
{
	INSTALLUILEVEL old = ui_level;
	if (level != INSTALLUILEVEL_NOCHANGE)
		ui_level = level;
	if (window) {
		HWND previous = ui_window;
		ui_window = *window;
		*window = previous;
	}
	return old;
}

INSTALLUI_HANDLERW WINAPI MsiSetExternalUIW(INSTALLUI_HANDLERW handler, DWORD filter, LPVOID context)
{
	EnterCriticalSection(&ui_lock);
	INSTALLUI_HANDLERW old = ui_handler_w;
	ui_handler_w = handler;
	ui_handler_a = NULL;
	ui_filter = filter;
	ui_context = context;
	LeaveCriticalSection(&ui_lock);
	return old;
}

INSTALLUI_HANDLERA WINAPI MsiSetExternalUIA(INSTALLUI_HANDLERA handler, DWORD filter, LPVOID context)
{
	EnterCriticalSection(&ui_lock);
	INSTALLUI_HANDLERA old = ui_handler_a;
	ui_handler_a = handler;
	ui_handler_w = NULL;
	ui_filter = filter;
	ui_context = context;
	LeaveCriticalSection(&ui_lock);
	return old;
}

UINT WINAPI MsiSetExternalUIRecord(INSTALLUI_HANDLER_RECORD handler, DWORD filter, LPVOID context, PINSTALLUI_HANDLER_RECORD previous)
{
	EnterCriticalSection(&ui_lock);
	if (previous)
		*previous = ui_handler_record;
	ui_handler_record = handler;
	ui_filter = filter;
	ui_context = context;
	LeaveCriticalSection(&ui_lock);
	return ERROR_SUCCESS;
}

UINT WINAPI MsiEnableLogW(DWORD mode, LPCWSTR path, DWORD attributes)
{
	if (log_file != INVALID_HANDLE_VALUE) {
		CloseHandle(log_file);
		log_file = INVALID_HANDLE_VALUE;
	}
	log_mode = mode;
	log_path[0] = 0;
	if (!path || !mode)
		return ERROR_SUCCESS;
	lstrcpynW(log_path, path, MAX_PATH);
	log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, attributes & INSTALLLOGATTRIBUTES_APPEND ? OPEN_ALWAYS : CREATE_ALWAYS, 0, NULL);
	if (log_file == INVALID_HANDLE_VALUE)
		return ERROR_INVALID_PARAMETER;
	if (attributes & INSTALLLOGATTRIBUTES_APPEND)
		SetFilePointer(log_file, 0, NULL, FILE_END);
	return ERROR_SUCCESS;
}

// The log's path for another process to write it, which needs this one to let go of the file.
const WCHAR *package_log_release(void)
{
	if (log_file != INVALID_HANDLE_VALUE) {
		CloseHandle(log_file);
		log_file = INVALID_HANDLE_VALUE;
	}
	return log_path;
}

UINT WINAPI MsiEnableLogA(DWORD mode, LPCSTR path, DWORD attributes)
{
	WCHAR *wide = path ? msi_from_ansi(path) : NULL;
	UINT status = MsiEnableLogW(mode, wide, attributes);
	msi_free(wide);
	return status;
}
