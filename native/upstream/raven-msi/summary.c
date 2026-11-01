/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

// The summary stream is an OLE property set with one section, per the public property set format [MS-OLEPS].
#define SUMMARY_STREAM L"\005SummaryInformation"
#define MAX_PROPERTY 20

static const GUID format_summary = {0xf29f85e0, 0x4ff9, 0x1068, {0xab, 0x91, 0x08, 0x00, 0x2b, 0x27, 0xb3, 0xd9}};

typedef struct {
	USHORT type;
	INT number;
	FILETIME time;
	char *text;
} summary_value;

typedef struct {
	msi_object base;
	msi_db *db;
	IStorage *storage;
	UINT updates;
	summary_value values[MAX_PROPERTY];
} msi_summary;

static void summary_destroy(msi_object *object)
{
	msi_summary *summary = (msi_summary *)object;
	for (UINT i = 0; i < MAX_PROPERTY; i++)
		msi_free(summary->values[i].text);
	if (summary->db)
		object_unref(&summary->db->base);
	if (summary->storage)
		IStorage_Release(summary->storage);
}

static UINT read_u32(const BYTE *at)
{
	return at[0] | (at[1] << 8) | (at[2] << 16) | ((UINT)at[3] << 24);
}

static void parse_property_set(msi_summary *summary, const BYTE *data, SIZE_T size)
{
	if (size < 48 || data[0] != 0xfe || data[1] != 0xff)
		return;
	UINT section = read_u32(data + 44);
	if (section + 8 > size || memcmp(data + 28, &format_summary, sizeof(GUID)))
		return;
	const BYTE *base = data + section;
	UINT count = read_u32(base + 4);
	if (count > (size - section - 8) / 8)
		return;
	for (UINT i = 0; i < count; i++) {
		UINT id = read_u32(base + 8 + i * 8), offset = read_u32(base + 12 + i * 8);
		if (id >= MAX_PROPERTY || (SIZE_T)section + offset + 8 > size)
			continue;
		const BYTE *value = base + offset;
		summary_value *out = &summary->values[id];
		out->type = (USHORT)read_u32(value);
		switch (out->type) {
		case VT_I2:
			out->number = (SHORT)(value[4] | (value[5] << 8));
			break;
		case VT_I4:
			out->number = (INT)read_u32(value + 4);
			break;
		case VT_FILETIME:
			out->time.dwLowDateTime = read_u32(value + 4);
			out->time.dwHighDateTime = read_u32(value + 8);
			break;
		case VT_LPSTR: {
			UINT length = read_u32(value + 4);
			if ((SIZE_T)section + offset + 8 + length > size)
				break;
			out->text = msi_alloc(length + 1);
			memcpy(out->text, value + 8, length);
			break;
		}
		default:
			out->type = VT_EMPTY;
		}
	}
}

static void put_u32(BYTE **at, UINT value)
{
	(*at)[0] = (BYTE)value;
	(*at)[1] = (BYTE)(value >> 8);
	(*at)[2] = (BYTE)(value >> 16);
	(*at)[3] = (BYTE)(value >> 24);
	*at += 4;
}

static msi_blob *build_property_set(const msi_summary *summary)
{
	UINT count = 0;
	SIZE_T room = 48 + 8;
	for (UINT i = 0; i < MAX_PROPERTY; i++) {
		const summary_value *value = &summary->values[i];
		if (value->type == VT_EMPTY)
			continue;
		count++;
		room += 8 + 16;
		if (value->type == VT_LPSTR)
			room += lstrlenA(value->text) + 5;
	}
	msi_blob *blob = blob_new(NULL, room);
	BYTE *data = blob->data, *at = data;
	put_u32(&at, 0x0000fffe);	// byte order and format version
	put_u32(&at, 0x00020005);	// the system that wrote it
	memset(at, 0, 16);
	at += 16;
	put_u32(&at, 1);
	memcpy(at, &format_summary, sizeof(GUID));
	at += sizeof(GUID);
	put_u32(&at, 48);
	BYTE *section = at;
	at += 4;
	put_u32(&at, count);
	BYTE *index = at;
	at += count * 8;
	for (UINT i = 0; i < MAX_PROPERTY; i++) {
		const summary_value *value = &summary->values[i];
		if (value->type == VT_EMPTY)
			continue;
		put_u32(&index, i);
		put_u32(&index, (UINT)(at - section));
		put_u32(&at, value->type);
		switch (value->type) {
		case VT_I2:
		case VT_I4:
			put_u32(&at, (UINT)value->number);
			break;
		case VT_FILETIME:
			put_u32(&at, value->time.dwLowDateTime);
			put_u32(&at, value->time.dwHighDateTime);
			break;
		case VT_LPSTR: {
			UINT length = lstrlenA(value->text) + 1;
			put_u32(&at, length);
			memcpy(at, value->text, length);
			at += (length + 3) & ~3u;
			break;
		}
		}
	}
	BYTE *size = section;
	put_u32(&size, (UINT)(at - section));
	blob->size = at - data;
	return blob;
}

static msi_summary *summary_new(UINT updates)
{
	msi_summary *summary = msi_alloc(sizeof(*summary));
	object_init(&summary->base, KIND_SUMMARY, summary_destroy);
	summary->updates = updates;
	return summary;
}

UINT summary_open_database(msi_db *db, UINT updates, MSIHANDLE *handle)
{
	msi_summary *summary = summary_new(updates);
	object_ref(&db->base);
	summary->db = db;
	msi_blob *blob = db_read_named_stream(db, SUMMARY_STREAM);
	if (blob) {
		parse_property_set(summary, blob->data, blob->size);
		blob_unref(blob);
	}
	*handle = handle_open(&summary->base);
	object_unref(&summary->base);
	return *handle ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
}

UINT WINAPI MsiGetSummaryInformationW(MSIHANDLE database, LPCWSTR path, UINT updates, MSIHANDLE *handle)
{
	if (!handle)
		return ERROR_INVALID_PARAMETER;
	if (database) {
		msi_db *db = (msi_db *)handle_lookup(database, KIND_DATABASE);
		if (!db)
			return ERROR_INVALID_HANDLE;
		UINT status = summary_open_database(db, updates, handle);
		object_unref(&db->base);
		return status;
	}
	if (!path || !*path)
		return ERROR_INSTALL_PACKAGE_INVALID;
	IStorage *storage = NULL;
	DWORD mode = STGM_DIRECT | STGM_SHARE_DENY_WRITE | (updates ? STGM_READWRITE : STGM_READ);
	if (FAILED(StgOpenStorage(path, NULL, updates ? STGM_DIRECT | STGM_READWRITE | STGM_SHARE_EXCLUSIVE : mode, NULL, 0, &storage)))
		return ERROR_INSTALL_PACKAGE_OPEN_FAILED;
	msi_summary *summary = summary_new(updates);
	summary->storage = storage;
	IStream *stream = NULL;
	WCHAR encoded[64];
	name_encode(SUMMARY_STREAM, FALSE, encoded);
	if (SUCCEEDED(IStorage_OpenStream(storage, encoded, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream))) {
		STATSTG stat;
		if (SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME))) {
			BYTE *data = msi_alloc(stat.cbSize.LowPart);
			ULONG done = 0;
			if (SUCCEEDED(IStream_Read(stream, data, stat.cbSize.LowPart, &done)))
				parse_property_set(summary, data, done);
			msi_free(data);
		}
		IStream_Release(stream);
	}
	*handle = handle_open(&summary->base);
	object_unref(&summary->base);
	return *handle ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
}

UINT WINAPI MsiGetSummaryInformationA(MSIHANDLE database, LPCSTR path, UINT updates, MSIHANDLE *handle)
{
	WCHAR *wide = msi_from_ansi(path);
	UINT status = MsiGetSummaryInformationW(database, wide, updates, handle);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiSummaryInfoGetPropertyCount(MSIHANDLE handle, PUINT count)
{
	msi_summary *summary = (msi_summary *)handle_lookup(handle, KIND_SUMMARY);
	if (!summary)
		return ERROR_INVALID_HANDLE;
	if (count) {
		*count = 0;
		for (UINT i = 0; i < MAX_PROPERTY; i++)
			if (summary->values[i].type != VT_EMPTY)
				(*count)++;
	}
	object_unref(&summary->base);
	return ERROR_SUCCESS;
}

static UINT summary_get(MSIHANDLE handle, UINT property, PUINT type, INT *number, FILETIME *time, char *out_a, WCHAR *out_w, LPDWORD size)
{
	if (property >= MAX_PROPERTY)
		return ERROR_UNKNOWN_PROPERTY;
	msi_summary *summary = (msi_summary *)handle_lookup(handle, KIND_SUMMARY);
	if (!summary)
		return ERROR_INVALID_HANDLE;
	const summary_value *value = &summary->values[property];
	UINT status = ERROR_SUCCESS;
	if (type)
		*type = value->type;
	switch (value->type) {
	case VT_I2:
	case VT_I4:
		if (number)
			*number = value->number;
		break;
	case VT_FILETIME:
		if (time)
			*time = value->time;
		break;
	case VT_LPSTR: {
		WCHAR *wide = msi_from_ansi(value->text);
		if (out_w || !out_a) {
			if (out_w || size)
				status = out_text_w(wide, lstrlenW(wide), out_w, size);
		} else {
			status = out_text_a(wide, lstrlenW(wide), out_a, size);
		}
		msi_free(wide);
		break;
	}
	}
	object_unref(&summary->base);
	return status;
}

UINT WINAPI MsiSummaryInfoGetPropertyA(MSIHANDLE handle, UINT property, PUINT type, INT *number, FILETIME *time, LPSTR out, LPDWORD size)
{
	return summary_get(handle, property, type, number, time, out, NULL, size);
}

UINT WINAPI MsiSummaryInfoGetPropertyW(MSIHANDLE handle, UINT property, PUINT type, INT *number, FILETIME *time, LPWSTR out, LPDWORD size)
{
	return summary_get(handle, property, type, number, time, NULL, out, size);
}

static USHORT property_type(UINT property)
{
	switch (property) {
	case 1:
		return VT_I2;
	case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 18:
		return VT_LPSTR;
	case 10: case 11: case 12: case 13:
		return VT_FILETIME;
	case 14: case 15: case 16: case 19:
		return VT_I4;
	default:
		return VT_EMPTY;
	}
}

static UINT summary_set(MSIHANDLE handle, UINT property, UINT type, INT number, FILETIME *time, const WCHAR *text, BOOL have_text)
{
	msi_summary *summary = (msi_summary *)handle_lookup(handle, KIND_SUMMARY);
	if (!summary)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_SUCCESS;
	USHORT expected = property < MAX_PROPERTY ? property_type(property) : VT_EMPTY;
	if (property >= MAX_PROPERTY || expected == VT_EMPTY) {
		status = property >= MAX_PROPERTY ? ERROR_UNKNOWN_PROPERTY : ERROR_DATATYPE_MISMATCH;
	} else if (type != expected) {
		status = ERROR_DATATYPE_MISMATCH;
	} else if ((type == VT_LPSTR && !have_text) || (type == VT_FILETIME && !time)) {
		status = ERROR_INVALID_PARAMETER;
	} else if (!summary->updates) {
		status = ERROR_FUNCTION_FAILED;
	} else {
		UINT used = 0;
		summary_value *value = &summary->values[property];
		for (UINT i = 0; i < MAX_PROPERTY; i++)
			if (summary->values[i].type != VT_EMPTY)
				used++;
		if (value->type == VT_EMPTY && used >= summary->updates) {
			status = ERROR_FUNCTION_FAILED;
		} else {
			msi_free(value->text);
			value->text = NULL;
			value->type = (USHORT)type;
			if (type == VT_LPSTR)
				value->text = msi_to_ansi(text);
			else if (type == VT_FILETIME)
				value->time = *time;
			else
				value->number = number;
		}
	}
	object_unref(&summary->base);
	return status;
}

UINT WINAPI MsiSummaryInfoSetPropertyW(MSIHANDLE handle, UINT property, UINT type, INT number, FILETIME *time, LPCWSTR text)
{
	return summary_set(handle, property, type, number, time, text, text != NULL);
}

UINT WINAPI MsiSummaryInfoSetPropertyA(MSIHANDLE handle, UINT property, UINT type, INT number, FILETIME *time, LPCSTR text)
{
	WCHAR *wide = msi_from_ansi(text);
	UINT status = summary_set(handle, property, type, number, time, wide, text != NULL);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiSummaryInfoPersist(MSIHANDLE handle)
{
	msi_summary *summary = (msi_summary *)handle_lookup(handle, KIND_SUMMARY);
	if (!summary)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_SUCCESS;
	if (!summary->updates) {
		status = ERROR_FUNCTION_FAILED;
	} else {
		msi_blob *blob = build_property_set(summary);
		if (summary->db) {
			stream_put(summary->db, SUMMARY_STREAM, blob);
		} else {
			IStream *stream = NULL;
			WCHAR encoded[64];
			ULONG done;
			name_encode(SUMMARY_STREAM, FALSE, encoded);
			if (FAILED(IStorage_CreateStream(summary->storage, encoded, STGM_WRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE, 0, 0, &stream))) {
				status = ERROR_FUNCTION_FAILED;
			} else {
				IStream_Write(stream, blob->data, (ULONG)blob->size, &done);
				IStream_Release(stream);
				IStorage_Commit(summary->storage, 0);
			}
		}
		blob_unref(blob);
	}
	object_unref(&summary->base);
	return status;
}
