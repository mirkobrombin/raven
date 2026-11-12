// A dispatcher queue runs the work posted to it on its own thread, through that thread's message loop, as paint.net's setup makes one
// for its window thread before it shows anything: a queue for the calling thread and one on a thread of its own, the queue a thread
// finds as its own, a timer of the queue, and shutting it down. The exit code names the first check that failed.
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winstring.h>
#include <roapi.h>
#include <asyncinfo.h>
#include <windows.system.h>

typedef __x_ABI_CWindows_CSystem_CIDispatcherQueue queue1;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueue2 queue2;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueueController controller;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueueTimer timer;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueueHandler work;
typedef __x_ABI_CWindows_CSystem_CIDispatcherQueueStatics queue_statics;

typedef struct {
	DWORD size, thread_type, apartment_type;
} options;

typedef HRESULT (WINAPI *create_function)(options, controller **);

/* One object for the work and the timer's tick handler: it counts its calls and notes the thread they ran on. */
struct counter {
	const void *table;
	LONG calls;
	DWORD thread;
	HANDLE done;
};

static HRESULT WINAPI counter_query(void *iface, REFIID iid, void **out)
{
	*out = iface;
	return S_OK;
}

static ULONG WINAPI counter_reference(void *iface) { return 2; }
static ULONG WINAPI counter_release(void *iface) { return 1; }

static HRESULT WINAPI counter_work(void *iface)
{
	struct counter *self = iface;
	self->thread = GetCurrentThreadId();
	InterlockedIncrement(&self->calls);
	if (self->done)
		SetEvent(self->done);
	return S_OK;
}

static HRESULT WINAPI counter_tick(void *iface, void *sender, void *args)
{
	return counter_work(iface);
}

static const void *work_table[] = {(void *)counter_query, (void *)counter_reference, (void *)counter_release, (void *)counter_work};
static const void *tick_table[] = {(void *)counter_query, (void *)counter_reference, (void *)counter_release, (void *)counter_tick};

// The thread's messages for up to a second, or until the counter reaches the calls wanted.
static void pump(struct counter *counter, LONG calls)
{
	DWORD start = GetTickCount();
	MSG message;
	while (counter->calls < calls && GetTickCount() - start < 1000) {
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		Sleep(1);
	}
}

int raven_entry(void)
{
	HMODULE library = LoadLibraryW(L"coremessaging.dll");
	if (!library)
		return 1;
	create_function create = (create_function)GetProcAddress(library, "CreateDispatcherQueueController");
	if (!create)
		return 2;
	controller *current = NULL;
	options bad = {sizeof bad, 3, 0}, mine = {sizeof mine, 2, 0}, own = {sizeof own, 1, 0};
	if (create(bad, &current) != E_INVALIDARG || FAILED(create(mine, &current)) || !current)
		return 3;
	// One queue to a thread.
	controller *second = NULL;
	if (SUCCEEDED(create(mine, &second)))
		return 4;

	queue1 *queue = NULL;
	queue2 *access = NULL;
	boolean flag = FALSE;
	if (FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueueController_get_DispatcherQueue(current, &queue)) || !queue)
		return 5;
	if (FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueue_QueryInterface(queue, &IID___x_ABI_CWindows_CSystem_CIDispatcherQueue2, (void **)&access)) ||
		FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueue2_get_HasThreadAccess(access, &flag)) || !flag)
		return 6;

	// Work posted runs only when the thread's loop dispatches it, and on this thread.
	struct counter posted = {work_table, 0, 0, NULL};
	if (FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueue_TryEnqueue(queue, (work *)&posted, &flag)) || !flag || posted.calls)
		return 7;
	pump(&posted, 1);
	if (posted.calls != 1 || posted.thread != GetCurrentThreadId())
		return 8;

	// The thread finds its queue through the class's statics.
	HSTRING name;
	queue_statics *statics = NULL;
	queue1 *found = NULL;
	if (FAILED(WindowsCreateString(L"Windows.System.DispatcherQueue", 30, &name)) ||
		FAILED(RoGetActivationFactory(name, &IID___x_ABI_CWindows_CSystem_CIDispatcherQueueStatics, (void **)&statics)) ||
		FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueueStatics_GetForCurrentThread(statics, &found)) || found != queue)
		return 9;

	// A timer that does not repeat ticks once.
	timer *clock = NULL;
	EventRegistrationToken token;
	struct counter ticks = {tick_table, 0, 0, NULL};
	__x_ABI_CWindows_CFoundation_CTimeSpan interval = {20 * 10000};
	if (FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueue_CreateTimer(queue, &clock)) ||
		FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueueTimer_put_Interval(clock, interval)) ||
		FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueueTimer_put_IsRepeating(clock, FALSE)) ||
		FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueueTimer_add_Tick(clock, (void *)&ticks, &token)) ||
		FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueueTimer_Start(clock)))
		return 10;
	pump(&ticks, 2);
	if (ticks.calls != 1 || FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueueTimer_get_IsRunning(clock, &flag)) || flag)
		return 11;
	__x_ABI_CWindows_CSystem_CIDispatcherQueueTimer_Release(clock);

	// Shut down, the queue takes no more work and the operation has completed.
	__x_ABI_CWindows_CFoundation_CIAsyncAction *action = NULL;
	IAsyncInfo *info = NULL;
	AsyncStatus status = Started;
	if (FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueueController_ShutdownQueueAsync(current, &action)) || !action ||
		FAILED(__x_ABI_CWindows_CFoundation_CIAsyncAction_QueryInterface(action, &IID_IAsyncInfo, (void **)&info)) ||
		FAILED(IAsyncInfo_get_Status(info, &status)) || status != Completed)
		return 12;
	if (FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueue_TryEnqueue(queue, (work *)&posted, &flag)) || flag)
		return 13;

	// A queue on a thread of its own runs the work there without this thread's loop.
	controller *dedicated = NULL;
	queue1 *remote = NULL;
	struct counter away = {work_table, 0, 0, CreateEventW(NULL, TRUE, FALSE, NULL)};
	if (FAILED(create(own, &dedicated)) || FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueueController_get_DispatcherQueue(dedicated, &remote)) ||
		FAILED(__x_ABI_CWindows_CSystem_CIDispatcherQueue_TryEnqueue(remote, (work *)&away, &flag)) || !flag)
		return 14;
	if (WaitForSingleObject(away.done, 2000) || away.calls != 1 || away.thread == GetCurrentThreadId())
		return 15;
	__x_ABI_CWindows_CSystem_CIDispatcherQueue_Release(remote);
	__x_ABI_CWindows_CSystem_CIDispatcherQueueController_Release(dedicated);
	return 0;
}
