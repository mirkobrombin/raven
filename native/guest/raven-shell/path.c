/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: the path and pattern helpers the file dialog shares with the shell objects and the file picker. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#include "shell.h"

void copy_text(WCHAR *to, size_t capacity, const WCHAR *from)
{
	size_t length = from ? lstrlenW(from) : 0;
	if (length >= capacity)
		length = capacity - 1;
	if (length)
		memcpy(to, from, length * sizeof(WCHAR));
	to[length] = 0;
}

static BOOL is_separator(WCHAR c)
{
	return c == L'\\' || c == L'/';
}

/* Joins a directory and a name with a single separator. */
void join_path(WCHAR *out, size_t capacity, const WCHAR *dir, const WCHAR *name)
{
	size_t length;
	copy_text(out, capacity, dir);
	length = lstrlenW(out);
	if (length && !is_separator(out[length - 1]) && length + 1 < capacity)
		out[length++] = L'\\', out[length] = 0;
	if (length + lstrlenW(name) < capacity)
		lstrcatW(out, name);
}

BOOL is_absolute(const WCHAR *path)
{
	return is_separator(path[0]) || (path[0] && path[1] == L':');
}

const WCHAR *file_part(const WCHAR *path)
{
	const WCHAR *last = path;
	for (; *path; ++path)
		if (is_separator(*path) || *path == L':')
			last = path + 1;
	return last;
}

void parent_directory(WCHAR *dir)
{
	size_t length = lstrlenW(dir);
	while (length > 3 && is_separator(dir[length - 1]))
		dir[--length] = 0;
	while (length > 3 && !is_separator(dir[length - 1]))
		dir[--length] = 0;
	while (length > 3 && is_separator(dir[length - 1]))
		dir[--length] = 0;
	if (length == 2 && dir[1] == L':')
		dir[length++] = L'\\', dir[length] = 0;
}

static WCHAR fold(WCHAR c)
{
	return c >= L'A' && c <= L'Z' ? (WCHAR)(c + 32) : c;
}

/* Case-insensitive wildcard match of one pattern ('*' and '?') against a file name. */
static BOOL match_pattern(const WCHAR *pattern, const WCHAR *name)
{
	while (*pattern) {
		if (*pattern == L'*') {
			while (*pattern == L'*')
				++pattern;
			if (!*pattern)
				return TRUE;
			for (; *name; ++name)
				if (match_pattern(pattern, name))
					return TRUE;
			return FALSE;
		}
		if (!*name || (*pattern != L'?' && fold(*pattern) != fold(*name)))
			return FALSE;
		++pattern;
		++name;
	}
	return !*name;
}

/* A filter specification is a semicolon separated list of patterns. */
BOOL match_spec(const WCHAR *spec, const WCHAR *name)
{
	WCHAR one[128];
	while (*spec) {
		size_t length = 0;
		while (*spec == L';' || *spec == L' ')
			++spec;
		while (spec[length] && spec[length] != L';' && length < 127)
			++length;
		memcpy(one, spec, length * sizeof(WCHAR));
		one[length] = 0;
		if (length && match_pattern(one, name))
			return TRUE;
		spec += length;
	}
	return FALSE;
}
