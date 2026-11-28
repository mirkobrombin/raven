/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Maps from strings to objects: Windows.Foundation.Collections.PropertySet, which a program creates or CoreApplication.Properties and
 * the settings of ApplicationData hand out, and the views and pairs that read them. */
#include "object.h"

/* IMapView<String, Object>, and IIterable and IIterator of pinterface({02b51929-c1c4-4a7e-8940-0312b5c18500};string;
 * cinterface(IInspectable)) */
static const IID iid_property_view = {0xbb78502a, 0xf79d, 0x54fa, {0x92, 0xc9, 0x90, 0xc5, 0x03, 0x9f, 0xdf, 0x7e}};
static const IID iid_property_iterable = {0xfe2f3d47, 0x5d47, 0x5499, {0x83, 0x74, 0x43, 0x0c, 0x7c, 0xda, 0x02, 0x04}};
static const IID iid_property_iterator = {0x5db5fa32, 0x707c, 0x5849, {0xa0, 0x6b, 0x91, 0xc8, 0xeb, 0x9d, 0x10, 0xe8}};
/* pinterface({02b51929-c1c4-4a7e-8940-0312b5c18500};string;cinterface(IInspectable)) */
static const IID iid_property_pair = {0x09335560, 0x6c6b, 0x5a26, {0x93, 0x48, 0x97, 0xb7, 0x81, 0x13, 0x2b, 0x20}};
/* pinterface({3c2925fe-8519-45c1-aa79-197b6718c1c1};string;cinterface(IInspectable)) */
static const IID iid_property_map = {0x1b0d3570, 0x0877, 0x5ec2, {0x8a, 0x2c, 0x3b, 0x95, 0x39, 0x50, 0x6a, 0xca}};
/* pinterface({65df2bf5-bf39-41b5-aebc-5a9d865e472b};string;cinterface(IInspectable)) */
static const IID iid_observable_property_map = {0x236aac9d, 0xfb12, 0x5c4d, {0xa4, 0x1c, 0x9e, 0x44, 0x5f, 0xb4, 0xd7, 0xec}};
/* pinterface({9939f4df-050a-4c0f-aa60-77075f9c4777};string) */
static const IID iid_map_change = {0x60141efb, 0xf2f9, 0x5377, {0x96, 0xfd, 0xf8, 0xc6, 0x0d, 0x95, 0x58, 0xb5}};
static const IID iid_property_set = {0x8a43ed9f, 0xf4e6, 0x4421, {0xac, 0xf9, 0x1d, 0xab, 0x29, 0x86, 0x82, 0x0c}};

/* A snapshot is IMapView<String, V> at place 0 and IIterable<IKeyValuePair<String, V>> at place 1; its iterator hands out one pair object
 * per entry. Which V it holds, and so the interfaces it answers, its map_types say. */
struct pairs {
	struct object base;
	const struct map_types *types;
	UINT32 count;
	struct entry entries[];
};

struct pair {
	struct object base;
	struct entry entry;
};

struct pair_iterator {
	struct object base;
	struct pairs *pairs;
	UINT32 at;
};

static HRESULT keep(struct entry *entry, HSTRING key, IInspectable *value)
{
	const HRESULT result = WindowsDuplicateString(key, &entry->key);
	if (SUCCEEDED(result))
		reference(entry->value = value);
	return result;
}

static void forget(struct entry *entry)
{
	WindowsDeleteString(entry->key);
	release(entry->value);
}

static void destroy_pairs(struct object *base)
{
	struct pairs *self = (struct pairs *)base;
	for (UINT32 index = 0; index < self->count; ++index)
		forget(&self->entries[index]);
}

static void destroy_pair(struct object *base) { forget(&((struct pair *)base)->entry); }
static void destroy_pair_iterator(struct object *base) { release(face_of(&((struct pair_iterator *)base)->pairs->base, 0)); }

/* The place of a key among entries, or count when no entry has it. */
static UINT32 find(const struct entry *entries, UINT32 count, HSTRING key)
{
	for (UINT32 at = 0; at < count; ++at) {
		INT32 order = 1;
		if (SUCCEEDED(WindowsCompareStringOrdinal(entries[at].key, key, &order)) && !order)
			return at;
	}
	return count;
}

static HRESULT lookup(const struct entry *entries, UINT32 count, HSTRING key, IInspectable **out)
{
	if (!out)
		return E_POINTER;
	const UINT32 at = find(entries, count, key);
	*out = NULL;
	if (at == count)
		return E_BOUNDS;
	reference(*out = entries[at].value);
	return S_OK;
}

static HRESULT WINAPI pair_key(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(((struct pair *)owner(iface))->entry.key, out);
}

static HRESULT WINAPI pair_value(void *iface, IInspectable **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = ((struct pair *)owner(iface))->entry.value);
	return S_OK;
}

TABLE(pair_table, 0, INSPECTABLE, (const void *)pair_key, (const void *)pair_value);

static HRESULT pair_out(const struct map_types *types, const struct entry *entry, void **out)
{
	static const void *const *const tables[] = {pair_table.slots};
	struct pair *self = make(sizeof *self, &types->pair, tables, 1);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	return keep(&self->entry, entry->key, entry->value);
}

static HRESULT WINAPI pair_current(void *iface, void **out)
{
	struct pair_iterator *self = (struct pair_iterator *)owner(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->at < self->pairs->count ? pair_out(self->pairs->types, &self->pairs->entries[self->at], out) : E_BOUNDS;
}

static HRESULT WINAPI pair_has_current(void *iface, boolean *out)
{
	struct pair_iterator *self = (struct pair_iterator *)owner(iface);
	return answer_flag(out, self->at < self->pairs->count);
}

static HRESULT WINAPI pair_next(void *iface, boolean *out)
{
	struct pair_iterator *self = (struct pair_iterator *)owner(iface);
	if (self->at < self->pairs->count)
		++self->at;
	return answer_flag(out, self->at < self->pairs->count);
}

static HRESULT WINAPI pair_many(void *iface, UINT32 capacity, void **items, UINT32 *actual)
{
	struct pair_iterator *self = (struct pair_iterator *)owner(iface);
	if (!actual || (capacity && !items))
		return E_POINTER;
	for (*actual = 0; *actual < capacity && self->at < self->pairs->count; ++*actual, ++self->at) {
		const HRESULT result = pair_out(self->pairs->types, &self->pairs->entries[self->at], &items[*actual]);
		if (FAILED(result))
			return result;
	}
	return S_OK;
}

TABLE(pair_iterator_table, 0, INSPECTABLE, (const void *)pair_current, (const void *)pair_has_current, (const void *)pair_next,
	(const void *)pair_many);

static HRESULT WINAPI pairs_first(void *iface, void **out)
{
	static const void *const *const tables[] = {pair_iterator_table.slots};
	struct pairs *pairs = (struct pairs *)owner(iface);
	if (!out)
		return E_POINTER;
	struct pair_iterator *self = make(sizeof *self, &pairs->types->iterator, tables, 1);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	self->pairs = pairs;
	reference(face_of(&pairs->base, 0));
	return S_OK;
}

static HRESULT WINAPI pairs_lookup(void *iface, HSTRING key, IInspectable **out)
{
	struct pairs *self = (struct pairs *)owner(iface);
	return lookup(self->entries, self->count, key, out);
}

static HRESULT WINAPI pairs_size(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct pairs *)owner(iface))->count;
	return S_OK;
}

static HRESULT WINAPI pairs_has_key(void *iface, HSTRING key, boolean *out)
{
	struct pairs *self = (struct pairs *)owner(iface);
	return answer_flag(out, find(self->entries, self->count, key) < self->count);
}

/* A view may decline to split, and a snapshot does. */
static HRESULT WINAPI pairs_split(void *iface, void **first, void **second)
{
	if (!first || !second)
		return E_POINTER;
	*first = *second = NULL;
	return S_OK;
}

TABLE(pairs_table, 0, INSPECTABLE, (const void *)pairs_lookup, (const void *)pairs_size, (const void *)pairs_has_key, (const void *)pairs_split);
TABLE(pairs_iterable_table, 1, INSPECTABLE, (const void *)pairs_first);

void map_types_init(struct map_types *types)
{
	types->view_iids[0] = (struct face_iid){types->view_iid, 0};
	types->view_iids[1] = (struct face_iid){types->iterable_iid, 1};
	types->view_iids[2] = (struct face_iid){NULL, 0};
	types->pair_iids[0] = (struct face_iid){types->pair_iid, 0};
	types->pair_iids[1] = (struct face_iid){NULL, 0};
	types->iterator_iids[0] = (struct face_iid){types->iterator_iid, 0};
	types->iterator_iids[1] = (struct face_iid){NULL, 0};
	types->view = (struct kind){"IMapView", types->view_name, types->view_iids, destroy_pairs};
	types->pair = (struct kind){"IKeyValuePair", types->pair_name, types->pair_iids, destroy_pair};
	types->iterator = (struct kind){"IIterator", types->iterator_name, types->iterator_iids, destroy_pair_iterator};
}

/* Maps from strings to objects. */
static struct map_types object_types = {&iid_property_view, &iid_property_iterable, &iid_property_iterator, &iid_property_pair,
	L"Windows.Foundation.Collections.IMapView`2<String, Object>", L"Windows.Foundation.Collections.IKeyValuePair`2<String, Object>",
	L"Windows.Foundation.Collections.IIterator`1<Windows.Foundation.Collections.IKeyValuePair`2<String, Object>>"};
static INIT_ONCE object_types_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK make_object_types(INIT_ONCE *once, void *parameter, void **context)
{
	map_types_init(&object_types);
	return TRUE;
}

/* A snapshot holding its own copies of the keys and references to the values. */
static struct pairs *snapshot(const struct map_types *types, const struct entry *entries, UINT32 count)
{
	static const void *const *const tables[] = {pairs_table.slots, pairs_iterable_table.slots};
	if (!types) {
		InitOnceExecuteOnce(&object_types_once, make_object_types, NULL, NULL);
		types = &object_types;
	}
	struct pairs *self = make(offsetof(struct pairs, entries) + count * sizeof(struct entry), &types->view, tables, 2);
	if (!self)
		return NULL;
	self->types = types;
	for (; self->count < count; ++self->count)
		if (FAILED(keep(&self->entries[self->count], entries[self->count].key, entries[self->count].value))) {
			release(face_of(&self->base, 0));
			return NULL;
		}
	return self;
}

HRESULT typed_map_view(const struct map_types *types, const struct entry *entries, UINT32 count, void **out)
{
	if (!out)
		return E_POINTER;
	struct pairs *self = snapshot(types, entries, count);
	*out = self ? face_of(&self->base, 0) : NULL;
	return self ? S_OK : E_OUTOFMEMORY;
}

HRESULT map_view(const struct entry *entries, UINT32 count, void **out) { return typed_map_view(NULL, entries, count, out); }

/* A property set: IObservableMap<String, Object> and IPropertySet at place 0, IMap<String, Object> at place 1,
 * IIterable<IKeyValuePair<String, Object>> at place 2. It calls the program's MapChanged handlers after each change, outside its lock,
 * and then its watcher, as the settings that persist it keep one. An iterator walks a snapshot taken when it is made. */
enum map_change { change_reset, change_inserted, change_removed, change_changed };

struct property_set {
	struct object base;
	SRWLOCK lock;
	struct entry *entries;
	UINT32 count, capacity;
	struct handlers handlers;
	void (*watcher)(void *context);
	void *context;
	HRESULT (*accept)(IInspectable *value);
};

struct change {
	struct object base;
	INT32 kind;
	HSTRING key;
};

static struct property_set *set_of(void *iface) { return (struct property_set *)owner(iface); }

static void destroy_change(struct object *base) { WindowsDeleteString(((struct change *)base)->key); }

static HRESULT WINAPI change_action(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct change *)owner(iface))->kind;
	return S_OK;
}

static HRESULT WINAPI change_key(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(((struct change *)owner(iface))->key, out);
}

TABLE(change_table, 0, INSPECTABLE, (const void *)change_action, (const void *)change_key);
static const struct face_iid change_iids[] = {{&iid_map_change, 0}, {NULL, 0}};
static const struct kind change_args_kind = {"IMapChangedEventArgs<String>", L"Windows.Foundation.Collections.IMapChangedEventArgs`1<String>",
	change_iids, destroy_change};

static void changed(struct property_set *self, INT32 kind, HSTRING key)
{
	static const void *const *const tables[] = {change_table.slots};
	IUnknown **handlers = NULL;
	UINT32 count = 0;
	AcquireSRWLockShared(&self->lock);
	if (self->handlers.count && (handlers = ALLOC(self->handlers.count * sizeof *handlers)))
		for (; count < self->handlers.count; ++count)
			reference(handlers[count] = self->handlers.entries[count].handler);
	ReleaseSRWLockShared(&self->lock);
	if (count) {
		struct change *args = make(sizeof *args, &change_args_kind, tables, 1);
		if (args) {
			args->kind = kind;
			WindowsDuplicateString(key, &args->key);
		}
		for (UINT32 index = 0; index < count; ++index) {
			/* MapChangedEventHandler<String, Object>.Invoke(sender, args) follows IUnknown. */
			if (args)
				((HRESULT (WINAPI *)(void *, void *, void *))method(handlers[index], 3))(handlers[index], face_of(&self->base, 0),
					face_of(&args->base, 0));
			release(handlers[index]);
		}
		if (args)
			release(face_of(&args->base, 0));
	}
	FREE(handlers);
	if (self->watcher)
		self->watcher(self->context);
}

static HRESULT WINAPI set_changed_add(void *iface, IUnknown *handler, INT64 *out)
{
	struct property_set *self = set_of(iface);
	AcquireSRWLockExclusive(&self->lock);
	const HRESULT result = handlers_add(&self->handlers, handler, out);
	ReleaseSRWLockExclusive(&self->lock);
	return result;
}

static HRESULT WINAPI set_changed_remove(void *iface, INT64 token)
{
	struct property_set *self = set_of(iface);
	AcquireSRWLockExclusive(&self->lock);
	const HRESULT result = handlers_remove(&self->handlers, token);
	ReleaseSRWLockExclusive(&self->lock);
	return result;
}

static HRESULT WINAPI set_lookup(void *iface, HSTRING key, IInspectable **out)
{
	struct property_set *self = set_of(iface);
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = lookup(self->entries, self->count, key, out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

static HRESULT WINAPI set_size(void *iface, UINT32 *out)
{
	struct property_set *self = set_of(iface);
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	*out = self->count;
	ReleaseSRWLockShared(&self->lock);
	return S_OK;
}

static HRESULT WINAPI set_has_key(void *iface, HSTRING key, boolean *out)
{
	struct property_set *self = set_of(iface);
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	*out = find(self->entries, self->count, key) < self->count;
	ReleaseSRWLockShared(&self->lock);
	return S_OK;
}

static HRESULT WINAPI set_view(void *iface, void **out)
{
	struct property_set *self = set_of(iface);
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = map_view(self->entries, self->count, out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

static HRESULT WINAPI set_remove(void *iface, HSTRING key);

static HRESULT WINAPI set_insert(void *iface, HSTRING key, IInspectable *value, boolean *replaced)
{
	struct property_set *self = set_of(iface);
	if (!replaced)
		return E_POINTER;
	*replaced = FALSE;
	if (self->accept) {
		/* Settings store no null: setting one removes the key, as on Windows. */
		if (!value) {
			boolean present = FALSE;
			set_has_key(iface, key, &present);
			*replaced = present;
			return present ? set_remove(iface, key) : S_OK;
		}
		const HRESULT accepted = self->accept(value);
		if (FAILED(accepted))
			return accepted;
	}
	HRESULT result = S_OK;
	IInspectable *old = NULL;
	AcquireSRWLockExclusive(&self->lock);
	const UINT32 at = find(self->entries, self->count, key);
	*replaced = at < self->count;
	if (*replaced) {
		old = self->entries[at].value;
		reference(self->entries[at].value = value);
	} else {
		if (self->count == self->capacity) {
			const UINT32 capacity = self->capacity ? self->capacity * 2 : 16;
			struct entry *grown = self->entries ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, self->entries, capacity * sizeof *grown)
				: ALLOC(capacity * sizeof *grown);
			if (grown) {
				self->entries = grown;
				self->capacity = capacity;
			}
		}
		result = self->count < self->capacity ? keep(&self->entries[self->count], key, value) : E_OUTOFMEMORY;
		if (SUCCEEDED(result))
			++self->count;
	}
	ReleaseSRWLockExclusive(&self->lock);
	release(old);
	if (SUCCEEDED(result))
		changed(self, *replaced ? change_changed : change_inserted, key);
	return result;
}

static HRESULT WINAPI set_remove(void *iface, HSTRING key)
{
	struct property_set *self = set_of(iface);
	struct entry removed = {NULL, NULL};
	AcquireSRWLockExclusive(&self->lock);
	const UINT32 at = find(self->entries, self->count, key);
	const BOOL found = at < self->count;
	if (found) {
		removed = self->entries[at];
		self->entries[at] = self->entries[--self->count];
	}
	ReleaseSRWLockExclusive(&self->lock);
	if (!found)
		return E_BOUNDS;
	forget(&removed);
	changed(self, change_removed, key);
	return S_OK;
}

static void drop_entries(struct entry *entries, UINT32 count)
{
	for (UINT32 index = 0; index < count; ++index)
		forget(&entries[index]);
	FREE(entries);
}

static HRESULT WINAPI set_clear(void *iface)
{
	struct property_set *self = set_of(iface);
	AcquireSRWLockExclusive(&self->lock);
	struct entry *entries = self->entries;
	const UINT32 count = self->count;
	self->entries = NULL;
	self->count = self->capacity = 0;
	ReleaseSRWLockExclusive(&self->lock);
	drop_entries(entries, count);
	changed(self, change_reset, NULL);
	return S_OK;
}

static HRESULT WINAPI set_first(void *iface, void **out)
{
	struct property_set *self = set_of(iface);
	AcquireSRWLockShared(&self->lock);
	struct pairs *pairs = snapshot(NULL, self->entries, self->count);
	ReleaseSRWLockShared(&self->lock);
	if (!out) {
		if (pairs)
			release(face_of(&pairs->base, 0));
		return E_POINTER;
	}
	*out = NULL;
	if (!pairs)
		return E_OUTOFMEMORY;
	const HRESULT result = pairs_first(face_of(&pairs->base, 1), out);
	release(face_of(&pairs->base, 0));
	return result;
}

static void destroy_set(struct object *base)
{
	struct property_set *self = (struct property_set *)base;
	drop_entries(self->entries, self->count);
	handlers_clear(&self->handlers);
}

TABLE(set_observable_table, 0, INSPECTABLE, (const void *)set_changed_add, (const void *)set_changed_remove);
TABLE(set_map_table, 1, INSPECTABLE, (const void *)set_lookup, (const void *)set_size, (const void *)set_has_key, (const void *)set_view,
	(const void *)set_insert, (const void *)set_remove, (const void *)set_clear);
TABLE(set_iterable_table, 2, INSPECTABLE, (const void *)set_first);
static const struct face_iid set_iids[] = {{&iid_observable_property_map, 0}, {&iid_property_set, 0}, {&iid_property_map, 1},
	{&iid_property_iterable, 2}, {NULL, 0}};
static const struct kind set_kind = {"PropertySet", L"Windows.Foundation.Collections.PropertySet", set_iids, destroy_set};

struct property_set *property_set_make(void)
{
	static const void *const *const tables[] = {set_observable_table.slots, set_map_table.slots, set_iterable_table.slots};
	struct property_set *self = make(sizeof *self, &set_kind, tables, 3);
	if (self)
		InitializeSRWLock(&self->lock);
	return self;
}

void *property_set_face(struct property_set *self) { return face_of(&self->base, 0); }

void property_set_watch(struct property_set *self, void (*watcher)(void *context), void *context)
{
	self->watcher = watcher;
	self->context = context;
}

HRESULT property_set_view(struct property_set *self, void **out) { return set_view(face_of(&self->base, 1), out); }

void property_set_accept(struct property_set *self, HRESULT (*accept)(IInspectable *value)) { self->accept = accept; }

HRESULT property_set_entries(struct property_set *self, struct entry **out, UINT32 *count)
{
	HRESULT result = S_OK;
	*out = NULL;
	*count = 0;
	AcquireSRWLockShared(&self->lock);
	if (self->count && !(*out = ALLOC(self->count * sizeof **out)))
		result = E_OUTOFMEMORY;
	for (; SUCCEEDED(result) && *count < self->count; ++*count)
		result = keep(&(*out)[*count], self->entries[*count].key, self->entries[*count].value);
	ReleaseSRWLockShared(&self->lock);
	if (FAILED(result)) {
		entries_free(*out, *count);
		*out = NULL;
		*count = 0;
	}
	return result;
}

void entries_free(struct entry *entries, UINT32 count) { drop_entries(entries, count); }

BOOL is_property_set(void *iface, struct property_set **out)
{
	const BOOL found = iface && method(iface, 0) == (void *)object_query && owner(iface)->kind == &set_kind;
	if (out)
		*out = found ? (struct property_set *)owner(iface) : NULL;
	return found;
}

HRESULT property_set_insert(struct property_set *self, HSTRING key, IInspectable *value)
{
	boolean replaced;
	return set_insert(face_of(&self->base, 1), key, value, &replaced);
}

/* Windows.Foundation.Collections.PropertySet: a program makes one with RoActivateInstance. */
static HRESULT WINAPI set_activate(void *iface, IInspectable **out)
{
	if (!out)
		return E_POINTER;
	struct property_set *self = property_set_make();
	*out = self ? property_set_face(self) : NULL;
	return self ? S_OK : E_OUTOFMEMORY;
}

TABLE(set_factory_table, 0, INSPECTABLE, (const void *)set_activate);
static const struct face_iid set_factory_iids[] = {{&iid_factory, 0}, {NULL, 0}};
static const struct kind set_factory_kind = {"PropertySet", L"Windows.Foundation.Collections.PropertySet", set_factory_iids, NULL};
struct object property_set_class = {{set_factory_table.slots}, &set_factory_kind, 1};
