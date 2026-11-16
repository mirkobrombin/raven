/* SPDX-License-Identifier: MIT
 *
 * Capture buffers: a capture stream of the engine in the buffer's format, whose frames a thread of the buffer copies into the circular
 * buffer each period and whenever the program asks where capture is.
 */
#include "dsound_private.h"

static capture_buffer *capture_buffer_from(IDirectSoundCaptureBuffer8 *iface)
{
	return CONTAINING_RECORD(iface, capture_buffer, IDirectSoundCaptureBuffer8_iface);
}

/* Called with the lock held: moves what the device captured into the buffer, setting the events of the offsets it fills. */
static void capture_pump(capture_buffer *buffer)
{
	if (!buffer->capturing)
		return;
	RavenAudioShared *shared = buffer->shared;
	const BYTE *ring = (const BYTE *)shared + RAVEN_AUDIO_DATA_OFFSET;
	const UINT ring_frames = shared->buffer_frames;
	const DWORD block = buffer->data->format.Format.nBlockAlign, size = buffer->data->size;
	const UINT64 written = load64(&shared->write_frames);
	UINT64 read = load64(&shared->read_frames);
	if (written - read > ring_frames)
		read = written - ring_frames;
	while (read < written && buffer->capturing) {
		const UINT offset = (UINT)(read % ring_frames);
		DWORD frames = (DWORD)(written - read);
		if (frames > ring_frames - offset)
			frames = ring_frames - offset;
		if (frames > (size - buffer->position) / block)
			frames = (size - buffer->position) / block;
		const DWORD bytes = frames * block, from = buffer->position;
		memcpy(buffer->data->memory + from, ring + (SIZE_T)offset * block, bytes);
		read += frames;
		buffer->position += bytes;
		if (bytes == size) {
			for (DWORD index = 0; index < buffer->notify.count; index++)
				if (buffer->notify.items[index].dwOffset != (DWORD)DSBPN_OFFSETSTOP)
					SetEvent(buffer->notify.items[index].hEventNotify);
		} else {
			/* An offset is passed once the byte there is captured. */
			notify_range(&buffer->notify, (from + size - 1) % size, buffer->position - 1, size);
		}
		if (buffer->position >= size) {
			buffer->position = 0;
			if (!buffer->looping) {
				buffer->capturing = FALSE;
				RavenAudioStop(buffer->stream);
				notify_stop(&buffer->notify);
			}
		}
	}
	store64(&shared->read_frames, read);
}

static DWORD WINAPI capture_main(void *context)
{
	capture_buffer *buffer = context;
	const HANDLE waits[] = {buffer->quit_event, buffer->period_event};
	for (;;) {
		if (WaitForMultipleObjects(2, waits, FALSE, buffer->capturing ? 20 : INFINITE) == WAIT_OBJECT_0)
			return 0;
		EnterCriticalSection(&ds_lock);
		capture_pump(buffer);
		LeaveCriticalSection(&ds_lock);
	}
}

static HRESULT WINAPI capture_buffer_QueryInterface(IDirectSoundCaptureBuffer8 *iface, REFIID riid, void **out)
{
	capture_buffer *buffer = capture_buffer_from(iface);
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IDirectSoundCaptureBuffer) ||
		same_guid(riid, &IID_IDirectSoundCaptureBuffer8))
		*out = &buffer->IDirectSoundCaptureBuffer8_iface;
	else if (same_guid(riid, &IID_IDirectSoundNotify))
		*out = &buffer->IDirectSoundNotify_iface;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&buffer->ref);
	return S_OK;
}

static ULONG WINAPI capture_buffer_AddRef(IDirectSoundCaptureBuffer8 *iface) { return InterlockedIncrement(&capture_buffer_from(iface)->ref); }

void capture_buffer_free(capture_buffer *buffer)
{
	if (buffer->thread) {
		SetEvent(buffer->quit_event);
		WaitForSingleObject(buffer->thread, INFINITE);
		CloseHandle(buffer->thread);
	}
	if (buffer->stream)
		RavenAudioClose(buffer->stream);
	if (buffer->period_event)
		CloseHandle(buffer->period_event);
	if (buffer->quit_event)
		CloseHandle(buffer->quit_event);
	FREE(buffer->notify.items);
	if (buffer->data) {
		FREE(buffer->data->memory);
		FREE(buffer->data);
	}
	FREE(buffer);
}

static ULONG WINAPI capture_buffer_Release(IDirectSoundCaptureBuffer8 *iface)
{
	capture_buffer *buffer = capture_buffer_from(iface);
	const LONG ref = InterlockedDecrement(&buffer->ref);
	if (ref)
		return ref;
	EnterCriticalSection(&ds_lock);
	if (buffer->capture && buffer->capture->buffer == buffer)
		buffer->capture->buffer = NULL;
	LeaveCriticalSection(&ds_lock);
	capture_buffer_free(buffer);
	return 0;
}

static HRESULT WINAPI capture_buffer_GetCaps(IDirectSoundCaptureBuffer8 *iface, DSCBCAPS *caps)
{
	capture_buffer *buffer = capture_buffer_from(iface);
	if (!caps || caps->dwSize < sizeof(*caps))
		return DSERR_INVALIDPARAM;
	caps->dwFlags = buffer->flags;
	caps->dwBufferBytes = buffer->data->size;
	caps->dwReserved = 0;
	return DS_OK;
}

static HRESULT WINAPI capture_buffer_GetCurrentPosition(IDirectSoundCaptureBuffer8 *iface, DWORD *captured, DWORD *readable)
{
	capture_buffer *buffer = capture_buffer_from(iface);
	if (!captured && !readable)
		return DSERR_INVALIDPARAM;
	EnterCriticalSection(&ds_lock);
	capture_pump(buffer);
	if (captured)
		*captured = buffer->position;
	if (readable)
		*readable = buffer->position;
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI capture_buffer_GetFormat(IDirectSoundCaptureBuffer8 *iface, WAVEFORMATEX *format, DWORD size, DWORD *written)
{
	return format_copy(&capture_buffer_from(iface)->data->format.Format, format, size, written);
}

static HRESULT WINAPI capture_buffer_GetStatus(IDirectSoundCaptureBuffer8 *iface, DWORD *status)
{
	capture_buffer *buffer = capture_buffer_from(iface);
	if (!status)
		return DSERR_INVALIDPARAM;
	EnterCriticalSection(&ds_lock);
	capture_pump(buffer);
	*status = buffer->capturing ? DSCBSTATUS_CAPTURING | (buffer->looping ? DSCBSTATUS_LOOPING : 0) : 0;
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI capture_buffer_Initialize(IDirectSoundCaptureBuffer8 *iface, IDirectSoundCapture *capture, const DSCBUFFERDESC *desc)
{
	return DSERR_ALREADYINITIALIZED;
}

static HRESULT WINAPI capture_buffer_Lock(IDirectSoundCaptureBuffer8 *iface, DWORD offset, DWORD bytes, void **first, DWORD *first_bytes,
	void **second, DWORD *second_bytes, DWORD flags)
{
	capture_buffer *buffer = capture_buffer_from(iface);
	if (flags & DSCBLOCK_ENTIREBUFFER)
		bytes = buffer->data->size;
	return lock_range(buffer->data, offset, bytes, first, first_bytes, second, second_bytes);
}

static HRESULT WINAPI capture_buffer_Start(IDirectSoundCaptureBuffer8 *iface, DWORD flags)
{
	capture_buffer *buffer = capture_buffer_from(iface);
	EnterCriticalSection(&ds_lock);
	buffer->looping = (flags & DSCBSTART_LOOPING) != 0;
	if (!buffer->capturing) {
		RavenAudioReset(buffer->stream);
		buffer->capturing = RavenAudioStart(buffer->stream);
	}
	const HRESULT result = buffer->capturing ? DS_OK : DSERR_NODRIVER;
	LeaveCriticalSection(&ds_lock);
	return result;
}

static HRESULT WINAPI capture_buffer_Stop(IDirectSoundCaptureBuffer8 *iface)
{
	capture_buffer *buffer = capture_buffer_from(iface);
	EnterCriticalSection(&ds_lock);
	if (buffer->capturing) {
		capture_pump(buffer);
		if (buffer->capturing) {
			buffer->capturing = FALSE;
			RavenAudioStop(buffer->stream);
			notify_stop(&buffer->notify);
		}
	}
	LeaveCriticalSection(&ds_lock);
	return DS_OK;
}

static HRESULT WINAPI capture_buffer_Unlock(IDirectSoundCaptureBuffer8 *iface, void *first, DWORD first_bytes, void *second, DWORD second_bytes)
{
	return unlock_range(capture_buffer_from(iface)->data, first, first_bytes, second, second_bytes);
}

static HRESULT WINAPI capture_buffer_GetObjectInPath(IDirectSoundCaptureBuffer8 *iface, REFGUID object, DWORD index, REFGUID riid, void **out)
{
	if (!out)
		return DSERR_INVALIDPARAM;
	*out = NULL;
	return DSERR_OBJECTNOTFOUND;
}

static HRESULT WINAPI capture_buffer_GetFXStatus(IDirectSoundCaptureBuffer8 *iface, DWORD count, DWORD *status)
{
	if (count && !status)
		return DSERR_INVALIDPARAM;
	for (DWORD index = 0; index < count; index++)
		status[index] = DSCFX_LOCSOFTWARE;
	return count ? DSERR_INVALIDPARAM : DS_OK;
}

static const IDirectSoundCaptureBuffer8Vtbl capture_buffer_vtbl = {
	capture_buffer_QueryInterface, capture_buffer_AddRef, capture_buffer_Release, capture_buffer_GetCaps, capture_buffer_GetCurrentPosition,
	capture_buffer_GetFormat, capture_buffer_GetStatus, capture_buffer_Initialize, capture_buffer_Lock, capture_buffer_Start,
	capture_buffer_Stop, capture_buffer_Unlock, capture_buffer_GetObjectInPath, capture_buffer_GetFXStatus,
};

/* ----- IDirectSoundNotify ----- */

static capture_buffer *capture_buffer_from_notify(IDirectSoundNotify *iface)
{
	return CONTAINING_RECORD(iface, capture_buffer, IDirectSoundNotify_iface);
}

static HRESULT WINAPI capture_notify_QueryInterface(IDirectSoundNotify *iface, REFIID riid, void **out)
{
	return capture_buffer_QueryInterface(&capture_buffer_from_notify(iface)->IDirectSoundCaptureBuffer8_iface, riid, out);
}

static ULONG WINAPI capture_notify_AddRef(IDirectSoundNotify *iface)
{
	return capture_buffer_AddRef(&capture_buffer_from_notify(iface)->IDirectSoundCaptureBuffer8_iface);
}

static ULONG WINAPI capture_notify_Release(IDirectSoundNotify *iface)
{
	return capture_buffer_Release(&capture_buffer_from_notify(iface)->IDirectSoundCaptureBuffer8_iface);
}

static HRESULT WINAPI capture_notify_SetNotificationPositions(IDirectSoundNotify *iface, DWORD count, const DSBPOSITIONNOTIFY *items)
{
	capture_buffer *buffer = capture_buffer_from_notify(iface);
	EnterCriticalSection(&ds_lock);
	const HRESULT result = buffer->capturing ? DSERR_INVALIDCALL : notify_set(&buffer->notify, buffer->data->size, count, items);
	LeaveCriticalSection(&ds_lock);
	return result;
}

static const IDirectSoundNotifyVtbl capture_notify_vtbl = {
	capture_notify_QueryInterface, capture_notify_AddRef, capture_notify_Release, capture_notify_SetNotificationPositions,
};

HRESULT capture_buffer_create(capture *capture, const DSCBUFFERDESC *desc, capture_buffer **out)
{
	capture_buffer *made = ALLOC(sizeof(*made));
	buffer_data *data = ALLOC(sizeof(*data));
	if (!made || !data) {
		FREE(made);
		FREE(data);
		return DSERR_OUTOFMEMORY;
	}
	made->IDirectSoundCaptureBuffer8_iface.lpVtbl = &capture_buffer_vtbl;
	made->IDirectSoundNotify_iface.lpVtbl = &capture_notify_vtbl;
	made->ref = 1;
	made->capture = capture;
	made->flags = desc->dwFlags;
	made->data = data;
	data->ref = 1;
	HRESULT result = format_check(desc->lpwfxFormat, data);
	const DWORD block = data->format.Format.nBlockAlign;
	if (SUCCEEDED(result) && (data->format.Format.nSamplesPerSec < 1000 || data->format.Format.nSamplesPerSec > 384000))
		result = DSERR_BADFORMAT;
	if (SUCCEEDED(result) && (desc->dwBufferBytes < block || desc->dwBufferBytes > DSBSIZE_MAX))
		result = DSERR_INVALIDPARAM;
	if (SUCCEEDED(result)) {
		data->size = desc->dwBufferBytes / block * block;
		data->memory = ALLOC(data->size);
		if (!data->memory)
			result = DSERR_OUTOFMEMORY;
	}
	if (SUCCEEDED(result)) {
		/* The engine takes the plain format the extensible one describes. */
		WAVEFORMATEX plain = data->format.Format;
		plain.wFormatTag = data->is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
		plain.cbSize = 0;
		made->period_event = CreateEventW(NULL, FALSE, FALSE, NULL);
		made->quit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (made->period_event && made->quit_event)
			made->stream = RavenAudioOpen(&plain, RAVEN_AUDIO_CAPTURE, 0, 0, made->period_event, &made->shared);
		if (made->stream)
			made->thread = CreateThread(NULL, 0, capture_main, made, 0, NULL);
		if (!made->thread)
			result = DSERR_NODRIVER;
	}
	if (FAILED(result)) {
		capture_buffer_free(made);
		return result;
	}
	*out = made;
	return DS_OK;
}
