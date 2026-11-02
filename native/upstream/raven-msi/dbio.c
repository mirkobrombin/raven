/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

// Tables as text files, transforms and merging, the ways a database takes in or hands out whole sets of rows.
static WCHAR *join_path(const WCHAR *folder, const WCHAR *file)
{
	WCHAR *path = msi_alloc((lstrlenW(folder) + lstrlenW(file) + 2) * sizeof(WCHAR));
	lstrcpyW(path, folder);
	if (*path && path[lstrlenW(path) - 1] != L'\\')
		lstrcatW(path, L"\\");
	lstrcatW(path, file);
	return path;
}

static WCHAR *narrow_to_wide(const char *data, INT size, UINT codepage)
{
	INT length = size ? MultiByteToWideChar(codepage, 0, data, size, NULL, 0) : 0;
	WCHAR *wide = msi_alloc((length + 1) * sizeof(WCHAR));
	if (length)
		MultiByteToWideChar(codepage, 0, data, size, wide, length);
	return wide;
}

static BOOL parse_type_text(const WCHAR *text, UINT *type)
{
	WCHAR letter = *text;
	UINT size = 0, bits = TYPE_VALID;
	if (!letter)
		return FALSE;
	if (letter >= L'A' && letter <= L'Z') {
		bits |= TYPE_NULLABLE;
		letter += L'a' - L'A';
	}
	const WCHAR *digits = text + 1;
	if (!*digits)
		return FALSE;
	for (; *digits; digits++) {
		if (*digits < L'0' || *digits > L'9')
			return FALSE;
		size = size * 10 + (*digits - L'0');
	}
	switch (letter) {
	case L'i':
		bits |= size <= 2 ? TYPE_NOT_BINARY | 2 : 4;
		break;
	case L'j':
		bits |= TYPE_TEMPORARY | (size <= 2 ? TYPE_NOT_BINARY | 2 : 4);
		break;
	case L'l':
		bits |= TYPE_LOCALIZABLE | TYPE_STRING | TYPE_NOT_BINARY | size;
		break;
	case L's':
		bits |= TYPE_STRING | TYPE_NOT_BINARY | size;
		break;
	case L'g':
		bits |= TYPE_TEMPORARY | TYPE_STRING | TYPE_NOT_BINARY | size;
		break;
	case L'v':
		bits |= TYPE_STRING;
		break;
	default:
		return FALSE;
	}
	*type = bits;
	return TRUE;
}

// Splits at tabs in place; a line's fields stay valid while the text does.
static UINT split_tabs(WCHAR *line, WCHAR **fields, UINT room)
{
	UINT count = 0;
	WCHAR *start = line;
	for (;;) {
		WCHAR *tab = wcschr(start, L'\t');
		if (count < room)
			fields[count] = start;
		count++;
		if (!tab)
			break;
		*tab = 0;
		start = tab + 1;
	}
	return count;
}

static WCHAR *next_line(WCHAR **at)
{
	WCHAR *start = *at;
	if (!start || !*start)
		return NULL;
	WCHAR *end = start;
	while (*end && *end != L'\n')
		end++;
	*at = *end ? end + 1 : end;
	if (end > start && end[-1] == L'\r')
		end[-1] = 0;
	*end = 0;
	return start;
}

// In a text table a tab, a line feed and a carriage return each travel as one control character.
static void unescape_field(WCHAR *field)
{
	WCHAR *to = field;
	for (; *field; field++) {
		if (*field == L'\r')
			continue;
		if (*field == 0x11)
			*to++ = L'\t';
		else if (*field == 0x19)
			*to++ = L'\n';
		else if (*field == 0x1b)
			*to++ = L'\r';
		else
			*to++ = *field;
	}
	*to = 0;
}

static BOOL parse_filetime(const WCHAR *text, FILETIME *time)
{
	SYSTEMTIME st = {0};
	UINT parts[6] = {0}, index = 0;
	for (; *text && index < 6; text++) {
		if (*text >= L'0' && *text <= L'9')
			parts[index] = parts[index] * 10 + (*text - L'0');
		else
			index++;
	}
	st.wYear = (WORD)parts[0];
	st.wMonth = (WORD)parts[1];
	st.wDay = (WORD)parts[2];
	st.wHour = (WORD)parts[3];
	st.wMinute = (WORD)parts[4];
	st.wSecond = (WORD)parts[5];
	return SystemTimeToFileTime(&st, time);
}

static UINT import_summary(MSIHANDLE database, WCHAR *rows)
{
	MSIHANDLE summary;
	UINT status = MsiGetSummaryInformationW(database, NULL, 20, &summary);
	if (status)
		return status;
	WCHAR *line;
	while (!status && (line = next_line(&rows))) {
		WCHAR *fields[2];
		if (!*line)
			continue;
		if (split_tabs(line, fields, 2) != 2) {
			status = ERROR_FUNCTION_FAILED;
			break;
		}
		unescape_field(fields[1]);
		UINT id = (UINT)wcstol(fields[0], NULL, 10);
		switch (id) {
		case 1:
			status = MsiSummaryInfoSetPropertyW(summary, id, VT_I2, wcstol(fields[1], NULL, 10), NULL, NULL);
			break;
		case 10: case 11: case 12: case 13: {
			FILETIME time = {0};
			parse_filetime(fields[1], &time);
			status = MsiSummaryInfoSetPropertyW(summary, id, VT_FILETIME, 0, &time, NULL);
			break;
		}
		case 14: case 15: case 16: case 19:
			status = MsiSummaryInfoSetPropertyW(summary, id, VT_I4, wcstol(fields[1], NULL, 10), NULL, NULL);
			break;
		default:
			status = MsiSummaryInfoSetPropertyW(summary, id, VT_LPSTR, 0, NULL, fields[1]);
		}
	}
	if (!status)
		status = MsiSummaryInfoPersist(summary);
	MsiCloseHandle(summary);
	return status;
}

static UINT import_file(MSIHANDLE database, msi_db *db, const WCHAR *folder, const WCHAR *file)
{
	WCHAR *path = join_path(folder, file);
	msi_blob *blob = blob_from_file(path);
	msi_free(path);
	if (!blob)
		return ERROR_FUNCTION_FAILED;
	WCHAR *text = narrow_to_wide((char *)blob->data, (INT)blob->size, CP_ACP), *at = text;
	blob_unref(blob);
	UINT status = ERROR_FUNCTION_FAILED;
	WCHAR *names[64], *types[64], *table_line[64];
	WCHAR *first = next_line(&at), *second = next_line(&at), *third = next_line(&at);
	if (!first || !second)
		goto done;
	// A code page line stands alone: blank first and second lines and the pair on the third.
	if (third && !*first && !*second) {
		UINT parts = split_tabs(third, table_line, 2);
		if (parts == 2 && !lstrcmpW(table_line[1], L"_ForceCodepage")) {
			INT codepage = wcstol(table_line[0], NULL, 10);
			if (codepage && codepage != CP_UTF7 && !IsValidCodePage(codepage)) {
				status = ERROR_FUNCTION_FAILED;
			} else {
				db->codepage = codepage;
				db->dirty = TRUE;
				status = ERROR_SUCCESS;
			}
		}
		goto done;
	}
	if (!third)
		goto done;
	UINT count = split_tabs(first, names, 64);
	if (split_tabs(second, types, 64) != count)
		goto done;
	UINT keys = split_tabs(third, table_line, 64);
	const WCHAR *name = table_line[0];
	if (!*name || keys < 2)
		goto done;
	if (!lstrcmpW(name, L"_SummaryInformation")) {
		status = import_summary(database, at);
		goto done;
	}
	db_table *table = db_find_table(db, name);
	if (!table) {
		db_column columns[64];
		for (UINT i = 0; i < count; i++) {
			columns[i].name = names[i];
			if (!parse_type_text(types[i], &columns[i].type))
				goto done;
			for (UINT k = 1; k < keys; k++)
				if (!lstrcmpW(table_line[k], names[i]))
					columns[i].type |= TYPE_KEY;
		}
		if (db_create_table(db, name, columns, count, FALSE, &table))
			goto done;
	}
	db_load_table(db, table);
	status = ERROR_SUCCESS;
	WCHAR *line;
	UINT *cells = msi_alloc((table->column_count + 1) * sizeof(UINT));
	while (!status && (line = next_line(&at))) {
		WCHAR *fields[64];
		if (!*line)
			continue;
		UINT got = split_tabs(line, fields, 64);
		if (got != count) {
			status = ERROR_FUNCTION_FAILED;
			break;
		}
		memset(cells, 0, table->column_count * sizeof(UINT));
		for (UINT c = 0; c < count && c < table->column_count; c++) {
			UINT type = table->columns[c].type;
			unescape_field(fields[c]);
			if (!*fields[c])
				continue;
			if (TYPE_IS_BINARY(type))
				cells[c] = 1;
			else if (type & TYPE_STRING)
				cells[c] = pool_add(db, fields[c], lstrlenW(fields[c]));
			else
				cells[c] = cell_from_int(type, wcstol(fields[c], NULL, 10));
		}
		// Replacing a row keeps a repeated import from failing on its keys.
		db_row *existing = table_find_key(db, table, cells);
		if (existing)
			memcpy(existing->cells, cells, table->column_count * sizeof(UINT));
		else
			table_insert(db, table, cells, FALSE);
		for (UINT c = 0; c < count && c < table->column_count; c++)
			if (TYPE_IS_BINARY(table->columns[c].type) && cells[c]) {
				// Its bytes sit in a folder named after the text file, in a file the row names.
				WCHAR *base = msi_copy(file), *dot = wcsrchr(base, L'.');
				if (dot)
					*dot = 0;
				WCHAR *inner = join_path(base, fields[c]);
				msi_free(base);
				WCHAR *full = join_path(folder, inner);
				msi_blob *data = blob_from_file(full);
				msi_free(inner);
				msi_free(full);
				if (data) {
					WCHAR *stream = row_stream_name(db, table, cells, c);
					stream_put(db, stream, data);
					msi_free(stream);
					blob_unref(data);
				} else {
					status = ERROR_FUNCTION_FAILED;
				}
			}
	}
	msi_free(cells);
done:
	msi_free(text);
	return status;
}

UINT WINAPI MsiDatabaseImportW(MSIHANDLE handle, LPCWSTR folder, LPCWSTR file)
{
	if (!folder || !file)
		return ERROR_INVALID_PARAMETER;
	msi_db *db = (msi_db *)handle_lookup(handle, KIND_DATABASE);
	if (!db)
		return ERROR_INVALID_HANDLE;
	UINT status = import_file(handle, db, folder, file);
	object_unref(&db->base);
	return status;
}

UINT WINAPI MsiDatabaseImportA(MSIHANDLE handle, LPCSTR folder, LPCSTR file)
{
	WCHAR *wide_folder = msi_from_ansi(folder), *wide_file = msi_from_ansi(file);
	UINT status = MsiDatabaseImportW(handle, wide_folder, wide_file);
	msi_free(wide_folder);
	msi_free(wide_file);
	return status;
}

static void write_text(HANDLE file, const WCHAR *text, UINT codepage)
{
	INT size = WideCharToMultiByte(codepage, 0, text, -1, NULL, 0, NULL, NULL);
	char *narrow = msi_alloc(size);
	DWORD done;
	WideCharToMultiByte(codepage, 0, text, -1, narrow, size, NULL, NULL);
	if (size > 1)
		WriteFile(file, narrow, size - 1, &done, NULL);
	msi_free(narrow);
}

static void write_field(HANDLE file, const WCHAR *text, UINT codepage)
{
	WCHAR *copy = msi_copy(text);
	for (WCHAR *at = copy; *at; at++) {
		if (*at == L'\t')
			*at = 0x11;
		else if (*at == L'\n')
			*at = 0x19;
		else if (*at == L'\r')
			*at = 0x1b;
	}
	write_text(file, copy, codepage);
	msi_free(copy);
}

static void column_type_for_export(UINT type, WCHAR *out)
{
	WCHAR letter;
	BOOL nullable = (type & TYPE_NULLABLE) != 0;
	if (type & TYPE_TEMPORARY)
		letter = (type & TYPE_STRING) ? L'g' : L'j';
	else if (TYPE_IS_BINARY(type))
		letter = L'v';
	else if (type & TYPE_LOCALIZABLE)
		letter = L'l';
	else if (type & TYPE_STRING)
		letter = L's';
	else
		letter = L'i';
	if (nullable)
		letter -= L'a' - L'A';
	wsprintfW(out, L"%c%u", letter, type & TYPE_SIZE);
}

static UINT export_table(msi_db *db, const WCHAR *name, const WCHAR *folder, const WCHAR *file_name)
{
	WCHAR *path = join_path(folder, file_name);
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	msi_free(path);
	if (file == INVALID_HANDLE_VALUE)
		return ERROR_FUNCTION_FAILED;
	UINT codepage = db->codepage ? db->codepage : CP_ACP, status = ERROR_SUCCESS;
	if (!lstrcmpW(name, L"_ForceCodepage")) {
		WCHAR line[64];
		wsprintfW(line, L"\r\n\r\n%u\t_ForceCodepage\r\n", db->codepage);
		write_text(file, line, CP_ACP);
		CloseHandle(file);
		return ERROR_SUCCESS;
	}
	if (name[0] == L'_')
		db_refresh_virtual(db);
	db_table *table = db_find_table(db, name);
	if (!table) {
		CloseHandle(file);
		return ERROR_FUNCTION_FAILED;
	}
	db_load_table(db, table);
	for (UINT c = 0; c < table->column_count; c++) {
		if (c)
			write_text(file, L"\t", codepage);
		write_text(file, table->columns[c].name, codepage);
	}
	write_text(file, L"\r\n", codepage);
	for (UINT c = 0; c < table->column_count; c++) {
		WCHAR type[16];
		column_type_for_export(table->columns[c].type, type);
		if (c)
			write_text(file, L"\t", codepage);
		write_text(file, type, codepage);
	}
	write_text(file, L"\r\n", codepage);
	write_text(file, table->name, codepage);
	for (UINT c = 0; c < table->column_count; c++)
		if (table->columns[c].type & TYPE_KEY) {
			write_text(file, L"\t", codepage);
			write_text(file, table->columns[c].name, codepage);
		}
	write_text(file, L"\r\n", codepage);
	for (UINT r = 0; r < table->row_count; r++) {
		const UINT *cells = table->rows[r]->cells;
		for (UINT c = 0; c < table->column_count; c++) {
			UINT type = table->columns[c].type;
			if (c)
				write_text(file, L"\t", codepage);
			if (!cells[c])
				continue;
			if (TYPE_IS_BINARY(type)) {
				WCHAR *stream = row_stream_name(db, table, cells, c);
				WCHAR *base = wcschr(stream, L'.');
				WCHAR *name_part = msi_alloc((lstrlenW(stream) + 8) * sizeof(WCHAR));
				lstrcpyW(name_part, base ? base + 1 : stream);
				lstrcatW(name_part, L".ibd");
				WCHAR *directory = join_path(folder, table->name);
				CreateDirectoryW(directory, NULL);
				WCHAR *target = join_path(directory, name_part);
				msi_blob *blob = db_read_named_stream(db, stream);
				HANDLE out = CreateFileW(target, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
				if (blob && out != INVALID_HANDLE_VALUE) {
					DWORD done;
					WriteFile(out, blob->data, (DWORD)blob->size, &done, NULL);
				}
				if (out != INVALID_HANDLE_VALUE)
					CloseHandle(out);
				blob_unref(blob);
				write_field(file, name_part, codepage);
				msi_free(stream);
				msi_free(name_part);
				msi_free(directory);
				msi_free(target);
			} else if (type & TYPE_STRING) {
				write_field(file, pool_text(db, cells[c]), codepage);
			} else {
				WCHAR digits[16];
				wsprintfW(digits, L"%d", cell_to_int(type, cells[c]));
				write_text(file, digits, codepage);
			}
		}
		write_text(file, L"\r\n", codepage);
	}
	CloseHandle(file);
	return status;
}

UINT WINAPI MsiDatabaseExportW(MSIHANDLE handle, LPCWSTR table, LPCWSTR folder, LPCWSTR file)
{
	if (!table || !folder || !file)
		return ERROR_INVALID_PARAMETER;
	msi_db *db = (msi_db *)handle_lookup(handle, KIND_DATABASE);
	if (!db)
		return ERROR_INVALID_HANDLE;
	UINT status = export_table(db, table, folder, file);
	object_unref(&db->base);
	return status;
}

UINT WINAPI MsiDatabaseExportA(MSIHANDLE handle, LPCSTR table, LPCSTR folder, LPCSTR file)
{
	WCHAR *a = msi_from_ansi(table), *b = msi_from_ansi(folder), *c = msi_from_ansi(file);
	UINT status = MsiDatabaseExportW(handle, a, b, c);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	return status;
}

typedef struct {
	msi_db *db;
	msi_db *strings;
	IStorage *storage;
	UINT ref_size;
} transform_source;

static UINT raw_value(const BYTE *at, UINT bytes)
{
	UINT value = at[0] | (at[1] << 8);
	if (bytes > 2)
		value |= at[2] << 16;
	if (bytes > 3)
		value |= (UINT)at[3] << 24;
	return value;
}

static UINT file_bytes(UINT type, UINT ref_size)
{
	if (TYPE_IS_BINARY(type))
		return 2;
	if (type & TYPE_STRING)
		return ref_size;
	return (type & TYPE_SIZE) <= 2 ? 2 : 4;
}

static BYTE *read_transform_table(IStorage *storage, const WCHAR *name, ULONG *size)
{
	WCHAR encoded[64];
	IStream *stream = NULL;
	STATSTG stat;
	BYTE *data = NULL;
	name_encode(name, TRUE, encoded);
	if (FAILED(IStorage_OpenStream(storage, encoded, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream)))
		return NULL;
	if (SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME))) {
		ULONG done = 0;
		data = msi_alloc(stat.cbSize.LowPart);
		IStream_Read(stream, data, stat.cbSize.LowPart, &done);
		*size = done;
	}
	IStream_Release(stream);
	return data;
}

static void transform_stream(transform_source *source, db_table *table, const UINT *cells, UINT column)
{
	WCHAR *name = row_stream_name(source->db, table, cells, column);
	WCHAR encoded[128];
	IStream *stream = NULL;
	if (lstrlenW(name) < 62)
		name_encode(name, FALSE, encoded);
	if (lstrlenW(name) < 62 && SUCCEEDED(IStorage_OpenStream(source->storage, encoded, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream))) {
		STATSTG stat;
		if (SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME))) {
			msi_blob *blob = blob_new(NULL, stat.cbSize.LowPart);
			ULONG done;
			IStream_Read(stream, blob->data, stat.cbSize.LowPart, &done);
			stream_put(source->db, name, blob);
			blob_unref(blob);
		}
		IStream_Release(stream);
	}
	msi_free(name);
}

// Each entry starts with a mask: odd means the leading columns follow, its high byte counting them; even means
// the keys and the columns whose bits are set; nothing but keys means the row goes away.
static UINT apply_table(transform_source *source, const WCHAR *name, const BYTE *data, ULONG size)
{
	msi_db *db = source->db;
	db_table *table = db_find_table(db, name);
	if (!table)
		return ERROR_SUCCESS;
	db_load_table(db, table);
	UINT columns = table->column_count;
	UINT *cells = msi_alloc((columns + 1) * sizeof(UINT));
	BOOL *present = msi_alloc((columns + 1) * sizeof(BOOL));
	for (ULONG at = 0; at + 2 <= size; ) {
		UINT mask = data[at] | (data[at + 1] << 8);
		ULONG cursor = at + 2;
		UINT count = mask & 1 ? mask >> 8 : columns;
		if (count > columns)
			break;
		BOOL truncated = FALSE;
		memset(cells, 0, columns * sizeof(UINT));
		memset(present, 0, columns * sizeof(BOOL));
		for (UINT c = 0; c < count; c++) {
			UINT type = table->columns[c].type;
			if (!(mask & 1) && !(type & TYPE_KEY) && !(mask & (1u << c)))
				continue;
			UINT bytes = file_bytes(type, source->ref_size);
			if (cursor + bytes > size) {
				truncated = TRUE;
				break;
			}
			UINT value = raw_value(data + cursor, bytes);
			cursor += bytes;
			if ((type & TYPE_STRING) && !TYPE_IS_BINARY(type)) {
				const WCHAR *text = pool_text(source->strings, value);
				value = text ? pool_add(db, text, lstrlenW(text)) : 0;
			}
			cells[c] = value;
			present[c] = TRUE;
		}
		if (truncated)
			break;
		db_row *row = table_find_key(db, table, cells);
		if (row && !mask) {
			table_delete(db, table, row);
		} else if (row) {
			for (UINT c = 0; c < columns; c++)
				if (present[c])
					row->cells[c] = cells[c];
			db->dirty = TRUE;
		} else if (mask) {
			table_insert(db, table, cells, FALSE);
		}
		for (UINT c = 0; c < columns && mask; c++)
			if (TYPE_IS_BINARY(table->columns[c].type) && present[c] && cells[c])
				transform_stream(source, table, cells, c);
		at = cursor;
	}
	msi_free(cells);
	msi_free(present);
	return ERROR_SUCCESS;
}

static void apply_tables_catalog(transform_source *source, const BYTE *data, ULONG size)
{
	UINT ref = source->ref_size;
	for (ULONG at = 0; at + 2 + ref <= size; at += 2 + ref) {
		UINT mask = data[at] | (data[at + 1] << 8);
		const WCHAR *name = pool_text(source->strings, raw_value(data + at + 2, ref));
		if (!name)
			continue;
		db_table *existing = db_find_table(source->db, name);
		if (!mask && existing)
			db_drop_table(source->db, existing);
		else if (mask && !existing)
			db_create_table(source->db, name, NULL, 0, FALSE, NULL);
	}
}

static void apply_columns_catalog(transform_source *source, const BYTE *data, ULONG size)
{
	UINT ref = source->ref_size;
	WCHAR *last = NULL;
	UINT counter = 0;
	for (ULONG at = 0; at + 2 <= size; ) {
		UINT mask = data[at] | (data[at + 1] << 8);
		ULONG cursor = at + 2;
		UINT table_id = raw_value(data + cursor, ref);
		cursor += ref;
		UINT number = raw_value(data + cursor, 2);
		cursor += 2;
		const WCHAR *table_name = pool_text(source->strings, table_id);
		if (!mask || !table_name) {
			at = cursor;
			continue;
		}
		UINT name_id = raw_value(data + cursor, ref);
		cursor += ref;
		UINT type = raw_value(data + cursor, 2) - 0x8000;
		cursor += 2;
		db_table *table = db_find_table(source->db, table_name);
		if (table && number)
			counter = number - 0x8000;
		else if (!last || lstrcmpW(last, table_name))
			counter = 1;
		else
			counter++;
		msi_free(last);
		last = msi_copy(table_name);
		const WCHAR *column = pool_text(source->strings, name_id);
		if (table && column) {
			BOOL found = FALSE;
			for (UINT c = 0; c < table->column_count; c++)
				if (!lstrcmpW(table->columns[c].name, column))
					found = TRUE;
			if (!found)
				db_add_column(source->db, table, column, type & 0xffff, counter);
		}
		at = cursor;
	}
	msi_free(last);
}

static void transform_view_add(msi_db *db, db_table *view, const WCHAR *table, const WCHAR *column, const WCHAR *row, const WCHAR *data, const WCHAR *current)
{
	UINT cells[5] = {pool_add(db, table, lstrlenW(table)), pool_add(db, column, lstrlenW(column)),
		row ? pool_add(db, row, lstrlenW(row)) : 0, data ? pool_add(db, data, lstrlenW(data)) : 0, current ? pool_add(db, current, lstrlenW(current)) : 0};
	table_insert(db, view, cells, TRUE);
}

static WCHAR *cell_text(msi_db *db, msi_db *strings, UINT type, UINT cell)
{
	if (!cell)
		return NULL;
	if (type & TYPE_STRING)
		return msi_copy(pool_text(strings, cell));
	WCHAR digits[16];
	wsprintfW(digits, L"%d", cell_to_int(type, cell));
	return msi_copy(digits);
}

static db_table *transform_view_table(msi_db *db)
{
	static const db_column shape[] = {
		{(WCHAR *)L"Table", TYPE_VALID | TYPE_TEMPORARY | TYPE_STRING | TYPE_NOT_BINARY | TYPE_KEY},
		{(WCHAR *)L"Column", TYPE_VALID | TYPE_TEMPORARY | TYPE_STRING | TYPE_NOT_BINARY | TYPE_KEY},
		{(WCHAR *)L"Row", TYPE_VALID | TYPE_TEMPORARY | TYPE_STRING | TYPE_NOT_BINARY | TYPE_NULLABLE | TYPE_KEY},
		{(WCHAR *)L"Data", TYPE_VALID | TYPE_TEMPORARY | TYPE_STRING | TYPE_NOT_BINARY | TYPE_NULLABLE},
		{(WCHAR *)L"Current", TYPE_VALID | TYPE_TEMPORARY | TYPE_STRING | TYPE_NOT_BINARY | TYPE_NULLABLE},
	};
	db_table *view = db_find_table(db, L"_TransformView");
	if (!view && db_create_table(db, L"_TransformView", shape, 5, TRUE, &view))
		return NULL;
	view->holds = 1;
	return view;
}

typedef struct {
	const WCHAR *table, *name;
	UINT type;
} view_column;

typedef struct {
	view_column *items;
	UINT count;
} view_columns;

// Columns the transform itself introduces are in its own catalog, not in the database yet.
static void view_catalogs(transform_source *source, db_table *view, view_columns *extra)
{
	msi_db *db = source->db;
	UINT ref = source->ref_size;
	ULONG size = 0;
	BYTE *data = read_transform_table(source->storage, L"_Tables", &size);
	if (data) {
		for (ULONG at = 0; at + 2 + ref <= size; at += 2 + ref) {
			UINT mask = data[at] | (data[at + 1] << 8);
			const WCHAR *name = pool_text(source->strings, raw_value(data + at + 2, ref));
			if (name)
				transform_view_add(db, view, name, mask ? L"CREATE" : L"DROP", NULL, NULL, NULL);
		}
		msi_free(data);
	}
	data = read_transform_table(source->storage, L"_Columns", &size);
	if (!data)
		return;
	const WCHAR *last = NULL;
	UINT counter = 0;
	extra->items = msi_alloc((size / 6 + 1) * sizeof(view_column));
	for (ULONG at = 0; at + 2 + ref + 2 <= size; ) {
		UINT mask = data[at] | (data[at + 1] << 8);
		ULONG cursor = at + 2;
		const WCHAR *table = pool_text(source->strings, raw_value(data + cursor, ref));
		cursor += ref;
		UINT number = raw_value(data + cursor, 2);
		cursor += 2;
		if (mask && cursor + ref + 2 <= size) {
			const WCHAR *name = pool_text(source->strings, raw_value(data + cursor, ref));
			cursor += ref;
			UINT type = (raw_value(data + cursor, 2) - 0x8000) & 0xffff;
			cursor += 2;
			if (number)
				counter = number - 0x8000;
			else
				counter = last && table && !lstrcmpW(last, table) ? counter + 1 : 1;
			last = table;
			if (table && name) {
				WCHAR type_text[16], number_text[16];
				wsprintfW(type_text, L"%u", type);
				wsprintfW(number_text, L"%u", counter);
				transform_view_add(db, view, table, name, NULL, type_text, number_text);
				view_column *added = &extra->items[extra->count++];
				added->table = table;
				added->name = name;
				added->type = type;
			}
		}
		at = cursor;
	}
	msi_free(data);
}

// The change as data instead of applied: one row for each cell a transform would touch.
static void view_table_changes(transform_source *source, db_table *view, const view_columns *extra, const WCHAR *name, const BYTE *data, ULONG size)
{
	msi_db *db = source->db;
	db_table *table = db_find_table(db, name);
	view_column shape[64];
	UINT columns = 0;
	if (table) {
		db_load_table(db, table);
		for (UINT c = 0; c < table->column_count && columns < 64; c++) {
			shape[columns].name = table->columns[c].name;
			shape[columns++].type = table->columns[c].type;
		}
	}
	for (UINT i = 0; i < extra->count && columns < 64; i++)
		if (!lstrcmpW(extra->items[i].table, name))
			shape[columns++] = extra->items[i];
	if (!columns)
		return;
	UINT cells[64];
	for (ULONG at = 0; at + 2 <= size; ) {
		UINT mask = data[at] | (data[at + 1] << 8);
		ULONG cursor = at + 2;
		UINT count = mask & 1 ? mask >> 8 : columns;
		BOOL present[64] = {0};
		if (count > columns)
			break;
		memset(cells, 0, sizeof(cells));
		for (UINT c = 0; c < count; c++) {
			UINT type = shape[c].type;
			if (!(mask & 1) && !(type & TYPE_KEY) && !(mask & (1u << c)))
				continue;
			UINT bytes = file_bytes(type, source->ref_size);
			UINT value = cursor + bytes <= size ? raw_value(data + cursor, bytes) : 0;
			cursor += bytes;
			if ((type & TYPE_STRING) && !TYPE_IS_BINARY(type)) {
				const WCHAR *text = pool_text(source->strings, value);
				value = text ? pool_add(db, text, lstrlenW(text)) : 0;
			}
			cells[c] = value;
			present[c] = TRUE;
		}
		WCHAR row_text[512] = L"", stream_text[600];
		for (UINT c = 0; c < columns; c++)
			if (shape[c].type & TYPE_KEY) {
				WCHAR *text = cell_text(db, db, shape[c].type, cells[c]);
				if (*row_text)
					lstrcatW(row_text, L"\t");
				if (text)
					lstrcatW(row_text, text);
				msi_free(text);
			}
		db_row *current = table && columns == table->column_count ? table_find_key(db, table, cells) : NULL;
		if (table && !current && columns != table->column_count) {
			// The row lookup only reads the columns the table has, all of them keys or not.
			current = table_find_key(db, table, cells);
		}
		if (!mask) {
			transform_view_add(db, view, name, L"DELETE", row_text, NULL, NULL);
		} else {
			if (!current)
				transform_view_add(db, view, name, L"INSERT", row_text, NULL, NULL);
			for (UINT c = 0; c < count; c++) {
				if (!present[c] || (shape[c].type & TYPE_KEY))
					continue;
				if (TYPE_IS_BINARY(shape[c].type)) {
					wsprintfW(stream_text, L"%s.%s", name, row_text);
					transform_view_add(db, view, name, shape[c].name, row_text, stream_text, NULL);
					continue;
				}
				WCHAR *text = cell_text(db, db, shape[c].type, cells[c]);
				WCHAR *old = current && table && c < table->column_count ? cell_text(db, db, shape[c].type, current->cells[c]) : NULL;
				transform_view_add(db, view, name, shape[c].name, row_text, text, old);
				msi_free(text);
				msi_free(old);
			}
		}
		at = cursor;
	}
}

static UINT apply_storage(msi_db *db, IStorage *storage, INT conditions)
{
	msi_db *strings = NULL;
	UINT status = db_open_on_storage(storage, &strings);
	if (status)
		return status;
	transform_source source = {db, strings, storage, strings->ref_size};
	db_table *view = (conditions & MSITRANSFORM_ERROR_VIEWTRANSFORM) ? transform_view_table(db) : NULL;
	ULONG size = 0;
	BYTE *data;
	view_columns extra = {0};
	if (view)
		view_catalogs(&source, view, &extra);
	if (!view) {
		if ((data = read_transform_table(storage, L"_Tables", &size))) {
			apply_tables_catalog(&source, data, size);
			msi_free(data);
		}
		if ((data = read_transform_table(storage, L"_Columns", &size))) {
			apply_columns_catalog(&source, data, size);
			msi_free(data);
		}
	}
	IEnumSTATSTG *items = NULL;
	if (SUCCEEDED(IStorage_EnumElements(storage, 0, NULL, 0, &items))) {
		STATSTG stat;
		ULONG got;
		while (IEnumSTATSTG_Next(items, 1, &stat, &got) == S_OK && got) {
			WCHAR name[128];
			if (stat.type == STGTY_STREAM && lstrlenW(stat.pwcsName) < 64 && name_decode(stat.pwcsName, name)
				&& lstrcmpW(name, L"_Tables") && lstrcmpW(name, L"_Columns") && lstrcmpW(name, L"_StringPool") && lstrcmpW(name, L"_StringData")) {
				if ((data = read_transform_table(storage, name, &size))) {
					if (view)
						view_table_changes(&source, view, &extra, name, data, size);
					else
						apply_table(&source, name, data, size);
					msi_free(data);
				}
			}
			CoTaskMemFree(stat.pwcsName);
		}
		IEnumSTATSTG_Release(items);
	}
	msi_free(extra.items);
	object_unref(&strings->base);
	return status;
}

UINT db_apply_storage(msi_db *db, IStorage *storage, INT conditions)
{
	return apply_storage(db, storage, conditions);
}

UINT db_apply_transform(msi_db *db, const WCHAR *path, INT conditions)
{
	IStorage *storage = NULL;
	HRESULT hr;
	if (path[0] == L':') {
		// Inside the package itself, under the name after the colon.
		WCHAR encoded[64];
		name_encode(path + 1, FALSE, encoded);
		hr = IStorage_OpenStorage(db->storage, encoded, NULL, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, NULL, 0, &storage);
		if (FAILED(hr))
			hr = IStorage_OpenStorage(db->storage, path + 1, NULL, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, NULL, 0, &storage);
	} else {
		hr = StgOpenStorage(path, NULL, STGM_DIRECT | STGM_READ | STGM_SHARE_DENY_WRITE, NULL, 0, &storage);
	}
	if (FAILED(hr))
		return ERROR_OPEN_FAILED;
	UINT status = apply_storage(db, storage, conditions);
	IStorage_Release(storage);
	return status;
}

UINT WINAPI MsiDatabaseApplyTransformW(MSIHANDLE handle, LPCWSTR path, int conditions)
{
	if (!path)
		return ERROR_INVALID_PARAMETER;
	msi_db *db = (msi_db *)handle_lookup(handle, KIND_DATABASE);
	if (!db)
		return ERROR_INVALID_HANDLE;
	UINT status = db_apply_transform(db, path, conditions);
	object_unref(&db->base);
	return status;
}

UINT WINAPI MsiDatabaseApplyTransformA(MSIHANDLE handle, LPCSTR path, int conditions)
{
	WCHAR *wide = msi_from_ansi(path);
	UINT status = MsiDatabaseApplyTransformW(handle, wide, conditions);
	msi_free(wide);
	return status;
}

// A transform is written with its own string pool; ids only mean something inside the one being built.
typedef struct {
	WCHAR **texts;
	UINT count, room;
} out_pool;

static UINT out_pool_id(out_pool *pool, const WCHAR *text)
{
	if (!text || !*text)
		return 0;
	for (UINT i = 1; i < pool->count; i++)
		if (!lstrcmpW(pool->texts[i], text))
			return i;
	if (pool->count >= pool->room) {
		pool->room = pool->room ? pool->room * 2 : 64;
		pool->texts = msi_grow(pool->texts, pool->room * sizeof(WCHAR *));
	}
	if (!pool->count)
		pool->count = 1;
	pool->texts[pool->count] = msi_copy(text);
	return pool->count++;
}

typedef struct {
	BYTE *data;
	SIZE_T size, room;
} out_bytes;

static void out_put(out_bytes *out, UINT value, UINT bytes)
{
	if (out->size + bytes > out->room) {
		out->room = out->room ? out->room * 2 : 256;
		out->data = msi_grow(out->data, out->room);
	}
	for (UINT i = 0; i < bytes; i++)
		out->data[out->size++] = (BYTE)(value >> (8 * i));
}

static void put_stream(IStorage *storage, const WCHAR *name, BOOL table, const void *data, ULONG size)
{
	WCHAR encoded[128];
	IStream *stream = NULL;
	name_encode(name, table, encoded);
	if (SUCCEEDED(IStorage_CreateStream(storage, encoded, STGM_WRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE, 0, 0, &stream))) {
		ULONG done;
		if (size)
			IStream_Write(stream, data, size, &done);
		IStream_Release(stream);
	}
}

static void out_cell(out_bytes *out, out_pool *pool, msi_db *db, UINT type, UINT cell)
{
	if ((type & TYPE_STRING) && !TYPE_IS_BINARY(type))
		out_put(out, out_pool_id(pool, pool_text(db, cell)), 2);
	else
		out_put(out, cell, file_bytes(type, 2));
}

static BOOL same_cell(msi_db *a, msi_db *b, UINT type, UINT left, UINT right)
{
	if ((type & TYPE_STRING) && !TYPE_IS_BINARY(type))
		return same_text(pool_text(a, left), pool_text(b, right));
	return left == right;
}

static db_row *find_row_across(msi_db *from, db_table *from_table, const UINT *cells, msi_db *in, db_table *in_table)
{
	for (UINT r = 0; r < in_table->row_count; r++) {
		BOOL match = TRUE;
		for (UINT c = 0; c < from_table->column_count && c < in_table->column_count && match; c++)
			if (from_table->columns[c].type & TYPE_KEY)
				match = same_cell(from, in, from_table->columns[c].type, cells[c], in_table->rows[r]->cells[c]);
		if (match)
			return in_table->rows[r];
	}
	return NULL;
}

// Every difference taking the reference database to the changed one, in the entry form a transform applies.
static BOOL generate_table(msi_db *changed, msi_db *reference, db_table *table, out_pool *pool, IStorage *storage, out_bytes *new_tables, out_bytes *new_columns)
{
	db_table *base = db_find_table(reference, table->name);
	out_bytes out = {0};
	db_load_table(changed, table);
	if (base)
		db_load_table(reference, base);
	if (!base) {
		out_put(new_tables, 1 | (1 << 8), 2);
		out_put(new_tables, out_pool_id(pool, table->name), 2);
		for (UINT c = 0; c < table->column_count; c++) {
			out_put(new_columns, 1 | (4 << 8), 2);
			out_put(new_columns, out_pool_id(pool, table->name), 2);
			out_put(new_columns, 0x8000 + c + 1, 2);
			out_put(new_columns, out_pool_id(pool, table->columns[c].name), 2);
			out_put(new_columns, 0x8000 + table->columns[c].type, 2);
		}
	} else {
		for (UINT c = base->column_count; c < table->column_count; c++) {
			out_put(new_columns, 1 | (4 << 8), 2);
			out_put(new_columns, out_pool_id(pool, table->name), 2);
			out_put(new_columns, 0x8000 + c + 1, 2);
			out_put(new_columns, out_pool_id(pool, table->columns[c].name), 2);
			out_put(new_columns, 0x8000 + table->columns[c].type, 2);
		}
	}
	for (UINT r = 0; r < table->row_count; r++) {
		const UINT *cells = table->rows[r]->cells;
		db_row *old = base ? find_row_across(changed, table, cells, reference, base) : NULL;
		if (!old) {
			out_put(&out, 1 | (table->column_count << 8), 2);
			for (UINT c = 0; c < table->column_count; c++)
				out_cell(&out, pool, changed, table->columns[c].type, cells[c]);
		} else {
			UINT mask = 0;
			for (UINT c = 0; c < table->column_count && c < 16; c++) {
				if (table->columns[c].type & TYPE_KEY)
					continue;
				UINT before = c < base->column_count ? old->cells[c] : 0;
				if (!same_cell(changed, reference, table->columns[c].type, cells[c], before))
					mask |= 1u << c;
			}
			if (!mask)
				continue;
			// The low bit is the marker of full rows, so a change there writes the row out whole.
			if (mask & 1) {
				out_put(&out, 1 | (table->column_count << 8), 2);
				for (UINT c = 0; c < table->column_count; c++)
					out_cell(&out, pool, changed, table->columns[c].type, cells[c]);
			} else {
				out_put(&out, mask, 2);
				for (UINT c = 0; c < table->column_count; c++)
					if ((table->columns[c].type & TYPE_KEY) || (mask & (1u << c)))
						out_cell(&out, pool, changed, table->columns[c].type, cells[c]);
			}
		}
	}
	for (UINT r = 0; base && r < base->row_count; r++) {
		const UINT *cells = base->rows[r]->cells;
		if (find_row_across(reference, base, cells, changed, table))
			continue;
		out_put(&out, 0, 2);
		for (UINT c = 0; c < base->column_count; c++)
			if (base->columns[c].type & TYPE_KEY)
				out_cell(&out, pool, reference, base->columns[c].type, cells[c]);
	}
	if (out.size && storage)
		put_stream(storage, table->name, TRUE, out.data, (ULONG)out.size);
	BOOL any = out.size != 0 || !base;
	msi_free(out.data);
	return any;
}

UINT WINAPI MsiDatabaseGenerateTransformW(MSIHANDLE handle, MSIHANDLE reference_handle, LPCWSTR path, int reserved1, int reserved2)
{
	if (reserved1 || reserved2)
		return ERROR_INVALID_PARAMETER;
	msi_db *changed = (msi_db *)handle_lookup(handle, KIND_DATABASE);
	msi_db *reference = (msi_db *)handle_lookup(reference_handle, KIND_DATABASE);
	UINT status = ERROR_INVALID_HANDLE;
	IStorage *storage = NULL;
	if (!changed || !reference)
		goto done;
	static const GUID clsid_transform = {0x000c1082, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
	if (path) {
		if (FAILED(StgCreateDocfile(path, STGM_CREATE | STGM_DIRECT | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &storage))) {
			status = ERROR_FUNCTION_FAILED;
			goto done;
		}
		IStorage_SetClass(storage, &clsid_transform);
	}
	out_pool pool = {0};
	out_bytes tables = {0}, columns = {0};
	BOOL any = FALSE;
	for (db_table *table = changed->tables; table; table = table->next) {
		if (table->virtual_table || table->temporary)
			continue;
		if (generate_table(changed, reference, table, &pool, storage, &tables, &columns))
			any = TRUE;
	}
	for (db_table *table = reference->tables; table; table = table->next) {
		if (table->virtual_table || table->temporary || db_find_table(changed, table->name))
			continue;
		out_put(&tables, 0, 2);
		out_put(&tables, out_pool_id(&pool, table->name), 2);
		any = TRUE;
	}
	status = any ? ERROR_SUCCESS : ERROR_NO_DATA;
	if (storage) {
		if (tables.size)
			put_stream(storage, L"_Tables", TRUE, tables.data, (ULONG)tables.size);
		if (columns.size)
			put_stream(storage, L"_Columns", TRUE, columns.data, (ULONG)columns.size);
		out_bytes entries = {0}, text = {0};
		out_put(&entries, changed->codepage, 2);
		out_put(&entries, changed->codepage >> 16, 2);
		for (UINT i = 1; i < pool.count; i++) {
			UINT codepage = changed->codepage ? changed->codepage : CP_ACP;
			INT bytes = WideCharToMultiByte(codepage, 0, pool.texts[i], -1, NULL, 0, NULL, NULL) - 1;
			char *narrow = msi_alloc(bytes + 1);
			WideCharToMultiByte(codepage, 0, pool.texts[i], -1, narrow, bytes + 1, NULL, NULL);
			for (INT k = 0; k < bytes; k++)
				out_put(&text, (BYTE)narrow[k], 1);
			msi_free(narrow);
			out_put(&entries, bytes, 2);
			out_put(&entries, 1, 2);
		}
		put_stream(storage, L"_StringPool", TRUE, entries.data, (ULONG)entries.size);
		put_stream(storage, L"_StringData", TRUE, text.data, (ULONG)text.size);
		msi_free(entries.data);
		msi_free(text.data);
	}
	for (UINT i = 1; i < pool.count; i++)
		msi_free(pool.texts[i]);
	msi_free(pool.texts);
	msi_free(tables.data);
	msi_free(columns.data);
done:
	if (storage)
		IStorage_Release(storage);
	if (changed)
		object_unref(&changed->base);
	if (reference)
		object_unref(&reference->base);
	return status;
}

UINT WINAPI MsiDatabaseGenerateTransformA(MSIHANDLE handle, MSIHANDLE reference, LPCSTR path, int reserved1, int reserved2)
{
	WCHAR *wide = msi_from_ansi(path);
	UINT status = MsiDatabaseGenerateTransformW(handle, reference, wide, reserved1, reserved2);
	msi_free(wide);
	return status;
}

static BOOL same_shape(db_table *a, db_table *b)
{
	if (a->column_count != b->column_count)
		return FALSE;
	for (UINT c = 0; c < a->column_count; c++) {
		if (lstrcmpW(a->columns[c].name, b->columns[c].name))
			return FALSE;
		if ((a->columns[c].type & ~(TYPE_NULLABLE | TYPE_SIZE)) != (b->columns[c].type & ~(TYPE_NULLABLE | TYPE_SIZE)))
			return FALSE;
	}
	return TRUE;
}

static UINT merge_cell(msi_db *to, msi_db *from, UINT type, UINT cell)
{
	if ((type & TYPE_STRING) && !TYPE_IS_BINARY(type)) {
		const WCHAR *text = pool_text(from, cell);
		return text ? pool_add(to, text, lstrlenW(text)) : 0;
	}
	return cell;
}

// Rows that disagree with an existing key are not taken and count as conflicts; the error table lists them per table.
static UINT merge_table(msi_db *to, msi_db *from, db_table *source, db_table *errors)
{
	db_table *target = db_find_table(to, source->name);
	db_load_table(from, source);
	if (target) {
		if (!same_shape(target, source))
			return ERROR_DATATYPE_MISMATCH;
		db_load_table(to, target);
	} else if (db_create_table(to, source->name, source->columns, source->column_count, FALSE, &target)) {
		return ERROR_FUNCTION_FAILED;
	}
	UINT conflicts = 0;
	UINT *cells = msi_alloc((source->column_count + 1) * sizeof(UINT));
	for (UINT r = 0; r < source->row_count; r++) {
		for (UINT c = 0; c < source->column_count; c++)
			cells[c] = merge_cell(to, from, source->columns[c].type, source->rows[r]->cells[c]);
		db_row *existing = table_find_key(to, target, cells);
		if (existing) {
			if (memcmp(existing->cells, cells, source->column_count * sizeof(UINT)))
				conflicts++;
			continue;
		}
		table_insert(to, target, cells, FALSE);
		for (UINT c = 0; c < source->column_count; c++)
			if (TYPE_IS_BINARY(source->columns[c].type) && cells[c]) {
				WCHAR *from_name = row_stream_name(from, source, source->rows[r]->cells, c);
				msi_blob *blob = db_read_named_stream(from, from_name);
				if (blob) {
					WCHAR *to_name = row_stream_name(to, target, cells, c);
					stream_put(to, to_name, blob);
					msi_free(to_name);
					blob_unref(blob);
				}
				msi_free(from_name);
			}
	}
	msi_free(cells);
	if (conflicts && errors) {
		UINT row[2] = {pool_add(to, source->name, lstrlenW(source->name)), cell_from_int(2, conflicts)};
		table_insert(to, errors, row, errors->temporary);
	}
	return conflicts ? ERROR_FUNCTION_FAILED : ERROR_SUCCESS;
}

UINT WINAPI MsiDatabaseMergeW(MSIHANDLE to_handle, MSIHANDLE from_handle, LPCWSTR error_table)
{
	msi_db *to = (msi_db *)handle_lookup(to_handle, KIND_DATABASE);
	msi_db *from = (msi_db *)handle_lookup(from_handle, KIND_DATABASE);
	UINT status = ERROR_SUCCESS;
	if (!to || !from) {
		status = ERROR_INVALID_HANDLE;
		goto done;
	}
	// Nothing is changed at all when any shared table has another shape.
	for (db_table *table = from->tables; table; table = table->next) {
		if (table->virtual_table)
			continue;
		db_table *target = db_find_table(to, table->name);
		if (target && !same_shape(target, table)) {
			status = ERROR_DATATYPE_MISMATCH;
			goto done;
		}
	}
	db_table *errors = NULL;
	if (error_table && *error_table) {
		errors = db_find_table(to, error_table);
		if (!errors) {
			db_column shape[2] = {{(WCHAR *)L"Table", TYPE_VALID | TYPE_STRING | TYPE_NOT_BINARY | TYPE_KEY | 255}, {(WCHAR *)L"NumRowMergeConflicts", TYPE_VALID | TYPE_NOT_BINARY | 2}};
			db_create_table(to, error_table, shape, 2, FALSE, &errors);
		}
	}
	for (db_table *table = from->tables; table; table = table->next) {
		if (table->virtual_table)
			continue;
		UINT result = merge_table(to, from, table, errors);
		if (result && !status)
			status = result;
	}
done:
	if (to)
		object_unref(&to->base);
	if (from)
		object_unref(&from->base);
	return status;
}

UINT WINAPI MsiDatabaseMergeA(MSIHANDLE to, MSIHANDLE from, LPCSTR error_table)
{
	WCHAR *wide = msi_from_ansi(error_table);
	UINT status = MsiDatabaseMergeW(to, from, wide);
	msi_free(wide);
	return status;
}
