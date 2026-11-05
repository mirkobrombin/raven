/* SPDX-License-Identifier: MIT */
#include "scrrun.h"

/* TextStream: text read from or written to a file, or to a standard stream, in the ANSI code page or as UTF-16. Reading takes the
 * file in pieces as far as it is needed, so a pipe or the console gives its lines as they come. */

typedef struct {
	sr_object base;
	HANDLE file;
	BOOL owned, unicode, closed, ended;
	LONG mode;
	/* What was read and not yet taken: text[at..length). */
	WCHAR *text;
	UINT length, room, at;
	BYTE carry;
	BOOL carrying, first;
	LONG line, column;
} text_stream;

enum {
	TS_LINE = 1, TS_COLUMN, TS_ATENDOFSTREAM, TS_ATENDOFLINE, TS_READ, TS_READLINE, TS_READALL, TS_WRITE, TS_WRITELINE,
	TS_WRITEBLANKLINES, TS_SKIP, TS_SKIPLINE, TS_CLOSE,
};

static const sr_member members[] = {
	{ L"Line", TS_LINE, 0 }, { L"Column", TS_COLUMN, 0 }, { L"AtEndOfStream", TS_ATENDOFSTREAM, 0 }, { L"AtEndOfLine", TS_ATENDOFLINE, 0 },
	{ L"Read", TS_READ, 1 }, { L"ReadLine", TS_READLINE, 0 }, { L"ReadAll", TS_READALL, 0 }, { L"Write", TS_WRITE, 1 }, { L"WriteLine", TS_WRITELINE, 1 },
	{ L"WriteBlankLines", TS_WRITEBLANKLINES, 1 }, { L"Skip", TS_SKIP, 1 }, { L"SkipLine", TS_SKIPLINE, 0 }, { L"Close", TS_CLOSE, 0 }, { NULL },
};

static void append(text_stream *ts, const WCHAR *text, UINT length)
{
	if (ts->length + length > ts->room) {
		ts->room = (ts->length + length) * 2 + 64;
		ts->text = sr_grow(ts->text, ts->room * sizeof(WCHAR));
	}
	memcpy(ts->text + ts->length, text, length * sizeof(WCHAR));
	ts->length += length;
}

/* Reads one more piece of the file; FALSE at its end. */
static BOOL fill(text_stream *ts)
{
	if (ts->ended)
		return FALSE;
	BYTE buffer[16384 + 1];
	DWORD got = 0, offset = 0;
	if (ts->carrying) {
		buffer[0] = ts->carry;
		offset = 1;
		ts->carrying = FALSE;
	}
	if (!ReadFile(ts->file, buffer + offset, sizeof(buffer) - 1 - offset, &got, NULL) || !got) {
		ts->ended = TRUE;
		return FALSE;
	}
	got += offset;
	BYTE *bytes = buffer;
	if (ts->first) {
		ts->first = FALSE;
		/* A UTF-16 file says so with its byte order mark, whatever format the caller asked for. */
		if (got >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
			ts->unicode = TRUE;
			bytes += 2;
			got -= 2;
		}
	}
	if (ts->unicode) {
		if (got & 1) {
			ts->carry = bytes[got - 1];
			ts->carrying = TRUE;
			got--;
		}
		append(ts, (const WCHAR *)bytes, got / 2);
	} else {
		int length = MultiByteToWideChar(CP_ACP, 0, (const char *)bytes, (int)got, NULL, 0);
		WCHAR *wide = sr_alloc((length + 1) * sizeof(WCHAR));
		MultiByteToWideChar(CP_ACP, 0, (const char *)bytes, (int)got, wide, length);
		append(ts, wide, (UINT)length);
		sr_free(wide);
	}
	return TRUE;
}

/* Makes sure count characters are read, if the file has them; how many there are. */
static UINT ensure(text_stream *ts, UINT count)
{
	while (ts->length - ts->at < count && fill(ts))
		;
	UINT have = ts->length - ts->at;
	return have < count ? have : count;
}

static void advance(text_stream *ts, const WCHAR *text, UINT length)
{
	for (UINT i = 0; i < length; i++) {
		if (text[i] == L'\n') {
			ts->line++;
			ts->column = 1;
		} else
			ts->column++;
	}
}

static BSTR take(text_stream *ts, UINT count)
{
	BSTR text = SysAllocStringLen(ts->text + ts->at, count);
	advance(ts, ts->text + ts->at, count);
	ts->at += count;
	/* Once everything read was taken, the buffer starts over. */
	if (ts->at == ts->length)
		ts->at = ts->length = 0;
	return text;
}

/* Where the current line ends, and how long its line break is (CR LF, LF or CR); FALSE when the stream ended first. */
static BOOL find_line(text_stream *ts, UINT *end, UINT *brk)
{
	UINT from = ts->at;
	for (;;) {
		for (UINT i = from; i < ts->length; i++) {
			if (ts->text[i] == L'\n') {
				*end = i;
				*brk = 1;
				return TRUE;
			}
			if (ts->text[i] == L'\r') {
				if (i + 1 >= ts->length && fill(ts))
					break;
				*end = i;
				*brk = i + 1 < ts->length && ts->text[i + 1] == L'\n' ? 2 : 1;
				return TRUE;
			}
			from = i + 1;
		}
		if (from >= ts->length && !fill(ts)) {
			*end = ts->length;
			*brk = 0;
			return FALSE;
		}
	}
}

static HRESULT write_text(text_stream *ts, const WCHAR *text, UINT length)
{
	DWORD put, mode;
	BOOL fine;
	/* A console takes characters, whatever the stream's encoding. */
	if (GetConsoleMode(ts->file, &mode))
		fine = WriteConsoleW(ts->file, text, length, &put, NULL);
	else if (ts->unicode)
		fine = WriteFile(ts->file, text, length * sizeof(WCHAR), &put, NULL);
	else {
		int size = WideCharToMultiByte(CP_ACP, 0, text, (int)length, NULL, 0, NULL, NULL);
		char *bytes = sr_alloc(size + 1);
		WideCharToMultiByte(CP_ACP, 0, text, (int)length, bytes, size, NULL, NULL);
		fine = WriteFile(ts->file, bytes, (DWORD)size, &put, NULL);
		sr_free(bytes);
	}
	if (!fine)
		return sr_last_error();
	advance(ts, text, length);
	return S_OK;
}

static HRESULT stream_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	text_stream *ts = (text_stream *)self;
	BOOL reading = ts->mode == SR_FOR_READING;
	LONG count = 0;
	BSTR text = NULL;
	HRESULT hr;
	if (sr_is_put(flags))
		return DISP_E_MEMBERNOTFOUND;
	if (ts->closed && id != TS_CLOSE)
		return sr_error(SR_E_BAD_FILE_MODE);
	switch (id) {
	case TS_LINE:
		return sr_return_long(result, ts->line);
	case TS_COLUMN:
		return sr_return_long(result, ts->column);
	case TS_ATENDOFSTREAM:
	case TS_ATENDOFLINE:
		if (!reading)
			return sr_error(SR_E_BAD_FILE_MODE);
		if (!ensure(ts, 1))
			return sr_return_bool(result, TRUE);
		return sr_return_bool(result, id == TS_ATENDOFLINE && (ts->text[ts->at] == L'\n' || ts->text[ts->at] == L'\r'));
	case TS_READ:
	case TS_SKIP:
		if (!reading)
			return sr_error(SR_E_BAD_FILE_MODE);
		if (FAILED(hr = sr_long(sr_arg(params, flags, 0), &count)))
			return hr;
		if (count < 0)
			return sr_error(SR_E_ARGUMENT);
		if (!ensure(ts, 1) && count)
			return sr_error(SR_E_PAST_END);
		text = take(ts, ensure(ts, (UINT)count));
		if (id == TS_SKIP) {
			SysFreeString(text);
			return S_OK;
		}
		return sr_return_bstr(result, text);
	case TS_READLINE:
	case TS_SKIPLINE: {
		if (!reading)
			return sr_error(SR_E_BAD_FILE_MODE);
		if (!ensure(ts, 1))
			return sr_error(SR_E_PAST_END);
		UINT end, brk;
		find_line(ts, &end, &brk);
		text = take(ts, end - ts->at);
		BSTR gap = take(ts, brk);
		SysFreeString(gap);
		if (id == TS_SKIPLINE) {
			SysFreeString(text);
			return S_OK;
		}
		return sr_return_bstr(result, text);
	}
	case TS_READALL:
		if (!reading)
			return sr_error(SR_E_BAD_FILE_MODE);
		while (fill(ts))
			;
		if (ts->at >= ts->length)
			return sr_error(SR_E_PAST_END);
		return sr_return_bstr(result, take(ts, ts->length - ts->at));
	case TS_WRITE:
	case TS_WRITELINE: {
		if (reading)
			return sr_error(SR_E_BAD_FILE_MODE);
		VARIANT *what = sr_arg(params, flags, 0);
		if (what || id == TS_WRITE) {
			if (FAILED(hr = sr_text(what, &text)))
				return hr;
			hr = write_text(ts, text, SysStringLen(text));
			SysFreeString(text);
			if (FAILED(hr))
				return hr;
		}
		return id == TS_WRITELINE ? write_text(ts, L"\r\n", 2) : S_OK;
	}
	case TS_WRITEBLANKLINES:
		if (reading)
			return sr_error(SR_E_BAD_FILE_MODE);
		if (FAILED(hr = sr_long(sr_arg(params, flags, 0), &count)))
			return hr;
		for (LONG i = 0; i < count; i++)
			if (FAILED(hr = write_text(ts, L"\r\n", 2)))
				return hr;
		return S_OK;
	case TS_CLOSE:
		if (!ts->closed && ts->owned)
			CloseHandle(ts->file);
		ts->closed = TRUE;
		return S_OK;
	}
	return DISP_E_MEMBERNOTFOUND;
}

static void stream_destroy(sr_object *self)
{
	text_stream *ts = (text_stream *)self;
	if (!ts->closed && ts->owned)
		CloseHandle(ts->file);
	sr_free(ts->text);
}

static const sr_class stream_class = { members, stream_invoke, stream_destroy };

IDispatch *sr_text_stream_new(HANDLE file, BOOL owned, LONG mode, BOOL unicode)
{
	text_stream *ts = sr_object_new(&stream_class, sizeof(text_stream));
	ts->file = file;
	ts->owned = owned;
	ts->mode = mode;
	ts->unicode = unicode;
	ts->first = TRUE;
	ts->line = ts->column = 1;
	return sr_dispatch(ts);
}

HRESULT sr_open_text(const WCHAR *path, LONG mode, BOOL create, LONG format, BOOL overwrite, VARIANT *result)
{
	if (mode != SR_FOR_READING && mode != SR_FOR_WRITING && mode != SR_FOR_APPENDING)
		return sr_error(SR_E_ARGUMENT);
	BOOL unicode = format == -1;
	DWORD access = mode == SR_FOR_READING ? GENERIC_READ : GENERIC_WRITE, disposition;
	if (mode == SR_FOR_READING)
		disposition = create ? OPEN_ALWAYS : OPEN_EXISTING;
	else if (mode == SR_FOR_WRITING)
		disposition = !overwrite ? CREATE_NEW : create ? CREATE_ALWAYS : TRUNCATE_EXISTING;
	else
		disposition = create ? OPEN_ALWAYS : OPEN_EXISTING;
	HANDLE file = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return sr_last_error();
	LARGE_INTEGER size = { 0 };
	GetFileSizeEx(file, &size);
	if (mode == SR_FOR_APPENDING) {
		LARGE_INTEGER zero = { 0 };
		SetFilePointerEx(file, zero, NULL, FILE_END);
		/* Appending to a UTF-16 file keeps writing UTF-16. */
		if (size.QuadPart >= 2 && !unicode && format == -2) {
			HANDLE peek = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
			BYTE bom[2];
			DWORD got = 0;
			if (peek != INVALID_HANDLE_VALUE) {
				unicode = ReadFile(peek, bom, 2, &got, NULL) && got == 2 && bom[0] == 0xff && bom[1] == 0xfe;
				CloseHandle(peek);
			}
		}
	}
	if (mode != SR_FOR_READING && unicode && (mode == SR_FOR_WRITING || !size.QuadPart)) {
		DWORD put;
		WriteFile(file, "\xff\xfe", 2, &put, NULL);
	}
	return sr_return_object(result, sr_text_stream_new(file, TRUE, mode, unicode));
}
