#include <windows.h>
#include <mmsystem.h>

/* Exported by winmm but missing from MinGW's headers. */
UINT WINAPI mmsystemGetVersion(void);

/* winmm as games and players use it: multimedia timers that call back or signal an event, killed with and without waiting for a
 * running callback, a timer that fires for a thread that only polls, callbacks that never overlap, and the device classes and PlaySound
 * calls that must answer instead of ending the program. */

static volatile LONG once_count, periodic_count, slow_done, active, overlapped, serial_runs, polled_count;
static volatile UINT seen_id, seen_message;
static volatile DWORD_PTR seen_user, seen_first, seen_second;
static HANDLE once_done, periodic_done, slow_started, serial_done;

static void CALLBACK once(UINT id, UINT message, DWORD_PTR user, DWORD_PTR first, DWORD_PTR second)
{
	seen_id = id;
	seen_message = message;
	seen_user = user;
	seen_first = first;
	seen_second = second;
	InterlockedIncrement(&once_count);
	SetEvent(once_done);
}

static void CALLBACK periodic(UINT id, UINT message, DWORD_PTR user, DWORD_PTR first, DWORD_PTR second)
{
	if (InterlockedIncrement(&periodic_count) == 3)
		SetEvent(periodic_done);
}

static void CALLBACK slow(UINT id, UINT message, DWORD_PTR user, DWORD_PTR first, DWORD_PTR second)
{
	SetEvent(slow_started);
	Sleep(60);
	slow_done = 1;
}

static void CALLBACK serial(UINT id, UINT message, DWORD_PTR user, DWORD_PTR first, DWORD_PTR second)
{
	if (InterlockedIncrement(&active) > 1)
		overlapped = 1;
	Sleep(30);
	InterlockedDecrement(&active);
	if (InterlockedIncrement(&serial_runs) == 2)
		SetEvent(serial_done);
}

static void CALLBACK polled(UINT id, UINT message, DWORD_PTR user, DWORD_PTR first, DWORD_PTR second)
{
	InterlockedIncrement(&polled_count);
}

static int timers(void)
{
	once_done = CreateEventW(NULL, TRUE, FALSE, NULL);
	periodic_done = CreateEventW(NULL, TRUE, FALSE, NULL);
	slow_started = CreateEventW(NULL, TRUE, FALSE, NULL);
	serial_done = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!once_done || !periodic_done || !slow_started || !serial_done) return 1;

	/* Delays outside timeGetDevCaps' range and a missing callback are refused; an unknown timer cannot be killed. */
	if (timeSetEvent(0, 1, once, 0, TIME_ONESHOT) || timeSetEvent(1000001, 1, once, 0, TIME_ONESHOT) || timeSetEvent(10, 1, NULL, 0, TIME_ONESHOT))
		return 2;
	/* The callback type is one of three: an event cannot be both set and pulsed, and unknown types are refused. */
	if (timeSetEvent(10, 1, (LPTIMECALLBACK)once_done, 0, TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE) ||
		timeSetEvent(10, 1, (LPTIMECALLBACK)once_done, 0, 0x40))
		return 2;
	if (timeKillEvent(0x7fff) != MMSYSERR_INVALPARAM) return 3;

	/* A one-shot timer calls back once with its own id, no message, and dwUser whole; afterwards its id is gone. */
	const DWORD_PTR user = (DWORD_PTR)-1;
	const MMRESULT first = timeSetEvent(20, 1, once, user, TIME_ONESHOT);
	if (!first) return 4;
	if (WaitForSingleObject(once_done, 5000) != WAIT_OBJECT_0) return 5;
	if (seen_id != first || seen_message || seen_user != user || seen_first || seen_second) return 6;
	Sleep(40);
	if (once_count != 1) return 7;
	if (timeKillEvent(first) != MMSYSERR_INVALPARAM) return 8;

	/* A periodic timer keeps calling back until it is killed and not after. */
	const MMRESULT repeating = timeSetEvent(10, 1, periodic, 0, TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
	if (!repeating || repeating == first) return 9;
	if (WaitForSingleObject(periodic_done, 5000) != WAIT_OBJECT_0) return 10;
	if (timeKillEvent(repeating) != TIMERR_NOERROR) return 11;
	const LONG stopped = periodic_count;
	Sleep(80);
	if (periodic_count != stopped) return 12;
	if (timeKillEvent(repeating) != MMSYSERR_INVALPARAM) return 13;

	/* TIME_KILL_SYNCHRONOUS: the kill returns only once the callback that is running has. */
	const MMRESULT waited = timeSetEvent(10, 1, slow, 0, TIME_ONESHOT | TIME_KILL_SYNCHRONOUS);
	if (!waited || WaitForSingleObject(slow_started, 5000) != WAIT_OBJECT_0) return 14;
	if (timeKillEvent(waited) != TIMERR_NOERROR || !slow_done) return 15;

	/* Callbacks of different timers due together run one after the other, as on Windows' single timer thread. */
	const MMRESULT left = timeSetEvent(10, 1, serial, 0, TIME_ONESHOT), right = timeSetEvent(10, 1, serial, 0, TIME_ONESHOT);
	if (!left || !right) return 16;
	if (WaitForSingleObject(serial_done, 5000) != WAIT_OBJECT_0 || overlapped) return 17;

	/* An event timer sets the event it names, with no callback thread. */
	HANDLE automatic = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!automatic) return 18;
	const MMRESULT set = timeSetEvent(15, 1, (LPTIMECALLBACK)automatic, 0, TIME_ONESHOT | TIME_CALLBACK_EVENT_SET);
	if (!set || WaitForSingleObject(automatic, 5000) != WAIT_OBJECT_0) return 19;

	/* A thread that never waits, only reads the clock, still gets its callback. */
	if (!timeSetEvent(10, 1, polled, 0, TIME_ONESHOT)) return 23;
	const DWORD until = GetTickCount() + 5000;
	while (!polled_count)
		if ((LONG)(GetTickCount() - until) > 0) return 24;
	return 0;
}

static volatile LONG pulse_woken, busy_stop;

static DWORD WINAPI pulse_waiter(void *event)
{
	if (WaitForSingleObject((HANDLE)event, 1000) == WAIT_OBJECT_0)
		InterlockedIncrement(&pulse_woken);
	return 0;
}

static DWORD WINAPI busy(void *unused)
{
	while (!busy_stop)
		GetFileAttributesW(L"C:\\raven-missing-file");
	return 0;
}

/* Pulse timers release the threads waiting at that moment and leave the event nonsignaled: all waiters of a manual-reset event, one of an
 * auto-reset event, nobody when nobody waits. */
static int pulses(void)
{
	HANDLE manual = CreateEventW(NULL, TRUE, FALSE, NULL), automatic = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!manual || !automatic) return 40;

	const MMRESULT oneshot = timeSetEvent(10, 1, (LPTIMECALLBACK)manual, 0, TIME_ONESHOT | TIME_CALLBACK_EVENT_PULSE);
	if (!oneshot || WaitForSingleObject(manual, 5000) != WAIT_OBJECT_0) return 41;
	if (WaitForSingleObject(manual, 0) != WAIT_TIMEOUT) return 42;

	/* Each period is waited for: the event is not left signaled after the first. */
	const DWORD start = GetTickCount();
	const MMRESULT periodic = timeSetEvent(15, 1, (LPTIMECALLBACK)manual, 0, TIME_PERIODIC | TIME_CALLBACK_EVENT_PULSE);
	if (!periodic) return 43;
	if (WaitForSingleObject(manual, 5000) != WAIT_OBJECT_0 || WaitForSingleObject(manual, 5000) != WAIT_OBJECT_0) return 44;
	if (GetTickCount() - start < 20) return 45;
	if (timeKillEvent(periodic) != TIMERR_NOERROR) return 46;

	/* A pulse nobody waits for changes nothing. */
	if (!timeSetEvent(10, 1, (LPTIMECALLBACK)automatic, 0, TIME_ONESHOT | TIME_CALLBACK_EVENT_PULSE)) return 47;
	Sleep(60);
	if (WaitForSingleObject(automatic, 0) != WAIT_TIMEOUT) return 48;

	/* Another thread that keeps calling the runtime does not make the waiter miss its pulse. */
	HANDLE spinner = CreateThread(NULL, 0, busy, NULL, 0, NULL);
	if (!spinner) return 49;
	if (!timeSetEvent(20, 1, (LPTIMECALLBACK)manual, 0, TIME_ONESHOT | TIME_CALLBACK_EVENT_PULSE)) return 50;
	const DWORD busy_wait = WaitForSingleObject(manual, 5000);
	busy_stop = 1;
	WaitForSingleObject(spinner, 5000);
	CloseHandle(spinner);
	if (busy_wait != WAIT_OBJECT_0) return 51;

	/* Two waiters: a manual-reset pulse releases both, an auto-reset pulse one. */
	for (int round = 0; round < 2; ++round) {
		HANDLE event = round ? automatic : manual, waiters[2];
		pulse_woken = 0;
		for (int index = 0; index < 2; ++index)
			if (!(waiters[index] = CreateThread(NULL, 0, pulse_waiter, event, 0, NULL))) return 52;
		if (!timeSetEvent(50, 1, (LPTIMECALLBACK)event, 0, TIME_ONESHOT | TIME_CALLBACK_EVENT_PULSE)) return 53;
		if (WaitForMultipleObjects(2, waiters, TRUE, 5000) != WAIT_OBJECT_0) return 54;
		CloseHandle(waiters[0]);
		CloseHandle(waiters[1]);
		if (pulse_woken != (round ? 1 : 2)) return 55 + round;
		if (WaitForSingleObject(event, 0) != WAIT_TIMEOUT) return 57;
	}

	/* PulseEvent itself does the same, which the guest winmm's timer thread relies on. */
	for (int round = 0; round < 2; ++round) {
		HANDLE event = round ? automatic : manual, waiters[2];
		pulse_woken = 0;
		for (int index = 0; index < 2; ++index)
			if (!(waiters[index] = CreateThread(NULL, 0, pulse_waiter, event, 0, NULL))) return 58;
		Sleep(50);
		if (!PulseEvent(event)) return 59;
		if (WaitForMultipleObjects(2, waiters, TRUE, 5000) != WAIT_OBJECT_0) return 60;
		CloseHandle(waiters[0]);
		CloseHandle(waiters[1]);
		if (pulse_woken != (round ? 1 : 2)) return 61 + round;
		if (WaitForSingleObject(event, 0) != WAIT_TIMEOUT) return 63;
	}
	if (PulseEvent(NULL) || GetLastError() != ERROR_INVALID_HANDLE) return 64;
	return 0;
}

static int devices(void)
{
	if (!PlaySoundW(NULL, NULL, 0) || !PlaySoundA(NULL, NULL, SND_PURGE) || !sndPlaySoundW(NULL, 0)) return 30;
	if (PlaySoundA("raven-missing-sound.wav", NULL, SND_ASYNC | SND_FILENAME | SND_NODEFAULT)) return 31;
	if (midiOutGetNumDevs() || midiInGetNumDevs() || auxGetNumDevs()) return 32;
	MIDIOUTCAPSW caps;
	if (midiOutGetDevCapsW(0, &caps, sizeof caps) != MMSYSERR_BADDEVICEID) return 33;
	HMIDIOUT out = (HMIDIOUT)1;
	if (midiOutOpen(&out, 0, 0, 0, CALLBACK_NULL) != MMSYSERR_BADDEVICEID || out) return 34;
	if (mmsystemGetVersion() != 0x030a) return 35;
	out = (HMIDIOUT)1;
	if (midiOutOpen(&out, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MIDIERR_NODEVICE || out) return 36;
	HMIDIIN in = (HMIDIIN)1;
	if (midiInOpen(&in, 0, 0, 0, CALLBACK_NULL) != MMSYSERR_BADDEVICEID || in) return 37;
	in = (HMIDIIN)1;
	if (midiInOpen(&in, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MIDIERR_NODEVICE || in) return 38;
	if (!PlaySoundW(L"raven-unconfigured-sound", NULL, SND_ALIAS | SND_ASYNC) ||
		!PlaySoundA("raven-unconfigured-sound", NULL, SND_ALIAS | SND_ASYNC)) return 65;
	if (PlaySoundW(L"raven-unconfigured-sound", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT) ||
		PlaySoundA("raven-unconfigured-sound", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT)) return 66;
	return 0;
}

int raven_entry(void)
{
	int failed = timers();
	if (!failed)
		failed = pulses();
	return failed ? failed : devices();
}
