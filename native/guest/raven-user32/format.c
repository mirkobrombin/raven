/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <stdarg.h>

/* wsprintf and its relatives of user32, and wnsprintf and its own of shlwapi, as Microsoft documents them: the conversions c, C, d, i, u,
 * x, X, s and S with their flags, width and precision and the sizes h, l and w; a conversion it does not know ends the text. c and s take
 * characters of the function's own width, C and S of the other, h narrow ones and l or w wide ones. wsprintf and wvsprintf write at most
 * 1024 characters with the terminator, wnsprintf and wvnsprintf as many as they are given. The text is made in the calling thread; only a
 * piece outside ASCII that changes width goes through the code page. */

struct output {
	char *narrow;
	WCHAR *wide;
	int room, used;
};

struct conversion {
	BOOL left, zero, plus, space, alternate, narrow_size, wide_size, width_argument, precision_argument;
	int width, precision;
	char type;
};

static BOOL ascii_narrow(const char *text, int length)
{
	for (int at = 0; at < length; ++at)
		if ((unsigned char)text[at] >= 0x80)
			return FALSE;
	return TRUE;
}

static BOOL ascii_wide(const WCHAR *text, int length)
{
	for (int at = 0; at < length; ++at)
		if (text[at] >= 0x80)
			return FALSE;
	return TRUE;
}

/* The units of the output a piece takes: characters of a wide output, bytes of a narrow one. */
static int narrow_units(const struct output *out, const char *text, int length)
{
	if (out->narrow || !length || ascii_narrow(text, length))
		return length;
	return MultiByteToWideChar(CP_ACP, 0, text, length, NULL, 0);
}

static int wide_units(const struct output *out, const WCHAR *text, int length)
{
	if (out->wide || !length || ascii_wide(text, length))
		return length;
	return WideCharToMultiByte(CP_ACP, 0, text, length, NULL, 0, NULL, NULL);
}

static void put_narrow(struct output *out, const char *text, int length)
{
	const int left = out->room - out->used;
	if (length <= 0 || left <= 0)
		return;
	const int fits = length < left ? length : left;
	if (out->narrow) {
		memcpy(out->narrow + out->used, text, fits);
		out->used += fits;
	} else if (ascii_narrow(text, length)) {
		for (int at = 0; at < fits; ++at)
			out->wide[out->used + at] = (unsigned char)text[at];
		out->used += fits;
	} else {
		const int units = MultiByteToWideChar(CP_ACP, 0, text, length, NULL, 0);
		if (units <= left) {
			out->used += MultiByteToWideChar(CP_ACP, 0, text, length, out->wide + out->used, units);
			return;
		}
		WCHAR *whole = HeapAlloc(GetProcessHeap(), 0, units * sizeof(WCHAR));
		if (!whole)
			return;
		MultiByteToWideChar(CP_ACP, 0, text, length, whole, units);
		memcpy(out->wide + out->used, whole, left * sizeof(WCHAR));
		out->used += left;
		HeapFree(GetProcessHeap(), 0, whole);
	}
}

static void put_wide(struct output *out, const WCHAR *text, int length)
{
	const int left = out->room - out->used;
	if (length <= 0 || left <= 0)
		return;
	const int fits = length < left ? length : left;
	if (out->wide) {
		memcpy(out->wide + out->used, text, fits * sizeof(WCHAR));
		out->used += fits;
	} else if (ascii_wide(text, length)) {
		for (int at = 0; at < fits; ++at)
			out->narrow[out->used + at] = (char)text[at];
		out->used += fits;
	} else {
		const int units = WideCharToMultiByte(CP_ACP, 0, text, length, NULL, 0, NULL, NULL);
		if (units <= left) {
			out->used += WideCharToMultiByte(CP_ACP, 0, text, length, out->narrow + out->used, units, NULL, NULL);
			return;
		}
		char *whole = HeapAlloc(GetProcessHeap(), 0, units);
		if (!whole)
			return;
		WideCharToMultiByte(CP_ACP, 0, text, length, whole, units, NULL, NULL);
		memcpy(out->narrow + out->used, whole, left);
		out->used += left;
		HeapFree(GetProcessHeap(), 0, whole);
	}
}

static void put_repeated(struct output *out, char character, int count)
{
	for (; count > 0 && out->used < out->room; --count) {
		if (out->narrow)
			out->narrow[out->used++] = character;
		else
			out->wide[out->used++] = (unsigned char)character;
	}
}

/* One unit of the pattern, narrow or wide. */
static unsigned pattern_at(const char *narrow, const WCHAR *wide, size_t at)
{
	return narrow ? (unsigned char)narrow[at] : wide[at];
}

static BOOL parse(const char *narrow, const WCHAR *wide, size_t *at, struct conversion *item)
{
	unsigned c;
	memset(item, 0, sizeof(*item));
	item->precision = -1;
	for (;; ++*at) {
		c = pattern_at(narrow, wide, *at);
		if (c == '-') item->left = TRUE;
		else if (c == '0') item->zero = TRUE;
		else if (c == '+') item->plus = TRUE;
		else if (c == ' ') item->space = TRUE;
		else if (c == '#') item->alternate = TRUE;
		else break;
	}
	if (pattern_at(narrow, wide, *at) == '*') {
		item->width_argument = TRUE;
		++*at;
	} else {
		for (; (c = pattern_at(narrow, wide, *at)) >= '0' && c <= '9'; ++*at)
			item->width = item->width * 10 + (int)(c - '0') < 1024 ? item->width * 10 + (int)(c - '0') : 1024;
	}
	if (pattern_at(narrow, wide, *at) == '.') {
		++*at;
		item->precision = 0;
		if (pattern_at(narrow, wide, *at) == '*') {
			item->precision_argument = TRUE;
			++*at;
		} else {
			for (; (c = pattern_at(narrow, wide, *at)) >= '0' && c <= '9'; ++*at)
				item->precision = item->precision * 10 + (int)(c - '0') < 1024 ? item->precision * 10 + (int)(c - '0') : 1024;
		}
	}
	for (;; ++*at) {
		c = pattern_at(narrow, wide, *at);
		if (c == 'l' || c == 'w') item->wide_size = TRUE;
		else if (c == 'h') item->narrow_size = TRUE;
		else break;
	}
	c = pattern_at(narrow, wide, *at);
	if (c != 'c' && c != 'C' && c != 'd' && c != 'i' && c != 'u' && c != 'x' && c != 'X' && c != 's' && c != 'S')
		return FALSE;
	item->type = (char)c;
	++*at;
	return TRUE;
}

static void put_number(struct output *out, const struct conversion *item, ULONG_PTR value)
{
	const BOOL is_signed = item->type == 'd' || item->type == 'i', hexadecimal = item->type == 'x' || item->type == 'X';
	UINT32 raw = (UINT32)value;
	if (item->narrow_size)
		raw = is_signed ? (UINT32)(INT32)(INT16)raw : (UINT16)raw;
	const BOOL negative = is_signed && (INT32)raw < 0;
	UINT32 magnitude = negative ? 0u - raw : raw;
	char digits[16], prefix[2];
	int count = 0, prefix_length = 0;
	do {
		const unsigned digit = magnitude % (hexadecimal ? 16 : 10);
		digits[count++] = (char)(digit < 10 ? '0' + digit : (item->type == 'X' ? 'A' : 'a') + digit - 10);
		magnitude /= hexadecimal ? 16 : 10;
	} while (magnitude);
	if (negative) prefix[prefix_length++] = '-';
	else if (is_signed && item->plus) prefix[prefix_length++] = '+';
	else if (is_signed && item->space) prefix[prefix_length++] = ' ';
	if (hexadecimal && item->alternate && raw) {
		prefix[0] = '0';
		prefix[1] = item->type == 'X' ? 'X' : 'x';
		prefix_length = 2;
	}
	int zeros = item->precision > count ? item->precision - count : 0;
	if (item->zero && !item->left && item->precision < 0 && item->width > prefix_length + count)
		zeros = item->width - prefix_length - count;
	const int pad = item->width - (prefix_length + zeros + count);
	if (!item->left)
		put_repeated(out, ' ', pad);
	put_narrow(out, prefix, prefix_length);
	put_repeated(out, '0', zeros);
	for (int at = count - 1; at >= 0; --at)
		put_narrow(out, digits + at, 1);
	if (item->left)
		put_repeated(out, ' ', pad);
}

static int format(struct output *out, const char *narrow, const WCHAR *wide, va_list *arguments)
{
	const BOOL ansi = narrow != NULL;
	size_t at = 0;
	for (;;) {
		const size_t start = at;
		while (pattern_at(narrow, wide, at) && pattern_at(narrow, wide, at) != '%')
			++at;
		if (narrow)
			put_narrow(out, narrow + start, (int)(at - start));
		else
			put_wide(out, wide + start, (int)(at - start));
		if (!pattern_at(narrow, wide, at))
			break;
		++at;
		if (pattern_at(narrow, wide, at) == '%') {
			put_narrow(out, "%", 1);
			++at;
			continue;
		}
		struct conversion item;
		if (!parse(narrow, wide, &at, &item))
			break;
		if (item.width_argument) {
			const int value = (int)va_arg(*arguments, ULONG_PTR);
			const INT64 magnitude = value < 0 ? -(INT64)value : value;
			item.left |= value < 0;
			item.width = magnitude < 1024 ? (int)magnitude : 1024;
		}
		if (item.precision_argument) {
			const int value = (int)va_arg(*arguments, ULONG_PTR);
			item.precision = value < 0 ? -1 : value < 1024 ? value : 1024;
		}
		const ULONG_PTR value = va_arg(*arguments, ULONG_PTR);
		if (item.type != 'c' && item.type != 'C' && item.type != 's' && item.type != 'S') {
			put_number(out, &item, value);
			continue;
		}
		BOOL wide_text = (item.type == 'c' || item.type == 's') != ansi;
		if (item.narrow_size)
			wide_text = FALSE;
		if (item.wide_size)
			wide_text = TRUE;
		const WCHAR wide_character = (WCHAR)value;
		const char narrow_character = (char)value;
		const void *text = item.type == 'c' || item.type == 'C' ? (wide_text ? (const void *)&wide_character : (const void *)&narrow_character) :
			value ? (const void *)value : NULL;
		int length = 0;
		if (!text) {
			text = "(null)";
			wide_text = FALSE;
			length = 6;
		} else if (item.type == 'c' || item.type == 'C') {
			length = 1;
		} else if (wide_text) {
			while ((item.precision < 0 || length < item.precision) && ((const WCHAR *)text)[length])
				++length;
		} else {
			while ((item.precision < 0 || length < item.precision) && ((const char *)text)[length])
				++length;
		}
		const int units = wide_text ? wide_units(out, text, length) : narrow_units(out, text, length);
		if (!item.left)
			put_repeated(out, ' ', item.width - units);
		if (wide_text)
			put_wide(out, text, length);
		else
			put_narrow(out, text, length);
		if (item.left)
			put_repeated(out, ' ', item.width - units);
	}
	if (out->narrow)
		out->narrow[out->used] = 0;
	else
		out->wide[out->used] = 0;
	return out->used;
}

static int format_narrow(char *buffer, int size, const char *pattern, va_list *arguments)
{
	if (size <= 0) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	struct output out = {buffer, NULL, size - 1, 0};
	return format(&out, pattern, NULL, arguments);
}

static int format_wide(WCHAR *buffer, int size, const WCHAR *pattern, va_list *arguments)
{
	if (size <= 0) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	struct output out = {NULL, buffer, size - 1, 0};
	return format(&out, NULL, pattern, arguments);
}

/* Each component carries its own library's entry points: raven-user32.dll user32's, raven-shell.dll, which calls user32's, shlwapi's. */
#ifndef RAVEN_FORMAT_SHLWAPI
int WINAPIV wsprintfW(LPWSTR buffer, LPCWSTR pattern, ...)
{
	va_list arguments;
	va_start(arguments, pattern);
	const int written = format_wide(buffer, 1024, pattern, &arguments);
	va_end(arguments);
	return written;
}

int WINAPIV wsprintfA(LPSTR buffer, LPCSTR pattern, ...)
{
	va_list arguments;
	va_start(arguments, pattern);
	const int written = format_narrow(buffer, 1024, pattern, &arguments);
	va_end(arguments);
	return written;
}

int WINAPI wvsprintfW(LPWSTR buffer, LPCWSTR pattern, va_list given)
{
	va_list arguments;
	va_copy(arguments, given);
	const int written = format_wide(buffer, 1024, pattern, &arguments);
	va_end(arguments);
	return written;
}

int WINAPI wvsprintfA(LPSTR buffer, LPCSTR pattern, va_list given)
{
	va_list arguments;
	va_copy(arguments, given);
	const int written = format_narrow(buffer, 1024, pattern, &arguments);
	va_end(arguments);
	return written;
}

#else
int WINAPIV wnsprintfW(LPWSTR buffer, int size, LPCWSTR pattern, ...)
{
	va_list arguments;
	va_start(arguments, pattern);
	const int written = format_wide(buffer, size, pattern, &arguments);
	va_end(arguments);
	return written;
}

int WINAPIV wnsprintfA(LPSTR buffer, int size, LPCSTR pattern, ...)
{
	va_list arguments;
	va_start(arguments, pattern);
	const int written = format_narrow(buffer, size, pattern, &arguments);
	va_end(arguments);
	return written;
}

int WINAPI wvnsprintfW(LPWSTR buffer, int size, LPCWSTR pattern, va_list given)
{
	va_list arguments;
	va_copy(arguments, given);
	const int written = format_wide(buffer, size, pattern, &arguments);
	va_end(arguments);
	return written;
}

int WINAPI wvnsprintfA(LPSTR buffer, int size, LPCSTR pattern, va_list given)
{
	va_list arguments;
	va_copy(arguments, given);
	const int written = format_narrow(buffer, size, pattern, &arguments);
	va_end(arguments);
	return written;
}
#endif
