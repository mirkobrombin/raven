/* SPDX-License-Identifier: MIT */
#include "wshom.h"

/* WshScriptExec: a program Exec started, its standard streams on pipes the script reads and writes as text streams. */

typedef struct {
	sr_object base;
	PROCESS_INFORMATION process;
	IDispatch *input, *output, *error;
} script_exec;

enum { EXEC_STATUS = 1, EXEC_EXITCODE, EXEC_PROCESSID, EXEC_STDIN, EXEC_STDOUT, EXEC_STDERR, EXEC_TERMINATE };

static const sr_member members[] = {
	{ L"Status", EXEC_STATUS, 0 }, { L"ExitCode", EXEC_EXITCODE, 0 }, { L"ProcessID", EXEC_PROCESSID, 0 }, { L"StdIn", EXEC_STDIN, 0 },
	{ L"StdOut", EXEC_STDOUT, 0 }, { L"StdErr", EXEC_STDERR, 0 }, { L"Terminate", EXEC_TERMINATE, 0 }, { NULL },
};

static HRESULT stream(IDispatch *stream, VARIANT *result)
{
	IDispatch_AddRef(stream);
	return sr_return_object(result, stream);
}

static HRESULT exec_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	script_exec *e = (script_exec *)self;
	DWORD code = 0;
	if (sr_is_put(flags))
		return DISP_E_MEMBERNOTFOUND;
	switch (id) {
	case EXEC_STATUS:
		/* 0 while the program runs, 1 once it ended. */
		return sr_return_long(result, WaitForSingleObject(e->process.hProcess, 0) == WAIT_OBJECT_0);
	case EXEC_EXITCODE:
		if (!GetExitCodeProcess(e->process.hProcess, &code) || code == STILL_ACTIVE)
			code = 0;
		return sr_return_long(result, (LONG)code);
	case EXEC_PROCESSID:
		return sr_return_long(result, (LONG)e->process.dwProcessId);
	case EXEC_STDIN:
		return stream(e->input, result);
	case EXEC_STDOUT:
		return stream(e->output, result);
	case EXEC_STDERR:
		return stream(e->error, result);
	case EXEC_TERMINATE:
		if (WaitForSingleObject(e->process.hProcess, 0) != WAIT_OBJECT_0)
			TerminateProcess(e->process.hProcess, 0);
		return S_OK;
	}
	return DISP_E_MEMBERNOTFOUND;
}

static void exec_destroy(sr_object *self)
{
	script_exec *e = (script_exec *)self;
	IDispatch_Release(e->input);
	IDispatch_Release(e->output);
	IDispatch_Release(e->error);
	CloseHandle(e->process.hProcess);
	CloseHandle(e->process.hThread);
}

static const sr_class exec_class = { members, exec_invoke, exec_destroy };

/* A pipe whose one end the child inherits; the other end stays here. */
static BOOL pipe_for_child(HANDLE *child, HANDLE *ours, BOOL child_reads)
{
	SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
	HANDLE read, write;
	if (!CreatePipe(&read, &write, &inherit, 0))
		return FALSE;
	*child = child_reads ? read : write;
	*ours = child_reads ? write : read;
	SetHandleInformation(*ours, HANDLE_FLAG_INHERIT, 0);
	return TRUE;
}

HRESULT wsh_exec(const WCHAR *command, VARIANT *result)
{
	HANDLE child[3] = { 0 }, ours[3] = { 0 };
	HRESULT hr = S_OK;
	for (int i = 0; i < 3 && SUCCEEDED(hr); i++)
		if (!pipe_for_child(&child[i], &ours[i], i == 0))
			hr = HRESULT_FROM_WIN32(GetLastError());
	PROCESS_INFORMATION process = { 0 };
	if (SUCCEEDED(hr)) {
		STARTUPINFOW startup = { sizeof(startup) };
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = child[0];
		startup.hStdOutput = child[1];
		startup.hStdError = child[2];
		/* CreateProcess may write into the command line. */
		WCHAR *line = sr_copy(command);
		if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process))
			hr = HRESULT_FROM_WIN32(GetLastError());
		sr_free(line);
	}
	for (int i = 0; i < 3; i++)
		if (child[i])
			CloseHandle(child[i]);
	if (FAILED(hr)) {
		for (int i = 0; i < 3; i++)
			if (ours[i])
				CloseHandle(ours[i]);
		return hr;
	}
	script_exec *e = sr_object_new(&exec_class, sizeof(script_exec));
	e->process = process;
	e->input = sr_text_stream_new(ours[0], TRUE, SR_FOR_WRITING, FALSE);
	e->output = sr_text_stream_new(ours[1], TRUE, SR_FOR_READING, FALSE);
	e->error = sr_text_stream_new(ours[2], TRUE, SR_FOR_READING, FALSE);
	return sr_return_object(result, sr_dispatch(e));
}
