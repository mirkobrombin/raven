/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven C runtime component, part six: local time, the wide and descriptor forms of the file functions, the remaining string,
 * conversion and math entry points, and the startup helpers that programs built by recent Visual C++ or MinGW-w64 import. This file
 * is included at the end of ucrtbase.c, so it shares the stream, descriptor and formatting helpers of the parts before it. */

/* ---- local time ---- */

#undef _timezone
#undef _daylight
#undef _tzname
/* msvcrt publishes the time zone as variables; the Universal CRT as functions that read the same values. */
CRT long _timezone;
CRT int _daylight;
CRT char *_tzname[2];
static char standard_zone_name[64] = "UTC", daylight_zone_name[64] = "UTC";

CRT void _tzset(void)
{
	TIME_ZONE_INFORMATION zone;
	const DWORD kind = GetTimeZoneInformation(&zone);
	_timezone = zone.Bias * 60;
	_daylight = kind != TIME_ZONE_ID_INVALID && zone.DaylightDate.wMonth != 0;
	WideCharToMultiByte(CP_ACP, 0, zone.StandardName, -1, standard_zone_name, sizeof(standard_zone_name), NULL, NULL);
	WideCharToMultiByte(CP_ACP, 0, zone.DaylightName, -1, daylight_zone_name, sizeof(daylight_zone_name), NULL, NULL);
	_tzname[0] = standard_zone_name;
	_tzname[1] = daylight_zone_name;
}

CRT int _get_timezone(long *seconds)
{
	if (!seconds)
		return 22;
	_tzset();
	*seconds = _timezone;
	return 0;
}

CRT int _get_daylight(int *hours)
{
	if (!hours)
		return 22;
	_tzset();
	*hours = _daylight;
	return 0;
}

static int day_of_year(int year, int month, int day)
{
	static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	const int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	return before[month] + day - 1 + (leap && month > 1);
}

/* Days from 1970-01-01 to a civil date, months counted from zero; months and days out of range carry into the next unit. */
static long long days_from_civil(long long year, long long month, long long day)
{
	year += month / 12;
	month %= 12;
	if (month < 0) {
		month += 12;
		--year;
	}
	year -= month < 2;
	const long long era = (year >= 0 ? year : year - 399) / 400, year_of_era = year - era * 400;
	const long long day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + (153 * (month + (month > 1 ? -2 : 10)) + 2) / 5 + day - 1;
	return era * 146097 + day_of_era - 719468;
}

static FILETIME unix_to_file_time(long long seconds)
{
	const unsigned long long ticks = (unsigned long long)(seconds + 11644473600LL) * 10000000ULL;
	FILETIME time = {(DWORD)ticks, (DWORD)(ticks >> 32)};
	return time;
}

static void fill_tm(RavenTime *out, const SYSTEMTIME *time, int daylight)
{
	out->second = time->wSecond;
	out->minute = time->wMinute;
	out->hour = time->wHour;
	out->day = time->wDay;
	out->month = time->wMonth - 1;
	out->year = time->wYear - 1900;
	out->weekday = time->wDayOfWeek;
	out->yearday = day_of_year(time->wYear, time->wMonth - 1, time->wDay);
	out->daylight = daylight;
}

CRT int _localtime64_s(RavenTime *out, const long long *time)
{
	if (!out || !time || *time < 0) {
		if (out)
			memset(out, 0xff, sizeof *out);
		return 22;
	}
	const FILETIME universal = unix_to_file_time(*time);
	SYSTEMTIME utc, local;
	if (!FileTimeToSystemTime(&universal, &utc) || !SystemTimeToTzSpecificLocalTime(NULL, &utc, &local))
		return 22;
	TIME_ZONE_INFORMATION zone;
	const DWORD kind = GetTimeZoneInformation(&zone);
	fill_tm(out, &local, kind == TIME_ZONE_ID_DAYLIGHT);
	return 0;
}

CRT RavenTime *_localtime64(const long long *time)
{
	static RavenTime result;
	return _localtime64_s(&result, time) ? NULL : &result;
}

CRT int _gmtime64_s(RavenTime *out, const long long *time)
{
	const RavenTime *found = _gmtime64(time);
	if (!out)
		return 22;
	if (!found) {
		memset(out, 0xff, sizeof *out);
		return 22;
	}
	*out = *found;
	return 0;
}

/* _mkgmtime64 reads the fields as UTC, _mktime64 as local time; both normalize the structure they were given. */
CRT long long _mkgmtime64(RavenTime *time)
{
	if (!time)
		return -1;
	const long long seconds = days_from_civil(time->year + 1900LL, time->month, time->day) * 86400 + time->hour * 3600LL + time->minute * 60LL + time->second;
	const RavenTime *normal = _gmtime64(&seconds);
	if (!normal)
		return -1;
	*time = *normal;
	return seconds;
}

CRT long long _mktime64(RavenTime *time)
{
	if (!time)
		return -1;
	const long long as_utc = days_from_civil(time->year + 1900LL, time->month, time->day) * 86400 + time->hour * 3600LL + time->minute * 60LL + time->second;
	const RavenTime *fields = _gmtime64(&as_utc);
	if (!fields)
		return -1;
	SYSTEMTIME local = {(WORD)(fields->year + 1900), (WORD)(fields->month + 1), 0, (WORD)fields->day, (WORD)fields->hour, (WORD)fields->minute, (WORD)fields->second, 0}, utc;
	FILETIME file;
	if (!TzSpecificLocalTimeToSystemTime(NULL, &local, &utc) || !SystemTimeToFileTime(&utc, &file))
		return -1;
	const long long seconds = unix_seconds(file);
	_localtime64_s(time, &seconds);
	return seconds;
}

static int print_into(char *buffer, size_t size, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const int result = __stdio_common_vsprintf(0, buffer, size, format, NULL, arguments);
	va_end(arguments);
	return result;
}

CRT char *_ctime64(const long long *time)
{
	static char text[32];
	static const char days[] = "SunMonTueWedThuFriSat", months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const RavenTime *local = _localtime64(time);
	if (!local)
		return NULL;
	print_into(text, sizeof text, "%.3s %.3s %02d %02d:%02d:%02d %d\n", days + local->weekday * 3, months + local->month * 3, local->day,
		local->hour, local->minute, local->second, local->year + 1900);
	return text;
}

CRT double _difftime64(long long later, long long earlier) { return (double)(later - earlier); }

/* clock: milliseconds of wall time since the process started, as on Windows (CLOCKS_PER_SEC is 1000). */
CRT long clock(void)
{
	static ULONGLONG start;
	if (!start)
		start = GetTickCount64();
	return (long)(GetTickCount64() - start);
}

typedef struct {
	long long time;
	unsigned short milliseconds;
	short zone, daylight;
} RavenTimeB;

CRT void _ftime64(RavenTimeB *out)
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	const unsigned long long ticks = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
	_tzset();
	out->time = unix_seconds(now);
	out->milliseconds = (unsigned short)(ticks / 10000 % 1000);
	out->zone = (short)(_timezone / 60);
	out->daylight = (short)_daylight;
}

CRT int _ftime64_s(RavenTimeB *out)
{
	if (!out)
		return 22;
	_ftime64(out);
	return 0;
}

/* ---- descriptors ---- */

static intptr_t descriptor_handle(int fd)
{
	const Descriptor *entry = descriptor_of(fd);
	return entry ? (intptr_t)entry->handle : -1;
}

CRT long _lseek(int fd, long offset, int origin) { return (long)_lseeki64(fd, offset, origin); }
CRT long long _telli64(int fd) { return _lseeki64(fd, 0, 1); }
CRT long _tell(int fd) { return (long)_lseeki64(fd, 0, 1); }

CRT long long _filelengthi64(int fd)
{
	const Descriptor *entry = descriptor_of(fd);
	LARGE_INTEGER size;
	if (!entry || !GetFileSizeEx(entry->handle, &size)) {
		set_errno(9);
		return -1;
	}
	return size.QuadPart;
}

CRT long _filelength(int fd) { return (long)_filelengthi64(fd); }

CRT int _eof(int fd)
{
	const long long position = _telli64(fd), size = _filelengthi64(fd);
	return position < 0 || size < 0 ? -1 : position >= size;
}

CRT int _commit(int fd)
{
	const Descriptor *entry = descriptor_of(fd);
	if (!entry) {
		set_errno(9);
		return -1;
	}
	FlushFileBuffers(entry->handle);
	return 0;
}

CRT int _chsize_s(int fd, long long size)
{
	const Descriptor *entry = descriptor_of(fd);
	LARGE_INTEGER here, zero = {0}, end;
	if (!entry)
		return 9;
	if (size < 0)
		return 22;
	end.QuadPart = size;
	if (!SetFilePointerEx(entry->handle, zero, &here, FILE_CURRENT) || !SetFilePointerEx(entry->handle, end, NULL, FILE_BEGIN) ||
		!SetEndOfFile(entry->handle))
		return 13;
	SetFilePointerEx(entry->handle, here, NULL, FILE_BEGIN);
	return 0;
}

CRT int _chsize(int fd, long size)
{
	const int error = _chsize_s(fd, size);
	if (error)
		set_errno(error);
	return error ? -1 : 0;
}

/* _locking takes a range from the current position: unlock, lock (retrying for about ten seconds) or lock without waiting. */
CRT int _locking(int fd, int mode, long count)
{
	const Descriptor *entry = descriptor_of(fd);
	const long long start = _telli64(fd);
	if (!entry || start < 0 || count < 0) {
		set_errno(entry ? 22 : 9);
		return -1;
	}
	const DWORD low = (DWORD)start, high = (DWORD)(start >> 32);
	if (mode == 0)
		return UnlockFile(entry->handle, low, high, (DWORD)count, 0) ? 0 : (set_errno(13), -1);
	for (int attempt = 0; attempt < (mode == 1 || mode == 3 ? 10 : 1); ++attempt) {
		if (LockFile(entry->handle, low, high, (DWORD)count, 0))
			return 0;
		if (mode == 1 || mode == 3)
			Sleep(1000);
	}
	set_errno(36);
	return -1;
}

CRT int _open_osfhandle(intptr_t handle, int flags)
{
	if (handle == (intptr_t)INVALID_HANDLE_VALUE || !handle) {
		set_errno(9);
		return -1;
	}
	return allocate_descriptor((HANDLE)handle, !(flags & 0x8000) && (flags & 0x4000));
}

CRT int _dup(int fd)
{
	const Descriptor *entry = descriptor_of(fd);
	HANDLE copy;
	if (!entry || !DuplicateHandle(GetCurrentProcess(), entry->handle, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
		set_errno(9);
		return -1;
	}
	const int made = allocate_descriptor(copy, entry->text);
	if (made < 0)
		CloseHandle(copy);
	return made;
}

CRT int _dup2(int from, int to)
{
	const Descriptor *entry = descriptor_of(from);
	HANDLE copy;
	if (!entry || to < 0 || to >= MAX_DESCRIPTORS ||
		!DuplicateHandle(GetCurrentProcess(), entry->handle, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
		set_errno(9);
		return -1;
	}
	if (descriptors[to].open && to > 2)
		CloseHandle(descriptors[to].handle);
	descriptors[to].handle = copy;
	descriptors[to].open = 1;
	descriptors[to].text = entry->text;
	if (to <= 2) {
		static const DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
		SetStdHandle(ids[to], copy);
		streams[to].handle = copy;
	}
	return 0;
}

CRT int _pipe(int *descriptors_out, unsigned size, int flags)
{
	HANDLE reader, writer;
	SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, !(flags & 0x80)};
	if (!descriptors_out || !CreatePipe(&reader, &writer, &inherit, size)) {
		set_errno(24);
		return -1;
	}
	const int text = !(flags & 0x8000) && (flags & 0x4000);
	descriptors_out[0] = allocate_descriptor(reader, text);
	descriptors_out[1] = allocate_descriptor(writer, text);
	if (descriptors_out[0] < 0 || descriptors_out[1] < 0) {
		CloseHandle(reader);
		CloseHandle(writer);
		return -1;
	}
	return 0;
}

CRT int _wopen(const wchar_t *path, int flags, ...)
{
	int text;
	HANDLE handle = open_file(path, flags, &text);
	if (handle == INVALID_HANDLE_VALUE)
		return -1;
	const int fd = allocate_descriptor(handle, text);
	if (fd < 0)
		CloseHandle(handle);
	return fd;
}

CRT int _wsopen(const wchar_t *path, int flags, int share, ...) { return _wopen(path, flags); }
CRT int _sopen(const char *path, int flags, int share, ...) { return _open(path, flags); }

CRT int _wsopen_dispatch(const wchar_t *path, int flags, int share, int permission, int *fd, int secure)
{
	if (!fd)
		return 22;
	*fd = _wopen(path, flags, permission);
	return *fd < 0 ? *_errno() : 0;
}

CRT int _sopen_dispatch(const char *path, int flags, int share, int permission, int *fd, int secure)
{
	if (!fd)
		return 22;
	*fd = _open(path, flags, permission);
	return *fd < 0 ? *_errno() : 0;
}

/* ---- file names ---- */

static int windows_failure(void)
{
	const DWORD error = GetLastError();
	set_errno(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 2 : error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ?
		17 : error == ERROR_DIR_NOT_EMPTY ? 41 : 13);
	return -1;
}

/* _stat64 and _wstat64 fill msvcrt's struct _stat64; _stat64i32 has a 32-bit size between the device fields and the times. */
static int stat_wide(const wchar_t *path, StatBuffer *out)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!path || !GetFileAttributesExW(path, GetFileExInfoStandard, &data))
		return windows_failure();
	memset(out, 0, sizeof *out);
	out->st_mode = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x4000 | 0x1ed : 0x8000 | ((data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0x124 : 0x1b6);
	out->st_nlink = 1;
	out->st_size = ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
	out->st_atime = file_time_to_unix(data.ftLastAccessTime);
	out->st_mtime = file_time_to_unix(data.ftLastWriteTime);
	out->st_ctime = file_time_to_unix(data.ftCreationTime);
	/* Drive letters count from zero for A:. */
	if (path[0] && path[1] == L':')
		out->st_dev = out->st_rdev = (unsigned)((path[0] | 0x20) - L'a');
	return 0;
}

CRT int _wstat64(const wchar_t *path, StatBuffer *out) { return stat_wide(path, out); }

CRT int _stat64(const char *path, StatBuffer *out)
{
	wchar_t *wide = widen_path(path);
	const int result = wide ? stat_wide(wide, out) : -1;
	HeapFree(GetProcessHeap(), 0, wide);
	return result;
}

typedef struct {
	unsigned st_dev;
	unsigned short st_ino, st_mode;
	short st_nlink, st_uid, st_gid;
	unsigned st_rdev;
	long st_size;
	long long st_atime, st_mtime, st_ctime;
} StatBuffer64i32;

static void narrow_stat(const StatBuffer *from, StatBuffer64i32 *to)
{
	to->st_dev = from->st_dev;
	to->st_ino = from->st_ino;
	to->st_mode = from->st_mode;
	to->st_nlink = from->st_nlink;
	to->st_uid = from->st_uid;
	to->st_gid = from->st_gid;
	to->st_rdev = from->st_rdev;
	to->st_size = (long)from->st_size;
	to->st_atime = from->st_atime;
	to->st_mtime = from->st_mtime;
	to->st_ctime = from->st_ctime;
}

CRT int _wstat64i32(const wchar_t *path, StatBuffer64i32 *out)
{
	StatBuffer full;
	if (stat_wide(path, &full))
		return -1;
	narrow_stat(&full, out);
	return 0;
}

CRT int _stat64i32(const char *path, StatBuffer64i32 *out)
{
	StatBuffer full;
	if (_stat64(path, &full))
		return -1;
	narrow_stat(&full, out);
	return 0;
}

CRT int _fstat64i32(int fd, StatBuffer64i32 *out)
{
	StatBuffer full;
	if (_fstat64(fd, &full))
		return -1;
	narrow_stat(&full, out);
	return 0;
}

CRT int _waccess(const wchar_t *path, int mode)
{
	const DWORD attributes = path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return windows_failure();
	if ((mode & 2) && (attributes & FILE_ATTRIBUTE_READONLY) && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		set_errno(13);
		return -1;
	}
	return 0;
}

CRT int _waccess_s(const wchar_t *path, int mode) { return _waccess(path, mode) ? *_errno() : 0; }

/* The write permission bit decides the read-only attribute; the other bits have no Windows counterpart. */
CRT int _wchmod(const wchar_t *path, int mode)
{
	const DWORD attributes = path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return windows_failure();
	const DWORD wanted = (mode & 0x80) ? attributes & ~FILE_ATTRIBUTE_READONLY : attributes | FILE_ATTRIBUTE_READONLY;
	return SetFileAttributesW(path, wanted) ? 0 : windows_failure();
}

CRT int _chmod(const char *path, int mode)
{
	wchar_t *wide = widen_path(path);
	const int result = wide ? _wchmod(wide, mode) : -1;
	HeapFree(GetProcessHeap(), 0, wide);
	return result;
}

CRT int _wmkdir(const wchar_t *path) { return CreateDirectoryW(path, NULL) ? 0 : windows_failure(); }
CRT int _rmdir(const char *path) { return RemoveDirectoryA(path) ? 0 : windows_failure(); }
CRT int _unlink(const char *path) { return DeleteFileA(path) ? 0 : windows_failure(); }
CRT int _wunlink(const wchar_t *path) { return DeleteFileW(path) ? 0 : windows_failure(); }
CRT int _chdir(const char *path) { return SetCurrentDirectoryA(path) ? 0 : windows_failure(); }

typedef struct {
	long long access_time, modify_time;
} RavenUtime;

CRT int _wutime64(const wchar_t *path, const RavenUtime *times)
{
	HANDLE file = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return windows_failure();
	FILETIME access, modify;
	if (times) {
		access = unix_to_file_time(times->access_time);
		modify = unix_to_file_time(times->modify_time);
	} else {
		GetSystemTimeAsFileTime(&modify);
		access = modify;
	}
	const BOOL done = SetFileTime(file, NULL, &access, &modify);
	CloseHandle(file);
	return done ? 0 : windows_failure();
}

CRT int _utime64(const char *path, const RavenUtime *times)
{
	wchar_t *wide = widen_path(path);
	const int result = wide ? _wutime64(wide, times) : -1;
	HeapFree(GetProcessHeap(), 0, wide);
	return result;
}

CRT wchar_t *_wfullpath(wchar_t *buffer, const wchar_t *path, size_t size)
{
	const DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
	if (!needed)
		return NULL;
	if (!buffer) {
		buffer = malloc(needed * sizeof(wchar_t));
		size = needed;
	}
	if (!buffer || size < needed) {
		set_errno(34);
		return NULL;
	}
	GetFullPathNameW(path, (DWORD)size, buffer, NULL);
	return buffer;
}

/* The current directory of another drive is that drive's root here, where only the current drive keeps one. */
CRT wchar_t *_wgetdcwd(int drive, wchar_t *buffer, int size)
{
	wchar_t current[MAX_PATH];
	GetCurrentDirectoryW(MAX_PATH, current);
	if (drive && (current[0] | 0x20) - L'a' + 1 != drive) {
		const wchar_t root[4] = {(wchar_t)(L'A' + drive - 1), L':', L'\\', 0};
		if (!buffer)
			return _wcsdup(root);
		if (size < 4) {
			set_errno(34);
			return NULL;
		}
		memcpy(buffer, root, sizeof root);
		return buffer;
	}
	return _wgetcwd(buffer, size);
}

CRT int _getdrive(void)
{
	wchar_t current[MAX_PATH];
	return GetCurrentDirectoryW(MAX_PATH, current) && current[1] == L':' ? (current[0] | 0x20) - L'a' + 1 : 0;
}

CRT int _getpid(void) { return (int)GetCurrentProcessId(); }

/* struct _wfinddata64_t and _finddata64_t: attributes, three 64-bit times, a 64-bit size and the name. */
typedef struct {
	unsigned attributes;
	long long created, accessed, written, size;
	wchar_t name[260];
} RavenFindWide;

static void fill_found(RavenFindWide *out, const WIN32_FIND_DATAW *data)
{
	out->attributes = data->dwFileAttributes;
	out->created = file_time_to_unix(data->ftCreationTime);
	out->accessed = file_time_to_unix(data->ftLastAccessTime);
	out->written = file_time_to_unix(data->ftLastWriteTime);
	out->size = ((long long)data->nFileSizeHigh << 32) | data->nFileSizeLow;
	memcpy(out->name, data->cFileName, sizeof out->name);
}

CRT intptr_t _wfindfirst64(const wchar_t *pattern, RavenFindWide *out)
{
	WIN32_FIND_DATAW data;
	HANDLE search = FindFirstFileW(pattern, &data);
	if (search == INVALID_HANDLE_VALUE) {
		windows_failure();
		return -1;
	}
	fill_found(out, &data);
	return (intptr_t)search;
}

CRT int _wfindnext64(intptr_t search, RavenFindWide *out)
{
	WIN32_FIND_DATAW data;
	if (!FindNextFileW((HANDLE)search, &data)) {
		set_errno(2);
		return -1;
	}
	fill_found(out, &data);
	return 0;
}

CRT int _findclose(intptr_t search) { return FindClose((HANDLE)search) ? 0 : (set_errno(9), -1); }

/* ---- streams ---- */

CRT int fopen_s(void **stream, const char *path, const char *mode)
{
	if (!stream)
		return 22;
	*stream = fopen(path, mode);
	return *stream ? 0 : *_errno() ? *_errno() : 2;
}

CRT int _wfopen_s(void **stream, const wchar_t *path, const wchar_t *mode)
{
	if (!stream)
		return 22;
	*stream = _wfopen(path, mode);
	return *stream ? 0 : *_errno() ? *_errno() : 2;
}

/* freopen opens the new file into the same FILE, so a program keeps writing through the pointer it holds, stdout among them. */
static void *reopen(void *stream, void *opened)
{
	Stream *old = stream, *fresh = opened;
	if (!old || !fresh)
		return NULL;
	fflush(old);
	if (old->index > 2) {
		CloseHandle(old->handle);
		descriptors[old->index].open = 0;
	}
	const int index = old->index <= 2 ? old->index : fresh->index;
	if (index <= 2) {
		descriptors[index].handle = fresh->handle;
		descriptors[index].open = 1;
		descriptors[fresh->index].open = 0;
	}
	HeapFree(GetProcessHeap(), 0, old->buffer);
	*old = *fresh;
	old->index = index;
	DeleteCriticalSection(stream_lock(fresh));
	HeapFree(GetProcessHeap(), 0, fresh);
	return old;
}

CRT void *freopen(const char *path, const char *mode, void *stream) { return reopen(stream, path ? fopen(path, mode) : NULL); }
CRT void *_wfreopen(const wchar_t *path, const wchar_t *mode, void *stream) { return reopen(stream, path ? _wfopen(path, mode) : NULL); }

/* The buffer refill and flush that msvcrt's getc and putc macros call when a FILE's count runs out. */
CRT int _filbuf(void *stream)
{
	((Stream *)stream)->macro_count = 0;
	return fgetc(stream);
}

CRT int _flsbuf(int c, void *stream)
{
	((Stream *)stream)->macro_count = 0;
	return fputc(c, stream);
}

CRT void clearerr(void *stream)
{
	if (stream)
		((Stream *)stream)->error = ((Stream *)stream)->end_of_file = 0;
}

CRT int fputws(const wchar_t *text, void *stream)
{
	for (; *text; ++text)
		if (fputwc(*text, stream) == 0xffff)
			return -1;
	return 0;
}

CRT wchar_t *fgetws(wchar_t *buffer, int size, void *stream)
{
	int used = 0;
	if (!buffer || size <= 0)
		return NULL;
	while (used + 1 < size) {
		const wint_t c = getwc(stream);
		if (c == 0xffff)
			break;
		buffer[used++] = (wchar_t)c;
		if (c == L'\n')
			break;
	}
	if (!used)
		return NULL;
	buffer[used] = 0;
	return buffer;
}

CRT int getchar(void) { return fgetc(__acrt_iob_func(0)); }
CRT int _putws(const wchar_t *text) { return fputws(text, __acrt_iob_func(1)) || fputwc(L'\n', __acrt_iob_func(1)) == 0xffff ? -1 : 0; }

/* A console key without echo: the next byte of standard input. _kbhit reports a waiting key event on a console. */
CRT int _getch(void)
{
	unsigned char byte;
	DWORD read = 0;
	return ReadFile(GetStdHandle(STD_INPUT_HANDLE), &byte, 1, &read, NULL) && read ? byte : -1;
}

CRT int _kbhit(void)
{
	DWORD events = 0;
	return GetNumberOfConsoleInputEvents(GetStdHandle(STD_INPUT_HANDLE), &events) && events > 0;
}

CRT void perror(const char *prefix)
{
	void *error = __acrt_iob_func(2);
	if (prefix && *prefix) {
		fputs(prefix, error);
		fputs(": ", error);
	}
	fputs(strerror(*_errno()), error);
	fputc('\n', error);
}

CRT void _wperror(const wchar_t *prefix)
{
	void *error = __acrt_iob_func(2);
	if (prefix && *prefix) {
		fputws(prefix, error);
		fputs(": ", error);
	}
	fputs(strerror(*_errno()), error);
	fputc('\n', error);
}

/* tmpnam names a file in the temporary directory that does not exist yet; tmpfile opens one that goes away when it is closed. */
CRT char *tmpnam(char *buffer)
{
	static char name[MAX_PATH];
	char directory[MAX_PATH];
	char *out = buffer ? buffer : name;
	if (!GetTempPathA(MAX_PATH, directory) || !GetTempFileNameA(directory, "s", 0, out))
		return NULL;
	DeleteFileA(out);
	return out;
}

CRT void *tmpfile(void)
{
	char directory[MAX_PATH], name[MAX_PATH];
	if (!GetTempPathA(MAX_PATH, directory) || !GetTempFileNameA(directory, "t", 0, name))
		return NULL;
	HANDLE handle = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return NULL;
	const int fd = allocate_descriptor(handle, 0);
	if (fd < 0) {
		CloseHandle(handle);
		return NULL;
	}
	Stream *stream = new_stream(handle, fd, 0, 1);
	if (!stream)
		_close(fd);
	return stream;
}

/* ---- formatted input and positional output ---- */

/* A scanf from a stream reads one line and scans it; programs read records line by line this way. */
CRT int __stdio_common_vfscanf(unsigned long long options, void *stream, const char *format, void *locale, va_list arguments)
{
	char line[4096];
	if (!fgets(line, sizeof line, stream))
		return -1;
	return vsscanf(line, format, arguments);
}

CRT int __stdio_common_vsscanf(unsigned long long options, const char *input, size_t count, const char *format, void *locale, va_list arguments)
{
	if (count == (size_t)-1 || !input)
		return vsscanf(input, format, arguments);
	char *copy = malloc(count + 1);
	if (!copy)
		return -1;
	memcpy(copy, input, count);
	copy[count] = 0;
	const int result = vsscanf(copy, format, arguments);
	free(copy);
	return result;
}

CRT int __stdio_common_vswscanf(unsigned long long options, const wchar_t *input, size_t count, const wchar_t *format, void *locale, va_list arguments)
{
	if (count == (size_t)-1 || !input)
		return vswscanf(input, format, arguments);
	wchar_t *copy = malloc((count + 1) * sizeof(wchar_t));
	if (!copy)
		return -1;
	memcpy(copy, input, count * sizeof(wchar_t));
	copy[count] = 0;
	const int result = vswscanf(copy, format, arguments);
	free(copy);
	return result;
}

/* The positional forms take the same formats; arguments named out of order are not reordered. */
CRT int __stdio_common_vsprintf_p(unsigned long long options, char *buffer, size_t count, const char *text, void *locale, va_list arguments)
{
	return __stdio_common_vsprintf(options, buffer, count, text, locale, arguments);
}

CRT int __stdio_common_vswprintf_p(unsigned long long options, wchar_t *buffer, size_t count, const wchar_t *text, void *locale, va_list arguments)
{
	return __stdio_common_vswprintf(options, buffer, count, text, locale, arguments);
}

/* ---- errors ---- */

#undef _sys_nerr
#undef _sys_errlist
CRT int _sys_nerr = 43;
CRT char *_sys_errlist[43];

static void fill_error_list(void)
{
	if (!_sys_errlist[0])
		for (int code = 0; code < 43; ++code)
			_sys_errlist[code] = strerror(code);
}

CRT char **__sys_errlist(void) { fill_error_list(); return _sys_errlist; }
CRT int *__sys_nerr(void) { return &_sys_nerr; }

CRT int _get_errno(int *value)
{
	if (!value)
		return 22;
	*value = *_errno();
	return 0;
}

CRT int _set_errno(int value) { *_errno() = value; return 0; }

CRT int _get_doserrno(unsigned long *value)
{
	if (!value) {
		_invalid_parameter_noinfo();
		*_errno() = 22;
		return 22;
	}
	*value = *__doserrno();
	return 0;
}

CRT int _set_doserrno(unsigned long value) { *__doserrno() = value; return 0; }

CRT int strerror_s(char *buffer, size_t size, int code)
{
	if (!buffer || !size)
		return 22;
	const char *text = strerror(code);
	size_t length = strlen(text);
	if (length >= size)
		length = size - 1;
	memcpy(buffer, text, length);
	buffer[length] = 0;
	return 0;
}

/* ---- environment ---- */

CRT wchar_t *_wgetenv(const wchar_t *name)
{
	static struct { wchar_t *name, *value; } cache[64];
	size_t slot = 64;
	for (size_t at = 0; at < 64; ++at)
		if (cache[at].name && !wcscmp(cache[at].name, name)) {
			slot = at;
			break;
		} else if (!cache[at].name && slot == 64) {
			slot = at;
		}
	if (slot == 64)
		return NULL;
	const DWORD needed = GetEnvironmentVariableW(name, NULL, 0);
	if (!needed)
		return NULL;
	wchar_t *value = HeapAlloc(GetProcessHeap(), 0, needed * sizeof(wchar_t));
	if (!value)
		return NULL;
	GetEnvironmentVariableW(name, value, needed);
	if (!cache[slot].name)
		cache[slot].name = _wcsdup(name);
	else
		HeapFree(GetProcessHeap(), 0, cache[slot].value);
	cache[slot].value = value;
	return value;
}

CRT int getenv_s(size_t *length, char *buffer, size_t size, const char *name)
{
	if (!length || !name)
		return 22;
	const DWORD needed = GetEnvironmentVariableA(name, NULL, 0);
	*length = needed;
	if (!needed) {
		if (buffer && size)
			buffer[0] = 0;
		return 0;
	}
	if (!buffer || size < needed)
		return buffer ? 34 : 0;
	GetEnvironmentVariableA(name, buffer, (DWORD)size);
	return 0;
}

static wchar_t **wide_environment;

CRT int _initialize_wide_environment(void)
{
	LPWCH block = GetEnvironmentStringsW(), at;
	size_t count = 0, index = 0;
	if (!block)
		return 0;
	for (at = block; *at; at += wcslen(at) + 1)
		++count;
	wide_environment = malloc((count + 1) * sizeof(wchar_t *));
	for (at = block; *at && wide_environment; at += wcslen(at) + 1)
		wide_environment[index++] = _wcsdup(at);
	if (wide_environment)
		wide_environment[index] = NULL;
	FreeEnvironmentStringsW(block);
	return 0;
}

CRT wchar_t ***__p__wenviron(void) { return &wide_environment; }
CRT wchar_t **_get_initial_wide_environment(void) { return wide_environment; }

/* What WinMain receives: the command line after the program name and the blanks that follow it. */
static const wchar_t *after_program_name(const wchar_t *line)
{
	int quoted = 0;
	while (*line && (quoted || (*line != L' ' && *line != L'\t'))) {
		if (*line == L'"')
			quoted = !quoted;
		++line;
	}
	while (*line == L' ' || *line == L'\t')
		++line;
	return line;
}

CRT wchar_t *_get_wide_winmain_command_line(void) { return (wchar_t *)after_program_name(GetCommandLineW()); }

CRT char *_get_narrow_winmain_command_line(void)
{
	const char *line = GetCommandLineA();
	int quoted = 0;
	while (*line && (quoted || (*line != ' ' && *line != '\t'))) {
		if (*line == '"')
			quoted = !quoted;
		++line;
	}
	while (*line == ' ' || *line == '\t')
		++line;
	return (char *)line;
}

/* ---- startup and termination ---- */

CRT int _seh_filter_exe(unsigned long code, void *pointers) { return _seh_filter_dll(code, pointers); }
CRT unsigned _set_abort_behavior(unsigned flags, unsigned mask) { return 0; }

static void (__cdecl *quick_exit_functions[32])(void);
static int quick_exit_count;

CRT int _crt_at_quick_exit(void (__cdecl *function)(void))
{
	if (quick_exit_count >= 32)
		return -1;
	quick_exit_functions[quick_exit_count++] = function;
	return 0;
}

CRT int at_quick_exit(void (__cdecl *function)(void)) { return _crt_at_quick_exit(function); }

CRT void quick_exit(int status)
{
	while (quick_exit_count > 0)
		quick_exit_functions[--quick_exit_count]();
	_exit(status);
}

/* The callback that runs thread-local destructors of the executable; threads here end without running it. */
CRT void _register_thread_local_exe_atexit_callback(void *callback) {}

/* ---- locales ---- */

/* Every locale is the C locale with the ANSI code page here, so a created locale is one shared object that formats as the default. */
static struct { int code_page; } shared_locale = {1252};

CRT void *_create_locale(int category, const char *name) { return &shared_locale; }
CRT void *_wcreate_locale(int category, const wchar_t *name) { return &shared_locale; }
CRT void _free_locale(void *locale) {}
CRT void *_get_current_locale(void) { return &shared_locale; }

CRT long _wcstol_l(const wchar_t *text, wchar_t **end, int base, _locale_t locale) { return wcstol(text, end, base); }
CRT unsigned long _wcstoul_l(const wchar_t *text, wchar_t **end, int base, _locale_t locale) { return wcstoul(text, end, base); }
CRT double _wcstod_l(const wchar_t *text, wchar_t **end, _locale_t locale) { return raven_wcstod(text, end); }

/* ---- strings and conversions ---- */

CRT unsigned long long _wcstoui64(const wchar_t *text, wchar_t **end, int base) { return (unsigned long long)_wcstoi64(text, end, base); }
CRT long _wtol(const wchar_t *text) { return wcstol(text, NULL, 10); }
CRT long long atoll(const char *text) { return strtoll(text, NULL, 10); }
CRT long long _atoi64(const char *text) { return strtoll(text, NULL, 10); }

CRT size_t mbstowcs(wchar_t *to, const char *text, size_t count)
{
	const int needed = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	if (needed <= 0) {
		set_errno(42);
		return (size_t)-1;
	}
	if (!to)
		return (size_t)needed - 1;
	const int written = MultiByteToWideChar(CP_ACP, 0, text, -1, to, (int)count);
	if (written > 0)
		return (size_t)written - 1;
	/* A buffer too small for the terminator holds what fits, unterminated, as the C library allows. */
	wchar_t *whole = malloc((size_t)needed * sizeof(wchar_t));
	if (!whole)
		return (size_t)-1;
	MultiByteToWideChar(CP_ACP, 0, text, -1, whole, needed);
	memcpy(to, whole, count * sizeof(wchar_t));
	free(whole);
	return count;
}

CRT size_t strspn(const char *text, const char *accepted)
{
	size_t length = 0;
	while (text[length] && strchr(accepted, text[length]))
		++length;
	return length;
}

CRT char *strtok_s(char *text, const char *delimiters, char **context)
{
	if (!context || !delimiters)
		return NULL;
	if (!text)
		text = *context;
	if (!text)
		return NULL;
	text += strspn(text, delimiters);
	if (!*text) {
		*context = text;
		return NULL;
	}
	char *end = text + strcspn(text, delimiters);
	if (*end)
		*end++ = 0;
	*context = end;
	return text;
}

CRT char *strtok(char *text, const char *delimiters)
{
	static char *context;
	return strtok_s(text, delimiters, &context);
}

/* strncpy_s copies at most count characters and always terminates; _TRUNCATE copies what fits. */
CRT int strncpy_s(char *to, size_t size, const char *from, size_t count)
{
	if (!to || !size)
		return 22;
	if (!from) {
		to[0] = 0;
		return 22;
	}
	size_t length = 0;
	while (length < count && from[length])
		++length;
	if (length >= size) {
		if (count != (size_t)-1) {
			to[0] = 0;
			return 34;
		}
		length = size - 1;
	}
	memcpy(to, from, length);
	to[length] = 0;
	return count == (size_t)-1 && from[length] && length == size - 1 ? 80 : 0;
}

CRT wchar_t *wcspbrk(const wchar_t *text, const wchar_t *set)
{
	for (; *text; ++text)
		if (wcschr(set, *text))
			return (wchar_t *)text;
	return NULL;
}

CRT wchar_t *wcsncat(wchar_t *to, const wchar_t *from, size_t count)
{
	wchar_t *end = to + wcslen(to);
	while (count-- && *from)
		*end++ = *from++;
	*end = 0;
	return to;
}

#undef iswprint
#undef iscntrl
#undef ispunct
CRT int iswprint(wint_t c) { return c >= 0x20 && c != 0x7f && !(c >= 0x80 && c < 0xa0) && c != 0xffff; }
CRT int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
CRT int ispunct(int c) { return c > 0x20 && c < 0x7f && !isalnum(c); }

/* div returns the quotient and remainder together; the headers of one width declare it and not the other, so it has a name of its own. */
typedef struct {
	int quotient, remainder;
} RavenDivision;

__asm__(".section .drectve\n\t.ascii \" -export:div=raven_div\"\n\t.text");
RavenDivision raven_div(int numerator, int denominator)
{
	RavenDivision result = {numerator / denominator, numerator % denominator};
	return result;
}

/* ---- math ---- */

/* Rounding to an integer in the current mode: adding and removing 2^52 leaves no fraction bits. Larger values are integers already. */
CRT double rint(double x)
{
	const double big = 4503599627370496.0;
	if (!(x < big && x > -big))
		return x;
	volatile double shifted = x >= 0 ? x + big : x - big;
	const double result = x >= 0 ? shifted - big : shifted + big;
	return result == 0 ? copysign(0.0, x) : result;
}

CRT float rintf(float x) { return (float)rint(x); }
CRT double nearbyint(double x) { return rint(x); }
CRT long lrint(double x) { return (long)rint(x); }
CRT long lrintf(float x) { return (long)rint(x); }
CRT long long llrint(double x) { return (long long)rint(x); }

CRT double trunc(double x)
{
	if (!(x < 4503599627370496.0 && x > -4503599627370496.0))
		return x;
	return copysign((double)(long long)x, x);
}

CRT float truncf(float x) { return (float)trunc(x); }

CRT double round(double x)
{
	double whole = trunc(x);
	if (x - whole >= 0.5)
		whole += 1;
	else if (whole - x >= 0.5)
		whole -= 1;
	return whole;
}

CRT float roundf(float x) { return (float)round(x); }
CRT long lround(double x) { return (long)round(x); }
CRT long long llround(double x) { return (long long)round(x); }
CRT long long llroundf(float x) { return (long long)round(x); }

CRT float expf(float x) { return (float)exp(x); }
CRT float exp2f(float x) { return (float)pow(2.0, x); }
CRT double exp2(double x) { return pow(2.0, x); }
CRT float log10f(float x) { return (float)log10(x); }
CRT double log2(double x) { return log(x) / 0.69314718055994530942; }
CRT long double log2l(long double x) { return log2((double)x); }
CRT float log2f(float x) { return (float)log2(x); }
CRT float _hypotf(float x, float y) { return (float)_hypot(x, y); }

/* _dpcomp compares two doubles: 1 for less, 2 for equal, 4 for greater, 0 when either is not a number. */
CRT int _dpcomp(double x, double y) { return x < y ? 1 : x == y ? 2 : x > y ? 4 : 0; }
CRT int _fdpcomp(float x, float y) { return _dpcomp(x, y); }
CRT int _dsign(double x) { return __builtin_signbit(x) ? 0x8000 : 0; }
CRT int _fdsign(float x) { return __builtin_signbit(x) ? 0x8000 : 0; }

/* No floating point exception is pending when a program asks; operations here do not record them for the C library. */
CRT unsigned _statusfp(void) { return 0; }
