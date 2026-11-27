/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "object.h"

LONG objects, tokens;

const IID iid_unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID iid_inspectable = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
const IID iid_agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
const IID iid_factory = {0x00000035, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID iid_async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

void trace(const char *first, const char *second)
{
	static void (WINAPI *raven_trace)(const char *);
	static BOOL looked;
	char line[200] = "twinapi: ";
	size_t at = 9;
	if (!looked) {
		raven_trace = (void (WINAPI *)(const char *))GetProcAddress(LoadLibraryW(L"ravenhost.dll"), "RavenTrace");
		looked = TRUE;
	}
	if (!raven_trace)
		return;
	for (const char *part = first; *part && at < 120; ++part)
		line[at++] = *part;
	for (const char *part = second; *part && at < 198; ++part)
		line[at++] = *part;
	line[at] = 0;
	raven_trace(line);
}

HRESULT missing(const char *method)
{
	trace(method, " is not implemented");
	return E_NOTIMPL;
}

struct object *owner(void *iface)
{
	const SIZE_T face = ((const SIZE_T *)*(const void *const **)iface)[-1];
	return (struct object *)((const void *const **)iface - face);
}

BOOL same(REFIID left, REFIID right) { return !memcmp(left, right, sizeof(IID)); }

static void trace_iid(const struct object *self, REFIID iid)
{
	static const char digits[] = "0123456789abcdef";
	static const BYTE order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
	char text[48];
	size_t at = 0;
	const BYTE *bytes = (const BYTE *)iid;
	for (size_t index = 0; index < 16; ++index) {
		if (index == 4 || index == 6 || index == 8 || index == 10)
			text[at++] = '-';
		text[at++] = digits[bytes[order[index]] >> 4];
		text[at++] = digits[bytes[order[index]] & 15];
	}
	text[at] = 0;
	char line[120] = "";
	size_t used = 0;
	for (const char *part = self->kind->name; *part && used < 70; ++part)
		line[used++] = *part;
	for (const char *part = " has no interface "; *part; ++part)
		line[used++] = *part;
	line[used] = 0;
	trace(line, text);
}

HRESULT WINAPI object_query(void *iface, REFIID iid, void **out)
{
	struct object *self = owner(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (same(iid, &iid_unknown) || same(iid, &iid_inspectable) || same(iid, &iid_agile))
		*out = &self->faces[0];
	for (const struct face_iid *entry = self->kind->iids; !*out && entry->iid; ++entry)
		if (same(iid, entry->iid))
			*out = &self->faces[entry->face];
	if (!*out) {
		trace_iid(self, iid);
		return E_NOINTERFACE;
	}
	if (self->kind->destroy)
		InterlockedIncrement(&self->refs);
	return S_OK;
}

ULONG WINAPI object_reference(void *iface)
{
	struct object *self = owner(iface);
	return self->kind->destroy ? (ULONG)InterlockedIncrement(&self->refs) : 2;
}

ULONG WINAPI object_release(void *iface)
{
	struct object *self = owner(iface);
	if (!self->kind->destroy)
		return 1;
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		self->kind->destroy(self);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return (ULONG)refs;
}

HRESULT WINAPI object_iids(void *iface, ULONG *count, IID **iids)
{
	if (!count || !iids)
		return E_POINTER;
	*count = 0;
	*iids = NULL;
	return S_OK;
}

HRESULT WINAPI object_name(void *iface, HSTRING *name)
{
	const WCHAR *text = owner(iface)->kind->class_name;
	if (!name)
		return E_POINTER;
	return WindowsCreateString(text, (UINT32)lstrlenW(text), name);
}

HRESULT WINAPI object_trust(void *iface, TrustLevel *level)
{
	if (!level)
		return E_POINTER;
	*level = BaseTrust;
	return S_OK;
}

/* A new object of a kind with its interface tables in place order, and one reference. */
void *make(size_t size, const struct kind *kind, const void *const *const *tables, size_t count)
{
	struct object *self = ALLOC(size);
	if (!self)
		return NULL;
	for (size_t index = 0; index < count; ++index)
		self->faces[index] = tables[index];
	self->kind = kind;
	self->refs = 1;
	InterlockedIncrement(&objects);
	return self;
}

void *face_of(struct object *self, size_t face) { return &self->faces[face]; }

void release(void *iface)
{
	if (iface)
		((IUnknown *)iface)->lpVtbl->Release((IUnknown *)iface);
}

void reference(void *iface)
{
	if (iface)
		((IUnknown *)iface)->lpVtbl->AddRef((IUnknown *)iface);
}

HRESULT answer_flag(boolean *out, boolean value)
{
	if (!out)
		return E_POINTER;
	*out = value;
	return S_OK;
}

HRESULT answer_null(void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return S_OK;
}

/* An event takes a handler for something that never happens: it gets a token of its own and is not kept. */
HRESULT WINAPI event_add(void *iface, void *handler, INT64 *out)
{
	if (!handler || !out)
		return E_INVALIDARG;
	*out = InterlockedIncrement(&tokens);
	return S_OK;
}

HRESULT WINAPI event_remove(void *iface, INT64 token) { return S_OK; }

/* The activation factory face of a class that has only static members: the class cannot be instantiated. */
HRESULT WINAPI static_activate(void *iface, IInspectable **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

void *method(void *iface, size_t slot) { return (*(void ***)iface)[slot]; }

HRESULT handlers_add(struct handlers *list, void *handler, INT64 *token)
{
	if (!handler || !token)
		return E_INVALIDARG;
	if (list->count == list->capacity) {
		const UINT32 capacity = list->capacity ? list->capacity * 2 : 4;
		struct handler_entry *grown = list->entries ? HeapReAlloc(GetProcessHeap(), 0, list->entries, capacity * sizeof *grown)
			: ALLOC(capacity * sizeof *grown);
		if (!grown)
			return E_OUTOFMEMORY;
		list->entries = grown;
		list->capacity = capacity;
	}
	reference(handler);
	list->entries[list->count].handler = handler;
	list->entries[list->count++].token = *token = InterlockedIncrement(&tokens);
	return S_OK;
}

HRESULT handlers_remove(struct handlers *list, INT64 token)
{
	for (UINT32 index = 0; index < list->count; ++index)
		if (list->entries[index].token == token) {
			release(list->entries[index].handler);
			list->entries[index] = list->entries[--list->count];
			return S_OK;
		}
	return S_OK;
}

void handlers_raise(struct handlers *list, void *sender, void *arguments)
{
	const UINT32 count = list->count;
	if (!count)
		return;
	IUnknown **called = ALLOC(count * sizeof *called);
	if (!called)
		return;
	for (UINT32 index = 0; index < count; ++index)
		reference(called[index] = list->entries[index].handler);
	for (UINT32 index = 0; index < count; ++index) {
		((HRESULT (WINAPI *)(void *, void *, void *))method(called[index], 3))(called[index], sender, arguments);
		release(called[index]);
	}
	FREE(called);
}

void handlers_clear(struct handlers *list)
{
	for (UINT32 index = 0; index < list->count; ++index)
		release(list->entries[index].handler);
	FREE(list->entries);
	list->entries = NULL;
	list->count = list->capacity = 0;
}
