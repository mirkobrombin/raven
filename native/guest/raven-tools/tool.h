/* What the command-line tools of a Raven prefix share: memory, text, output, their arguments and reading their input. Raven code on
   the public Windows headers, without a C runtime; each tool is written from Microsoft's public description of it and from how it
   behaves, and no Windows code is used or consulted. */

#include <windows.h>

#define UNUSED __attribute__((unused))

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

static HANDLE input, output, error_output;

static UNUSED void *allocate(SIZE_T size) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1); }
static UNUSED void release(void *block) { if (block) HeapFree(GetProcessHeap(), 0, block); }

static UNUSED DWORD length(const WCHAR *text)
{
	DWORD size = 0;
	while (text[size])
		++size;
	return size;
}

static UNUSED WCHAR *duplicate(const WCHAR *text, DWORD size)
{
	WCHAR *copy = allocate((size + 1) * sizeof(WCHAR));
	for (DWORD index = 0; index < size; ++index)
		copy[index] = text[index];
	return copy;
}

static UNUSED WCHAR *copy_of(const WCHAR *text) { return duplicate(text, length(text)); }
static UNUSED WCHAR folded(WCHAR character) { return character >= L'a' && character <= L'z' ? (WCHAR)(character - 32) : character; }
static UNUSED WCHAR lowered(WCHAR character) { return character >= L'A' && character <= L'Z' ? (WCHAR)(character + 32) : character; }
static UNUSED BOOL blank(WCHAR character) { return character == L' ' || character == L'\t'; }
static UNUSED BOOL digit(WCHAR character) { return character >= L'0' && character <= L'9'; }

static UNUSED BOOL starts(const WCHAR *text, const WCHAR *word)
{
	for (; *word; ++text, ++word)
		if (folded(*text) != folded(*word))
			return FALSE;
	return TRUE;
}

static UNUSED BOOL equal(const WCHAR *left, const WCHAR *right) { return starts(left, right) && !left[length(right)]; }

static UNUSED int compare_folded(const WCHAR *left, const WCHAR *right)
{
	while (*left && folded(*left) == folded(*right))
		++left, ++right;
	return (int)folded(*left) - (int)folded(*right);
}

static UNUSED BOOL wild(const WCHAR *text)
{
	for (; *text; ++text)
		if (*text == L'*' || *text == L'?')
			return TRUE;
	return FALSE;
}

/* Text that grows as it is written; `data` always ends with a zero once anything was added. */
struct Text {
	WCHAR *data;
	DWORD size, capacity;
};

static UNUSED void text_add(struct Text *text, const WCHAR *characters, DWORD size)
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

static UNUSED void text_put(struct Text *text, const WCHAR *characters) { if (characters) text_add(text, characters, length(characters)); }
static UNUSED void text_char(struct Text *text, WCHAR character) { text_add(text, &character, 1); }

static UNUSED WCHAR *text_take(struct Text *text)
{
	if (!text->data)
		text_add(text, L"", 0);
	WCHAR *data = text->data;
	text->data = NULL;
	text->size = text->capacity = 0;
	return data;
}

/* Writes text to a handle: as characters to a console, else in the console's code page, as the tools write to files and pipes. */
static UNUSED void write_to(HANDLE handle, const WCHAR *text, DWORD size)
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

static UNUSED void say(const WCHAR *text) { write_to(output, text, length(text)); }
static UNUSED void complain(const WCHAR *text) { write_to(error_output, text, length(text)); }

static UNUSED void say_text(struct Text *text)
{
	if (text->size)
		write_to(output, text->data, text->size);
	text->size = 0;
	if (text->data)
		text->data[0] = 0;
}

static UNUSED void put_number(struct Text *text, ULONGLONG value, BOOL grouped)
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
	while (count)
		text_char(text, reversed[--count]);
}

/* A field `width` characters wide, its text on the right, or on the left with `left`. */
static UNUSED void put_field(struct Text *text, const WCHAR *value, DWORD width, BOOL left)
{
	const DWORD size = length(value);
	if (left)
		text_put(text, value);
	for (DWORD pad = size; pad < width; ++pad)
		text_char(text, L' ');
	if (!left)
		text_put(text, value);
}

static UNUSED void put_hex(struct Text *text, ULONGLONG value, int digits, BOOL lower)
{
	const WCHAR *set = lower ? L"0123456789abcdef" : L"0123456789ABCDEF";
	if (digits <= 0) {
		digits = 1;
		while (digits < 16 && (value >> (digits * 4)))
			++digits;
	}
	while (digits--)
		text_char(text, set[(value >> (digits * 4)) & 15]);
}

/* A number as the tools read one: decimal, or hexadecimal after 0x. Whether all of `text` is one. */
static UNUSED BOOL read_number(const WCHAR *text, ULONGLONG *value)
{
	ULONGLONG number = 0;
	BOOL negative = FALSE;
	if (*text == L'-')
		negative = TRUE, ++text;
	if (!*text)
		return FALSE;
	if (text[0] == L'0' && folded(text[1]) == L'X') {
		text += 2;
		if (!*text)
			return FALSE;
		for (; *text; ++text) {
			const WCHAR character = folded(*text);
			if (digit(character))
				number = number * 16 + (ULONGLONG)(character - L'0');
			else if (character >= L'A' && character <= L'F')
				number = number * 16 + (ULONGLONG)(character - L'A' + 10);
			else
				return FALSE;
		}
	} else
		for (; *text; ++text) {
			if (!digit(*text))
				return FALSE;
			number = number * 10 + (ULONGLONG)(*text - L'0');
		}
	*value = negative ? (ULONGLONG)(-(LONGLONG)number) : number;
	return TRUE;
}

/* The program's arguments, split as Windows programs split their command lines: at blanks outside quotes, with backslashes before a
   quote escaping it in pairs. */
struct Arguments {
	WCHAR **items;
	DWORD count;
};

static UNUSED void read_arguments(struct Arguments *arguments)
{
	const WCHAR *at = GetCommandLineW();
	DWORD capacity = 8;
	arguments->items = allocate(capacity * sizeof(WCHAR *));
	arguments->count = 0;
	BOOL first = TRUE;
	for (;;) {
		while (blank(*at))
			++at;
		if (!*at)
			break;
		struct Text word = {0};
		BOOL quoted = FALSE;
		if (first) {
			/* The program's own name: quotes only group, and backslashes stay. */
			for (; *at && (quoted || !blank(*at)); ++at)
				if (*at == L'"')
					quoted = !quoted;
			first = FALSE;
			continue;
		}
		while (*at && (quoted || !blank(*at))) {
			DWORD slashes = 0;
			while (*at == L'\\')
				++slashes, ++at;
			if (*at == L'"') {
				for (DWORD index = 0; index < slashes / 2; ++index)
					text_char(&word, L'\\');
				if (slashes & 1)
					text_char(&word, L'"');
				else if (quoted && at[1] == L'"') {
					text_char(&word, L'"');
					++at;
				} else
					quoted = !quoted;
				++at;
			} else {
				for (DWORD index = 0; index < slashes; ++index)
					text_char(&word, L'\\');
				if (*at && (quoted || !blank(*at)))
					text_char(&word, *at++);
			}
		}
		if (arguments->count == capacity) {
			WCHAR **grown = allocate(capacity * 2 * sizeof(WCHAR *));
			memcpy(grown, arguments->items, capacity * sizeof(WCHAR *));
			release(arguments->items);
			arguments->items = grown;
			capacity *= 2;
		}
		arguments->items[arguments->count++] = text_take(&word);
	}
}

/* Whether an argument is a switch, given with / or -. */
static UNUSED BOOL is_switch(const WCHAR *argument) { return (argument[0] == L'/' || argument[0] == L'-') && argument[1]; }

/* Whether an argument is the switch `name` (written with /), ignoring case and which of / or - it starts with. */
static UNUSED BOOL switch_is(const WCHAR *argument, const WCHAR *name) { return is_switch(argument) && equal(argument + 1, name + 1); }

/* Bytes as text: UTF-16 after its byte order mark, else the console's code page, without a UTF-8 byte order mark. */
static UNUSED WCHAR *decoded(const char *bytes, DWORD size, DWORD *count)
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
	*count = size ? (DWORD)MultiByteToWideChar(GetConsoleCP(), 0, bytes, (int)size, text, (int)size) : 0;
	return text;
}

static UNUSED char *read_all(HANDLE handle, DWORD *size)
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

/* All of a handle's text: a console's lines until Ctrl+Z, or a file's or a pipe's bytes decoded. */
static UNUSED WCHAR *read_text(HANDLE handle, DWORD *count)
{
	DWORD mode;
	if (GetConsoleMode(handle, &mode)) {
		struct Text text = {0};
		WCHAR buffer[1024];
		DWORD got;
		while (ReadConsoleW(handle, buffer, 1024, &got, NULL) && got) {
			DWORD keep = got;
			for (DWORD index = 0; index < got; ++index)
				if (buffer[index] == 26) {
					keep = index;
					break;
				}
			text_add(&text, buffer, keep);
			if (keep < got)
				break;
		}
		*count = text.size;
		return text_take(&text);
	}
	DWORD size;
	char *bytes = read_all(handle, &size);
	WCHAR *text = decoded(bytes, size, count);
	release(bytes);
	return text;
}

static UNUSED WCHAR *read_file_text(const WCHAR *path, DWORD *count)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	WCHAR *text = read_text(file, count);
	CloseHandle(file);
	return text;
}

/* The next line of text from `*at`, without its line end; FALSE after the last. */
static UNUSED BOOL next_line(const WCHAR *text, DWORD size, DWORD *at, DWORD *start, DWORD *line_size)
{
	if (*at >= size)
		return FALSE;
	*start = *at;
	DWORD end = *at;
	while (end < size && text[end] != L'\n')
		++end;
	DWORD stop = end;
	if (stop > *start && text[stop - 1] == L'\r')
		--stop;
	*line_size = stop - *start;
	*at = end < size ? end + 1 : end;
	return TRUE;
}

/* Windows wildcard matching: * and ? against a name, ignoring case; *.* matches every name. */
static UNUSED BOOL matches(const WCHAR *pattern, const WCHAR *name)
{
	if (equal(pattern, L"*.*") || equal(pattern, L"*"))
		return TRUE;
	const WCHAR *star = NULL, *mark = NULL;
	while (*name) {
		if (*pattern == L'?' || (*pattern && *pattern != L'*' && folded(*pattern) == folded(*name)))
			++pattern, ++name;
		else if (*pattern == L'*')
			star = pattern++, mark = name;
		else if (star)
			pattern = star + 1, name = ++mark;
		else
			return FALSE;
	}
	while (*pattern == L'*' || *pattern == L'.')
		++pattern;
	return !*pattern;
}

static UNUSED WCHAR *joined(const WCHAR *folder, const WCHAR *name)
{
	struct Text path = {0};
	text_put(&path, folder);
	if (path.size && path.data[path.size - 1] != L'\\')
		text_char(&path, L'\\');
	text_put(&path, name);
	return text_take(&path);
}

static UNUSED BOOL is_folder(const WCHAR *path)
{
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static UNUSED WCHAR *full_path(const WCHAR *name)
{
	WCHAR *full = allocate(MAX_PATH * 2 * sizeof(WCHAR));
	if (!GetFullPathNameW(*name ? name : L".", MAX_PATH * 2, full, NULL))
		lstrcpynW(full, name, MAX_PATH * 2);
	return full;
}

/* A full path split into its folder and its last part; the folder of C:\x is C:\. */
static UNUSED void split_path(const WCHAR *full, WCHAR **folder, WCHAR **name)
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

/* Reads one key press from the console, for MORE, CHOICE and TIMEOUT; 0 when there is no console. */
static UNUSED WCHAR read_key(HANDLE console, WORD *code)
{
	DWORD mode, count;
	if (!GetConsoleMode(console, &mode))
		return 0;
	SetConsoleMode(console, mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
	WCHAR result = 0;
	for (;;) {
		INPUT_RECORD record;
		if (!ReadConsoleInputW(console, &record, 1, &count) || !count)
			break;
		if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
			continue;
		const WORD key = record.Event.KeyEvent.wVirtualKeyCode;
		if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_CAPITAL)
			continue;
		if (code)
			*code = key;
		result = record.Event.KeyEvent.uChar.UnicodeChar ? record.Event.KeyEvent.uChar.UnicodeChar : 1;
		break;
	}
	SetConsoleMode(console, mode);
	return result;
}

/* The console's input even when the standard input is a pipe, as MORE reads keys while it reads text from the pipe. */
static UNUSED HANDLE console_input(void)
{
	DWORD mode;
	if (GetConsoleMode(input, &mode))
		return input;
	HANDLE console = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	return console != INVALID_HANDLE_VALUE && GetConsoleMode(console, &mode) ? console : NULL;
}

static UNUSED void start_tool(void)
{
	input = GetStdHandle(STD_INPUT_HANDLE);
	output = GetStdHandle(STD_OUTPUT_HANDLE);
	error_output = GetStdHandle(STD_ERROR_HANDLE);
}
