#include <windows.h>

// The Universal CRT entry points a larger program needs: file streams, descriptors, sorting, time formatting, math and jumps.
typedef struct { int sec, min, hour, mday, mon, year, wday, yday, isdst; } Tm;
extern void *fopen(const char *, const char *);
extern int fclose(void *);
extern size_t fread(void *, size_t, size_t, void *);
extern size_t fwrite(const void *, size_t, size_t, void *);
extern char *fgets(char *, int, void *);
extern int fseek(void *, long, int);
extern long ftell(void *);
extern int ungetc(int, void *);
extern int getc(void *);
extern int remove(const char *);
extern void qsort(void *, size_t, size_t, int (__cdecl *)(const void *, const void *));
extern size_t strftime(char *, size_t, const char *, const Tm *);
extern float powf(float, float);
extern float cosf(float);
extern float fmaxf(float, float);
extern float fminf(float, float);
extern long lroundf(float);
extern char *getenv(const char *);
extern int _open(const char *, int, ...);
extern int _read(int, void *, unsigned);
extern int _write(int, const void *, unsigned);
extern int _close(int);
extern long long _lseeki64(int, long long, int);
extern int __intrinsic_setjmp(void *, void *);
extern void longjmp(void *, int);
extern long long strtoll(const char *, char **, int);
extern int __stdio_common_vsnwprintf_s(unsigned long long, wchar_t *, size_t, size_t, const wchar_t *, void *, __builtin_va_list);
extern void *_aligned_malloc(size_t, size_t);
extern void _aligned_free(void *);
extern void free(void *);
struct time_of_day { long long seconds; long nanoseconds; };
extern int strcmp(const char *, const char *);
extern char *_mktemp(char *);
extern int *_errno(void);

static int temp_names(void)
{
	char names[26][32], exhausted[] = "crt-mktemp-XXXXXX", invalid[] = "crt-mktemp-XXXXX";
	for (int index = 0; index < 26; ++index) {
		lstrcpyA(names[index], "crt-mktemp-XXXXXX");
		if (_mktemp(names[index]) != names[index] || names[index][11] != 'a' + index ||
			GetFileAttributesA(names[index]) != INVALID_FILE_ATTRIBUTES) return 18;
		HANDLE file = CreateFileA(names[index], GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE) return 19;
		CloseHandle(file);
	}
	if (_mktemp(exhausted) || exhausted[0] || *_errno() != 17) return 20;
	for (int index = 0; index < 26; ++index)
		if (!DeleteFileA(names[index])) return 21;
	lstrcpyA(exhausted, "crt-mktemp-XXXXXX");
	if (_mktemp(exhausted) != exhausted || strcmp(exhausted, names[0])) return 22;
	if (_mktemp(invalid) || *_errno() != 22 || strcmp(invalid, "crt-mktemp-XXXXX")) return 23;
	if (_mktemp(NULL) || *_errno() != 22) return 24;
	return 0;
}

extern int __stdio_common_vsprintf(unsigned long long, char *, size_t, const char *, void *, __builtin_va_list);
extern int __stdio_common_vsscanf(unsigned long long, const char *, size_t, const char *, void *, __builtin_va_list);

static int print_float(char *buffer, const char *text, ...)
{
	__builtin_va_list arguments;
	__builtin_va_start(arguments, text);
	int result = __stdio_common_vsprintf(0, buffer, 128, text, 0, arguments);
	__builtin_va_end(arguments);
	return result;
}

static int scan_float(const char *input, const char *text, ...)
{
	__builtin_va_list arguments;
	__builtin_va_start(arguments, text);
	int result = __stdio_common_vsscanf(0, input, (size_t)-1, text, 0, arguments);
	__builtin_va_end(arguments);
	return result;
}

/* Floating-point text: exact digits rounded to nearest, ties to even, in the f, e and g forms, and decimal text read to the nearest double. */
static int float_conversions(void)
{
	static const struct { const char *format; double value; const char *expected; } cases[] = {
		{"%f", 3.14159265358979, "3.141593"}, {"%.2f", 2.675, "2.67"}, {"%.0f", 0.5, "0"}, {"%.0f", 2.5, "2"},
		{"%.3f", 1e-10, "0.000"}, {"%f", 1e20, "100000000000000000000.000000"}, {"%.1f", 0.95, "0.9"}, {"%.1f", 9.96, "10.0"},
		{"%e", 0.0, "0.000000e+00"}, {"%e", 610.0, "6.100000e+02"}, {"%.3E", 123456.0, "1.235E+05"}, {"%e", 1e300, "1.000000e+300"},
		{"%e", 5e-324, "4.940656e-324"}, {"%g", 0.0001, "0.0001"}, {"%g", 0.00001, "1e-05"}, {"%g", 123456.0, "123456"},
		{"%g", 1234567.0, "1.23457e+06"}, {"%g", 100.0, "100"}, {"%.3g", 3.14159, "3.14"}, {"%#g", 1.0, "1.00000"},
		{"%G", 1e-10, "1E-10"}, {"%.0g", 25.0, "2e+01"}, {"%.17g", 0.1, "0.10000000000000001"}, {"%#.0f", 7.0, "7."},
		{"%+.1f", 1.25, "+1.2"}, {"% .2f", 1.0, " 1.00"}, {"%08.2f", -3.5, "-0003.50"}, {"%-8.1f|", 2.0, "2.0     |"},
		{"%f", -0.0, "-0.000000"}, {"%.1e", 9.96, "1.0e+01"},
	};
	char buffer[128];
	for (unsigned index = 0; index < sizeof cases / sizeof cases[0]; ++index)
		if (print_float(buffer, cases[index].format, cases[index].value) < 0 || strcmp(buffer, cases[index].expected)) return 1 + (int)index;
	union { double value; unsigned long long bits; } infinity = {.bits = 0x7ff0000000000000ULL}, quiet = {.bits = 0xfff8000000000000ULL};
	if (print_float(buffer, "%f|%E|%5.1f", infinity.value, -infinity.value, quiet.value) < 0 || strcmp(buffer, "inf|-INF|-nan(ind)")) return 31;
	static const struct { const char *text; unsigned long long bits; int used; } reads[] = {
		{"0.1", 0x3fb999999999999aULL, 3}, {"1e23", 0x44b52d02c7e14af6ULL, 4}, {"2.2250738585072011e-308", 0x000fffffffffffffULL, 23},
		{"4.9e-324", 1ULL, 8}, {"1.7976931348623157e308", 0x7fefffffffffffffULL, 22}, {"  -12.5e-1x", 0xbff4000000000000ULL, 10},
		{"0x1.8p3", 0x4028000000000000ULL, 7}, {"Infinity", 0x7ff0000000000000ULL, 8}, {"123456789012345678901234567890", 0x45f8ee90ff6c373eULL, 30},
		{"9007199254740993", 0x4340000000000000ULL, 16}, {"1e", 0x3ff0000000000000ULL, 1}, {".5", 0x3fe0000000000000ULL, 2},
	};
	/* By address: the MinGW headers send strtod to a helper of their own. */
	double (__cdecl *strtod)(const char *, char **) = (void *)GetProcAddress(GetModuleHandleA("ucrtbase.dll"), "strtod");
	if (!strtod) return 32;
	for (unsigned index = 0; index < sizeof reads / sizeof reads[0]; ++index) {
		char *end;
		union { double value; unsigned long long bits; } read = {.value = strtod(reads[index].text, &end)};
		if (read.bits != reads[index].bits || end - reads[index].text != reads[index].used) return 40 + (int)index;
	}
	double first = 0, second = 0;
	if (scan_float("3.25 -1e-3", "%lf %lf", &first, &second) != 2 || first != 3.25 || second != -0.001) return 60;
	if (scan_float("12345", "%3lf", &first) != 1 || first != 123.0) return 61;
	return 0;
}

static int compare(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int __cdecl compare_context(void *context, const void *a, const void *b) { return compare(a, b); }
static int close_to(float value, float expected) { float d = value - expected; return d < 0.001f && d > -0.001f; }

/* The bounded secure print: what fits in count characters, -1 when cut short by count or _TRUNCATE, nothing when the buffer is too small.
   Programs built with the Windows SDK pass the legacy wide specifier option (4), where %s in a wide format is a wide string. */
static int print_with(unsigned long long options, wchar_t *buffer, size_t size, size_t count, const wchar_t *text, __builtin_va_list arguments)
{
	return __stdio_common_vsnwprintf_s(options, buffer, size, count, text, 0, arguments);
}

static int print_bounded(wchar_t *buffer, size_t size, size_t count, const wchar_t *text, ...)
{
	__builtin_va_list arguments;
	__builtin_va_start(arguments, text);
	const int result = print_with(4, buffer, size, count, text, arguments);
	__builtin_va_end(arguments);
	return result;
}

static int print_iso(wchar_t *buffer, size_t size, const wchar_t *text, ...)
{
	__builtin_va_list arguments;
	__builtin_va_start(arguments, text);
	const int result = print_with(0, buffer, size, (size_t)-1, text, arguments);
	__builtin_va_end(arguments);
	return result;
}

static int bounded_print(void)
{
	wchar_t buffer[8];
	if (print_bounded(buffer, 8, 7, L"%d-%s", 12, L"ab") != 5 || buffer[0] != '1' || buffer[5]) return 1;
	if (print_bounded(buffer, 8, (size_t)-1, L"%s", L"abcdefghij") != -1 || buffer[6] != 'g' || buffer[7]) return 2;
	if (print_bounded(buffer, 8, 3, L"%s", L"abcdef") != -1 || buffer[2] != 'c' || buffer[3]) return 3;
	if (print_bounded(buffer, 8, 20, L"%s", L"abcdefghij") != -1 || buffer[0]) return 4;
	if (print_bounded(buffer, 8, 7, L"%s\\%S", L"C:", "M") != 4 || buffer[1] != ':' || buffer[3] != 'M') return 5;
	if (print_iso(buffer, 8, L"%s-%S", "ab", L"cd") != 5 || buffer[1] != 'b' || buffer[4] != 'd') return 6;
	return 0;
}

/* Conversions, path parts and aligned blocks a larger program resolves by name: integers to text, case changes, a path split and rebuilt,
 * a bounded multibyte conversion, a block aligned at an offset, a grown block cleared and the time since 1970. */
static int named_helpers(void)
{
	HMODULE crt = GetModuleHandleA("ucrtbase.dll");
	int (__cdecl *to_text)(unsigned long long, char *, size_t, int) = (void *)GetProcAddress(crt, "_ui64toa_s");
	int (__cdecl *upper)(wchar_t *, size_t) = (void *)GetProcAddress(crt, "_wcsupr_s");
	int (__cdecl *split)(const wchar_t *, wchar_t *, size_t, wchar_t *, size_t, wchar_t *, size_t, wchar_t *, size_t) =
		(void *)GetProcAddress(crt, "_wsplitpath_s");
	int (__cdecl *make)(wchar_t *, size_t, const wchar_t *, const wchar_t *, const wchar_t *, const wchar_t *) = (void *)GetProcAddress(crt, "_wmakepath_s");
	int (__cdecl *to_wide)(size_t *, wchar_t *, size_t, const char *, size_t) = (void *)GetProcAddress(crt, "mbstowcs_s");
	void *(__cdecl *offset_block)(size_t, size_t, size_t) = (void *)GetProcAddress(crt, "_aligned_offset_malloc");
	void (__cdecl *free_aligned)(void *) = (void *)GetProcAddress(crt, "_aligned_free");
	void *(__cdecl *grow)(void *, size_t, size_t) = (void *)GetProcAddress(crt, "_recalloc");
	int (__cdecl *now)(struct time_of_day *, int) = (void *)GetProcAddress(crt, "_timespec64_get");
	if (!to_text || !upper || !split || !make || !to_wide || !offset_block || !free_aligned || !grow || !now) return 1;
	char digits[24];
	if (to_text(18446744073709551615ULL, digits, sizeof digits, 10) || digits[0] != '1' || digits[19] != '5' || digits[20]) return 2;
	if (to_text(255, digits, 3, 16) || digits[0] != 'f' || digits[2] || to_text(4096, digits, 3, 10) != 34 || digits[0]) return 3;
	wchar_t text[8] = L"abc";
	if (upper(text, 8) || text[0] != L'A' || text[2] != L'C' || upper(text, 3) != 22 || text[0]) return 4;
	wchar_t drive[4], directory[32], name[16], extension[8], whole[64];
	if (split(L"C:\\Program Files\\Office.C2R\\setup.exe", drive, 4, directory, 32, name, 16, extension, 8) || drive[1] != L':' ||
		lstrcmpW(directory, L"\\Program Files\\Office.C2R\\") || lstrcmpW(name, L"setup") || lstrcmpW(extension, L".exe")) return 5;
	if (split(L"C:\\a\\long-name.txt", drive, 4, directory, 32, name, 4, extension, 8) != 34 || drive[0] || name[0]) return 6;
	if (make(whole, 64, L"D", L"\\Temp", L"log", L"txt") || lstrcmpW(whole, L"D:\\Temp\\log.txt")) return 7;
	if (make(whole, 8, L"D", L"\\Temp", L"log", L"txt") != 34 || whole[0]) return 8;
	size_t converted = 0;
	if (to_wide(&converted, NULL, 0, "office", 0) || converted != 7) return 9;
	if (to_wide(&converted, whole, 4, "office", (size_t)-1) != 80 || converted != 4 || lstrcmpW(whole, L"off")) return 10;
	if (to_wide(&converted, whole, 4, "office", 6) != 34 || whole[0]) return 11;
	char *block = offset_block(100, 64, 8);
	if (!block || ((unsigned long long)(block + 8) & 63)) return 12;
	free_aligned(block);
	unsigned char *bytes = grow(NULL, 4, 1);
	if (!bytes) return 13;
	bytes[0] = 7;
	bytes = grow(bytes, 64, 1);
	if (!bytes || bytes[0] != 7 || bytes[63]) return 14;
	free(bytes);
	struct time_of_day time = {0, -1};
	if (now(&time, 1) != 1 || time.seconds < 1700000000LL || time.nanoseconds < 0 || time.nanoseconds > 999999999 || now(&time, 2)) return 15;
	return 0;
}

/* The entry points the Microsoft 365 libraries import besides; looked up by name, since import libraries lag behind the runtime. */
static int office_helpers(void)
{
	HMODULE crt = GetModuleHandleA("ucrtbase.dll");
#define FIND(name, type) __typeof__(type) name = (type)(void *)GetProcAddress(crt, #name); if (!name) return 1;
	FIND(hypot, double (__cdecl *)(double, double))
	FIND(fmax, double (__cdecl *)(double, double))
	FIND(nextafter, double (__cdecl *)(double, double))
	FIND(nextafterf, float (__cdecl *)(float, float))
	FIND(cbrt, double (__cdecl *)(double))
	FIND(_ecvt_s, int (__cdecl *)(char *, size_t, double, int, int *, int *))
	FIND(_gcvt_s, int (__cdecl *)(char *, size_t, double, int))
	FIND(_ultoa_s, int (__cdecl *)(unsigned long, char *, size_t, int))
	FIND(_itoa_s, int (__cdecl *)(int, char *, size_t, int))
	FIND(_mbsrchr, unsigned char *(__cdecl *)(const unsigned char *, unsigned))
	FIND(_mbsstr, unsigned char *(__cdecl *)(const unsigned char *, const unsigned char *))
	FIND(strncat_s, int (__cdecl *)(char *, size_t, const char *, size_t))
	FIND(_splitpath_s, int (__cdecl *)(const char *, char *, size_t, char *, size_t, char *, size_t, char *, size_t))
	FIND(fesetround, int (__cdecl *)(int))
	FIND(fegetround, int (__cdecl *)(void))
	FIND(qsort_s, void (__cdecl *)(void *, size_t, size_t, int (__cdecl *)(void *, const void *, const void *), void *))
	FIND(_o_malloc, void *(__cdecl *)(size_t))
	FIND(strtold, double (__cdecl *)(const char *, char **))
	FIND(_wgetenv_s, int (__cdecl *)(size_t *, wchar_t *, size_t, const wchar_t *))
#undef FIND
	if (hypot(3, 4) != 5 || fmax(1, 2) != 2 || fmax(0.0 / 0.0, 2) != 2) return 2;
	if (nextafter(1.0, 2.0) != 1.0 + 2.220446049250313e-16 || nextafter(0.0, -1.0) >= 0 || nextafterf(1.0f, 0.0f) >= 1.0f) return 3;
	if (cbrt(27.0) != 3.0 || cbrt(-8.0) != -2.0) return 4;
	char text[32];
	int point, sign;
	if (_ecvt_s(text, sizeof text, -3.14159, 4, &point, &sign) || strcmp(text, "3142") || point != 1 || !sign) return 5;
	if (_ecvt_s(text, sizeof text, 0.000999, 2, &point, &sign) || strcmp(text, "10") || point != -2 || sign) return 6;
	if (_gcvt_s(text, sizeof text, 1234.0, 10) || strcmp(text, "1234.") || _gcvt_s(text, sizeof text, 0.5, 6) || strcmp(text, "0.5")) return 7;
	if (_gcvt_s(text, sizeof text, 1.5e20, 6) || strcmp(text, "1.5e+20") || _gcvt_s(text, 4, 123456.0, 6) != 34 || text[0]) return 8;
	if (_ultoa_s(4294967295UL, text, sizeof text, 16) || strcmp(text, "ffffffff") || _itoa_s(-42, text, sizeof text, 10) || strcmp(text, "-42")) return 9;
	if (_itoa_s(12345, text, 3, 10) != 34 || text[0]) return 10;
	const unsigned char *path = (const unsigned char *)"C:\\Office\\root\\WWLIB.DLL";
	if (_mbsrchr(path, '\\') != path + 14 || _mbsstr(path, (const unsigned char *)"root") != path + 10) return 11;
	lstrcpyA(text, "ab");
	if (strncat_s(text, 8, "cdef", 2) || strcmp(text, "abcd") || strncat_s(text, 6, "efgh", (size_t)-1) != 80 || strcmp(text, "abcde")) return 12;
	char drive[4], directory[32], name[16], extension[8];
	if (_splitpath_s("D:\\Temp\\log.txt", drive, 4, directory, 32, name, 16, extension, 8) || strcmp(drive, "D:") ||
		strcmp(directory, "\\Temp\\") || strcmp(name, "log") || strcmp(extension, ".txt")) return 13;
	if (fesetround(0x300) || fegetround() != 0x300 || fesetround(0) || fegetround() != 0) return 14;
	int numbers[5] = {4, 1, 5, 2, 3};
	qsort_s(numbers, 5, sizeof numbers[0], compare_context, NULL);
	for (int at = 0; at < 5; ++at)
		if (numbers[at] != at + 1) return 15;
	if (strtold("2.5", NULL) != 2.5) return 17;
	size_t length = 0;
	wchar_t value[8];
	SetEnvironmentVariableW(L"RAVEN_CRT_WIDE", L"wide");
	if (_wgetenv_s(&length, value, 8, L"RAVEN_CRT_WIDE") || length != 5 || value[0] != L'w' || _wgetenv_s(&length, value, 3, L"RAVEN_CRT_WIDE") != 34) return 18;
	void *block = _o_malloc(16);
	if (!block) return 16;
	free(block);
	return 0;
}

/* The secure scanners take a capacity after each string target; a text that does not fit empties the target and stops. */
extern int __stdio_common_vswscanf(unsigned long long, const wchar_t *, size_t, const wchar_t *, void *, __builtin_va_list);
static int secure_scan_call(const wchar_t *input, const wchar_t *format, ...)
{
	__builtin_va_list arguments;
	__builtin_va_start(arguments, format);
	int result = __stdio_common_vswscanf(1, input, (size_t)-1, format, NULL, arguments);
	__builtin_va_end(arguments);
	return result;
}

static int secure_scan(void)
{
	wchar_t word[6], after[6];
	if (secure_scan_call(L"en-US rest", L"%5[^-]-%s", word, 6, after, 6) != 2 || word[0] != 'e' || word[2] != 0 || after[1] != 'S' || after[2] != 0)
		return 1;
	word[0] = 'x';
	if (secure_scan_call(L"toolongword", L"%s", word, 6) != 0 || word[0] != 0)
		return 2;
	return 0;
}

/* The entry points the .NET host and runtime import: an error message in UTF-16, a fused multiply-add, the time zone as functions. */
static int runtime_helpers(void)
{
	HMODULE crt = GetModuleHandleA("ucrtbase.dll");
	int (__cdecl *wide_error)(wchar_t *, size_t, int) = (void *)GetProcAddress(crt, "_wcserror_s");
	double (__cdecl *fused)(double, double, double) = (void *)GetProcAddress(crt, "fma");
	float (__cdecl *fused_float)(float, float, float) = (void *)GetProcAddress(crt, "fmaf");
	int (__cdecl *flush_all)(void) = (void *)GetProcAddress(crt, "_flushall");
	long *(__cdecl *zone)(void) = (void *)GetProcAddress(crt, "__timezone");
	char **(__cdecl *zone_names)(void) = (void *)GetProcAddress(crt, "__tzname");
	int (__cdecl *get_zone)(long *) = (void *)GetProcAddress(crt, "_get_timezone");
	if (!wide_error || !fused || !fused_float || !flush_all || !zone || !zone_names || !get_zone) return 1;
	wchar_t text[64];
	if (wide_error(text, 64, 2) || lstrcmpW(text, L"No such file or directory")) return 2;
	if (wide_error(text, 4, 2) || lstrcmpW(text, L"No ")) return 3;
	/* 0.1 * 10 rounds to 1 on its own; fused, the product keeps the 2^-54 that 0.1 is off by. */
	volatile double tenth = 0.1, ten = 10, one = 1;
	if (fused(tenth, ten, -one) != 0x1p-54) return 4;
	if (fused_float(2.0f, 3.0f, 1.0f) != 7.0f || fused(2, 3, 1) != 7) return 5;
	if (flush_all() < 0) return 6;
	long seconds = 0;
	if (get_zone(&seconds) || *zone() != seconds || !zone_names()[0] || !zone_names()[1]) return 7;
	return 0;
}

int raven_entry(void)
{
	char line[64], big[64];
	void *file;
	int numbers[7] = {5, 3, 9, 1, 7, 2, 8}, fd;
	Tm when = {9, 7, 14, 5, 2, 124, 2, 64, 0};
	__attribute__((aligned(16))) char jump[256];
	volatile int reached = 0;
	void *block;
	size_t count;
	file = fopen("crt-io.txt", "w");
	if (!file || fwrite("alpha\nbeta\n", 1, 11, file) != 11 || fclose(file)) return 1;
	file = fopen("crt-io.txt", "rb");
	if (!file || (count = fread(big, 1, sizeof big, file)) != 13 || big[5] != '\r') return 2;
	fclose(file);
	file = fopen("crt-io.txt", "r");
	if (!file || !fgets(line, sizeof line, file) || strcmp(line, "alpha\n") || ftell(file) != 7) return 3;
	if (!fgets(line, sizeof line, file) || strcmp(line, "beta\n") || fgets(line, sizeof line, file)) return 4;
	if (fseek(file, 2, 0) || getc(file) != 'p' || ungetc('Q', file) != 'Q' || getc(file) != 'Q' || getc(file) != 'h') return 5;
	fclose(file);
	fd = _open("crt-io.txt", 0x8000);
	if (fd < 3 || _lseeki64(fd, 6, 0) != 6 || _read(fd, line, 5) != 5 || line[0] != '\n' || line[1] != 'b') return 6;
	_close(fd);
	if (remove("crt-io.txt") || fopen("crt-io.txt", "r")) return 7;
	qsort(numbers, 7, sizeof numbers[0], compare);
	for (int index = 1; index < 7; ++index)
		if (numbers[index - 1] > numbers[index]) return 8;
	if (!strftime(big, sizeof big, "%Y-%m-%d %H:%M:%S %a %b %j", &when) || strcmp(big, "2024-03-05 14:07:09 Tue Mar 065") != 0) return 9;
	if (powf(2.0f, 10.0f) < 1023.9f || powf(2.0f, 10.0f) > 1024.1f || powf(-2.0f, 3.0f) > -7.99f) return 10;
	if (!close_to(cosf(0.0f), 1.0f) || !close_to(cosf(3.14159265f), -1.0f) || !close_to(cosf(1.0f), 0.5403023f)) return 11;
	if (fmaxf(1.0f, 2.0f) != 2.0f || fminf(1.0f, 2.0f) != 1.0f || lroundf(2.5f) != 3 || lroundf(-2.5f) != -3) return 12;
	SetEnvironmentVariableA("RAVEN_CRT_PROBE", "value");
	if (!getenv("RAVEN_CRT_PROBE") || strcmp(getenv("RAVEN_CRT_PROBE"), "value") || getenv("RAVEN_CRT_ABSENT")) return 13;
	/* Older MinGW import libraries omit this UCRT export. */
	size_t (__cdecl *limited_length)(const char *, size_t) = (void *)GetProcAddress(GetModuleHandleA("ucrtbase.dll"), "strnlen");
	if (!limited_length || strtoll("-9223372036854775808", 0, 10) != (long long)0x8000000000000000ULL || limited_length("abcdef", 3) != 3) return 14;
	/* The locale forms of the case-insensitive compares, which the extended events engine of Analysis Services imports. */
	int (__cdecl *compare_locale)(const char *, const char *, size_t, _locale_t) = (void *)GetProcAddress(GetModuleHandleA("ucrtbase.dll"), "_strnicmp_l");
	int (__cdecl *whole_locale)(const char *, const char *, _locale_t) = (void *)GetProcAddress(GetModuleHandleA("ucrtbase.dll"), "_stricmp_l");
	if (!compare_locale || !whole_locale || compare_locale("Event.X", "EVENT.y", 6, NULL) || !compare_locale("abc", "abd", 3, NULL) ||
		whole_locale("Session", "SESSION", NULL) || whole_locale("a", "b", NULL) >= 0) return 61;
	block = _aligned_malloc(100, 64);
	if (!block || ((unsigned long long)block & 63)) return 15;
	_aligned_free(block);
	if (!__intrinsic_setjmp(jump, 0)) {
		reached = 1;
		longjmp(jump, 7);
		return 16;
	}
	if (reached != 1) return 17;
	if (bounded_print() != 0) return 18;
	/* The CRT heap is the process heap, and a block from malloc belongs to it. */
	intptr_t (__cdecl *heap_handle)(void) = (void *)GetProcAddress(GetModuleHandleA("ucrtbase.dll"), "_get_heap_handle");
	block = malloc(24);
	if (!heap_handle || heap_handle() != (intptr_t)GetProcessHeap() || HeapSize((HANDLE)heap_handle(), 0, block) != 24) return 60;
	free(block);
	if (named_helpers() != 0) return 20 + named_helpers();
	if ((count = office_helpers())) return 100 + (int)count;
	if ((count = secure_scan()))
		return 200 + (int)count;
	if ((count = runtime_helpers()))
		return 300 + (int)count;
	if ((count = float_conversions()))
		return 400 + (int)count;
	return temp_names();
}
