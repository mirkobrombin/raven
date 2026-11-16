#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <stddef.h>

#ifndef WAVE_FORMAT_IEEE_FLOAT
#define WAVE_FORMAT_IEEE_FLOAT 3
#endif

#include "../../native/nt/raven_audio.h"

/* The host sound engine as programs and Raven's own audio components meet it, against the device that plays to nowhere in real time
 * (RAVEN_AUDIO=null), so the pace is the format's on any machine:
 * - ravenhost.dll's RavenAudio* primitives: the shared page's layout, frames written into the ring by the guest alone and taken at the
 *   format's pace, an event once a period, stop and reset, a recording that fills the ring with silence and counts what found it full. */

_Static_assert(offsetof(RavenAudioShared, write_frames) == 0 && offsetof(RavenAudioShared, read_frames) == 8 &&
	offsetof(RavenAudioShared, position_frames) == 16 && offsetof(RavenAudioShared, glitch_frames) == 32 &&
	offsetof(RavenAudioShared, running) == 40 && offsetof(RavenAudioShared, flags) == 76 && sizeof(RavenAudioShared) == 80,
	"the shared audio page has one layout for both widths");

/* No C runtime is linked; zeroing a structure still asks for this. */
void *memset(void *target, int value, size_t size)
{
	volatile unsigned char *bytes = target;
	while (size--)
		*bytes++ = (unsigned char)value;
	return target;
}

static uint64_t load(uint64_t *field) { return __atomic_load_n(field, __ATOMIC_ACQUIRE); }
static void store(uint64_t *field, uint64_t value) { __atomic_store_n(field, value, __ATOMIC_RELEASE); }

static WAVEFORMATEX format_of(WORD tag, DWORD rate, WORD channels, WORD bits)
{
	WAVEFORMATEX format = {0};
	format.wFormatTag = tag;
	format.nChannels = channels;
	format.nSamplesPerSec = rate;
	format.wBitsPerSample = bits;
	format.nBlockAlign = (WORD)(channels * bits / 8);
	format.nAvgBytesPerSec = rate * format.nBlockAlign;
	return format;
}

typedef HANDLE (WINAPI *open_function)(const WAVEFORMATEX *, DWORD, DWORD, DWORD, HANDLE, RavenAudioShared **);
typedef BOOL (WINAPI *handle_function)(HANDLE);
static open_function audio_open;
static handle_function audio_start, audio_stop, audio_reset, audio_close;

static int render(void)
{
	const HMODULE host = LoadLibraryA("ravenhost.dll");
	if (!host) return 40;
	audio_open = (open_function)GetProcAddress(host, "RavenAudioOpen");
	audio_start = (handle_function)GetProcAddress(host, "RavenAudioStart");
	audio_stop = (handle_function)GetProcAddress(host, "RavenAudioStop");
	audio_reset = (handle_function)GetProcAddress(host, "RavenAudioReset");
	audio_close = (handle_function)GetProcAddress(host, "RavenAudioClose");
	if (!audio_open || !audio_start || !audio_stop || !audio_reset || !audio_close) return 41;

	const HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	const WAVEFORMATEX format = format_of(WAVE_FORMAT_IEEE_FLOAT, 48000, 2, 32);
	RavenAudioShared *shared = NULL;
	const HANDLE stream = audio_open(&format, 0, 4800, 480, event, &shared);
	if (!stream || !shared) return 42;
	if (shared->buffer_frames != 4800 || shared->frame_bytes != 8 || shared->rate != 48000 || shared->channels != 2 || shared->bits != 32 ||
		shared->format_tag != 3 || shared->period_frames != 480 || shared->flags != 0 || shared->running || shared->volume != 0x3f800000 ||
		load(&shared->write_frames) || load(&shared->read_frames) || load(&shared->position_frames)) return 43;

	/* Half the ring, written by the guest alone, is taken at the format's pace once the stream runs. */
	float *ring = (float *)((BYTE *)shared + RAVEN_AUDIO_DATA_OFFSET);
	for (int frame = 0; frame < 2400; ++frame)
		ring[frame * 2] = ring[frame * 2 + 1] = 0.25f;
	store(&shared->write_frames, 2400);
	if (WaitForSingleObject(event, 50) != WAIT_TIMEOUT) return 44;
	const DWORD start = GetTickCount();
	if (!audio_start(stream) || !shared->running) return 45;
	int periods = 0;
	while (load(&shared->read_frames) < 2400) {
		if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 46;
		++periods;
	}
	const DWORD took = GetTickCount() - start;
	if (periods < 3 || took < 30 || took > 1000) return 47;
	while (load(&shared->position_frames) < 2400)
		if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 48;
	if (load(&shared->position_frames) != 2400 || load(&shared->read_frames) != 2400 || !shared->position_counter) return 49;
	/* With the ring empty the device goes on asking: the events keep coming and the frames it missed are counted. */
	if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0 || WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 50;
	if (!load(&shared->glitch_frames)) return 51;

	/* Written again after running dry, frames are played at the format's pace: the time the device went without counts for nothing. */
	Sleep(150);
	const uint64_t dry = load(&shared->position_frames);
	for (int frame = 2400; frame < 4800; ++frame)
		ring[frame * 2] = ring[frame * 2 + 1] = 0.25f;
	store(&shared->write_frames, 4800);
	const DWORD resumed = GetTickCount();
	Sleep(15);
	/* 48 frames a millisecond, with 22 ms of margin over the time that really passed. */
	if (load(&shared->position_frames) - dry >= (uint64_t)(GetTickCount() - resumed + 22) * 48) return 70;
	while (load(&shared->position_frames) < 4800)
		if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 71;
	if (GetTickCount() - resumed < 35) return 72;

	/* Stopped, nothing more is taken. */
	if (!audio_stop(stream) || shared->running) return 52;
	store(&shared->write_frames, 4800 + 480);
	Sleep(60);
	if (load(&shared->read_frames) != 4800 || load(&shared->position_frames) != 4800) return 53;
	if (!audio_reset(stream) || load(&shared->write_frames) || load(&shared->read_frames) || load(&shared->position_frames) ||
		load(&shared->glitch_frames)) return 54;
	if (!audio_close(stream)) return 55;
	SetLastError(0);
	if (audio_close(stream) || GetLastError() != ERROR_INVALID_HANDLE) return 56;
	CloseHandle(event);
	return 0;
}

static int capture(void)
{
	const HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	const WAVEFORMATEX format = format_of(WAVE_FORMAT_PCM, 8000, 1, 16);
	RavenAudioShared *shared = NULL;
	/* Zero sizes ask for a tenth of a second of ring and a hundredth of period. */
	const HANDLE stream = audio_open(&format, RAVEN_AUDIO_CAPTURE, 0, 0, event, &shared);
	if (!stream || !shared) return 60;
	if (shared->buffer_frames != 800 || shared->period_frames != 80 || shared->flags != RAVEN_AUDIO_CAPTURE) return 61;
	short *ring = (short *)((BYTE *)shared + RAVEN_AUDIO_DATA_OFFSET);
	for (int frame = 0; frame < 800; ++frame)
		ring[frame] = 0x1234;
	if (!audio_start(stream)) return 62;
	if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 63;
	const uint64_t got = load(&shared->write_frames);
	if (!got || got > 800 || ring[0] != 0 || load(&shared->position_frames) < got) return 64;
	store(&shared->read_frames, got);
	/* Unread, the ring fills up and the frames that found it full are counted, never written over what waits. */
	Sleep(250);
	const uint64_t waiting = load(&shared->write_frames) - load(&shared->read_frames);
	if (waiting != 800 || !load(&shared->glitch_frames)) return 65;
	if (!audio_close(stream)) return 66;
	CloseHandle(event);

	/* What the primitives refuse: a handle that is no event, unknown flags, a period longer than the ring. */
	const HANDLE mutex = CreateMutexW(NULL, FALSE, NULL);
	SetLastError(0);
	if (audio_open(&format, 0, 0, 0, mutex, &shared) || GetLastError() != ERROR_INVALID_HANDLE) return 67;
	if (audio_open(&format, 0x100, 0, 0, NULL, &shared) || GetLastError() != ERROR_INVALID_PARAMETER) return 68;
	if (audio_open(&format, 0, 100, 200, NULL, &shared)) return 69;
	CloseHandle(mutex);

	/* WASAPI's formats are extensible: the subformat GUID, after cbSize, the valid bits and the channel mask, names the encoding. */
	static const GUID subtype_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
	WAVEFORMATEXTENSIBLE extensible = {0};
	extensible.Format = format_of(WAVE_FORMAT_EXTENSIBLE, 48000, 2, 32);
	extensible.Format.cbSize = sizeof(extensible) - sizeof(extensible.Format);
	extensible.Samples.wValidBitsPerSample = 32;
	extensible.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	extensible.SubFormat = subtype_float;
	const HANDLE floating = audio_open(&extensible.Format, 0, 0, 0, NULL, &shared);
	if (!floating || shared->format_tag != WAVE_FORMAT_IEEE_FLOAT || shared->bits != 32 || shared->frame_bytes != 8) return 88;
	if (!audio_close(floating)) return 88;
	extensible.SubFormat.Data1 = 0x55;
	SetLastError(0);
	if (audio_open(&extensible.Format, 0, 0, 0, NULL, &shared) || GetLastError() != ERROR_INVALID_PARAMETER) return 89;
	return 0;
}

static int mapped(const void *address)
{
	MEMORY_BASIC_INFORMATION info;
	return VirtualQuery(address, &info, sizeof info) == sizeof info && info.State == MEM_COMMIT;
}

static int lifetimes(void)
{
	/* The page stays while any handle to the stream does, however the handles are closed; a page whose last handle CloseHandle closed
	 * goes by the time the thread has waited. */
	const WAVEFORMATEX format = format_of(WAVE_FORMAT_PCM, 8000, 1, 16);
	RavenAudioShared *shared = NULL;
	HANDLE stream = audio_open(&format, 0, 0, 0, NULL, &shared), copy = NULL;
	if (!stream || !mapped(shared)) return 73;
	if (!CloseHandle(stream)) return 74;
	Sleep(20);
	if (mapped(shared)) return 74;
	stream = audio_open(&format, 0, 0, 0, NULL, &shared);
	if (!stream || !DuplicateHandle(GetCurrentProcess(), stream, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) return 75;
	if (!audio_close(stream) || !mapped(shared) || shared->buffer_frames != 800) return 76;
	if (!audio_close(copy) || mapped(shared)) return 77;

	/* A default ring takes a longer period, up to the ten seconds a ring may hold. */
	SetLastError(0);
	if (audio_open(&format, 0, 0, format.nSamplesPerSec * 20, NULL, &shared) || GetLastError() != ERROR_INVALID_PARAMETER) return 78;

	/* Unsigned 8-bit silence is the midpoint. */
	const HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	const WAVEFORMATEX narrow = format_of(WAVE_FORMAT_PCM, 8000, 1, 8);
	stream = audio_open(&narrow, RAVEN_AUDIO_CAPTURE, 0, 0, event, &shared);
	if (!stream) return 79;
	BYTE *bytes = (BYTE *)shared + RAVEN_AUDIO_DATA_OFFSET;
	if (!audio_start(stream) || WaitForSingleObject(event, 1000) != WAIT_OBJECT_0 || !load(&shared->write_frames) || bytes[0] != 0x80)
		return 80;

	/* The event a stream signals can be changed or taken away while it runs; a handle that is not a stream or not an event is refused. */
	typedef BOOL (WINAPI *bind_function)(HANDLE, HANDLE);
	const bind_function bind = (bind_function)GetProcAddress(GetModuleHandleA("ravenhost.dll"), "RavenAudioSetEvent");
	const HANDLE other = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!bind || !bind(stream, other) || WaitForSingleObject(other, 1000) != WAIT_OBJECT_0) return 82;
	ResetEvent(event);
	if (WaitForSingleObject(other, 1000) != WAIT_OBJECT_0 || WaitForSingleObject(event, 0) != WAIT_TIMEOUT) return 83;
	if (!bind(stream, NULL)) return 84;
	Sleep(30);
	ResetEvent(other);
	if (WaitForSingleObject(other, 60) != WAIT_TIMEOUT) return 85;
	SetLastError(0);
	if (bind(other, event) || GetLastError() != ERROR_INVALID_HANDLE) return 86;
	SetLastError(0);
	if (bind(stream, stream) || GetLastError() != ERROR_INVALID_HANDLE) return 87;
	if (!audio_close(stream)) return 81;
	CloseHandle(other);
	CloseHandle(event);
	return 0;
}

/* The page is the program's to write. The runtime keeps its own sizes, so a stream goes on after its page is overwritten. */
static int hostile(void)
{
	const HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	const WAVEFORMATEX format = format_of(WAVE_FORMAT_PCM, 48000, 2, 16);
	RavenAudioShared *shared = NULL;
	const HANDLE stream = audio_open(&format, 0, 4800, 480, event, &shared);
	if (!stream || !shared) return 90;
	shared->buffer_frames = 0;
	shared->frame_bytes = 0x10000000;
	shared->period_frames = 0;
	store(&shared->write_frames, 0xffffffff00000000ULL);
	if (!audio_start(stream)) return 91;
	for (int period = 0; period < 3; ++period)
		if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 92;
	shared->buffer_frames = 0xffffffff;
	for (int period = 0; period < 3; ++period)
		if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 93;
	if (!audio_stop(stream) || !audio_close(stream)) return 94;
	CloseHandle(event);

	/* An extensible format is taken only with a whole audio subtype. */
	WAVEFORMATEXTENSIBLE wide = {0};
	wide.Format = format;
	wide.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	wide.Format.cbSize = 22;
	wide.Samples.wValidBitsPerSample = 16;
	wide.SubFormat = (GUID){0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
	const HANDLE good = audio_open(&wide.Format, 0, 4800, 480, NULL, &shared);
	if (!good || !audio_close(good)) return 95;
	wide.SubFormat.Data4[7] = 0;
	if (audio_open(&wide.Format, 0, 4800, 480, NULL, &shared)) return 96;
	return 0;
}

int raven_entry(void)
{
	int failed = render();
	if (!failed)
		failed = capture();
	if (!failed)
		failed = hostile();
	return failed ? failed : lifetimes();
}
