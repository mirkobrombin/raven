/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven C runtime component, part two: file streams, descriptors, locale, time formatting, math and the remaining string and
 * conversion entry points that larger programs import. This file is included at the end of ucrtbase.c, so it shares that file's
 * stream type and helpers. */

/* ---- descriptors ---- */
#define MAX_DESCRIPTORS 1024
typedef struct {
	HANDLE handle;
	int open, text;
} Descriptor;

static Descriptor descriptors[MAX_DESCRIPTORS];

static void set_errno(int value) { *_errno() = value; }

static Descriptor *descriptor_of(int fd)
{
	if (fd < 0 || fd >= MAX_DESCRIPTORS)
		return NULL;
	if (fd <= 2 && !descriptors[fd].open) {
		static const DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
		descriptors[fd].handle = GetStdHandle(ids[fd]);
		descriptors[fd].open = 1;
	}
	return descriptors[fd].open ? &descriptors[fd] : NULL;
}

static int allocate_descriptor(HANDLE handle, int text)
{
	for (int fd = 3; fd < MAX_DESCRIPTORS; ++fd)
		if (!descriptors[fd].open) {
			descriptors[fd].handle = handle;
			descriptors[fd].open = 1;
			descriptors[fd].text = text;
			return fd;
		}
	set_errno(24);
	return -1;
}

static HANDLE open_file(const wchar_t *path, int flags, int *text)
{
	DWORD access = (flags & 3) == 0 ? GENERIC_READ : (flags & 3) == 1 ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
	DWORD disposition = OPEN_EXISTING;
	if (flags & 0x100)
		disposition = (flags & 0x400) ? CREATE_NEW : (flags & 0x200) ? CREATE_ALWAYS : OPEN_ALWAYS;
	else if (flags & 0x200)
		disposition = TRUNCATE_EXISTING;
	HANDLE handle = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		*__doserrno() = error;
		set_errno(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 2 : error == ERROR_FILE_EXISTS ? 17 : 13);
		return INVALID_HANDLE_VALUE;
	}
	if (flags & 8)
		SetFilePointer(handle, 0, NULL, FILE_END);
	/* The default translation mode is text unless the flags or the default mode say binary. */
	*text = (flags & 0x8000) ? 0 : (flags & 0x4000) ? 1 : (fmode & 0x8000) ? 0 : 1;
	return handle;
}

static wchar_t *widen_path(const char *path)
{
	size_t length = strlen(path);
	wchar_t *wide = HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(wchar_t));
	if (!wide)
		return NULL;
	MultiByteToWideChar(CP_ACP, 0, path, (int)length + 1, wide, (int)length + 1);
	return wide;
}

CRT int _open(const char *path, int flags, ...)
{
	int text;
	wchar_t *wide = widen_path(path);
	if (!wide)
		return -1;
	HANDLE handle = open_file(wide, flags, &text);
	HeapFree(GetProcessHeap(), 0, wide);
	if (handle == INVALID_HANDLE_VALUE)
		return -1;
	int fd = allocate_descriptor(handle, text);
	if (fd < 0)
		CloseHandle(handle);
	return fd;
}

CRT int _close(int fd)
{
	Descriptor *entry = descriptor_of(fd);
	if (!entry) {
		set_errno(9);
		return -1;
	}
	if (fd > 2)
		CloseHandle(entry->handle);
	entry->open = 0;
	return 0;
}

CRT int _read(int fd, void *buffer, unsigned count)
{
	Descriptor *entry = descriptor_of(fd);
	DWORD got;
	if (!entry) {
		set_errno(9);
		return -1;
	}
	if (!ReadFile(entry->handle, buffer, count, &got, NULL))
		return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
	if (entry->text) {
		/* Text mode drops the carriage return of a CRLF pair and stops at a Ctrl-Z. */
		unsigned char *bytes = buffer;
		DWORD kept = 0;
		for (DWORD at = 0; at < got; ++at) {
			if (bytes[at] == 0x1a) {
				got = kept;
				break;
			}
			if (bytes[at] == '\r' && at + 1 < got && bytes[at + 1] == '\n')
				continue;
			bytes[kept++] = bytes[at];
		}
		got = kept;
	}
	return (int)got;
}

CRT int _write(int fd, const void *buffer, unsigned count)
{
	Descriptor *entry = descriptor_of(fd);
	DWORD done;
	if (!entry) {
		set_errno(9);
		return -1;
	}
	if (entry->text && fd > 2) {
		const char *bytes = buffer;
		unsigned total = 0;
		while (total < count) {
			char chunk[1024];
			unsigned used = 0, start = total;
			while (total < count && used + 2 <= sizeof chunk) {
				if (bytes[total] == '\n')
					chunk[used++] = '\r';
				chunk[used++] = bytes[total++];
			}
			if (!WriteFile(entry->handle, chunk, used, &done, NULL))
				return -1;
			(void)start;
		}
		return (int)count;
	}
	return WriteFile(entry->handle, buffer, count, &done, NULL) ? (int)done : -1;
}

CRT long long _lseeki64(int fd, long long offset, int origin)
{
	Descriptor *entry = descriptor_of(fd);
	LARGE_INTEGER target, result;
	if (!entry) {
		set_errno(9);
		return -1;
	}
	target.QuadPart = offset;
	if (!SetFilePointerEx(entry->handle, target, &result, (DWORD)origin))
		return -1;
	return result.QuadPart;
}

CRT int _setmode(int fd, int mode)
{
	Descriptor *entry = descriptor_of(fd);
	if (!entry) {
		set_errno(9);
		return -1;
	}
	int previous = entry->text ? 0x4000 : 0x8000;
	entry->text = mode == 0x4000;
	return previous;
}

typedef struct {
	unsigned st_dev;
	unsigned short st_ino, st_mode;
	short st_nlink, st_uid, st_gid;
	unsigned st_rdev;
	long long st_size, st_atime, st_mtime, st_ctime;
} StatBuffer;

static long long file_time_to_unix(FILETIME time)
{
	unsigned long long value = ((unsigned long long)time.dwHighDateTime << 32) | time.dwLowDateTime;
	return value ? (long long)(value / 10000000ULL) - 11644473600LL : 0;
}

CRT int _fstat64(int fd, void *result)
{
	Descriptor *entry = descriptor_of(fd);
	BY_HANDLE_FILE_INFORMATION info;
	StatBuffer *buffer = result;
	if (!entry) {
		set_errno(9);
		return -1;
	}
	memset(buffer, 0, sizeof *buffer);
	DWORD type = GetFileType(entry->handle);
	if (type != FILE_TYPE_DISK) {
		buffer->st_mode = type == FILE_TYPE_CHAR ? 0x2000 : 0x1000;
		buffer->st_nlink = 1;
		return 0;
	}
	if (!GetFileInformationByHandle(entry->handle, &info))
		return -1;
	buffer->st_mode = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x4000 | 0x1ed : 0x8000 | ((info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0x124 : 0x1b6);
	buffer->st_nlink = (short)info.nNumberOfLinks;
	buffer->st_size = ((long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
	buffer->st_atime = file_time_to_unix(info.ftLastAccessTime);
	buffer->st_mtime = file_time_to_unix(info.ftLastWriteTime);
	buffer->st_ctime = file_time_to_unix(info.ftCreationTime);
	return 0;
}

/* ---- FILE streams ---- */
/* An opened stream is laid out as msvcrt's _FILEX: the FILE, then the critical section that guards it. Programs built against msvcrt
   lock a stream by entering the section right after its FILE, so it has to be there and initialized. The standard streams use numbered
   locks instead. */
static CRITICAL_SECTION *stream_lock(Stream *stream) { return (CRITICAL_SECTION *)(stream + 1); }

static Stream *new_stream(HANDLE handle, int fd, int text, int writable)
{
	Stream *stream = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Stream) + sizeof(CRITICAL_SECTION));
	if (!stream)
		return NULL;
	InitializeCriticalSection(stream_lock(stream));
	stream->handle = handle;
	stream->index = fd;
	stream->text = text;
	stream->writable = writable;
	stream->pushed = -1;
	return stream;
}

static int parse_mode(const char *mode, int *text)
{
	int flags = 0;
	*text = (fmode & 0x8000) ? 0 : 1;
	switch (mode[0]) {
	case 'r': flags = 0; break;
	case 'w': flags = 1 | 0x100 | 0x200; break;
	case 'a': flags = 1 | 0x100 | 8; break;
	default: return -1;
	}
	for (const char *at = mode + 1; *at; ++at) {
		if (*at == '+')
			flags = (flags & ~3) | 2;
		else if (*at == 'b')
			flags = (flags & ~0xc000) | 0x8000, *text = 0;
		else if (*at == 't')
			flags = (flags & ~0xc000) | 0x4000, *text = 1;
	}
	return flags;
}

static void *open_stream_wide(const wchar_t *path, const char *mode)
{
	int text;
	int flags = parse_mode(mode, &text);
	if (flags < 0) {
		set_errno(22);
		return NULL;
	}
	HANDLE handle = open_file(path, flags, &text);
	if (handle == INVALID_HANDLE_VALUE)
		return NULL;
	int fd = allocate_descriptor(handle, text);
	if (fd < 0) {
		CloseHandle(handle);
		return NULL;
	}
	Stream *stream = new_stream(handle, fd, text, (flags & 3) != 0);
	if (!stream) {
		_close(fd);
		return NULL;
	}
	return stream;
}

CRT void *fopen(const char *path, const char *mode)
{
	wchar_t *wide = widen_path(path);
	if (!wide)
		return NULL;
	void *stream = open_stream_wide(wide, mode);
	HeapFree(GetProcessHeap(), 0, wide);
	return stream;
}

CRT void *_wfopen(const wchar_t *path, const wchar_t *mode)
{
	char narrow[8];
	size_t at = 0;
	while (mode[at] && at + 1 < sizeof narrow) {
		narrow[at] = mode[at] < 128 ? (char)mode[at] : '?';
		++at;
	}
	narrow[at] = 0;
	return open_stream_wide(path, narrow);
}

CRT void *_fdopen(int fd, const char *mode)
{
	Descriptor *entry = descriptor_of(fd);
	int text;
	int flags = parse_mode(mode, &text);
	if (!entry || flags < 0) {
		set_errno(9);
		return NULL;
	}
	return new_stream(entry->handle, fd, entry->text, (flags & 3) != 0);
}

/* Pending read-ahead is returned to the file position before anything else touches it. */
static void discard_read_ahead(Stream *stream)
{
	if (stream->buffer_position < stream->buffer_length) {
		LARGE_INTEGER back, ignored;
		back.QuadPart = -(long long)(stream->buffer_length - stream->buffer_position);
		SetFilePointerEx(stream->handle, back, &ignored, FILE_CURRENT);
	}
	stream->buffer_length = stream->buffer_position = 0;
}

static void stream_prepare_write(Stream *stream)
{
	if (stream->index > 2 || stream->buffer_length)
		discard_read_ahead(stream);
	stream->pushed = -1;
}

static int stream_get(Stream *stream)
{
	DWORD got;
	if (stream->pushed >= 0) {
		int value = stream->pushed;
		stream->pushed = -1;
		return value;
	}
	for (;;) {
		if (stream->buffer_position == stream->buffer_length) {
			stream->buffer_position = 0;
			if (!stream->buffer && !(stream->buffer = HeapAlloc(GetProcessHeap(), 0, STREAM_BUFFER))) {
				stream->error = 1;
				return -1;
			}
			if (!ReadFile(stream->handle, stream->buffer, STREAM_BUFFER, &got, NULL) || !got) {
				stream->buffer_length = 0;
				if (GetLastError() != ERROR_BROKEN_PIPE && got == 0 && GetLastError() != 0 && GetLastError() != ERROR_HANDLE_EOF)
					stream->error = 1;
				stream->end_of_file = 1;
				return -1;
			}
			stream->buffer_length = got;
		}
		int value = stream->buffer[stream->buffer_position++];
		if (stream->text) {
			if (value == '\r' && stream->buffer_position < stream->buffer_length && stream->buffer[stream->buffer_position] == '\n')
				continue;
			if (value == 0x1a) {
				stream->end_of_file = 1;
				return -1;
			}
		}
		return value;
	}
}

CRT int getc(void *stream) { return stream_get(stream); }
CRT int fgetc(void *stream) { return stream_get(stream); }
CRT int ungetc(int c, void *stream)
{
	Stream *file = stream;
	if (c == -1 || file->pushed >= 0)
		return -1;
	file->pushed = c & 0xff;
	file->end_of_file = 0;
	return c & 0xff;
}

CRT char *fgets(char *buffer, int size, void *stream)
{
	int at = 0;
	if (size <= 0)
		return NULL;
	while (at + 1 < size) {
		int c = stream_get(stream);
		if (c < 0)
			break;
		buffer[at++] = (char)c;
		if (c == '\n')
			break;
	}
	if (!at)
		return NULL;
	buffer[at] = 0;
	return buffer;
}

CRT size_t fread(void *data, size_t size, size_t count, void *stream)
{
	unsigned char *bytes = data;
	size_t total = size * count, done = 0;
	if (!size || !count)
		return 0;
	while (done < total) {
		int c = stream_get(stream);
		if (c < 0)
			break;
		bytes[done++] = (unsigned char)c;
	}
	return done / size;
}

CRT int putc(int c, void *stream) { return fputc(c, stream); }

CRT wint_t fputwc(wchar_t c, void *stream)
{
	/* The narrow code page is the stream encoding here; wider characters become a question mark. */
	return fputc(c < 256 ? (int)c : '?', stream) < 0 ? (wint_t)0xffff : c;
}
CRT wint_t putwc(wchar_t c, void *stream) { return fputwc(c, stream); }
CRT wint_t getwc(void *stream)
{
	int c = stream_get(stream);
	return c < 0 ? (wint_t)0xffff : (wint_t)c;
}
CRT wint_t ungetwc(wint_t c, void *stream) { return c == 0xffff || c > 255 ? (wint_t)0xffff : (wint_t)ungetc((int)c, stream); }

CRT int fseek(void *stream, long offset, int origin)
{
	Stream *file = stream;
	LARGE_INTEGER target, ignored;
	if (origin == 1)
		offset -= (long)(file->buffer_length - file->buffer_position);
	file->buffer_length = file->buffer_position = 0;
	file->pushed = -1;
	file->end_of_file = 0;
	target.QuadPart = offset;
	return SetFilePointerEx(file->handle, target, &ignored, (DWORD)origin) ? 0 : -1;
}

CRT int _fseeki64(void *stream, long long offset, int origin)
{
	Stream *file = stream;
	LARGE_INTEGER target, ignored;
	if (origin == 1)
		offset -= (long long)(file->buffer_length - file->buffer_position);
	file->buffer_length = file->buffer_position = 0;
	file->pushed = -1;
	file->end_of_file = 0;
	target.QuadPart = offset;
	return SetFilePointerEx(file->handle, target, &ignored, (DWORD)origin) ? 0 : -1;
}

CRT long long _ftelli64(void *stream)
{
	Stream *file = stream;
	LARGE_INTEGER zero, position;
	zero.QuadPart = 0;
	if (!SetFilePointerEx(file->handle, zero, &position, FILE_CURRENT))
		return -1;
	return position.QuadPart - (long long)(file->buffer_length - file->buffer_position) - (file->pushed >= 0 ? 1 : 0);
}

CRT long ftell(void *stream) { return (long)_ftelli64(stream); }
CRT void rewind(void *stream) { fseek(stream, 0, 0); ((Stream *)stream)->error = 0; }
CRT int setvbuf(void *stream, char *buffer, int mode, size_t size) { return 0; }
CRT void _lock_file(void *stream)
{
	if (stream && ((Stream *)stream)->index > 2)
		EnterCriticalSection(stream_lock(stream));
}

CRT void _unlock_file(void *stream)
{
	if (stream && ((Stream *)stream)->index > 2)
		LeaveCriticalSection(stream_lock(stream));
}

CRT int fclose(void *stream)
{
	Stream *file = stream;
	if (!file)
		return -1;
	if (file->index > 2) {
		int fd = file->index;
		discard_read_ahead(file);
		_close(fd);
		if (file->buffer)
			HeapFree(GetProcessHeap(), 0, file->buffer);
		DeleteCriticalSection(stream_lock(file));
		HeapFree(GetProcessHeap(), 0, file);
	}
	return 0;
}

CRT int remove(const char *path)
{
	wchar_t *wide = widen_path(path);
	BOOL ok;
	if (!wide)
		return -1;
	ok = DeleteFileW(wide);
	HeapFree(GetProcessHeap(), 0, wide);
	if (!ok)
		set_errno(2);
	return ok ? 0 : -1;
}

/* ---- memory, strings and conversions ---- */
CRT void *_aligned_malloc(size_t size, size_t alignment)
{
	char *raw;
	uintptr_t aligned;
	if (!alignment || (alignment & (alignment - 1)))
		return NULL;
	if (alignment < sizeof(void *))
		alignment = sizeof(void *);
	raw = HeapAlloc(GetProcessHeap(), 0, size + alignment + sizeof(void *));
	if (!raw)
		return NULL;
	aligned = ((uintptr_t)raw + sizeof(void *) + alignment - 1) & ~(uintptr_t)(alignment - 1);
	((void **)aligned)[-1] = raw;
	return (void *)aligned;
}

CRT void _aligned_free(void *block) { if (block) HeapFree(GetProcessHeap(), 0, ((void **)block)[-1]); }

CRT char *_strdup(const char *text)
{
	if (!text)
		return NULL;
	size_t length = strlen(text) + 1;
	char *copy = malloc(length);
	if (copy)
		memcpy(copy, text, length);
	return copy;
}

CRT size_t strnlen(const char *text, size_t limit)
{
	size_t length = 0;
	while (length < limit && text[length])
		++length;
	return length;
}

CRT size_t wcsnlen(const wchar_t *text, size_t limit)
{
	size_t length = 0;
	while (length < limit && text[length])
		++length;
	return length;
}

CRT char *strncat(char *to, const char *from, size_t count)
{
	char *end = to + strlen(to);
	while (count-- && *from)
		*end++ = *from++;
	*end = 0;
	return to;
}

CRT long long strtoll(const char *text, char **end, int base)
{
	int negative;
	unsigned long long value = _strtoull_core(text, end, base, &negative);
	if (negative)
		return value > 0x8000000000000000ULL ? (long long)0x8000000000000000ULL : -(long long)value;
	return value > 0x7fffffffffffffffULL ? 0x7fffffffffffffffLL : (long long)value;
}

CRT unsigned long long strtoull(const char *text, char **end, int base)
{
	int negative;
	unsigned long long value = _strtoull_core(text, end, base, &negative);
	return negative ? (unsigned long long)(-(long long)value) : value;
}

CRT int isprint(int c) { return c >= 32 && c < 127; }
CRT int iswctype(wint_t c, unsigned short type)
{
	/* The type values are the classification bits of the C runtime: upper 1, lower 2, digit 4, space 8, punct 16, cntrl 32, blank 64, hex 128, alpha 256. */
	int upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
	int result = 0;
	if (upper) result |= 1;
	if (lower) result |= 2;
	if (digit) result |= 4;
	if (c == ' ' || (c >= 9 && c <= 13)) result |= 8;
	if ((c > 32 && c < 127) && !upper && !lower && !digit) result |= 16;
	if (c < 32 || c == 127) result |= 32;
	if (c == ' ' || c == '\t') result |= 64;
	if (digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) result |= 128;
	if (upper || lower || c >= 0xc0) result |= 256;
	return (result & type) != 0;
}

CRT unsigned short wctype(const char *name)
{
	static const struct { const char *name; unsigned short bits; } table[] = {
		{"upper", 1}, {"lower", 2}, {"digit", 4}, {"space", 8}, {"punct", 16}, {"cntrl", 32}, {"blank", 64}, {"xdigit", 128},
		{"alpha", 256}, {"alnum", 256 | 4}, {"graph", 256 | 4 | 16}, {"print", 256 | 4 | 16 | 64}};
	for (size_t at = 0; at < sizeof table / sizeof table[0]; ++at)
		if (!strcmp(name, table[at].name))
			return table[at].bits;
	return 0;
}

CRT int strcoll(const char *a, const char *b) { return strcmp(a, b); }
CRT int wcscoll(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
CRT size_t strxfrm(char *to, const char *from, size_t count)
{
	size_t length = strlen(from);
	if (to && count) {
		size_t copy = length < count - 1 ? length : count - 1;
		memcpy(to, from, copy);
		to[copy] = 0;
	}
	return length;
}
CRT size_t wcsxfrm(wchar_t *to, const wchar_t *from, size_t count)
{
	size_t length = wcslen(from);
	if (to && count) {
		size_t copy = length < count - 1 ? length : count - 1;
		memcpy(to, from, copy * sizeof(wchar_t));
		to[copy] = 0;
	}
	return length;
}

CRT int ___mb_cur_max_func(void) { return 1; }
/* The code page of the locale's multibyte characters: zero is the "C" locale, whose bytes stand for themselves. */
CRT unsigned int ___lc_codepage_func(void) { return 0; }
CRT size_t mbrtowc(wchar_t *wide, const char *bytes, size_t count, void *state)
{
	if (!bytes)
		return 0;
	if (!count)
		return (size_t)-2;
	if (wide)
		*wide = (unsigned char)bytes[0];
	return bytes[0] ? 1 : 0;
}
CRT size_t mbrlen(const char *bytes, size_t count, void *state) { return mbrtowc(NULL, bytes, count, state); }
/* The older form: the bytes the character takes, zero for the terminator, -1 when they are not a character. */
CRT int mbtowc(wchar_t *wide, const char *bytes, size_t count)
{
	if (!bytes)
		return 0;
	const size_t taken = mbrtowc(wide, bytes, count, NULL);
	return taken == (size_t)-1 || taken == (size_t)-2 ? -1 : (int)taken;
}

/* The bytes a multibyte character can take in the "C" locale, as a variable older programs read directly. */
int raven_mb_cur_max = 1;
__asm__(".section .drectve\n\t.ascii \" -export:__mb_cur_max=raven_mb_cur_max,DATA\"\n\t.text");
CRT size_t mbsrtowcs(wchar_t *to, const char **from, size_t count, void *state)
{
	size_t done = 0;
	const char *cursor = *from;
	while (!to || done < count) {
		wchar_t c = (unsigned char)*cursor;
		if (to)
			to[done] = c;
		if (!c) {
			if (to)
				*from = NULL;
			return done;
		}
		++cursor;
		++done;
	}
	*from = cursor;
	return done;
}
CRT size_t wcrtomb(char *to, wchar_t c, void *state)
{
	if (!to)
		return 1;
	if (c > 255) {
		set_errno(42);
		return (size_t)-1;
	}
	*to = (char)c;
	return 1;
}

typedef struct {
	char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point, *mon_thousands_sep, *mon_grouping,
		*positive_sign, *negative_sign;
	char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
	wchar_t *w_decimal_point, *w_thousands_sep, *w_int_curr_symbol, *w_currency_symbol, *w_mon_decimal_point, *w_mon_thousands_sep,
		*w_positive_sign, *w_negative_sign;
} LocaleConventions;

CRT void *localeconv(void)
{
	static char dot[] = ".", empty[] = "";
	static wchar_t wide_dot[] = L".", wide_empty[] = L"";
	static LocaleConventions conventions;
	conventions = (LocaleConventions){dot, empty, empty, empty, empty, empty, empty, empty, empty, empty,
		127, 127, 127, 127, 127, 127, 127, 127, wide_dot, wide_empty, wide_empty, wide_empty, wide_empty, wide_empty, wide_empty, wide_empty};
	return &conventions;
}

CRT char *setlocale(int category, const char *name)
{
	static char c_locale[] = "C";
	if (name && *name && strcmp(name, "C") && strcmp(name, "POSIX"))
		return NULL;
	return c_locale;
}

CRT char *strerror(int code)
{
	switch (code) {
	case 0: return "No error";
	case 1: return "Operation not permitted";
	case 2: return "No such file or directory";
	case 9: return "Bad file descriptor";
	case 12: return "Not enough space";
	case 13: return "Permission denied";
	case 17: return "File exists";
	case 22: return "Invalid argument";
	case 24: return "Too many open files";
	case 28: return "No space left on device";
	case 33: return "Domain error";
	case 34: return "Result too large";
	case 42: return "Illegal byte sequence";
	default: return "Unknown error";
	}
}

/* _putenv takes "NAME=value" and sets the variable in the process environment that getenv reads; an empty value removes it. */
CRT int _putenv_s(const char *name, const char *value)
{
	if (!name || !*name || !value)
		return 22;
	return SetEnvironmentVariableA(name, *value ? value : NULL) || !*value ? 0 : 22;
}

CRT int _wputenv_s(const wchar_t *name, const wchar_t *value)
{
	if (!name || !*name || !value)
		return 22;
	return SetEnvironmentVariableW(name, *value ? value : NULL) || !*value ? 0 : 22;
}

CRT int _putenv(const char *assignment)
{
	const char *equals = assignment ? strchr(assignment, '=') : NULL;
	char name[1024];
	if (!equals || equals == assignment || (size_t)(equals - assignment) >= sizeof(name))
		return -1;
	memcpy(name, assignment, (size_t)(equals - assignment));
	name[equals - assignment] = 0;
	return _putenv_s(name, equals + 1) ? -1 : 0;
}

CRT int _wputenv(const wchar_t *assignment)
{
	const wchar_t *equals = assignment ? wcschr(assignment, L'=') : NULL;
	wchar_t name[1024];
	if (!equals || equals == assignment || (size_t)(equals - assignment) >= sizeof(name) / sizeof(*name))
		return -1;
	memcpy(name, assignment, (size_t)(equals - assignment) * sizeof(wchar_t));
	name[equals - assignment] = 0;
	return _wputenv_s(name, equals + 1) ? -1 : 0;
}

CRT char *getenv(const char *name)
{
	static struct { char *name, *value; } cache[64];
	size_t slot = 64;
	for (size_t at = 0; at < 64; ++at)
		if (cache[at].name && !strcmp(cache[at].name, name)) {
			slot = at;
			break;
		} else if (!cache[at].name && slot == 64) {
			slot = at;
		}
	if (slot == 64)
		return NULL;
	DWORD needed = GetEnvironmentVariableA(name, NULL, 0);
	if (!needed) {
		if (cache[slot].value) {
			HeapFree(GetProcessHeap(), 0, cache[slot].value);
			cache[slot].value = NULL;
		}
		return NULL;
	}
	char *value = HeapAlloc(GetProcessHeap(), 0, needed);
	if (!value)
		return NULL;
	GetEnvironmentVariableA(name, value, needed);
	if (!cache[slot].name)
		cache[slot].name = _strdup(name);
	if (cache[slot].value)
		HeapFree(GetProcessHeap(), 0, cache[slot].value);
	cache[slot].value = value;
	return value;
}

CRT int rand_s(unsigned int *value)
{
	static unsigned long long state;
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	state ^= (unsigned long long)counter.QuadPart * 0x9e3779b97f4a7c15ULL ^ GetCurrentProcessId() ^ ((unsigned long long)GetTickCount() << 32);
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	if (!value)
		return 22;
	*value = (unsigned int)(state >> 16);
	return 0;
}

/* ---- sorting ---- */
static void swap_bytes(unsigned char *a, unsigned char *b, size_t size)
{
	while (size--) {
		unsigned char t = *a;
		*a++ = *b;
		*b++ = t;
	}
}

static void sift_down(unsigned char *base, size_t size, size_t start, size_t count, int (__cdecl *compare)(const void *, const void *))
{
	for (size_t root = start;;) {
		size_t child = root * 2 + 1;
		if (child >= count)
			return;
		if (child + 1 < count && compare(base + child * size, base + (child + 1) * size) < 0)
			++child;
		if (compare(base + root * size, base + child * size) >= 0)
			return;
		swap_bytes(base + root * size, base + child * size, size);
		root = child;
	}
}

CRT void qsort(void *data, size_t count, size_t size, int (__cdecl *compare)(const void *, const void *))
{
	unsigned char *base = data;
	if (count < 2 || !size)
		return;
	for (size_t start = count / 2; start-- > 0;)
		sift_down(base, size, start, count, compare);
	for (size_t end = count - 1; end > 0; --end) {
		swap_bytes(base, base + end * size, size);
		sift_down(base, size, 0, end, compare);
	}
}

/* ---- time formatting ---- */
typedef struct {
	int sec, min, hour, mday, mon, year, wday, yday, isdst;
} BrokenTime;

static const char *const day_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const month_names[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

static size_t append_number(char *to, size_t used, size_t capacity, int value, int digits)
{
	char text[16];
	int length = 0;
	do { text[length++] = (char)('0' + value % 10); value /= 10; } while (value && length < 15);
	while (length < digits && length < 15)
		text[length++] = '0';
	while (length && used < capacity)
		to[used++] = text[--length];
	return used;
}

static size_t append_text(char *to, size_t used, size_t capacity, const char *text, size_t limit)
{
	while (*text && limit-- && used < capacity)
		to[used++] = *text++;
	return used;
}

static size_t format_time(char *to, size_t capacity, const char *format, const BrokenTime *time)
{
	size_t used = 0;
	for (; *format && used < capacity; ++format) {
		if (*format != '%') {
			to[used++] = *format;
			continue;
		}
		switch (*++format) {
		case 'Y': used = append_number(to, used, capacity, time->year + 1900, 4); break;
		case 'y': used = append_number(to, used, capacity, (time->year + 1900) % 100, 2); break;
		case 'm': used = append_number(to, used, capacity, time->mon + 1, 2); break;
		case 'd': used = append_number(to, used, capacity, time->mday, 2); break;
		case 'e': used = append_number(to, used, capacity, time->mday, time->mday < 10 ? 1 : 2); break;
		case 'H': used = append_number(to, used, capacity, time->hour, 2); break;
		case 'I': used = append_number(to, used, capacity, time->hour % 12 ? time->hour % 12 : 12, 2); break;
		case 'M': used = append_number(to, used, capacity, time->min, 2); break;
		case 'S': used = append_number(to, used, capacity, time->sec, 2); break;
		case 'j': used = append_number(to, used, capacity, time->yday + 1, 3); break;
		case 'p': used = append_text(to, used, capacity, time->hour < 12 ? "AM" : "PM", 2); break;
		case 'a': used = append_text(to, used, capacity, day_names[time->wday % 7], 3); break;
		case 'A': used = append_text(to, used, capacity, day_names[time->wday % 7], 32); break;
		case 'b': case 'h': used = append_text(to, used, capacity, month_names[time->mon % 12], 3); break;
		case 'B': used = append_text(to, used, capacity, month_names[time->mon % 12], 32); break;
		case 'Z': used = append_text(to, used, capacity, "UTC", 3); break;
		case 'z': used = append_text(to, used, capacity, "+0000", 5); break;
		case 'F': used = format_time(to + used, capacity - used, "%Y-%m-%d", time) + used; break;
		case 'T': used = format_time(to + used, capacity - used, "%H:%M:%S", time) + used; break;
		case 'D': used = format_time(to + used, capacity - used, "%m/%d/%y", time) + used; break;
		case 'c': used = format_time(to + used, capacity - used, "%a %b %e %H:%M:%S %Y", time) + used; break;
		case 'x': used = format_time(to + used, capacity - used, "%m/%d/%y", time) + used; break;
		case 'X': used = format_time(to + used, capacity - used, "%H:%M:%S", time) + used; break;
		case '%': to[used++] = '%'; break;
		default: to[used++] = '%'; if (used < capacity) to[used++] = *format; break;
		}
	}
	return used;
}

CRT size_t strftime(char *to, size_t capacity, const char *format, const void *time)
{
	size_t used;
	if (!capacity)
		return 0;
	used = format_time(to, capacity, format, time);
	if (used >= capacity) {
		to[0] = 0;
		return 0;
	}
	to[used] = 0;
	return used;
}

CRT size_t wcsftime(wchar_t *to, size_t capacity, const wchar_t *format, const void *time)
{
	char narrow_format[256], narrow[512];
	size_t length = wcsnlen(format, sizeof narrow_format - 1), used;
	for (size_t at = 0; at < length; ++at)
		narrow_format[at] = format[at] < 128 ? (char)format[at] : '?';
	narrow_format[length] = 0;
	used = strftime(narrow, sizeof narrow < capacity ? sizeof narrow : capacity, narrow_format, time);
	if (!used && narrow[0] == 0 && length)
		return 0;
	for (size_t at = 0; at <= used && at < capacity; ++at)
		to[at] = (unsigned char)narrow[at];
	return used;
}

/* ---- floating point ---- */
static double raven_exp(double x)
{
	static const double ln2 = 0.69314718055994530942;
	long long k;
	double r, term = 1, sum = 1, scale;
	union { double d; unsigned long long bits; } power;
	if (x > 709.0) return 1.0 / 0.0 * 1.0;
	if (x < -745.0) return 0.0;
	k = (long long)(x / ln2 + (x >= 0 ? 0.5 : -0.5));
	r = x - (double)k * ln2;
	for (int n = 1; n < 18; ++n) {
		term *= r / n;
		sum += term;
	}
	power.bits = (unsigned long long)(k + 1023) << 52;
	scale = power.d;
	return sum * scale;
}

static double raven_log(double x)
{
	static const double ln2 = 0.69314718055994530942;
	union { double d; unsigned long long bits; } value;
	long long exponent;
	double mantissa, f, f2, sum = 0, term;
	if (x <= 0)
		return x == 0 ? -1.0 / 0.0 : 0.0 / 0.0;
	value.d = x;
	exponent = (long long)((value.bits >> 52) & 0x7ff) - 1023;
	value.bits = (value.bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
	mantissa = value.d;
	if (mantissa > 1.4142135623730951) {
		mantissa *= 0.5;
		++exponent;
	}
	f = (mantissa - 1) / (mantissa + 1);
	f2 = f * f;
	term = f;
	for (int n = 1; n < 40; n += 2) {
		sum += term / n;
		term *= f2;
	}
	return 2 * sum + (double)exponent * ln2;
}

static double raven_cos(double x)
{
	static const double two_pi = 6.28318530717958647692;
	double r, term = 1, sum = 1;
	long long turns = (long long)(x / two_pi + (x >= 0 ? 0.5 : -0.5));
	r = x - (double)turns * two_pi;
	for (int n = 1; n < 14; ++n) {
		term *= -r * r / ((2 * n - 1) * (2 * n));
		sum += term;
	}
	return sum;
}

static double raven_sin(double x)
{
	static const double two_pi = 6.28318530717958647692;
	double r, term, sum;
	long long turns = (long long)(x / two_pi + (x >= 0 ? 0.5 : -0.5));
	r = x - (double)turns * two_pi;
	term = sum = r;
	for (int n = 1; n < 14; ++n) {
		term *= -r * r / ((2 * n) * (2 * n + 1));
		sum += term;
	}
	return sum;
}

CRT float cosf(float x) { return (float)raven_cos(x); }
CRT float sinf(float x) { return (float)raven_sin(x); }
CRT float fmaxf(float a, float b) { return a != a ? b : b != b ? a : a > b ? a : b; }
CRT float fminf(float a, float b) { return a != a ? b : b != b ? a : a < b ? a : b; }
CRT long lroundf(float x) { return (long)(x >= 0 ? (double)x + 0.5 : (double)x - 0.5); }

static double raven_pow(double base, double exponent)
{
	if (exponent == 0 || base == 1)
		return 1.0;
	if (base == 0)
		return exponent > 0 ? 0.0 : 1.0 / 0.0;
	/* A whole exponent multiplies by squaring, which keeps powers of small whole numbers exact. */
	if (exponent >= -1024 && exponent <= 1024 && (double)(int)exponent == exponent) {
		int count = (int)exponent;
		double result = 1.0, factor = base;
		for (unsigned left = (unsigned)(count < 0 ? -count : count); left; left >>= 1) {
			if (left & 1)
				result *= factor;
			factor *= factor;
		}
		return count < 0 ? 1.0 / result : result;
	}
	if (base < 0) {
		long long whole = (long long)exponent;
		if ((double)whole != exponent)
			return 0.0 / 0.0;
		double magnitude = raven_exp(exponent * raven_log(-base));
		return (whole & 1) ? -magnitude : magnitude;
	}
	return raven_exp(exponent * raven_log(base));
}

/* Values from 2^52 up, infinities and NaN have no fraction: they are their own floor and ceiling. */
CRT double floor(double x)
{
	if (!(x > -4503599627370496.0 && x < 4503599627370496.0))
		return x;
	double whole = (double)(long long)x;
	return whole > x ? whole - 1 : whole;
}

CRT double ceil(double x)
{
	if (!(x > -4503599627370496.0 && x < 4503599627370496.0))
		return x;
	double whole = (double)(long long)x;
	return whole < x ? whole + 1 : whole;
}

CRT double fabs(double x) { return x < 0 || (x == 0 && 1 / x < 0) ? -x : x; }

/* The remainder with the sign of x: multiples of y, doubled while they fit, come off exactly. */
CRT double fmod(double x, double y)
{
	double rest = fabs(x), step = fabs(y);
	if (y == 0 || x != x || y != y || rest == 1.0 / 0.0)
		return 0.0 / 0.0;
	while (rest >= step) {
		double part = step;
		while (part <= rest / 2)
			part *= 2;
		rest -= part;
	}
	return x < 0 ? -rest : rest;
}

/* The square root and the arc tangents come from the processor: SSE2 on 64 bits, the x87 unit for the rest. */
CRT double sqrt(double x)
{
	double root;
#ifdef __x86_64__
	__asm__("sqrtsd %1, %0" : "=x"(root) : "x"(x));
#else
	__asm__("fsqrt" : "=t"(root) : "0"(x));
#endif
	return root;
}

CRT double atan2(double y, double x)
{
	long double result;
	__asm__("fpatan" : "=t"(result) : "0"((long double)x), "u"((long double)y) : "st(1)");
	return (double)result;
}

CRT double atan(double x) { return atan2(x, 1.0); }

CRT double tan(double x)
{
	long double result, one;
	__asm__("fptan" : "=t"(one), "=u"(result) : "0"((long double)x));
	(void)one;
	return (double)result;
}

CRT double _hypot(double x, double y)
{
	x = fabs(x);
	y = fabs(y);
	double larger = x > y ? x : y, smaller = x > y ? y : x;
	if (larger == 0 || larger == 1.0 / 0.0)
		return larger;
	double ratio = smaller / larger;
	return larger * sqrt(1 + ratio * ratio);
}

/* The mantissa in [0.5, 1) and the power of two; and the fraction with the whole part apart, both with the sign of x. */
CRT double frexp(double x, int *exponent)
{
	union { double d; unsigned long long bits; } value = {x};
	int raw = (int)((value.bits >> 52) & 0x7ff);
	*exponent = 0;
	if (x == 0 || x != x || raw == 0x7ff)
		return x;
	if (!raw) {
		value.d = x * 18014398509481984.0; /* 2^54 */
		raw = (int)((value.bits >> 52) & 0x7ff);
		*exponent = -54;
	}
	*exponent += raw - 1022;
	value.bits = (value.bits & 0x800fffffffffffffULL) | 0x3fe0000000000000ULL;
	return value.d;
}

CRT double modf(double x, double *whole)
{
	if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) {
		*whole = x;
		return x != x ? x : (x < 0 ? -0.0 : 0.0);
	}
	*whole = (double)(long long)x;
	return x - *whole;
}

CRT float floorf(float x) { return (float)floor(x); }
CRT float ceilf(float x) { return (float)ceil(x); }

CRT double log(double x) { return raven_log(x); }
CRT double log10(double x) { return raven_log(x) / 2.30258509299404568402; }
CRT double exp(double x) { return raven_exp(x); }

/* e^x - 1 and log(1 + x) near zero, where the plain forms lose every digit: the rounding error of the inner step is divided back out
 * (Kahan's method), so the result keeps the precision of exp and log. */
CRT double expm1(double x)
{
	if (x != x || x == 0)
		return x;
	double u = raven_exp(x);
	if (u == 1)
		return x;
	if (u - 1 == -1)
		return -1;
	if (u == 1.0 / 0.0)
		return u;
	return (u - 1) * x / raven_log(u);
}

CRT double log1p(double x)
{
	if (x != x || x == 0)
		return x;
	double u = 1 + x;
	if (u == 1)
		return x;
	if (u == 0)
		return -1.0 / 0.0;
	if (u < 0)
		return (x - x) / (x - x);
	return raven_log(u) * x / (u - 1);
}

CRT float expm1f(float x) { return (float)expm1(x); }
CRT float log1pf(float x) { return (float)log1p(x); }
CRT long double expm1l(long double x) { return expm1((double)x); }
CRT long double log1pl(long double x) { return log1p((double)x); }

CRT float powf(float x, float y) { return (float)raven_pow(x, y); }
CRT double pow(double x, double y) { return raven_pow(x, y); }
CRT double cos(double x) { return raven_cos(x); }
CRT double sin(double x) { return raven_sin(x); }
CRT float sqrtf(float x) { return (float)sqrt(x); }

/* The inverse functions through atan2, which keeps both ends exact; outside [-1, 1] the result is not a number. */
CRT double asin(double x) { return x < -1 || x > 1 ? (x - x) / (x - x) : atan2(x, sqrt((1 - x) * (1 + x))); }
CRT double acos(double x) { return x < -1 || x > 1 ? (x - x) / (x - x) : atan2(sqrt((1 - x) * (1 + x)), x); }

/* The hyperbolic functions from the exponential; near zero, where the difference of exponentials loses its digits, from their series. */
CRT double sinh(double x)
{
	if (x > -0.5 && x < 0.5) {
		const double square = x * x;
		return x * (1 + square / 6 * (1 + square / 20 * (1 + square / 42 * (1 + square / 72))));
	}
	const double grown = raven_exp(x);
	return (grown - 1 / grown) / 2;
}

CRT double cosh(double x)
{
	const double grown = raven_exp(x);
	return (grown + 1 / grown) / 2;
}

CRT double tanh(double x)
{
	if (x > 20)
		return 1;
	if (x < -20)
		return -1;
	if (x > -0.5 && x < 0.5)
		return sinh(x) / cosh(x);
	const double grown = raven_exp(2 * x);
	return (grown - 1) / (grown + 1);
}

CRT float asinf(float x) { return (float)asin(x); }
CRT float acosf(float x) { return (float)acos(x); }
CRT float atanf(float x) { return (float)atan(x); }
CRT float atan2f(float y, float x) { return (float)atan2(y, x); }
CRT float tanf(float x) { return (float)tan(x); }
CRT float fmodf(float x, float y) { return (float)fmod(x, y); }

CRT float modff(float x, float *whole)
{
	double part;
	const double fraction = modf(x, &part);
	*whole = (float)part;
	return (float)fraction;
}

#ifdef __i386__
/* The x87 forms the 32-bit compiler calls: the arguments on the floating point stack, the first one deeper, and the result left there. */
#define ONE_ARGUMENT(name) \
	".globl __CI" #name "\n__CI" #name ":\n	subl $8, %esp\n	fstpl (%esp)\n	call _" #name "\n	addl $8, %esp\n	ret\n"
#define TWO_ARGUMENTS(name) \
	".globl __CI" #name "\n__CI" #name ":\n	subl $16, %esp\n	fstpl 8(%esp)\n	fstpl (%esp)\n	call _" #name "\n	addl $16, %esp\n	ret\n"
__asm__(
	".section .drectve\n"
	".ascii \" -export:_ftol -export:_CIsqrt -export:_CIsin -export:_CIcos -export:_CItan -export:_CIatan -export:_CIexp -export:_CIlog -export:_CIlog10\"\n"
	".ascii \" -export:_CIasin -export:_CIacos -export:_CIsinh -export:_CIcosh -export:_CItanh -export:_CIpow -export:_CIatan2 -export:_CIfmod\"\n"
	".text\n"
	ONE_ARGUMENT(sqrt) ONE_ARGUMENT(sin) ONE_ARGUMENT(cos) ONE_ARGUMENT(tan) ONE_ARGUMENT(atan) ONE_ARGUMENT(exp) ONE_ARGUMENT(log)
	ONE_ARGUMENT(log10) ONE_ARGUMENT(asin) ONE_ARGUMENT(acos) ONE_ARGUMENT(sinh) ONE_ARGUMENT(cosh) ONE_ARGUMENT(tanh) TWO_ARGUMENTS(pow)
	TWO_ARGUMENTS(atan2) TWO_ARGUMENTS(fmod)
	/* _ftol: the value on the floating point stack, truncated towards zero, in edx:eax. */
	".globl __ftol\n__ftol:\n	subl $12, %esp\n	fnstcw 8(%esp)\n	movw 8(%esp), %ax\n	orw $0x0c00, %ax\n	movw %ax, 10(%esp)\n"
	"	fldcw 10(%esp)\n	fistpll (%esp)\n	fldcw 8(%esp)\n	movl (%esp), %eax\n	movl 4(%esp), %edx\n	addl $12, %esp\n	ret\n");
#undef ONE_ARGUMENT
#undef TWO_ARGUMENTS
#endif

/* ---- non-local jumps and threads ---- */
#ifdef __x86_64__
__asm__(
	".section .drectve\n"
	".ascii \" -export:__intrinsic_setjmp -export:longjmp\"\n"
	".text\n"
	".globl __intrinsic_setjmp\n"
	"__intrinsic_setjmp:\n"
	"	movq %rcx, %rax\n"
	"	movq %rdx, 0(%rcx)\n"      /* frame */
	"	movq %rbx, 8(%rcx)\n"
	"	leaq 8(%rsp), %rdx\n"
	"	movq %rdx, 16(%rcx)\n"     /* stack pointer after the return */
	"	movq %rbp, 24(%rcx)\n"
	"	movq %rsi, 32(%rcx)\n"
	"	movq %rdi, 40(%rcx)\n"
	"	movq %r12, 48(%rcx)\n"
	"	movq %r13, 56(%rcx)\n"
	"	movq %r14, 64(%rcx)\n"
	"	movq %r15, 72(%rcx)\n"
	"	movq (%rsp), %rdx\n"
	"	movq %rdx, 80(%rcx)\n"     /* return address */
	"	stmxcsr 88(%rcx)\n"
	"	fnstcw 92(%rcx)\n"
	"	movdqa %xmm6, 96(%rcx)\n"
	"	movdqa %xmm7, 112(%rcx)\n"
	"	movdqa %xmm8, 128(%rcx)\n"
	"	movdqa %xmm9, 144(%rcx)\n"
	"	movdqa %xmm10, 160(%rcx)\n"
	"	movdqa %xmm11, 176(%rcx)\n"
	"	movdqa %xmm12, 192(%rcx)\n"
	"	movdqa %xmm13, 208(%rcx)\n"
	"	movdqa %xmm14, 224(%rcx)\n"
	"	movdqa %xmm15, 240(%rcx)\n"
	"	xorl %eax, %eax\n"
	"	ret\n"
	".globl longjmp\n"
	"longjmp:\n"
	"	movl %edx, %eax\n"
	"	testl %eax, %eax\n"
	"	jnz 1f\n"
	"	movl $1, %eax\n"
	"1:	movq 8(%rcx), %rbx\n"
	"	movq 24(%rcx), %rbp\n"
	"	movq 32(%rcx), %rsi\n"
	"	movq 40(%rcx), %rdi\n"
	"	movq 48(%rcx), %r12\n"
	"	movq 56(%rcx), %r13\n"
	"	movq 64(%rcx), %r14\n"
	"	movq 72(%rcx), %r15\n"
	"	ldmxcsr 88(%rcx)\n"
	"	fldcw 92(%rcx)\n"
	"	movdqa 96(%rcx), %xmm6\n"
	"	movdqa 112(%rcx), %xmm7\n"
	"	movdqa 128(%rcx), %xmm8\n"
	"	movdqa 144(%rcx), %xmm9\n"
	"	movdqa 160(%rcx), %xmm10\n"
	"	movdqa 176(%rcx), %xmm11\n"
	"	movdqa 192(%rcx), %xmm12\n"
	"	movdqa 208(%rcx), %xmm13\n"
	"	movdqa 224(%rcx), %xmm14\n"
	"	movdqa 240(%rcx), %xmm15\n"
	"	movq 16(%rcx), %rsp\n"
	"	jmp *80(%rcx)\n"
);
#else
/* The 32-bit buffer keeps the callee-saved registers, the stack pointer after the return and the return address. */
__asm__(
	".section .drectve\n"
	".ascii \" -export:_setjmp3 -export:_setjmp=_setjmp3 -export:longjmp\"\n"
	".text\n"
	".globl __setjmp3\n"
	"__setjmp3:\n"
	"	movl 4(%esp), %edx\n"
	"	movl %ebp, 0(%edx)\n"
	"	movl %ebx, 4(%edx)\n"
	"	movl %edi, 8(%edx)\n"
	"	movl %esi, 12(%edx)\n"
	"	leal 4(%esp), %eax\n"
	"	movl %eax, 16(%edx)\n"
	"	movl (%esp), %eax\n"
	"	movl %eax, 20(%edx)\n"
	"	xorl %eax, %eax\n"
	"	ret\n"
	".globl _longjmp\n"
	"_longjmp:\n"
	"	movl 4(%esp), %edx\n"
	"	movl 8(%esp), %eax\n"
	"	testl %eax, %eax\n"
	"	jnz 1f\n"
	"	movl $1, %eax\n"
	"1:	movl 0(%edx), %ebp\n"
	"	movl 4(%edx), %ebx\n"
	"	movl 8(%edx), %edi\n"
	"	movl 12(%edx), %esi\n"
	"	movl 16(%edx), %esp\n"
	"	jmp *20(%edx)\n"
);
#endif

typedef struct {
	unsigned (__stdcall *function)(void *);
	void *argument;
} ThreadStart;

static DWORD WINAPI thread_trampoline(void *parameter)
{
	ThreadStart start = *(ThreadStart *)parameter;
	HeapFree(GetProcessHeap(), 0, parameter);
	return start.function(start.argument);
}

CRT uintptr_t _beginthreadex(void *security, unsigned stack, unsigned (__stdcall *function)(void *), void *argument, unsigned flags, unsigned *identifier)
{
	ThreadStart *start = HeapAlloc(GetProcessHeap(), 0, sizeof *start);
	DWORD id = 0;
	HANDLE thread;
	if (!start)
		return 0;
	start->function = function;
	start->argument = argument;
	thread = CreateThread(security, stack, thread_trampoline, start, flags, &id);
	if (!thread) {
		HeapFree(GetProcessHeap(), 0, start);
		return 0;
	}
	if (identifier)
		*identifier = id;
	return (uintptr_t)thread;
}

CRT void _endthreadex(unsigned code) { ExitThread(code); }
