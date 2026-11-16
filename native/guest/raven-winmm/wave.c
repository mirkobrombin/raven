/* SPDX-License-Identifier: MIT
 *
 * The wave devices: one output and one input, each handle a stream of the runtime's sound engine in the program's own format and a
 * thread that moves the program's buffers through the stream's ring and hands them back. A buffer is done once the device has played
 * it (the stream's position passed its last frame) or, recording, once it is full; the thread then calls the program back without
 * holding the device's lock, so a callback may write the next buffer.
 */
#include "winmm_private.h"

typedef struct wave {
	struct wave *next;
	BOOL capture;
	CRITICAL_SECTION lock;
	notify notify;
	WAVEFORMATEX format;
	HANDLE stream, period, wake, thread;
	DWORD thread_id;
	RavenAudioShared *shared;
	BYTE *ring;
	UINT32 buffer_frames, frame_bytes;
	/* The program's buffers in order, linked through lpNext, from the oldest not yet handed back. */
	WAVEHDR *first, *last;
	/* Playing: the buffer being copied into the ring and how many of its bytes are, and the loop being repeated with the times it has
	 * left. Every buffer before feed (and before loop_start while a loop repeats) is in the ring; its reserved field holds the low 32
	 * bits of the stream frame it ends at. */
	WAVEHDR *feed, *loop_start;
	DWORD feed_offset, loops_left;
	/* The stream frame the loop's pass under way started at. */
	UINT64 loop_from;
	BOOL started, paused, closing, closed_by_worker;
	/* The handle's volume as waveOutSetVolume takes it: the left channel in the low word. */
	DWORD volume;
} wave;

static CRITICAL_SECTION wave_lock;
static wave *waves;
/* The volume of the device as a whole, set through its id rather than a handle. */
static DWORD device_volume = 0xffffffff;

void wave_init(void) { InitializeCriticalSection(&wave_lock); }

/* The open device of a handle, with its lock held: a close waits for that lock before the device is freed. */
static wave *wave_of(void *handle, BOOL capture)
{
	wave *found = NULL;
	EnterCriticalSection(&wave_lock);
	for (wave *each = waves; each && !found; each = each->next)
		if ((void *)each == handle && each->capture == capture)
			found = each;
	if (found) {
		EnterCriticalSection(&found->lock);
		if (found->closing) {
			LeaveCriticalSection(&found->lock);
			found = NULL;
		}
	}
	LeaveCriticalSection(&wave_lock);
	return found;
}

static BOOL wave_known(void *handle, BOOL capture)
{
	wave *wave = wave_of(handle, capture);
	if (wave)
		LeaveCriticalSection(&wave->lock);
	return wave != NULL;
}

static BOOL device_id_valid(UINT_PTR id) { return id == 0 || id == WAVE_MAPPER || id == (UINT_PTR)(UINT)WAVE_MAPPER; }

/* shortcut: the louder channel scales both, a stream has one gain; split it when the engine takes a gain per channel. */
static void apply_volume(wave *wave)
{
	if (wave->capture)
		return;
	const DWORD volume = wave->volume, device = device_volume;
	const float own = (float)(LOWORD(volume) > HIWORD(volume) ? LOWORD(volume) : HIWORD(volume)) / 65535.0f;
	const float whole = (float)(LOWORD(device) > HIWORD(device) ? LOWORD(device) : HIWORD(device)) / 65535.0f;
	union {
		float value;
		UINT32 bits;
	} scale = {own * whole * master_volume()};
	__atomic_store_n(&wave->shared->volume, scale.bits, __ATOMIC_RELEASE);
}

void wave_volumes_changed(void)
{
	EnterCriticalSection(&wave_lock);
	for (wave *each = waves; each; each = each->next)
		apply_volume(each);
	LeaveCriticalSection(&wave_lock);
}

static void ring_put(wave *wave, UINT64 at, const BYTE *source, UINT32 frames)
{
	const UINT32 start = (UINT32)(at % wave->buffer_frames), before_end = wave->buffer_frames - start;
	const UINT32 head = frames < before_end ? frames : before_end;
	memcpy(wave->ring + (size_t)start * wave->frame_bytes, source, (size_t)head * wave->frame_bytes);
	memcpy(wave->ring, source + (size_t)head * wave->frame_bytes, (size_t)(frames - head) * wave->frame_bytes);
}

static void ring_get(wave *wave, UINT64 at, BYTE *target, UINT32 frames)
{
	const UINT32 start = (UINT32)(at % wave->buffer_frames), before_end = wave->buffer_frames - start;
	const UINT32 head = frames < before_end ? frames : before_end;
	memcpy(target, wave->ring + (size_t)start * wave->frame_bytes, (size_t)head * wave->frame_bytes);
	memcpy(target + (size_t)head * wave->frame_bytes, wave->ring, (size_t)(frames - head) * wave->frame_bytes);
}

/* Copies the queued buffers into the ring as far as it has room, following loops, and starts the stream once it has something to play
 * unless the device is paused. With the device's lock held. */
static void render_feed(wave *wave)
{
	RavenAudioShared *shared = wave->shared;
	UINT64 written = load64(&shared->write_frames);
	const UINT64 read = load64(&shared->read_frames);
	while (wave->feed) {
		WAVEHDR *header = wave->feed;
		if (!wave->feed_offset && (header->dwFlags & WHDR_BEGINLOOP) && wave->loop_start != header) {
			wave->loop_start = header;
			wave->loops_left = header->dwLoops ? header->dwLoops - 1 : 0;
			wave->loop_from = written;
		}
		const DWORD length = header->dwBufferLength / wave->frame_bytes * wave->frame_bytes;
		if (wave->feed_offset < length) {
			const UINT32 room = wave->buffer_frames - (UINT32)(written - read);
			if (!room)
				break;
			UINT32 frames = (length - wave->feed_offset) / wave->frame_bytes;
			if (frames > room)
				frames = room;
			ring_put(wave, written, (const BYTE *)header->lpData + wave->feed_offset, frames);
			written += frames;
			wave->feed_offset += frames * wave->frame_bytes;
			continue;
		}
		header->reserved = (DWORD)written;
		if ((header->dwFlags & WHDR_ENDLOOP) && wave->loop_start) {
			/* A pass with no frame in it would repeat here without ever filling the ring. */
			if (wave->loops_left && written != wave->loop_from) {
				wave->loops_left--;
				wave->feed = wave->loop_start;
				wave->feed_offset = 0;
				wave->loop_from = written;
				continue;
			}
			wave->loop_start = NULL;
		}
		wave->feed = header->lpNext;
		wave->feed_offset = 0;
	}
	store64(&shared->write_frames, written);
	if (!wave->started && !wave->paused && written > load64(&shared->position_frames))
		wave->started = RavenAudioStart(wave->stream);
}

/* Takes the first count buffers off the queue and marks them done, in an array ending in NULL; NULL with no memory, leaving them queued.
 * A done buffer is the program's again, to free or queue again, so it is handed back from this array and never read or written after. */
static WAVEHDR **take_done(wave *wave, UINT32 count)
{
	WAVEHDR **done = count ? ALLOC((count + 1) * sizeof *done) : NULL;
	if (!done)
		return NULL;
	for (UINT32 index = 0; index < count; index++) {
		WAVEHDR *header = wave->first;
		wave->first = header->lpNext;
		header->lpNext = NULL;
		header->dwFlags = (header->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
		done[index] = header;
	}
	if (!wave->first)
		wave->last = NULL;
	return done;
}

/* Takes off the queue the buffers the device has played, and stops the stream once everything written has been. */
static WAVEHDR **render_step(wave *wave)
{
	render_feed(wave);
	const UINT64 played = load64(&wave->shared->position_frames);
	UINT32 count = 0;
	for (WAVEHDR *header = wave->first; header && header != wave->feed && header != wave->loop_start &&
		(INT32)((DWORD)played - (DWORD)header->reserved) >= 0; header = header->lpNext)
		count++;
	WAVEHDR **done = take_done(wave, count);
	if (wave->started && !wave->first && played >= load64(&wave->shared->write_frames)) {
		RavenAudioStop(wave->stream);
		wave->started = FALSE;
	}
	return done;
}

/* Moves what was recorded into the queued buffers and returns how many at the front of the queue it has filled; what arrives with no
 * buffer queued is lost, as on Windows. */
static UINT32 capture_step(wave *wave)
{
	RavenAudioShared *shared = wave->shared;
	const UINT64 written = load64(&shared->write_frames);
	UINT64 read = load64(&shared->read_frames);
	WAVEHDR *header = wave->first;
	UINT32 filled = 0;
	while (read < written) {
		if (!header) {
			read = written;
			break;
		}
		const DWORD capacity = header->dwBufferLength / wave->frame_bytes * wave->frame_bytes;
		UINT32 frames = (capacity - header->dwBytesRecorded) / wave->frame_bytes;
		if (frames > written - read)
			frames = (UINT32)(written - read);
		ring_get(wave, read, (BYTE *)header->lpData + header->dwBytesRecorded, frames);
		read += frames;
		header->dwBytesRecorded += frames * wave->frame_bytes;
		if (header->dwBytesRecorded >= capacity) {
			header = header->lpNext;
			filled++;
		}
	}
	store64(&shared->read_frames, read);
	return filled;
}

/* Called without the device's lock, and with a copy of its notify where a callback may close the device. */
static void hand_back(const notify *notify, HANDLE handle, BOOL capture, WAVEHDR **done)
{
	if (!done)
		return;
	for (WAVEHDR **each = done; *each; each++)
		notify_send(notify, handle, capture ? WIM_DATA : WOM_DONE, (DWORD_PTR)*each, 0);
	FREE(done);
}

static void wave_free(wave *wave)
{
	RavenAudioClose(wave->stream);
	CloseHandle(wave->period);
	CloseHandle(wave->wake);
	DeleteCriticalSection(&wave->lock);
	FREE(wave);
}

static DWORD WINAPI wave_main(void *parameter)
{
	wave *wave = parameter;
	const HANDLE events[2] = {wave->period, wave->wake};
	for (;;) {
		EnterCriticalSection(&wave->lock);
		if (wave->closing) {
			const BOOL free_here = wave->closed_by_worker;
			LeaveCriticalSection(&wave->lock);
			if (free_here) {
				CloseHandle(wave->thread);
				wave_free(wave);
			}
			return 0;
		}
		WAVEHDR **done = wave->capture ? take_done(wave, wave->started ? capture_step(wave) : 0) : render_step(wave);
		const BOOL active = wave->started;
		LeaveCriticalSection(&wave->lock);
		hand_back(&wave->notify, (HANDLE)wave, wave->capture, done);
		WaitForMultipleObjects(2, events, FALSE, active ? 100 : INFINITE);
	}
}

static MMRESULT wave_open(void **out, UINT id, const WAVEFORMATEX *format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags,
	BOOL capture)
{
	if (!device_id_valid(id))
		return MMSYSERR_BADDEVICEID;
	if (!format)
		return MMSYSERR_INVALPARAM;
	/* shortcut: the mapper, and so PlaySound, refuses ADPCM and other compressed formats; convert through the ACM when a program needs it. */
	if (!format_valid(format))
		return WAVERR_BADFORMAT;
	if (flags & WAVE_FORMAT_QUERY)
		return MMSYSERR_NOERROR;
	if (!out)
		return MMSYSERR_INVALPARAM;
	notify notify;
	const MMRESULT checked = notify_init(&notify, callback, instance, flags);
	if (checked)
		return checked;
	wave *made = ALLOC(sizeof *made);
	if (!made)
		return MMSYSERR_NOMEM;
	made->capture = capture;
	made->notify = notify;
	/* Only the plain header is kept: an extensible format reaches the engine as the program gave it. */
	made->format = *format;
	made->volume = 0xffffffff;
	InitializeCriticalSection(&made->lock);
	made->period = CreateEventW(NULL, FALSE, FALSE, NULL);
	made->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	const BOOL events = made->period && made->wake;
	if (events)
		made->stream = RavenAudioOpen(format, capture ? RAVEN_AUDIO_CAPTURE : 0, 0, 0, made->period, &made->shared);
	if (!made->stream) {
		if (made->period)
			CloseHandle(made->period);
		if (made->wake)
			CloseHandle(made->wake);
		DeleteCriticalSection(&made->lock);
		FREE(made);
		return events ? MMSYSERR_ERROR : MMSYSERR_NOMEM;
	}
	made->ring = (BYTE *)made->shared + RAVEN_AUDIO_DATA_OFFSET;
	made->buffer_frames = made->shared->buffer_frames;
	made->frame_bytes = made->shared->frame_bytes;
	made->thread = CreateThread(NULL, 64 * 1024, wave_main, made, 0, &made->thread_id);
	if (!made->thread) {
		wave_free(made);
		return MMSYSERR_NOMEM;
	}
	SetThreadPriority(made->thread, THREAD_PRIORITY_TIME_CRITICAL);
	EnterCriticalSection(&wave_lock);
	made->next = waves;
	waves = made;
	apply_volume(made);
	LeaveCriticalSection(&wave_lock);
	*out = made;
	notify_send(&made->notify, (HANDLE)made, capture ? WIM_OPEN : WOM_OPEN, 0, 0);
	return MMSYSERR_NOERROR;
}

static MMRESULT wave_close(void *handle, BOOL capture)
{
	wave *wave = wave_of(handle, capture);
	if (!wave)
		return MMSYSERR_INVALHANDLE;
	if (wave->first) {
		LeaveCriticalSection(&wave->lock);
		return WAVERR_STILLPLAYING;
	}
	if (wave->started)
		RavenAudioStop(wave->stream);
	wave->started = FALSE;
	wave->paused = FALSE;
	wave->closing = TRUE;
	/* Closed from a callback, the device's own thread frees it once the callback returns. */
	const BOOL on_worker = GetCurrentThreadId() == wave->thread_id;
	wave->closed_by_worker = on_worker;
	SetEvent(wave->wake);
	LeaveCriticalSection(&wave->lock);
	EnterCriticalSection(&wave_lock);
	for (struct wave **link = &waves; *link; link = &(*link)->next)
		if (*link == wave) {
			*link = wave->next;
			break;
		}
	LeaveCriticalSection(&wave_lock);
	const struct notify notify = wave->notify;
	if (!on_worker) {
		WaitForSingleObject(wave->thread, INFINITE);
		CloseHandle(wave->thread);
		wave_free(wave);
	}
	notify_send(&notify, (HANDLE)handle, capture ? WIM_CLOSE : WOM_CLOSE, 0, 0);
	return MMSYSERR_NOERROR;
}

/* MMSYSERR_NOERROR leaves the device's lock held. */
static MMRESULT header_check(void *handle, WAVEHDR *header, UINT size, BOOL capture, wave **out)
{
	if (!header || size < sizeof(WAVEHDR))
		return wave_known(handle, capture) ? MMSYSERR_INVALPARAM : MMSYSERR_INVALHANDLE;
	return (*out = wave_of(handle, capture)) ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

static MMRESULT set_prepared(void *handle, WAVEHDR *header, UINT size, BOOL capture, BOOL prepared)
{
	wave *wave;
	MMRESULT result = header_check(handle, header, size, capture, &wave);
	if (result)
		return result;
	if (header->dwFlags & WHDR_INQUEUE)
		result = WAVERR_STILLPLAYING;
	else if (prepared)
		header->dwFlags |= WHDR_PREPARED;
	else
		header->dwFlags &= ~WHDR_PREPARED;
	LeaveCriticalSection(&wave->lock);
	return result;
}

static MMRESULT prepare(void *handle, WAVEHDR *header, UINT size, BOOL capture) { return set_prepared(handle, header, size, capture, TRUE); }
static MMRESULT unprepare(void *handle, WAVEHDR *header, UINT size, BOOL capture) { return set_prepared(handle, header, size, capture, FALSE); }

static MMRESULT enqueue(void *handle, WAVEHDR *header, UINT size, BOOL capture)
{
	wave *wave;
	const MMRESULT checked = header_check(handle, header, size, capture, &wave);
	if (checked)
		return checked;
	if ((header->dwFlags & (WHDR_PREPARED | WHDR_INQUEUE)) != WHDR_PREPARED) {
		LeaveCriticalSection(&wave->lock);
		return header->dwFlags & WHDR_PREPARED ? WAVERR_STILLPLAYING : WAVERR_UNPREPARED;
	}
	header->dwFlags = (header->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
	header->lpNext = NULL;
	header->reserved = 0;
	if (capture)
		header->dwBytesRecorded = 0;
	if (wave->last)
		wave->last->lpNext = header;
	else
		wave->first = header;
	wave->last = header;
	if (!capture) {
		if (!wave->feed && !wave->feed_offset)
			wave->feed = header;
		render_feed(wave);
	}
	SetEvent(wave->wake);
	LeaveCriticalSection(&wave->lock);
	return MMSYSERR_NOERROR;
}

/* Stops the device and hands every queued buffer back before returning; the position starts again from zero. */
static MMRESULT reset(void *handle, BOOL capture)
{
	wave *wave = wave_of(handle, capture);
	if (!wave)
		return MMSYSERR_INVALHANDLE;
	if (capture && wave->started)
		capture_step(wave);
	UINT32 count = 0;
	for (WAVEHDR *header = wave->first; header; header = header->lpNext)
		count++;
	WAVEHDR **done = take_done(wave, count);
	if (count && !done) {
		LeaveCriticalSection(&wave->lock);
		return MMSYSERR_NOMEM;
	}
	RavenAudioStop(wave->stream);
	RavenAudioReset(wave->stream);
	wave->started = FALSE;
	wave->paused = FALSE;
	wave->feed = wave->loop_start = NULL;
	wave->feed_offset = wave->loops_left = 0;
	const notify notify = wave->notify;
	LeaveCriticalSection(&wave->lock);
	hand_back(&notify, (HANDLE)handle, capture, done);
	return MMSYSERR_NOERROR;
}

static MMRESULT get_position(void *handle, MMTIME *time, UINT size, BOOL capture)
{
	wave *wave = wave_of(handle, capture);
	if (!wave)
		return MMSYSERR_INVALHANDLE;
	if (!time || size < sizeof(MMTIME)) {
		LeaveCriticalSection(&wave->lock);
		return MMSYSERR_INVALPARAM;
	}
	const UINT64 frames = load64(&wave->shared->position_frames);
	switch (time->wType) {
	case TIME_SAMPLES:
		time->u.sample = (DWORD)frames;
		break;
	case TIME_MS:
		time->u.ms = (DWORD)(frames * 1000 / wave->format.nSamplesPerSec);
		break;
	default:
		time->wType = TIME_BYTES;
		time->u.cb = (DWORD)(frames * wave->format.nBlockAlign);
		break;
	}
	LeaveCriticalSection(&wave->lock);
	return MMSYSERR_NOERROR;
}

static MMRESULT error_text(MMRESULT error, WCHAR *text, UINT size)
{
	static const struct {
		MMRESULT error;
		const WCHAR *text;
	} texts[] = {
		{MMSYSERR_NOERROR, L"The command completed successfully."},
		{MMSYSERR_ERROR, L"An unspecified error occurred."},
		{MMSYSERR_BADDEVICEID, L"The device identifier is out of range."},
		{MMSYSERR_NOTENABLED, L"The device's driver is disabled."},
		{MMSYSERR_ALLOCATED, L"Another handle has the device open."},
		{MMSYSERR_INVALHANDLE, L"The handle is not valid."},
		{MMSYSERR_NODRIVER, L"No driver is installed for the device."},
		{MMSYSERR_NOMEM, L"There is not enough memory."},
		{MMSYSERR_NOTSUPPORTED, L"The function is not supported."},
		{MMSYSERR_BADERRNUM, L"The error number is not known."},
		{MMSYSERR_INVALFLAG, L"A flag is not valid."},
		{MMSYSERR_INVALPARAM, L"A parameter is not valid."},
		{WAVERR_BADFORMAT, L"The wave format is not supported."},
		{WAVERR_STILLPLAYING, L"The device is still playing."},
		{WAVERR_UNPREPARED, L"The header is not prepared."},
		{WAVERR_SYNC, L"The device opens only with the WAVE_ALLOWSYNC flag."},
	};
	if (!text || !size)
		return MMSYSERR_INVALPARAM;
	for (size_t index = 0; index < sizeof texts / sizeof *texts; index++)
		if (texts[index].error == error) {
			lstrcpynW(text, texts[index].text, (int)size);
			return MMSYSERR_NOERROR;
		}
	return MMSYSERR_BADERRNUM;
}

static MMRESULT error_text_ascii(MMRESULT error, char *text, UINT size)
{
	if (!text || !size)
		return MMSYSERR_INVALPARAM;
	WCHAR wide[256];
	const MMRESULT result = error_text(error, wide, sizeof wide / sizeof *wide);
	if (!result)
		ascii_from_wide(text, size, wide);
	return result;
}

static MMRESULT message(UINT_PTR handle, UINT message, DWORD_PTR first, DWORD_PTR second, BOOL capture)
{
	if (!device_id_valid(handle) && !wave_known((void *)handle, capture))
		return MMSYSERR_INVALHANDLE;
	switch (message) {
	/* The preferred device is the only one, and not only for the console's voice. */
	case DRVM_MAPPER_PREFERRED_GET:
	case DRVM_MAPPER_CONSOLEVOICECOM_GET:
		if (!first || !second)
			return MMSYSERR_INVALPARAM;
		*(DWORD *)first = 0;
		*(DWORD *)second = 0;
		return MMSYSERR_NOERROR;
	default:
		return MMSYSERR_NOTSUPPORTED;
	}
}

/* ----- Output ----- */

EXPORT UINT WINAPI waveOutGetNumDevs(void) { return 1; }

static void output_caps(UINT_PTR id, WAVEOUTCAPSW *caps)
{
	memset(caps, 0, sizeof *caps);
	caps->wMid = MM_MICROSOFT;
	caps->wPid = id == 0 ? MM_MSFT_WSS_WAVEOUT : MM_WAVE_MAPPER;
	caps->vDriverVersion = 0x0500;
	lstrcpynW(caps->szPname, id == 0 ? L"Speakers (Raven Audio)" : L"Microsoft Sound Mapper", MAXPNAMELEN);
	caps->dwFormats = 0x000fffff;
	caps->wChannels = 2;
	caps->dwSupport = WAVECAPS_VOLUME | WAVECAPS_SAMPLEACCURATE;
}

static MMRESULT output_id(UINT_PTR id, UINT_PTR *device)
{
	if (wave_known((void *)id, FALSE)) {
		*device = 0;
		return MMSYSERR_NOERROR;
	}
	if (!device_id_valid(id))
		return MMSYSERR_BADDEVICEID;
	*device = id == 0 ? 0 : WAVE_MAPPER;
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR id, LPWAVEOUTCAPSW caps, UINT size)
{
	UINT_PTR device;
	const MMRESULT result = output_id(id, &device);
	if (result)
		return result;
	if (!caps)
		return MMSYSERR_INVALPARAM;
	WAVEOUTCAPSW full;
	output_caps(device, &full);
	memcpy(caps, &full, size < sizeof full ? size : sizeof full);
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR id, LPWAVEOUTCAPSA caps, UINT size)
{
	UINT_PTR device;
	const MMRESULT result = output_id(id, &device);
	if (result)
		return result;
	if (!caps)
		return MMSYSERR_INVALPARAM;
	WAVEOUTCAPSW wide;
	output_caps(device, &wide);
	WAVEOUTCAPSA full;
	full.wMid = wide.wMid;
	full.wPid = wide.wPid;
	full.vDriverVersion = wide.vDriverVersion;
	ascii_from_wide(full.szPname, sizeof full.szPname, wide.szPname);
	full.dwFormats = wide.dwFormats;
	full.wChannels = wide.wChannels;
	full.wReserved1 = 0;
	full.dwSupport = wide.dwSupport;
	memcpy(caps, &full, size < sizeof full ? size : sizeof full);
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutOpen(LPHWAVEOUT out, UINT id, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
	return wave_open((void **)out, id, format, callback, instance, flags, FALSE);
}

EXPORT MMRESULT WINAPI waveOutClose(HWAVEOUT handle) { return wave_close(handle, FALSE); }
EXPORT MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT handle, LPWAVEHDR header, UINT size) { return prepare(handle, header, size, FALSE); }
EXPORT MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT handle, LPWAVEHDR header, UINT size) { return unprepare(handle, header, size, FALSE); }
EXPORT MMRESULT WINAPI waveOutWrite(HWAVEOUT handle, LPWAVEHDR header, UINT size) { return enqueue(handle, header, size, FALSE); }
EXPORT MMRESULT WINAPI waveOutReset(HWAVEOUT handle) { return reset(handle, FALSE); }

/* Pausing holds the position; new buffers queue without playing until the device is restarted or reset. */
EXPORT MMRESULT WINAPI waveOutPause(HWAVEOUT handle)
{
	wave *wave = wave_of(handle, FALSE);
	if (!wave)
		return MMSYSERR_INVALHANDLE;
	if (wave->started)
		RavenAudioStop(wave->stream);
	wave->started = FALSE;
	wave->paused = TRUE;
	LeaveCriticalSection(&wave->lock);
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutRestart(HWAVEOUT handle)
{
	wave *wave = wave_of(handle, FALSE);
	if (!wave)
		return MMSYSERR_INVALHANDLE;
	wave->paused = FALSE;
	render_feed(wave);
	SetEvent(wave->wake);
	LeaveCriticalSection(&wave->lock);
	return MMSYSERR_NOERROR;
}

/* The loop being played ends after the pass under way. */
EXPORT MMRESULT WINAPI waveOutBreakLoop(HWAVEOUT handle)
{
	wave *wave = wave_of(handle, FALSE);
	if (!wave)
		return MMSYSERR_INVALHANDLE;
	wave->loops_left = 0;
	LeaveCriticalSection(&wave->lock);
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutGetPosition(HWAVEOUT handle, LPMMTIME time, UINT size) { return get_position(handle, time, size, FALSE); }

EXPORT MMRESULT WINAPI waveOutGetVolume(HWAVEOUT handle, LPDWORD volume)
{
	wave *wave = wave_of(handle, FALSE);
	if (!wave && !device_id_valid((UINT_PTR)handle))
		return MMSYSERR_INVALHANDLE;
	if (volume)
		*volume = wave ? wave->volume : device_volume;
	if (wave)
		LeaveCriticalSection(&wave->lock);
	return volume ? MMSYSERR_NOERROR : MMSYSERR_INVALPARAM;
}

EXPORT MMRESULT WINAPI waveOutSetVolume(HWAVEOUT handle, DWORD volume)
{
	wave *wave = wave_of(handle, FALSE);
	if (!wave && !device_id_valid((UINT_PTR)handle))
		return MMSYSERR_INVALHANDLE;
	if (wave) {
		wave->volume = volume;
		LeaveCriticalSection(&wave->lock);
	} else {
		device_volume = volume;
	}
	wave_volumes_changed();
	return MMSYSERR_NOERROR;
}

static MMRESULT unsupported(HWAVEOUT handle) { return wave_known(handle, FALSE) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutGetPitch(HWAVEOUT handle, LPDWORD pitch) { return unsupported(handle); }
EXPORT MMRESULT WINAPI waveOutSetPitch(HWAVEOUT handle, DWORD pitch) { return unsupported(handle); }
EXPORT MMRESULT WINAPI waveOutGetPlaybackRate(HWAVEOUT handle, LPDWORD rate) { return unsupported(handle); }
EXPORT MMRESULT WINAPI waveOutSetPlaybackRate(HWAVEOUT handle, DWORD rate) { return unsupported(handle); }

EXPORT MMRESULT WINAPI waveOutGetID(HWAVEOUT handle, LPUINT id)
{
	if (!wave_known(handle, FALSE))
		return MMSYSERR_INVALHANDLE;
	if (!id)
		return MMSYSERR_INVALPARAM;
	*id = 0;
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutMessage(HWAVEOUT handle, UINT message_id, DWORD_PTR first, DWORD_PTR second)
{
	return message((UINT_PTR)handle, message_id, first, second, FALSE);
}

EXPORT MMRESULT WINAPI waveOutGetErrorTextW(MMRESULT error, LPWSTR text, UINT size) { return error_text(error, text, size); }
EXPORT MMRESULT WINAPI waveOutGetErrorTextA(MMRESULT error, LPSTR text, UINT size) { return error_text_ascii(error, text, size); }

/* ----- Input ----- */

EXPORT UINT WINAPI waveInGetNumDevs(void) { return 1; }

static void input_caps(UINT_PTR id, WAVEINCAPSW *caps)
{
	memset(caps, 0, sizeof *caps);
	caps->wMid = MM_MICROSOFT;
	caps->wPid = id == 0 ? MM_MSFT_WSS_WAVEIN : MM_WAVE_MAPPER;
	caps->vDriverVersion = 0x0500;
	lstrcpynW(caps->szPname, id == 0 ? L"Microphone (Raven Audio)" : L"Microsoft Sound Mapper", MAXPNAMELEN);
	caps->dwFormats = 0x000fffff;
	caps->wChannels = 2;
}

static MMRESULT input_id(UINT_PTR id, UINT_PTR *device)
{
	if (wave_known((void *)id, TRUE)) {
		*device = 0;
		return MMSYSERR_NOERROR;
	}
	if (!device_id_valid(id))
		return MMSYSERR_BADDEVICEID;
	*device = id == 0 ? 0 : WAVE_MAPPER;
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR id, LPWAVEINCAPSW caps, UINT size)
{
	UINT_PTR device;
	const MMRESULT result = input_id(id, &device);
	if (result)
		return result;
	if (!caps)
		return MMSYSERR_INVALPARAM;
	WAVEINCAPSW full;
	input_caps(device, &full);
	memcpy(caps, &full, size < sizeof full ? size : sizeof full);
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInGetDevCapsA(UINT_PTR id, LPWAVEINCAPSA caps, UINT size)
{
	UINT_PTR device;
	const MMRESULT result = input_id(id, &device);
	if (result)
		return result;
	if (!caps)
		return MMSYSERR_INVALPARAM;
	WAVEINCAPSW wide;
	input_caps(device, &wide);
	WAVEINCAPSA full;
	full.wMid = wide.wMid;
	full.wPid = wide.wPid;
	full.vDriverVersion = wide.vDriverVersion;
	ascii_from_wide(full.szPname, sizeof full.szPname, wide.szPname);
	full.dwFormats = wide.dwFormats;
	full.wChannels = wide.wChannels;
	full.wReserved1 = 0;
	memcpy(caps, &full, size < sizeof full ? size : sizeof full);
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInOpen(LPHWAVEIN out, UINT id, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
	return wave_open((void **)out, id, format, callback, instance, flags, TRUE);
}

EXPORT MMRESULT WINAPI waveInClose(HWAVEIN handle) { return wave_close(handle, TRUE); }
EXPORT MMRESULT WINAPI waveInPrepareHeader(HWAVEIN handle, LPWAVEHDR header, UINT size) { return prepare(handle, header, size, TRUE); }
EXPORT MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN handle, LPWAVEHDR header, UINT size) { return unprepare(handle, header, size, TRUE); }
EXPORT MMRESULT WINAPI waveInAddBuffer(HWAVEIN handle, LPWAVEHDR header, UINT size) { return enqueue(handle, header, size, TRUE); }
EXPORT MMRESULT WINAPI waveInReset(HWAVEIN handle) { return reset(handle, TRUE); }

EXPORT MMRESULT WINAPI waveInStart(HWAVEIN handle)
{
	wave *wave = wave_of(handle, TRUE);
	if (!wave)
		return MMSYSERR_INVALHANDLE;
	if (!wave->started)
		wave->started = RavenAudioStart(wave->stream);
	const BOOL started = wave->started;
	SetEvent(wave->wake);
	LeaveCriticalSection(&wave->lock);
	return started ? MMSYSERR_NOERROR : MMSYSERR_ERROR;
}

/* Stops recording; the buffer being filled comes back with what it holds, the empty ones stay queued. */
EXPORT MMRESULT WINAPI waveInStop(HWAVEIN handle)
{
	wave *wave = wave_of(handle, TRUE);
	if (!wave)
		return MMSYSERR_INVALHANDLE;
	WAVEHDR **done = NULL;
	UINT32 count = 0;
	if (wave->started) {
		RavenAudioStop(wave->stream);
		count = capture_step(wave);
		wave->started = FALSE;
		WAVEHDR *partial = wave->first;
		for (UINT32 index = 0; index < count; index++)
			partial = partial->lpNext;
		if (partial && partial->dwBytesRecorded)
			count++;
		done = take_done(wave, count);
	}
	const notify notify = wave->notify;
	LeaveCriticalSection(&wave->lock);
	hand_back(&notify, (HANDLE)handle, TRUE, done);
	return count && !done ? MMSYSERR_NOMEM : MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInGetPosition(HWAVEIN handle, LPMMTIME time, UINT size) { return get_position(handle, time, size, TRUE); }

EXPORT MMRESULT WINAPI waveInGetID(HWAVEIN handle, LPUINT id)
{
	if (!wave_known(handle, TRUE))
		return MMSYSERR_INVALHANDLE;
	if (!id)
		return MMSYSERR_INVALPARAM;
	*id = 0;
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInMessage(HWAVEIN handle, UINT message_id, DWORD_PTR first, DWORD_PTR second)
{
	return message((UINT_PTR)handle, message_id, first, second, TRUE);
}

EXPORT MMRESULT WINAPI waveInGetErrorTextW(MMRESULT error, LPWSTR text, UINT size) { return error_text(error, text, size); }
EXPORT MMRESULT WINAPI waveInGetErrorTextA(MMRESULT error, LPSTR text, UINT size) { return error_text_ascii(error, text, size); }
