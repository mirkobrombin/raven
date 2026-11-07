/* find.exe: FIND [/V] [/C] [/N] [/I] [/OFF[LINE]] "string" [[drive:][path]filename[ ...]] — the lines of files, or of the input, that
   contain a string. ERRORLEVEL is 0 when a line matched, 1 when none did and 2 on an error. */

#include "tool.h"

static BOOL invert, count_only, numbered, fold;
static const WCHAR *wanted;
static DWORD wanted_size;

static BOOL contains(const WCHAR *line, DWORD size)
{
	/* An empty string is in no line, so FIND /V "" passes every line, as it is used to count them. */
	if (!wanted_size)
		return FALSE;
	for (DWORD at = 0; at + wanted_size <= size; ++at) {
		DWORD index = 0;
		while (index < wanted_size && (fold ? folded(line[at + index]) == folded(wanted[index]) : line[at + index] == wanted[index]))
			++index;
		if (index == wanted_size)
			return TRUE;
	}
	return FALSE;
}

/* Searches one text; with a name, under the ---------- heading FIND gives each file. */
static DWORD search(const WCHAR *text, DWORD size, const WCHAR *name)
{
	struct Text out = {0};
	DWORD at = 0, start, line_size, number = 0, found = 0;
	if (name) {
		text_put(&out, L"\r\n---------- ");
		for (const WCHAR *character = name; *character; ++character)
			text_char(&out, folded(*character));
		if (!count_only)
			text_put(&out, L"\r\n");
	}
	while (next_line(text, size, &at, &start, &line_size)) {
		++number;
		if (contains(text + start, line_size) == invert)
			continue;
		++found;
		if (count_only)
			continue;
		if (numbered) {
			text_char(&out, L'[');
			put_number(&out, number, FALSE);
			text_char(&out, L']');
		}
		text_add(&out, text + start, line_size);
		text_put(&out, L"\r\n");
		if (out.size > 8192)
			say_text(&out);
	}
	if (count_only) {
		text_put(&out, name ? L": " : L"");
		put_number(&out, found, FALSE);
		text_put(&out, L"\r\n");
	}
	say_text(&out);
	release(out.data);
	return found;
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	DWORD files = 0;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (switch_is(argument, L"/V"))
			invert = TRUE;
		else if (switch_is(argument, L"/C"))
			count_only = TRUE;
		else if (switch_is(argument, L"/N"))
			numbered = TRUE;
		else if (switch_is(argument, L"/I"))
			fold = TRUE;
		else if (switch_is(argument, L"/OFF") || switch_is(argument, L"/OFFLINE"))
			;
		else if (switch_is(argument, L"/?")) {
			say(L"Searches for a text string in a file or files.\r\n\r\n"
				L"FIND [/V] [/C] [/N] [/I] [/OFF[LINE]] \"string\" [[drive:][path]filename[ ...]]\r\n");
			ExitProcess(0);
		} else if (is_switch(argument)) {
			complain(L"FIND: Parameter format not correct\r\n");
			ExitProcess(2);
		} else if (!wanted) {
			wanted = argument;
			wanted_size = length(argument);
		} else
			++files;
	}
	if (!wanted) {
		complain(L"FIND: Parameter format not correct\r\n");
		ExitProcess(2);
	}
	DWORD found = 0, size;
	BOOL failed = FALSE;
	if (!files) {
		WCHAR *text = read_text(input, &size);
		found = search(text, size, NULL);
		release(text);
	}
	BOOL seen_wanted = FALSE;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (is_switch(argument))
			continue;
		if (!seen_wanted && argument == wanted) {
			seen_wanted = TRUE;
			continue;
		}
		WCHAR *full = full_path(argument), *folder, *pattern;
		split_path(full, &folder, &pattern);
		WIN32_FIND_DATAW data;
		HANDLE handle = FindFirstFileW(full, &data);
		if (handle == INVALID_HANDLE_VALUE) {
			complain(L"File not found - ");
			for (const WCHAR *character = argument; *character; ++character) {
				const WCHAR upper = folded(*character);
				write_to(error_output, &upper, 1);
			}
			complain(L"\r\n");
			failed = TRUE;
		} else {
			do {
				if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;
				WCHAR *path = joined(folder, data.cFileName);
				WCHAR *text = read_file_text(path, &size);
				if (text) {
					/* The heading names the file as it was given, with the found name for a pattern. */
					struct Text name = {0};
					DWORD prefix = length(argument);
					while (prefix && argument[prefix - 1] != L'\\' && argument[prefix - 1] != L'/' && argument[prefix - 1] != L':')
						--prefix;
					text_add(&name, argument, prefix);
					text_put(&name, data.cFileName);
					found += search(text, size, name.data);
					release(name.data);
					release(text);
				}
				release(path);
			} while (FindNextFileW(handle, &data));
			FindClose(handle);
		}
		release(full);
		release(folder);
		release(pattern);
	}
	ExitProcess(failed ? 2 : found ? 0 : 1);
	return 0;
}
