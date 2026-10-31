/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "setup.h"
#include <string.h>

/* An open INF is a list of parsed files; a line is an array of fields, with the key apart. Text
 * goes through two stages: the lexer gives raw fields and the [Strings] substitution runs once the
 * whole file has been read, since that section usually comes last. */

#define MAX_FIELD 4096
#define MAX_SECTION_NAME 255
#define MAX_STRING_VALUE 4096

typedef struct inf_line {
	WCHAR *key;
	WCHAR **fields;
	DWORD field_count;
	BOOL bare;
} inf_line;

typedef struct inf_section {
	WCHAR *name;
	inf_line *lines;
	DWORD count;
	DWORD capacity;
} inf_section;

typedef struct inf_file {
	struct inf_file *next;
	WCHAR *path;
	WCHAR *directory;
	inf_section *sections;
	DWORD section_count;
	DWORD section_capacity;
	struct dir_id *dir_ids;
	DWORD style;
} inf_file;

struct dir_id {
	DWORD id;
	WCHAR *path;
	struct dir_id *next;
};

static WCHAR *copy_range(const WCHAR *text, SIZE_T length)
{
	WCHAR *copy = setup_alloc((length + 1) * sizeof(WCHAR));

	if (copy)
		memcpy(copy, text, length * sizeof(WCHAR));
	return copy;
}

WCHAR *setup_copy_wide(const WCHAR *text)
{
	return text ? copy_range(text, lstrlenW(text)) : NULL;
}

static inf_section *find_section(const inf_file *file, const WCHAR *name)
{
	DWORD at;

	for (at = 0; at < file->section_count; at++) {
		if (!lstrcmpiW(file->sections[at].name, name))
			return &file->sections[at];
	}
	return NULL;
}

static inf_section *section_for_name(inf_file *file, const WCHAR *name, SIZE_T length)
{
	inf_section *section;
	WCHAR *copy = copy_range(name, length);

	if (!copy)
		return NULL;
	section = find_section(file, copy);
	if (section) {
		setup_free(copy);
		return section;
	}
	if (file->section_count == file->section_capacity) {
		DWORD capacity = file->section_capacity ? file->section_capacity * 2 : 16;
		inf_section *grown = setup_realloc(file->sections, capacity * sizeof(*grown));
		if (!grown) {
			setup_free(copy);
			return NULL;
		}
		file->sections = grown;
		file->section_capacity = capacity;
	}
	section = &file->sections[file->section_count++];
	memset(section, 0, sizeof(*section));
	section->name = copy;
	return section;
}

/* The lexer works on one logical line at a time. */

struct field_builder {
	WCHAR *text;
	SIZE_T length;
	SIZE_T capacity;
	/* The end of what must survive trimming: everything up to and including quoted text. */
	SIZE_T keep;
	BOOL any;
};

static BOOL build_put(struct field_builder *field, WCHAR unit)
{
	if (field->length + 1 >= field->capacity) {
		SIZE_T capacity = field->capacity ? field->capacity * 2 : 64;
		WCHAR *grown = setup_realloc(field->text, capacity * sizeof(WCHAR));
		if (!grown)
			return FALSE;
		field->text = grown;
		field->capacity = capacity;
	}
	field->text[field->length++] = unit;
	return TRUE;
}

static BOOL is_blank(WCHAR unit)
{
	return unit == ' ' || unit == '\t' || unit == '\r' || unit == '\f' || unit == '\v';
}

static void build_reset(struct field_builder *field)
{
	field->length = 0;
	field->keep = 0;
	field->any = FALSE;
}

static WCHAR *build_finish(struct field_builder *field)
{
	SIZE_T length = field->length;

	while (length > field->keep && is_blank(field->text[length - 1]))
		length--;
	return copy_range(field->text ? field->text : L"", length);
}

static BOOL add_field(inf_line *line, WCHAR *text)
{
	WCHAR **grown = setup_realloc(line->fields, (line->field_count + 1) * sizeof(WCHAR *));

	if (!grown)
		return FALSE;
	line->fields = grown;
	line->fields[line->field_count++] = text;
	return TRUE;
}

/* From a backslash: only blanks, more backslashes and a comment may stand before the line end for it
 * to continue the line. Returns where the next line starts, or NULL when it is an ordinary unit. */
static const WCHAR *continuation_after(const WCHAR *at, const WCHAR *end)
{
	const WCHAR *scan = at;

	while (scan < end && (is_blank(*scan) || *scan == '\\'))
		scan++;
	if (scan < end && *scan == ';') {
		while (scan < end && *scan != '\n')
			scan++;
	}
	if (scan == end)
		return end;
	return *scan == '\n' ? scan + 1 : NULL;
}

static BOOL section_add_line(inf_section *section, inf_line *line)
{
	if (section->count == section->capacity) {
		DWORD capacity = section->capacity ? section->capacity * 2 : 16;
		inf_line *grown = setup_realloc(section->lines, capacity * sizeof(*grown));
		if (!grown)
			return FALSE;
		section->lines = grown;
		section->capacity = capacity;
	}
	section->lines[section->count++] = *line;
	return TRUE;
}

/* Parses one logical line starting at text. Sets *line_number to count the physical lines it took. */
static const WCHAR *lex_line(const WCHAR *text, const WCHAR *end, inf_line *line, struct field_builder *field, DWORD *line_number, BOOL *any, BOOL *failed)
{
	BOOL in_quotes = FALSE, have_key = FALSE, after_comma = FALSE;

	*any = FALSE;
	build_reset(field);
	while (text < end) {
		WCHAR unit = *text;

		if (in_quotes) {
			if (unit == '\n' ) {
				in_quotes = FALSE;
				continue;
			}
			if (unit == '"') {
				if (text + 1 < end && text[1] == '"') {
					build_put(field, '"');
					field->keep = field->length;
					text += 2;
					continue;
				}
				in_quotes = FALSE;
				text++;
				continue;
			}
			build_put(field, unit);
			field->keep = field->length;
			text++;
			continue;
		}
		if (unit == '\n')
			break;
		if (unit == ';') {
			while (text < end && *text != '\n')
				text++;
			continue;
		}
		if (unit == '\\') {
			const WCHAR *next = continuation_after(text + 1, end);
			if (next) {
				const WCHAR *scan;
				for (scan = text; scan < next; scan++) {
					if (*scan == '\n')
						(*line_number)++;
				}
				text = next;
				while (text < end && is_blank(*text))
					text++;
				/* Blanks standing before the backslash go with it. */
				while (field->length > field->keep && is_blank(field->text[field->length - 1]))
					field->length--;
				continue;
			}
		}
		if (unit == '"') {
			in_quotes = TRUE;
			field->any = TRUE;
			field->keep = field->length;
			*any = TRUE;
			text++;
			continue;
		}
		if (unit == ',') {
			WCHAR *value = build_finish(field);
			if (!value || !add_field(line, value))
				*failed = TRUE;
			build_reset(field);
			*any = TRUE;
			after_comma = TRUE;
			text++;
			while (text < end && is_blank(*text))
				text++;
			continue;
		}
		if (unit == '=' && !have_key && !line->field_count) {
			line->key = build_finish(field);
			if (!line->key)
				*failed = TRUE;
			have_key = TRUE;
			build_reset(field);
			*any = TRUE;
			after_comma = TRUE;
			text++;
			while (text < end && is_blank(*text))
				text++;
			continue;
		}
		if (is_blank(unit) && !field->length && !field->any) {
			text++;
			continue;
		}
		build_put(field, unit);
		field->any = TRUE;
		*any = TRUE;
		text++;
	}
	if (*any) {
		if (field->length || field->any || after_comma) {
			WCHAR *value = build_finish(field);
			if (!value || !add_field(line, value))
				*failed = TRUE;
		}
		/* A lone unquoted word or quoted text is its own key, but the comma forms have none. */
		if (!have_key && line->field_count == 1)
			line->bare = TRUE;
	}
	return text;
}

static BOOL is_valid_signature(const WCHAR *signature)
{
	return !lstrcmpiW(signature, L"$Chicago$") || !lstrcmpiW(signature, L"$Windows NT$") || !lstrcmpiW(signature, L"$Windows 95$");
}

static DWORD parse_text(inf_file *file, const WCHAR *text, SIZE_T length, UINT *error_line, DWORD style)
{
	const WCHAR *at = text, *end = text + length;
	struct field_builder field = { 0 };
	inf_section *current = NULL;
	DWORD line_number = 1, error = 0, first_garbage = 0;

	while (at < end) {
		const WCHAR *scan = at;

		while (scan < end && is_blank(*scan))
			scan++;
		if (scan < end && *scan == '[') {
			const WCHAR *name = ++scan;
			while (scan < end && *scan != ']' && *scan != '\n')
				scan++;
			if (scan >= end || *scan != ']') {
				error = ERROR_BAD_SECTION_NAME_LINE;
				goto failed;
			}
			if (scan - name > MAX_SECTION_NAME) {
				error = ERROR_SECTION_NAME_TOO_LONG;
				goto failed;
			}
			current = section_for_name(file, name, scan - name);
			if (!current) {
				error = ERROR_NOT_ENOUGH_MEMORY;
				goto failed;
			}
			while (scan < end && *scan != '\n')
				scan++;
			at = scan;
		} else {
			inf_line line = { 0 };
			BOOL any = FALSE, oom = FALSE;
			at = lex_line(at, end, &line, &field, &line_number, &any, &oom);
			if (oom) {
				error = ERROR_NOT_ENOUGH_MEMORY;
				goto failed;
			}
			if (any) {
				if (current) {
					if (!section_add_line(current, &line)) {
						error = ERROR_NOT_ENOUGH_MEMORY;
						goto failed;
					}
				} else {
					DWORD at_field;
					if (!first_garbage)
						first_garbage = line_number;
					setup_free(line.key);
					for (at_field = 0; at_field < line.field_count; at_field++)
						setup_free(line.fields[at_field]);
					setup_free(line.fields);
				}
			}
		}
		if (at < end && *at == '\n') {
			at++;
			line_number++;
		}
	}
	/* Text before the first section only passes in a file that has a [Strings] section. */
	if (first_garbage && !find_section(file, L"Strings")) {
		if (find_section(file, L"Version")) {
			line_number = first_garbage;
			error = ERROR_EXPECTED_SECTION_NAME;
			goto failed;
		}
	}
	setup_free(field.text);
	return 0;
failed:
	if (error_line)
		*error_line = line_number;
	setup_free(field.text);
	return error;
}

/* Substitution. */

static const WCHAR *find_string(const inf_file *file, const WCHAR *name)
{
	const inf_section *strings = find_section(file, L"Strings");
	DWORD at;

	if (!strings)
		return NULL;
	for (at = 0; at < strings->count; at++) {
		const inf_line *line = &strings->lines[at];
		if (line->key && !lstrcmpiW(line->key, name))
			return line->field_count ? line->fields[0] : L"";
	}
	return NULL;
}

static BOOL all_digits(const WCHAR *text, SIZE_T length)
{
	SIZE_T at;

	if (!length)
		return FALSE;
	for (at = 0; at < length; at++) {
		if (text[at] < '0' || text[at] > '9')
			return FALSE;
	}
	return TRUE;
}

static WCHAR *substitute(inf_file *file, const WCHAR *text)
{
	struct field_builder out = { 0 };
	const WCHAR *at = text;

	while (*at) {
		const WCHAR *close;
		if (*at != '%') {
			build_put(&out, *at++);
			continue;
		}
		if (at[1] == '%') {
			build_put(&out, '%');
			at += 2;
			continue;
		}
		close = at + 1;
		while (*close && *close != '%')
			close++;
		if (!*close) {
			while (*at)
				build_put(&out, *at++);
			break;
		}
		{
			WCHAR name[256];
			SIZE_T length = close - at - 1;
			const WCHAR *value = NULL;
			BOOL is_directory = FALSE;
			WCHAR *directory = NULL;

			if (length < ARRAYSIZE(name)) {
				memcpy(name, at + 1, length * sizeof(WCHAR));
				name[length] = 0;
				value = find_string(file, name);
				if (!value && all_digits(name, length)) {
					directory = setup_dir_id_path((HINF)file, (int)wcstol(name, NULL, 10));
					value = directory;
					is_directory = value != NULL;
				}
			}
			if (value) {
				SIZE_T part = lstrlenW(value), put;
				if (part > MAX_STRING_VALUE)
					part = MAX_STRING_VALUE;
				for (put = 0; put < part; put++)
					build_put(&out, value[put]);
				at = close + 1;
				/* A system directory swallows the separator following it when it ends in one. */
				if (is_directory && part && value[part - 1] == '\\' && *at == '\\')
					at++;
				setup_free(directory);
			} else {
				SIZE_T put;
				for (put = 0; put < length + 2; put++)
					build_put(&out, at[put]);
				at = close + 1;
			}
		}
	}
	if (out.length > MAX_FIELD)
		out.length = MAX_FIELD;
	{
		WCHAR *result = copy_range(out.text ? out.text : L"", out.length);
		setup_free(out.text);
		return result;
	}
}

static void substitute_all(inf_file *file)
{
	DWORD at_section, at_line, at_field;

	for (at_section = 0; at_section < file->section_count; at_section++) {
		inf_section *section = &file->sections[at_section];
		BOOL strings = !lstrcmpiW(section->name, L"Strings");
		for (at_line = 0; at_line < section->count; at_line++) {
			inf_line *line = &section->lines[at_line];
			if (strings)
				continue;
			if (line->key) {
				WCHAR *value = substitute(file, line->key);
				if (value) {
					setup_free(line->key);
					line->key = value;
				}
			}
			for (at_field = 0; at_field < line->field_count; at_field++) {
				WCHAR *value = substitute(file, line->fields[at_field]);
				if (value) {
					setup_free(line->fields[at_field]);
					line->fields[at_field] = value;
				}
			}
		}
	}
}

/* Files. */

static void free_file(inf_file *file)
{
	DWORD at_section, at_line, at_field;

	for (at_section = 0; at_section < file->section_count; at_section++) {
		inf_section *section = &file->sections[at_section];
		for (at_line = 0; at_line < section->count; at_line++) {
			inf_line *line = &section->lines[at_line];
			setup_free(line->key);
			for (at_field = 0; at_field < line->field_count; at_field++)
				setup_free(line->fields[at_field]);
			setup_free(line->fields);
		}
		setup_free(section->lines);
		setup_free(section->name);
	}
	setup_free(file->sections);
	while (file->dir_ids) {
		struct dir_id *gone = file->dir_ids;
		file->dir_ids = gone->next;
		setup_free(gone->path);
		setup_free(gone);
	}
	setup_free(file->path);
	setup_free(file->directory);
	setup_free(file);
}

static WCHAR *decode_file(const BYTE *data, DWORD size, DWORD *length)
{
	WCHAR *text;
	DWORD at;
	int count;

	if (size >= 2 && data[0] == 0xff && data[1] == 0xfe) {
		count = (size - 2) / 2;
		text = setup_alloc(((SIZE_T)count + 1) * sizeof(WCHAR));
		if (text)
			memcpy(text, data + 2, (SIZE_T)count * sizeof(WCHAR));
	} else {
		UINT code_page = CP_ACP;
		if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
			code_page = CP_UTF8;
			data += 3;
			size -= 3;
		}
		count = size ? MultiByteToWideChar(code_page, 0, (const char *)data, size, NULL, 0) : 0;
		text = setup_alloc(((SIZE_T)count + 1) * sizeof(WCHAR));
		if (text && count)
			MultiByteToWideChar(code_page, 0, (const char *)data, size, text, count);
	}
	if (!text)
		return NULL;
	/* Nulls read as blanks, and the DOS end of file mark ends the text. */
	for (at = 0; at < (DWORD)count; at++) {
		if (text[at] == 0x1a) {
			count = at;
			break;
		}
		if (!text[at])
			text[at] = ' ';
	}
	*length = count;
	return text;
}

static const WCHAR *version_signature(const inf_file *file)
{
	const inf_section *version = find_section(file, L"Version");
	DWORD at;

	if (!version)
		return NULL;
	for (at = 0; at < version->count; at++) {
		const inf_line *line = &version->lines[at];
		if (line->key && !lstrcmpiW(line->key, L"Signature") && line->field_count)
			return line->fields[0];
	}
	return NULL;
}

static inf_file *load_file(const WCHAR *path, DWORD style, UINT *error_line, DWORD *error)
{
	HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	DWORD size, got = 0, length = 0;
	WCHAR *text;
	BYTE *data;
	inf_file *file;
	const WCHAR *slash;

	*error = 0;
	if (handle == INVALID_HANDLE_VALUE) {
		*error = GetLastError();
		return NULL;
	}
	size = GetFileSize(handle, NULL);
	data = setup_alloc(size ? size : 1);
	if (!data || !ReadFile(handle, data, size, &got, NULL)) {
		*error = data ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
		setup_free(data);
		CloseHandle(handle);
		return NULL;
	}
	CloseHandle(handle);
	text = decode_file(data, got, &length);
	setup_free(data);
	file = text ? setup_alloc(sizeof(*file)) : NULL;
	if (!file) {
		setup_free(text);
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return NULL;
	}
	file->style = style;
	file->path = setup_copy_wide(path);
	slash = path + lstrlenW(path);
	while (slash > path && slash[-1] != '\\' && slash[-1] != '/')
		slash--;
	file->directory = copy_range(path, slash > path ? slash - path - 1 : 0);
	*error = parse_text(file, text, length, error_line, style);
	setup_free(text);
	if (*error) {
		free_file(file);
		return NULL;
	}
	if ((style & (INF_STYLE_WIN4 | INF_STYLE_OLDNT)) == INF_STYLE_WIN4 || !(style & INF_STYLE_OLDNT)) {
		const WCHAR *signature = version_signature(file);
		if (!signature || !is_valid_signature(signature)) {
			free_file(file);
			*error = ERROR_WRONG_INF_STYLE;
			if (error_line)
				*error_line = 0;
			return NULL;
		}
	}
	substitute_all(file);
	return file;
}

static BOOL resolve_path(const WCHAR *name, WCHAR *full, DWORD size)
{
	WCHAR directory[MAX_PATH];

	if (wcschr(name, '\\') || wcschr(name, '/') || wcschr(name, ':'))
		return GetFullPathNameW(name, size, full, NULL) != 0;
	if (GetFileAttributesW(name) != INVALID_FILE_ATTRIBUTES)
		return GetFullPathNameW(name, size, full, NULL) != 0;
	if (!GetWindowsDirectoryW(directory, ARRAYSIZE(directory)))
		return FALSE;
	if ((DWORD)lstrlenW(directory) + lstrlenW(name) + 6 > size)
		return FALSE;
	lstrcpyW(full, directory);
	lstrcatW(full, L"\\inf\\");
	lstrcatW(full, name);
	return TRUE;
}

static BOOL class_matches(const inf_file *file, const WCHAR *class_name)
{
	const inf_section *version = find_section(file, L"Version");
	DWORD at;

	if (!class_name)
		return TRUE;
	if (!version)
		return FALSE;
	for (at = 0; at < version->count; at++) {
		const inf_line *line = &version->lines[at];
		if (line->key && !lstrcmpiW(line->key, L"Class") && line->field_count)
			return !lstrcmpiW(line->fields[0], class_name);
	}
	return FALSE;
}

HINF WINAPI SetupOpenInfFileW(PCWSTR name, PCWSTR class_name, DWORD style, UINT *error_line)
{
	WCHAR full[MAX_PATH * 2];
	inf_file *file;
	DWORD error;

	if (error_line)
		*error_line = 0;
	if (!name) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if (style & ~(INF_STYLE_OLDNT | INF_STYLE_WIN4 | INF_STYLE_CACHE_ENABLE | INF_STYLE_CACHE_DISABLE | INF_STYLE_CACHE_IGNORE)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if (!resolve_path(name, full, ARRAYSIZE(full))) {
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	file = load_file(full, style, error_line, &error);
	if (!file) {
		/* A bare name searched in a missing inf directory is still just a missing file. */
		if (error == ERROR_PATH_NOT_FOUND && !wcschr(name, '\\') && !wcschr(name, '/'))
			error = ERROR_FILE_NOT_FOUND;
		SetLastError(error);
		return INVALID_HANDLE_VALUE;
	}
	if (!class_matches(file, class_name)) {
		free_file(file);
		SetLastError(ERROR_CLASS_MISMATCH);
		return INVALID_HANDLE_VALUE;
	}
	SetLastError(0);
	return (HINF)file;
}

HINF WINAPI SetupOpenInfFileA(PCSTR name, PCSTR class_name, DWORD style, UINT *error_line)
{
	WCHAR *wide_name = setup_to_wide(name), *wide_class = setup_to_wide(class_name);
	HINF result = SetupOpenInfFileW(wide_name, wide_class, style, error_line);

	setup_free(wide_name);
	setup_free(wide_class);
	return result;
}

HINF WINAPI SetupOpenMasterInf(void)
{
	WCHAR path[MAX_PATH];

	GetWindowsDirectoryW(path, MAX_PATH);
	lstrcatW(path, L"\\Inf\\Layout.inf");
	return SetupOpenInfFileW(path, NULL, INF_STYLE_WIN4, NULL);
}

BOOL WINAPI SetupOpenAppendInfFileW(PCWSTR name, HINF parent, UINT *error_line)
{
	inf_file *root = parent, *added, *tail;
	WCHAR full[MAX_PATH * 2];
	DWORD error;

	if (error_line)
		*error_line = 0;
	if (!parent || parent == INVALID_HANDLE_VALUE) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!name) {
		/* Every file named by the LayoutFile line of [Version]. */
		INFCONTEXT context;
		DWORD at, count;
		if (!SetupFindFirstLineW(parent, L"Version", L"LayoutFile", &context)) {
			SetLastError(ERROR_INVALID_DATA);
			return FALSE;
		}
		count = SetupGetFieldCount(&context);
		for (at = 1; at <= count; at++) {
			WCHAR layout[MAX_PATH];
			if (SetupGetStringFieldW(&context, at, layout, ARRAYSIZE(layout), NULL))
				SetupOpenAppendInfFileW(layout, parent, NULL);
		}
		return TRUE;
	}
	if (!resolve_path(name, full, ARRAYSIZE(full))) {
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	added = load_file(full, INF_STYLE_WIN4, error_line, &error);
	if (!added) {
		SetLastError(error);
		return FALSE;
	}
	for (tail = root; tail->next; tail = tail->next)
		;
	tail->next = added;
	SetLastError(0);
	return TRUE;
}

BOOL WINAPI SetupOpenAppendInfFileA(PCSTR name, HINF parent, UINT *error_line)
{
	WCHAR *wide = setup_to_wide(name);
	BOOL result = SetupOpenAppendInfFileW(wide, parent, error_line);

	setup_free(wide);
	return result;
}

void WINAPI SetupCloseInfFile(HINF handle)
{
	inf_file *file = handle;

	if (!handle || handle == INVALID_HANDLE_VALUE)
		return;
	while (file) {
		inf_file *next = file->next;
		free_file(file);
		file = next;
	}
}

/* Directory ids of an open file. */

/* Every occurrence of one %id% token in a value, as a new string; NULL when there is none. */
static WCHAR *replace_dir_id(const WCHAR *text, const WCHAR *token, const WCHAR *path)
{
	SIZE_T token_length = lstrlenW(token), path_length = lstrlenW(path);
	struct field_builder out = {0};
	const WCHAR *at = text;
	BOOL found = FALSE;
	WCHAR *result;

	while (*at) {
		if (*at == '%' && !wcsncmp(at, token, token_length)) {
			SIZE_T put;
			for (put = 0; put < path_length; put++)
				build_put(&out, path[put]);
			at += token_length;
			if (path_length && path[path_length - 1] == '\\' && *at == '\\')
				at++;
			found = TRUE;
			continue;
		}
		build_put(&out, *at++);
	}
	result = found ? copy_range(out.text ? out.text : L"", out.length) : NULL;
	setup_free(out.text);
	return result;
}

/* A file is substituted when read, so ids set afterwards fill the tokens left in its lines. */
static void fill_dir_id(inf_file *file, DWORD id, const WCHAR *path)
{
	DWORD at_section, at_line, at_field;
	WCHAR token[16], digits[12], *end = digits + ARRAYSIZE(digits) - 1;
	DWORD left = id;

	*end = 0;
	do {
		*--end = (WCHAR)('0' + left % 10);
		left /= 10;
	} while (left);
	lstrcpyW(token, L"%");
	lstrcatW(token, end);
	lstrcatW(token, L"%");
	for (at_section = 0; at_section < file->section_count; at_section++) {
		inf_section *section = &file->sections[at_section];
		for (at_line = 0; at_line < section->count; at_line++) {
			inf_line *line = &section->lines[at_line];
			for (at_field = 0; at_field < line->field_count; at_field++) {
				WCHAR *value = replace_dir_id(line->fields[at_field], token, path);
				if (value) {
					setup_free(line->fields[at_field]);
					line->fields[at_field] = value;
				}
			}
		}
	}
}

BOOL setup_set_dir_id(HINF handle, DWORD id, const WCHAR *path)
{
	inf_file *file = handle;
	struct dir_id *entry;

	if (path)
		fill_dir_id(file, id, path);
	for (entry = file->dir_ids; entry; entry = entry->next) {
		if (entry->id == id) {
			setup_free(entry->path);
			entry->path = setup_copy_wide(path);
			return TRUE;
		}
	}
	entry = setup_alloc(sizeof(*entry));
	if (!entry)
		return FALSE;
	entry->id = id;
	entry->path = setup_copy_wide(path);
	entry->next = file->dir_ids;
	file->dir_ids = entry;
	return TRUE;
}

const WCHAR *setup_user_dir_id(HINF handle, DWORD id)
{
	inf_file *file = handle;
	struct dir_id *entry;

	for (entry = file->dir_ids; entry; entry = entry->next) {
		if (entry->id == id)
			return entry->path;
	}
	return NULL;
}

const WCHAR *setup_inf_directory(HINF handle)
{
	return ((inf_file *)handle)->directory;
}

BOOL setup_inf_is_win4(HINF handle)
{
	const WCHAR *signature = version_signature(handle);

	return signature && is_valid_signature(signature);
}

const WCHAR *setup_inf_path(HINF handle)
{
	return ((inf_file *)handle)->path;
}

/* Lines and fields. A context names the open INF, the file inside it, the section and the line. */

static inf_file *context_file(INFCONTEXT *context)
{
	return context->CurrentInf;
}

static inf_line *context_line(INFCONTEXT *context)
{
	inf_file *file = context_file(context);
	inf_section *section;

	if (!file || context->Section >= file->section_count)
		return NULL;
	section = &file->sections[context->Section];
	return context->Line < section->count ? &section->lines[context->Line] : NULL;
}

BOOL WINAPI SetupFindFirstLineW(HINF handle, PCWSTR section_name, PCWSTR key, INFCONTEXT *context)
{
	inf_file *file;

	if (!handle || handle == INVALID_HANDLE_VALUE || !section_name) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	for (file = handle; file; file = file->next) {
		inf_section *section = find_section(file, section_name);
		DWORD at;
		if (!section)
			continue;
		for (at = 0; at < section->count; at++) {
			if (!key || (section->lines[at].key && !lstrcmpiW(section->lines[at].key, key)) ||
					(section->lines[at].bare && !lstrcmpiW(section->lines[at].fields[0], key))) {
				context->Inf = handle;
				context->CurrentInf = file;
				context->Section = (UINT)(section - file->sections);
				context->Line = at;
				SetLastError(0);
				return TRUE;
			}
		}
	}
	SetLastError(ERROR_LINE_NOT_FOUND);
	return FALSE;
}

BOOL WINAPI SetupFindFirstLineA(HINF handle, PCSTR section_name, PCSTR key, INFCONTEXT *context)
{
	WCHAR *wide_section = setup_to_wide(section_name), *wide_key = setup_to_wide(key);
	BOOL result = SetupFindFirstLineW(handle, wide_section, wide_key, context);

	setup_free(wide_section);
	setup_free(wide_key);
	return result;
}

/* Finds the line at or after a position, going on in the next file for the same section name. */
static BOOL next_match(INFCONTEXT *from, DWORD line, const WCHAR *key, INFCONTEXT *to)
{
	inf_file *file = from->CurrentInf;
	const WCHAR *name;

	if (!file || from->Section >= file->section_count) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	name = file->sections[from->Section].name;
	for (; file; file = file->next, line = 0) {
		inf_section *section = find_section(file, name);
		if (!section)
			continue;
		for (; line < section->count; line++) {
			const inf_line *item = &section->lines[line];
			if (!key || (item->key && !lstrcmpiW(item->key, key)) || (item->bare && !lstrcmpiW(item->fields[0], key))) {
				to->Inf = from->Inf;
				to->CurrentInf = file;
				to->Section = (UINT)(section - file->sections);
				to->Line = line;
				SetLastError(0);
				return TRUE;
			}
		}
	}
	SetLastError(ERROR_LINE_NOT_FOUND);
	return FALSE;
}

BOOL WINAPI SetupFindNextLine(INFCONTEXT *from, INFCONTEXT *to)
{
	return next_match(from, from->Line + 1, NULL, to);
}

BOOL WINAPI SetupFindNextMatchLineW(INFCONTEXT *from, PCWSTR key, INFCONTEXT *to)
{
	return next_match(from, from->Line + 1, key, to);
}

BOOL WINAPI SetupFindNextMatchLineA(INFCONTEXT *from, PCSTR key, INFCONTEXT *to)
{
	WCHAR *wide = setup_to_wide(key);
	BOOL result = SetupFindNextMatchLineW(from, wide, to);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupGetLineByIndexW(HINF handle, PCWSTR section_name, DWORD index, INFCONTEXT *context)
{
	inf_file *file;

	if (!handle || handle == INVALID_HANDLE_VALUE || !section_name) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	for (file = handle; file; file = file->next) {
		inf_section *section = find_section(file, section_name);
		if (!section)
			continue;
		if (index < section->count) {
			context->Inf = handle;
			context->CurrentInf = file;
			context->Section = (UINT)(section - file->sections);
			context->Line = index;
			SetLastError(0);
			return TRUE;
		}
		index -= section->count;
	}
	SetLastError(ERROR_LINE_NOT_FOUND);
	return FALSE;
}

BOOL WINAPI SetupGetLineByIndexA(HINF handle, PCSTR section_name, DWORD index, INFCONTEXT *context)
{
	WCHAR *wide = setup_to_wide(section_name);
	BOOL result = SetupGetLineByIndexW(handle, wide, index, context);

	setup_free(wide);
	return result;
}

LONG WINAPI SetupGetLineCountW(HINF handle, PCWSTR section_name)
{
	inf_file *file;
	LONG total = -1;

	if (!handle || handle == INVALID_HANDLE_VALUE || !section_name) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return -1;
	}
	for (file = handle; file; file = file->next) {
		const inf_section *section = find_section(file, section_name);
		if (section)
			total = (total < 0 ? 0 : total) + (LONG)section->count;
	}
	SetLastError(total < 0 ? ERROR_SECTION_NOT_FOUND : 0);
	return total;
}

LONG WINAPI SetupGetLineCountA(HINF handle, PCSTR section_name)
{
	WCHAR *wide = setup_to_wide(section_name);
	LONG result = SetupGetLineCountW(handle, wide);

	setup_free(wide);
	return result;
}

DWORD WINAPI SetupGetFieldCount(INFCONTEXT *context)
{
	const inf_line *line = context_line(context);

	return line ? line->field_count : 0;
}

static const WCHAR *line_field(const inf_line *line, DWORD index)
{
	if (!index) {
		if (line->key)
			return line->key;
		return line->bare ? line->fields[0] : NULL;
	}
	return index <= line->field_count ? line->fields[index - 1] : NULL;
}

PCWSTR WINAPI pSetupGetField(INFCONTEXT *context, DWORD index)
{
	const inf_line *line = context_line(context);
	const WCHAR *field = line ? line_field(line, index) : NULL;

	SetLastError(field ? 0 : ERROR_INVALID_PARAMETER);
	return field;
}

static BOOL give_wide(const WCHAR *text, WCHAR *buffer, DWORD size, DWORD *required)
{
	DWORD length = lstrlenW(text) + 1;

	if (required)
		*required = length;
	if (buffer) {
		if (size < length) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		memcpy(buffer, text, length * sizeof(WCHAR));
	}
	SetLastError(0);
	return TRUE;
}

static BOOL give_ansi(const WCHAR *text, char *buffer, DWORD size, DWORD *required)
{
	DWORD length = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);

	if (required)
		*required = length;
	if (buffer) {
		if (size < length) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		WideCharToMultiByte(CP_ACP, 0, text, -1, buffer, size, NULL, NULL);
	}
	SetLastError(0);
	return TRUE;
}

BOOL WINAPI SetupGetStringFieldW(INFCONTEXT *context, DWORD index, PWSTR buffer, DWORD size, DWORD *required)
{
	const inf_line *line = context_line(context);
	const WCHAR *field = line ? line_field(line, index) : NULL;

	if (!field) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return give_wide(field, buffer, size, required);
}

BOOL WINAPI SetupGetStringFieldA(INFCONTEXT *context, DWORD index, PSTR buffer, DWORD size, DWORD *required)
{
	const inf_line *line = context_line(context);
	const WCHAR *field = line ? line_field(line, index) : NULL;

	if (!field) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return give_ansi(field, buffer, size, required);
}

BOOL WINAPI SetupGetIntField(INFCONTEXT *context, DWORD index, INT *value)
{
	const inf_line *line = context_line(context);
	const WCHAR *field = line ? line_field(line, index) : NULL;
	WCHAR *end;
	long parsed;

	if (!field) {
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (!*field) {
		*value = 0;
		SetLastError(0);
		return TRUE;
	}
	parsed = wcstol(field, &end, 0);
	if (end == field) {
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	/* The INF way to write hexadecimal: a trailing h. */
	if (*end == 'h' || *end == 'H')
		parsed = wcstol(field, &end, 16), end++;
	if (*end) {
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	*value = (INT)parsed;
	SetLastError(0);
	return TRUE;
}

BOOL WINAPI SetupGetBinaryField(INFCONTEXT *context, DWORD index, BYTE *buffer, DWORD size, DWORD *required)
{
	const inf_line *line = context_line(context);
	DWORD count, at;

	if (!line || !index || index > line->field_count + 1) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	count = line->field_count >= index ? line->field_count - index + 1 : 0;
	if (required)
		*required = count;
	if (!buffer) {
		SetLastError(0);
		return TRUE;
	}
	if (size < count) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	for (at = 0; at < count; at++) {
		const WCHAR *text = line->fields[index - 1 + at];
		WCHAR *end;
		long value = wcstol(text, &end, 16);
		if (end == text || *end || value < 0 || value > 255) {
			SetLastError(ERROR_INVALID_DATA);
			return FALSE;
		}
		buffer[at] = (BYTE)value;
	}
	SetLastError(0);
	return TRUE;
}

BOOL WINAPI SetupGetMultiSzFieldW(INFCONTEXT *context, DWORD index, PWSTR buffer, DWORD size, DWORD *required)
{
	const inf_line *line = context_line(context);
	DWORD at, total = 1;
	WCHAR *out = buffer;

	if (!line || !index || index > line->field_count + 1) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	for (at = index - 1; at < line->field_count; at++) {
		if (!line->fields[at][0])
			break;
		total += lstrlenW(line->fields[at]) + 1;
	}
	if (required)
		*required = total;
	if (!buffer) {
		SetLastError(0);
		return TRUE;
	}
	if (size < total) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	for (at = index - 1; at < line->field_count; at++) {
		DWORD length = lstrlenW(line->fields[at]) + 1;
		if (length == 1)
			break;
		memcpy(out, line->fields[at], length * sizeof(WCHAR));
		out += length;
	}
	*out = 0;
	SetLastError(0);
	return TRUE;
}

BOOL WINAPI SetupGetMultiSzFieldA(INFCONTEXT *context, DWORD index, PSTR buffer, DWORD size, DWORD *required)
{
	const inf_line *line = context_line(context);
	DWORD at, total = 1;
	char *out = buffer;

	if (!line || !index || index > line->field_count + 1) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	for (at = index - 1; at < line->field_count; at++) {
		if (!line->fields[at][0])
			break;
		total += WideCharToMultiByte(CP_ACP, 0, line->fields[at], -1, NULL, 0, NULL, NULL);
	}
	if (required)
		*required = total;
	if (!buffer) {
		SetLastError(0);
		return TRUE;
	}
	if (size < total) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	for (at = index - 1; at < line->field_count; at++) {
		if (!line->fields[at][0])
			break;
		out += WideCharToMultiByte(CP_ACP, 0, line->fields[at], -1, out, (int)(buffer + size - out), NULL, NULL);
	}
	*out = 0;
	SetLastError(0);
	return TRUE;
}

static BOOL locate_line(INFCONTEXT *context, HINF handle, const WCHAR *section, const WCHAR *key, INFCONTEXT *found)
{
	if (context) {
		*found = *context;
		return TRUE;
	}
	if (!handle || handle == INVALID_HANDLE_VALUE || !section || !key) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return SetupFindFirstLineW(handle, section, key, found);
}

static WCHAR *join_fields(const inf_line *line)
{
	DWORD at, total = 1;
	WCHAR *text, *out;

	for (at = 0; at < line->field_count; at++)
		total += lstrlenW(line->fields[at]) + 1;
	text = setup_alloc((SIZE_T)total * sizeof(WCHAR));
	if (!text)
		return NULL;
	out = text;
	for (at = 0; at < line->field_count; at++) {
		DWORD length = lstrlenW(line->fields[at]);
		if (at)
			*out++ = ',';
		memcpy(out, line->fields[at], length * sizeof(WCHAR));
		out += length;
	}
	*out = 0;
	return text;
}

BOOL WINAPI SetupGetLineTextW(INFCONTEXT *context, HINF handle, PCWSTR section, PCWSTR key, PWSTR buffer, DWORD size, DWORD *required)
{
	INFCONTEXT found;
	const inf_line *line;
	WCHAR *text;
	BOOL result;

	if (!locate_line(context, handle, section, key, &found))
		return FALSE;
	line = context_line(&found);
	if (!line) {
		SetLastError(ERROR_LINE_NOT_FOUND);
		return FALSE;
	}
	text = join_fields(line);
	if (!text) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	result = give_wide(text, buffer, size, required);
	setup_free(text);
	return result;
}

BOOL WINAPI SetupGetLineTextA(INFCONTEXT *context, HINF handle, PCSTR section, PCSTR key, PSTR buffer, DWORD size, DWORD *required)
{
	WCHAR *wide_section = setup_to_wide(section), *wide_key = setup_to_wide(key), *text;
	INFCONTEXT found;
	const inf_line *line;
	BOOL result = FALSE;

	if (locate_line(context, handle, wide_section, wide_key, &found)) {
		line = context_line(&found);
		if (!line) {
			SetLastError(ERROR_LINE_NOT_FOUND);
		} else {
			text = join_fields(line);
			if (text) {
				result = give_ansi(text, buffer, size, required);
				setup_free(text);
			}
		}
	}
	setup_free(wide_section);
	setup_free(wide_key);
	return result;
}

BOOL WINAPI SetupEnumInfSectionsW(HINF handle, UINT index, PWSTR buffer, UINT size, UINT *required)
{
	inf_file *file;
	DWORD length;

	for (file = handle; file; file = file->next) {
		if (index < file->section_count) {
			const WCHAR *name = file->sections[index].name;
			length = lstrlenW(name) + 1;
			if (required)
				*required = length;
			if (!buffer) {
				if (required && !size) {
					SetLastError(0);
					return TRUE;
				}
				SetLastError(ERROR_INVALID_USER_BUFFER);
				return FALSE;
			}
			if (size < length) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return FALSE;
			}
			memcpy(buffer, name, length * sizeof(WCHAR));
			SetLastError(0);
			return TRUE;
		}
		index -= file->section_count;
	}
	SetLastError(ERROR_NO_MORE_ITEMS);
	return FALSE;
}

BOOL WINAPI SetupEnumInfSectionsA(HINF handle, UINT index, PSTR buffer, UINT size, UINT *required)
{
	WCHAR name[MAX_SECTION_NAME + 1];
	UINT wide_size;
	DWORD length;

	if (!SetupEnumInfSectionsW(handle, index, name, ARRAYSIZE(name), &wide_size))
		return FALSE;
	length = WideCharToMultiByte(CP_ACP, 0, name, -1, NULL, 0, NULL, NULL);
	if (required)
		*required = length;
	if (!buffer) {
		if (required && !size) {
			SetLastError(0);
			return TRUE;
		}
		SetLastError(ERROR_INVALID_USER_BUFFER);
		return FALSE;
	}
	if (size < length) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	WideCharToMultiByte(CP_ACP, 0, name, -1, buffer, size, NULL, NULL);
	SetLastError(0);
	return TRUE;
}

BOOL WINAPI SetupGetInfSections(HINF handle, PWSTR buffer, DWORD size, DWORD *required)
{
	inf_file *file;
	DWORD total = 1, at;
	WCHAR *out = buffer;

	for (file = handle; file; file = file->next)
		for (at = 0; at < file->section_count; at++)
			total += lstrlenW(file->sections[at].name) + 1;
	if (required)
		*required = total;
	if (!buffer)
		return TRUE;
	if (size < total) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	for (file = handle; file; file = file->next) {
		for (at = 0; at < file->section_count; at++) {
			DWORD length = lstrlenW(file->sections[at].name) + 1;
			memcpy(out, file->sections[at].name, length * sizeof(WCHAR));
			out += length;
		}
	}
	*out = 0;
	return TRUE;
}
