/* SPDX-License-Identifier: MIT
 *
 * The multimedia timers: one thread of the process calls every timer back, so callbacks never overlap, as on Windows. Deadlines are kept
 * on the performance counter; a periodic timer that fell behind is not called back in a burst to catch up.
 */
#include "winmm_private.h"

typedef struct timer {
	struct timer *next;
	UINT id, delay, flags;
	DWORD_PTR callback, user;
	LONGLONG deadline;
	/* Killed while its callback ran: the timer thread frees it once the callback returns. */
	BOOL killed;
} timer;

#define CALLBACK_TYPE 0x00f0u

static CRITICAL_SECTION timer_lock;
static timer *timers;
static timer *running;
static UINT next_id = 1;
static HANDLE timer_thread, wake, finished;
static DWORD timer_thread_id;
static LONGLONG frequency;

void timer_init(void)
{
	InitializeCriticalSection(&timer_lock);
	LARGE_INTEGER rate;
	QueryPerformanceFrequency(&rate);
	frequency = rate.QuadPart;
}

static LONGLONG now(void)
{
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart;
}

static LONGLONG ticks_of(UINT milliseconds) { return (LONGLONG)milliseconds * frequency / 1000; }

static void unlink_timer(timer *gone)
{
	for (timer **link = &timers; *link; link = &(*link)->next)
		if (*link == gone) {
			*link = gone->next;
			return;
		}
}

static DWORD WINAPI timer_main(void *unused)
{
	EnterCriticalSection(&timer_lock);
	for (;;) {
		timer *due = NULL;
		for (timer *each = timers; each; each = each->next)
			if (!due || each->deadline < due->deadline)
				due = each;
		const LONGLONG at = now();
		if (!due || due->deadline > at) {
			DWORD wait = INFINITE;
			if (due) {
				/* Rounded up, so the wait never ends before the deadline and spins. */
				const LONGLONG left = due->deadline - at;
				wait = (DWORD)((left * 1000 + frequency - 1) / frequency);
			}
			LeaveCriticalSection(&timer_lock);
			WaitForSingleObject(wake, wait);
			EnterCriticalSection(&timer_lock);
			continue;
		}
		const BOOL periodic = due->flags & TIME_PERIODIC;
		if (periodic) {
			due->deadline += ticks_of(due->delay);
			if (due->deadline <= at)
				due->deadline = at + ticks_of(due->delay);
		} else {
			unlink_timer(due);
		}
		const UINT type = due->flags & CALLBACK_TYPE;
		if (type == TIME_CALLBACK_EVENT_SET) {
			SetEvent((HANDLE)due->callback);
		} else if (type == TIME_CALLBACK_EVENT_PULSE) {
			PulseEvent((HANDLE)due->callback);
		} else {
			running = due;
			ResetEvent(finished);
			LeaveCriticalSection(&timer_lock);
			((LPTIMECALLBACK)due->callback)(due->id, 0, due->user, 0, 0);
			EnterCriticalSection(&timer_lock);
			running = NULL;
			SetEvent(finished);
			if (due->killed) {
				FREE(due);
				continue;
			}
		}
		if (!periodic)
			FREE(due);
	}
	return 0;
}

EXPORT MMRESULT WINAPI timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback, DWORD_PTR user, UINT flags)
{
	const UINT type = flags & CALLBACK_TYPE;
	if (delay < 1 || delay > 1000000 || !callback ||
		(type != TIME_CALLBACK_FUNCTION && type != TIME_CALLBACK_EVENT_SET && type != TIME_CALLBACK_EVENT_PULSE))
		return 0;
	timer *made = ALLOC(sizeof *made);
	if (!made)
		return 0;
	made->delay = delay;
	made->flags = flags;
	made->callback = (DWORD_PTR)callback;
	made->user = user;
	EnterCriticalSection(&timer_lock);
	if (!timer_thread) {
		wake = CreateEventW(NULL, FALSE, FALSE, NULL);
		finished = CreateEventW(NULL, TRUE, TRUE, NULL);
		if (wake && finished)
			timer_thread = CreateThread(NULL, 64 * 1024, timer_main, NULL, 0, &timer_thread_id);
		if (!timer_thread) {
			if (wake)
				CloseHandle(wake);
			if (finished)
				CloseHandle(finished);
			wake = finished = NULL;
			LeaveCriticalSection(&timer_lock);
			FREE(made);
			return 0;
		}
		SetThreadPriority(timer_thread, THREAD_PRIORITY_TIME_CRITICAL);
	}
	/* Ids are not reused while their timer lives, or while a killed one's callback still runs. */
	for (;;) {
		UINT id = next_id++;
		if (!id || id > 0xffff)
			next_id = 1, id = next_id++;
		BOOL taken = running && running->id == id;
		for (timer *each = timers; each && !taken; each = each->next)
			taken = each->id == id;
		if (!taken) {
			made->id = id;
			break;
		}
	}
	made->deadline = now() + ticks_of(delay);
	made->next = timers;
	timers = made;
	const UINT id = made->id;
	LeaveCriticalSection(&timer_lock);
	SetEvent(wake);
	return id;
}

EXPORT MMRESULT WINAPI timeKillEvent(UINT id)
{
	EnterCriticalSection(&timer_lock);
	timer *found = NULL;
	for (timer *each = timers; each; each = each->next)
		if (each->id == id)
			found = each;
	/* A one-shot timer whose callback still runs can be killed too: TIME_KILL_SYNCHRONOUS then waits for it. */
	if (!found && running && running->id == id && !running->killed)
		found = running;
	if (!found) {
		LeaveCriticalSection(&timer_lock);
		return MMSYSERR_INVALPARAM;
	}
	unlink_timer(found);
	if (found == running) {
		found->killed = TRUE;
		const BOOL wait = (found->flags & TIME_KILL_SYNCHRONOUS) && GetCurrentThreadId() != timer_thread_id;
		LeaveCriticalSection(&timer_lock);
		if (wait)
			while (WaitForSingleObject(finished, INFINITE) == WAIT_OBJECT_0) {
				EnterCriticalSection(&timer_lock);
				const BOOL done = running != found;
				LeaveCriticalSection(&timer_lock);
				if (done)
					break;
			}
		return TIMERR_NOERROR;
	}
	FREE(found);
	LeaveCriticalSection(&timer_lock);
	return TIMERR_NOERROR;
}
