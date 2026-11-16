/* SPDX-License-Identifier: MIT
 *
 * Sound buffers: IDirectSoundBuffer8 and, as their flags allow, IDirectSoundNotify and IDirectSound3DBuffer; the primary buffer, which
 * holds the device's format, volume and IDirectSound3DListener.
 */
#include "dsound_private.h"

static const DS3DBUFFER default_3d = {sizeof(DS3DBUFFER), {0, 0, 0}, {0, 0, 0}, DS3D_DEFAULTCONEANGLE, DS3D_DEFAULTCONEANGLE, {0, 0, 1},
	DS3D_DEFAULTCONEOUTSIDEVOLUME, DS3D_DEFAULTMINDISTANCE, DS3D_DEFAULTMAXDISTANCE, DS3DMODE_NORMAL};

buffer *buffer_from(IDirectSoundBuffer *iface) { return CONTAINING_RECORD(iface, buffer, IDirectSoundBuffer8_iface); }
static buffer *buffer_from8(IDirectSoundBuffer8 *iface) { return CONTAINING_RECORD(iface, buffer, IDirectSoundBuffer8_iface); }

/* ----- Formats, locks and notifications, which capture buffers share ----- */

HRESULT format_check(const WAVEFORMATEX *format, buffer_data *data)
{
	if (!format || !format->nChannels || format->nChannels > 8 || format->nSamplesPerSec < DSBFREQUENCY_MIN ||
		format->nSamplesPerSec > DSBFREQUENCY_MAX)
		return DSERR_INVALIDPARAM;
	BOOL is_float;
	if (format->wFormatTag == WAVE_FORMAT_PCM)
		is_float = FALSE;
	else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		is_float = TRUE;
	else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		if (format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			return DSERR_INVALIDPARAM;
		const WAVEFORMATEXTENSIBLE *extensible = (const WAVEFORMATEXTENSIBLE *)format;
		if (same_guid(&extensible->SubFormat, &subtype_pcm))
			is_float = FALSE;
		else if (same_guid(&extensible->SubFormat, &subtype_float))
			is_float = TRUE;
		else
			return DSERR_BADFORMAT;
	} else
		return DSERR_BADFORMAT;
	const WORD bits = format->wBitsPerSample;
	if (is_float ? bits != 32 : bits != 8 && bits != 16 && bits != 24 && bits != 32)
		return DSERR_BADFORMAT;
	if (format->nBlockAlign != format->nChannels * bits / 8)
		return DSERR_INVALIDPARAM;
	if (data) {
		memset(&data->format, 0, sizeof(data->format));
		memcpy(&data->format, format, format->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX));
		data->format.Format.cbSize = format->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) : 0;
		data->sample_bytes = bits / 8;
		data->is_float = is_float;
	}
	return DS_OK;
}

HRESULT format_copy(const WAVEFORMATEX *format, WAVEFORMATEX *out, DWORD size, DWORD *written)
{
	const DWORD needed = sizeof(WAVEFORMATEX) + format->cbSize;
	if (!out && !written)
		return DSERR_INVALIDPARAM;
	if (!out) {
		*written = needed;
		return DS_OK;
	}
	if (size < needed)
		return DSERR_INVALIDPARAM;
	memcpy(out, format, needed);
	if (written)
		*written = needed;
	return DS_OK;
}

HRESULT lock_range(buffer_data *data, DWORD offset, DWORD bytes, void **first, DWORD *first_bytes, void **second, DWORD *second_bytes)
{
	if (!first || !first_bytes || offset >= data->size || !bytes || bytes > data->size)
		return DSERR_INVALIDPARAM;
	*first = data->memory + offset;
	if (offset + bytes <= data->size) {
		*first_bytes = bytes;
		if (second)
			*second = NULL;
		if (second_bytes)
			*second_bytes = 0;
		return DS_OK;
	}
	*first_bytes = data->size - offset;
	if (second)
		*second = data->memory;
	if (second_bytes)
		*second_bytes = second ? bytes - *first_bytes : 0;
	return DS_OK;
}

HRESULT unlock_range(buffer_data *data, void *first, DWORD first_bytes, void *second, DWORD second_bytes)
{
	const BYTE *start = data->memory, *end = data->memory + data->size;
	if (!first || (BYTE *)first < start || (BYTE *)first + first_bytes > end)
		return DSERR_INVALIDPARAM;
	if (second && ((BYTE *)second != start || (BYTE *)second + second_bytes > end))
		return DSERR_INVALIDPARAM;
	return DS_OK;
}

HRESULT notify_set(notify_list *list, DWORD size, DWORD count, const DSBPOSITIONNOTIFY *items)
{
	if (count > DSBNOTIFICATIONS_MAX || (count && !items))
		return DSERR_INVALIDPARAM;
	for (DWORD index = 0; index < count; index++)
		if (items[index].dwOffset >= size && items[index].dwOffset != (DWORD)DSBPN_OFFSETSTOP)
			return DSERR_INVALIDPARAM;
	DSBPOSITIONNOTIFY *copy = NULL;
	if (count) {
		copy = ALLOC(count * sizeof(*copy));
		if (!copy)
			return DSERR_OUTOFMEMORY;
		memcpy(copy, items, count * sizeof(*copy));
	}
	FREE(list->items);
	list->items = copy;
	list->count = count;
	return DS_OK;
}

/* Sets the events of the offsets the cursor passed going from `from` to `to`, wrapping at size: those after from up to and including to. */
void notify_range(const notify_list *list, DWORD from, DWORD to, DWORD size)
{
	for (DWORD index = 0; index < list->count; index++) {
		const DWORD offset = list->items[index].dwOffset;
		if (offset == (DWORD)DSBPN_OFFSETSTOP)
			continue;
		const BOOL passed = from <= to ? offset > from && offset <= to : offset > from || offset <= to;
		if (passed && offset < size)
			SetEvent(list->items[index].hEventNotify);
	}
}

void notify_stop(const notify_list *list)
{
	for (DWORD index = 0; index < list->count; index++)
		if (list->items[index].dwOffset == (DWORD)DSBPN_OFFSETSTOP)
			SetEvent(list->items[index].hEventNotify);
}

/* ----- Gains ----- */

static void buffer_gains(buffer *buffer)
{
	const float volume = gain_of(buffer->volume);
	buffer->gain_left = volume * (buffer->pan > 0 ? gain_of(-buffer->pan) : 1);
	buffer->gain_right = volume * (buffer->pan < 0 ? gain_of(buffer->pan) : 1);
}

void buffer_reset_primary(buffer *buffer)
{
	buffer->volume = DSBVOLUME_MAX;
	buffer->pan = DSBPAN_CENTER;
	buffer->gain_3d_left = buffer->gain_3d_right = 1;
	buffer->playing = buffer->looping = FALSE;
	buffer_gains(buffer);
}

static float dot(D3DVECTOR first, D3DVECTOR second) { return first.x * second.x + first.y * second.y + first.z * second.z; }

/* The distance and side of the sound from the listener: the gain the inverse distance law with the listener's rolloff gives between the
 * buffer's minimum and maximum distances, and a pan toward the side it is on.
 * shortcut: cones, velocity, the Doppler shift and the distance factor are stored and not heard, model them when a program depends on them. */
void buffer_update_3d(buffer *buffer)
{
	buffer->gain_3d_left = buffer->gain_3d_right = 1;
	if (!(buffer->flags & DSBCAPS_CTRL3D) || buffer->params.dwMode == DS3DMODE_DISABLE)
		return;
	const DS3DLISTENER *listener = &buffer->device->listener;
	D3DVECTOR relative = buffer->params.vPosition;
	float side;
	if (buffer->params.dwMode == DS3DMODE_HEADRELATIVE) {
		side = relative.x;
	} else {
		relative.x -= listener->vPosition.x;
		relative.y -= listener->vPosition.y;
		relative.z -= listener->vPosition.z;
		const D3DVECTOR front = listener->vOrientFront, top = listener->vOrientTop;
		/* DirectSound's axes are left-handed: the listener's right is its top crossed with its front. */
		const D3DVECTOR right = {top.y * front.z - top.z * front.y, top.z * front.x - top.x * front.z, top.x * front.y - top.y * front.x};
		const float length = root_of(dot(right, right));
		side = length > 0 ? dot(relative, right) / length : 0;
	}
	const float distance = root_of(dot(relative, relative));
	const float minimum = buffer->params.flMinDistance > 0 ? buffer->params.flMinDistance : DS3D_DEFAULTMINDISTANCE;
	const float maximum = buffer->params.flMaxDistance > minimum ? buffer->params.flMaxDistance : minimum;
	float gain = 1;
	if (distance > maximum && (buffer->flags & DSBCAPS_MUTE3DATMAXDISTANCE))
		gain = 0;
	else if (distance > minimum) {
		const float clamped = distance < maximum ? distance : maximum;
		gain = minimum / (minimum + listener->flRolloffFactor * (clamped - minimum));
	}
	float pan = distance > 0 ? side / distance : 0;
	pan = pan > 1 ? 1 : pan < -1 ? -1 : pan;
	buffer->gain_3d_left = gain * (pan > 0 ? 1 - 0.75f * pan : 1);
	buffer->gain_3d_right = gain * (pan < 0 ? 1 + 0.75f * pan : 1);
}

/* ----- IDirectSoundBuffer8 ----- */

static BOOL buffer_finished(buffer *buffer)
{
	device *device = buffer->device;
	return buffer->ending && device->running && device_clock(device) >= buffer->end_device;
}

static HRESULT WINAPI buffer_QueryInterface(IDirectSoundBuffer8 *iface, REFIID riid, void **out)
{
	buffer *buffer = buffer_from8(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IDirectSoundBuffer))
		*out = &buffer->IDirectSoundBuffer8_iface;
	else if (same_guid(riid, &IID_IDirectSoundBuffer8) && buffer->is8 && !buffer->primary)
		*out = &buffer->IDirectSoundBuffer8_iface;
	else if (same_guid(riid, &IID_IDirectSoundNotify) && !buffer->primary && (buffer->flags & DSBCAPS_CTRLPOSITIONNOTIFY))
		*out = &buffer->IDirectSoundNotify_iface;
	else if (same_guid(riid, &IID_IDirectSound3DBuffer) && !buffer->primary && (buffer->flags & DSBCAPS_CTRL3D))
		*out = &buffer->IDirectSound3DBuffer_iface;
	else if (same_guid(riid, &IID_IDirectSound3DListener) && buffer->primary && (buffer->flags & DSBCAPS_CTRL3D))
		*out = &buffer->IDirectSound3DListener_iface;
	else
		return E_NOINTERFACE;
	InterlockedIncrement(&buffer->ref);
	return S_OK;
}

static ULONG WINAPI buffer_AddRef(IDirectSoundBuffer8 *iface) { return InterlockedIncrement(&buffer_from8(iface)->ref); }

void buffer_free(buffer *buffer)
{
	FREE(buffer->notify.items);
	if (!InterlockedDecrement(&buffer->data->ref)) {
		FREE(buffer->data->memory);
		FREE(buffer->data);
	}
	FREE(buffer);
}

static ULONG WINAPI buffer_Release(IDirectSoundBuffer8 *iface)
{
	buffer *buffer = buffer_from8(iface);
	if (buffer->primary) {
		EnterCriticalSection(&ds_lock);
		const LONG ref = InterlockedDecrement(&buffer->ref);
		if (!ref) {
			buffer_reset_primary(buffer);
			mixer_kick(buffer->device);
		}
		LeaveCriticalSection(&ds_lock);
		return ref;
	}
	const LONG ref = InterlockedDecrement(&buffer->ref);
	if (ref)
		return ref;
	EnterCriticalSection(&ds_lock);
	for (struct buffer **link = &buffer->device->buffers; *link; link = &(*link)->next)
		if (*link == buffer) {
			*link = buffer->next;
			break;
		}
	buffer_free(buffer);
	LeaveCriticalSection(&ds_lock);
	return 0;
}

static DWORD primary_bytes(device *device)
{
	const WAVEFORMATEX *format = &device->primary_format;
	return format->nSamplesPerSec / 10 * format->nBlockAlign;
}

static HRESULT WINAPI buffer_GetCaps(IDirectSoundBuffer8 *iface, DSBCAPS *caps)
{
	buffer *buffer = buffer_from8(iface);
	if (!caps || caps->dwSize < sizeof(*caps))
		return DSERR_INVALIDPARAM;
	caps->dwFlags = buffer->flags;
	caps->dwBufferBytes = buffer->primary ? primary_bytes(buffer->device) : buffer->data->size;
	caps->dwUnlockTransferRate = 0;
	caps->dwPlayCpuOverhead = 0;
	return DS_OK;
}

static HRESULT WINAPI buffer_GetCurrentPosition(IDirectSoundBuffer8 *iface, DWORD *play, DWORD *write)
{
	buffer *buffer = buffer_from8(iface);
	device *device = buffer->device;
	EnterCriticalSection(&ds_lock);
	if (buffer->primary) {
		const DWORD size = primary_bytes(device), block = device->primary_format.nBlockAlign, frames = size / block;
		const UINT64 played = device_clock(device);
		const UINT64 mixed = device->running ? device->mixed : 0;
		if (play)
			*play = (DWORD)(played * device->primary_format.nSamplesPerSec / MIX_RATE % frames) * block;
		if (write)
			*write = (DWORD)(mixed * device->primary_format.nSamplesPerSec / MIX_RATE % frames) * block;
	} else {
		DWORD played, written;
		buffer_cursors(buffer, &played, &written);
		if (play)
			*play = played;
		if (write)
			*write = written;
	}
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI buffer_GetFormat(IDirectSoundBuffer8 *iface, WAVEFORMATEX *format, DWORD size, DWORD *written)
{
	buffer *buffer = buffer_from8(iface);
	return format_copy(buffer->primary ? &buffer->device->primary_format : &buffer->data->format.Format, format, size, written);
}

static HRESULT WINAPI buffer_GetVolume(IDirectSoundBuffer8 *iface, LONG *volume)
{
	buffer *buffer = buffer_from8(iface);
	if (!volume)
		return DSERR_INVALIDPARAM;
	if (!(buffer->flags & DSBCAPS_CTRLVOLUME))
		return DSERR_CONTROLUNAVAIL;
	*volume = buffer->volume;
	return DS_OK;
}

static HRESULT WINAPI buffer_GetPan(IDirectSoundBuffer8 *iface, LONG *pan)
{
	buffer *buffer = buffer_from8(iface);
	if (!pan)
		return DSERR_INVALIDPARAM;
	if (!(buffer->flags & DSBCAPS_CTRLPAN))
		return DSERR_CONTROLUNAVAIL;
	*pan = buffer->pan;
	return DS_OK;
}

static HRESULT WINAPI buffer_GetFrequency(IDirectSoundBuffer8 *iface, DWORD *frequency)
{
	buffer *buffer = buffer_from8(iface);
	if (!frequency)
		return DSERR_INVALIDPARAM;
	if (buffer->primary || !(buffer->flags & DSBCAPS_CTRLFREQUENCY))
		return DSERR_CONTROLUNAVAIL;
	*frequency = buffer->frequency;
	return DS_OK;
}

static HRESULT WINAPI buffer_GetStatus(IDirectSoundBuffer8 *iface, DWORD *status)
{
	buffer *buffer = buffer_from8(iface);
	if (!status)
		return DSERR_INVALIDPARAM;
	EnterCriticalSection(&ds_lock);
	*status = 0;
	if (buffer->playing && !buffer_finished(buffer))
		*status = DSBSTATUS_PLAYING | (buffer->looping ? DSBSTATUS_LOOPING : 0);
	if ((buffer->flags & DSBCAPS_LOCDEFER) && (buffer->flags & DSBCAPS_LOCSOFTWARE))
		*status |= DSBSTATUS_LOCSOFTWARE;
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI buffer_Initialize(IDirectSoundBuffer8 *iface, IDirectSound *sound, const DSBUFFERDESC *desc)
{
	return DSERR_ALREADYINITIALIZED;
}

static HRESULT WINAPI buffer_Lock(IDirectSoundBuffer8 *iface, DWORD offset, DWORD bytes, void **first, DWORD *first_bytes, void **second,
	DWORD *second_bytes, DWORD flags)
{
	buffer *buffer = buffer_from8(iface);
	/* shortcut: the primary buffer has no memory to write, give it some when a program mixes for itself at DSSCL_WRITEPRIMARY. */
	if (buffer->primary)
		return buffer->device->level == DSSCL_WRITEPRIMARY ? DSERR_INVALIDCALL : DSERR_PRIOLEVELNEEDED;
	EnterCriticalSection(&ds_lock);
	if (flags & DSBLOCK_FROMWRITECURSOR)
		buffer_cursors(buffer, NULL, &offset);
	if (flags & DSBLOCK_ENTIREBUFFER)
		bytes = buffer->data->size;
	const HRESULT result = lock_range(buffer->data, offset, bytes, first, first_bytes, second, second_bytes);
	LeaveCriticalSection(&ds_lock);
	return result;
}

/* The play cursor goes on from where it is under a new rate or looping. */
static void buffer_reanchor(buffer *buffer)
{
	if (!buffer->playing || buffer->ending)
		return;
	DWORD played;
	buffer_cursors(buffer, &played, NULL);
	buffer->start_cursor = (UINT64)(played / buffer->data->format.Format.nBlockAlign) << 32;
	buffer->start_device = device_clock(buffer->device);
	if (buffer->start_device > buffer->device->mixed)
		buffer->start_device = buffer->device->mixed;
}

static HRESULT WINAPI buffer_Play(IDirectSoundBuffer8 *iface, DWORD reserved1, DWORD priority, DWORD flags)
{
	buffer *buffer = buffer_from8(iface);
	if (reserved1)
		return DSERR_INVALIDPARAM;
	if (priority && !(buffer->flags & DSBCAPS_LOCDEFER))
		return DSERR_INVALIDPARAM;
	EnterCriticalSection(&ds_lock);
	if (buffer->primary) {
		buffer->playing = TRUE;
		buffer->looping = TRUE;
		mixer_kick(buffer->device);
		LeaveCriticalSection(&ds_lock);
		return DS_OK;
	}
	if (buffer->flags & DSBCAPS_LOCDEFER)
		buffer->flags |= DSBCAPS_LOCSOFTWARE;
	if (buffer_finished(buffer)) {
		buffer->playing = buffer->ending = FALSE;
		buffer->cursor = 0;
	}
	const BOOL looping = (flags & DSBPLAY_LOOPING) != 0;
	if (looping != buffer->looping)
		buffer_reanchor(buffer);
	buffer->looping = looping;
	if (!buffer->playing) {
		buffer->playing = TRUE;
		buffer->ending = FALSE;
		buffer_anchor(buffer);
		const DWORD size = buffer->data->size;
		buffer->notified = ((DWORD)(buffer->cursor >> 32) * buffer->data->format.Format.nBlockAlign + size - 1) % size;
		mixer_kick(buffer->device);
	} else if (buffer->looping && buffer->ending) {
		/* A buffer whose last frame has been mixed but not yet heard goes on from its start. */
		buffer->ending = FALSE;
		buffer->cursor = 0;
		buffer_anchor(buffer);
		mixer_kick(buffer->device);
	}
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI buffer_SetCurrentPosition(IDirectSoundBuffer8 *iface, DWORD position)
{
	buffer *buffer = buffer_from8(iface);
	if (buffer->primary)
		return DSERR_INVALIDCALL;
	if (position >= buffer->data->size)
		return DSERR_INVALIDPARAM;
	EnterCriticalSection(&ds_lock);
	const DWORD block = buffer->data->format.Format.nBlockAlign;
	buffer->cursor = (UINT64)(position / block) << 32;
	if (buffer->playing) {
		buffer->ending = FALSE;
		buffer_anchor(buffer);
		buffer->notified = (position / block * block + buffer->data->size - 1) % buffer->data->size;
	}
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI buffer_SetFormat(IDirectSoundBuffer8 *iface, const WAVEFORMATEX *format)
{
	buffer *buffer = buffer_from8(iface);
	if (!buffer->primary)
		return DSERR_INVALIDCALL;
	if (buffer->device->level < DSSCL_PRIORITY)
		return DSERR_PRIOLEVELNEEDED;
	buffer_data parsed;
	const HRESULT result = format_check(format, &parsed);
	if (FAILED(result))
		return result;
	/* The device mixes in float whatever the primary format is; the format is what GetFormat reports. */
	EnterCriticalSection(&ds_lock);
	WAVEFORMATEX *stored = &buffer->device->primary_format;
	*stored = parsed.format.Format;
	stored->wFormatTag = parsed.is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
	stored->cbSize = 0;
	stored->nAvgBytesPerSec = stored->nSamplesPerSec * stored->nBlockAlign;
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI buffer_SetVolume(IDirectSoundBuffer8 *iface, LONG volume)
{
	buffer *buffer = buffer_from8(iface);
	if (!(buffer->flags & DSBCAPS_CTRLVOLUME))
		return DSERR_CONTROLUNAVAIL;
	if (volume < DSBVOLUME_MIN || volume > DSBVOLUME_MAX)
		return DSERR_INVALIDPARAM;
	EnterCriticalSection(&ds_lock);
	buffer->volume = volume;
	buffer_gains(buffer);
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI buffer_SetPan(IDirectSoundBuffer8 *iface, LONG pan)
{
	buffer *buffer = buffer_from8(iface);
	if (!(buffer->flags & DSBCAPS_CTRLPAN))
		return DSERR_CONTROLUNAVAIL;
	if (pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT)
		return DSERR_INVALIDPARAM;
	EnterCriticalSection(&ds_lock);
	buffer->pan = pan;
	buffer_gains(buffer);
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI buffer_SetFrequency(IDirectSoundBuffer8 *iface, DWORD frequency)
{
	buffer *buffer = buffer_from8(iface);
	if (buffer->primary || !(buffer->flags & DSBCAPS_CTRLFREQUENCY))
		return DSERR_CONTROLUNAVAIL;
	if (frequency == DSBFREQUENCY_ORIGINAL)
		frequency = buffer->data->format.Format.nSamplesPerSec;
	if (frequency < DSBFREQUENCY_MIN || frequency > DSBFREQUENCY_MAX)
		return DSERR_INVALIDPARAM;
	EnterCriticalSection(&ds_lock);
	buffer_reanchor(buffer);
	buffer->frequency = frequency;
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI buffer_Stop(IDirectSoundBuffer8 *iface)
{
	buffer *buffer = buffer_from8(iface);
	EnterCriticalSection(&ds_lock);
	if (buffer->primary) {
		buffer->playing = FALSE;
	} else if (buffer->playing) {
		if (buffer_finished(buffer)) {
			buffer->cursor = 0;
		} else {
			DWORD played;
			buffer_cursors(buffer, &played, NULL);
			buffer->cursor = (UINT64)(played / buffer->data->format.Format.nBlockAlign) << 32;
		}
		buffer->playing = buffer->ending = FALSE;
		notify_stop(&buffer->notify);
	}
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI buffer_Unlock(IDirectSoundBuffer8 *iface, void *first, DWORD first_bytes, void *second, DWORD second_bytes)
{
	buffer *buffer = buffer_from8(iface);
	if (buffer->primary)
		return DSERR_INVALIDCALL;
	return unlock_range(buffer->data, first, first_bytes, second, second_bytes);
}

static HRESULT WINAPI buffer_Restore(IDirectSoundBuffer8 *iface) { return DS_OK; }

/* The effects are DMOs that are not provided: a program may clear them, and learns that none can be set. */
static HRESULT WINAPI buffer_SetFX(IDirectSoundBuffer8 *iface, DWORD count, DSEFFECTDESC *effects, DWORD *results)
{
	buffer *buffer = buffer_from8(iface);
	if (!(buffer->flags & DSBCAPS_CTRLFX))
		return DSERR_CONTROLUNAVAIL;
	if (count && !effects)
		return DSERR_INVALIDPARAM;
	if (buffer->playing)
		return DSERR_INVALIDCALL;
	if (!count)
		return DS_OK;
	if (results)
		for (DWORD index = 0; index < count; index++)
			results[index] = DSFXR_UNKNOWN;
	return DSERR_FXUNAVAILABLE;
}

static HRESULT WINAPI buffer_AcquireResources(IDirectSoundBuffer8 *iface, DWORD flags, DWORD count, DWORD *results)
{
	buffer *buffer = buffer_from8(iface);
	if (!(buffer->flags & DSBCAPS_LOCDEFER))
		return DSERR_INVALIDCALL;
	if (count && !results)
		return DSERR_INVALIDPARAM;
	for (DWORD index = 0; index < count; index++)
		results[index] = DSFXR_UNKNOWN;
	buffer->flags |= DSBCAPS_LOCSOFTWARE;
	return DS_OK;
}

static HRESULT WINAPI buffer_GetObjectInPath(IDirectSoundBuffer8 *iface, REFGUID object, DWORD index, REFGUID riid, void **out)
{
	buffer *buffer = buffer_from8(iface);
	if (!out)
		return DSERR_INVALIDPARAM;
	*out = NULL;
	return buffer->flags & DSBCAPS_CTRLFX ? DSERR_OBJECTNOTFOUND : DSERR_CONTROLUNAVAIL;
}

static const IDirectSoundBuffer8Vtbl buffer_vtbl = {
	buffer_QueryInterface, buffer_AddRef, buffer_Release, buffer_GetCaps, buffer_GetCurrentPosition, buffer_GetFormat, buffer_GetVolume,
	buffer_GetPan, buffer_GetFrequency, buffer_GetStatus, buffer_Initialize, buffer_Lock, buffer_Play, buffer_SetCurrentPosition,
	buffer_SetFormat, buffer_SetVolume, buffer_SetPan, buffer_SetFrequency, buffer_Stop, buffer_Unlock, buffer_Restore, buffer_SetFX,
	buffer_AcquireResources, buffer_GetObjectInPath,
};

/* ----- IDirectSoundNotify ----- */

static buffer *buffer_from_notify(IDirectSoundNotify *iface) { return CONTAINING_RECORD(iface, buffer, IDirectSoundNotify_iface); }

static HRESULT WINAPI notify_QueryInterface(IDirectSoundNotify *iface, REFIID riid, void **out)
{
	return buffer_QueryInterface(&buffer_from_notify(iface)->IDirectSoundBuffer8_iface, riid, out);
}

static ULONG WINAPI notify_AddRef(IDirectSoundNotify *iface) { return buffer_AddRef(&buffer_from_notify(iface)->IDirectSoundBuffer8_iface); }
static ULONG WINAPI notify_Release(IDirectSoundNotify *iface) { return buffer_Release(&buffer_from_notify(iface)->IDirectSoundBuffer8_iface); }

static HRESULT WINAPI notify_SetNotificationPositions(IDirectSoundNotify *iface, DWORD count, const DSBPOSITIONNOTIFY *items)
{
	buffer *buffer = buffer_from_notify(iface);
	EnterCriticalSection(&ds_lock);
	const HRESULT result = buffer->playing ? DSERR_INVALIDCALL : notify_set(&buffer->notify, buffer->data->size, count, items);
	LeaveCriticalSection(&ds_lock);
	return result;
}

static const IDirectSoundNotifyVtbl notify_vtbl = {
	notify_QueryInterface, notify_AddRef, notify_Release, notify_SetNotificationPositions,
};

/* ----- IDirectSound3DBuffer ----- */

static buffer *buffer_from_3d(IDirectSound3DBuffer *iface) { return CONTAINING_RECORD(iface, buffer, IDirectSound3DBuffer_iface); }

static HRESULT WINAPI buffer3d_QueryInterface(IDirectSound3DBuffer *iface, REFIID riid, void **out)
{
	return buffer_QueryInterface(&buffer_from_3d(iface)->IDirectSoundBuffer8_iface, riid, out);
}

static ULONG WINAPI buffer3d_AddRef(IDirectSound3DBuffer *iface) { return buffer_AddRef(&buffer_from_3d(iface)->IDirectSoundBuffer8_iface); }
static ULONG WINAPI buffer3d_Release(IDirectSound3DBuffer *iface) { return buffer_Release(&buffer_from_3d(iface)->IDirectSoundBuffer8_iface); }

/* A setting applies now, or with the listener's CommitDeferredSettings when DS3D_DEFERRED. */
#define SET_3D(iface, apply, assignment)                                                                                               \
	do {                                                                                                                               \
		buffer *buffer = buffer_from_3d(iface);                                                                                        \
		if ((apply) & ~DS3D_DEFERRED)                                                                                                  \
			return DSERR_INVALIDPARAM;                                                                                                 \
		EnterCriticalSection(&ds_lock);                                                                                                \
		DS3DBUFFER *target = &buffer->pending;                                                                                         \
		assignment;                                                                                                                    \
		if (!((apply) & DS3D_DEFERRED)) {                                                                                              \
			target = &buffer->params;                                                                                                  \
			assignment;                                                                                                                \
			buffer_update_3d(buffer);                                                                                                  \
		}                                                                                                                              \
		LeaveCriticalSection(&ds_lock);                                                                                                \
		return DS_OK;                                                                                                                  \
	} while (0)

static HRESULT WINAPI buffer3d_GetAllParameters(IDirectSound3DBuffer *iface, DS3DBUFFER *params)
{
	if (!params || params->dwSize < sizeof(*params))
		return DSERR_INVALIDPARAM;
	*params = buffer_from_3d(iface)->params;
	return DS_OK;
}

static HRESULT WINAPI buffer3d_GetConeAngles(IDirectSound3DBuffer *iface, DWORD *inside, DWORD *outside)
{
	if (!inside || !outside)
		return DSERR_INVALIDPARAM;
	*inside = buffer_from_3d(iface)->params.dwInsideConeAngle;
	*outside = buffer_from_3d(iface)->params.dwOutsideConeAngle;
	return DS_OK;
}

static HRESULT WINAPI buffer3d_GetConeOrientation(IDirectSound3DBuffer *iface, D3DVECTOR *orientation)
{
	if (!orientation)
		return DSERR_INVALIDPARAM;
	*orientation = buffer_from_3d(iface)->params.vConeOrientation;
	return DS_OK;
}

static HRESULT WINAPI buffer3d_GetConeOutsideVolume(IDirectSound3DBuffer *iface, LONG *volume)
{
	if (!volume)
		return DSERR_INVALIDPARAM;
	*volume = buffer_from_3d(iface)->params.lConeOutsideVolume;
	return DS_OK;
}

static HRESULT WINAPI buffer3d_GetMaxDistance(IDirectSound3DBuffer *iface, D3DVALUE *distance)
{
	if (!distance)
		return DSERR_INVALIDPARAM;
	*distance = buffer_from_3d(iface)->params.flMaxDistance;
	return DS_OK;
}

static HRESULT WINAPI buffer3d_GetMinDistance(IDirectSound3DBuffer *iface, D3DVALUE *distance)
{
	if (!distance)
		return DSERR_INVALIDPARAM;
	*distance = buffer_from_3d(iface)->params.flMinDistance;
	return DS_OK;
}

static HRESULT WINAPI buffer3d_GetMode(IDirectSound3DBuffer *iface, DWORD *mode)
{
	if (!mode)
		return DSERR_INVALIDPARAM;
	*mode = buffer_from_3d(iface)->params.dwMode;
	return DS_OK;
}

static HRESULT WINAPI buffer3d_GetPosition(IDirectSound3DBuffer *iface, D3DVECTOR *position)
{
	if (!position)
		return DSERR_INVALIDPARAM;
	*position = buffer_from_3d(iface)->params.vPosition;
	return DS_OK;
}

static HRESULT WINAPI buffer3d_GetVelocity(IDirectSound3DBuffer *iface, D3DVECTOR *velocity)
{
	if (!velocity)
		return DSERR_INVALIDPARAM;
	*velocity = buffer_from_3d(iface)->params.vVelocity;
	return DS_OK;
}

static HRESULT WINAPI buffer3d_SetAllParameters(IDirectSound3DBuffer *iface, const DS3DBUFFER *params, DWORD apply)
{
	if (!params || params->dwSize < sizeof(*params) || params->dwMode > DS3DMODE_DISABLE)
		return DSERR_INVALIDPARAM;
	SET_3D(iface, apply, *target = *params);
}

static HRESULT WINAPI buffer3d_SetConeAngles(IDirectSound3DBuffer *iface, DWORD inside, DWORD outside, DWORD apply)
{
	if (inside > DS3D_MAXCONEANGLE || outside > DS3D_MAXCONEANGLE)
		return DSERR_INVALIDPARAM;
	SET_3D(iface, apply, (target->dwInsideConeAngle = inside, target->dwOutsideConeAngle = outside));
}

static HRESULT WINAPI buffer3d_SetConeOrientation(IDirectSound3DBuffer *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
	SET_3D(iface, apply, (target->vConeOrientation = (D3DVECTOR){x, y, z}));
}

static HRESULT WINAPI buffer3d_SetConeOutsideVolume(IDirectSound3DBuffer *iface, LONG volume, DWORD apply)
{
	if (volume < DSBVOLUME_MIN || volume > DSBVOLUME_MAX)
		return DSERR_INVALIDPARAM;
	SET_3D(iface, apply, target->lConeOutsideVolume = volume);
}

static HRESULT WINAPI buffer3d_SetMaxDistance(IDirectSound3DBuffer *iface, D3DVALUE distance, DWORD apply)
{
	if (!(distance > 0))
		return DSERR_INVALIDPARAM;
	SET_3D(iface, apply, target->flMaxDistance = distance);
}

static HRESULT WINAPI buffer3d_SetMinDistance(IDirectSound3DBuffer *iface, D3DVALUE distance, DWORD apply)
{
	if (!(distance > 0))
		return DSERR_INVALIDPARAM;
	SET_3D(iface, apply, target->flMinDistance = distance);
}

static HRESULT WINAPI buffer3d_SetMode(IDirectSound3DBuffer *iface, DWORD mode, DWORD apply)
{
	if (mode > DS3DMODE_DISABLE)
		return DSERR_INVALIDPARAM;
	SET_3D(iface, apply, target->dwMode = mode);
}

static HRESULT WINAPI buffer3d_SetPosition(IDirectSound3DBuffer *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
	SET_3D(iface, apply, (target->vPosition = (D3DVECTOR){x, y, z}));
}

static HRESULT WINAPI buffer3d_SetVelocity(IDirectSound3DBuffer *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
	SET_3D(iface, apply, (target->vVelocity = (D3DVECTOR){x, y, z}));
}

static const IDirectSound3DBufferVtbl buffer3d_vtbl = {
	buffer3d_QueryInterface, buffer3d_AddRef, buffer3d_Release, buffer3d_GetAllParameters, buffer3d_GetConeAngles,
	buffer3d_GetConeOrientation, buffer3d_GetConeOutsideVolume, buffer3d_GetMaxDistance, buffer3d_GetMinDistance, buffer3d_GetMode,
	buffer3d_GetPosition, buffer3d_GetVelocity, buffer3d_SetAllParameters, buffer3d_SetConeAngles, buffer3d_SetConeOrientation,
	buffer3d_SetConeOutsideVolume, buffer3d_SetMaxDistance, buffer3d_SetMinDistance, buffer3d_SetMode, buffer3d_SetPosition,
	buffer3d_SetVelocity,
};

/* ----- IDirectSound3DListener, on the primary buffer ----- */

static buffer *buffer_from_listener(IDirectSound3DListener *iface) { return CONTAINING_RECORD(iface, buffer, IDirectSound3DListener_iface); }

static HRESULT WINAPI listener_QueryInterface(IDirectSound3DListener *iface, REFIID riid, void **out)
{
	return buffer_QueryInterface(&buffer_from_listener(iface)->IDirectSoundBuffer8_iface, riid, out);
}

static ULONG WINAPI listener_AddRef(IDirectSound3DListener *iface)
{
	return buffer_AddRef(&buffer_from_listener(iface)->IDirectSoundBuffer8_iface);
}

static ULONG WINAPI listener_Release(IDirectSound3DListener *iface)
{
	return buffer_Release(&buffer_from_listener(iface)->IDirectSoundBuffer8_iface);
}

static void listener_apply(device *device)
{
	for (buffer *each = device->buffers; each; each = each->next)
		buffer_update_3d(each);
}

#define SET_LISTENER(iface, apply, assignment)                                                                                         \
	do {                                                                                                                               \
		device *device = buffer_from_listener(iface)->device;                                                                          \
		if ((apply) & ~DS3D_DEFERRED)                                                                                                  \
			return DSERR_INVALIDPARAM;                                                                                                 \
		EnterCriticalSection(&ds_lock);                                                                                                \
		DS3DLISTENER *target = &device->listener_pending;                                                                              \
		assignment;                                                                                                                    \
		if (!((apply) & DS3D_DEFERRED)) {                                                                                              \
			target = &device->listener;                                                                                                \
			assignment;                                                                                                                \
			listener_apply(device);                                                                                                    \
		}                                                                                                                              \
		LeaveCriticalSection(&ds_lock);                                                                                                \
		return DS_OK;                                                                                                                  \
	} while (0)

static HRESULT WINAPI listener_GetAllParameters(IDirectSound3DListener *iface, DS3DLISTENER *params)
{
	if (!params || params->dwSize < sizeof(*params))
		return DSERR_INVALIDPARAM;
	*params = buffer_from_listener(iface)->device->listener;
	return DS_OK;
}

static HRESULT WINAPI listener_GetDistanceFactor(IDirectSound3DListener *iface, D3DVALUE *factor)
{
	if (!factor)
		return DSERR_INVALIDPARAM;
	*factor = buffer_from_listener(iface)->device->listener.flDistanceFactor;
	return DS_OK;
}

static HRESULT WINAPI listener_GetDopplerFactor(IDirectSound3DListener *iface, D3DVALUE *factor)
{
	if (!factor)
		return DSERR_INVALIDPARAM;
	*factor = buffer_from_listener(iface)->device->listener.flDopplerFactor;
	return DS_OK;
}

static HRESULT WINAPI listener_GetOrientation(IDirectSound3DListener *iface, D3DVECTOR *front, D3DVECTOR *top)
{
	if (!front || !top)
		return DSERR_INVALIDPARAM;
	*front = buffer_from_listener(iface)->device->listener.vOrientFront;
	*top = buffer_from_listener(iface)->device->listener.vOrientTop;
	return DS_OK;
}

static HRESULT WINAPI listener_GetPosition(IDirectSound3DListener *iface, D3DVECTOR *position)
{
	if (!position)
		return DSERR_INVALIDPARAM;
	*position = buffer_from_listener(iface)->device->listener.vPosition;
	return DS_OK;
}

static HRESULT WINAPI listener_GetRolloffFactor(IDirectSound3DListener *iface, D3DVALUE *factor)
{
	if (!factor)
		return DSERR_INVALIDPARAM;
	*factor = buffer_from_listener(iface)->device->listener.flRolloffFactor;
	return DS_OK;
}

static HRESULT WINAPI listener_GetVelocity(IDirectSound3DListener *iface, D3DVECTOR *velocity)
{
	if (!velocity)
		return DSERR_INVALIDPARAM;
	*velocity = buffer_from_listener(iface)->device->listener.vVelocity;
	return DS_OK;
}

static HRESULT WINAPI listener_SetAllParameters(IDirectSound3DListener *iface, const DS3DLISTENER *params, DWORD apply)
{
	if (!params || params->dwSize < sizeof(*params))
		return DSERR_INVALIDPARAM;
	SET_LISTENER(iface, apply, *target = *params);
}

static HRESULT WINAPI listener_SetDistanceFactor(IDirectSound3DListener *iface, D3DVALUE factor, DWORD apply)
{
	if (!(factor > 0))
		return DSERR_INVALIDPARAM;
	SET_LISTENER(iface, apply, target->flDistanceFactor = factor);
}

static HRESULT WINAPI listener_SetDopplerFactor(IDirectSound3DListener *iface, D3DVALUE factor, DWORD apply)
{
	if (factor < DS3D_MINDOPPLERFACTOR || factor > DS3D_MAXDOPPLERFACTOR)
		return DSERR_INVALIDPARAM;
	SET_LISTENER(iface, apply, target->flDopplerFactor = factor);
}

static HRESULT WINAPI listener_SetOrientation(IDirectSound3DListener *iface, D3DVALUE front_x, D3DVALUE front_y, D3DVALUE front_z,
	D3DVALUE top_x, D3DVALUE top_y, D3DVALUE top_z, DWORD apply)
{
	SET_LISTENER(iface, apply,
		(target->vOrientFront = (D3DVECTOR){front_x, front_y, front_z}, target->vOrientTop = (D3DVECTOR){top_x, top_y, top_z}));
}

static HRESULT WINAPI listener_SetPosition(IDirectSound3DListener *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
	SET_LISTENER(iface, apply, (target->vPosition = (D3DVECTOR){x, y, z}));
}

static HRESULT WINAPI listener_SetRolloffFactor(IDirectSound3DListener *iface, D3DVALUE factor, DWORD apply)
{
	if (factor < DS3D_MINROLLOFFFACTOR || factor > DS3D_MAXROLLOFFFACTOR)
		return DSERR_INVALIDPARAM;
	SET_LISTENER(iface, apply, target->flRolloffFactor = factor);
}

static HRESULT WINAPI listener_SetVelocity(IDirectSound3DListener *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
	SET_LISTENER(iface, apply, (target->vVelocity = (D3DVECTOR){x, y, z}));
}

static HRESULT WINAPI listener_CommitDeferredSettings(IDirectSound3DListener *iface)
{
	device *device = buffer_from_listener(iface)->device;
	EnterCriticalSection(&ds_lock);
	device->listener = device->listener_pending;
	for (buffer *each = device->buffers; each; each = each->next)
		each->params = each->pending;
	listener_apply(device);
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static const IDirectSound3DListenerVtbl listener_vtbl = {
	listener_QueryInterface, listener_AddRef, listener_Release, listener_GetAllParameters, listener_GetDistanceFactor,
	listener_GetDopplerFactor, listener_GetOrientation, listener_GetPosition, listener_GetRolloffFactor, listener_GetVelocity,
	listener_SetAllParameters, listener_SetDistanceFactor, listener_SetDopplerFactor, listener_SetOrientation, listener_SetPosition,
	listener_SetRolloffFactor, listener_SetVelocity, listener_CommitDeferredSettings,
};

/* ----- Making buffers ----- */

static void buffer_interfaces(buffer *buffer)
{
	buffer->IDirectSoundBuffer8_iface.lpVtbl = &buffer_vtbl;
	buffer->IDirectSoundNotify_iface.lpVtbl = &notify_vtbl;
	buffer->IDirectSound3DBuffer_iface.lpVtbl = &buffer3d_vtbl;
	buffer->IDirectSound3DListener_iface.lpVtbl = &listener_vtbl;
}

void buffer_init_primary(device *device)
{
	buffer *primary = &device->primary;
	buffer_interfaces(primary);
	primary->device = device;
	primary->primary = TRUE;
	primary->is8 = device->is8;
	buffer_gains(primary);
	device->primary_made = TRUE;
}

static buffer *buffer_alloc(device *device, DWORD flags)
{
	buffer *made = ALLOC(sizeof(*made));
	if (!made)
		return NULL;
	buffer_interfaces(made);
	made->ref = 1;
	made->device = device;
	made->is8 = device->is8;
	made->flags = flags;
	made->params = made->pending = default_3d;
	return made;
}

static void buffer_link(device *device, buffer *made)
{
	EnterCriticalSection(&ds_lock);
	made->next = device->buffers;
	device->buffers = made;
	buffer_gains(made);
	buffer_update_3d(made);
	LeaveCriticalSection(&ds_lock);
}

HRESULT buffer_create(device *device, const DSBUFFERDESC *desc, buffer **out)
{
	buffer_data *data = ALLOC(sizeof(*data));
	if (!data)
		return DSERR_OUTOFMEMORY;
	HRESULT result = format_check(desc->lpwfxFormat, data);
	const DWORD block = data->format.Format.nBlockAlign;
	if (SUCCEEDED(result) && (desc->dwBufferBytes < DSBSIZE_MIN || desc->dwBufferBytes > DSBSIZE_MAX || desc->dwBufferBytes < block))
		result = DSERR_INVALIDPARAM;
	/* 3D sound is positioned mono, which the pan control would contradict. */
	if (SUCCEEDED(result) && (desc->dwFlags & DSBCAPS_CTRL3D) && (data->format.Format.nChannels != 1 || (desc->dwFlags & DSBCAPS_CTRLPAN)))
		result = DSERR_INVALIDPARAM;
	if (SUCCEEDED(result) && (desc->dwFlags & DSBCAPS_CTRLFX) && desc->dwBufferBytes < data->format.Format.nAvgBytesPerSec * DSBSIZE_FX_MIN / 1000)
		result = DSERR_BUFFERTOOSMALL;
	if (SUCCEEDED(result) && (desc->dwFlags & DSBCAPS_LOCHARDWARE) && (desc->dwFlags & (DSBCAPS_LOCSOFTWARE | DSBCAPS_LOCDEFER)))
		result = DSERR_INVALIDPARAM;
	if (FAILED(result)) {
		FREE(data);
		return result;
	}
	data->ref = 1;
	data->size = desc->dwBufferBytes / block * block;
	data->memory = ALLOC(data->size);
	DWORD flags = desc->dwFlags;
	if (!(flags & (DSBCAPS_LOCHARDWARE | DSBCAPS_LOCDEFER)))
		flags |= DSBCAPS_LOCSOFTWARE;
	/* Unsigned 8-bit silence is 0x80. */
	if (data->memory && data->sample_bytes == 1)
		memset(data->memory, 0x80, data->size);
	buffer *made = data->memory ? buffer_alloc(device, flags) : NULL;
	if (!made) {
		FREE(data->memory);
		FREE(data);
		return DSERR_OUTOFMEMORY;
	}
	made->data = data;
	made->frequency = data->format.Format.nSamplesPerSec;
	buffer_link(device, made);
	*out = made;
	return DS_OK;
}

HRESULT buffer_duplicate(device *device, buffer *original, buffer **out)
{
	buffer *made = buffer_alloc(device, original->flags);
	if (!made)
		return DSERR_OUTOFMEMORY;
	EnterCriticalSection(&ds_lock);
	made->data = original->data;
	InterlockedIncrement(&made->data->ref);
	made->is8 = original->is8;
	made->frequency = original->frequency;
	made->volume = original->volume;
	made->pan = original->pan;
	made->params = original->params;
	made->pending = original->pending;
	LeaveCriticalSection(&ds_lock);
	buffer_link(device, made);
	*out = made;
	return DS_OK;
}
