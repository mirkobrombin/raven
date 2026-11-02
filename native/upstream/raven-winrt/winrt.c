/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The shared plumbing of Raven's Windows Runtime components; see winrt.h. */
#define INITGUID
#include "winrt.h"

/* {f2f0b0c8-3a9c-4f6b-9f1e-52e2f6c0a7d1}: matches no interface; entries holding it answer the object's own identifier. */
const IID WINRT_OWN = {0xf2f0b0c8, 0x3a9c, 0x4f6b, {0x9f, 0x1e, 0x52, 0xe2, 0xf6, 0xc0, 0xa7, 0xd1}};
const IID WINRT_OWN_2 = {0xf2f0b0c8, 0x3a9c, 0x4f6b, {0x9f, 0x1e, 0x52, 0xe2, 0xf6, 0xc0, 0xa7, 0xd2}};
const IID WINRT_OWN_3 = {0xf2f0b0c8, 0x3a9c, 0x4f6b, {0x9f, 0x1e, 0x52, 0xe2, 0xf6, 0xc0, 0xa7, 0xd3}};
/* IAgileObject, which marks an object any thread may call without marshaling. */
static const IID agile_object = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

/* Objects. */

static LONG live_objects;

BOOL winrt_in_use(void) { return InterlockedCompareExchange(&live_objects, 0, 0) != 0; }

static const IID *entry_iid(const struct winrt_object *object, const struct winrt_entry *entry)
{
	return entry->iid == &WINRT_OWN ? object->own[0] : entry->iid == &WINRT_OWN_2 ? object->own[1] : entry->iid == &WINRT_OWN_3 ?
		object->own[2] : entry->iid;
}

void *winrt_get(struct winrt_object *object, REFIID iid)
{
	const struct winrt_entry *entries = object->kind->entries;
	const struct winrt_entry *found = NULL;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &agile_object))
		found = entries;
	for (const struct winrt_entry *entry = entries; !found && entry->iid; ++entry)
		if (entry_iid(object, entry) && IsEqualIID(iid, entry_iid(object, entry)))
			found = entry;
	if (!found)
		return NULL;
	InterlockedIncrement(&object->refs);
	return (char *)object + found->offset;
}

HRESULT STDMETHODCALLTYPE winrt_query(void *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = winrt_get(winrt_object_of(iface), iid);
	return *out ? S_OK : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE winrt_add_ref(void *iface) { return (ULONG)InterlockedIncrement(&winrt_object_of(iface)->refs); }

ULONG STDMETHODCALLTYPE winrt_release(void *iface)
{
	struct winrt_object *object = winrt_object_of(iface);
	const LONG refs = InterlockedDecrement(&object->refs);
	if (!refs) {
		if (object->kind->destroy)
			object->kind->destroy(object);
		WINRT_FREE(object);
		InterlockedDecrement(&live_objects);
	}
	return (ULONG)refs;
}

HRESULT STDMETHODCALLTYPE winrt_iids(void *iface, ULONG *count, IID **iids)
{
	if (!count || !iids)
		return E_POINTER;
	const struct winrt_object *object = winrt_object_of(iface);
	ULONG total = 0;
	for (const struct winrt_entry *entry = object->kind->entries; entry->iid; ++entry)
		++total;
	*iids = total ? CoTaskMemAlloc(total * sizeof(IID)) : NULL;
	if (total && !*iids)
		return E_OUTOFMEMORY;
	for (ULONG index = 0; index < total; ++index)
		(*iids)[index] = *entry_iid(object, &object->kind->entries[index]);
	*count = total;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE winrt_class_name(void *iface, HSTRING *name)
{
	if (!name)
		return E_POINTER;
	return winrt_string(winrt_object_of(iface)->kind->name, name);
}

HRESULT STDMETHODCALLTYPE winrt_trust(void *iface, TrustLevel *level)
{
	if (!level)
		return E_POINTER;
	*level = BaseTrust;
	return S_OK;
}

void *winrt_new(const struct winrt_class *kind, size_t size, const IID *own) { return winrt_new_own(kind, size, own, NULL, NULL); }

void *winrt_new_own(const struct winrt_class *kind, size_t size, const IID *own, const IID *own_2, const IID *own_3)
{
	struct winrt_object *object = WINRT_ALLOC(size);
	if (!object)
		return NULL;
	InterlockedIncrement(&live_objects);
	object->kind = kind;
	object->refs = 1;
	object->own[0] = own;
	object->own[1] = own_2;
	object->own[2] = own_3;
	for (const struct winrt_entry *entry = kind->entries; entry->iid; ++entry)
		((struct winrt_interface *)((char *)object + entry->offset))->object = object;
	return object;
}

/* Strings. */

HRESULT winrt_string_n(const WCHAR *text, UINT32 length, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsCreateString(text, length, out);
}

HRESULT winrt_string(const WCHAR *text, HSTRING *out) { return winrt_string_n(text, text ? lstrlenW(text) : 0, out); }

const WCHAR *winrt_text(HSTRING string, UINT32 *length)
{
	UINT32 ignored;
	return WindowsGetStringRawBuffer(string, length ? length : &ignored);
}

static WCHAR fold(WCHAR character) { return character >= 'A' && character <= 'Z' ? (WCHAR)(character + ('a' - 'A')) : character; }

BOOL winrt_equal_ascii(const WCHAR *left, UINT32 left_length, const WCHAR *right, UINT32 right_length)
{
	if (left_length != right_length)
		return FALSE;
	for (UINT32 index = 0; index < left_length; ++index)
		if (fold(left[index]) != fold(right[index]))
			return FALSE;
	return TRUE;
}

UINT32 winrt_decimal(UINT64 number, WCHAR *text)
{
	WCHAR reversed[20];
	UINT32 length = 0;
	do {
		reversed[length++] = (WCHAR)('0' + number % 10);
		number /= 10;
	} while (number);
	for (UINT32 index = 0; index < length; ++index)
		text[index] = reversed[length - 1 - index];
	return length;
}

char *winrt_utf8(const WCHAR *text, UINT32 length, UINT32 *bytes)
{
	const int needed = length ? WideCharToMultiByte(CP_UTF8, 0, text, (int)length, NULL, 0, NULL, NULL) : 0;
	char *out = WINRT_ALLOC((size_t)needed + 1);
	if (!out)
		return NULL;
	if (needed)
		WideCharToMultiByte(CP_UTF8, 0, text, (int)length, out, needed, NULL, NULL);
	if (bytes)
		*bytes = (UINT32)needed;
	return out;
}

WCHAR *winrt_utf16(const char *text, UINT32 bytes, UINT32 *length)
{
	const int needed = bytes ? MultiByteToWideChar(CP_UTF8, 0, text, (int)bytes, NULL, 0) : 0;
	WCHAR *out = WINRT_ALLOC(((size_t)needed + 1) * sizeof(WCHAR));
	if (!out)
		return NULL;
	if (needed)
		MultiByteToWideChar(CP_UTF8, 0, text, (int)bytes, out, needed);
	if (length)
		*length = (UINT32)needed;
	return out;
}

/* Asynchronous operations. */

struct winrt_async {
	struct winrt_object base;
	struct winrt_interface operation, info;
	CRITICAL_SECTION lock;
	AsyncStatus status;
	HRESULT error;
	BOOL closed;
	LONG canceled;
	UINT32 id;
	enum winrt_result kind;
	union winrt_value result;
	IUnknown *completed, *progress;
	winrt_work work;
	void *context;
	void (*free_context)(void *);
};

/* The completion handler of any operation: IUnknown and Invoke(operation, status). */
typedef struct {
	IUnknownVtbl base;
	HRESULT (STDMETHODCALLTYPE *Invoke)(IUnknown *handler, void *operation, AsyncStatus status);
} completed_table;

static LONG operation_ids;

static struct winrt_async *async_of(void *iface) { return (struct winrt_async *)winrt_object_of(iface); }

static void clear_result(struct winrt_async *self)
{
	if (self->kind == WINRT_RESULT_OBJECT)
		winrt_put(self->result.object);
	else if (self->kind == WINRT_RESULT_STRING)
		WindowsDeleteString(self->result.string);
	self->result.uint64 = 0;
}

static void async_destroy(struct winrt_object *object)
{
	struct winrt_async *self = (struct winrt_async *)object;
	clear_result(self);
	winrt_put(self->completed);
	winrt_put(self->progress);
	if (self->free_context)
		self->free_context(self->context);
	DeleteCriticalSection(&self->lock);
}

static void invoke_completed(struct winrt_async *self, IUnknown *handler, AsyncStatus status)
{
	((const completed_table *)handler->lpVtbl)->Invoke(handler, &self->operation, status);
}

static HRESULT STDMETHODCALLTYPE async_put_progress(void *iface, IUnknown *handler)
{
	struct winrt_async *self = async_of(iface);
	EnterCriticalSection(&self->lock);
	HRESULT result = self->closed ? E_ILLEGAL_METHOD_CALL : S_OK;
	if (SUCCEEDED(result)) {
		if (handler)
			handler->lpVtbl->AddRef(handler);
		winrt_put(self->progress);
		self->progress = handler;
	}
	LeaveCriticalSection(&self->lock);
	return result;
}

static HRESULT STDMETHODCALLTYPE async_get_progress(void *iface, IUnknown **handler)
{
	struct winrt_async *self = async_of(iface);
	if (!handler)
		return E_POINTER;
	EnterCriticalSection(&self->lock);
	*handler = self->progress;
	if (*handler)
		(*handler)->lpVtbl->AddRef(*handler);
	LeaveCriticalSection(&self->lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE async_put_completed(void *iface, IUnknown *handler)
{
	struct winrt_async *self = async_of(iface);
	EnterCriticalSection(&self->lock);
	HRESULT result = self->closed ? E_ILLEGAL_METHOD_CALL : self->completed ? E_ILLEGAL_DELEGATE_ASSIGNMENT : S_OK;
	AsyncStatus status = self->status;
	if (SUCCEEDED(result) && handler) {
		handler->lpVtbl->AddRef(handler);
		self->completed = handler;
	}
	LeaveCriticalSection(&self->lock);
	/* An operation that has already ended calls the handler at once. */
	if (SUCCEEDED(result) && handler && status != Started)
		invoke_completed(self, handler, status);
	return result;
}

static HRESULT STDMETHODCALLTYPE async_get_completed(void *iface, IUnknown **handler)
{
	struct winrt_async *self = async_of(iface);
	if (!handler)
		return E_POINTER;
	EnterCriticalSection(&self->lock);
	*handler = self->completed;
	if (*handler)
		(*handler)->lpVtbl->AddRef(*handler);
	LeaveCriticalSection(&self->lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE async_results(void *iface, void *out)
{
	struct winrt_async *self = async_of(iface);
	if (!out)
		return E_POINTER;
	EnterCriticalSection(&self->lock);
	HRESULT result = self->closed ? E_ILLEGAL_METHOD_CALL : self->status == Started ? E_ILLEGAL_METHOD_CALL :
		self->status == Canceled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : self->status == Error ? self->error : S_OK;
	if (SUCCEEDED(result))
		switch (self->kind) {
		case WINRT_RESULT_OBJECT:
			*(void **)out = self->result.object;
			if (self->result.object)
				((IUnknown *)self->result.object)->lpVtbl->AddRef(self->result.object);
			break;
		case WINRT_RESULT_STRING:
			result = WindowsDuplicateString(self->result.string, out);
			break;
		case WINRT_RESULT_UINT64:
			*(UINT64 *)out = self->result.uint64;
			break;
		case WINRT_RESULT_UINT32:
			*(UINT32 *)out = self->result.uint32;
			break;
		case WINRT_RESULT_BOOLEAN:
			*(boolean *)out = self->result.boolean;
			break;
		}
	LeaveCriticalSection(&self->lock);
	return result;
}

static HRESULT STDMETHODCALLTYPE info_id(void *iface, unsigned int *id)
{
	if (!id)
		return E_POINTER;
	*id = async_of(iface)->id;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE info_status(void *iface, AsyncStatus *status)
{
	struct winrt_async *self = async_of(iface);
	if (!status)
		return E_POINTER;
	EnterCriticalSection(&self->lock);
	*status = self->status;
	const BOOL closed = self->closed;
	LeaveCriticalSection(&self->lock);
	return closed ? E_ILLEGAL_METHOD_CALL : S_OK;
}

static HRESULT STDMETHODCALLTYPE info_error(void *iface, HRESULT *error)
{
	struct winrt_async *self = async_of(iface);
	if (!error)
		return E_POINTER;
	EnterCriticalSection(&self->lock);
	*error = self->status == Error ? self->error : self->status == Canceled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK;
	const BOOL closed = self->closed;
	LeaveCriticalSection(&self->lock);
	return closed ? E_ILLEGAL_METHOD_CALL : S_OK;
}

static HRESULT STDMETHODCALLTYPE info_cancel(void *iface)
{
	struct winrt_async *self = async_of(iface);
	EnterCriticalSection(&self->lock);
	const HRESULT result = self->closed ? E_ILLEGAL_METHOD_CALL : S_OK;
	if (SUCCEEDED(result) && self->status == Started)
		InterlockedExchange(&self->canceled, 1);
	LeaveCriticalSection(&self->lock);
	return result;
}

static HRESULT STDMETHODCALLTYPE info_close(void *iface)
{
	struct winrt_async *self = async_of(iface);
	EnterCriticalSection(&self->lock);
	HRESULT result = S_OK;
	if (self->status == Started)
		result = E_ILLEGAL_STATE_CHANGE;
	else if (!self->closed) {
		self->closed = TRUE;
		clear_result(self);
	}
	LeaveCriticalSection(&self->lock);
	return result;
}

BOOL winrt_async_canceled(struct winrt_async *operation) { return InterlockedCompareExchange(&operation->canceled, 0, 0) != 0; }

static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *put_progress, *get_progress, *put_completed, *get_completed, *results;
} progress_table = {WINRT_INSPECTABLE, async_put_progress, async_get_progress, async_put_completed, async_get_completed, async_results};

static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *put_completed, *get_completed, *results;
} plain_table = {WINRT_INSPECTABLE, async_put_completed, async_get_completed, async_results};

static const IAsyncInfoVtbl info_table = {WINRT_INSPECTABLE, (void *)info_id, (void *)info_status, (void *)info_error, (void *)info_cancel,
	(void *)info_close};

static const struct winrt_entry async_entries[] = {
	WINRT_ENTRY(struct winrt_async, operation, WINRT_OWN), WINRT_ENTRY(struct winrt_async, info, IID_IAsyncInfo), WINRT_END};
static const struct winrt_class async_class = {L"Windows.Foundation.IAsyncOperation", async_entries, async_destroy};

static DWORD WINAPI async_run(void *parameter)
{
	struct winrt_async *self = parameter;
	union winrt_value value = {0};
	const HRESULT outcome = self->work(self, self->context, &value);
	EnterCriticalSection(&self->lock);
	if (winrt_async_canceled(self) && outcome == E_ABORT)
		self->status = Canceled;
	else if (FAILED(outcome)) {
		self->status = Error;
		self->error = outcome;
	} else {
		self->status = Completed;
		self->result = value;
		value.uint64 = 0;
	}
	IUnknown *handler = self->completed;
	if (handler)
		handler->lpVtbl->AddRef(handler);
	const AsyncStatus status = self->status;
	LeaveCriticalSection(&self->lock);
	if (value.uint64 && self->kind == WINRT_RESULT_OBJECT)
		winrt_put(value.object);
	else if (value.uint64 && self->kind == WINRT_RESULT_STRING)
		WindowsDeleteString(value.string);
	if (handler) {
		invoke_completed(self, handler, status);
		handler->lpVtbl->Release(handler);
	}
	winrt_release(&self->operation);
	return 0;
}

HRESULT winrt_async_start(const IID *operation_iid, BOOL with_progress, enum winrt_result kind, winrt_work work, void *context,
	void (*free_context)(void *), void **out)
{
	if (!out) {
		if (free_context)
			free_context(context);
		return E_POINTER;
	}
	*out = NULL;
	struct winrt_async *self = winrt_new(&async_class, sizeof(*self), operation_iid);
	if (!self) {
		if (free_context)
			free_context(context);
		return E_OUTOFMEMORY;
	}
	self->operation.table = with_progress ? (const void *)&progress_table : (const void *)&plain_table;
	self->info.table = &info_table;
	InitializeCriticalSection(&self->lock);
	self->status = Started;
	self->id = (UINT32)InterlockedIncrement(&operation_ids);
	self->kind = kind;
	self->work = work;
	self->context = context;
	self->free_context = free_context;
	/* The thread holds a reference until the work has run. */
	winrt_add_ref(&self->operation);
	HANDLE thread = CreateThread(NULL, 0, async_run, self, 0, NULL);
	if (!thread) {
		const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
		winrt_release(&self->operation);
		winrt_release(&self->operation);
		return error;
	}
	CloseHandle(thread);
	*out = &self->operation;
	return S_OK;
}

/* Waiting for an operation: a completion handler that signals an event. */
struct waiter {
	const void *table;
	LONG refs;
	HANDLE event;
};

static HRESULT STDMETHODCALLTYPE waiter_query(struct waiter *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &agile_object)) {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	*out = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE waiter_add_ref(struct waiter *self) { return (ULONG)InterlockedIncrement(&self->refs); }

static ULONG STDMETHODCALLTYPE waiter_release(struct waiter *self)
{
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		CloseHandle(self->event);
		WINRT_FREE(self);
	}
	return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE waiter_invoke(struct waiter *self, void *operation, AsyncStatus status)
{
	SetEvent(self->event);
	return S_OK;
}

static const struct {
	void *query, *add_ref, *release, *invoke;
} waiter_table = {waiter_query, waiter_add_ref, waiter_release, waiter_invoke};

HRESULT winrt_wait(void *operation, BOOL with_progress, void *result)
{
	/* The methods of IAsyncOperation and IAsyncOperationWithProgress after IInspectable's six: put_Completed then GetResults two
	 * places on, after the progress handler's pair for the second. */
	typedef HRESULT (STDMETHODCALLTYPE *method)(void *, void *);
	method *table = *(method **)operation;
	const unsigned completed = with_progress ? 8 : 6;
	struct waiter *waiter = WINRT_ALLOC(sizeof(*waiter));
	if (!waiter)
		return E_OUTOFMEMORY;
	waiter->table = &waiter_table;
	waiter->refs = 1;
	waiter->event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!waiter->event) {
		WINRT_FREE(waiter);
		return HRESULT_FROM_WIN32(GetLastError());
	}
	HRESULT outcome = table[completed](operation, waiter);
	if (SUCCEEDED(outcome))
		WaitForSingleObject(waiter->event, INFINITE);
	waiter_release(waiter);
	return SUCCEEDED(outcome) ? table[completed + 2](operation, result) : outcome;
}

/* Objects in a list. */

struct items {
	struct winrt_object base;
	struct winrt_interface view, iterable, iterator;
	const struct winrt_items_kind *kind;
	IInspectable **items;
	UINT32 count, position;
};

static struct items *items_of(void *iface) { return (struct items *)winrt_object_of(iface); }

static void items_destroy(struct winrt_object *object)
{
	struct items *self = (struct items *)object;
	for (UINT32 index = 0; index < self->count; ++index)
		winrt_put(self->items[index]);
	WINRT_FREE(self->items);
}

static HRESULT item_out(IInspectable *item, void **out)
{
	if (item)
		item->lpVtbl->AddRef(item);
	*out = item;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE items_at(void *iface, UINT32 index, void **out)
{
	struct items *self = items_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return index < self->count ? item_out(self->items[index], out) : E_BOUNDS;
}

static HRESULT STDMETHODCALLTYPE items_size(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = items_of(iface)->count;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE items_index(void *iface, IInspectable *value, UINT32 *index, boolean *found)
{
	struct items *self = items_of(iface);
	if (!index || !found)
		return E_POINTER;
	*index = 0;
	*found = FALSE;
	for (UINT32 at = 0; at < self->count; ++at)
		if (self->items[at] == value) {
			*index = at;
			*found = TRUE;
			break;
		}
	return S_OK;
}

static HRESULT copy_items(struct items *self, UINT32 start, UINT32 capacity, void **into, UINT32 *count)
{
	if (!count || (capacity && !into))
		return E_POINTER;
	UINT32 taken = 0;
	for (; start + taken < self->count && taken < capacity; ++taken)
		item_out(self->items[start + taken], &into[taken]);
	*count = taken;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE items_many(void *iface, UINT32 start, UINT32 capacity, void **into, UINT32 *count)
{
	return copy_items(items_of(iface), start, capacity, into, count);
}

static HRESULT STDMETHODCALLTYPE items_first(void *iface, void **out)
{
	struct items *self = items_of(iface);
	if (!out)
		return E_POINTER;
	return winrt_items(self->kind, self->items, self->count, TRUE, out);
}

static HRESULT STDMETHODCALLTYPE items_current(void *iface, void **out)
{
	struct items *self = items_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->position < self->count ? item_out(self->items[self->position], out) : E_BOUNDS;
}

static HRESULT STDMETHODCALLTYPE items_has_current(void *iface, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = items_of(iface)->position < items_of(iface)->count;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE items_move_next(void *iface, boolean *out)
{
	struct items *self = items_of(iface);
	if (!out)
		return E_POINTER;
	if (self->position < self->count)
		++self->position;
	*out = self->position < self->count;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE items_take(void *iface, UINT32 capacity, void **into, UINT32 *count)
{
	struct items *self = items_of(iface);
	const HRESULT result = copy_items(self, self->position, capacity, into, count);
	if (SUCCEEDED(result))
		self->position += *count;
	return result;
}

static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *at, *size, *index, *many;
} items_view_table = {WINRT_INSPECTABLE, items_at, items_size, items_index, items_many};
static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *first;
} items_iterable_table = {WINRT_INSPECTABLE, items_first};
static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *current, *has_current, *move_next, *many;
} items_iterator_table = {WINRT_INSPECTABLE, items_current, items_has_current, items_move_next, items_take};
static const struct winrt_entry items_view_entries[] = {
	WINRT_ENTRY(struct items, view, WINRT_OWN), WINRT_ENTRY(struct items, iterable, WINRT_OWN_2), WINRT_END};
static const struct winrt_entry items_iterator_entries[] = {WINRT_ENTRY(struct items, iterator, WINRT_OWN_3), WINRT_END};
static const struct winrt_class items_view_class = {L"Windows.Foundation.Collections.IVectorView`1", items_view_entries, items_destroy};
static const struct winrt_class items_iterator_class = {L"Windows.Foundation.Collections.IIterator`1", items_iterator_entries, items_destroy};

HRESULT winrt_items(const struct winrt_items_kind *kind, IInspectable **items, UINT32 count, BOOL iterator, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct items *self = winrt_new_own(iterator ? &items_iterator_class : &items_view_class, sizeof(*self), kind->view, kind->iterable,
		kind->iterator);
	if (!self)
		return E_OUTOFMEMORY;
	self->view.table = &items_view_table;
	self->iterable.table = &items_iterable_table;
	self->iterator.table = &items_iterator_table;
	self->kind = kind;
	if (count && !(self->items = WINRT_ALLOC(count * sizeof(*items)))) {
		winrt_release(iterator ? &self->iterator : &self->view);
		return E_OUTOFMEMORY;
	}
	for (UINT32 index = 0; index < count; ++index)
		item_out(items[index], (void **)&self->items[index]);
	self->count = count;
	*out = iterator ? &self->iterator : &self->view;
	return S_OK;
}

/* Boxed values. */

struct box {
	struct winrt_object base;
	struct winrt_interface reference;
	size_t size;
	BYTE value[16];
};

static HRESULT STDMETHODCALLTYPE box_value(void *iface, void *value)
{
	struct box *self = (struct box *)winrt_object_of(iface);
	if (!value)
		return E_POINTER;
	memcpy(value, self->value, self->size);
	return S_OK;
}

static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *value;
} box_table = {WINRT_INSPECTABLE, box_value};
static const struct winrt_entry box_entries[] = {WINRT_ENTRY(struct box, reference, WINRT_OWN), WINRT_END};
static const struct winrt_class box_class = {L"Windows.Foundation.IReference`1", box_entries, NULL};

HRESULT winrt_box(const IID *reference_iid, const void *value, size_t size, void **out)
{
	if (!out)
		return E_POINTER;
	if (size > sizeof(((struct box *)0)->value))
		return E_INVALIDARG;
	struct box *self = winrt_new(&box_class, sizeof(*self), reference_iid);
	if (!self)
		return E_OUTOFMEMORY;
	self->reference.table = &box_table;
	self->size = size;
	memcpy(self->value, value, size);
	*out = &self->reference;
	return S_OK;
}

/* Buffers. */

struct buffer {
	struct winrt_object base;
	struct winrt_interface buffer, access;
	UINT32 capacity, length;
	BYTE *bytes;
};

static struct buffer *buffer_of(void *iface) { return (struct buffer *)winrt_object_of(iface); }

static HRESULT STDMETHODCALLTYPE buffer_capacity(void *iface, UINT32 *value)
{
	if (!value)
		return E_POINTER;
	*value = buffer_of(iface)->capacity;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE buffer_length(void *iface, UINT32 *value)
{
	if (!value)
		return E_POINTER;
	*value = buffer_of(iface)->length;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE buffer_put_length(void *iface, UINT32 value)
{
	struct buffer *self = buffer_of(iface);
	if (value > self->capacity)
		return E_INVALIDARG;
	self->length = value;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE buffer_bytes(void *iface, BYTE **value)
{
	if (!value)
		return E_POINTER;
	*value = buffer_of(iface)->bytes;
	return S_OK;
}

static void buffer_destroy(struct winrt_object *object) { WINRT_FREE(((struct buffer *)object)->bytes); }

static const __x_ABI_CWindows_CStorage_CStreams_CIBufferVtbl buffer_table = {WINRT_INSPECTABLE, (void *)buffer_capacity, (void *)buffer_length,
	(void *)buffer_put_length};
static const __x_Windows_CStorage_CStreams_CIBufferByteAccessVtbl access_table = {(void *)winrt_query, (void *)winrt_add_ref, (void *)winrt_release, (void *)buffer_bytes};
static const struct winrt_entry buffer_entries[] = {
	WINRT_ENTRY(struct buffer, buffer, IID___x_ABI_CWindows_CStorage_CStreams_CIBuffer),
	WINRT_ENTRY(struct buffer, access, IID___x_Windows_CStorage_CStreams_CIBufferByteAccess), WINRT_END};
static const struct winrt_class buffer_class = {L"Windows.Storage.Streams.Buffer", buffer_entries, buffer_destroy};

HRESULT winrt_buffer(const BYTE *bytes, UINT32 length, UINT32 capacity, __x_ABI_CWindows_CStorage_CStreams_CIBuffer **out)
{
	if (!out)
		return E_POINTER;
	if (capacity < length)
		capacity = length;
	struct buffer *self = winrt_new(&buffer_class, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->buffer.table = &buffer_table;
	self->access.table = &access_table;
	self->bytes = WINRT_ALLOC(capacity ? capacity : 1);
	if (!self->bytes) {
		winrt_release(&self->buffer);
		return E_OUTOFMEMORY;
	}
	self->capacity = capacity;
	if (bytes) {
		memcpy(self->bytes, bytes, length);
		self->length = length;
	}
	*out = (void *)&self->buffer;
	return S_OK;
}

HRESULT winrt_buffer_bytes(__x_ABI_CWindows_CStorage_CStreams_CIBuffer *buffer, BYTE **bytes, UINT32 *length)
{
	__x_Windows_CStorage_CStreams_CIBufferByteAccess *access = NULL;
	if (!buffer || !bytes || !length)
		return E_POINTER;
	HRESULT result = buffer->lpVtbl->QueryInterface(buffer, &IID___x_Windows_CStorage_CStreams_CIBufferByteAccess, (void **)&access);
	if (SUCCEEDED(result)) {
		result = access->lpVtbl->Buffer(access, bytes);
		access->lpVtbl->Release(access);
	}
	if (SUCCEEDED(result))
		result = buffer->lpVtbl->get_Length(buffer, length);
	return result;
}

/* Maps of strings. */

static struct winrt_map *map_of(void *iface) { return CONTAINING_RECORD(iface, struct winrt_map, map); }
static struct winrt_map *map_of_iterable(void *iface) { return CONTAINING_RECORD(iface, struct winrt_map, iterable); }

static BOOL same_key(const struct winrt_map *map, const WCHAR *left, UINT32 left_length, HSTRING right)
{
	UINT32 right_length;
	const WCHAR *text = winrt_text(right, &right_length);
	if (map->ignore_case)
		return winrt_equal_ascii(left, left_length, text, right_length);
	return left_length == right_length && !memcmp(left, text, left_length * sizeof(WCHAR));
}

static INT32 find_key(const struct winrt_map *map, const WCHAR *key, UINT32 length)
{
	for (UINT32 index = 0; index < map->count; ++index)
		if (same_key(map, key, length, map->pairs[index].key))
			return (INT32)index;
	return -1;
}

void winrt_map_init(struct winrt_map *map, BOOL ignore_case)
{
	map->map.table = &winrt_map_table;
	map->iterable.table = &winrt_map_iterable_table;
	map->ignore_case = ignore_case;
	InitializeCriticalSection(&map->lock);
}

void winrt_map_free(struct winrt_map *map)
{
	for (UINT32 index = 0; index < map->count; ++index) {
		WindowsDeleteString(map->pairs[index].key);
		WindowsDeleteString(map->pairs[index].value);
	}
	WINRT_FREE(map->pairs);
	map->pairs = NULL;
	map->count = map->capacity = 0;
	DeleteCriticalSection(&map->lock);
}

static HRESULT map_set_locked(struct winrt_map *map, const WCHAR *key, UINT32 key_length, const WCHAR *value, UINT32 value_length,
	BOOL append, boolean *replaced)
{
	const INT32 found = find_key(map, key, key_length);
	HSTRING text = NULL;
	HRESULT result;
	if (found >= 0 && append) {
		UINT32 old_length;
		const WCHAR *old = winrt_text(map->pairs[found].value, &old_length);
		WCHAR *joined;
		if (!old_length)
			result = winrt_string_n(value, value_length, &text);
		else if (!(joined = WINRT_ALLOC(((size_t)old_length + 2 + value_length) * sizeof(WCHAR))))
			result = E_OUTOFMEMORY;
		else {
			memcpy(joined, old, old_length * sizeof(WCHAR));
			joined[old_length] = ',';
			joined[old_length + 1] = ' ';
			memcpy(joined + old_length + 2, value, value_length * sizeof(WCHAR));
			result = winrt_string_n(joined, old_length + 2 + value_length, &text);
			WINRT_FREE(joined);
		}
	} else
		result = winrt_string_n(value, value_length, &text);
	if (FAILED(result))
		return result;
	if (replaced)
		*replaced = found >= 0;
	if (found >= 0) {
		WindowsDeleteString(map->pairs[found].value);
		map->pairs[found].value = text;
		return S_OK;
	}
	if (map->count == map->capacity) {
		const UINT32 capacity = map->capacity ? map->capacity * 2 : 8;
		void *grown = map->pairs ? WINRT_REALLOC(map->pairs, capacity * sizeof(*map->pairs)) : WINRT_ALLOC(capacity * sizeof(*map->pairs));
		if (!grown) {
			WindowsDeleteString(text);
			return E_OUTOFMEMORY;
		}
		map->pairs = grown;
		map->capacity = capacity;
	}
	HSTRING name;
	if (FAILED(result = winrt_string_n(key, key_length, &name))) {
		WindowsDeleteString(text);
		return result;
	}
	map->pairs[map->count].key = name;
	map->pairs[map->count].value = text;
	++map->count;
	return S_OK;
}

HRESULT winrt_map_set(struct winrt_map *map, const WCHAR *key, UINT32 key_length, const WCHAR *value, UINT32 value_length, BOOL append)
{
	EnterCriticalSection(&map->lock);
	const HRESULT result = map_set_locked(map, key, key_length, value, value_length, append, NULL);
	LeaveCriticalSection(&map->lock);
	return result;
}

HRESULT winrt_map_get(struct winrt_map *map, const WCHAR *key, HSTRING *value)
{
	EnterCriticalSection(&map->lock);
	const INT32 found = find_key(map, key, lstrlenW(key));
	const HRESULT result = found < 0 ? E_BOUNDS : WindowsDuplicateString(map->pairs[found].value, value);
	LeaveCriticalSection(&map->lock);
	return result;
}

static BOOL map_remove_locked(struct winrt_map *map, const WCHAR *key, UINT32 key_length)
{
	const INT32 found = find_key(map, key, key_length);
	if (found < 0)
		return FALSE;
	WindowsDeleteString(map->pairs[found].key);
	WindowsDeleteString(map->pairs[found].value);
	memmove(map->pairs + found, map->pairs + found + 1, (map->count - (UINT32)found - 1) * sizeof(*map->pairs));
	--map->count;
	return TRUE;
}

BOOL winrt_map_remove(struct winrt_map *map, const WCHAR *key, UINT32 key_length)
{
	EnterCriticalSection(&map->lock);
	const BOOL removed = map_remove_locked(map, key, key_length);
	LeaveCriticalSection(&map->lock);
	return removed;
}

HRESULT winrt_map_each(struct winrt_map *map, HRESULT (*visit)(void *context, HSTRING key, HSTRING value), void *context)
{
	HRESULT result = S_OK;
	EnterCriticalSection(&map->lock);
	for (UINT32 index = 0; index < map->count && SUCCEEDED(result); ++index)
		result = visit(context, map->pairs[index].key, map->pairs[index].value);
	LeaveCriticalSection(&map->lock);
	return result;
}

/* A key and its value, IKeyValuePair<String, String>. */
struct pair {
	struct winrt_object base;
	struct winrt_interface pair;
	HSTRING key, value;
};

static HRESULT STDMETHODCALLTYPE pair_key(void *iface, HSTRING *out) { return WindowsDuplicateString(((struct pair *)winrt_object_of(iface))->key, out); }
static HRESULT STDMETHODCALLTYPE pair_value(void *iface, HSTRING *out) { return WindowsDuplicateString(((struct pair *)winrt_object_of(iface))->value, out); }

static void pair_destroy(struct winrt_object *object)
{
	WindowsDeleteString(((struct pair *)object)->key);
	WindowsDeleteString(((struct pair *)object)->value);
}

static const __FIKeyValuePair_2_HSTRING_HSTRINGVtbl pair_table = {WINRT_INSPECTABLE, (void *)pair_key, (void *)pair_value};
static const struct winrt_entry pair_entries[] = {WINRT_ENTRY(struct pair, pair, IID___FIKeyValuePair_2_HSTRING_HSTRING), WINRT_END};
static const struct winrt_class pair_class = {L"Windows.Foundation.Collections.IKeyValuePair`2<String, String>", pair_entries, pair_destroy};

static HRESULT make_pair(const struct winrt_pair *from, __FIKeyValuePair_2_HSTRING_HSTRING **out)
{
	struct pair *self = winrt_new(&pair_class, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->pair.table = &pair_table;
	WindowsDuplicateString(from->key, &self->key);
	WindowsDuplicateString(from->value, &self->value);
	*out = (void *)&self->pair;
	return S_OK;
}

/* A snapshot of the pairs of a map: the iterator its First hands out, and the view its GetView hands out. Both see the map as it was. */
struct snapshot {
	struct winrt_object base;
	struct winrt_interface iterator, view, iterable;
	struct winrt_map copy;
	UINT32 position;
};

static struct snapshot *snapshot_of(void *iface) { return (struct snapshot *)winrt_object_of(iface); }

static void snapshot_destroy(struct winrt_object *object) { winrt_map_free(&((struct snapshot *)object)->copy); }

static HRESULT STDMETHODCALLTYPE iterator_current(void *iface, __FIKeyValuePair_2_HSTRING_HSTRING **out)
{
	struct snapshot *self = snapshot_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->position >= self->copy.count)
		return E_BOUNDS;
	return make_pair(&self->copy.pairs[self->position], out);
}

static HRESULT STDMETHODCALLTYPE iterator_has_current(void *iface, boolean *out)
{
	struct snapshot *self = snapshot_of(iface);
	if (!out)
		return E_POINTER;
	*out = self->position < self->copy.count;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE iterator_move_next(void *iface, boolean *out)
{
	struct snapshot *self = snapshot_of(iface);
	if (!out)
		return E_POINTER;
	if (self->position < self->copy.count)
		++self->position;
	*out = self->position < self->copy.count;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE iterator_many(void *iface, UINT32 capacity, __FIKeyValuePair_2_HSTRING_HSTRING **items, UINT32 *count)
{
	struct snapshot *self = snapshot_of(iface);
	if (!count || (capacity && !items))
		return E_POINTER;
	UINT32 taken = 0;
	HRESULT result = S_OK;
	while (taken < capacity && self->position < self->copy.count && SUCCEEDED(result))
		if (SUCCEEDED(result = make_pair(&self->copy.pairs[self->position], &items[taken]))) {
			++taken;
			++self->position;
		}
	if (FAILED(result)) {
		while (taken)
			winrt_put(items[--taken]);
		return result;
	}
	*count = taken;
	return S_OK;
}

static HRESULT copy_map(struct winrt_map *from, struct snapshot **out);

static HRESULT STDMETHODCALLTYPE view_lookup(void *iface, HSTRING key, HSTRING *value)
{
	UINT32 length;
	const WCHAR *text = winrt_text(key, &length);
	struct winrt_map *map = &snapshot_of(iface)->copy;
	if (!value)
		return E_POINTER;
	const INT32 found = find_key(map, text, length);
	return found < 0 ? E_BOUNDS : WindowsDuplicateString(map->pairs[found].value, value);
}

static HRESULT STDMETHODCALLTYPE view_size(void *iface, UINT32 *size)
{
	if (!size)
		return E_POINTER;
	*size = snapshot_of(iface)->copy.count;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE view_has_key(void *iface, HSTRING key, boolean *found)
{
	UINT32 length;
	const WCHAR *text = winrt_text(key, &length);
	if (!found)
		return E_POINTER;
	*found = find_key(&snapshot_of(iface)->copy, text, length) >= 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE view_split(void *iface, __FIMapView_2_HSTRING_HSTRING **first, __FIMapView_2_HSTRING_HSTRING **second)
{
	if (!first || !second)
		return E_POINTER;
	*first = *second = NULL;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE snapshot_first(void *iface, __FIIterator_1___FIKeyValuePair_2_HSTRING_HSTRING **out)
{
	struct snapshot *copy;
	if (!out)
		return E_POINTER;
	HRESULT result = copy_map(&snapshot_of(iface)->copy, &copy);
	if (SUCCEEDED(result))
		*out = (void *)&copy->iterator;
	return result;
}

static const __FIIterator_1___FIKeyValuePair_2_HSTRING_HSTRINGVtbl iterator_table = {WINRT_INSPECTABLE, (void *)iterator_current,
	(void *)iterator_has_current, (void *)iterator_move_next, (void *)iterator_many};
static const __FIMapView_2_HSTRING_HSTRINGVtbl view_table = {WINRT_INSPECTABLE, (void *)view_lookup, (void *)view_size, (void *)view_has_key,
	(void *)view_split};
static const __FIIterable_1___FIKeyValuePair_2_HSTRING_HSTRINGVtbl snapshot_iterable_table = {WINRT_INSPECTABLE, (void *)snapshot_first};
static const struct winrt_entry snapshot_entries[] = {
	WINRT_ENTRY(struct snapshot, view, IID___FIMapView_2_HSTRING_HSTRING),
	WINRT_ENTRY(struct snapshot, iterator, IID___FIIterator_1___FIKeyValuePair_2_HSTRING_HSTRING),
	WINRT_ENTRY(struct snapshot, iterable, IID___FIIterable_1___FIKeyValuePair_2_HSTRING_HSTRING), WINRT_END};
static const struct winrt_class snapshot_class = {L"Windows.Foundation.Collections.IMapView`2<String, String>", snapshot_entries,
	snapshot_destroy};

static HRESULT copy_map(struct winrt_map *from, struct snapshot **out)
{
	struct snapshot *self = winrt_new(&snapshot_class, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->iterator.table = &iterator_table;
	self->view.table = &view_table;
	self->iterable.table = &snapshot_iterable_table;
	winrt_map_init(&self->copy, from->ignore_case);
	HRESULT result = S_OK;
	EnterCriticalSection(&from->lock);
	for (UINT32 index = 0; index < from->count && SUCCEEDED(result); ++index) {
		UINT32 key_length, value_length;
		const WCHAR *key = winrt_text(from->pairs[index].key, &key_length), *value = winrt_text(from->pairs[index].value, &value_length);
		result = map_set_locked(&self->copy, key, key_length, value, value_length, FALSE, NULL);
	}
	LeaveCriticalSection(&from->lock);
	if (FAILED(result)) {
		winrt_release(&self->view);
		return result;
	}
	*out = self;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE map_lookup(void *iface, HSTRING key, HSTRING *value)
{
	struct winrt_map *map = map_of(iface);
	UINT32 length;
	const WCHAR *text = winrt_text(key, &length);
	if (!value)
		return E_POINTER;
	EnterCriticalSection(&map->lock);
	const INT32 found = find_key(map, text, length);
	const HRESULT result = found < 0 ? E_BOUNDS : WindowsDuplicateString(map->pairs[found].value, value);
	LeaveCriticalSection(&map->lock);
	return result;
}

static HRESULT STDMETHODCALLTYPE map_size(void *iface, UINT32 *size)
{
	struct winrt_map *map = map_of(iface);
	if (!size)
		return E_POINTER;
	EnterCriticalSection(&map->lock);
	*size = map->count;
	LeaveCriticalSection(&map->lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE map_has_key(void *iface, HSTRING key, boolean *found)
{
	struct winrt_map *map = map_of(iface);
	UINT32 length;
	const WCHAR *text = winrt_text(key, &length);
	if (!found)
		return E_POINTER;
	EnterCriticalSection(&map->lock);
	*found = find_key(map, text, length) >= 0;
	LeaveCriticalSection(&map->lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE map_view(void *iface, __FIMapView_2_HSTRING_HSTRING **out)
{
	struct snapshot *copy;
	if (!out)
		return E_POINTER;
	HRESULT result = copy_map(map_of(iface), &copy);
	if (SUCCEEDED(result))
		*out = (void *)&copy->view;
	return result;
}

static HRESULT STDMETHODCALLTYPE map_insert(void *iface, HSTRING key, HSTRING value, boolean *replaced)
{
	struct winrt_map *map = map_of(iface);
	UINT32 key_length, value_length;
	const WCHAR *key_text = winrt_text(key, &key_length), *value_text = winrt_text(value, &value_length);
	if (!replaced)
		return E_POINTER;
	if (map->read_only)
		return E_ILLEGAL_METHOD_CALL;
	EnterCriticalSection(&map->lock);
	const HRESULT result = map_set_locked(map, key_text, key_length, value_text, value_length, FALSE, replaced);
	LeaveCriticalSection(&map->lock);
	return result;
}

static HRESULT STDMETHODCALLTYPE map_remove(void *iface, HSTRING key)
{
	struct winrt_map *map = map_of(iface);
	UINT32 length;
	const WCHAR *text = winrt_text(key, &length);
	if (map->read_only)
		return E_ILLEGAL_METHOD_CALL;
	return winrt_map_remove(map, text, length) ? S_OK : E_BOUNDS;
}

static HRESULT STDMETHODCALLTYPE map_clear(void *iface)
{
	struct winrt_map *map = map_of(iface);
	if (map->read_only)
		return E_ILLEGAL_METHOD_CALL;
	EnterCriticalSection(&map->lock);
	while (map->count) {
		--map->count;
		WindowsDeleteString(map->pairs[map->count].key);
		WindowsDeleteString(map->pairs[map->count].value);
	}
	LeaveCriticalSection(&map->lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE map_first(void *iface, __FIIterator_1___FIKeyValuePair_2_HSTRING_HSTRING **out)
{
	struct snapshot *copy;
	if (!out)
		return E_POINTER;
	HRESULT result = copy_map(map_of_iterable(iface), &copy);
	if (SUCCEEDED(result))
		*out = (void *)&copy->iterator;
	return result;
}

const __FIMap_2_HSTRING_HSTRINGVtbl winrt_map_table = {WINRT_INSPECTABLE, (void *)map_lookup, (void *)map_size, (void *)map_has_key,
	(void *)map_view, (void *)map_insert, (void *)map_remove, (void *)map_clear};
const __FIIterable_1___FIKeyValuePair_2_HSTRING_HSTRINGVtbl winrt_map_iterable_table = {WINRT_INSPECTABLE, (void *)map_first};
