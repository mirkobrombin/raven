/* reg.exe: REG QUERY | ADD | DELETE | COPY | EXPORT | IMPORT — reads and changes the registry as REG /? describes, with keys named
   from their root (HKLM, HKCU, HKCR, HKU, HKCC or the long names) and /reg:32 or /reg:64 for either view. ERRORLEVEL is 0 on success
   and 1 on failure. */

#include "tool.h"

static REGSAM view;
static BOOL forced;
static const WCHAR *ok_message = L"The operation completed successfully.\r\n";

static void fail_with(const WCHAR *message)
{
	complain(message);
	ExitProcess(1);
}

static void invalid_syntax(void) { fail_with(L"ERROR: Invalid syntax.\r\nType \"REG /?\" for usage.\r\n"); }
static void not_found(void) { fail_with(L"ERROR: The system was unable to find the specified registry key or value.\r\n"); }

/* A key's root and the path under it, and the name it is shown with. */
struct Key {
	HKEY root;
	const WCHAR *root_name;
	WCHAR *path;
};

static BOOL parse_key(const WCHAR *text, struct Key *key)
{
	static const struct {
		const WCHAR *brief, *name;
		HKEY root;
	} roots[] = {{L"HKLM", L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE}, {L"HKCU", L"HKEY_CURRENT_USER", HKEY_CURRENT_USER},
		{L"HKCR", L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT}, {L"HKU", L"HKEY_USERS", HKEY_USERS},
		{L"HKCC", L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG}};
	/* A computer name before the key, \\machine\HKLM\..., names this one. */
	if (text[0] == L'\\' && text[1] == L'\\') {
		text += 2;
		while (*text && *text != L'\\')
			++text;
		if (*text)
			++text;
	}
	const WCHAR *slash = text;
	while (*slash && *slash != L'\\')
		++slash;
	WCHAR *root = duplicate(text, (DWORD)(slash - text));
	for (DWORD index = 0; index < sizeof roots / sizeof roots[0]; ++index)
		if (equal(root, roots[index].brief) || equal(root, roots[index].name)) {
			key->root = roots[index].root;
			key->root_name = roots[index].name;
			struct Text path = {0};
			text_put(&path, *slash ? slash + 1 : L"");
			while (path.size && path.data[path.size - 1] == L'\\')
				path.data[--path.size] = 0;
			key->path = text_take(&path);
			release(root);
			return TRUE;
		}
	release(root);
	return FALSE;
}

static WCHAR *key_name(const struct Key *key, const WCHAR *path)
{
	struct Text name = {0};
	text_put(&name, key->root_name);
	if (path && *path) {
		text_char(&name, L'\\');
		text_put(&name, path);
	}
	return text_take(&name);
}

static const WCHAR *type_name(DWORD type)
{
	switch (type) {
	case REG_SZ: return L"REG_SZ";
	case REG_EXPAND_SZ: return L"REG_EXPAND_SZ";
	case REG_BINARY: return L"REG_BINARY";
	case REG_DWORD: return L"REG_DWORD";
	case REG_DWORD_BIG_ENDIAN: return L"REG_DWORD_BIG_ENDIAN";
	case REG_LINK: return L"REG_LINK";
	case REG_MULTI_SZ: return L"REG_MULTI_SZ";
	case REG_QWORD: return L"REG_QWORD";
	default: return L"REG_NONE";
	}
}

static BOOL type_of(const WCHAR *name, DWORD *type)
{
	static const DWORD types[] = {REG_SZ, REG_EXPAND_SZ, REG_BINARY, REG_DWORD, REG_DWORD_BIG_ENDIAN, REG_MULTI_SZ, REG_QWORD, REG_NONE};
	for (DWORD index = 0; index < sizeof types / sizeof types[0]; ++index)
		if (equal(name, type_name(types[index]))) {
			*type = types[index];
			return TRUE;
		}
	if (equal(name, L"REG_DWORD_LITTLE_ENDIAN")) {
		*type = REG_DWORD;
		return TRUE;
	}
	return FALSE;
}

/* A value's data as REG QUERY shows it: text, 0x numbers, or hexadecimal bytes; a list's strings joined by \0. */
static void put_data(struct Text *text, DWORD type, const BYTE *data, DWORD size)
{
	switch (type) {
	case REG_SZ:
	case REG_EXPAND_SZ:
	case REG_LINK: {
		const WCHAR *characters = (const WCHAR *)data;
		DWORD count = size / 2;
		while (count && !characters[count - 1])
			--count;
		text_add(text, characters, count);
		break;
	}
	case REG_MULTI_SZ: {
		const WCHAR *characters = (const WCHAR *)data;
		DWORD count = size / 2;
		while (count && !characters[count - 1])
			--count;
		for (DWORD index = 0; index < count; ++index)
			if (characters[index])
				text_char(text, characters[index]);
			else
				text_put(text, L"\\0");
		break;
	}
	case REG_DWORD:
	case REG_DWORD_BIG_ENDIAN:
		if (size >= 4) {
			DWORD value = data[0] | data[1] << 8 | data[2] << 16 | (DWORD)data[3] << 24;
			if (type == REG_DWORD_BIG_ENDIAN)
				value = data[3] | data[2] << 8 | data[1] << 16 | (DWORD)data[0] << 24;
			text_put(text, L"0x");
			put_hex(text, value, 0, TRUE);
		}
		break;
	case REG_QWORD:
		if (size >= 8) {
			ULONGLONG value = 0;
			for (int index = 7; index >= 0; --index)
				value = value << 8 | data[index];
			text_put(text, L"0x");
			put_hex(text, value, 0, TRUE);
		}
		break;
	default:
		for (DWORD index = 0; index < size; ++index)
			put_hex(text, data[index], 2, FALSE);
		break;
	}
}

static BOOL contains_text(const WCHAR *haystack, const WCHAR *needle, BOOL exact, BOOL case_sensitive)
{
	if (exact)
		return case_sensitive ? !lstrcmpW(haystack, needle) : equal(haystack, needle);
	const DWORD size = length(needle);
	for (const WCHAR *at = haystack; *at; ++at) {
		DWORD matched = 0;
		while (matched < size && at[matched] && (case_sensitive ? at[matched] == needle[matched] : folded(at[matched]) == folded(needle[matched])))
			++matched;
		if (matched == size)
			return TRUE;
	}
	return !size;
}

struct Query {
	const WCHAR *value_name;
	BOOL default_only, recursive, searching, in_keys, in_names, in_data, exact, case_sensitive;
	const WCHAR *find;
	BOOL type_given;
	DWORD type;
	DWORD matches;
	const WCHAR *separator;
};

static BOOL query_key(const struct Key *key, const WCHAR *path, struct Query *query, BOOL top)
{
	HKEY handle;
	if (RegOpenKeyExW(key->root, *path ? path : NULL, 0, KEY_READ | view, &handle))
		return FALSE;
	WCHAR *name = key_name(key, path);
	struct Text out = {0};
	BOOL header_shown = FALSE, any_value = FALSE;
	const BOOL key_matches = query->searching && query->in_keys && !top && contains_text(name + length(name) - length(path), query->find,
		query->exact, query->case_sensitive);
	WCHAR *value_name = allocate(16384 * sizeof(WCHAR));
	BYTE *data = allocate(1 << 20);
	for (DWORD index = 0;; ++index) {
		DWORD name_size = 16384, data_size = 1 << 20, type = 0;
		const LONG result = RegEnumValueW(handle, index, value_name, &name_size, NULL, &type, data, &data_size);
		if (result == ERROR_NO_MORE_ITEMS)
			break;
		if (result)
			continue;
		value_name[name_size] = 0;
		if (query->default_only && value_name[0])
			continue;
		if (query->value_name && !equal(value_name, query->value_name))
			continue;
		if (query->type_given && type != query->type)
			continue;
		struct Text shown = {0};
		put_data(&shown, type, data, data_size);
		if (query->separator && type == REG_MULTI_SZ) {
			struct Text joined_text = {0};
			for (WCHAR *at = shown.data; at && *at; ++at)
				if (at[0] == L'\\' && at[1] == L'0')
					text_put(&joined_text, query->separator), ++at;
				else
					text_char(&joined_text, *at);
			release(shown.data);
			shown = joined_text;
		}
		if (query->searching) {
			BOOL hit = query->in_names && contains_text(value_name, query->find, query->exact, query->case_sensitive);
			if (!hit && query->in_data)
				hit = (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ) &&
					contains_text(shown.data ? shown.data : L"", query->find, query->exact, query->case_sensitive);
			if (!hit) {
				release(shown.data);
				continue;
			}
			++query->matches;
		}
		if (!header_shown) {
			text_put(&out, L"\r\n");
			text_put(&out, name);
			text_put(&out, L"\r\n");
			header_shown = TRUE;
		}
		text_put(&out, L"    ");
		text_put(&out, value_name[0] ? value_name : L"(Default)");
		text_put(&out, L"    ");
		text_put(&out, type_name(type));
		text_put(&out, L"    ");
		text_put(&out, shown.data);
		text_put(&out, L"\r\n");
		release(shown.data);
		any_value = TRUE;
	}
	if (top && !header_shown && !query->searching && !query->value_name && !query->default_only) {
		text_put(&out, L"\r\n");
		text_put(&out, name);
		text_put(&out, L"\r\n");
		header_shown = TRUE;
	}
	if (key_matches && !header_shown) {
		++query->matches;
		text_put(&out, L"\r\n");
		text_put(&out, name);
		text_put(&out, L"\r\n");
		header_shown = TRUE;
	}
	say_text(&out);
	/* Without /S the subkeys are listed; with it each one is queried in turn. */
	WCHAR *sub = allocate(256 * sizeof(WCHAR));
	BOOL listed = FALSE;
	for (DWORD index = 0;; ++index) {
		DWORD sub_size = 256;
		const LONG result = RegEnumKeyExW(handle, index, sub, &sub_size, NULL, NULL, NULL, NULL);
		if (result == ERROR_NO_MORE_ITEMS)
			break;
		if (result)
			continue;
		struct Text child = {0};
		text_put(&child, path);
		if (*path)
			text_char(&child, L'\\');
		text_put(&child, sub);
		if (query->recursive)
			any_value |= query_key(key, child.data, query, FALSE);
		else if (!query->value_name && !query->default_only) {
			if (!listed)
				say(L"\r\n");
			listed = TRUE;
			WCHAR *child_name = key_name(key, child.data);
			say(child_name);
			say(L"\r\n");
			release(child_name);
		}
		release(child.data);
	}
	RegCloseKey(handle);
	release(sub);
	release(data);
	release(value_name);
	release(name);
	return any_value || header_shown;
}

static BOOL ask_yes(const WCHAR *question, BOOL *all)
{
	if (forced || (all && *all))
		return TRUE;
	say(question);
	HANDLE keys = console_input();
	WCHAR answer = 0;
	if (keys) {
		WCHAR line[64];
		DWORD count = 0;
		if (ReadConsoleW(keys, line, 64, &count, NULL) && count)
			answer = folded(line[0]);
	} else {
		char byte = 0;
		DWORD count = 0;
		while (ReadFile(input, &byte, 1, &count, NULL) && count && (byte == ' ' || byte == '\r' || byte == '\n'))
			;
		answer = count ? folded((WCHAR)byte) : 0;
		say(L"\r\n");
	}
	if (answer == L'A' && all)
		*all = TRUE;
	return answer == L'Y' || (answer == L'A' && all);
}

/* The bytes of a value from REG ADD's /D, by its type. */
static BOOL encode_data(DWORD type, const WCHAR *text, const WCHAR *separator, BYTE **data, DWORD *size)
{
	if (type == REG_DWORD || type == REG_DWORD_BIG_ENDIAN || type == REG_QWORD) {
		ULONGLONG value = 0;
		if (*text && !read_number(text, &value))
			return FALSE;
		if (type != REG_QWORD && value > 0xffffffffULL)
			return FALSE;
		*size = type == REG_QWORD ? 8 : 4;
		*data = allocate(8);
		for (DWORD index = 0; index < *size; ++index)
			(*data)[type == REG_DWORD_BIG_ENDIAN ? 3 - index : index] = (BYTE)(value >> (index * 8));
		return TRUE;
	}
	if (type == REG_BINARY || type == REG_NONE) {
		const DWORD count = length(text);
		if (count % 2)
			return FALSE;
		*size = count / 2;
		*data = allocate(*size + 1);
		for (DWORD index = 0; index < count; ++index) {
			const WCHAR character = folded(text[index]);
			BYTE nibble;
			if (digit(character))
				nibble = (BYTE)(character - L'0');
			else if (character >= L'A' && character <= L'F')
				nibble = (BYTE)(character - L'A' + 10);
			else
				return FALSE;
			(*data)[index / 2] = (BYTE)((*data)[index / 2] << 4 | nibble);
		}
		return TRUE;
	}
	struct Text value = {0};
	if (type == REG_MULTI_SZ) {
		/* The strings are separated by \0, or by the character /S names. */
		const DWORD separator_size = length(separator);
		for (const WCHAR *at = text; *at;) {
			BOOL split = TRUE;
			for (DWORD index = 0; index < separator_size && split; ++index)
				split = at[index] == separator[index];
			if (split && separator_size) {
				text_char(&value, 0);
				at += separator_size;
			} else
				text_char(&value, *at++);
		}
		text_char(&value, 0);
		text_char(&value, 0);
	} else {
		text_put(&value, text);
		text_char(&value, 0);
	}
	*size = value.size * sizeof(WCHAR);
	*data = (BYTE *)text_take(&value);
	return TRUE;
}

static int command_add(struct Arguments *arguments)
{
	struct Key key;
	if (arguments->count < 2 || !parse_key(arguments->items[1], &key))
		invalid_syntax();
	const WCHAR *name = NULL, *data_text = L"", *separator = L"\\0";
	BOOL has_value = FALSE, has_data = FALSE;
	DWORD type = REG_SZ;
	for (DWORD index = 2; index < arguments->count; ++index) {
		const WCHAR *argument = arguments->items[index];
		const BOOL value = index + 1 < arguments->count;
		if (switch_is(argument, L"/V") && value)
			name = arguments->items[++index], has_value = TRUE;
		else if (switch_is(argument, L"/VE"))
			name = L"", has_value = TRUE;
		else if (switch_is(argument, L"/T") && value) {
			if (!type_of(arguments->items[++index], &type))
				invalid_syntax();
		} else if (switch_is(argument, L"/S") && value)
			separator = arguments->items[++index];
		else if (switch_is(argument, L"/D") && value)
			data_text = arguments->items[++index], has_data = TRUE;
		else if (switch_is(argument, L"/F"))
			forced = TRUE;
		else if (!switch_is(argument, L"/REG:32") && !switch_is(argument, L"/REG:64"))
			invalid_syntax();
	}
	HKEY handle;
	if (RegCreateKeyExW(key.root, key.path, 0, NULL, 0, KEY_READ | KEY_WRITE | view, NULL, &handle, NULL))
		fail_with(L"ERROR: Access is denied.\r\n");
	if (has_value || has_data) {
		if (!has_value)
			name = L"";
		DWORD existing_size = 0;
		if (!RegQueryValueExW(handle, name, NULL, NULL, NULL, &existing_size)) {
			struct Text question = {0};
			text_put(&question, L"Value ");
			text_put(&question, *name ? name : L"(Default)");
			text_put(&question, L" exists, overwrite(Yes/No)? ");
			const BOOL yes = ask_yes(question.data, NULL);
			release(question.data);
			if (!yes) {
				RegCloseKey(handle);
				say(L"The operation was canceled by the user.\r\n");
				return 0;
			}
		}
		BYTE *data;
		DWORD size;
		if (!encode_data(type, data_text, separator, &data, &size)) {
			RegCloseKey(handle);
			invalid_syntax();
		}
		const LONG result = RegSetValueExW(handle, name, 0, type, data, size);
		release(data);
		if (result) {
			RegCloseKey(handle);
			fail_with(L"ERROR: Access is denied.\r\n");
		}
	}
	RegCloseKey(handle);
	say(ok_message);
	return 0;
}

/* Deletes a key and everything under it. */
static LONG delete_tree(HKEY root, const WCHAR *path)
{
	HKEY handle;
	LONG result = RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WRITE | view, &handle);
	if (result)
		return result;
	WCHAR sub[256];
	for (;;) {
		DWORD size = 256;
		if (RegEnumKeyExW(handle, 0, sub, &size, NULL, NULL, NULL, NULL))
			break;
		WCHAR *child = joined(path, sub);
		result = delete_tree(root, child);
		release(child);
		if (result)
			break;
	}
	RegCloseKey(handle);
	return result ? result : RegDeleteKeyW(root, path);
}

static int command_delete(struct Arguments *arguments)
{
	struct Key key;
	if (arguments->count < 2 || !parse_key(arguments->items[1], &key))
		invalid_syntax();
	const WCHAR *name = NULL;
	BOOL all_values = FALSE;
	for (DWORD index = 2; index < arguments->count; ++index) {
		const WCHAR *argument = arguments->items[index];
		if (switch_is(argument, L"/V") && index + 1 < arguments->count)
			name = arguments->items[++index];
		else if (switch_is(argument, L"/VE"))
			name = L"";
		else if (switch_is(argument, L"/VA"))
			all_values = TRUE;
		else if (switch_is(argument, L"/F"))
			forced = TRUE;
		else if (!switch_is(argument, L"/REG:32") && !switch_is(argument, L"/REG:64"))
			invalid_syntax();
	}
	HKEY handle;
	if (RegOpenKeyExW(key.root, key.path, 0, KEY_READ | KEY_WRITE | view, &handle))
		not_found();
	if (!name && !all_values) {
		RegCloseKey(handle);
		if (!*key.path)
			fail_with(L"ERROR: Invalid key name.\r\n");
		WCHAR *shown = key_name(&key, key.path);
		struct Text question = {0};
		text_put(&question, L"Permanently delete the registry key ");
		text_put(&question, shown);
		text_put(&question, L" (Yes/No)? ");
		release(shown);
		const BOOL yes = ask_yes(question.data, NULL);
		release(question.data);
		if (!yes)
			return 0;
		if (delete_tree(key.root, key.path))
			fail_with(L"ERROR: Access is denied.\r\n");
		say(ok_message);
		return 0;
	}
	struct Text question = {0};
	if (all_values)
		text_put(&question, L"Delete all values under the registry key ");
	else
		text_put(&question, L"Delete the registry value ");
	if (all_values) {
		WCHAR *shown = key_name(&key, key.path);
		text_put(&question, shown);
		release(shown);
	} else
		text_put(&question, *name ? name : L"(Default)");
	text_put(&question, L" (Yes/No)? ");
	const BOOL yes = ask_yes(question.data, NULL);
	release(question.data);
	if (!yes) {
		RegCloseKey(handle);
		return 0;
	}
	if (all_values) {
		WCHAR value_name[16384];
		for (;;) {
			DWORD size = 16384;
			if (RegEnumValueW(handle, 0, value_name, &size, NULL, NULL, NULL, NULL))
				break;
			if (RegDeleteValueW(handle, value_name))
				break;
		}
	} else if (RegDeleteValueW(handle, name)) {
		RegCloseKey(handle);
		not_found();
	}
	RegCloseKey(handle);
	say(ok_message);
	return 0;
}

static LONG copy_key(HKEY from_root, const WCHAR *from, HKEY to_root, const WCHAR *to, BOOL recursive, BOOL *all)
{
	HKEY source, target;
	LONG result = RegOpenKeyExW(from_root, *from ? from : NULL, 0, KEY_READ | view, &source);
	if (result)
		return result;
	result = RegCreateKeyExW(to_root, to, 0, NULL, 0, KEY_READ | KEY_WRITE | view, NULL, &target, NULL);
	if (result) {
		RegCloseKey(source);
		return result;
	}
	WCHAR *name = allocate(16384 * sizeof(WCHAR));
	BYTE *data = allocate(1 << 20);
	for (DWORD index = 0;; ++index) {
		DWORD name_size = 16384, data_size = 1 << 20, type;
		const LONG got = RegEnumValueW(source, index, name, &name_size, NULL, &type, data, &data_size);
		if (got == ERROR_NO_MORE_ITEMS)
			break;
		if (got)
			continue;
		DWORD existing = 0;
		if (!RegQueryValueExW(target, name, NULL, NULL, NULL, &existing)) {
			struct Text question = {0};
			text_put(&question, L"Value ");
			text_put(&question, *name ? name : L"(Default)");
			text_put(&question, L" exists, overwrite(Yes/No/All)? ");
			const BOOL yes = ask_yes(question.data, all);
			release(question.data);
			if (!yes)
				continue;
		}
		RegSetValueExW(target, name, 0, type, data, data_size);
	}
	if (recursive)
		for (DWORD index = 0;; ++index) {
			DWORD size = 256;
			WCHAR sub[256];
			const LONG got = RegEnumKeyExW(source, index, sub, &size, NULL, NULL, NULL, NULL);
			if (got == ERROR_NO_MORE_ITEMS)
				break;
			if (got)
				continue;
			WCHAR *child_from = *from ? joined(from, sub) : copy_of(sub), *child_to = joined(to, sub);
			copy_key(from_root, child_from, to_root, child_to, recursive, all);
			release(child_from);
			release(child_to);
		}
	release(name);
	release(data);
	RegCloseKey(source);
	RegCloseKey(target);
	return 0;
}

static int command_copy(struct Arguments *arguments)
{
	struct Key from, to;
	if (arguments->count < 3 || !parse_key(arguments->items[1], &from) || !parse_key(arguments->items[2], &to))
		invalid_syntax();
	BOOL recursive = FALSE, all = FALSE;
	for (DWORD index = 3; index < arguments->count; ++index) {
		if (switch_is(arguments->items[index], L"/S"))
			recursive = TRUE;
		else if (switch_is(arguments->items[index], L"/F"))
			forced = TRUE;
		else if (!switch_is(arguments->items[index], L"/REG:32") && !switch_is(arguments->items[index], L"/REG:64"))
			invalid_syntax();
	}
	if (copy_key(from.root, from.path, to.root, to.path, recursive, &all))
		not_found();
	say(ok_message);
	return 0;
}

/* .reg files: the text REGEDIT writes, in UTF-16 with its byte order mark. */
static void put_reg_string(struct Text *text, const WCHAR *value, DWORD count)
{
	text_char(text, L'"');
	for (DWORD index = 0; index < count; ++index) {
		if (value[index] == L'\\' || value[index] == L'"')
			text_char(text, L'\\');
		text_char(text, value[index]);
	}
	text_char(text, L'"');
}

static void put_reg_hex(struct Text *text, const BYTE *data, DWORD size, DWORD line_start)
{
	DWORD column = line_start;
	for (DWORD index = 0; index < size; ++index) {
		put_hex(text, data[index], 2, TRUE);
		if (index + 1 < size) {
			text_char(text, L',');
			column += 3;
			if (column > 76) {
				text_put(text, L"\\\r\n  ");
				column = 2;
			}
		}
	}
}

static void export_key(struct Text *text, HKEY root, const WCHAR *root_name, const WCHAR *path)
{
	HKEY handle;
	if (RegOpenKeyExW(root, *path ? path : NULL, 0, KEY_READ | view, &handle))
		return;
	text_put(text, L"\r\n[");
	text_put(text, root_name);
	if (*path) {
		text_char(text, L'\\');
		text_put(text, path);
	}
	text_put(text, L"]\r\n");
	WCHAR *name = allocate(16384 * sizeof(WCHAR));
	BYTE *data = allocate(1 << 20);
	for (DWORD index = 0;; ++index) {
		DWORD name_size = 16384, data_size = 1 << 20, type;
		const LONG got = RegEnumValueW(handle, index, name, &name_size, NULL, &type, data, &data_size);
		if (got == ERROR_NO_MORE_ITEMS)
			break;
		if (got)
			continue;
		const DWORD start = text->size;
		if (name[0])
			put_reg_string(text, name, name_size);
		else
			text_char(text, L'@');
		text_char(text, L'=');
		if (type == REG_SZ) {
			DWORD count = data_size / 2;
			while (count && !((WCHAR *)data)[count - 1])
				--count;
			put_reg_string(text, (WCHAR *)data, count);
		} else if (type == REG_DWORD && data_size == 4) {
			text_put(text, L"dword:");
			put_hex(text, data[0] | data[1] << 8 | data[2] << 16 | (DWORD)data[3] << 24, 8, TRUE);
		} else {
			if (type == REG_BINARY)
				text_put(text, L"hex:");
			else {
				text_put(text, L"hex(");
				put_hex(text, type, 0, TRUE);
				text_put(text, L"):");
			}
			put_reg_hex(text, data, data_size, text->size - start);
		}
		text_put(text, L"\r\n");
	}
	release(name);
	release(data);
	for (DWORD index = 0;; ++index) {
		DWORD size = 256;
		WCHAR sub[256];
		const LONG got = RegEnumKeyExW(handle, index, sub, &size, NULL, NULL, NULL, NULL);
		if (got == ERROR_NO_MORE_ITEMS)
			break;
		if (got)
			continue;
		WCHAR *child = *path ? joined(path, sub) : copy_of(sub);
		export_key(text, root, root_name, child);
		release(child);
	}
	RegCloseKey(handle);
}

static int command_export(struct Arguments *arguments)
{
	struct Key key;
	if (arguments->count < 3 || !parse_key(arguments->items[1], &key))
		invalid_syntax();
	for (DWORD index = 3; index < arguments->count; ++index)
		if (switch_is(arguments->items[index], L"/Y"))
			forced = TRUE;
		else if (!switch_is(arguments->items[index], L"/REG:32") && !switch_is(arguments->items[index], L"/REG:64"))
			invalid_syntax();
	HKEY probe;
	if (RegOpenKeyExW(key.root, *key.path ? key.path : NULL, 0, KEY_READ | view, &probe))
		not_found();
	RegCloseKey(probe);
	const WCHAR *file_name = arguments->items[2];
	if (GetFileAttributesW(file_name) != INVALID_FILE_ATTRIBUTES) {
		struct Text question = {0};
		text_put(&question, L"File ");
		text_put(&question, file_name);
		text_put(&question, L" already exists. Overwrite (Yes/No)? ");
		const BOOL yes = ask_yes(question.data, NULL);
		release(question.data);
		if (!yes)
			return 0;
	}
	struct Text text = {0};
	text_char(&text, 0xfeff);
	text_put(&text, L"Windows Registry Editor Version 5.00\r\n");
	export_key(&text, key.root, key.root_name, key.path);
	text_put(&text, L"\r\n");
	HANDLE file = CreateFileW(file_name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD written;
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, text.data, text.size * sizeof(WCHAR), &written, NULL))
		fail_with(L"ERROR: Error accessing the registry.\r\n");
	CloseHandle(file);
	say(ok_message);
	return 0;
}

/* A string of a .reg file at `*at`, after its opening quote, with \\ and \" undone. */
static WCHAR *read_reg_string(const WCHAR **at)
{
	struct Text value = {0};
	while (**at && **at != L'"') {
		if (**at == L'\\' && (*at)[1])
			++*at;
		text_char(&value, *(*at)++);
	}
	if (**at == L'"')
		++*at;
	return text_take(&value);
}

static int command_import(struct Arguments *arguments)
{
	if (arguments->count < 2)
		invalid_syntax();
	DWORD size;
	WCHAR *text = read_file_text(arguments->items[1], &size);
	if (!text)
		fail_with(L"ERROR: Error opening the file. There may be a disk or file system error.\r\n");
	/* Lines that end in a backslash go on in the next one. */
	struct Text joined_text = {0};
	DWORD at = 0, start, line_size;
	HKEY handle = NULL;
	BOOL header = FALSE;
	while (next_line(text, size, &at, &start, &line_size)) {
		text_add(&joined_text, text + start, line_size);
		if (joined_text.size && joined_text.data[joined_text.size - 1] == L'\\') {
			joined_text.data[--joined_text.size] = 0;
			continue;
		}
		const WCHAR *line = joined_text.data ? joined_text.data : L"";
		while (blank(*line))
			++line;
		if (!header) {
			if (*line) {
				if (!starts(line, L"Windows Registry Editor Version 5.00") && !starts(line, L"REGEDIT4"))
					fail_with(L"ERROR: The specified file is not a registry file.\r\n");
				header = TRUE;
			}
		} else if (*line == L'[') {
			if (handle)
				RegCloseKey(handle);
			handle = NULL;
			const BOOL removing = line[1] == L'-';
			const WCHAR *name_start = line + (removing ? 2 : 1), *close = name_start;
			while (*close && *close != L']')
				++close;
			WCHAR *name = duplicate(name_start, (DWORD)(close - name_start));
			struct Key key;
			if (parse_key(name, &key)) {
				if (removing)
					delete_tree(key.root, key.path);
				else
					RegCreateKeyExW(key.root, key.path, 0, NULL, 0, KEY_READ | KEY_WRITE | view, NULL, &handle, NULL);
			}
			release(name);
		} else if (handle && (*line == L'"' || *line == L'@')) {
			WCHAR *name;
			const WCHAR *cursor = line;
			if (*cursor == L'@') {
				name = copy_of(L"");
				++cursor;
			} else {
				++cursor;
				name = read_reg_string(&cursor);
			}
			while (blank(*cursor))
				++cursor;
			if (*cursor == L'=') {
				++cursor;
				while (blank(*cursor))
					++cursor;
				if (*cursor == L'-')
					RegDeleteValueW(handle, name);
				else if (*cursor == L'"') {
					++cursor;
					WCHAR *value = read_reg_string(&cursor);
					RegSetValueExW(handle, name, 0, REG_SZ, (const BYTE *)value, (length(value) + 1) * sizeof(WCHAR));
					release(value);
				} else if (starts(cursor, L"dword:")) {
					ULONGLONG number = 0;
					struct Text hex = {0};
					text_put(&hex, L"0x");
					text_put(&hex, cursor + 6);
					read_number(hex.data, &number);
					release(hex.data);
					const DWORD value = (DWORD)number;
					RegSetValueExW(handle, name, 0, REG_DWORD, (const BYTE *)&value, 4);
				} else if (starts(cursor, L"hex")) {
					DWORD type = REG_BINARY;
					cursor += 3;
					if (*cursor == L'(') {
						ULONGLONG number = 0;
						struct Text hex = {0};
						text_put(&hex, L"0x");
						for (++cursor; *cursor && *cursor != L')'; ++cursor)
							text_char(&hex, *cursor);
						read_number(hex.data, &number);
						release(hex.data);
						type = (DWORD)number;
						if (*cursor)
							++cursor;
					}
					if (*cursor == L':')
						++cursor;
					BYTE *data = allocate(length(cursor) / 2 + 2);
					DWORD count = 0, nibbles = 0;
					BYTE current = 0;
					for (; *cursor; ++cursor) {
						const WCHAR character = folded(*cursor);
						if (digit(character) || (character >= L'A' && character <= L'F')) {
							current = (BYTE)(current << 4 | (digit(character) ? character - L'0' : character - L'A' + 10));
							if (++nibbles == 2) {
								data[count++] = current;
								current = 0;
								nibbles = 0;
							}
						}
					}
					RegSetValueExW(handle, name, 0, type, data, count);
					release(data);
				}
			}
			release(name);
		}
		joined_text.size = 0;
		if (joined_text.data)
			joined_text.data[0] = 0;
	}
	if (handle)
		RegCloseKey(handle);
	release(text);
	say(ok_message);
	return 0;
}

static int command_query(struct Arguments *arguments)
{
	struct Key key;
	if (arguments->count < 2 || !parse_key(arguments->items[1], &key))
		invalid_syntax();
	struct Query query;
	memset(&query, 0, sizeof query);
	for (DWORD index = 2; index < arguments->count; ++index) {
		const WCHAR *argument = arguments->items[index];
		const BOOL value = index + 1 < arguments->count;
		if (switch_is(argument, L"/V") && value)
			query.value_name = arguments->items[++index];
		else if (switch_is(argument, L"/VE"))
			query.default_only = TRUE;
		else if (switch_is(argument, L"/S"))
			query.recursive = TRUE;
		else if (switch_is(argument, L"/F") && value) {
			query.find = arguments->items[++index];
			query.searching = TRUE;
		} else if (switch_is(argument, L"/K"))
			query.in_keys = TRUE;
		else if (switch_is(argument, L"/D"))
			query.in_data = TRUE;
		else if (switch_is(argument, L"/C"))
			query.case_sensitive = TRUE;
		else if (switch_is(argument, L"/E"))
			query.exact = TRUE;
		else if (switch_is(argument, L"/T") && value) {
			if (!type_of(arguments->items[++index], &query.type))
				invalid_syntax();
			query.type_given = TRUE;
		} else if (switch_is(argument, L"/SE") && value)
			query.separator = arguments->items[++index];
		else if (switch_is(argument, L"/Z"))
			;
		else if (!switch_is(argument, L"/REG:32") && !switch_is(argument, L"/REG:64"))
			invalid_syntax();
	}
	/* With /F, a search in key names, value names and data, unless /K or /D narrows it to key names or data. */
	if (query.searching && !query.in_keys && !query.in_data)
		query.in_keys = query.in_names = query.in_data = TRUE;
	HKEY probe;
	if (RegOpenKeyExW(key.root, *key.path ? key.path : NULL, 0, KEY_READ | view, &probe))
		not_found();
	RegCloseKey(probe);
	const BOOL found = query_key(&key, key.path, &query, TRUE);
	if (query.searching) {
		struct Text summary = {0};
		text_put(&summary, L"End of search: ");
		put_number(&summary, query.matches, FALSE);
		text_put(&summary, L" match(es) found.\r\n");
		say_text(&summary);
		return query.matches ? 0 : 1;
	}
	if ((query.value_name || query.default_only) && !found)
		not_found();
	return 0;
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	for (DWORD index = 0; index < arguments.count; ++index) {
		if (switch_is(arguments.items[index], L"/REG:32"))
			view = KEY_WOW64_32KEY;
		else if (switch_is(arguments.items[index], L"/REG:64"))
			view = KEY_WOW64_64KEY;
	}
	if (!arguments.count || switch_is(arguments.items[0], L"/?")) {
		say(L"\r\nREG Operation [Parameter List]\r\n\r\n"
			L"  Operation  [ QUERY   | ADD    | DELETE  | COPY    |\r\n               EXPORT  | IMPORT ]\r\n\r\n"
			L"Return Code: (Except for REG COMPARE):\r\n\r\n  0 - Successful\r\n  1 - Failed\r\n");
		ExitProcess(arguments.count ? 0 : 1);
	}
	const WCHAR *operation = arguments.items[0];
	if (arguments.count > 1 && switch_is(arguments.items[1], L"/?")) {
		say(L"Type \"REG /?\" for the operations; each takes a key name such as HKLM\\Software\\Name.\r\n");
		ExitProcess(0);
	}
	int status;
	if (equal(operation, L"QUERY"))
		status = command_query(&arguments);
	else if (equal(operation, L"ADD"))
		status = command_add(&arguments);
	else if (equal(operation, L"DELETE"))
		status = command_delete(&arguments);
	else if (equal(operation, L"COPY"))
		status = command_copy(&arguments);
	else if (equal(operation, L"EXPORT"))
		status = command_export(&arguments);
	else if (equal(operation, L"IMPORT"))
		status = command_import(&arguments);
	else {
		complain(L"ERROR: Invalid key name.\r\nType \"REG /?\" for usage.\r\n");
		status = 1;
	}
	ExitProcess((UINT)status);
	return 0;
}
