/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <stdarg.h>
#include <stddef.h>

/* The small C library surface the component needs, so it does not import msvcrt.dll. */
#define PLAIN __attribute__((optimize("no-tree-loop-distribute-patterns")))

typedef unsigned short wchar;

PLAIN void *memcpy(void *destination, const void *source, size_t count)
{
	unsigned char *to = destination;
	const unsigned char *from = source;
	while (count--)
		*to++ = *from++;
	return destination;
}

PLAIN void *memmove(void *destination, const void *source, size_t count)
{
	unsigned char *to = destination;
	const unsigned char *from = source;
	if (to < from || to >= from + count) {
		while (count--)
			*to++ = *from++;
	} else {
		to += count;
		from += count;
		while (count--)
			*--to = *--from;
	}
	return destination;
}

PLAIN void *memset(void *destination, int value, size_t count)
{
	unsigned char *to = destination;
	while (count--)
		*to++ = (unsigned char)value;
	return destination;
}

size_t strlen(const char *text)
{
	size_t length = 0;
	while (text[length])
		++length;
	return length;
}

char *strcpy(char *destination, const char *source)
{
	char *start = destination;
	while ((*destination++ = *source++))
		;
	return start;
}

char *strcat(char *destination, const char *source)
{
	strcpy(destination + strlen(destination), source);
	return destination;
}

size_t wcslen(const wchar *text)
{
	size_t length = 0;
	while (text[length])
		++length;
	return length;
}

wchar *wcschr(const wchar *text, wchar character)
{
	for (;; ++text) {
		if (*text == character)
			return (wchar *)text;
		if (!*text)
			return 0;
	}
}

wchar *wcsrchr(const wchar *text, wchar character)
{
	const wchar *found = 0;
	for (;; ++text) {
		if (*text == character)
			found = text;
		if (!*text)
			return (wchar *)found;
	}
}

wchar *wcsstr(const wchar *text, const wchar *pattern)
{
	if (!*pattern)
		return (wchar *)text;
	for (; *text; ++text) {
		size_t index = 0;
		while (pattern[index] && text[index] == pattern[index])
			++index;
		if (!pattern[index])
			return (wchar *)text;
	}
	return 0;
}

size_t wcscspn(const wchar *text, const wchar *reject)
{
	size_t length = 0;
	for (; text[length]; ++length)
		for (const wchar *item = reject; *item; ++item)
			if (*item == text[length])
				return length;
	return length;
}

int wcscmp(const wchar *left, const wchar *right)
{
	while (*left && *left == *right) {
		++left;
		++right;
	}
	return (int)*left - (int)*right;
}

int wcsncmp(const wchar *left, const wchar *right, size_t count)
{
	while (count && *left && *left == *right) {
		++left;
		++right;
		--count;
	}
	return count ? (int)*left - (int)*right : 0;
}

wchar towupper(wchar character)
{
	if (character >= 'a' && character <= 'z')
		return character - 32;
	if ((character >= 0xe0 && character <= 0xfe && character != 0xf7) )
		return character - 32;
	if (character >= 0x3b1 && character <= 0x3c9 && character != 0x3c2)
		return character - 32;
	if (character >= 0x430 && character <= 0x44f)
		return character - 32;
	return character;
}

int _wcsicmp(const wchar *left, const wchar *right)
{
	while (*left && towupper(*left) == towupper(*right)) {
		++left;
		++right;
	}
	return (int)towupper(*left) - (int)towupper(*right);
}

int _wcsnicmp(const wchar *left, const wchar *right, size_t count)
{
	while (count && *left && towupper(*left) == towupper(*right)) {
		++left;
		++right;
		--count;
	}
	return count ? (int)towupper(*left) - (int)towupper(*right) : 0;
}

long wcstol(const wchar *text, wchar **end, int base)
{
	const wchar *cursor = text;
	long value = 0;
	int negative = 0;
	while (*cursor == ' ' || (*cursor >= 9 && *cursor <= 13))
		++cursor;
	if (*cursor == '-' || *cursor == '+')
		negative = *cursor++ == '-';
	if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
		cursor += 2;
		base = 16;
	} else if (base == 0) {
		base = cursor[0] == '0' ? 8 : 10;
	}
	for (;; ++cursor) {
		int digit = *cursor >= '0' && *cursor <= '9' ? *cursor - '0' :
			*cursor >= 'a' && *cursor <= 'z' ? *cursor - 'a' + 10 : *cursor >= 'A' && *cursor <= 'Z' ? *cursor - 'A' + 10 : 99;
		if (digit >= base)
			break;
		value = value * base + digit;
	}
	if (end)
		*end = (wchar *)cursor;
	return negative ? -value : value;
}

int atoi(const char *text)
{
	int value = 0, negative = 0;
	while (*text == ' ' || (*text >= 9 && *text <= 13))
		++text;
	if (*text == '-' || *text == '+')
		negative = *text++ == '-';
	while (*text >= '0' && *text <= '9')
		value = value * 10 + (*text++ - '0');
	return negative ? -value : value;
}

int isalnum(int character)
{
	return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

/* Classification masks follow the C library: upper 1, lower 2, digit 4, space 8, punct 0x10, control 0x20, blank 0x40, hex 0x80, alpha 0x100. */
int iswctype(wchar character, wchar mask)
{
	int flags = 0;
	if (character >= 'A' && character <= 'Z')
		flags |= 1 | 0x100;
	if (character >= 'a' && character <= 'z')
		flags |= 2 | 0x100;
	if (character >= 0xc0 && character != 0xd7 && character != 0xf7)
		flags |= 0x100 | (character >= 0xe0 ? 2 : 1);
	if (character >= '0' && character <= '9')
		flags |= 4 | 0x80;
	if ((character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F'))
		flags |= 0x80;
	if (character == ' ' || (character >= 9 && character <= 13))
		flags |= 8;
	if (character == ' ' || character == 9)
		flags |= 0x40;
	if (character < 32 || character == 127)
		flags |= 0x20;
	if (character > 32 && character < 127 && !isalnum(character))
		flags |= 0x10;
	return flags & mask;
}

void qsort(void *base, size_t count, size_t width, int (__cdecl *compare)(const void *, const void *))
{
	/* Insertion sort keeps the routine small; the component sorts short arrays (tick marks, ranges). */
	unsigned char *items = base;
	unsigned char scratch[256];
	if (width > sizeof scratch)
		return;
	for (size_t index = 1; index < count; ++index) {
		memcpy(scratch, items + index * width, width);
		size_t cursor = index;
		while (cursor > 0 && compare(items + (cursor - 1) * width, scratch) > 0) {
			memcpy(items + cursor * width, items + (cursor - 1) * width, width);
			--cursor;
		}
		memcpy(items + cursor * width, scratch, width);
	}
}

static int format_text(char *buffer, size_t size, const char *format, va_list arguments)
{
	size_t length = 0;
#define EMIT(character) do { if (length + 1 < size) buffer[length] = (character); ++length; } while (0)
	for (; *format; ++format) {
		if (*format != '%') {
			EMIT(*format);
			continue;
		}
		++format;
		int longs = 0;
		while (*format == 'l' || *format == 'I' || *format == '6' || *format == '4' || *format == 'z') {
			longs += *format == 'l' || *format == 'I' || *format == 'z';
			++format;
		}
		if (*format == '%') {
			EMIT('%');
		} else if (*format == 'c') {
			EMIT((char)va_arg(arguments, int));
		} else if (*format == 's') {
			const char *text = va_arg(arguments, const char *);
			if (!text)
				text = "(null)";
			while (*text)
				EMIT(*text++);
		} else if (*format == 'd' || *format == 'i' || *format == 'u' || *format == 'x' || *format == 'X' || *format == 'p') {
			unsigned long long value;
			int negative = 0, radix = *format == 'x' || *format == 'X' || *format == 'p' ? 16 : 10;
			if (*format == 'd' || *format == 'i') {
				long long signed_value = longs ? va_arg(arguments, long long) : va_arg(arguments, int);
				negative = signed_value < 0;
				value = negative ? -(unsigned long long)signed_value : (unsigned long long)signed_value;
			} else {
				value = longs || *format == 'p' ? va_arg(arguments, unsigned long long) : va_arg(arguments, unsigned int);
			}
			char digits[24];
			int count = 0;
			do {
				digits[count++] = "0123456789abcdef"[value % radix];
				value /= radix;
			} while (value);
			if (negative)
				EMIT('-');
			while (count)
				EMIT(digits[--count]);
		}
	}
	if (size)
		buffer[length < size ? length : size - 1] = 0;
	return (int)length;
}

int _snprintf(char *buffer, size_t size, const char *format, ...)
{
	va_list arguments;
	int length;
	va_start(arguments, format);
	length = format_text(buffer, size, format, arguments);
	va_end(arguments);
	return length;
}

void __cdecl _assert(const char *expression, const char *file, unsigned line)
{
	__builtin_trap();
}

double floor(double value)
{
	if (value != value || value >= 4503599627370496.0 || value <= -4503599627370496.0)
		return value;
	double whole = (double)(long long)value;
	return whole > value ? whole - 1.0 : whole;
}

int wcsicmp(const wchar *left, const wchar *right)
{
	return _wcsicmp(left, right);
}

wchar *wcscpy(wchar *destination, const wchar *source)
{
	wchar *start = destination;
	while ((*destination++ = *source++))
		;
	return start;
}

wchar *wcscat(wchar *destination, const wchar *source)
{
	wcscpy(destination + wcslen(destination), source);
	return destination;
}

wchar towlower(wchar character)
{
	if (character >= 'A' && character <= 'Z')
		return character + 32;
	if (character >= 0xc0 && character <= 0xde && character != 0xd7)
		return character + 32;
	if (character >= 0x391 && character <= 0x3a9)
		return character + 32;
	if (character >= 0x410 && character <= 0x42f)
		return character + 32;
	return character;
}

wchar *_wcsupr(wchar *text)
{
	for (wchar *cursor = text; *cursor; ++cursor)
		*cursor = towupper(*cursor);
	return text;
}

wchar *_wcslwr(wchar *text)
{
	for (wchar *cursor = text; *cursor; ++cursor)
		*cursor = towlower(*cursor);
	return text;
}

unsigned long strtoul(const char *text, char **end, int base)
{
	unsigned long value = 0;
	if ((base == 0 || base == 16) && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		text += 2;
		base = 16;
	} else if (base == 0) {
		base = text[0] == '0' ? 8 : 10;
	}
	for (;; ++text) {
		int digit = *text >= '0' && *text <= '9' ? *text - '0' :
			*text >= 'a' && *text <= 'z' ? *text - 'a' + 10 : *text >= 'A' && *text <= 'Z' ? *text - 'A' + 10 : 99;
		if (digit >= base)
			break;
		value = value * base + digit;
	}
	if (end)
		*end = (char *)text;
	return value;
}

long strtol(const char *text, char **end, int base)
{
	int negative = 0;
	while (*text == ' ' || (*text >= 9 && *text <= 13))
		++text;
	if (*text == '-' || *text == '+')
		negative = *text++ == '-';
	long value = (long)strtoul(text, end, base);
	return negative ? -value : value;
}

int sprintf(char *buffer, const char *format, ...)
{
	va_list arguments;
	int length;
	va_start(arguments, format);
	length = format_text(buffer, 1 << 20, format, arguments);
	va_end(arguments);
	return length;
}
