/* attrib.exe: ATTRIB [+R | -R] [+A | -A] [+S | -S] [+H | -H] [+I | -I] [+O | -O] [drive:][path][filename] [/S [/D]] [/L] — shows or
   changes the attributes of files, and with /D of folders too. */

#include "tool.h"

static DWORD to_set, to_clear;
static BOOL recursive, folders_too;
static BOOL found_any;

static DWORD attribute_of(WCHAR letter)
{
	switch (folded(letter)) {
	case L'R': return FILE_ATTRIBUTE_READONLY;
	case L'A': return FILE_ATTRIBUTE_ARCHIVE;
	case L'S': return FILE_ATTRIBUTE_SYSTEM;
	case L'H': return FILE_ATTRIBUTE_HIDDEN;
	case L'I': return FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
	case L'O': return FILE_ATTRIBUTE_OFFLINE;
	case L'P': return 0x80000;
	case L'U': return 0x100000;
	case L'X': return 0x20000;
	case L'V': return FILE_ATTRIBUTE_INTEGRITY_STREAM;
	default: return 0;
	}
}

/* The letters ATTRIB shows, each in its own column before the path. */
static void show(const WCHAR *path, DWORD attributes)
{
	struct Text line = {0};
	static const struct {
		DWORD bit;
		WCHAR letter;
	} columns[] = {{FILE_ATTRIBUTE_ARCHIVE, L'A'}, {0, L' '}, {0, L' '}, {FILE_ATTRIBUTE_SYSTEM, L'S'}, {FILE_ATTRIBUTE_HIDDEN, L'H'},
		{FILE_ATTRIBUTE_READONLY, L'R'}, {FILE_ATTRIBUTE_OFFLINE, L'O'}, {0, L' '}, {FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, L'I'}, {0x20000, L'X'},
		{0, L' '}, {0x80000, L'P'}, {0x100000, L'U'}};
	for (DWORD index = 0; index < sizeof columns / sizeof columns[0]; ++index)
		text_char(&line, columns[index].bit && (attributes & columns[index].bit) ? columns[index].letter : L' ');
	text_put(&line, L"        ");
	text_put(&line, path);
	text_put(&line, L"\r\n");
	say_text(&line);
	release(line.data);
}

static void apply(const WCHAR *path, DWORD attributes)
{
	found_any = TRUE;
	if (!to_set && !to_clear) {
		show(path, attributes);
		return;
	}
	/* A hidden or system file keeps those unless the change names them, as ATTRIB asks. */
	if ((attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) && !((to_set | to_clear) & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
		say(attributes & FILE_ATTRIBUTE_SYSTEM ? L"Not resetting system file - " : L"Not resetting hidden file - ");
		say(path);
		say(L"\r\n");
		return;
	}
	DWORD changed = (attributes | to_set) & ~to_clear & ~(DWORD)FILE_ATTRIBUTE_DIRECTORY;
	if (!changed)
		changed = FILE_ATTRIBUTE_NORMAL;
	if (!SetFileAttributesW(path, changed)) {
		complain(GetLastError() == ERROR_ACCESS_DENIED ? L"Access denied - " : L"File not found - ");
		complain(path);
		complain(L"\r\n");
	}
}

static void visit(const WCHAR *folder, const WCHAR *pattern)
{
	WIN32_FIND_DATAW data;
	WCHAR *search = joined(folder, pattern);
	HANDLE handle = FindFirstFileW(search, &data);
	release(search);
	if (handle != INVALID_HANDLE_VALUE) {
		do {
			if (equal(data.cFileName, L".") || equal(data.cFileName, L".."))
				continue;
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !folders_too)
				continue;
			WCHAR *path = joined(folder, data.cFileName);
			apply(path, data.dwFileAttributes);
			release(path);
		} while (FindNextFileW(handle, &data));
		FindClose(handle);
	}
	if (!recursive)
		return;
	WCHAR *all = joined(folder, L"*");
	handle = FindFirstFileW(all, &data);
	release(all);
	if (handle == INVALID_HANDLE_VALUE)
		return;
	do {
		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
			equal(data.cFileName, L".") || equal(data.cFileName, L".."))
			continue;
		WCHAR *sub = joined(folder, data.cFileName);
		visit(sub, pattern);
		release(sub);
	} while (FindNextFileW(handle, &data));
	FindClose(handle);
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	WCHAR **names = allocate((arguments.count + 1) * sizeof(WCHAR *));
	DWORD name_count = 0;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if ((argument[0] == L'+' || argument[0] == L'-') && argument[1] && !argument[2] && attribute_of(argument[1])) {
			if (argument[0] == L'+')
				to_set |= attribute_of(argument[1]);
			else
				to_clear |= attribute_of(argument[1]);
		} else if (switch_is(argument, L"/S"))
			recursive = TRUE;
		else if (switch_is(argument, L"/D"))
			folders_too = TRUE;
		else if (switch_is(argument, L"/L"))
			;
		else if (switch_is(argument, L"/?")) {
			say(L"Displays or changes file attributes.\r\n\r\n"
				L"ATTRIB [+R | -R] [+A | -A] [+S | -S] [+H | -H] [+O | -O] [+I | -I] [+X | -X] [+P | -P] [+U | -U]\r\n"
				L"       [drive:][path][filename] [/S [/D]] [/L]\r\n");
			ExitProcess(0);
		} else if (is_switch(argument) || argument[0] == L'+') {
			complain(L"Parameter format not correct - ");
			complain(argument);
			complain(L"\r\n");
			ExitProcess(1);
		} else
			names[name_count++] = arguments.items[index];
	}
	if (!name_count)
		names[name_count++] = L"*";
	DWORD status = 0;
	for (DWORD index = 0; index < name_count; ++index) {
		WCHAR *full = full_path(names[index]), *folder, *pattern;
		found_any = FALSE;
		/* A folder named by itself is shown, or changed, as a folder. */
		if (!wild(full) && is_folder(full) && !recursive) {
			apply(full, GetFileAttributesW(full));
			release(full);
			continue;
		}
		split_path(full, &folder, &pattern);
		visit(folder, pattern);
		if (!found_any) {
			complain(L"File not found - ");
			complain(full);
			complain(L"\r\n");
			status = 1;
		}
		release(full);
		release(folder);
		release(pattern);
	}
	ExitProcess(status);
	return 0;
}
