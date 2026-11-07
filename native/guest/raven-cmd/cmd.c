/* cmd.exe, the command processor of a Raven prefix. Raven code on the public Windows headers, written from Microsoft's public
   description of the command processor and of its commands; no Windows code is used or consulted.

   A line goes through the phases cmd documents: %variable% and batch argument expansion first, then parsing into commands joined
   by &, &&, || and |, with redirections, parenthesised blocks, IF and FOR. Each command, as it runs, gets its FOR variables and,
   with delayed expansion, its !variables!. Built-in commands run here; anything else is a program found by PATH and PATHEXT, whose
   exit code becomes ERRORLEVEL. At a console the prompt has its own line editor, with history and file name completion. */

#include <windows.h>

#define LINE_LIMIT 8192

/* No C runtime: the compiler may still ask for these. */
void *memset(void *target, int value, size_t size)
{
	unsigned char *bytes = target;
	while (size--)
		*bytes++ = (unsigned char)value;
	return target;
}

void *memcpy(void *target, const void *source, size_t size)
{
	unsigned char *to = target;
	const unsigned char *from = source;
	while (size--)
		*to++ = *from++;
	return target;
}

void *memmove(void *target, const void *source, size_t size)
{
	unsigned char *to = target;
	const unsigned char *from = source;
	if (to < from)
		while (size--)
			*to++ = *from++;
	else
		while (size--)
			to[size] = from[size];
	return target;
}

/* The handles commands use now: the standard three, the process's own or the files and pipes a redirection names, and 3 to 9,
   which only a redirection opens, for others to be pointed at (3>log.txt, then 1>&3). */
static HANDLE standard[10];
#define input standard[0]
#define output standard[1]
#define error_output standard[2]

static BOOL echo_on = TRUE, interactive_input, delayed;
/* Whether commands come from the prompt, rather than from /C. */
static BOOL prompting;
static DWORD errorlevel;
/* Set by EXIT, EXIT /B and GOTO: the rest of the processor, of the batch file, or of the line does not run. */
static BOOL leaving, leaving_batch, jumped;
/* Set when a batch job is given up, after Ctrl+C or a syntax error: every batch file running ends. */
static BOOL abandon;
/* Set by CALL for the command it runs: a batch file that command names returns to its caller instead of replacing it. */
static BOOL calling;
static volatile LONG interrupted;
static WCHAR self[MAX_PATH];
static const WCHAR *command_line;
static ULONG random_state;

/* The batch file running now, or a label CALL runs in it: its text, where its next line starts, and its arguments. */
struct Batch {
	WCHAR *text;
	DWORD size, next;
	BOOL owns_text;
	WCHAR *file;
	WCHAR **arguments;
	DWORD count;
	WCHAR *rest;
};
static struct Batch *batch;

static BOOL halted(void) { return leaving || leaving_batch || jumped || abandon || interrupted; }

static void *allocate(SIZE_T size) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1); }
static void release(void *block) { if (block) HeapFree(GetProcessHeap(), 0, block); }

static DWORD length(const WCHAR *text)
{
	DWORD size = 0;
	while (text[size])
		++size;
	return size;
}

static WCHAR *duplicate(const WCHAR *text, DWORD size)
{
	WCHAR *copy = allocate((size + 1) * sizeof(WCHAR));
	for (DWORD index = 0; index < size; ++index)
		copy[index] = text[index];
	return copy;
}

static WCHAR *copy_of(const WCHAR *text) { return duplicate(text, length(text)); }
static WCHAR folded(WCHAR character) { return character >= L'a' && character <= L'z' ? (WCHAR)(character - 32) : character; }
static BOOL blank(WCHAR character) { return character == L' ' || character == L'\t'; }
/* Commas, semicolons and equals signs separate words where blanks do, as cmd's own delimiters. */
static BOOL delimiter(WCHAR character) { return blank(character) || character == L',' || character == L';' || character == L'='; }
static BOOL digit(WCHAR character) { return character >= L'0' && character <= L'9'; }
static BOOL letter(WCHAR character) { return folded(character) >= L'A' && folded(character) <= L'Z'; }
static BOOL wild(const WCHAR *text) { for (; *text; ++text) if (*text == L'*' || *text == L'?') return TRUE; return FALSE; }

/* Whether `text` starts with `word`, ignoring case. */
static BOOL starts(const WCHAR *text, const WCHAR *word)
{
	for (; *word; ++text, ++word)
		if (folded(*text) != folded(*word))
			return FALSE;
	return TRUE;
}

static BOOL equal(const WCHAR *left, const WCHAR *right) { return starts(left, right) && !left[length(right)]; }

static BOOL same(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right)
		++left, ++right;
	return *left == *right;
}

static int compare_folded(const WCHAR *left, const WCHAR *right)
{
	while (*left && folded(*left) == folded(*right))
		++left, ++right;
	return (int)folded(*left) - (int)folded(*right);
}

/* Whether `text` starts with `word` as a whole word: what follows it is the end, a blank, or one of `after`. */
static BOOL keyword(const WCHAR *text, const WCHAR *word, const WCHAR *after)
{
	if (!starts(text, word))
		return FALSE;
	const WCHAR next = text[length(word)];
	if (!next || blank(next) || next == L'\n')
		return TRUE;
	for (; after && *after; ++after)
		if (next == *after)
			return TRUE;
	return FALSE;
}

/* Text that grows as it is written; `data` always ends with a zero once anything was added. */
struct Text {
	WCHAR *data;
	DWORD size, capacity;
};

static void text_add(struct Text *text, const WCHAR *characters, DWORD size)
{
	if (text->size + size + 1 > text->capacity) {
		DWORD capacity = text->capacity ? text->capacity : 64;
		while (capacity < text->size + size + 1)
			capacity *= 2;
		WCHAR *grown = allocate(capacity * sizeof(WCHAR));
		for (DWORD index = 0; index < text->size; ++index)
			grown[index] = text->data[index];
		release(text->data);
		text->data = grown;
		text->capacity = capacity;
	}
	for (DWORD index = 0; index < size; ++index)
		text->data[text->size + index] = characters[index];
	text->size += size;
	text->data[text->size] = 0;
}

static void text_put(struct Text *text, const WCHAR *characters) { if (characters) text_add(text, characters, length(characters)); }
static void text_char(struct Text *text, WCHAR character) { text_add(text, &character, 1); }

static WCHAR *text_take(struct Text *text)
{
	if (!text->data)
		text_add(text, L"", 0);
	WCHAR *data = text->data;
	text->data = NULL;
	text->size = text->capacity = 0;
	return data;
}

/* Text without the quotes in it. */
static WCHAR *unquoted(const WCHAR *text)
{
	struct Text result = {0};
	for (; *text; ++text)
		if (*text != L'"')
			text_char(&result, *text);
	return text_take(&result);
}

/* Text without the blanks at either end. */
static WCHAR *trimmed(const WCHAR *text)
{
	while (blank(*text))
		++text;
	DWORD size = length(text);
	while (size && blank(text[size - 1]))
		--size;
	return duplicate(text, size);
}

/* Writes text to a handle: as characters to a console, else in the console's code page as cmd writes to files and pipes. */
static void write_to(HANDLE handle, const WCHAR *text, DWORD size)
{
	DWORD done, mode;
	if (!size)
		return;
	if (GetConsoleMode(handle, &mode)) {
		WriteConsoleW(handle, text, size, &done, NULL);
		return;
	}
	const UINT page = GetConsoleOutputCP();
	const int bytes = WideCharToMultiByte(page, 0, text, (int)size, NULL, 0, NULL, NULL);
	char *encoded = allocate((SIZE_T)bytes + 1);
	WideCharToMultiByte(page, 0, text, (int)size, encoded, bytes, NULL, NULL);
	WriteFile(handle, encoded, (DWORD)bytes, &done, NULL);
	release(encoded);
}

static void say(const WCHAR *text) { write_to(output, text, length(text)); }
static void complain(const WCHAR *text) { write_to(error_output, text, length(text)); }

/* A failed command: its message, and ERRORLEVEL 1. */
static DWORD failure(const WCHAR *message)
{
	complain(message);
	errorlevel = 1;
	return 1;
}

static DWORD syntax_error(void) { return failure(L"The syntax of the command is incorrect.\r\n"); }

/* A number in decimal, with thousands separators when asked; `digits` holds at least 32 characters. */
static DWORD format_number(WCHAR *digits, ULONGLONG value, BOOL grouped)
{
	WCHAR reversed[32];
	DWORD count = 0, group = 0;
	do {
		if (grouped && group == 3) {
			reversed[count++] = L',';
			group = 0;
		}
		reversed[count++] = (WCHAR)(L'0' + (WCHAR)(value % 10));
		++group;
		value /= 10;
	} while (value);
	for (DWORD index = 0; index < count; ++index)
		digits[index] = reversed[count - 1 - index];
	digits[count] = 0;
	return count;
}

static void put_number(struct Text *text, ULONGLONG value, BOOL grouped)
{
	WCHAR digits[32];
	text_add(text, digits, format_number(digits, value, grouped));
}

static void put_signed(struct Text *text, LONG value)
{
	if (value < 0)
		text_char(text, L'-');
	put_number(text, value < 0 ? (ULONGLONG)(-(LONGLONG)value) : (ULONGLONG)value, FALSE);
}

/* A number right-aligned in `width` characters. */
static void put_padded(struct Text *text, ULONGLONG value, DWORD width, BOOL grouped)
{
	WCHAR digits[32];
	const DWORD size = format_number(digits, value, grouped);
	for (DWORD pad = size; pad < width; ++pad)
		text_char(text, L' ');
	text_add(text, digits, size);
}

static void put_two(struct Text *text, DWORD value)
{
	text_char(text, (WCHAR)(L'0' + value / 10 % 10));
	text_char(text, (WCHAR)(L'0' + value % 10));
}

static void put_hex(struct Text *text, DWORD value, int digits)
{
	while (digits--)
		text_char(text, L"0123456789ABCDEF"[(value >> (digits * 4)) & 15]);
}

static void say_text(struct Text *text)
{
	if (text->size)
		write_to(output, text->data, text->size);
	text->size = 0;
	if (text->data)
		text->data[0] = 0;
}

/* Reads a number as cmd does: decimal, hexadecimal after 0x, octal after a leading 0, with a sign. Whether all of `text` is one. */
static BOOL read_integer(const WCHAR *text, LONG *value)
{
	BOOL negative = FALSE;
	ULONG number = 0, base = 10;
	if (*text == L'-' || *text == L'+')
		negative = *text++ == L'-';
	if (!*text)
		return FALSE;
	if (text[0] == L'0' && folded(text[1]) == L'X') {
		base = 16;
		text += 2;
		if (!*text)
			return FALSE;
	} else if (text[0] == L'0' && text[1])
		base = 8;
	for (; *text; ++text) {
		ULONG place;
		if (digit(*text))
			place = (ULONG)(*text - L'0');
		else if (folded(*text) >= L'A' && folded(*text) <= L'F')
			place = (ULONG)(folded(*text) - L'A' + 10);
		else
			return FALSE;
		if (place >= base)
			return FALSE;
		number = number * base + place;
	}
	*value = negative ? -(LONG)number : (LONG)number;
	return TRUE;
}

/* The leading decimal number of `text`, as ERRORLEVEL, EXIT and SHIFT take one. */
static LONG leading_number(const WCHAR *text)
{
	LONG value = 0;
	BOOL negative = FALSE;
	while (blank(*text))
		++text;
	if (*text == L'-' || *text == L'+')
		negative = *text++ == L'-';
	while (digit(*text))
		value = value * 10 + (*text++ - L'0');
	return negative ? -value : value;
}

/* The value of an environment variable, or NULL when it is not set. */
static WCHAR *environment_value(const WCHAR *name)
{
	WCHAR *value = allocate(32768 * sizeof(WCHAR));
	const DWORD size = GetEnvironmentVariableW(name, value, 32768);
	if (!size || size >= 32768) {
		release(value);
		return NULL;
	}
	WCHAR *result = duplicate(value, size);
	release(value);
	return result;
}

static void put_date(struct Text *text, const SYSTEMTIME *time)
{
	put_two(text, time->wMonth);
	text_char(text, L'/');
	put_two(text, time->wDay);
	text_char(text, L'/');
	put_number(text, time->wYear, FALSE);
}

static void put_clock(struct Text *text, const SYSTEMTIME *time)
{
	put_two(text, time->wHour % 12 ? time->wHour % 12 : 12);
	text_char(text, L':');
	put_two(text, time->wMinute);
	text_put(text, time->wHour < 12 ? L" AM" : L" PM");
}

/* The dynamic variables, for names no environment variable has: CD, DATE, TIME, RANDOM, ERRORLEVEL and the others cmd documents. */
static WCHAR *dynamic_value(const WCHAR *name)
{
	struct Text text = {0};
	SYSTEMTIME now;
	if (equal(name, L"CD") || equal(name, L"__CD__")) {
		WCHAR folder[MAX_PATH];
		const DWORD size = GetCurrentDirectoryW(MAX_PATH, folder);
		text_add(&text, folder, size);
		if (equal(name, L"__CD__") && size && folder[size - 1] != L'\\')
			text_char(&text, L'\\');
	} else if (equal(name, L"ERRORLEVEL"))
		put_signed(&text, (LONG)errorlevel);
	else if (equal(name, L"DATE")) {
		GetLocalTime(&now);
		static const WCHAR days[] = L"SunMonTueWedThuFriSat";
		text_add(&text, days + (now.wDayOfWeek % 7) * 3, 3);
		text_char(&text, L' ');
		put_date(&text, &now);
	} else if (equal(name, L"TIME")) {
		GetLocalTime(&now);
		if (now.wHour < 10)
			text_char(&text, L' ');
		put_number(&text, now.wHour, FALSE);
		text_char(&text, L':');
		put_two(&text, now.wMinute);
		text_char(&text, L':');
		put_two(&text, now.wSecond);
		text_char(&text, L'.');
		put_two(&text, now.wMilliseconds / 10);
	} else if (equal(name, L"RANDOM")) {
		random_state = random_state * 1103515245 + 12345;
		put_number(&text, (random_state >> 16) & 0x7fff, FALSE);
	} else if (equal(name, L"CMDEXTVERSION"))
		text_put(&text, L"2");
	else if (equal(name, L"CMDCMDLINE"))
		text_put(&text, command_line);
	else if (equal(name, L"HIGHESTNUMANODENUMBER"))
		text_put(&text, L"0");
	else
		return NULL;
	return text_take(&text);
}

static WCHAR *variable_value(const WCHAR *name)
{
	WCHAR *value = environment_value(name);
	return value ? value : dynamic_value(name);
}

/* A signed decimal number at `*at`, before `end`. */
static LONG read_offset(const WCHAR **at, const WCHAR *end)
{
	BOOL negative = FALSE;
	LONG value = 0;
	while (*at < end && blank(**at))
		++*at;
	if (*at < end && (**at == L'-' || **at == L'+'))
		negative = *(*at)++ == L'-';
	while (*at < end && digit(**at))
		value = value * 10 + (*(*at)++ - L'0');
	return negative ? -value : value;
}

/* A variable's value through the edits after its name: :~start,count takes part of it, :old=new replaces text (from the start, with
   *old=new). */
static WCHAR *edited(const WCHAR *value, const WCHAR *edit, DWORD edit_size)
{
	const LONG size = (LONG)length(value);
	const WCHAR *end = edit + edit_size;
	if (edit_size && *edit == L'~') {
		const WCHAR *at = edit + 1;
		LONG start = read_offset(&at, end), stop = size;
		if (start < 0)
			start += size;
		if (start < 0)
			start = 0;
		if (start > size)
			start = size;
		if (at < end && *at == L',') {
			++at;
			const LONG count = read_offset(&at, end);
			stop = count < 0 ? size + count : start + count;
		}
		if (stop > size)
			stop = size;
		if (stop < start)
			stop = start;
		return duplicate(value + start, (DWORD)(stop - start));
	}
	const WCHAR *equals = edit;
	while (equals < end && *equals != L'=')
		++equals;
	if (equals == end)
		return copy_of(value);
	const WCHAR *old = edit;
	const BOOL leading = old < equals && *old == L'*';
	if (leading)
		++old;
	const DWORD old_size = (DWORD)(equals - old);
	if (!old_size)
		return copy_of(value);
	struct Text result = {0};
	for (LONG at = 0; at < size;) {
		BOOL match = at + (LONG)old_size <= size;
		for (DWORD index = 0; match && index < old_size; ++index)
			match = folded(value[at + (LONG)index]) == folded(old[index]);
		if (match) {
			if (leading)
				result.size = 0;
			text_add(&result, equals + 1, (DWORD)(end - equals - 1));
			at += (LONG)old_size;
			if (leading) {
				text_put(&result, value + at);
				break;
			}
			continue;
		}
		text_char(&result, value[at++]);
	}
	return text_take(&result);
}

/* What %name% or !name! stands for, with its edits after a colon; NULL when the variable is not set. */
static WCHAR *reference_value(const WCHAR *reference, DWORD size)
{
	DWORD colon = 0;
	while (colon < size && reference[colon] != L':')
		++colon;
	WCHAR *name = duplicate(reference, colon);
	WCHAR *value = variable_value(name);
	release(name);
	if (value && colon < size) {
		WCHAR *changed = edited(value, reference + colon + 1, size - colon - 1);
		release(value);
		value = changed;
	}
	return value;
}

static BOOL modifier(WCHAR character)
{
	character = folded(character);
	return character == L'F' || character == L'D' || character == L'P' || character == L'N' || character == L'X' || character == L'S' ||
		character == L'A' || character == L'T' || character == L'Z';
}

static void put_attributes(struct Text *text, DWORD attributes)
{
	static const DWORD bits[] = {FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_ARCHIVE, FILE_ATTRIBUTE_HIDDEN,
		FILE_ATTRIBUTE_SYSTEM, FILE_ATTRIBUTE_COMPRESSED, FILE_ATTRIBUTE_OFFLINE, FILE_ATTRIBUTE_TEMPORARY, FILE_ATTRIBUTE_REPARSE_POINT};
	static const WCHAR letters[] = L"drahscotl";
	for (int index = 0; index < 9; ++index)
		text_char(text, attributes & bits[index] ? letters[index] : L'-');
}

static void put_file_time(struct Text *text, const FILETIME *time, const WCHAR *gap)
{
	FILETIME local;
	SYSTEMTIME parts;
	FileTimeToLocalFileTime(time, &local);
	FileTimeToSystemTime(&local, &parts);
	put_date(text, &parts);
	text_put(text, gap);
	put_clock(text, &parts);
}

static BOOL find_program(const WCHAR *name, WCHAR *found);

/* Finds a file by name in the folders a variable lists, as %~$PATH:1 does. */
static BOOL search_folders(const WCHAR *variable, DWORD variable_size, const WCHAR *name, WCHAR *found)
{
	WCHAR *names = duplicate(variable, variable_size);
	WCHAR *folders = variable_value(names);
	release(names);
	BOOL success = FALSE;
	for (WCHAR *start = folders; folders && *start && !success;) {
		WCHAR *end = start;
		while (*end && *end != L';')
			++end;
		struct Text path = {0};
		text_add(&path, start, (DWORD)(end - start));
		if (path.size && path.data[path.size - 1] != L'\\')
			text_char(&path, L'\\');
		text_put(&path, name);
		WCHAR *candidate = text_take(&path);
		const DWORD attributes = GetFileAttributesW(candidate);
		if (attributes != INVALID_FILE_ATTRIBUTES && GetFullPathNameW(candidate, MAX_PATH, found, NULL))
			success = TRUE;
		release(candidate);
		start = *end ? end + 1 : end;
	}
	release(folders);
	return success;
}

/* The %~ modifiers on a value, a batch argument or a FOR variable: the quotes go, and f d p n x s a t z and $VAR: take the parts of
   the file it names that cmd /? documents. */
static void path_modifiers(struct Text *out, const WCHAR *value, const WCHAR *modifiers, DWORD count, const WCHAR *search,
	DWORD search_size)
{
	WCHAR *plain = unquoted(value);
	WCHAR full[MAX_PATH * 2];
	BOOL f = FALSE, d = FALSE, p = FALSE, n = FALSE, x = FALSE, a = FALSE, t = FALSE, z = FALSE;
	for (DWORD index = 0; index < count; ++index)
		switch (folded(modifiers[index])) {
		case L'F': f = TRUE; break;
		case L'S': f = TRUE; break;
		case L'D': d = TRUE; break;
		case L'P': p = TRUE; break;
		case L'N': n = TRUE; break;
		case L'X': x = TRUE; break;
		case L'A': a = TRUE; break;
		case L'T': t = TRUE; break;
		case L'Z': z = TRUE; break;
		default: break;
		}
	if (search) {
		if (!*plain || !search_folders(search, search_size, plain, full)) {
			release(plain);
			return;
		}
		if (!count) {
			text_put(out, full);
			release(plain);
			return;
		}
	} else if (!count) {
		text_put(out, plain);
		release(plain);
		return;
	} else if (!*plain || !GetFullPathNameW(plain, MAX_PATH * 2, full, NULL)) {
		release(plain);
		return;
	}
	release(plain);
	if (f)
		d = p = n = x = TRUE;
	WIN32_FIND_DATAW data;
	HANDLE found = (a || t || z) ? FindFirstFileW(full, &data) : INVALID_HANDLE_VALUE;
	BOOL any = FALSE;
	if (found != INVALID_HANDLE_VALUE) {
		FindClose(found);
		if (a) {
			put_attributes(out, data.dwFileAttributes);
			any = TRUE;
		}
		if (t) {
			if (any)
				text_char(out, L' ');
			put_file_time(out, &data.ftLastWriteTime, L" ");
			any = TRUE;
		}
		if (z) {
			if (any)
				text_char(out, L' ');
			put_number(out, ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow, FALSE);
			any = TRUE;
		}
	}
	if (!(d || p || n || x))
		return;
	if (any)
		text_char(out, L' ');
	const DWORD size = length(full);
	DWORD slash = 0, dot = size;
	for (DWORD index = 0; index < size; ++index)
		if (full[index] == L'\\')
			slash = index + 1;
	for (DWORD index = size; index > slash; --index)
		if (full[index - 1] == L'.') {
			dot = index - 1;
			break;
		}
	const DWORD drive = size >= 2 && full[1] == L':' ? 2 : 0;
	if (d)
		text_add(out, full, drive);
	if (p)
		text_add(out, full + drive, slash > drive ? slash - drive : 0);
	if (n)
		text_add(out, full + slash, dot - slash);
	if (x)
		text_add(out, full + dot, size - dot);
}

/* %0 to %9 in a batch file, with modifiers when given. In a label CALL runs, %0 with modifiers is still the batch file. */
static void batch_argument(struct Text *out, DWORD index, const WCHAR *modifiers, DWORD count, const WCHAR *search, DWORD search_size)
{
	const WCHAR *value = index < batch->count ? batch->arguments[index] : L"";
	if (!modifiers) {
		text_put(out, value);
		return;
	}
	if (!index && batch->file && (count || search))
		value = batch->file;
	path_modifiers(out, value, modifiers, count, search, search_size);
}

/* The first phase of every line: %NAME% and its edits, and in a batch file %0 to %9 with their modifiers, %* and %% for one percent
   sign. A name that is not set stays as it was typed at the prompt, and goes away in a batch file. */
static WCHAR *expand_percent(const WCHAR *line)
{
	struct Text out = {0};
	for (const WCHAR *at = line; *at;) {
		if (*at != L'%') {
			text_char(&out, *at++);
			continue;
		}
		if (batch) {
			if (at[1] == L'%') {
				text_char(&out, L'%');
				at += 2;
				continue;
			}
			if (digit(at[1])) {
				batch_argument(&out, (DWORD)(at[1] - L'0'), NULL, 0, NULL, 0);
				at += 2;
				continue;
			}
			if (at[1] == L'*') {
				text_put(&out, batch->rest);
				at += 2;
				continue;
			}
			if (at[1] == L'~') {
				const WCHAR *modifiers = at + 2, *scan = modifiers, *search = NULL;
				DWORD search_size = 0;
				while (modifier(*scan))
					++scan;
				const DWORD count = (DWORD)(scan - modifiers);
				if (*scan == L'$') {
					const WCHAR *colon = scan + 1;
					while (*colon && *colon != L':')
						++colon;
					if (*colon) {
						search = scan + 1;
						search_size = (DWORD)(colon - search);
						scan = colon + 1;
					}
				}
				if (digit(*scan)) {
					batch_argument(&out, (DWORD)(*scan - L'0'), modifiers, count, search, search_size);
					at = scan + 1;
					continue;
				}
			}
		}
		const WCHAR *end = at + 1;
		while (*end && *end != L'%' && *end != L'\n')
			++end;
		if (*end != L'%') {
			if (!batch)
				text_char(&out, *at);
			++at;
			continue;
		}
		if (end == at + 1) {
			text_char(&out, L'%');
			++at;
			continue;
		}
		WCHAR *value = reference_value(at + 1, (DWORD)(end - at - 1));
		if (value)
			text_put(&out, value);
		else if (!batch)
			text_add(&out, at, (DWORD)(end - at + 1));
		release(value);
		at = end + 1;
	}
	return text_take(&out);
}

/* The FOR variables in effect, by their character, and how many loops are running. */
static WCHAR *for_values[128];
static DWORD for_depth;

/* A command's FOR variables, %v and %~[modifiers]v, as it runs. With modifiers, the variable is the character after the longest run
   of them that a set variable follows, so %~nxi is n and x of i while %~n alone may be the variable n. */
static WCHAR *substitute_for(const WCHAR *text)
{
	if (!for_depth)
		return copy_of(text);
	struct Text out = {0};
	for (const WCHAR *at = text; *at;) {
		if (*at == L'%' && at[1] == L'~') {
			const WCHAR *modifiers = at + 2;
			DWORD run = 0;
			while (modifier(modifiers[run]))
				++run;
			if (modifiers[run] == L'$') {
				const WCHAR *colon = modifiers + run + 1;
				while (*colon && *colon != L':')
					++colon;
				if (*colon && colon[1] && colon[1] < 128 && for_values[colon[1]]) {
					path_modifiers(&out, for_values[colon[1]], modifiers, run, modifiers + run + 1, (DWORD)(colon - modifiers - run - 1));
					at = colon + 2;
					continue;
				}
			}
			BOOL done = FALSE;
			for (DWORD count = run + 1; count-- > 0;) {
				const WCHAR variable = modifiers[count];
				if (variable && variable < 128 && for_values[variable]) {
					path_modifiers(&out, for_values[variable], modifiers, count, NULL, 0);
					at = modifiers + count + 1;
					done = TRUE;
					break;
				}
			}
			if (done)
				continue;
		} else if (*at == L'%' && at[1] && at[1] < 128 && for_values[at[1]]) {
			text_put(&out, for_values[at[1]]);
			at += 2;
			continue;
		}
		text_char(&out, *at++);
	}
	return text_take(&out);
}

/* Delayed expansion, when SETLOCAL ENABLEDELAYEDEXPANSION or /V:ON turned it on: !NAME! and its edits as the command runs. In text
   with an exclamation mark, a caret takes the next character as it is. */
static WCHAR *expand_delayed(const WCHAR *text)
{
	BOOL marked = FALSE;
	for (const WCHAR *at = text; *at && !marked; ++at)
		marked = *at == L'!';
	if (!delayed || !marked)
		return copy_of(text);
	struct Text out = {0};
	for (const WCHAR *at = text; *at;) {
		if (*at == L'^') {
			if (at[1])
				text_char(&out, at[1]), at += 2;
			else
				++at;
			continue;
		}
		if (*at != L'!') {
			text_char(&out, *at++);
			continue;
		}
		const WCHAR *end = at + 1;
		while (*end && *end != L'!')
			++end;
		if (!*end) {
			++at;
			continue;
		}
		WCHAR *value = end > at + 1 ? reference_value(at + 1, (DWORD)(end - at - 1)) : NULL;
		if (value)
			text_put(&out, value);
		else if (!batch && end > at + 1)
			text_add(&out, at, (DWORD)(end - at + 1));
		release(value);
		at = end + 1;
	}
	return text_take(&out);
}

/* What a command's text becomes as it runs: its FOR variables, then its delayed variables. */
static WCHAR *prepared(const WCHAR *text)
{
	if (!text)
		return NULL;
	WCHAR *substituted = substitute_for(text);
	WCHAR *result = expand_delayed(substituted);
	release(substituted);
	return result;
}

/* A line parsed: commands, the operators between them, blocks, IF and FOR. The text of a command keeps its quotes and loses the
   carets that escaped characters; its redirections are taken out of it. */
enum Kind { COMMAND, SEQUENCE, AND, OR, PIPE, BLOCK, IF_COMMAND, FOR_COMMAND };
enum Test { TEST_ERRORLEVEL, TEST_EXIST, TEST_DEFINED, TEST_VERSION, TEST_EQUAL, TEST_EQU, TEST_NEQ, TEST_LSS, TEST_LEQ, TEST_GTR, TEST_GEQ };
enum Loop { LOOP_FILES, LOOP_FOLDERS, LOOP_TREE, LOOP_NUMBERS, LOOP_LINES };

struct Redirect {
	struct Redirect *next;
	int handle;
	/* < reads, > writes, A appends, & duplicates `source`. */
	WCHAR kind;
	int source;
	WCHAR *target;
};

struct Node {
	enum Kind kind;
	/* The two sides of an operator, a block's body, IF's command and its ELSE, FOR's command. */
	struct Node *left, *right;
	WCHAR *text;
	/* The text the node came from, as the command processor that runs one side of a pipe is given it. */
	WCHAR *source;
	struct Redirect *redirects;
	BOOL negate, fold;
	enum Test test;
	WCHAR *first, *second;
	enum Loop loop;
	WCHAR *options, *set, *root;
	WCHAR variable;
};

static const WCHAR *cursor;
static int depth;
static BOOL incomplete, bad_syntax;

static void free_node(struct Node *node)
{
	if (!node)
		return;
	free_node(node->left);
	free_node(node->right);
	for (struct Redirect *redirect = node->redirects, *next; redirect; redirect = next) {
		next = redirect->next;
		release(redirect->target);
		release(redirect);
	}
	release(node->text);
	release(node->source);
	release(node->first);
	release(node->second);
	release(node->options);
	release(node->set);
	release(node->root);
	release(node);
}

static struct Node *new_node(enum Kind kind)
{
	struct Node *node = allocate(sizeof *node);
	node->kind = kind;
	return node;
}

static struct Node *pair(enum Kind kind, struct Node *left, struct Node *right)
{
	if (!left)
		return right;
	if (!right)
		return left;
	struct Node *node = new_node(kind);
	node->left = left;
	node->right = right;
	return node;
}

static void skip_blanks(void)
{
	while (blank(*cursor))
		++cursor;
}

/* Where a word ends outside quotes: a blank, an operator, a redirection, the end of the line, or the end of the block. */
static BOOL word_end(WCHAR character)
{
	return !character || blank(character) || character == L'\n' || character == L'&' || character == L'|' || character == L'<' ||
		character == L'>' || (character == L')' && depth > 0);
}

/* One word, quoted or with escapes, as a redirection's file or IF's operands take it; at `==` too when asked. */
static WCHAR *read_word(BOOL stop_at_equals)
{
	struct Text word = {0};
	BOOL quoted = FALSE;
	skip_blanks();
	while (*cursor && *cursor != L'\n') {
		const WCHAR character = *cursor;
		if (!quoted) {
			if (word_end(character) || character == L'(')
				break;
			if (stop_at_equals && character == L'=' && cursor[1] == L'=')
				break;
			if (character == L'^' && cursor[1] && cursor[1] != L'\n') {
				text_char(&word, cursor[1]);
				cursor += 2;
				continue;
			}
		}
		if (character == L'"')
			quoted = !quoted;
		text_char(&word, character);
		++cursor;
	}
	return text_take(&word);
}

/* A redirection at the cursor: [n]< file, [n]> file, [n]>> file, [n]>&m. */
static void parse_redirect(struct Redirect **list)
{
	struct Redirect *redirect = allocate(sizeof *redirect);
	redirect->handle = *cursor == L'<' ? 0 : 1;
	if (digit(*cursor))
		redirect->handle = *cursor++ - L'0';
	if (*cursor == L'<') {
		redirect->kind = L'<';
		++cursor;
	} else {
		++cursor;
		redirect->kind = L'>';
		if (*cursor == L'>') {
			redirect->kind = L'A';
			++cursor;
		}
	}
	if (*cursor == L'&' && digit(cursor[1])) {
		redirect->kind = L'&';
		redirect->source = cursor[1] - L'0';
		cursor += 2;
	} else {
		redirect->target = read_word(FALSE);
		if (!*redirect->target)
			bad_syntax = TRUE;
	}
	while (*list)
		list = &(*list)->next;
	*list = redirect;
}

/* A digit starts a redirection only as a word of its own: 2>nul, not echo a2>nul. */
static BOOL at_redirect(BOOL word_start)
{
	return *cursor == L'<' || *cursor == L'>' || (word_start && digit(*cursor) && (cursor[1] == L'<' || cursor[1] == L'>'));
}

static void parse_redirects(struct Node *node)
{
	for (;;) {
		skip_blanks();
		if (!at_redirect(TRUE))
			return;
		parse_redirect(&node->redirects);
	}
}

/* A simple command: its text up to an operator, a redirection taken out, or the end of the line or block. */
static struct Node *parse_command(void)
{
	struct Node *node = new_node(COMMAND);
	struct Text text = {0};
	BOOL quoted = FALSE;
	while (*cursor && *cursor != L'\n') {
		const WCHAR character = *cursor;
		if (!quoted) {
			if (character == L'^') {
				if (cursor[1] && cursor[1] != L'\n')
					text_char(&text, cursor[1]), cursor += 2;
				else
					++cursor;
				continue;
			}
			if (character == L'&' || character == L'|' || (character == L')' && depth > 0))
				break;
			if (at_redirect(!text.size || blank(text.data[text.size - 1]))) {
				parse_redirect(&node->redirects);
				continue;
			}
		}
		if (character == L'"')
			quoted = !quoted;
		text_char(&text, character);
		++cursor;
	}
	node->text = text_take(&text);
	return node;
}

static struct Node *parse_sequence(BOOL lines);
static struct Node *parse_primary(void);

/* IF [/I] [NOT] ERRORLEVEL n | EXIST file | DEFINED name | CMDEXTVERSION n | a==b | a op b, its command, and after a block ELSE and
   another. */
static struct Node *parse_if(void)
{
	struct Node *node = new_node(IF_COMMAND);
	cursor += 2;
	for (;;) {
		skip_blanks();
		if (keyword(cursor, L"/I", NULL)) {
			node->fold = TRUE;
			cursor += 2;
		} else if (keyword(cursor, L"NOT", NULL)) {
			node->negate = TRUE;
			cursor += 3;
		} else
			break;
	}
	if (keyword(cursor, L"ERRORLEVEL", NULL)) {
		node->test = TEST_ERRORLEVEL;
		cursor += 10;
		node->first = read_word(FALSE);
	} else if (keyword(cursor, L"EXIST", NULL)) {
		node->test = TEST_EXIST;
		cursor += 5;
		node->first = read_word(FALSE);
	} else if (keyword(cursor, L"DEFINED", NULL)) {
		node->test = TEST_DEFINED;
		cursor += 7;
		node->first = read_word(FALSE);
	} else if (keyword(cursor, L"CMDEXTVERSION", NULL)) {
		node->test = TEST_VERSION;
		cursor += 13;
		node->first = read_word(FALSE);
	} else {
		node->first = read_word(TRUE);
		skip_blanks();
		static const WCHAR *const operators[] = {L"EQU", L"NEQ", L"LSS", L"LEQ", L"GTR", L"GEQ"};
		if (cursor[0] == L'=' && cursor[1] == L'=') {
			node->test = TEST_EQUAL;
			while (*cursor == L'=')
				++cursor;
		} else {
			int found = -1;
			for (int index = 0; index < 6 && found < 0; ++index)
				if (keyword(cursor, operators[index], NULL))
					found = index;
			if (found < 0) {
				bad_syntax = TRUE;
				return node;
			}
			node->test = (enum Test)(TEST_EQU + found);
			cursor += 3;
		}
		node->second = read_word(FALSE);
	}
	skip_blanks();
	if (!*node->first || (node->test >= TEST_EQUAL && !*node->second) || !*cursor || *cursor == L'\n') {
		bad_syntax = TRUE;
		return node;
	}
	node->left = parse_sequence(FALSE);
	skip_blanks();
	if (node->left && node->left->kind == BLOCK && keyword(cursor, L"ELSE", L"(")) {
		cursor += 4;
		skip_blanks();
		node->right = parse_sequence(FALSE);
		if (!node->right && !incomplete)
			bad_syntax = TRUE;
	}
	return node;
}

/* FOR [/D | /R [root] | /L | /F ["options"]] %v IN (set) DO command. */
static struct Node *parse_for(void)
{
	struct Node *node = new_node(FOR_COMMAND);
	cursor += 3;
	node->loop = LOOP_FILES;
	for (;;) {
		skip_blanks();
		if (*cursor != L'/')
			break;
		const WCHAR option = folded(cursor[1]);
		cursor += 2;
		skip_blanks();
		if (option == L'D')
			node->loop = LOOP_FOLDERS;
		else if (option == L'L')
			node->loop = LOOP_NUMBERS;
		else if (option == L'R') {
			node->loop = LOOP_TREE;
			if (*cursor != L'%')
				node->root = read_word(FALSE);
		} else if (option == L'F') {
			node->loop = LOOP_LINES;
			if (*cursor == L'"') {
				const WCHAR *start = ++cursor;
				while (*cursor && *cursor != L'"' && *cursor != L'\n')
					++cursor;
				node->options = duplicate(start, (DWORD)(cursor - start));
				if (*cursor == L'"')
					++cursor;
			}
		} else {
			bad_syntax = TRUE;
			return node;
		}
	}
	if (*cursor != L'%' || !cursor[1] || blank(cursor[1])) {
		bad_syntax = TRUE;
		return node;
	}
	node->variable = cursor[1];
	cursor += 2;
	skip_blanks();
	if (!keyword(cursor, L"IN", L"(")) {
		bad_syntax = TRUE;
		return node;
	}
	cursor += 2;
	skip_blanks();
	if (*cursor != L'(') {
		bad_syntax = TRUE;
		return node;
	}
	/* The set, up to its closing parenthesis outside quotes; in a batch file it may go on over lines. */
	const WCHAR *start = ++cursor;
	WCHAR quote = 0;
	int nested = 0;
	for (; *cursor; ++cursor) {
		if (quote) {
			if (*cursor == quote)
				quote = 0;
		} else if (*cursor == L'"' || *cursor == L'\'' || *cursor == L'`')
			quote = *cursor;
		else if (*cursor == L'(')
			++nested;
		else if (*cursor == L')' && !nested--)
			break;
	}
	if (!*cursor) {
		incomplete = TRUE;
		return node;
	}
	struct Text set = {0};
	for (const WCHAR *at = start; at < cursor; ++at)
		text_char(&set, *at == L'\n' ? L' ' : *at);
	node->set = text_take(&set);
	++cursor;
	skip_blanks();
	if (!keyword(cursor, L"DO", L"(")) {
		bad_syntax = TRUE;
		return node;
	}
	cursor += 2;
	skip_blanks();
	if (!*cursor || *cursor == L'\n') {
		bad_syntax = TRUE;
		return node;
	}
	node->left = parse_sequence(FALSE);
	return node;
}

/* A block, IF, FOR, REM, a label, or a simple command. */
static struct Node *parse_primary(void)
{
	while (blank(*cursor) || *cursor == L',' || *cursor == L';' || *cursor == L'=' || *cursor == L'@')
		++cursor;
	const WCHAR *start = cursor;
	struct Node *node;
	if (!*cursor || *cursor == L'\n' || *cursor == L')' || *cursor == L'&' || *cursor == L'|')
		return NULL;
	if (*cursor == L'(') {
		++cursor;
		++depth;
		struct Node *body = parse_sequence(TRUE);
		while (blank(*cursor) || *cursor == L'\n')
			++cursor;
		if (*cursor != L')') {
			if (!*cursor)
				incomplete = TRUE;
			else
				bad_syntax = TRUE;
			free_node(body);
			--depth;
			return NULL;
		}
		++cursor;
		--depth;
		node = new_node(BLOCK);
		node->left = body;
		parse_redirects(node);
	} else if (*cursor == L':') {
		/* A label, or a comment written as one: the rest of the line. */
		while (*cursor && *cursor != L'\n')
			++cursor;
		node = new_node(COMMAND);
		node->text = copy_of(L"");
	} else if (keyword(cursor, L"REM", L"/")) {
		/* REM takes the rest of the line as it is, operators and all. */
		while (*cursor && *cursor != L'\n')
			++cursor;
		node = new_node(COMMAND);
		node->text = duplicate(start, (DWORD)(cursor - start));
	} else if (keyword(cursor, L"IF", L"/"))
		node = parse_if();
	else if (keyword(cursor, L"FOR", L"/%"))
		node = parse_for();
	else
		node = parse_command();
	return node;
}

/* Commands joined by |; each side keeps its text, for the command processor that runs it. */
static struct Node *parse_pipeline(void)
{
	while (blank(*cursor))
		++cursor;
	const WCHAR *start = cursor;
	struct Node *left = parse_primary();
	for (;;) {
		skip_blanks();
		if (bad_syntax || incomplete || *cursor != L'|' || cursor[1] == L'|')
			return left;
		if (!left) {
			bad_syntax = TRUE;
			return NULL;
		}
		DWORD size = (DWORD)(cursor - start);
		while (size && blank(start[size - 1]))
			--size;
		release(left->source);
		left->source = duplicate(start, size);
		++cursor;
		while (blank(*cursor))
			++cursor;
		const WCHAR *right_start = cursor;
		struct Node *right = parse_primary();
		if (!right) {
			if (!incomplete)
				bad_syntax = TRUE;
			free_node(left);
			return NULL;
		}
		DWORD right_size = (DWORD)(cursor - right_start);
		while (right_size && blank(right_start[right_size - 1]))
			--right_size;
		right->source = duplicate(right_start, right_size);
		left = pair(PIPE, left, right);
	}
}

static struct Node *parse_conditional(void)
{
	struct Node *left = parse_pipeline();
	for (;;) {
		skip_blanks();
		if (bad_syntax || incomplete)
			return left;
		enum Kind kind;
		if (cursor[0] == L'&' && cursor[1] == L'&')
			kind = AND;
		else if (cursor[0] == L'|' && cursor[1] == L'|')
			kind = OR;
		else
			return left;
		if (!left) {
			bad_syntax = TRUE;
			return NULL;
		}
		cursor += 2;
		struct Node *right = parse_pipeline();
		if (!right) {
			if (!incomplete)
				bad_syntax = TRUE;
			free_node(left);
			return NULL;
		}
		struct Node *node = new_node(kind);
		node->left = left;
		node->right = right;
		left = node;
	}
}

/* Commands joined by &, and in a block by line ends. IF and FOR take the rest of their line, & and all, but not the next line. */
static struct Node *parse_sequence(BOOL lines)
{
	struct Node *left = parse_conditional();
	for (;;) {
		skip_blanks();
		if (bad_syntax || incomplete)
			return left;
		if (*cursor == L'&' && cursor[1] != L'&')
			++cursor;
		else if (*cursor == L'\n' && lines)
			++cursor;
		else
			return left;
		left = pair(SEQUENCE, left, parse_conditional());
	}
}

/* Parses a whole line. `*more` says it ended inside a block or a FOR set, and needs the next line; a syntax error says so here. */
static struct Node *parse_text(const WCHAR *text, BOOL *more, BOOL *bad)
{
	cursor = text;
	depth = 0;
	incomplete = bad_syntax = FALSE;
	struct Node *tree = parse_sequence(TRUE);
	while (blank(*cursor) || *cursor == L'\n')
		++cursor;
	*more = incomplete;
	*bad = FALSE;
	if (incomplete) {
		free_node(tree);
		return NULL;
	}
	/* A line that starts with a closing parenthesis is ignored, as cmd does. */
	if (!bad_syntax && *cursor == L')' && !tree)
		return NULL;
	if (bad_syntax || *cursor) {
		const WCHAR *word = *cursor ? cursor : L"";
		DWORD size = 0;
		while (word[size] && !blank(word[size]) && word[size] != L'\n')
			++size;
		if (size) {
			WCHAR *shown = duplicate(word, size);
			complain(shown);
			complain(L" was unexpected at this time.\r\n");
			release(shown);
		} else
			complain(L"The syntax of the command is incorrect.\r\n");
		errorlevel = 255;
		free_node(tree);
		*bad = TRUE;
		return NULL;
	}
	return tree;
}

static DWORD execute(struct Node *node);
static DWORD run_simple(const WCHAR *line);
static DWORD run_batch(const WCHAR *path, const WCHAR *arguments);

/* The files a command's redirections opened, closed when it ends. */
struct Opened {
	HANDLE saved[10];
	HANDLE handles[16];
	int count;
};

static void restore(struct Opened *opened)
{
	for (int index = 0; index < 10; ++index)
		standard[index] = opened->saved[index];
	for (int index = 0; index < opened->count; ++index)
		CloseHandle(opened->handles[index]);
	opened->count = 0;
}

static const WCHAR *file_error(DWORD error)
{
	switch (error) {
	case ERROR_PATH_NOT_FOUND: return L"The system cannot find the path specified.\r\n";
	case ERROR_ACCESS_DENIED: return L"Access is denied.\r\n";
	case ERROR_SHARING_VIOLATION: return L"The process cannot access the file because it is being used by another process.\r\n";
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS: return L"A duplicate file name exists, or the file\r\ncannot be found.\r\n";
	case ERROR_DIR_NOT_EMPTY: return L"The directory is not empty.\r\n";
	case ERROR_DIRECTORY: return L"The directory name is invalid.\r\n";
	case ERROR_INVALID_NAME: return L"The filename, directory name, or volume label syntax is incorrect.\r\n";
	default: return L"The system cannot find the file specified.\r\n";
	}
}

/* Points the standard handles where a command's redirections say, in their order, so that 2>&1 after >file joins the file. */
static BOOL redirect(struct Redirect *list, struct Opened *opened)
{
	for (int index = 0; index < 10; ++index)
		opened->saved[index] = standard[index];
	opened->count = 0;
	SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
	for (struct Redirect *item = list; item; item = item->next) {
		if (item->kind == L'&') {
			/* n>&m copies what m is now; a handle no redirection has opened can't be copied. */
			if (!standard[item->source]) {
				const WCHAR number[] = {(WCHAR)(L'0' + item->handle), L'.', L'\r', L'\n', 0};
				complain(L"The handle could not be duplicated\r\nduring redirection of handle ");
				complain(number);
				restore(opened);
				return FALSE;
			}
			standard[item->handle] = standard[item->source];
			continue;
		}
		WCHAR *expanded = prepared(item->target), *target = unquoted(expanded);
		release(expanded);
		HANDLE file;
		if (item->kind == L'<')
			file = CreateFileW(target, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		else
			file = CreateFileW(target, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, item->kind == L'A' ? OPEN_ALWAYS : CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, NULL);
		release(target);
		if (file == INVALID_HANDLE_VALUE) {
			complain(file_error(GetLastError()));
			restore(opened);
			return FALSE;
		}
		if (item->kind == L'A')
			SetFilePointer(file, 0, NULL, FILE_END);
		if (opened->count < 16)
			opened->handles[opened->count++] = file;
		standard[item->handle] = file;
	}
	return TRUE;
}

/* Starts a program with the standard handles in effect now; the process handle comes back for the caller to wait on. */
static BOOL launch(const WCHAR *path, const WCHAR *command, const WCHAR *directory, HANDLE *process_handle)
{
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;
	memset(&startup, 0, sizeof startup);
	startup.cb = sizeof startup;
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = input;
	startup.hStdOutput = output;
	startup.hStdError = error_output;
	WCHAR *line = copy_of(command);
	const BOOL started = CreateProcessW(path, line, NULL, NULL, TRUE, 0, NULL, directory, &startup, &process);
	release(line);
	if (!started) {
		const DWORD error = GetLastError();
		complain(error == ERROR_BAD_EXE_FORMAT ? L"This program cannot be run here: it is not a Windows program for this machine.\r\n" :
			L"The system cannot execute the specified program.\r\n");
		errorlevel = error;
		return FALSE;
	}
	CloseHandle(process.hThread);
	*process_handle = process.hProcess;
	return TRUE;
}

/* Whether a program has a window of its own rather than the console: the subsystem in its PE optional header is 2. */
static BOOL windowed(const WCHAR *path)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	unsigned char header[512];
	DWORD count = 0;
	ReadFile(file, header, sizeof header, &count, NULL);
	CloseHandle(file);
	if (count < 64 || header[0] != 'M' || header[1] != 'Z')
		return FALSE;
	const DWORD at = header[60] | header[61] << 8 | header[62] << 16 | (DWORD)header[63] << 24;
	if (at + 4 + 20 + 70 > count || header[at] != 'P' || header[at + 1] != 'E')
		return FALSE;
	return (header[at + 4 + 20 + 68] | header[at + 4 + 20 + 69] << 8) == IMAGE_SUBSYSTEM_WINDOWS_GUI;
}

/* Runs a program and waits for it; its exit code becomes ERRORLEVEL. At the prompt, a program with a window of its own runs on
   while the prompt comes back, as cmd does. */
static DWORD run_program(const WCHAR *path, const WCHAR *command)
{
	HANDLE process;
	if (!launch(path, command, NULL, &process))
		return errorlevel;
	if (prompting && !batch && windowed(path)) {
		CloseHandle(process);
		errorlevel = 0;
		return 0;
	}
	WaitForSingleObject(process, INFINITE);
	GetExitCodeProcess(process, &errorlevel);
	CloseHandle(process);
	return errorlevel;
}

/* A command line that runs `text` in another command processor, as the sides of a pipe and FOR /F commands run. */
static WCHAR *processor_line(const WCHAR *text)
{
	struct Text line = {0};
	text_char(&line, L'"');
	text_put(&line, self);
	text_put(&line, L"\" /d /s /c \"");
	text_put(&line, text);
	text_char(&line, L'"');
	return text_take(&line);
}

/* Starts one side of a pipe in a command processor of its own. */
static BOOL launch_side(struct Node *side, HANDLE *process)
{
	WCHAR *text = prepared(side->source ? side->source : L"");
	WCHAR *line = processor_line(text);
	release(text);
	const BOOL started = launch(self, line, NULL, process);
	release(line);
	return started;
}

/* a | b: each side runs in a command processor of its own, as cmd runs them, so what a side sets stays in it; the left writes into
   the pipe and the right reads it. ERRORLEVEL is the right side's exit code. */
static DWORD run_pipe(struct Node *node)
{
	SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
	HANDLE reader, writer, left = NULL, right = NULL;
	if (!CreatePipe(&reader, &writer, &inherit, 0))
		return failure(L"The pipe could not be created.\r\n");
	/* Each child inherits only its own end, so the reader sees the end of the input when the left side ends. */
	SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
	const HANDLE saved_input = input, saved_output = output;
	output = writer;
	const BOOL left_started = launch_side(node->left, &left);
	output = saved_output;
	CloseHandle(writer);
	SetHandleInformation(reader, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	input = reader;
	const BOOL right_started = launch_side(node->right, &right);
	input = saved_input;
	CloseHandle(reader);
	DWORD status = errorlevel;
	if (right_started) {
		WaitForSingleObject(right, INFINITE);
		GetExitCodeProcess(right, &status);
		CloseHandle(right);
	}
	if (left_started) {
		WaitForSingleObject(left, INFINITE);
		CloseHandle(left);
	}
	errorlevel = status;
	return status;
}

static BOOL exists(const WCHAR *name)
{
	WCHAR *path = unquoted(name);
	DWORD size = length(path);
	BOOL found;
	/* folder\NUL names the folder, as an old test for folders has it. */
	if (size >= 4 && equal(path + size - 3, L"NUL") && (path[size - 4] == L'\\' || path[size - 4] == L'/')) {
		path[size - 4] = 0;
		const DWORD attributes = GetFileAttributesW(*path ? path : L"\\");
		found = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
	} else if (wild(path)) {
		WIN32_FIND_DATAW data;
		HANDLE search = FindFirstFileW(path, &data);
		found = search != INVALID_HANDLE_VALUE;
		if (found)
			FindClose(search);
	} else
		found = *path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
	release(path);
	return found;
}

static DWORD run_if(struct Node *node)
{
	WCHAR *first = prepared(node->first), *second = prepared(node->second);
	BOOL result = FALSE;
	LONG left, right;
	switch (node->test) {
	case TEST_ERRORLEVEL: result = (LONG)errorlevel >= leading_number(first); break;
	case TEST_EXIST: result = exists(first); break;
	case TEST_DEFINED: {
		WCHAR *value = environment_value(first);
		result = value != NULL;
		release(value);
		break;
	}
	case TEST_VERSION: result = 2 >= leading_number(first); break;
	case TEST_EQUAL: result = node->fold ? equal(first, second) && length(first) == length(second) : same(first, second); break;
	default: {
		int order;
		if (read_integer(first, &left) && read_integer(second, &right))
			order = left < right ? -1 : left > right;
		else
			order = node->fold ? lstrcmpiW(first, second) : lstrcmpW(first, second);
		switch (node->test) {
		case TEST_EQU: result = order == 0; break;
		case TEST_NEQ: result = order != 0; break;
		case TEST_LSS: result = order < 0; break;
		case TEST_LEQ: result = order <= 0; break;
		case TEST_GTR: result = order > 0; break;
		default: result = order >= 0; break;
		}
	}
	}
	release(first);
	release(second);
	if (node->negate)
		result = !result;
	if (result)
		return node->left ? execute(node->left) : 0;
	return node->right ? execute(node->right) : 0;
}

static void for_set(WCHAR variable, const WCHAR *value, DWORD size, WCHAR **saved)
{
	if (variable >= 128)
		return;
	if (for_values[variable] != saved[variable])
		release(for_values[variable]);
	for_values[variable] = duplicate(value, size);
}

static void for_restore(WCHAR **saved)
{
	for (int index = 0; index < 128; ++index) {
		if (for_values[index] != saved[index])
			release(for_values[index]);
		for_values[index] = saved[index];
	}
}

static DWORD for_body(struct Node *node, const WCHAR *value, WCHAR **saved)
{
	for_set(node->variable, value, length(value), saved);
	return execute(node->left);
}

/* The items of a plain FOR set: words, and file names for those with wildcards; folders instead with /D. Each name keeps the folder
   its pattern gave. */
static DWORD for_items(struct Node *node, const WCHAR *set, const WCHAR *folder, BOOL folders, WCHAR **saved)
{
	DWORD status = 0;
	for (const WCHAR *at = set; *at && !halted();) {
		while (delimiter(*at))
			++at;
		if (!*at)
			break;
		const WCHAR *start = at;
		BOOL quoted = FALSE;
		while (*at && (quoted || !delimiter(*at))) {
			if (*at == L'"')
				quoted = !quoted;
			++at;
		}
		WCHAR *item = duplicate(start, (DWORD)(at - start));
		struct Text path = {0};
		if (folder) {
			text_put(&path, folder);
			if (path.size && path.data[path.size - 1] != L'\\')
				text_char(&path, L'\\');
		}
		WCHAR *plain = unquoted(item);
		text_put(&path, plain);
		release(plain);
		if (!wild(item))
			status = for_body(node, folder ? path.data : item, saved); else {
			WIN32_FIND_DATAW data;
			HANDLE search = FindFirstFileW(path.data, &data);
			DWORD prefix = path.size;
			while (prefix && path.data[prefix - 1] != L'\\' && path.data[prefix - 1] != L':')
				--prefix;
			if (search != INVALID_HANDLE_VALUE) {
				do {
					const BOOL is_folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
					if (is_folder != folders || same(data.cFileName, L".") || same(data.cFileName, L"..") ||
						(data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
						continue;
					struct Text name = {0};
					text_add(&name, path.data, prefix);
					text_put(&name, data.cFileName);
					status = for_body(node, name.data, saved);
					release(name.data);
				} while (!halted() && FindNextFileW(search, &data));
				FindClose(search);
			}
		}
		release(path.data);
		release(item);
	}
	return status;
}

/* FOR /R: the set in the root folder and in every folder under it. */
static DWORD for_tree(struct Node *node, const WCHAR *set, const WCHAR *folder, WCHAR **saved)
{
	DWORD status = for_items(node, set, folder, FALSE, saved);
	struct Text pattern = {0};
	text_put(&pattern, folder);
	if (pattern.size && pattern.data[pattern.size - 1] != L'\\')
		text_char(&pattern, L'\\');
	const DWORD base = pattern.size;
	text_char(&pattern, L'*');
	WIN32_FIND_DATAW data;
	HANDLE search = FindFirstFileW(pattern.data, &data);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
				same(data.cFileName, L".") || same(data.cFileName, L".."))
				continue;
			pattern.size = base;
			text_put(&pattern, data.cFileName);
			WCHAR *sub = copy_of(pattern.data);
			status = for_tree(node, set, sub, saved);
			release(sub);
		} while (!halted() && FindNextFileW(search, &data));
		FindClose(search);
	}
	release(pattern.data);
	return status;
}

/* The options of FOR /F: eol=c skip=n delims=xxx tokens=x,y,m-n[*] usebackq. */
struct LineOptions {
	WCHAR eol;
	DWORD skip;
	WCHAR delims[64];
	DWORD delim_count;
	DWORD tokens[32];
	DWORD token_count;
	BOOL rest, backquote;
};

static BOOL line_options(const WCHAR *text, struct LineOptions *options)
{
	options->eol = L';';
	options->skip = 0;
	options->delims[0] = L' ';
	options->delims[1] = L'\t';
	options->delim_count = 2;
	options->tokens[0] = 1;
	options->token_count = 1;
	options->rest = options->backquote = FALSE;
	for (const WCHAR *at = text ? text : L""; *at;) {
		if (blank(*at)) {
			++at;
			continue;
		}
		if (starts(at, L"EOL=")) {
			at += 4;
			options->eol = *at;
			if (*at)
				++at;
		} else if (starts(at, L"SKIP=")) {
			at += 5;
			options->skip = (DWORD)leading_number(at);
			while (digit(*at))
				++at;
		} else if (starts(at, L"USEBACKQ")) {
			options->backquote = TRUE;
			at += 8;
		} else if (starts(at, L"DELIMS=")) {
			/* The delimiters run to the end, or to a blank that another option follows. */
			at += 7;
			options->delim_count = 0;
			while (*at) {
				if (blank(*at) && (starts(at + 1, L"EOL=") || starts(at + 1, L"SKIP=") || starts(at + 1, L"TOKENS=") ||
					starts(at + 1, L"USEBACKQ") || starts(at + 1, L"DELIMS=")))
					break;
				if (options->delim_count < 64)
					options->delims[options->delim_count++] = *at;
				++at;
			}
		} else if (starts(at, L"TOKENS=")) {
			at += 7;
			options->token_count = 0;
			while (*at && !blank(*at)) {
				if (*at == L'*') {
					options->rest = TRUE;
					++at;
				} else if (digit(*at)) {
					DWORD first = 0, last;
					while (digit(*at))
						first = first * 10 + (DWORD)(*at++ - L'0');
					last = first;
					if (*at == L'-') {
						++at;
						last = 0;
						while (digit(*at))
							last = last * 10 + (DWORD)(*at++ - L'0');
					}
					for (DWORD token = first; token <= last && token && options->token_count < 32; ++token)
						options->tokens[options->token_count++] = token;
				} else if (*at == L',')
					++at;
				else
					return FALSE;
			}
		} else
			return FALSE;
	}
	/* In ascending order, as the variables take them. */
	for (DWORD index = 1; index < options->token_count; ++index)
		for (DWORD back = index; back && options->tokens[back - 1] > options->tokens[back]; --back) {
			const DWORD swap = options->tokens[back];
			options->tokens[back] = options->tokens[back - 1];
			options->tokens[back - 1] = swap;
		}
	return TRUE;
}

static BOOL line_delimiter(const struct LineOptions *options, WCHAR character)
{
	for (DWORD index = 0; index < options->delim_count; ++index)
		if (options->delims[index] == character)
			return TRUE;
	return FALSE;
}

/* One line for FOR /F: split into tokens, which go to the variable and those after it. A line with none of them is skipped. */
static DWORD for_line(struct Node *node, const struct LineOptions *options, const WCHAR *line, DWORD size, WCHAR **saved)
{
	if (!size || (options->eol && line[0] == options->eol))
		return 0;
	DWORD starts_at[64], ends_at[64], found = 0;
	for (DWORD at = 0; at < size && found < 64;) {
		while (at < size && line_delimiter(options, line[at]))
			++at;
		if (at >= size)
			break;
		starts_at[found] = at;
		while (at < size && !line_delimiter(options, line[at]))
			++at;
		ends_at[found++] = at;
	}
	WCHAR variable = node->variable;
	BOOL any = FALSE;
	DWORD highest = 0;
	for (DWORD index = 0; index < options->token_count; ++index, ++variable) {
		const DWORD token = options->tokens[index];
		highest = token;
		if (token <= found) {
			for_set(variable, line + starts_at[token - 1], ends_at[token - 1] - starts_at[token - 1], saved);
			any = TRUE;
		} else
			for_set(variable, L"", 0, saved);
	}
	if (options->rest) {
		if (highest < found) {
			for_set(variable, line + starts_at[highest], size - starts_at[highest], saved);
			any = TRUE;
		} else
			for_set(variable, L"", 0, saved);
	}
	if (!any)
		return 0;
	return execute(node->left);
}

/* Bytes as text: UTF-16 after its byte order mark, else the console's code page, without a UTF-8 byte order mark. */
static WCHAR *decoded(const char *bytes, DWORD size, DWORD *count)
{
	if (size >= 2 && (unsigned char)bytes[0] == 0xff && (unsigned char)bytes[1] == 0xfe) {
		*count = (size - 2) / 2;
		WCHAR *text = allocate((*count + 1) * sizeof(WCHAR));
		for (DWORD index = 0; index < *count; ++index)
			text[index] = (WCHAR)((unsigned char)bytes[2 + index * 2] | (unsigned char)bytes[3 + index * 2] << 8);
		return text;
	}
	if (size >= 3 && (unsigned char)bytes[0] == 0xef && (unsigned char)bytes[1] == 0xbb && (unsigned char)bytes[2] == 0xbf) {
		bytes += 3;
		size -= 3;
	}
	WCHAR *text = allocate(((SIZE_T)size + 1) * sizeof(WCHAR));
	*count = size ? (DWORD)MultiByteToWideChar(GetConsoleOutputCP(), 0, bytes, (int)size, text, (int)size) : 0;
	return text;
}

static DWORD for_lines(struct Node *node, const struct LineOptions *options, const WCHAR *text, DWORD size, WCHAR **saved)
{
	DWORD status = 0, skipped = 0;
	for (DWORD at = 0; at < size && !halted();) {
		DWORD end = at;
		while (end < size && text[end] != L'\n')
			++end;
		DWORD stop = end;
		if (stop > at && text[stop - 1] == L'\r')
			--stop;
		if (skipped < options->skip)
			++skipped;
		else
			status = for_line(node, options, text + at, stop - at, saved);
		at = end + 1;
	}
	return status;
}

static char *read_all(HANDLE handle, DWORD *size)
{
	DWORD capacity = 4096, count;
	char *bytes = allocate(capacity);
	*size = 0;
	while (ReadFile(handle, bytes + *size, capacity - *size, &count, NULL) && count) {
		*size += count;
		if (*size == capacity) {
			char *grown = allocate(capacity * 2);
			memcpy(grown, bytes, capacity);
			release(bytes);
			bytes = grown;
			capacity *= 2;
		}
	}
	return bytes;
}

/* The output of a command run in another command processor, as FOR /F reads it. */
static char *capture(const WCHAR *command, DWORD *size)
{
	SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
	HANDLE reader, writer, process;
	*size = 0;
	if (!CreatePipe(&reader, &writer, &inherit, 0))
		return allocate(1);
	SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
	WCHAR *line = processor_line(command);
	const HANDLE saved = output;
	output = writer;
	const BOOL started = launch(self, line, NULL, &process);
	output = saved;
	release(line);
	CloseHandle(writer);
	char *bytes = read_all(reader, size);
	CloseHandle(reader);
	if (started) {
		WaitForSingleObject(process, INFINITE);
		CloseHandle(process);
	}
	return bytes;
}

static DWORD for_file_lines(struct Node *node, const WCHAR *set, const struct LineOptions *options, WCHAR **saved)
{
	const WCHAR open = options->backquote ? L'`' : L'\'', string = options->backquote ? L'\'' : L'"';
	const DWORD size = length(set);
	if (size >= 2 && (set[0] == open || set[0] == string) && set[size - 1] == set[0]) {
		WCHAR *inside = duplicate(set + 1, size - 2);
		DWORD status = 0;
		if (set[0] == string)
			status = for_lines(node, options, inside, size - 2, saved);
		else {
			DWORD bytes;
			char *captured = capture(inside, &bytes);
			DWORD count;
			WCHAR *text = decoded(captured, bytes, &count);
			release(captured);
			status = for_lines(node, options, text, count, saved);
			release(text);
		}
		release(inside);
		return status;
	}
	DWORD status = 0;
	for (const WCHAR *at = set; *at && !halted();) {
		while (blank(*at))
			++at;
		if (!*at)
			break;
		const WCHAR *start = at;
		BOOL quoted = FALSE;
		while (*at && (quoted || !blank(*at))) {
			if (*at == L'"' && options->backquote)
				quoted = !quoted;
			++at;
		}
		WCHAR *item = duplicate(start, (DWORD)(at - start)), *name = options->backquote ? unquoted(item) : copy_of(item);
		release(item);
		HANDLE file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		if (file == INVALID_HANDLE_VALUE) {
			complain(L"The system cannot find the file ");
			complain(name);
			complain(L".\r\n");
			release(name);
			errorlevel = 1;
			return 1;
		}
		release(name);
		DWORD bytes, count;
		char *contents = read_all(file, &bytes);
		CloseHandle(file);
		WCHAR *text = decoded(contents, bytes, &count);
		release(contents);
		status = for_lines(node, options, text, count, saved);
		release(text);
	}
	return status;
}

static DWORD run_for(struct Node *node)
{
	WCHAR *set = prepared(node->set), *root = prepared(node->root), *options_text = prepared(node->options);
	WCHAR **saved = allocate(128 * sizeof(WCHAR *));
	for (int index = 0; index < 128; ++index)
		saved[index] = for_values[index];
	++for_depth;
	DWORD status = 0;
	if (node->loop == LOOP_NUMBERS) {
		LONG numbers[3] = {0, 0, 0};
		const WCHAR *at = set;
		for (int index = 0; index < 3; ++index) {
			while (delimiter(*at))
				++at;
			numbers[index] = leading_number(at);
			while (*at && !delimiter(*at))
				++at;
		}
		for (LONG value = numbers[0]; !halted() && (numbers[1] >= 0 ? value <= numbers[2] : value >= numbers[2]); value += numbers[1]) {
			struct Text number = {0};
			put_signed(&number, value);
			status = for_body(node, number.data, saved);
			release(number.data);
			if (!numbers[1])
				break;
		}
	} else if (node->loop == LOOP_LINES) {
		struct LineOptions options;
		WCHAR *whole = trimmed(set);
		if (!line_options(options_text, &options))
			status = failure(L"The syntax of the command is incorrect.\r\n");
		else
			status = for_file_lines(node, whole, &options, saved);
		release(whole);
	} else if (node->loop == LOOP_TREE) {
		WCHAR folder[MAX_PATH];
		WCHAR *plain = root ? unquoted(root) : NULL;
		if (!plain || !GetFullPathNameW(plain, MAX_PATH, folder, NULL))
			GetCurrentDirectoryW(MAX_PATH, folder);
		release(plain);
		status = for_tree(node, set, folder, saved);
	} else
		status = for_items(node, set, NULL, node->loop == LOOP_FOLDERS, saved);
	--for_depth;
	for_restore(saved);
	release(saved);
	release(set);
	release(root);
	release(options_text);
	return status;
}

static DWORD run_command(struct Node *node)
{
	struct Opened opened;
	if (!redirect(node->redirects, &opened)) {
		errorlevel = 1;
		return 1;
	}
	WCHAR *line = prepared(node->text);
	const DWORD status = run_simple(line);
	release(line);
	restore(&opened);
	return status;
}

/* Runs a parsed line; what it returns is the status && and || test, 0 for success. */
static DWORD execute(struct Node *node)
{
	if (!node || halted())
		return 0;
	switch (node->kind) {
	case COMMAND: return run_command(node);
	case SEQUENCE: {
		const DWORD status = execute(node->left);
		return halted() ? status : execute(node->right);
	}
	case AND: {
		const DWORD status = execute(node->left);
		return status || halted() ? status : execute(node->right);
	}
	case OR: {
		const DWORD status = execute(node->left);
		return !status || halted() ? status : execute(node->right);
	}
	case PIPE: return run_pipe(node);
	case BLOCK: {
		struct Opened opened;
		if (!redirect(node->redirects, &opened)) {
			errorlevel = 1;
			return 1;
		}
		const DWORD status = execute(node->left);
		restore(&opened);
		return status;
	}
	case IF_COMMAND: return run_if(node);
	case FOR_COMMAND: return run_for(node);
	}
	return 0;
}

/* Where lines come from: the prompt, a batch file, or a command given whole. `more` asks for a line that continues the last one. */
typedef BOOL (*LineSource)(void *state, BOOL more, struct Text *line);

/* Whether a line ends with a caret outside quotes, which joins the next line to it. */
static BOOL continues(const struct Text *line)
{
	BOOL quoted = FALSE;
	for (DWORD index = 0; index < line->size; ++index)
		if (line->data[index] == L'"')
			quoted = !quoted;
	DWORD carets = 0;
	while (carets < line->size && line->data[line->size - 1 - carets] == L'^')
		++carets;
	return !quoted && (carets & 1);
}

/* Reads lines until they make a whole command: each expanded as it is read, joined after a final caret, and while a block or a FOR
   set stays open. NULL for an empty line, a syntax error (`*bad`) or the end of the input (`*ended`). */
static struct Node *read_logical(LineSource source, void *state, WCHAR **text, BOOL *ended, BOOL *bad)
{
	struct Text logical = {0};
	BOOL first = TRUE, join = FALSE;
	*ended = *bad = FALSE;
	*text = NULL;
	for (;;) {
		struct Text raw = {0};
		if (!source(state, !first, &raw)) {
			release(raw.data);
			if (first) {
				*ended = TRUE;
				return NULL;
			}
			complain(L"The syntax of the command is incorrect.\r\n");
			errorlevel = 255;
			*bad = TRUE;
			*text = text_take(&logical);
			return NULL;
		}
		WCHAR *expanded = expand_percent(raw.data ? raw.data : L"");
		release(raw.data);
		if (!first && !join)
			text_char(&logical, L'\n');
		text_put(&logical, expanded);
		release(expanded);
		first = FALSE;
		join = continues(&logical);
		if (join) {
			logical.data[--logical.size] = 0;
			continue;
		}
		BOOL more;
		struct Node *tree = parse_text(logical.data ? logical.data : L"", &more, bad);
		if (more)
			continue;
		*text = text_take(&logical);
		return tree;
	}
}

struct Once {
	const WCHAR *text;
	BOOL used;
};

static BOOL once_source(void *state, BOOL more, struct Text *line)
{
	struct Once *once = state;
	(void)more;
	if (once->used)
		return FALSE;
	once->used = TRUE;
	text_put(line, once->text);
	return TRUE;
}

/* Parses and runs a command given whole, as /C and /K give one. */
static DWORD run_text(const WCHAR *text)
{
	struct Once once = {text, FALSE};
	WCHAR *source;
	BOOL ended, bad;
	struct Node *tree = read_logical(once_source, &once, &source, &ended, &bad);
	const DWORD status = tree ? execute(tree) : (DWORD)bad;
	free_node(tree);
	release(source);
	return status;
}

static BOOL batch_source(void *state, BOOL more, struct Text *line)
{
	struct Batch *frame = state;
	(void)more;
	if (frame->next >= frame->size)
		return FALSE;
	DWORD end = frame->next;
	while (end < frame->size && frame->text[end] != L'\n')
		++end;
	DWORD stop = end;
	if (stop > frame->next && frame->text[stop - 1] == L'\r')
		--stop;
	text_add(line, frame->text + frame->next, stop - frame->next);
	frame->next = end < frame->size ? end + 1 : end;
	return TRUE;
}

/* SETLOCAL's saved state: the environment, the current folder and delayed expansion, for ENDLOCAL or the end of the batch file or
   CALL that made it. */
struct Local {
	struct Local *previous;
	struct Batch *owner;
	WCHAR *environment;
	WCHAR folder[MAX_PATH];
	BOOL delayed;
};
static struct Local *locals;

static WCHAR *environment_copy(void)
{
	WCHAR *block = GetEnvironmentStringsW();
	DWORD size = 0;
	while (block[size] || block[size + 1])
		++size;
	WCHAR *copy = duplicate(block, size + 1);
	FreeEnvironmentStringsW(block);
	return copy;
}

/* Puts back an environment: what is set now goes, then what the copy holds is set again. Names that start with = stay. */
static void environment_restore(const WCHAR *copy)
{
	WCHAR *now = environment_copy();
	for (WCHAR *entry = now; *entry; entry += length(entry) + 1) {
		if (*entry == L'=')
			continue;
		WCHAR *equals = entry;
		while (*equals && *equals != L'=')
			++equals;
		WCHAR *name = duplicate(entry, (DWORD)(equals - entry));
		SetEnvironmentVariableW(name, NULL);
		release(name);
	}
	release(now);
	for (const WCHAR *entry = copy; *entry; entry += length(entry) + 1) {
		if (*entry == L'=')
			continue;
		const WCHAR *equals = entry;
		while (*equals && *equals != L'=')
			++equals;
		if (!*equals)
			continue;
		WCHAR *name = duplicate(entry, (DWORD)(equals - entry));
		SetEnvironmentVariableW(name, equals + 1);
		release(name);
	}
}

static void end_local(void)
{
	struct Local *local = locals;
	locals = local->previous;
	environment_restore(local->environment);
	SetCurrentDirectoryW(local->folder);
	delayed = local->delayed;
	release(local->environment);
	release(local);
}

static DWORD command_setlocal(const WCHAR *rest)
{
	if (!batch)
		return 0;
	struct Local *local = allocate(sizeof *local);
	local->previous = locals;
	local->owner = batch;
	local->environment = environment_copy();
	GetCurrentDirectoryW(MAX_PATH, local->folder);
	local->delayed = delayed;
	locals = local;
	for (const WCHAR *at = rest; *at;) {
		while (delimiter(*at))
			++at;
		if (keyword(at, L"ENABLEDELAYEDEXPANSION", NULL))
			delayed = TRUE;
		else if (keyword(at, L"DISABLEDELAYEDEXPANSION", NULL))
			delayed = FALSE;
		else if (*at && !keyword(at, L"ENABLEEXTENSIONS", NULL) && !keyword(at, L"DISABLEEXTENSIONS", NULL))
			return failure(L"Invalid parameter to SETLOCAL command\r\n");
		while (*at && !delimiter(*at))
			++at;
	}
	errorlevel = 0;
	return 0;
}

static DWORD command_endlocal(const WCHAR *rest)
{
	(void)rest;
	if (batch && locals && locals->owner == batch)
		end_local();
	return 0;
}

/* Finds a label in a batch file, from the line after the current one to the end and then from the start, as GOTO and CALL look;
   the position after the label's line, or the file's size when there is none. */
static BOOL find_label(struct Batch *frame, const WCHAR *label, DWORD *position)
{
	const DWORD label_size = length(label);
	for (int pass = 0; pass < 2; ++pass) {
		const DWORD from = pass ? 0 : frame->next, to = pass ? frame->next : frame->size;
		for (DWORD at = from; at < to;) {
			DWORD end = at;
			while (end < frame->size && frame->text[end] != L'\n')
				++end;
			DWORD start = at;
			while (start < end && (delimiter(frame->text[start]) || frame->text[start] == L'@'))
				++start;
			if (start < end && frame->text[start] == L':') {
				++start;
				DWORD index = 0;
				while (index < label_size && start + index < end && folded(frame->text[start + index]) == folded(label[index]))
					++index;
				const DWORD stop = start + index;
				const WCHAR next = stop < end ? frame->text[stop] : 0;
				if (index == label_size && (!next || next == L'\r' || delimiter(next) || next == L':' || next == L'+' || next == L'&')) {
					*position = end < frame->size ? end + 1 : end;
					return TRUE;
				}
			}
			at = end + 1;
		}
	}
	return FALSE;
}

/* A label as GOTO and CALL name it: after its colon, up to a blank or an operator. */
static WCHAR *label_word(const WCHAR **rest)
{
	const WCHAR *at = *rest;
	while (delimiter(*at))
		++at;
	if (*at == L':')
		++at;
	const WCHAR *start = at;
	while (*at && !delimiter(*at) && *at != L'+' && *at != L':')
		++at;
	*rest = at;
	return duplicate(start, (DWORD)(at - start));
}

static DWORD command_goto(const WCHAR *rest)
{
	WCHAR *label = label_word(&rest);
	if (!*label) {
		release(label);
		return failure(L"No batch label specified to GOTO command.\r\n");
	}
	if (!batch) {
		release(label);
		return 0;
	}
	jumped = TRUE;
	DWORD position;
	if (equal(label, L"EOF"))
		batch->next = batch->size;
	else if (find_label(batch, label, &position))
		batch->next = position;
	else {
		complain(L"The system cannot find the batch label specified - ");
		complain(label);
		complain(L"\r\n");
		batch->next = batch->size;
		abandon = TRUE;
		errorlevel = 1;
	}
	release(label);
	return 0;
}

/* Splits a batch file's arguments into %1, %2 and on, at blanks, commas, semicolons and equals signs outside quotes; %* is all of
   them as they were given. */
static void batch_arguments(struct Batch *frame, const WCHAR *zero, const WCHAR *arguments)
{
	DWORD capacity = 16;
	frame->arguments = allocate(capacity * sizeof(WCHAR *));
	frame->arguments[0] = copy_of(zero);
	frame->count = 1;
	while (delimiter(*arguments))
		++arguments;
	frame->rest = copy_of(arguments);
	while (*arguments) {
		const WCHAR *start = arguments;
		BOOL quoted = FALSE;
		while (*arguments && (quoted || !delimiter(*arguments))) {
			if (*arguments == L'"')
				quoted = !quoted;
			++arguments;
		}
		if (frame->count == capacity) {
			WCHAR **grown = allocate(capacity * 2 * sizeof(WCHAR *));
			memcpy(grown, frame->arguments, capacity * sizeof(WCHAR *));
			release(frame->arguments);
			frame->arguments = grown;
			capacity *= 2;
		}
		frame->arguments[frame->count++] = duplicate(start, (DWORD)(arguments - start));
		while (delimiter(*arguments))
			++arguments;
	}
}

static void free_frame(struct Batch *frame)
{
	for (DWORD index = 0; index < frame->count; ++index)
		release(frame->arguments[index]);
	release(frame->arguments);
	release(frame->rest);
	if (frame->owns_text)
		release(frame->text);
	release(frame->file);
	release(frame);
}

static void show_prompt(void);

/* After Ctrl+C in a batch file, asks whether to give it up. */
static BOOL terminate_batch(void)
{
	interrupted = 0;
	WCHAR answer[64];
	DWORD count = 0, mode;
	HANDLE console = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (console == INVALID_HANDLE_VALUE || !GetConsoleMode(console, &mode)) {
		if (console != INVALID_HANDLE_VALUE)
			CloseHandle(console);
		return TRUE;
	}
	for (;;) {
		complain(L"Terminate batch job (Y/N)? ");
		if (!ReadConsoleW(console, answer, 63, &count, NULL) || !count) {
			CloseHandle(console);
			return TRUE;
		}
		if (folded(answer[0]) == L'Y' || folded(answer[0]) == L'N')
			break;
	}
	CloseHandle(console);
	return folded(answer[0]) == L'Y';
}

/* Runs a batch file, or a label CALL runs in one, line by line until its end, EXIT /B or GOTO :EOF; then the SETLOCALs it left end. */
static void run_frame(struct Batch *frame)
{
	struct Batch *caller = batch;
	batch = frame;
	while (!leaving && !leaving_batch && !abandon && frame->next < frame->size) {
		if (interrupted && terminate_batch()) {
			abandon = TRUE;
			break;
		}
		jumped = FALSE;
		WCHAR *text;
		BOOL ended, bad;
		struct Node *tree = read_logical(batch_source, frame, &text, &ended, &bad);
		if (ended)
			break;
		if (bad) {
			abandon = TRUE;
			release(text);
			break;
		}
		const WCHAR *shown = text;
		while (blank(*shown))
			++shown;
		/* With ECHO on, each command shows after the prompt, except one that starts with @, and labels. */
		if (echo_on && tree && *shown != L'@' && *shown != L':') {
			say(L"\r\n");
			show_prompt();
			say(shown);
			say(L"\r\n");
		}
		execute(tree);
		free_node(tree);
		release(text);
	}
	while (locals && locals->owner == frame)
		end_local();
	leaving_batch = FALSE;
	jumped = FALSE;
	batch = caller;
	if (!batch)
		abandon = FALSE;
}

/* Runs a batch file with its arguments, then comes back to what called it. */
static DWORD run_batch(const WCHAR *path, const WCHAR *arguments)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return failure(L"The batch file cannot be found.\r\n");
	DWORD bytes, count;
	char *raw = read_all(file, &bytes);
	CloseHandle(file);
	struct Batch *frame = allocate(sizeof *frame);
	frame->text = decoded(raw, bytes, &count);
	frame->size = count;
	frame->owns_text = TRUE;
	release(raw);
	WCHAR full[MAX_PATH];
	frame->file = copy_of(GetFullPathNameW(path, MAX_PATH, full, NULL) ? full : path);
	batch_arguments(frame, path, arguments);
	const BOOL outermost = !batch, echo = echo_on;
	run_frame(frame);
	free_frame(frame);
	/* At the prompt, ECHO is as it was before the batch file ran. */
	if (outermost && prompting)
		echo_on = echo;
	return errorlevel;
}

/* A batch file that a command names: one CALL names returns to its caller; otherwise it takes the caller's place. */
static DWORD invoke_batch(const WCHAR *path, const WCHAR *arguments)
{
	const BOOL called = calling;
	struct Batch *caller = batch;
	calling = FALSE;
	const DWORD status = run_batch(path, arguments);
	if (caller && !called) {
		caller->next = caller->size;
		jumped = TRUE;
	}
	return status;
}

/* CALL :label runs a part of the batch file with arguments of its own, until GOTO :EOF or EXIT /B. */
static DWORD call_label(const WCHAR *rest)
{
	const WCHAR *after = rest;
	WCHAR *label = label_word(&after);
	DWORD position;
	if (!find_label(batch, label, &position)) {
		complain(L"The system cannot find the batch label specified - ");
		complain(label);
		complain(L"\r\n");
		release(label);
		errorlevel = 1;
		return 1;
	}
	struct Batch *frame = allocate(sizeof *frame);
	frame->text = batch->text;
	frame->size = batch->size;
	frame->next = position;
	frame->file = copy_of(batch->file);
	struct Text zero = {0};
	text_char(&zero, L':');
	text_put(&zero, label);
	release(label);
	batch_arguments(frame, zero.data, after);
	release(zero.data);
	run_frame(frame);
	free_frame(frame);
	jumped = FALSE;
	return errorlevel;
}

static DWORD command_call(const WCHAR *rest)
{
	while (blank(*rest))
		++rest;
	if (!*rest)
		return 0;
	if (*rest == L':') {
		if (!batch)
			return failure(L"Invalid attempt to call batch label outside of batch script.\r\n");
		return call_label(rest);
	}
	/* The command is expanded once more, so that %%NAME%% in a batch file is the variable as CALL runs. */
	WCHAR *again = expand_percent(rest);
	calling = TRUE;
	const DWORD status = run_simple(again);
	calling = FALSE;
	release(again);
	return status;
}

static DWORD command_shift(const WCHAR *rest)
{
	if (!batch)
		return 0;
	DWORD from = 0;
	while (blank(*rest))
		++rest;
	if (*rest == L'/') {
		if (!digit(rest[1]) || rest[1] == L'0')
			return syntax_error();
		from = (DWORD)(rest[1] - L'0');
	}
	if (from >= batch->count)
		return 0;
	release(batch->arguments[from]);
	for (DWORD index = from; index + 1 < batch->count; ++index)
		batch->arguments[index] = batch->arguments[index + 1];
	--batch->count;
	return 0;
}

static DWORD command_exit(const WCHAR *rest)
{
	while (blank(*rest))
		++rest;
	if (keyword(rest, L"/B", NULL)) {
		rest += 2;
		if (batch)
			leaving_batch = TRUE;
		else
			leaving = TRUE;
	} else
		leaving = TRUE;
	while (blank(*rest))
		++rest;
	if (*rest)
		errorlevel = (DWORD)leading_number(rest);
	return 0;
}

/* Reads a line from the input, without its line end: a console's own line, or bytes up to a line feed. FALSE at the end of it. */
static BOOL read_input_line(struct Text *line)
{
	DWORD mode, count;
	if (GetConsoleMode(input, &mode)) {
		WCHAR buffer[512];
		BOOL any = FALSE;
		for (;;) {
			if (!ReadConsoleW(input, buffer, 512, &count, NULL) || !count)
				break;
			any = TRUE;
			text_add(line, buffer, count);
			if (buffer[count - 1] == L'\n')
				break;
		}
		if (!any)
			return FALSE;
	} else {
		/* Byte by byte, so that a program run after this line reads what follows it. */
		char *bytes = allocate(LINE_LIMIT);
		DWORD size = 0, got;
		while (size < LINE_LIMIT && ReadFile(input, bytes + size, 1, &got, NULL) && got)
			if (bytes[size++] == '\n')
				break;
		if (!size) {
			release(bytes);
			return FALSE;
		}
		WCHAR *text = allocate(((SIZE_T)size + 1) * sizeof(WCHAR));
		const int characters = MultiByteToWideChar(GetConsoleCP(), 0, bytes, (int)size, text, (int)size);
		text_add(line, text, characters > 0 ? (DWORD)characters : 0);
		release(text);
		release(bytes);
	}
	while (line->size && (line->data[line->size - 1] == L'\n' || line->data[line->size - 1] == L'\r'))
		line->data[--line->size] = 0;
	if (!line->data)
		text_add(line, L"", 0);
	return TRUE;
}

/* Asks a question and returns the first letter of the answer, folded; 0 at the end of the input. */
static WCHAR ask(const WCHAR *question, const WCHAR *subject)
{
	struct Text line = {0};
	if (subject)
		say(subject);
	say(question);
	WCHAR answer = 0;
	if (read_input_line(&line)) {
		const WCHAR *at = line.data;
		while (blank(*at))
			++at;
		answer = folded(*at);
	}
	release(line.data);
	return answer;
}

/* The words of a command's arguments, split at blanks outside quotes; quotes stay. */
struct Words {
	WCHAR **items;
	DWORD count;
};

static void split_words(const WCHAR *text, struct Words *words, BOOL commas)
{
	DWORD capacity = 8;
	words->items = allocate(capacity * sizeof(WCHAR *));
	words->count = 0;
	for (const WCHAR *at = text; *at;) {
		while (blank(*at) || (commas && (*at == L',' || *at == L';')))
			++at;
		if (!*at)
			break;
		const WCHAR *start = at;
		BOOL quoted = FALSE;
		while (*at && (quoted || !(blank(*at) || (commas && (*at == L',' || *at == L';'))))) {
			if (*at == L'"')
				quoted = !quoted;
			++at;
			/* A switch ends where the next one starts: /b/s is /b and /s. */
			if (!quoted && *start == L'/' && *at == L'/')
				break;
		}
		if (words->count == capacity) {
			WCHAR **grown = allocate(capacity * 2 * sizeof(WCHAR *));
			memcpy(grown, words->items, capacity * sizeof(WCHAR *));
			release(words->items);
			words->items = grown;
			capacity *= 2;
		}
		words->items[words->count++] = duplicate(start, (DWORD)(at - start));
	}
}

static void free_words(struct Words *words)
{
	for (DWORD index = 0; index < words->count; ++index)
		release(words->items[index]);
	release(words->items);
}

static BOOL is_file(const WCHAR *path)
{
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL is_folder(const WCHAR *path)
{
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL has_extension(const WCHAR *name)
{
	const WCHAR *dot = NULL;
	for (const WCHAR *at = name; *at; ++at)
		if (*at == L'.')
			dot = at;
		else if (*at == L'\\' || *at == L'/' || *at == L':')
			dot = NULL;
	return dot != NULL;
}

/* Tries a name as it is when it has an extension, else with each extension of PATHEXT. */
static BOOL try_extensions(WCHAR *candidate, DWORD stem)
{
	WCHAR extensions[1024];
	if (has_extension(candidate) && is_file(candidate))
		return TRUE;
	if (!GetEnvironmentVariableW(L"PATHEXT", extensions, 1024))
		lstrcpyW(extensions, L".COM;.EXE;.BAT;.CMD");
	for (WCHAR *start = extensions; *start;) {
		WCHAR *end = start;
		while (*end && *end != L';')
			++end;
		DWORD size = stem;
		for (WCHAR *copy = start; copy < end && size < MAX_PATH - 1; ++copy)
			candidate[size++] = *copy;
		candidate[size] = 0;
		if (end > start && is_file(candidate))
			return TRUE;
		candidate[stem] = 0;
		start = *end ? end + 1 : end;
	}
	return FALSE;
}

/* Finds the program a command names: by its path when it has one, else in the current folder and then the folders of PATH. */
static BOOL find_program(const WCHAR *name, WCHAR *found)
{
	BOOL has_path = FALSE;
	for (const WCHAR *at = name; *at; ++at)
		if (*at == L'\\' || *at == L'/' || *at == L':')
			has_path = TRUE;
	if (!*name)
		return FALSE;
	if (has_path) {
		lstrcpynW(found, name, MAX_PATH);
		return try_extensions(found, length(found));
	}
	WCHAR *folders = allocate(32768 * sizeof(WCHAR));
	DWORD folder_size = GetCurrentDirectoryW(MAX_PATH, folders);
	folders[folder_size++] = L';';
	folder_size += GetEnvironmentVariableW(L"PATH", folders + folder_size, 32768 - folder_size);
	BOOL success = FALSE;
	for (WCHAR *start = folders; !success && start < folders + folder_size;) {
		WCHAR *end = start;
		while (end < folders + folder_size && *end != L';')
			++end;
		DWORD size = 0;
		for (WCHAR *copy = start; copy < end && size < MAX_PATH - 2; ++copy)
			if (*copy != L'"')
				found[size++] = *copy;
		if (size) {
			if (found[size - 1] != L'\\')
				found[size++] = L'\\';
			for (const WCHAR *copy = name; *copy && size < MAX_PATH - 1; ++copy)
				found[size++] = *copy;
			found[size] = 0;
			success = try_extensions(found, size);
		}
		start = end + 1;
	}
	release(folders);
	return success;
}

static BOOL batch_file(const WCHAR *path)
{
	const DWORD size = length(path);
	return size > 4 && (equal(path + size - 4, L".BAT") || equal(path + size - 4, L".CMD"));
}

/* The first word of a command: up to a blank, or quoted. `rest` is what follows it. */
static void first_word(const WCHAR *text, WCHAR *word, DWORD limit, const WCHAR **rest)
{
	DWORD out = 0;
	BOOL quoted = FALSE;
	while (blank(*text))
		++text;
	for (; *text && (quoted || !blank(*text)); ++text) {
		if (*text == L'"') {
			quoted = !quoted;
			continue;
		}
		if (out < limit - 1)
			word[out++] = *text;
	}
	word[out] = 0;
	*rest = text;
}

/* The prompt from PROMPT, $P$G when it is not set, with the codes PROMPT /? documents. */
static DWORD pushd_depth;

static void show_prompt(void)
{
	WCHAR format[256], folder[MAX_PATH];
	struct Text text = {0};
	if (!GetEnvironmentVariableW(L"PROMPT", format, 256))
		lstrcpyW(format, L"$P$G");
	GetCurrentDirectoryW(MAX_PATH, folder);
	for (const WCHAR *at = format; *at; ++at) {
		if (*at != L'$' || !at[1]) {
			text_char(&text, *at);
			continue;
		}
		WCHAR *value;
		switch (folded(*++at)) {
		case L'P': text_put(&text, folder); break;
		case L'G': text_char(&text, L'>'); break;
		case L'L': text_char(&text, L'<'); break;
		case L'B': text_char(&text, L'|'); break;
		case L'Q': text_char(&text, L'='); break;
		case L'A': text_char(&text, L'&'); break;
		case L'C': text_char(&text, L'('); break;
		case L'F': text_char(&text, L')'); break;
		case L'N': text_add(&text, folder, 1); break;
		case L'_': text_put(&text, L"\r\n"); break;
		case L'S': text_char(&text, L' '); break;
		case L'E': text_char(&text, 27); break;
		case L'H': text_char(&text, 8); break;
		case L'$': text_char(&text, L'$'); break;
		case L'V': text_put(&text, L"Raven Command Processor [Version 0.2]"); break;
		case L'+':
			for (DWORD index = 0; index < pushd_depth; ++index)
				text_char(&text, L'+');
			break;
		case L'D':
			value = dynamic_value(L"DATE");
			text_put(&text, value);
			release(value);
			break;
		case L'T':
			value = dynamic_value(L"TIME");
			text_put(&text, value);
			release(value);
			break;
		default: break;
		}
	}
	say_text(&text);
	release(text.data);
}

static DWORD command_echo(const WCHAR *rest)
{
	/* The character right after ECHO only separates: ECHO. and ECHO( print an empty line. */
	if (*rest && !blank(*rest)) {
		say(rest + 1);
		say(L"\r\n");
		return 0;
	}
	const WCHAR *text = *rest ? rest + 1 : rest;
	WCHAR *word = trimmed(text);
	if (!*word)
		say(echo_on ? L"ECHO is on.\r\n" : L"ECHO is off.\r\n");
	else if (equal(word, L"ON") || equal(word, L"OFF"))
		echo_on = equal(word, L"ON");
	else {
		say(text);
		say(L"\r\n");
	}
	release(word);
	return 0;
}

/* The folder each drive was last in, as cmd keeps it in the variables =C:, =D: and so on. */
static void remember_drive(void)
{
	WCHAR folder[MAX_PATH], name[4] = {L'=', 0, L':', 0};
	if (GetCurrentDirectoryW(MAX_PATH, folder) && folder[1] == L':') {
		name[1] = folded(folder[0]);
		SetEnvironmentVariableW(name, folder);
	}
}

static DWORD change_drive(WCHAR drive)
{
	WCHAR name[4] = {L'=', folded(drive), L':', 0}, root[4] = {folded(drive), L':', L'\\', 0}, folder[MAX_PATH];
	remember_drive();
	if (GetEnvironmentVariableW(name, folder, MAX_PATH) && SetCurrentDirectoryW(folder))
		return 0;
	if (!SetCurrentDirectoryW(root))
		return failure(L"The system cannot find the drive specified.\r\n");
	return 0;
}

static DWORD command_cd(const WCHAR *rest)
{
	WCHAR folder[MAX_PATH];
	BOOL change_drive_too = FALSE;
	while (blank(*rest))
		++rest;
	if (keyword(rest, L"/D", NULL)) {
		change_drive_too = TRUE;
		rest += 2;
	}
	WCHAR *target = unquoted(rest), *clean = trimmed(target);
	release(target);
	if (!*clean) {
		GetCurrentDirectoryW(MAX_PATH, folder);
		say(folder);
		say(L"\r\n");
		release(clean);
		return 0;
	}
	GetCurrentDirectoryW(MAX_PATH, folder);
	/* CD X: shows that drive's folder; CD X:\path without /D sets it, but stays on this drive. */
	if (clean[1] == L':' && folded(clean[0]) != folded(folder[0])) {
		WCHAR name[4] = {L'=', folded(clean[0]), L':', 0}, full[MAX_PATH];
		if (!clean[2]) {
			if (!GetEnvironmentVariableW(name, full, MAX_PATH)) {
				full[0] = folded(clean[0]);
				full[1] = L':';
				full[2] = L'\\';
				full[3] = 0;
			}
			say(full);
			say(L"\r\n");
			release(clean);
			return 0;
		}
		if (!change_drive_too) {
			if (!GetFullPathNameW(clean, MAX_PATH, full, NULL) || !is_folder(full)) {
				release(clean);
				return failure(L"The system cannot find the path specified.\r\n");
			}
			SetEnvironmentVariableW(name, full);
			release(clean);
			return 0;
		}
		remember_drive();
	}
	const BOOL changed = SetCurrentDirectoryW(clean), file = !changed && is_file(clean);
	release(clean);
	if (!changed)
		return failure(file ? L"The directory name is invalid.\r\n" : L"The system cannot find the path specified.\r\n");
	remember_drive();
	return 0;
}

/* SET /A: integer arithmetic on 32-bit numbers, with the operators, precedence and assignments SET /? documents. */
static const WCHAR *arith_at;
static const WCHAR *arith_error;

static BOOL arith_name_character(WCHAR character)
{
	return character && !blank(character) && !digit(character) && !(character == L'(' || character == L')' || character == L'!' ||
		character == L'~' || character == L'*' || character == L'/' || character == L'%' || character == L'+' || character == L'-' ||
		character == L'<' || character == L'>' || character == L'&' || character == L'^' || character == L'|' || character == L'=' ||
		character == L',');
}

static void arith_skip(void)
{
	while (blank(*arith_at))
		++arith_at;
}

static void arith_fail(const WCHAR *message)
{
	if (!arith_error)
		arith_error = message;
}

static WCHAR *arith_name(void)
{
	const WCHAR *start = arith_at;
	while (arith_name_character(*arith_at) || (arith_at > start && digit(*arith_at)))
		++arith_at;
	return duplicate(start, (DWORD)(arith_at - start));
}

static LONG arith_variable(const WCHAR *name)
{
	WCHAR *value = environment_value(name);
	LONG number = 0;
	if (value) {
		WCHAR *clean = trimmed(value);
		if (!read_integer(clean, &number))
			number = leading_number(clean);
		release(clean);
	}
	release(value);
	return number;
}

static void arith_store(const WCHAR *name, LONG value)
{
	struct Text text = {0};
	put_signed(&text, value);
	SetEnvironmentVariableW(name, text.data);
	release(text.data);
}

static LONG arith_comma(void);

static LONG arith_primary(void)
{
	arith_skip();
	const WCHAR character = *arith_at;
	if (character == L'(') {
		++arith_at;
		const LONG value = arith_comma();
		arith_skip();
		if (*arith_at != L')')
			arith_fail(L"Missing closing parenthesis.\r\n");
		else
			++arith_at;
		return value;
	}
	if (character == L'-') {
		++arith_at;
		return -arith_primary();
	}
	if (character == L'+') {
		++arith_at;
		return arith_primary();
	}
	if (character == L'!') {
		++arith_at;
		return !arith_primary();
	}
	if (character == L'~') {
		++arith_at;
		return ~arith_primary();
	}
	if (digit(character)) {
		const WCHAR *start = arith_at;
		while (digit(*arith_at) || letter(*arith_at))
			++arith_at;
		WCHAR *number = duplicate(start, (DWORD)(arith_at - start));
		LONG value = 0;
		if (!read_integer(number, &value))
			arith_fail(L"Invalid number.  Numeric constants are either decimal (17),\r\nhexadecimal (0x11), or octal (021).\r\n");
		release(number);
		return value;
	}
	if (arith_name_character(character)) {
		WCHAR *name = arith_name();
		const LONG value = arith_variable(name);
		release(name);
		return value;
	}
	arith_fail(L"Missing operand.\r\n");
	return 0;
}

/* Whether the operator at the cursor is `op` and not the start of an assignment like *= . */
static BOOL arith_operator(const WCHAR *op)
{
	arith_skip();
	const DWORD size = length(op);
	for (DWORD index = 0; index < size; ++index)
		if (arith_at[index] != op[index])
			return FALSE;
	if (arith_at[size] == L'=')
		return FALSE;
	arith_at += size;
	return TRUE;
}

static LONG arith_multiply(void)
{
	LONG value = arith_primary();
	for (;;) {
		if (arith_operator(L"*"))
			value *= arith_primary();
		else if (arith_operator(L"/") || arith_operator(L"%")) {
			const BOOL remainder = arith_at[-1] == L'%';
			const LONG divisor = arith_primary();
			if (!divisor) {
				arith_fail(L"Divide by zero error.\r\n");
				return 0;
			}
			value = remainder ? value % divisor : value / divisor;
		} else
			return value;
	}
}

static LONG arith_add(void)
{
	LONG value = arith_multiply();
	for (;;) {
		if (arith_operator(L"+"))
			value += arith_multiply();
		else if (arith_operator(L"-"))
			value -= arith_multiply();
		else
			return value;
	}
}

static LONG arith_shift(void)
{
	LONG value = arith_add();
	for (;;) {
		if (arith_operator(L"<<"))
			value = (LONG)((ULONG)value << (arith_add() & 31));
		else if (arith_operator(L">>"))
			value >>= arith_add() & 31;
		else
			return value;
	}
}

static LONG arith_and(void)
{
	LONG value = arith_shift();
	while (arith_operator(L"&"))
		value &= arith_shift();
	return value;
}

static LONG arith_xor(void)
{
	LONG value = arith_and();
	while (arith_operator(L"^"))
		value ^= arith_and();
	return value;
}

static LONG arith_or(void)
{
	LONG value = arith_xor();
	while (arith_operator(L"|"))
		value |= arith_xor();
	return value;
}

static LONG arith_assign(void)
{
	arith_skip();
	const WCHAR *start = arith_at;
	if (arith_name_character(*arith_at)) {
		WCHAR *name = arith_name();
		arith_skip();
		static const WCHAR *const assignments[] = {L"<<=", L">>=", L"*=", L"/=", L"%=", L"+=", L"-=", L"&=", L"^=", L"|=", L"="};
		for (int index = 0; index < 11; ++index) {
			const WCHAR *op = assignments[index];
			const DWORD size = length(op);
			BOOL match = TRUE;
			for (DWORD at = 0; at < size && match; ++at)
				match = arith_at[at] == op[at];
			if (!match || (size == 1 && arith_at[1] == L'='))
				continue;
			arith_at += size;
			const LONG right = arith_assign();
			LONG value = arith_variable(name);
			switch (op[0]) {
			case L'<': value = (LONG)((ULONG)value << (right & 31)); break;
			case L'>': value >>= right & 31; break;
			case L'*': value *= right; break;
			case L'/':
			case L'%':
				if (!right) {
					arith_fail(L"Divide by zero error.\r\n");
					release(name);
					return 0;
				}
				value = op[0] == L'/' ? value / right : value % right;
				break;
			case L'+': value += right; break;
			case L'-': value -= right; break;
			case L'&': value &= right; break;
			case L'^': value ^= right; break;
			case L'|': value |= right; break;
			default: value = right; break;
			}
			if (!arith_error)
				arith_store(name, value);
			release(name);
			return value;
		}
		release(name);
		arith_at = start;
	}
	return arith_or();
}

static LONG arith_comma(void)
{
	LONG value = arith_assign();
	for (;;) {
		arith_skip();
		if (*arith_at != L',')
			return value;
		++arith_at;
		value = arith_assign();
	}
}

static DWORD set_arithmetic(const WCHAR *expression)
{
	WCHAR *text = unquoted(expression);
	arith_at = text;
	arith_error = NULL;
	const LONG value = arith_comma();
	arith_skip();
	if (!arith_error && *arith_at)
		arith_fail(*arith_at == L')' ? L"Unbalanced parenthesis.\r\n" : L"Missing operator.\r\n");
	release(text);
	if (arith_error) {
		complain(arith_error);
		errorlevel = 1073750988;
		return 1;
	}
	/* At the prompt, SET /A shows the value of its last expression. */
	if (!batch) {
		struct Text shown = {0};
		put_signed(&shown, value);
		say_text(&shown);
		release(shown.data);
	}
	return 0;
}

static DWORD command_set(const WCHAR *rest)
{
	while (blank(*rest))
		++rest;
	if (keyword(rest, L"/A", L"\""))
		return set_arithmetic(rest + 2);
	BOOL prompt = FALSE;
	if (keyword(rest, L"/P", L"\"")) {
		prompt = TRUE;
		rest += 2;
		while (blank(*rest))
			++rest;
	}
	/* SET "name=value": what is inside the quotes, up to the last one. */
	WCHAR *assignment;
	if (*rest == L'"') {
		DWORD last = length(rest);
		while (last > 1 && rest[last - 1] != L'"')
			--last;
		assignment = last > 1 ? duplicate(rest + 1, last - 2) : copy_of(rest + 1);
	} else
		assignment = copy_of(rest);
	WCHAR *equals = assignment;
	while (*equals && *equals != L'=')
		++equals;
	if (*equals == L'=') {
		if (equals == assignment) {
			release(assignment);
			return syntax_error();
		}
		*equals = 0;
		if (prompt) {
			const WCHAR *question = equals + 1;
			WCHAR *shown = NULL;
			const DWORD size = length(question);
			if (size >= 2 && question[0] == L'"' && question[size - 1] == L'"')
				shown = duplicate(question + 1, size - 2);
			say(shown ? shown : question);
			release(shown);
			struct Text line = {0};
			const BOOL read = read_input_line(&line);
			if (read && line.size)
				SetEnvironmentVariableW(assignment, line.data);
			release(line.data);
			release(assignment);
			if (!read || !line.size) {
				errorlevel = 1;
				return 1;
			}
			return 0;
		}
		SetEnvironmentVariableW(assignment, equals[1] ? equals + 1 : NULL);
		release(assignment);
		return 0;
	}
	if (prompt) {
		release(assignment);
		return syntax_error();
	}
	/* SET alone lists every variable in order of their names; SET name lists those whose names start with it. */
	WCHAR *block = environment_copy();
	DWORD count = 0;
	for (WCHAR *entry = block; *entry; entry += length(entry) + 1)
		++count;
	const WCHAR **entries = allocate((count + 1) * sizeof(WCHAR *));
	count = 0;
	for (WCHAR *entry = block; *entry; entry += length(entry) + 1)
		if (*entry != L'=' && starts(entry, assignment))
			entries[count++] = entry;
	for (DWORD index = 1; index < count; ++index)
		for (DWORD back = index; back && compare_folded(entries[back - 1], entries[back]) > 0; --back) {
			const WCHAR *swap = entries[back];
			entries[back] = entries[back - 1];
			entries[back - 1] = swap;
		}
	struct Text listing = {0};
	for (DWORD index = 0; index < count; ++index) {
		text_put(&listing, entries[index]);
		text_put(&listing, L"\r\n");
		if (listing.size > 4096)
			say_text(&listing);
	}
	say_text(&listing);
	release(listing.data);
	release(entries);
	release(block);
	if (*assignment && !count) {
		complain(L"Environment variable ");
		complain(assignment);
		complain(L" not defined\r\n");
		release(assignment);
		errorlevel = 1;
		return 1;
	}
	release(assignment);
	return 0;
}

static DWORD command_cls(const WCHAR *rest)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	COORD origin = {0, 0};
	DWORD written;
	(void)rest;
	if (!GetConsoleScreenBufferInfo(output, &info)) {
		say(L"\f");
		return 0;
	}
	FillConsoleOutputCharacterW(output, L' ', (DWORD)(info.dwSize.X * info.dwSize.Y), origin, &written);
	FillConsoleOutputAttribute(output, info.wAttributes, (DWORD)(info.dwSize.X * info.dwSize.Y), origin, &written);
	SetConsoleCursorPosition(output, origin);
	return 0;
}

static DWORD command_ver(const WCHAR *rest)
{
	(void)rest;
	say(L"\r\nRaven Command Processor [Version 0.2]\r\n");
	return 0;
}

static void volume_lines(const WCHAR *path)
{
	WCHAR root[4] = {path[0], L':', L'\\', 0}, label[MAX_PATH + 1];
	DWORD serial = 0;
	struct Text text = {0};
	if (!GetVolumeInformationW(root, label, MAX_PATH + 1, &serial, NULL, NULL, NULL, 0))
		label[0] = 0;
	text_put(&text, L" Volume in drive ");
	text_char(&text, folded(path[0]));
	if (label[0]) {
		text_put(&text, L" is ");
		text_put(&text, label);
		text_put(&text, L"\r\n");
	} else
		text_put(&text, L" has no label.\r\n");
	text_put(&text, L" Volume Serial Number is ");
	put_hex(&text, serial >> 16, 4);
	text_char(&text, L'-');
	put_hex(&text, serial & 0xffff, 4);
	text_put(&text, L"\r\n");
	say_text(&text);
	release(text.data);
}

static DWORD command_vol(const WCHAR *rest)
{
	WCHAR folder[MAX_PATH];
	WCHAR *drive = trimmed(rest);
	if (*drive && drive[1] == L':') {
		folder[0] = drive[0];
		folder[1] = L':';
		folder[2] = 0;
	} else
		GetCurrentDirectoryW(MAX_PATH, folder);
	release(drive);
	volume_lines(folder);
	return 0;
}

static DWORD command_title(const WCHAR *rest)
{
	while (blank(*rest))
		++rest;
	SetConsoleTitleW(rest);
	return 0;
}

static DWORD command_pause(const WCHAR *rest)
{
	DWORD mode, count;
	WCHAR key;
	(void)rest;
	say(L"Press any key to continue . . . ");
	if (GetConsoleMode(input, &mode)) {
		SetConsoleMode(input, mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
		ReadConsoleW(input, &key, 1, &count, NULL);
		SetConsoleMode(input, mode);
	} else {
		char byte;
		ReadFile(input, &byte, 1, &count, NULL);
	}
	say(L"\r\n");
	return 0;
}

static DWORD command_path(const WCHAR *rest)
{
	while (blank(*rest))
		++rest;
	if (*rest == L'=')
		++rest;
	if (!*rest) {
		WCHAR *path = environment_value(L"PATH");
		say(path ? L"PATH=" : L"No Path");
		say(path ? path : L"");
		say(L"\r\n");
		release(path);
	} else if (same(rest, L";"))
		SetEnvironmentVariableW(L"PATH", NULL);
	else
		SetEnvironmentVariableW(L"PATH", rest);
	return 0;
}

static DWORD command_prompt(const WCHAR *rest)
{
	while (blank(*rest))
		++rest;
	if (*rest == L'=')
		++rest;
	SetEnvironmentVariableW(L"PROMPT", *rest ? rest : L"$P$G");
	return 0;
}

static DWORD command_color(const WCHAR *rest)
{
	WCHAR *value = trimmed(rest);
	WORD attribute = 0x07;
	if (*value) {
		LONG number = 0;
		struct Text hex = {0};
		text_put(&hex, L"0x");
		text_put(&hex, value);
		const BOOL valid = length(value) <= 2 && read_integer(hex.data, &number);
		release(hex.data);
		if (!valid || (number >> 4) == (number & 15)) {
			release(value);
			errorlevel = 1;
			return 1;
		}
		attribute = (WORD)number;
	}
	release(value);
	SetConsoleTextAttribute(output, attribute);
	return 0;
}

/* DATE and TIME show the date or time; without /T they ask for a new one, which only an administrator could set. */
static DWORD date_or_time(const WCHAR *rest, BOOL date)
{
	SYSTEMTIME now;
	struct Text text = {0};
	WCHAR *switches = trimmed(rest);
	GetLocalTime(&now);
	const BOOL brief = equal(switches, L"/T");
	if (*switches && !brief) {
		release(switches);
		return failure(L"A required privilege is not held by the client.\r\n");
	}
	release(switches);
	WCHAR *value = dynamic_value(date ? L"DATE" : L"TIME");
	if (brief) {
		if (date)
			text_put(&text, value);
		else
			put_clock(&text, &now);
		text_put(&text, L"\r\n");
		say_text(&text);
	} else {
		text_put(&text, date ? L"The current date is: " : L"The current time is: ");
		text_put(&text, value);
		text_put(&text, date ? L"\r\nEnter the new date: (mm-dd-yy) " : L"\r\nEnter the new time: ");
		say_text(&text);
		struct Text answer = {0};
		const BOOL read = read_input_line(&answer);
		const BOOL given = read && answer.size;
		release(answer.data);
		if (given) {
			release(value);
			release(text.data);
			return failure(L"A required privilege is not held by the client.\r\n");
		}
	}
	release(value);
	release(text.data);
	return 0;
}

static DWORD command_date(const WCHAR *rest) { return date_or_time(rest, TRUE); }
static DWORD command_time(const WCHAR *rest) { return date_or_time(rest, FALSE); }

static BOOL verify_on;

static DWORD command_verify(const WCHAR *rest)
{
	WCHAR *word = trimmed(rest);
	if (!*word)
		say(verify_on ? L"VERIFY is on.\r\n" : L"VERIFY is off.\r\n");
	else if (equal(word, L"ON") || equal(word, L"OFF"))
		verify_on = equal(word, L"ON");
	else {
		release(word);
		return failure(L"Must specify ON or OFF.\r\n");
	}
	release(word);
	return 0;
}

static DWORD command_nothing(const WCHAR *rest)
{
	(void)rest;
	return 0;
}

/* PUSHD keeps the folders it left, newest first, for POPD to go back to. */
struct Pushed {
	struct Pushed *previous;
	WCHAR folder[MAX_PATH];
};
static struct Pushed *pushed;

static DWORD command_pushd(const WCHAR *rest)
{
	WCHAR *target = unquoted(rest), *clean = trimmed(target);
	release(target);
	if (!*clean) {
		for (struct Pushed *entry = pushed; entry; entry = entry->previous) {
			say(entry->folder);
			say(L"\r\n");
		}
		release(clean);
		return 0;
	}
	struct Pushed *entry = allocate(sizeof *entry);
	GetCurrentDirectoryW(MAX_PATH, entry->folder);
	remember_drive();
	if (!SetCurrentDirectoryW(clean)) {
		release(entry);
		release(clean);
		return failure(L"The system cannot find the path specified.\r\n");
	}
	release(clean);
	remember_drive();
	entry->previous = pushed;
	pushed = entry;
	++pushd_depth;
	return 0;
}

static DWORD command_popd(const WCHAR *rest)
{
	(void)rest;
	struct Pushed *entry = pushed;
	if (!entry)
		return 0;
	pushed = entry->previous;
	--pushd_depth;
	SetCurrentDirectoryW(entry->folder);
	remember_drive();
	release(entry);
	return 0;
}

/* START ["title"] [/D folder] [/WAIT] [/B] [other options] [command [arguments]]: a program started without waiting for it, unless
   /WAIT; a name that is not a program opens with what its type is associated with. */
static DWORD command_start(const WCHAR *rest)
{
	const WCHAR *at = rest;
	BOOL wait = FALSE;
	WCHAR *folder = NULL;
	while (blank(*at))
		++at;
	if (*at == L'"') {
		++at;
		while (*at && *at != L'"')
			++at;
		if (*at)
			++at;
	}
	for (;;) {
		while (blank(*at))
			++at;
		if (*at != L'/')
			break;
		const WCHAR *start = at;
		while (*at && !blank(*at))
			++at;
		WCHAR *option = duplicate(start, (DWORD)(at - start));
		if (equal(option, L"/WAIT"))
			wait = TRUE;
		else if (starts(option, L"/D")) {
			release(folder);
			if (option[2])
				folder = unquoted(option + 2);
			else {
				while (blank(*at))
					++at;
				const WCHAR *path = at;
				BOOL quoted = FALSE;
				while (*at && (quoted || !blank(*at))) {
					if (*at == L'"')
						quoted = !quoted;
					++at;
				}
				WCHAR *word = duplicate(path, (DWORD)(at - path));
				folder = unquoted(word);
				release(word);
			}
		} else if (equal(option, L"/NODE") || equal(option, L"/AFFINITY")) {
			while (blank(*at))
				++at;
			while (*at && !blank(*at))
				++at;
		}
		release(option);
	}
	WCHAR program[MAX_PATH], path[MAX_PATH];
	const WCHAR *arguments;
	/* With nothing to start, another command processor: this terminal is the only console, so it runs here until it ends. */
	if (!*at) {
		at = self;
		wait = TRUE;
	}
	first_word(at, program, MAX_PATH, &arguments);
	HANDLE process = NULL;
	BOOL started = FALSE;
	if (find_program(program, path)) {
		if (batch_file(path)) {
			struct Text line = {0};
			text_char(&line, L'"');
			text_put(&line, self);
			text_put(&line, L"\" /c \"");
			text_put(&line, at);
			text_char(&line, L'"');
			started = launch(self, line.data, folder, &process);
			release(line.data);
		} else
			started = launch(path, at, folder, &process);
		if (!started) {
			release(folder);
			return errorlevel;
		}
		if (wait) {
			WaitForSingleObject(process, INFINITE);
			GetExitCodeProcess(process, &errorlevel);
		}
		CloseHandle(process);
		release(folder);
		return wait ? errorlevel : 0;
	}
	typedef HINSTANCE(WINAPI * Execute)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
	HMODULE shell = LoadLibraryW(L"shell32.dll");
	Execute open = shell ? (Execute)(void (*)(void))GetProcAddress(shell, "ShellExecuteW") : NULL;
	while (blank(*arguments))
		++arguments;
	if (open && (INT_PTR)open(NULL, NULL, program, *arguments ? arguments : NULL, folder, SW_SHOWNORMAL) > 32) {
		release(folder);
		return 0;
	}
	release(folder);
	complain(L"The system cannot find the file ");
	complain(program);
	complain(L".\r\n");
	errorlevel = 9059;
	return 9059;
}

/* A path made of a folder and a name, with one backslash between them. */
static WCHAR *joined(const WCHAR *folder, const WCHAR *name)
{
	struct Text path = {0};
	text_put(&path, folder);
	if (path.size && path.data[path.size - 1] != L'\\')
		text_char(&path, L'\\');
	text_put(&path, name);
	return text_take(&path);
}

/* A full path split into its folder and its last part; the folder of C:\x is C:\. */
static void split_path(const WCHAR *full, WCHAR **folder, WCHAR **name)
{
	const DWORD size = length(full);
	DWORD slash = size;
	while (slash && full[slash - 1] != L'\\')
		--slash;
	*name = copy_of(full + slash);
	DWORD folder_size = slash ? slash - 1 : 0;
	if (folder_size <= 2 && size >= 2 && full[1] == L':')
		folder_size = 3;
	*folder = duplicate(full, folder_size);
}

static WCHAR *full_path(const WCHAR *name)
{
	WCHAR *plain = unquoted(name);
	WCHAR *full = allocate(MAX_PATH * 2 * sizeof(WCHAR));
	for (WCHAR *at = plain; *at; ++at)
		if (*at == L'/')
			*at = L'\\';
	if (!GetFullPathNameW(*plain ? plain : L".", MAX_PATH * 2, full, NULL))
		lstrcpynW(full, plain, MAX_PATH * 2);
	release(plain);
	return full;
}

/* The names a pattern matches in a folder, files or folders or both, in the order the search gives them. */
struct Found {
	WIN32_FIND_DATAW *items;
	DWORD count, capacity;
};

static void find_all(const WCHAR *pattern, struct Found *found, BOOL files, BOOL folders)
{
	WIN32_FIND_DATAW data;
	found->items = NULL;
	found->count = found->capacity = 0;
	HANDLE search = FindFirstFileW(pattern, &data);
	if (search == INVALID_HANDLE_VALUE)
		return;
	do {
		const BOOL folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if ((folder && !folders) || (!folder && !files) || same(data.cFileName, L".") || same(data.cFileName, L".."))
			continue;
		if (found->count == found->capacity) {
			const DWORD capacity = found->capacity ? found->capacity * 2 : 16;
			WIN32_FIND_DATAW *grown = allocate(capacity * sizeof *grown);
			if (found->count)
				memcpy(grown, found->items, found->count * sizeof *grown);
			release(found->items);
			found->items = grown;
			found->capacity = capacity;
		}
		found->items[found->count++] = data;
	} while (FindNextFileW(search, &data));
	FindClose(search);
}

/* A name made from another by a pattern with wildcards, as COPY and REN make them: ? takes a character, * the rest up to the
   pattern's next character, and the pattern's other characters stay. */
static WCHAR *mapped_name(const WCHAR *name, const WCHAR *pattern)
{
	struct Text out = {0};
	const DWORD size = length(name);
	DWORD at = 0;
	for (const WCHAR *p = pattern; *p; ++p) {
		if (*p == L'?') {
			if (at < size && name[at] != L'.')
				text_char(&out, name[at++]);
		} else if (*p == L'*') {
			if (!p[1]) {
				text_put(&out, name + at);
				at = size;
				break;
			}
			DWORD stop = size;
			for (DWORD index = size; index > at; --index)
				if (folded(name[index - 1]) == folded(p[1])) {
					stop = index - 1;
					break;
				}
			text_add(&out, name + at, stop - at);
			at = stop;
		} else if (*p == L'.') {
			text_char(&out, L'.');
			while (at < size && name[at] != L'.')
				++at;
			if (at < size)
				++at;
		} else {
			text_char(&out, *p);
			if (at < size && name[at] != L'.')
				++at;
		}
	}
	while (out.size && out.data[out.size - 1] == L'.')
		out.data[--out.size] = 0;
	return text_take(&out);
}

static ULONGLONG file_size(const WIN32_FIND_DATAW *data) { return ((ULONGLONG)data->nFileSizeHigh << 32) | data->nFileSizeLow; }
static ULONGLONG file_stamp(const FILETIME *time) { return ((ULONGLONG)time->dwHighDateTime << 32) | time->dwLowDateTime; }

/* The attributes /A takes in DIR and DEL: D R H S A L I O, each perhaps after a minus. */
static DWORD attribute_bit(WCHAR letter_given)
{
	switch (folded(letter_given)) {
	case L'D': return FILE_ATTRIBUTE_DIRECTORY;
	case L'R': return FILE_ATTRIBUTE_READONLY;
	case L'H': return FILE_ATTRIBUTE_HIDDEN;
	case L'S': return FILE_ATTRIBUTE_SYSTEM;
	case L'A': return FILE_ATTRIBUTE_ARCHIVE;
	case L'L': return FILE_ATTRIBUTE_REPARSE_POINT;
	case L'I': return FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
	case L'O': return FILE_ATTRIBUTE_OFFLINE;
	default: return 0;
	}
}

static void attribute_filter(const WCHAR *text, DWORD *want, DWORD *refuse)
{
	*want = *refuse = 0;
	if (*text == L':')
		++text;
	for (BOOL negate = FALSE; *text; ++text) {
		if (*text == L'-') {
			negate = TRUE;
			continue;
		}
		if (negate)
			*refuse |= attribute_bit(*text);
		else
			*want |= attribute_bit(*text);
		negate = FALSE;
	}
}

/* DIR's state: its switches and the totals it adds up. */
struct Listing {
	BOOL bare, recursive, wide, lower, grouped;
	DWORD want, refuse;
	WCHAR order[16];
	DWORD files, folders, total_files, total_folders, listed_folders;
	ULONGLONG bytes, total_bytes;
	BOOL found;
};

static const WCHAR *sort_order;

static int compare_found(const WIN32_FIND_DATAW *left, const WIN32_FIND_DATAW *right)
{
	for (const WCHAR *key = sort_order; *key; ++key) {
		const BOOL reverse = *key == L'-';
		if (reverse && !*++key)
			break;
		int order = 0;
		switch (folded(*key)) {
		case L'N': order = compare_folded(left->cFileName, right->cFileName); break;
		case L'E': {
			const WCHAR *a = left->cFileName + length(left->cFileName), *b = right->cFileName + length(right->cFileName);
			while (a > left->cFileName && a[-1] != L'.')
				--a;
			while (b > right->cFileName && b[-1] != L'.')
				--b;
			order = compare_folded(a > left->cFileName ? a : L"", b > right->cFileName ? b : L"");
			break;
		}
		case L'S': order = file_size(left) < file_size(right) ? -1 : file_size(left) > file_size(right); break;
		case L'D': {
			const ULONGLONG a = file_stamp(&left->ftLastWriteTime), b = file_stamp(&right->ftLastWriteTime);
			order = a < b ? -1 : a > b;
			break;
		}
		case L'G': {
			const BOOL a = (left->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, b = (right->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			order = a == b ? 0 : a ? -1 : 1;
			break;
		}
		default: break;
		}
		if (order)
			return reverse ? -order : order;
	}
	return compare_folded(left->cFileName, right->cFileName);
}

/* Sorts found names by DIR's /O order, keeping their order where it does not tell them apart. */
static void sort_found(struct Found *found, const WCHAR *order)
{
	if (found->count < 2)
		return;
	sort_order = order;
	WIN32_FIND_DATAW *scratch = allocate(found->count * sizeof *scratch);
	for (DWORD width = 1; width < found->count; width *= 2) {
		for (DWORD start = 0; start < found->count; start += 2 * width) {
			DWORD middle = start + width < found->count ? start + width : found->count;
			DWORD end = start + 2 * width < found->count ? start + 2 * width : found->count;
			DWORD left = start, right = middle, out = start;
			while (left < middle && right < end)
				scratch[out++] = compare_found(&found->items[right], &found->items[left]) < 0 ? found->items[right++] : found->items[left++];
			while (left < middle)
				scratch[out++] = found->items[left++];
			while (right < end)
				scratch[out++] = found->items[right++];
		}
		memcpy(found->items, scratch, found->count * sizeof *scratch);
	}
	release(scratch);
}

static void put_name(struct Text *text, const WCHAR *name, BOOL lower)
{
	for (; *name; ++name)
		text_char(text, lower && *name >= L'A' && *name <= L'Z' ? (WCHAR)(*name + 32) : *name);
}

static void put_files_line(struct Text *text, DWORD files, ULONGLONG bytes, BOOL grouped)
{
	put_padded(text, files, 16, grouped);
	text_put(text, L" File(s) ");
	put_padded(text, bytes, 14, grouped);
	text_put(text, L" bytes\r\n");
}

static void put_folders_line(struct Text *text, DWORD folders, const WCHAR *path, BOOL grouped)
{
	ULARGE_INTEGER free_bytes = {0};
	DWORD sectors = 0, bytes = 0, free_clusters = 0, clusters = 0;
	WCHAR root[4] = {path[0], L':', L'\\', 0};
	/* The 64-bit count where there is one; the older call counts in clusters that may stop short of a large disk. */
	typedef BOOL(WINAPI * FreeSpace)(LPCWSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER);
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	FreeSpace free_space = kernel ? (FreeSpace)(void (*)(void))GetProcAddress(kernel, "GetDiskFreeSpaceExW") : NULL;
	if (!free_space || !free_space(root, &free_bytes, NULL, NULL)) {
		free_bytes.QuadPart = 0;
		if (GetDiskFreeSpaceW(root, &sectors, &bytes, &free_clusters, &clusters))
			free_bytes.QuadPart = (ULONGLONG)sectors * bytes * free_clusters;
	}
	put_padded(text, folders, 16, grouped);
	text_put(text, L" Dir(s) ");
	put_padded(text, free_bytes.QuadPart, 15, grouped);
	text_put(text, L" bytes free\r\n");
}

static BOOL listed(const struct Listing *listing, DWORD attributes)
{
	return (attributes & listing->want) == listing->want && !(attributes & listing->refuse);
}

/* One folder of a DIR listing, then with /S the folders under it. */
static void list_folder(const WCHAR *folder, const WCHAR *pattern, struct Listing *listing)
{
	struct Found found;
	WCHAR *search = joined(folder, pattern);
	WIN32_FIND_DATAW data;
	found.items = NULL;
	found.count = found.capacity = 0;
	HANDLE handle = FindFirstFileW(search, &data);
	release(search);
	if (handle != INVALID_HANDLE_VALUE) {
		do {
			if (!listed(listing, data.dwFileAttributes))
				continue;
			if (listing->bare && (same(data.cFileName, L".") || same(data.cFileName, L"..")))
				continue;
			if (found.count == found.capacity) {
				const DWORD capacity = found.capacity ? found.capacity * 2 : 32;
				WIN32_FIND_DATAW *grown = allocate(capacity * sizeof *grown);
				if (found.count)
					memcpy(grown, found.items, found.count * sizeof *grown);
				release(found.items);
				found.items = grown;
				found.capacity = capacity;
			}
			found.items[found.count++] = data;
		} while (!interrupted && FindNextFileW(handle, &data));
		FindClose(handle);
	}
	if (listing->order[0])
		sort_found(&found, listing->order);
	struct Text text = {0};
	DWORD files = 0, folders = 0;
	ULONGLONG bytes = 0;
	if (found.count || !listing->recursive) {
		if (!listing->bare) {
			text_put(&text, L"\r\n Directory of ");
			text_put(&text, folder);
			text_put(&text, L"\r\n\r\n");
		}
		DWORD column = 0, widest = 0, columns = 1;
		if (listing->wide) {
			CONSOLE_SCREEN_BUFFER_INFO info;
			const DWORD width = GetConsoleScreenBufferInfo(output, &info) ? (DWORD)info.dwSize.X : 80;
			for (DWORD index = 0; index < found.count; ++index) {
				const DWORD size = length(found.items[index].cFileName) + 2;
				if (size > widest)
					widest = size;
			}
			widest += 1;
			columns = widest ? width / widest : 1;
			if (!columns)
				columns = 1;
		}
		for (DWORD index = 0; index < found.count && !interrupted; ++index) {
			const WIN32_FIND_DATAW *item = &found.items[index];
			const BOOL is_dir = (item->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			if (is_dir)
				++folders;
			else {
				++files;
				bytes += file_size(item);
			}
			if (listing->bare) {
				if (listing->recursive) {
					text_put(&text, folder);
					if (text.size && text.data[text.size - 1] != L'\\')
						text_char(&text, L'\\');
				}
				put_name(&text, item->cFileName, listing->lower);
				text_put(&text, L"\r\n");
			} else if (listing->wide) {
				const DWORD start = text.size;
				if (is_dir)
					text_char(&text, L'[');
				put_name(&text, item->cFileName, listing->lower);
				if (is_dir)
					text_char(&text, L']');
				if (++column == columns || index + 1 == found.count) {
					text_put(&text, L"\r\n");
					column = 0;
				} else
					for (DWORD pad = text.size - start; pad < widest; ++pad)
						text_char(&text, L' ');
			} else {
				put_file_time(&text, &item->ftLastWriteTime, L"  ");
				if (is_dir)
					text_put(&text, (item->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? L"    <JUNCTION>     " : L"    <DIR>          ");
				else {
					put_padded(&text, file_size(item), 18, listing->grouped);
					text_char(&text, L' ');
				}
				put_name(&text, item->cFileName, listing->lower);
				text_put(&text, L"\r\n");
			}
			if (text.size > 4096)
				say_text(&text);
		}
		if (!listing->bare && found.count)
			put_files_line(&text, files, bytes, listing->grouped);
		if (found.count) {
			listing->found = TRUE;
			++listing->listed_folders;
		}
		say_text(&text);
	}
	listing->total_files += files;
	listing->total_folders += folders;
	listing->total_bytes += bytes;
	listing->files = files;
	listing->folders = folders;
	listing->bytes = bytes;
	release(found.items);
	release(text.data);
	if (!listing->recursive || interrupted)
		return;
	struct Found subfolders;
	WCHAR *all = joined(folder, L"*");
	find_all(all, &subfolders, FALSE, TRUE);
	release(all);
	sort_found(&subfolders, L"N");
	for (DWORD index = 0; index < subfolders.count && !interrupted; ++index) {
		if (subfolders.items[index].dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			continue;
		WCHAR *sub = joined(folder, subfolders.items[index].cFileName);
		list_folder(sub, pattern, listing);
		release(sub);
	}
	release(subfolders.items);
}

static BOOL dir_switch(const WCHAR *option, struct Listing *listing)
{
	BOOL negate = option[1] == L'-';
	const WCHAR *name = option + (negate ? 2 : 1);
	switch (folded(*name)) {
	case L'B': listing->bare = !negate; break;
	case L'S': listing->recursive = !negate; break;
	case L'W': listing->wide = !negate; break;
	case L'L': listing->lower = !negate; break;
	case L'C': listing->grouped = !negate; break;
	case L'A':
		if (negate)
			listing->want = 0, listing->refuse = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
		else
			attribute_filter(name + 1, &listing->want, &listing->refuse);
		break;
	case L'O': {
		const WCHAR *order = name[1] == L':' ? name + 2 : name + 1;
		if (negate)
			listing->order[0] = 0;
		else
			lstrcpynW(listing->order, *order ? order : L"GN", 16);
		break;
	}
	case L'P': case L'Q': case L'N': case L'X': case L'4': case L'T': case L'R': break;
	default: return FALSE;
	}
	return TRUE;
}

static DWORD command_dir(const WCHAR *rest)
{
	struct Listing listing;
	memset(&listing, 0, sizeof listing);
	listing.grouped = TRUE;
	listing.refuse = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
	struct Text all = {0};
	WCHAR *defaults = environment_value(L"DIRCMD");
	if (defaults) {
		text_put(&all, defaults);
		text_char(&all, L' ');
		release(defaults);
	}
	text_put(&all, rest);
	struct Words words;
	split_words(all.data ? all.data : L"", &words, FALSE);
	release(all.data);
	DWORD paths = 0;
	for (DWORD index = 0; index < words.count; ++index) {
		if (words.items[index][0] != L'/') {
			++paths;
			continue;
		}
		if (!dir_switch(words.items[index], &listing)) {
			complain(L"Invalid switch - \"");
			complain(words.items[index] + 1);
			complain(L"\".\r\n");
			free_words(&words);
			errorlevel = 1;
			return 1;
		}
	}
	BOOL any_found = FALSE, headed = FALSE;
	WCHAR *first_path = NULL;
	for (DWORD index = 0; index < words.count || (!paths && index == 0); ++index) {
		const WCHAR *spec = paths ? words.items[index] : L"*";
		if (paths && spec[0] == L'/')
			continue;
		WCHAR *full = full_path(spec), *folder, *pattern;
		if (!wild(full) && is_folder(full)) {
			folder = copy_of(full);
			pattern = copy_of(L"*");
		} else {
			split_path(full, &folder, &pattern);
		}
		if (!headed && !listing.bare) {
			volume_lines(folder);
			headed = TRUE;
		}
		if (!first_path)
			first_path = copy_of(folder);
		listing.found = FALSE;
		list_folder(folder, pattern, &listing);
		if (listing.found)
			any_found = TRUE;
		else if (!listing.recursive && !listing.bare)
			complain(L"File Not Found\r\n");
		release(full);
		release(folder);
		release(pattern);
		if (!paths)
			break;
	}
	free_words(&words);
	if (!any_found) {
		if (listing.recursive || listing.bare)
			complain(L"File Not Found\r\n");
		release(first_path);
		errorlevel = 1;
		return 1;
	}
	if (!listing.bare) {
		struct Text text = {0};
		if (listing.recursive) {
			text_put(&text, L"\r\n     Total Files Listed:\r\n");
			put_files_line(&text, listing.total_files, listing.total_bytes, listing.grouped);
		}
		put_folders_line(&text, listing.recursive ? listing.total_folders : listing.folders, first_path, listing.grouped);
		say_text(&text);
		release(text.data);
	}
	release(first_path);
	return 0;
}

/* MD makes a folder and the folders above it that are missing. */
static DWORD command_md(const WCHAR *rest)
{
	struct Words words;
	split_words(rest, &words, FALSE);
	DWORD status = 0;
	if (!words.count) {
		free_words(&words);
		return syntax_error();
	}
	for (DWORD index = 0; index < words.count; ++index) {
		WCHAR *full = full_path(words.items[index]);
		if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) {
			WCHAR *shown = unquoted(words.items[index]);
			complain(L"A subdirectory or file ");
			complain(shown);
			complain(L" already exists.\r\n");
			release(shown);
			status = errorlevel = 1;
			release(full);
			continue;
		}
		for (DWORD at = 3; full[at]; ++at)
			if (full[at] == L'\\') {
				full[at] = 0;
				CreateDirectoryW(full, NULL);
				full[at] = L'\\';
			}
		if (!CreateDirectoryW(full, NULL) && !is_folder(full)) {
			complain(file_error(GetLastError()));
			status = errorlevel = 1;
		}
		release(full);
	}
	free_words(&words);
	return status;
}

static BOOL remove_tree(const WCHAR *path)
{
	const DWORD attributes = GetFileAttributesW(path);
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
		return RemoveDirectoryW(path);
	struct Found found;
	WCHAR *all = joined(path, L"*");
	find_all(all, &found, TRUE, TRUE);
	release(all);
	for (DWORD index = 0; index < found.count; ++index) {
		WCHAR *item = joined(path, found.items[index].cFileName);
		if (found.items[index].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			remove_tree(item);
		else {
			if (found.items[index].dwFileAttributes & FILE_ATTRIBUTE_READONLY)
				SetFileAttributesW(item, found.items[index].dwFileAttributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
			DeleteFileW(item);
		}
		release(item);
	}
	release(found.items);
	return RemoveDirectoryW(path);
}

/* RD removes an empty folder; with /S the folder and all it holds, after asking unless /Q. */
static DWORD command_rd(const WCHAR *rest)
{
	struct Words words;
	BOOL tree = FALSE, quiet = FALSE;
	DWORD status = 0, names = 0;
	split_words(rest, &words, FALSE);
	for (DWORD index = 0; index < words.count; ++index) {
		if (equal(words.items[index], L"/S"))
			tree = TRUE;
		else if (equal(words.items[index], L"/Q"))
			quiet = TRUE;
		else if (words.items[index][0] == L'/') {
			free_words(&words);
			return failure(L"Invalid switch.\r\n");
		} else
			++names;
	}
	if (!names) {
		free_words(&words);
		return syntax_error();
	}
	for (DWORD index = 0; index < words.count; ++index) {
		if (words.items[index][0] == L'/')
			continue;
		WCHAR *full = full_path(words.items[index]);
		const DWORD attributes = GetFileAttributesW(full);
		if (attributes == INVALID_FILE_ATTRIBUTES) {
			complain(L"The system cannot find the file specified.\r\n");
			status = errorlevel = 2;
		} else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
			complain(L"The directory name is invalid.\r\n");
			status = errorlevel = 267;
		} else if (tree) {
			WCHAR *shown = unquoted(words.items[index]);
			const WCHAR answer = quiet ? L'Y' : ask(L", Are you sure (Y/N)? ", shown);
			release(shown);
			if (answer == L'Y' && !remove_tree(full)) {
				complain(file_error(GetLastError()));
				status = errorlevel = 1;
			}
		} else if (!RemoveDirectoryW(full)) {
			const DWORD error = GetLastError();
			complain(file_error(error));
			status = errorlevel = error == ERROR_DIR_NOT_EMPTY ? 145 : 1;
		}
		release(full);
	}
	free_words(&words);
	return status;
}

struct Deleting {
	BOOL prompt, force, tree, quiet;
	DWORD want, refuse;
	BOOL found;
};

static void delete_matching(const WCHAR *folder, const WCHAR *pattern, struct Deleting *options)
{
	struct Found found;
	WCHAR *search = joined(folder, pattern);
	find_all(search, &found, TRUE, FALSE);
	release(search);
	for (DWORD index = 0; index < found.count && !interrupted; ++index) {
		const DWORD attributes = found.items[index].dwFileAttributes;
		if ((attributes & options->want) != options->want || (attributes & options->refuse))
			continue;
		WCHAR *path = joined(folder, found.items[index].cFileName);
		options->found = TRUE;
		if (options->prompt && ask(L", Delete (Y/N)? ", path) != L'Y') {
			release(path);
			continue;
		}
		if (attributes & FILE_ATTRIBUTE_READONLY) {
			if (!options->force) {
				complain(path);
				complain(L"\r\nAccess is denied.\r\n");
				release(path);
				continue;
			}
			SetFileAttributesW(path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
		}
		if (DeleteFileW(path)) {
			if (options->tree && !options->quiet) {
				say(L"Deleted file - ");
				say(path);
				say(L"\r\n");
			}
		} else {
			complain(path);
			complain(L"\r\n");
			complain(file_error(GetLastError()));
		}
		release(path);
	}
	release(found.items);
	if (!options->tree)
		return;
	WCHAR *all = joined(folder, L"*");
	find_all(all, &found, FALSE, TRUE);
	release(all);
	for (DWORD index = 0; index < found.count && !interrupted; ++index) {
		if (found.items[index].dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			continue;
		WCHAR *sub = joined(folder, found.items[index].cFileName);
		delete_matching(sub, pattern, options);
		release(sub);
	}
	release(found.items);
}

/* DEL deletes files: a folder's name means the files in it, and all of them is asked about first unless /Q. */
static DWORD command_del(const WCHAR *rest)
{
	struct Words words;
	struct Deleting options;
	memset(&options, 0, sizeof options);
	options.refuse = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
	DWORD names = 0;
	split_words(rest, &words, TRUE);
	for (DWORD index = 0; index < words.count; ++index) {
		const WCHAR *word = words.items[index];
		if (word[0] != L'/')
			++names;
		else if (equal(word, L"/P"))
			options.prompt = TRUE;
		else if (equal(word, L"/F"))
			options.force = TRUE;
		else if (equal(word, L"/S"))
			options.tree = TRUE;
		else if (equal(word, L"/Q"))
			options.quiet = TRUE;
		else if (folded(word[1]) == L'A')
			attribute_filter(word + 2, &options.want, &options.refuse);
		else {
			free_words(&words);
			return failure(L"Invalid switch.\r\n");
		}
	}
	if (!names) {
		free_words(&words);
		return syntax_error();
	}
	for (DWORD index = 0; index < words.count; ++index) {
		if (words.items[index][0] == L'/')
			continue;
		WCHAR *full = full_path(words.items[index]), *folder, *pattern;
		BOOL everything;
		if (!wild(full) && is_folder(full)) {
			folder = copy_of(full);
			pattern = copy_of(L"*");
			everything = TRUE;
		} else {
			split_path(full, &folder, &pattern);
			everything = same(pattern, L"*") || same(pattern, L"*.*");
		}
		if (everything && !options.quiet) {
			WCHAR *shown = joined(folder, pattern);
			const WCHAR answer = ask(L", Are you sure (Y/N)? ", shown);
			release(shown);
			if (answer != L'Y') {
				release(full);
				release(folder);
				release(pattern);
				continue;
			}
		}
		options.found = FALSE;
		delete_matching(folder, pattern, &options);
		if (!options.found) {
			complain(L"Could Not Find ");
			complain(full);
			complain(L"\r\n");
		}
		release(full);
		release(folder);
		release(pattern);
	}
	free_words(&words);
	return 0;
}

/* Whether a file may be written over: with /Y or COPYCMD=/Y yes, else at the prompt it is asked (with All for the rest). */
static BOOL may_replace(const WCHAR *path, BOOL *all)
{
	if (*all || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return TRUE;
	for (;;) {
		const WCHAR answer = ask(L"? (Yes/No/All): ", path);
		if (answer == L'A')
			*all = TRUE;
		if (answer == L'Y' || answer == L'A')
			return TRUE;
		if (answer == L'N' || !answer)
			return FALSE;
	}
}

/* Whether to ask before replacing: not in a batch file, not with /Y, as COPYCMD says otherwise. */
static BOOL replace_without_asking(struct Words *words)
{
	BOOL yes = batch != NULL || !interactive_input;
	WCHAR *defaults = environment_value(L"COPYCMD");
	if (defaults) {
		if (equal(defaults, L"/Y"))
			yes = TRUE;
		else if (equal(defaults, L"/-Y"))
			yes = FALSE;
		release(defaults);
	}
	for (DWORD index = 0; index < words->count; ++index)
		if (equal(words->items[index], L"/Y"))
			yes = TRUE;
		else if (equal(words->items[index], L"/-Y"))
			yes = FALSE;
	return yes;
}

static BOOL append_file(HANDLE target, const WCHAR *path)
{
	HANDLE source = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (source == INVALID_HANDLE_VALUE)
		return FALSE;
	char *buffer = allocate(65536);
	DWORD count, written;
	BOOL success = TRUE;
	while (ReadFile(source, buffer, 65536, &count, NULL) && count)
		if (!WriteFile(target, buffer, count, &written, NULL) || written != count) {
			success = FALSE;
			break;
		}
	release(buffer);
	CloseHandle(source);
	return success;
}

static void say_count(DWORD count, const WCHAR *what)
{
	struct Text text = {0};
	put_padded(&text, count, 9, FALSE);
	text_put(&text, what);
	say_text(&text);
	release(text.data);
}

/* The name a COPY or MOVE source shows as, with the folder it was given in. */
static WCHAR *shown_name(const WCHAR *spec, const WCHAR *name)
{
	WCHAR *plain = unquoted(spec);
	DWORD prefix = length(plain);
	while (prefix && plain[prefix - 1] != L'\\' && plain[prefix - 1] != L'/' && plain[prefix - 1] != L':')
		--prefix;
	struct Text text = {0};
	text_add(&text, plain, prefix);
	text_put(&text, name);
	release(plain);
	return text_take(&text);
}

/* COPY source [destination], with wildcards, into a folder or onto a file; COPY a+b target joins files into one. */
static DWORD command_copy(const WCHAR *rest)
{
	struct Words words, parts = {0};
	split_words(rest, &words, FALSE);
	BOOL all = replace_without_asking(&words);
	/* The sources and the destination, with + joining sources whether or not blanks are around it. */
	struct Text joined_text = {0};
	for (DWORD index = 0; index < words.count; ++index) {
		if (words.items[index][0] == L'/')
			continue;
		if (joined_text.size)
			text_char(&joined_text, L' ');
		text_put(&joined_text, words.items[index]);
	}
	WCHAR *spec = text_take(&joined_text);
	struct Text normal = {0};
	BOOL quoted = FALSE;
	for (const WCHAR *at = spec; *at; ++at) {
		if (*at == L'"')
			quoted = !quoted;
		if (!quoted && *at == L'+') {
			while (normal.size && blank(normal.data[normal.size - 1]))
				normal.data[--normal.size] = 0;
			text_char(&normal, L'+');
			while (blank(at[1]))
				++at;
			continue;
		}
		text_char(&normal, *at);
	}
	release(spec);
	spec = text_take(&normal);
	split_words(spec, &parts, FALSE);
	release(spec);
	free_words(&words);
	if (!parts.count || parts.count > 2) {
		free_words(&parts);
		return syntax_error();
	}
	const WCHAR *sources = parts.items[0];
	WCHAR *destination = parts.count == 2 ? full_path(parts.items[1]) : full_path(L".");
	const BOOL destination_folder = is_folder(destination) || (parts.count == 2 && parts.items[1][length(parts.items[1]) - 1] == L'\\');
	DWORD copied = 0;
	BOOL concatenate = FALSE;
	for (const WCHAR *at = sources; *at; ++at)
		if (*at == L'+')
			concatenate = TRUE;
	if (!concatenate && !destination_folder) {
		WCHAR *full = full_path(sources);
		struct Found found;
		WCHAR *folder, *pattern;
		if (!wild(full) && is_folder(full)) {
			folder = copy_of(full);
			pattern = copy_of(L"*");
		} else
			split_path(full, &folder, &pattern);
		WCHAR *search = joined(folder, pattern);
		find_all(search, &found, TRUE, FALSE);
		release(search);
		concatenate = found.count > 1 && !wild(destination);
		release(found.items);
		release(full);
		release(folder);
		release(pattern);
	}
	if (concatenate) {
		/* Every source, in order, into one file. */
		if (!may_replace(destination, &all)) {
			say_count(0, L" file(s) copied.\r\n");
			release(destination);
			free_words(&parts);
			return 0;
		}
		HANDLE target = INVALID_HANDLE_VALUE;
		BOOL any = FALSE;
		for (const WCHAR *at = sources; *at;) {
			const WCHAR *start = at;
			while (*at && *at != L'+')
				++at;
			WCHAR *one = duplicate(start, (DWORD)(at - start));
			if (*at)
				++at;
			WCHAR *full = full_path(one), *folder, *pattern;
			split_path(full, &folder, &pattern);
			struct Found found;
			WCHAR *search = joined(folder, pattern);
			find_all(search, &found, TRUE, FALSE);
			release(search);
			for (DWORD index = 0; index < found.count; ++index) {
				WCHAR *path = joined(folder, found.items[index].cFileName);
				if (!compare_folded(path, destination)) {
					release(path);
					continue;
				}
				if (target == INVALID_HANDLE_VALUE)
					target = CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
				if (target == INVALID_HANDLE_VALUE) {
					complain(file_error(GetLastError()));
					release(path);
					break;
				}
				WCHAR *shown = shown_name(one, found.items[index].cFileName);
				say(shown);
				say(L"\r\n");
				release(shown);
				append_file(target, path);
				any = TRUE;
				release(path);
			}
			release(found.items);
			release(full);
			release(folder);
			release(pattern);
			release(one);
		}
		if (target != INVALID_HANDLE_VALUE)
			CloseHandle(target);
		release(destination);
		free_words(&parts);
		if (!any) {
			complain(L"The system cannot find the file specified.\r\n");
			say_count(0, L" file(s) copied.\r\n");
			errorlevel = 1;
			return 1;
		}
		say_count(1, L" file(s) copied.\r\n");
		return 0;
	}
	WCHAR *full = full_path(sources), *folder, *pattern;
	if (!wild(full) && is_folder(full)) {
		folder = copy_of(full);
		pattern = copy_of(L"*");
	} else
		split_path(full, &folder, &pattern);
	struct Found found;
	WCHAR *search = joined(folder, pattern);
	find_all(search, &found, TRUE, FALSE);
	release(search);
	const BOOL show = wild(full) || is_folder(full);
	WCHAR *target_folder, *target_name;
	split_path(destination, &target_folder, &target_name);
	DWORD status = 0;
	if (!found.count) {
		complain(L"The system cannot find the file specified.\r\n");
		status = errorlevel = 1;
	}
	for (DWORD index = 0; index < found.count && !interrupted; ++index) {
		const WCHAR *name = found.items[index].cFileName;
		WCHAR *from = joined(folder, name), *to;
		if (destination_folder)
			to = joined(destination, name);
		else if (wild(target_name)) {
			WCHAR *made = mapped_name(name, target_name);
			to = joined(target_folder, made);
			release(made);
		} else
			to = copy_of(destination);
		if (!compare_folded(from, to)) {
			complain(L"The file cannot be copied onto itself.\r\n");
			status = errorlevel = 1;
		} else if (may_replace(to, &all)) {
			if (show) {
				WCHAR *shown = shown_name(sources, name);
				say(shown);
				say(L"\r\n");
				release(shown);
			}
			if (CopyFileW(from, to, FALSE))
				++copied;
			else {
				complain(file_error(GetLastError()));
				status = errorlevel = 1;
			}
		}
		release(from);
		release(to);
	}
	say_count(copied, L" file(s) copied.\r\n");
	release(found.items);
	release(full);
	release(folder);
	release(pattern);
	release(target_folder);
	release(target_name);
	release(destination);
	free_words(&parts);
	return status;
}

/* MOVE source[,source] destination: files into a folder or onto a name, or a folder to a new name. */
static DWORD command_move(const WCHAR *rest)
{
	struct Words words;
	split_words(rest, &words, FALSE);
	BOOL all = replace_without_asking(&words);
	const WCHAR *names[2] = {NULL, NULL};
	DWORD count = 0;
	for (DWORD index = 0; index < words.count; ++index)
		if (words.items[index][0] != L'/') {
			if (count == 2) {
				free_words(&words);
				return syntax_error();
			}
			names[count++] = words.items[index];
		}
	if (count != 2) {
		free_words(&words);
		return syntax_error();
	}
	WCHAR *destination = full_path(names[1]);
	const BOOL into_folder = is_folder(destination);
	DWORD moved = 0, status = 0;
	BOOL folders_moved = FALSE;
	for (const WCHAR *at = names[0]; *at && !interrupted;) {
		const WCHAR *start = at;
		while (*at && *at != L',')
			++at;
		WCHAR *one = duplicate(start, (DWORD)(at - start));
		if (*at)
			++at;
		WCHAR *full = full_path(one), *folder, *pattern;
		split_path(full, &folder, &pattern);
		struct Found found;
		WCHAR *search = joined(folder, pattern);
		find_all(search, &found, TRUE, !wild(pattern));
		release(search);
		if (!found.count) {
			complain(L"The system cannot find the file specified.\r\n");
			status = errorlevel = 1;
		}
		for (DWORD index = 0; index < found.count; ++index) {
			WCHAR *from = joined(folder, found.items[index].cFileName);
			WCHAR *to = into_folder ? joined(destination, found.items[index].cFileName) : copy_of(destination);
			const BOOL is_dir = (found.items[index].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			if (is_dir || may_replace(to, &all)) {
				if (wild(pattern) || found.count > 1) {
					say(from);
					say(L"\r\n");
				}
				if (MoveFileExW(from, to, (is_dir ? 0 : MOVEFILE_REPLACE_EXISTING) | MOVEFILE_COPY_ALLOWED)) {
					++moved;
					folders_moved |= is_dir;
				} else {
					complain(file_error(GetLastError()));
					status = errorlevel = 1;
				}
			}
			release(from);
			release(to);
		}
		release(found.items);
		release(full);
		release(folder);
		release(pattern);
		release(one);
	}
	if (moved || !status)
		say_count(moved, folders_moved ? L" dir(s) moved.\r\n" : L" file(s) moved.\r\n");
	release(destination);
	free_words(&words);
	return status;
}

/* REN source newname: a new name in the same folder, made from the old one where it has wildcards. */
static DWORD command_ren(const WCHAR *rest)
{
	struct Words words;
	split_words(rest, &words, FALSE);
	if (words.count != 2) {
		free_words(&words);
		return syntax_error();
	}
	WCHAR *target = unquoted(words.items[1]);
	for (const WCHAR *at = target; *at; ++at)
		if (*at == L'\\' || *at == L'/' || *at == L':') {
			release(target);
			free_words(&words);
			return syntax_error();
		}
	WCHAR *full = full_path(words.items[0]), *folder, *pattern;
	split_path(full, &folder, &pattern);
	struct Found found;
	WCHAR *search = joined(folder, pattern);
	find_all(search, &found, TRUE, TRUE);
	release(search);
	DWORD status = 0;
	if (!found.count) {
		complain(L"The system cannot find the file specified.\r\n");
		status = errorlevel = 1;
	}
	for (DWORD index = 0; index < found.count; ++index) {
		WCHAR *made = wild(target) ? mapped_name(found.items[index].cFileName, target) : copy_of(target);
		WCHAR *from = joined(folder, found.items[index].cFileName), *to = joined(folder, made);
		if (!MoveFileW(from, to)) {
			const DWORD error = GetLastError();
			complain(error == ERROR_FILE_NOT_FOUND ? L"The system cannot find the file specified.\r\n" :
				L"A duplicate file name exists, or the file\r\ncannot be found.\r\n");
			status = errorlevel = 1;
		}
		release(made);
		release(from);
		release(to);
	}
	release(found.items);
	release(full);
	release(folder);
	release(pattern);
	release(target);
	free_words(&words);
	return status;
}

/* How many of a chunk's bytes end with whole UTF-8 characters, so that one is not split between two reads. */
static DWORD whole_characters(const char *bytes, DWORD size)
{
	if (GetConsoleOutputCP() != CP_UTF8)
		return size;
	DWORD back = 0;
	while (back < 3 && back < size && ((unsigned char)bytes[size - 1 - back] & 0xc0) == 0x80)
		++back;
	if (back >= size)
		return size;
	const unsigned char lead = (unsigned char)bytes[size - 1 - back];
	const DWORD needs = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2 : lead >= 0xc0 ? 1 : 0;
	return needs > back ? size - 1 - back : size;
}

/* Copies a file to the output: as text to a console, and as it is to a file or a pipe. */
static void type_file(HANDLE file)
{
	DWORD mode, count, written, kept = 0;
	char *bytes = allocate(65536 + 4);
	if (!GetConsoleMode(output, &mode)) {
		while (ReadFile(file, bytes, 65536, &count, NULL) && count)
			WriteFile(output, bytes, count, &written, NULL);
		release(bytes);
		return;
	}
	WCHAR *text = allocate((65536 + 4) * sizeof(WCHAR));
	BOOL first = TRUE, wide = FALSE;
	while (ReadFile(file, bytes + kept, 65536, &count, NULL) && count) {
		DWORD size = kept + count, start = 0;
		if (first) {
			first = FALSE;
			if (size >= 2 && (unsigned char)bytes[0] == 0xff && (unsigned char)bytes[1] == 0xfe)
				wide = TRUE, start = 2;
			else if (size >= 3 && (unsigned char)bytes[0] == 0xef && (unsigned char)bytes[1] == 0xbb && (unsigned char)bytes[2] == 0xbf)
				start = 3;
		}
		DWORD whole = wide ? start + (size - start) / 2 * 2 : whole_characters(bytes, size);
		if (wide) {
			const DWORD characters = (whole - start) / 2;
			for (DWORD index = 0; index < characters; ++index)
				text[index] = (WCHAR)((unsigned char)bytes[start + index * 2] | (unsigned char)bytes[start + index * 2 + 1] << 8);
			write_to(output, text, characters);
		} else if (whole > start) {
			const int characters = MultiByteToWideChar(GetConsoleOutputCP(), 0, bytes + start, (int)(whole - start), text, 65536 + 4);
			write_to(output, text, characters > 0 ? (DWORD)characters : 0);
		}
		kept = size - whole;
		memmove(bytes, bytes + whole, kept);
	}
	release(text);
	release(bytes);
}

static DWORD command_type(const WCHAR *rest)
{
	struct Words words;
	split_words(rest, &words, FALSE);
	if (!words.count) {
		free_words(&words);
		return syntax_error();
	}
	DWORD status = 0;
	for (DWORD index = 0; index < words.count; ++index) {
		WCHAR *full = full_path(words.items[index]), *folder, *pattern;
		split_path(full, &folder, &pattern);
		const BOOL many = wild(pattern);
		struct Found found;
		WCHAR *search = joined(folder, pattern);
		find_all(search, &found, TRUE, FALSE);
		release(search);
		if (!found.count) {
			complain(is_folder(full) ? L"Access is denied.\r\n" : L"The system cannot find the file specified.\r\n");
			if (words.count > 1) {
				WCHAR *shown = unquoted(words.items[index]);
				complain(L"Error occurred while processing: ");
				complain(shown);
				complain(L".\r\n");
				release(shown);
			}
			status = errorlevel = 1;
		}
		for (DWORD item = 0; item < found.count; ++item) {
			WCHAR *path = joined(folder, found.items[item].cFileName);
			if (many || words.count > 1) {
				complain(L"\r\n");
				complain(found.items[item].cFileName);
				complain(L"\r\n\r\n\r\n");
			}
			HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
			if (file == INVALID_HANDLE_VALUE) {
				complain(file_error(GetLastError()));
				status = errorlevel = 1;
			} else {
				type_file(file);
				CloseHandle(file);
			}
			release(path);
		}
		release(found.items);
		release(full);
		release(folder);
		release(pattern);
	}
	free_words(&words);
	return status;
}

/* MKLINK [/D | /H | /J] link target. */
static DWORD command_mklink(const WCHAR *rest)
{
	struct Words words;
	split_words(rest, &words, FALSE);
	BOOL folder = FALSE, hard = FALSE, junction = FALSE;
	const WCHAR *names[2] = {NULL, NULL};
	DWORD count = 0;
	for (DWORD index = 0; index < words.count; ++index) {
		const WCHAR *word = words.items[index];
		if (equal(word, L"/D"))
			folder = TRUE;
		else if (equal(word, L"/H"))
			hard = TRUE;
		else if (equal(word, L"/J"))
			junction = TRUE;
		else if (count < 2 && word[0] != L'/')
			names[count++] = word;
		else {
			free_words(&words);
			return syntax_error();
		}
	}
	if (count != 2) {
		free_words(&words);
		return syntax_error();
	}
	WCHAR *link = unquoted(names[0]), *target = unquoted(names[1]);
	typedef BOOLEAN(WINAPI * Symbolic)(LPCWSTR, LPCWSTR, DWORD);
	typedef BOOL(WINAPI * Hard)(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES);
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	BOOL made = FALSE;
	if (hard) {
		Hard create = kernel ? (Hard)(void (*)(void))GetProcAddress(kernel, "CreateHardLinkW") : NULL;
		made = create && create(link, target, NULL);
	} else {
		Symbolic create = kernel ? (Symbolic)(void (*)(void))GetProcAddress(kernel, "CreateSymbolicLinkW") : NULL;
		made = create && create(link, target, (folder || junction ? 1 : 0) | 2) != 0;
	}
	DWORD status = 0;
	if (made) {
		say(hard ? L"Hardlink created for " : junction ? L"Junction created for " : L"symbolic link created for ");
		say(link);
		say(L" <<===>> ");
		say(target);
		say(L"\r\n");
	} else {
		const DWORD error = GetLastError();
		complain(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? L"Cannot create a file when that file already exists.\r\n" :
			file_error(error));
		status = errorlevel = 1;
	}
	release(link);
	release(target);
	free_words(&words);
	return status;
}

/* ASSOC and FTYPE read and write HKEY_CLASSES_ROOT; the registry functions are found as they are needed. */
typedef LONG(WINAPI *RegistryOpen)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LONG(WINAPI *RegistryQuery)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG(WINAPI *RegistryEnumerate)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPWSTR, LPDWORD, PFILETIME);
typedef LONG(WINAPI *RegistryCreate)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
typedef LONG(WINAPI *RegistrySet)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD);
typedef LONG(WINAPI *RegistryDelete)(HKEY, LPCWSTR);
typedef LONG(WINAPI *RegistryClose)(HKEY);

static struct {
	RegistryOpen open;
	RegistryQuery query;
	RegistryEnumerate enumerate;
	RegistryCreate create;
	RegistrySet set;
	RegistryDelete remove;
	RegistryClose close;
} registry;

static BOOL load_registry(void)
{
	if (registry.open)
		return TRUE;
	HMODULE module = LoadLibraryW(L"advapi32.dll");
	if (!module)
		return FALSE;
	registry.open = (RegistryOpen)(void (*)(void))GetProcAddress(module, "RegOpenKeyExW");
	registry.query = (RegistryQuery)(void (*)(void))GetProcAddress(module, "RegQueryValueExW");
	registry.enumerate = (RegistryEnumerate)(void (*)(void))GetProcAddress(module, "RegEnumKeyExW");
	registry.create = (RegistryCreate)(void (*)(void))GetProcAddress(module, "RegCreateKeyExW");
	registry.set = (RegistrySet)(void (*)(void))GetProcAddress(module, "RegSetValueExW");
	registry.remove = (RegistryDelete)(void (*)(void))GetProcAddress(module, "RegDeleteKeyW");
	registry.close = (RegistryClose)(void (*)(void))GetProcAddress(module, "RegCloseKey");
	if (!registry.open || !registry.query || !registry.enumerate || !registry.create || !registry.set || !registry.remove || !registry.close) {
		registry.open = NULL;
		return FALSE;
	}
	return TRUE;
}

/* The default value of a key under HKEY_CLASSES_ROOT, or NULL. */
static WCHAR *class_value(const WCHAR *key)
{
	HKEY handle;
	if (registry.open(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &handle))
		return NULL;
	WCHAR *value = allocate(4096 * sizeof(WCHAR));
	DWORD type = 0, size = 4095 * sizeof(WCHAR);
	const LONG result = registry.query(handle, NULL, NULL, &type, (BYTE *)value, &size);
	registry.close(handle);
	if (result || (type != REG_SZ && type != REG_EXPAND_SZ) || !value[0]) {
		release(value);
		return NULL;
	}
	return value;
}

static BOOL set_class_value(const WCHAR *key, const WCHAR *value)
{
	HKEY handle;
	if (registry.create(HKEY_CLASSES_ROOT, key, 0, NULL, 0, KEY_WRITE, NULL, &handle, NULL))
		return FALSE;
	const LONG result = registry.set(handle, NULL, 0, REG_SZ, (const BYTE *)value, (length(value) + 1) * sizeof(WCHAR));
	registry.close(handle);
	return !result;
}

static WCHAR *open_command_key(const WCHAR *type)
{
	struct Text key = {0};
	text_put(&key, type);
	text_put(&key, L"\\shell\\open\\command");
	return text_take(&key);
}

/* ASSOC [.ext[=[type]]] and FTYPE [type[=[command]]]: listing, showing, setting and removing. */
static DWORD associations(const WCHAR *rest, BOOL types)
{
	if (!load_registry())
		return failure(L"The registry cannot be reached.\r\n");
	WCHAR *argument = trimmed(rest);
	WCHAR *equals = argument;
	while (*equals && *equals != L'=')
		++equals;
	DWORD status = 0;
	if (!*argument) {
		struct Text text = {0};
		WCHAR name[256];
		for (DWORD index = 0;; ++index) {
			DWORD size = 256;
			if (registry.enumerate(HKEY_CLASSES_ROOT, index, name, &size, NULL, NULL, NULL, NULL))
				break;
			if (!types && name[0] != L'.')
				continue;
			WCHAR *key = types ? open_command_key(name) : copy_of(name);
			WCHAR *value = class_value(key);
			release(key);
			if (value) {
				text_put(&text, name);
				text_char(&text, L'=');
				text_put(&text, value);
				text_put(&text, L"\r\n");
				release(value);
			}
			if (text.size > 4096)
				say_text(&text);
		}
		say_text(&text);
		release(text.data);
	} else if (*equals) {
		*equals = 0;
		WCHAR *key = types ? open_command_key(argument) : copy_of(argument);
		if (!equals[1]) {
			if (types) {
				WCHAR *value = class_value(key);
				if (value)
					set_class_value(key, L"");
				release(value);
			} else
				registry.remove(HKEY_CLASSES_ROOT, key);
		} else if (!set_class_value(key, equals + 1)) {
			complain(L"Access is denied.\r\n");
			status = errorlevel = 1;
		} else {
			say(argument);
			say(L"=");
			say(equals + 1);
			say(L"\r\n");
		}
		release(key);
	} else {
		WCHAR *key = types ? open_command_key(argument) : copy_of(argument);
		WCHAR *value = class_value(key);
		release(key);
		if (value) {
			say(argument);
			say(L"=");
			say(value);
			say(L"\r\n");
			release(value);
		} else {
			complain(types ? L"File type '" : L"File association not found for extension ");
			complain(argument);
			complain(types ? L"' not found or no open command associated with it.\r\n" : L"\r\n");
			status = errorlevel = 1;
		}
	}
	release(argument);
	return status;
}

static DWORD command_assoc(const WCHAR *rest) { return associations(rest, FALSE); }
static DWORD command_ftype(const WCHAR *rest) { return associations(rest, TRUE); }

static DWORD command_rem(const WCHAR *rest)
{
	(void)rest;
	return 0;
}

struct Builtin {
	const WCHAR *name;
	DWORD (*run)(const WCHAR *rest);
	const WCHAR *usage;
};

static const struct Builtin builtins[] = {
	{L"ASSOC", command_assoc, L"Displays or modifies file extension associations.\r\n\r\nASSOC [.ext[=[fileType]]]\r\n"},
	{L"BREAK", command_nothing, L"Sets or clears extended CTRL+C checking.\r\n\r\nBREAK [ON | OFF]\r\n"},
	{L"CALL", command_call, L"Calls one batch program from another.\r\n\r\nCALL [drive:][path]filename [batch-parameters]\r\nCALL :label [arguments]\r\n"},
	{L"CD", command_cd, L"Displays the name of or changes the current directory.\r\n\r\nCD [/D] [drive:][path]\r\nCD [..]\r\n"},
	{L"CHDIR", command_cd, L"Displays the name of or changes the current directory.\r\n\r\nCHDIR [/D] [drive:][path]\r\nCHDIR [..]\r\n"},
	{L"CLS", command_cls, L"Clears the screen.\r\n\r\nCLS\r\n"},
	{L"COLOR", command_color, L"Sets the default console foreground and background colors.\r\n\r\nCOLOR [attr]\r\n"},
	{L"COPY", command_copy, L"Copies one or more files to another location.\r\n\r\nCOPY [/Y | /-Y] source [+ source [+ ...]] [destination]\r\n"},
	{L"DATE", command_date, L"Displays or sets the date.\r\n\r\nDATE [/T | date]\r\n"},
	{L"DEL", command_del, L"Deletes one or more files.\r\n\r\nDEL [/P] [/F] [/S] [/Q] [/A[[:]attributes]] names\r\n"},
	{L"DIR", command_dir, L"Displays a list of files and subdirectories in a directory.\r\n\r\n"
		L"DIR [drive:][path][filename] [/A[[:]attributes]] [/B] [/L] [/O[[:]sortorder]] [/S] [/W] [/-C]\r\n"},
	{L"ECHO", command_echo, L"Displays messages, or turns command-echoing on or off.\r\n\r\n  ECHO [ON | OFF]\r\n  ECHO [message]\r\n"},
	{L"ENDLOCAL", command_endlocal, L"Ends localization of environment changes in a batch file.\r\n\r\nENDLOCAL\r\n"},
	{L"ERASE", command_del, L"Deletes one or more files.\r\n\r\nERASE [/P] [/F] [/S] [/Q] [/A[[:]attributes]] names\r\n"},
	{L"EXIT", command_exit, L"Quits the command processor or the current batch script.\r\n\r\nEXIT [/B] [exitCode]\r\n"},
	{L"FTYPE", command_ftype, L"Displays or modifies file types used in file extension associations.\r\n\r\nFTYPE [fileType[=[openCommandString]]]\r\n"},
	{L"GOTO", command_goto, L"Directs the command processor to a labeled line in a batch program.\r\n\r\nGOTO label\r\nGOTO :EOF\r\n"},
	{L"MD", command_md, L"Creates a directory.\r\n\r\nMD [drive:]path\r\n"},
	{L"MKDIR", command_md, L"Creates a directory.\r\n\r\nMKDIR [drive:]path\r\n"},
	{L"MKLINK", command_mklink, L"Creates a symbolic link.\r\n\r\nMKLINK [[/D] | [/H] | [/J]] Link Target\r\n"},
	{L"MOVE", command_move, L"Moves files and renames files and directories.\r\n\r\nMOVE [/Y | /-Y] [drive:][path]filename1[,...] destination\r\n"},
	{L"PATH", command_path, L"Displays or sets a search path for executable files.\r\n\r\nPATH [[drive:]path[;...][;%PATH%]]\r\nPATH ;\r\n"},
	{L"PAUSE", command_pause, L"Suspends processing of a batch program and displays the message\r\n    Press any key to continue . . .\r\n"},
	{L"POPD", command_popd, L"Changes to the directory stored by the PUSHD command.\r\n\r\nPOPD\r\n"},
	{L"PROMPT", command_prompt, L"Changes the command prompt.\r\n\r\nPROMPT [text]\r\n"},
	{L"PUSHD", command_pushd, L"Stores the current directory for use by the POPD command, then\r\nchanges to the specified directory.\r\n\r\nPUSHD [path]\r\n"},
	{L"RD", command_rd, L"Removes (deletes) a directory.\r\n\r\nRD [/S] [/Q] [drive:]path\r\n"},
	{L"REM", command_rem, L"Records comments (remarks) in a batch file.\r\n\r\nREM [comment]\r\n"},
	{L"REN", command_ren, L"Renames a file or files.\r\n\r\nREN [drive:][path]filename1 filename2\r\n"},
	{L"RENAME", command_ren, L"Renames a file or files.\r\n\r\nRENAME [drive:][path]filename1 filename2\r\n"},
	{L"RMDIR", command_rd, L"Removes (deletes) a directory.\r\n\r\nRMDIR [/S] [/Q] [drive:]path\r\n"},
	{L"SET", command_set, L"Displays, sets, or removes environment variables.\r\n\r\nSET [variable=[string]]\r\nSET /A expression\r\nSET /P variable=[promptString]\r\n"},
	{L"SETLOCAL", command_setlocal, L"Begins localization of environment changes in a batch file.\r\n\r\n"
		L"SETLOCAL [ENABLEDELAYEDEXPANSION | DISABLEDELAYEDEXPANSION] [ENABLEEXTENSIONS | DISABLEEXTENSIONS]\r\n"},
	{L"SHIFT", command_shift, L"Changes the position of replaceable parameters in a batch file.\r\n\r\nSHIFT [/n]\r\n"},
	{L"START", command_start, L"Starts a program or command.\r\n\r\nSTART [\"title\"] [/D path] [/WAIT] [/B] [command/program] [parameters]\r\n"},
	{L"TIME", command_time, L"Displays or sets the system time.\r\n\r\nTIME [/T | time]\r\n"},
	{L"TITLE", command_title, L"Sets the window title for the command prompt window.\r\n\r\nTITLE [string]\r\n"},
	{L"TYPE", command_type, L"Displays the contents of a text file or files.\r\n\r\nTYPE [drive:][path]filename\r\n"},
	{L"VER", command_ver, L"Displays the Raven Command Processor version.\r\n\r\nVER\r\n"},
	{L"VERIFY", command_verify, L"Tells cmd whether to verify that your files are written correctly.\r\n\r\nVERIFY [ON | OFF]\r\n"},
	{L"VOL", command_vol, L"Displays the disk volume label and serial number, if they exist.\r\n\r\nVOL [drive:]\r\n"},
};

/* Runs one command: a drive change, a built-in, or a program or batch file. */
static DWORD run_simple(const WCHAR *line)
{
	const WCHAR *command = line;
	while (delimiter(*command) || *command == L'@')
		++command;
	if (!*command || *command == L':')
		return 0;
	/* X: alone changes to that drive. */
	if (letter(command[0]) && command[1] == L':') {
		const WCHAR *after = command + 2;
		while (blank(*after))
			++after;
		if (!*after)
			return change_drive(command[0]);
	}
	/* A built-in's name ends at a blank or at the characters cmd lets follow it directly: ECHO. CD\ CD.. ECHO( */
	const WCHAR *end = command;
	while (*end && !blank(*end) && *end != L'.' && *end != L'\\' && *end != L'/' && *end != L'(' && *end != L'=' && *end != L',' &&
		*end != L';' && *end != L'+' && *end != L'"' && *end != L':' && *end != L'[' && *end != L']')
		++end;
	const DWORD size = (DWORD)(end - command);
	for (DWORD index = 0; size && size < 16 && index < sizeof builtins / sizeof builtins[0]; ++index) {
		const struct Builtin *builtin = &builtins[index];
		if (length(builtin->name) != size || !starts(command, builtin->name))
			continue;
		const WCHAR *rest = end;
		const WCHAR *option = rest;
		while (blank(*option))
			++option;
		if (option[0] == L'/' && option[1] == L'?' && builtin->run != command_echo) {
			say(builtin->usage);
			return 0;
		}
		return builtin->run(rest);
	}
	WCHAR program[MAX_PATH], path[MAX_PATH];
	const WCHAR *arguments;
	first_word(command, program, MAX_PATH, &arguments);
	if (!find_program(program, path)) {
		complain(L"'");
		complain(program);
		complain(L"' is not recognized as an internal or external command,\r\noperable program or batch file.\r\n");
		errorlevel = 9009;
		return 9009;
	}
	if (batch_file(path))
		return invoke_batch(path, arguments);
	return run_program(path, command);
}

/* The prompt's line editor, over the console's key events: Left, Right, Home, End (with Ctrl, by words, or deleting to either end),
   Backspace, Delete, Insert, Escape, Up and Down through the commands typed before, and Tab completing file names. */
struct Editor {
	WCHAR *buffer;
	DWORD size, at, shown;
	COORD start;
	SHORT width, height;
};

static WCHAR *history[64];
static DWORD history_count;

static void editor_place(struct Editor *editor, DWORD index)
{
	LONG linear = (LONG)editor->start.Y * editor->width + editor->start.X + (LONG)index;
	if (linear < 0)
		linear = 0;
	COORD at = {(SHORT)(linear % editor->width), (SHORT)(linear / editor->width)};
	if (at.Y >= editor->height) {
		at.Y = (SHORT)(editor->height - 1);
		at.X = (SHORT)(editor->width - 1);
	}
	SetConsoleCursorPosition(output, at);
}

/* After the line was written out to `end` characters, how far the screen scrolled under it. */
static void editor_follow(struct Editor *editor, DWORD end)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	if (!GetConsoleScreenBufferInfo(output, &info))
		return;
	const LONG linear = (LONG)editor->start.Y * editor->width + editor->start.X + (LONG)end;
	LONG row = linear / editor->width;
	if (end && !(linear % editor->width) && info.dwCursorPosition.X)
		--row;
	if (row > info.dwCursorPosition.Y)
		editor->start.Y = (SHORT)(editor->start.Y - (row - info.dwCursorPosition.Y));
}

static void editor_redraw(struct Editor *editor)
{
	DWORD written;
	editor_place(editor, 0);
	WriteConsoleW(output, editor->buffer, editor->size, &written, NULL);
	DWORD end = editor->size;
	for (; end < editor->shown; ++end)
		WriteConsoleW(output, L" ", 1, &written, NULL);
	editor_follow(editor, end);
	editor->shown = editor->size;
	editor_place(editor, editor->at);
}

static void editor_set(struct Editor *editor, const WCHAR *text)
{
	editor->size = 0;
	for (; text && *text && editor->size < LINE_LIMIT - 1; ++text)
		editor->buffer[editor->size++] = *text;
	editor->at = editor->size;
	editor_redraw(editor);
}

static void editor_remove(struct Editor *editor, DWORD from, DWORD to)
{
	memmove(editor->buffer + from, editor->buffer + to, (editor->size - to) * sizeof(WCHAR));
	editor->size -= to - from;
	editor->at = from;
	editor_redraw(editor);
}

/* Tab completion: the names that start with the word before the cursor, one after another on each press. */
struct Completion {
	BOOL active;
	DWORD start, end, index;
	struct Found found;
	WCHAR *prefix;
};

static void completion_end(struct Completion *completion)
{
	if (!completion->active)
		return;
	release(completion->found.items);
	release(completion->prefix);
	completion->active = FALSE;
}

static void complete(struct Editor *editor, struct Completion *completion, BOOL backwards)
{
	if (!completion->active) {
		DWORD start = 0;
		BOOL quoted = FALSE;
		for (DWORD index = 0; index < editor->at; ++index)
			if (editor->buffer[index] == L'"')
				quoted = !quoted;
			else if (!quoted && (blank(editor->buffer[index]) || editor->buffer[index] == L'>' || editor->buffer[index] == L'<' ||
				editor->buffer[index] == L'&' || editor->buffer[index] == L'|'))
				start = index + 1;
		struct Text word = {0};
		for (DWORD index = start; index < editor->at; ++index)
			if (editor->buffer[index] != L'"')
				text_char(&word, editor->buffer[index]);
		DWORD prefix = word.size;
		while (prefix && word.data[prefix - 1] != L'\\' && word.data[prefix - 1] != L'/' && word.data[prefix - 1] != L':')
			--prefix;
		completion->prefix = duplicate(word.data ? word.data : L"", prefix);
		text_char(&word, L'*');
		find_all(word.data, &completion->found, TRUE, TRUE);
		release(word.data);
		if (!completion->found.count) {
			release(completion->found.items);
			release(completion->prefix);
			return;
		}
		completion->active = TRUE;
		completion->start = start;
		completion->end = editor->at;
		completion->index = backwards ? completion->found.count - 1 : 0;
	} else
		completion->index = (completion->index + (backwards ? completion->found.count - 1 : 1)) % completion->found.count;
	struct Text replacement = {0};
	text_put(&replacement, completion->prefix);
	text_put(&replacement, completion->found.items[completion->index].cFileName);
	BOOL quote = FALSE;
	for (DWORD index = 0; index < replacement.size; ++index) {
		const WCHAR character = replacement.data[index];
		for (const WCHAR *special = L" &()[]{}^=;!'+,`~"; *special; ++special)
			quote |= character == *special;
	}
	if (quote) {
		struct Text quoted = {0};
		text_char(&quoted, L'"');
		text_put(&quoted, replacement.data);
		text_char(&quoted, L'"');
		release(replacement.data);
		replacement = quoted;
	}
	const DWORD tail = editor->size - completion->end;
	if (completion->start + replacement.size + tail < LINE_LIMIT) {
		memmove(editor->buffer + completion->start + replacement.size, editor->buffer + completion->end, tail * sizeof(WCHAR));
		memcpy(editor->buffer + completion->start, replacement.data, replacement.size * sizeof(WCHAR));
		editor->size = completion->start + replacement.size + tail;
		completion->end = completion->start + replacement.size;
		editor->at = completion->end;
		editor_redraw(editor);
	}
	release(replacement.data);
}

static BOOL word_character(WCHAR character) { return !blank(character); }

static BOOL edit_line(struct Text *line)
{
	DWORD mode, count;
	GetConsoleMode(input, &mode);
	SetConsoleMode(input, (mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)) | ENABLE_WINDOW_INPUT);
	struct Editor editor;
	struct Completion completion;
	memset(&editor, 0, sizeof editor);
	memset(&completion, 0, sizeof completion);
	editor.buffer = allocate(LINE_LIMIT * sizeof(WCHAR));
	CONSOLE_SCREEN_BUFFER_INFO info;
	editor.width = 80;
	editor.height = 25;
	if (GetConsoleScreenBufferInfo(output, &info)) {
		editor.start = info.dwCursorPosition;
		editor.width = info.dwSize.X > 0 ? info.dwSize.X : 80;
		editor.height = info.dwSize.Y > 0 ? info.dwSize.Y : 25;
	}
	DWORD recall = history_count;
	BOOL insert = TRUE, result = TRUE;
	for (;;) {
		INPUT_RECORD record;
		if (!ReadConsoleInputW(input, &record, 1, &count) || !count) {
			result = editor.size > 0;
			break;
		}
		if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
			if (record.Event.WindowBufferSizeEvent.dwSize.X > 0)
				editor.width = record.Event.WindowBufferSizeEvent.dwSize.X;
			if (record.Event.WindowBufferSizeEvent.dwSize.Y > 0)
				editor.height = record.Event.WindowBufferSizeEvent.dwSize.Y;
			continue;
		}
		if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
			continue;
		const KEY_EVENT_RECORD *key = &record.Event.KeyEvent;
		const WORD code = key->wVirtualKeyCode;
		const WCHAR character = key->uChar.UnicodeChar;
		const BOOL control = (key->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
		const BOOL shift = (key->dwControlKeyState & SHIFT_PRESSED) != 0;
		if (code != VK_TAB)
			completion_end(&completion);
		if (code == VK_RETURN || character == L'\r' || character == L'\n') {
			editor_place(&editor, editor.size);
			say(L"\r\n");
			break;
		}
		if (character == 3) {
			editor_place(&editor, editor.size);
			say(L"^C\r\n");
			editor.size = 0;
			break;
		}
		for (WORD times = key->wRepeatCount ? key->wRepeatCount : 1; times; --times) {
			if (code == VK_BACK || character == 8 || character == 127) {
				if (editor.at)
					editor_remove(&editor, editor.at - 1, editor.at);
			} else if (code == VK_DELETE) {
				if (editor.at < editor.size)
					editor_remove(&editor, editor.at, editor.at + 1);
			} else if (code == VK_LEFT) {
				if (control) {
					while (editor.at && !word_character(editor.buffer[editor.at - 1]))
						--editor.at;
					while (editor.at && word_character(editor.buffer[editor.at - 1]))
						--editor.at;
				} else if (editor.at)
					--editor.at;
				editor_place(&editor, editor.at);
			} else if (code == VK_RIGHT) {
				if (control) {
					while (editor.at < editor.size && word_character(editor.buffer[editor.at]))
						++editor.at;
					while (editor.at < editor.size && !word_character(editor.buffer[editor.at]))
						++editor.at;
				} else if (editor.at < editor.size)
					++editor.at;
				else if (history_count && editor.size < length(history[history_count - 1])) {
					/* At the end of the line, Right copies the next character of the last command, as F1 does. */
					editor.buffer[editor.size] = history[history_count - 1][editor.size];
					++editor.size;
					++editor.at;
					editor_redraw(&editor);
				}
				editor_place(&editor, editor.at);
			} else if (code == VK_HOME) {
				if (control)
					editor_remove(&editor, 0, editor.at);
				editor.at = 0;
				editor_place(&editor, 0);
			} else if (code == VK_END) {
				if (control) {
					editor.size = editor.at;
					editor_redraw(&editor);
				}
				editor.at = editor.size;
				editor_place(&editor, editor.at);
			} else if (code == VK_UP) {
				if (recall)
					editor_set(&editor, history[--recall]);
			} else if (code == VK_DOWN) {
				if (recall < history_count)
					++recall;
				editor_set(&editor, recall < history_count ? history[recall] : L"");
			} else if (code == VK_ESCAPE) {
				recall = history_count;
				editor_set(&editor, L"");
			} else if (code == VK_INSERT)
				insert = !insert;
			else if (code == VK_F3) {
				if (history_count)
					for (const WCHAR *last = history[history_count - 1]; editor.size < length(last) && editor.size < LINE_LIMIT - 1;)
						editor.buffer[editor.size] = last[editor.size], ++editor.size;
				editor.at = editor.size;
				editor_redraw(&editor);
			} else if (code == VK_TAB)
				complete(&editor, &completion, shift);
			else if (character >= 32 && !(control && character < 128 && letter(character))) {
				if (editor.at == editor.size || insert) {
					if (editor.size >= LINE_LIMIT - 1)
						continue;
					const BOOL appending = editor.at == editor.size;
					memmove(editor.buffer + editor.at + 1, editor.buffer + editor.at, (editor.size - editor.at) * sizeof(WCHAR));
					editor.buffer[editor.at++] = character;
					++editor.size;
					if (appending) {
						/* Typing at the end of the line writes the character, without drawing the line again. */
						WriteConsoleW(output, &character, 1, &count, NULL);
						editor.shown = editor.size;
						editor_follow(&editor, editor.size);
					} else
						editor_redraw(&editor);
				} else {
					editor.buffer[editor.at++] = character;
					editor_redraw(&editor);
				}
			}
		}
	}
	completion_end(&completion);
	SetConsoleMode(input, mode);
	text_add(line, editor.buffer, editor.size);
	if (editor.size && (!history_count || !same(history[history_count - 1], line->data))) {
		if (history_count == 64) {
			release(history[0]);
			memmove(history, history + 1, 63 * sizeof(WCHAR *));
			--history_count;
		}
		history[history_count++] = copy_of(line->data);
	}
	release(editor.buffer);
	if (!line->data)
		text_add(line, L"", 0);
	return result;
}

/* The prompt's lines: from the line editor at a console, else from the input as it comes, shown after the prompt as cmd does. */
static BOOL prompt_source(void *state, BOOL more, struct Text *line)
{
	(void)state;
	if (more)
		say(L"More? ");
	const BOOL read = interactive_input ? edit_line(line) : read_input_line(line);
	if (read && !interactive_input && echo_on) {
		say(line->data);
		say(L"\r\n");
	}
	return read;
}

/* Ctrl+C belongs to the program running in the foreground; the processor notes it, to stop what it runs and ask about a batch job. */
static BOOL WINAPI on_control(DWORD event)
{
	if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT)
		interrupted = 1;
	return TRUE;
}

/* What follows /C or /K, with quotes treated as cmd /? describes: kept when there are exactly two, with nothing special and at least
   one blank between them, around the name of a program; otherwise a leading quote and the last quote are removed. */
static WCHAR *command_text(const WCHAR *text, BOOL strip_always)
{
	while (blank(*text))
		++text;
	const DWORD size = length(text);
	WCHAR *copy = duplicate(text, size);
	if (*copy != L'"')
		return copy;
	DWORD quotes = 0, last = 0;
	BOOL special = FALSE, spaced = FALSE;
	for (DWORD index = 0; index < size; ++index) {
		if (copy[index] == L'"') {
			++quotes;
			last = index;
		} else if (quotes == 1) {
			const WCHAR character = copy[index];
			special |= character == L'&' || character == L'<' || character == L'>' || character == L'(' || character == L')' ||
				character == L'@' || character == L'^' || character == L'|';
			spaced |= blank(character);
		}
	}
	if (!strip_always && quotes == 2 && !special && spaced) {
		WCHAR name[MAX_PATH], path[MAX_PATH];
		DWORD out = 0;
		for (DWORD index = 1; index < last && out < MAX_PATH - 1; ++index)
			name[out++] = copy[index];
		name[out] = 0;
		if (find_program(name, path))
			return copy;
	}
	DWORD out = 0;
	for (DWORD index = 1; index < size; ++index)
		if (index != last)
			copy[out++] = copy[index];
	copy[out] = 0;
	return copy;
}

int raven_cmd_entry(void)
{
	input = GetStdHandle(STD_INPUT_HANDLE);
	output = GetStdHandle(STD_OUTPUT_HANDLE);
	error_output = GetStdHandle(STD_ERROR_HANDLE);
	DWORD mode;
	interactive_input = GetConsoleMode(input, &mode);
	SetConsoleCtrlHandler(on_control, TRUE);
	GetModuleFileNameW(NULL, self, MAX_PATH);
	random_state = GetTickCount();
	command_line = GetCommandLineW();

	/* The options before /C or /K; what follows either is the command. */
	const WCHAR *ignored;
	WCHAR program[MAX_PATH];
	first_word(command_line, program, MAX_PATH, &ignored);
	const WCHAR *at = ignored;
	BOOL run_once = FALSE, keep = FALSE, strip = FALSE;
	const WCHAR *command = NULL;
	for (;;) {
		while (blank(*at))
			++at;
		if (*at != L'/')
			break;
		const WCHAR option = folded(at[1]);
		if (option == L'C' || option == L'K' || option == L'R') {
			run_once = option != L'K';
			keep = option == L'K';
			command = at + 2;
			break;
		}
		if (option == L'Q')
			echo_on = FALSE;
		else if (option == L'S')
			strip = TRUE;
		else if (option == L'V' && at[2] == L':')
			delayed = folded(at[3]) == L'O' && folded(at[4]) == L'N';
		else if (option == L'T' && at[2] == L':') {
			LONG color = 0;
			WCHAR digits[5] = {L'0', L'x', at[3], at[4], 0};
			if (read_integer(digits, &color) && (color >> 4) != (color & 15))
				SetConsoleTextAttribute(output, (WORD)color);
		}
		at += 2;
		while (*at && !blank(*at) && *at != L'/')
			++at;
	}
	if (!environment_value(L"PROMPT"))
		SetEnvironmentVariableW(L"PROMPT", L"$P$G");
	remember_drive();

	if (command) {
		WCHAR *text = command_text(command, strip);
		run_text(text);
		release(text);
		if (run_once || leaving)
			ExitProcess(errorlevel);
	}
	if (!keep)
		say(L"Raven Command Processor [Version 0.2]\r\n\r\n");
	prompting = TRUE;
	while (!leaving) {
		interrupted = 0;
		if (echo_on)
			show_prompt();
		WCHAR *text;
		BOOL ended, bad;
		struct Node *tree = read_logical(prompt_source, NULL, &text, &ended, &bad);
		if (ended)
			break;
		execute(tree);
		free_node(tree);
		release(text);
		if (echo_on && interactive_input && !leaving)
			say(L"\r\n");
	}
	ExitProcess(errorlevel);
	return 0;
}
