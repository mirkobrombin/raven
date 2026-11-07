/* sort.exe: SORT [/R] [/+n] [/L locale] [/M kilobytes] [/REC recordbytes] [/T [tempdir]] [/O outfile] [/C[ASE_SENSITIVE]] [file] —
   the lines of a file or of the input in order, ignoring case unless /C, from character n with /+n, reversed with /R. */

#include "tool.h"

static BOOL reverse, case_sensitive;
static DWORD column;

struct Line {
	const WCHAR *text;
	DWORD size;
};

static int compare_lines(const struct Line *left, const struct Line *right)
{
	DWORD a = column < left->size ? column : left->size, b = column < right->size ? column : right->size;
	for (; a < left->size && b < right->size; ++a, ++b) {
		const WCHAR x = case_sensitive ? left->text[a] : folded(left->text[a]), y = case_sensitive ? right->text[b] : folded(right->text[b]);
		if (x != y)
			return x < y ? -1 : 1;
	}
	return (a < left->size) - (b < right->size);
}

/* A stable merge sort, so that equal lines keep their order. */
static void sort_lines(struct Line *lines, DWORD count)
{
	struct Line *scratch = allocate((count + 1) * sizeof *scratch);
	for (DWORD width = 1; width < count; width *= 2) {
		for (DWORD start = 0; start < count; start += 2 * width) {
			const DWORD middle = start + width < count ? start + width : count, end = start + 2 * width < count ? start + 2 * width : count;
			DWORD left = start, right = middle, out = start;
			while (left < middle && right < end)
				scratch[out++] = compare_lines(&lines[right], &lines[left]) < 0 ? lines[right++] : lines[left++];
			while (left < middle)
				scratch[out++] = lines[left++];
			while (right < end)
				scratch[out++] = lines[right++];
		}
		memcpy(lines, scratch, count * sizeof *scratch);
	}
	release(scratch);
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	const WCHAR *file = NULL, *target = NULL;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (switch_is(argument, L"/R") || switch_is(argument, L"/REVERSE"))
			reverse = TRUE;
		else if (switch_is(argument, L"/C") || switch_is(argument, L"/CASE_SENSITIVE"))
			case_sensitive = TRUE;
		else if (is_switch(argument) && argument[1] == L'+') {
			ULONGLONG start = 1;
			read_number(argument + 2, &start);
			column = start ? (DWORD)start - 1 : 0;
		} else if (switch_is(argument, L"/O") || switch_is(argument, L"/OUTPUT")) {
			if (index + 1 < arguments.count)
				target = arguments.items[++index];
		} else if (switch_is(argument, L"/L") || switch_is(argument, L"/LOCALE") || switch_is(argument, L"/M") ||
			switch_is(argument, L"/MEMORY") || switch_is(argument, L"/REC") || switch_is(argument, L"/RECORD_MAXIMUM")) {
			++index;
		} else if (switch_is(argument, L"/T") || switch_is(argument, L"/TEMPORARY")) {
			if (index + 1 < arguments.count && !is_switch(arguments.items[index + 1]) && file)
				++index;
		} else if (switch_is(argument, L"/?")) {
			say(L"SORT [/R] [/+n] [/M kilobytes] [/L locale] [/REC recordbytes]\r\n"
				L"  [[drive1:][path1]filename1] [/T [drive2:][path2]]\r\n  [/O [drive3:][path3]filename3]\r\n");
			ExitProcess(0);
		} else if (is_switch(argument)) {
			complain(L"Invalid switch.\r\n");
			ExitProcess(1);
		} else
			file = argument;
	}
	DWORD size;
	WCHAR *text = file ? read_file_text(file, &size) : read_text(input, &size);
	if (!text) {
		complain(L"The system cannot find the file specified.\r\n");
		ExitProcess(1);
	}
	DWORD count = 0, at = 0, start, line_size;
	while (next_line(text, size, &at, &start, &line_size))
		++count;
	struct Line *lines = allocate((count + 1) * sizeof *lines);
	count = at = 0;
	while (next_line(text, size, &at, &start, &line_size)) {
		lines[count].text = text + start;
		lines[count++].size = line_size;
	}
	sort_lines(lines, count);
	if (reverse)
		for (DWORD index = 0; index < count / 2; ++index) {
			const struct Line swap = lines[index];
			lines[index] = lines[count - 1 - index];
			lines[count - 1 - index] = swap;
		}
	if (target) {
		HANDLE file_handle = CreateFileW(target, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file_handle == INVALID_HANDLE_VALUE) {
			complain(L"The system cannot open the output file.\r\n");
			ExitProcess(1);
		}
		output = file_handle;
	}
	struct Text out = {0};
	for (DWORD index = 0; index < count; ++index) {
		text_add(&out, lines[index].text, lines[index].size);
		text_put(&out, L"\r\n");
		if (out.size > 8192)
			say_text(&out);
	}
	say_text(&out);
	if (target)
		CloseHandle(output);
	ExitProcess(0);
	return 0;
}
