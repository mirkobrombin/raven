/* xcopy.exe: XCOPY source [destination] [/S [/E]] [/I] [/Q] [/F] [/L] [/Y | /-Y] [/H] [/R] [/K] [/C] [/D[:m-d-y]] [/T] [/U]
   [/EXCLUDE:file1[+file2]] [/A | /M] [/P] [/W] [/V] [/N] [/O] [/X] [/Z] [/B] [/J] — files and folder trees copied, as XCOPY /?
   describes. ERRORLEVEL is 0 when files were copied, 1 when none were found, 4 on an error or bad syntax, 5 on a write error. */

#include "tool.h"

static BOOL tree, empty_too, assume_folder, quiet, full_names, list_only, overwrite_all, hidden_too, read_only_too, keep_attributes,
	keep_going, newer_only, folders_only, existing_only, archive_only, reset_archive, prompt_each, wait_first;
static BOOL since_given;
static ULONGLONG since;
static DWORD copied;
static BOOL write_failed;
static WCHAR **excluded;
static DWORD excluded_count;

static ULONGLONG stamp(const FILETIME *time) { return ((ULONGLONG)time->dwHighDateTime << 32) | time->dwLowDateTime; }

/* Asks a question with the console, or reads the answer from the input; the first letter, folded. */
static WCHAR ask(const WCHAR *question)
{
	say(question);
	HANDLE keys = console_input();
	WCHAR answer = 0;
	if (keys) {
		WCHAR line[64];
		DWORD count = 0;
		if (ReadConsoleW(keys, line, 64, &count, NULL) && count)
			answer = folded(line[0]);
	} else {
		char byte = 0;
		DWORD count = 0;
		while (ReadFile(input, &byte, 1, &count, NULL) && count && (byte == ' ' || byte == '\r' || byte == '\n'))
			;
		if (count)
			answer = folded((WCHAR)byte);
		if (count)
			say((WCHAR[]){answer, L'\r', L'\n', 0});
	}
	return answer;
}

static BOOL is_excluded(const WCHAR *path)
{
	for (DWORD index = 0; index < excluded_count; ++index) {
		const WCHAR *needle = excluded[index];
		const DWORD size = length(needle);
		for (const WCHAR *at = path; *at; ++at) {
			DWORD matched = 0;
			while (matched < size && at[matched] && folded(at[matched]) == folded(needle[matched]))
				++matched;
			if (matched == size)
				return TRUE;
		}
	}
	return FALSE;
}

static void read_exclusions(const WCHAR *list)
{
	for (const WCHAR *at = list; *at;) {
		const WCHAR *end = at;
		while (*end && *end != L'+')
			++end;
		WCHAR *name = duplicate(at, (DWORD)(end - at));
		DWORD size, position = 0, start, line_size;
		WCHAR *text = read_file_text(name, &size);
		if (!text) {
			complain(L"Can't read file: ");
			complain(name);
			complain(L"\r\n");
			ExitProcess(4);
		}
		while (next_line(text, size, &position, &start, &line_size))
			if (line_size) {
				WCHAR **grown = allocate((excluded_count + 1) * sizeof(WCHAR *));
				if (excluded_count)
					memcpy(grown, excluded, excluded_count * sizeof(WCHAR *));
				release(excluded);
				excluded = grown;
				excluded[excluded_count++] = duplicate(text + start, line_size);
			}
		release(text);
		release(name);
		at = *end ? end + 1 : end;
	}
}

static void make_folders(const WCHAR *path)
{
	WCHAR *copy = copy_of(path);
	for (DWORD at = 3; copy[at]; ++at)
		if (copy[at] == L'\\') {
			copy[at] = 0;
			CreateDirectoryW(copy, NULL);
			copy[at] = L'\\';
		}
	CreateDirectoryW(copy, NULL);
	release(copy);
}

/* Copies one file; FALSE stops the copy, after an error without /C. */
static BOOL copy_one(const WCHAR *from, const WCHAR *to, const WCHAR *shown, const WIN32_FIND_DATAW *data)
{
	if (is_excluded(from))
		return TRUE;
	if (archive_only && !(data->dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE))
		return TRUE;
	WIN32_FIND_DATAW existing;
	HANDLE handle = FindFirstFileW(to, &existing);
	const BOOL exists = handle != INVALID_HANDLE_VALUE;
	if (exists)
		FindClose(handle);
	if (existing_only && !exists)
		return TRUE;
	if (newer_only && exists && stamp(&existing.ftLastWriteTime) >= stamp(&data->ftLastWriteTime))
		return TRUE;
	if (since_given && stamp(&data->ftLastWriteTime) < since)
		return TRUE;
	if (prompt_each) {
		struct Text question = {0};
		text_put(&question, shown);
		text_put(&question, L" (Y/N)? ");
		const WCHAR answer = ask(question.data);
		release(question.data);
		if (answer != L'Y')
			return TRUE;
	}
	if (exists && !overwrite_all && !list_only) {
		struct Text question = {0};
		text_put(&question, L"Overwrite ");
		text_put(&question, to);
		text_put(&question, L" (Yes/No/All)? ");
		const WCHAR answer = ask(question.data);
		release(question.data);
		if (answer == L'A')
			overwrite_all = TRUE;
		else if (answer != L'Y')
			return TRUE;
	}
	if (!quiet) {
		say(shown);
		if (full_names) {
			say(L" -> ");
			say(to);
		}
		say(L"\r\n");
	}
	if (list_only) {
		++copied;
		return TRUE;
	}
	if (exists && (existing.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) {
		if (!read_only_too) {
			complain(L"Access denied\r\n");
			write_failed = TRUE;
			return keep_going;
		}
		SetFileAttributesW(to, existing.dwFileAttributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
	}
	if (!CopyFileW(from, to, FALSE)) {
		complain(GetLastError() == ERROR_ACCESS_DENIED ? L"Access denied\r\n" : L"File creation error - The system cannot find the path specified.\r\n");
		write_failed = TRUE;
		return keep_going;
	}
	/* The copy is marked to be archived and loses read-only, unless /K keeps its attributes. */
	if (!keep_attributes)
		SetFileAttributesW(to, (data->dwFileAttributes & ~(DWORD)FILE_ATTRIBUTE_READONLY) | FILE_ATTRIBUTE_ARCHIVE);
	if (reset_archive)
		SetFileAttributesW(from, data->dwFileAttributes & ~(DWORD)FILE_ATTRIBUTE_ARCHIVE);
	++copied;
	return TRUE;
}

/* The files of a folder that match the pattern, then with /S the folders under it. `shown` is the folder as the source names it. */
static BOOL copy_folder(const WCHAR *folder, const WCHAR *shown, const WCHAR *pattern, const WCHAR *target)
{
	WIN32_FIND_DATAW data;
	WCHAR *search = joined(folder, pattern);
	HANDLE handle = FindFirstFileW(search, &data);
	release(search);
	BOOL any = FALSE;
	if (handle != INVALID_HANDLE_VALUE) {
		do {
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			if (!hidden_too && (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
				continue;
			if (!any && !folders_only && !list_only)
				make_folders(target);
			any = TRUE;
			if (folders_only)
				continue;
			WCHAR *from = joined(folder, data.cFileName), *to = joined(target, data.cFileName), *named = joined(shown, data.cFileName);
			const BOOL go_on = copy_one(from, to, named, &data);
			release(from);
			release(to);
			release(named);
			if (!go_on) {
				FindClose(handle);
				return FALSE;
			}
		} while (FindNextFileW(handle, &data));
		FindClose(handle);
	}
	if (!tree)
		return TRUE;
	WCHAR *all = joined(folder, L"*");
	handle = FindFirstFileW(all, &data);
	release(all);
	if (handle == INVALID_HANDLE_VALUE)
		return TRUE;
	do {
		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || equal(data.cFileName, L".") || equal(data.cFileName, L".."))
			continue;
		if (!hidden_too && (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
			continue;
		WCHAR *sub = joined(folder, data.cFileName), *sub_target = joined(target, data.cFileName), *sub_shown = joined(shown, data.cFileName);
		if (is_excluded(sub)) {
			release(sub);
			release(sub_target);
			release(sub_shown);
			continue;
		}
		/* /E copies a folder even when nothing in it is copied; /T copies only folders. */
		if ((empty_too || folders_only) && !list_only)
			make_folders(sub_target);
		const BOOL go_on = copy_folder(sub, sub_shown, pattern, sub_target);
		release(sub);
		release(sub_target);
		release(sub_shown);
		if (!go_on) {
			FindClose(handle);
			return FALSE;
		}
	} while (FindNextFileW(handle, &data));
	FindClose(handle);
	return TRUE;
}

/* /D:m-d-y: files changed on or after that date. */
static BOOL read_date(const WCHAR *text)
{
	DWORD parts[3] = {0, 0, 0}, part = 0;
	for (; *text && part < 3; ++text) {
		if (digit(*text))
			parts[part] = parts[part] * 10 + (DWORD)(*text - L'0');
		else if (*text == L'-' || *text == L'/')
			++part;
		else
			return FALSE;
	}
	if (parts[2] < 100)
		parts[2] += 2000;
	SYSTEMTIME day;
	memset(&day, 0, sizeof day);
	day.wMonth = (WORD)parts[0];
	day.wDay = (WORD)parts[1];
	day.wYear = (WORD)parts[2];
	FILETIME local, universal;
	if (!SystemTimeToFileTime(&day, &local) || !LocalFileTimeToFileTime(&local, &universal))
		return FALSE;
	since = stamp(&universal);
	since_given = TRUE;
	return TRUE;
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	const WCHAR *source = NULL, *destination = NULL;
	WCHAR defaults[64];
	if (GetEnvironmentVariableW(L"COPYCMD", defaults, 64) && equal(defaults, L"/Y"))
		overwrite_all = TRUE;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (!is_switch(argument)) {
			if (!source)
				source = argument;
			else if (!destination)
				destination = argument;
			else {
				complain(L"Invalid number of parameters\r\n");
				ExitProcess(4);
			}
			continue;
		}
		const WCHAR *name = argument + 1;
		switch (folded(name[0])) {
		case L'S': tree = TRUE; break;
		case L'E': if (starts(name, L"EXCLUDE:")) read_exclusions(name + 8); else tree = empty_too = TRUE; break;
		case L'I': assume_folder = TRUE; break;
		case L'Q': quiet = TRUE; break;
		case L'F': full_names = TRUE; break;
		case L'L': list_only = TRUE; break;
		case L'Y': overwrite_all = TRUE; break;
		case L'-': if (folded(name[1]) == L'Y') overwrite_all = FALSE; break;
		case L'H': hidden_too = TRUE; break;
		case L'R': read_only_too = TRUE; break;
		case L'K': keep_attributes = TRUE; break;
		case L'C': keep_going = TRUE; break;
		case L'D':
			if (name[1] == L':') {
				if (!read_date(name + 2)) {
					complain(L"Invalid parameter - ");
					complain(argument);
					complain(L"\r\n");
					ExitProcess(4);
				}
			} else
				newer_only = TRUE;
			break;
		case L'T': folders_only = TRUE; break;
		case L'U': existing_only = TRUE; break;
		case L'A': archive_only = TRUE; break;
		case L'M': archive_only = reset_archive = TRUE; break;
		case L'P': prompt_each = TRUE; break;
		case L'W': wait_first = TRUE; break;
		case L'V': case L'N': case L'O': case L'X': case L'Z': case L'B': case L'J': case L'G': break;
		case L'?':
			say(L"Copies files and directory trees.\r\n\r\n"
				L"XCOPY source [destination] [/A | /M] [/D[:date]] [/P] [/S [/E]] [/V] [/W]\r\n"
				L"                           [/C] [/I] [/Q] [/F] [/L] [/G] [/H] [/R] [/T] [/U]\r\n"
				L"                           [/K] [/N] [/O] [/X] [/Y] [/-Y] [/Z] [/B] [/J]\r\n"
				L"                           [/EXCLUDE:file1[+file2][+file3]...]\r\n");
			ExitProcess(0);
			break;
		default:
			complain(L"Invalid switch - ");
			complain(argument);
			complain(L"\r\n");
			ExitProcess(4);
		}
	}
	if (!source) {
		complain(L"Invalid number of parameters\r\n");
		ExitProcess(4);
	}
	if (wait_first) {
		say(L"Press any key when ready to begin copying file(s)");
		HANDLE keys = console_input();
		if (keys)
			read_key(keys, NULL);
		say(L"\r\n");
	}
	WCHAR *source_full = full_path(source), *folder, *pattern;
	BOOL source_is_folder = !wild(source_full) && is_folder(source_full);
	if (source_is_folder) {
		folder = copy_of(source_full);
		pattern = copy_of(L"*");
	} else
		split_path(source_full, &folder, &pattern);
	if (GetFileAttributesW(folder) == INVALID_FILE_ATTRIBUTES) {
		complain(L"File not found - ");
		complain(pattern);
		complain(L"\r\n0 File(s) copied\r\n");
		ExitProcess(4);
	}
	WCHAR *target = full_path(destination ? destination : L".");
	/* A destination that does not exist is a folder when the source is several files or a folder, with /I; otherwise it is asked. */
	const DWORD target_attributes = GetFileAttributesW(target);
	BOOL target_folder = target_attributes != INVALID_FILE_ATTRIBUTES && (target_attributes & FILE_ATTRIBUTE_DIRECTORY);
	const DWORD given = destination ? length(destination) : 0;
	if (given && (destination[given - 1] == L'\\' || destination[given - 1] == L'/'))
		target_folder = TRUE;
	if (target_attributes == INVALID_FILE_ATTRIBUTES && !target_folder) {
		if (assume_folder && (source_is_folder || wild(pattern) || tree))
			target_folder = TRUE;
		else {
			struct Text question = {0};
			text_put(&question, L"Does ");
			text_put(&question, destination);
			text_put(&question, L" specify a file name\r\nor directory name on the target\r\n(F = file, D = directory)? ");
			WCHAR answer;
			do
				answer = ask(question.data);
			while (answer && answer != L'F' && answer != L'D');
			release(question.data);
			if (!answer)
				ExitProcess(4);
			target_folder = answer == L'D';
		}
	}
	/* The names XCOPY shows keep the source as it was given. */
	struct Text shown = {0};
	if (source_is_folder)
		text_put(&shown, source);
	else {
		DWORD prefix = length(source);
		while (prefix && source[prefix - 1] != L'\\' && source[prefix - 1] != L'/' && source[prefix - 1] != L':')
			--prefix;
		text_add(&shown, source, prefix);
		while (shown.size > 1 && (shown.data[shown.size - 1] == L'\\' || shown.data[shown.size - 1] == L'/') &&
			!(shown.size == 3 && shown.data[1] == L':'))
			shown.data[--shown.size] = 0;
	}
	if (target_folder)
		copy_folder(folder, shown.data, pattern, target);
	else {
		/* One file onto a file name. */
		WIN32_FIND_DATAW data;
		WCHAR *search = joined(folder, pattern);
		HANDLE handle = FindFirstFileW(search, &data);
		release(search);
		if (handle != INVALID_HANDLE_VALUE) {
			do {
				if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;
				WCHAR *from = joined(folder, data.cFileName), *named = joined(shown.data, data.cFileName);
				WCHAR *target_folder_path, *target_name;
				split_path(target, &target_folder_path, &target_name);
				make_folders(target_folder_path);
				const BOOL go_on = copy_one(from, target, named, &data);
				release(target_folder_path);
				release(target_name);
				release(from);
				release(named);
				if (!go_on)
					break;
			} while (FindNextFileW(handle, &data));
			FindClose(handle);
		}
	}
	struct Text summary = {0};
	put_number(&summary, copied, FALSE);
	text_put(&summary, list_only ? L" File(s)\r\n" : L" File(s) copied\r\n");
	if (!copied && !folders_only) {
		complain(L"File not found - ");
		complain(pattern);
		complain(L"\r\n");
	}
	say_text(&summary);
	ExitProcess(write_failed ? 5 : copied || folders_only ? 0 : 1);
	return 0;
}
