#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <stddef.h>

/* raven-winmm.dll, the guest half of winmm.dll, on the device that plays to nowhere in real time (RAVEN_AUDIO=null):
 * - the multimedia timers run on one persistent timer thread of their own, and a synchronous kill waits out a running callback;
 * - a wave output calls back every way Windows does (function, thread message, window message, event), with WOM_OPEN before
 *   waveOutOpen returns and WOM_CLOSE before waveOutClose does, and buffers come back on the device's thread;
 * - a looped buffer plays its loops before it comes back, and waveOutBreakLoop ends a long loop;
 * - buffers come back at the pace of the format, pause holds the position, reset hands every buffer back before it returns, and a
 *   buffer the program frees once it is done is not touched again; the common errors come back from an output and an input alike;
 * - a wave input fills its buffers at the pace of the format;
 * - the mixer's speakers line has volume and mute controls whose changes are posted to an open mixer's window;
 * - PlaySound plays a RIFF image from memory and from a file synchronously, loops asynchronously, refuses SND_NOSTOP while it does,
 *   and stops on purge; a malformed image fails. */

void *memset(void *target, int value, size_t size)
{
	volatile unsigned char *bytes = target;
	while (size--)
		*bytes++ = (unsigned char)value;
	return target;
}

void *memcpy(void *target, const void *source, size_t size)
{
	volatile unsigned char *to = target;
	const unsigned char *from = source;
	while (size--)
		*to++ = *from++;
	return target;
}

static DWORD main_thread;

/* ----- Timers ----- */

static volatile LONG ticks;
static volatile DWORD tick_threads[8];
static volatile DWORD oneshot_thread;
static volatile LONG slow_started, slow_finished;

static void CALLBACK on_tick(UINT id, UINT message, DWORD_PTR instance, DWORD_PTR first, DWORD_PTR second)
{
	const LONG count = InterlockedIncrement(&ticks);
	if (count <= 8)
		tick_threads[count - 1] = GetCurrentThreadId();
}

static void CALLBACK on_oneshot(UINT id, UINT message, DWORD_PTR instance, DWORD_PTR first, DWORD_PTR second)
{
	oneshot_thread = GetCurrentThreadId();
}

static void CALLBACK on_slow(UINT id, UINT message, DWORD_PTR instance, DWORD_PTR first, DWORD_PTR second)
{
	InterlockedExchange(&slow_started, 1);
	Sleep(60);
	InterlockedExchange(&slow_finished, 1);
}

static volatile LONG late_ticks;
static volatile DWORD late_times[4];

static void CALLBACK on_late_tick(UINT id, UINT message, DWORD_PTR instance, DWORD_PTR first, DWORD_PTR second)
{
	const LONG count = InterlockedIncrement(&late_ticks);
	if (count <= 4)
		late_times[count - 1] = timeGetTime();
}

static void CALLBACK on_stall(UINT id, UINT message, DWORD_PTR instance, DWORD_PTR first, DWORD_PTR second)
{
	Sleep(130);
}

static int timers(void)
{
	const MMRESULT periodic = timeSetEvent(5, 1, on_tick, 0, TIME_PERIODIC);
	if (!periodic) return 1;
	for (int waited = 0; ticks < 5 && waited < 200; waited++)
		Sleep(5);
	if (timeKillEvent(periodic) != TIMERR_NOERROR) return 2;
	if (ticks < 5) return 3;
	/* One thread for every tick, and not the program's. */
	if (!tick_threads[0] || tick_threads[0] == main_thread) return 4;
	for (int index = 1; index < 5; index++)
		if (tick_threads[index] != tick_threads[0]) return 5;
	if (!timeSetEvent(5, 1, on_oneshot, 0, TIME_ONESHOT)) return 6;
	for (int waited = 0; !oneshot_thread && waited < 200; waited++)
		Sleep(5);
	if (oneshot_thread != tick_threads[0]) return 7;
	/* A synchronous kill returns only once the callback that is running has. */
	const MMRESULT slow = timeSetEvent(5, 1, on_slow, 0, TIME_ONESHOT | TIME_KILL_SYNCHRONOUS);
	if (!slow) return 8;
	for (int waited = 0; !slow_started && waited < 400; waited++)
		Sleep(1);
	if (!slow_started) return 9;
	if (timeKillEvent(slow) != TIMERR_NOERROR) return 10;
	if (!slow_finished) return 11;
	if (timeKillEvent(slow) != MMSYSERR_INVALPARAM) return 12;
	/* A periodic timer the timer thread reached more than a period late is called once, then a period later. */
	if (!timeSetEvent(1, 1, on_stall, 0, TIME_ONESHOT)) return 13;
	const MMRESULT late = timeSetEvent(40, 1, on_late_tick, 0, TIME_PERIODIC);
	if (!late) return 14;
	for (int waited = 0; late_ticks < 2 && waited < 400; waited++)
		Sleep(5);
	if (timeKillEvent(late) != TIMERR_NOERROR || late_ticks < 2) return 15;
	if (late_times[1] - late_times[0] < 10) return 16;
	return 0;
}

/* ----- Wave output ----- */

/* 8 kHz, 16-bit mono: a hundredth of a second is 80 samples. */
static short samples[8000];

static WAVEFORMATEX format_8k(void)
{
	WAVEFORMATEX format = {0};
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = 1;
	format.nSamplesPerSec = 8000;
	format.wBitsPerSample = 16;
	format.nBlockAlign = 2;
	format.nAvgBytesPerSec = 16000;
	return format;
}

static int prepare(HWAVEOUT device, WAVEHDR *header, DWORD count, DWORD flags, DWORD loops)
{
	ZeroMemory(header, sizeof *header);
	header->lpData = (LPSTR)samples;
	header->dwBufferLength = count * 2;
	header->dwFlags = flags;
	header->dwLoops = loops;
	return waveOutPrepareHeader(device, header, sizeof *header) != MMSYSERR_NOERROR;
}

static DWORD position(HWAVEOUT device)
{
	MMTIME time = {0};
	time.wType = TIME_SAMPLES;
	return waveOutGetPosition(device, &time, sizeof time) == MMSYSERR_NOERROR ? time.u.sample : (DWORD)-1;
}

static int wait_done(volatile DWORD *flags, DWORD milliseconds)
{
	for (DWORD waited = 0; !(*flags & WHDR_DONE) && waited < milliseconds; waited++)
		Sleep(1);
	return !(*flags & WHDR_DONE);
}

static volatile LONG opened, done, closed;
static volatile DWORD done_thread, open_thread;
static volatile HWAVEOUT callback_handle;
static volatile DWORD_PTR callback_instance;

static void CALLBACK on_wave(HWAVEOUT device, UINT message, DWORD_PTR instance, DWORD_PTR first, DWORD_PTR second)
{
	callback_handle = device;
	callback_instance = instance;
	if (message == WOM_OPEN) {
		open_thread = GetCurrentThreadId();
		InterlockedIncrement(&opened);
	} else if (message == WOM_DONE) {
		done_thread = GetCurrentThreadId();
		InterlockedIncrement(&done);
	} else if (message == WOM_CLOSE) {
		InterlockedIncrement(&closed);
	}
}

static int function_callback(void)
{
	const WAVEFORMATEX format = format_8k();
	HWAVEOUT device;
	if (waveOutOpen(&device, WAVE_MAPPER, &format, (DWORD_PTR)on_wave, 0x1234, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) return 20;
	if (opened != 1 || open_thread != main_thread || callback_instance != 0x1234) return 21;
	WAVEHDR first, second;
	if (prepare(device, &first, 160, 0, 0) || prepare(device, &second, 160, 0, 0)) return 22;
	if (waveOutWrite(device, &first, sizeof first) || waveOutWrite(device, &second, sizeof second)) return 23;
	if (wait_done(&second.dwFlags, 2000) || !(first.dwFlags & WHDR_DONE)) return 24;
	for (int waited = 0; done < 2 && waited < 200; waited++)
		Sleep(1);
	if (done != 2 || done_thread == main_thread || callback_handle != device) return 25;
	waveOutUnprepareHeader(device, &first, sizeof first);
	waveOutUnprepareHeader(device, &second, sizeof second);
	if (waveOutClose(device) != MMSYSERR_NOERROR || closed != 1) return 26;
	if (waveOutClose(device) != MMSYSERR_INVALHANDLE) return 27;
	return 0;
}

static volatile LONG self_close_result = -1, self_closed;

static void CALLBACK on_self_close(HWAVEOUT device, UINT message, DWORD_PTR instance, DWORD_PTR first, DWORD_PTR second)
{
	if (message == WOM_DONE)
		InterlockedExchange(&self_close_result, (LONG)waveOutClose(device));
	else if (message == WOM_CLOSE)
		InterlockedExchange(&self_closed, 1);
}

/* A device closed from the callback that hands back its last buffer closes, and its thread ends. */
static int close_in_callback(void)
{
	const WAVEFORMATEX format = format_8k();
	HWAVEOUT device;
	if (waveOutOpen(&device, WAVE_MAPPER, &format, (DWORD_PTR)on_self_close, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) return 112;
	static WAVEHDR header;
	if (prepare(device, &header, 80, 0, 0) || waveOutWrite(device, &header, sizeof header)) return 113;
	for (int waited = 0; !self_closed && waited < 2000; waited++)
		Sleep(1);
	if (self_close_result != MMSYSERR_NOERROR || !self_closed) return 114;
	if (waveOutClose(device) != MMSYSERR_INVALHANDLE) return 115;
	return 0;
}

/* A message for the device, taken from the thread's queue within a second. */
static int expect_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	MSG got;
	for (int waited = 0; waited < 1000; waited++) {
		if (PeekMessageW(&got, window, message, message, PM_REMOVE))
			return got.wParam != wparam || (lparam != -1 && got.lParam != lparam);
		Sleep(1);
	}
	return 1;
}

static int message_callback(BOOL by_window)
{
	HWND window = NULL;
	MSG unused;
	PeekMessageW(&unused, NULL, 0, 0, PM_NOREMOVE);
	if (by_window) {
		WNDCLASSW definition = {0};
		definition.lpfnWndProc = DefWindowProcW;
		definition.lpszClassName = L"RavenWaveClient";
		RegisterClassW(&definition);
		window = CreateWindowExW(0, L"RavenWaveClient", L"wave", 0, 0, 0, 1, 1, HWND_MESSAGE, NULL, NULL, NULL);
		if (!window) return 30;
	}
	const WAVEFORMATEX format = format_8k();
	HWAVEOUT device;
	const DWORD_PTR target = by_window ? (DWORD_PTR)window : (DWORD_PTR)main_thread;
	if (waveOutOpen(&device, 0, &format, target, 0, by_window ? CALLBACK_WINDOW : CALLBACK_THREAD) != MMSYSERR_NOERROR) return 31;
	if (expect_message(window, MM_WOM_OPEN, (WPARAM)device, 0)) return 32;
	WAVEHDR header;
	if (prepare(device, &header, 80, 0, 0) || waveOutWrite(device, &header, sizeof header)) return 33;
	if (expect_message(window, MM_WOM_DONE, (WPARAM)device, (LPARAM)&header)) return 34;
	if (!(header.dwFlags & WHDR_DONE) || (header.dwFlags & WHDR_INQUEUE)) return 35;
	waveOutUnprepareHeader(device, &header, sizeof header);
	if (waveOutClose(device) != MMSYSERR_NOERROR) return 36;
	if (expect_message(window, MM_WOM_CLOSE, (WPARAM)device, 0)) return 37;
	if (window)
		DestroyWindow(window);
	return 0;
}

static int loops(void)
{
	const WAVEFORMATEX format = format_8k();
	HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	HWAVEOUT device;
	if (waveOutOpen(&device, 0, &format, (DWORD_PTR)event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) return 40;
	/* Three passes over 10 ms come back as one buffer, after all of them have played. */
	WAVEHDR looped;
	if (prepare(device, &looped, 80, WHDR_BEGINLOOP | WHDR_ENDLOOP, 3) || waveOutWrite(device, &looped, sizeof looped)) return 41;
	if (wait_done(&looped.dwFlags, 2000)) return 42;
	if (position(device) != 240) return 43;
	/* A loop that would last minutes ends with the pass that is playing when it is broken. */
	if (waveOutReset(device) != MMSYSERR_NOERROR) return 44;
	WAVEHDR endless;
	if (prepare(device, &endless, 80, WHDR_BEGINLOOP | WHDR_ENDLOOP, 100000) || waveOutWrite(device, &endless, sizeof endless)) return 45;
	Sleep(40);
	if (endless.dwFlags & WHDR_DONE) return 46;
	if (waveOutBreakLoop(device) != MMSYSERR_NOERROR) return 47;
	if (wait_done(&endless.dwFlags, 1000)) return 48;
	/* A loop with no frame in it has nothing to repeat and comes back at once. */
	WAVEHDR empty;
	if (prepare(device, &empty, 0, WHDR_BEGINLOOP | WHDR_ENDLOOP, 0xffffffff) || waveOutWrite(device, &empty, sizeof empty)) return 110;
	if (wait_done(&empty.dwFlags, 1000)) return 111;
	waveOutUnprepareHeader(device, &looped, sizeof looped);
	waveOutUnprepareHeader(device, &endless, sizeof endless);
	waveOutUnprepareHeader(device, &empty, sizeof empty);
	if (waveOutClose(device) != MMSYSERR_NOERROR) return 49;
	CloseHandle(event);
	return 0;
}

static int reset_paused(void)
{
	const WAVEFORMATEX format = format_8k();
	HWAVEOUT device;
	if (waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
		return 120;
	WAVEHDR header;
	if (prepare(device, &header, 80, 0, 0))
		return 121;
	if (waveOutPause(device) != MMSYSERR_NOERROR || waveOutReset(device) != MMSYSERR_NOERROR)
		return 122;
	if (waveOutWrite(device, &header, sizeof header) != MMSYSERR_NOERROR || wait_done(&header.dwFlags, 500))
		return 123;
	if (waveOutUnprepareHeader(device, &header, sizeof header) || waveOutClose(device))
		return 124;
	return 0;
}

static WAVEHDR *reset_batch[2];
static volatile LONG reset_done, reset_flags_valid;

static void CALLBACK on_reset_batch(HWAVEOUT device, UINT message, DWORD_PTR instance, DWORD_PTR first, DWORD_PTR second)
{
	if (message != WOM_DONE)
		return;
	if (!(reset_batch[0]->dwFlags & WHDR_DONE) || (reset_batch[0]->dwFlags & WHDR_INQUEUE) ||
		!(reset_batch[1]->dwFlags & WHDR_DONE) || (reset_batch[1]->dwFlags & WHDR_INQUEUE))
		InterlockedExchange(&reset_flags_valid, 0);
	InterlockedIncrement(&reset_done);
}

static int reset_batch_flags(void)
{
	const WAVEFORMATEX format = format_8k();
	HWAVEOUT device;
	if (waveOutOpen(&device, WAVE_MAPPER, &format, (DWORD_PTR)on_reset_batch, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
		return 125;
	static short long_samples[2][8000];
	WAVEHDR headers[2] = {0};
	reset_batch[0] = &headers[0];
	reset_batch[1] = &headers[1];
	reset_done = 0;
	reset_flags_valid = 1;
	for (int index = 0; index < 2; index++) {
		headers[index].lpData = (LPSTR)long_samples[index];
		headers[index].dwBufferLength = sizeof long_samples[index];
		if (waveOutPrepareHeader(device, &headers[index], sizeof headers[index]) ||
			waveOutWrite(device, &headers[index], sizeof headers[index]))
			return 126;
	}
	if (waveOutReset(device) != MMSYSERR_NOERROR || reset_done != 2 || !reset_flags_valid)
		return 127;
	for (int index = 0; index < 2; index++)
		if (waveOutUnprepareHeader(device, &headers[index], sizeof headers[index]))
			return 128;
	if (waveOutClose(device))
		return 129;
	return 0;
}

/* A done buffer is the program's again: one it frees (here zeroes) from the callback of another buffer of the same batch still comes
 * back, and only once, from the device's thread and from a reset. */
static WAVEHDR reclaimed[6];
static volatile LONG reclaimed_calls[6];

static void CALLBACK on_reclaim(HWAVEOUT device, UINT message, DWORD_PTR instance, DWORD_PTR first, DWORD_PTR second)
{
	if (message != WOM_DONE)
		return;
	for (int index = 0; index < 6; index++) {
		if (first == (DWORD_PTR)&reclaimed[index])
			InterlockedIncrement(&reclaimed_calls[index]);
		if (reclaimed[index].dwFlags & WHDR_DONE)
			ZeroMemory(&reclaimed[index], sizeof reclaimed[index]);
	}
}

static int reclaim(void)
{
	const WAVEFORMATEX format = format_8k();
	HWAVEOUT device;
	if (waveOutOpen(&device, WAVE_MAPPER, &format, (DWORD_PTR)on_reclaim, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) return 130;
	/* Six buffers of a sample each play within one period of the device's thread; paused, they wait for the reset. */
	for (LONG round = 1; round <= 2; round++) {
		if (round == 2 && waveOutPause(device)) return 131;
		for (int index = 0; index < 6; index++)
			if (prepare(device, &reclaimed[index], 1, 0, 0) || waveOutWrite(device, &reclaimed[index], sizeof reclaimed[index])) return 132;
		if (round == 2 && waveOutReset(device)) return 133;
		for (int waited = 0; waited < 1000; waited++) {
			LONG returned = 0;
			for (int index = 0; index < 6; index++)
				returned += reclaimed_calls[index];
			if (returned >= 6 * round)
				break;
			Sleep(1);
		}
		Sleep(20);
		for (int index = 0; index < 6; index++)
			if (reclaimed_calls[index] != round) return 134;
		if (round == 1 && position(device) != 6) return 135;
	}
	if (waveOutClose(device)) return 136;
	return 0;
}

/* The errors a program meets most, from an output and an input alike. */
static int errors(void)
{
	WAVEFORMATEX format = format_8k();
	HWAVEOUT out;
	HWAVEIN in;
	if (waveOutOpen(&out, 0, &format, 0, 0, CALLBACK_NULL) || waveInOpen(&in, 0, &format, 0, 0, CALLBACK_NULL)) return 140;
	WAVEHDR header;
	ZeroMemory(&header, sizeof header);
	header.lpData = (LPSTR)samples;
	header.dwBufferLength = 160;
	if (waveOutWrite(out, &header, sizeof header) != WAVERR_UNPREPARED || waveInAddBuffer(in, &header, sizeof header) != WAVERR_UNPREPARED)
		return 141;
	if (waveOutPrepareHeader(out, &header, sizeof header - 1) != MMSYSERR_INVALPARAM ||
		waveInPrepareHeader(in, &header, sizeof header - 1) != MMSYSERR_INVALPARAM)
		return 142;
	MMTIME time = {0};
	time.wType = TIME_SAMPLES;
	if (waveOutGetPosition(out, &time, sizeof time - 1) != MMSYSERR_INVALPARAM || waveInGetPosition(in, &time, sizeof time - 1) != MMSYSERR_INVALPARAM)
		return 143;
	/* A queued buffer can be neither queued again nor unprepared. */
	if (waveOutPause(out) || waveOutPrepareHeader(out, &header, sizeof header) || waveOutWrite(out, &header, sizeof header)) return 144;
	if (waveOutWrite(out, &header, sizeof header) != WAVERR_STILLPLAYING || waveOutUnprepareHeader(out, &header, sizeof header) != WAVERR_STILLPLAYING)
		return 145;
	if (waveOutReset(out) || waveOutUnprepareHeader(out, &header, sizeof header)) return 146;
	if (waveInPrepareHeader(in, &header, sizeof header) || waveInAddBuffer(in, &header, sizeof header)) return 147;
	if (waveInAddBuffer(in, &header, sizeof header) != WAVERR_STILLPLAYING || waveInUnprepareHeader(in, &header, sizeof header) != WAVERR_STILLPLAYING)
		return 148;
	if (waveInReset(in) || waveInUnprepareHeader(in, &header, sizeof header)) return 149;
	/* A handle of the other direction is not one. */
	if (waveOutWrite((HWAVEOUT)in, &header, sizeof header) != MMSYSERR_INVALHANDLE || waveInClose((HWAVEIN)out) != MMSYSERR_INVALHANDLE)
		return 150;
	if (waveOutClose(out) || waveInClose(in)) return 151;
	if (waveOutClose(out) != MMSYSERR_INVALHANDLE || waveInClose(in) != MMSYSERR_INVALHANDLE) return 152;
	if (waveOutOpen(&out, 5, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_BADDEVICEID ||
		waveInOpen(&in, 5, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_BADDEVICEID)
		return 153;
	format.wFormatTag = WAVE_FORMAT_ADPCM;
	format.wBitsPerSample = 4;
	if (waveOutOpen(&out, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != WAVERR_BADFORMAT ||
		waveInOpen(&in, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != WAVERR_BADFORMAT)
		return 154;
	return 0;
}

/* Two tenths of a second in two buffers come back after about that long, the position is then exactly what they held, pause holds it,
 * reset hands every buffer back before it returns and starts the position again, and a device that still plays cannot close. */
static int pace(void)
{
	if (waveOutGetNumDevs() != 1) return 160;
	WAVEOUTCAPSW caps;
	if (waveOutGetDevCapsW(0, &caps, sizeof caps) != MMSYSERR_NOERROR || caps.wChannels != 2) return 161;
	const HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	const WAVEFORMATEX format = format_8k();
	HWAVEOUT device = NULL;
	if (waveOutOpen(&device, WAVE_MAPPER, &format, (DWORD_PTR)event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR || !device) return 162;
	/* WOM_OPEN */
	if (WaitForSingleObject(event, 0) != WAIT_OBJECT_0) return 163;

	WAVEHDR first, second;
	if (prepare(device, &first, 800, 0, 0) || prepare(device, &second, 800, 0, 0)) return 164;
	const DWORD start = GetTickCount();
	if (waveOutWrite(device, &first, sizeof first) || waveOutWrite(device, &second, sizeof second)) return 165;
	if (!(first.dwFlags & WHDR_INQUEUE) || (first.dwFlags & WHDR_DONE)) return 166;
	while (!(first.dwFlags & WHDR_DONE) || !(second.dwFlags & WHDR_DONE))
		if (WaitForSingleObject(event, 2000) != WAIT_OBJECT_0) return 167;
	const DWORD took = GetTickCount() - start;
	if (took < 150 || took > 1500) return 168;
	if ((first.dwFlags & WHDR_INQUEUE) || (second.dwFlags & WHDR_INQUEUE)) return 169;
	MMTIME bytes = {0};
	bytes.wType = TIME_BYTES;
	if (position(device) != 1600 || waveOutGetPosition(device, &bytes, sizeof bytes) || bytes.wType != TIME_BYTES || bytes.u.cb != 3200)
		return 170;

	WAVEHDR long_one;
	if (prepare(device, &long_one, 4000, 0, 0)) return 171;
	if (waveOutWrite(device, &long_one, sizeof long_one)) return 172;
	Sleep(100);
	if (waveOutPause(device)) return 173;
	const DWORD held = position(device);
	if (held <= 1600 || held >= 1600 + 4000) return 174;
	Sleep(100);
	if (position(device) != held) return 175;
	if (waveOutRestart(device)) return 176;
	Sleep(60);
	if (position(device) <= held) return 177;

	if (waveOutClose(device) != WAVERR_STILLPLAYING) return 178;
	while (WaitForSingleObject(event, 0) == WAIT_OBJECT_0)
		;
	if (waveOutReset(device)) return 179;
	if (!(long_one.dwFlags & WHDR_DONE) || (long_one.dwFlags & WHDR_INQUEUE)) return 180;
	if (WaitForSingleObject(event, 0) != WAIT_OBJECT_0) return 181;
	if (position(device) != 0) return 182;

	/* A short buffer wakes the waiter soon after it played. */
	WAVEHDR short_one;
	if (prepare(device, &short_one, 240, 0, 0)) return 183;
	const DWORD written = GetTickCount();
	if (waveOutWrite(device, &short_one, sizeof short_one)) return 184;
	if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0 || !(short_one.dwFlags & WHDR_DONE)) return 185;
	const DWORD waited = GetTickCount() - written;
	if (waited < 15 || waited > 300) return 186;

	for (WAVEHDR *header = &first; header; header = header == &first ? &second : header == &second ? &long_one :
			header == &long_one ? &short_one : NULL)
		if (waveOutUnprepareHeader(device, header, sizeof *header)) return 187;
	if (waveOutClose(device)) return 188;
	CloseHandle(event);
	return 0;
}

/* ----- Wave input ----- */

static volatile LONG recorded;

static void CALLBACK on_record(HWAVEIN device, UINT message, DWORD_PTR instance, DWORD_PTR first, DWORD_PTR second)
{
	if (message == WIM_DATA)
		InterlockedIncrement(&recorded);
}

static int input(void)
{
	if (waveInGetNumDevs() != 1) return 50;
	WAVEINCAPSW caps;
	if (waveInGetDevCapsW(0, &caps, sizeof caps) != MMSYSERR_NOERROR || !caps.szPname[0]) return 51;
	const WAVEFORMATEX format = format_8k();
	HWAVEIN device;
	if (waveInOpen(&device, WAVE_MAPPER, &format, (DWORD_PTR)on_record, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) return 52;
	static short recording[2][160];
	WAVEHDR headers[2];
	for (int index = 0; index < 2; index++) {
		ZeroMemory(&headers[index], sizeof headers[index]);
		headers[index].lpData = (LPSTR)recording[index];
		headers[index].dwBufferLength = sizeof recording[index];
		if (waveInPrepareHeader(device, &headers[index], sizeof headers[index]) || waveInAddBuffer(device, &headers[index], sizeof headers[index]))
			return 53;
	}
	const DWORD started = GetTickCount();
	if (waveInStart(device) != MMSYSERR_NOERROR) return 54;
	if (wait_done(&headers[1].dwFlags, 2000)) return 55;
	/* Two buffers of 20 ms each, filled at the pace of the format. */
	if (GetTickCount() - started < 25) return 56;
	if (headers[0].dwBytesRecorded != sizeof recording[0] || headers[1].dwBytesRecorded != sizeof recording[1]) return 57;
	for (int waited = 0; recorded < 2 && waited < 200; waited++)
		Sleep(1);
	if (recorded != 2) return 58;
	if (waveInStop(device) || waveInReset(device)) return 59;
	for (int index = 0; index < 2; index++)
		waveInUnprepareHeader(device, &headers[index], sizeof headers[index]);
	if (waveInClose(device) != MMSYSERR_NOERROR) return 60;
	return 0;
}

/* ----- Mixer ----- */

static int mixer(void)
{
	if (mixerGetNumDevs() != 1) return 70;
	MSG unused;
	PeekMessageW(&unused, NULL, 0, 0, PM_NOREMOVE);
	HWND window = CreateWindowExW(0, L"RavenWaveClient", L"mixer", 0, 0, 0, 1, 1, HWND_MESSAGE, NULL, NULL, NULL);
	if (!window) return 71;
	HMIXER handle;
	if (mixerOpen(&handle, 0, (DWORD_PTR)window, 0, CALLBACK_WINDOW) != MMSYSERR_NOERROR) return 72;
	MIXERLINEW line = {0};
	line.cbStruct = sizeof line;
	line.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
	if (mixerGetLineInfoW((HMIXEROBJ)handle, &line, MIXER_OBJECTF_HMIXER | MIXER_GETLINEINFOF_COMPONENTTYPE) != MMSYSERR_NOERROR) return 73;
	if (line.cControls != 2 || line.Target.dwType != MIXERLINE_TARGETTYPE_WAVEOUT) return 74;
	MIXERCONTROLW control = {0};
	MIXERLINECONTROLSW controls = {0};
	controls.cbStruct = sizeof controls;
	controls.dwLineID = line.dwLineID;
	controls.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
	controls.cControls = 1;
	controls.cbmxctrl = sizeof control;
	controls.pamxctrl = &control;
	if (mixerGetLineControlsW((HMIXEROBJ)handle, &controls, MIXER_OBJECTF_HMIXER | MIXER_GETLINECONTROLSF_ONEBYTYPE) != MMSYSERR_NOERROR)
		return 75;
	if (control.dwControlType != MIXERCONTROL_CONTROLTYPE_VOLUME || control.Bounds.dwMaximum != 0xffff) return 76;
	MIXERCONTROLDETAILS_UNSIGNED value = {0x8000};
	MIXERCONTROLDETAILS details = {0};
	details.cbStruct = sizeof details;
	details.dwControlID = control.dwControlID;
	details.cChannels = 1;
	details.cbDetails = sizeof value;
	details.paDetails = &value;
	if (mixerSetControlDetails((HMIXEROBJ)handle, &details, MIXER_OBJECTF_HMIXER | MIXER_SETCONTROLDETAILSF_VALUE) != MMSYSERR_NOERROR)
		return 77;
	if (expect_message(window, MM_MIXM_CONTROL_CHANGE, (WPARAM)handle, (LPARAM)control.dwControlID)) return 78;
	value.dwValue = 0;
	if (mixerGetControlDetailsW((HMIXEROBJ)handle, &details, MIXER_OBJECTF_HMIXER | MIXER_GETCONTROLDETAILSF_VALUE) != MMSYSERR_NOERROR ||
		value.dwValue != 0x8000)
		return 79;
	value.dwValue = 0xffff;
	mixerSetControlDetails((HMIXEROBJ)handle, &details, MIXER_OBJECTF_HMIXER | MIXER_SETCONTROLDETAILSF_VALUE);
	if (mixerClose(handle) != MMSYSERR_NOERROR || mixerClose(handle) != MMSYSERR_INVALHANDLE) return 80;
	DestroyWindow(window);
	return 0;
}

/* ----- PlaySound ----- */

/* A RIFF WAVE image of `count` samples of 8 kHz 16-bit mono. */
static BYTE image[44 + 16000];
static HANDLE sound_gate;
static volatile LONG sound_results[2];

static DWORD WINAPI concurrent_sound(void *parameter)
{
	WaitForSingleObject(sound_gate, INFINITE);
	const DWORD index = (DWORD)(DWORD_PTR)parameter;
	sound_results[index] = PlaySoundW((LPCWSTR)image, NULL, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NOSTOP | SND_NODEFAULT);
	return 0;
}

static DWORD build_image(DWORD count)
{
	const DWORD data = count * 2;
	const DWORD header[] = {0x46464952 /* RIFF */, 36 + data, 0x45564157 /* WAVE */, 0x20746d66 /* fmt  */, 16, 0x00010001, 8000, 16000,
		0x00100002, 0x61746164 /* data */, data};
	memcpy(image, header, sizeof header);
	return 44 + data;
}

static int sounds(void)
{
	DWORD size = build_image(400);
	DWORD started = GetTickCount();
	if (!PlaySoundW((LPCWSTR)image, NULL, SND_MEMORY | SND_SYNC | SND_NODEFAULT)) return 90;
	/* 50 ms of sound has played by the time a synchronous call returns. */
	if (GetTickCount() - started < 35) return 91;
	/* A looping sound plays on until it is purged, and refuses a sound that would not stop it. */
	started = GetTickCount();
	if (!PlaySoundW((LPCWSTR)image, NULL, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT)) return 92;
	if (GetTickCount() - started > 40) return 93;
	Sleep(80);
	if (PlaySoundW((LPCWSTR)image, NULL, SND_MEMORY | SND_ASYNC | SND_NOSTOP | SND_NODEFAULT)) return 94;
	if (!PlaySoundW(NULL, NULL, SND_PURGE)) return 95;
	if (!PlaySoundW((LPCWSTR)image, NULL, SND_MEMORY | SND_ASYNC | SND_NOSTOP | SND_NODEFAULT)) return 96;
	if (!PlaySoundW(NULL, NULL, 0)) return 97;
	/* Concurrent NOSTOP calls are serialized: one starts, and the other sees it and refuses. */
	sound_gate = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!sound_gate) return 104;
	HANDLE sounds[2] = {CreateThread(NULL, 0, concurrent_sound, (void *)0, 0, NULL),
		CreateThread(NULL, 0, concurrent_sound, (void *)1, 0, NULL)};
	if (!sounds[0] || !sounds[1]) return 104;
	SetEvent(sound_gate);
	if (WaitForMultipleObjects(2, sounds, TRUE, 2000) != WAIT_OBJECT_0) return 105;
	CloseHandle(sounds[0]);
	CloseHandle(sounds[1]);
	if ((sound_results[0] != 0) + (sound_results[1] != 0) != 1) return 106;
	if (!PlaySoundW(NULL, NULL, SND_PURGE)) return 107;
	CloseHandle(sound_gate);
	/* From a file, by name. */
	WCHAR path[MAX_PATH];
	const DWORD length = GetTempPathW(MAX_PATH - 32, path);
	if (!length) return 98;
	const WCHAR name[] = L"raven-wave-client.wav";
	memcpy(path + length, name, sizeof name);
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) return 99;
	DWORD written = 0;
	size = build_image(160);
	WriteFile(file, image, size, &written, NULL);
	CloseHandle(file);
	if (written != size) return 100;
	if (!PlaySoundW(path, NULL, SND_FILENAME | SND_SYNC | SND_NODEFAULT)) return 101;
	if (!sndPlaySoundW(path, SND_SYNC | SND_NODEFAULT)) return 102;
	DeleteFileW(path);
	/* An image that is not RIFF WAVE fails before anything plays. */
	image[8] = 'X';
	if (PlaySoundW((LPCWSTR)image, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT)) return 103;
	return 0;
}

int raven_entry(void)
{
	main_thread = GetCurrentThreadId();
	int failed = timers();
	if (!failed)
		failed = function_callback();
	if (!failed)
		failed = close_in_callback();
	if (!failed)
		failed = message_callback(FALSE);
	if (!failed)
		failed = message_callback(TRUE);
	if (!failed)
		failed = loops();
	if (!failed)
		failed = reset_paused();
	if (!failed)
		failed = reset_batch_flags();
	if (!failed)
		failed = reclaim();
	if (!failed)
		failed = errors();
	if (!failed)
		failed = pace();
	if (!failed)
		failed = input();
	if (!failed)
		failed = mixer();
	if (!failed)
		failed = sounds();
	return failed;
}
