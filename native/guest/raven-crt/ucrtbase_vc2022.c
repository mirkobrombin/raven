/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The entry points that large Visual C++ 2019 and 2022 programs and the libraries they ship (Qt 6, OpenCASCADE, OpenCV) import beyond the
 * rest: the exception and type_info helpers the C++ runtime libraries take under their _o_ names, the lock-free stream functions, more
 * conversions under a locale, file searches with 32-bit sizes, wide temporary names, path splitting and the C99 math functions erf, erfc,
 * tgamma and remainder. Also the msvcrt names libraries built by MinGW-w64 import (VLC's plugins among them): the secure scans, fscanf,
 * _vsnprintf_s, file searches with 64-bit sizes and pipes to a command. Then the programs a program starts, which Python 3.14's
 * python314.dll imports: system and _wsystem, the wide spawn and exec functions and _cwait. Included by ucrtbase.c, after the other parts,
 * whose helpers it shares. */

__asm__(".section .drectve\n\t.ascii \""
	" -export:_o___std_exception_copy=raven_std_exception_copy -export:_o___std_exception_destroy=raven_std_exception_destroy"
	" -export:_o___std_type_info_destroy_list=raven_std_type_info_destroy_list"
	" -export:_strtold_l=_strtod_l -export:_strtoi64_l=raven_strtoi64_l -export:_strtoui64_l=raven_strtoui64_l"
	"\"\n\t.text");

/* ---- the C++ runtime's helpers ---- */

/* The text an exception object carries, and whether it owns a copy: what std::exception keeps, which the C++ runtime copies and frees. */
typedef struct {
	const char *what;
	char owned;
} RavenExceptionData;

void raven_std_exception_copy(const RavenExceptionData *from, RavenExceptionData *to)
{
	if (from->owned && from->what) {
		to->what = _strdup(from->what);
		to->owned = to->what != NULL;
	} else {
		to->what = from->what;
		to->owned = 0;
	}
}

void raven_std_exception_destroy(RavenExceptionData *data)
{
	if (data->owned)
		free((void *)data->what);
	data->what = NULL;
	data->owned = 0;
}

/* The names type_info built for its objects, on a lock-free list that the C++ runtime frees when its library unloads. */
void raven_std_type_info_destroy_list(PSLIST_HEADER root)
{
	PSLIST_ENTRY entry = InterlockedFlushSList(root);
	while (entry) {
		PSLIST_ENTRY next = entry->Next;
		free(entry);
		entry = next;
	}
}

/* The thread's own invalid parameter handler, which takes the place of the process's while it is set. */
static DWORD invalid_parameter_slot = TLS_OUT_OF_INDEXES;

CRT invalid_parameter_handler _set_thread_local_invalid_parameter_handler(invalid_parameter_handler handler)
{
	if (invalid_parameter_slot == TLS_OUT_OF_INDEXES) {
		DWORD slot = TlsAlloc();
		if (InterlockedCompareExchange((LONG *)&invalid_parameter_slot, (LONG)slot, (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES)
			TlsFree(slot);
	}
	const invalid_parameter_handler previous = (invalid_parameter_handler)TlsGetValue(invalid_parameter_slot);
	TlsSetValue(invalid_parameter_slot, (void *)handler);
	return previous;
}

/* ---- conversions under a locale: the runtime has the C locale only, so each is the plain function ---- */

long long raven_strtoi64_l(const char *text, char **end, int base, _locale_t locale) { return _strtoi64(text, end, base); }
unsigned long long raven_strtoui64_l(const char *text, char **end, int base, _locale_t locale) { return _strtoui64(text, end, base); }
CRT float _strtof_l(const char *text, char **end, _locale_t locale) { return (float)raven_strtod(text, end); }
CRT int _mbtowc_l(wchar_t *wide, const char *bytes, size_t count, _locale_t locale) { return mbtowc(wide, bytes, count); }
CRT int ___mb_cur_max_l_func(_locale_t locale) { return ___mb_cur_max_func(); }
CRT size_t _strftime_l(char *to, size_t capacity, const char *format, const void *time, _locale_t locale)
{
	return strftime(to, capacity, format, time);
}

CRT int wcrtomb_s(size_t *length, char *to, size_t size, wchar_t c, void *state)
{
	char bytes[8];
	const size_t written = wcrtomb(bytes, c, state);
	if (written == (size_t)-1) {
		if (length)
			*length = (size_t)-1;
		return 42;
	}
	if (to) {
		if (size < written) {
			if (size)
				to[0] = 0;
			return 34;
		}
		memcpy(to, bytes, written);
	}
	if (length)
		*length = written;
	return 0;
}

/* ---- time ---- */

CRT char *asctime(const RavenTime *time)
{
	static char text[26];
	if (asctime_s(text, sizeof text, time)) {
		set_errno(22);
		return NULL;
	}
	return text;
}

/* The 32-bit forms of api-ms-win-crt-time: the 64-bit calendar, held to the range a 32-bit time can name, 1970 to January 2038. */
static int fits_32(long long seconds) { return seconds >= 0 && seconds <= 0x7fffffffLL; }

CRT RavenTime *_gmtime32(const long *time)
{
	const long long wide = time ? *time : -1;
	return _gmtime64(&wide);
}

CRT int _gmtime32_s(RavenTime *out, const long *time)
{
	const long long wide = time ? *time : -1;
	return _gmtime64_s(out, time ? &wide : NULL);
}

CRT int _localtime32_s(RavenTime *out, const long *time)
{
	const long long wide = time ? *time : -1;
	return _localtime64_s(out, time ? &wide : NULL);
}

CRT RavenTime *_localtime32(const long *time)
{
	static RavenTime result;
	return _localtime32_s(&result, time) ? NULL : &result;
}

/* A time past 2038 is refused, and the structure is then left as it was given. */
static long fit_time(RavenTime *time, long long (*make)(RavenTime *))
{
	if (!time)
		return -1;
	RavenTime normal = *time;
	const long long seconds = make(&normal);
	if (!fits_32(seconds))
		return -1;
	*time = normal;
	return (long)seconds;
}

CRT long _mkgmtime32(RavenTime *time) { return fit_time(time, _mkgmtime64); }
CRT long _mktime32(RavenTime *time) { return fit_time(time, _mktime64); }
CRT double _difftime32(long later, long earlier) { return (double)later - (double)earlier; }

CRT char *_ctime32(const long *time)
{
	const long long wide = time ? *time : -1;
	return _ctime64(&wide);
}

/* The _s forms of ctime: a buffer too small for the 26 characters of the text gets an empty string and ERANGE. */
static int time_text_s(char *buffer, size_t size, const long long *time)
{
	if (!buffer || !size)
		return 22;
	buffer[0] = 0;
	if (!time || *time < 0)
		return 22;
	if (size < 26)
		return 34;
	const char *text = _ctime64(time);
	if (!text)
		return 22;
	memcpy(buffer, text, strlen(text) + 1);
	return 0;
}

CRT int _ctime64_s(char *buffer, size_t size, const long long *time) { return time_text_s(buffer, size, time); }

CRT int _ctime32_s(char *buffer, size_t size, const long *time)
{
	const long long wide = time ? *time : -1;
	return time_text_s(buffer, size, time ? &wide : NULL);
}

CRT wchar_t *_wctime32(const long *time)
{
	const long long wide = time ? *time : -1;
	return _wctime64(&wide);
}

CRT int _wctime32_s(wchar_t *buffer, size_t size, const long *time)
{
	const long long wide = time ? *time : -1;
	return _wctime64_s(buffer, size, time ? &wide : NULL);
}

static void widen_text(wchar_t *to, const char *text)
{
	while ((*to++ = (unsigned char)*text++))
		;
}

CRT wchar_t *_wasctime(const RavenTime *time)
{
	static wchar_t text[26];
	const char *narrow = asctime(time);
	if (!narrow)
		return NULL;
	widen_text(text, narrow);
	return text;
}

CRT int _wasctime_s(wchar_t *buffer, size_t size, const RavenTime *time)
{
	char text[26];
	if (!buffer || !size)
		return 22;
	buffer[0] = 0;
	if (size < 26)
		return 34;
	const int failure = asctime_s(text, sizeof text, time);
	if (failure)
		return failure;
	widen_text(buffer, text);
	return 0;
}

CRT size_t _wcsftime_l(wchar_t *to, size_t capacity, const wchar_t *format, const void *time, _locale_t locale)
{
	return wcsftime(to, capacity, format, time);
}

typedef struct {
	long time;
	unsigned short milliseconds;
	short zone, daylight;
} RavenTimeB32;

CRT void _ftime32(RavenTimeB32 *out)
{
	RavenTimeB now;
	_ftime64(&now);
	out->time = (long)now.time;
	out->milliseconds = now.milliseconds;
	out->zone = now.zone;
	out->daylight = now.daylight;
}

CRT int _ftime32_s(RavenTimeB32 *out)
{
	if (!out)
		return 22;
	_ftime32(out);
	return 0;
}

CRT int _get_dstbias(long *seconds)
{
	if (!seconds)
		return 22;
	_tzset();
	*seconds = _dstbias;
	return 0;
}

/* The local time as calendar fields; the milliseconds are the answer. _setsystime sets the clock from them. */
CRT unsigned _getsystime(RavenTime *out)
{
	SYSTEMTIME now;
	TIME_ZONE_INFORMATION zone;
	const DWORD kind = GetTimeZoneInformation(&zone);
	GetLocalTime(&now);
	fill_tm(out, &now, kind == TIME_ZONE_ID_DAYLIGHT);
	return now.wMilliseconds;
}

CRT unsigned _setsystime(const RavenTime *time, unsigned milliseconds)
{
	const SYSTEMTIME local = {(WORD)(time->year + 1900), (WORD)(time->month + 1), (WORD)time->weekday, (WORD)time->day, (WORD)time->hour,
		(WORD)time->minute, (WORD)time->second, (WORD)milliseconds};
	return SetLocalTime(&local) ? 0 : GetLastError();
}

/* The _s forms of _strdate and _strtime: nine characters, mm/dd/yy or hh:mm:ss and the terminator. */
static int clock_text_s(char *buffer, size_t size, char *(*make)(char *))
{
	if (!buffer || !size)
		return 22;
	if (size < 9) {
		buffer[0] = 0;
		return 34;
	}
	make(buffer);
	return 0;
}

CRT int _strdate_s(char *buffer, size_t size) { return clock_text_s(buffer, size, _strdate); }
CRT int _strtime_s(char *buffer, size_t size) { return clock_text_s(buffer, size, _strtime); }

CRT wchar_t *_wstrdate(wchar_t *to)
{
	char text[9];
	widen_text(to, _strdate(text));
	return to;
}

CRT wchar_t *_wstrtime(wchar_t *to)
{
	char text[9];
	widen_text(to, _strtime(text));
	return to;
}

typedef struct {
	long access_time, modify_time;
} RavenUtime32;

static const RavenUtime *widen_times(RavenUtime *wide, const RavenUtime32 *times)
{
	if (!times)
		return NULL;
	wide->access_time = times->access_time;
	wide->modify_time = times->modify_time;
	return wide;
}

CRT int _utime32(const char *path, const RavenUtime32 *times)
{
	RavenUtime wide;
	return _utime64(path, widen_times(&wide, times));
}

CRT int _wutime32(const wchar_t *path, const RavenUtime32 *times)
{
	RavenUtime wide;
	return _wutime64(path, widen_times(&wide, times));
}

/* The file times of an open descriptor; no times means now. */
CRT int _futime64(int fd, const RavenUtime *times)
{
	const HANDLE file = (HANDLE)descriptor_handle(fd);
	if (file == INVALID_HANDLE_VALUE) {
		set_errno(9);
		return -1;
	}
	FILETIME access, modify;
	if (times) {
		access = unix_to_file_time(times->access_time);
		modify = unix_to_file_time(times->modify_time);
	} else {
		GetSystemTimeAsFileTime(&modify);
		access = modify;
	}
	return SetFileTime(file, NULL, &access, &modify) ? 0 : windows_failure();
}

CRT int _futime32(int fd, const RavenUtime32 *times)
{
	RavenUtime wide;
	return _futime64(fd, widen_times(&wide, times));
}

/* ---- streams and descriptors: the runtime's streams take no locks, so the lock-free forms are the same functions ---- */

CRT size_t _fread_nolock(void *data, size_t size, size_t count, void *stream) { return fread(data, size, count, stream); }
CRT size_t _fwrite_nolock(const void *data, size_t size, size_t count, void *stream) { return fwrite(data, size, count, stream); }
CRT int _fseek_nolock(void *stream, long offset, int origin) { return fseek(stream, offset, origin); }
CRT int _fseeki64_nolock(void *stream, long long offset, int origin) { return _fseeki64(stream, offset, origin); }
CRT long _ftell_nolock(void *stream) { return ftell(stream); }
CRT long long _ftelli64_nolock(void *stream) { return _ftelli64(stream); }

/* Streams open at once: the runtime's table has no fixed size, so any count a program asks for within Windows' range is granted. */
static int stream_limit = 512;
CRT int _getmaxstdio(void) { return stream_limit; }
CRT int _setmaxstdio(int count)
{
	if (count < 20 || count > 8192) {
		set_errno(22);
		return -1;
	}
	return stream_limit = count;
}

/* _O_WRONLY | _O_CREAT | _O_TRUNC. */
CRT int _creat(const char *path, int permission) { return _open(path, 0x0001 | 0x0100 | 0x0200, permission); }

CRT int _wfreopen_s(void **out, const wchar_t *path, const wchar_t *mode, void *stream)
{
	if (!out || !mode || !stream)
		return 22;
	*out = _wfreopen(path, mode, stream);
	return *out ? 0 : *_errno();
}

/* The permission mask new files take: only the owner's write bit means anything on Windows, but the mask is kept as given. */
static int permission_mask;
CRT int _umask(int mask)
{
	const int previous = permission_mask;
	permission_mask = mask & 0600;
	return previous;
}

/* ---- file searches with 32-bit sizes ---- */

/* struct _finddata64i32_t: attributes, three 64-bit times, a 32-bit size and the name. */
typedef struct {
	unsigned attributes;
	long long created, accessed, written;
	unsigned long size;
	char name[260];
} RavenFind64i32;

static void fill_found_narrow(RavenFind64i32 *out, const WIN32_FIND_DATAA *data)
{
	out->attributes = data->dwFileAttributes;
	out->created = file_time_to_unix(data->ftCreationTime);
	out->accessed = file_time_to_unix(data->ftLastAccessTime);
	out->written = file_time_to_unix(data->ftLastWriteTime);
	out->size = data->nFileSizeLow;
	memcpy(out->name, data->cFileName, sizeof out->name);
}

CRT intptr_t _findfirst64i32(const char *pattern, RavenFind64i32 *out)
{
	WIN32_FIND_DATAA data;
	if (!pattern || !out) {
		set_errno(22);
		return -1;
	}
	HANDLE search = FindFirstFileA(pattern, &data);
	if (search == INVALID_HANDLE_VALUE) {
		windows_failure();
		return -1;
	}
	fill_found_narrow(out, &data);
	return (intptr_t)search;
}

CRT int _findnext64i32(intptr_t search, RavenFind64i32 *out)
{
	WIN32_FIND_DATAA data;
	if (!out || !FindNextFileA((HANDLE)search, &data)) {
		set_errno(out ? 2 : 22);
		return -1;
	}
	fill_found_narrow(out, &data);
	return 0;
}

/* ---- temporary names, as tmpnam and _tempnam give them ---- */

CRT wchar_t *_wtmpnam(wchar_t *buffer)
{
	static wchar_t name[MAX_PATH];
	wchar_t directory[MAX_PATH];
	wchar_t *out = buffer ? buffer : name;
	if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"s", 0, out))
		return NULL;
	DeleteFileW(out);
	return out;
}

CRT wchar_t *_wtempnam(const wchar_t *folder, const wchar_t *prefix)
{
	wchar_t directory[MAX_PATH], name[MAX_PATH];
	const DWORD length = GetEnvironmentVariableW(L"TMP", directory, MAX_PATH);
	if (!length || length >= MAX_PATH || GetFileAttributesW(directory) == INVALID_FILE_ATTRIBUTES) {
		if (folder && GetFileAttributesW(folder) != INVALID_FILE_ATTRIBUTES)
			lstrcpynW(directory, folder, MAX_PATH);
		else if (!GetTempPathW(MAX_PATH, directory))
			return NULL;
	}
	if (!GetTempFileNameW(directory, prefix ? prefix : L"", 0, name))
		return NULL;
	DeleteFileW(name);
	return _wcsdup(name);
}

/* ---- strings ---- */

CRT void _splitpath(const char *path, char *drive, char *folder, char *name, char *extension)
{
	const char *at = path, *last_slash = NULL, *last_dot = NULL;
	if (at[0] && at[1] == ':')
		at += 2;
	const char *const drive_end = at;
	for (const char *scan = at; *scan; ++scan)
		if (*scan == '\\' || *scan == '/')
			last_slash = scan;
		else if (*scan == '.')
			last_dot = scan;
	const char *const folder_end = last_slash ? last_slash + 1 : at, *const end = path + strlen(path);
	const char *const name_end = last_dot && last_dot >= folder_end ? last_dot : end;
	const struct { char *to; const char *from, *until; } parts[] = {
		{drive, path, drive_end}, {folder, drive_end, folder_end}, {name, folder_end, name_end}, {extension, name_end, end}};
	for (size_t part = 0; part < 4; ++part)
		if (parts[part].to) {
			memcpy(parts[part].to, parts[part].from, (size_t)(parts[part].until - parts[part].from));
			parts[part].to[parts[part].until - parts[part].from] = 0;
		}
}

CRT char *_strrev(char *text)
{
	for (size_t a = 0, b = strlen(text); b-- > a; ++a) {
		const char swap = text[a];
		text[a] = text[b];
		text[b] = swap;
	}
	return text;
}

CRT unsigned long long _byteswap_uint64(unsigned long long value) { return __builtin_bswap64(value); }

/* ---- math ---- */

CRT double remainder(double x, double y)
{
	if (x != x || y != y || y == 0.0 || x - x != 0.0)
		return (x * y) / (x * y);
	if (y - y != 0.0)
		return x;
	return x - rint(x / y) * y;
}

/* erf and erfc by W. J. Cody's published rational approximations (Math. Comp. 23, 1969), which hold to about the last digit of a double. */
static double erfc_large(double magnitude)
{
	static const double p[] = {3.05326634961232344e-1, 3.60344899949804439e-1, 1.25781726111229246e-1, 1.60837851487422766e-2,
		6.58749161529837803e-4, 1.63153871373020978e-2};
	static const double q[] = {2.56852019228982242e0, 1.87295284992346725e0, 5.27905102951428412e-1, 6.05183413124413191e-2,
		2.33520497626869185e-3};
	const double z = 1.0 / (magnitude * magnitude);
	double numerator = p[5] * z, denominator = z;
	for (int at = 0; at < 4; ++at) {
		numerator = (numerator + p[at]) * z;
		denominator = (denominator + q[at]) * z;
	}
	double result = z * (numerator + p[4]) / (denominator + q[4]);
	result = (0.56418958354775628695 - result) / magnitude;
	const double rounded = (double)(long long)(magnitude * 16.0) / 16.0;
	const double delta = (magnitude - rounded) * (magnitude + rounded);
	return exp(-rounded * rounded) * exp(-delta) * result;
}

static double erfc_middle(double magnitude)
{
	static const double c[] = {5.64188496988670089e-1, 8.88314979438837594e0, 6.61191906371416295e1, 2.98635138197400131e2,
		8.81952221241769090e2, 1.71204761263407058e3, 2.05107837782607147e3, 1.23033935479799725e3, 2.15311535474403846e-8};
	static const double d[] = {1.57449261107098347e1, 1.17693950891312499e2, 5.37181101862009858e2, 1.62138957456669019e3,
		3.29079923573345963e3, 4.36261909014324716e3, 3.43936767414372164e3, 1.23033935480374942e3};
	double numerator = c[8] * magnitude, denominator = magnitude;
	for (int at = 0; at < 7; ++at) {
		numerator = (numerator + c[at]) * magnitude;
		denominator = (denominator + d[at]) * magnitude;
	}
	const double result = (numerator + c[7]) / (denominator + d[7]);
	const double rounded = (double)(long long)(magnitude * 16.0) / 16.0;
	const double delta = (magnitude - rounded) * (magnitude + rounded);
	return exp(-rounded * rounded) * exp(-delta) * result;
}

static double erf_small(double x)
{
	static const double a[] = {3.16112374387056560e0, 1.13864154151050156e2, 3.77485237685302021e2, 3.20937758913846947e3,
		1.85777706184603153e-1};
	static const double b[] = {2.36012909523441209e1, 2.44024637934444173e2, 1.28261652607737228e3, 2.84423683343917062e3};
	const double z = x * x;
	double numerator = a[4] * z, denominator = z;
	for (int at = 0; at < 3; ++at) {
		numerator = (numerator + a[at]) * z;
		denominator = (denominator + b[at]) * z;
	}
	return x * (numerator + a[3]) / (denominator + b[3]);
}

CRT double erf(double x)
{
	if (x != x)
		return x;
	const double magnitude = x < 0.0 ? -x : x;
	if (magnitude <= 0.46875)
		return erf_small(x);
	const double complement = magnitude <= 4.0 ? erfc_middle(magnitude) : magnitude < 26.0 ? erfc_large(magnitude) : 0.0;
	return x < 0.0 ? complement - 1.0 : 1.0 - complement;
}

CRT double erfc(double x)
{
	if (x != x)
		return x;
	const double magnitude = x < 0.0 ? -x : x;
	if (magnitude <= 0.46875)
		return 1.0 - erf_small(x);
	const double complement = magnitude <= 4.0 ? erfc_middle(magnitude) : magnitude < 26.6 ? erfc_large(magnitude) : 0.0;
	return x < 0.0 ? 2.0 - complement : complement;
}

/* The gamma function by Lanczos' approximation (g = 7, nine terms), with the reflection formula below one half. */
CRT double tgamma(double x)
{
	static const double coefficients[] = {0.99999999999980993, 676.5203681218851, -1259.1392167224028, 771.32342877765313,
		-176.61502916214059, 12.507343278686905, -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7};
	if (x != x)
		return x;
	if (x == 0.0)
		return 1.0 / x;
	if (x < 0.0 && x == (double)(long long)x)
		return (x - x) / (x - x);
	if (x > 171.7)
		return 1.0 / 0.0;
	if (x < 0.5)
		return 3.14159265358979323846 / (sin(3.14159265358979323846 * x) * tgamma(1.0 - x));
	x -= 1.0;
	double sum = coefficients[0];
	for (int at = 1; at < 9; ++at)
		sum += coefficients[at] / (x + at);
	const double t = x + 7.5;
	return 2.5066282746310005024 * pow(t, x + 0.5) * exp(-t) * sum;
}

/* ---- the msvcrt names of MinGW-w64 libraries ---- */

CRT int sscanf_s(const char *input, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const int result = scan_narrow_text(input, format, arguments, 1);
	va_end(arguments);
	return result;
}

CRT int swscanf_s(const wchar_t *input, const wchar_t *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const int result = scan_wide_text(input, format, arguments, 1);
	va_end(arguments);
	return result;
}

CRT int fscanf(void *stream, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const int result = __stdio_common_vfscanf(0, stream, format, NULL, arguments);
	va_end(arguments);
	return result;
}

/* At most count characters, or with _TRUNCATE as many as the buffer holds; text cut short gives -1. Text that does not fit the buffer at
 * all when a count was asked for is an error, and the buffer is left empty. */
CRT int _vsnprintf_s(char *buffer, size_t size, size_t count, const char *text, va_list arguments)
{
	const int truncate = count == (size_t)-1;
	if (!buffer || !size || !text) {
		if (buffer && size)
			buffer[0] = 0;
		set_errno(22);
		return -1;
	}
	const size_t room = truncate || count >= size ? size - 1 : count;
	const int length = __stdio_common_vsprintf(0, buffer, room + 1, text, NULL, arguments);
	if (length >= 0 && (size_t)length <= room)
		return length;
	if (truncate || room < size - 1)
		return -1;
	buffer[0] = 0;
	set_errno(34);
	return -1;
}

/* struct __finddata64_t: attributes, three 64-bit times, a 64-bit size and the name. */
typedef struct {
	unsigned attributes;
	long long created, accessed, written;
	long long size;
	char name[260];
} RavenFind64;

static void fill_found_64(RavenFind64 *out, const WIN32_FIND_DATAA *data)
{
	out->attributes = data->dwFileAttributes;
	out->created = file_time_to_unix(data->ftCreationTime);
	out->accessed = file_time_to_unix(data->ftLastAccessTime);
	out->written = file_time_to_unix(data->ftLastWriteTime);
	out->size = (long long)data->nFileSizeHigh << 32 | data->nFileSizeLow;
	memcpy(out->name, data->cFileName, sizeof out->name);
}

CRT intptr_t _findfirst64(const char *pattern, RavenFind64 *out)
{
	WIN32_FIND_DATAA data;
	if (!pattern || !out) {
		set_errno(22);
		return -1;
	}
	HANDLE search = FindFirstFileA(pattern, &data);
	if (search == INVALID_HANDLE_VALUE) {
		windows_failure();
		return -1;
	}
	fill_found_64(out, &data);
	return (intptr_t)search;
}

CRT int _findnext64(intptr_t search, RavenFind64 *out)
{
	WIN32_FIND_DATAA data;
	if (!out || !FindNextFileA((HANDLE)search, &data)) {
		set_errno(out ? 2 : 22);
		return -1;
	}
	fill_found_64(out, &data);
	return 0;
}

/* A pipe to a command the command interpreter runs: the stream reads what it writes, or writes what it reads. _pclose waits for it and
 * gives its exit code. */
static struct {
	void *stream;
	HANDLE process;
} command_pipes[64];
static SRWLOCK command_pipes_lock = SRWLOCK_INIT;

CRT void *_popen(const char *command, const char *mode)
{
	SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
	STARTUPINFOA startup = {sizeof startup};
	PROCESS_INFORMATION child;
	HANDLE read_end, write_end;
	char shell[MAX_PATH];
	if (!command || !mode || (mode[0] != 'r' && mode[0] != 'w')) {
		set_errno(22);
		return NULL;
	}
	const int reading = mode[0] == 'r';
	const DWORD length = GetEnvironmentVariableA("ComSpec", shell, MAX_PATH);
	if (!length || length >= MAX_PATH)
		lstrcpyA(shell, "C:\\Windows\\System32\\cmd.exe");
	char *line = malloc(strlen(shell) + strlen(command) + 8);
	if (!line) {
		set_errno(12);
		return NULL;
	}
	lstrcpyA(line, "\"");
	lstrcatA(line, shell);
	lstrcatA(line, "\" /c ");
	lstrcatA(line, command);
	if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
		free(line);
		windows_failure();
		return NULL;
	}
	HANDLE ours = reading ? read_end : write_end, theirs = reading ? write_end : read_end;
	SetHandleInformation(ours, HANDLE_FLAG_INHERIT, 0);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = reading ? GetStdHandle(STD_INPUT_HANDLE) : theirs;
	startup.hStdOutput = reading ? theirs : GetStdHandle(STD_OUTPUT_HANDLE);
	startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	const BOOL started = CreateProcessA(shell, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child);
	free(line);
	CloseHandle(theirs);
	if (!started) {
		CloseHandle(ours);
		windows_failure();
		return NULL;
	}
	CloseHandle(child.hThread);
	const int fd = _open_osfhandle((intptr_t)ours, (reading ? 0 : 1) | (strchr(mode, 'b') ? 0x8000 : 0x4000));
	void *stream = fd < 0 ? NULL : _fdopen(fd, mode);
	if (stream) {
		AcquireSRWLockExclusive(&command_pipes_lock);
		size_t at = 0;
		while (at < sizeof command_pipes / sizeof *command_pipes && command_pipes[at].stream)
			++at;
		if (at < sizeof command_pipes / sizeof *command_pipes) {
			command_pipes[at].stream = stream;
			command_pipes[at].process = child.hProcess;
		} else {
			fclose(stream);
			stream = NULL;
		}
		ReleaseSRWLockExclusive(&command_pipes_lock);
	} else if (fd >= 0) {
		_close(fd);
	} else {
		CloseHandle(ours);
	}
	if (!stream) {
		CloseHandle(child.hProcess);
		set_errno(24);
	}
	return stream;
}

CRT int _pclose(void *stream)
{
	HANDLE process = NULL;
	DWORD code = (DWORD)-1;
	AcquireSRWLockExclusive(&command_pipes_lock);
	for (size_t at = 0; at < sizeof command_pipes / sizeof *command_pipes; ++at)
		if (stream && command_pipes[at].stream == stream) {
			process = command_pipes[at].process;
			command_pipes[at].stream = NULL;
			break;
		}
	ReleaseSRWLockExclusive(&command_pipes_lock);
	if (!process) {
		set_errno(22);
		return -1;
	}
	fclose(stream);
	WaitForSingleObject(process, INFINITE);
	GetExitCodeProcess(process, &code);
	CloseHandle(process);
	return (int)code;
}

/* ---- programs a program starts ---- */

/* A program that does not start: ENOEXEC for a file that is not one, EACCES for one locked or shared away, ENOMEM, or else ENOENT. */
static int process_failure(void)
{
	const DWORD error = GetLastError();
	set_errno(error == ERROR_BAD_EXE_FORMAT || error == ERROR_EXE_MACHINE_TYPE_MISMATCH ? 8 :
		error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ? 13 :
		error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ? 12 : 2);
	return -1;
}

/* system and _wsystem hand a command to the command interpreter, the program ComSpec names or else cmd.exe on the path, and wait for it.
 * The answer is the interpreter's exit code, or -1 with errno set when it does not start. A null command asks whether there is an
 * interpreter: nonzero when there is, 0 with ENOENT when there is not. */
static int command_interpreter(wchar_t *shell)
{
	const DWORD length = GetEnvironmentVariableW(L"ComSpec", shell, MAX_PATH);
	if (length && length < MAX_PATH)
		return 1;
	const DWORD found = SearchPathW(NULL, L"cmd.exe", NULL, MAX_PATH, shell, NULL);
	return found && found < MAX_PATH;
}

CRT int _wsystem(const wchar_t *command)
{
	wchar_t shell[MAX_PATH];
	const int found = command_interpreter(shell);
	if (!command) {
		const DWORD attributes = found ? GetFileAttributesW(shell) : INVALID_FILE_ATTRIBUTES;
		if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
			return 1;
		set_errno(2);
		return 0;
	}
	if (!found) {
		set_errno(2);
		return -1;
	}
	/* "interpreter" /c command, within the 32767 characters of a command line. */
	const size_t length = wcslen(shell) + wcslen(command) + 7;
	if (length > 32767) {
		set_errno(7);
		return -1;
	}
	wchar_t *line = malloc(length * sizeof *line);
	if (!line) {
		set_errno(12);
		return -1;
	}
	lstrcpyW(line, L"\"");
	lstrcatW(line, shell);
	lstrcatW(line, L"\" /c ");
	lstrcatW(line, command);
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION child;
	const BOOL started = CreateProcessW(shell, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child);
	free(line);
	if (!started)
		return process_failure();
	DWORD code = (DWORD)-1;
	CloseHandle(child.hThread);
	WaitForSingleObject(child.hProcess, INFINITE);
	GetExitCodeProcess(child.hProcess, &code);
	CloseHandle(child.hProcess);
	return (int)code;
}

CRT int system(const char *command)
{
	if (!command)
		return _wsystem(NULL);
	const int length = MultiByteToWideChar(CP_ACP, 0, command, -1, NULL, 0);
	wchar_t *wide = length > 0 ? malloc((size_t)length * sizeof *wide) : NULL;
	if (!wide) {
		set_errno(length > 0 ? 12 : 42);
		return -1;
	}
	MultiByteToWideChar(CP_ACP, 0, command, -1, wide, length);
	const int result = _wsystem(wide);
	free(wide);
	return result;
}

/* _wspawnv and _wspawnve start a program with the arguments of a vector, joined by spaces as they are, and the environment of the calling
 * process or the one given; _wexecv and _wexecve start it in place of the calling process, which ends once the new one runs. A name
 * without an extension is tried with .com, .exe, .bat and .cmd, and a batch file runs through the command interpreter. _P_WAIT answers
 * the program's exit code, _P_NOWAIT and _P_NOWAITO its process handle for _cwait, and _P_DETACH starts it without a console. Open handles
 * stay open in the new process. The p variants also look for a bare name in the folders of PATH. */
enum { SPAWN_WAIT, SPAWN_NOWAIT, SPAWN_OVERLAY, SPAWN_NOWAITO, SPAWN_DETACH };

/* The program a bare name names in the folders of PATH, tried with each extension when it has none. */
static int search_program(const wchar_t *name, int extended, wchar_t *path)
{
	static const wchar_t *const extensions[] = {L".com", L".exe", L".bat", L".cmd"};
	for (const wchar_t *at = name; *at; ++at)
		if (*at == '\\' || *at == '/' || *at == ':')
			return 0;
	for (size_t at = 0; at < (extended ? 1 : sizeof extensions / sizeof *extensions); ++at) {
		const DWORD found = SearchPathW(NULL, name, extended ? NULL : extensions[at], MAX_PATH, path, NULL);
		if (found && found < MAX_PATH && !(GetFileAttributesW(path) & FILE_ATTRIBUTE_DIRECTORY))
			return 1;
	}
	return 0;
}

static int find_program(const wchar_t *name, wchar_t *path, int *batch, int search)
{
	static const wchar_t *const extensions[] = {L".com", L".exe", L".bat", L".cmd"};
	const size_t length = wcslen(name);
	const wchar_t *dot = NULL;
	for (const wchar_t *at = name; *at; ++at)
		dot = *at == '.' ? at : *at == '\\' || *at == '/' || *at == ':' ? NULL : dot;
	if (length + 5 > MAX_PATH)
		return 0;
	lstrcpyW(path, name);
	if (!dot) {
		size_t at = 0;
		for (; at < sizeof extensions / sizeof *extensions; ++at) {
			lstrcpyW(path + length, extensions[at]);
			const DWORD attributes = GetFileAttributesW(path);
			if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
				break;
		}
		if (at == sizeof extensions / sizeof *extensions && !(search && search_program(name, 0, path)))
			return 0;
	} else if (search) {
		const DWORD attributes = GetFileAttributesW(path);
		if ((attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) && !search_program(name, 1, path))
			return 0;
	}
	const size_t found = wcslen(path);
	*batch = found >= 4 && (!lstrcmpiW(path + found - 4, L".bat") || !lstrcmpiW(path + found - 4, L".cmd"));
	return 1;
}

static intptr_t spawn_program(int mode, const wchar_t *name, const wchar_t *const *arguments, const wchar_t *const *environment, int search)
{
	wchar_t path[MAX_PATH], shell[MAX_PATH];
	int batch = 0;
	if (!name || !arguments || !arguments[0] || !arguments[0][0] || mode < SPAWN_WAIT || mode > SPAWN_DETACH) {
		set_errno(22);
		return -1;
	}
	if (!find_program(name, path, &batch, search) || (batch && !command_interpreter(shell))) {
		set_errno(2);
		return -1;
	}
	/* The arguments one space apart; a batch file's path and arguments make the interpreter's command: "shell" /s /c ""path" ...". */
	size_t length = batch ? wcslen(shell) + wcslen(path) + 16 : 1;
	for (const wchar_t *const *argument = arguments; *argument; ++argument)
		length += wcslen(*argument) + 1;
	if (length > 32767) {
		set_errno(7);
		return -1;
	}
	size_t size = 2;
	if (environment)
		for (const wchar_t *const *entry = environment; *entry; ++entry)
			size += wcslen(*entry) + 1;
	wchar_t *line = malloc(length * sizeof *line), *block = environment ? malloc(size * sizeof *block) : NULL;
	if (!line || (environment && !block)) {
		free(line);
		free(block);
		set_errno(12);
		return -1;
	}
	line[0] = 0;
	if (batch) {
		lstrcatW(line, L"\"");
		lstrcatW(line, shell);
		lstrcatW(line, L"\" /s /c \"\"");
		lstrcatW(line, path);
		lstrcatW(line, L"\"");
	}
	for (const wchar_t *const *argument = batch ? arguments + 1 : arguments; *argument; ++argument) {
		if (line[0])
			lstrcatW(line, L" ");
		lstrcatW(line, *argument);
	}
	if (batch)
		lstrcatW(line, L"\"");
	/* The environment block: each NAME=value with its terminator, then one more. */
	if (block) {
		wchar_t *at = block;
		for (const wchar_t *const *entry = environment; *entry; ++entry) {
			const size_t count = wcslen(*entry) + 1;
			memcpy(at, *entry, count * sizeof *at);
			at += count;
		}
		at[0] = at[1] = 0;
	}
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION child;
	const DWORD flags = (block ? CREATE_UNICODE_ENVIRONMENT : 0) | (mode == SPAWN_DETACH ? DETACHED_PROCESS : 0);
	const BOOL started = CreateProcessW(batch ? shell : path, line, NULL, NULL, TRUE, flags, block, NULL, &startup, &child);
	free(line);
	free(block);
	if (!started)
		return process_failure();
	CloseHandle(child.hThread);
	if (mode == SPAWN_NOWAIT || mode == SPAWN_NOWAITO)
		return (intptr_t)child.hProcess;
	DWORD code = 0;
	if (mode == SPAWN_WAIT) {
		WaitForSingleObject(child.hProcess, INFINITE);
		GetExitCodeProcess(child.hProcess, &code);
	}
	CloseHandle(child.hProcess);
	if (mode == SPAWN_OVERLAY)
		ExitProcess(0);
	return (intptr_t)code;
}

CRT intptr_t _wspawnv(int mode, const wchar_t *name, const wchar_t *const *arguments) { return spawn_program(mode, name, arguments, NULL, 0); }
CRT intptr_t _wspawnvp(int mode, const wchar_t *name, const wchar_t *const *arguments) { return spawn_program(mode, name, arguments, NULL, 1); }
CRT intptr_t _wspawnve(int mode, const wchar_t *name, const wchar_t *const *arguments, const wchar_t *const *environment)
{
	return spawn_program(mode, name, arguments, environment, 0);
}
CRT intptr_t _wexecv(const wchar_t *name, const wchar_t *const *arguments) { return spawn_program(SPAWN_OVERLAY, name, arguments, NULL, 0); }
CRT intptr_t _wexecve(const wchar_t *name, const wchar_t *const *arguments, const wchar_t *const *environment)
{
	return spawn_program(SPAWN_OVERLAY, name, arguments, environment, 0);
}

/* The narrow forms convert the name and arguments from the ANSI code page. */
static wchar_t *widen(const char *text)
{
	const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	wchar_t *wide = length > 0 ? malloc((size_t)length * sizeof *wide) : NULL;
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

static intptr_t spawn_narrow(int mode, const char *name, const char *const *arguments, int search)
{
	size_t count = 0;
	if (!name || !arguments) {
		set_errno(22);
		return -1;
	}
	while (arguments[count])
		++count;
	wchar_t *wide_name = widen(name), **wide = calloc(count + 1, sizeof *wide);
	int complete = wide_name && wide;
	for (size_t at = 0; complete && at < count; ++at)
		complete = (wide[at] = widen(arguments[at])) != NULL;
	intptr_t result = -1;
	if (complete)
		result = spawn_program(mode, wide_name, (const wchar_t *const *)wide, NULL, search);
	else
		set_errno(12);
	for (size_t at = 0; wide && at < count; ++at)
		free(wide[at]);
	free(wide);
	free(wide_name);
	return result;
}

CRT intptr_t _spawnvp(int mode, const char *name, const char *const *arguments) { return spawn_narrow(mode, name, arguments, 1); }

/* The l forms take the arguments in the call, ended by a null pointer. */
CRT intptr_t _spawnlp(int mode, const char *name, const char *argument, ...)
{
	va_list list;
	size_t count = 1;
	va_start(list, argument);
	while (argument && va_arg(list, const char *))
		++count;
	va_end(list);
	const char **arguments = malloc((count + 1) * sizeof *arguments);
	if (!arguments) {
		set_errno(12);
		return -1;
	}
	arguments[0] = argument;
	va_start(list, argument);
	for (size_t at = 1; at <= count; ++at)
		arguments[at] = argument ? va_arg(list, const char *) : NULL;
	va_end(list);
	const intptr_t result = spawn_narrow(mode, name, arguments, 1);
	free(arguments);
	return result;
}

/* Waits for a program _wspawnv started without waiting, stores its exit code and closes its handle; the action is ignored, as Windows
 * keeps no tree of processes. ECHILD for a handle that cannot be waited on. */
CRT intptr_t _cwait(int *status, intptr_t process, int action)
{
	DWORD code;
	if (WaitForSingleObject((HANDLE)process, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess((HANDLE)process, &code)) {
		set_errno(10);
		return -1;
	}
	if (status)
		*status = (int)code;
	if (process != -1 && process != -2)
		CloseHandle((HANDLE)process);
	return process;
}

CRT int freopen_s(void **out, const char *path, const char *mode, void *stream)
{
	if (!out || !mode || !stream)
		return 22;
	*out = freopen(path, mode, stream);
	return *out ? 0 : *_errno();
}

/* _heapchk looks over the process heap the runtime allocates from: _HEAPOK, or _HEAPBADNODE when it is damaged. */
CRT int _heapchk(void) { return HeapValidate(GetProcessHeap(), 0, NULL) ? -2 : -4; }

/* The rotations of a 32-bit long. */
CRT unsigned long _lrotl(unsigned long value, int shift)
{
	shift &= 31;
	return shift ? (value << shift) | (value >> (32 - shift)) : value;
}

CRT unsigned long _lrotr(unsigned long value, int shift)
{
	shift &= 31;
	return shift ? (value >> shift) | (value << (32 - shift)) : value;
}

/* _except1 is how a math function of the runtime reports a floating-point exception it found: `flags` names it (overflow 1, underflow 2,
 * division by zero 4, invalid 8, inexact 16) and `control` is the caller's control word, MXCSR's masks on x64 and the x87 one on x86. A
 * masked exception sets errno, as the C standard asks, and the function returns its result; an unmasked one is raised as the processor
 * would raise it. */
CRT double _except1(DWORD flags, int operation, double argument, double result, DWORD control, void *reserved)
{
	static const struct {
		DWORD flag, code;
		int error;
	} kinds[] = {{8, EXCEPTION_FLT_INVALID_OPERATION, 33}, {4, EXCEPTION_FLT_DIVIDE_BY_ZERO, 34}, {1, EXCEPTION_FLT_OVERFLOW, 34},
		{2, EXCEPTION_FLT_UNDERFLOW, 34}, {16, EXCEPTION_FLT_INEXACT_RESULT, 0}};
#ifdef _WIN64
	/* MXCSR masks invalid at bit 7, division by zero at 9, overflow at 10, underflow at 11 and precision at 12. */
	static const int mask_bits[] = {7, 9, 10, 11, 12};
#else
	/* The x87 control word masks invalid at bit 0, division by zero at 2, overflow at 3, underflow at 4 and precision at 5. */
	static const int mask_bits[] = {0, 2, 3, 4, 5};
#endif
	for (size_t at = 0; at < sizeof kinds / sizeof *kinds; ++at) {
		if (!(flags & kinds[at].flag))
			continue;
		if (!(control & (1u << mask_bits[at]))) {
			RaiseException(kinds[at].code, 0, 0, NULL);
			break;
		}
		if (kinds[at].error)
			set_errno(kinds[at].error);
		break;
	}
	return result;
}

/* More of the _o_ names newer runtime libraries import. */
__asm__(".section .drectve\n\t.ascii \""
	" -export:_o___stdio_common_vswprintf=__stdio_common_vswprintf -export:_o__aligned_free=_aligned_free"
	" -export:_o__aligned_malloc=_aligned_malloc -export:_o__dclass=_dclass -export:_o__free_base=free -export:_o__fullpath=_fullpath"
	" -export:_o__ldclass=_ldclass -export:_o__malloc_base=malloc -export:_o_ceilf=ceilf -export:_o_memcpy_s=memcpy_s -export:_o_rand=rand"
	" -export:_o_srand=srand"
	"\"\n\t.text");
