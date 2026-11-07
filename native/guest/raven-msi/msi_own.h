/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_MSI_OWN_H
#define RAVEN_MSI_OWN_H

#define WIN32_LEAN_AND_MEAN
#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef _WIN32_MSI
#define _WIN32_MSI 500
#endif
#include <windows.h>
#include <objbase.h>
#include <msi.h>
#include <msiquery.h>
#include <msidefs.h>

typedef enum {
	KIND_RECORD = 1,
	KIND_DATABASE,
	KIND_VIEW,
	KIND_SUMMARY,
	KIND_PACKAGE,
} object_kind;

typedef struct msi_object {
	object_kind kind;
	LONG refs;
	void (*destroy)(struct msi_object *object);
} msi_object;

// The summary stream's property for the source image flags.
#define PID_WORDCOUNT 15

// memory.c
void *msi_alloc(SIZE_T size);
void *msi_grow(void *memory, SIZE_T size);
void msi_free(void *memory);
WCHAR *msi_copy(const WCHAR *text);
WCHAR *msi_copy_n(const WCHAR *text, SIZE_T length);
WCHAR *msi_from_ansi(const char *text);
char *msi_to_ansi(const WCHAR *text);
BOOL same_text(const WCHAR *a, const WCHAR *b);
// Answers the *_W and *_A pairs of the API: a length without the terminator, in the caller's characters.
UINT out_text_w(const WCHAR *text, SIZE_T length, WCHAR *out, DWORD *size);
UINT out_text_a(const WCHAR *text, SIZE_T length, char *out, DWORD *size);

// handle.c
MSIHANDLE handle_open(msi_object *object);
msi_object *handle_lookup(MSIHANDLE handle, object_kind kind);
void object_init(msi_object *object, object_kind kind, void (*destroy)(msi_object *));
void object_ref(msi_object *object);
void object_unref(msi_object *object);

// A stream's bytes in memory, shared between records, views and the database.
typedef struct {
	LONG refs;
	BYTE *data;
	SIZE_T size;
} msi_blob;

msi_blob *blob_new(const void *data, SIZE_T size);
msi_blob *blob_ref(msi_blob *blob);
void blob_unref(msi_blob *blob);
msi_blob *blob_from_file(const WCHAR *path);

// record.c
typedef enum { FIELD_NULL, FIELD_INT, FIELD_TEXT, FIELD_BLOB } field_type;

typedef struct {
	field_type type;
	INT number;
	WCHAR *text;
	msi_blob *blob;
	SIZE_T position;
} record_field;

typedef struct {
	msi_object base;
	UINT count;
	record_field fields[1];
} msi_record;

msi_record *record_new(UINT count);
UINT record_set_int(msi_record *record, UINT field, INT value);
UINT record_set_text(msi_record *record, UINT field, const WCHAR *text);
UINT record_set_blob(msi_record *record, UINT field, msi_blob *blob);
BOOL record_is_null(const msi_record *record, UINT field);
INT record_get_int(const msi_record *record, UINT field);
// NULL for a field holding neither text nor a number; the number's digits go in the scratch space.
const WCHAR *record_text(const msi_record *record, UINT field, WCHAR scratch[16]);
void record_clear(msi_record *record);
msi_record *record_copy(const msi_record *record);

// dbfile.c
enum {
	TYPE_SIZE = 0x00ff,
	TYPE_VALID = 0x0100,
	TYPE_LOCALIZABLE = 0x0200,
	TYPE_NOT_BINARY = 0x0400,
	TYPE_STRING = 0x0800,
	TYPE_NULLABLE = 0x1000,
	TYPE_KEY = 0x2000,
	TYPE_TEMPORARY = 0x4000,
};

#define TYPE_IS_BINARY(type) (((type) & (TYPE_STRING | TYPE_NOT_BINARY)) == TYPE_STRING)
#define TYPE_IS_TEXT(type) (((type) & (TYPE_STRING | TYPE_NOT_BINARY)) == (TYPE_STRING | TYPE_NOT_BINARY))

typedef struct {
	WCHAR *name;
	UINT type;
} db_column;

// A cell is what the file stores: a string's id, a number with the offset that leaves 0 for null, 1 for a stream.
typedef struct db_row {
	UINT *cells;
	BOOL dead;
	BOOL temporary;
} db_row;

typedef struct db_table {
	struct db_table *next;
	WCHAR *name;
	db_column *columns;
	UINT column_count;
	db_row **rows;
	UINT row_count, row_room;
	LONG holds;
	BOOL temporary;
	BOOL loaded;
	BOOL virtual_table;
	BOOL on_disk;
} db_table;

typedef struct {
	WCHAR *text;
	UINT length;
	UINT next;		// hash chain
} pool_entry;

typedef struct db_stream {
	struct db_stream *next;
	WCHAR *name;
	msi_blob *blob;
	BOOL dirty;
	BOOL storage;
} db_stream;

typedef struct {
	msi_object base;
	IStorage *storage;
	WCHAR *path;		// the file the storage is open on
	WCHAR *target;		// where a commit ends up
	UINT mode;
	BOOL patch;
	BOOL dirty;
	BOOL uncommitted_file;
	UINT codepage;
	pool_entry *pool;
	UINT pool_count, pool_room;
	UINT *pool_hash;
	UINT pool_hash_size;
	db_table *tables, *dropped;
	UINT ref_size;
	db_table *table_catalog, *column_catalog;
	db_stream *streams;
	GUID clsid;
	WCHAR *applied_transforms;
	void *summary_pending;
	UINT media_transform_offset, media_transform_disk;
	HANDLE remote;		// in a custom action host: the installer whose session this is a copy of
} msi_db;

#define DB_READONLY(db) ((db)->mode == (UINT)(ULONG_PTR)MSIDBOPEN_READONLY)

UINT db_open(const WCHAR *path, const WCHAR *persist, msi_db **db);
UINT db_commit(msi_db *db);
UINT db_open_on_storage(IStorage *storage, msi_db **db);
UINT db_apply_transform(msi_db *db, const WCHAR *path, INT conditions);
UINT db_apply_storage(msi_db *db, IStorage *storage, INT conditions);
UINT pool_add(msi_db *db, const WCHAR *text, SIZE_T length);
UINT pool_find(msi_db *db, const WCHAR *text);
const WCHAR *pool_text(msi_db *db, UINT id);
db_table *db_find_table(msi_db *db, const WCHAR *name);
UINT db_load_table(msi_db *db, db_table *table);
UINT db_create_table(msi_db *db, const WCHAR *name, const db_column *columns, UINT count, BOOL temporary, db_table **result);
void db_drop_table(msi_db *db, db_table *table);
UINT db_add_column(msi_db *db, db_table *table, const WCHAR *name, UINT type, UINT number);
void db_refresh_virtual(msi_db *db);
void db_drop_temporary_columns(db_table *table);
db_row *table_insert(msi_db *db, db_table *table, const UINT *cells, BOOL temporary);
void table_delete(msi_db *db, db_table *table, db_row *row);
int table_compare_keys(msi_db *db, db_table *table, const UINT *a, const UINT *b);
db_row *table_find_key(msi_db *db, db_table *table, const UINT *cells);
UINT cell_from_int(UINT type, INT value);
INT cell_to_int(UINT type, UINT cell);
WCHAR *row_stream_name(msi_db *db, db_table *table, const UINT *cells, UINT column);
db_stream *stream_find(msi_db *db, const WCHAR *name);
void stream_put(msi_db *db, const WCHAR *name, msi_blob *blob);
void stream_remove(msi_db *db, const WCHAR *name);
msi_blob *db_read_named_stream(msi_db *db, const WCHAR *name);
BOOL db_copy_named_stream(msi_db *db, const WCHAR *name, HANDLE file);
void name_encode(const WCHAR *in, BOOL table, WCHAR *out);
BOOL name_decode(const WCHAR *in, WCHAR *out);
UINT row_to_record(msi_db *db, db_table *table, const UINT *cells, const UINT *columns, UINT count, msi_record *record, UINT first);

// sql.c
typedef enum { OP_AND, OP_OR, OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE, OP_IS_NULL, OP_NOT_NULL, EXPR_COLUMN, EXPR_NUMBER, EXPR_TEXT, EXPR_PARAM } expr_kind;

typedef struct sql_expr {
	expr_kind kind;
	struct sql_expr *left, *right;
	WCHAR *table, *name;
	INT number;
	UINT param;
	// resolved:
	UINT table_index, column_index;
} sql_expr;

typedef struct sql_column {
	WCHAR *table, *name;
	UINT type;
	BOOL hold_ignored;
	sql_expr *value;
} sql_column;

typedef enum { SQL_SELECT, SQL_INSERT, SQL_UPDATE, SQL_DELETE, SQL_CREATE, SQL_ALTER, SQL_DROP } sql_kind;

typedef struct {
	sql_kind kind;
	BOOL distinct, temporary, hold, free_table, add_column, star;
	WCHAR *tables[32];
	UINT table_count;
	sql_column *columns;
	UINT column_count;
	sql_column *order;
	UINT order_count;
	WCHAR **keys;
	UINT key_count;
	sql_expr *where;
	UINT param_count;
} sql_statement;

UINT sql_parse(const WCHAR *text, sql_statement **statement);
void sql_free(sql_statement *statement);

// query.c
UINT view_open(msi_db *db, const WCHAR *query, MSIHANDLE *handle);
UINT db_run_query(msi_db *db, const WCHAR *query);
UINT db_query_each(msi_db *db, const WCHAR *query, msi_record *params, UINT (*callback)(msi_record *row, void *context), void *context);

// summary.c
UINT summary_open_database(msi_db *db, UINT updates, MSIHANDLE *handle);

// package.c
typedef struct { WCHAR *name, *value; } msi_property;
typedef struct msi_directory msi_directory;
typedef struct msi_feature msi_feature;
typedef struct msi_component msi_component;
typedef struct msi_file msi_file;

struct msi_directory {
	WCHAR *key, *parent, *target_name, *source_name;
	WCHAR *target;		// resolved, with the trailing backslash
	BOOL resolving;
	BOOL published;		// its target stands in the property of its name
};

struct msi_component {
	WCHAR *key, *guid, *directory, *condition, *key_path;
	INT attributes;
	INSTALLSTATE installed, action;
	BOOL enabled;
	WCHAR *assembly_folder;	// for the global cache: where its files wait for it
	INT assembly_kind;
};

struct msi_feature {
	WCHAR *key, *parent, *title, *description, *directory;
	INT display, level, attributes;
	INSTALLSTATE installed, action;
	msi_component **components;
	UINT component_count;
};

struct msi_file {
	WCHAR *key, *name, *version, *language;
	msi_component *component;
	INT size, attributes, sequence;
	BOOL extracted;	// a newer cabinet already gave it
	BOOL has_delta;	// a Patch row says the cabinet holds a difference
	WCHAR *delta_path;
};

typedef struct {
	const void *action;	// a standard action, or NULL for a custom one
	WCHAR *name, *data;
	INT type;
	WCHAR *source, *target;
} script_entry;

// A rollback custom action is undone as the changes are: it runs, in reverse order, only when the installation fails after it.
typedef enum { UNDO_FILE, UNDO_DIRECTORY, UNDO_REGISTRY, UNDO_ACTION } undo_kind;

typedef struct {
	undo_kind kind;
	script_entry action;
	WCHAR *path, *backup;
	HKEY root;
	WCHAR *key, *name;
	BYTE *data;
	DWORD type, size;
	BOOL existed;
} undo_entry;

typedef struct {
	msi_object base;
	msi_db *db;
	HANDLE remote;		// in a custom action host: the pipe to the installing process
	CRITICAL_SECTION lock;
	msi_property *properties;
	UINT property_count, property_room;
	WCHAR *path;
	msi_directory *directories;
	UINT directory_count;
	msi_feature *features;
	UINT feature_count;
	msi_component *components;
	UINT component_count;
	msi_file *files;
	UINT file_count;
	BOOL tables_loaded, costed, directories_published;
	script_entry *script;
	UINT script_count, script_room;
	undo_entry *undo;
	UINT undo_count, undo_room;
	script_entry *commits;	// the commit custom actions the script passed, run once it succeeds
	UINT commit_count;
	BOOL in_script, scheduled, rollback, commit, reboot_at_end, reboot_now, nested;
	BOOL removing, maintenance, prepared, patched;
	WCHAR *command_line;	// kept: it beats what a patch brings to the Property table
	WCHAR *current_action;
	WCHAR squashed[33];
	MSIHANDLE session;
	void *ui;		// the dialogs, while the user interface sequence runs
} msi_package;

UINT package_open(const WCHAR *path, msi_package **package);
UINT package_open_db(msi_db *db, const WCHAR *path, msi_package **package);
msi_package *package_from_handle(MSIHANDLE handle);
WCHAR *package_get(msi_package *package, const WCHAR *name);	// NULL when unset; free it
INT package_get_int(msi_package *package, const WCHAR *name, INT fallback);
BOOL package_is(msi_package *package, const WCHAR *name, const WCHAR *value);
UINT package_set(msi_package *package, const WCHAR *name, const WCHAR *value);
UINT package_command_line(msi_package *package, const WCHAR *line);
void package_reload_properties(msi_package *package);
void package_message(msi_package *package, INSTALLMESSAGE type, const WCHAR *text);
void package_log(const WCHAR *format, ...);
const WCHAR *package_log_release(void);
UINT ui_deliver(INSTALLMESSAGE type, const WCHAR *text, UINT field_one);
INSTALLUILEVEL ui_internal_level(void);
BOOL guid_squash(const WCHAR *guid, WCHAR out[33]);
// Counts one use of an installed feature for MsiGetFeatureUsage.
void count_feature_use(LPCWSTR product, LPCWSTR feature);
BOOL guid_unsquash(const WCHAR *squashed, WCHAR out[39]);

// condition.c
MSICONDITION condition_evaluate(msi_package *package, const WCHAR *text);
BOOL condition_true(msi_package *package, const WCHAR *text);

// format.c
WCHAR *format_text(msi_package *package, const WCHAR *text, msi_record *record);
WCHAR *format_record(msi_package *package, msi_record *record);

// tables.c
UINT tables_load(msi_package *package);
msi_directory *directory_find(msi_package *package, const WCHAR *key);
msi_feature *feature_find(msi_package *package, const WCHAR *key);
msi_component *component_find(msi_package *package, const WCHAR *key);
msi_file *file_find(msi_package *package, const WCHAR *key);
const WCHAR *directory_target(msi_package *package, const WCHAR *key);
WCHAR *file_target(msi_package *package, const msi_file *file);
void directories_reset(msi_package *package);
void directories_publish(msi_package *package);
void states_select(msi_package *package);

// actions.c
UINT action_run(msi_package *package, const WCHAR *name);
UINT sequence_run(msi_package *package, const WCHAR *table);
UINT script_run(msi_package *package);
void script_rollback(msi_package *package);
UINT install_run(msi_package *package);
void undo_file(msi_package *package, const WCHAR *path, const WCHAR *backup);
void undo_directory(msi_package *package, const WCHAR *path);
void undo_registry(msi_package *package, HKEY root, const WCHAR *key, const WCHAR *name);
void script_entry_free(script_entry *entry);
UINT reg_write(msi_package *package, HKEY root, const WCHAR *key, const WCHAR *name, DWORD type, const void *data, DWORD size);
UINT reg_write_text(msi_package *package, HKEY root, const WCHAR *key, const WCHAR *name, const WCHAR *text);
UINT reg_write_number(msi_package *package, HKEY root, const WCHAR *key, const WCHAR *name, DWORD number);
UINT create_directories(msi_package *package, const WCHAR *path);

// actions2.c
typedef struct {
	const WCHAR *name;
	UINT (*call)(msi_package *package);
	BOOL scheduled;
} extra_action;
extern const extra_action extra_actions[];
void reg_set_view(REGSAM view);

// actions3.c
extern const extra_action more_actions[];
void assemblies_load(msi_package *package);
void costs_publish(msi_package *package);
UINT component_cost(msi_package *package, const msi_component *component);

// custom.c
UINT custom_action_run(msi_package *package, const WCHAR *name, const WCHAR *data, BOOL *handled);
UINT custom_scripted_run(msi_package *package, const script_entry *entry);

// script.c
UINT script_action_run(msi_package *package, MSIHANDLE session, const WCHAR *language, const BYTE *data, SIZE_T size, const WCHAR *entry,
	const WCHAR *action);

// remote.c
enum {
	REMOTE_GET_PROPERTY = 1, REMOTE_SET_PROPERTY, REMOTE_TARGET_PATH, REMOTE_SOURCE_PATH, REMOTE_SET_TARGET,
	REMOTE_MODE, REMOTE_SET_MODE, REMOTE_LANGUAGE, REMOTE_CONDITION, REMOTE_MESSAGE, REMOTE_FILE_PATH,
	REMOTE_COMPONENT_STATE, REMOTE_FEATURE_STATE, REMOTE_SET_COMPONENT, REMOTE_SET_FEATURE, REMOTE_DATABASE_PATH,
	REMOTE_DO_ACTION, REMOTE_SEQUENCE, REMOTE_INSERT_ROW,
};
UINT remote_call(HANDLE pipe, UINT op, const WCHAR *a, const WCHAR *b, INT number, WCHAR **text, INT *result);
DWORD WINAPI remote_serve(void *context);

// product.c
UINT WINAPI MsiDecomposeDescriptorW(LPCWSTR descriptor, LPWSTR product, LPWSTR feature, LPWSTR component, DWORD *used);
INSTALLSTATE WINAPI MsiGetComponentPathExW(LPCWSTR product, LPCWSTR component, LPCWSTR user, MSIINSTALLCONTEXT context, LPWSTR out, LPDWORD size);
typedef struct {
	BOOL machine;
	WCHAR sid[80];
} install_context;
UINT product_register(msi_package *package);
UINT product_unregister(msi_package *package);
UINT product_publish_components(msi_package *package);
UINT product_publish(msi_package *package);
UINT product_publish_features(msi_package *package);
INSTALLSTATE product_state(const WCHAR *product);
BOOL product_local_package(const WCHAR *product, WCHAR *path, DWORD room);
void product_user_sid(WCHAR out[80]);
BOOL package_is_machine(msi_package *package);
INSTALLSTATE product_component_installed(const WCHAR *product, const WCHAR *component);
INSTALLSTATE product_feature_installed(const WCHAR *product, const WCHAR *feature);
void user_data_key(WCHAR *key, BOOL machine, const WCHAR *rest);
void published_key(WCHAR *key, BOOL machine, const WCHAR *rest);

// patch.c
UINT package_prepare(msi_package *package);
UINT patches_register(msi_package *package);
void patches_forget(msi_package *package);

// ui.c
BOOL ui_wanted(msi_package *package, const WCHAR *sequence);
UINT ui_run(msi_package *package, const WCHAR *sequence, const WCHAR *execute);
BOOL ui_action(msi_package *package, const WCHAR *name, UINT *status);
void ui_action_started(msi_package *package, const WCHAR *name);
BOOL ui_progress(msi_package *package, UINT done, UINT total);	// FALSE once the user has cancelled

#endif
