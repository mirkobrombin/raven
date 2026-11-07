/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

// The layout of an installer database inside its compound file comes from the public format: each table is a
// stream written column by column, strings live in a shared pool, and stream names are packed into 6-bit pairs.
static const GUID clsid_database = {0x000c1084, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID clsid_patch = {0x000c1086, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID clsid_transform = {0x000c1082, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

enum { NAME_PAIR = 0x3800, NAME_SINGLE = 0x4800, NAME_TABLE = 0x4840 };

static int name_digit(WCHAR c)
{
	if (c >= L'0' && c <= L'9')
		return c - L'0';
	if (c >= L'A' && c <= L'Z')
		return c - L'A' + 10;
	if (c >= L'a' && c <= L'z')
		return c - L'a' + 36;
	if (c == L'.')
		return 62;
	if (c == L'_')
		return 63;
	return -1;
}

static WCHAR digit_name(UINT digit)
{
	if (digit < 10)
		return L'0' + digit;
	if (digit < 36)
		return L'A' + digit - 10;
	if (digit < 62)
		return L'a' + digit - 36;
	return digit == 62 ? L'.' : L'_';
}

void name_encode(const WCHAR *in, BOOL table, WCHAR *out)
{
	if (table)
		*out++ = NAME_TABLE;
	// The property set streams of OLE keep their well known spelling.
	if (!table && *in && *in < 0x20) {
		lstrcpyW(out, in);
		return;
	}
	while (*in) {
		int first = name_digit(*in);
		if (first < 0) {
			*out++ = *in++;
			continue;
		}
		int second = in[1] ? name_digit(in[1]) : -1;
		if (second >= 0) {
			*out++ = NAME_PAIR + first + (second << 6);
			in += 2;
		} else {
			*out++ = NAME_SINGLE + first;
			in++;
		}
	}
	*out = 0;
}

// TRUE for the name of a table's stream.
BOOL name_decode(const WCHAR *in, WCHAR *out)
{
	BOOL table = FALSE;
	for (; *in; in++) {
		WCHAR c = *in;
		if (c == NAME_TABLE) {
			table = TRUE;
		} else if (c >= NAME_PAIR && c < NAME_SINGLE) {
			*out++ = digit_name((c - NAME_PAIR) & 0x3f);
			*out++ = digit_name(((c - NAME_PAIR) >> 6) & 0x3f);
		} else if (c >= NAME_SINGLE && c < NAME_TABLE) {
			*out++ = digit_name(c - NAME_SINGLE);
		} else {
			*out++ = c;
		}
	}
	*out = 0;
	return table;
}

static BYTE *read_element(IStorage *storage, const WCHAR *name, BOOL table, ULONG *size)
{
	WCHAR *encoded = msi_alloc((lstrlenW(name) + 2) * sizeof(WCHAR));
	IStream *stream = NULL;
	STATSTG stat;
	BYTE *data = NULL;
	name_encode(name, table, encoded);
	HRESULT hr = IStorage_OpenStream(storage, encoded, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream);
	msi_free(encoded);
	if (FAILED(hr))
		return NULL;
	if (SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME))) {
		data = msi_alloc(stat.cbSize.LowPart);
		ULONG done = 0;
		if (data && stat.cbSize.LowPart && (FAILED(IStream_Read(stream, data, stat.cbSize.LowPart, &done)) || done != stat.cbSize.LowPart)) {
			msi_free(data);
			data = NULL;
		}
		*size = stat.cbSize.LowPart;
	}
	IStream_Release(stream);
	return data;
}

static BOOL write_element(IStorage *storage, const WCHAR *name, BOOL table, const void *data, ULONG size)
{
	WCHAR *encoded = msi_alloc((lstrlenW(name) + 2) * sizeof(WCHAR));
	IStream *stream = NULL;
	name_encode(name, table, encoded);
	HRESULT hr = IStorage_CreateStream(storage, encoded, STGM_WRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE, 0, 0, &stream);
	msi_free(encoded);
	if (FAILED(hr))
		return FALSE;
	ULONG done = 0;
	hr = size ? IStream_Write(stream, data, size, &done) : S_OK;
	IStream_Release(stream);
	return SUCCEEDED(hr);
}

static void destroy_element(IStorage *storage, const WCHAR *name, BOOL table)
{
	WCHAR *encoded = msi_alloc((lstrlenW(name) + 2) * sizeof(WCHAR));
	name_encode(name, table, encoded);
	IStorage_DestroyElement(storage, encoded);
	msi_free(encoded);
}

static UINT text_hash(const WCHAR *text, SIZE_T length)
{
	UINT hash = 2166136261u;
	for (SIZE_T i = 0; i < length; i++)
		hash = (hash ^ text[i]) * 16777619u;
	return hash;
}

static void pool_rehash(msi_db *db, UINT size)
{
	msi_free(db->pool_hash);
	db->pool_hash = msi_alloc(size * sizeof(UINT));
	db->pool_hash_size = size;
	for (UINT id = 1; id < db->pool_count; id++) {
		pool_entry *entry = &db->pool[id];
		if (!entry->text)
			continue;
		UINT slot = text_hash(entry->text, entry->length) % size;
		entry->next = db->pool_hash[slot];
		db->pool_hash[slot] = id;
	}
}

static UINT pool_find_n(msi_db *db, const WCHAR *text, SIZE_T length)
{
	if (!db->pool_hash_size)
		return 0;
	for (UINT id = db->pool_hash[text_hash(text, length) % db->pool_hash_size]; id; id = db->pool[id].next)
		if (db->pool[id].length == length && !memcmp(db->pool[id].text, text, length * sizeof(WCHAR)))
			return id;
	return 0;
}

UINT pool_find(msi_db *db, const WCHAR *text)
{
	return pool_find_n(db, text, lstrlenW(text));
}

UINT pool_add(msi_db *db, const WCHAR *text, SIZE_T length)
{
	if (!length)
		return 0;
	UINT id = pool_find_n(db, text, length);
	if (id)
		return id;
	if (!db->pool_count)
		db->pool_count = 1;
	if (db->pool_count >= db->pool_room) {
		UINT room = db->pool_room ? db->pool_room * 2 : 256;
		db->pool = msi_grow(db->pool, room * sizeof(pool_entry));
		db->pool_room = room;
	}
	if (db->pool_count * 2 >= db->pool_hash_size)
		pool_rehash(db, (db->pool_count + 64) * 4 + 1);
	id = db->pool_count++;
	db->pool[id].text = msi_copy_n(text, length);
	db->pool[id].length = (UINT)length;
	UINT slot = text_hash(text, length) % db->pool_hash_size;
	db->pool[id].next = db->pool_hash[slot];
	db->pool_hash[slot] = id;
	return id;
}

const WCHAR *pool_text(msi_db *db, UINT id)
{
	return id && id < db->pool_count ? db->pool[id].text : NULL;
}

static UINT effective_codepage(const msi_db *db)
{
	return db->codepage ? db->codepage : CP_ACP;
}

// Each id keeps its place: tables on disk refer to strings by it, and a hole in the pool is an empty entry.
// Packages hold tens of thousands of strings, nearly all ASCII, and every custom action host reads them all.
// Stateful code pages (50000 and up, except UTF-8) give plain bytes other meanings.
static void pool_decode(pool_entry *entry, UINT codepage, const char *bytes, UINT length)
{
	BOOL plain = codepage < 50000 || codepage == CP_UTF8;
	for (UINT i = 0; plain && i < length; i++)
		plain = (BYTE)bytes[i] < 0x80;
	if (plain) {
		entry->text = msi_alloc((length + 1) * sizeof(WCHAR));
		for (UINT i = 0; i < length; i++)
			entry->text[i] = (BYTE)bytes[i];
		entry->length = length;
		return;
	}
	INT wide = MultiByteToWideChar(codepage, 0, bytes, length, NULL, 0);
	entry->text = msi_alloc((wide + 1) * sizeof(WCHAR));
	MultiByteToWideChar(codepage, 0, bytes, length, entry->text, wide);
	entry->length = wide;
}

static BOOL pool_load(msi_db *db, UINT *ref_size)
{
	ULONG pool_size = 0, data_size = 0;
	USHORT *entries = (USHORT *)read_element(db->storage, L"_StringPool", TRUE, &pool_size);
	BYTE *data = read_element(db->storage, L"_StringData", TRUE, &data_size);
	BOOL ok = FALSE;
	if (!entries || !data || pool_size < 4)
		goto done;
	db->codepage = entries[0] | ((entries[1] & 0x7fff) << 16);
	*ref_size = (entries[1] & 0x8000) ? 3 : 2;
	UINT count = pool_size / 4;
	db->pool_room = count + 256;
	db->pool = msi_alloc(db->pool_room * sizeof(pool_entry));
	db->pool_count = 1;
	UINT offset = 0, n = 1;
	while (n < count) {
		UINT length = entries[n * 2];
		USHORT refs = entries[n * 2 + 1];
		n++;
		if (!length && refs && n < count) {
			length = (refs << 16) | entries[n * 2];
			n++;
		}
		UINT id = db->pool_count++;
		if (length && offset + length <= data_size) {
			pool_decode(&db->pool[id], effective_codepage(db), (char *)data + offset, length);
			offset += length;
		}
	}
	pool_rehash(db, db->pool_count * 2 + 257);
	ok = TRUE;
done:
	msi_free(entries);
	msi_free(data);
	return ok;
}

static UINT column_bytes(UINT type, UINT ref_size)
{
	if (TYPE_IS_BINARY(type))
		return 2;
	if (type & TYPE_STRING)
		return ref_size;
	return (type & TYPE_SIZE) <= 2 ? 2 : 4;
}

static db_table *table_alloc(const WCHAR *name)
{
	db_table *table = msi_alloc(sizeof(*table));
	table->name = msi_copy(name);
	return table;
}

static void table_link(msi_db *db, db_table *table)
{
	db_table **at = &db->tables;
	while (*at)
		at = &(*at)->next;
	*at = table;
}

db_table *db_find_table(msi_db *db, const WCHAR *name)
{
	for (db_table *table = db->tables; table; table = table->next)
		if (!lstrcmpW(table->name, name))
			return table;
	return NULL;
}

static db_row *row_push(db_table *table, const UINT *cells, UINT index)
{
	db_row *row = msi_alloc(sizeof(*row));
	row->cells = msi_alloc(table->column_count * sizeof(UINT) + sizeof(UINT));
	if (cells)
		memcpy(row->cells, cells, table->column_count * sizeof(UINT));
	if (table->row_count == table->row_room) {
		table->row_room = table->row_room ? table->row_room * 2 : 16;
		table->rows = msi_grow(table->rows, table->row_room * sizeof(*table->rows));
	}
	memmove(table->rows + index + 1, table->rows + index, (table->row_count - index) * sizeof(*table->rows));
	table->rows[index] = row;
	table->row_count++;
	return row;
}

// One column after the other, so a row's cells are scattered over the whole stream.
static void table_decode(db_table *table, const BYTE *data, ULONG size, UINT ref_size)
{
	UINT row_bytes = 0;
	for (UINT c = 0; c < table->column_count; c++)
		if (!(table->columns[c].type & TYPE_TEMPORARY))
			row_bytes += column_bytes(table->columns[c].type, ref_size);
	if (!row_bytes)
		return;
	UINT rows = size / row_bytes, start = table->row_count;
	for (UINT r = 0; r < rows; r++)
		row_push(table, NULL, table->row_count);
	const BYTE *at = data;
	for (UINT c = 0; c < table->column_count; c++) {
		if (table->columns[c].type & TYPE_TEMPORARY)
			continue;
		UINT bytes = column_bytes(table->columns[c].type, ref_size);
		for (UINT r = 0; r < rows; r++, at += bytes) {
			UINT value = at[0] | (at[1] << 8);
			if (bytes > 2)
				value |= at[2] << 16;
			if (bytes > 3)
				value |= (UINT)at[3] << 24;
			table->rows[start + r]->cells[c] = value;
		}
	}
}

UINT db_load_table(msi_db *db, db_table *table)
{
	if (table->loaded || table->virtual_table)
		return ERROR_SUCCESS;
	table->loaded = TRUE;
	if (!table->on_disk)
		return ERROR_SUCCESS;
	ULONG size = 0;
	BYTE *data = read_element(db->storage, table->name, TRUE, &size);
	if (data) {
		table_decode(table, data, size, db->ref_size);
		msi_free(data);
	}
	return ERROR_SUCCESS;
}

static db_table *catalog_table(msi_db *db, const WCHAR *name, const db_column *columns, UINT count)
{
	db_table *table = table_alloc(name);
	table->column_count = count;
	table->columns = msi_alloc(count * sizeof(db_column));
	for (UINT i = 0; i < count; i++) {
		table->columns[i].name = msi_copy(columns[i].name);
		table->columns[i].type = columns[i].type;
	}
	table->virtual_table = TRUE;
	table->loaded = TRUE;
	table_link(db, table);
	return table;
}

static const db_column tables_columns[] = {{(WCHAR *)L"Name", TYPE_VALID | TYPE_STRING | TYPE_NOT_BINARY | TYPE_KEY | 64}};
static const db_column columns_columns[] = {
	{(WCHAR *)L"Table", TYPE_VALID | TYPE_STRING | TYPE_NOT_BINARY | TYPE_KEY | 64},
	{(WCHAR *)L"Number", TYPE_VALID | TYPE_NOT_BINARY | TYPE_KEY | 2},
	{(WCHAR *)L"Name", TYPE_VALID | TYPE_STRING | TYPE_NOT_BINARY | 64},
	{(WCHAR *)L"Type", TYPE_VALID | TYPE_NOT_BINARY | 2},
};
static const db_column streams_columns[] = {{(WCHAR *)L"Name", TYPE_VALID | TYPE_STRING | TYPE_NOT_BINARY | TYPE_KEY | 62}, {(WCHAR *)L"Data", TYPE_VALID | TYPE_STRING | TYPE_NULLABLE}};

static BOOL catalog_load(msi_db *db)
{
	ULONG size = 0;
	BYTE *data = read_element(db->storage, L"_Tables", TRUE, &size);
	UINT ref = db->ref_size;
	if (data) {
		for (ULONG at = 0; at + ref <= size; at += ref) {
			UINT id = data[at] | (data[at + 1] << 8) | (ref > 2 ? data[at + 2] << 16 : 0);
			const WCHAR *name = pool_text(db, id);
			if (name && !db_find_table(db, name)) {
				db_table *table = table_alloc(name);
				table->on_disk = TRUE;
				table_link(db, table);
			}
		}
		msi_free(data);
	}
	data = read_element(db->storage, L"_Columns", TRUE, &size);
	if (data) {
		UINT row_bytes = ref + 2 + ref + 2, rows = size / row_bytes;
		const BYTE *tables = data, *numbers = tables + rows * ref, *names = numbers + rows * 2, *types = names + rows * ref;
		for (UINT pass = 1; pass <= 512; pass++) {
			BOOL any = FALSE;
			for (UINT r = 0; r < rows; r++) {
				UINT number = (numbers[r * 2] | (numbers[r * 2 + 1] << 8)) - 0x8000;
				if (number != pass)
					continue;
				any = TRUE;
				UINT table_id = tables[r * ref] | (tables[r * ref + 1] << 8) | (ref > 2 ? tables[r * ref + 2] << 16 : 0);
				UINT name_id = names[r * ref] | (names[r * ref + 1] << 8) | (ref > 2 ? names[r * ref + 2] << 16 : 0);
				UINT type = (types[r * 2] | (types[r * 2 + 1] << 8)) - 0x8000;
				const WCHAR *table_name = pool_text(db, table_id);
				db_table *table = table_name ? db_find_table(db, table_name) : NULL;
				if (!table) {
					if (!table_name)
						continue;
					table = table_alloc(table_name);
					table->on_disk = TRUE;
					table_link(db, table);
				}
				table->columns = msi_grow(table->columns, (table->column_count + 1) * sizeof(db_column));
				table->columns[table->column_count].name = msi_copy(pool_text(db, name_id));
				table->columns[table->column_count].type = type & 0xffff;
				table->column_count++;
			}
			if (!any)
				break;
		}
		msi_free(data);
	}
	return TRUE;
}

static void catalog_init(msi_db *db)
{
	db->table_catalog = catalog_table(db, L"_Tables", tables_columns, 1);
	db->column_catalog = catalog_table(db, L"_Columns", columns_columns, 4);
	catalog_table(db, L"_Streams", streams_columns, 2);
	catalog_table(db, L"_Storages", streams_columns, 2);
}

static void table_free(db_table *table)
{
	for (UINT r = 0; r < table->row_count; r++) {
		msi_free(table->rows[r]->cells);
		msi_free(table->rows[r]);
	}
	msi_free(table->rows);
	for (UINT c = 0; c < table->column_count; c++)
		msi_free(table->columns[c].name);
	msi_free(table->columns);
	msi_free(table->name);
	msi_free(table);
}

// Rows handed out to views stay readable until the table goes: removal only marks them.
static void table_clear_rows(db_table *table)
{
	for (UINT r = 0; r < table->row_count; r++)
		table->rows[r]->dead = TRUE;
	table->row_count = 0;
}

static void refresh_elements(msi_db *db, db_table *table, DWORD kind)
{
	table_clear_rows(table);
	IEnumSTATSTG *items = NULL;
	if (db->storage && SUCCEEDED(IStorage_EnumElements(db->storage, 0, NULL, 0, &items))) {
		STATSTG stat;
		ULONG got;
		while (IEnumSTATSTG_Next(items, 1, &stat, &got) == S_OK && got) {
			WCHAR name[128];
			BOOL is_table = lstrlenW(stat.pwcsName) < 64 ? name_decode(stat.pwcsName, name) : TRUE;
			if (stat.type == kind && !is_table) {
				db_stream *pending = stream_find(db, name);
				if (!pending || (pending->blob && pending->storage == (kind == STGTY_STORAGE))) {
					UINT cells[2] = {pool_add(db, name, lstrlenW(name)), 1};
					row_push(table, cells, table->row_count);
				}
			}
			CoTaskMemFree(stat.pwcsName);
		}
		IEnumSTATSTG_Release(items);
	}
	for (db_stream *stream = db->streams; stream; stream = stream->next) {
		if (!stream->blob || stream->storage != (kind == STGTY_STORAGE))
			continue;
		UINT id = pool_add(db, stream->name, lstrlenW(stream->name)), found = 0;
		for (UINT r = 0; r < table->row_count; r++)
			if (table->rows[r]->cells[0] == id)
				found = 1;
		if (!found) {
			UINT cells[2] = {id, 1};
			row_push(table, cells, table->row_count);
		}
	}
}

void db_refresh_virtual(msi_db *db)
{
	table_clear_rows(db->table_catalog);
	table_clear_rows(db->column_catalog);
	for (db_table *table = db->tables; table; table = table->next) {
		if (table->virtual_table)
			continue;
		UINT id = pool_add(db, table->name, lstrlenW(table->name));
		table_insert(db, db->table_catalog, &id, table->temporary);
		if (table->temporary)
			continue;
		for (UINT c = 0; c < table->column_count; c++) {
			if (table->columns[c].type & TYPE_TEMPORARY)
				continue;
			UINT cells[4] = {id, cell_from_int(2, c + 1), pool_add(db, table->columns[c].name, lstrlenW(table->columns[c].name)), cell_from_int(2, table->columns[c].type)};
			table_insert(db, db->column_catalog, cells, table->temporary);
		}
	}
	db_table *streams = db_find_table(db, L"_Streams"), *storages = db_find_table(db, L"_Storages");
	refresh_elements(db, streams, STGTY_STREAM);
	refresh_elements(db, storages, STGTY_STORAGE);
}

static void db_destroy(msi_object *object)
{
	msi_db *db = (msi_db *)object;
	if (db->mode == 2 && db->dirty)
		db_commit(db);
	if (db->storage)
		IStorage_Release(db->storage);
	// A database created here exists only as far as somebody committed it.
	if (db->uncommitted_file && db->path)
		DeleteFileW(db->path);
	while (db->tables) {
		db_table *next = db->tables->next;
		table_free(db->tables);
		db->tables = next;
	}
	while (db->dropped) {
		db_table *next = db->dropped->next;
		table_free(db->dropped);
		db->dropped = next;
	}
	while (db->streams) {
		db_stream *next = db->streams->next;
		blob_unref(db->streams->blob);
		msi_free(db->streams->name);
		msi_free(db->streams);
		db->streams = next;
	}
	for (UINT i = 1; i < db->pool_count; i++)
		msi_free(db->pool[i].text);
	msi_free(db->pool);
	msi_free(db->pool_hash);
	msi_free(db->path);
	msi_free(db->target);
	msi_free(db->applied_transforms);
}

UINT db_open(const WCHAR *path, const WCHAR *persist, msi_db **result)
{
	ULONG_PTR raw = (ULONG_PTR)persist;
	UINT mode = 0;
	BOOL named = raw > 0xffff, patch = FALSE;
	if (!named) {
		patch = (raw & 32) != 0;
		mode = (UINT)(raw & ~(ULONG_PTR)32);
		if (mode > 4)
			return ERROR_INVALID_PARAMETER;
	} else {
		mode = 1;
	}
	msi_db *db = msi_alloc(sizeof(*db));
	object_init(&db->base, KIND_DATABASE, db_destroy);
	db->mode = mode;
	db->target = msi_copy(named ? persist : path);
	HRESULT hr;
	GUID clsid = patch ? clsid_patch : clsid_database;
	if (mode == 3 || mode == 4) {
		db->path = msi_copy(path);
		db->uncommitted_file = TRUE;
		hr = StgCreateDocfile(db->path, STGM_CREATE | STGM_DIRECT | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &db->storage);
		if (FAILED(hr)) {
			object_unref(&db->base);
			return ERROR_FUNCTION_FAILED;
		}
		IStorage_SetClass(db->storage, &clsid);
		db->clsid = clsid;
		db->pool_count = 1;
		db->dirty = TRUE;
		db->ref_size = 2;
		catalog_init(db);
		*result = db;
		return ERROR_SUCCESS;
	}
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
		object_unref(&db->base);
		return ERROR_OPEN_FAILED;
	}
	// Changes stay in memory until a commit writes them; a separate output starts out as a copy nobody has committed.
	db->path = msi_copy(named ? persist : path);
	if (named) {
		if (!CopyFileW(path, persist, FALSE)) {
			object_unref(&db->base);
			return ERROR_OPEN_FAILED;
		}
		SetFileAttributesW(persist, FILE_ATTRIBUTE_NORMAL);
		db->uncommitted_file = TRUE;
	}
	hr = StgOpenStorage(db->path, NULL, mode ? STGM_DIRECT | STGM_READWRITE | STGM_SHARE_EXCLUSIVE : STGM_DIRECT | STGM_READ | STGM_SHARE_DENY_WRITE, NULL, 0, &db->storage);
	STATSTG stat;
	if (FAILED(hr) || FAILED(IStorage_Stat(db->storage, &stat, STATFLAG_NONAME))) {
		object_unref(&db->base);
		return ERROR_OPEN_FAILED;
	}
	db->clsid = stat.clsid;
	if (!IsEqualGUID(&stat.clsid, &clsid_database) && !IsEqualGUID(&stat.clsid, &clsid_patch) && !IsEqualGUID(&stat.clsid, &clsid_transform)) {
		object_unref(&db->base);
		return ERROR_OPEN_FAILED;
	}
	UINT ref_size = 2;
	catalog_init(db);
	if (!pool_load(db, &ref_size)) {
		object_unref(&db->base);
		return ERROR_OPEN_FAILED;
	}
	db->ref_size = ref_size;
	catalog_load(db);
	*result = db;
	return ERROR_SUCCESS;
}

UINT cell_from_int(UINT type, INT value)
{
	if (value == MSI_NULL_INTEGER)
		return 0;
	if ((type & TYPE_SIZE) <= 2)
		return ((UINT)value + 0x8000) & 0xffff;
	return (UINT)value + 0x80000000u;
}

INT cell_to_int(UINT type, UINT cell)
{
	if (!cell)
		return MSI_NULL_INTEGER;
	if ((type & TYPE_SIZE) <= 2)
		return (INT)cell - 0x8000;
	return (INT)(cell - 0x80000000u);
}

int table_compare_keys(msi_db *db, db_table *table, const UINT *a, const UINT *b)
{
	(void)db;
	// Strings order by their place in the pool, which is the order they first appeared in.
	for (UINT c = 0; c < table->column_count; c++)
		if ((table->columns[c].type & TYPE_KEY) && a[c] != b[c])
			return a[c] < b[c] ? -1 : 1;
	return 0;
}

db_row *table_find_key(msi_db *db, db_table *table, const UINT *cells)
{
	BOOL keyed = FALSE;
	for (UINT c = 0; c < table->column_count; c++)
		if (table->columns[c].type & TYPE_KEY)
			keyed = TRUE;
	if (!keyed)
		return NULL;
	for (UINT r = 0; r < table->row_count; r++)
		if (!table_compare_keys(db, table, table->rows[r]->cells, cells))
			return table->rows[r];
	return NULL;
}

// Rows keep the order of their keys, as the installer's sequences and joins expect of a table.
db_row *table_insert(msi_db *db, db_table *table, const UINT *cells, BOOL temporary)
{
	UINT index = table->row_count;
	BOOL keyed = FALSE;
	for (UINT c = 0; c < table->column_count; c++)
		if (table->columns[c].type & TYPE_KEY)
			keyed = TRUE;
	if (keyed)
		while (index && table_compare_keys(db, table, table->rows[index - 1]->cells, cells) > 0)
			index--;
	db_row *row = row_push(table, cells, index);
	row->temporary = temporary;
	if (!temporary && !table->temporary)
		db->dirty = TRUE;
	return row;
}

void table_delete(msi_db *db, db_table *table, db_row *row)
{
	for (UINT r = 0; r < table->row_count; r++)
		if (table->rows[r] == row) {
			for (UINT c = 0; c < table->column_count; c++)
				if (TYPE_IS_BINARY(table->columns[c].type) && row->cells[c]) {
					WCHAR *name = row_stream_name(db, table, row->cells, c);
					stream_remove(db, name);
					msi_free(name);
				}
			memmove(table->rows + r, table->rows + r + 1, (table->row_count - r - 1) * sizeof(*table->rows));
			table->row_count--;
			row->dead = TRUE;
			db->dirty = TRUE;
			return;
		}
}

WCHAR *row_stream_name(msi_db *db, db_table *table, const UINT *cells, UINT column)
{
	(void)column;
	SIZE_T room = lstrlenW(table->name) + 2;
	for (UINT c = 0; c < table->column_count; c++) {
		if (!(table->columns[c].type & TYPE_KEY))
			continue;
		const WCHAR *text = (table->columns[c].type & TYPE_STRING) ? pool_text(db, cells[c]) : NULL;
		room += (text ? lstrlenW(text) : 12) + 1;
	}
	WCHAR *name = msi_alloc(room * sizeof(WCHAR));
	lstrcpyW(name, table->name);
	for (UINT c = 0; c < table->column_count; c++) {
		if (!(table->columns[c].type & TYPE_KEY))
			continue;
		lstrcatW(name, L".");
		if (table->columns[c].type & TYPE_STRING) {
			const WCHAR *text = pool_text(db, cells[c]);
			if (text)
				lstrcatW(name, text);
		} else {
			WCHAR digits[16];
			wsprintfW(digits, L"%d", cell_to_int(table->columns[c].type, cells[c]));
			lstrcatW(name, digits);
		}
	}
	return name;
}

db_stream *stream_find(msi_db *db, const WCHAR *name)
{
	for (db_stream *stream = db->streams; stream; stream = stream->next)
		if (!lstrcmpW(stream->name, name))
			return stream;
	return NULL;
}

void stream_put(msi_db *db, const WCHAR *name, msi_blob *blob)
{
	db_stream *stream = stream_find(db, name);
	if (!stream) {
		stream = msi_alloc(sizeof(*stream));
		stream->name = msi_copy(name);
		stream->next = db->streams;
		db->streams = stream;
	}
	blob_unref(stream->blob);
	stream->blob = blob_ref(blob);
	stream->dirty = TRUE;
	db->dirty = TRUE;
}

// A stream with no bytes stands for one to destroy when the changes are committed.
void stream_remove(msi_db *db, const WCHAR *name)
{
	stream_put(db, name, NULL);
}

msi_blob *db_read_named_stream(msi_db *db, const WCHAR *name)
{
	db_stream *stream = stream_find(db, name);
	if (stream)
		return blob_ref(stream->blob);
	ULONG size = 0;
	BYTE *data = read_element(db->storage, name, FALSE, &size);
	if (!data)
		return NULL;
	msi_blob *blob = blob_new(NULL, 0);
	msi_free(blob->data);
	blob->data = data;
	blob->size = size;
	return blob;
}

/* A stream of the database written into a file a piece at a time, as an embedded cabinet of hundreds of megabytes is: FALSE when there is
 * no such stream or the copy fails. */
BOOL db_copy_named_stream(msi_db *db, const WCHAR *name, HANDLE file)
{
	DWORD put = 0;
	db_stream *pending = stream_find(db, name);
	if (pending)
		return WriteFile(file, pending->blob->data, (DWORD)pending->blob->size, &put, NULL) && put == (DWORD)pending->blob->size;
	WCHAR *encoded = msi_alloc((lstrlenW(name) + 2) * sizeof(WCHAR));
	IStream *stream = NULL;
	name_encode(name, FALSE, encoded);
	HRESULT hr = IStorage_OpenStream(db->storage, encoded, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream);
	msi_free(encoded);
	if (FAILED(hr))
		return FALSE;
	enum { piece = 1 << 20 };
	BYTE *buffer = msi_alloc(piece);
	BOOL ok = buffer != NULL;
	while (ok) {
		ULONG done = 0;
		hr = IStream_Read(stream, buffer, piece, &done);
		if (FAILED(hr))
			ok = FALSE;
		else if (!done)
			break;
		else
			ok = WriteFile(file, buffer, done, &put, NULL) && put == done;
	}
	msi_free(buffer);
	IStream_Release(stream);
	return ok;
}

UINT row_to_record(msi_db *db, db_table *table, const UINT *cells, const UINT *columns, UINT count, msi_record *record, UINT first)
{
	for (UINT i = 0; i < count; i++) {
		UINT c = columns ? columns[i] : i;
		UINT type = table->columns[c].type;
		UINT field = first + i;
		if (TYPE_IS_BINARY(type)) {
			if (!cells[c])
				continue;
			if (table->virtual_table && !lstrcmpW(table->name, L"_Storages")) {
				record_set_blob(record, field, NULL);
				continue;
			}
			WCHAR *name;
			if (table->virtual_table)
				name = msi_copy(pool_text(db, cells[0]));
			else
				name = row_stream_name(db, table, cells, c);
			msi_blob *blob = db_read_named_stream(db, name);
			msi_free(name);
			if (blob) {
				record_set_blob(record, field, blob);
				blob_unref(blob);
			}
		} else if (type & TYPE_STRING) {
			record_set_text(record, field, pool_text(db, cells[c]));
		} else {
			record_set_int(record, field, cell_to_int(type, cells[c]));
		}
	}
	return ERROR_SUCCESS;
}

UINT db_add_column(msi_db *db, db_table *table, const WCHAR *name, UINT type, UINT number)
{
	(void)number;
	db_load_table(db, table);
	table->columns = msi_grow(table->columns, (table->column_count + 1) * sizeof(db_column));
	table->columns[table->column_count].name = msi_copy(name);
	table->columns[table->column_count].type = type;
	table->column_count++;
	for (UINT r = 0; r < table->row_count; r++) {
		table->rows[r]->cells = msi_grow(table->rows[r]->cells, (table->column_count + 1) * sizeof(UINT));
		table->rows[r]->cells[table->column_count - 1] = 0;
	}
	db->dirty = TRUE;
	return ERROR_SUCCESS;
}

void db_drop_temporary_columns(db_table *table)
{
	while (table->column_count && (table->columns[table->column_count - 1].type & TYPE_TEMPORARY)) {
		table->column_count--;
		msi_free(table->columns[table->column_count].name);
		table->columns[table->column_count].name = NULL;
	}
}

UINT db_create_table(msi_db *db, const WCHAR *name, const db_column *columns, UINT count, BOOL temporary, db_table **result)
{
	if (db_find_table(db, name))
		return ERROR_BAD_QUERY_SYNTAX;
	db_table *table = table_alloc(name);
	table->temporary = temporary;
	table->loaded = TRUE;
	table->column_count = count;
	table->columns = msi_alloc(count * sizeof(db_column));
	for (UINT i = 0; i < count; i++) {
		table->columns[i].name = msi_copy(columns[i].name);
		table->columns[i].type = columns[i].type;
	}
	table_link(db, table);
	if (!temporary)
		db->dirty = TRUE;
	if (result)
		*result = table;
	return ERROR_SUCCESS;
}

void db_drop_table(msi_db *db, db_table *table)
{
	db_load_table(db, table);
	for (UINT r = table->row_count; r; r--)
		table_delete(db, table, table->rows[r - 1]);
	if (table->on_disk) {
		// The table's own stream goes on commit: its name carries the table mark that a pending entry can spell.
		WCHAR *marked = msi_alloc((lstrlenW(table->name) + 2) * sizeof(WCHAR));
		marked[0] = 0x4840;
		lstrcpyW(marked + 1, table->name);
		stream_remove(db, marked);
		msi_free(marked);
	}
	db_table **at = &db->tables;
	while (*at != table)
		at = &(*at)->next;
	*at = table->next;
	// Views may still look at it: its memory goes with the database.
	table->next = db->dropped;
	db->dropped = table;
	db->dirty = TRUE;
}

// A row of _Storages brings a whole compound file as its bytes; inside the database it is a storage again.
static BOOL write_nested(IStorage *storage, const WCHAR *name, msi_blob *blob)
{
	ILockBytes *bytes = NULL;
	IStorage *source = NULL, *target = NULL;
	ULARGE_INTEGER zero = {{0, 0}};
	ULONG done;
	BOOL ok = FALSE;
	if (FAILED(CreateILockBytesOnHGlobal(NULL, TRUE, &bytes)))
		return FALSE;
	if (SUCCEEDED(ILockBytes_WriteAt(bytes, zero, blob->data, (ULONG)blob->size, &done))
		&& SUCCEEDED(StgOpenStorageOnILockBytes(bytes, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, NULL, 0, &source))) {
		IStorage_DestroyElement(storage, name);
		if (SUCCEEDED(IStorage_CreateStorage(storage, name, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &target))) {
			ok = SUCCEEDED(IStorage_CopyTo(source, 0, NULL, NULL, target));
			IStorage_Release(target);
		}
		IStorage_Release(source);
	}
	ILockBytes_Release(bytes);
	return ok;
}

typedef struct {
	UINT *map;
	UINT *refs;
	UINT next;
} pool_writer;

static UINT writer_id(pool_writer *writer, UINT id)
{
	if (!id)
		return 0;
	if (!writer->map[id])
		writer->map[id] = writer->next++;
	writer->refs[writer->map[id]]++;
	return writer->map[id];
}

static void put_cell(BYTE **at, UINT value, UINT bytes)
{
	for (UINT i = 0; i < bytes; i++)
		*(*at)++ = (BYTE)(value >> (8 * i));
}

static BOOL write_table(msi_db *db, db_table *table, pool_writer *writer, UINT ref_size)
{
	UINT row_bytes = 0, rows = 0;
	for (UINT c = 0; c < table->column_count; c++)
		if (!(table->columns[c].type & TYPE_TEMPORARY))
			row_bytes += column_bytes(table->columns[c].type, ref_size);
	// The file keeps only the rows ahead of the first temporary one.
	while (rows < table->row_count && !table->rows[rows]->temporary)
		rows++;
	BYTE *data = msi_alloc(row_bytes * rows), *at = data;
	for (UINT c = 0; c < table->column_count; c++) {
		UINT type = table->columns[c].type;
		if (type & TYPE_TEMPORARY)
			continue;
		UINT bytes = column_bytes(type, ref_size);
		for (UINT r = 0; r < rows; r++) {
			UINT cell = table->rows[r]->cells[c];
			put_cell(&at, ((type & TYPE_STRING) && !TYPE_IS_BINARY(type)) ? writer_id(writer, cell) : cell, bytes);
		}
	}
	BOOL ok = write_element(db->storage, table->name, TRUE, data, row_bytes * rows);
	msi_free(data);
	return ok;
}

// Everything is written out again with the pool renumbered, which keeps the file free of strings nothing refers to.
static UINT write_all(msi_db *db)
{
	pool_writer writer = {msi_alloc((db->pool_count + 1) * sizeof(UINT)), NULL, 1};
	UINT table_count = 0, column_count = 0;
	writer.refs = msi_alloc((db->pool_count + 16 + 1) * sizeof(UINT));
	for (db_table *table = db->tables; table; table = table->next) {
		if (table->virtual_table || table->temporary)
			continue;
		db_load_table(db, table);
		table_count++;
		for (UINT c = 0; c < table->column_count; c++)
			if (!(table->columns[c].type & TYPE_TEMPORARY))
				column_count++;
	}
	UINT strings = db->pool_count + 1;
	UINT ref_size = strings > 0xffff ? 3 : 2;
	BYTE *tables = msi_alloc(table_count * ref_size + 1), *tables_at = tables;
	BYTE *columns = msi_alloc(column_count * (ref_size * 2 + 4) + 1);
	BYTE *col_tables = columns, *col_numbers = col_tables + column_count * ref_size, *col_names = col_numbers + column_count * 2, *col_types = col_names + column_count * ref_size;
	for (db_table *table = db->tables; table; table = table->next) {
		if (table->virtual_table || table->temporary)
			continue;
		UINT name = pool_add(db, table->name, lstrlenW(table->name));
		if (name >= strings) {
			strings = name + 1;
			writer.map = msi_grow(writer.map, (strings + 256) * sizeof(UINT));
			writer.refs = msi_grow(writer.refs, (strings + 256) * sizeof(UINT));
		}
		put_cell(&tables_at, writer_id(&writer, name), ref_size);
		UINT number = 0;
		for (UINT c = 0; c < table->column_count; c++) {
			if (table->columns[c].type & TYPE_TEMPORARY)
				continue;
			UINT column_name = pool_add(db, table->columns[c].name, lstrlenW(table->columns[c].name));
			if (column_name >= strings) {
				strings = column_name + 1;
				writer.map = msi_grow(writer.map, (strings + 256) * sizeof(UINT));
				writer.refs = msi_grow(writer.refs, (strings + 256) * sizeof(UINT));
			}
			put_cell(&col_tables, writer_id(&writer, name), ref_size);
			put_cell(&col_numbers, 0x8000 + ++number, 2);
			put_cell(&col_names, writer_id(&writer, column_name), ref_size);
			put_cell(&col_types, 0x8000 + table->columns[c].type, 2);
		}
	}
	UINT status = ERROR_SUCCESS;
	if (!write_element(db->storage, L"_Tables", TRUE, tables, table_count * ref_size)
		|| (column_count && !write_element(db->storage, L"_Columns", TRUE, columns, column_count * (ref_size * 2 + 4))))
		status = ERROR_FUNCTION_FAILED;
	if (!column_count)
		destroy_element(db->storage, L"_Columns", TRUE);
	msi_free(tables);
	msi_free(columns);
	for (db_table *table = db->tables; table && !status; table = table->next) {
		if (table->virtual_table || table->temporary)
			continue;
		if (!write_table(db, table, &writer, ref_size))
			status = ERROR_FUNCTION_FAILED;
		table->on_disk = TRUE;
	}
	for (db_stream *stream = db->streams; stream && !status; stream = stream->next) {
		if (!stream->dirty)
			continue;
		if (!stream->blob) {
			if (stream->name[0] == 0x4840)
				destroy_element(db->storage, stream->name + 1, TRUE);
			else
				destroy_element(db->storage, stream->name, FALSE);
		} else if (stream->storage ? !write_nested(db->storage, stream->name, stream->blob) : !write_element(db->storage, stream->name, FALSE, stream->blob->data, (ULONG)stream->blob->size)) {
			status = ERROR_FUNCTION_FAILED;
		}
		stream->dirty = FALSE;
	}
	// The pool last, in the new numbering.
	UINT used = writer.next;
	USHORT *entries = msi_alloc((used + 2) * 8);
	const WCHAR **texts = msi_alloc((used + 1) * sizeof(*texts));
	UINT *lengths = msi_alloc((used + 1) * sizeof(UINT));
	for (UINT id = 1; id < db->pool_count && id < strings; id++)
		if (writer.map[id]) {
			texts[writer.map[id]] = db->pool[id].text;
			lengths[writer.map[id]] = db->pool[id].length;
		}
	SIZE_T data_room = 16, data_size = 0;
	for (UINT i = 1; i < used; i++)
		data_room += lengths[i] * 4;
	BYTE *data = msi_alloc(data_room);
	UINT n = 2;
	entries[0] = (USHORT)(db->codepage & 0xffff);
	entries[1] = (USHORT)((db->codepage >> 16) & 0x7fff) | (ref_size > 2 ? 0x8000 : 0);
	for (UINT i = 1; i < used; i++) {
		INT bytes = WideCharToMultiByte(effective_codepage(db), 0, texts[i], lengths[i], (char *)data + data_size, (INT)(data_room - data_size), NULL, NULL);
		data_size += bytes;
		if (bytes >= 0x10000) {
			entries[n++] = 0;
			entries[n++] = (USHORT)(bytes >> 16);
		}
		entries[n++] = (USHORT)bytes;
		entries[n++] = (USHORT)writer.refs[i];
	}
	if (!status && (!write_element(db->storage, L"_StringPool", TRUE, entries, n * 2) || !write_element(db->storage, L"_StringData", TRUE, data, (ULONG)data_size)))
		status = ERROR_FUNCTION_FAILED;
	msi_free(entries);
	msi_free(texts);
	msi_free(lengths);
	msi_free(data);
	msi_free(writer.map);
	msi_free(writer.refs);
	db->ref_size = ref_size;
	return status;
}

// Only what reading strings takes: a transform's pool and its tables are not a database of their own.
UINT db_open_on_storage(IStorage *storage, msi_db **result)
{
	msi_db *db = msi_alloc(sizeof(*db));
	object_init(&db->base, KIND_DATABASE, db_destroy);
	IStorage_AddRef(storage);
	db->storage = storage;
	db->ref_size = 2;
	if (!pool_load(db, &db->ref_size)) {
		object_unref(&db->base);
		return ERROR_FUNCTION_FAILED;
	}
	*result = db;
	return ERROR_SUCCESS;
}

UINT db_commit(msi_db *db)
{
	if (DB_READONLY(db))
		return ERROR_SUCCESS;
	UINT status = write_all(db);
	if (status)
		return status;
	IStorage_Commit(db->storage, 0);
	db->dirty = FALSE;
	db->uncommitted_file = FALSE;
	return ERROR_SUCCESS;
}
