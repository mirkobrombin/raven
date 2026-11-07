/* timeout.exe: TIMEOUT [/T] timeout [/NOBREAK] — waits a number of seconds, -1 for a key, counting down on the console; a key ends
   the wait unless /NOBREAK, when only Ctrl+C does. Without a console as its input it does not wait. */

#include "tool.h"

/* The waiting line, written over itself from its start as the count goes down. */
static void show_line(LONGLONG left, LONGLONG seconds, BOOL no_break)
{
	struct Text text = {0}, number = {0};
	put_number(&number, (ULONGLONG)(left > 0 ? left : 0), FALSE);
	text_put(&text, L"\rWaiting for ");
	put_field(&text, number.data, seconds >= 100 ? 5 : 2, FALSE);
	text_put(&text, no_break ? L" seconds, press CTRL+C to quit ..." : L" seconds, press a key to continue ...");
	say_text(&text);
	release(number.data);
	release(text.data);
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	BOOL no_break = FALSE, given = FALSE;
	LONGLONG seconds = 0;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (switch_is(argument, L"/T")) {
			if (index + 1 >= arguments.count) {
				complain(L"ERROR: Invalid syntax. Value expected for '/t'.\r\nType \"TIMEOUT /?\" for usage.\r\n");
				ExitProcess(1);
			}
			argument = arguments.items[++index];
		} else if (switch_is(argument, L"/NOBREAK")) {
			no_break = TRUE;
			continue;
		} else if (switch_is(argument, L"/?")) {
			say(L"TIMEOUT [/T] timeout [/NOBREAK]\r\n\r\n"
				L"Description:\r\n    This utility accepts a timeout parameter to wait for the specified\r\n"
				L"    time period (in seconds) or until any key is pressed. It also\r\n    accepts a parameter to ignore the key press.\r\n");
			ExitProcess(0);
		}
		ULONGLONG value;
		if (given || !read_number(argument, &value) || (LONGLONG)value < -1 || (LONGLONG)value > 99999) {
			complain(L"ERROR: Invalid value for timeout (/T) specified. Valid range is -1 to 99999.\r\n");
			ExitProcess(1);
		}
		seconds = (LONGLONG)value;
		given = TRUE;
	}
	if (!given) {
		complain(L"ERROR: Invalid syntax. Default option is not allowed more than '1' time(s).\r\nType \"TIMEOUT /?\" for usage.\r\n");
		ExitProcess(1);
	}
	DWORD mode;
	if (!GetConsoleMode(input, &mode)) {
		complain(L"ERROR: Input redirection is not supported, exiting the process immediately.\r\n");
		ExitProcess(1);
	}
	if (seconds < 0)
		say(no_break ? L"Press CTRL+C to quit ..." : L"Press any key to continue ...");
	else {
		say(L"\r\n");
		show_line(seconds, seconds, no_break);
	}
	/* Key events come as they are pressed; Ctrl+C keeps its usual effect of ending the program. */
	SetConsoleMode(input, (mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) | ENABLE_PROCESSED_INPUT);
	FlushConsoleInputBuffer(input);
	const DWORD began = GetTickCount();
	LONGLONG left = seconds;
	for (;;) {
		if (seconds >= 0) {
			const LONGLONG elapsed = (LONGLONG)((GetTickCount() - began) / 1000);
			if (seconds - elapsed != left) {
				left = seconds - elapsed;
				show_line(left, seconds, no_break);
			}
			if (left <= 0)
				break;
		}
		DWORD events = 0;
		if (GetNumberOfConsoleInputEvents(input, &events) && events) {
			INPUT_RECORD record;
			DWORD count;
			if (ReadConsoleInputW(input, &record, 1, &count) && count && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
				const WORD key = record.Event.KeyEvent.wVirtualKeyCode;
				if (!no_break && key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU && key != VK_CAPITAL)
					break;
			}
			continue;
		}
		Sleep(100);
	}
	SetConsoleMode(input, mode);
	say(L"\r\n");
	ExitProcess(0);
	return 0;
}
