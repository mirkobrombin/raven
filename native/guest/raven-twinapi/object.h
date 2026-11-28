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

/* Objects alive, which decide whether the library may unload, and the last event token handed out. */
extern LONG objects, tokens;

extern const IID iid_unknown, iid_inspectable, iid_agile, iid_factory, iid_async_info;

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

/* The app's view, from core_window.c: CoreApplication runs it, finds it and ends it, and its window and cursor classes are activated by
 * name. */
HRESULT application_run(IUnknown *source);
HRESULT application_view(void **out);
HRESULT application_exit(void);
extern struct object core_window_class, core_cursor_class, display_information_class;
/* The package of the app and its data, from package.c. */
extern struct object package_class, application_data_class;
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

/* Maps from strings to objects, from map.c. An entry holds its own copy of the key and a reference to the value. */
struct entry {
	HSTRING key;
	IInspectable *value;
};

/* An IMapView<String, Object> holding its own copies of these entries. */
HRESULT map_view(const struct entry *entries, UINT32 count, void **out);
/* The interfaces of a map from strings to another type: the identifiers and names are given, map_types_init makes the kinds once. */
struct map_types {
	const IID *view_iid, *iterable_iid, *iterator_iid, *pair_iid;
	const WCHAR *view_name, *pair_name, *iterator_name;
	struct face_iid view_iids[3], pair_iids[2], iterator_iids[2];
	struct kind view, pair, iterator;
};
void map_types_init(struct map_types *types);
HRESULT typed_map_view(const struct map_types *types, const struct entry *entries, UINT32 count, void **out);
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
