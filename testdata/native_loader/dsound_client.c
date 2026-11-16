#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <ks.h>
#include <ksmedia.h>
#include <stddef.h>

/* dsound.dll, the guest DirectSound, on the device that plays to nowhere in real time (RAVEN_AUDIO=null):
 * - the exports answer by name and by ordinal, enumerate the primary driver and the Raven endpoint, and map the default device ids;
 * - IDirectSoundBuffer8 is a DirectSound8 device's alone, and controls a buffer was not created with are unavailable;
 * - secondary buffers lock round their end, play at the pace of their frequency, loop until stopped, and set the events of the offsets
 *   they pass and of their stop; duplicates share samples but not cursors;
 * - 3D buffers need mono and take deferred settings once the listener commits them, and only a priority device sets the primary format;
 * - a capture buffer fills at the pace of its format, notifies, stops when full unless looping, and outlives its capture object;
 * - COM creates uninitialized objects, and the full duplex object makes both halves. */

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

static const GUID null_guid;

static BOOL same(const GUID *left, const GUID *right)
{
	const unsigned char *a = (const unsigned char *)left, *b = (const unsigned char *)right;
	for (int index = 0; index < 16; index++)
		if (a[index] != b[index])
			return FALSE;
	return TRUE;
}

static WAVEFORMATEX pcm(DWORD rate, WORD channels, WORD bits)
{
	WAVEFORMATEX format = {0};
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = channels;
	format.nSamplesPerSec = rate;
	format.wBitsPerSample = bits;
	format.nBlockAlign = (WORD)(channels * bits / 8);
	format.nAvgBytesPerSec = rate * format.nBlockAlign;
	return format;
}

static IDirectSoundBuffer *make(IDirectSound8 *sound, DWORD flags, DWORD bytes, WAVEFORMATEX *format, HRESULT *result)
{
	DSBUFFERDESC desc = {0};
	desc.dwSize = sizeof(desc);
	desc.dwFlags = flags;
	desc.dwBufferBytes = bytes;
	desc.lpwfxFormat = format;
	IDirectSoundBuffer *buffer = NULL;
	*result = IDirectSound8_CreateSoundBuffer(sound, &desc, &buffer, NULL);
	return buffer;
}

/* ----- Exports ----- */

static LONG enumerated;
static BOOL first_null;

static BOOL CALLBACK count_devices(GUID *guid, const WCHAR *description, const WCHAR *module, void *context)
{
	if (!enumerated++)
		first_null = guid == NULL;
	return TRUE;
}

static int exports(void)
{
	HMODULE library = GetModuleHandleA("dsound.dll");
	if (!library)
		return 100;
	if (GetProcAddress(library, (const char *)11) != GetProcAddress(library, "DirectSoundCreate8") || !GetProcAddress(library, (const char *)11))
		return 101;
	if (GetProcAddress(library, (const char *)1) != GetProcAddress(library, "DirectSoundCreate") || !GetProcAddress(library, (const char *)6))
		return 102;
	GUID render, capture, again;
	if (GetDeviceID(&DSDEVID_DefaultPlayback, &render) != DS_OK || GetDeviceID(&DSDEVID_DefaultCapture, &capture) != DS_OK)
		return 103;
	if (same(&render, &capture) || same(&render, &null_guid))
		return 104;
	if (GetDeviceID(&render, &again) != DS_OK || !same(&render, &again))
		return 105;
	if (GetDeviceID(&DSDEVID_DefaultVoiceCapture, &again) != DS_OK || !same(&capture, &again))
		return 106;
	if (DirectSoundEnumerateW(count_devices, NULL) != DS_OK || enumerated != 2 || !first_null)
		return 107;
	enumerated = 0;
	if (DirectSoundCaptureEnumerateW(count_devices, NULL) != DS_OK || enumerated != 2 || !first_null)
		return 108;
	return 0;
}

/* ----- Playback ----- */

static int controls(void)
{
	IDirectSound *plain = NULL;
	if (DirectSoundCreate(NULL, &plain, NULL) != DS_OK)
		return 200;
	IDirectSound8 *upgraded = NULL;
	if (IDirectSound_QueryInterface(plain, &IID_IDirectSound8, (void **)&upgraded) != E_NOINTERFACE || upgraded)
		return 201;
	if (IDirectSound_SetCooperativeLevel(plain, GetDesktopWindow(), DSSCL_NORMAL) != DS_OK)
		return 202;
	WAVEFORMATEX format = pcm(48000, 2, 16);
	HRESULT result;
	IDirectSoundBuffer *old = make((IDirectSound8 *)plain, DSBCAPS_CTRLVOLUME, 4800, &format, &result);
	IDirectSoundBuffer8 *old8 = NULL;
	if (result != DS_OK || IDirectSoundBuffer_QueryInterface(old, &IID_IDirectSoundBuffer8, (void **)&old8) != E_NOINTERFACE)
		return 203;
	IDirectSoundBuffer_Release(old);
	IDirectSound_Release(plain);

	IDirectSound8 *sound = NULL;
	if (DirectSoundCreate8(NULL, &sound, NULL) != DS_OK || IDirectSound8_SetCooperativeLevel(sound, GetDesktopWindow(), DSSCL_NORMAL) != DS_OK)
		return 204;
	IDirectSoundBuffer *buffer = make(sound, DSBCAPS_CTRLVOLUME, 4800, &format, &result);
	IDirectSoundBuffer8 *buffer8 = NULL;
	if (result != DS_OK || IDirectSoundBuffer_QueryInterface(buffer, &IID_IDirectSoundBuffer8, (void **)&buffer8) != DS_OK)
		return 205;
	IDirectSoundBuffer8_Release(buffer8);
	LONG volume = 0, pan;
	DWORD frequency;
	if (IDirectSoundBuffer_SetVolume(buffer, 1) != DSERR_INVALIDPARAM || IDirectSoundBuffer_SetVolume(buffer, -10001) != DSERR_INVALIDPARAM)
		return 206;
	if (IDirectSoundBuffer_SetVolume(buffer, -600) != DS_OK || IDirectSoundBuffer_GetVolume(buffer, &volume) != DS_OK || volume != -600)
		return 207;
	if (IDirectSoundBuffer_SetPan(buffer, 0) != DSERR_CONTROLUNAVAIL || IDirectSoundBuffer_GetPan(buffer, &pan) != DSERR_CONTROLUNAVAIL)
		return 208;
	if (IDirectSoundBuffer_SetFrequency(buffer, 22050) != DSERR_CONTROLUNAVAIL || IDirectSoundBuffer_GetFrequency(buffer, &frequency) != DSERR_CONTROLUNAVAIL)
		return 209;
	IDirectSoundNotify *notify = NULL;
	IDirectSound3DBuffer *spatial = NULL;
	if (IDirectSoundBuffer_QueryInterface(buffer, &IID_IDirectSoundNotify, (void **)&notify) != E_NOINTERFACE ||
		IDirectSoundBuffer_QueryInterface(buffer, &IID_IDirectSound3DBuffer, (void **)&spatial) != E_NOINTERFACE)
		return 210;
	IDirectSoundBuffer_Release(buffer);

	buffer = make(sound, DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY, 4800, &format, &result);
	if (result != DS_OK)
		return 211;
	if (IDirectSoundBuffer_SetPan(buffer, 10001) != DSERR_INVALIDPARAM || IDirectSoundBuffer_SetPan(buffer, -10000) != DS_OK ||
		IDirectSoundBuffer_GetPan(buffer, &pan) != DS_OK || pan != -10000)
		return 212;
	if (IDirectSoundBuffer_SetFrequency(buffer, 99) != DSERR_INVALIDPARAM || IDirectSoundBuffer_SetFrequency(buffer, 22050) != DS_OK ||
		IDirectSoundBuffer_GetFrequency(buffer, &frequency) != DS_OK || frequency != 22050)
		return 213;
	if (IDirectSoundBuffer_SetFrequency(buffer, DSBFREQUENCY_ORIGINAL) != DS_OK || IDirectSoundBuffer_GetFrequency(buffer, &frequency) != DS_OK ||
		frequency != 48000)
		return 214;
	IDirectSoundBuffer_Release(buffer);

	/* Flags that cannot go together, and sizes and formats a buffer cannot have. */
	WAVEFORMATEX mono = pcm(48000, 1, 16), odd = format;
	odd.nBlockAlign = 3;
	if (make(sound, DSBCAPS_CTRL3D, 4800, &format, &result) || result != DSERR_INVALIDPARAM)
		return 215;
	if (make(sound, DSBCAPS_CTRL3D | DSBCAPS_CTRLPAN, 4800, &mono, &result) || result != DSERR_INVALIDPARAM)
		return 216;
	if (make(sound, 0, 2, &format, &result) || result != DSERR_INVALIDPARAM)
		return 217;
	if (make(sound, 0, 4800, &odd, &result) || SUCCEEDED(result))
		return 218;
	if (make(sound, 0, 4800, NULL, &result) || result != DSERR_INVALIDPARAM)
		return 219;

	/* Only a priority device sets the primary format. */
	IDirectSoundBuffer *primary = make(sound, DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME, 0, NULL, &result);
	if (result != DS_OK || !primary)
		return 220;
	WAVEFORMATEX wanted = pcm(44100, 2, 16), got = {0};
	if (IDirectSoundBuffer_SetFormat(primary, &wanted) != DSERR_PRIOLEVELNEEDED)
		return 221;
	if (IDirectSound8_SetCooperativeLevel(sound, GetDesktopWindow(), DSSCL_PRIORITY) != DS_OK || IDirectSoundBuffer_SetFormat(primary, &wanted) != DS_OK)
		return 222;
	DWORD written = 0;
	if (IDirectSoundBuffer_GetFormat(primary, &got, sizeof(got), &written) != DS_OK || got.nSamplesPerSec != 44100 || got.nChannels != 2)
		return 223;
	void *first, *second;
	DWORD first_bytes, second_bytes;
	if (IDirectSoundBuffer_Lock(primary, 0, 0, &first, &first_bytes, &second, &second_bytes, DSBLOCK_ENTIREBUFFER) != DSERR_PRIOLEVELNEEDED)
		return 224;
	IDirectSoundBuffer *again = make(sound, DSBCAPS_PRIMARYBUFFER, 0, NULL, &result);
	if (result != DS_OK || again != primary || IDirectSoundBuffer_Release(again) != 1 || IDirectSoundBuffer_Release(primary) != 0)
		return 225;
	primary = make(sound, DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME, 0, NULL, &result);
	if (result != DS_OK || IDirectSoundBuffer_GetVolume(primary, &volume) != DS_OK || volume != DSBVOLUME_MAX)
		return 227;
	if (IDirectSoundBuffer_SetVolume(primary, -2000) != DS_OK || IDirectSoundBuffer_Release(primary) != 0)
		return 228;
	primary = make(sound, DSBCAPS_PRIMARYBUFFER, 0, NULL, &result);
	if (result != DS_OK || IDirectSoundBuffer_SetVolume(primary, 0) != DSERR_CONTROLUNAVAIL || IDirectSoundBuffer_Release(primary) != 0)
		return 229;
	/* An extensible format reports its own 22 extra bytes whatever size the program claimed for it. */
	static union {
		WAVEFORMATEXTENSIBLE extensible;
		BYTE bytes[sizeof(WAVEFORMATEX) + 1022];
	} wide;
	wide.extensible.Format = pcm(22050, 1, 8);
	wide.extensible.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	wide.extensible.Format.cbSize = 1022;
	wide.extensible.Samples.wValidBitsPerSample = 8;
	wide.extensible.dwChannelMask = SPEAKER_FRONT_CENTER;
	wide.extensible.SubFormat = (GUID){0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
	IDirectSoundBuffer *quiet = make(sound, 0, 4096, &wide.extensible.Format, &result);
	WAVEFORMATEXTENSIBLE reported = {0};
	DWORD reported_bytes = 0;
	if (result != DS_OK || IDirectSoundBuffer_GetFormat(quiet, NULL, 0, &reported_bytes) != DS_OK || reported_bytes != sizeof(reported) ||
		IDirectSoundBuffer_GetFormat(quiet, &reported.Format, sizeof(reported), NULL) != DS_OK || reported.Format.cbSize != 22)
		return 230;
	/* An 8-bit buffer nobody wrote holds silence, which is 0x80. */
	if (IDirectSoundBuffer_Lock(quiet, 0, 0, &first, &first_bytes, NULL, NULL, DSBLOCK_ENTIREBUFFER) != DS_OK || first_bytes != 4096 ||
		((BYTE *)first)[0] != 0x80 || ((BYTE *)first)[4095] != 0x80 || IDirectSoundBuffer_Unlock(quiet, first, first_bytes, NULL, 0) != DS_OK ||
		IDirectSoundBuffer_Release(quiet) != 0)
		return 231;
	if (IDirectSound8_Release(sound) != 0)
		return 226;
	return 0;
}

static int playback(void)
{
	IDirectSound8 *sound = NULL;
	if (DirectSoundCreate8(&DSDEVID_DefaultPlayback, &sound, NULL) != DS_OK ||
		IDirectSound8_SetCooperativeLevel(sound, GetDesktopWindow(), DSSCL_PRIORITY) != DS_OK)
		return 300;
	/* A quarter second of 48 kHz 16-bit stereo. */
	WAVEFORMATEX format = pcm(48000, 2, 16);
	const DWORD size = 48000;
	HRESULT result;
	IDirectSoundBuffer *buffer =
		make(sound, DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_CTRLFREQUENCY, size, &format, &result);
	if (result != DS_OK)
		return 301;
	void *first, *second;
	DWORD first_bytes, second_bytes;
	if (IDirectSoundBuffer_Lock(buffer, 47000, 2000, &first, &first_bytes, &second, &second_bytes, 0) != DS_OK || first_bytes != 1000 ||
		second_bytes != 1000)
		return 302;
	memset(first, 1, first_bytes);
	memset(second, 2, second_bytes);
	if (IDirectSoundBuffer_Unlock(buffer, first, first_bytes, second, second_bytes) != DS_OK)
		return 303;

	/* A duplicate shares the samples but has a cursor of its own. */
	IDirectSoundBuffer *twin = NULL;
	if (IDirectSound8_DuplicateSoundBuffer(sound, buffer, &twin) != DS_OK || !twin)
		return 304;
	if (IDirectSoundBuffer_Lock(twin, 0, size, &first, &first_bytes, &second, &second_bytes, 0) != DS_OK || first_bytes != size ||
		((BYTE *)first)[0] != 2 || ((BYTE *)first)[47999] != 1)
		return 305;
	IDirectSoundBuffer_Unlock(twin, first, first_bytes, second, second_bytes);
	if (IDirectSoundBuffer_SetCurrentPosition(twin, 24000) != DS_OK || IDirectSoundBuffer_SetCurrentPosition(twin, size) != DSERR_INVALIDPARAM)
		return 306;
	DWORD play = 1, write = 1, twin_play = 0;
	if (IDirectSoundBuffer_GetCurrentPosition(buffer, &play, &write) != DS_OK || play != 0 ||
		IDirectSoundBuffer_GetCurrentPosition(twin, &twin_play, NULL) != DS_OK || twin_play != 24000)
		return 307;
	IDirectSoundBuffer_Release(twin);

	/* Notifications at a quarter and at the stop of a buffer that plays once through. */
	IDirectSoundNotify *notify = NULL;
	if (IDirectSoundBuffer_QueryInterface(buffer, &IID_IDirectSoundNotify, (void **)&notify) != DS_OK)
		return 308;
	const HANDLE zero = CreateEventW(NULL, FALSE, FALSE, NULL), quarter = CreateEventW(NULL, FALSE, FALSE, NULL),
		stopped = CreateEventW(NULL, FALSE, FALSE, NULL);
	DSBPOSITIONNOTIFY positions[] = {{0, zero}, {size / 4, quarter}, {DSBPN_OFFSETSTOP, stopped}};
	DSBPOSITIONNOTIFY wrong = {size, quarter};
	if (IDirectSoundNotify_SetNotificationPositions(notify, DSBNOTIFICATIONS_MAX + 1, &wrong) != DSERR_INVALIDPARAM)
		return 332;
	if (IDirectSoundNotify_SetNotificationPositions(notify, 1, &wrong) != DSERR_INVALIDPARAM)
		return 309;
	if (IDirectSoundNotify_SetNotificationPositions(notify, 3, positions) != DS_OK)
		return 310;
	DWORD status = 99;
	if (IDirectSoundBuffer_GetStatus(buffer, &status) != DS_OK || status != 0)
		return 311;
	DWORD start = GetTickCount();
	if (IDirectSoundBuffer_Play(buffer, 0, 0, 0) != DS_OK || IDirectSoundBuffer_GetStatus(buffer, &status) != DS_OK ||
		status != DSBSTATUS_PLAYING)
		return 312;
	if (IDirectSoundNotify_SetNotificationPositions(notify, 3, positions) != DSERR_INVALIDCALL)
		return 313;
	if (WaitForSingleObject(zero, 2000) != WAIT_OBJECT_0 || GetTickCount() - start < 1)
		return 331;
	if (WaitForSingleObject(quarter, 2000) != WAIT_OBJECT_0 || GetTickCount() - start < 40)
		return 314;
	if (WaitForSingleObject(stopped, 2000) != WAIT_OBJECT_0)
		return 315;
	const DWORD once = GetTickCount() - start;
	if (once < 200 || once > 1000)
		return 316;
	if (IDirectSoundBuffer_GetStatus(buffer, &status) != DS_OK || status != 0)
		return 317;

	/* Twice the frequency plays the same buffer in half the time. */
	if (IDirectSoundBuffer_SetFrequency(buffer, 96000) != DS_OK)
		return 318;
	start = GetTickCount();
	if (IDirectSoundBuffer_Play(buffer, 0, 0, 0) != DS_OK)
		return 319;
	/* The play cursor moves at 384 bytes a millisecond rather than 192, though it starts up to 40 ms late on a device that still plays
	 * out what it mixed for the last buffer; a check that comes after the buffer could have ended proves nothing. */
	Sleep(100);
	DWORD moved = 0;
	IDirectSoundBuffer_GetCurrentPosition(buffer, &moved, NULL);
	const DWORD elapsed = GetTickCount() - start;
	if (elapsed < 115 && moved <= elapsed * 192)
		return 330;
	if (WaitForSingleObject(stopped, 2000) != WAIT_OBJECT_0)
		return 319;
	const DWORD fast = GetTickCount() - start;
	if (fast < 90 || fast + 50 > once)
		return 320;
	IDirectSoundBuffer_SetFrequency(buffer, DSBFREQUENCY_ORIGINAL);

	/* A looping buffer goes round until it is stopped, and its cursor stays where Stop left it. */
	if (IDirectSoundBuffer_Play(buffer, 0, 0, DSBPLAY_LOOPING) != DS_OK)
		return 321;
	if (WaitForSingleObject(stopped, 400) != WAIT_TIMEOUT)
		return 322;
	if (IDirectSoundBuffer_GetStatus(buffer, &status) != DS_OK || status != (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING))
		return 323;
	if (WaitForSingleObject(quarter, 0) != WAIT_OBJECT_0)
		return 324;
	if (IDirectSoundBuffer_Stop(buffer) != DS_OK || WaitForSingleObject(stopped, 0) != WAIT_OBJECT_0)
		return 325;
	DWORD held = 0, later = 0;
	IDirectSoundBuffer_GetCurrentPosition(buffer, &held, NULL);
	Sleep(40);
	IDirectSoundBuffer_GetCurrentPosition(buffer, &later, NULL);
	if (held != later || IDirectSoundBuffer_GetStatus(buffer, &status) != DS_OK || status != 0)
		return 326;
	/* Play goes on from the stopped cursor, and the cursor moves while it plays. */
	if (IDirectSoundBuffer_Play(buffer, 0, 0, DSBPLAY_LOOPING) != DS_OK)
		return 327;
	Sleep(60);
	IDirectSoundBuffer_GetCurrentPosition(buffer, &later, &write);
	if (later == held || IDirectSoundBuffer_Stop(buffer) != DS_OK)
		return 328;

	/* Play without looping on a buffer that has gone round more than once ends the lap it is on: its play cursor stays where it was. */
	IDirectSoundBuffer *round = make(sound, DSBCAPS_GETCURRENTPOSITION2, 19200, &format, &result);
	if (result != DS_OK || IDirectSoundBuffer_Play(round, 0, 0, DSBPLAY_LOOPING) != DS_OK)
		return 333;
	Sleep(250);
	DWORD lapped = 0, ending = 0;
	IDirectSoundBuffer_GetCurrentPosition(round, &lapped, NULL);
	if (IDirectSoundBuffer_Play(round, 0, 0, 0) != DS_OK || IDirectSoundBuffer_GetCurrentPosition(round, &ending, NULL) != DS_OK)
		return 334;
	if (ending < lapped || ending - lapped >= 4800 || IDirectSoundBuffer_Stop(round) != DS_OK || IDirectSoundBuffer_Release(round) != 0)
		return 335;

	/* A buffer that ends before a period has passed still sets the events of its offsets: it starts here on a device at rest, which has
	 * played out what it mixed. The device again plays that out before its stream stops, as a reset drops the frames a sound server
	 * holds: the primary buffer's cursors are apart until then. */
	Sleep(100);
	IDirectSoundBuffer *primary = make(sound, DSBCAPS_PRIMARYBUFFER, 0, NULL, &result);
	IDirectSoundBuffer *brief = make(sound, DSBCAPS_CTRLPOSITIONNOTIFY, 400, &format, &result);
	IDirectSoundNotify *brief_notify = NULL;
	if (!primary || result != DS_OK || IDirectSoundBuffer_QueryInterface(brief, &IID_IDirectSoundNotify, (void **)&brief_notify) != DS_OK)
		return 336;
	const HANDLE begun = CreateEventW(NULL, FALSE, FALSE, NULL), halfway = CreateEventW(NULL, FALSE, FALSE, NULL),
		ended = CreateEventW(NULL, FALSE, FALSE, NULL);
	DSBPOSITIONNOTIFY marks[] = {{0, begun}, {200, halfway}, {DSBPN_OFFSETSTOP, ended}};
	if (IDirectSoundNotify_SetNotificationPositions(brief_notify, 3, marks) != DS_OK || IDirectSoundBuffer_Play(brief, 0, 0, 0) != DS_OK)
		return 337;
	if (WaitForSingleObject(ended, 2000) != WAIT_OBJECT_0)
		return 338;
	DWORD heard = 0, mixed = 0;
	if (IDirectSoundBuffer_GetCurrentPosition(primary, &heard, &mixed) != DS_OK || heard == mixed)
		return 339;
	if (WaitForSingleObject(begun, 0) != WAIT_OBJECT_0 || WaitForSingleObject(halfway, 0) != WAIT_OBJECT_0)
		return 340;
	IDirectSoundNotify_Release(brief_notify);
	if (IDirectSoundBuffer_Release(brief) != 0 || IDirectSoundBuffer_Release(primary) != 0)
		return 341;
	CloseHandle(begun);
	CloseHandle(halfway);
	CloseHandle(ended);

	IDirectSoundNotify_Release(notify);
	if (IDirectSoundBuffer_Release(buffer) != 0 || IDirectSound8_Release(sound) != 0)
		return 329;
	CloseHandle(quarter);
	CloseHandle(stopped);
	CloseHandle(zero);
	return 0;
}

static int spatial(void)
{
	IDirectSound8 *sound = NULL;
	if (DirectSoundCreate8(NULL, &sound, NULL) != DS_OK || IDirectSound8_SetCooperativeLevel(sound, GetDesktopWindow(), DSSCL_NORMAL) != DS_OK)
		return 400;
	WAVEFORMATEX mono = pcm(22050, 1, 16);
	HRESULT result;
	IDirectSoundBuffer *buffer = make(sound, DSBCAPS_CTRL3D, 4410, &mono, &result);
	IDirectSound3DBuffer *voice = NULL;
	if (result != DS_OK || IDirectSoundBuffer_QueryInterface(buffer, &IID_IDirectSound3DBuffer, (void **)&voice) != DS_OK)
		return 401;
	IDirectSoundBuffer *primary = make(sound, DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, 0, NULL, &result);
	IDirectSound3DListener *listener = NULL;
	if (result != DS_OK || IDirectSoundBuffer_QueryInterface(primary, &IID_IDirectSound3DListener, (void **)&listener) != DS_OK)
		return 402;
	D3DVECTOR at = {9, 9, 9};
	if (IDirectSound3DBuffer_SetPosition(voice, 1, 2, 3, DS3D_DEFERRED) != DS_OK || IDirectSound3DBuffer_GetPosition(voice, &at) != DS_OK ||
		at.x != 0 || at.y != 0 || at.z != 0)
		return 403;
	if (IDirectSound3DListener_CommitDeferredSettings(listener) != DS_OK || IDirectSound3DBuffer_GetPosition(voice, &at) != DS_OK ||
		at.x != 1 || at.y != 2 || at.z != 3)
		return 404;
	if (IDirectSound3DBuffer_SetPosition(voice, 4, 5, 6, DS3D_IMMEDIATE) != DS_OK || IDirectSound3DBuffer_GetPosition(voice, &at) != DS_OK ||
		at.x != 4)
		return 405;
	IDirectSound3DBuffer_Release(voice);
	IDirectSound3DListener_Release(listener);
	IDirectSoundBuffer_Release(primary);
	IDirectSoundBuffer_Release(buffer);
	if (IDirectSound8_Release(sound) != 0)
		return 406;
	return 0;
}

/* ----- Capture ----- */

static int recording(void)
{
	IDirectSoundCapture *capture = NULL;
	if (DirectSoundCaptureCreate8(NULL, &capture, NULL) != DS_OK)
		return 500;
	DSCCAPS caps = {sizeof(caps)};
	if (IDirectSoundCapture_GetCaps(capture, &caps) != DS_OK || !caps.dwChannels)
		return 501;
	/* A fifth of a second of 44.1 kHz 16-bit stereo. */
	WAVEFORMATEX format = pcm(44100, 2, 16);
	const DWORD size = 35280;
	DSCBUFFERDESC desc = {sizeof(desc)};
	desc.dwBufferBytes = size;
	desc.lpwfxFormat = &format;
	IDirectSoundCaptureBuffer *buffer = NULL, *extra = NULL;
	WAVEFORMATEX invalid_rate = pcm(500000, 2, 16);
	desc.lpwfxFormat = &invalid_rate;
	const HRESULT rate_result = IDirectSoundCapture_CreateCaptureBuffer(capture, &desc, &extra, NULL);
	if (rate_result != DSERR_INVALIDPARAM && rate_result != DSERR_BADFORMAT)
		return 524;
	desc.lpwfxFormat = &format;
	if (IDirectSoundCapture_CreateCaptureBuffer(capture, &desc, &buffer, NULL) != DS_OK || !buffer)
		return 502;
	if (IDirectSoundCapture_CreateCaptureBuffer(capture, &desc, &extra, NULL) != DSERR_ALLOCATED)
		return 503;
	IDirectSoundNotify *notify = NULL;
	if (IDirectSoundCaptureBuffer_QueryInterface(buffer, &IID_IDirectSoundNotify, (void **)&notify) != DS_OK)
		return 504;
	const HANDLE half = CreateEventW(NULL, FALSE, FALSE, NULL), stopped = CreateEventW(NULL, FALSE, FALSE, NULL);
	DSBPOSITIONNOTIFY positions[] = {{size / 2 - 1, half}, {DSBPN_OFFSETSTOP, stopped}};
	if (IDirectSoundNotify_SetNotificationPositions(notify, 2, positions) != DS_OK)
		return 505;
	DWORD status = 99, captured = 1, readable = 1;
	if (IDirectSoundCaptureBuffer_GetStatus(buffer, &status) != DS_OK || status != 0)
		return 506;
	const DWORD start = GetTickCount();
	if (IDirectSoundCaptureBuffer_Start(buffer, 0) != DS_OK || IDirectSoundCaptureBuffer_GetStatus(buffer, &status) != DS_OK ||
		status != DSCBSTATUS_CAPTURING)
		return 507;
	if (IDirectSoundNotify_SetNotificationPositions(notify, 2, positions) != DSERR_INVALIDCALL)
		return 508;
	if (WaitForSingleObject(half, 2000) != WAIT_OBJECT_0 || GetTickCount() - start < 60)
		return 509;
	if (IDirectSoundCaptureBuffer_GetCurrentPosition(buffer, &captured, &readable) != DS_OK || captured < size / 2 || captured != readable)
		return 510;
	/* A buffer that does not loop stops once it is full. */
	if (WaitForSingleObject(stopped, 2000) != WAIT_OBJECT_0 || GetTickCount() - start < 160)
		return 511;
	if (IDirectSoundCaptureBuffer_GetStatus(buffer, &status) != DS_OK || status != 0)
		return 512;
	void *first, *second;
	DWORD first_bytes, second_bytes;
	if (IDirectSoundCaptureBuffer_Lock(buffer, 0, 0, &first, &first_bytes, &second, &second_bytes, DSCBLOCK_ENTIREBUFFER) != DS_OK ||
		first_bytes != size || second_bytes)
		return 513;
	IDirectSoundCaptureBuffer_Unlock(buffer, first, first_bytes, second, second_bytes);
	/* A looping buffer goes round until it is stopped. */
	if (IDirectSoundCaptureBuffer_Start(buffer, DSCBSTART_LOOPING) != DS_OK)
		return 514;
	if (WaitForSingleObject(stopped, 400) != WAIT_TIMEOUT || IDirectSoundCaptureBuffer_GetStatus(buffer, &status) != DS_OK ||
		status != (DSCBSTATUS_CAPTURING | DSCBSTATUS_LOOPING))
		return 515;
	if (IDirectSoundCaptureBuffer_Stop(buffer) != DS_OK || WaitForSingleObject(stopped, 0) != WAIT_OBJECT_0)
		return 516;
	IDirectSoundNotify_Release(notify);
	if (IDirectSoundCaptureBuffer_Release(buffer) != 0)
		return 517;

	/* Another format once the first buffer is gone, which goes on working after its capture object is released. */
	WAVEFORMATEX narrow = pcm(11025, 1, 8);
	desc.dwBufferBytes = 1103;
	desc.lpwfxFormat = &narrow;
	if (IDirectSoundCapture_CreateCaptureBuffer(capture, &desc, &buffer, NULL) != DS_OK)
		return 518;
	DSCBCAPS buffer_caps = {sizeof(buffer_caps)};
	if (IDirectSoundCaptureBuffer_GetCaps(buffer, &buffer_caps) != DS_OK || buffer_caps.dwBufferBytes != 1103)
		return 519;
	if (IDirectSoundCapture_Release(capture) != 0)
		return 520;
	if (IDirectSoundCaptureBuffer_Start(buffer, DSCBSTART_LOOPING) != DS_OK)
		return 521;
	Sleep(50);
	if (IDirectSoundCaptureBuffer_GetCurrentPosition(buffer, &captured, NULL) != DS_OK ||
		IDirectSoundCaptureBuffer_GetStatus(buffer, &status) != DS_OK || !(status & DSCBSTATUS_CAPTURING))
		return 522;
	if (IDirectSoundCaptureBuffer_Release(buffer) != 0)
		return 523;
	CloseHandle(half);
	CloseHandle(stopped);
	return 0;
}

/* ----- COM ----- */

static int objects(void)
{
	if (FAILED(CoInitialize(NULL)))
		return 600;
	typedef HRESULT(WINAPI *get_class_function)(REFCLSID, REFIID, void **);
	get_class_function get_class = (get_class_function)GetProcAddress(GetModuleHandleA("dsound.dll"), "DllGetClassObject");
	IClassFactory *factory = NULL;
	if (!get_class || get_class(&CLSID_DirectSound8, &IID_IClassFactory, (void **)&factory) != S_OK || !factory)
		return 611;
	IClassFactory_Release(factory);
	IDirectSound8 *sound = NULL;
	if (CoCreateInstance(&CLSID_DirectSound8, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectSound8, (void **)&sound) != S_OK || !sound)
		return 601;
	if (IDirectSound8_SetCooperativeLevel(sound, GetDesktopWindow(), DSSCL_NORMAL) != DSERR_UNINITIALIZED)
		return 602;
	if (IDirectSound8_Initialize(sound, NULL) != DS_OK || IDirectSound8_Initialize(sound, NULL) != DSERR_ALREADYINITIALIZED)
		return 603;
	if (IDirectSound8_SetCooperativeLevel(sound, GetDesktopWindow(), DSSCL_NORMAL) != DS_OK || IDirectSound8_Release(sound) != 0)
		return 604;
	IDirectSound *plain = NULL;
	IDirectSound8 *upgraded = NULL;
	if (CoCreateInstance(&CLSID_DirectSound, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectSound, (void **)&plain) != S_OK ||
		IDirectSound_QueryInterface(plain, &IID_IDirectSound8, (void **)&upgraded) != E_NOINTERFACE)
		return 605;
	IDirectSound_Release(plain);
	IDirectSoundCapture *capture = NULL;
	if (CoCreateInstance(&CLSID_DirectSoundCapture8, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectSoundCapture, (void **)&capture) != S_OK)
		return 606;
	DSCCAPS caps = {sizeof(caps)};
	if (IDirectSoundCapture_GetCaps(capture, &caps) != DSERR_UNINITIALIZED || IDirectSoundCapture_Initialize(capture, NULL) != DS_OK ||
		IDirectSoundCapture_GetCaps(capture, &caps) != DS_OK)
		return 607;
	IDirectSoundCapture_Release(capture);

	WAVEFORMATEX format = pcm(48000, 2, 16);
	DSCBUFFERDESC capture_desc = {sizeof(capture_desc)};
	capture_desc.dwBufferBytes = 19200;
	capture_desc.lpwfxFormat = &format;
	DSBUFFERDESC render_desc = {sizeof(render_desc)};
	render_desc.dwBufferBytes = 19200;
	render_desc.lpwfxFormat = &format;
	IDirectSoundFullDuplex *duplex = NULL;
	IDirectSoundCaptureBuffer8 *recorder = NULL;
	IDirectSoundBuffer8 *player = NULL;
	if (DirectSoundFullDuplexCreate(NULL, NULL, &capture_desc, &render_desc, GetDesktopWindow(), DSSCL_PRIORITY, &duplex, &recorder, &player,
			NULL) != DS_OK || !duplex || !recorder || !player)
		return 608;
	DWORD status = 99;
	if (IDirectSoundBuffer8_GetStatus(player, &status) != DS_OK || IDirectSoundCaptureBuffer8_GetStatus(recorder, &status) != DS_OK)
		return 609;
	IDirectSoundBuffer8_Release(player);
	IDirectSoundCaptureBuffer8_Release(recorder);
	IDirectSoundFullDuplex_Release(duplex);
	CoUninitialize();
	return 0;
}

int raven_entry(void)
{
	int failed = exports();
	if (!failed)
		failed = controls();
	if (!failed)
		failed = playback();
	if (!failed)
		failed = spatial();
	if (!failed)
		failed = recording();
	if (!failed)
		failed = objects();
	return failed;
}
