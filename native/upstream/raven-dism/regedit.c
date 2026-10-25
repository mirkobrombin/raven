/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven regedit: the Registry Editor as setup programs use it, `regedit /s file.reg`, importing registration entries into the registry.
 *
 * Both formats Windows writes are read: "Windows Registry Editor Version 5.00" files in UTF-16 and REGEDIT4 files in the ANSI code page.
 * A [key] line creates the key and [-key] deletes it with everything below; a value line names the value ("name" or @ for the default
 * one) and gives a string, dword:, hex: or hex(type): data, or - to delete it. Lines that end in a backslash go on in the next line. The
 * editor has no window here: without /s the file is imported all the same, as if the confirmation had been accepted. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static BOOL ansi_file;

static WCHAR upper(WCHAR c) { return c >= L'a' && c <= L'z' ? (WCHAR)(c - 32) : c; }

static BOOL starts_with(const WCHAR *text, const WCHAR *prefix)
{
	while (*prefix && upper(*text) == upper(*prefix))
		++text, ++prefix;
	return !*prefix;
}

static int hex_digit(WCHAR c)
{
	if (c >= L'0' && c <= L'9')
		return c - L'0';
	if (c >= L'a' && c <= L'f')
		return c - L'a' + 10;
	if (c >= L'A' && c <= L'F')
		return c - L'A' + 10;
	return -1;
}

/* The root a key path starts with, and the rest of the path after its backslash. */
static HKEY root_of(const WCHAR *path, const WCHAR **rest)
{
	static const struct { const WCHAR *name; HKEY key; } roots[] = {
		{L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE}, {L"HKEY_CURRENT_USER", HKEY_CURRENT_USER}, {L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT},
		{L"HKEY_USERS", HKEY_USERS}, {L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG}, {L"HKLM", HKEY_LOCAL_MACHINE}, {L"HKCU", HKEY_CURRENT_USER},
		{L"HKCR", HKEY_CLASSES_ROOT}, {L"HKU", HKEY_USERS}, {L"HKCC", HKEY_CURRENT_CONFIG}};
	for (UINT i = 0; i < sizeof(roots) / sizeof(*roots); ++i) {
		const int length = lstrlenW(roots[i].name);
		if (starts_with(path, roots[i].name) && (path[length] == L'\\' || !path[length])) {
			*rest = path[length] ? path + length + 1 : path + length;
			return roots[i].key;
		}
	}
	return NULL;
}

/* A quoted string at text, its escapes undone, into out; the position after the closing quote, or NULL. */
static const WCHAR *quoted(const WCHAR *text, WCHAR *out)
{
	if (*text++ != L'"')
		return NULL;
	while (*text && *text != L'"') {
		if (*text == L'\\' && text[1])
			++text;
		*out++ = *text++;
	}
	*out = 0;
	return *text == L'"' ? text + 1 : NULL;
}

/* Comma-separated hex bytes; the count read, or -1. */
static int hex_bytes(const WCHAR *text, BYTE *out, int capacity)
{
	int count = 0;
	while (*text) {
		while (*text == L' ' || *text == L'\t' || *text == L',')
			++text;
		if (!*text)
			break;
		const int high = hex_digit(text[0]), low = hex_digit(text[1]);
		if (high < 0 || low < 0 || count >= capacity)
			return -1;
		out[count++] = (BYTE)(high * 16 + low);
		text += 2;
	}
	return count;
}

static HKEY open_key(const WCHAR *path)
{
	const WCHAR *rest;
	HKEY root = root_of(path, &rest), key;
	if (!root)
		return NULL;
	if (!*rest)
		return root;
	return RegCreateKeyExW(root, rest, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL) ? NULL : key;
}

static BOOL delete_key(const WCHAR *path)
{
	const WCHAR *rest;
	HKEY root = root_of(path, &rest);
	if (!root || !*rest)
		return FALSE;
	const LONG status = RegDeleteTreeW(root, rest);
	if (!status || status == ERROR_FILE_NOT_FOUND) {
		RegDeleteKeyW(root, rest);
		return TRUE;
	}
	return FALSE;
}

/* One value line of the current key: "name"=data, @=data, or either with - to delete it. */
static BOOL value_line(HKEY key, const WCHAR *line, BYTE *buffer, int capacity)
{
	WCHAR name[1024], *text = (WCHAR *)buffer;
	if (*line == L'@') {
		name[0] = 0;
		++line;
	} else if (!(line = quoted(line, name)))
		return FALSE;
	while (*line == L' ' || *line == L'\t')
		++line;
	if (*line++ != L'=')
		return FALSE;
	while (*line == L' ' || *line == L'\t')
		++line;
	if (*line == L'-') {
		const LONG status = RegDeleteValueW(key, name);
		return !status || status == ERROR_FILE_NOT_FOUND;
	}
	if (*line == L'"') {
		if (!quoted(line, text))
			return FALSE;
		return !RegSetValueExW(key, name, 0, REG_SZ, buffer, (DWORD)(lstrlenW(text) + 1) * sizeof(WCHAR));
	}
	if (starts_with(line, L"dword:")) {
		DWORD value = 0;
		for (line += 6; hex_digit(*line) >= 0; ++line)
			value = value * 16 + (DWORD)hex_digit(*line);
		return !RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
	}
	if (starts_with(line, L"hex")) {
		DWORD type = REG_BINARY;
		line += 3;
		if (*line == L'(') {
			type = 0;
			for (++line; hex_digit(*line) >= 0; ++line)
				type = type * 16 + (DWORD)hex_digit(*line);
			if (*line++ != L')')
				return FALSE;
		}
		if (*line++ != L':')
			return FALSE;
		const int count = hex_bytes(line, buffer, capacity);
		if (count < 0)
			return FALSE;
		/* The string types of a REGEDIT4 file hold ANSI bytes, which the ANSI call widens as Windows stores them. */
		if (ansi_file && (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)) {
			char narrow[1024];
			WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof(narrow), NULL, NULL);
			return !RegSetValueExA(key, narrow, 0, type, buffer, (DWORD)count);
		}
		return !RegSetValueExW(key, name, 0, type, buffer, (DWORD)count);
	}
	return FALSE;
}

/* The file's text as UTF-16: with a byte order mark it already is; otherwise it is in the ANSI code page. */
static WCHAR *read_text(const WCHAR *path)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	const DWORD size = GetFileSize(file, NULL);
	BYTE *bytes = HeapAlloc(GetProcessHeap(), 0, size + 2);
	DWORD read = 0;
	const BOOL done = bytes && ReadFile(file, bytes, size, &read, NULL) && read == size;
	CloseHandle(file);
	if (!done)
		return NULL;
	bytes[size] = bytes[size + 1] = 0;
	if (size >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe)
		return (WCHAR *)(bytes + 2);
	ansi_file = TRUE;
	const int length = MultiByteToWideChar(CP_ACP, 0, (const char *)bytes, (int)size, NULL, 0);
	WCHAR *text = HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
	if (!text)
		return NULL;
	MultiByteToWideChar(CP_ACP, 0, (const char *)bytes, (int)size, text, length);
	text[length] = 0;
	return text;
}

static int import(const WCHAR *path)
{
	WCHAR *text = read_text(path);
	if (!text)
		return 1;
	if (!starts_with(text, L"REGEDIT4") && !starts_with(text, L"Windows Registry Editor Version 5.00"))
		return 1;
	const int capacity = 1 << 20;
	BYTE *buffer = HeapAlloc(GetProcessHeap(), 0, capacity);
	WCHAR *line = HeapAlloc(GetProcessHeap(), 0, capacity);
	if (!buffer || !line)
		return 1;
	HKEY key = NULL;
	int failures = 0;
	for (WCHAR *at = text; *at;) {
		/* A logical line: physical lines joined where one ends in a backslash, leading blanks of the next one dropped. */
		int length = 0;
		for (;;) {
			while (*at && *at != L'\r' && *at != L'\n' && length < capacity / 2 - 1)
				line[length++] = *at++;
			while (*at == L'\r' || *at == L'\n')
				++at;
			if (!length || line[length - 1] != L'\\' || !*at)
				break;
			--length;
			while (*at == L' ' || *at == L'\t')
				++at;
		}
		line[length] = 0;
		WCHAR *start = line;
		while (*start == L' ' || *start == L'\t')
			++start;
		if (*start == L'[') {
			WCHAR *end = start + lstrlenW(start);
			while (end > start && end[-1] != L']')
				--end;
			if (end > start)
				end[-1] = 0;
			if (key && key != HKEY_LOCAL_MACHINE && key != HKEY_CURRENT_USER && key != HKEY_CLASSES_ROOT && key != HKEY_USERS &&
				key != HKEY_CURRENT_CONFIG)
				RegCloseKey(key);
			key = NULL;
			if (start[1] == L'-')
				failures += !delete_key(start + 2);
			else if (!(key = open_key(start + 1)))
				++failures;
		} else if (key && (*start == L'"' || *start == L'@')) {
			failures += !value_line(key, start, buffer, capacity);
		}
	}
	return failures ? 1 : 0;
}

static int regedit(WCHAR *arguments)
{
	int result = 0, files = 0;
	while (*arguments) {
		while (*arguments == L' ' || *arguments == L'\t')
			++arguments;
		if (!*arguments)
			break;
		WCHAR *word = arguments, *out = arguments;
		BOOL in_quotes = FALSE;
		while (*arguments && (in_quotes || (*arguments != L' ' && *arguments != L'\t'))) {
			if (*arguments == L'"')
				in_quotes = !in_quotes;
			else
				*out++ = *arguments;
			++arguments;
		}
		if (*arguments)
			++arguments;
		*out = 0;
		if (*word == L'/' || *word == L'-')
			continue;
		++files;
		result |= import(word);
	}
	return files ? result : 1;
}

/* The start of a program built without a C runtime start file: the command line after the program name. */
void WINAPI raven_program_entry(void)
{
	LPWSTR line = GetCommandLineW();
	BOOL quoted_name = FALSE;
	while (*line && (quoted_name || (*line != L' ' && *line != L'\t'))) {
		if (*line == L'"')
			quoted_name = !quoted_name;
		++line;
	}
	ExitProcess(regedit(line));
}
