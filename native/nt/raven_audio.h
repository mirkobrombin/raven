#pragma once

/*
 * The control page of an audio stream that the runtime and a guest component (mmdevapi, dsound, winmm) share without calling each other.
 * RavenAudioOpen in ravenhost.dll maps it into the guest; the samples follow it at RAVEN_AUDIO_DATA_OFFSET as a ring of buffer_frames
 * frames. A frame's place in the ring is its counter modulo buffer_frames.
 *
 * Render: the guest writes frames at write_frames and then advances it; the runtime consumes from read_frames and advances that.
 * Capture: the runtime writes and advances write_frames; the guest reads and advances read_frames.
 * The counters only grow until RavenAudioReset sets them to zero. They are 64-bit and aligned, read and written with atomic operations
 * (acquire loads, release stores); a 32-bit guest uses its 8-byte atomic instructions on them.
 *
 * position_frames is what the device has played (render) or captured (capture) since the last reset, refreshed at least every period;
 * position_counter is the performance counter (QueryPerformanceCounter, 100 ns units) when it was refreshed.
 */

#include <stdint.h>

#define RAVEN_AUDIO_CAPTURE 0x1u
#define RAVEN_AUDIO_DATA_OFFSET 4096u

typedef struct RavenAudioShared {
	uint64_t write_frames;
	uint64_t read_frames;
	uint64_t position_frames;
	uint64_t position_counter;
	/* Frames the device wanted while the ring had none (render) or that found the ring full and were dropped (capture). */
	uint64_t glitch_frames;
	uint32_t running;
	/* The stream volume as the bits of an IEEE float, 1.0 when untouched; the runtime scales 16-bit and float samples with it. */
	uint32_t volume;
	uint32_t buffer_frames;
	uint32_t frame_bytes;
	uint32_t rate;
	uint32_t channels;
	uint32_t bits;
	/* 1 for integer PCM, 3 for IEEE float, as the WAVE_FORMAT tags. */
	uint32_t format_tag;
	uint32_t period_frames;
	uint32_t flags;
} RavenAudioShared;

#ifdef __cplusplus
static_assert(sizeof(RavenAudioShared) == 80 && alignof(RavenAudioShared) == 8, "the shared audio page has one layout for every guest");
#endif

#ifdef _WIN32
/* ravenhost.dll's calls and the counters' atomic access, for the guest components. */
__declspec(dllimport) HANDLE WINAPI RavenAudioOpen(const WAVEFORMATEX *format, DWORD flags, DWORD buffer_frames, DWORD period_frames,
	HANDLE event, RavenAudioShared **shared);
__declspec(dllimport) BOOL WINAPI RavenAudioSetEvent(HANDLE stream, HANDLE event);
__declspec(dllimport) BOOL WINAPI RavenAudioStart(HANDLE stream);
__declspec(dllimport) BOOL WINAPI RavenAudioStop(HANDLE stream);
__declspec(dllimport) BOOL WINAPI RavenAudioReset(HANDLE stream);
__declspec(dllimport) BOOL WINAPI RavenAudioClose(HANDLE stream);

static inline UINT64 load64(const UINT64 *field) { return __atomic_load_n(field, __ATOMIC_ACQUIRE); }
static inline void store64(UINT64 *field, UINT64 value) { __atomic_store_n(field, value, __ATOMIC_RELEASE); }

/* What the engine plays: integer PCM of 8, 16, 24 or 32 bits or 32-bit float, plain or extensible, 1 to 8 channels, whole frames. */
static inline BOOL format_valid(const WAVEFORMATEX *format)
{
	static const GUID subtype_base = {0x00000000, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
	WORD tag = format->wFormatTag;
	if (tag == WAVE_FORMAT_EXTENSIBLE) {
		if (format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			return FALSE;
		const WAVEFORMATEXTENSIBLE *extensible = (const WAVEFORMATEXTENSIBLE *)format;
		GUID sub = extensible->SubFormat;
		const DWORD data1 = sub.Data1;
		sub.Data1 = 0;
		if (memcmp(&sub, &subtype_base, sizeof sub) || (data1 != WAVE_FORMAT_PCM && data1 != WAVE_FORMAT_IEEE_FLOAT))
			return FALSE;
		if (extensible->Samples.wValidBitsPerSample > format->wBitsPerSample)
			return FALSE;
		tag = (WORD)data1;
	}
	const WORD bits = format->wBitsPerSample;
	if (tag == WAVE_FORMAT_PCM) {
		if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
			return FALSE;
	} else if (tag != WAVE_FORMAT_IEEE_FLOAT || bits != 32) {
		return FALSE;
	}
	return format->nChannels >= 1 && format->nChannels <= 8 && format->nSamplesPerSec >= 1000 && format->nSamplesPerSec <= 384000 &&
		format->nBlockAlign == format->nChannels * bits / 8;
}
#endif
