/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The settings of a package: ApplicationData.LocalSettings and RoamingSettings, each an ApplicationDataContainer with its Values and
 * the containers made in it, and Windows.Storage.ApplicationDataCompositeValue.
 *
 * Values keep what PropertyValue boxes, as scalars or arrays, and composite values of such boxes; an object of another kind, a boxed
 * object or a nested composite is refused with ERROR_UNSUPPORTED_TYPE, and storing a null removes the key. Each locality lives in one
 * file, Settings\<locality>.settings in the package's data folder, read when the locality is first asked for and written whole after
 * every change; a file that does not read back is left aside and the settings start empty. Nothing roams. A composite is written as it
 * is when its container is next written. */
#include "object.h"

static const IID iid_container = {0xc5aefd1e, 0xf467, 0x40ba, {0x85, 0x66, 0xab, 0x64, 0x0a, 0x44, 0x1e, 0x1d}};
static const IID iid_closable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
static const IID iid_property_value = {0x4bd682dd, 0x7554, 0x40e9, {0x9a, 0x9b, 0x82, 0x65, 0x4e, 0xde, 0x7e, 0x62}};
/* IMapView`2<String, Windows.Storage.ApplicationDataContainer> and its iterable, iterator and pair: pinterface of
 * {e480ce40-a338-4ada-adcf-272272e48cb9}, {faa585ea-6214-4217-afda-7f46de5869b3}, {6a79e863-4300-459a-9966-cbb660963ee1} and
 * {02b51929-c1c4-4a7e-8940-0312b5c18500} over string and rc(Windows.Storage.ApplicationDataContainer;{c5aefd1e-...}). */
static const IID iid_container_view = {0x13624f8d, 0x85cc, 0x5780, {0xa7, 0x8d, 0x64, 0xdb, 0xa5, 0x8f, 0x2c, 0x3c}};
static const IID iid_container_iterable = {0xa785be1d, 0x159e, 0x53ad, {0x95, 0x53, 0x59, 0x8b, 0x03, 0xdc, 0xa0, 0x48}};
static const IID iid_container_iterator = {0xaf3c131d, 0x67aa, 0x5c8d, {0xae, 0x0e, 0x27, 0x2b, 0xa2, 0x4a, 0xe7, 0x4f}};
static const IID iid_container_pair = {0x5adbc543, 0x2170, 0x5ad9, {0xb3, 0x5e, 0x96, 0x8c, 0xdb, 0x78, 0xfb, 0x30}};

#define UNSUPPORTED_TYPE HRESULT_FROM_WIN32(ERROR_UNSUPPORTED_TYPE)
/* PropertyType: Inspectable, String, the array flag, and the mark this file gives a composite. */
#define TYPE_INSPECTABLE 13
#define TYPE_STRING 12
#define TYPE_ARRAY 1024
#define TYPE_COMPOSITE 0x10000
/* A container's name is at most 255 characters, as on Windows. */
#define LONGEST_NAME 255
static const char magic[8] = {'R', 'V', 'S', 'E', 'T', 'T', '0', '1'};

static struct map_types container_types = {&iid_container_view, &iid_container_iterable, &iid_container_iterator, &iid_container_pair,
	L"Windows.Foundation.Collections.IMapView`2<String, Windows.Storage.ApplicationDataContainer>",
	L"Windows.Foundation.Collections.IKeyValuePair`2<String, Windows.Storage.ApplicationDataContainer>",
	L"Windows.Foundation.Collections.IIterator`1<Windows.Foundation.Collections.IKeyValuePair`2<String, "
	L"Windows.Storage.ApplicationDataContainer>>"};

struct root;

/* A container: its name, its values and the containers made in it, which it holds a reference to. */
struct container {
	struct object base;
	struct root *root;
	WCHAR *name;
	struct property_set *values;
	struct container **children;
	UINT32 child_count, child_capacity;
	BOOL deleted;
};

/* A locality: its top container and its file. The lock guards the tree of containers; saving takes its own lock, so that writes of
 * the file follow one another. */
struct root {
	INT32 locality;
	SRWLOCK lock, save_lock;
	struct container *top;
	WCHAR path[MAX_PATH];
	INIT_ONCE once;
	HRESULT made;
};

static struct root roots[2] = {{0, SRWLOCK_INIT, SRWLOCK_INIT, NULL, {0}, INIT_ONCE_STATIC_INIT, S_OK},
	{1, SRWLOCK_INIT, SRWLOCK_INIT, NULL, {0}, INIT_ONCE_STATIC_INIT, S_OK}};

static struct container *container_of(void *iface) { return (struct container *)owner(iface); }

/* The methods of IPropertyValue that read a scalar of a type, and an array of a type, by their place in its table. */
static size_t scalar_slot(INT32 type)
{
	static const BYTE slots[20] = {0, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 0, 21, 22, 20, 23, 24, 25};
	return type > 0 && type < 20 ? slots[type] : 0;
}

static size_t array_slot(INT32 type)
{
	static const BYTE slots[20] = {0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 40, 41, 39, 42, 43, 44};
	return type > 0 && type < 20 ? slots[type] : 0;
}

/* The PropertyType of a value, or 0 when it is no IPropertyValue. */
static INT32 value_type(IInspectable *value, IInspectable **property_value)
{
	INT32 type = 0;
	*property_value = NULL;
	if (FAILED(IInspectable_QueryInterface(value, &iid_property_value, (void **)property_value)) || !*property_value)
		return 0;
	if (FAILED(((HRESULT (WINAPI *)(void *, INT32 *))method(*property_value, 6))(*property_value, &type)))
		type = 0;
	return type;
}

/* A box of something settings keep: any scalar or array of PropertyValue but those of objects, and nothing beyond Rect. */
static HRESULT accept_box(IInspectable *value)
{
	IInspectable *box;
	const INT32 type = value_type(value, &box);
	if (box)
		release(box);
	const INT32 element = type & ~TYPE_ARRAY;
	return element >= 1 && element <= 19 && element != TYPE_INSPECTABLE ? S_OK : UNSUPPORTED_TYPE;
}

/* A container's values also keep composites, whose own values are boxes. */
static HRESULT accept_setting(IInspectable *value)
{
	struct property_set *composite;
	if (!is_property_set(value, &composite))
		return accept_box(value);
	struct entry *entries;
	UINT32 count;
	HRESULT result = property_set_entries(composite, &entries, &count);
	for (UINT32 index = 0; SUCCEEDED(result) && index < count; ++index)
		result = entries[index].value ? accept_box(entries[index].value) : UNSUPPORTED_TYPE;
	entries_free(entries, count);
	return result;
}

/* The bytes of a file being written; a failure to grow is kept and ends the write. */
struct buffer {
	BYTE *data;
	size_t size, capacity;
	BOOL failed;
};

static void put(struct buffer *buffer, const void *bytes, size_t size)
{
	if (buffer->failed)
		return;
	if (buffer->size + size > buffer->capacity) {
		size_t capacity = buffer->capacity ? buffer->capacity : 4096;
		while (capacity < buffer->size + size)
			capacity *= 2;
		BYTE *grown = buffer->data ? HeapReAlloc(GetProcessHeap(), 0, buffer->data, capacity) : ALLOC(capacity);
		if (!grown) {
			buffer->failed = TRUE;
			return;
		}
		buffer->data = grown;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->size, bytes, size);
	buffer->size += size;
}

static void put_u32(struct buffer *buffer, UINT32 value) { put(buffer, &value, 4); }

static void put_text(struct buffer *buffer, const WCHAR *text, UINT32 length)
{
	put_u32(buffer, length);
	put(buffer, text, (size_t)length * sizeof(WCHAR));
}

static void put_string(struct buffer *buffer, HSTRING string)
{
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(string, &length);
	put_text(buffer, text, length);
}

/* A box: its type, then a scalar's bytes, a string, or an array's count and elements. */
static void put_box(struct buffer *buffer, IInspectable *value)
{
	IInspectable *box;
	const INT32 type = value_type(value, &box), element = type & ~TYPE_ARRAY;
	const size_t size = property_type_size(element);
	if (!box || !size || element == TYPE_INSPECTABLE) {
		buffer->failed = TRUE;
		if (box)
			release(box);
		return;
	}
	put_u32(buffer, (UINT32)type);
	if (type & TYPE_ARRAY) {
		UINT32 count = 0;
		void *elements = NULL;
		if (FAILED(((HRESULT (WINAPI *)(void *, UINT32 *, void **))method(box, array_slot(element)))(box, &count, &elements)))
			buffer->failed = TRUE;
		put_u32(buffer, count);
		if (element == TYPE_STRING) {
			for (UINT32 index = 0; index < count; ++index) {
				put_string(buffer, ((HSTRING *)elements)[index]);
				WindowsDeleteString(((HSTRING *)elements)[index]);
			}
		} else {
			put(buffer, elements, (size_t)count * size);
		}
		CoTaskMemFree(elements);
	} else if (element == TYPE_STRING) {
		HSTRING string = NULL;
		if (FAILED(((HRESULT (WINAPI *)(void *, HSTRING *))method(box, scalar_slot(element)))(box, &string)))
			buffer->failed = TRUE;
		put_string(buffer, string);
		WindowsDeleteString(string);
	} else {
		BYTE bytes[16] = {0};
		if (FAILED(((HRESULT (WINAPI *)(void *, void *))method(box, scalar_slot(element)))(box, bytes)))
			buffer->failed = TRUE;
		put(buffer, bytes, size);
	}
	release(box);
}

/* Entries of a property set: their count, then each key and value. */
static void put_values(struct buffer *buffer, struct property_set *values, BOOL composites)
{
	struct entry *entries;
	UINT32 count;
	if (FAILED(property_set_entries(values, &entries, &count))) {
		buffer->failed = TRUE;
		return;
	}
	put_u32(buffer, count);
	for (UINT32 index = 0; index < count; ++index) {
		struct property_set *composite;
		put_string(buffer, entries[index].key);
		if (composites && is_property_set(entries[index].value, &composite)) {
			put_u32(buffer, TYPE_COMPOSITE);
			put_values(buffer, composite, FALSE);
		} else {
			put_box(buffer, entries[index].value);
		}
	}
	entries_free(entries, count);
}

/* A container: its values, then the count of its containers and each one's name and contents. The caller holds the tree's lock. */
static void put_container(struct buffer *buffer, struct container *self)
{
	put_values(buffer, self->values, TRUE);
	put_u32(buffer, self->child_count);
	for (UINT32 index = 0; index < self->child_count; ++index) {
		put_text(buffer, self->children[index]->name, (UINT32)lstrlenW(self->children[index]->name));
		put_container(buffer, self->children[index]);
	}
}

/* Writes the locality's file whole, through a file beside it that then takes its place. */
static void save(struct root *root)
{
	struct buffer buffer = {0};
	WCHAR temporary[MAX_PATH + 8];
	AcquireSRWLockExclusive(&root->save_lock);
	put(&buffer, magic, sizeof magic);
	AcquireSRWLockShared(&root->lock);
	put_container(&buffer, root->top);
	ReleaseSRWLockShared(&root->lock);
	lstrcpyW(temporary, root->path);
	lstrcatW(temporary, L".new");
	if (!buffer.failed) {
		HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD written = 0;
		BOOL done = file != INVALID_HANDLE_VALUE && WriteFile(file, buffer.data, (DWORD)buffer.size, &written, NULL) && written == buffer.size;
		if (file != INVALID_HANDLE_VALUE)
			CloseHandle(file);
		if (done)
			done = MoveFileExW(temporary, root->path, MOVEFILE_REPLACE_EXISTING);
		if (!done) {
			DeleteFileW(temporary);
			trace("settings were not written to ", "the package's Settings folder");
		}
	}
	ReleaseSRWLockExclusive(&root->save_lock);
	FREE(buffer.data);
}

/* A change of values writes the file; while the file is read, before the locality has its top container, nothing is written. */
static void values_changed(void *context)
{
	struct container *self = context;
	if (!self->deleted && self->root->top)
		save(self->root);
}

static HRESULT WINAPI container_name(void *iface, HSTRING *out)
{
	const WCHAR *name = container_of(iface)->name;
	if (!out)
		return E_POINTER;
	return WindowsCreateString(name, (UINT32)lstrlenW(name), out);
}

static HRESULT WINAPI container_locality(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = container_of(iface)->root->locality;
	return S_OK;
}

static HRESULT WINAPI container_values(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = property_set_face(container_of(iface)->values));
	return S_OK;
}

/* The container map's kinds are made once, before the first view of containers. */
static INIT_ONCE container_types_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK make_container_types(INIT_ONCE *once, void *parameter, void **context)
{
	map_types_init(&container_types);
	return TRUE;
}

static HRESULT WINAPI container_children(void *iface, void **out)
{
	InitOnceExecuteOnce(&container_types_once, make_container_types, NULL, NULL);
	struct container *self = container_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	AcquireSRWLockShared(&self->root->lock);
	struct entry *entries = self->child_count ? ALLOC(self->child_count * sizeof *entries) : NULL;
	HRESULT result = self->child_count && !entries ? E_OUTOFMEMORY : S_OK;
	UINT32 count = 0;
	for (; SUCCEEDED(result) && count < self->child_count; ++count) {
		struct container *child = self->children[count];
		result = WindowsCreateString(child->name, (UINT32)lstrlenW(child->name), &entries[count].key);
		reference(entries[count].value = face_of(&child->base, 0));
	}
	ReleaseSRWLockShared(&self->root->lock);
	if (SUCCEEDED(result))
		result = typed_map_view(&container_types, entries, count, out);
	if (entries)
		entries_free(entries, count);
	return result;
}

static struct container *new_container(struct root *root, const WCHAR *name, UINT32 length);

/* The place of a named container among a container's own, or child_count. */
static UINT32 child_named(struct container *self, const WCHAR *name, UINT32 length)
{
	for (UINT32 index = 0; index < self->child_count; ++index)
		if (CompareStringOrdinal(self->children[index]->name, -1, name, (int)length, TRUE) == CSTR_EQUAL)
			return index;
	return self->child_count;
}

static HRESULT add_child(struct container *self, struct container *child)
{
	if (self->child_count == self->child_capacity) {
		const UINT32 capacity = self->child_capacity ? self->child_capacity * 2 : 4;
		struct container **grown = self->children ? HeapReAlloc(GetProcessHeap(), 0, self->children, capacity * sizeof *grown)
			: ALLOC(capacity * sizeof *grown);
		if (!grown)
			return E_OUTOFMEMORY;
		self->children = grown;
		self->child_capacity = capacity;
	}
	self->children[self->child_count++] = child;
	return S_OK;
}

/* CreateContainer(name, disposition): the container of that name, made unless the disposition is Existing (1). Names are compared
 * without regard to case. */
static HRESULT WINAPI container_create(void *iface, HSTRING name, INT32 disposition, void **out)
{
	struct container *self = container_of(iface);
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!length || length > LONGEST_NAME || (disposition != 0 && disposition != 1))
		return E_INVALIDARG;
	if (self->deleted)
		return NOT_FOUND;
	BOOL made = FALSE;
	HRESULT result = S_OK;
	AcquireSRWLockExclusive(&self->root->lock);
	const UINT32 at = child_named(self, text, length);
	if (at < self->child_count) {
		reference(*out = face_of(&self->children[at]->base, 0));
	} else if (disposition == 1) {
		result = NOT_FOUND;
	} else {
		struct container *child = new_container(self->root, text, length);
		if (!child || FAILED(result = add_child(self, child))) {
			if (child)
				release(face_of(&child->base, 0));
			result = FAILED(result) ? result : E_OUTOFMEMORY;
		} else {
			reference(*out = face_of(&child->base, 0));
			made = TRUE;
		}
	}
	ReleaseSRWLockExclusive(&self->root->lock);
	if (made)
		save(self->root);
	return result;
}

static void mark_deleted(struct container *self)
{
	self->deleted = TRUE;
	for (UINT32 index = 0; index < self->child_count; ++index)
		mark_deleted(self->children[index]);
}

/* DeleteContainer(name): the container and those in it are gone, from the file too; deleting one that is not there does nothing. */
static HRESULT WINAPI container_delete(void *iface, HSTRING name)
{
	struct container *self = container_of(iface), *removed = NULL;
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
	if (!length)
		return E_INVALIDARG;
	AcquireSRWLockExclusive(&self->root->lock);
	const UINT32 at = child_named(self, text, length);
	if (at < self->child_count) {
		removed = self->children[at];
		self->children[at] = self->children[--self->child_count];
		mark_deleted(removed);
	}
	ReleaseSRWLockExclusive(&self->root->lock);
	if (removed) {
		release(face_of(&removed->base, 0));
		if (!self->deleted)
			save(self->root);
	}
	return S_OK;
}

static HRESULT WINAPI container_close(void *iface) { return S_OK; }

static void destroy_container(struct object *base)
{
	struct container *self = (struct container *)base;
	if (self->values) {
		property_set_watch(self->values, NULL, NULL);
		release(property_set_face(self->values));
	}
	for (UINT32 index = 0; index < self->child_count; ++index)
		release(face_of(&self->children[index]->base, 0));
	FREE(self->children);
	FREE(self->name);
}

TABLE(container_table, 0, INSPECTABLE, (const void *)container_name, (const void *)container_locality, (const void *)container_values,
	(const void *)container_children, (const void *)container_create, (const void *)container_delete);
TABLE(container_closable_table, 1, INSPECTABLE, (const void *)container_close);
static const struct face_iid container_iids[] = {{&iid_container, 0}, {&iid_closable, 1}, {NULL, 0}};
static const struct kind container_kind = {"ApplicationDataContainer", L"Windows.Storage.ApplicationDataContainer", container_iids,
	destroy_container};

static struct container *new_container(struct root *root, const WCHAR *name, UINT32 length)
{
	static const void *const *const tables[] = {container_table.slots, container_closable_table.slots};
	struct container *self = make(sizeof *self, &container_kind, tables, 2);
	if (!self)
		return NULL;
	self->root = root;
	self->name = ALLOC(((size_t)length + 1) * sizeof(WCHAR));
	self->values = property_set_make();
	if (!self->name || !self->values) {
		release(face_of(&self->base, 0));
		return NULL;
	}
	memcpy(self->name, name, (size_t)length * sizeof(WCHAR));
	property_set_accept(self->values, accept_setting);
	property_set_watch(self->values, values_changed, self);
	return self;
}

/* Reading a file back: every read checks that the bytes are there. */
struct reader {
	const BYTE *at, *end;
	BOOL failed;
};

static const void *take(struct reader *reader, size_t size)
{
	if (reader->failed || (size_t)(reader->end - reader->at) < size) {
		reader->failed = TRUE;
		return NULL;
	}
	const void *bytes = reader->at;
	reader->at += size;
	return bytes;
}

static UINT32 take_u32(struct reader *reader)
{
	const void *bytes = take(reader, 4);
	UINT32 value = 0;
	if (bytes)
		memcpy(&value, bytes, 4);
	return value;
}

static HSTRING take_string(struct reader *reader)
{
	const UINT32 length = take_u32(reader);
	const WCHAR *text = length > 0x100000 ? (reader->failed = TRUE, NULL) : take(reader, (size_t)length * sizeof(WCHAR));
	HSTRING string = NULL;
	if (text && FAILED(WindowsCreateString(text, length, &string)))
		reader->failed = TRUE;
	return string;
}

static void *take_box(struct reader *reader, INT32 type)
{
	const INT32 element = type & ~TYPE_ARRAY;
	const size_t size = property_type_size(element);
	void *box = NULL;
	if (!size || element == TYPE_INSPECTABLE || (type & ~(TYPE_ARRAY | 0x1f))) {
		reader->failed = TRUE;
		return NULL;
	}
	if (type & TYPE_ARRAY) {
		const UINT32 count = take_u32(reader);
		if (count > 0x1000000 || reader->failed) {
			reader->failed = TRUE;
			return NULL;
		}
		if (element == TYPE_STRING) {
			HSTRING *strings = count ? ALLOC(count * sizeof *strings) : NULL;
			UINT32 made = 0;
			if (count && !strings)
				reader->failed = TRUE;
			for (; !reader->failed && made < count; ++made)
				strings[made] = take_string(reader);
			if (!reader->failed && FAILED(property_value_make_array(element, count, strings, &box)))
				reader->failed = TRUE;
			for (UINT32 index = 0; index < made; ++index)
				WindowsDeleteString(strings[index]);
			FREE(strings);
		} else {
			const void *elements = take(reader, (size_t)count * size);
			if (elements && FAILED(property_value_make_array(element, count, elements, &box)))
				reader->failed = TRUE;
		}
	} else if (element == TYPE_STRING) {
		HSTRING string = take_string(reader);
		if (!reader->failed && FAILED(property_value_make(element, &string, &box)))
			reader->failed = TRUE;
		WindowsDeleteString(string);
	} else {
		BYTE bytes[16] = {0};
		const void *read = take(reader, size);
		if (read) {
			memcpy(bytes, read, size);
			if (FAILED(property_value_make(element, bytes, &box)))
				reader->failed = TRUE;
		}
	}
	return reader->failed ? (box ? (release(box), NULL) : NULL) : box;
}

static void take_values(struct reader *reader, struct property_set *values, BOOL composites)
{
	const UINT32 count = take_u32(reader);
	for (UINT32 index = 0; !reader->failed && index < count; ++index) {
		HSTRING key = take_string(reader);
		const INT32 type = (INT32)take_u32(reader);
		void *value = NULL;
		if (reader->failed) {
		} else if (type == TYPE_COMPOSITE && composites) {
			struct property_set *composite = property_set_make();
			if (!composite) {
				reader->failed = TRUE;
			} else {
				take_values(reader, composite, FALSE);
				value = property_set_face(composite);
			}
		} else {
			value = take_box(reader, type);
		}
		if (value && !reader->failed && FAILED(property_set_insert(values, key, value)))
			reader->failed = TRUE;
		if (value)
			release(value);
		WindowsDeleteString(key);
	}
}

static void take_container(struct reader *reader, struct container *self, UINT32 depth)
{
	take_values(reader, self->values, TRUE);
	const UINT32 count = take_u32(reader);
	if (depth > 64)
		reader->failed = TRUE;
	for (UINT32 index = 0; !reader->failed && index < count; ++index) {
		const UINT32 length = take_u32(reader);
		const WCHAR *name = length && length <= LONGEST_NAME ? take(reader, (size_t)length * sizeof(WCHAR)) : NULL;
		struct container *child = name ? new_container(self->root, name, length) : NULL;
		if (!child || FAILED(add_child(self, child))) {
			if (child)
				release(face_of(&child->base, 0));
			reader->failed = TRUE;
			break;
		}
		take_container(reader, child, depth + 1);
	}
}

/* Reads the locality's file into a new top container. */
static struct container *load(struct root *root)
{
	struct container *top = new_container(root, L"", 0);
	if (!top)
		return NULL;
	HANDLE file = CreateFileW(root->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return top;
	LARGE_INTEGER size;
	BYTE *bytes = NULL;
	DWORD read = 0;
	BOOL whole = GetFileSizeEx(file, &size) && size.QuadPart >= (LONGLONG)sizeof magic && size.QuadPart < 0x10000000 &&
		(bytes = ALLOC((size_t)size.QuadPart)) && ReadFile(file, bytes, (DWORD)size.QuadPart, &read, NULL) && read == size.QuadPart;
	CloseHandle(file);
	if (whole && !memcmp(bytes, magic, sizeof magic)) {
		struct reader reader = {bytes + sizeof magic, bytes + read, FALSE};
		take_container(&reader, top, 0);
		whole = !reader.failed && reader.at == reader.end;
	} else {
		whole = FALSE;
	}
	FREE(bytes);
	if (!whole) {
		/* What does not read back is set aside, and the settings start over. */
		WCHAR aside[MAX_PATH + 8];
		lstrcpyW(aside, root->path);
		lstrcatW(aside, L".bad");
		MoveFileExW(root->path, aside, MOVEFILE_REPLACE_EXISTING);
		trace("settings that did not read back were set aside: ", "Settings\\*.settings.bad");
		release(face_of(&top->base, 0));
		top = new_container(root, L"", 0);
	}
	return top;
}

static BOOL CALLBACK make_root(INIT_ONCE *once, void *parameter, void **context)
{
	struct root *root = parameter;
	WCHAR folder[MAX_PATH];
	root->made = package_data_path(L"Settings", folder);
	if (SUCCEEDED(root->made) && (size_t)lstrlenW(folder) + 20 >= MAX_PATH)
		root->made = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	if (FAILED(root->made))
		return TRUE;
	lstrcpyW(root->path, folder);
	lstrcatW(root->path, root->locality ? L"\\roaming.settings" : L"\\local.settings");
	root->top = load(root);
	if (!root->top)
		root->made = E_OUTOFMEMORY;
	return TRUE;
}

HRESULT settings_root(INT32 locality, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct root *root = &roots[locality ? 1 : 0];
	InitOnceExecuteOnce(&root->once, make_root, root, NULL);
	if (FAILED(root->made))
		return root->made;
	reference(*out = face_of(&root->top->base, 0));
	return S_OK;
}

/* Windows.Storage.ApplicationDataCompositeValue: a property set of boxes, which a container keeps as one value. */
static HRESULT WINAPI composite_activate(void *iface, IInspectable **out)
{
	if (!out)
		return E_POINTER;
	struct property_set *self = property_set_make();
	if (self)
		property_set_accept(self, accept_box);
	*out = self ? property_set_face(self) : NULL;
	return self ? S_OK : E_OUTOFMEMORY;
}

TABLE(composite_factory_table, 0, INSPECTABLE, (const void *)composite_activate);
static const struct face_iid composite_factory_iids[] = {{&iid_factory, 0}, {NULL, 0}};
static const struct kind composite_factory_kind = {"ApplicationDataCompositeValue", L"Windows.Storage.ApplicationDataCompositeValue",
	composite_factory_iids, NULL};
struct object composite_value_class = {{composite_factory_table.slots}, &composite_factory_kind, 1};
