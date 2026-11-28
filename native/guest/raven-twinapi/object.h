/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The objects of the Raven Windows Runtime component, as the classes in twinapi.c and core_window.c build them. */
#ifndef RAVEN_TWINAPI_OBJECT_H
#define RAVEN_TWINAPI_OBJECT_H

#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include <winstring.h>
#include <activation.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

/* What the Windows Runtime and Windows report when there is nothing to give. */
#define NO_PACKAGE ((HRESULT)0x80073d54)
#define NOT_FOUND ((HRESULT)0x80070490)
#define NO_SUCH_USER ((HRESULT)0x80070525)
#define DELEGATE_ASSIGNED ((HRESULT)0x80000018)
#define TYPE_MISMATCH ((HRESULT)0x80020005)

/* Objects alive, which decide whether the library may unload, and the last event token handed out. */
extern LONG objects, tokens;

extern const IID iid_unknown, iid_inspectable, iid_agile, iid_factory, iid_async_info;
/* IInitializeWithWindow, through which a desktop app gives a picker or a dialog the window that owns it: its table is UNKNOWN and then
 * Initialize. */
extern const IID iid_initialize_with_window;

/* An object answers several interfaces, each a table pointer at its own place at the start of the object. Each table is preceded by the
 * number of its place, so a method finds the object from the interface it was called through. */
#define FACES 8

struct face_iid {
	const IID *iid;
	SIZE_T face;
};

struct object;

struct kind {
	const char *name;
	const WCHAR *class_name;
	/* Ends with a null identifier. */
	const struct face_iid *iids;
	/* NULL for an object that lives as long as the library. */
	void (*destroy)(struct object *);
};

struct object {
	const void *const *faces[FACES];
	const struct kind *kind;
	LONG refs;
};

#define TABLE(name, place, ...) \
	static const struct { \
		SIZE_T face; \
		const void *slots[sizeof((const void *[]){__VA_ARGS__}) / sizeof(void *)]; \
	} name = {place, {__VA_ARGS__}}

void trace(const char *first, const char *second);
/* Names a method this component does not serve in Raven's trace, and answers E_NOTIMPL. */
HRESULT missing(const char *method);
struct object *owner(void *iface);
BOOL same(REFIID left, REFIID right);
HRESULT WINAPI object_query(void *iface, REFIID iid, void **out);
ULONG WINAPI object_reference(void *iface);
ULONG WINAPI object_release(void *iface);
HRESULT WINAPI object_iids(void *iface, ULONG *count, IID **iids);
HRESULT WINAPI object_name(void *iface, HSTRING *name);
HRESULT WINAPI object_trust(void *iface, TrustLevel *level);
/* A new object of a kind with its interface tables in place order, and one reference. */
void *make(size_t size, const struct kind *kind, const void *const *const *tables, size_t count);
void *face_of(struct object *self, size_t face);
void release(void *iface);
void reference(void *iface);
HRESULT answer_flag(boolean *out, boolean value);
HRESULT answer_null(void **out);
/* An event takes a handler for something that never happens: it gets a token of its own and is not kept. */
HRESULT WINAPI event_add(void *iface, void *handler, INT64 *out);
HRESULT WINAPI event_remove(void *iface, INT64 token);
/* The activation factory face of a class that has only static members: the class cannot be instantiated. */
HRESULT WINAPI static_activate(void *iface, IInspectable **out);

/* The method at a place of an interface's table; a delegate's Invoke is at the fourth. */
void *method(void *iface, size_t slot);

/* The handlers of one event, each with its token. Raising an event calls a copy of the list, so a handler may add or remove handlers. */
struct handler_entry {
	INT64 token;
	IUnknown *handler;
};

struct handlers {
	struct handler_entry *entries;
	UINT32 count, capacity;
};

HRESULT handlers_add(struct handlers *list, void *handler, INT64 *token);
HRESULT handlers_remove(struct handlers *list, INT64 token);
/* Calls each handler with the sender and the event's arguments. */
void handlers_raise(struct handlers *list, void *sender, void *arguments);
void handlers_clear(struct handlers *list);

#define INSPECTABLE (const void *)object_query, (const void *)object_reference, (const void *)object_release, (const void *)object_iids, \
	(const void *)object_name, (const void *)object_trust
#define FACTORY INSPECTABLE, (const void *)static_activate
#define UNKNOWN (const void *)object_query, (const void *)object_reference, (const void *)object_release

/* The app's view, from core_window.c: CoreApplication runs it, finds it and ends it, and its window and cursor classes are activated by
 * name. */
HRESULT application_run(IUnknown *source);
HRESULT application_view(void **out);
HRESULT application_exit(void);
/* The window of the app's CoreWindow, or NULL: what owns a dialog no window was given for. */
HWND application_window(void);
/* The messages of the core window that concern its ApplicationView, from view.c, which sees each before the core window does: TRUE when
 * it answered the message with *result. */
BOOL application_view_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result);
/* The CoreApplicationViewTitleBar of the app's view, from view.c. */
HRESULT application_title_bar(void **out);
extern struct object core_window_class, core_cursor_class, display_information_class;
/* The package of the app and its data, from package.c. */
extern struct object package_class, application_data_class;
/* Files and folders, from storage.c: the StorageFolder of a path that exists, through its IStorageFolder, and the classes. */
HRESULT make_folder(const WCHAR *path, void **out);
/* A storage file for the path of a file, and IVectorView<StorageFile> of those of several. */
HRESULT make_file(const WCHAR *path, void **out);
HRESULT make_files(const WCHAR *const *paths, UINT32 count, void **out);
/* IVectorView<String> and IVector<String> of strings, from storage.c. */
extern const struct winrt_items_kind string_list;
/* IVectorView<IStorageItem> of storage items and IAsyncOperation<IVectorView<IStorageItem>>, from storage.c. */
extern const struct winrt_items_kind item_list;
extern const IID iid_items_operation;
/* IAsyncOperation<StorageFolder>, <StorageFile>, <IVectorView<StorageFile>> and <IStorageItem>, from storage.c, and IStorageItem. */
extern const IID iid_folder_operation, iid_file_operation, iid_files_operation, iid_item_operation, iid_storage_item;
/* The full path of any IStorageItem, ours or not, into a MAX_PATH buffer. */
HRESULT path_of_any(void *iface, WCHAR *path);
/* Windows.Storage.Pickers.FileOpenPicker, FileSavePicker and FolderPicker, from pickers.c. */
extern struct object open_picker_class, save_picker_class, folder_picker_class;
/* Windows.ApplicationModel.DataTransfer: Clipboard, DataPackage, StandardDataFormats, HtmlFormatHelper and ClipboardContentOptions, from
 * clipboard.c. */
extern struct object clipboard_class, data_package_class, standard_formats_class, html_format_helper_class, clipboard_content_options_class;
/* popups.c. */
extern struct object message_dialog_class, popup_menu_class, ui_command_class, ui_command_separator_class;
/* view.c. */
extern struct object application_view_class;
/* crypto.c. */
extern struct object cryptographic_buffer_class, hash_provider_class, hash_names_class, mac_provider_class, mac_names_class,
	cryptographic_engine_class, cipher_provider_class, cipher_names_class;
/* accesscache.c. */
extern struct object storage_permissions_class;
/* datetime.c. */
extern struct object date_time_formatter_class;
/* threading.c. */
extern struct object thread_pool_class, thread_pool_timer_class;
/* Windows.System.Launcher and LauncherOptions, from launcher.c. */
extern struct object launcher_class, launcher_options_class;
/* Windows.Storage.KnownFolders, from known.c. */
extern struct object known_folders_class;
extern struct object storage_file_class, storage_folder_class, file_io_class, buffer_class, memory_stream_class;
/* A file opened for reading as an IRandomAccessStreamWithContentType, its type that of its extension, from storage.c. */
HRESULT open_typed_stream(const WCHAR *path, void **out);
/* RandomAccessStream and RandomAccessStreamReference, from streams.c. */
extern struct object random_access_stream_class, stream_reference_class;
/* A RandomAccessStreamReference over a stream, as CreateFromStream makes it. */
HRESULT stream_reference_make(IUnknown *stream, void **out);
/* Windows.Data.Json: JsonValue, JsonObject, JsonArray and JsonError, from json.c. */
extern struct object json_value_class, json_object_class, json_array_class, json_error_class;
/* Values read from and written to streams, from data.c. */
extern struct object data_reader_class, data_writer_class;
/* The folder %LOCALAPPDATA%\Packages\<family name>\<leaf> of the running package, made when missing, into a MAX_PATH buffer. */
HRESULT package_data_path(const WCHAR *leaf, WCHAR *path);
/* The root ApplicationDataContainer of a locality (0 local, 1 roaming), from settings.c, and the composite value class. */
HRESULT settings_root(INT32 locality, void **out);
extern struct object composite_value_class;
/* Windows.Foundation.PropertyValue, from value.c, and boxes made as its Create methods make them: a scalar of a PropertyType from 1 to
 * 19 but 13 (Inspectable), or an array of one, which the box copies. property_type_size is the size of one element of a type. */
extern struct object property_value_class;
HRESULT property_value_make(INT32 type, const void *value, void **out);
HRESULT property_value_make_array(INT32 type, UINT32 count, const void *values, void **out);
size_t property_type_size(INT32 type);
/* The string in any IPropertyValue box; TYPE_MISMATCH for anything else. */
HRESULT property_value_string(IUnknown *value, HSTRING *out);

/* Maps from strings to objects, from map.c. An entry holds its own copy of the key and a reference to the value. */
struct entry {
	HSTRING key;
	IInspectable *value;
};

/* An IMapView<String, Object> holding its own copies of these entries. */
HRESULT map_view(const struct entry *entries, UINT32 count, void **out);
/* The interfaces of a map from strings to another type: the identifiers and names are given, map_types_init makes the kinds once. A
 * map_iid and map_name, when given, make a mutable map of the type too. */
struct map_face {
	const IID *iid;
	const void *const *table;
};
/* A view and a map may answer interfaces of their own besides the collection's, as a runtime class that is a map does: map_faces at
 * places 3 on of the map and view_faces at places 2 on of its views, the kind then named map_class and view_class. Their methods
 * read and change the map through map_lookup_any and map_insert_any. */
struct map_types {
	const IID *view_iid, *iterable_iid, *iterator_iid, *pair_iid;
	const WCHAR *view_name, *pair_name, *iterator_name;
	const IID *map_iid;
	const WCHAR *map_name;
	const struct map_face *map_faces, *view_faces;
	UINT32 map_face_count, view_face_count;
	const char *map_class, *view_class;
	struct face_iid view_iids[FACES], pair_iids[2], iterator_iids[2], map_iids[FACES];
	const void *const *view_tables[FACES];
	const void *const *map_tables[FACES];
	struct kind view, pair, iterator, map;
};
void map_types_init(struct map_types *types);
HRESULT typed_map_view(const struct map_types *types, const struct entry *entries, UINT32 count, void **out);
/* A new, empty IMap<String, V> of the types, kept in insertion order, with one reference: a property set that answers only the map
 * and its IIterable, through map_face. */
struct property_set *typed_map_make(const struct map_types *types);
void *map_face(struct property_set *self);
/* The value of a key in a map or a view of one, through any of its interfaces; E_BOUNDS when it has none. */
HRESULT map_lookup_any(void *iface, HSTRING key, IInspectable **out);
/* Sets a key of a map, through any of its interfaces. */
HRESULT map_insert_any(void *iface, HSTRING key, IInspectable *value);
/* IMapView<String, Object>, IMap<String, Object>, and IIterable, IIterator and IKeyValuePair of their pairs, from map.c. */
extern const IID iid_property_view, iid_property_iterable, iid_property_iterator, iid_property_pair, iid_property_map;
/* A new, empty PropertySet with one reference, answered through property_set_face. A watcher, when set, is called after each change,
 * outside the set's lock; the set's own changes through these calls are changes too. */
struct property_set;
struct property_set *property_set_make(void);
void *property_set_face(struct property_set *self);
void property_set_watch(struct property_set *self, void (*watcher)(void *context), void *context);
HRESULT property_set_view(struct property_set *self, void **out);
HRESULT property_set_insert(struct property_set *self, HSTRING key, IInspectable *value);
/* Values the set refuses with accept's failure; with accept set, storing a null removes the key. */
void property_set_accept(struct property_set *self, HRESULT (*accept)(IInspectable *value));
/* A copy of the entries, which entries_free releases. */
HRESULT property_set_entries(struct property_set *self, struct entry **out, UINT32 *count);
void entries_free(struct entry *entries, UINT32 count);
/* Whether an interface is one of this component's property sets, and which. */
BOOL is_property_set(void *iface, struct property_set **out);
extern struct object property_set_class;

#endif
