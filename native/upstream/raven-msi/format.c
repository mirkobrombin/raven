/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

/* Formatted text: [Property], [1] for a record's field, [%VARIABLE], [#FileKey], [!FileKey], [$ComponentKey], [\c] for a
 * literal character, [~] for a null and {...}, which disappears whole when a property inside it had no value. */

typedef struct {
	WCHAR *text;
	SIZE_T length, room;
} builder;

static void put(builder *out, const WCHAR *text, SIZE_T length)
{
	if (out->length + length + 1 > out->room) {
		out->room = (out->length + length + 1) * 2;
		out->text = msi_grow(out->text, out->room * sizeof(WCHAR));
	}
	if (length)
		CopyMemory(out->text + out->length, text, length * sizeof(WCHAR));
	out->length += length;
	out->text[out->length] = 0;
}

static WCHAR *file_path(msi_package *package, const WCHAR *key)
{
	if (package->remote) {
		WCHAR *text = NULL;
		remote_call(package->remote, REMOTE_FILE_PATH, key, NULL, 0, &text, NULL);
		return text;
	}
	tables_load(package);
	msi_file *file = file_find(package, key);
	return file ? file_target(package, file) : NULL;
}

static WCHAR *component_path(msi_package *package, const WCHAR *key)
{
	if (package->remote) {
		WCHAR *text = NULL;
		remote_call(package->remote, REMOTE_TARGET_PATH, key, L"component", 0, &text, NULL);
		return text;
	}
	tables_load(package);
	msi_component *component = component_find(package, key);
	const WCHAR *target = component ? directory_target(package, component->directory) : NULL;
	return target ? msi_copy(target) : NULL;
}

// What one bracket stands for; NULL when it has no value. *missing says the group around it should go.
static WCHAR *resolve(msi_package *package, msi_record *record, const WCHAR *inner, BOOL *missing)
{
	if (inner[0] == '\\' && inner[1])
		return msi_copy_n(inner + 1, 1);
	if (inner[0] == '~' && !inner[1])
		return msi_copy(L"");
	if (inner[0] == '%') {
		WCHAR buffer[4096];
		DWORD got = GetEnvironmentVariableW(inner + 1, buffer, ARRAYSIZE(buffer));
		return got && got < ARRAYSIZE(buffer) ? msi_copy(buffer) : NULL;
	}
	if (inner[0] == '#' || inner[0] == '!')
		return file_path(package, inner + 1);
	if (inner[0] == '$')
		return component_path(package, inner + 1);
	if (record && inner[0] >= '0' && inner[0] <= '9') {
		UINT field = 0;
		const WCHAR *at = inner;
		while (*at >= '0' && *at <= '9')
			field = field * 10 + (*at++ - '0');
		if (!*at) {
			WCHAR scratch[16];
			const WCHAR *text = field <= record->count ? record_text(record, field, scratch) : NULL;
			if (!text)
				*missing = TRUE;
			return msi_copy(text ? text : L"");
		}
	}
	WCHAR *value = package ? package_get(package, inner) : NULL;
	if (!value || !*value)
		*missing = TRUE;
	return value;
}

static void expand(msi_package *package, msi_record *record, const WCHAR *text, builder *out, BOOL *missing, int depth)
{
	for (const WCHAR *at = text; *at;) {
		if (*at == '{' && depth < 16) {
			int nesting = 1;
			const WCHAR *end = at + 1;
			while (*end && nesting) {
				if (*end == '{')
					nesting++;
				else if (*end == '}')
					nesting--;
				end++;
			}
			if (nesting) {
				put(out, at, 1);
				at++;
				continue;
			}
			// A group naming no property is plain text, braces and all, such as the class of a registry key.
			const WCHAR *bracket = at + 1;
			while (bracket < end && *bracket != '[')
				bracket++;
			if (bracket == end) {
				put(out, at, end - at);
				at = end;
				continue;
			}
			WCHAR *inner = msi_copy_n(at + 1, end - at - 2);
			builder group = {0};
			BOOL gone = FALSE;
			expand(package, record, inner, &group, &gone, depth + 1);
			if (!gone && group.text)
				put(out, group.text, group.length);
			msi_free(group.text);
			msi_free(inner);
			at = end;
		} else if (*at == '[' && depth < 16) {
			int nesting = 1;
			const WCHAR *end = at + 1;
			if (*end == '\\' && end[1] && end[2] == ']') {
				put(out, end + 1, 1);
				at = end + 3;
				continue;
			}
			while (*end && nesting) {
				if (*end == '[')
					nesting++;
				else if (*end == ']')
					nesting--;
				end++;
			}
			if (nesting) {
				put(out, at, 1);
				at++;
				continue;
			}
			WCHAR *inner = msi_copy_n(at + 1, end - at - 2);
			builder name = {0};
			BOOL ignored = FALSE;
			expand(package, record, inner, &name, &ignored, depth + 1);
			WCHAR *value = name.text ? resolve(package, record, name.text, missing) : NULL;
			if (value) {
				put(out, value, lstrlenW(value));
				msi_free(value);
			}
			msi_free(name.text);
			msi_free(inner);
			at = end;
		} else {
			const WCHAR *start = at++;
			while (*at && *at != '[' && *at != '{')
				at++;
			put(out, start, at - start);
		}
	}
}

WCHAR *format_text(msi_package *package, const WCHAR *text, msi_record *record)
{
	builder out = {0};
	BOOL missing = FALSE;
	put(&out, L"", 0);
	if (text)
		expand(package, record, text, &out, &missing, 0);
	return out.text;
}

// A record without a template lists its fields, as the documentation shows.
WCHAR *format_record(msi_package *package, msi_record *record)
{
	WCHAR scratch[16];
	const WCHAR *template_text = record_text(record, 0, scratch);
	if (template_text && *template_text)
		return format_text(package, template_text, record);
	builder out = {0};
	put(&out, L"", 0);
	for (UINT i = 1; i <= record->count; i++) {
		const WCHAR *field = record_text(record, i, scratch);
		WCHAR number[24];
		if (!field)
			continue;
		wsprintfW(number, L"%u: ", i);
		put(&out, number, lstrlenW(number));
		put(&out, field, lstrlenW(field));
		put(&out, L" ", 1);
	}
	return out.text;
}

UINT WINAPI MsiFormatRecordW(MSIHANDLE install, MSIHANDLE record_handle, LPWSTR out, LPDWORD size)
{
	msi_record *record = (msi_record *)handle_lookup(record_handle, KIND_RECORD);
	if (!record)
		return ERROR_INVALID_HANDLE;
	msi_package *package = install ? package_from_handle(install) : NULL;
	WCHAR *text = format_record(package, record);
	UINT status = out_text_w(text, lstrlenW(text), out, size);
	msi_free(text);
	if (package)
		object_unref(&package->base);
	object_unref(&record->base);
	return status;
}

UINT WINAPI MsiFormatRecordA(MSIHANDLE install, MSIHANDLE record_handle, LPSTR out, LPDWORD size)
{
	msi_record *record = (msi_record *)handle_lookup(record_handle, KIND_RECORD);
	if (!record)
		return ERROR_INVALID_HANDLE;
	msi_package *package = install ? package_from_handle(install) : NULL;
	WCHAR *text = format_record(package, record);
	UINT status = out_text_a(text, lstrlenW(text), out, size);
	msi_free(text);
	if (package)
		object_unref(&package->base);
	object_unref(&record->base);
	return status;
}
