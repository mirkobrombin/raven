#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>

/* The C runtime entry points large Visual C++ 2019 and 2022 programs and their libraries (Qt 6, OpenCASCADE, OpenCV) import, for 64-bit
   and 32-bit callers: each is exported by ucrtbase.dll, and the lock-free streams, path splitting, the locale conversions, the C99 math,
   the time text, the stream limit and the C++ runtime's helpers answer as the Universal CRT does, failures included. The exit code names
   the first check that failed; 100 and up names an export ucrtbase.dll does not have. */

static const char *const names[] = {
	"___mb_cur_max_l_func", "_byteswap_uint64", "_creat", "_findfirst64i32", "_findnext64i32", "_fread_nolock", "_fseek_nolock",
	"_fseeki64_nolock", "_ftell_nolock", "_ftelli64_nolock", "_fwrite_nolock", "_getmaxstdio", "_mbtowc_l", "_o___std_exception_copy",
	"_o___std_exception_destroy", "_o___std_type_info_destroy_list", "_o___stdio_common_vsprintf", "_o__fpclass", "_o__get_osfhandle",
	"_o_isalnum", "_o_isalpha", "_o_isdigit", "_o_isprint", "_o_toupper", "_set_thread_local_invalid_parameter_handler", "_setmaxstdio",
	"_splitpath", "_strftime_l", "_strrev", "_strtof_l", "_strtoi64_l", "_strtold_l", "_strtoui64_l", "_umask", "_wfreopen_s", "_wtempnam",
	"_wtmpnam", "asctime", "erf", "erfc", "remainder", "tgamma", "wcrtomb_s",
};

typedef struct { int second, minute, hour, day, month, year, weekday, yearday, daylight; } calendar;
typedef struct { const char *what; char owned; } exception_data;
typedef struct { unsigned attributes; long long created, accessed, written; unsigned long size; char name[260]; } found_file;
typedef void (__cdecl *handler_function)(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t);

static HMODULE crt;
static FARPROC find(const char *name) { return GetProcAddress(crt, name); }

static int close_to(double value, double expected, double tolerance)
{
	const double difference = value > expected ? value - expected : expected - value;
	const double scale = expected < 0 ? -expected : expected;
	return difference <= tolerance * (scale > 1e-300 ? scale : 1.0);
}

static int same(const char *a, const char *b) { return lstrcmpA(a, b) == 0; }

static void __cdecl ignore_parameter(const wchar_t *expression, const wchar_t *function, const wchar_t *file, unsigned line, uintptr_t reserved) {}

static int streams(void)
{
	wchar_t *(__cdecl *temporary_name)(wchar_t *) = (void *)find("_wtmpnam");
	wchar_t *(__cdecl *temporary_in)(const wchar_t *, const wchar_t *) = (void *)find("_wtempnam");
	void *(__cdecl *open)(const wchar_t *, const wchar_t *) = (void *)find("_wfopen");
	int (__cdecl *close)(void *) = (void *)find("fclose");
	void (__cdecl *release)(void *) = (void *)find("free");
	size_t (__cdecl *write)(const void *, size_t, size_t, void *) = (void *)find("_fwrite_nolock");
	size_t (__cdecl *read)(void *, size_t, size_t, void *) = (void *)find("_fread_nolock");
	int (__cdecl *seek)(void *, long, int) = (void *)find("_fseek_nolock");
	int (__cdecl *seek64)(void *, long long, int) = (void *)find("_fseeki64_nolock");
	long (__cdecl *tell)(void *) = (void *)find("_ftell_nolock");
	long long (__cdecl *tell64)(void *) = (void *)find("_ftelli64_nolock");
	intptr_t (__cdecl *find_first)(const char *, found_file *) = (void *)find("_findfirst64i32");
	int (__cdecl *find_next)(intptr_t, found_file *) = (void *)find("_findnext64i32");
	int (__cdecl *find_close)(intptr_t) = (void *)find("_findclose");
	int (__cdecl *reopen)(void **, const wchar_t *, const wchar_t *, void *) = (void *)find("_wfreopen_s");
	int (__cdecl *create)(const char *, int) = (void *)find("_creat");
	int (__cdecl *close_descriptor)(int) = (void *)find("_close");
	intptr_t (__cdecl *handle_of)(int) = (void *)find("_o__get_osfhandle");
	int (__cdecl *maximum)(void) = (void *)find("_getmaxstdio");
	int (__cdecl *set_maximum)(int) = (void *)find("_setmaxstdio");
	int (__cdecl *mask)(int) = (void *)find("_umask");
	static wchar_t path[MAX_PATH];
	static char narrow[MAX_PATH];
	char text[8] = {0};

	if (!open || !close || !release || !find_close || !close_descriptor)
		return 10;
	if (!temporary_name(path) || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
		return 11;
	wchar_t *named = temporary_in(NULL, L"rv");
	if (!named || GetFileAttributesW(named) != INVALID_FILE_ATTRIBUTES)
		return 12;
	release(named);
	void *stream = open(path, L"w+b");
	if (!stream)
		return 13;
	if (write("Raven", 1, 5, stream) != 5 || tell(stream) != 5 || seek(stream, 1, 0) || tell(stream) != 1)
		return 14;
	if (read(text, 1, 4, stream) != 4 || !same(text, "aven") || seek64(stream, 0, 2) || tell64(stream) != 5)
		return 15;
	if (seek(stream, 0, 7) == 0)
		return 16;
	void *again = NULL;
	if (reopen(&again, path, L"rb", stream) || !again || read(text, 1, 5, again) != 5 || text[0] != 'R')
		return 17;
	if (reopen(NULL, path, L"rb", again) != 22)
		return 18;
	close(again);

	WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, MAX_PATH, NULL, NULL);
	found_file found;
	const intptr_t search = find_first(narrow, &found);
	if (search == -1 || found.size != 5 || (found.attributes & FILE_ATTRIBUTE_DIRECTORY) || found.written <= 0)
		return 19;
	if (find_next(search, &found) != -1 || find_close(search))
		return 20;
	lstrcatA(narrow, ".none");
	if (find_first(narrow, &found) != -1)
		return 21;
	narrow[lstrlenA(narrow) - 5] = 0;

	const int descriptor = create(narrow, 0600);
	if (descriptor < 0 || handle_of(descriptor) == -1 || close_descriptor(descriptor))
		return 22;
	HANDLE emptied = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (emptied == INVALID_HANDLE_VALUE || GetFileSize(emptied, NULL) != 0)
		return 23;
	CloseHandle(emptied);
	DeleteFileW(path);

	if (maximum() != 512 || set_maximum(2048) != 2048 || maximum() != 2048 || set_maximum(10) != -1 || maximum() != 2048)
		return 24;
	set_maximum(512);
	if (mask(0200) != 0 || mask(0) != 0200)
		return 25;
	return 0;
}

static int strings(void)
{
	void (__cdecl *split)(const char *, char *, char *, char *, char *) = (void *)find("_splitpath");
	char *(__cdecl *reverse)(char *) = (void *)find("_strrev");
	long long (__cdecl *to_i64)(const char *, char **, int, void *) = (void *)find("_strtoi64_l");
	unsigned long long (__cdecl *to_u64)(const char *, char **, int, void *) = (void *)find("_strtoui64_l");
	float (__cdecl *to_float)(const char *, char **, void *) = (void *)find("_strtof_l");
	double (__cdecl *to_long_double)(const char *, char **, void *) = (void *)find("_strtold_l");
	int (__cdecl *to_wide)(wchar_t *, const char *, size_t, void *) = (void *)find("_mbtowc_l");
	int (__cdecl *longest)(void *) = (void *)find("___mb_cur_max_l_func");
	int (__cdecl *narrow_of)(size_t *, char *, size_t, wchar_t, void *) = (void *)find("wcrtomb_s");
	unsigned long long (__cdecl *swap)(unsigned long long) = (void *)find("_byteswap_uint64");
	int (__cdecl *upper)(int) = (void *)find("_o_toupper");
	int (__cdecl *digit)(int) = (void *)find("_o_isdigit");
	int (__cdecl *alpha)(int) = (void *)find("_o_isalpha");
	int (__cdecl *alnum)(int) = (void *)find("_o_isalnum");
	int (__cdecl *printable)(int) = (void *)find("_o_isprint");
	char drive[8], folder[64], name[64], extension[16], *end;

	split("C:\\Program Files\\Chief\\chief.exe", drive, folder, name, extension);
	if (!same(drive, "C:") || !same(folder, "\\Program Files\\Chief\\") || !same(name, "chief") || !same(extension, ".exe"))
		return 30;
	split("lib.d/archive.tar.gz", drive, folder, name, NULL);
	if (!same(drive, "") || !same(folder, "lib.d/") || !same(name, "archive.tar"))
		return 31;
	split("folder.d\\name", NULL, NULL, name, extension);
	if (!same(name, "name") || !same(extension, ""))
		return 32;
	char word[] = "Raven", none[] = "";
	if (reverse(word) != word || !same(word, "nevaR") || !same(reverse(none), ""))
		return 33;
	if (to_i64("-9223372036854775807x", &end, 10, NULL) != -9223372036854775807LL || *end != 'x')
		return 34;
	if (to_u64("ffffffffffffffff", &end, 16, NULL) != 0xffffffffffffffffULL || *end || to_u64("zz", &end, 10, NULL) != 0)
		return 35;
	if (to_float("1.5e2f", &end, NULL) != 150.0f || *end != 'f' || to_long_double("-2.25", &end, NULL) != -2.25 || *end)
		return 36;
	wchar_t wide = 0;
	if (to_wide(&wide, "A", 1, NULL) != 1 || wide != L'A' || longest(NULL) < 1)
		return 37;
	size_t length = 0;
	char bytes[8] = {0};
	if (narrow_of(&length, bytes, sizeof bytes, L'Z', NULL) || length != 1 || bytes[0] != 'Z')
		return 38;
	if (narrow_of(&length, bytes, 0, L'Z', NULL) != 34)
		return 39;
	if (swap(0x0102030405060708ULL) != 0x0807060504030201ULL)
		return 40;
	if (upper('q') != 'Q' || upper('7') != '7' || !digit('7') || digit('a') || !alpha('a') || !alnum('0') || alnum('-') ||
		!printable(' ') || printable('\n'))
		return 41;
	return 0;
}

static int numbers(void)
{
	double (__cdecl *erf)(double) = (void *)find("erf");
	double (__cdecl *erfc)(double) = (void *)find("erfc");
	double (__cdecl *gamma)(double) = (void *)find("tgamma");
	double (__cdecl *remainder)(double, double) = (void *)find("remainder");
	int (__cdecl *classify)(double) = (void *)find("_o__fpclass");

	if (erf(0.0) != 0.0 || !close_to(erf(0.25), 0.27632639016823696, 1e-14) || !close_to(erf(0.5), 0.52049987781304654, 1e-14) ||
		!close_to(erf(-1.0), -0.84270079294971487, 1e-14) || !close_to(erf(3.0), 0.99997790950300141, 1e-14) || erf(30.0) != 1.0)
		return 50;
	if (!close_to(erfc(0.25), 0.72367360983176304, 1e-14) || !close_to(erfc(2.0), 0.0046777349810472658, 1e-13) ||
		!close_to(erfc(6.0), 2.1519736712498913e-17, 1e-12) || !close_to(erfc(-2.0), 1.9953222650189527, 1e-14) || erfc(30.0) != 0.0)
		return 51;
	if (!close_to(gamma(5.0), 24.0, 1e-13) || !close_to(gamma(0.5), 1.7724538509055160, 1e-13) || !close_to(gamma(-1.5), 2.3632718012073547, 1e-13) ||
		!close_to(gamma(20.0), 1.21645100408832e17, 1e-12))
		return 52;
	const double pole = gamma(-2.0), overflow = gamma(200.0);
	if (pole == pole || overflow != overflow || overflow < 1e308)
		return 53;
	if (remainder(5.0, 3.0) != -1.0 || remainder(7.0, 2.0) != -1.0 || remainder(-7.5, 2.0) != 0.5 || remainder(4.0, 1e300) != 4.0)
		return 54;
	const double undefined = remainder(1.0, 0.0);
	if (undefined == undefined)
		return 55;
	if (classify(1.0) != 0x0100 || classify(-0.0) != 0x0020)
		return 56;
	return 0;
}

static int runtime(void)
{
	char *(__cdecl *text_of)(const calendar *) = (void *)find("asctime");
	size_t (__cdecl *format_time)(char *, size_t, const char *, const calendar *, void *) = (void *)find("_strftime_l");
	void (__cdecl *copy)(const exception_data *, exception_data *) = (void *)find("_o___std_exception_copy");
	void (__cdecl *destroy)(exception_data *) = (void *)find("_o___std_exception_destroy");
	void (__cdecl *destroy_list)(PSLIST_HEADER) = (void *)find("_o___std_type_info_destroy_list");
	handler_function (__cdecl *set_handler)(handler_function) = (void *)find("_set_thread_local_invalid_parameter_handler");
	int (__cdecl *vsprintf)(unsigned long long, char *, size_t, const char *, void *, va_list) = (void *)find("_o___stdio_common_vsprintf");
	void *(__cdecl *allocate)(size_t) = (void *)find("malloc");
	const calendar millennium = {0, 30, 12, 1, 0, 100, 6, 0, 0}, wrong = {0, 0, 0, 1, 12, 100, 6, 0, 0};
	char text[32];

	if (!allocate || !vsprintf)
		return 60;
	const char *line = text_of(&millennium);
	if (!line || !same(line, "Sat Jan 01 12:30:00 2000\n") || text_of(&wrong) || text_of(NULL))
		return 61;
	if (format_time(text, sizeof text, "%Y-%m-%d", &millennium, NULL) != 10 || !same(text, "2000-01-01"))
		return 62;

	exception_data borrowed = {"static", 0}, owned = {NULL, 1}, copied = {NULL, 0};
	copy(&borrowed, &copied);
	if (copied.what != borrowed.what || copied.owned)
		return 63;
	owned.what = "kept";
	copy(&owned, &copied);
	if (copied.what == owned.what || !copied.owned || !same(copied.what, "kept"))
		return 64;
	destroy(&copied);
	if (copied.what || copied.owned)
		return 65;

	static SLIST_HEADER names_list;
	InitializeSListHead(&names_list);
	for (int at = 0; at < 3; ++at) {
		PSLIST_ENTRY entry = allocate(64);
		if (!entry)
			return 66;
		InterlockedPushEntrySList(&names_list, entry);
	}
	destroy_list(&names_list);
	if (QueryDepthSList(&names_list))
		return 67;

	if (set_handler(ignore_parameter) || set_handler(NULL) != ignore_parameter || set_handler(NULL))
		return 68;
	return 0;
}

int raven_entry(void)
{
	crt = LoadLibraryA("ucrtbase.dll");
	if (!crt)
		return 1;
	for (size_t at = 0; at < sizeof names / sizeof names[0]; ++at)
		if (!find(names[at]))
			return 100 + (int)at;
	int result = streams();
	if (!result)
		result = strings();
	if (!result)
		result = numbers();
	if (!result)
		result = runtime();
	return result;
}
