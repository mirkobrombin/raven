#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>

/* The msvcrt.dll entry points libraries built by MinGW-w64 import, as VLC's plugins do: the secure scans, fscanf, _vsnprintf_s, file
   searches with 64-bit sizes and a pipe to a command. Each is resolved by name, and answers as msvcrt does. The exit code names the first
   check that failed; 100 and up names an export msvcrt.dll does not have. */

static const char *const names[] = {
	"sscanf_s", "swscanf_s", "fscanf", "_vsnprintf_s", "_findfirst64", "_findnext64", "_popen", "_pclose",
};

typedef struct { unsigned attributes; long long created, accessed, written; long long size; char name[260]; } found_file;

static HMODULE crt;
static FARPROC find(const char *name) { return GetProcAddress(crt, name); }
static int same(const char *a, const char *b) { return lstrcmpA(a, b) == 0; }

static int format(char *buffer, size_t size, size_t count, const char *text, ...)
{
	int (__cdecl *print)(char *, size_t, size_t, const char *, va_list) = (void *)find("_vsnprintf_s");
	va_list arguments;
	va_start(arguments, text);
	const int result = print(buffer, size, count, text, arguments);
	va_end(arguments);
	return result;
}

static int scans(void)
{
	int (__cdecl *scan)(const char *, const char *, ...) = (void *)find("sscanf_s");
	int (__cdecl *scan_wide)(const wchar_t *, const wchar_t *, ...) = (void *)find("swscanf_s");
	int number = 0;
	char word[8] = {0};
	if (scan("12 ab", "%d %s", &number, word, (unsigned)sizeof word) != 2 || number != 12 || !same(word, "ab"))
		return 1;
	/* A string longer than the size given is not stored. */
	if (scan("toolong", "%s", word, 4u) == 1)
		return 2;
	wchar_t wide[8] = {0};
	if (scan_wide(L"34 cd", L"%d %s", &number, wide, (unsigned)(sizeof wide / sizeof *wide)) != 2 || number != 34 || lstrcmpW(wide, L"cd"))
		return 3;
	return 0;
}

static int formats(void)
{
	char buffer[8];
	if (format(buffer, sizeof buffer, (size_t)-1, "%s", "abc") != 3 || !same(buffer, "abc"))
		return 10;
	/* With _TRUNCATE the text is cut to the buffer and the answer is -1. */
	if (format(buffer, 4, (size_t)-1, "%s", "abcdef") != -1 || !same(buffer, "abc"))
		return 11;
	/* A count shorter than the buffer cuts the text there. */
	if (format(buffer, sizeof buffer, 2, "%d", 12345) != -1 || !same(buffer, "12"))
		return 12;
	return 0;
}

static int files(void)
{
	void *(__cdecl *open)(const char *, const char *) = (void *)find("fopen");
	int (__cdecl *close)(void *) = (void *)find("fclose");
	int (__cdecl *put)(const char *, void *) = (void *)find("fputs");
	int (__cdecl *scan)(void *, const char *, ...) = (void *)find("fscanf");
	intptr_t (__cdecl *find_first)(const char *, found_file *) = (void *)find("_findfirst64");
	int (__cdecl *find_next)(intptr_t, found_file *) = (void *)find("_findnext64");
	int (__cdecl *find_close)(intptr_t) = (void *)find("_findclose");
	char directory[MAX_PATH], path[MAX_PATH], pattern[MAX_PATH];
	if (!open || !close || !put || !find_close || !GetTempPathA(MAX_PATH, directory))
		return 20;
	lstrcpyA(path, directory);
	lstrcatA(path, "msvcrt-mingw.txt");
	void *stream = open(path, "wb");
	if (!stream || put("7 seven\n", stream) < 0)
		return 21;
	close(stream);
	stream = open(path, "r");
	int number = 0;
	char word[16] = {0};
	if (!stream || scan(stream, "%d %15s", &number, word) != 2 || number != 7 || !same(word, "seven"))
		return 22;
	close(stream);
	found_file found;
	lstrcpyA(pattern, directory);
	lstrcatA(pattern, "msvcrt-mingw.*");
	const intptr_t search = find_first(pattern, &found);
	if (search == -1 || found.size != 8 || !same(found.name, "msvcrt-mingw.txt") || (found.attributes & FILE_ATTRIBUTE_DIRECTORY))
		return 23;
	if (find_next(search, &found) != -1)
		return 24;
	find_close(search);
	DeleteFileA(path);
	return 0;
}

static int pipes(void)
{
	void *(__cdecl *open)(const char *, const char *) = (void *)find("_popen");
	int (__cdecl *close)(void *) = (void *)find("_pclose");
	char *(__cdecl *line)(char *, int, void *) = (void *)find("fgets");
	char text[32] = {0};
	void *stream = open("echo piped", "r");
	if (!stream)
		return 30;
	if (!line(text, sizeof text, stream) || text[0] != 'p' || text[1] != 'i' || text[4] != 'd')
		return 31;
	if (close(stream) != 0)
		return 32;
	/* A stream the pipe table does not hold is refused. */
	if (close((void *)text) != -1)
		return 33;
	return 0;
}

int raven_entry(void)
{
	crt = LoadLibraryA("msvcrt.dll");
	if (!crt)
		return 1;
	for (size_t at = 0; at < sizeof names / sizeof names[0]; ++at)
		if (!find(names[at]))
			return 100 + (int)at;
	int result = scans();
	if (!result)
		result = formats();
	if (!result)
		result = files();
	if (!result)
		result = pipes();
	return result;
}
