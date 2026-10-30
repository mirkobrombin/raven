#include <windows.h>

/* The command processor in the prefix, through pipes: a batch file started with CreateProcess runs in cmd /c, cmd /c runs built-in
   commands and programs and ends with their ERRORLEVEL, commands piped to its prompt run one by one, and a batch file that uses the
   parser and the built-ins, and one that uses the command-line tools, give exactly the output they should. Returns the failed step,
   after the output that failed it. */

static WCHAR folder[MAX_PATH];

/* No C runtime: zeroing the structures below may ask for this. */
void *memset(void *target, int value, size_t size)
{
	unsigned char *bytes = target;
	while (size--)
		*bytes++ = (unsigned char)value;
	return target;
}

static DWORD length(const WCHAR *text)
{
	DWORD size = 0;
	while (text[size])
		++size;
	return size;
}

static void append(WCHAR *target, const WCHAR *text)
{
	WCHAR *end = target + length(target);
	while (*text)
		*end++ = *text++;
	*end = 0;
}

static BOOL contains(const char *text, const char *wanted)
{
	for (; *text; ++text) {
		const char *a = text, *b = wanted;
		while (*a && *b && *a == *b)
			++a, ++b;
		if (!*b)
			return TRUE;
	}
	return FALSE;
}

static DWORD length_of(const char *text)
{
	DWORD size = 0;
	while (text[size])
		++size;
	return size;
}

static BOOL same(const char *left, const char *right)
{
	while (*left && *left == *right)
		++left, ++right;
	return *left == *right;
}

/* Runs a command line with its output, and its error output, in `text`; `typed` is what it reads, if anything. */
static BOOL run(const WCHAR *application, const WCHAR *command, const char *typed, char *text, DWORD capacity, DWORD *code)
{
	SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
	HANDLE read_end, write_end, feed_read = NULL, feed_write = NULL;
	if (!CreatePipe(&read_end, &write_end, &inherit, 0))
		return FALSE;
	SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
	if (typed) {
		if (!CreatePipe(&feed_read, &feed_write, &inherit, 0))
			return FALSE;
		SetHandleInformation(feed_write, HANDLE_FLAG_INHERIT, 0);
	}
	STARTUPINFOW startup = {sizeof startup};
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = feed_read;
	startup.hStdOutput = write_end;
	startup.hStdError = write_end;
	PROCESS_INFORMATION process;
	WCHAR line[1024] = {0};
	append(line, command);
	if (!CreateProcessW(application, line, NULL, NULL, TRUE, 0, NULL, folder, &startup, &process))
		return FALSE;
	CloseHandle(write_end);
	if (typed) {
		DWORD written, size = 0;
		while (typed[size])
			++size;
		CloseHandle(feed_read);
		WriteFile(feed_write, typed, size, &written, NULL);
		CloseHandle(feed_write);
	}
	DWORD total = 0, count;
	while (total < capacity - 1 && ReadFile(read_end, text + total, capacity - 1 - total, &count, NULL) && count)
		total += count;
	text[total] = 0;
	CloseHandle(read_end);
	WaitForSingleObject(process.hProcess, INFINITE);
	GetExitCodeProcess(process.hProcess, code);
	CloseHandle(process.hProcess);
	CloseHandle(process.hThread);
	return TRUE;
}

static const char script[] = "@echo off\r\n"
	"set GREETING=hello\r\n"
	"echo %GREETING% from batch\r\n"
	"goto skip\r\n"
	"echo not skipped\r\n"
	":skip\r\n"
	"echo args %1 %2\r\n"
	"exit /b 7\r\n";

/* The parser and the built-ins together: IF and ELSE, FOR in its forms, CALL to a label, SETLOCAL with delayed expansion, SET /A and
   the edits of variables, redirection (handles 3 to 9 too) and pipes, the file commands, GOTO and EXIT /B. */
static const char features[] = "@echo off\r\n"
	"set A=hello\r\n"
	"if \"%A%\"==\"hello\" (echo if) else (echo bad)\r\n"
	"if /i not \"%A%\"==\"X\" echo not\r\n"
	"if 10 GTR 9 echo gtr\r\n"
	"if exist features.bat echo exist\r\n"
	"for %%i in (a b) do echo for %%i\r\n"
	"for /l %%n in (1,2,5) do echo l %%n\r\n"
	"for /f \"tokens=1,2 delims=,\" %%a in (\"x,y,z\") do echo f %%a %%b\r\n"
	"for /f \"delims=\" %%l in ('echo captured') do echo %%l\r\n"
	"call :sub one \"two words\"\r\n"
	"echo called %ERRORLEVEL%\r\n"
	"setlocal enabledelayedexpansion\r\n"
	"set B=0\r\n"
	"for %%i in (1 2 3) do set /a B+=%%i >nul\r\n"
	"echo delayed !B!\r\n"
	"endlocal\r\n"
	"echo local [%B%]\r\n"
	"set /a C=(2+3)*4\r\n"
	"echo arith %C%\r\n"
	"set S=abcdef\r\n"
	"echo edit %S:~1,3% %S:c=X% %S:~-2%\r\n"
	"echo redirected> out.txt\r\n"
	"echo appended>> out.txt\r\n"
	"type out.txt\r\n"
	"echo exit 7| cmd /q /d >nul\r\n"
	"echo pipe %ERRORLEVEL%\r\n"
	"(echo three>&3) 3>three.txt\r\n"
	"type three.txt\r\n"
	"del three.txt\r\n"
	"(echo swap>&2) 3>&1 2>nul 2>&3\r\n"
	"echo bad 2>nul 1>&5 || echo nodup\r\n"
	"md sub\\deep\r\n"
	"echo x> sub\\deep\\f.txt\r\n"
	"copy sub\\deep\\f.txt copy.txt >nul && echo copy\r\n"
	"ren copy.txt renamed.txt && echo ren\r\n"
	"move renamed.txt sub >nul && echo move\r\n"
	"dir /b sub\r\n"
	"del sub\\renamed.txt && if not exist sub\\renamed.txt echo del\r\n"
	"rd /s /q sub && if not exist sub echo rd\r\n"
	"del out.txt\r\n"
	"pushd sub 2>nul || echo pushd failed\r\n"
	"nosuch 2>nul || echo missing %ERRORLEVEL%\r\n"
	"goto end\r\n"
	"echo never\r\n"
	":end\r\n"
	"echo nx0 %~nx0\r\n"
	"exit /b 3\r\n"
	":sub\r\n"
	"echo sub %1 %~2 %0\r\n"
	"exit /b 5\r\n";

static const char features_output[] = "if\r\n"
	"not\r\n"
	"gtr\r\n"
	"exist\r\n"
	"for a\r\n"
	"for b\r\n"
	"l 1\r\n"
	"l 3\r\n"
	"l 5\r\n"
	"f x y\r\n"
	"captured\r\n"
	"sub one two words :sub\r\n"
	"called 5\r\n"
	"delayed 6\r\n"
	"local []\r\n"
	"arith 20\r\n"
	"edit bcd abXdef ef\r\n"
	"redirected\r\n"
	"appended\r\n"
	"pipe 7\r\n"
	"three\r\n"
	"swap\r\n"
	"nodup\r\n"
	"copy\r\n"
	"ren\r\n"
	"move\r\n"
	"deep\r\n"
	"renamed.txt\r\n"
	"del\r\n"
	"rd\r\n"
	"pushd failed\r\n"
	"missing 1\r\n"
	"nx0 features.bat\r\n";

/* The command-line tools beside cmd: FIND, FINDSTR, SORT, WHERE, XCOPY, ATTRIB, REG (with a .reg file that escapes, splits a line,
   and deletes a value and a key), CHOICE, HOSTNAME, WHOAMI, TASKLIST and TASKKILL, each used as a batch file uses it. */
static const char tools[] = "@echo off\r\n"
	"echo banana> fruit.txt\r\n"
	"echo apple>> fruit.txt\r\n"
	"echo Cherry>> fruit.txt\r\n"
	"echo apple pie>> fruit.txt\r\n"
	"find \"apple\" fruit.txt\r\n"
	"find /c /i \"APPLE\" fruit.txt\r\n"
	"type fruit.txt | find /v \"apple\"\r\n"
	"find \"zzz\" fruit.txt >nul || echo find none\r\n"
	"findstr /n \"^a.*e$\" fruit.txt\r\n"
	"findstr /i /c:\"apple pie\" fruit.txt\r\n"
	"findstr /i \"ban che\" fruit.txt\r\n"
	"findstr /x \"apple\" fruit.txt\r\n"
	"findstr \"\\<pie\\>\" fruit.txt\r\n"
	"findstr \"[bc]\" fruit.txt\r\n"
	"findstr \"zzz\" fruit.txt || echo findstr none\r\n"
	"sort fruit.txt\r\n"
	"sort /r fruit.txt\r\n"
	"where cmd\r\n"
	"where /q nothing-here || echo where none\r\n"
	"md src\\sub\r\n"
	"echo one> src\\a.txt\r\n"
	"echo two> src\\sub\\b.txt\r\n"
	"xcopy src dst /s /i\r\n"
	"dir /b dst dst\\sub\r\n"
	"attrib +r src\\a.txt\r\n"
	"attrib src\\a.txt | find /c \"R\"\r\n"
	"attrib -r src\\a.txt\r\n"
	"rd /s /q src dst\r\n"
	"reg add HKCU\\Software\\RavenToolsTest /v Name /d hello /f\r\n"
	"reg add HKCU\\Software\\RavenToolsTest /v Count /t REG_DWORD /d 42 /f\r\n"
	"reg add HKCU\\Software\\RavenToolsTest\\Sub /ve /d inner /f\r\n"
	"reg query HKCU\\Software\\RavenToolsTest /s\r\n"
	"reg export HKCU\\Software\\RavenToolsTest saved.reg /y\r\n"
	"reg delete HKCU\\Software\\RavenToolsTest /f\r\n"
	"reg query HKCU\\Software\\RavenToolsTest 2>nul || echo reg deleted\r\n"
	"reg import saved.reg\r\n"
	"reg query HKCU\\Software\\RavenToolsTest\\Sub /ve\r\n"
	"reg delete HKCU\\Software\\RavenToolsTest /f\r\n"
	"del saved.reg fruit.txt\r\n"
	"echo Windows Registry Editor Version 5.00> import.reg\r\n"
	"echo.>> import.reg\r\n"
	"echo [HKEY_CURRENT_USER\\Software\\RavenImportTest]>> import.reg\r\n"
	"echo \"Quote\"=\"say \\\"hi\\\" C:\\\\dir\">> import.reg\r\n"
	"echo \"Multi\"=hex(7):61,00,00,00,62,00,00,00,00,00>> import.reg\r\n"
	"echo \"Expand\"=hex(2):25,00,41,00,25,00,00,00>> import.reg\r\n"
	"echo \"Bytes\"=hex:01,02,\\>> import.reg\r\n"
	"echo   ab>> import.reg\r\n"
	"echo \"Gone\"=\"x\">> import.reg\r\n"
	"echo \"Gone\"=->> import.reg\r\n"
	"echo.>> import.reg\r\n"
	"echo [HKEY_CURRENT_USER\\Software\\RavenImportTest\\Old]>> import.reg\r\n"
	"echo \"Value\"=dword:00000001>> import.reg\r\n"
	"echo.>> import.reg\r\n"
	"echo [-HKEY_CURRENT_USER\\Software\\RavenImportTest\\Old]>> import.reg\r\n"
	"reg import import.reg\r\n"
	"reg query HKCU\\Software\\RavenImportTest /s\r\n"
	"reg delete HKCU\\Software\\RavenImportTest /f\r\n"
	"del import.reg\r\n"
	"echo n| choice /c yn /m \"Go\"\r\n"
	"echo choice %errorlevel%\r\n"
	"hostname | find /v \"\" >nul && echo hostname\r\n"
	"whoami | find /v \"\" >nul && echo whoami\r\n"
	"tasklist /nh /fo csv | find /i \"tasklist.exe\" >nul && echo tasklist\r\n"
	"taskkill /pid 999999 2>nul || echo taskkill none\r\n"
	"taskkill /im cmd.exe >nul 2>nul\r\n"
	"if errorlevel 1 echo taskkill windowless\r\n"
	"exit /b 0\r\n";

static const char tools_output[] = "\r\n"
	"---------- FRUIT.TXT\r\n"
	"apple\r\n"
	"apple pie\r\n"
	"\r\n"
	"---------- FRUIT.TXT: 2\r\n"
	"banana\r\n"
	"Cherry\r\n"
	"find none\r\n"
	"2:apple\r\n"
	"4:apple pie\r\n"
	"apple pie\r\n"
	"banana\r\n"
	"Cherry\r\n"
	"apple\r\n"
	"apple pie\r\n"
	"banana\r\n"
	"findstr none\r\n"
	"apple\r\n"
	"apple pie\r\n"
	"banana\r\n"
	"Cherry\r\n"
	"Cherry\r\n"
	"banana\r\n"
	"apple pie\r\n"
	"apple\r\n"
	"C:\\Windows\\system32\\cmd.exe\r\n"
	"where none\r\n"
	"src\\a.txt\r\n"
	"src\\sub\\b.txt\r\n"
	"2 File(s) copied\r\n"
	"a.txt\r\n"
	"sub\r\n"
	"b.txt\r\n"
	"1\r\n"
	"The operation completed successfully.\r\n"
	"The operation completed successfully.\r\n"
	"The operation completed successfully.\r\n"
	"\r\n"
	"HKEY_CURRENT_USER\\Software\\RavenToolsTest\r\n"
	"    Count    REG_DWORD    0x2a\r\n"
	"    Name    REG_SZ    hello\r\n"
	"\r\n"
	"HKEY_CURRENT_USER\\Software\\RavenToolsTest\\Sub\r\n"
	"    (Default)    REG_SZ    inner\r\n"
	"The operation completed successfully.\r\n"
	"The operation completed successfully.\r\n"
	"reg deleted\r\n"
	"The operation completed successfully.\r\n"
	"\r\n"
	"HKEY_CURRENT_USER\\Software\\RavenToolsTest\\Sub\r\n"
	"    (Default)    REG_SZ    inner\r\n"
	"The operation completed successfully.\r\n"
	"The operation completed successfully.\r\n"
	"\r\n"
	"HKEY_CURRENT_USER\\Software\\RavenImportTest\r\n"
	"    Bytes    REG_BINARY    0102AB\r\n"
	"    Expand    REG_EXPAND_SZ    %A%\r\n"
	"    Multi    REG_MULTI_SZ    a\\0b\r\n"
	"    Quote    REG_SZ    say \"hi\" C:\\dir\r\n"
	"The operation completed successfully.\r\n"
	"Go [Y,N]?N\r\n"
	"choice 2\r\n"
	"hostname\r\n"
	"whoami\r\n"
	"tasklist\r\n"
	"taskkill none\r\n"
	"taskkill windowless\r\n";

int raven_entry(void)
{
	static char text[8192];
	DWORD code = 0, written;
	const WCHAR *processor = L"C:\\Windows\\System32\\cmd.exe";

	if (GetFileAttributesW(processor) == INVALID_FILE_ATTRIBUTES)
		return 1;
	GetTempPathW(MAX_PATH, folder);
	append(folder, L"raven-cmd-test");
	CreateDirectoryW(folder, NULL);
	WCHAR path[MAX_PATH] = {0};
	append(path, folder);
	append(path, L"\\script.bat");
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, script, sizeof script - 1, &written, NULL))
		return 2;
	CloseHandle(file);

	/* A batch file given to CreateProcess runs in the command processor, with its arguments. */
	WCHAR line[MAX_PATH + 32] = L"\"";
	append(line, path);
	append(line, L"\" one two");
	if (!run(NULL, line, NULL, text, sizeof text, &code) || code != 7 || !same(text, "hello from batch\r\nargs one two\r\n"))
		return 3;
	if (!run(processor, L"cmd.exe /c exit 5", NULL, text, sizeof text, &code) || code != 5)
		return 4;
	if (!run(processor, L"cmd.exe /c echo x", NULL, text, sizeof text, &code) || code || !same(text, "x\r\n"))
		return 5;
	if (!run(processor, L"cmd.exe /c no-such-program", NULL, text, sizeof text, &code) || code != 9009 ||
		!contains(text, "is not recognized as an internal or external command"))
		return 6;
	if (!run(processor, L"cmd.exe /c ver", NULL, text, sizeof text, &code) || code || !contains(text, "Raven Command Processor"))
		return 7;
	/* A program cmd finds on PATH: another command processor, whose exit code becomes ERRORLEVEL. */
	if (!run(processor, L"cmd.exe /c cmd /c exit 3", NULL, text, sizeof text, &code) || code != 3)
		return 8;
	/* Commands typed at the prompt, from a pipe: the prompt shows the folder, and EXIT ends with its code. */
	if (!run(processor, L"cmd.exe", "echo piped\r\nset RAVEN_TYPED=yes\r\necho %RAVEN_TYPED%\r\nexit 4\r\n", text, sizeof text, &code) ||
		code != 4 || !contains(text, "raven-cmd-test>") || !contains(text, "piped\r\n") || !contains(text, "yes\r\n"))
		return 9;
	/* A batch file through cmd /c, its output exact, and the code its EXIT /B gives. */
	WCHAR features_path[MAX_PATH] = {0};
	append(features_path, folder);
	append(features_path, L"\\features.bat");
	file = CreateFileW(features_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, features, sizeof features - 1, &written, NULL))
		return 10;
	CloseHandle(file);
	if (!run(processor, L"cmd.exe /c features.bat", NULL, text, sizeof text, &code) || code != 3 || !same(text, features_output)) {
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length_of(text), &written, NULL);
		return 11;
	}
	DeleteFileW(features_path);
	WCHAR tools_path[MAX_PATH] = {0};
	append(tools_path, folder);
	append(tools_path, L"\\tools.bat");
	file = CreateFileW(tools_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, tools, sizeof tools - 1, &written, NULL))
		return 12;
	CloseHandle(file);
	if (!run(processor, L"cmd.exe /c tools.bat", NULL, text, sizeof text, &code) || code || !same(text, tools_output)) {
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length_of(text), &written, NULL);
		return 13;
	}
	DeleteFileW(tools_path);
	DeleteFileW(path);
	RemoveDirectoryW(folder);
	return 0;
}
