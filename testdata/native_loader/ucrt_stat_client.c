#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

/* ucrtbase.dll's _wstat32i64 and _stat32i64 (Geometry Dash 2.2081 imports the first), for 64-bit and 32-bit callers: struct _stat32i64 has
   the 64-bit size with 32-bit times, 48 bytes on both, and nothing past it is written. A file, a file past 4 GiB, a folder, and a missing
   file (-1 with ENOENT). The exit code names the first check that failed; 100 and up names an export ucrtbase.dll does not have. */

typedef struct {
	unsigned st_dev;
	unsigned short st_ino, st_mode;
	short st_nlink, st_uid, st_gid;
	unsigned st_rdev;
	long long st_size;
	long st_atime, st_mtime, st_ctime;
} stat32i64;

typedef struct {
	unsigned st_dev;
	unsigned short st_ino, st_mode;
	short st_nlink, st_uid, st_gid;
	unsigned st_rdev;
	long long st_size, st_atime, st_mtime, st_ctime;
} stat64;

static const char *const names[] = { "_wstat32i64", "_stat32i64", "_wstat64", "_errno" };

static HMODULE crt;
static FARPROC find(const char *name) { return GetProcAddress(crt, name); }
static int *error(void) { return ((int *(__cdecl *)(void))find("_errno"))(); }

typedef union {
	stat32i64 info;
	unsigned char bytes[64];
} guarded;

static void fill(guarded *buffer)
{
	for (size_t at = 0; at < sizeof buffer->bytes; ++at)
		buffer->bytes[at] = 0xcc;
}

static int untouched_after(const guarded *buffer, size_t size)
{
	for (size_t at = size; at < sizeof buffer->bytes; ++at)
		if (buffer->bytes[at] != 0xcc)
			return 0;
	return 1;
}

static int write_file(const wchar_t *path, const char *text, LONGLONG size)
{
	DWORD written = 0;
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	BOOL done = WriteFile(file, text, (DWORD)lstrlenA(text), &written, NULL);
	if (done && size) {
		LARGE_INTEGER end = { .QuadPart = size };
		done = SetFilePointerEx(file, end, NULL, FILE_BEGIN) && SetEndOfFile(file);
	}
	CloseHandle(file);
	return done;
}

int raven_entry(void)
{
	crt = LoadLibraryA("ucrtbase.dll");
	if (!crt)
		return 1;
	for (size_t at = 0; at < sizeof names / sizeof names[0]; ++at)
		if (!find(names[at]))
			return 100 + (int)at;
	int (__cdecl *stat_wide)(const wchar_t *, stat32i64 *) = (void *)find("_wstat32i64");
	int (__cdecl *stat_narrow)(const char *, stat32i64 *) = (void *)find("_stat32i64");
	int (__cdecl *stat_wide64)(const wchar_t *, stat64 *) = (void *)find("_wstat64");
	if (sizeof(stat32i64) != 48)
		return 2;

	wchar_t directory[MAX_PATH], path[MAX_PATH];
	char narrow[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, directory))
		return 3;
	lstrcpyW(path, directory);
	lstrcatW(path, L"ucrt-stat.txt");
	if (!write_file(path, "raven", 0) || !WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, MAX_PATH, NULL, NULL))
		return 4;

	/* A file: its size, a regular file's mode, and the times _wstat64 gives, in 32 bits. */
	guarded buffer;
	stat64 full;
	fill(&buffer);
	if (stat_wide(path, &buffer.info) != 0 || stat_wide64(path, &full) != 0)
		return 10;
	if (buffer.info.st_size != 5 || (buffer.info.st_mode & 0xf000) != 0x8000 || buffer.info.st_nlink != 1)
		return 11;
	if (!full.st_mtime || buffer.info.st_mtime != (long)full.st_mtime || buffer.info.st_atime != (long)full.st_atime ||
		buffer.info.st_ctime != (long)full.st_ctime || buffer.info.st_dev != full.st_dev)
		return 12;
	if (!untouched_after(&buffer, sizeof(stat32i64)))
		return 13;
	fill(&buffer);
	if (stat_narrow(narrow, &buffer.info) != 0 || buffer.info.st_size != 5 || (buffer.info.st_mode & 0xf000) != 0x8000 ||
		buffer.info.st_mtime != (long)full.st_mtime || !untouched_after(&buffer, sizeof(stat32i64)))
		return 14;

	/* Past 4 GiB the size keeps its high half. */
	if (!write_file(path, "raven", 0x100000005LL))
		return 20;
	fill(&buffer);
	if (stat_wide(path, &buffer.info) != 0 || buffer.info.st_size != 0x100000005LL)
		return 21;
	fill(&buffer);
	if (stat_narrow(narrow, &buffer.info) != 0 || buffer.info.st_size != 0x100000005LL)
		return 22;
	DeleteFileW(path);

	/* A folder has the directory mode. */
	fill(&buffer);
	if (stat_wide(directory, &buffer.info) != 0 || (buffer.info.st_mode & 0xf000) != 0x4000)
		return 30;

	/* A missing file: -1 with ENOENT, by both names. */
	*error() = 0;
	if (stat_wide(path, &buffer.info) != -1 || *error() != 2)
		return 40;
	*error() = 0;
	if (stat_narrow(narrow, &buffer.info) != -1 || *error() != 2)
		return 41;
	return 0;
}
