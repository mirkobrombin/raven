/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.System.Threading: ThreadPool and ThreadPoolTimer.
 *
 * - ThreadPool.RunAsync runs a work item on a thread of its own and gives the IAsyncAction it completes, which the handler is passed;
 *   a canceled action still runs its handler, which can look at the action's status. The priority sets the thread's: below normal,
 *   normal or above normal. TimeSliced is taken as a hint and changes nothing.
 * - ThreadPoolTimer runs its elapsed handler on the system thread pool after the delay, or every period, until canceled. A timer is
 *   kept alive while it is set, as an app need not hold it. Its destroyed handler runs once, when a single-use timer has run or when a
 *   timer is canceled and its callbacks have ended. */
#include "object.h"
#include "../raven-winrt/winrt.h"

static const IID iid_pool_statics = {0xb6bf67dd, 0x84bd, 0x44f8, {0xac, 0x1c, 0x93, 0xeb, 0xcb, 0x9d, 0xba, 0x91}};
static const IID iid_timer = {0x594ebe78, 0x55ea, 0x4a88, {0xa5, 0x0d, 0x34, 0x02, 0xae, 0x1f, 0x9c, 0xf2}};
static const IID iid_timer_statics = {0x1a8a9d02, 0xe482, 0x461b, {0xb8, 0xc7, 0x8e, 0xfa, 0xd1, 0xcc, 0xe5, 0x90}};

/* WorkItemPriority. */
enum { LOW = -1, NORMAL = 0, HIGH = 1 };

typedef HRESULT (WINAPI *invoke_method)(void *handler, void *argument);

static HRESULT invoke(IUnknown *handler, void *argument) { return ((invoke_method)method(handler, 3))(handler, argument); }

/* ---- ThreadPool ---- */

struct work {
	IUnknown *handler;
	INT32 priority;
};

static void free_work(void *context)
{
	struct work *work = context;
	release(work->handler);
	FREE(work);
}

static HRESULT run_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct work *work = context;
	SetThreadPriority(GetCurrentThread(), work->priority == LOW ? THREAD_PRIORITY_BELOW_NORMAL :
		work->priority == HIGH ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL);
	return invoke(work->handler, winrt_async_interface(operation));
}

static HRESULT WINAPI run_with_options(void *iface, void *handler, INT32 priority, UINT32 options, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!handler)
		return E_INVALIDARG;
	if (priority < LOW || priority > HIGH)
		return E_INVALIDARG;
	struct work *work = ALLOC(sizeof *work);
	if (!work)
		return E_OUTOFMEMORY;
	reference(work->handler = handler);
	work->priority = priority;
	return winrt_action_start(run_work, work, free_work, out);
}

static HRESULT WINAPI run(void *iface, void *handler, void **out) { return run_with_options(iface, handler, NORMAL, 0, out); }
static HRESULT WINAPI run_with_priority(void *iface, void *handler, INT32 priority, void **out)
{
	return run_with_options(iface, handler, priority, 0, out);
}

TABLE(pool_factory_table, 0, FACTORY);
TABLE(pool_statics_table, 1, INSPECTABLE, (const void *)run, (const void *)run_with_priority, (const void *)run_with_options);
static const struct face_iid pool_iids[] = {{&iid_factory, 0}, {&iid_pool_statics, 1}, {NULL, 0}};
static const struct kind pool_kind = {"ThreadPool", L"Windows.System.Threading.ThreadPool", pool_iids, NULL};
struct object thread_pool_class = {{pool_factory_table.slots, pool_statics_table.slots}, &pool_kind, 1};

/* ---- ThreadPoolTimer ---- */

struct timer {
	struct object base;
	PTP_TIMER timer;
	IUnknown *elapsed, *destroyed;
	/* In 100-nanosecond units, as TimeSpan: one of them is zero. */
	INT64 period, delay;
	LONG canceled, finished;
};

static struct timer *timer_of(void *iface) { return (struct timer *)owner(iface); }

static void destroy_timer(struct object *base)
{
	struct timer *self = (struct timer *)base;
	if (self->timer)
		CloseThreadpoolTimer(self->timer);
	if (self->elapsed)
		release(self->elapsed);
	if (self->destroyed)
		release(self->destroyed);
}

/* Once: the destroyed handler runs, and the reference that kept the timer while set is dropped. */
static void finish(struct timer *self)
{
	if (InterlockedExchange(&self->finished, 1))
		return;
	if (self->destroyed)
		invoke(self->destroyed, face_of(&self->base, 0));
	release(face_of(&self->base, 0));
}

static void CALLBACK elapsed(PTP_CALLBACK_INSTANCE instance, void *context, PTP_TIMER timer)
{
	struct timer *self = context;
	if (InterlockedCompareExchange(&self->canceled, 0, 0))
		return;
	invoke(self->elapsed, face_of(&self->base, 0));
	if (!self->period)
		finish(self);
}

/* Waits on a thread of its own, as Cancel may come from the elapsed handler itself, for the callbacks to end. */
static DWORD WINAPI wait_callbacks(void *context)
{
	struct timer *self = context;
	WaitForThreadpoolTimerCallbacks(self->timer, TRUE);
	finish(self);
	release(face_of(&self->base, 0));
	return 0;
}

static HRESULT WINAPI timer_period(void *iface, INT64 *out)
{
	if (!out)
		return E_POINTER;
	*out = timer_of(iface)->period;
	return S_OK;
}

static HRESULT WINAPI timer_delay(void *iface, INT64 *out)
{
	if (!out)
		return E_POINTER;
	*out = timer_of(iface)->delay;
	return S_OK;
}

static HRESULT WINAPI timer_cancel(void *iface)
{
	struct timer *self = timer_of(iface);
	if (InterlockedExchange(&self->canceled, 1) || InterlockedCompareExchange(&self->finished, 0, 0))
		return S_OK;
	SetThreadpoolTimer(self->timer, NULL, 0, 0);
	/* The waiting thread holds the timer, which the app may let go as soon as Cancel returns. */
	reference(iface);
	HANDLE thread = CreateThread(NULL, 0, wait_callbacks, self, 0, NULL);
	if (thread)
		CloseHandle(thread);
	else {
		finish(self);
		release(iface);
	}
	return S_OK;
}

TABLE(timer_table, 0, INSPECTABLE, (const void *)timer_period, (const void *)timer_delay, (const void *)timer_cancel);
static const struct face_iid timer_iids[] = {{&iid_timer, 0}, {NULL, 0}};
static const struct kind timer_kind = {"ThreadPoolTimer", L"Windows.System.Threading.ThreadPoolTimer", timer_iids, destroy_timer};

static HRESULT start_timer(void *handler, INT64 span, BOOL periodic, void *destroyed, void **out)
{
	static const void *const *const tables[] = {timer_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!handler || span < 0)
		return E_INVALIDARG;
	struct timer *self = make(sizeof *self, &timer_kind, tables, 1);
	if (!self)
		return E_OUTOFMEMORY;
	reference(self->elapsed = handler);
	if ((self->destroyed = destroyed))
		reference(destroyed);
	if (periodic)
		self->period = span;
	else
		self->delay = span;
	if (!(self->timer = CreateThreadpoolTimer(elapsed, self, NULL))) {
		const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
		release(face_of(&self->base, 0));
		return error;
	}
	/* The pool counts periods in milliseconds: a shorter one runs every millisecond. */
	const DWORD milliseconds = periodic ? (DWORD)(span / 10000 > 0 ? (span / 10000 < MAXDWORD ? span / 10000 : MAXDWORD) : 1) : 0;
	ULARGE_INTEGER due;
	due.QuadPart = (ULONGLONG)-span;
	FILETIME when = {due.LowPart, due.HighPart};
	/* The reference the set timer holds, dropped by finish. */
	reference(face_of(&self->base, 0));
	SetThreadpoolTimer(self->timer, &when, milliseconds, 0);
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI create_periodic(void *iface, void *handler, INT64 period, void **out) { return start_timer(handler, period, TRUE, NULL, out); }
static HRESULT WINAPI create_single(void *iface, void *handler, INT64 delay, void **out) { return start_timer(handler, delay, FALSE, NULL, out); }
static HRESULT WINAPI create_periodic_completion(void *iface, void *handler, INT64 period, void *destroyed, void **out)
{
	return start_timer(handler, period, TRUE, destroyed, out);
}
static HRESULT WINAPI create_single_completion(void *iface, void *handler, INT64 delay, void *destroyed, void **out)
{
	return start_timer(handler, delay, FALSE, destroyed, out);
}

TABLE(timer_factory_table, 0, FACTORY);
TABLE(timer_statics_table, 1, INSPECTABLE, (const void *)create_periodic, (const void *)create_single, (const void *)create_periodic_completion,
	(const void *)create_single_completion);
static const struct face_iid timer_class_iids[] = {{&iid_factory, 0}, {&iid_timer_statics, 1}, {NULL, 0}};
static const struct kind timer_class_kind = {"ThreadPoolTimer", L"Windows.System.Threading.ThreadPoolTimer", timer_class_iids, NULL};
struct object thread_pool_timer_class = {{timer_factory_table.slots, timer_statics_table.slots}, &timer_class_kind, 1};
