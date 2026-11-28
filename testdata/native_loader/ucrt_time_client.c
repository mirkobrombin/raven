#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The time contract of the Universal CRT as a program resolves it by hand: every name of api-ms-win-crt-time-l1-1-0.dll is found
   through that API set, and the 32-bit forms answer as their 64-bit ones do inside their range and refuse what lies outside it. The
   exit code names the first check that failed; 100 and up names a missing export. */

static const char *const names[] = {
	"_Getdays", "_Getmonths", "_Gettnames", "_Strftime", "_W_Getdays", "_W_Getmonths", "_W_Gettnames", "_Wcsftime", "__daylight",
	"__dstbias", "__timezone", "__tzname", "_ctime32", "_ctime32_s", "_ctime64", "_ctime64_s", "_difftime32", "_difftime64", "_ftime32",
	"_ftime32_s", "_ftime64", "_ftime64_s", "_futime32", "_futime64", "_get_daylight", "_get_dstbias", "_get_timezone", "_get_tzname",
	"_getsystime", "_gmtime32", "_gmtime32_s", "_gmtime64", "_gmtime64_s", "_localtime32", "_localtime32_s", "_localtime64",
	"_localtime64_s", "_mkgmtime32", "_mkgmtime64", "_mktime32", "_mktime64", "_setsystime", "_strdate", "_strdate_s", "_strftime_l",
	"_strtime", "_strtime_s", "_time32", "_time64", "_timespec32_get", "_timespec64_get", "_tzset", "_utime32", "_utime64", "_wasctime",
	"_wasctime_s", "_wcsftime_l", "_wctime32", "_wctime32_s", "_wctime64", "_wctime64_s", "_wstrdate", "_wstrdate_s", "_wstrtime",
	"_wstrtime_s", "_wutime32", "_wutime64", "asctime", "asctime_s", "clock", "strftime", "wcsftime",
};

typedef struct { int second, minute, hour, day, month, year, weekday, yearday, daylight; } calendar;
typedef struct { long time; unsigned short milliseconds; short zone, daylight; } time_b32;
typedef struct { long long time; unsigned short milliseconds; short zone, daylight; } time_b64;
typedef struct { long access, modify; } times32;

static HMODULE crt;
static FARPROC find(const char *name) { return GetProcAddress(crt, name); }

static int same_wide(const wchar_t *wide, const char *narrow)
{
	for (; *narrow; ++wide, ++narrow)
		if (*wide != (unsigned char)*narrow)
			return 0;
	return *wide == 0;
}

int raven_entry(void)
{
	crt = LoadLibraryA("api-ms-win-crt-time-l1-1-0.dll");
	if (!crt)
		return 1;
	for (int index = 0; index < (int)(sizeof names / sizeof *names); ++index)
		if (!find(names[index]))
			return 100 + index;
	int (__cdecl *gmtime32_s)(calendar *, const long *) = (void *)find("_gmtime32_s");
	calendar *(__cdecl *gmtime32)(const long *) = (void *)find("_gmtime32");
	int (__cdecl *localtime32_s)(calendar *, const long *) = (void *)find("_localtime32_s");
	int (__cdecl *localtime64_s)(calendar *, const long long *) = (void *)find("_localtime64_s");
	long (__cdecl *mkgmtime32)(calendar *) = (void *)find("_mkgmtime32");
	long (__cdecl *mktime32)(calendar *) = (void *)find("_mktime32");
	double (__cdecl *difftime32)(long, long) = (void *)find("_difftime32");
	char *(__cdecl *ctime32)(const long *) = (void *)find("_ctime32");
	char *(__cdecl *ctime64)(const long long *) = (void *)find("_ctime64");
	int (__cdecl *ctime32_s)(char *, size_t, const long *) = (void *)find("_ctime32_s");
	int (__cdecl *ctime64_s)(char *, size_t, const long long *) = (void *)find("_ctime64_s");
	wchar_t *(__cdecl *wctime32)(const long *) = (void *)find("_wctime32");
	int (__cdecl *wctime32_s)(wchar_t *, size_t, const long *) = (void *)find("_wctime32_s");
	void (__cdecl *ftime32)(time_b32 *) = (void *)find("_ftime32");
	void (__cdecl *ftime64)(time_b64 *) = (void *)find("_ftime64");
	long (__cdecl *time32)(long *) = (void *)find("_time32");
	int (__cdecl *get_dstbias)(long *) = (void *)find("_get_dstbias");
	long *(__cdecl *dstbias)(void) = (void *)find("__dstbias");
	unsigned (__cdecl *getsystime)(calendar *) = (void *)find("_getsystime");
	int (__cdecl *strdate_s)(char *, size_t) = (void *)find("_strdate_s");
	int (__cdecl *strtime_s)(char *, size_t) = (void *)find("_strtime_s");
	wchar_t *(__cdecl *wstrdate)(wchar_t *) = (void *)find("_wstrdate");
	wchar_t *(__cdecl *wstrtime)(wchar_t *) = (void *)find("_wstrtime");
	char *(__cdecl *asctime)(const calendar *) = (void *)find("asctime");
	wchar_t *(__cdecl *wasctime)(const calendar *) = (void *)find("_wasctime");
	int (__cdecl *wasctime_s)(wchar_t *, size_t, const calendar *) = (void *)find("_wasctime_s");
	size_t (__cdecl *wcsftime_l)(wchar_t *, size_t, const wchar_t *, const calendar *, void *) = (void *)find("_wcsftime_l");
	int (__cdecl *utime32)(const char *, const times32 *) = (void *)find("_utime32");
	int (__cdecl *wutime32)(const wchar_t *, const times32 *) = (void *)find("_wutime32");
	int (__cdecl *futime32)(int, const times32 *) = (void *)find("_futime32");
	int (__cdecl *open_osfhandle)(intptr_t, int) = (void *)GetProcAddress(LoadLibraryA("ucrtbase.dll"), "_open_osfhandle");

	// 2018-10-25 17:40:21 UTC, a Thursday, day 297 of the year.
	const long stamp = 1540489221;
	calendar fields;
	if (gmtime32_s(&fields, &stamp) || fields.year != 118 || fields.month != 9 || fields.day != 25 || fields.hour != 17 ||
		fields.minute != 40 || fields.second != 21 || fields.weekday != 4 || fields.yearday != 297 || fields.daylight)
		return 2;
	const calendar *shared = gmtime32(&stamp);
	if (!shared || shared->day != 25 || shared->hour != 17)
		return 3;
	const long negative = -1;
	if (gmtime32_s(&fields, &negative) != 22 || fields.year != -1 || gmtime32(&negative))
		return 4;
	if (mkgmtime32(&fields) != -1)
		return 5;
	gmtime32_s(&fields, &stamp);
	fields.day += 31;
	if (mkgmtime32(&fields) != stamp + 31 * 86400 || fields.month != 10 || fields.day != 25)
		return 6;
	// Local time: the 32-bit forms give what the 64-bit ones give, and mktime takes it back.
	const long long wide = stamp;
	calendar local, local64;
	if (localtime32_s(&local, &stamp) || localtime64_s(&local64, &wide) || local.hour != local64.hour || local.day != local64.day ||
		local.daylight != local64.daylight)
		return 7;
	if (mktime32(&local) != stamp)
		return 8;
	calendar later_year = local;
	later_year.year = 140;
	if (mktime32(&later_year) != -1)
		return 9;
	if (difftime32(stamp, stamp - 90) != 90.0)
		return 10;
	// The text forms.
	const char *text = ctime32(&stamp);
	if (!text || lstrcmpA(text, ctime64(&wide)) || lstrlenA(text) != 25 || text[24] != '\n')
		return 11;
	char buffer[32];
	if (ctime32_s(buffer, sizeof buffer, &stamp) || lstrcmpA(buffer, text) || ctime64_s(buffer, sizeof buffer, &wide) || lstrcmpA(buffer, text))
		return 12;
	if (ctime32_s(buffer, 25, &stamp) != 34 || buffer[0] || ctime32_s(NULL, 32, &stamp) != 22 || ctime32_s(buffer, sizeof buffer, &negative) != 22)
		return 13;
	wchar_t wide_buffer[32];
	if (!same_wide(wctime32(&stamp), text) || wctime32_s(wide_buffer, 32, &stamp) || !same_wide(wide_buffer, text))
		return 14;
	gmtime32_s(&fields, &stamp);
	if (!same_wide(wasctime(&fields), asctime(&fields)) || wasctime_s(wide_buffer, 32, &fields) || !same_wide(wide_buffer, asctime(&fields)))
		return 15;
	if (wcsftime_l(wide_buffer, 32, L"%Y-%m-%d %H:%M:%S", &fields, NULL) != 19 || lstrcmpW(wide_buffer, L"2018-10-25 17:40:21"))
		return 16;
	// The clock.
	time_b32 now32;
	time_b64 now64;
	ftime64(&now64);
	ftime32(&now32);
	if (now32.time < now64.time || now32.time - now64.time > 2 || now32.zone != now64.zone || now32.milliseconds > 999)
		return 17;
	const long seconds = time32(NULL);
	if (seconds < now32.time || seconds - now32.time > 2)
		return 18;
	long bias = 1;
	if (get_dstbias(&bias) || bias != *dstbias())
		return 19;
	calendar system;
	if (getsystime(&system) > 999 || system.month < 0 || system.month > 11 || system.year < 118)
		return 20;
	if (strdate_s(buffer, 9) || lstrlenA(buffer) != 8 || buffer[2] != '/' || strtime_s(buffer, 9) || lstrlenA(buffer) != 8 || buffer[2] != ':')
		return 21;
	if (strdate_s(buffer, 8) != 34 || buffer[0] || strtime_s(NULL, 9) != 22)
		return 22;
	if (wstrdate(wide_buffer) != wide_buffer || lstrlenW(wide_buffer) != 8 || wide_buffer[2] != L'/' || wstrtime(wide_buffer) != wide_buffer ||
		wide_buffer[2] != L':')
		return 23;
	// File times through a path, a wide path and a descriptor.
	char path[MAX_PATH];
	GetTempPathA(MAX_PATH, path);
	lstrcatA(path, "raven-ucrt-time.txt");
	HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 24;
	CloseHandle(file);
	const times32 set = {stamp, stamp + 60};
	WIN32_FILE_ATTRIBUTE_DATA data;
	const unsigned long long expected = (unsigned long long)(stamp + 60 + 11644473600LL) * 10000000ULL;
	if (utime32(path, &set) || !GetFileAttributesExA(path, GetFileExInfoStandard, &data) ||
		(((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime) != expected)
		return 25;
	wchar_t wide_path[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, path, -1, wide_path, MAX_PATH);
	const times32 later = {stamp + 120, stamp + 180};
	if (wutime32(wide_path, &later) || !GetFileAttributesExA(path, GetFileExInfoStandard, &data) ||
		(((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime) != expected + 120 * 10000000ULL)
		return 26;
	file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	const int descriptor = open_osfhandle((intptr_t)file, 0);
	if (descriptor < 0 || futime32(descriptor, &set) || !GetFileTime(file, NULL, NULL, &data.ftLastWriteTime) ||
		(((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime) != expected)
		return 27;
	if (futime32(-1, &set) != -1)
		return 28;
	CloseHandle(file);
	DeleteFileA(path);
	return 0;
}
