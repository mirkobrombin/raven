/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The entry points the Visual C++ runtime libraries (vcruntime140, msvcp140, concrt140) import from the Universal CRT: bounded
 * string copies and formatting, the C locale's names of days and months, character classes, wide file and directory names, stream
 * positions and the C++ new handler. Included by ucrtbase.c. */

typedef int (__cdecl *new_handler)(size_t);
static new_handler current_new_handler;

typedef void (__cdecl *terminate_handler)(void);
static terminate_handler current_terminate_handler;

/* The program's handler runs first; if it returns, the process ends as it would without one. */
CRT void terminate(void)
{
	if (current_terminate_handler)
		current_terminate_handler();
	abort();
}

CRT terminate_handler set_terminate(terminate_handler handler)
{
	const terminate_handler previous = current_terminate_handler;
	current_terminate_handler = handler;
	return previous;
}
CRT new_handler _set_new_handler(new_handler handler)
{
	new_handler previous = current_new_handler;
	current_new_handler = handler;
	return previous;
}
CRT int _callnewh(size_t size) { return current_new_handler && current_new_handler(size); }
CRT void _invoke_watson(const wchar_t *expression, const wchar_t *function, const wchar_t *file, unsigned line, uintptr_t reserved)
{
	ExitProcess(0xc0000417);
}
CRT int _seh_filter_dll(unsigned long code, void *pointers) { return 0; }

/* Bounded copies: a destination that cannot hold the whole source is emptied and the call fails, as the secure CRT does. */
CRT int strcpy_s(char *to, size_t size, const char *from)
{
	size_t length;
	if (!to || !size)
		return 22;
	if (!from) {
		to[0] = 0;
		return 22;
	}
	length = strlen(from);
	if (length >= size) {
		to[0] = 0;
		return 34;
	}
	memcpy(to, from, length + 1);
	return 0;
}

/* Bounded appends: the destination must hold a terminated string, and the whole result must fit, or the destination is emptied. */
CRT int strcat_s(char *to, size_t size, const char *from)
{
	size_t used = 0, length;
	if (!to || !size)
		return 22;
	while (used < size && to[used])
		++used;
	if (used == size || !from) {
		to[0] = 0;
		return 22;
	}
	length = strlen(from);
	if (used + length >= size) {
		to[0] = 0;
		return 34;
	}
	memcpy(to + used, from, length + 1);
	return 0;
}

CRT int wcscat_s(wchar_t *to, size_t size, const wchar_t *from)
{
	size_t used = 0, length;
	if (!to || !size)
		return 22;
	while (used < size && to[used])
		++used;
	if (used == size || !from) {
		to[0] = 0;
		return 22;
	}
	length = wcslen(from);
	if (used + length >= size) {
		to[0] = 0;
		return 34;
	}
	memcpy(to + used, from, (length + 1) * sizeof(wchar_t));
	return 0;
}

CRT int wcscpy_s(wchar_t *to, size_t size, const wchar_t *from)
{
	size_t length;
	if (!to || !size)
		return 22;
	if (!from) {
		to[0] = 0;
		return 22;
	}
	length = wcslen(from);
	if (length >= size) {
		to[0] = 0;
		return 34;
	}
	memcpy(to, from, (length + 1) * sizeof(wchar_t));
	return 0;
}

/* _TRUNCATE, (size_t)-1, copies what fits and reports the truncation with STRUNCATE (80). */
CRT int wcsncpy_s(wchar_t *to, size_t size, const wchar_t *from, size_t count)
{
	size_t length = 0;
	if (!to || !size)
		return 22;
	if (!from) {
		to[0] = 0;
		return count ? 22 : 0;
	}
	while (length < count && from[length])
		++length;
	if (length >= size) {
		if (count == (size_t)-1) {
			memcpy(to, from, (size - 1) * sizeof(wchar_t));
			to[size - 1] = 0;
			return 80;
		}
		to[0] = 0;
		return 34;
	}
	memcpy(to, from, length * sizeof(wchar_t));
	to[length] = 0;
	return 0;
}

/* The checked copies: nothing to copy succeeds; a missing destination is EINVAL; a missing source or a destination too small clears
   the destination and reports EINVAL or ERANGE. The move forms leave the destination alone on a missing source. */
static int checked_copy(void *to, size_t size, const void *from, size_t count, int move)
{
	if (!count)
		return 0;
	if (!to)
		return 22;
	if (!from) {
		if (!move)
			memset(to, 0, size);
		return 22;
	}
	if (size < count) {
		if (!move)
			memset(to, 0, size);
		return 34;
	}
	memmove(to, from, count);
	return 0;
}

CRT int memcpy_s(void *to, size_t size, const void *from, size_t count)
{
	return checked_copy(to, size, from, count, 0);
}

CRT int memmove_s(void *to, size_t size, const void *from, size_t count)
{
	return checked_copy(to, size, from, count, 1);
}

CRT int wmemcpy_s(wchar_t *to, size_t size, const wchar_t *from, size_t count)
{
	if (size > (size_t)-1 / sizeof(wchar_t) || count > (size_t)-1 / sizeof(wchar_t))
		return 34;
	return checked_copy(to, size * sizeof(wchar_t), from, count * sizeof(wchar_t), 0);
}

CRT int wmemmove_s(wchar_t *to, size_t size, const wchar_t *from, size_t count)
{
	if (size > (size_t)-1 / sizeof(wchar_t) || count > (size_t)-1 / sizeof(wchar_t))
		return 34;
	return checked_copy(to, size * sizeof(wchar_t), from, count * sizeof(wchar_t), 1);
}

/* _TRUNCATE appends what fits and reports STRUNCATE; otherwise an append that does not fit clears the destination. */
CRT int wcsncat_s(wchar_t *to, size_t size, const wchar_t *from, size_t count)
{
	size_t used = 0, length = 0;
	if (!to || !size)
		return 22;
	while (used < size && to[used])
		++used;
	if (used == size) {
		to[0] = 0;
		return 22;
	}
	if (!from) {
		if (!count)
			return 0;
		to[0] = 0;
		return 22;
	}
	while (length < count && from[length])
		++length;
	if (used + length >= size) {
		if (count == (size_t)-1) {
			memcpy(to + used, from, (size - used - 1) * sizeof(wchar_t));
			to[size - 1] = 0;
			return 80;
		}
		to[0] = 0;
		return 34;
	}
	memcpy(to + used, from, length * sizeof(wchar_t));
	to[used + length] = 0;
	return 0;
}

CRT wchar_t *wcstok_s(wchar_t *text, const wchar_t *delimiters, wchar_t **context)
{
	if (!delimiters || !context || (!text && !*context))
		return NULL;
	if (!text)
		text = *context;
	while (*text && wcschr(delimiters, *text))
		++text;
	if (!*text) {
		*context = text;
		return NULL;
	}
	wchar_t *start = text;
	while (*text && !wcschr(delimiters, *text))
		++text;
	if (*text)
		*text++ = 0;
	*context = text;
	return start;
}

/* Integers to wide text in a buffer of known size; a negative value has its sign only in base 10. */
static int integer_to_wide(unsigned long long value, int negative, wchar_t *buffer, size_t size, int radix)
{
	wchar_t digits[66];
	size_t count = 0;
	if (!buffer || !size)
		return 22;
	if (radix < 2 || radix > 36) {
		buffer[0] = 0;
		return 22;
	}
	do {
		const unsigned digit = (unsigned)(value % (unsigned)radix);
		digits[count++] = (wchar_t)(digit < 10 ? L'0' + digit : L'a' + digit - 10);
		value /= (unsigned)radix;
	} while (value);
	if (negative)
		digits[count++] = L'-';
	if (count >= size) {
		buffer[0] = 0;
		return 34;
	}
	for (size_t at = 0; at < count; ++at)
		buffer[at] = digits[count - 1 - at];
	buffer[count] = 0;
	return 0;
}

CRT int _itow_s(int value, wchar_t *buffer, size_t size, int radix)
{
	const int negative = radix == 10 && value < 0;
	return integer_to_wide(negative ? 0ULL - (unsigned long long)(long long)value : (unsigned)value, negative, buffer, size, radix);
}

CRT int _i64tow_s(long long value, wchar_t *buffer, size_t size, int radix)
{
	const int negative = radix == 10 && value < 0;
	return integer_to_wide(negative ? 0ULL - (unsigned long long)value : (unsigned long long)value, negative, buffer, size, radix);
}

CRT int _ultow_s(unsigned long value, wchar_t *buffer, size_t size, int radix)
{
	return integer_to_wide(value, 0, buffer, size, radix);
}

CRT int _ui64tow_s(unsigned long long value, wchar_t *buffer, size_t size, int radix)
{
	return integer_to_wide(value, 0, buffer, size, radix);
}

CRT double _wtof(const wchar_t *text) { return text ? raven_wcstod(text, NULL) : 0.0; }
CRT int _ldsign(long double x) { return __builtin_signbit(x) ? 0x8000 : 0; }
CRT int __initialize_lconv_for_unsigned_char(void) { return 0; }

/* At most count characters, or as many as fit when count is _TRUNCATE: a result cut short returns -1, and one that does not fit the
   buffer at all leaves an empty string. */
CRT int __stdio_common_vsnwprintf_s(unsigned long long options, wchar_t *buffer, size_t size, size_t count, const wchar_t *text, void *locale,
	va_list arguments)
{
	va_list copy;
	int length;
	if (!buffer || !size || !text)
		return -1;
	va_copy(copy, arguments);
	length = __stdio_common_vswprintf(options, NULL, 0, text, locale, copy);
	va_end(copy);
	if (length < 0) {
		buffer[0] = 0;
		return -1;
	}
	const size_t room = count == (size_t)-1 || count >= size ? size - 1 : count;
	if ((size_t)length > room && count != (size_t)-1 && count >= size) {
		buffer[0] = 0;
		return -1;
	}
	__stdio_common_vswprintf(options, buffer, room + 1, text, locale, arguments);
	buffer[(size_t)length < room ? (size_t)length : room] = 0;
	return (size_t)length > room ? -1 : length;
}

#ifdef __i386__
/* The SSE2 math entry points of 32-bit programs built for SSE2: the argument comes in xmm0 (the exponent of pow in xmm1) and the
   result goes back in xmm0 rather than on the x87 stack. */
__attribute__((used)) double raven_sse2_exp(double x) { return exp(x); }
__attribute__((used)) double raven_sse2_log(double x) { return log(x); }
__attribute__((used)) double raven_sse2_log10(double x) { return log10(x); }
__attribute__((used)) double raven_sse2_pow(double x, double y) { return pow(x, y); }
#define RAVEN_SSE2_UNARY(name, target) \
	__asm__(".text\n.globl __libm_sse2_" name "_precise\n__libm_sse2_" name "_precise:\n\tsubl $8, %esp\n\tmovsd %xmm0, (%esp)\n" \
		"\tcall _" target "\n\tfstpl (%esp)\n\tmovsd (%esp), %xmm0\n\taddl $8, %esp\n\tret\n" \
		".section .drectve\n\t.ascii \" -export:_libm_sse2_" name "_precise\"\n\t.text\n");
RAVEN_SSE2_UNARY("exp", "raven_sse2_exp")
RAVEN_SSE2_UNARY("log", "raven_sse2_log")
RAVEN_SSE2_UNARY("log10", "raven_sse2_log10")
__asm__(".text\n.globl __libm_sse2_pow_precise\n__libm_sse2_pow_precise:\n\tsubl $16, %esp\n\tmovsd %xmm0, (%esp)\n\tmovsd %xmm1, 8(%esp)\n"
	"\tcall _raven_sse2_pow\n\tfstpl (%esp)\n\tmovsd (%esp), %xmm0\n\taddl $16, %esp\n\tret\n"
	".section .drectve\n\t.ascii \" -export:_libm_sse2_pow_precise\"\n\t.text\n");
#endif

CRT size_t strcspn(const char *text, const char *reject)
{
	size_t length = 0;
	for (; text[length]; ++length)
		for (const char *scan = reject; *scan; ++scan)
			if (*scan == text[length])
				return length;
	return length;
}

CRT char *strpbrk(const char *text, const char *accept)
{
	const size_t length = strcspn(text, accept);
	return text[length] ? (char *)text + length : NULL;
}

CRT size_t __strncnt(const char *text, size_t count) { return strnlen(text, count); }

/* The secure forms fail when the result does not fit, and leave an empty string. */
CRT int __stdio_common_vsprintf_s(unsigned long long options, char *buffer, size_t size, const char *text, void *locale, va_list arguments)
{
	va_list copy;
	int length;
	if (!buffer || !size || !text)
		return -1;
	va_copy(copy, arguments);
	length = __stdio_common_vsprintf(options, NULL, 0, text, locale, copy);
	va_end(copy);
	if (length < 0 || (size_t)length >= size) {
		buffer[0] = 0;
		return -1;
	}
	return __stdio_common_vsprintf(options, buffer, size, text, locale, arguments);
}

CRT int __stdio_common_vswprintf_s(unsigned long long options, wchar_t *buffer, size_t size, const wchar_t *text, void *locale, va_list arguments)
{
	va_list copy;
	int length;
	if (!buffer || !size || !text)
		return -1;
	va_copy(copy, arguments);
	length = __stdio_common_vswprintf(options, NULL, 0, text, locale, copy);
	va_end(copy);
	if (length < 0 || (size_t)length >= size) {
		buffer[0] = 0;
		return -1;
	}
	return __stdio_common_vswprintf(options, buffer, size, text, locale, arguments);
}

/* The headers define strtof inline; the export takes it from a name of its own, as strtod does. */
__asm__(".section .drectve\n\t.ascii \" -export:strtof=raven_strtof\"\n\t.text");
float raven_strtof(const char *text, char **end) { return (float)raven_strtod(text, end); }
CRT float logf(float x) { return (float)log(x); }

CRT double copysign(double x, double y)
{
	union { double value; unsigned long long bits; } a = {x}, b = {y};
	a.bits = (a.bits & ~(1ULL << 63)) | (b.bits & (1ULL << 63));
	return a.value;
}

CRT double ldexp(double x, int exponent)
{
	while (exponent > 0) {
		const int step = exponent > 1000 ? 1000 : exponent;
		union { double value; unsigned long long bits; } scale = {0};
		scale.bits = (unsigned long long)(1023 + step) << 52;
		x *= scale.value;
		exponent -= step;
	}
	while (exponent < 0) {
		const int step = exponent < -1000 ? 1000 : -exponent;
		union { double value; unsigned long long bits; } scale = {0};
		scale.bits = (unsigned long long)(1023 - step) << 52;
		x *= scale.value;
		exponent += step;
	}
	return x;
}

/* The classification of a double: FP_INFINITE 1, FP_NAN 2, FP_NORMAL -1, FP_SUBNORMAL -2 and FP_ZERO 0. A long double is a double here. */
CRT short _dclass(double x)
{
	union { double value; unsigned long long bits; } v = {x};
	const unsigned exponent = (unsigned)(v.bits >> 52) & 0x7ff;
	const unsigned long long fraction = v.bits & ((1ULL << 52) - 1);
	if (exponent == 0x7ff)
		return fraction ? 2 : 1;
	if (!exponent)
		return fraction ? -2 : 0;
	return -1;
}
CRT short _ldclass(double x) { return _dclass(x); }

/* The same classification of a float. */
CRT short _fdclass(float x)
{
	union { float value; unsigned bits; } v = {x};
	const unsigned exponent = (v.bits >> 23) & 0xff, fraction = v.bits & ((1U << 23) - 1);
	if (exponent == 0xff)
		return fraction ? 2 : 1;
	if (!exponent)
		return fraction ? -2 : 0;
	return -1;
}

CRT wint_t btowc(int c)
{
	char byte = (char)c;
	wchar_t wide = 0;
	if (c == -1)
		return 0xffff;
	if ((unsigned)c < 0x80)
		return (wint_t)c;
	return MultiByteToWideChar(CP_ACP, 0, &byte, 1, &wide, 1) == 1 ? wide : 0xffff;
}

/* The character classes of the C locale for bytes, with one entry before them for EOF. */
CRT const unsigned short *__pctype_func(void)
{
	static unsigned short table[257];
	if (!table[1 + 'a']) {
		for (int c = 0; c < 256; ++c) {
			unsigned short flags = 0;
			if (c >= 'A' && c <= 'Z')
				flags |= 0x1 | 0x100 | (c <= 'F' ? 0x80 : 0);
			if (c >= 'a' && c <= 'z')
				flags |= 0x2 | 0x100 | (c <= 'f' ? 0x80 : 0);
			if (c >= '0' && c <= '9')
				flags |= 0x4 | 0x80;
			if (c == ' ' || (c >= 9 && c <= 13))
				flags |= 0x8;
			if (c == ' ' || c == '\t')
				flags |= 0x40;
			if (c > 32 && c < 127 && !(flags & 0x107))
				flags |= 0x10;
			if (c < 32 || c == 127)
				flags |= 0x20;
			table[1 + c] = flags;
		}
	}
	return table + 1;
}

/* The C locale: no collation code page, no locale names, and nothing to lock. */
CRT unsigned ___lc_collate_cp_func(void) { return 0; }
CRT wchar_t **___lc_locale_name_func(void)
{
	static wchar_t *names[6];
	return names;
}
CRT void _lock_locales(void) {}
CRT void _unlock_locales(void) {}

static const char *const cpp_day_names[7][2] = {{"Sun", "Sunday"}, {"Mon", "Monday"}, {"Tue", "Tuesday"}, {"Wed", "Wednesday"}, {"Thu", "Thursday"},
	{"Fri", "Friday"}, {"Sat", "Saturday"}};
static const char *const cpp_month_names[12][2] = {{"Jan", "January"}, {"Feb", "February"}, {"Mar", "March"}, {"Apr", "April"}, {"May", "May"},
	{"Jun", "June"}, {"Jul", "July"}, {"Aug", "August"}, {"Sep", "September"}, {"Oct", "October"}, {"Nov", "November"}, {"Dec", "December"}};

/* The names as the C++ library asks for them: ":Sun:Sunday:Mon:Monday..." in memory the caller frees. */
static char *joined_names(const char *const (*names)[2], int count)
{
	size_t size = 1;
	for (int index = 0; index < count; ++index)
		size += strlen(names[index][0]) + strlen(names[index][1]) + 2;
	char *text = malloc(size), *at = text;
	if (!text)
		return NULL;
	for (int index = 0; index < count; ++index)
		for (int form = 0; form < 2; ++form) {
			const size_t length = strlen(names[index][form]);
			*at++ = ':';
			memcpy(at, names[index][form], length);
			at += length;
		}
	*at = 0;
	return text;
}

static wchar_t *widened_names(const char *narrow)
{
	if (!narrow)
		return NULL;
	const size_t length = strlen(narrow);
	wchar_t *wide = malloc((length + 1) * sizeof(wchar_t));
	if (wide)
		for (size_t index = 0; index <= length; ++index)
			wide[index] = (unsigned char)narrow[index];
	free((void *)narrow);
	return wide;
}

CRT char *_Getdays(void) { return joined_names(cpp_day_names, 7); }
CRT char *_Getmonths(void) { return joined_names(cpp_month_names, 12); }
CRT wchar_t *_W_Getdays(void) { return widened_names(_Getdays()); }
CRT wchar_t *_W_Getmonths(void) { return widened_names(_Getmonths()); }

/* The time names of the C locale in the layout of the CRT's __lc_time_data, in memory the caller frees as one block. */
typedef struct {
	const char *wday_abbr[7], *wday[7], *month_abbr[12], *month[12], *ampm[2], *ww_sdatefmt, *ww_ldatefmt, *ww_timefmt;
	int ww_caltype;
	long refcount;
	const wchar_t *_W_wday_abbr[7], *_W_wday[7], *_W_month_abbr[12], *_W_month[12], *_W_ampm[2], *_W_ww_sdatefmt, *_W_ww_ldatefmt,
		*_W_ww_timefmt, *_W_ww_locale_name;
} TimeNames;

static const wchar_t *const wide_days[7][2] = {{L"Sun", L"Sunday"}, {L"Mon", L"Monday"}, {L"Tue", L"Tuesday"}, {L"Wed", L"Wednesday"},
	{L"Thu", L"Thursday"}, {L"Fri", L"Friday"}, {L"Sat", L"Saturday"}};
static const wchar_t *const wide_months[12][2] = {{L"Jan", L"January"}, {L"Feb", L"February"}, {L"Mar", L"March"}, {L"Apr", L"April"},
	{L"May", L"May"}, {L"Jun", L"June"}, {L"Jul", L"July"}, {L"Aug", L"August"}, {L"Sep", L"September"}, {L"Oct", L"October"},
	{L"Nov", L"November"}, {L"Dec", L"December"}};

CRT void *_Gettnames(void)
{
	TimeNames *names = calloc(1, sizeof *names);
	if (!names)
		return NULL;
	for (int index = 0; index < 7; ++index) {
		names->wday_abbr[index] = cpp_day_names[index][0];
		names->wday[index] = cpp_day_names[index][1];
		names->_W_wday_abbr[index] = wide_days[index][0];
		names->_W_wday[index] = wide_days[index][1];
	}
	for (int index = 0; index < 12; ++index) {
		names->month_abbr[index] = cpp_month_names[index][0];
		names->month[index] = cpp_month_names[index][1];
		names->_W_month_abbr[index] = wide_months[index][0];
		names->_W_month[index] = wide_months[index][1];
	}
	names->ampm[0] = "AM";
	names->ampm[1] = "PM";
	names->_W_ampm[0] = L"AM";
	names->_W_ampm[1] = L"PM";
	names->ww_sdatefmt = "MM/dd/yy";
	names->ww_ldatefmt = "dddd, MMMM dd, yyyy";
	names->ww_timefmt = "HH:mm:ss";
	names->_W_ww_sdatefmt = L"MM/dd/yy";
	names->_W_ww_ldatefmt = L"dddd, MMMM dd, yyyy";
	names->_W_ww_timefmt = L"HH:mm:ss";
	names->_W_ww_locale_name = L"";
	names->refcount = 1;
	return names;
}
CRT void *_W_Gettnames(void) { return _Gettnames(); }

CRT size_t _Strftime(char *to, size_t capacity, const char *format, const void *time, void *names) { return strftime(to, capacity, format, time); }
CRT size_t _Wcsftime(wchar_t *to, size_t capacity, const wchar_t *format, const void *time, void *names) { return wcsftime(to, capacity, format, time); }

/* Streams: positions as 64-bit offsets, wide characters read as single bytes of the C locale, and no buffer handed out, so the C++
   library reads and writes through the stream functions. */
CRT int fgetpos(void *stream, long long *position)
{
	const long long at = _ftelli64(stream);
	if (at < 0)
		return -1;
	*position = at;
	return 0;
}
CRT int fsetpos(void *stream, const long long *position) { return _fseeki64(stream, *position, 0); }
CRT wint_t fgetwc(void *stream)
{
	const int c = stream_get(stream);
	return c < 0 ? 0xffff : btowc(c);
}
CRT int _get_stream_buffer_pointers(void *stream, char ***base, char ***pointer, int **count)
{
	static char *no_base, *no_pointer;
	static int no_count;
	if (base)
		*base = &no_base;
	if (pointer)
		*pointer = &no_pointer;
	if (count)
		*count = &no_count;
	return 0;
}
CRT void *_fsopen(const char *path, const char *mode, int share) { return fopen(path, mode); }
CRT void *_wfsopen(const wchar_t *path, const wchar_t *mode, int share) { return _wfopen(path, mode); }

/* Wide names of files and directories. */
static int windows_result(BOOL done)
{
	if (done)
		return 0;
	set_errno(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? 2 : 13);
	return -1;
}
CRT int _wchdir(const wchar_t *path) { return windows_result(SetCurrentDirectoryW(path)); }
CRT int _wremove(const wchar_t *path) { return windows_result(DeleteFileW(path)); }
CRT int _wrename(const wchar_t *from, const wchar_t *to) { return windows_result(MoveFileW(from, to)); }
CRT int _wrmdir(const wchar_t *path) { return windows_result(RemoveDirectoryW(path)); }
CRT wchar_t *_wgetcwd(wchar_t *buffer, int size)
{
	const DWORD needed = GetCurrentDirectoryW(0, NULL);
	if (!needed)
		return NULL;
	if (!buffer) {
		const int capacity = size > (int)needed ? size : (int)needed;
		buffer = malloc((size_t)capacity * sizeof(wchar_t));
		if (!buffer)
			return NULL;
		size = capacity;
	}
	if ((DWORD)size < needed) {
		set_errno(34);
		return NULL;
	}
	GetCurrentDirectoryW((DWORD)size, buffer);
	return buffer;
}
