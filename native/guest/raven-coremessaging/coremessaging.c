/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven core messaging component: the dispatcher queues of the Windows Runtime (Windows.System.DispatcherQueue and its controller), on
 * which programs post work to run on one thread, and CreateDispatcherQueueController, which gives desktop programs one for the thread
 * that calls it or for a thread of its own.
 *
 * A queue is a message-only window of its thread: work posted to it arrives as a message and runs when the thread's message loop
 * dispatches it, in the order posted, and a timer of the queue is a timer of that window. Shutting a queue down stops it taking work
 * and, for a thread the queue started, ends that thread's loop; the operation it answers has completed by the time it is returned. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include <winstring.h>
#include <activation.h>
#include <asyncinfo.h>
#include <windows.system.h>

#ifndef E_ILLEGAL_METHOD_CALL
#define E_ILLEGAL_METHOD_CALL ((HRESULT)0x8000000EL)
#endif
#ifndef E_ILLEGAL_DELEGATE_ASSIGNMENT
#define E_ILLEGAL_DELEGATE_ASSIGNMENT ((HRESULT)0x80000018L)
#endif

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

typedef __x_ABI_CWindows_CSystem_CIDispatcherQueue queue1;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueue2 queue2;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueueController controller_iface;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueueTimer timer_iface;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueueHandler work_handler;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueueStatics queue_statics;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueueControllerStatics controller_statics;
typedef __x_ABI_CWindows_CFoundation_CIAsyncAction action_iface;
typedef __x_ABI_CWindows_CFoundation_CIAsyncActionCompletedHandler action_handler;
typedef __x_ABI_CWindows_CFoundation_CTimeSpan time_span;
typedef EventRegistrationToken token;

/* The thread kinds and apartments CreateDispatcherQueueController takes, from dispatcherqueue.h. */
enum { DQTYPE_THREAD_DEDICATED = 1, DQTYPE_THREAD_CURRENT = 2 };
enum { DQTAT_COM_NONE = 0, DQTAT_COM_ASTA = 1, DQTAT_COM_STA = 2 };

typedef struct DispatcherQueueOptions {
	DWORD dwSize;
	DWORD threadType;
	DWORD apartmentType;
} DispatcherQueueOptions;

#define WM_QUEUE_WORK (WM_APP + 0x51)

static const WCHAR queue_class[] = L"Windows.System.DispatcherQueue";
static const WCHAR controller_class[] = L"Windows.System.DispatcherQueueController";
static const WCHAR timer_class[] = L"Windows.System.DispatcherQueueTimer";
static const WCHAR window_class[] = L"RavenDispatcherQueue";
static LONG objects, tokens;
static HINSTANCE module;

static HRESULT inspectable_iids(ULONG *count, IID **iids)
{
	if (!count || !iids)
		return E_POINTER;
	*count = 0;
	*iids = NULL;
	return S_OK;
}

static HRESULT inspectable_name(const WCHAR *name, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsCreateString(name, lstrlenW(name), out);
}

static HRESULT inspectable_trust(TrustLevel *level)
{
	if (!level)
		return E_POINTER;
	*level = BaseTrust;
	return S_OK;
}

/* Handlers of an event: kept with the token each was added under. */
struct handlers {
	CRITICAL_SECTION lock;
	struct handler_entry {
		IUnknown *handler;
		INT64 token;
	} *entries;
	UINT count, capacity;
};

static HRESULT handlers_add(struct handlers *list, void *handler, token *out)
{
	if (!handler || !out)
		return E_POINTER;
	EnterCriticalSection(&list->lock);
	if (list->count == list->capacity) {
		UINT capacity = list->capacity ? list->capacity * 2 : 4;
		struct handler_entry *grown = list->entries ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, list->entries, capacity * sizeof *grown) :
			ALLOC(capacity * sizeof *grown);
		if (!grown) {
			LeaveCriticalSection(&list->lock);
			return E_OUTOFMEMORY;
		}
		list->entries = grown;
		list->capacity = capacity;
	}
	IUnknown_AddRef((IUnknown *)handler);
	list->entries[list->count].handler = handler;
	list->entries[list->count].token = out->value = InterlockedIncrement(&tokens);
	list->count++;
	LeaveCriticalSection(&list->lock);
	return S_OK;
}

static HRESULT handlers_remove(struct handlers *list, token value)
{
	IUnknown *removed = NULL;
	EnterCriticalSection(&list->lock);
	for (UINT index = 0; index < list->count; index++)
		if (list->entries[index].token == value.value) {
			removed = list->entries[index].handler;
			list->entries[index] = list->entries[--list->count];
			break;
		}
	LeaveCriticalSection(&list->lock);
	if (removed)
		IUnknown_Release(removed);
	return S_OK;
}

/* A copy of the handlers, each with a reference, so that they run outside the lock and may add or remove handlers themselves. */
static IUnknown **handlers_take(struct handlers *list, UINT *count)
{
	EnterCriticalSection(&list->lock);
	IUnknown **taken = list->count ? ALLOC(list->count * sizeof *taken) : NULL;
	*count = taken ? list->count : 0;
	for (UINT index = 0; index < *count; index++) {
		taken[index] = list->entries[index].handler;
		IUnknown_AddRef(taken[index]);
	}
	LeaveCriticalSection(&list->lock);
	return taken;
}

static void handlers_init(struct handlers *list)
{
	InitializeCriticalSection(&list->lock);
}

static void handlers_free(struct handlers *list)
{
	for (UINT index = 0; index < list->count; index++)
		IUnknown_Release(list->entries[index].handler);
	FREE(list->entries);
	DeleteCriticalSection(&list->lock);
}

/* TypedEventHandler<TSender, TResult>::Invoke, whatever the two types: the table is IUnknown's followed by Invoke. */
typedef struct typed_handler typed_handler;
struct typed_handler_table {
	HRESULT (STDMETHODCALLTYPE *QueryInterface)(typed_handler *, REFIID, void **);
	ULONG (STDMETHODCALLTYPE *AddRef)(typed_handler *);
	ULONG (STDMETHODCALLTYPE *Release)(typed_handler *);
	HRESULT (STDMETHODCALLTYPE *Invoke)(typed_handler *, void *sender, void *args);
};
struct typed_handler {
	const struct typed_handler_table *lpVtbl;
};

static void raise_event(struct handlers *list, void *sender, void *args)
{
	UINT count;
	IUnknown **taken = handlers_take(list, &count);
	for (UINT index = 0; index < count; index++) {
		typed_handler *handler = (typed_handler *)taken[index];
		handler->lpVtbl->Invoke(handler, sender, args);
		IUnknown_Release(taken[index]);
	}
	FREE(taken);
}

/* The queue: IDispatcherQueue and IDispatcherQueue2 on one object. */
struct queue {
	queue1 one;
	queue2 two;
	struct queue *next;
	LONG refs;
	HWND window;
	DWORD thread;
	LONG shut;
	struct handlers starting, completed;
};

#define QUEUE(iface, member) ((struct queue *)((char *)(iface) - offsetof(struct queue, member)))

/* The live queues, so that a thread finds its own: a queue leaves the list under the lock as its last reference goes, so a reference
 * taken under the lock is never one to a queue being freed. */
static CRITICAL_SECTION queues_lock;
static struct queue *queues;

static struct queue *queue_of_thread(DWORD thread)
{
	EnterCriticalSection(&queues_lock);
	struct queue *found = queues;
	while (found && (found->thread != thread || found->shut))
		found = found->next;
	if (found)
		InterlockedIncrement(&found->refs);
	LeaveCriticalSection(&queues_lock);
	return found;
}

/* A timer of a queue: while it runs it holds a reference to itself, which the window's timer of the same identity stands for. */
struct timer {
	timer_iface iface;
	LONG refs;
	struct queue *queue;
	INT64 interval;
	BOOL running, repeating;
	struct handlers tick;
};

#define TIMER(iface) ((struct timer *)((char *)(iface) - offsetof(struct timer, iface)))

static LRESULT CALLBACK queue_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

static HRESULT queue_query(struct queue *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject) ||
		IsEqualIID(iid, &IID___x_ABI_CWindows_CSystem_CIDispatcherQueue))
		*out = &self->one;
	else if (IsEqualIID(iid, &IID___x_ABI_CWindows_CSystem_CIDispatcherQueue2))
		*out = &self->two;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG queue_reference(struct queue *self)
{
	return InterlockedIncrement(&self->refs);
}

static ULONG queue_release(struct queue *self)
{
	EnterCriticalSection(&queues_lock);
	ULONG refs = InterlockedDecrement(&self->refs);
	if (!refs)
		for (struct queue **link = &queues; *link; link = &(*link)->next)
			if (*link == self) {
				*link = self->next;
				break;
			}
	LeaveCriticalSection(&queues_lock);
	if (!refs) {
		// The window belongs to the queue's thread; a thread that has ended took it along.
		if (self->window && GetCurrentThreadId() == self->thread)
			DestroyWindow(self->window);
		else if (self->window)
			PostMessageW(self->window, WM_CLOSE, 0, 0);
		handlers_free(&self->starting);
		handlers_free(&self->completed);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return refs;
}

static HRESULT WINAPI one_query(queue1 *iface, REFIID iid, void **out) { return queue_query(QUEUE(iface, one), iid, out); }
static ULONG WINAPI one_reference(queue1 *iface) { return queue_reference(QUEUE(iface, one)); }
static ULONG WINAPI one_release(queue1 *iface) { return queue_release(QUEUE(iface, one)); }
static HRESULT WINAPI one_iids(queue1 *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI one_name(queue1 *iface, HSTRING *name) { return inspectable_name(queue_class, name); }
static HRESULT WINAPI one_trust(queue1 *iface, TrustLevel *level) { return inspectable_trust(level); }

static HRESULT new_timer(struct queue *queue, timer_iface **out);

static HRESULT WINAPI one_create_timer(queue1 *iface, timer_iface **out)
{
	if (!out)
		return E_POINTER;
	return new_timer(QUEUE(iface, one), out);
}

// The work keeps a reference until it has run; a queue that is shutting down takes no more, and answers FALSE.
static HRESULT enqueue(struct queue *self, work_handler *work, boolean *out)
{
	if (!work || !out)
		return E_POINTER;
	*out = FALSE;
	if (self->shut)
		return S_OK;
	work->lpVtbl->AddRef(work);
	if (!PostMessageW(self->window, WM_QUEUE_WORK, 0, (LPARAM)work)) {
		work->lpVtbl->Release(work);
		return S_OK;
	}
	*out = TRUE;
	return S_OK;
}

static HRESULT WINAPI one_enqueue(queue1 *iface, work_handler *work, boolean *out) { return enqueue(QUEUE(iface, one), work, out); }

// Priorities order the work of one queue; posted in order, all of it runs in the order it came.
static HRESULT WINAPI one_enqueue_priority(queue1 *iface, __x_ABI_CWindows_CSystem_CDispatcherQueuePriority priority, work_handler *work,
	boolean *out)
{
	return enqueue(QUEUE(iface, one), work, out);
}

static HRESULT WINAPI one_add_starting(queue1 *iface, void *handler, token *out) { return handlers_add(&QUEUE(iface, one)->starting, handler, out); }
static HRESULT WINAPI one_remove_starting(queue1 *iface, token value) { return handlers_remove(&QUEUE(iface, one)->starting, value); }
static HRESULT WINAPI one_add_completed(queue1 *iface, void *handler, token *out) { return handlers_add(&QUEUE(iface, one)->completed, handler, out); }
static HRESULT WINAPI one_remove_completed(queue1 *iface, token value) { return handlers_remove(&QUEUE(iface, one)->completed, value); }

static HRESULT WINAPI two_query(queue2 *iface, REFIID iid, void **out) { return queue_query(QUEUE(iface, two), iid, out); }
static ULONG WINAPI two_reference(queue2 *iface) { return queue_reference(QUEUE(iface, two)); }
static ULONG WINAPI two_release(queue2 *iface) { return queue_release(QUEUE(iface, two)); }
static HRESULT WINAPI two_iids(queue2 *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI two_name(queue2 *iface, HSTRING *name) { return inspectable_name(queue_class, name); }
static HRESULT WINAPI two_trust(queue2 *iface, TrustLevel *level) { return inspectable_trust(level); }

static HRESULT WINAPI two_access(queue2 *iface, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = GetCurrentThreadId() == QUEUE(iface, two)->thread;
	return S_OK;
}

static const __x_ABI_CWindows_CSystem_CIDispatcherQueueVtbl one_table = {one_query, one_reference, one_release, one_iids, one_name,
	one_trust, one_create_timer, one_enqueue, one_enqueue_priority, (void *)one_add_starting, one_remove_starting,
	(void *)one_add_completed, one_remove_completed};
static const __x_ABI_CWindows_CSystem_CIDispatcherQueue2Vtbl two_table = {two_query, two_reference, two_release, two_iids, two_name,
	two_trust, two_access};

/* A queue for the calling thread, with its window, in the list GetForCurrentThread looks in. */
static struct queue *new_queue(void)
{
	struct queue *self = ALLOC(sizeof *self);
	if (!self)
		return NULL;
	self->one.lpVtbl = (__x_ABI_CWindows_CSystem_CIDispatcherQueueVtbl *)&one_table;
	self->two.lpVtbl = (__x_ABI_CWindows_CSystem_CIDispatcherQueue2Vtbl *)&two_table;
	self->refs = 1;
	self->thread = GetCurrentThreadId();
	handlers_init(&self->starting);
	handlers_init(&self->completed);
	self->window = CreateWindowExW(0, window_class, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, module, NULL);
	if (!self->window) {
		handlers_free(&self->starting);
		handlers_free(&self->completed);
		FREE(self);
		return NULL;
	}
	EnterCriticalSection(&queues_lock);
	self->next = queues;
	queues = self;
	LeaveCriticalSection(&queues_lock);
	InterlockedIncrement(&objects);
	return self;
}

/* The timer. */

static HRESULT WINAPI timer_query(timer_iface *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject) ||
		IsEqualIID(iid, &IID___x_ABI_CWindows_CSystem_CIDispatcherQueueTimer)) {
		*out = iface;
		InterlockedIncrement(&TIMER(iface)->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI timer_reference(timer_iface *iface) { return InterlockedIncrement(&TIMER(iface)->refs); }

static ULONG WINAPI timer_release(timer_iface *iface)
{
	struct timer *self = TIMER(iface);
	ULONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		handlers_free(&self->tick);
		queue_release(self->queue);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return refs;
}

static HRESULT WINAPI timer_iids(timer_iface *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI timer_name(timer_iface *iface, HSTRING *name) { return inspectable_name(timer_class, name); }
static HRESULT WINAPI timer_trust(timer_iface *iface, TrustLevel *level) { return inspectable_trust(level); }

static HRESULT WINAPI timer_get_interval(timer_iface *iface, time_span *out)
{
	if (!out)
		return E_POINTER;
	out->Duration = TIMER(iface)->interval;
	return S_OK;
}

// The interval is in units of 100 ns; the window's timer counts milliseconds, and a running timer takes the new interval at once.
static UINT interval_ms(const struct timer *self)
{
	INT64 ms = self->interval / 10000;
	return ms < USER_TIMER_MINIMUM ? USER_TIMER_MINIMUM : ms > USER_TIMER_MAXIMUM ? USER_TIMER_MAXIMUM : (UINT)ms;
}

static HRESULT WINAPI timer_put_interval(timer_iface *iface, time_span value)
{
	struct timer *self = TIMER(iface);
	if (value.Duration < 0)
		return E_INVALIDARG;
	self->interval = value.Duration;
	if (self->running)
		SetTimer(self->queue->window, (UINT_PTR)self, interval_ms(self), NULL);
	return S_OK;
}

static HRESULT WINAPI timer_get_running(timer_iface *iface, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = TIMER(iface)->running;
	return S_OK;
}

static HRESULT WINAPI timer_get_repeating(timer_iface *iface, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = TIMER(iface)->repeating;
	return S_OK;
}

static HRESULT WINAPI timer_put_repeating(timer_iface *iface, boolean value)
{
	TIMER(iface)->repeating = value != 0;
	return S_OK;
}

static HRESULT WINAPI timer_start(timer_iface *iface)
{
	struct timer *self = TIMER(iface);
	if (!SetTimer(self->queue->window, (UINT_PTR)self, interval_ms(self), NULL))
		return HRESULT_FROM_WIN32(GetLastError());
	if (!self->running) {
		self->running = TRUE;
		InterlockedIncrement(&self->refs);
	}
	return S_OK;
}

static HRESULT WINAPI timer_stop(timer_iface *iface)
{
	struct timer *self = TIMER(iface);
	if (!self->running)
		return S_OK;
	KillTimer(self->queue->window, (UINT_PTR)self);
	self->running = FALSE;
	timer_release(iface);
	return S_OK;
}

static HRESULT WINAPI timer_add_tick(timer_iface *iface, void *handler, token *out) { return handlers_add(&TIMER(iface)->tick, handler, out); }
static HRESULT WINAPI timer_remove_tick(timer_iface *iface, token value) { return handlers_remove(&TIMER(iface)->tick, value); }

static const __x_ABI_CWindows_CSystem_CIDispatcherQueueTimerVtbl timer_table = {timer_query, timer_reference, timer_release, timer_iids,
	timer_name, timer_trust, timer_get_interval, timer_put_interval, timer_get_running, timer_get_repeating, timer_put_repeating,
	timer_start, timer_stop, (void *)timer_add_tick, timer_remove_tick};

static HRESULT new_timer(struct queue *queue, timer_iface **out)
{
	struct timer *self = ALLOC(sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = (__x_ABI_CWindows_CSystem_CIDispatcherQueueTimerVtbl *)&timer_table;
	self->refs = 1;
	self->queue = queue;
	self->repeating = TRUE;
	queue_reference(queue);
	handlers_init(&self->tick);
	InterlockedIncrement(&objects);
	*out = &self->iface;
	return S_OK;
}

// A tick: a timer that does not repeat stops first, so that a handler may start it again.
static void tick(struct timer *self)
{
	self->iface.lpVtbl->AddRef(&self->iface);
	if (!self->repeating)
		timer_stop(&self->iface);
	raise_event(&self->tick, &self->iface, NULL);
	self->iface.lpVtbl->Release(&self->iface);
}

static LRESULT CALLBACK queue_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_QUEUE_WORK) {
		work_handler *work = (work_handler *)lparam;
		work->lpVtbl->Invoke(work);
		work->lpVtbl->Release(work);
		return 0;
	}
	if (message == WM_TIMER && wparam) {
		tick((struct timer *)wparam);
		return 0;
	}
	if (message == WM_CLOSE) {
		DestroyWindow(window);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

/* The operation ShutdownQueueAsync answers, IAsyncAction with IAsyncInfo: complete from the start. */
struct action {
	action_iface action;
	IAsyncInfo info;
	LONG refs;
	action_handler *handler;
	BOOL closed;
};

#define ACTION(iface, member) ((struct action *)((char *)(iface) - offsetof(struct action, member)))

static HRESULT action_query(struct action *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject) ||
		IsEqualIID(iid, &IID___x_ABI_CWindows_CFoundation_CIAsyncAction))
		*out = &self->action;
	else if (IsEqualIID(iid, &IID_IAsyncInfo))
		*out = &self->info;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG action_release(struct action *self)
{
	ULONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		if (self->handler)
			self->handler->lpVtbl->Release(self->handler);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return refs;
}

static HRESULT WINAPI action_query_one(action_iface *iface, REFIID iid, void **out) { return action_query(ACTION(iface, action), iid, out); }
static ULONG WINAPI action_reference(action_iface *iface) { return InterlockedIncrement(&ACTION(iface, action)->refs); }
static ULONG WINAPI action_release_one(action_iface *iface) { return action_release(ACTION(iface, action)); }
static HRESULT WINAPI action_iids(action_iface *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI action_name(action_iface *iface, HSTRING *name)
{
	return inspectable_name(L"Windows.Foundation.IAsyncAction", name);
}
static HRESULT WINAPI action_trust(action_iface *iface, TrustLevel *level) { return inspectable_trust(level); }

// The handler may be set once; the operation has already completed, so it runs at once on the thread that sets it.
static HRESULT WINAPI action_put_completed(action_iface *iface, action_handler *handler)
{
	struct action *self = ACTION(iface, action);
	if (self->closed)
		return E_ILLEGAL_METHOD_CALL;
	if (self->handler)
		return E_ILLEGAL_DELEGATE_ASSIGNMENT;
	if (!handler)
		return S_OK;
	handler->lpVtbl->AddRef(handler);
	self->handler = handler;
	return handler->lpVtbl->Invoke(handler, iface, Completed);
}

static HRESULT WINAPI action_get_completed(action_iface *iface, action_handler **out)
{
	struct action *self = ACTION(iface, action);
	if (!out)
		return E_POINTER;
	*out = self->handler;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI action_results(action_iface *iface) { return ACTION(iface, action)->closed ? E_ILLEGAL_METHOD_CALL : S_OK; }

static const __x_ABI_CWindows_CFoundation_CIAsyncActionVtbl action_table = {action_query_one, action_reference, action_release_one,
	action_iids, action_name, action_trust, action_put_completed, action_get_completed, action_results};

static HRESULT WINAPI info_query(IAsyncInfo *iface, REFIID iid, void **out) { return action_query(ACTION(iface, info), iid, out); }
static ULONG WINAPI info_reference(IAsyncInfo *iface) { return InterlockedIncrement(&ACTION(iface, info)->refs); }
static ULONG WINAPI info_release(IAsyncInfo *iface) { return action_release(ACTION(iface, info)); }
static HRESULT WINAPI info_iids(IAsyncInfo *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI info_name(IAsyncInfo *iface, HSTRING *name) { return inspectable_name(L"Windows.Foundation.IAsyncAction", name); }
static HRESULT WINAPI info_trust(IAsyncInfo *iface, TrustLevel *level) { return inspectable_trust(level); }

static HRESULT WINAPI info_id(IAsyncInfo *iface, unsigned int *out)
{
	if (!out)
		return E_POINTER;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI info_status(IAsyncInfo *iface, AsyncStatus *out)
{
	if (!out)
		return E_POINTER;
	*out = Completed;
	return S_OK;
}

static HRESULT WINAPI info_error(IAsyncInfo *iface, HRESULT *out)
{
	if (!out)
		return E_POINTER;
	*out = S_OK;
	return S_OK;
}

static HRESULT WINAPI info_cancel(IAsyncInfo *iface) { return S_OK; }

static HRESULT WINAPI info_close(IAsyncInfo *iface)
{
	ACTION(iface, info)->closed = TRUE;
	return S_OK;
}

static const IAsyncInfoVtbl info_table = {info_query, info_reference, info_release, info_iids, info_name, info_trust, info_id, info_status,
	info_error, info_cancel, info_close};

static HRESULT completed_action(action_iface **out)
{
	struct action *self = ALLOC(sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->action.lpVtbl = (__x_ABI_CWindows_CFoundation_CIAsyncActionVtbl *)&action_table;
	self->info.lpVtbl = (IAsyncInfoVtbl *)&info_table;
	self->refs = 1;
	InterlockedIncrement(&objects);
	*out = &self->action;
	return S_OK;
}

/* The controller: the queue it made, and the thread it started for it when it is a dedicated one. */
struct controller {
	controller_iface iface;
	LONG refs;
	struct queue *queue;
	HANDLE thread;
};

#define CONTROLLER(iface) ((struct controller *)((char *)(iface) - offsetof(struct controller, iface)))

static HRESULT WINAPI controller_query(controller_iface *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject) ||
		IsEqualIID(iid, &IID___x_ABI_CWindows_CSystem_CIDispatcherQueueController)) {
		*out = iface;
		InterlockedIncrement(&CONTROLLER(iface)->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI controller_reference(controller_iface *iface) { return InterlockedIncrement(&CONTROLLER(iface)->refs); }

static ULONG WINAPI controller_release(controller_iface *iface)
{
	struct controller *self = CONTROLLER(iface);
	ULONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		queue_release(self->queue);
		if (self->thread)
			CloseHandle(self->thread);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return refs;
}

static HRESULT WINAPI controller_iids(controller_iface *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI controller_name(controller_iface *iface, HSTRING *name) { return inspectable_name(controller_class, name); }
static HRESULT WINAPI controller_trust(controller_iface *iface, TrustLevel *level) { return inspectable_trust(level); }

static HRESULT WINAPI controller_queue(controller_iface *iface, queue1 **out)
{
	if (!out)
		return E_POINTER;
	*out = &CONTROLLER(iface)->queue->one;
	queue_reference(CONTROLLER(iface)->queue);
	return S_OK;
}

// The queue stops taking work, its handlers hear that it is shutting down and then that it has, and a thread of its own leaves its loop
// once the work already posted has run.
static HRESULT WINAPI controller_shutdown(controller_iface *iface, action_iface **out)
{
	struct controller *self = CONTROLLER(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!InterlockedExchange(&self->queue->shut, 1)) {
		raise_event(&self->queue->starting, &self->queue->one, NULL);
		raise_event(&self->queue->completed, &self->queue->one, NULL);
		if (self->thread)
			PostThreadMessageW(self->queue->thread, WM_QUIT, 0, 0);
	}
	return completed_action(out);
}

static const __x_ABI_CWindows_CSystem_CIDispatcherQueueControllerVtbl controller_table = {controller_query, controller_reference,
	controller_release, controller_iids, controller_name, controller_trust, controller_queue, controller_shutdown};

/* A dedicated thread: it makes its queue, hands it over, and runs its message loop until the queue is shut down. */
struct start {
	HANDLE ready;
	struct queue *queue;
	DWORD apartment;
};

static DWORD WINAPI dedicated_thread(void *context)
{
	struct start *start = context;
	HRESULT (WINAPI *initialize)(void *, DWORD) = NULL;
	HMODULE ole = start->apartment != DQTAT_COM_NONE ? LoadLibraryW(L"ole32.dll") : NULL;
	if (ole && (initialize = (void *)GetProcAddress(ole, "CoInitializeEx")))
		initialize(NULL, COINIT_APARTMENTTHREADED);
	struct queue *queue = new_queue();
	if (queue)
		queue_reference(queue);
	start->queue = queue;
	SetEvent(start->ready);
	if (!queue)
		return 1;
	MSG message;
	while (GetMessageW(&message, NULL, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	DestroyWindow(queue->window);
	queue->window = NULL;
	queue_release(queue);
	return 0;
}

static HRESULT new_controller(DWORD type, DWORD apartment, controller_iface **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct controller *self = ALLOC(sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = (__x_ABI_CWindows_CSystem_CIDispatcherQueueControllerVtbl *)&controller_table;
	self->refs = 1;
	if (type == DQTYPE_THREAD_CURRENT) {
		// A thread has one queue.
		struct queue *existing = queue_of_thread(GetCurrentThreadId());
		if (existing) {
			queue_release(existing);
			FREE(self);
			return RPC_E_WRONG_THREAD;
		}
		self->queue = new_queue();
	} else {
		struct start start = {CreateEventW(NULL, TRUE, FALSE, NULL), NULL, apartment};
		if (start.ready && (self->thread = CreateThread(NULL, 0, dedicated_thread, &start, 0, NULL)))
			WaitForSingleObject(start.ready, INFINITE);
		if (start.ready)
			CloseHandle(start.ready);
		self->queue = start.queue;
	}
	if (!self->queue) {
		if (self->thread)
			CloseHandle(self->thread);
		FREE(self);
		return E_OUTOFMEMORY;
	}
	InterlockedIncrement(&objects);
	*out = &self->iface;
	return S_OK;
}

/* The options come by value, as the declaration in dispatcherqueue.h has them. */
__declspec(dllexport) HRESULT WINAPI CreateDispatcherQueueController(DispatcherQueueOptions options, controller_iface **out)
{
	if (options.dwSize != sizeof options || (options.threadType != DQTYPE_THREAD_DEDICATED && options.threadType != DQTYPE_THREAD_CURRENT) ||
		options.apartmentType > DQTAT_COM_STA)
		return E_INVALIDARG;
	return new_controller(options.threadType, options.apartmentType, out);
}

/* The activation factories: DispatcherQueue's statics give the calling thread's queue, the controller's make one on a thread of its own. */
struct factory {
	IActivationFactory iface;
	const WCHAR *name;
	const IID *statics_iid;
	const void *statics;
};

static HRESULT WINAPI factory_query(IActivationFactory *iface, REFIID iid, void **out);
static ULONG WINAPI factory_reference(IActivationFactory *iface) { return 2; }
static ULONG WINAPI factory_release(IActivationFactory *iface) { return 1; }
static HRESULT WINAPI factory_iids(IActivationFactory *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI factory_name(IActivationFactory *iface, HSTRING *name) { return inspectable_name(((struct factory *)iface)->name, name); }
static HRESULT WINAPI factory_trust(IActivationFactory *iface, TrustLevel *level) { return inspectable_trust(level); }
static HRESULT WINAPI factory_activate(IActivationFactory *iface, IInspectable **out) { return E_NOTIMPL; }

static IActivationFactoryVtbl factory_table = {factory_query, factory_reference, factory_release, factory_iids, factory_name, factory_trust,
	factory_activate};

static HRESULT WINAPI statics_query(void *iface, REFIID iid, void **out);
static ULONG WINAPI statics_reference(void *iface) { return 2; }
static ULONG WINAPI statics_release(void *iface) { return 1; }
static HRESULT WINAPI statics_iids(void *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI statics_trust(void *iface, TrustLevel *level) { return inspectable_trust(level); }

static HRESULT WINAPI queue_statics_name(queue_statics *iface, HSTRING *name) { return inspectable_name(queue_class, name); }

// The queue of the calling thread, or none: a thread without a queue answers NULL with success.
static HRESULT WINAPI queue_statics_current(queue_statics *iface, queue1 **out)
{
	if (!out)
		return E_POINTER;
	struct queue *queue = queue_of_thread(GetCurrentThreadId());
	*out = queue ? &queue->one : NULL;
	return S_OK;
}

static HRESULT WINAPI controller_statics_name(controller_statics *iface, HSTRING *name) { return inspectable_name(controller_class, name); }

static HRESULT WINAPI controller_statics_create(controller_statics *iface, controller_iface **out)
{
	return new_controller(DQTYPE_THREAD_DEDICATED, DQTAT_COM_NONE, out);
}

static const __x_ABI_CWindows_CSystem_CIDispatcherQueueStaticsVtbl queue_statics_table = {(void *)statics_query, (void *)statics_reference,
	(void *)statics_release, (void *)statics_iids, queue_statics_name, (void *)statics_trust, queue_statics_current};
static const __x_ABI_CWindows_CSystem_CIDispatcherQueueControllerStaticsVtbl controller_statics_table = {(void *)statics_query,
	(void *)statics_reference, (void *)statics_release, (void *)statics_iids, controller_statics_name, (void *)statics_trust,
	controller_statics_create};
static queue_statics queue_statics_object = {(__x_ABI_CWindows_CSystem_CIDispatcherQueueStaticsVtbl *)&queue_statics_table};
static controller_statics controller_statics_object = {(__x_ABI_CWindows_CSystem_CIDispatcherQueueControllerStaticsVtbl *)&controller_statics_table};

static struct factory factories[] = {
	{{&factory_table}, queue_class, &IID___x_ABI_CWindows_CSystem_CIDispatcherQueueStatics, &queue_statics_object},
	{{&factory_table}, controller_class, &IID___x_ABI_CWindows_CSystem_CIDispatcherQueueControllerStatics, &controller_statics_object},
};

static HRESULT WINAPI factory_query(IActivationFactory *iface, REFIID iid, void **out)
{
	struct factory *self = (struct factory *)iface;
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject) ||
		IsEqualIID(iid, &IID_IActivationFactory)) {
		*out = iface;
		return S_OK;
	}
	if (IsEqualIID(iid, self->statics_iid)) {
		*out = (void *)self->statics;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

// The statics answer for their factory: the factory whose statics object this is.
static HRESULT WINAPI statics_query(void *iface, REFIID iid, void **out)
{
	for (UINT index = 0; index < ARRAYSIZE(factories); index++)
		if (factories[index].statics == iface)
			return factory_query(&factories[index].iface, iid, out);
	return E_NOINTERFACE;
}

__declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
	UINT32 length = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
	for (UINT index = 0; index < ARRAYSIZE(factories); index++)
		if (length == (UINT32)lstrlenW(factories[index].name) && CompareStringOrdinal(text, (int)length, factories[index].name, -1, FALSE) == CSTR_EQUAL) {
			*out = &factories[index].iface;
			return S_OK;
		}
	return CLASS_E_CLASSNOTAVAILABLE;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		module = instance;
		DisableThreadLibraryCalls(instance);
		InitializeCriticalSection(&queues_lock);
		WNDCLASSW kind = {0};
		kind.lpfnWndProc = queue_window;
		kind.hInstance = instance;
		kind.lpszClassName = window_class;
		RegisterClassW(&kind);
	} else if (reason == DLL_PROCESS_DETACH && !reserved) {
		UnregisterClassW(window_class, instance);
		DeleteCriticalSection(&queues_lock);
	}
	return TRUE;
}
