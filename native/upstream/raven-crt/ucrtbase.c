/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven C runtime component: the Universal CRT entry points that Windows programs import from ucrtbase.dll.
 *
 * This is Raven code on the public Windows headers. It covers program startup, memory, strings, formatted output and the
 * standard streams; it grows with the programs that need it. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>

#define CRT __declspec(dllexport)

/* The classification names are macros in the public headers; this file defines the functions behind them. */
#undef isdigit
#undef isalpha
#undef isalnum
#undef isspace
#undef isupper
#undef islower
#undef isxdigit
#undef iswspace
#undef iswdigit
#undef iswalpha
#undef iswalnum
#undef iswcntrl
#undef iswupper
#undef iswlower
#undef iswxdigit
#undef iswpunct
#undef tolower
#undef toupper
#undef towlower
#undef towupper

/* ---- memory and strings: plain loops, no compiler idiom recognition, so they cannot call themselves ---- */
#define PLAIN __attribute__((optimize("no-tree-loop-distribute-patterns")))

CRT PLAIN void *memcpy(void *to, const void *from, size_t count)
{
	unsigned char *a = to;
	const unsigned char *b = from;
	while (count--)
		*a++ = *b++;
	return to;
}

CRT PLAIN void *memmove(void *to, const void *from, size_t count)
{
	unsigned char *a = to;
	const unsigned char *b = from;
	if (a < b || a >= b + count) {
		while (count--)
			*a++ = *b++;
	} else {
		a += count;
		b += count;
		while (count--)
			*--a = *--b;
	}
	return to;
}

CRT PLAIN void *memset(void *to, int value, size_t count)
{
	unsigned char *a = to;
	while (count--)
		*a++ = (unsigned char)value;
	return to;
}

CRT PLAIN int memcmp(const void *left, const void *right, size_t count)
{
	const unsigned char *a = left, *b = right;
	for (; count; --count, ++a, ++b)
		if (*a != *b)
			return *a < *b ? -1 : 1;
	return 0;
}

CRT void *memchr(const void *block, int value, size_t count)
{
	const unsigned char *a = block;
	for (; count; --count, ++a)
		if (*a == (unsigned char)value)
			return (void *)a;
	return NULL;
}

CRT size_t strlen(const char *text)
{
	const char *at = text;
	while (*at)
		++at;
	return (size_t)(at - text);
}

CRT size_t wcslen(const wchar_t *text)
{
	const wchar_t *at = text;
	while (*at)
		++at;
	return (size_t)(at - text);
}

CRT int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		++a, ++b;
	return (unsigned char)*a - (unsigned char)*b;
}

CRT int strncmp(const char *a, const char *b, size_t count)
{
	while (count && *a && *a == *b)
		++a, ++b, --count;
	return count ? (unsigned char)*a - (unsigned char)*b : 0;
}

CRT int wcscmp(const wchar_t *a, const wchar_t *b)
{
	while (*a && *a == *b)
		++a, ++b;
	return (int)*a - (int)*b;
}

CRT int wcsncmp(const wchar_t *a, const wchar_t *b, size_t count)
{
	while (count && *a && *a == *b)
		++a, ++b, --count;
	return count ? (int)*a - (int)*b : 0;
}

CRT char *strcpy(char *to, const char *from)
{
	char *start = to;
	while ((*to++ = *from++))
		;
	return start;
}

CRT char *strncpy(char *to, const char *from, size_t count)
{
	size_t index = 0;
	for (; index < count && from[index]; ++index)
		to[index] = from[index];
	for (; index < count; ++index)
		to[index] = 0;
	return to;
}

CRT wchar_t *wcscpy(wchar_t *to, const wchar_t *from)
{
	wchar_t *start = to;
	while ((*to++ = *from++))
		;
	return start;
}

CRT wchar_t *wcsncpy(wchar_t *to, const wchar_t *from, size_t count)
{
	size_t index = 0;
	for (; index < count && from[index]; ++index)
		to[index] = from[index];
	for (; index < count; ++index)
		to[index] = 0;
	return to;
}

CRT char *strcat(char *to, const char *from)
{
	strcpy(to + strlen(to), from);
	return to;
}

CRT wchar_t *wcscat(wchar_t *to, const wchar_t *from)
{
	wcscpy(to + wcslen(to), from);
	return to;
}

CRT char *strchr(const char *text, int value)
{
	for (;; ++text) {
		if (*text == (char)value)
			return (char *)text;
		if (!*text)
			return NULL;
	}
}

CRT char *strrchr(const char *text, int value)
{
	const char *found = NULL;
	for (;; ++text) {
		if (*text == (char)value)
			found = text;
		if (!*text)
			return (char *)found;
	}
}

CRT wchar_t *wcschr(const wchar_t *text, wchar_t value)
{
	for (;; ++text) {
		if (*text == value)
			return (wchar_t *)text;
		if (!*text)
			return NULL;
	}
}

CRT wchar_t *wcsrchr(const wchar_t *text, wchar_t value)
{
	const wchar_t *found = NULL;
	for (;; ++text) {
		if (*text == value)
			found = text;
		if (!*text)
			return (wchar_t *)found;
	}
}

CRT char *strstr(const char *text, const char *pattern)
{
	size_t length = strlen(pattern);
	if (!length)
		return (char *)text;
	for (; *text; ++text)
		if (!strncmp(text, pattern, length))
			return (char *)text;
	return NULL;
}

CRT wchar_t *wcsstr(const wchar_t *text, const wchar_t *pattern)
{
	size_t length = wcslen(pattern);
	if (!length)
		return (wchar_t *)text;
	for (; *text; ++text)
		if (!wcsncmp(text, pattern, length))
			return (wchar_t *)text;
	return NULL;
}

CRT int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
CRT int toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
CRT wint_t towlower(wint_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : (c >= 0xc0 && c <= 0xde && c != 0xd7) ? c + 32 : c; }
CRT wint_t towupper(wint_t c) { return c >= 'a' && c <= 'z' ? c - 32 : (c >= 0xe0 && c <= 0xfe && c != 0xf7) ? c - 32 : c; }
CRT int isdigit(int c) { return c >= '0' && c <= '9'; }
CRT int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
CRT int isalnum(int c) { return isdigit(c) || isalpha(c); }
CRT int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
CRT int isupper(int c) { return c >= 'A' && c <= 'Z'; }
CRT int islower(int c) { return c >= 'a' && c <= 'z'; }
CRT int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
CRT int iswspace(wint_t c) { return c == ' ' || (c >= 9 && c <= 13); }
CRT int iswdigit(wint_t c) { return c >= '0' && c <= '9'; }
CRT int iswalpha(wint_t c) { return isalpha((int)c) || c >= 0xc0; }

CRT int _stricmp(const char *a, const char *b)
{
	while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
		++a, ++b;
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

CRT int _strnicmp(const char *a, const char *b, size_t count)
{
	while (count && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
		++a, ++b, --count;
	return count ? tolower((unsigned char)*a) - tolower((unsigned char)*b) : 0;
}

CRT int _wcsicmp(const wchar_t *a, const wchar_t *b)
{
	while (*a && towlower(*a) == towlower(*b))
		++a, ++b;
	return (int)towlower(*a) - (int)towlower(*b);
}

CRT unsigned long long _strtoull_core(const char *text, char **end, int base, int *negative)
{
	const char *cursor = text;
	unsigned long long value = 0;
	*negative = 0;
	while (isspace((unsigned char)*cursor))
		++cursor;
	if (*cursor == '-' || *cursor == '+')
		*negative = *cursor++ == '-';
	if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
		cursor += 2;
		base = 16;
	} else if (base == 0) {
		base = cursor[0] == '0' ? 8 : 10;
	}
	for (;; ++cursor) {
		int digit = isdigit((unsigned char)*cursor) ? *cursor - '0' : isalpha((unsigned char)*cursor) ? tolower((unsigned char)*cursor) - 'a' + 10 : 99;
		if (digit >= base)
			break;
		value = value * (unsigned)base + (unsigned)digit;
	}
	if (end)
		*end = (char *)cursor;
	return value;
}

CRT long strtol(const char *text, char **end, int base)
{
	int negative;
	unsigned long long value = _strtoull_core(text, end, base, &negative);
	return negative ? -(long)value : (long)value;
}

CRT unsigned long strtoul(const char *text, char **end, int base)
{
	int negative;
	unsigned long long value = _strtoull_core(text, end, base, &negative);
	return negative ? (unsigned long)(-(long)value) : (unsigned long)value;
}

CRT long long _strtoi64(const char *text, char **end, int base)
{
	int negative;
	unsigned long long value = _strtoull_core(text, end, base, &negative);
	return negative ? -(long long)value : (long long)value;
}

CRT int atoi(const char *text) { return (int)strtol(text, NULL, 10); }
CRT long atol(const char *text) { return strtol(text, NULL, 10); }

CRT long wcstol(const wchar_t *text, wchar_t **end, int base)
{
	char narrow[64];
	size_t index = 0;
	char *narrow_end;
	long value;
	while (text[index] && text[index] < 128 && index < 63)
		narrow[index] = (char)text[index], ++index;
	narrow[index] = 0;
	value = strtol(narrow, &narrow_end, base);
	if (end)
		*end = (wchar_t *)text + (narrow_end - narrow);
	return value;
}

CRT unsigned long wcstoul(const wchar_t *text, wchar_t **end, int base)
{
	char narrow[64];
	size_t index = 0;
	char *narrow_end;
	unsigned long value;
	while (text[index] && text[index] < 128 && index < 63)
		narrow[index] = (char)text[index], ++index;
	narrow[index] = 0;
	value = strtoul(narrow, &narrow_end, base);
	if (end)
		*end = (wchar_t *)text + (narrow_end - narrow);
	return value;
}

CRT size_t wcsspn(const wchar_t *text, const wchar_t *accepted)
{
	size_t length = 0;
	while (text[length] && wcschr(accepted, text[length]))
		++length;
	return length;
}

/* A decimal number with an optional exponent, its digits scaled by powers of ten; no digits read nothing. The headers define wcstod
 * inline, so the function has a name of its own and is exported as wcstod. */
__asm__(".section .drectve\n\t.ascii \" -export:wcstod=raven_wcstod\"\n\t.text");
double raven_wcstod(const wchar_t *text, wchar_t **end)
{
	const wchar_t *at = text;
	int negative = 0, digits = 0;
	double value = 0, scale = 1;
	while (*at == ' ' || (*at >= 9 && *at <= 13))
		++at;
	if (*at == '-' || *at == '+')
		negative = *at++ == '-';
	while (*at >= '0' && *at <= '9')
		value = value * 10 + (*at++ - '0'), ++digits;
	if (*at == '.') {
		++at;
		while (*at >= '0' && *at <= '9') {
			scale /= 10;
			value += (*at++ - '0') * scale;
			++digits;
		}
	}
	if (!digits) {
		if (end)
			*end = (wchar_t *)text;
		return 0;
	}
	if ((*at == 'e' || *at == 'E') && ((at[1] >= '0' && at[1] <= '9') || ((at[1] == '-' || at[1] == '+') && at[2] >= '0' && at[2] <= '9'))) {
		int exponent = 0, exponent_negative = 0;
		++at;
		if (*at == '-' || *at == '+')
			exponent_negative = *at++ == '-';
		while (*at >= '0' && *at <= '9')
			exponent = exponent * 10 + (*at++ - '0');
		while (exponent-- > 0)
			value = exponent_negative ? value / 10 : value * 10;
	}
	if (end)
		*end = (wchar_t *)at;
	return negative ? -value : value;
}

/* wcstod narrowed to a float. */
__asm__(".section .drectve\n\t.ascii \" -export:wcstof=raven_wcstof\"\n\t.text");
float raven_wcstof(const wchar_t *text, wchar_t **end)
{
	return (float)raven_wcstod(text, end);
}

CRT int _wtoi(const wchar_t *text) { return (int)wcstol(text, NULL, 10); }

CRT long long _wcstoi64(const wchar_t *text, wchar_t **end, int base)
{
	char narrow[64];
	size_t index = 0;
	char *narrow_end;
	long long value;
	while (text[index] && text[index] < 128 && index < 63)
		narrow[index] = (char)text[index], ++index;
	narrow[index] = 0;
	value = _strtoi64(narrow, &narrow_end, base);
	if (end)
		*end = (wchar_t *)text + (narrow_end - narrow);
	return value;
}

CRT long long _wtoi64(const wchar_t *text) { return _wcstoi64(text, NULL, 10); }

/* ---- heap ---- */
CRT void *malloc(size_t size) { return HeapAlloc(GetProcessHeap(), 0, size ? size : 1); }
CRT void *calloc(size_t count, size_t size) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * size ? count * size : 1); }
CRT void free(void *block) { if (block) HeapFree(GetProcessHeap(), 0, block); }
CRT size_t _msize(void *block) { return HeapSize(GetProcessHeap(), 0, block); }

CRT void *realloc(void *block, size_t size)
{
	if (!block)
		return malloc(size);
	if (!size) {
		free(block);
		return NULL;
	}
	return HeapReAlloc(GetProcessHeap(), 0, block, size);
}

/* ---- program startup and termination ---- */
typedef void (__cdecl *initializer)(void);
typedef int (__cdecl *initializer_checked)(void);

static int argument_count;
static char **argument_vector;
static wchar_t **wide_argument_vector;
static char **environment;
static int commode, fmode;

CRT int *__p___argc(void) { return &argument_count; }
CRT char ***__p___argv(void) { return &argument_vector; }
CRT wchar_t ***__p___wargv(void) { return &wide_argument_vector; }
CRT char ***__p__environ(void) { return &environment; }
CRT int *__p__commode(void) { return &commode; }
CRT int *__p__fmode(void) { return &fmode; }
CRT int _set_fmode(int mode) { fmode = mode; return 0; }
CRT void _set_app_type(int type) {}
CRT int _set_new_mode(int mode) { return 0; }
CRT int _configthreadlocale(int mode) { return 0; }
CRT int _controlfp_s(unsigned int *current, unsigned int value, unsigned int mask) { if (current) *current = 0; return 0; }
/* The control word programs start with: round to nearest, 53-bit precision, every exception masked. */
CRT unsigned int _controlfp(unsigned int value, unsigned int mask) { return 0x9001f; }
CRT void __setusermatherr(void *handler) {}
CRT int _crt_atexit(void (__cdecl *function)(void));

CRT void _initterm(initializer *begin, initializer *end)
{
	for (; begin < end; ++begin)
		if (*begin)
			(*begin)();
}

CRT int _initterm_e(initializer_checked *begin, initializer_checked *end)
{
	for (; begin < end; ++begin)
		if (*begin) {
			int result = (*begin)();
			if (result)
				return result;
		}
	return 0;
}

static int split_command_line(char *line, char **vector, char *store)
{
	int count = 0, quoted = 0;
	char *out = store;
	while (*line) {
		while (*line == ' ' || *line == '\t')
			++line;
		if (!*line)
			break;
		if (vector)
			vector[count] = out;
		++count;
		for (quoted = 0; *line && (quoted || (*line != ' ' && *line != '\t')); ++line) {
			if (*line == '"')
				quoted = !quoted;
			else if (out)
				*out++ = *line;
		}
		if (out)
			*out++ = 0;
	}
	return count;
}

CRT int _configure_narrow_argv(int mode)
{
	const char *line = GetCommandLineA();
	size_t length = strlen(line);
	int count;
	char *copy = malloc(length + 1), *store = malloc(length + 2);
	char **vector;
	if (!copy || !store)
		return 12;
	memcpy(copy, line, length + 1);
	count = split_command_line(copy, NULL, NULL);
	vector = malloc(((size_t)count + 1) * sizeof(char *));
	if (!vector)
		return 12;
	memcpy(copy, line, length + 1);
	split_command_line(copy, vector, store);
	vector[count] = NULL;
	argument_count = count;
	argument_vector = vector;
	free(copy);
	return 0;
}

CRT int _configure_wide_argv(int mode)
{
	const wchar_t *line = GetCommandLineW();
	size_t length = wcslen(line), count = 0, at = 0;
	wchar_t **vector = malloc((length / 2 + 2) * sizeof(wchar_t *));
	wchar_t *store = malloc((length + 2) * sizeof(wchar_t)), *out = store;
	int quoted;
	if (!vector || !store)
		return 12;
	while (line[at]) {
		while (line[at] == ' ' || line[at] == '\t')
			++at;
		if (!line[at])
			break;
		vector[count++] = out;
		for (quoted = 0; line[at] && (quoted || (line[at] != ' ' && line[at] != '\t')); ++at) {
			if (line[at] == '"')
				quoted = !quoted;
			else
				*out++ = line[at];
		}
		*out++ = 0;
	}
	vector[count] = NULL;
	argument_count = (int)count;
	wide_argument_vector = vector;
	return 0;
}

CRT int _initialize_narrow_environment(void)
{
	LPWCH block = GetEnvironmentStringsW(), at;
	size_t count = 0, index = 0;
	if (!block)
		return 0;
	for (at = block; *at; at += wcslen(at) + 1)
		++count;
	environment = malloc((count + 1) * sizeof(char *));
	for (at = block; *at && environment; at += wcslen(at) + 1) {
		int size = WideCharToMultiByte(CP_ACP, 0, at, -1, NULL, 0, NULL, NULL);
		environment[index] = malloc((size_t)size);
		if (environment[index])
			WideCharToMultiByte(CP_ACP, 0, at, -1, environment[index], size, NULL, NULL);
		++index;
	}
	if (environment)
		environment[index] = NULL;
	FreeEnvironmentStringsW(block);
	return 0;
}

CRT char **_get_initial_narrow_environment(void) { return environment; }

#define MAX_EXIT_FUNCTIONS 64
static void (__cdecl *exit_functions[MAX_EXIT_FUNCTIONS])(void);
static int exit_function_count;

CRT int _crt_atexit(void (__cdecl *function)(void))
{
	if (exit_function_count >= MAX_EXIT_FUNCTIONS)
		return -1;
	exit_functions[exit_function_count++] = function;
	return 0;
}

CRT int atexit(void (__cdecl *function)(void)) { return _crt_atexit(function); }
CRT int _register_onexit_function(void *table, void (__cdecl *function)(void)) { return _crt_atexit(function); }
CRT int _initialize_onexit_table(void *table) { return 0; }
CRT int _execute_onexit_table(void *table) { return 0; }

CRT void _cexit(void)
{
	while (exit_function_count)
		exit_functions[--exit_function_count]();
}

CRT void _c_exit(void) {}
CRT void _exit(int status) { ExitProcess((UINT)status); }
CRT void exit(int status) { _cexit(); ExitProcess((UINT)status); }
CRT void abort(void) { ExitProcess(3); }
CRT void __cdecl _assert(const char *expression, const char *file, unsigned line) { ExitProcess(3); }
typedef struct {
	int error;
	unsigned long dos_error;
} ErrorState;

DWORD _tls_index;
static ErrorState error_template;
const IMAGE_TLS_DIRECTORY _tls_used __attribute__((section(".rdata$T"))) = {
	(ULONG_PTR)&error_template, (ULONG_PTR)&error_template + sizeof(error_template),
	(ULONG_PTR)&_tls_index, 0, 0, IMAGE_SCN_ALIGN_4BYTES
};

static ErrorState *thread_errors(void)
{
	ErrorState **slots;
#ifdef __x86_64__
	__asm__("movq %%gs:0x58,%0" : "=r"(slots));
#else
	__asm__("movl %%fs:0x2c,%0" : "=r"(slots));
#endif
	return slots[_tls_index];
}

CRT int *_errno(void) { return &thread_errors()->error; }
CRT unsigned long *__doserrno(void) { return &thread_errors()->dos_error; }
CRT void _invalid_parameter_noinfo(void) {}
CRT void _invalid_parameter_noinfo_noreturn(void) { ExitProcess(3); }

/* ---- streams ---- */
/* The record is the size of msvcrt's FILE on x64 (48 bytes), so the table of standard streams is also the array that msvcrt
   programs index; the read buffer is allocated when a stream first reads. */
#define STREAM_BUFFER 4096
/* The fields sit where msvcrt's FILE keeps its own: programs built against msvcrt read and write through getc and putc macros that
   take the next byte at the pointer while the count stays above zero. The pointer is always null and the count zero here, so those
   macros call _filbuf and _flsbuf for every byte, and the index sits where msvcrt keeps the descriptor. */
typedef struct {
	char *macro_pointer;
	int macro_count;
	/* File streams: text mode translates CRLF, reads are buffered, one character can be pushed back. */
	int pushed;
	unsigned char *buffer;
	unsigned char text, end_of_file, error, writable;
	int index;
	HANDLE handle;
	unsigned buffer_length, buffer_position;
} Stream;
#ifdef __x86_64__
_Static_assert(sizeof(Stream) == 48, "a stream must have the size of msvcrt's FILE");
#endif

Stream streams[3] = {{.index = 0, .pushed = -1}, {.index = 1, .pushed = -1}, {.index = 2, .pushed = -1}};

CRT void *__acrt_iob_func(unsigned index)
{
	static const DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
	if (index > 2)
		return NULL;
	streams[index].index = (int)index;
	streams[index].handle = GetStdHandle(ids[index]);
	return &streams[index];
}

static void stream_prepare_write(Stream *stream);

static int write_stream(Stream *stream, const char *bytes, size_t count)
{
	DWORD written;
	if (!stream || !count)
		return 0;
	stream_prepare_write(stream);
	if (stream->text) {
		/* Text mode writes each line feed as a carriage return and a line feed. */
		char chunk[1024];
		size_t done = 0;
		while (done < count) {
			size_t used = 0;
			while (done < count && used + 2 <= sizeof chunk) {
				if (bytes[done] == '\n')
					chunk[used++] = '\r';
				chunk[used++] = bytes[done++];
			}
			if (!WriteFile(stream->handle, chunk, (DWORD)used, &written, NULL) || written != used) {
				stream->error = 1;
				return -1;
			}
		}
		return (int)count;
	}
	return WriteFile(stream->handle, bytes, (DWORD)count, &written, NULL) ? (int)written : -1;
}

CRT size_t fwrite(const void *data, size_t size, size_t count, void *stream)
{
	int written = write_stream(stream, data, size * count);
	return written < 0 || !size ? 0 : (size_t)written / size;
}

CRT int fputs(const char *text, void *stream) { return write_stream(stream, text, strlen(text)) < 0 ? -1 : 0; }
CRT int fputc(int c, void *stream) { char byte = (char)c; return write_stream(stream, &byte, 1) == 1 ? (unsigned char)c : -1; }
CRT int putchar(int c) { return fputc(c, __acrt_iob_func(1)); }
CRT int puts(const char *text) { return fputs(text, __acrt_iob_func(1)) < 0 || putchar('\n') < 0 ? -1 : 0; }
CRT int fflush(void *stream) { return 0; }
CRT int _fileno(void *stream) { return stream ? ((Stream *)stream)->index : -1; }
static intptr_t descriptor_handle(int fd);
CRT intptr_t _get_osfhandle(int fd) { return descriptor_handle(fd); }

/* ---- formatted output ---- */
typedef struct {
	char *narrow;
	wchar_t *wide;
	size_t capacity, length;
	Stream *stream;
	char chunk[256];
	size_t chunk_length;
} Sink;

static void sink_flush(Sink *sink)
{
	if (sink->stream && sink->chunk_length)
		write_stream(sink->stream, sink->chunk, sink->chunk_length);
	sink->chunk_length = 0;
}

static void sink_put(Sink *sink, wchar_t c)
{
	if (sink->stream) {
		sink->chunk[sink->chunk_length++] = c < 128 ? (char)c : '?';
		if (sink->chunk_length == sizeof sink->chunk)
			sink_flush(sink);
	} else if (sink->length + 1 < sink->capacity) {
		if (sink->wide)
			sink->wide[sink->length] = c;
		else
			sink->narrow[sink->length] = c < 256 ? (char)c : '?';
	}
	++sink->length;
}

static void pad(Sink *sink, int count, wchar_t c)
{
	while (count-- > 0)
		sink_put(sink, c);
}

/* One formatter serves narrow and wide formats; source characters are read through accessors. */
static unsigned long long format_options;

static int format(Sink *sink, const char *narrow_format, const wchar_t *wide_format, va_list arguments)
{
#define NEXT() (wide_format ? (wchar_t)*wide_format++ : (wchar_t)(unsigned char)*narrow_format++)
#define PEEK() (wide_format ? (wchar_t)*wide_format : (wchar_t)(unsigned char)*narrow_format)
	for (;;) {
		wchar_t c = PEEK();
		int left = 0, plus = 0, space = 0, zero = 0, alternate = 0, width = 0, precision = -1, size = 0;
		wchar_t spec;
		if (!c)
			break;
		NEXT();
		if (c != '%') {
			sink_put(sink, c);
			continue;
		}
		for (;; NEXT()) {
			c = PEEK();
			if (c == '-') left = 1;
			else if (c == '+') plus = 1;
			else if (c == ' ') space = 1;
			else if (c == '0') zero = 1;
			else if (c == '#') alternate = 1;
			else break;
		}
		if (PEEK() == '*') {
			NEXT();
			width = va_arg(arguments, int);
			if (width < 0)
				left = 1, width = -width;
		} else {
			while (PEEK() >= '0' && PEEK() <= '9')
				width = width * 10 + (NEXT() - '0');
		}
		if (PEEK() == '.') {
			NEXT();
			precision = 0;
			if (PEEK() == '*') {
				NEXT();
				precision = va_arg(arguments, int);
			} else {
				while (PEEK() >= '0' && PEEK() <= '9')
					precision = precision * 10 + (NEXT() - '0');
			}
		}
		for (;;) {
			c = PEEK();
			if (c == 'l') { NEXT(); size = PEEK() == 'l' ? (NEXT(), 2) : 1; }
			else if (c == 'h') { NEXT(); size = -1; if (PEEK() == 'h') NEXT(); }
			else if (c == 'w') { NEXT(); size = 3; }
			else if (c == 'z' || c == 'j' || c == 't') { NEXT(); size = 2; }
			else if (c == 'I') {
				NEXT();
				if (PEEK() == '6') { NEXT(); NEXT(); size = 2; }
				else if (PEEK() == '3') { NEXT(); NEXT(); }
				else size = 2;
			} else break;
		}
		spec = NEXT();
		if (!spec)
			break;
		if (spec == '%') {
			sink_put(sink, '%');
		} else if (spec == 'c' || spec == 'C') {
			wchar_t value = (wchar_t)va_arg(arguments, int);
			pad(sink, left ? 0 : width - 1, ' ');
			sink_put(sink, value);
			pad(sink, left ? width - 1 : 0, ' ');
		} else if (spec == 's' || spec == 'S' || spec == 'Z') {
			/* %s follows the format's width, %S the opposite; %ls and %hs force one. */
			int wide_argument = size == 1 || size == 3 || (spec == 's' ? wide_format != NULL : wide_format == NULL);
			/* The legacy wide specifier option makes %s in a wide format a narrow string, as in the classic wprintf. */
			if (size == 0 && spec == 's' && wide_format && (format_options & 4))
				wide_argument = 0;
			else if (size == 0 && spec == 'S' && wide_format && (format_options & 4))
				wide_argument = 1;
			const void *argument = va_arg(arguments, const void *);
			size_t length = 0, index;
			if (size == -1)
				wide_argument = 0;
			if (!argument)
				argument = wide_argument ? (const void *)L"(null)" : (const void *)"(null)";
			while ((precision < 0 || (int)length < precision) && (wide_argument ? ((const wchar_t *)argument)[length] : ((const unsigned char *)argument)[length]))
				++length;
			pad(sink, left ? 0 : width - (int)length, ' ');
			for (index = 0; index < length; ++index)
				sink_put(sink, wide_argument ? ((const wchar_t *)argument)[index] : ((const unsigned char *)argument)[index]);
			pad(sink, left ? width - (int)length : 0, ' ');
		} else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o' || spec == 'p') {
			unsigned long long value;
			int negative = 0, radix = spec == 'x' || spec == 'X' || spec == 'p' ? 16 : spec == 'o' ? 8 : 10;
			char digits[72];
			int count = 0, total;
			const char *alphabet = spec == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
			if (spec == 'p') {
				value = (unsigned long long)va_arg(arguments, void *);
				precision = precision < 0 ? 16 : precision;
			} else if (spec == 'd' || spec == 'i') {
				long long signed_value = size >= 2 ? va_arg(arguments, long long) : size == 1 ? (long long)va_arg(arguments, long) : (long long)va_arg(arguments, int);
				if (size == -1)
					signed_value = (short)signed_value;
				negative = signed_value < 0;
				value = negative ? 0ULL - (unsigned long long)signed_value : (unsigned long long)signed_value;
			} else {
				value = size >= 2 ? va_arg(arguments, unsigned long long) : (unsigned long long)va_arg(arguments, unsigned int);
				if (size == -1)
					value &= 0xffff;
			}
			do {
				digits[count++] = alphabet[value % (unsigned)radix];
				value /= (unsigned)radix;
			} while (value);
			while (precision > count && count < 70)
				digits[count++] = '0';
			total = count + (negative || plus || space ? 1 : 0) + (alternate && radix == 16 && spec != 'p' ? 2 : 0);
			if (!left && !zero)
				pad(sink, width - total, ' ');
			if (negative) sink_put(sink, '-');
			else if (plus) sink_put(sink, '+');
			else if (space) sink_put(sink, ' ');
			if (alternate && radix == 16 && spec != 'p') {
				sink_put(sink, '0');
				sink_put(sink, spec);
			}
			if (!left && zero)
				pad(sink, width - total, '0');
			while (count)
				sink_put(sink, (wchar_t)digits[--count]);
			if (left)
				pad(sink, width - total, ' ');
		} else if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'g' || spec == 'G') {
			double value = va_arg(arguments, double);
			int negative = value < 0, digits = precision < 0 ? 6 : precision, count = 0, total;
			char text[96];
			unsigned long long whole;
			double fraction;
			if (negative)
				value = -value;
			whole = (unsigned long long)value;
			fraction = value - (double)whole;
			{
				char reversed[24];
				int used = 0;
				do { reversed[used++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
				while (used)
					text[count++] = reversed[--used];
			}
			if (digits) {
				text[count++] = '.';
				while (digits-- > 0 && count < 90) {
					int digit;
					fraction *= 10;
					digit = (int)fraction;
					text[count++] = (char)('0' + digit);
					fraction -= digit;
				}
			}
			total = count + (negative || plus ? 1 : 0);
			if (!left && !zero)
				pad(sink, width - total, ' ');
			if (negative) sink_put(sink, '-');
			else if (plus) sink_put(sink, '+');
			if (!left && zero)
				pad(sink, width - total, '0');
			for (total = 0; total < count; ++total)
				sink_put(sink, (wchar_t)text[total]);
			if (left)
				pad(sink, width - count - (negative || plus ? 1 : 0), ' ');
		} else {
			sink_put(sink, '%');
			sink_put(sink, spec);
		}
	}
#undef NEXT
#undef PEEK
	return (int)sink->length;
}

static int finish(Sink *sink)
{
	if (sink->stream)
		sink_flush(sink);
	else if (sink->capacity) {
		size_t at = sink->length < sink->capacity ? sink->length : sink->capacity - 1;
		if (sink->wide)
			sink->wide[at] = 0;
		else
			sink->narrow[at] = 0;
	}
	return (int)sink->length;
}

CRT int __stdio_common_vfprintf(unsigned long long options, void *stream, const char *text, void *locale, va_list arguments)
{
	Sink sink = {0};
	if (!text)
		return -1;
	format_options = options;
	sink.stream = stream;
	format(&sink, text, NULL, arguments);
	return finish(&sink);
}

CRT int __stdio_common_vfwprintf(unsigned long long options, void *stream, const wchar_t *text, void *locale, va_list arguments)
{
	Sink sink = {0};
	if (!text)
		return -1;
	format_options = options;
	sink.stream = stream;
	format(&sink, NULL, text, arguments);
	return finish(&sink);
}

CRT int __stdio_common_vsprintf(unsigned long long options, char *buffer, size_t count, const char *text, void *locale, va_list arguments)
{
	Sink sink = {0};
	if (!text)
		return -1;
	format_options = options;
	sink.narrow = buffer;
	sink.capacity = count;
	format(&sink, text, NULL, arguments);
	return finish(&sink);
}

CRT int __stdio_common_vswprintf(unsigned long long options, wchar_t *buffer, size_t count, const wchar_t *text, void *locale, va_list arguments)
{
	Sink sink = {0};
	if (!text)
		return -1;
	format_options = options;
	sink.wide = buffer;
	sink.capacity = count;
	format(&sink, NULL, text, arguments);
	return finish(&sink);
}

CRT int __stdio_common_vsnprintf_s(unsigned long long options, char *buffer, size_t size, size_t count, const char *text, void *locale, va_list arguments)
{
	return __stdio_common_vsprintf(options, buffer, count < size ? count + 1 : size, text, locale, arguments);
}

static void msvcrt_attach(void);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		msvcrt_attach();
	return TRUE;
}

#include "ucrtbase_io.c"
#include "ucrtbase_legacy.c"
#include "ucrtbase_msvcrt.c"
#include "ucrtbase_cpp.c"
#include "ucrtbase_cxx.c"
#include "ucrtbase_more.c"
