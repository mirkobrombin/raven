/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The entry points that large Visual C++ 2019 and 2022 programs and the libraries they ship (Qt 6, OpenCASCADE, OpenCV) import beyond the
 * rest: the exception and type_info helpers the C++ runtime libraries take under their _o_ names, the lock-free stream functions, more
 * conversions under a locale, file searches with 32-bit sizes, wide temporary names, path splitting and the C99 math functions erf, erfc,
 * tgamma and remainder. Included by ucrtbase.c, after the other parts, whose helpers it shares. */

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
