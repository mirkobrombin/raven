/* SPDX-License-Identifier: MIT
 *
 * The device's mixer: a thread that wakes each period of the device's stream and mixes the playing buffers, resampled, converted and
 * downmixed to float stereo, into the ring of the stream LEAD_PERIODS ahead of what the device has played. The cursors of a buffer follow
 * from the frame the device has reached, which the shared page gives without a call.
 */
#include "dsound_private.h"

static float sample_of(const BYTE *at, UINT bytes, BOOL is_float)
{
	switch (bytes) {
	case 1:
		return (at[0] - 128) / 128.0f;
	case 2:
		return (short)(at[0] | at[1] << 8) / 32768.0f;
	case 3:
		return (int)((UINT)at[0] << 8 | (UINT)at[1] << 16 | (UINT)at[2] << 24) / 2147483648.0f;
	default:
		if (is_float) {
			float value;
			memcpy(&value, at, sizeof(value));
			return value;
		}
		return (int)((UINT)at[0] | (UINT)at[1] << 8 | (UINT)at[2] << 16 | (UINT)at[3] << 24) / 2147483648.0f;
	}
}

/* A frame as stereo: the centre and surround channels of the standard layouts fold into the front pair at -3 dB and the LFE is dropped. */
static void frame_of(const buffer_data *data, DWORD frame, float *left, float *right)
{
	static const float fold = 0.7071f;
	const WORD channels = data->format.Format.nChannels;
	const UINT bytes = data->sample_bytes;
	const BYTE *at = data->memory + (SIZE_T)frame * data->format.Format.nBlockAlign;
	float s[8];
	for (WORD channel = 0; channel < channels; channel++)
		s[channel] = sample_of(at + channel * bytes, bytes, data->is_float);
	switch (channels) {
	case 1:
		*left = *right = s[0];
		return;
	case 2:
		*left = s[0];
		*right = s[1];
		return;
	case 3:
		*left = s[0] + fold * s[2];
		*right = s[1] + fold * s[2];
		return;
	case 4:
		*left = s[0] + fold * s[2];
		*right = s[1] + fold * s[3];
		return;
	case 5:
		*left = s[0] + fold * (s[2] + s[3]);
		*right = s[1] + fold * (s[2] + s[4]);
		return;
	case 6:
		*left = s[0] + fold * (s[2] + s[4]);
		*right = s[1] + fold * (s[2] + s[5]);
		return;
	case 7:
		*left = s[0] + fold * (s[2] + s[4] + s[5]);
		*right = s[1] + fold * (s[2] + s[4] + s[6]);
		return;
	default:
		*left = s[0] + fold * (s[2] + s[4] + s[6]);
		*right = s[1] + fold * (s[2] + s[5] + s[7]);
		return;
	}
}

/* Adds `frames` device frames of the buffer to out, interpolating linearly between its frames at its frequency. A buffer that is not
 * looping and runs out is ending: its last frame sounds at device frame end_device. */
static void mix_buffer(buffer *buffer, float *out, UINT frames, UINT64 device_frame)
{
	const buffer_data *data = buffer->data;
	const DWORD size = data->size / data->format.Format.nBlockAlign;
	const UINT64 step = ((UINT64)buffer->frequency << 32) / MIX_RATE, end = (UINT64)size << 32;
	const float gain_left = buffer->gain_left * buffer->gain_3d_left, gain_right = buffer->gain_right * buffer->gain_3d_right;
	UINT64 cursor = buffer->cursor;
	for (UINT index = 0; index < frames; index++) {
		const DWORD frame = (DWORD)(cursor >> 32);
		const DWORD next = frame + 1 < size ? frame + 1 : buffer->looping ? 0 : frame;
		const float fraction = (float)(cursor & 0xffffffffu) / 4294967296.0f;
		float left, right, next_left, next_right;
		frame_of(data, frame, &left, &right);
		frame_of(data, next, &next_left, &next_right);
		out[index * 2] += (left + (next_left - left) * fraction) * gain_left;
		out[index * 2 + 1] += (right + (next_right - right) * fraction) * gain_right;
		cursor += step;
		if (cursor >= end) {
			if (buffer->looping) {
				cursor %= end;
			} else {
				buffer->ending = TRUE;
				buffer->end_device = device_frame + index + 1;
				cursor = end;
				break;
			}
		}
	}
	buffer->cursor = cursor;
}

void buffer_anchor(buffer *buffer)
{
	buffer->start_device = buffer->device->running ? buffer->device->mixed : 0;
	buffer->start_cursor = buffer->cursor;
	buffer->notified_device = buffer->start_device;
}

/* The play cursor is where the device is in the buffer: the frame it began playing from, advanced by the device frames heard since at the
 * buffer's frequency. The write cursor is the next frame to mix: what lies before it has gone to the device. */
void buffer_cursors(buffer *buffer, DWORD *play, DWORD *write)
{
	const buffer_data *data = buffer->data;
	const DWORD block = data->format.Format.nBlockAlign, size = data->size / block;
	DWORD written = (DWORD)(buffer->cursor >> 32);
	if (written >= size)
		written = 0;
	if (write)
		*write = written * block;
	if (!play)
		return;
	device *device = buffer->device;
	if (!buffer->playing) {
		*play = written * block;
		return;
	}
	const UINT64 heard = device_clock(device);
	if (buffer->ending && device->running && heard >= buffer->end_device) {
		*play = 0;
		return;
	}
	const UINT64 elapsed = heard > buffer->start_device ? heard - buffer->start_device : 0;
	UINT64 frame = (buffer->start_cursor >> 32) +
		(elapsed * buffer->frequency + (((buffer->start_cursor & 0xffffffffu) * MIX_RATE) >> 32)) / MIX_RATE;
	if (buffer->looping)
		frame %= size;
	else if (frame >= size)
		frame = size - 1;
	*play = (DWORD)frame * block;
}

/* A buffer the device has finished playing stops, with the notifications of its remaining offsets and of its stop. */
static void buffer_finish(buffer *buffer)
{
	const DWORD size = buffer->data->size;
	/* A buffer that ends before any period saw it move passed its start offset too, which Play left just after notified. */
	if (buffer->notified_device == buffer->start_device) {
		const DWORD start = (buffer->notified + 1) % size;
		notify_range(&buffer->notify, buffer->notified, start, size);
		buffer->notified = start;
	}
	if (buffer->notified + 1 < size)
		notify_range(&buffer->notify, buffer->notified, size - 1, size);
	buffer->playing = buffer->ending = FALSE;
	buffer->cursor = 0;
	notify_stop(&buffer->notify);
}

/* Sets the events of the offsets the play cursor passed since the last period; a buffer short enough to go round in one period passes
 * them all. */
static void buffer_notify(buffer *buffer, UINT64 heard)
{
	if (!buffer->notify.count)
		return;
	DWORD now;
	buffer_cursors(buffer, &now, NULL);
	const DWORD size = buffer->data->size, block = buffer->data->format.Format.nBlockAlign;
	const UINT64 since = heard > buffer->notified_device ? heard - buffer->notified_device : 0;
	if (!since)
		return;
	if (since * buffer->frequency / MIX_RATE >= size / block) {
		for (DWORD index = 0; index < buffer->notify.count; index++)
			if (buffer->notify.items[index].dwOffset != (DWORD)DSBPN_OFFSETSTOP)
				SetEvent(buffer->notify.items[index].hEventNotify);
	} else if (now != buffer->notified) {
		notify_range(&buffer->notify, buffer->notified, now, size);
	}
	buffer->notified = now;
	buffer->notified_device = heard;
}

/* Called with the lock held: retires the buffers the device has finished, then fills the ring up to LEAD_PERIODS ahead of the device,
 * starting the stream when something plays and stopping it when nothing does. */
static void device_mix(device *device)
{
	RavenAudioShared *shared = device->shared;
	const UINT64 heard = device_clock(device);
	BOOL active = device->primary_made && device->primary.playing;
	for (buffer *each = device->buffers; each; each = each->next) {
		if (!each->playing)
			continue;
		if (each->ending && device->running && heard >= each->end_device)
			buffer_finish(each);
		else
			active = TRUE;
	}
	if (!active) {
		/* Reset drops what the sound server still holds, so the stream stops only once the device has played all that was mixed. */
		if (device->running && load64(&shared->position_frames) >= device->mixed) {
			RavenAudioReset(device->stream);
			device->running = FALSE;
			device->mixed = 0;
		}
		return;
	}
	if (!device->running) {
		RavenAudioReset(device->stream);
		device->mixed = 0;
	}
	float *ring = (float *)((BYTE *)shared + RAVEN_AUDIO_DATA_OFFSET);
	const UINT ring_frames = shared->buffer_frames;
	const UINT64 consumed = load64(&shared->read_frames);
	UINT64 target = heard + LEAD_PERIODS * MIX_PERIOD;
	if (target > consumed + ring_frames)
		target = consumed + ring_frames;
	const float master_left = device->primary_made ? device->primary.gain_left : 1;
	const float master_right = device->primary_made ? device->primary.gain_right : 1;
	while (device->mixed < target) {
		const UINT offset = (UINT)(device->mixed % ring_frames);
		UINT frames = (UINT)(target - device->mixed);
		if (frames > ring_frames - offset)
			frames = ring_frames - offset;
		float *out = ring + offset * 2;
		memset(out, 0, frames * 2 * sizeof(float));
		for (buffer *each = device->buffers; each; each = each->next)
			if (each->playing && !each->ending)
				mix_buffer(each, out, frames, device->mixed);
		for (UINT index = 0; index < frames; index++) {
			float left = out[index * 2] * master_left, right = out[index * 2 + 1] * master_right;
			out[index * 2] = left > 1 ? 1 : left < -1 ? -1 : left;
			out[index * 2 + 1] = right > 1 ? 1 : right < -1 ? -1 : right;
		}
		device->mixed += frames;
	}
	store64(&shared->write_frames, device->mixed);
	if (!device->running)
		device->running = RavenAudioStart(device->stream);
	for (buffer *each = device->buffers; each; each = each->next)
		if (each->playing)
			buffer_notify(each, heard);
}

void mixer_kick(device *device) { device_mix(device); }

static DWORD WINAPI mixer_main(void *context)
{
	device *device = context;
	const HANDLE waits[] = {device->quit_event, device->period_event};
	for (;;) {
		/* The period event drives a running stream, and the timeout covers a period the engine skips; a stopped stream waits for Play,
		 * which starts it itself. */
		if (WaitForMultipleObjects(2, waits, FALSE, device->running ? 20 : INFINITE) == WAIT_OBJECT_0)
			return 0;
		EnterCriticalSection(&ds_lock);
		device_mix(device);
		LeaveCriticalSection(&ds_lock);
	}
}

HRESULT mixer_open(device *device)
{
	const WAVEFORMATEX format = {WAVE_FORMAT_IEEE_FLOAT, 2, MIX_RATE, MIX_RATE * 8, 8, 32, 0};
	device->period_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	device->quit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (device->period_event && device->quit_event)
		device->stream = RavenAudioOpen(&format, 0, MIX_RING, MIX_PERIOD, device->period_event, &device->shared);
	if (device->stream)
		device->thread = CreateThread(NULL, 0, mixer_main, device, 0, NULL);
	if (!device->thread) {
		mixer_close(device);
		return DSERR_NODRIVER;
	}
	return DS_OK;
}

void mixer_close(device *device)
{
	if (device->thread) {
		SetEvent(device->quit_event);
		WaitForSingleObject(device->thread, INFINITE);
		CloseHandle(device->thread);
		device->thread = NULL;
	}
	if (device->stream) {
		RavenAudioClose(device->stream);
		device->stream = NULL;
		device->shared = NULL;
	}
	device->running = FALSE;
	if (device->period_event)
		CloseHandle(device->period_event);
	if (device->quit_event)
		CloseHandle(device->quit_event);
	device->period_event = device->quit_event = NULL;
}
