/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven audio endpoint component: mmdevapi.dll, the device enumerator programs find the speakers through and the audio clients
 * (WASAPI) they play sound with.
 *
 * There is one output device, the default one, and no input device while the runtime has no wave input. A stream plays through the
 * runtime's wave output, which takes any PCM or floating point format, so every such format is supported in shared mode and none is
 * converted here. A stream hands each buffer the program releases to the wave output at once, paused until the program starts the
 * stream; the padding is what was released and not played yet, and the clock stands where the wave output says the speaker is. The
 * volumes of the session, the stream and the endpoint scale the samples as they are handed over.
 * Exclusive mode is refused, as on a device whose owner does not allow it, and notifications are accepted but never raised. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <propsys.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stddef.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))
#define OWNER(type, member, iface) ((type *)((char *)(iface) - offsetof(type, member)))

/* The results of the audio client (AUDCLNT_E_*), which mingw spells through macros of its own. */
#define E_NOT_INITIALIZED ((HRESULT)0x88890001L)
#define E_ALREADY_INITIALIZED ((HRESULT)0x88890002L)
#define E_WRONG_ENDPOINT_TYPE ((HRESULT)0x88890003L)
#define E_BUFFER_TOO_LARGE ((HRESULT)0x88890006L)
#define E_OUT_OF_ORDER ((HRESULT)0x88890007L)
#define E_UNSUPPORTED_FORMAT ((HRESULT)0x88890008L)
#define E_INVALID_SIZE ((HRESULT)0x88890009L)
#define E_NOT_STOPPED ((HRESULT)0x88890005L)
#define E_BUFFER_PENDING ((HRESULT)0x8889000BL)
#define E_EVENTHANDLE_NOT_EXPECTED ((HRESULT)0x88890011L)
#define E_EXCLUSIVE_NOT_ALLOWED ((HRESULT)0x8889000EL)
#define E_EVENTHANDLE_NOT_SET ((HRESULT)0x88890014L)
#define E_INVALID_DEVICE_PERIOD ((HRESULT)0x88890020L)
#define E_NOT_FOUND ((HRESULT)0x80070490L)
#define STREAM_EVENTS 0x00040000
#define STREAM_LOOPBACK 0x00020000

/* The device mixes at 48 kHz, two channels of floating point samples, every 10 ms. */
#define MIX_RATE 48000
#define MIX_CHANNELS 2
#define PERIOD 100000
#define MIN_PERIOD 30000
/* The wave output keeps about a twentieth of a second ahead of the speaker. */
#define AHEAD_DIVISOR 20

DEFINE_GUID(CLSID_RavenMMDeviceEnumerator, 0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
DEFINE_GUID(SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(SUBTYPE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(ENDPOINT_GUID, 0x52415645, 0x4e00, 0x4155, 0x44, 0x49, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x01);

static const WCHAR render_id[] = L"{0.0.0.00000000}.{52415645-4e00-4155-4449-4f0000000001}";

static LONG objects;

static BOOL same(REFIID left, REFIID right)
{
	const DWORD *a = (const DWORD *)left, *b = (const DWORD *)right;
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static UINT32 text_length(const WCHAR *text)
{
	UINT32 length = 0;
	while (text[length])
		++length;
	return length;
}

static WCHAR *task_text(const WCHAR *text)
{
	const UINT32 size = (text_length(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = CoTaskMemAlloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

static HRESULT text_out(const WCHAR *text, WCHAR **out)
{
	if (!out)
		return E_POINTER;
	*out = task_text(text);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* The format the device mixes in, as the engine reports it. */
static void mix_format(WAVEFORMATEXTENSIBLE *format)
{
	memset(format, 0, sizeof(*format));
	format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	format->Format.nChannels = MIX_CHANNELS;
	format->Format.nSamplesPerSec = MIX_RATE;
	format->Format.wBitsPerSample = 32;
	format->Format.nBlockAlign = MIX_CHANNELS * 4;
	format->Format.nAvgBytesPerSec = MIX_RATE * MIX_CHANNELS * 4;
	format->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	format->Samples.wValidBitsPerSample = 32;
	format->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	format->SubFormat = SUBTYPE_FLOAT;
}

/* The kinds of samples a stream carries. */
enum { SAMPLE_U8, SAMPLE_S16, SAMPLE_S24, SAMPLE_S32, SAMPLE_FLOAT };

/* The kind of samples of a PCM or floating point format the wave output plays, or -1 for any other format. */
static int sample_kind(const WAVEFORMATEX *format)
{
	if (!format)
		return -1;
	WORD tag = format->wFormatTag;
	if (tag == WAVE_FORMAT_EXTENSIBLE) {
		if (format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			return -1;
		const WAVEFORMATEXTENSIBLE *extended = (const WAVEFORMATEXTENSIBLE *)format;
		if (same(&extended->SubFormat, &SUBTYPE_PCM))
			tag = WAVE_FORMAT_PCM;
		else if (same(&extended->SubFormat, &SUBTYPE_FLOAT))
			tag = WAVE_FORMAT_IEEE_FLOAT;
		else
			return -1;
	}
	const WORD bits = format->wBitsPerSample;
	if (!format->nChannels || format->nChannels > 8 || format->nSamplesPerSec < 1000 || format->nSamplesPerSec > 384000 ||
		format->nBlockAlign != format->nChannels * (bits / 8))
		return -1;
	if (tag == WAVE_FORMAT_IEEE_FLOAT)
		return bits == 32 ? SAMPLE_FLOAT : -1;
	if (tag != WAVE_FORMAT_PCM)
		return -1;
	return bits == 8 ? SAMPLE_U8 : bits == 16 ? SAMPLE_S16 : bits == 24 ? SAMPLE_S24 : bits == 32 ? SAMPLE_S32 : -1;
}

static UINT32 format_size(const WAVEFORMATEX *format)
{
	/* What the tag has, not what cbSize claims, which a program may leave wild. */
	return format->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX);
}

/* ---- The volumes: the session's and the endpoint's are shared by every stream of the process. ---- */

static float session_level = 1.0f, endpoint_level = 1.0f;
static BOOL session_muted, endpoint_muted;

/* ---- Property store of the output device. ---- */

static const PROPERTYKEY *const device_keys[] = {&PKEY_Device_FriendlyName, &PKEY_Device_DeviceDesc, &PKEY_DeviceInterface_FriendlyName,
	&PKEY_AudioEndpoint_FormFactor, &PKEY_AudioEndpoint_PhysicalSpeakers, &PKEY_AudioEndpoint_GUID, &PKEY_AudioEngine_DeviceFormat};

static BOOL same_key(const PROPERTYKEY *left, const PROPERTYKEY *right)
{
	return same(&left->fmtid, &right->fmtid) && left->pid == right->pid;
}

static HRESULT WINAPI store_query(IPropertyStore *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IPropertyStore)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI static_reference(void *iface) { return 2; }
static ULONG WINAPI static_release(void *iface) { return 1; }

static HRESULT WINAPI store_count(IPropertyStore *iface, DWORD *count)
{
	if (!count)
		return E_POINTER;
	*count = sizeof(device_keys) / sizeof(device_keys[0]);
	return S_OK;
}

static HRESULT WINAPI store_key(IPropertyStore *iface, DWORD index, PROPERTYKEY *key)
{
	if (!key)
		return E_POINTER;
	if (index >= sizeof(device_keys) / sizeof(device_keys[0]))
		return E_INVALIDARG;
	*key = *device_keys[index];
	return S_OK;
}

static HRESULT WINAPI store_value(IPropertyStore *iface, REFPROPERTYKEY key, PROPVARIANT *value)
{
	if (!key || !value)
		return E_POINTER;
	memset(value, 0, sizeof(*value));
	const WCHAR *text = NULL;
	if (same_key(key, &PKEY_Device_FriendlyName))
		text = L"Speakers (Raven)";
	else if (same_key(key, &PKEY_Device_DeviceDesc))
		text = L"Speakers";
	else if (same_key(key, &PKEY_DeviceInterface_FriendlyName))
		text = L"Raven";
	else if (same_key(key, &PKEY_AudioEndpoint_GUID))
		text = render_id + 17;
	if (text) {
		value->vt = VT_LPWSTR;
		value->pwszVal = task_text(text);
		return value->pwszVal ? S_OK : E_OUTOFMEMORY;
	}
	if (same_key(key, &PKEY_AudioEndpoint_FormFactor)) {
		value->vt = VT_UI4;
		value->ulVal = Speakers;
	} else if (same_key(key, &PKEY_AudioEndpoint_PhysicalSpeakers)) {
		value->vt = VT_UI4;
		value->ulVal = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	} else if (same_key(key, &PKEY_AudioEngine_DeviceFormat)) {
		WAVEFORMATEXTENSIBLE *format = CoTaskMemAlloc(sizeof(*format));
		if (!format)
			return E_OUTOFMEMORY;
		mix_format(format);
		value->vt = VT_BLOB;
		value->blob.cbSize = sizeof(*format);
		value->blob.pBlobData = (BYTE *)format;
	}
	return S_OK;
}

static HRESULT WINAPI store_set(IPropertyStore *iface, REFPROPERTYKEY key, REFPROPVARIANT value) { return STG_E_ACCESSDENIED; }
static HRESULT WINAPI store_commit(IPropertyStore *iface) { return STG_E_ACCESSDENIED; }

static IPropertyStoreVtbl store_table = {store_query, (void *)static_reference, (void *)static_release, store_count, store_key, store_value,
	store_set, store_commit};
static IPropertyStore device_store = {&store_table};

/* ---- The session every stream of the process plays in, and its volume. ---- */

static HRESULT WINAPI session_query(IAudioSessionControl2 *iface, REFIID iid, void **out);
static HRESULT WINAPI simple_query(ISimpleAudioVolume *iface, REFIID iid, void **out);

static LONG streams_started;

static HRESULT WINAPI session_state(IAudioSessionControl2 *iface, AudioSessionState *state)
{
	if (!state)
		return E_POINTER;
	*state = streams_started ? AudioSessionStateActive : AudioSessionStateInactive;
	return S_OK;
}

static HRESULT WINAPI session_name(IAudioSessionControl2 *iface, LPWSTR *name) { return text_out(L"", name); }
static HRESULT WINAPI session_set_name(IAudioSessionControl2 *iface, LPCWSTR name, LPCGUID context) { return S_OK; }
static HRESULT WINAPI session_icon(IAudioSessionControl2 *iface, LPWSTR *path) { return text_out(L"", path); }
static HRESULT WINAPI session_set_icon(IAudioSessionControl2 *iface, LPCWSTR path, LPCGUID context) { return S_OK; }

static HRESULT WINAPI session_grouping(IAudioSessionControl2 *iface, GUID *grouping)
{
	if (!grouping)
		return E_POINTER;
	memset(grouping, 0, sizeof(*grouping));
	return S_OK;
}

static HRESULT WINAPI session_set_grouping(IAudioSessionControl2 *iface, LPCGUID grouping, LPCGUID context) { return S_OK; }

static HRESULT WINAPI session_events(IAudioSessionControl2 *iface, IAudioSessionEvents *events)
{
	return events ? S_OK : E_POINTER;
}

static HRESULT WINAPI session_identifier(IAudioSessionControl2 *iface, LPWSTR *identifier)
{
	return text_out(render_id, identifier);
}

static HRESULT WINAPI session_process(IAudioSessionControl2 *iface, DWORD *process)
{
	if (!process)
		return E_POINTER;
	*process = GetCurrentProcessId();
	return S_OK;
}

static HRESULT WINAPI session_system(IAudioSessionControl2 *iface) { return S_FALSE; }
static HRESULT WINAPI session_ducking(IAudioSessionControl2 *iface, BOOL optout) { return S_OK; }

static IAudioSessionControl2Vtbl session_table = {session_query, (void *)static_reference, (void *)static_release, session_state,
	session_name, session_set_name, session_icon, session_set_icon, session_grouping, session_set_grouping, session_events, session_events,
	session_identifier, session_identifier, session_process, session_system, session_ducking};
static IAudioSessionControl2 session = {&session_table};

static HRESULT WINAPI session_query(IAudioSessionControl2 *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IAudioSessionControl) || same(iid, &IID_IAudioSessionControl2)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT WINAPI simple_set(ISimpleAudioVolume *iface, float level, LPCGUID context)
{
	if (!(level >= 0.0f && level <= 1.0f))
		return E_INVALIDARG;
	session_level = level;
	return S_OK;
}

static HRESULT WINAPI simple_get(ISimpleAudioVolume *iface, float *level)
{
	if (!level)
		return E_POINTER;
	*level = session_level;
	return S_OK;
}

static HRESULT WINAPI simple_set_mute(ISimpleAudioVolume *iface, BOOL mute, LPCGUID context)
{
	session_muted = mute;
	return S_OK;
}

static HRESULT WINAPI simple_get_mute(ISimpleAudioVolume *iface, BOOL *mute)
{
	if (!mute)
		return E_POINTER;
	*mute = session_muted;
	return S_OK;
}

static ISimpleAudioVolumeVtbl simple_table = {simple_query, (void *)static_reference, (void *)static_release, simple_set, simple_get,
	simple_set_mute, simple_get_mute};
static ISimpleAudioVolume simple_volume = {&simple_table};

static HRESULT WINAPI simple_query(ISimpleAudioVolume *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_ISimpleAudioVolume)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT WINAPI manager_query(IAudioSessionManager2 *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IAudioSessionManager) || same(iid, &IID_IAudioSessionManager2)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT WINAPI manager_control(IAudioSessionManager2 *iface, LPCGUID guid, DWORD flags, IAudioSessionControl **out)
{
	if (!out)
		return E_POINTER;
	*out = (IAudioSessionControl *)&session;
	return S_OK;
}

static HRESULT WINAPI manager_volume(IAudioSessionManager2 *iface, LPCGUID guid, DWORD flags, ISimpleAudioVolume **out)
{
	if (!out)
		return E_POINTER;
	*out = &simple_volume;
	return S_OK;
}

static HRESULT WINAPI manager_sessions(IAudioSessionManager2 *iface, IAudioSessionEnumerator **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI manager_notification(IAudioSessionManager2 *iface, void *notification)
{
	return notification ? S_OK : E_POINTER;
}

static HRESULT WINAPI manager_duck(IAudioSessionManager2 *iface, LPCWSTR session, IAudioVolumeDuckNotification *notification)
{
	return notification ? S_OK : E_POINTER;
}

static HRESULT WINAPI manager_unduck(IAudioSessionManager2 *iface, IAudioVolumeDuckNotification *notification)
{
	return notification ? S_OK : E_POINTER;
}

static IAudioSessionManager2Vtbl manager_table = {manager_query, (void *)static_reference, (void *)static_release, manager_control,
	manager_volume, manager_sessions, (void *)manager_notification, (void *)manager_notification, manager_duck, manager_unduck};
static IAudioSessionManager2 session_manager = {&manager_table};

/* ---- The endpoint's master volume, in decibels from -96 to 0 or as a scalar. ---- */

static float decibels(float scalar)
{
	/* 20 log10, from the binary exponent and a short series of the mantissa's logarithm, close enough for a volume slider. */
	if (scalar <= 0.0f)
		return -96.0f;
	int exponent = 0;
	while (scalar < 0.5f) {
		scalar *= 2.0f;
		--exponent;
	}
	while (scalar >= 1.0f) {
		scalar *= 0.5f;
		++exponent;
	}
	const float y = (scalar - 1.0f) / (scalar + 1.0f), y2 = y * y;
	const float natural = 2.0f * y * (1.0f + y2 / 3.0f + y2 * y2 / 5.0f + y2 * y2 * y2 / 7.0f) + exponent * 0.69314718f;
	const float result = natural * 8.68588964f;
	return result < -96.0f ? -96.0f : result;
}

static float scalar_of(float level)
{
	/* 10^(dB/20) = e^(dB * ln10 / 20), by halving the exponent until a series converges and squaring back. */
	if (level <= -96.0f)
		return 0.0f;
	float x = level * 0.11512925f;
	int halvings = 0;
	while (x < -0.5f) {
		x *= 0.5f;
		++halvings;
	}
	float result = 1.0f + x + x * x / 2.0f + x * x * x / 6.0f + x * x * x * x / 24.0f;
	while (halvings--)
		result *= result;
	return result > 1.0f ? 1.0f : result;
}

static HRESULT WINAPI endpoint_query(IAudioEndpointVolume *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IAudioEndpointVolume)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT WINAPI endpoint_notify(IAudioEndpointVolume *iface, IAudioEndpointVolumeCallback *callback)
{
	return callback ? S_OK : E_POINTER;
}

static HRESULT WINAPI endpoint_channels(IAudioEndpointVolume *iface, UINT *count)
{
	if (!count)
		return E_POINTER;
	*count = MIX_CHANNELS;
	return S_OK;
}

static HRESULT WINAPI endpoint_set_level(IAudioEndpointVolume *iface, float level, LPCGUID context)
{
	if (!(level >= -96.0f && level <= 0.0f))
		return E_INVALIDARG;
	endpoint_level = scalar_of(level);
	return S_OK;
}

static HRESULT WINAPI endpoint_set_scalar(IAudioEndpointVolume *iface, float level, LPCGUID context)
{
	if (!(level >= 0.0f && level <= 1.0f))
		return E_INVALIDARG;
	endpoint_level = level;
	return S_OK;
}

static HRESULT WINAPI endpoint_level_db(IAudioEndpointVolume *iface, float *level)
{
	if (!level)
		return E_POINTER;
	*level = decibels(endpoint_level);
	return S_OK;
}

static HRESULT WINAPI endpoint_scalar(IAudioEndpointVolume *iface, float *level)
{
	if (!level)
		return E_POINTER;
	*level = endpoint_level;
	return S_OK;
}

static HRESULT WINAPI endpoint_set_channel_level(IAudioEndpointVolume *iface, UINT channel, float level, LPCGUID context)
{
	return channel < MIX_CHANNELS ? endpoint_set_level(iface, level, context) : E_INVALIDARG;
}

static HRESULT WINAPI endpoint_set_channel_scalar(IAudioEndpointVolume *iface, UINT channel, float level, LPCGUID context)
{
	return channel < MIX_CHANNELS ? endpoint_set_scalar(iface, level, context) : E_INVALIDARG;
}

static HRESULT WINAPI endpoint_channel_level(IAudioEndpointVolume *iface, UINT channel, float *level)
{
	return channel < MIX_CHANNELS ? endpoint_level_db(iface, level) : E_INVALIDARG;
}

static HRESULT WINAPI endpoint_channel_scalar(IAudioEndpointVolume *iface, UINT channel, float *level)
{
	return channel < MIX_CHANNELS ? endpoint_scalar(iface, level) : E_INVALIDARG;
}

static HRESULT WINAPI endpoint_set_mute(IAudioEndpointVolume *iface, BOOL mute, LPCGUID context)
{
	endpoint_muted = mute;
	return S_OK;
}

static HRESULT WINAPI endpoint_mute(IAudioEndpointVolume *iface, BOOL *mute)
{
	if (!mute)
		return E_POINTER;
	*mute = endpoint_muted;
	return S_OK;
}

/* The slider moves in steps of a fiftieth. */
static HRESULT WINAPI endpoint_steps(IAudioEndpointVolume *iface, UINT *step, UINT *count)
{
	if (!step || !count)
		return E_POINTER;
	*count = 51;
	*step = (UINT)(endpoint_level * 50.0f + 0.5f);
	return S_OK;
}

static HRESULT WINAPI endpoint_up(IAudioEndpointVolume *iface, LPCGUID context)
{
	endpoint_level = endpoint_level + 0.02f > 1.0f ? 1.0f : endpoint_level + 0.02f;
	return S_OK;
}

static HRESULT WINAPI endpoint_down(IAudioEndpointVolume *iface, LPCGUID context)
{
	endpoint_level = endpoint_level - 0.02f < 0.0f ? 0.0f : endpoint_level - 0.02f;
	return S_OK;
}

static HRESULT WINAPI endpoint_support(IAudioEndpointVolume *iface, DWORD *mask)
{
	if (!mask)
		return E_POINTER;
	*mask = 0;
	return S_OK;
}

static HRESULT WINAPI endpoint_range(IAudioEndpointVolume *iface, float *minimum, float *maximum, float *increment)
{
	if (!minimum || !maximum || !increment)
		return E_POINTER;
	*minimum = -96.0f;
	*maximum = 0.0f;
	*increment = 0.375f;
	return S_OK;
}

static IAudioEndpointVolumeVtbl endpoint_table = {endpoint_query, (void *)static_reference, (void *)static_release, endpoint_notify,
	endpoint_notify, endpoint_channels, endpoint_set_level, endpoint_set_scalar, endpoint_level_db, endpoint_scalar,
	endpoint_set_channel_level, endpoint_set_channel_scalar, endpoint_channel_level, endpoint_channel_scalar, endpoint_set_mute,
	endpoint_mute, endpoint_steps, endpoint_up, endpoint_down, endpoint_support, endpoint_range};
static IAudioEndpointVolume endpoint_volume = {&endpoint_table};

/* ---- An audio stream. ---- */

/* A buffer handed to the wave output, freed once it has played. */
struct block {
	WAVEHDR header;
	struct block *next;
	UINT32 frames, generation;
};

struct stream {
	IAudioClient3 client;
	IAudioRenderClient render;
	IAudioClock clock;
	IAudioStreamVolume volume;
	LONG references;
	CRITICAL_SECTION lock;
	BOOL initialized, started, closing;
	DWORD flags;
	WAVEFORMATEX *format;
	int kind;
	UINT32 frames, pending, generation;
	UINT64 written, played, speaker;
	DWORD last_sample;
	BYTE *staging;
	struct block *blocks;
	float channel_levels[8];
	HANDLE event, wave_event, worker;
	HWAVEOUT wave;
};

static void stream_reap(struct stream *self)
{
	struct block **link = &self->blocks;
	while (*link) {
		struct block *block = *link;
		if (!(((volatile WAVEHDR *)&block->header)->dwFlags & WHDR_DONE)) {
			link = &block->next;
			continue;
		}
		*link = block->next;
		waveOutUnprepareHeader(self->wave, &block->header, sizeof(block->header));
		if (block->generation == self->generation)
			self->played += block->frames;
		FREE(block);
	}
}

/* While the stream lives: buffers that played are freed, and a stream driven by an event is told every period that it may write. */
static DWORD WINAPI stream_worker(void *parameter)
{
	struct stream *self = parameter;
	for (;;) {
		WaitForSingleObject(self->wave_event, PERIOD / 10000);
		EnterCriticalSection(&self->lock);
		const BOOL closing = self->closing;
		stream_reap(self);
		if (self->started && self->event)
			SetEvent(self->event);
		LeaveCriticalSection(&self->lock);
		if (closing)
			return 0;
	}
}

static float stream_level(struct stream *self, UINT32 channel)
{
	if (session_muted || endpoint_muted)
		return 0.0f;
	return session_level * endpoint_level * self->channel_levels[channel];
}

/* The frames of the staging buffer at their volume, into a buffer of the wave output. */
static void scale_into(struct stream *self, BYTE *to, const BYTE *from, UINT32 frames)
{
	const UINT32 channels = self->format->nChannels, size = frames * self->format->nBlockAlign;
	BOOL whole = TRUE;
	for (UINT32 channel = 0; channel < channels; ++channel)
		if (stream_level(self, channel) != 1.0f)
			whole = FALSE;
	memcpy(to, from, size);
	if (whole)
		return;
	for (UINT32 frame = 0; frame < frames; ++frame)
		for (UINT32 channel = 0; channel < channels; ++channel) {
			const float level = stream_level(self, channel);
			const UINT32 index = frame * channels + channel;
			switch (self->kind) {
			case SAMPLE_FLOAT:
				((float *)to)[index] *= level;
				break;
			case SAMPLE_S16:
				((short *)to)[index] = (short)(((short *)to)[index] * level);
				break;
			case SAMPLE_S32:
				((int *)to)[index] = (int)((float)((int *)to)[index] * level);
				break;
			case SAMPLE_U8:
				to[index] = (BYTE)(128 + (int)((to[index] - 128) * level));
				break;
			case SAMPLE_S24: {
				BYTE *sample = to + index * 3;
				int value = (int)((unsigned)sample[0] << 8 | (unsigned)sample[1] << 16 | (unsigned)sample[2] << 24) >> 8;
				value = (int)(value * level);
				sample[0] = (BYTE)value;
				sample[1] = (BYTE)(value >> 8);
				sample[2] = (BYTE)(value >> 16);
				break;
			}
			}
		}
}

static void stream_close(struct stream *self)
{
	if (self->worker) {
		EnterCriticalSection(&self->lock);
		self->closing = TRUE;
		LeaveCriticalSection(&self->lock);
		SetEvent(self->wave_event);
		WaitForSingleObject(self->worker, INFINITE);
		CloseHandle(self->worker);
	}
	if (self->wave) {
		waveOutReset(self->wave);
		/* The buffers the reset returned are marked done when the scheduler runs. */
		for (int tries = 0; self->blocks && tries < 200; ++tries) {
			stream_reap(self);
			if (self->blocks)
				Sleep(5);
		}
		waveOutClose(self->wave);
	}
	if (self->wave_event)
		CloseHandle(self->wave_event);
	FREE(self->staging);
	FREE(self->format);
}

static HRESULT WINAPI client_query(IAudioClient3 *iface, REFIID iid, void **out)
{
	struct stream *self = OWNER(struct stream, client, iface);
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IAudioClient) || same(iid, &IID_IAudioClient2) || same(iid, &IID_IAudioClient3))
		*out = &self->client;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->references);
	return S_OK;
}

static ULONG WINAPI client_reference(IAudioClient3 *iface)
{
	return InterlockedIncrement(&OWNER(struct stream, client, iface)->references);
}

static ULONG WINAPI client_release(IAudioClient3 *iface)
{
	struct stream *self = OWNER(struct stream, client, iface);
	const LONG left = InterlockedDecrement(&self->references);
	if (!left) {
		if (self->started)
			InterlockedDecrement(&streams_started);
		stream_close(self);
		DeleteCriticalSection(&self->lock);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return left;
}

static HRESULT open_stream(struct stream *self, DWORD flags, REFERENCE_TIME duration, const WAVEFORMATEX *format)
{
	const int kind = sample_kind(format);
	if (kind < 0)
		return E_UNSUPPORTED_FORMAT;
	if (flags & STREAM_LOOPBACK)
		return E_WRONG_ENDPOINT_TYPE;
	/* A minute is more than a program asks for, and keeps the count of frames in 32 bits. */
	if (duration < 0 || duration > 600000000)
		return E_INVALIDARG;
	const UINT32 size = format_size(format);
	self->format = ALLOC(size);
	if (!self->format)
		return E_OUTOFMEMORY;
	memcpy(self->format, format, size);
	self->format->cbSize = (WORD)(size - sizeof(WAVEFORMATEX));
	self->kind = kind;
	self->flags = flags;
	/* The buffer holds what was asked for, and at least two periods. */
	if (duration < 2 * PERIOD)
		duration = 2 * PERIOD;
	self->frames = (UINT32)((duration * format->nSamplesPerSec + 9999999) / 10000000);
	self->staging = ALLOC((SIZE_T)self->frames * format->nBlockAlign);
	self->wave_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!self->staging || !self->wave_event)
		return E_OUTOFMEMORY;
	/* The wave output takes what the format describes; an extensible format is handed over as it is. */
	if (waveOutOpen(&self->wave, WAVE_MAPPER, format, (DWORD_PTR)self->wave_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
		self->wave = NULL;
		return E_UNSUPPORTED_FORMAT;
	}
	waveOutPause(self->wave);
	self->worker = CreateThread(NULL, 0, stream_worker, self, 0, NULL);
	if (!self->worker)
		return E_OUTOFMEMORY;
	self->initialized = TRUE;
	return S_OK;
}

static HRESULT WINAPI client_initialize(IAudioClient3 *iface, AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration,
	REFERENCE_TIME periodicity, const WAVEFORMATEX *format, LPCGUID session_guid)
{
	struct stream *self = OWNER(struct stream, client, iface);
	if (!format)
		return E_POINTER;
	if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE)
		return E_EXCLUSIVE_NOT_ALLOWED;
	if (mode != AUDCLNT_SHAREMODE_SHARED || duration < 0)
		return E_INVALIDARG;
	EnterCriticalSection(&self->lock);
	HRESULT result = E_ALREADY_INITIALIZED;
	if (!self->initialized) {
		result = open_stream(self, flags, duration, format);
		if (FAILED(result))
			stream_close(self), self->worker = NULL, self->wave = NULL, self->wave_event = NULL, self->staging = NULL, self->format = NULL;
	}
	LeaveCriticalSection(&self->lock);
	return result;
}

static HRESULT WINAPI client_buffer_size(IAudioClient3 *iface, UINT32 *frames)
{
	struct stream *self = OWNER(struct stream, client, iface);
	if (!frames)
		return E_POINTER;
	if (!self->initialized)
		return E_NOT_INITIALIZED;
	*frames = self->frames;
	return S_OK;
}

/* The latency is what the wave output keeps ahead of the speaker and one period of the engine. */
static HRESULT WINAPI client_latency(IAudioClient3 *iface, REFERENCE_TIME *latency)
{
	struct stream *self = OWNER(struct stream, client, iface);
	if (!latency)
		return E_POINTER;
	if (!self->initialized)
		return E_NOT_INITIALIZED;
	*latency = 10000000 / AHEAD_DIVISOR + PERIOD;
	return S_OK;
}

static HRESULT WINAPI client_padding(IAudioClient3 *iface, UINT32 *padding)
{
	struct stream *self = OWNER(struct stream, client, iface);
	if (!padding)
		return E_POINTER;
	if (!self->initialized)
		return E_NOT_INITIALIZED;
	EnterCriticalSection(&self->lock);
	stream_reap(self);
	*padding = (UINT32)(self->written - self->played);
	LeaveCriticalSection(&self->lock);
	return S_OK;
}

static HRESULT WINAPI client_supported(IAudioClient3 *iface, AUDCLNT_SHAREMODE mode, const WAVEFORMATEX *format, WAVEFORMATEX **closest)
{
	if (!format)
		return E_POINTER;
	if (closest)
		*closest = NULL;
	if (mode == AUDCLNT_SHAREMODE_SHARED && !closest)
		return E_POINTER;
	if (mode != AUDCLNT_SHAREMODE_SHARED && mode != AUDCLNT_SHAREMODE_EXCLUSIVE)
		return E_INVALIDARG;
	if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE || sample_kind(format) >= 0)
		return mode == AUDCLNT_SHAREMODE_EXCLUSIVE ? E_UNSUPPORTED_FORMAT : S_OK;
	/* A format the device cannot play: the mix format is the closest one. */
	WAVEFORMATEXTENSIBLE *nearest = CoTaskMemAlloc(sizeof(*nearest));
	if (!nearest)
		return E_OUTOFMEMORY;
	mix_format(nearest);
	*closest = &nearest->Format;
	return S_FALSE;
}

static HRESULT WINAPI client_mix_format(IAudioClient3 *iface, WAVEFORMATEX **format)
{
	if (!format)
		return E_POINTER;
	WAVEFORMATEXTENSIBLE *mix = CoTaskMemAlloc(sizeof(*mix));
	if (!mix)
		return E_OUTOFMEMORY;
	mix_format(mix);
	*format = &mix->Format;
	return S_OK;
}

static HRESULT WINAPI client_period(IAudioClient3 *iface, REFERENCE_TIME *period, REFERENCE_TIME *minimum)
{
	if (!period && !minimum)
		return E_POINTER;
	if (period)
		*period = PERIOD;
	if (minimum)
		*minimum = MIN_PERIOD;
	return S_OK;
}

static HRESULT WINAPI client_start(IAudioClient3 *iface)
{
	struct stream *self = OWNER(struct stream, client, iface);
	EnterCriticalSection(&self->lock);
	HRESULT result = S_OK;
	if (!self->initialized)
		result = E_NOT_INITIALIZED;
	else if (self->started)
		result = E_NOT_STOPPED;
	else if ((self->flags & STREAM_EVENTS) && !self->event)
		result = E_EVENTHANDLE_NOT_SET;
	else {
		self->started = TRUE;
		InterlockedIncrement(&streams_started);
		waveOutRestart(self->wave);
		if (self->event)
			SetEvent(self->event);
	}
	LeaveCriticalSection(&self->lock);
	return result;
}

static HRESULT WINAPI client_stop(IAudioClient3 *iface)
{
	struct stream *self = OWNER(struct stream, client, iface);
	EnterCriticalSection(&self->lock);
	HRESULT result = S_OK;
	if (!self->initialized)
		result = E_NOT_INITIALIZED;
	else if (!self->started)
		result = S_FALSE;
	else {
		self->started = FALSE;
		InterlockedDecrement(&streams_started);
		waveOutPause(self->wave);
	}
	LeaveCriticalSection(&self->lock);
	return result;
}

/* A stopped stream drops what it holds and its clock starts again from zero. */
static HRESULT WINAPI client_reset(IAudioClient3 *iface)
{
	struct stream *self = OWNER(struct stream, client, iface);
	EnterCriticalSection(&self->lock);
	HRESULT result = S_OK;
	if (!self->initialized)
		result = E_NOT_INITIALIZED;
	else if (self->started)
		result = E_NOT_STOPPED;
	else if (self->pending)
		result = E_BUFFER_PENDING;
	else {
		++self->generation;
		waveOutReset(self->wave);
		waveOutPause(self->wave);
		self->written = self->played = self->speaker = 0;
		self->last_sample = 0;
	}
	LeaveCriticalSection(&self->lock);
	return result;
}

static HRESULT WINAPI client_event(IAudioClient3 *iface, HANDLE event)
{
	struct stream *self = OWNER(struct stream, client, iface);
	if (!event)
		return E_INVALIDARG;
	if (!self->initialized)
		return E_NOT_INITIALIZED;
	if (!(self->flags & STREAM_EVENTS))
		return E_EVENTHANDLE_NOT_EXPECTED;
	EnterCriticalSection(&self->lock);
	self->event = event;
	LeaveCriticalSection(&self->lock);
	return S_OK;
}

static HRESULT WINAPI client_service(IAudioClient3 *iface, REFIID iid, void **out)
{
	struct stream *self = OWNER(struct stream, client, iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!self->initialized)
		return E_NOT_INITIALIZED;
	if (same(iid, &IID_IAudioCaptureClient))
		return E_WRONG_ENDPOINT_TYPE;
	if (same(iid, &IID_IAudioRenderClient))
		*out = &self->render;
	else if (same(iid, &IID_IAudioClock))
		*out = &self->clock;
	else if (same(iid, &IID_IAudioStreamVolume))
		*out = &self->volume;
	else if (same(iid, &IID_IAudioSessionControl))
		*out = &session;
	else if (same(iid, &IID_ISimpleAudioVolume))
		*out = &simple_volume;
	else
		return E_NOINTERFACE;
	if (*out != &session && *out != &simple_volume)
		InterlockedIncrement(&self->references);
	return S_OK;
}

static HRESULT WINAPI client_offload(IAudioClient3 *iface, AUDIO_STREAM_CATEGORY category, BOOL *capable)
{
	if (!capable)
		return E_POINTER;
	*capable = FALSE;
	return S_OK;
}

static HRESULT WINAPI client_properties(IAudioClient3 *iface, const AudioClientProperties *properties)
{
	return properties ? S_OK : E_POINTER;
}

static HRESULT WINAPI client_limits(IAudioClient3 *iface, const WAVEFORMATEX *format, BOOL event, REFERENCE_TIME *minimum,
	REFERENCE_TIME *maximum)
{
	if (!format || !minimum || !maximum)
		return E_POINTER;
	*minimum = MIN_PERIOD;
	*maximum = 20000000;
	return S_OK;
}

static HRESULT WINAPI client_engine_period(IAudioClient3 *iface, const WAVEFORMATEX *format, UINT32 *period, UINT32 *fundamental,
	UINT32 *minimum, UINT32 *maximum)
{
	if (!format || !period || !fundamental || !minimum || !maximum)
		return E_POINTER;
	*period = *minimum = *maximum = format->nSamplesPerSec / 100;
	*fundamental = format->nSamplesPerSec / 100;
	return S_OK;
}

static HRESULT WINAPI client_current_period(IAudioClient3 *iface, WAVEFORMATEX **format, UINT32 *period)
{
	if (!format || !period)
		return E_POINTER;
	const HRESULT result = client_mix_format(iface, format);
	*period = MIX_RATE / 100;
	return result;
}

static HRESULT WINAPI client_shared(IAudioClient3 *iface, DWORD flags, UINT32 period, const WAVEFORMATEX *format, LPCGUID session_guid)
{
	if (!format)
		return E_POINTER;
	if (period != format->nSamplesPerSec / 100)
		return E_INVALID_DEVICE_PERIOD;
	return client_initialize(iface, AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, format, session_guid);
}

static IAudioClient3Vtbl client_table = {client_query, client_reference, client_release, client_initialize, client_buffer_size,
	client_latency, client_padding, client_supported, client_mix_format, client_period, client_start, client_stop, client_reset,
	client_event, client_service, client_offload, client_properties, client_limits, client_engine_period, client_current_period,
	client_shared};

/* The interfaces a stream hands out through GetService share its reference count. */
#define STREAM_UNKNOWN(prefix, member, type) \
	static HRESULT WINAPI prefix##_query(type *iface, REFIID iid, void **out) \
	{ \
		struct stream *self = OWNER(struct stream, member, iface); \
		if (!out) \
			return E_POINTER; \
		if (same(iid, &IID_IUnknown) || same(iid, &IID_##type)) { \
			*out = iface; \
			InterlockedIncrement(&self->references); \
			return S_OK; \
		} \
		*out = NULL; \
		return E_NOINTERFACE; \
	} \
	static ULONG WINAPI prefix##_reference(type *iface) { return client_reference(&OWNER(struct stream, member, iface)->client); } \
	static ULONG WINAPI prefix##_release(type *iface) { return client_release(&OWNER(struct stream, member, iface)->client); }

STREAM_UNKNOWN(render, render, IAudioRenderClient)
STREAM_UNKNOWN(clock, clock, IAudioClock)
STREAM_UNKNOWN(levels, volume, IAudioStreamVolume)

static HRESULT WINAPI render_buffer(IAudioRenderClient *iface, UINT32 frames, BYTE **data)
{
	struct stream *self = OWNER(struct stream, render, iface);
	if (!data)
		return E_POINTER;
	*data = NULL;
	EnterCriticalSection(&self->lock);
	HRESULT result = S_OK;
	stream_reap(self);
	if (self->pending)
		result = E_OUT_OF_ORDER;
	else if (frames > self->frames - (UINT32)(self->written - self->played))
		result = E_BUFFER_TOO_LARGE;
	else if (frames) {
		self->pending = frames;
		*data = self->staging;
	}
	LeaveCriticalSection(&self->lock);
	return result;
}

static HRESULT WINAPI render_commit(IAudioRenderClient *iface, UINT32 frames, DWORD flags)
{
	struct stream *self = OWNER(struct stream, render, iface);
	EnterCriticalSection(&self->lock);
	HRESULT result = S_OK;
	if (frames > self->pending)
		result = self->pending ? E_INVALID_SIZE : E_OUT_OF_ORDER;
	else if (frames) {
		const UINT32 size = frames * self->format->nBlockAlign;
		struct block *block = ALLOC(sizeof(*block) + size);
		if (!block)
			result = E_OUTOFMEMORY;
		else {
			BYTE *samples = (BYTE *)(block + 1);
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
				memset(samples, self->kind == SAMPLE_U8 ? 0x80 : 0, size);
			else
				scale_into(self, samples, self->staging, frames);
			block->header.lpData = (LPSTR)samples;
			block->header.dwBufferLength = size;
			block->frames = frames;
			block->generation = self->generation;
			waveOutPrepareHeader(self->wave, &block->header, sizeof(block->header));
			if (waveOutWrite(self->wave, &block->header, sizeof(block->header)) != MMSYSERR_NOERROR) {
				waveOutUnprepareHeader(self->wave, &block->header, sizeof(block->header));
				FREE(block);
				result = E_OUTOFMEMORY;
			} else {
				block->next = self->blocks;
				self->blocks = block;
				self->written += frames;
			}
		}
	}
	/* A size that is too large leaves the buffer with the program, which may release it again. */
	if (result != E_INVALID_SIZE)
		self->pending = 0;
	LeaveCriticalSection(&self->lock);
	return result;
}

static IAudioRenderClientVtbl render_table = {render_query, render_reference, render_release, render_buffer, render_commit};

/* The clock counts bytes of the stream's format, and stands where the wave output says the speaker is. */
static HRESULT WINAPI clock_frequency(IAudioClock *iface, UINT64 *frequency)
{
	struct stream *self = OWNER(struct stream, clock, iface);
	if (!frequency)
		return E_POINTER;
	*frequency = (UINT64)self->format->nSamplesPerSec * self->format->nBlockAlign;
	return S_OK;
}

static HRESULT WINAPI clock_position(IAudioClock *iface, UINT64 *position, UINT64 *counter)
{
	struct stream *self = OWNER(struct stream, clock, iface);
	if (!position)
		return E_POINTER;
	EnterCriticalSection(&self->lock);
	MMTIME time = {TIME_SAMPLES};
	if (waveOutGetPosition(self->wave, &time, sizeof(time)) == MMSYSERR_NOERROR) {
		const DWORD sample = time.wType == TIME_SAMPLES ? time.u.sample : time.u.cb / self->format->nBlockAlign;
		/* The wave output counts in 32 bits: the clock adds what moved since it last asked. */
		self->speaker += (DWORD)(sample - self->last_sample);
		self->last_sample = sample;
	}
	if (self->speaker > self->written)
		self->speaker = self->written;
	*position = self->speaker * self->format->nBlockAlign;
	LeaveCriticalSection(&self->lock);
	if (counter) {
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		*counter = (UINT64)((double)now.QuadPart * 10000000.0 / (double)frequency.QuadPart);
	}
	return S_OK;
}

static HRESULT WINAPI clock_characteristics(IAudioClock *iface, DWORD *characteristics)
{
	if (!characteristics)
		return E_POINTER;
	*characteristics = 0;
	return S_OK;
}

static IAudioClockVtbl clock_table = {clock_query, clock_reference, clock_release, clock_frequency, clock_position, clock_characteristics};

static HRESULT WINAPI levels_count(IAudioStreamVolume *iface, UINT32 *count)
{
	struct stream *self = OWNER(struct stream, volume, iface);
	if (!count)
		return E_POINTER;
	*count = self->format->nChannels;
	return S_OK;
}

static HRESULT WINAPI levels_set(IAudioStreamVolume *iface, UINT32 channel, const float level)
{
	struct stream *self = OWNER(struct stream, volume, iface);
	if (channel >= self->format->nChannels || !(level >= 0.0f && level <= 1.0f))
		return E_INVALIDARG;
	self->channel_levels[channel] = level;
	return S_OK;
}

static HRESULT WINAPI levels_get(IAudioStreamVolume *iface, UINT32 channel, float *level)
{
	struct stream *self = OWNER(struct stream, volume, iface);
	if (!level)
		return E_POINTER;
	if (channel >= self->format->nChannels)
		return E_INVALIDARG;
	*level = self->channel_levels[channel];
	return S_OK;
}

static HRESULT WINAPI levels_set_all(IAudioStreamVolume *iface, UINT32 count, const float *levels)
{
	struct stream *self = OWNER(struct stream, volume, iface);
	if (!levels)
		return E_POINTER;
	if (count != self->format->nChannels)
		return E_INVALIDARG;
	for (UINT32 channel = 0; channel < count; ++channel)
		if (!(levels[channel] >= 0.0f && levels[channel] <= 1.0f))
			return E_INVALIDARG;
	for (UINT32 channel = 0; channel < count; ++channel)
		self->channel_levels[channel] = levels[channel];
	return S_OK;
}

static HRESULT WINAPI levels_get_all(IAudioStreamVolume *iface, UINT32 count, float *levels)
{
	struct stream *self = OWNER(struct stream, volume, iface);
	if (!levels)
		return E_POINTER;
	if (count != self->format->nChannels)
		return E_INVALIDARG;
	for (UINT32 channel = 0; channel < count; ++channel)
		levels[channel] = self->channel_levels[channel];
	return S_OK;
}

static IAudioStreamVolumeVtbl levels_table = {levels_query, levels_reference, levels_release, levels_count, levels_set, levels_get,
	levels_set_all, levels_get_all};

static HRESULT new_stream(void **out)
{
	struct stream *self = ALLOC(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->client.lpVtbl = &client_table;
	self->render.lpVtbl = &render_table;
	self->clock.lpVtbl = &clock_table;
	self->volume.lpVtbl = &levels_table;
	self->references = 1;
	for (UINT32 channel = 0; channel < 8; ++channel)
		self->channel_levels[channel] = 1.0f;
	InitializeCriticalSection(&self->lock);
	InterlockedIncrement(&objects);
	*out = &self->client;
	return S_OK;
}

/* ---- The output device. ---- */

static HRESULT WINAPI endpoint_flow_query(IMMEndpoint *iface, REFIID iid, void **out);

static HRESULT WINAPI device_query(IMMDevice *iface, REFIID iid, void **out);

static HRESULT WINAPI device_activate(IMMDevice *iface, REFIID iid, DWORD context, PROPVARIANT *parameters, void **out)
{
	if (!iid || !out)
		return E_POINTER;
	*out = NULL;
	if (same(iid, &IID_IAudioClient) || same(iid, &IID_IAudioClient2) || same(iid, &IID_IAudioClient3))
		return new_stream(out);
	if (same(iid, &IID_IAudioSessionManager) || same(iid, &IID_IAudioSessionManager2)) {
		*out = &session_manager;
		return S_OK;
	}
	if (same(iid, &IID_IAudioEndpointVolume)) {
		*out = &endpoint_volume;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static HRESULT WINAPI device_properties(IMMDevice *iface, DWORD access, IPropertyStore **out)
{
	if (!out)
		return E_POINTER;
	*out = &device_store;
	return S_OK;
}

static HRESULT WINAPI device_id(IMMDevice *iface, LPWSTR *id) { return text_out(render_id, id); }

static HRESULT WINAPI device_state(IMMDevice *iface, DWORD *state)
{
	if (!state)
		return E_POINTER;
	*state = DEVICE_STATE_ACTIVE;
	return S_OK;
}

static IMMDeviceVtbl device_table = {device_query, (void *)static_reference, (void *)static_release, device_activate, device_properties,
	device_id, device_state};

static HRESULT WINAPI endpoint_flow(IMMEndpoint *iface, EDataFlow *flow)
{
	if (!flow)
		return E_POINTER;
	*flow = eRender;
	return S_OK;
}

static IMMEndpointVtbl endpoint_flow_table = {endpoint_flow_query, (void *)static_reference, (void *)static_release, endpoint_flow};

static struct {
	IMMDevice device;
	IMMEndpoint endpoint;
} speakers = {{&device_table}, {&endpoint_flow_table}};

static HRESULT WINAPI device_query(IMMDevice *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IMMDevice))
		*out = &speakers.device;
	else if (same(iid, &IID_IMMEndpoint))
		*out = &speakers.endpoint;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	return S_OK;
}

static HRESULT WINAPI endpoint_flow_query(IMMEndpoint *iface, REFIID iid, void **out)
{
	return device_query(&speakers.device, iid, out);
}

/* ---- The list of output devices, the default one alone, or of input devices, none. ---- */

struct list {
	IMMDeviceCollection iface;
	BOOL holds_speakers;
};

static HRESULT WINAPI list_query(IMMDeviceCollection *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IMMDeviceCollection)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT WINAPI list_count(IMMDeviceCollection *iface, UINT *count)
{
	if (!count)
		return E_POINTER;
	*count = OWNER(struct list, iface, iface)->holds_speakers ? 1 : 0;
	return S_OK;
}

static HRESULT WINAPI list_item(IMMDeviceCollection *iface, UINT index, IMMDevice **device)
{
	if (!device)
		return E_POINTER;
	*device = NULL;
	if (index || !OWNER(struct list, iface, iface)->holds_speakers)
		return E_INVALIDARG;
	*device = &speakers.device;
	return S_OK;
}

static IMMDeviceCollectionVtbl list_table = {list_query, (void *)static_reference, (void *)static_release, list_count, list_item};
static struct list outputs = {{&list_table}, TRUE}, nothing = {{&list_table}, FALSE};

/* ---- The enumerator. ---- */

static HRESULT WINAPI enumerator_query(IMMDeviceEnumerator *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IMMDeviceEnumerator)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT WINAPI enumerator_list(IMMDeviceEnumerator *iface, EDataFlow flow, DWORD mask, IMMDeviceCollection **out)
{
	if (!out)
		return E_POINTER;
	if ((flow != eRender && flow != eCapture && flow != eAll) || (mask & ~DEVICE_STATEMASK_ALL))
		return E_INVALIDARG;
	*out = flow != eCapture && (mask & DEVICE_STATE_ACTIVE) ? &outputs.iface : &nothing.iface;
	return S_OK;
}

static HRESULT WINAPI enumerator_default(IMMDeviceEnumerator *iface, EDataFlow flow, ERole role, IMMDevice **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if ((flow != eRender && flow != eCapture) || (unsigned)role > eCommunications)
		return E_INVALIDARG;
	if (flow == eCapture)
		return E_NOT_FOUND;
	*out = &speakers.device;
	return S_OK;
}

static HRESULT WINAPI enumerator_device(IMMDeviceEnumerator *iface, LPCWSTR id, IMMDevice **out)
{
	if (!id || !out)
		return E_POINTER;
	*out = NULL;
	for (UINT32 index = 0;; ++index) {
		if (id[index] != render_id[index])
			return E_NOT_FOUND;
		if (!id[index])
			break;
	}
	*out = &speakers.device;
	return S_OK;
}

static HRESULT WINAPI enumerator_notify(IMMDeviceEnumerator *iface, IMMNotificationClient *client)
{
	return client ? S_OK : E_POINTER;
}

static IMMDeviceEnumeratorVtbl enumerator_table = {enumerator_query, (void *)static_reference, (void *)static_release, enumerator_list,
	enumerator_default, enumerator_device, enumerator_notify, enumerator_notify};
static IMMDeviceEnumerator enumerator = {&enumerator_table};

/* ---- The class factory. ---- */

static HRESULT WINAPI factory_query(IClassFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT WINAPI factory_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return enumerator_query(&enumerator, iid, out);
}

static HRESULT WINAPI factory_lock(IClassFactory *iface, BOOL lock) { return S_OK; }

static IClassFactoryVtbl factory_table = {factory_query, (void *)static_reference, (void *)static_release, factory_create, factory_lock};
static IClassFactory factory = {&factory_table};

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!same(clsid, &CLSID_RavenMMDeviceEnumerator))
		return CLASS_E_CLASSNOTAVAILABLE;
	return factory_query(&factory, iid, out);
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
