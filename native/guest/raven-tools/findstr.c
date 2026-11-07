/* findstr.exe: FINDSTR [/B] [/E] [/L] [/R] [/S] [/I] [/X] [/V] [/N] [/M] [/O] [/P] [/OFF[LINE]] [/A:attr] [/F:file] [/C:string]
   [/G:file] [/D:dir list] [strings] [[drive:][path]filename[ ...]] — the lines that match any of the strings, literally or as the
   regular expressions FINDSTR /? describes: . * ^ $ [class] [^class] [x-y] \< \> and \x. ERRORLEVEL is 0 when a line matched, 1
   when none did and 2 on an error. */

#include "tool.h"

enum Token { ONE, ANY, CLASS, LINE_START, LINE_END, WORD_START, WORD_END };

struct Piece {
	enum Token token;
	WCHAR character;
	BOOL star, negated;
	/* A class as pairs of first and last characters. */
	WCHAR *ranges;
	DWORD range_count;
};

struct Pattern {
	struct Piece *pieces;
	DWORD count;
};

static BOOL fold, literal_default, regular, at_start, at_end, whole, invert, numbered, names_only, offsets, printable_only, subfolders;
static struct Pattern *patterns;
static DWORD pattern_count, pattern_capacity;

static void add_piece(struct Pattern *pattern, struct Piece piece)
{
	struct Piece *grown = allocate((pattern->count + 1) * sizeof *grown);
	if (pattern->count)
		memcpy(grown, pattern->pieces, pattern->count * sizeof *grown);
	release(pattern->pieces);
	pattern->pieces = grown;
	pattern->pieces[pattern->count++] = piece;
}

static void compile(const WCHAR *text, BOOL as_regular)
{
	struct Pattern pattern = {NULL, 0};
	for (const WCHAR *at = text; *at; ++at) {
		struct Piece piece;
		memset(&piece, 0, sizeof piece);
		piece.token = ONE;
		piece.character = *at;
		if (as_regular) {
			if (*at == L'.')
				piece.token = ANY;
			else if (*at == L'^' && at == text)
				piece.token = LINE_START;
			else if (*at == L'$' && !at[1])
				piece.token = LINE_END;
			else if (*at == L'*' && pattern.count && !pattern.pieces[pattern.count - 1].star &&
				pattern.pieces[pattern.count - 1].token <= CLASS) {
				pattern.pieces[pattern.count - 1].star = TRUE;
				continue;
			} else if (*at == L'\\' && at[1]) {
				++at;
				if (*at == L'<')
					piece.token = WORD_START;
				else if (*at == L'>')
					piece.token = WORD_END;
				else
					piece.character = *at;
			} else if (*at == L'[') {
				const WCHAR *close = at + 1;
				if (*close == L'^')
					++close;
				if (*close == L']')
					++close;
				while (*close && *close != L']')
					++close;
				if (*close) {
					piece.token = CLASS;
					const WCHAR *member = at + 1;
					if (*member == L'^')
						piece.negated = TRUE, ++member;
					piece.ranges = allocate((DWORD)(close - member + 1) * 2 * sizeof(WCHAR));
					for (; member < close; ++member) {
						WCHAR first = *member, last = *member;
						if (member + 2 < close && member[1] == L'-')
							last = member[2], member += 2;
						piece.ranges[piece.range_count * 2] = first;
						piece.ranges[piece.range_count * 2 + 1] = last;
						++piece.range_count;
					}
					at = close;
				}
			}
		}
		add_piece(&pattern, piece);
	}
	if (pattern_count == pattern_capacity) {
		pattern_capacity = pattern_capacity ? pattern_capacity * 2 : 8;
		struct Pattern *grown = allocate(pattern_capacity * sizeof *grown);
		if (pattern_count)
			memcpy(grown, patterns, pattern_count * sizeof *grown);
		release(patterns);
		patterns = grown;
	}
	patterns[pattern_count++] = pattern;
}

static BOOL same_character(WCHAR left, WCHAR right) { return fold ? folded(left) == folded(right) : left == right; }

static BOOL word_character(WCHAR character)
{
	return digit(character) || (folded(character) >= L'A' && folded(character) <= L'Z') || character == L'_' || character >= 128;
}

static BOOL piece_takes(const struct Piece *piece, WCHAR character)
{
	switch (piece->token) {
	case ONE: return same_character(piece->character, character);
	case ANY: return TRUE;
	case CLASS: {
		BOOL inside = FALSE;
		for (DWORD index = 0; index < piece->range_count && !inside; ++index) {
			WCHAR first = piece->ranges[index * 2], last = piece->ranges[index * 2 + 1], probe = character;
			if (fold) {
				first = folded(first);
				last = folded(last);
				probe = folded(probe);
			}
			inside = probe >= first && probe <= last;
		}
		return inside != piece->negated;
	}
	default: return FALSE;
	}
}

/* Whether the pieces from `index` on match the line from `at`; `*end` is where the match ended. */
static BOOL match_here(const struct Pattern *pattern, DWORD index, const WCHAR *line, DWORD size, DWORD at, DWORD *end)
{
	if (index == pattern->count) {
		*end = at;
		return TRUE;
	}
	const struct Piece *piece = &pattern->pieces[index];
	switch (piece->token) {
	case LINE_START: return at == 0 && match_here(pattern, index + 1, line, size, at, end);
	case LINE_END: return at == size && match_here(pattern, index + 1, line, size, at, end);
	case WORD_START:
		return at < size && word_character(line[at]) && (!at || !word_character(line[at - 1])) && match_here(pattern, index + 1, line, size, at, end);
	case WORD_END:
		return at && word_character(line[at - 1]) && (at == size || !word_character(line[at])) && match_here(pattern, index + 1, line, size, at, end);
	default: break;
	}
	if (piece->star) {
		DWORD most = at;
		while (most < size && piece_takes(piece, line[most]))
			++most;
		for (DWORD try = most + 1; try-- > at;)
			if (match_here(pattern, index + 1, line, size, try, end))
				return TRUE;
		return FALSE;
	}
	return at < size && piece_takes(piece, line[at]) && match_here(pattern, index + 1, line, size, at + 1, end);
}

static BOOL line_matches(const WCHAR *line, DWORD size, DWORD *offset)
{
	for (DWORD index = 0; index < pattern_count; ++index) {
		const struct Pattern *pattern = &patterns[index];
		const BOOL anchored = at_start || whole;
		for (DWORD at = 0; at <= size; ++at) {
			DWORD end;
			if (match_here(pattern, 0, line, size, at, &end) && (!(at_end || whole) || end == size)) {
				*offset = at;
				return TRUE;
			}
			if (anchored)
				break;
		}
	}
	return FALSE;
}

static DWORD searched_files;

/* Searches one text, showing matches under its name when several files are searched. */
static DWORD search(const WCHAR *text, DWORD size, const WCHAR *name, BOOL show_name)
{
	struct Text out = {0};
	DWORD at = 0, start, line_size, number = 0, found = 0;
	while (next_line(text, size, &at, &start, &line_size)) {
		++number;
		DWORD offset = 0;
		if (line_matches(text + start, line_size, &offset) == invert)
			continue;
		++found;
		if (names_only) {
			text_put(&out, name);
			text_put(&out, L"\r\n");
			break;
		}
		if (show_name) {
			text_put(&out, name);
			text_char(&out, L':');
		}
		if (numbered) {
			put_number(&out, number, FALSE);
			text_char(&out, L':');
		}
		if (offsets) {
			put_number(&out, start + offset, FALSE);
			text_char(&out, L':');
		}
		text_add(&out, text + start, line_size);
		text_put(&out, L"\r\n");
		if (out.size > 8192)
			say_text(&out);
	}
	say_text(&out);
	release(out.data);
	return found;
}

static BOOL has_unprintable(const WCHAR *text, DWORD size)
{
	for (DWORD index = 0; index < size; ++index)
		if (text[index] < 32 && text[index] != L'\r' && text[index] != L'\n' && text[index] != L'\t' && text[index] != 12 && text[index] != 26)
			return TRUE;
	return FALSE;
}

static DWORD search_file(const WCHAR *path, const WCHAR *shown, BOOL show_name)
{
	DWORD size;
	WCHAR *text = read_file_text(path, &size);
	if (!text) {
		complain(L"FINDSTR: Cannot open ");
		complain(shown);
		complain(L"\r\n");
		return 0;
	}
	++searched_files;
	DWORD found = 0;
	if (!printable_only || !has_unprintable(text, size))
		found = search(text, size, shown, show_name);
	release(text);
	return found;
}

/* The files a name or pattern gives, in its folder and with /S in the folders under it too. */
static DWORD search_pattern(const WCHAR *folder, const WCHAR *shown_folder, const WCHAR *pattern, BOOL show_name, BOOL *any)
{
	DWORD found = 0;
	WIN32_FIND_DATAW data;
	WCHAR *search_path = joined(folder, pattern);
	HANDLE handle = FindFirstFileW(search_path, &data);
	release(search_path);
	if (handle != INVALID_HANDLE_VALUE) {
		do {
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			*any = TRUE;
			WCHAR *path = joined(folder, data.cFileName);
			struct Text shown = {0};
			text_put(&shown, shown_folder);
			text_put(&shown, data.cFileName);
			found += search_file(path, shown.data, show_name);
			release(shown.data);
			release(path);
		} while (FindNextFileW(handle, &data));
		FindClose(handle);
	}
	if (!subfolders)
		return found;
	WCHAR *all = joined(folder, L"*");
	handle = FindFirstFileW(all, &data);
	release(all);
	if (handle != INVALID_HANDLE_VALUE) {
		do {
			if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
				equal(data.cFileName, L".") || equal(data.cFileName, L".."))
				continue;
			WCHAR *sub = joined(folder, data.cFileName);
			struct Text shown = {0};
			text_put(&shown, shown_folder);
			text_put(&shown, data.cFileName);
			text_char(&shown, L'\\');
			found += search_pattern(sub, shown.data, pattern, show_name, any);
			release(shown.data);
			release(sub);
		} while (FindNextFileW(handle, &data));
		FindClose(handle);
	}
	return found;
}

/* Strings for /G: and file names for /F:, one on each line of a file, or of the input when the name is /. */
static WCHAR *list_text(const WCHAR *name, DWORD *size)
{
	if (equal(name, L"/"))
		return read_text(input, size);
	return read_file_text(name, size);
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	struct Text strings = {0};
	BOOL have_strings = FALSE, literal = FALSE;
	WCHAR **files = allocate((arguments.count + 1) * sizeof(WCHAR *));
	DWORD file_count = 0;
	WCHAR *list_file = NULL, *strings_file = NULL;
	struct Text literal_strings = {0};
	BOOL have_literal = FALSE;
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (!is_switch(argument)) {
			if (!have_strings && !have_literal && !strings_file) {
				text_put(&strings, argument);
				have_strings = TRUE;
			} else
				files[file_count++] = arguments.items[index];
			continue;
		}
		/* Switches may be run together, as /SI; those with a value take the rest of the argument. */
		for (const WCHAR *option = argument + 1; *option; ++option) {
			const WCHAR letter = folded(*option);
			const WCHAR *value = option[1] == L':' ? option + 2 : option + 1;
			switch (letter) {
			case L'B': at_start = TRUE; continue;
			case L'E': at_end = TRUE; continue;
			case L'L': literal = TRUE; continue;
			case L'R': regular = TRUE; continue;
			case L'S': subfolders = TRUE; continue;
			case L'I': fold = TRUE; continue;
			case L'X': whole = TRUE; continue;
			case L'V': invert = TRUE; continue;
			case L'N': numbered = TRUE; continue;
			case L'M': names_only = TRUE; continue;
			case L'O':
				if (starts(option, L"OFF"))
					option += length(option) - 1;
				else
					offsets = TRUE;
				continue;
			case L'P': printable_only = TRUE; continue;
			case L'?':
				say(L"Searches for strings in files.\r\n\r\n"
					L"FINDSTR [/B] [/E] [/L] [/R] [/S] [/I] [/X] [/V] [/N] [/M] [/O] [/P] [/F:file]\r\n"
					L"        [/C:string] [/G:file] [/D:dir list] [/A:color attributes] [/OFF[LINE]]\r\n"
					L"        strings [[drive:][path]filename[ ...]]\r\n");
				ExitProcess(0);
				break;
			case L'C':
				if (have_literal)
					text_char(&literal_strings, L'\n');
				text_put(&literal_strings, value);
				have_literal = TRUE;
				break;
			case L'G': strings_file = copy_of(value); break;
			case L'F': list_file = copy_of(value); break;
			case L'A':
			case L'D': break;
			default:
				complain(L"FINDSTR: /");
				write_to(error_output, option, 1);
				complain(L" ignored\r\n");
				continue;
			}
			break;
		}
	}
	if ((have_literal || strings_file) && have_strings) {
		/* With /C: or /G:, the first word is a file name too. */
		memmove(files + 1, files, file_count * sizeof(WCHAR *));
		files[0] = strings.data;
		++file_count;
		have_strings = FALSE;
	}
	literal_default = have_literal;
	const BOOL as_regular = regular || (!literal && !literal_default);
	if (have_literal)
		for (WCHAR *at = literal_strings.data; at && *at;) {
			WCHAR *end = at;
			while (*end && *end != L'\n')
				++end;
			WCHAR *one = duplicate(at, (DWORD)(end - at));
			compile(one, regular);
			release(one);
			at = *end ? end + 1 : end;
		}
	if (have_strings)
		for (WCHAR *at = strings.data; *at;) {
			while (*at == L' ')
				++at;
			if (!*at)
				break;
			WCHAR *end = at;
			while (*end && *end != L' ')
				++end;
			WCHAR *one = duplicate(at, (DWORD)(end - at));
			compile(one, as_regular);
			release(one);
			at = end;
		}
	if (strings_file) {
		DWORD size, at = 0, start, line_size;
		WCHAR *text = list_text(strings_file, &size);
		if (!text) {
			complain(L"FINDSTR: Cannot open ");
			complain(strings_file);
			complain(L"\r\n");
			ExitProcess(2);
		}
		while (next_line(text, size, &at, &start, &line_size))
			if (line_size) {
				WCHAR *one = duplicate(text + start, line_size);
				compile(one, as_regular);
				release(one);
			}
		release(text);
	}
	if (!pattern_count) {
		complain(L"FINDSTR: Bad command line\r\n");
		ExitProcess(2);
	}
	if (list_file) {
		DWORD size, at = 0, start, line_size;
		WCHAR *text = list_text(list_file, &size);
		if (text) {
			WCHAR **grown = allocate((file_count + size + 1) * sizeof(WCHAR *));
			memcpy(grown, files, file_count * sizeof(WCHAR *));
			release(files);
			files = grown;
			while (next_line(text, size, &at, &start, &line_size))
				if (line_size)
					files[file_count++] = duplicate(text + start, line_size);
		}
	}
	DWORD found = 0;
	BOOL error = FALSE;
	if (!file_count) {
		DWORD size;
		WCHAR *text = read_text(input, &size);
		found = search(text, size, L"", FALSE);
		release(text);
	} else {
		const BOOL show_name = file_count > 1 || subfolders || wild(files[0]);
		for (DWORD index = 0; index < file_count; ++index) {
			WCHAR *full = full_path(files[index]), *folder, *pattern;
			split_path(full, &folder, &pattern);
			/* The names shown keep the folder as it was given. */
			const WCHAR *given = files[index];
			DWORD prefix = length(given);
			while (prefix && given[prefix - 1] != L'\\' && given[prefix - 1] != L'/' && given[prefix - 1] != L':')
				--prefix;
			WCHAR *shown_folder = duplicate(given, prefix);
			BOOL any = FALSE;
			if (!wild(pattern) && !subfolders) {
				if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
					complain(L"FINDSTR: Cannot open ");
					complain(given);
					complain(L"\r\n");
					error = TRUE;
				} else
					found += search_file(full, given, show_name);
			} else {
				found += search_pattern(folder, shown_folder, pattern, show_name, &any);
				if (!any && !subfolders) {
					complain(L"FINDSTR: Cannot open ");
					complain(given);
					complain(L"\r\n");
					error = TRUE;
				}
			}
			release(shown_folder);
			release(full);
			release(folder);
			release(pattern);
		}
	}
	ExitProcess(found ? 0 : error ? 2 : 1);
	return 0;
}
