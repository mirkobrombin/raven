// Windows.System.Threading as an app uses it: a work item runs on another thread with the action it completes, a failing one ends the
// action in error, a priority out of range is refused; a single-use timer runs once after its delay and then its destroyed handler, one
// the app lets go of still runs, and a periodic timer canceled from its own handler stops and is destroyed. The exit code names the first
// check that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <winstring.h>

typedef struct object object;
struct object {
	void **table;
};
#define CALL(self, place, type) ((type)(self)->table[place])

typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *query_method)(object *, const GUID *, void **);
typedef HRESULT (WINAPI *number_method)(object *, INT32 *);
typedef HRESULT (WINAPI *span_method)(object *, INT64 *);

static const GUID pool_statics = {0xb6bf67dd, 0x84bd, 0x44f8, {0xac, 0x1c, 0x93, 0xeb, 0xcb, 0x9d, 0xba, 0x91}};
static const GUID timer_statics = {0x1a8a9d02, 0xe482, 0x461b, {0xb8, 0xc7, 0x8e, 0xfa, 0xd1, 0xcc, 0xe5, 0x90}};
static const GUID async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

static object *factory(const WCHAR *name, const GUID *iid)
{
	struct reference reference = {1, (UINT32)lstrlenW(name), {0, 0}, name};
	object *out = NULL;
	return RoGetActivationFactory((HSTRING)&reference, iid, (void **)&out) == S_OK ? out : NULL;
}

static void release(object *self)
{
	if (self)
		CALL(self, 2, release_method)(self);
}

/* A delegate of any kind: it counts its calls, keeps what it was given and the thread it ran on, and answers what it is told to. */
struct handler {
	void **table;
	LONG references, calls;
	object *given;
	DWORD thread;
	HRESULT answer;
	HANDLE called;
	/* A timer canceled from its own handler on this call. */
	LONG cancel_at;
};

static HRESULT WINAPI handler_query(struct handler *self, const GUID *iid, void **out)
{
	*out = self;
	InterlockedIncrement(&self->references);
	return S_OK;
}

static ULONG WINAPI handler_reference(struct handler *self) { return (ULONG)InterlockedIncrement(&self->references); }
static ULONG WINAPI handler_release(struct handler *self) { return (ULONG)InterlockedDecrement(&self->references); }

static HRESULT WINAPI handler_invoke(struct handler *self, object *given)
{
	self->given = given;
	self->thread = GetCurrentThreadId();
	const LONG calls = InterlockedIncrement(&self->calls);
	if (self->cancel_at && calls == self->cancel_at)
		CALL(given, 8, HRESULT (WINAPI *)(object *))(given);
	SetEvent(self->called);
	return self->answer;
}

static void *handler_table[] = {(void *)handler_query, (void *)handler_reference, (void *)handler_release, (void *)handler_invoke};

static void handler_init(struct handler *self, HRESULT answer)
{
	self->table = handler_table;
	self->references = 1;
	self->calls = 0;
	self->given = NULL;
	self->answer = answer;
	self->cancel_at = 0;
	self->called = CreateEventW(NULL, FALSE, FALSE, NULL);
}

/* Waits for an action to end, and gives its status. */
static INT32 finished(object *action)
{
	object *info = NULL;
	INT32 status = 0;
	if (CALL(action, 0, query_method)(action, &async_info, (void **)&info) != S_OK)
		return -1;
	for (int round = 0; round < 500 && status == 0; ++round) {
		CALL(info, 7, number_method)(info, &status);
		if (!status)
			Sleep(10);
	}
	release(info);
	return status;
}

static int check_pool(void)
{
	object *pool = factory(L"Windows.System.Threading.ThreadPool", &pool_statics), *action = NULL;
	struct handler work;
	if (!pool) return 10;
	handler_init(&work, S_OK);
	if (CALL(pool, 6, HRESULT (WINAPI *)(object *, void *, object **))(pool, &work, &action) != S_OK || !action) return 11;
	if (WaitForSingleObject(work.called, 5000) || work.given != action || work.thread == GetCurrentThreadId()) return 12;
	if (finished(action) != 1) return 13;
	release(action);
	/* A handler that fails ends the action in error with its code. */
	handler_init(&work, E_FAIL);
	if (CALL(pool, 7, HRESULT (WINAPI *)(object *, void *, INT32, object **))(pool, &work, 1, &action) != S_OK) return 14;
	if (finished(action) != 3) return 15;
	object *info = NULL;
	HRESULT code = S_OK;
	CALL(action, 0, query_method)(action, &async_info, (void **)&info);
	if (CALL(info, 8, HRESULT (WINAPI *)(object *, HRESULT *))(info, &code) != S_OK || code != E_FAIL) return 16;
	release(info);
	release(action);
	if (CALL(pool, 8, HRESULT (WINAPI *)(object *, void *, INT32, UINT32, object **))(pool, &work, 5, 0, &action) == S_OK) return 17;
	release(pool);
	return 0;
}

static int check_timers(void)
{
	object *timers = factory(L"Windows.System.Threading.ThreadPoolTimer", &timer_statics), *timer = NULL;
	struct handler elapsed, destroyed;
	INT64 span = -1;
	if (!timers) return 30;
	/* Single use, after 50 ms, then destroyed. */
	handler_init(&elapsed, S_OK);
	handler_init(&destroyed, S_OK);
	const DWORD start = GetTickCount();
	if (CALL(timers, 9, HRESULT (WINAPI *)(object *, void *, INT64, void *, object **))(timers, &elapsed, 500000, &destroyed, &timer) != S_OK ||
		!timer) return 31;
	if (CALL(timer, 7, span_method)(timer, &span) != S_OK || span != 500000 || CALL(timer, 6, span_method)(timer, &span) != S_OK || span)
		return 32;
	if (WaitForSingleObject(destroyed.called, 5000) || elapsed.calls != 1 || GetTickCount() - start < 40 || elapsed.given != timer) return 33;
	Sleep(100);
	if (elapsed.calls != 1 || destroyed.calls != 1) return 34;
	release(timer);
	/* One the app lets go of at once still runs. */
	handler_init(&elapsed, S_OK);
	if (CALL(timers, 7, HRESULT (WINAPI *)(object *, void *, INT64, object **))(timers, &elapsed, 0, &timer) != S_OK) return 35;
	release(timer);
	if (WaitForSingleObject(elapsed.called, 5000)) return 36;
	/* Periodic every 20 ms, canceled by its handler on the third call: no fourth, and destroyed once. */
	handler_init(&elapsed, S_OK);
	handler_init(&destroyed, S_OK);
	elapsed.cancel_at = 3;
	if (CALL(timers, 8, HRESULT (WINAPI *)(object *, void *, INT64, void *, object **))(timers, &elapsed, 200000, &destroyed, &timer) != S_OK)
		return 37;
	if (CALL(timer, 6, span_method)(timer, &span) != S_OK || span != 200000) return 38;
	if (WaitForSingleObject(destroyed.called, 5000)) return 39;
	Sleep(150);
	if (elapsed.calls != 3 || destroyed.calls != 1) return 40;
	/* Cancel once more changes nothing. */
	if (CALL(timer, 8, HRESULT (WINAPI *)(object *))(timer) != S_OK) return 41;
	Sleep(50);
	if (destroyed.calls != 1) return 42;
	release(timer);
	release(timers);
	return 0;
}

int raven_entry(void)
{
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;
	int failed = check_pool();
	if (!failed)
		failed = check_timers();
	return failed;
}
