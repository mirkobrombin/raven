#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <dsound.h>
#include <stddef.h>

/* Plays or records one second of a tone through one audio API, for scripts/check-audio-tones.py, which records what the sound server
 * got and checks its frequency, length and gaps. The mode is the first argument; a recording mode takes the file to write as the second:
 *   wasapi waveout dsound playsound mix cap-wasapi cap-wavein cap-dsound
 * The exit code is 0, or the step that failed. */

void *memset(void *target, int value, size_t size)
{
	volatile unsigned char *bytes = target;
	while (size--) *bytes++ = (unsigned char)value;
	return target;
}

typedef struct tone {
	float phase, step;
} tone;

/* The sine of a phase in turns, [0, 1): this program links no C runtime. */
static float sine(float turns)
{
	float x = turns < 0.5f ? turns : turns - 1.0f;
	if (x > 0.25f) x = 0.5f - x;
	else if (x < -0.25f) x = -0.5f - x;
	x *= 6.2831853f;
	const float square = x * x;
	return x * (1 - square / 6 * (1 - square / 20 * (1 - square / 42 * (1 - square / 72))));
}

static tone tone_of(float frequency, float rate) { return (tone){0, frequency / rate}; }

/* Half of full scale. */
static float next(tone *wave)
{
	const float value = 0.5f * sine(wave->phase);
	wave->phase += wave->step;
	if (wave->phase >= 1) wave->phase -= 1;
	return value;
}

static void fill16(short *out, DWORD frames, DWORD channels, tone *wave)
{
	for (DWORD frame = 0; frame < frames; frame++) {
		const short value = (short)(next(wave) * 32767.0f);
		for (DWORD channel = 0; channel < channels; channel++) *out++ = value;
	}
}

static WAVEFORMATEX pcm(DWORD rate, WORD channels, WORD bits)
{
	WAVEFORMATEX format = {WAVE_FORMAT_PCM, channels, rate, rate * channels * bits / 8, (WORD)(channels * bits / 8), bits, 0};
	return format;
}

static BYTE recorded[1 << 20];

/* The recording with its format in front: rate, channels, bits, 1 for float. */
static int save(const WCHAR *path, const WAVEFORMATEX *format, BOOL is_float, DWORD bytes)
{
	const DWORD header[4] = {format->nSamplesPerSec, format->nChannels, format->wBitsPerSample, (DWORD)is_float};
	const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written;
	if (file == INVALID_HANDLE_VALUE) return 90;
	if (!WriteFile(file, header, sizeof header, &written, NULL) || !WriteFile(file, recorded, bytes, &written, NULL)) return 91;
	CloseHandle(file);
	return 0;
}

static BOOL float_format(const WAVEFORMATEX *format)
{
	return format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
		(format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((const WAVEFORMATEXTENSIBLE *)format)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
}

static int open_client(EDataFlow flow, IAudioClient **client, WAVEFORMATEX **format)
{
	IMMDeviceEnumerator *devices;
	IMMDevice *device;
	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 1;
	if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&devices))) return 2;
	if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(devices, flow, eConsole, &device))) return 3;
	if (FAILED(IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)client))) return 4;
	if (FAILED(IAudioClient_GetMixFormat(*client, format))) return 5;
	if ((*format)->wBitsPerSample != (float_format(*format) ? 32 : 16)) return 6;
	if (FAILED(IAudioClient_Initialize(*client, AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, *format, NULL))) return 7;
	return 0;
}

static int wasapi(void)
{
	IAudioClient *client;
	IAudioRenderClient *render;
	WAVEFORMATEX *format;
	UINT32 size, padding;
	const int failed = open_client(eRender, &client, &format);
	if (failed) return failed;
	if (FAILED(IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render))) return 8;
	if (FAILED(IAudioClient_GetBufferSize(client, &size)) || FAILED(IAudioClient_Start(client))) return 9;
	tone wave = tone_of(440, (float)format->nSamplesPerSec);
	for (DWORD left = format->nSamplesPerSec, spins = 0; left; spins++) {
		BYTE *data;
		if (spins > 2000 || FAILED(IAudioClient_GetCurrentPadding(client, &padding))) return 10;
		const UINT32 frames = size - padding < left ? size - padding : left;
		if (!frames) {
			Sleep(5);
			continue;
		}
		if (FAILED(IAudioRenderClient_GetBuffer(render, frames, &data))) return 11;
		if (float_format(format))
			for (UINT32 frame = 0; frame < frames; frame++) {
				const float value = next(&wave);
				for (WORD channel = 0; channel < format->nChannels; channel++) ((float *)data)[frame * format->nChannels + channel] = value;
			}
		else
			fill16((short *)data, frames, format->nChannels, &wave);
		if (FAILED(IAudioRenderClient_ReleaseBuffer(render, frames, 0))) return 12;
		left -= frames;
	}
	for (int spins = 0; spins < 400 && SUCCEEDED(IAudioClient_GetCurrentPadding(client, &padding)) && padding; spins++) Sleep(5);
	if (padding) return 13;
	Sleep(300);
	return FAILED(IAudioClient_Stop(client)) ? 14 : 0;
}

static short samples[48000];

static int waveout(void)
{
	const WAVEFORMATEX format = pcm(22050, 1, 16);
	HWAVEOUT device;
	WAVEHDR header = {0};
	tone wave = tone_of(660, 22050);
	fill16(samples, 22050, 1, &wave);
	header.lpData = (LPSTR)samples;
	header.dwBufferLength = 22050 * 2;
	if (waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL)) return 20;
	if (waveOutPrepareHeader(device, &header, sizeof header) || waveOutWrite(device, &header, sizeof header)) return 21;
	for (int spins = 0; !(header.dwFlags & WHDR_DONE); spins++) {
		if (spins > 1000) return 22;
		Sleep(5);
	}
	Sleep(300);
	return waveOutUnprepareHeader(device, &header, sizeof header) || waveOutClose(device) ? 23 : 0;
}

/* One second of a mono tone in a buffer of its own rate, panned hard when asked. */
static IDirectSoundBuffer *sound_buffer(IDirectSound8 *sound, DWORD rate, float frequency, LONG pan)
{
	WAVEFORMATEX format = pcm(rate, 1, 16);
	DSBUFFERDESC description = {sizeof description, DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLPAN, rate * 2, 0, &format};
	IDirectSoundBuffer *buffer;
	void *first, *second;
	DWORD first_bytes, second_bytes;
	tone wave = tone_of(frequency, (float)rate);
	if (FAILED(IDirectSound8_CreateSoundBuffer(sound, &description, &buffer, NULL))) return NULL;
	if (FAILED(IDirectSoundBuffer_Lock(buffer, 0, rate * 2, &first, &first_bytes, &second, &second_bytes, 0)) || first_bytes != rate * 2)
		return NULL;
	fill16(first, rate, 1, &wave);
	if (FAILED(IDirectSoundBuffer_Unlock(buffer, first, first_bytes, second, second_bytes))) return NULL;
	return FAILED(IDirectSoundBuffer_SetPan(buffer, pan)) ? NULL : buffer;
}

static int dsound(BOOL mix)
{
	IDirectSound8 *sound;
	IDirectSoundBuffer *buffers[2] = {NULL, NULL};
	if (FAILED(DirectSoundCreate8(NULL, &sound, NULL))) return 30;
	if (FAILED(IDirectSound8_SetCooperativeLevel(sound, GetDesktopWindow(), DSSCL_PRIORITY))) return 31;
	buffers[0] = mix ? sound_buffer(sound, 44100, 1000, DSBPAN_LEFT) : sound_buffer(sound, 44100, 880, DSBPAN_CENTER);
	if (mix) buffers[1] = sound_buffer(sound, 22050, 1500, DSBPAN_RIGHT);
	if (!buffers[0] || (mix && !buffers[1])) return 32;
	for (int index = 0; index < 2; index++)
		if (buffers[index] && FAILED(IDirectSoundBuffer_Play(buffers[index], 0, 0, 0))) return 33;
	for (int spins = 0;; spins++) {
		DWORD status = 0, playing = 0;
		for (int index = 0; index < 2; index++)
			if (buffers[index] && SUCCEEDED(IDirectSoundBuffer_GetStatus(buffers[index], &status)) && (status & DSBSTATUS_PLAYING)) playing = 1;
		if (!playing) break;
		if (spins > 1000) return 34;
		Sleep(5);
	}
	Sleep(300);
	return 0;
}

static int playsound(void)
{
	/* A WAVE file in memory: 8-bit mono at 11025 Hz. */
	static BYTE file[44 + 11025];
	const DWORD rate = 11025, riff[11] = {0x46464952, 36 + rate, 0x45564157, 0x20746d66, 16, 0x00010001, rate, rate, 0x00080001, 0x61746164, rate};
	tone wave = tone_of(550, (float)rate);
	for (int index = 0; index < 44; index++) file[index] = ((const BYTE *)riff)[index];
	for (DWORD index = 0; index < rate; index++) file[44 + index] = (BYTE)(128 + (int)(next(&wave) * 127.0f));
	if (!PlaySoundW((LPCWSTR)file, NULL, SND_MEMORY | SND_SYNC | SND_NODEFAULT)) return 40;
	Sleep(300);
	return 0;
}

static int capture_wasapi(const WCHAR *path)
{
	IAudioClient *client;
	IAudioCaptureClient *capture;
	WAVEFORMATEX *format;
	DWORD bytes = 0;
	const int failed = open_client(eCapture, &client, &format);
	if (failed) return 50 + failed;
	if (FAILED(IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void **)&capture)) || FAILED(IAudioClient_Start(client))) return 58;
	const DWORD wanted = format->nSamplesPerSec * format->nBlockAlign;
	if (wanted > sizeof recorded) return 59;
	for (int spins = 0; bytes < wanted; spins++) {
		BYTE *data;
		UINT32 frames = 0;
		DWORD flags;
		if (spins > 1000) return 60;
		if (FAILED(IAudioCaptureClient_GetNextPacketSize(capture, &frames)) || !frames) {
			Sleep(5);
			continue;
		}
		if (FAILED(IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL))) return 61;
		DWORD size = frames * format->nBlockAlign;
		if (size > wanted - bytes) size = wanted - bytes;
		for (DWORD index = 0; index < size; index++) recorded[bytes + index] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : data[index];
		bytes += size;
		if (FAILED(IAudioCaptureClient_ReleaseBuffer(capture, frames))) return 62;
	}
	IAudioClient_Stop(client);
	return save(path, format, float_format(format), bytes);
}

static int capture_wavein(const WCHAR *path)
{
	const WAVEFORMATEX format = pcm(16000, 1, 16);
	HWAVEIN device;
	WAVEHDR header = {0};
	header.lpData = (LPSTR)recorded;
	header.dwBufferLength = 16000 * 2;
	if (waveInOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL)) return 70;
	if (waveInPrepareHeader(device, &header, sizeof header) || waveInAddBuffer(device, &header, sizeof header) || waveInStart(device)) return 71;
	for (int spins = 0; !(header.dwFlags & WHDR_DONE); spins++) {
		if (spins > 1000) return 72;
		Sleep(5);
	}
	waveInStop(device);
	return save(path, &format, FALSE, header.dwBytesRecorded);
}

static int capture_dsound(const WCHAR *path)
{
	WAVEFORMATEX format = pcm(22050, 1, 16);
	DSCBUFFERDESC description = {sizeof description, 0, 22050 * 2, 0, &format, 0, NULL};
	IDirectSoundCapture8 *device;
	IDirectSoundCaptureBuffer *buffer;
	void *first, *second;
	DWORD first_bytes, second_bytes, status = 0;
	if (FAILED(DirectSoundCaptureCreate8(NULL, &device, NULL))) return 80;
	if (FAILED(IDirectSoundCapture_CreateCaptureBuffer(device, &description, &buffer, NULL))) return 81;
	if (FAILED(IDirectSoundCaptureBuffer_Start(buffer, 0))) return 82;
	for (int spins = 0; SUCCEEDED(IDirectSoundCaptureBuffer_GetStatus(buffer, &status)) && (status & DSCBSTATUS_CAPTURING); spins++) {
		if (spins > 1000) return 83;
		Sleep(5);
	}
	if (FAILED(IDirectSoundCaptureBuffer_Lock(buffer, 0, 22050 * 2, &first, &first_bytes, &second, &second_bytes, 0))) return 84;
	for (DWORD index = 0; index < first_bytes; index++) recorded[index] = ((const BYTE *)first)[index];
	return save(path, &format, FALSE, first_bytes);
}

/* The argument that starts with `word`, or NULL; *rest is what follows it. */
static BOOL argument(const WCHAR *line, const WCHAR *word, const WCHAR **rest)
{
	for (; *line; line++) {
		if (line[-1] != L' ') continue;
		const WCHAR *at = line, *want = word;
		while (*want && *at == *want) at++, want++;
		if (*want || (*at && *at != L' ')) continue;
		while (*at == L' ') at++;
		*rest = at;
		return TRUE;
	}
	return FALSE;
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW() + 1, *rest;
	if (argument(line, L"wasapi", &rest)) return wasapi();
	if (argument(line, L"waveout", &rest)) return waveout();
	if (argument(line, L"dsound", &rest)) return dsound(FALSE);
	if (argument(line, L"mix", &rest)) return dsound(TRUE);
	if (argument(line, L"playsound", &rest)) return playsound();
	if (argument(line, L"cap-wasapi", &rest)) return capture_wasapi(rest);
	if (argument(line, L"cap-wavein", &rest)) return capture_wavein(rest);
	if (argument(line, L"cap-dsound", &rest)) return capture_dsound(rest);
	return 99;
}
