/* choice.exe: CHOICE [/C choices] [/N] [/CS] [/T timeout /D choice] [/M text] — asks for one of a set of keys; ERRORLEVEL is the
   position of the one pressed, 1 for the first, or 255 on an error. */

#include "tool.h"

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	const WCHAR *choices = L"YN", *message = NULL;
	BOOL hide = FALSE, case_sensitive = FALSE;
	LONGLONG timeout = -1;
	WCHAR fallback = 0;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		const BOOL value = index + 1 < arguments.count;
		if (switch_is(argument, L"/C") && value)
			choices = arguments.items[++index];
		else if (switch_is(argument, L"/N"))
			hide = TRUE;
		else if (switch_is(argument, L"/CS"))
			case_sensitive = TRUE;
		else if (switch_is(argument, L"/T") && value) {
			ULONGLONG seconds;
			if (!read_number(arguments.items[++index], &seconds) || seconds > 9999) {
				complain(L"ERROR: Invalid syntax. /T only accepts values in the range 0 - 9999.\r\n");
				ExitProcess(255);
			}
			timeout = (LONGLONG)seconds;
		} else if (switch_is(argument, L"/D") && value)
			fallback = arguments.items[++index][0];
		else if (switch_is(argument, L"/M") && value)
			message = arguments.items[++index];
		else if (switch_is(argument, L"/?")) {
			say(L"CHOICE [/C choices] [/N] [/CS] [/T timeout /D choice] [/M text]\r\n\r\n"
				L"Description:\r\n    This tool allows users to select one item from a list\r\n"
				L"    of choices and returns the index of the selected choice.\r\n");
			ExitProcess(0);
		} else if (!is_switch(argument) && !message)
			message = argument;
		else {
			complain(L"ERROR: Invalid argument/option - '");
			complain(argument);
			complain(L"'.\r\nType \"CHOICE /?\" for usage.\r\n");
			ExitProcess(255);
		}
	}
	const DWORD count = length(choices);
	if (!count) {
		complain(L"ERROR: Invalid syntax. /C requires a choice list.\r\n");
		ExitProcess(255);
	}
	if ((timeout >= 0) != (fallback != 0)) {
		complain(timeout >= 0 ? L"ERROR: Invalid syntax. /D is required when /T is given.\r\n" :
			L"ERROR: Invalid syntax. /T is required when /D is given.\r\n");
		ExitProcess(255);
	}
	DWORD fallback_index = 0;
	for (DWORD index = 0; index < count && fallback; ++index)
		if (case_sensitive ? choices[index] == fallback : folded(choices[index]) == folded(fallback))
			fallback_index = index + 1;
	if (fallback && !fallback_index) {
		complain(L"ERROR: Invalid syntax. The default choice is not one of the choices.\r\n");
		ExitProcess(255);
	}
	struct Text prompt = {0};
	if (message) {
		text_put(&prompt, message);
		text_char(&prompt, L' ');
	}
	if (!hide) {
		text_char(&prompt, L'[');
		for (DWORD index = 0; index < count; ++index) {
			if (index)
				text_char(&prompt, L',');
			text_char(&prompt, case_sensitive ? choices[index] : folded(choices[index]));
		}
		text_put(&prompt, L"]?");
	}
	say_text(&prompt);
	DWORD mode, picked = 0;
	const BOOL console = GetConsoleMode(input, &mode);
	const DWORD began = GetTickCount();
	while (!picked) {
		WCHAR key = 0;
		if (console) {
			if (timeout >= 0) {
				if ((GetTickCount() - began) / 1000 >= (DWORD)timeout) {
					picked = fallback_index;
					break;
				}
				DWORD events = 0;
				if (!GetNumberOfConsoleInputEvents(input, &events) || !events) {
					Sleep(50);
					continue;
				}
			}
			key = read_key(input, NULL);
			if (!key)
				break;
		} else {
			/* From a file or a pipe: the next character that is one of the choices, or the end. */
			char byte;
			DWORD got = 0;
			if (!ReadFile(input, &byte, 1, &got, NULL) || !got) {
				picked = fallback_index;
				break;
			}
			key = (WCHAR)(unsigned char)byte;
		}
		for (DWORD index = 0; index < count && !picked; ++index)
			if (case_sensitive ? choices[index] == key : folded(choices[index]) == folded(key))
				picked = index + 1;
		if (!picked && console)
			say(L"\a");
	}
	if (!picked) {
		say(L"\r\n");
		ExitProcess(255);
	}
	const WCHAR shown[4] = {case_sensitive ? choices[picked - 1] : folded(choices[picked - 1]), L'\r', L'\n', 0};
	say(shown);
	ExitProcess(picked);
	return 0;
}
