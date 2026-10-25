/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The entry points of the older msvcrt.dll that programs built against it import: startup, the stream table, formatted output to
 * a stream, signals and the x64 language handler of structured exceptions. The runtime also serves this component as msvcrt.dll to
 * 64-bit programs, a C runtime of its own as it is on Windows. Included by ucrtbase.c. */

/* The public headers name these through accessor macros; the variables themselves are what msvcrt exports. */
#undef _fmode
#undef _commode
CRT int _fmode;
CRT int _commode;
CRT char **__initenv;
CRT wchar_t **__winitenv;
#undef __argc
#undef __argv
CRT int __argc;
CRT char **__argv;
/* The command line, which the startup code of older programs reads straight from these variables. */
CRT char *_acmdln;
CRT wchar_t *_wcmdln;

static void msvcrt_attach(void)
{
	_acmdln = (char *)GetCommandLineA();
	_wcmdln = GetCommandLineW();
}

/* The startup code of programs built by MinGW-w64 against msvcrt asks the runtime to set up the numeric locale fields; they already are. */
CRT int __lconv_init(void) { return 0; }

/* The arguments and environment a msvcrt program's startup code asks for before it calls main. */
CRT int __getmainargs(int *count, char ***vector, char ***environment_out, int wildcard, void *startup)
{
	if (!argument_vector)
		_configure_narrow_argv(1);
	if (!environment)
		_initialize_narrow_environment();
	*count = __argc = argument_count;
	*vector = __argv = argument_vector;
	*environment_out = environment;
	__initenv = environment;
	return 0;
}

CRT void __set_app_type(int type) {}

/* msvcrt's numbered internal locks, which startup code takes around the exit table: a critical section each, made on first use. */
#define CRT_LOCKS 64
static CRITICAL_SECTION crt_locks[CRT_LOCKS];
static volatile LONG crt_lock_state[CRT_LOCKS];

CRT void _lock(int number)
{
	if (number < 0 || number >= CRT_LOCKS)
		return;
	/* 0 is untouched, 1 is being made, 2 is ready. */
	if (InterlockedCompareExchange(&crt_lock_state[number], 1, 0) == 0) {
		InitializeCriticalSection(&crt_locks[number]);
		InterlockedExchange(&crt_lock_state[number], 2);
	}
	while (crt_lock_state[number] != 2)
		Sleep(0);
	EnterCriticalSection(&crt_locks[number]);
}

CRT void _unlock(int number)
{
	if (number >= 0 && number < CRT_LOCKS && crt_lock_state[number] == 2)
		LeaveCriticalSection(&crt_locks[number]);
}

/* The standard streams as msvcrt's FILE array: stdin, stdout and stderr follow each other at its 48-byte stride. msvcrt exports the
 * array itself as _iob too, for code that names the streams through it. */
__asm__(".section .drectve\n\t.ascii \" -export:_iob=streams,DATA\"\n\t.text");
CRT void *__iob_func(void)
{
	int index;
	for (index = 0; index < 3; ++index)
		__acrt_iob_func((unsigned)index);
	return streams;
}

CRT void *__p__iob(void) { return __iob_func(); }
/* The environment the startup code hands main, and the multibyte width, as the pointers msvcrt's accessors return. */
CRT char ***__p___initenv(void) { return __p__environ(); }
CRT int *__p___mb_cur_max(void) { return &raven_mb_cur_max; }

/* A runtime error the startup code reports: the program ends with the code msvcrt gives it. */
CRT void _amsg_exit(int code) { _exit(255); }

CRT int vfprintf(void *stream, const char *text, va_list arguments) { return __stdio_common_vfprintf(0, stream, text, NULL, arguments); }
CRT int vprintf(const char *text, va_list arguments) { return vfprintf(__acrt_iob_func(1), text, arguments); }

CRT int fprintf(void *stream, const char *text, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, text);
	result = vfprintf(stream, text, arguments);
	va_end(arguments);
	return result;
}

CRT int printf(const char *text, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, text);
	result = vfprintf(__acrt_iob_func(1), text, arguments);
	va_end(arguments);
	return result;
}

/* Signal handlers: kept per signal, called by raise. SIG_DFL (0) ends the program as an abort does, SIG_IGN (1) does nothing. */
typedef void (__cdecl *signal_handler)(int);
static signal_handler signal_handlers[23];

CRT signal_handler signal(int number, signal_handler handler)
{
	signal_handler previous;
	if (number < 0 || number >= 23) {
		*_errno() = 22;
		return (signal_handler)(ULONG_PTR)-1;
	}
	previous = signal_handlers[number];
	signal_handlers[number] = handler;
	return previous;
}

CRT int raise(int number)
{
	signal_handler handler;
	if (number < 0 || number >= 23)
		return -1;
	handler = signal_handlers[number];
	if (handler == (signal_handler)(ULONG_PTR)1)
		return 0;
	if (!handler)
		_exit(3);
	signal_handlers[number] = NULL;
	handler(number);
	return 0;
}

#ifdef __x86_64__
/* msvcrt's _setjmp takes the frame to unwind to as the CRT's own one does. */
__asm__(
	".section .drectve\n"
	".ascii \" -export:_setjmp\"\n"
	".text\n"
	".globl _setjmp\n"
	"_setjmp:\n"
	"	jmp __intrinsic_setjmp\n");

/* The language handler of C structured exceptions: the scope table lists the guarded ranges with their filter, their handler
 * and, for __finally blocks, no jump target. A dispatch asks the filters; an unwind runs the termination handlers it leaves. */
typedef struct {
	DWORD begin, end, handler, target;
} ScopeRecord;

typedef struct {
	DWORD count;
	ScopeRecord records[1];
} ScopeTable;

CRT EXCEPTION_DISPOSITION __C_specific_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context, DISPATCHER_CONTEXT *dispatch)
{
	const ScopeTable *table = dispatch->HandlerData;
	const ULONG64 base = dispatch->ImageBase, pc = dispatch->ControlPc - base;
	DWORD index;
	if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
		for (index = dispatch->ScopeIndex; index < table->count; ++index) {
			const ScopeRecord *scope = &table->records[index];
			if (pc < scope->begin || pc >= scope->end)
				continue;
			if ((record->ExceptionFlags & EXCEPTION_TARGET_UNWIND) && scope->target && scope->target == dispatch->TargetIp - base)
				break;
			if (!scope->target) {
				dispatch->ScopeIndex = index + 1;
				((void (*)(BOOLEAN, void *))(base + scope->handler))(TRUE, frame);
			}
		}
		return ExceptionContinueSearch;
	}
	for (index = dispatch->ScopeIndex; index < table->count; ++index) {
		const ScopeRecord *scope = &table->records[index];
		LONG decision = EXCEPTION_EXECUTE_HANDLER;
		if (pc < scope->begin || pc >= scope->end || !scope->target)
			continue;
		if (scope->handler != 1) {
			EXCEPTION_POINTERS pointers = {record, context};
			decision = ((LONG (*)(EXCEPTION_POINTERS *, void *))(base + scope->handler))(&pointers, frame);
		}
		if (decision < 0)
			return ExceptionContinueExecution;
		if (decision > 0)
			RtlUnwindEx(frame, (void *)(base + scope->target), record, (void *)(ULONG_PTR)record->ExceptionCode, dispatch->ContextRecord,
				dispatch->HistoryTable);
	}
	return ExceptionContinueSearch;
}
#endif

/* The drives that exist, one bit per letter from A. */
CRT unsigned long _getdrives(void) { return GetLogicalDrives(); }

/* Whether a byte begins a double-byte character in the program's code page. */
CRT int _ismbblead(unsigned int character) { return IsDBCSLeadByte((BYTE)character); }
