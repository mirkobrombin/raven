/* more.exe: MORE [/E [/C] [/P] [/S] [/Tn] [+n]] [files] or command | MORE — text a screen at a time on a console, with -- More --
   between screens: Space for the next screen, Enter for the next line, Q to stop. To a file or a pipe the text goes on as it is. */

#include "tool.h"

static BOOL clear_first, squeeze, form_feeds;
static DWORD tab_size = 8, skip_lines;

/* Shows a text, pausing each screen; FALSE when Q stopped it. */
static BOOL show(const WCHAR *text, DWORD size, HANDLE keys, DWORD rows, DWORD columns)
{
	DWORD at = 0, start, line_size, shown = 0, line_number = 0;
	DWORD page = rows > 1 ? rows - 1 : 24;
	BOOL last_blank = FALSE;
	struct Text out = {0};
	while (next_line(text, size, &at, &start, &line_size)) {
		if (line_number++ < skip_lines)
			continue;
		if (squeeze && !line_size) {
			if (last_blank)
				continue;
			last_blank = TRUE;
		} else
			last_blank = FALSE;
		struct Text line = {0};
		for (DWORD index = 0; index < line_size; ++index) {
			const WCHAR character = text[start + index];
			if (character == L'\t')
				do
					text_char(&line, L' ');
				while (line.size % tab_size);
			else if (character == 12 && form_feeds)
				shown = page;
			else
				text_char(&line, character);
		}
		/* A line wider than the console takes several of its rows. */
		DWORD taken = columns && line.size > columns ? (line.size + columns - 1) / columns : 1;
		if (keys && shown + taken > page && shown) {
			say_text(&out);
			say(L"-- More --");
			WORD code = 0;
			const WCHAR key = read_key(keys, &code);
			say(L"\r          \r");
			if (folded(key) == L'Q' || key == 3) {
				release(line.data);
				release(out.data);
				return FALSE;
			}
			shown = code == VK_RETURN || key == L'\r' ? page - 1 : 0;
		}
		text_put(&out, line.data);
		text_put(&out, L"\r\n");
		shown += taken;
		release(line.data);
		if (!keys && out.size > 8192)
			say_text(&out);
	}
	say_text(&out);
	release(out.data);
	return TRUE;
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	DWORD mode;
	CONSOLE_SCREEN_BUFFER_INFO info;
	DWORD rows = 25, columns = 80;
	const BOOL console = GetConsoleMode(output, &mode);
	if (console && GetConsoleScreenBufferInfo(output, &info)) {
		rows = (DWORD)(info.srWindow.Bottom - info.srWindow.Top + 1);
		columns = (DWORD)info.dwSize.X;
	}
	/* Keys come from the console, even when the text comes from a pipe. */
	HANDLE keys = console ? console_input() : NULL;
	DWORD files = 0;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (argument[0] == L'+' && digit(argument[1])) {
			ULONGLONG lines = 0;
			read_number(argument + 1, &lines);
			skip_lines = (DWORD)lines;
		} else if (switch_is(argument, L"/C"))
			clear_first = TRUE;
		else if (switch_is(argument, L"/S"))
			squeeze = TRUE;
		else if (switch_is(argument, L"/P"))
			form_feeds = TRUE;
		else if (switch_is(argument, L"/E"))
			;
		else if (is_switch(argument) && folded(argument[1]) == L'T' && digit(argument[2])) {
			ULONGLONG size = 8;
			read_number(argument + 2, &size);
			tab_size = size ? (DWORD)size : 8;
		} else if (switch_is(argument, L"/?")) {
			say(L"Displays output one screen at a time.\r\n\r\n"
				L"MORE [drive:][path]filename\r\ncommand-name | MORE [drive:][path][filename]\r\n"
				L"MORE /E [/C] [/P] [/S] [/Tn] [+n] [files]\r\n");
			ExitProcess(0);
		} else if (is_switch(argument)) {
			complain(L"Invalid switch - ");
			complain(argument);
			complain(L"\r\n");
			ExitProcess(1);
		} else
			++files;
	}
	if (clear_first && console)
		say(L"\x1b[2J\x1b[H");
	DWORD size;
	if (!files) {
		WCHAR *text = read_text(input, &size);
		show(text, size, keys, rows, columns);
		release(text);
		ExitProcess(0);
	}
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (is_switch(argument) || (argument[0] == L'+' && digit(argument[1])))
			continue;
		WCHAR *text = read_file_text(argument, &size);
		if (!text) {
			complain(L"Cannot access file ");
			WCHAR *full = full_path(argument);
			complain(full);
			release(full);
			complain(L"\r\n");
			ExitProcess(1);
		}
		const BOOL go_on = show(text, size, keys, rows, columns);
		release(text);
		if (!go_on)
			break;
	}
	ExitProcess(0);
	return 0;
}
