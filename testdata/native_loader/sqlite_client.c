#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The interface of winsqlite3.dll this client uses. */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef int (*open_function)(const char *, sqlite3 **, int, const char *);
typedef int (*close_function)(sqlite3 *);
typedef int (*exec_function)(sqlite3 *, const char *, void *, void *, char **);
typedef int (*prepare_function)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
typedef int (*bind_function)(sqlite3_stmt *, int, int);
typedef int (*step_function)(sqlite3_stmt *);
typedef int (*statement_function)(sqlite3_stmt *);
typedef long long (*column_function)(sqlite3_stmt *, int);
typedef const char *(*version_function)(void);

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_OPEN_READWRITE 0x2
#define SQLITE_OPEN_CREATE 0x4

/* SQLite as Windows ships it, found by name in the system directory: a database in a file is created, written in a transaction with
 * bound parameters, closed, opened again and read back; a malformed statement is refused. */
int raven_entry(void)
{
	HMODULE library = LoadLibraryW(L"winsqlite3.dll");
	if (!library) return 1;
	open_function open = (open_function)GetProcAddress(library, "sqlite3_open_v2");
	close_function close = (close_function)GetProcAddress(library, "sqlite3_close_v2");
	exec_function exec = (exec_function)GetProcAddress(library, "sqlite3_exec");
	prepare_function prepare = (prepare_function)GetProcAddress(library, "sqlite3_prepare_v2");
	bind_function bind = (bind_function)GetProcAddress(library, "sqlite3_bind_int");
	step_function step = (step_function)GetProcAddress(library, "sqlite3_step");
	statement_function reset = (statement_function)GetProcAddress(library, "sqlite3_reset");
	statement_function finalize = (statement_function)GetProcAddress(library, "sqlite3_finalize");
	column_function column = (column_function)GetProcAddress(library, "sqlite3_column_int64");
	version_function version = (version_function)GetProcAddress(library, "sqlite3_libversion");
	if (!open || !close || !exec || !prepare || !bind || !step || !reset || !finalize || !column || !version) return 2;
	if (version()[0] != '3' || version()[1] != '.') return 3;
	char path[MAX_PATH];
	DWORD length = GetTempPathA(MAX_PATH - 16, path);
	if (!length) return 4;
	lstrcpyA(path + length, "raven-sqlite.db");
	DeleteFileA(path);
	sqlite3 *database = NULL;
	if (open(path, &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK || !database) return 5;
	if (exec(database, "CREATE TABLE numbers(value INTEGER NOT NULL); BEGIN", NULL, NULL, NULL) != SQLITE_OK) return 6;
	sqlite3_stmt *insert = NULL;
	if (prepare(database, "INSERT INTO numbers VALUES (?1)", -1, &insert, NULL) != SQLITE_OK || !insert) return 7;
	for (int value = 1; value <= 100; ++value)
		if (bind(insert, 1, value) != SQLITE_OK || step(insert) != SQLITE_DONE || reset(insert) != SQLITE_OK) return 8;
	if (finalize(insert) != SQLITE_OK || exec(database, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) return 9;
	sqlite3_stmt *broken = NULL;
	if (prepare(database, "SELEKT 1", -1, &broken, NULL) == SQLITE_OK || broken) return 10;
	if (close(database) != SQLITE_OK) return 11;
	database = NULL;
	if (open(path, &database, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) return 12;
	sqlite3_stmt *query = NULL;
	if (prepare(database, "SELECT count(*), sum(value) FROM numbers", -1, &query, NULL) != SQLITE_OK) return 13;
	if (step(query) != SQLITE_ROW || column(query, 0) != 100 || column(query, 1) != 5050 || step(query) != SQLITE_DONE) return 14;
	if (finalize(query) != SQLITE_OK || close(database) != SQLITE_OK) return 15;
	if (!DeleteFileA(path)) return 16;
	return 0;
}
