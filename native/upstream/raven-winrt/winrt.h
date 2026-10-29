/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The plumbing of Raven's Windows Runtime components, shared so that each class only holds its own behaviour: objects that answer
 * several interfaces with one reference count, the IInspectable methods every interface has, asynchronous operations, boxed values,
 * and maps of strings with their iterators.
 *
 * An object is a struct that starts with struct winrt_object and holds one struct winrt_interface per interface it implements; its
 * class lists which interface identifier answers with which of them. Interface tables start with WINRT_INSPECTABLE, so QueryInterface,
 * AddRef, Release and the IInspectable methods are the same functions for every interface of every object. All objects are agile (any
 * thread may call them), as the Windows classes these components stand for are. */
#ifndef RAVEN_WINRT_H
#define RAVEN_WINRT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winstring.h>
#include <inspectable.h>
#include <activation.h>
#include <asyncinfo.h>
#include <windows.foundation.h>
#include <windows.foundation.collections.h>
#include <windows.storage.streams.h>
#include <robuffer.h>
#include <objbase.h>
#include <stddef.h>

/* The Windows Runtime's errors, which the MinGW headers lack. */
#ifndef E_ILLEGAL_STATE_CHANGE
#define E_ILLEGAL_STATE_CHANGE ((HRESULT)0x8000000DL)
#endif
#ifndef E_ILLEGAL_METHOD_CALL
#define E_ILLEGAL_METHOD_CALL ((HRESULT)0x8000000EL)
#endif
#ifndef E_ILLEGAL_DELEGATE_ASSIGNMENT
#define E_ILLEGAL_DELEGATE_ASSIGNMENT ((HRESULT)0x80000018L)
#endif

#define WINRT_ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define WINRT_REALLOC(block, size) HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (block), (size))
#define WINRT_FREE(block) HeapFree(GetProcessHeap(), 0, (block))

struct winrt_object;

/* One interface of an object: the pointer handed out points at this. */
struct winrt_interface {
	const void *table;
	struct winrt_object *object;
};

/* WINRT_OWN (and WINRT_OWN_2, WINRT_OWN_3) in a class entry stand for the interface identifiers the object was made with
 * (winrt_object.own): the instances of a generic interface, an asynchronous operation of one result type or a boxed value of one type,
 * share one class. */
extern const IID WINRT_OWN, WINRT_OWN_2, WINRT_OWN_3;

struct winrt_entry {
	const IID *iid;
	size_t offset;
};

struct winrt_class {
	const WCHAR *name;
	/* Ends with an entry whose iid is NULL. The first entry answers IUnknown, IInspectable and IAgileObject. */
	const struct winrt_entry *entries;
	void (*destroy)(struct winrt_object *object);
};

struct winrt_object {
	const struct winrt_class *kind;
	LONG refs;
	const IID *own[3];
};

#define WINRT_ENTRY(type, field, iid) {&(iid), offsetof(type, field)}
#define WINRT_END {NULL, 0}

HRESULT STDMETHODCALLTYPE winrt_query(void *iface, REFIID iid, void **out);
ULONG STDMETHODCALLTYPE winrt_add_ref(void *iface);
ULONG STDMETHODCALLTYPE winrt_release(void *iface);
HRESULT STDMETHODCALLTYPE winrt_iids(void *iface, ULONG *count, IID **iids);
HRESULT STDMETHODCALLTYPE winrt_class_name(void *iface, HSTRING *name);
HRESULT STDMETHODCALLTYPE winrt_trust(void *iface, TrustLevel *level);
#define WINRT_INSPECTABLE (void *)winrt_query, (void *)winrt_add_ref, (void *)winrt_release, (void *)winrt_iids, \
	(void *)winrt_class_name, (void *)winrt_trust

/* A new object of the class, zeroed, with one reference: its interfaces are wired to it. NULL when out of memory. */
void *winrt_new(const struct winrt_class *kind, size_t size, const IID *own);
/* The same, for a class with up to three entries of its own identifiers. */
void *winrt_new_own(const struct winrt_class *kind, size_t size, const IID *own, const IID *own_2, const IID *own_3);
/* Whether any object of the component is alive, for DllCanUnloadNow. */
BOOL winrt_in_use(void);
/* The object an interface pointer of it belongs to. */
static inline struct winrt_object *winrt_object_of(void *iface) { return ((struct winrt_interface *)iface)->object; }
/* The interface of the object answering iid, with a reference added, or NULL. */
void *winrt_get(struct winrt_object *object, REFIID iid);
/* Releases an interface pointer that may be NULL. */
static inline void winrt_put(void *iface)
{
	if (iface)
		((IUnknown *)iface)->lpVtbl->Release((IUnknown *)iface);
}

/* Strings. */
HRESULT winrt_string(const WCHAR *text, HSTRING *out);
HRESULT winrt_string_n(const WCHAR *text, UINT32 length, HSTRING *out);
const WCHAR *winrt_text(HSTRING string, UINT32 *length);
/* Equal ignoring ASCII case, as protocol names and header names compare. */
BOOL winrt_equal_ascii(const WCHAR *left, UINT32 left_length, const WCHAR *right, UINT32 right_length);
/* The decimal digits of number into text (room for 20), answering their count. */
UINT32 winrt_decimal(UINT64 number, WCHAR *text);
/* UTF-16 to UTF-8 and back, in a block of the process heap the caller frees; NULL when out of memory. */
char *winrt_utf8(const WCHAR *text, UINT32 length, UINT32 *bytes);
WCHAR *winrt_utf16(const char *text, UINT32 bytes, UINT32 *length);

/* Asynchronous operations: IAsyncOperation<T>, IAsyncOperationWithProgress<T, P>, with IAsyncInfo. The work runs on a thread of its
 * own and gives the result; the completion handler runs once, on that thread, or at once on the thread that sets it when the operation
 * has already ended, as the Windows Runtime documents. The progress handler is kept and given back; no progress is reported. */
enum winrt_result {
	WINRT_RESULT_OBJECT, /* an interface pointer, which the operation owns */
	WINRT_RESULT_STRING, /* an HSTRING, which the operation owns */
	WINRT_RESULT_UINT64,
	WINRT_RESULT_UINT32,
	WINRT_RESULT_BOOLEAN
};

union winrt_value {
	void *object;
	HSTRING string;
	UINT64 uint64;
	UINT32 uint32;
	boolean boolean;
};

struct winrt_async;
/* Runs the operation: fills the result, or answers the failure the operation ends with. */
typedef HRESULT (*winrt_work)(struct winrt_async *operation, void *context, union winrt_value *result);

/* Starts an operation of the interface operation_iid (whose result is of the given kind; with_progress for an IAsyncOperationWithProgress,
 * otherwise an IAsyncOperation), and answers it with a reference in *out. free_context, when given, frees the context once the work has
 * run or the operation could not start. */
HRESULT winrt_async_start(const IID *operation_iid, BOOL with_progress, enum winrt_result kind, winrt_work work, void *context,
	void (*free_context)(void *), void **out);
/* Waits for any operation, ours or another component's, and takes its results into *result (as its GetResults gives them): for work
 * that runs one operation after another on its own thread. */
HRESULT winrt_wait(void *operation, BOOL with_progress, void *result);
/* True once Cancel was called: work that checks it may end early with E_ABORT. */
BOOL winrt_async_canceled(struct winrt_async *operation);

/* Objects in a list, kept in a snapshot: IVectorView<T> with IIterable<T> and the IIterator<T> they hand out, for any interface type T.
 * The snapshot holds a reference to each item; *out is the view, or with iterator the iterator at the first item. */
struct winrt_items_kind {
	const IID *view, *iterable, *iterator;
};
HRESULT winrt_items(const struct winrt_items_kind *kind, IInspectable **items, UINT32 count, BOOL iterator, void **out);

/* Boxed values, IReference<T> for a value type T of the given size: UINT64, DateTime, TimeSpan. */
HRESULT winrt_box(const IID *reference_iid, const void *value, size_t size, void **out);

/* A buffer, IBuffer with IBufferByteAccess, holding a copy of the bytes (or capacity zeroed bytes with length 0 when bytes is NULL). */
HRESULT winrt_buffer(const BYTE *bytes, UINT32 length, UINT32 capacity, __x_ABI_CWindows_CStorage_CStreams_CIBuffer **out);
/* The bytes of any IBuffer, through IBufferByteAccess. */
HRESULT winrt_buffer_bytes(__x_ABI_CWindows_CStorage_CStreams_CIBuffer *buffer, BYTE **bytes, UINT32 *length);

/* A map of strings, IMap<String, String> with its view and IIterable<IKeyValuePair<String, String>>, kept in insertion order, to embed
 * in an object; keys compare ignoring ASCII case when asked to, as header names do. The object's class lists its interfaces with
 * WINRT_MAP_ENTRIES. Methods are safe to call from several threads. */
struct winrt_map {
	struct winrt_interface map;
	struct winrt_interface iterable;
	CRITICAL_SECTION lock;
	BOOL ignore_case, read_only;
	UINT32 count, capacity;
	struct winrt_pair {
		HSTRING key, value;
	} *pairs;
};

#define WINRT_MAP_ENTRIES(type, field) \
	WINRT_ENTRY(type, field.map, IID___FIMap_2_HSTRING_HSTRING), \
	WINRT_ENTRY(type, field.iterable, IID___FIIterable_1___FIKeyValuePair_2_HSTRING_HSTRING)

extern const __FIMap_2_HSTRING_HSTRINGVtbl winrt_map_table;
extern const __FIIterable_1___FIKeyValuePair_2_HSTRING_HSTRINGVtbl winrt_map_iterable_table;
void winrt_map_init(struct winrt_map *map, BOOL ignore_case);
void winrt_map_free(struct winrt_map *map);
/* Sets key to value, or with append adds value after the one the key has, separated by a comma and a space (RFC 9110, 5.3). */
HRESULT winrt_map_set(struct winrt_map *map, const WCHAR *key, UINT32 key_length, const WCHAR *value, UINT32 value_length, BOOL append);
/* The value of key as a new string, or E_BOUNDS when it has none. */
HRESULT winrt_map_get(struct winrt_map *map, const WCHAR *key, HSTRING *value);
BOOL winrt_map_remove(struct winrt_map *map, const WCHAR *key, UINT32 key_length);
/* Calls visit for each pair, in order, under the map's lock; stops at the first failure and answers it. */
HRESULT winrt_map_each(struct winrt_map *map, HRESULT (*visit)(void *context, HSTRING key, HSTRING value), void *context);

#endif
