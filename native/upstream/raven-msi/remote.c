/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

/* Custom action libraries run in a process of their own, of the library's width, so a crash or a 32-bit library cannot
 * take the installer down. Their session handle is a proxy: every call that needs the package travels over a pipe to the
 * installing process, as an operation, up to two strings and a number; the answer is a status, a number and a string. */

typedef struct {
	UINT op, a_size, b_size;
	INT number;
} request_head;

typedef struct {
	UINT status, text_size;
	INT number;
} answer_head;

static BOOL read_all(HANDLE pipe, void *data, DWORD size)
{
	BYTE *at = data;
	while (size) {
		DWORD got = 0;
		if (!ReadFile(pipe, at, size, &got, NULL) || !got)
			return FALSE;
		at += got;
		size -= got;
	}
	return TRUE;
}

static BOOL write_all(HANDLE pipe, const void *data, DWORD size)
{
	const BYTE *at = data;
	while (size) {
		DWORD put = 0;
		if (!WriteFile(pipe, at, size, &put, NULL) || !put)
			return FALSE;
		at += put;
		size -= put;
	}
	return TRUE;
}

UINT remote_call(HANDLE pipe, UINT op, const WCHAR *a, const WCHAR *b, INT number, WCHAR **text, INT *result)
{
	request_head head = { op, a ? (UINT)(lstrlenW(a) + 1) * sizeof(WCHAR) : 0, b ? (UINT)(lstrlenW(b) + 1) * sizeof(WCHAR) : 0, number };
	answer_head answer;
	if (text)
		*text = NULL;
	if (!write_all(pipe, &head, sizeof head) || (head.a_size && !write_all(pipe, a, head.a_size)) || (head.b_size && !write_all(pipe, b, head.b_size)) ||
		!read_all(pipe, &answer, sizeof answer))
		return ERROR_FUNCTION_FAILED;
	WCHAR *body = NULL;
	if (answer.text_size) {
		body = msi_alloc(answer.text_size);
		if (!read_all(pipe, body, answer.text_size)) {
			msi_free(body);
			return ERROR_FUNCTION_FAILED;
		}
	}
	if (text)
		*text = body;
	else
		msi_free(body);
	if (result)
		*result = answer.number;
	return answer.status;
}

static WCHAR *read_text(HANDLE pipe, UINT size)
{
	if (!size)
		return NULL;
	WCHAR *text = msi_alloc(size);
	if (!read_all(pipe, text, size)) {
		msi_free(text);
		return NULL;
	}
	return text;
}

static UINT serve_one(msi_package *package, UINT op, const WCHAR *a, const WCHAR *b, INT number, WCHAR **text, INT *result)
{
	MSIHANDLE session = package->session;
	switch (op) {
	case REMOTE_GET_PROPERTY:
		*text = package_get(package, a);
		return ERROR_SUCCESS;
	case REMOTE_SET_PROPERTY:
		return package_set(package, a, b);
	case REMOTE_FILE_PATH: {
		tables_load(package);
		msi_file *file = file_find(package, a);
		*text = file ? file_target(package, file) : NULL;
		return ERROR_SUCCESS;
	}
	case REMOTE_TARGET_PATH: {
		tables_load(package);
		const WCHAR *target;
		if (b) {
			msi_component *component = component_find(package, a);
			target = component ? directory_target(package, component->directory) : NULL;
		} else
			target = directory_target(package, a);
		if (!target)
			return ERROR_DIRECTORY;
		*text = msi_copy(target);
		return ERROR_SUCCESS;
	}
	case REMOTE_SOURCE_PATH: {
		*text = package_get(package, L"SourceDir");
		return *text ? ERROR_SUCCESS : ERROR_DIRECTORY;
	}
	case REMOTE_SET_TARGET:
		return MsiSetTargetPathW(session, a, b);
	case REMOTE_MODE:
		*result = MsiGetMode(session, number);
		return ERROR_SUCCESS;
	case REMOTE_SET_MODE:
		return MsiSetMode(session, number & 0xffff, number >> 16);
	case REMOTE_LANGUAGE:
		*result = MsiGetLanguage(session);
		return ERROR_SUCCESS;
	case REMOTE_CONDITION:
		*result = condition_evaluate(package, a);
		return ERROR_SUCCESS;
	case REMOTE_MESSAGE:
		package_message(package, number, a);
		*result = 1;
		return ERROR_SUCCESS;
	case REMOTE_COMPONENT_STATE: {
		tables_load(package);
		msi_component *component = component_find(package, a);
		*result = component ? (number ? component->action : component->installed) : INSTALLSTATE_UNKNOWN;
		return component ? ERROR_SUCCESS : ERROR_UNKNOWN_COMPONENT;
	}
	case REMOTE_FEATURE_STATE: {
		tables_load(package);
		msi_feature *feature = feature_find(package, a);
		*result = feature ? (number ? feature->action : feature->installed) : INSTALLSTATE_UNKNOWN;
		return feature ? ERROR_SUCCESS : ERROR_UNKNOWN_FEATURE;
	}
	case REMOTE_SET_COMPONENT:
		return MsiSetComponentStateW(session, a, number);
	case REMOTE_SET_FEATURE:
		return MsiSetFeatureStateW(session, a, number);
	case REMOTE_DATABASE_PATH:
		*text = package->db->path ? msi_copy(package->db->path) : NULL;
		return ERROR_SUCCESS;
	case REMOTE_DO_ACTION:
		return MsiDoActionW(session, a);
	case REMOTE_SEQUENCE:
		return MsiSequenceW(session, a, number);
	}
	return ERROR_INVALID_FUNCTION;
}

typedef struct {
	msi_package *package;
	HANDLE pipe;
} serve_context;

// Answers one action's host until it closes the pipe. The package stays referenced by the waiting installer.
DWORD WINAPI remote_serve(void *context)
{
	serve_context *serve = context;
	request_head head;
	if (!ConnectNamedPipe(serve->pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
		return 1;
	while (read_all(serve->pipe, &head, sizeof head)) {
		WCHAR *a = read_text(serve->pipe, head.a_size), *b = read_text(serve->pipe, head.b_size), *text = NULL;
		INT result = 0;
		UINT status = serve_one(serve->package, head.op, a, b, head.number, &text, &result);
		answer_head answer = { status, text ? (UINT)(lstrlenW(text) + 1) * sizeof(WCHAR) : 0, result };
		BOOL sent = write_all(serve->pipe, &answer, sizeof answer) && (!answer.text_size || write_all(serve->pipe, text, answer.text_size));
		msi_free(a);
		msi_free(b);
		msi_free(text);
		if (!sent)
			break;
	}
	return 0;
}
