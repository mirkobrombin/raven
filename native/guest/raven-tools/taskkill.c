/* taskkill.exe: TASKKILL [/FI filter] [/PID processid | /IM imagename] [/T] [/F] — ends processes by their PID or their program's name,
   with /T the processes they started too. /F ends a process at once; without it each of the process's top-level windows is asked to
   close (WM_CLOSE), and a process with no window of its own can only be ended with /F. ERRORLEVEL is 0 when every process named was
   ended or asked to close, 128 when one was not found, 1 on another error. */

#include "tool.h"
#include "processes.h"

static struct Processes list;
static DWORD status;
static BOOL forceful;

enum Outcome { ENDED, ASKED, WINDOWLESS, DENIED };

/* The user32 functions that reach a process's windows, looked up so that the tool loads where they are missing. */
typedef BOOL (WINAPI *EnumWindowsFunction)(WNDENUMPROC, LPARAM);
typedef DWORD (WINAPI *WindowProcessFunction)(HWND, DWORD *);
typedef BOOL (WINAPI *PostMessageFunction)(HWND, UINT, WPARAM, LPARAM);
static EnumWindowsFunction enum_windows;
static WindowProcessFunction window_process;
static PostMessageFunction post_message;

struct Closing {
	DWORD id;
	BOOL asked;
};

static BOOL CALLBACK ask_window(HWND window, LPARAM parameter)
{
	struct Closing *closing = (struct Closing *)parameter;
	DWORD owner = 0;
	window_process(window, &owner);
	if (owner == closing->id && post_message(window, WM_CLOSE, 0, 0))
		closing->asked = TRUE;
	return TRUE;
}

static enum Outcome end_process(DWORD id)
{
	if (!forceful) {
		struct Closing closing = {id, FALSE};
		if (enum_windows && window_process && post_message)
			enum_windows(ask_window, (LPARAM)&closing);
		return closing.asked ? ASKED : WINDOWLESS;
	}
	HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, id);
	if (!handle)
		return DENIED;
	const BOOL ended = TerminateProcess(handle, 1);
	CloseHandle(handle);
	return ended ? ENDED : DENIED;
}

/* "name" with PID n, the name only when the process was asked for by it. */
static void put_process(struct Text *text, const struct Process *process, BOOL by_name, DWORD parent)
{
	if (by_name) {
		text_char(text, L'"');
		text_put(text, process->name);
		text_put(text, L"\" ");
	}
	text_put(text, L"with PID ");
	put_number(text, process->id, FALSE);
	if (parent) {
		text_put(text, L" (child process of PID ");
		put_number(text, parent, FALSE);
		text_char(text, L')');
	}
}

static void report(const struct Process *process, BOOL by_name, DWORD parent)
{
	struct Text text = {0};
	const enum Outcome outcome = end_process(process->id);
	if (outcome == WINDOWLESS || outcome == DENIED) {
		text_put(&text, L"ERROR: The process ");
		put_process(&text, process, by_name, 0);
		text_put(&text, outcome == WINDOWLESS ? L" could not be terminated.\r\nReason: This process can only be terminated forcefully (with /F option).\r\n" :
			L" could not be terminated.\r\nReason: Access is denied.\r\n");
		complain(text.data);
		release(text.data);
		status = 1;
		return;
	}
	if (outcome == ASKED) {
		text_put(&text, L"SUCCESS: Sent termination signal to the process ");
		put_process(&text, process, by_name, parent);
		text_put(&text, L".\r\n");
	} else {
		text_put(&text, L"SUCCESS: The process ");
		put_process(&text, process, by_name, parent);
		text_put(&text, L" has been terminated.\r\n");
	}
	say_text(&text);
	release(text.data);
}

/* With /T, the processes a process started are ended first, deepest first. */
static void end_tree(const struct Process *process, BOOL by_name, DWORD parent, BOOL tree)
{
	if (tree)
		for (DWORD index = 0; index < list.count; ++index)
			if (list.items[index].parent == process->id && list.items[index].id != process->id && list.items[index].id != GetCurrentProcessId())
				end_tree(&list.items[index], by_name, process->id, tree);
	report(process, by_name, parent);
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	struct Filter filters[16];
	DWORD filter_count = 0, targets = 0;
	BOOL tree = FALSE;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		const BOOL value = index + 1 < arguments.count;
		if ((switch_is(argument, L"/PID") || switch_is(argument, L"/IM")) && value) {
			++targets;
			++index;
		} else if (switch_is(argument, L"/FI") && value) {
			if (filter_count == 16 || !read_filter(arguments.items[++index], &filters[filter_count])) {
				complain(L"ERROR: The search filter cannot be recognized.\r\n");
				ExitProcess(1);
			}
			++filter_count;
		} else if (switch_is(argument, L"/T"))
			tree = TRUE;
		else if (switch_is(argument, L"/F"))
			forceful = TRUE;
		else if ((switch_is(argument, L"/S") || switch_is(argument, L"/U") || switch_is(argument, L"/P")) && value)
			++index;
		else if (switch_is(argument, L"/?")) {
			say(L"TASKKILL [/S system [/U username [/P [password]]]]\r\n"
				L"         { [/FI filter] [/PID processid | /IM imagename] } [/T] [/F]\r\n\r\n"
				L"Description:\r\n    This tool is used to terminate tasks by process id (PID) or image name.\r\n");
			ExitProcess(0);
		} else {
			complain(L"ERROR: Invalid argument/option - '");
			complain(argument);
			complain(L"'.\r\nType \"TASKKILL /?\" for usage.\r\n");
			ExitProcess(1);
		}
	}
	if (!targets && !filter_count) {
		complain(L"ERROR: Invalid syntax. Neither /FI nor /PID nor /IM were specified.\r\nType \"TASKKILL /?\" for usage.\r\n");
		ExitProcess(1);
	}
	if (!forceful) {
		const HMODULE user = LoadLibraryW(L"user32.dll");
		if (user) {
			enum_windows = (EnumWindowsFunction)(void (*)(void))GetProcAddress(user, "EnumWindows");
			window_process = (WindowProcessFunction)(void (*)(void))GetProcAddress(user, "GetWindowThreadProcessId");
			post_message = (PostMessageFunction)(void (*)(void))GetProcAddress(user, "PostMessageW");
		}
	}
	if (!list_processes(&list)) {
		complain(L"ERROR: The process list cannot be read.\r\n");
		ExitProcess(1);
	}
	const DWORD self = GetCurrentProcessId();
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		const BOOL by_id = switch_is(argument, L"/PID"), by_name = switch_is(argument, L"/IM");
		if ((!by_id && !by_name) || index + 1 >= arguments.count)
			continue;
		const WCHAR *target = arguments.items[++index];
		ULONGLONG id = 0;
		if (by_id && !read_number(target, &id)) {
			complain(L"ERROR: Invalid syntax. Value expected for '/pid'.\r\n");
			status = 1;
			continue;
		}
		BOOL found = FALSE;
		for (DWORD item = 0; item < list.count; ++item) {
			const struct Process *process = &list.items[item];
			if (process->id == self || (by_id ? process->id != id : !matches(target, process->name)))
				continue;
			BOOL wanted = TRUE;
			for (DWORD filter = 0; filter < filter_count && wanted; ++filter)
				wanted = passes(&filters[filter], process);
			if (!wanted)
				continue;
			found = TRUE;
			end_tree(process, by_name, 0, tree);
		}
		if (!found) {
			complain(L"ERROR: The process \"");
			complain(target);
			complain(L"\" not found.\r\n");
			status = 128;
		}
	}
	/* Filters alone end every process they let through. */
	if (!targets) {
		BOOL found = FALSE;
		for (DWORD item = 0; item < list.count; ++item) {
			BOOL wanted = list.items[item].id != self;
			for (DWORD filter = 0; filter < filter_count && wanted; ++filter)
				wanted = passes(&filters[filter], &list.items[item]);
			if (wanted) {
				found = TRUE;
				end_tree(&list.items[item], TRUE, 0, tree);
			}
		}
		if (!found)
			say(L"INFO: No tasks running with the specified criteria.\r\n");
	}
	ExitProcess(status);
	return 0;
}
