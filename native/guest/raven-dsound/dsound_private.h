/* SPDX-License-Identifier: MIT
 *
 * dsound.dll for each width: DirectSound playback, 3D sound and capture, Raven code on the public headers. Each device mixes its secondary
 * buffers in software on a thread of its own into one float stereo stream of the runtime's sound engine (raven_audio.h); capture buffers
 * read a stream of their own. Lock, Unlock, GetCurrentPosition and GetStatus never leave the guest: they read the buffers and the shared
 * page of the stream.
 */
#pragma once

#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define CONST_VTABLE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <dsound.h>
#include <stddef.h>
#include <string.h>

#include "../../nt/raven_audio.h"

/* The heap functions need the guest RTL component, which a build may not provision; LocalAlloc is always served. */
#define ALLOC(size) LocalAlloc(LPTR, (size))
#define FREE(block) LocalFree(block)

/* The device's stream: float stereo at 48 kHz, a 10 ms period, and a 100 ms ring of which the mixer keeps LEAD_PERIODS filled ahead of
 * the device. */
#define MIX_RATE 48000u
#define MIX_PERIOD (MIX_RATE / 100u)
#define MIX_RING (MIX_RATE / 10u)
#define LEAD_PERIODS 3u

/* The extensible format's subtypes; MinGW declares ksmedia.h's but leaves them to a library this component does not link. */
static const GUID subtype_pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID subtype_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/* The endpoints mmdevapi.dll reports, so a program that matches DirectSound's devices to the Core Audio ones finds them. */
static const GUID render_guid = {0x5a1d0e01, 0x7a3c, 0x4b8e, {0x9a, 0x52, 0x52, 0x41, 0x56, 0x45, 0x4e, 0x01}};
static const GUID capture_guid = {0x5a1d0e02, 0x7a3c, 0x4b8e, {0x9a, 0x52, 0x52, 0x41, 0x56, 0x45, 0x4e, 0x02}};

static inline BOOL same_guid(const GUID *first, const GUID *second) { return !memcmp(first, second, sizeof(GUID)); }

/* Every object of the component is guarded by this one lock, which the mixer and capture threads take for each period. */
extern CRITICAL_SECTION ds_lock;

/* The samples of a buffer and the format they are in, which Duplicate shares between buffers. */
typedef struct buffer_data {
	LONG ref;
	BYTE *memory;
	DWORD size;
	WAVEFORMATEXTENSIBLE format;
	/* What reading a frame needs: bytes per sample, whether samples are floats. */
	UINT sample_bytes;
	BOOL is_float;
} buffer_data;

typedef struct notify_list {
	DSBPOSITIONNOTIFY *items;
	DWORD count;
} notify_list;

typedef struct buffer {
	IDirectSoundBuffer8 IDirectSoundBuffer8_iface;
	IDirectSoundNotify IDirectSoundNotify_iface;
	IDirectSound3DBuffer IDirectSound3DBuffer_iface;
	IDirectSound3DListener IDirectSound3DListener_iface;
	LONG ref;
	struct device *device;
	struct buffer *next;
	BOOL primary, is8;
	DWORD flags;
	buffer_data *data;
	DWORD frequency;
	LONG volume, pan;
	/* Linear gains of volume and pan, and of the 3D position once the listener is applied. */
	float gain_left, gain_right, gain_3d_left, gain_3d_right;
	BOOL playing, looping;
	/* The next frame to mix, in 32.32 fixed point; the device frame at which the buffer began playing from start_cursor, which places the play
	 * cursor while that frame has not yet sounded. */
	UINT64 cursor;
	UINT64 start_device;
	UINT64 start_cursor;
	/* A buffer that is not looping and has had its last frame mixed plays until the device reaches end_device. */
	BOOL ending;
	UINT64 end_device;
	/* Where the play cursor was when notifications were last checked, in bytes, and the device's position then. */
	DWORD notified;
	UINT64 notified_device;
	notify_list notify;
	DS3DBUFFER params, pending;
} buffer;

typedef struct device {
	IDirectSound8 IDirectSound8_iface;
	LONG ref;
	BOOL is8, initialized;
	DWORD level, speaker;
	buffer *buffers;
	/* The primary buffer exists once a program creates it and is freed with the device. */
	buffer primary;
	BOOL primary_made;
	WAVEFORMATEX primary_format;
	DS3DLISTENER listener, listener_pending;
	BOOL listener_dirty;
	HANDLE stream, period_event, quit_event, thread;
	RavenAudioShared *shared;
	BOOL running;
	/* The device frames mixed into the ring since the stream was last reset: the ring's write counter. */
	UINT64 mixed;
} device;

/* The device's clock: the frames the engine has taken from the ring, which is where Windows puts the play cursor, at the mixer that
 * reads ahead of the speaker, rather than what has been heard, which a sound server's latency holds back. */
static inline UINT64 device_clock(const device *device) { return device->running ? load64(&device->shared->read_frames) : 0; }

typedef struct capture_buffer {
	IDirectSoundCaptureBuffer8 IDirectSoundCaptureBuffer8_iface;
	IDirectSoundNotify IDirectSoundNotify_iface;
	LONG ref;
	struct capture *capture;
	DWORD flags;
	buffer_data *data;
	HANDLE stream, period_event, quit_event, thread;
	RavenAudioShared *shared;
	BOOL capturing, looping;
	/* Bytes captured into the buffer, where the next frame goes. */
	DWORD position;
	notify_list notify;
} capture_buffer;

typedef struct capture {
	IDirectSoundCapture IDirectSoundCapture_iface;
	LONG ref;
	BOOL initialized;
	capture_buffer *buffer;
} capture;

/* device.c */
HRESULT device_create(REFIID riid, void **out, BOOL is8);
HRESULT device_initialize(device *device, const GUID *guid);
HRESULT capture_create(REFIID riid, void **out);
HRESULT capture_initialize(capture *capture, const GUID *guid);
float gain_of(LONG hundredths);
float root_of(float value);

/* buffer.c */
HRESULT buffer_create(device *device, const DSBUFFERDESC *desc, buffer **out);
HRESULT buffer_duplicate(device *device, buffer *original, buffer **out);
void buffer_init_primary(device *device);
void buffer_free(buffer *buffer);
void buffer_reset_primary(buffer *buffer);
buffer *buffer_from(IDirectSoundBuffer *iface);
void buffer_update_3d(buffer *buffer);
HRESULT format_check(const WAVEFORMATEX *format, buffer_data *data);
HRESULT format_copy(const WAVEFORMATEX *format, WAVEFORMATEX *out, DWORD size, DWORD *written);
HRESULT notify_set(notify_list *list, DWORD size, DWORD count, const DSBPOSITIONNOTIFY *items);
void notify_range(const notify_list *list, DWORD from, DWORD to, DWORD size);
void notify_stop(const notify_list *list);
HRESULT lock_range(buffer_data *data, DWORD offset, DWORD bytes, void **first, DWORD *first_bytes, void **second, DWORD *second_bytes);
HRESULT unlock_range(buffer_data *data, void *first, DWORD first_bytes, void *second, DWORD second_bytes);

/* mixer.c */
HRESULT mixer_open(device *device);
void mixer_close(device *device);
void mixer_kick(device *device);
void buffer_cursors(buffer *buffer, DWORD *play, DWORD *write);
void buffer_anchor(buffer *buffer);

/* capture.c */
void capture_buffer_free(capture_buffer *buffer);
HRESULT capture_buffer_create(capture *capture, const DSCBUFFERDESC *desc, capture_buffer **out);
