#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The C runtime entry points the EA app's launchers and services import from ucrtbase.dll beyond the rest, for 64-bit and 32-bit callers:
   _ecvt, the time zone's daylight bias, _cputs, _heapchk, the long rotations, _stat32i64, freopen_s, _except1 for a masked exception,
   _spawnlp finding a program through PATH, and the _o_ names newer runtime libraries import for functions the runtime already has. Failures
   included. The exit code names the first check that failed; 100 and up names an export ucrtbase.dll does not have. */

static const char *const names[] = {
	"_ecvt", "__dstbias", "_cputs", "_heapchk", "_lrotl", "_lrotr", "_stat32i64", "freopen_s", "_except1", "_spawnlp", "_spawnvp",
	"_wspawnvp", "_o___stdio_common_vswprintf", "_o__aligned_free", "_o__aligned_malloc", "_o__dclass", "_o__free_base", "_o__fullpath",
	"_o__ldclass", "_o__malloc_base", "_o_ceilf", "_o_memcpy_s", "_o_rand", "_o_srand",
};

static HMODULE crt;
static FARPROC find(const char *name) { return GetProcAddress(crt, name); }
static int *error(void) { return ((int *(__cdecl *)(void))find("_errno"))(); }

static int same_text(const char *left, const char *right)
{
	while (*left && *left == *right)
		++left, ++right;
	return *left == *right;
}

/* msvcrt's struct _stat32i64. */
struct stat32i64 {
	unsigned st_dev;
	unsigned short st_ino, st_mode;
	short st_nlink, st_uid, st_gid;
	unsigned st_rdev;
	long long st_size;
	long st_atime, st_mtime, st_ctime;
};

static int numbers(void)
{
	char *(__cdecl *ecvt)(double, int, int *, int *) = (void *)find("_ecvt");
	int point = 0, sign = 0;
	const char *digits = ecvt(-1234.5678, 6, &point, &sign);
	if (!digits || !same_text(digits, "123457") || point != 4 || sign != 1)
		return 10;
	digits = ecvt(0.000125, 3, &point, &sign);
	if (!digits || !same_text(digits, "125") || point != -3 || sign != 0)
		return 11;
	unsigned long (__cdecl *rotate_left)(unsigned long, int) = (void *)find("_lrotl");
	unsigned long (__cdecl *rotate_right)(unsigned long, int) = (void *)find("_lrotr");
	if (rotate_left(0x80000001UL, 1) != 0x3UL || rotate_right(0x3UL, 1) != 0x80000001UL || rotate_left(0x12345678UL, 32) != 0x12345678UL)
		return 12;
	/* A masked overflow sets ERANGE and gives back the result; the masks are set in each width's own control word. */
#ifdef _WIN64
	const DWORD masked = 0x1f80;
#else
	const DWORD masked = 0x027f;
#endif
	double (__cdecl *except1)(DWORD, int, double, double, DWORD, void *) = (void *)find("_except1");
	*error() = 0;
	if (except1(1, 0, 1e308, 2.5, masked, NULL) != 2.5 || *error() != 34)
		return 13;
	void (__cdecl *seed)(unsigned) = (void *)find("_o_srand");
	int (__cdecl *random)(void) = (void *)find("_o_rand");
	seed(7);
	const int first = random();
	seed(7);
	if (random() != first)
		return 14;
	float (__cdecl *ceiling)(float) = (void *)find("_o_ceilf");
	if (ceiling(1.25f) != 2.0f)
		return 15;
	return 0;
}

static int memory(void)
{
	int (__cdecl *check)(void) = (void *)find("_heapchk");
	if (check() != -2)
		return 20;
	void *(__cdecl *allocate)(size_t, size_t) = (void *)find("_o__aligned_malloc");
	void (__cdecl *release)(void *) = (void *)find("_o__aligned_free");
	void *block = allocate(100, 64);
	if (!block || ((ULONG_PTR)block & 63))
		return 21;
	release(block);
	void *(__cdecl *base)(size_t) = (void *)find("_o__malloc_base");
	void (__cdecl *free_base)(void *) = (void *)find("_o__free_base");
	int (__cdecl *copy)(void *, size_t, const void *, size_t) = (void *)find("_o_memcpy_s");
	char *buffer = base(8);
	if (!buffer || copy(buffer, 8, "launch", 7) || !same_text(buffer, "launch") || copy(buffer, 4, "launch", 7) != 34)
		return 22;
	free_base(buffer);
	return 0;
}

static int files(void)
{
	char directory[MAX_PATH], path[MAX_PATH];
	if (!GetTempPathA(MAX_PATH, directory))
		return 30;
	lstrcpyA(path, directory);
	lstrcatA(path, "ucrt-launcher.txt");
	HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written = 0;
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, "twelve bytes", 12, &written, NULL))
		return 31;
	CloseHandle(file);
	int (__cdecl *stat32i64)(const char *, struct stat32i64 *) = (void *)find("_stat32i64");
	struct stat32i64 seen = {0};
	if (stat32i64(path, &seen) || seen.st_size != 12 || !(seen.st_mode & 0x8000) || seen.st_mtime <= 0)
		return 32;
	char missing[MAX_PATH];
	lstrcpyA(missing, directory);
	lstrcatA(missing, "ucrt-launcher-missing.txt");
	if (stat32i64(missing, &seen) != -1 || *error() != 2)
		return 33;
	/* freopen_s puts the file behind a stream it reopens, and the standard stream keeps it when the program asks for it again. */
	void *(__cdecl *stream)(unsigned) = (void *)find("__acrt_iob_func");
	int (__cdecl *reopen)(void **, const char *, const char *, void *) = (void *)find("freopen_s");
	int (__cdecl *get)(void *) = (void *)find("fgetc");
	int (__cdecl *close)(void *) = (void *)find("fclose");
	void *opened = NULL;
	if (reopen(&opened, path, "r", stream(0)) || opened != stream(0) || get(stream(0)) != 't')
		return 34;
	close(opened);
	if (reopen(NULL, path, "r", stream(0)) != 22)
		return 35;
	char *(__cdecl *full)(char *, const char *, size_t) = (void *)find("_o__fullpath");
	char whole[MAX_PATH];
	if (!full(whole, path, MAX_PATH) || !same_text(whole, path))
		return 36;
	DeleteFileA(path);
	return 0;
}

/* _spawnlp finds the command interpreter by its bare name through PATH and gives its exit code. */
static int programs(void)
{
	intptr_t (__cdecl *spawn)(int, const char *, const char *, ...) = (void *)find("_spawnlp");
	char interpreter[MAX_PATH], folder[MAX_PATH], path[4096];
	if (!GetEnvironmentVariableA("ComSpec", interpreter, MAX_PATH))
		return 40;
	lstrcpyA(folder, interpreter);
	for (int at = lstrlenA(folder); at > 0; --at)
		if (folder[at - 1] == '\\') {
			folder[at - 1] = 0;
			break;
		}
	lstrcpyA(path, folder);
	lstrcatA(path, ";");
	const int length = lstrlenA(path);
	GetEnvironmentVariableA("PATH", path + length, sizeof path - (DWORD)length);
	SetEnvironmentVariableA("PATH", path);
	if (spawn(0, "cmd", "cmd", "/c", "exit 6", (const char *)NULL) != 6)
		return 41;
	if (spawn(0, "no-such-program-anywhere", "no-such-program-anywhere", (const char *)NULL) != -1 || *error() != 2)
		return 42;
	return 0;
}

/* The daylight bias is in seconds, after _tzset; _cputs answers 0 with a console and -1 without. */
static int console_and_zone(void)
{
	void (__cdecl *zone)(void) = (void *)find("_tzset");
	long *(__cdecl *bias)(void) = (void *)find("__dstbias");
	TIME_ZONE_INFORMATION information;
	GetTimeZoneInformation(&information);
	zone();
	if (!bias() || *bias() != information.DaylightBias * 60)
		return 50;
	int (__cdecl *put)(const char *) = (void *)find("_cputs");
	HANDLE console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	DWORD mode;
	const int present = console != INVALID_HANDLE_VALUE && GetConsoleMode(console, &mode);
	if (console != INVALID_HANDLE_VALUE)
		CloseHandle(console);
	if (put(".\n") != (present ? 0 : -1))
		return 51;
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
	int result = numbers();
	if (!result)
		result = memory();
	if (!result)
		result = files();
	if (!result)
		result = programs();
	if (!result)
		result = console_and_zone();
	return result;
}
