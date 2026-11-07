/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

static void field_clear(record_field *field)
{
	msi_free(field->text);
	blob_unref(field->blob);
	memset(field, 0, sizeof(*field));
}

static void record_destroy(msi_object *object)
{
	msi_record *record = (msi_record *)object;
	for (UINT i = 0; i <= record->count; i++)
		field_clear(&record->fields[i]);
}

msi_record *record_new(UINT count)
{
	if (count >= 0x10000)
		return NULL;
	msi_record *record = msi_alloc(sizeof(*record) + count * sizeof(record_field));
	if (!record)
		return NULL;
	object_init(&record->base, KIND_RECORD, record_destroy);
	record->count = count;
	return record;
}

UINT record_set_int(msi_record *record, UINT field, INT value)
{
	if (field > record->count)
		return ERROR_INVALID_PARAMETER;
	field_clear(&record->fields[field]);
	if (value != MSI_NULL_INTEGER) {
		record->fields[field].type = FIELD_INT;
		record->fields[field].number = value;
	}
	return ERROR_SUCCESS;
}

UINT record_set_text(msi_record *record, UINT field, const WCHAR *text)
{
	if (field > record->count)
		return ERROR_INVALID_PARAMETER;
	field_clear(&record->fields[field]);
	if (text && *text) {
		record->fields[field].type = FIELD_TEXT;
		record->fields[field].text = msi_copy(text);
	}
	return ERROR_SUCCESS;
}

UINT record_set_blob(msi_record *record, UINT field, msi_blob *blob)
{
	if (field > record->count)
		return ERROR_INVALID_PARAMETER;
	field_clear(&record->fields[field]);
	record->fields[field].type = FIELD_BLOB;
	record->fields[field].blob = blob_ref(blob);
	return ERROR_SUCCESS;
}

BOOL record_is_null(const msi_record *record, UINT field)
{
	return field > record->count || record->fields[field].type == FIELD_NULL;
}

// The text of a number: decimal digits with nothing around them, wrapping as a 32-bit value does.
static BOOL text_to_int(const WCHAR *text, INT *value)
{
	BOOL negative = *text == L'-';
	UINT number = 0;
	if (negative)
		text++;
	if (!*text)
		return FALSE;
	for (; *text; text++) {
		if (*text < L'0' || *text > L'9')
			return FALSE;
		number = number * 10 + (*text - L'0');
	}
	*value = negative ? -(INT)number : (INT)number;
	return TRUE;
}

INT record_get_int(const msi_record *record, UINT field)
{
	INT value;
	if (field > record->count)
		return MSI_NULL_INTEGER;
	switch (record->fields[field].type) {
	case FIELD_INT:
		return record->fields[field].number;
	case FIELD_TEXT:
		return text_to_int(record->fields[field].text, &value) ? value : MSI_NULL_INTEGER;
	default:
		return MSI_NULL_INTEGER;
	}
}

const WCHAR *record_text(const msi_record *record, UINT field, WCHAR scratch[16])
{
	if (field > record->count)
		return NULL;
	switch (record->fields[field].type) {
	case FIELD_INT:
		wsprintfW(scratch, L"%d", record->fields[field].number);
		return scratch;
	case FIELD_TEXT:
		return record->fields[field].text;
	default:
		return NULL;
	}
}

void record_clear(msi_record *record)
{
	for (UINT i = 0; i <= record->count; i++)
		field_clear(&record->fields[i]);
}

msi_record *record_copy(const msi_record *record)
{
	msi_record *copy = record_new(record->count);
	if (!copy)
		return NULL;
	for (UINT i = 0; i <= record->count; i++) {
		const record_field *from = &record->fields[i];
		copy->fields[i].type = from->type;
		copy->fields[i].number = from->number;
		copy->fields[i].text = msi_copy(from->text);
		copy->fields[i].blob = blob_ref(from->blob);
		copy->fields[i].position = from->position;
	}
	return copy;
}

MSIHANDLE WINAPI MsiCreateRecord(UINT count)
{
	msi_record *record = record_new(count);
	if (!record)
		return 0;
	MSIHANDLE handle = handle_open(&record->base);
	object_unref(&record->base);
	return handle;
}

#define WITH_RECORD(failed) \
	msi_record *record = (msi_record *)handle_lookup(handle, KIND_RECORD); \
	if (!record) \
		return failed

UINT WINAPI MsiRecordGetFieldCount(MSIHANDLE handle)
{
	WITH_RECORD((UINT)-1);
	UINT count = record->count;
	object_unref(&record->base);
	return count;
}

BOOL WINAPI MsiRecordIsNull(MSIHANDLE handle, UINT field)
{
	WITH_RECORD(FALSE);
	BOOL null = record_is_null(record, field);
	object_unref(&record->base);
	return null;
}

int WINAPI MsiRecordGetInteger(MSIHANDLE handle, UINT field)
{
	WITH_RECORD(MSI_NULL_INTEGER);
	INT value = record_get_int(record, field);
	object_unref(&record->base);
	return value;
}

UINT WINAPI MsiRecordSetInteger(MSIHANDLE handle, UINT field, int value)
{
	WITH_RECORD(ERROR_INVALID_HANDLE);
	UINT status = record_set_int(record, field, value);
	object_unref(&record->base);
	return status;
}

UINT WINAPI MsiRecordSetStringW(MSIHANDLE handle, UINT field, LPCWSTR text)
{
	WITH_RECORD(ERROR_INVALID_HANDLE);
	UINT status = record_set_text(record, field, text);
	object_unref(&record->base);
	return status;
}

UINT WINAPI MsiRecordSetStringA(MSIHANDLE handle, UINT field, LPCSTR text)
{
	WCHAR *wide = msi_from_ansi(text);
	UINT status = MsiRecordSetStringW(handle, field, wide);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiRecordGetStringW(MSIHANDLE handle, UINT field, LPWSTR out, LPDWORD size)
{
	WITH_RECORD(ERROR_INVALID_HANDLE);
	WCHAR scratch[16];
	const WCHAR *text = record_text(record, field, scratch);
	UINT status = ERROR_SUCCESS;
	if (field <= record->count && record->fields[field].type == FIELD_BLOB)
		status = ERROR_INVALID_DATATYPE;
	else
		status = out_text_w(text ? text : L"", text ? lstrlenW(text) : 0, out, size);
	object_unref(&record->base);
	return status;
}

UINT WINAPI MsiRecordGetStringA(MSIHANDLE handle, UINT field, LPSTR out, LPDWORD size)
{
	WITH_RECORD(ERROR_INVALID_HANDLE);
	WCHAR scratch[16];
	const WCHAR *text = record_text(record, field, scratch);
	UINT status = ERROR_SUCCESS;
	if (field <= record->count && record->fields[field].type == FIELD_BLOB)
		status = ERROR_INVALID_DATATYPE;
	else
		status = out_text_a(text ? text : L"", text ? lstrlenW(text) : 0, out, size);
	object_unref(&record->base);
	return status;
}

UINT WINAPI MsiRecordDataSize(MSIHANDLE handle, UINT field)
{
	WITH_RECORD(0);
	UINT size = 0;
	if (field <= record->count) {
		const record_field *at = &record->fields[field];
		if (at->type == FIELD_INT)
			size = sizeof(INT);
		else if (at->type == FIELD_TEXT)
			size = lstrlenW(at->text);
		else if (at->type == FIELD_BLOB)
			size = at->blob ? (UINT)at->blob->size : 0;
	}
	object_unref(&record->base);
	return size;
}

UINT WINAPI MsiRecordClearData(MSIHANDLE handle)
{
	WITH_RECORD(ERROR_INVALID_HANDLE);
	record_clear(record);
	object_unref(&record->base);
	return ERROR_SUCCESS;
}

UINT WINAPI MsiRecordSetStreamW(MSIHANDLE handle, UINT field, LPCWSTR path)
{
	WITH_RECORD(ERROR_INVALID_HANDLE);
	UINT status = ERROR_SUCCESS;
	if (!field || field > record->count) {
		status = ERROR_INVALID_PARAMETER;
	} else if (!path) {
		// Without a file the stream already there starts over.
		if (record->fields[field].type == FIELD_BLOB)
			record->fields[field].position = 0;
		else
			status = ERROR_INVALID_PARAMETER;
	} else {
		msi_blob *blob = blob_from_file(path);
		if (blob) {
			record_set_blob(record, field, blob);
			blob_unref(blob);
		} else {
			status = GetLastError();
		}
	}
	object_unref(&record->base);
	return status;
}

UINT WINAPI MsiRecordSetStreamA(MSIHANDLE handle, UINT field, LPCSTR path)
{
	WCHAR *wide = msi_from_ansi(path);
	UINT status = MsiRecordSetStreamW(handle, field, wide);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiRecordReadStream(MSIHANDLE handle, UINT field, char *out, LPDWORD size)
{
	if (!size) {
		msi_object *known = handle_lookup(handle, KIND_RECORD);
		if (!known)
			return ERROR_INVALID_HANDLE;
		object_unref(known);
		return ERROR_INVALID_PARAMETER;
	}
	WITH_RECORD(ERROR_INVALID_HANDLE);
	UINT status = ERROR_SUCCESS;
	if (field > record->count) {
		status = ERROR_INVALID_PARAMETER;
	} else if (record->fields[field].type != FIELD_BLOB) {
		status = ERROR_INVALID_DATATYPE;
	} else if (!record->fields[field].blob) {
		// A nested storage shows up as a stream that has no bytes to give.
		*size = 0;
		status = ERROR_INVALID_DATA;
	} else {
		record_field *at = &record->fields[field];
		SIZE_T left = at->blob->size - at->position;
		if (!out) {
			*size = (DWORD)left;
		} else {
			DWORD count = *size < left ? *size : (DWORD)left;
			memcpy(out, at->blob->data + at->position, count);
			at->position += count;
			*size = count;
		}
	}
	object_unref(&record->base);
	return status;
}
