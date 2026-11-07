/* where.exe: WHERE [/R dir] [/Q] [/F] [/T] pattern... — the files a pattern names, in the current folder and the folders of PATH, or
   under a folder with /R; $VAR:pattern searches the folders of a variable and dir:pattern one folder. A pattern without an extension
   also tries those of PATHEXT. ERRORLEVEL is 0 when every pattern was found, 1 otherwise, 2 on an error. */

#include "tool.h"

static BOOL quiet, quoted_names, details;
static DWORD found_total;

static void report(const WCHAR *path, const WIN32_FIND_DATAW *data)
{
	if (quiet)
		return;
	struct Text line = {0};
	if (details) {
		const ULONGLONG size = ((ULONGLONG)data->nFileSizeHigh << 32) | data->nFileSizeLow;
		WCHAR digits[32];
		struct Text number = {0};
		put_number(&number, size, FALSE);
		put_field(&line, number.data, 10, FALSE);
		release(number.data);
		FILETIME local;
		SYSTEMTIME parts;
		FileTimeToLocalFileTime(&data->ftLastWriteTime, &local);
		FileTimeToSystemTime(&local, &parts);
		text_char(&line, L' ');
		struct Text date = {0};
		put_number(&date, parts.wMonth, FALSE);
		text_char(&date, L'/');
		put_number(&date, parts.wDay, FALSE);
		text_char(&date, L'/');
		put_number(&date, parts.wYear, FALSE);
		put_field(&line, date.data, 10, FALSE);
		release(date.data);
		text_put(&line, L"  ");
		const DWORD hour = parts.wHour % 12 ? parts.wHour % 12 : 12;
		digits[0] = (WCHAR)(L'0' + hour / 10);
		digits[1] = (WCHAR)(L'0' + hour % 10);
		digits[2] = L':';
		digits[3] = (WCHAR)(L'0' + parts.wMinute / 10);
		digits[4] = (WCHAR)(L'0' + parts.wMinute % 10);
		digits[5] = L':';
		digits[6] = (WCHAR)(L'0' + parts.wSecond / 10);
		digits[7] = (WCHAR)(L'0' + parts.wSecond % 10);
		digits[8] = 0;
		text_put(&line, digits);
		text_put(&line, parts.wHour < 12 ? L" AM  " : L" PM  ");
	}
	if (quoted_names)
		text_char(&line, L'"');
	text_put(&line, path);
	if (quoted_names)
		text_char(&line, L'"');
	text_put(&line, L"\r\n");
	say_text(&line);
	release(line.data);
}

static BOOL has_extension(const WCHAR *name)
{
	const WCHAR *dot = NULL;
	for (const WCHAR *at = name; *at; ++at)
		if (*at == L'.')
			dot = at;
	return dot != NULL;
}

/* The files in one folder that a pattern matches, with PATHEXT's extensions too when it has none. */
static DWORD search_folder(const WCHAR *folder, const WCHAR *pattern)
{
	DWORD found = 0;
	struct Text patterns = {0};
	text_put(&patterns, pattern);
	if (!has_extension(pattern)) {
		WCHAR extensions[1024];
		if (!GetEnvironmentVariableW(L"PATHEXT", extensions, 1024))
			lstrcpynW(extensions, L".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC", 1024);
		for (WCHAR *at = extensions; *at;) {
			WCHAR *end = at;
			while (*end && *end != L';')
				++end;
			text_char(&patterns, L';');
			text_put(&patterns, pattern);
			text_add(&patterns, at, (DWORD)(end - at));
			at = *end ? end + 1 : end;
		}
	}
	/* Each name once, in the order the patterns give them. */
	struct Text seen = {0};
	for (WCHAR *at = patterns.data; *at;) {
		WCHAR *end = at;
		while (*end && *end != L';')
			++end;
		WCHAR *one = duplicate(at, (DWORD)(end - at));
		WCHAR *search = joined(folder, one);
		WIN32_FIND_DATAW data;
		HANDLE handle = FindFirstFileW(search, &data);
		if (handle != INVALID_HANDLE_VALUE) {
			do {
				if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !matches(one, data.cFileName))
					continue;
				struct Text key = {0};
				text_char(&key, L'|');
				text_put(&key, data.cFileName);
				text_char(&key, L'|');
				BOOL repeated = FALSE;
				for (WCHAR *probe = seen.data; probe && *probe && !repeated; ++probe)
					repeated = starts(probe, key.data);
				if (!repeated) {
					text_put(&seen, key.data);
					WCHAR *path = joined(folder, data.cFileName);
					report(path, &data);
					release(path);
					++found;
				}
				release(key.data);
			} while (FindNextFileW(handle, &data));
			FindClose(handle);
		}
		release(search);
		release(one);
		at = *end ? end + 1 : end;
	}
	release(seen.data);
	release(patterns.data);
	return found;
}

static DWORD search_tree(const WCHAR *folder, const WCHAR *pattern)
{
	DWORD found = search_folder(folder, pattern);
	WCHAR *all = joined(folder, L"*");
	WIN32_FIND_DATAW data;
	HANDLE handle = FindFirstFileW(all, &data);
	release(all);
	if (handle == INVALID_HANDLE_VALUE)
		return found;
	do {
		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
			equal(data.cFileName, L".") || equal(data.cFileName, L".."))
			continue;
		WCHAR *sub = joined(folder, data.cFileName);
		found += search_tree(sub, pattern);
		release(sub);
	} while (FindNextFileW(handle, &data));
	FindClose(handle);
	return found;
}

/* A list of folders separated by semicolons. */
static DWORD search_list(const WCHAR *list, const WCHAR *pattern)
{
	DWORD found = 0;
	WCHAR *copy = copy_of(list);
	for (WCHAR *at = copy; *at;) {
		WCHAR *end = at;
		while (*end && *end != L';')
			++end;
		const WCHAR saved = *end;
		*end = 0;
		struct Text folder = {0};
		for (WCHAR *character = at; *character; ++character)
			if (*character != L'"')
				text_char(&folder, *character);
		if (folder.size)
			found += search_folder(folder.data, pattern);
		release(folder.data);
		*end = saved;
		at = *end ? end + 1 : end;
	}
	release(copy);
	return found;
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	const WCHAR *root = NULL;
	DWORD patterns = 0;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (switch_is(argument, L"/R")) {
			if (index + 1 >= arguments.count) {
				complain(L"ERROR: Invalid syntax. Value expected for '/r'.\r\nType \"WHERE /?\" for usage.\r\n");
				ExitProcess(2);
			}
			root = arguments.items[++index];
		} else if (switch_is(argument, L"/Q"))
			quiet = TRUE;
		else if (switch_is(argument, L"/F"))
			quoted_names = TRUE;
		else if (switch_is(argument, L"/T"))
			details = TRUE;
		else if (switch_is(argument, L"/?")) {
			say(L"WHERE [/R dir] [/Q] [/F] [/T] pattern...\r\n\r\n"
				L"Displays the location of files that match the search pattern.\r\n"
				L"By default, the search is done along the current directory and\r\nin the paths specified by the PATH environment variable.\r\n");
			ExitProcess(0);
		} else if (is_switch(argument)) {
			complain(L"ERROR: Invalid argument or option - '");
			complain(argument);
			complain(L"'.\r\nType \"WHERE /?\" for usage.\r\n");
			ExitProcess(2);
		} else
			++patterns;
	}
	if (!patterns) {
		complain(L"ERROR: A search pattern must be specified.\r\nType \"WHERE /?\" for usage.\r\n");
		ExitProcess(2);
	}
	BOOL all_found = TRUE;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (is_switch(argument) || (index && root == argument && switch_is(arguments.items[index - 1], L"/R")))
			continue;
		DWORD found = 0;
		const WCHAR *colon = argument;
		while (*colon && *colon != L':')
			++colon;
		if (root) {
			WCHAR *full = full_path(root);
			found = search_tree(full, argument);
			release(full);
		} else if (argument[0] == L'$' && *colon) {
			WCHAR *name = duplicate(argument + 1, (DWORD)(colon - argument - 1));
			WCHAR *list = allocate(32768 * sizeof(WCHAR));
			if (!GetEnvironmentVariableW(name, list, 32768)) {
				complain(L"ERROR: The variable \"");
				complain(name);
				complain(L"\" cannot be found.\r\n");
				ExitProcess(2);
			}
			found = search_list(list, colon + 1);
			release(list);
			release(name);
		} else if (*colon && colon - argument > 1) {
			WCHAR *list = duplicate(argument, (DWORD)(colon - argument));
			found = search_list(list, colon + 1);
			release(list);
		} else {
			WCHAR folder[MAX_PATH];
			GetCurrentDirectoryW(MAX_PATH, folder);
			found = search_folder(folder, argument);
			WCHAR *list = allocate(32768 * sizeof(WCHAR));
			if (GetEnvironmentVariableW(L"PATH", list, 32768))
				found += search_list(list, argument);
			release(list);
		}
		found_total += found;
		if (!found)
			all_found = FALSE;
	}
	if (!all_found && !quiet)
		complain(L"INFO: Could not find files for the given pattern(s).\r\n");
	ExitProcess(all_found ? 0 : 1);
	return 0;
}
