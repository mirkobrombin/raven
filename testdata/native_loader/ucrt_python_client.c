#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

/* The C runtime entry points Python 3.14's python314.dll imports from ucrtbase.dll beyond the rest (Autodesk's Fusion Client Downloader
   runs it), for 64-bit and 32-bit callers: system and _wsystem run a command through the command interpreter and give its exit code; the
   wide spawn functions start a program, wait for it or hand its handle to _cwait, and run a batch file through the interpreter; _wexecv
   starts a program in place of the calling one; and the console key functions read standard input, keep one key aside and write to the
   console. Failures included. The exit code names the first check that failed; 100 and up names an export ucrtbase.dll does not have.
   Run with the argument "exec", the program calls _wexecv itself, for the parent to see the program it started. */

static const char *const names[] = {
	"system", "_wsystem", "_wspawnv", "_wspawnve", "_wexecv", "_wexecve", "_cwait", "_getch", "_getche", "_getwch", "_getwche", "_putch",
	"_putwch", "_ungetch", "_ungetwch",
};

static HMODULE crt;
static FARPROC find(const char *name) { return GetProcAddress(crt, name); }
static int *error(void) { return ((int *(__cdecl *)(void))find("_errno"))(); }

static wchar_t directory[MAX_PATH], interpreter[MAX_PATH];

static void file_path(wchar_t *path, const wchar_t *name)
{
	lstrcpyW(path, directory);
	lstrcatW(path, name);
}

static int file_starts(const wchar_t *path, const char *text)
{
	char read_text[16] = {0};
	DWORD read = 0;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	ReadFile(file, read_text, sizeof read_text - 1, &read, NULL);
	CloseHandle(file);
	for (int at = 0; text[at]; ++at)
		if (read_text[at] != text[at])
			return 0;
	return 1;
}

static int write_file(const wchar_t *path, const char *text)
{
	DWORD written = 0;
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	const BOOL done = WriteFile(file, text, (DWORD)lstrlenA(text), &written, NULL);
	CloseHandle(file);
	return done;
}

static int commands(void)
{
	int (__cdecl *run_wide)(const wchar_t *) = (void *)find("_wsystem");
	int (__cdecl *run)(const char *) = (void *)find("system");
	int (__cdecl *set_variable)(const wchar_t *, const wchar_t *) = (void *)find("_wputenv_s");
	if (!set_variable)
		return 10;
	if (!run_wide(NULL) || !run(NULL))
		return 11;
	if (run_wide(L"exit 7") != 7 || run("exit 3") != 3 || run_wide(L"exit 0") != 0)
		return 12;
	/* The command writes a file whose name has letters outside the ANSI code page. */
	wchar_t path[MAX_PATH], command[3 * MAX_PATH];
	file_path(path, L"ucrt-python-\x0444\x0430\x0439\x043b.txt");
	DeleteFileW(path);
	lstrcpyW(command, L"echo wide> \"");
	lstrcatW(command, path);
	lstrcatW(command, L"\"");
	if (run_wide(command) != 0 || !file_starts(path, "wide"))
		return 13;
	DeleteFileW(path);
	/* No interpreter where ComSpec points: a null command answers 0, any other -1, both with ENOENT. */
	file_path(path, L"ucrt-python-missing\\cmd.exe");
	if (set_variable(L"ComSpec", path))
		return 14;
	*error() = 0;
	if (run_wide(NULL) != 0 || *error() != 2)
		return 15;
	*error() = 0;
	if (run_wide(L"exit 7") != -1 || *error() != 2)
		return 16;
	*error() = 0;
	if (run("exit 3") != -1 || *error() != 2)
		return 17;
	if (set_variable(L"ComSpec", interpreter) || run_wide(L"exit 5") != 5)
		return 18;
	return 0;
}

static int programs(void)
{
	intptr_t (__cdecl *spawn)(int, const wchar_t *, const wchar_t *const *) = (void *)find("_wspawnv");
	intptr_t (__cdecl *spawn_environment)(int, const wchar_t *, const wchar_t *const *, const wchar_t *const *) = (void *)find("_wspawnve");
	intptr_t (__cdecl *wait)(int *, intptr_t, int) = (void *)find("_cwait");
	const wchar_t *const exit_nine[] = {L"cmd", L"/c", L"exit", L"9", NULL};
	if (spawn(0, interpreter, exit_nine) != 9)
		return 20;
	/* A name without an extension finds the program with .exe. */
	wchar_t name[MAX_PATH];
	lstrcpyW(name, interpreter);
	name[lstrlenW(name) - 4] = 0;
	const wchar_t *const exit_four[] = {L"cmd", L"/c", L"exit", L"4", NULL};
	if (spawn(0, name, exit_four) != 4)
		return 21;
	/* Started without waiting, its handle waited on by _cwait, which closes it. */
	const intptr_t process = spawn(1, interpreter, exit_four);
	int status = -1;
	if (process == -1 || process == 0 || wait(&status, process, 0) != process || status != 4)
		return 22;
	*error() = 0;
	if (wait(&status, 0, 0) != -1 || *error() != 10)
		return 23;
	/* The environment given is the program's own. */
	const wchar_t *const check[] = {L"cmd", L"/c", L"if", L"\"%RAVEN_SPAWN%\"==\"yes\"", L"exit", L"6", NULL};
	const wchar_t *const environment[] = {L"RAVEN_SPAWN=yes", NULL};
	if (spawn_environment(0, interpreter, check, environment) != 6 || spawn(0, interpreter, check) == 6)
		return 24;
	/* A batch file found by its name runs through the interpreter, with the arguments after the first. */
	wchar_t batch[MAX_PATH], batch_name[MAX_PATH];
	file_path(batch, L"ucrt-python-batch.bat");
	file_path(batch_name, L"ucrt-python-batch");
	if (!write_file(batch, "@exit %1\r\n"))
		return 25;
	const wchar_t *const batch_arguments[] = {L"ucrt-python-batch", L"8", NULL};
	const intptr_t batch_result = spawn(0, batch_name, batch_arguments);
	DeleteFileW(batch);
	if (batch_result != 8)
		return 26;
	/* Errors: a mode that is none, an empty first argument, a program that is not there. */
	const wchar_t *const empty[] = {L"", NULL};
	*error() = 0;
	if (spawn(7, interpreter, exit_nine) != -1 || *error() != 22)
		return 27;
	*error() = 0;
	if (spawn(0, interpreter, empty) != -1 || *error() != 22)
		return 28;
	file_path(name, L"ucrt-python-missing");
	*error() = 0;
	if (spawn(0, name, exit_nine) != -1 || *error() != 2)
		return 29;
	return 0;
}

/* The program again with the argument "exec" calls _wexecv for a command that writes a file, and ends with 0 once it has started. */
static int replaced(void)
{
	intptr_t (__cdecl *spawn)(int, const wchar_t *, const wchar_t *const *) = (void *)find("_wspawnv");
	wchar_t self[MAX_PATH], path[MAX_PATH];
	if (!GetModuleFileNameW(NULL, self, MAX_PATH))
		return 30;
	file_path(path, L"ucrt-python-exec.txt");
	DeleteFileW(path);
	const wchar_t *const arguments[] = {L"ucrt-python", L"exec", NULL};
	if (spawn(0, self, arguments) != 0)
		return 31;
	for (int tries = 0; tries < 100 && !file_starts(path, "exec"); ++tries)
		Sleep(100);
	const int found = file_starts(path, "exec");
	DeleteFileW(path);
	return found ? 0 : 32;
}

static int execute(void)
{
	intptr_t (__cdecl *exec)(const wchar_t *, const wchar_t *const *) = (void *)find("_wexecv");
	wchar_t path[MAX_PATH], command[2 * MAX_PATH];
	file_path(path, L"ucrt-python-exec.txt");
	lstrcpyW(command, L"echo exec> \"");
	lstrcatW(command, path);
	lstrcatW(command, L"\"");
	const wchar_t *const arguments[] = {L"cmd", L"/c", command, NULL};
	exec(interpreter, arguments);
	return 33;
}

static int keys(void)
{
	int (__cdecl *get)(void) = (void *)find("_getch");
	wint_t (__cdecl *get_wide)(void) = (void *)find("_getwch");
	int (__cdecl *unget)(int) = (void *)find("_ungetch");
	wint_t (__cdecl *unget_wide)(wint_t) = (void *)find("_ungetwch");
	/* One key is kept aside, and only one; WEOF is never kept. */
	if (unget_wide(L'x') != L'x' || unget_wide(L'y') != 0xffff || unget('a') != -1 || get_wide() != L'x')
		return 40;
	if (unget_wide(0xffff) != 0xffff || unget(-1) != -1 || unget('a') != 'a' || get() != 'a')
		return 41;
	/* Standard input that is not a console gives its characters. */
	wchar_t path[MAX_PATH];
	file_path(path, L"ucrt-python-keys.txt");
	if (!write_file(path, "qr"))
		return 42;
	HANDLE input = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL), standard = GetStdHandle(STD_INPUT_HANDLE);
	if (input == INVALID_HANDLE_VALUE)
		return 43;
	SetStdHandle(STD_INPUT_HANDLE, input);
	const int first = get(), second = get_wide(), after = get();
	SetStdHandle(STD_INPUT_HANDLE, standard);
	CloseHandle(input);
	DeleteFileW(path);
	if (first != 'q' || second != L'r' || after != -1)
		return 44;
	return 0;
}

/* _putch and _putwch write to the console and answer the character; without one they answer EOF and WEOF. */
static int console_output(void)
{
	int (__cdecl *put)(int) = (void *)find("_putch");
	wint_t (__cdecl *put_wide)(wchar_t) = (void *)find("_putwch");
	HANDLE console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	DWORD mode;
	const int present = console != INVALID_HANDLE_VALUE && GetConsoleMode(console, &mode);
	if (console != INVALID_HANDLE_VALUE)
		CloseHandle(console);
	if (put('.') != (present ? '.' : -1) || put_wide(L'\n') != (present ? L'\n' : 0xffff))
		return 50;
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
	if (!GetTempPathW(MAX_PATH, directory) || !GetEnvironmentVariableW(L"ComSpec", interpreter, MAX_PATH))
		return 2;
	const wchar_t *line = GetCommandLineW();
	const int length = lstrlenW(line);
	if (length > 5 && !lstrcmpW(line + length - 5, L" exec"))
		return execute();
	int result = commands();
	if (!result)
		result = programs();
	if (!result)
		result = replaced();
	if (!result)
		result = keys();
	if (!result)
		result = console_output();
	return result;
}
