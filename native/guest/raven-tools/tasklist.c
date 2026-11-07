/* tasklist.exe: TASKLIST [/FI filter] [/FO TABLE | LIST | CSV] [/NH] [/V] — the running processes, with their PID, session and the
   memory they use. */

#include "tool.h"
#include "processes.h"

static void memory_text(struct Text *text, ULONGLONG bytes)
{
	put_number(text, bytes / 1024, TRUE);
	text_put(text, L" K");
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	struct Filter filters[16];
	DWORD filter_count = 0;
	BOOL no_header = FALSE;
	const WCHAR *format = L"TABLE";
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		const BOOL value = index + 1 < arguments.count;
		if (switch_is(argument, L"/FI") && value) {
			if (filter_count == 16 || !read_filter(arguments.items[++index], &filters[filter_count])) {
				complain(L"ERROR: The search filter cannot be recognized.\r\n");
				ExitProcess(1);
			}
			++filter_count;
		} else if (switch_is(argument, L"/FO") && value) {
			format = arguments.items[++index];
			if (!equal(format, L"TABLE") && !equal(format, L"LIST") && !equal(format, L"CSV")) {
				complain(L"ERROR: Invalid argument/option - '");
				complain(format);
				complain(L"'.\r\nType \"TASKLIST /?\" for usage.\r\n");
				ExitProcess(1);
			}
		} else if (switch_is(argument, L"/NH"))
			no_header = TRUE;
		else if (switch_is(argument, L"/V") || switch_is(argument, L"/SVC") || switch_is(argument, L"/APPS"))
			;
		else if ((switch_is(argument, L"/S") || switch_is(argument, L"/U") || switch_is(argument, L"/P") || switch_is(argument, L"/M")) && value)
			++index;
		else if (switch_is(argument, L"/?")) {
			say(L"TASKLIST [/S system [/U username [/P [password]]]]\r\n"
				L"         [/M [module] | /SVC | /V] [/FI filter] [/FO format] [/NH]\r\n\r\n"
				L"Description:\r\n    This tool displays a list of currently running processes on\r\n    either a local or remote machine.\r\n");
			ExitProcess(0);
		} else {
			complain(L"ERROR: Invalid argument/option - '");
			complain(argument);
			complain(L"'.\r\nType \"TASKLIST /?\" for usage.\r\n");
			ExitProcess(1);
		}
	}
	struct Processes list;
	if (!list_processes(&list)) {
		complain(L"ERROR: The process list cannot be read.\r\n");
		ExitProcess(1);
	}
	struct Text out = {0};
	DWORD shown = 0;
	const BOOL table = equal(format, L"TABLE"), csv = equal(format, L"CSV");
	for (DWORD index = 0; index < list.count; ++index) {
		const struct Process *process = &list.items[index];
		BOOL wanted = TRUE;
		for (DWORD filter = 0; filter < filter_count && wanted; ++filter)
			wanted = passes(&filters[filter], process);
		if (!wanted)
			continue;
		if (!shown++ && !no_header) {
			if (table)
				text_put(&out, L"\r\nImage Name                     PID Session Name        Session#    Mem Usage\r\n"
					L"========================= ======== ================ =========== ============\r\n");
			else if (csv)
				text_put(&out, L"\"Image Name\",\"PID\",\"Session Name\",\"Session#\",\"Mem Usage\"\r\n");
		}
		struct Text id = {0}, memory = {0};
		put_number(&id, process->id, FALSE);
		memory_text(&memory, process->memory);
		if (table) {
			WCHAR name[26];
			lstrcpynW(name, process->name, 26);
			put_field(&out, name, 25, TRUE);
			text_char(&out, L' ');
			put_field(&out, id.data, 8, FALSE);
			text_put(&out, L" Console                    1 ");
			put_field(&out, memory.data, 12, FALSE);
			text_put(&out, L"\r\n");
		} else if (csv) {
			text_char(&out, L'"');
			text_put(&out, process->name);
			text_put(&out, L"\",\"");
			text_put(&out, id.data);
			text_put(&out, L"\",\"Console\",\"1\",\"");
			text_put(&out, memory.data);
			text_put(&out, L"\"\r\n");
		} else {
			text_put(&out, L"\r\nImage Name:   ");
			text_put(&out, process->name);
			text_put(&out, L"\r\nPID:          ");
			text_put(&out, id.data);
			text_put(&out, L"\r\nSession Name: Console\r\nSession#:     1\r\nMem Usage:    ");
			text_put(&out, memory.data);
			text_put(&out, L"\r\n");
		}
		release(id.data);
		release(memory.data);
		if (out.size > 8192)
			say_text(&out);
	}
	say_text(&out);
	if (!shown)
		say(L"INFO: No tasks are running which match the specified criteria.\r\n");
	ExitProcess(0);
	return 0;
}
