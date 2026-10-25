/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The older msvcrt.dll entry points that programs import next to the Universal CRT ones: bounded formatting with the
 * historic truncation rules, wide scanning, wide character classes and the C++ allocation operators. Included by ucrtbase.c. */

#define SCAN_END (-1)

CRT int iswalnum(wint_t c) { return iswdigit(c) || iswalpha(c); }
CRT int iswcntrl(wint_t c) { return c < 32 || (c >= 0x7f && c < 0xa0); }
CRT int iswupper(wint_t c) { return towlower(c) != c; }
CRT int iswlower(wint_t c) { return towupper(c) != c; }
CRT int iswxdigit(wint_t c) { return iswdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
CRT int iswpunct(wint_t c) { return c > 32 && c < 127 && !iswalnum(c); }

CRT int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t count)
{
	while (count--) {
		wint_t x = towlower(*a), y = towlower(*b);
		if (x != y)
			return x < y ? -1 : 1;
		if (!x)
			return 0;
		++a;
		++b;
	}
	return 0;
}

CRT wchar_t *_wcslwr(wchar_t *text)
{
	for (wchar_t *at = text; *at; ++at)
		*at = towlower(*at);
	return text;
}

CRT char *_strlwr(char *text)
{
	for (char *at = text; *at; ++at)
		if (*at >= 'A' && *at <= 'Z')
			*at = (char)(*at + 32);
	return text;
}

CRT char *_strupr(char *text)
{
	for (char *at = text; *at; ++at)
		if (*at >= 'a' && *at <= 'z')
			*at = (char)(*at - 32);
	return text;
}

CRT wchar_t *_wcsupr(wchar_t *text)
{
	for (wchar_t *at = text; *at; ++at)
		*at = towupper(*at);
	return text;
}

CRT wchar_t *_wcsdup(const wchar_t *text)
{
	if (!text)
		return NULL;
	size_t size = (wcslen(text) + 1) * sizeof(wchar_t);
	wchar_t *copy = malloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

/* The C library's own generator, as msvcrt has it: a linear congruence whose high bits give 15-bit numbers, seeded per program. */
static unsigned long random_state = 1;
CRT void srand(unsigned int seed) { random_state = seed; }
CRT int rand(void)
{
	random_state = random_state * 214013 + 2531011;
	return (int)((random_state >> 16) & 0x7fff);
}

CRT void *bsearch(const void *key, const void *base, size_t count, size_t size, int (__cdecl *compare)(const void *, const void *))
{
	const char *low = base;
	while (count) {
		const char *middle = low + (count / 2) * size;
		const int order = compare(key, middle);
		if (!order)
			return (void *)middle;
		if (order > 0) {
			low = middle + size;
			count -= count / 2 + 1;
		} else
			count /= 2;
	}
	return NULL;
}

/* The count of characters a formatted string takes, with the historic rule: when the text does not fit, no terminator is
 * written and the result is -1. */
CRT int _vsnprintf(char *buffer, size_t count, const char *text, va_list arguments)
{
	va_list again;
	int length;
	char *whole;
	va_copy(again, arguments);
	length = __stdio_common_vsprintf(0, buffer, count, text, NULL, arguments);
	if (length >= 0 && (size_t)length < count) {
		va_end(again);
		return length;
	}
	/* Truncated: the buffer holds count - 1 characters and a terminator, but the last place must hold a character. */
	if (length >= 0 && count) {
		whole = malloc((size_t)length + 1);
		if (whole) {
			__stdio_common_vsprintf(0, whole, (size_t)length + 1, text, NULL, again);
			memcpy(buffer, whole, count);
			free(whole);
		}
	}
	va_end(again);
	return -1;
}

CRT int _snprintf(char *buffer, size_t count, const char *text, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, text);
	result = _vsnprintf(buffer, count, text, arguments);
	va_end(arguments);
	return result;
}

/* The unbounded form: the caller sized the buffer. */
CRT int sprintf(char *buffer, const char *text, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, text);
	result = __stdio_common_vsprintf(0, buffer, (size_t)-1, text, NULL, arguments);
	va_end(arguments);
	return result;
}

CRT int _vsnwprintf(wchar_t *buffer, size_t count, const wchar_t *text, va_list arguments)
{
	va_list again;
	int length;
	wchar_t *whole;
	va_copy(again, arguments);
	length = __stdio_common_vswprintf(0, buffer, count, text, NULL, arguments);
	if (length >= 0 && (size_t)length < count) {
		va_end(again);
		return length;
	}
	if (length >= 0 && count) {
		whole = malloc(((size_t)length + 1) * sizeof(wchar_t));
		if (whole) {
			__stdio_common_vswprintf(0, whole, (size_t)length + 1, text, NULL, again);
			memcpy(buffer, whole, count * sizeof(wchar_t));
			free(whole);
		}
	}
	va_end(again);
	return -1;
}

CRT int _snwprintf(wchar_t *buffer, size_t count, const wchar_t *text, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, text);
	result = _vsnwprintf(buffer, count, text, arguments);
	va_end(arguments);
	return result;
}

/* msvcrt's own swprintf takes no count: the caller sized the buffer. */
CRT int swprintf(wchar_t *buffer, const wchar_t *text, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, text);
	result = __stdio_common_vswprintf(0, buffer, (size_t)-1 / sizeof(wchar_t), text, NULL, arguments);
	va_end(arguments);
	return result;
}

CRT int vfwprintf(void *stream, const wchar_t *text, va_list arguments) { return __stdio_common_vfwprintf(0, stream, text, NULL, arguments); }

CRT int fwprintf(void *stream, const wchar_t *text, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, text);
	result = vfwprintf(stream, text, arguments);
	va_end(arguments);
	return result;
}

CRT int wprintf(const wchar_t *text, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, text);
	result = __stdio_common_vfwprintf(0, __acrt_iob_func(1), text, NULL, arguments);
	va_end(arguments);
	return result;
}

/* ---- wide scanning ---- */

static int scan_digit(wchar_t c, int base)
{
	int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
	return value < base ? value : -1;
}

/* Scans wide text; with narrow set, %s, %c and %[ without a size store narrow strings, as sscanf's do. */
static int scan_wide(const wchar_t *input, const wchar_t *format, va_list arguments, int narrow_strings)
{
	const wchar_t *start = input;
	int assigned = 0, converted_any = 0;
	while (*format) {
		wchar_t c = *format++;
		int suppress = 0, width = 0, size = 0;
		if (iswspace(c)) {
			while (iswspace(*input))
				++input;
			continue;
		}
		if (c != '%') {
			if (*input != c)
				return assigned || converted_any ? assigned : (*input ? assigned : SCAN_END);
			++input;
			continue;
		}
		c = *format++;
		if (c == '%') {
			while (iswspace(*input))
				++input;
			if (*input != '%')
				return assigned;
			++input;
			continue;
		}
		if (c == '*') {
			suppress = 1;
			c = *format++;
		}
		while (c >= '0' && c <= '9') {
			width = width * 10 + (c - '0');
			c = *format++;
		}
		if (c == 'h') {
			size = -1;
			c = *format++;
		} else if (c == 'l') {
			size = 1;
			c = *format++;
			if (c == 'l') {
				size = 2;
				c = *format++;
			}
		} else if (c == 'I' && format[0] == '6' && format[1] == '4') {
			size = 2;
			format += 2;
			c = *format++;
		}
		if (c == 'n') {
			if (!suppress)
				*va_arg(arguments, int *) = (int)(input - start);
			continue;
		}
		if (c != 'c' && c != '[')
			while (iswspace(*input))
				++input;
		if (!*input)
			return assigned || converted_any ? assigned : SCAN_END;
		if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o') {
			int base = c == 'x' || c == 'X' ? 16 : c == 'o' ? 8 : c == 'i' ? 0 : 10, negative = 0, digits = 0, remaining = width ? width : 1 << 30;
			unsigned long long value = 0;
			if (*input == '-' || *input == '+') {
				negative = *input == '-';
				++input;
				--remaining;
			}
			if ((base == 0 || base == 16) && remaining > 1 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X') && scan_digit(input[2], 16) >= 0) {
				input += 2;
				remaining -= 2;
				base = 16;
			} else if (base == 0) {
				base = input[0] == '0' ? 8 : 10;
			}
			while (remaining > 0 && scan_digit(*input, base) >= 0) {
				value = value * (unsigned)base + (unsigned)scan_digit(*input, base);
				++input;
				--remaining;
				++digits;
			}
			if (!digits)
				return assigned;
			if (negative)
				value = (unsigned long long)(-(long long)value);
			if (!suppress) {
				if (size == 2)
					*va_arg(arguments, long long *) = (long long)value;
				else if (size == -1)
					*va_arg(arguments, short *) = (short)value;
				else
					*va_arg(arguments, int *) = (int)value;
				++assigned;
			}
			converted_any = 1;
		} else if (c == 'e' || c == 'f' || c == 'g' || c == 'E' || c == 'G') {
			/* A decimal number with an optional exponent: digits scaled by powers of ten. */
			int remaining = width ? width : 1 << 30, negative = 0, digits = 0, exponent = 0, exponent_negative = 0;
			double value = 0, scale = 1;
			if (remaining > 0 && (*input == '-' || *input == '+')) {
				negative = *input++ == '-';
				--remaining;
			}
			while (remaining > 0 && *input >= '0' && *input <= '9') {
				value = value * 10 + (*input++ - '0');
				--remaining;
				++digits;
			}
			if (remaining > 0 && *input == '.') {
				++input;
				--remaining;
				while (remaining > 0 && *input >= '0' && *input <= '9') {
					scale /= 10;
					value += (*input++ - '0') * scale;
					--remaining;
					++digits;
				}
			}
			if (!digits)
				return assigned;
			if (remaining > 1 && (*input == 'e' || *input == 'E') && ((input[1] >= '0' && input[1] <= '9') || input[1] == '-' || input[1] == '+')) {
				++input;
				--remaining;
				if (*input == '-' || *input == '+') {
					exponent_negative = *input++ == '-';
					--remaining;
				}
				while (remaining > 0 && *input >= '0' && *input <= '9') {
					exponent = exponent * 10 + (*input++ - '0');
					--remaining;
				}
				while (exponent-- > 0)
					value = exponent_negative ? value / 10 : value * 10;
			}
			if (negative)
				value = -value;
			if (!suppress) {
				if (size > 0)
					*va_arg(arguments, double *) = value;
				else
					*va_arg(arguments, float *) = (float)value;
				++assigned;
			}
			converted_any = 1;
		} else if (c == 'p') {
			unsigned long long value = 0;
			int digits = 0, digit;
			if (input[0] == '0' && (input[1] == 'x' || input[1] == 'X'))
				input += 2;
			while ((digit = scan_digit(*input, 16)) >= 0) {
				value = value * 16 + (unsigned)digit;
				++input;
				++digits;
			}
			if (!digits)
				return assigned;
			if (!suppress) {
				*va_arg(arguments, void **) = (void *)(uintptr_t)value;
				++assigned;
			}
			converted_any = 1;
		} else if (c == 's') {
			int remaining = width ? width : 1 << 30;
			const int narrow_target = size < 0 || (narrow_strings && size == 0);
			wchar_t *wide = !suppress && !narrow_target ? va_arg(arguments, wchar_t *) : NULL;
			char *narrow = !suppress && narrow_target ? va_arg(arguments, char *) : NULL;
			size_t written = 0;
			while (remaining-- > 0 && *input && !iswspace(*input)) {
				if (wide)
					wide[written++] = *input;
				else if (narrow)
					narrow[written++] = *input < 256 ? (char)*input : '?';
				++input;
			}
			if (wide)
				wide[written] = 0;
			else if (narrow)
				narrow[written] = 0;
			if (!suppress)
				++assigned;
			converted_any = 1;
		} else if (c == 'c') {
			int remaining = width ? width : 1;
			const int narrow_target = size < 0 || (narrow_strings && size == 0);
			wchar_t *wide = !suppress && !narrow_target ? va_arg(arguments, wchar_t *) : NULL;
			char *narrow = !suppress && narrow_target ? va_arg(arguments, char *) : NULL;
			size_t written = 0;
			while (remaining-- > 0 && *input) {
				if (wide)
					wide[written++] = *input;
				else if (narrow)
					narrow[written++] = *input < 256 ? (char)*input : '?';
				++input;
			}
			if (!written)
				return assigned;
			if (!suppress)
				++assigned;
			converted_any = 1;
		} else if (c == '[') {
			int invert = 0, remaining = width ? width : 1 << 30;
			const wchar_t *set, *end;
			const int narrow_target = size < 0 || (narrow_strings && size == 0);
			wchar_t *wide = !suppress && !narrow_target ? va_arg(arguments, wchar_t *) : NULL;
			char *narrow = !suppress && narrow_target ? va_arg(arguments, char *) : NULL;
			size_t written = 0;
			if (*format == '^') {
				invert = 1;
				++format;
			}
			set = format;
			if (*format == ']')
				++format;
			while (*format && *format != ']')
				++format;
			end = format;
			if (*format == ']')
				++format;
			while (remaining-- > 0 && *input) {
				int member = 0;
				const wchar_t *scan;
				for (scan = set; scan < end; ++scan) {
					if (scan + 2 < end && scan[1] == '-') {
						if (*input >= scan[0] && *input <= scan[2])
							member = 1;
						scan += 2;
					} else if (*scan == *input) {
						member = 1;
					}
				}
				if (member == invert)
					break;
				if (wide)
					wide[written++] = *input;
				else if (narrow)
					narrow[written++] = *input < 256 ? (char)*input : '?';
				++input;
			}
			if (!written)
				return assigned;
			if (wide)
				wide[written] = 0;
			else if (narrow)
				narrow[written] = 0;
			if (!suppress)
				++assigned;
			converted_any = 1;
		} else {
			return assigned;
		}
	}
	return assigned;
}

CRT int vswscanf(const wchar_t *input, const wchar_t *format, va_list arguments) { return scan_wide(input, format, arguments, 0); }

CRT int swscanf(const wchar_t *input, const wchar_t *format, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, format);
	result = scan_wide(input, format, arguments, 0);
	va_end(arguments);
	return result;
}

/* ---- narrow scanning ---- */

/* sscanf reads through the wide scanner: the text and the format widen, narrow strings come back narrow. */
CRT int vsscanf(const char *input, const char *format, va_list arguments)
{
	size_t input_length = strlen(input), format_length = strlen(format);
	wchar_t *wide_input = malloc((input_length + 1) * sizeof(wchar_t)), *wide_format = malloc((format_length + 1) * sizeof(wchar_t));
	int result = SCAN_END;
	if (wide_input && wide_format) {
		for (size_t at = 0; at <= input_length; ++at)
			wide_input[at] = (unsigned char)input[at];
		for (size_t at = 0; at <= format_length; ++at)
			wide_format[at] = (unsigned char)format[at];
		result = scan_wide(wide_input, wide_format, arguments, 1);
	}
	free(wide_input);
	free(wide_format);
	return result;
}

CRT int sscanf(const char *input, const char *format, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, format);
	result = vsscanf(input, format, arguments);
	va_end(arguments);
	return result;
}

CRT int vsprintf(char *buffer, const char *text, va_list arguments)
{
	return __stdio_common_vsprintf(0, buffer, (size_t)-1, text, NULL, arguments);
}

/* The narrow number reader, through the wide one. */
__asm__(".section .drectve\n\t.ascii \" -export:strtod=raven_strtod\"\n\t.text");
double raven_wcstod(const wchar_t *text, wchar_t **end);
double raven_strtod(const char *text, char **end)
{
	wchar_t wide[128];
	size_t length = 0;
	wchar_t *wide_end;
	while (text[length] && length < 127) {
		wide[length] = (unsigned char)text[length];
		++length;
	}
	wide[length] = 0;
	double value = raven_wcstod(wide, &wide_end);
	if (end)
		*end = (char *)text + (wide_end - wide);
	return value;
}

CRT double atof(const char *text) { return raven_strtod(text, NULL); }

/* ---- calendar time ---- */

/* The UTC calendar fields of a time in seconds since 1970, in msvcrt's struct tm; the result lives in one buffer, as msvcrt's does. */
typedef struct {
	int second, minute, hour, day, month, year, weekday, yearday, daylight;
} RavenTime;

CRT RavenTime *_gmtime64(const long long *time)
{
	static RavenTime result;
	if (!time || *time < 0)
		return NULL;
	long long days = *time / 86400, rest = *time % 86400;
	result.hour = (int)(rest / 3600);
	result.minute = (int)(rest / 60 % 60);
	result.second = (int)(rest % 60);
	result.weekday = (int)((days + 4) % 7);
	/* Days to the civil date, counting eras of 400 years from 0000-03-01. */
	long long z = days + 719468, era = z / 146097, offset = z - era * 146097;
	long long year_of_era = (offset - offset / 1460 + offset / 36524 - offset / 146096) / 365;
	long long day_of_year = offset - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	long long month_index = (5 * day_of_year + 2) / 153;
	int day = (int)(day_of_year - (153 * month_index + 2) / 5 + 1), month = (int)(month_index < 10 ? month_index + 3 : month_index - 9);
	long long year = year_of_era + era * 400 + (month <= 2);
	static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	const int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	result.day = day;
	result.month = month - 1;
	result.year = (int)(year - 1900);
	result.yearday = before[month - 1] + day - 1 + (leap && month > 2);
	result.daylight = 0;
	return &result;
}

/* ---- floating point classes ---- */

CRT int _isnan(double x) { return x != x; }
CRT int _finite(double x) { return x == x && x - x == 0; }

/* The class of a value, as the _FPCLASS_ bits name it: signalling or quiet NaN, then infinite, normal, denormal or zero by sign. */
CRT int _fpclass(double x)
{
	union { double d; unsigned long long bits; } value = {x};
	const int negative = (int)(value.bits >> 63);
	const unsigned exponent = (unsigned)(value.bits >> 52) & 0x7ff;
	const unsigned long long fraction = value.bits & 0xfffffffffffffULL;
	if (exponent == 0x7ff)
		return fraction ? ((fraction >> 51) ? 0x2 : 0x1) : (negative ? 0x4 : 0x200);
	if (exponent)
		return negative ? 0x8 : 0x100;
	if (fraction)
		return negative ? 0x10 : 0x80;
	return negative ? 0x20 : 0x40;
}

/* The exception flags the floating point unit has raised, as _SW_ bits, which reading them clears: the SSE unit's on 64 bits, the x87's on
 * 32 bits. */
CRT unsigned int _clearfp(void)
{
	unsigned int raised;
#ifdef __x86_64__
	unsigned int state;
	__asm__ volatile ("stmxcsr %0" : "=m"(state));
	raised = state & 0x3f;
	state &= ~0x3fU;
	__asm__ volatile ("ldmxcsr %0" : : "m"(state));
#else
	unsigned short status;
	__asm__ volatile ("fnstsw %0\n\tfnclex" : "=m"(status));
	raised = status & 0x3f;
#endif
	return ((raised & 0x01) ? 0x10 : 0) | ((raised & 0x02) ? 0x80000 : 0) | ((raised & 0x04) ? 0x08 : 0) | ((raised & 0x08) ? 0x04 : 0) |
		((raised & 0x10) ? 0x02 : 0) | ((raised & 0x20) ? 0x01 : 0);
}

CRT unsigned int _control87(unsigned int value, unsigned int mask) { return _controlfp(value, mask); }

/* ---- the older conversions and helpers of msvcrt ---- */

/* An unsigned value in a radix from 2 to 36, with a minus sign when asked. */
static char *digits_of(unsigned long long value, int negative, char *to, int radix)
{
	char reversed[72];
	int count = 0, at = 0;
	if (radix < 2 || radix > 36) {
		*to = 0;
		return to;
	}
	do {
		const int digit = (int)(value % (unsigned)radix);
		reversed[count++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
		value /= (unsigned)radix;
	} while (value);
	if (negative)
		to[at++] = '-';
	while (count)
		to[at++] = reversed[--count];
	to[at] = 0;
	return to;
}

/* A negative number is signed in base ten only; in other bases its bits are written as they are. */
CRT char *_itoa(int value, char *to, int radix)
{
	return radix == 10 && value < 0 ? digits_of(0ULL - (unsigned long long)(long long)value, 1, to, radix) :
		digits_of(radix == 10 ? (unsigned long long)value : (unsigned int)value, 0, to, radix);
}

CRT char *_ultoa(unsigned long value, char *to, int radix) { return digits_of(value, 0, to, radix); }
CRT unsigned long long _strtoui64(const char *text, char **end, int base) { return strtoull(text, end, base); }

CRT int isleadbyte(int character) { return IsDBCSLeadByte((BYTE)character); }

/* The characters of a multibyte string, a lead byte and its trail counting as one. */
CRT size_t _mbstrlen(const char *text)
{
	size_t count = 0;
	for (; *text; ++count)
		text += IsDBCSLeadByte((BYTE)*text) && text[1] ? 2 : 1;
	return count;
}

/* The character before `current`, found by walking from the start, since a trail byte can look like a lead byte. */
CRT unsigned char *_mbsdec(const unsigned char *start, const unsigned char *current)
{
	const unsigned char *at = start, *previous = NULL;
	if (current <= start)
		return NULL;
	while (at < current) {
		previous = at;
		at += IsDBCSLeadByte(*at) && at[1] ? 2 : 1;
	}
	return (unsigned char *)previous;
}

CRT int wctomb(char *to, wchar_t character)
{
	BOOL lost = FALSE;
	int length;
	if (!to)
		return 0;
	length = WideCharToMultiByte(CP_ACP, 0, &character, 1, to, 5, NULL, &lost);
	if (!length || lost) {
		*_errno() = 42;
		return -1;
	}
	return length;
}

/* Up to `count` bytes of the ANSI form of a wide string; without a buffer, the bytes the whole of it takes. */
CRT size_t wcstombs(char *to, const wchar_t *text, size_t count)
{
	BOOL lost = FALSE;
	const int needed = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, &lost);
	char *whole;
	if (!needed || lost) {
		*_errno() = 42;
		return (size_t)-1;
	}
	if (!to)
		return (size_t)needed - 1;
	whole = malloc((size_t)needed);
	if (!whole)
		return (size_t)-1;
	WideCharToMultiByte(CP_ACP, 0, text, -1, whole, needed, NULL, NULL);
	if (count > (size_t)needed - 1) {
		memcpy(to, whole, (size_t)needed);
		count = (size_t)needed - 1;
	} else {
		memcpy(to, whole, count);
	}
	free(whole);
	return count;
}

CRT int _vscprintf(const char *text, va_list arguments) { return __stdio_common_vsprintf(0, NULL, 0, text, NULL, arguments); }

/* The local time as hh:mm:ss, and the local date as mm/dd/yy. */
CRT char *_strtime(char *to)
{
	SYSTEMTIME now;
	GetLocalTime(&now);
	sprintf(to, "%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
	return to;
}

CRT char *_strdate(char *to)
{
	SYSTEMTIME now;
	GetLocalTime(&now);
	sprintf(to, "%02u/%02u/%02u", now.wMonth, now.wDay, now.wYear % 100);
	return to;
}

/* A name for a temporary file that does not exist yet, in the folder TMP names, else in the one given, else the system's temporary folder. */
CRT char *_tempnam(const char *folder, const char *prefix)
{
	char base[MAX_PATH];
	DWORD length = GetEnvironmentVariableA("TMP", base, MAX_PATH);
	if (!length || length >= MAX_PATH || GetFileAttributesA(base) == INVALID_FILE_ATTRIBUTES) {
		if (folder && GetFileAttributesA(folder) != INVALID_FILE_ATTRIBUTES)
			lstrcpynA(base, folder, MAX_PATH);
		else if (!GetTempPathA(MAX_PATH, base))
			return NULL;
	}
	length = (DWORD)strlen(base);
	if (length && base[length - 1] != '\\' && base[length - 1] != '/')
		base[length++] = '\\', base[length] = 0;
	for (unsigned number = GetTickCount() & 0xffff, tries = 0; tries < 65536; ++number, ++tries) {
		char *name = malloc(length + (prefix ? strlen(prefix) : 0) + 16);
		if (!name)
			return NULL;
		sprintf(name, "%s%s%x", base, prefix ? prefix : "", number & 0xffff);
		if (GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES)
			return name;
		free(name);
	}
	return NULL;
}

CRT char *_mktemp(char *name)
{
	const size_t length = name ? strlen(name) : 0;
	if (length < 6 || memcmp(name + length - 6, "XXXXXX", 6)) {
		*_errno() = 22;
		return NULL;
	}
	char *suffix = name + length - 6;
	sprintf(suffix + 1, "%05lu", (unsigned long)(GetCurrentThreadId() % 100000));
	for (char letter = 'a'; letter <= 'z'; ++letter) {
		suffix[0] = letter;
		if (GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES)
			return name;
	}
	name[0] = 0;
	*_errno() = 17;
	return NULL;
}

/* A descriptor is a terminal when the handle behind it is a character device. */
CRT int _isatty(int fd)
{
	const intptr_t handle = _get_osfhandle(fd);
	return handle != -1 && GetFileType((HANDLE)handle) == FILE_TYPE_CHAR ? 0x40 : 0;
}

/* There is no command interpreter: system says so for a null command, and fails for any other. */
CRT int system(const char *command)
{
	if (!command)
		return 0;
	*_errno() = 2;
	return -1;
}

/* The low-level I/O table msvcrt's own inline code reads: every slot names a descriptor that is not open. */
static unsigned char closed_descriptors[4096];
void *raven_pioinfo[64] = {
#define CLOSED closed_descriptors, closed_descriptors, closed_descriptors, closed_descriptors, closed_descriptors, closed_descriptors, \
	closed_descriptors, closed_descriptors
	CLOSED, CLOSED, CLOSED, CLOSED, CLOSED, CLOSED, CLOSED, CLOSED
#undef CLOSED
};
struct { intptr_t handle; char flags, pipe; } raven_badioinfo = {-1, 0, 10};
__asm__(".section .drectve\n\t.ascii \" -export:__pioinfo=raven_pioinfo,DATA -export:__badioinfo=raven_badioinfo,DATA\"\n\t.text");

/* The filter msvcrt's startup code runs exceptions through: without signal handlers installed, every exception goes on to the next frame. */
CRT int _XcptFilter(unsigned long code, EXCEPTION_POINTERS *pointers) { return EXCEPTION_CONTINUE_SEARCH; }

/* ---- the file system, as msvcrt's POSIX names see it ---- */

CRT int _access(const char *path, int mode)
{
	DWORD attributes = GetFileAttributesA(path);
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		*_errno() = 2;
		return -1;
	}
	if ((mode & 2) && (attributes & FILE_ATTRIBUTE_READONLY) && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		*_errno() = 13;
		return -1;
	}
	return 0;
}

CRT int _mkdir(const char *path)
{
	if (CreateDirectoryA(path, NULL))
		return 0;
	*_errno() = GetLastError() == ERROR_ALREADY_EXISTS ? 17 : 2;
	return -1;
}

CRT char *_getcwd(char *buffer, int size)
{
	DWORD length = GetCurrentDirectoryA(0, NULL);
	if (!length)
		return NULL;
	if (!buffer) {
		if ((DWORD)size < length)
			size = (int)length;
		buffer = malloc((size_t)size);
		if (!buffer)
			return NULL;
	} else if ((DWORD)size < length) {
		*_errno() = 34;
		return NULL;
	}
	GetCurrentDirectoryA((DWORD)size, buffer);
	return buffer;
}

/* msvcrt's struct _stat: 32-bit times on 32 bits, 64-bit ones on 64 bits. */
typedef struct {
	unsigned int dev;
	unsigned short ino, mode;
	short nlink, uid, gid;
	unsigned int rdev;
	long size;
#ifdef _WIN64
	long long access_time, modify_time, change_time;
#else
	long access_time, modify_time, change_time;
#endif
} RavenStat;

static long long unix_seconds(FILETIME time)
{
	unsigned long long ticks = ((unsigned long long)time.dwHighDateTime << 32) | time.dwLowDateTime;
	return (long long)(ticks / 10000000ULL) - 11644473600LL;
}

static int fill_stat(const WIN32_FILE_ATTRIBUTE_DATA *data, RavenStat *out)
{
	memset(out, 0, sizeof *out);
	/* Directories and files, readable by everyone, writable unless marked read-only. */
	out->mode = (unsigned short)(((data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x4000 | 0111 : 0x8000) | 0444 |
		((data->dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0 : 0222));
	out->nlink = 1;
	out->size = (long)data->nFileSizeLow;
	out->access_time = unix_seconds(data->ftLastAccessTime);
	out->modify_time = unix_seconds(data->ftLastWriteTime);
	out->change_time = unix_seconds(data->ftCreationTime);
	return 0;
}

CRT int _stat(const char *path, RavenStat *out)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
		*_errno() = 2;
		return -1;
	}
	return fill_stat(&data, out);
}

CRT int _wstat(const wchar_t *path, RavenStat *out)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) {
		*_errno() = 2;
		return -1;
	}
	return fill_stat(&data, out);
}

CRT long long _time64(long long *result)
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	long long seconds = unix_seconds(now);
	if (result)
		*result = seconds;
	return seconds;
}

CRT int ferror(void *stream) { return stream ? ((Stream *)stream)->error : 0; }
CRT int feof(void *stream) { return stream ? ((Stream *)stream)->end_of_file : 0; }

/* ---- time and startup ---- */

CRT long time(long *result)
{
	FILETIME now;
	unsigned long long ticks;
	long seconds;
	GetSystemTimeAsFileTime(&now);
	ticks = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
	seconds = (long)((ticks - 116444736000000000ULL) / 10000000ULL);
	if (result)
		*result = seconds;
	return seconds;
}

static wchar_t *no_wide_environment[1];

CRT int __wgetmainargs(int *count, wchar_t ***vector, wchar_t ***environment_out, int wildcard, void *startup)
{
	if (!wide_argument_vector)
		_configure_wide_argv(1);
	*count = argument_count;
	*vector = wide_argument_vector;
	*environment_out = no_wide_environment;
	return 0;
}

/* ---- C++ allocation: the operators of the 32-bit Microsoft ABI, which ucrtbase32.def exports under their decorated names ---- */

CRT void *raven_operator_new(size_t size) { return malloc(size); }
CRT void raven_operator_delete(void *block) { free(block); }
#ifdef __x86_64__
/* The same operators under their 64-bit decorated names, scalar and array. */
__asm__(".section .drectve\n\t.ascii \" -export:??2@YAPEAX_K@Z=raven_operator_new -export:??_U@YAPEAX_K@Z=raven_operator_new\"\n"
	"\t.ascii \" -export:??3@YAXPEAX@Z=raven_operator_delete -export:??_V@YAXPEAX@Z=raven_operator_delete\"\n\t.text");
#endif
