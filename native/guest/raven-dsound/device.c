/* SPDX-License-Identifier: MIT
 *
 * The device objects (IDirectSound8, IDirectSoundCapture, IDirectSoundFullDuplex), the enumeration of the two devices, and the library's
 * class objects.
 */
#include "dsound_private.h"

CRITICAL_SECTION ds_lock;

#define RENDER_ID "{0.0.0.00000000}.{5A1D0E01-7A3C-4B8E-9A52-524156454E01}"
#define CAPTURE_ID "{0.0.1.00000000}.{5A1D0E02-7A3C-4B8E-9A52-524156454E02}"

/* ----- Arithmetic without a C library ----- */

static double exp_of(double value)
{
	static const double ln2 = 0.69314718055994530942;
	int halvings = 0;
	while (value > 0.5) {
		value -= ln2;
		halvings++;
	}
	while (value < -0.5) {
		value += ln2;
		halvings--;
	}
	double term = 1, sum = 1;
	for (int k = 1; k < 24; k++) {
		term *= value / k;
		sum += term;
	}
	for (; halvings > 0; halvings--)
		sum *= 2;
	for (; halvings < 0; halvings++)
		sum /= 2;
	return sum;
}

/* A DirectSound attenuation, in hundredths of a decibel, as a linear gain: 10^(hundredths / 2000). */
float gain_of(LONG hundredths)
{
	if (hundredths <= DSBVOLUME_MIN)
		return 0;
	if (hundredths >= 0)
		return 1;
	return (float)exp_of(hundredths / 2000.0 * 2.30258509299404568402);
}

float root_of(float value)
{
	if (value <= 0)
		return 0;
	float root = value > 1 ? value : 1;
	for (int step = 0; step < 32; step++) {
		const float next = (root + value / root) / 2;
		if (next == root)
			break;
		root = next;
	}
	return root;
}

static const GUID null_guid;

static BOOL is_render(const GUID *guid)
{
	return !guid || same_guid(guid, &null_guid) || same_guid(guid, &DSDEVID_DefaultPlayback) ||
		same_guid(guid, &DSDEVID_DefaultVoicePlayback) || same_guid(guid, &render_guid);
}

static BOOL is_capture(const GUID *guid)
{
	return !guid || same_guid(guid, &null_guid) || same_guid(guid, &DSDEVID_DefaultCapture) ||
		same_guid(guid, &DSDEVID_DefaultVoiceCapture) || same_guid(guid, &capture_guid);
}

/* ----- IDirectSound8 ----- */

static device *device_from(IDirectSound8 *iface) { return CONTAINING_RECORD(iface, device, IDirectSound8_iface); }

static HRESULT WINAPI device_QueryInterface(IDirectSound8 *iface, REFIID riid, void **out)
{
	device *device = device_from(iface);
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IDirectSound) || (device->is8 && same_guid(riid, &IID_IDirectSound8))) {
		*out = iface;
		IDirectSound8_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI device_AddRef(IDirectSound8 *iface) { return InterlockedIncrement(&device_from(iface)->ref); }

static ULONG WINAPI device_Release(IDirectSound8 *iface)
{
	device *device = device_from(iface);
	const LONG ref = InterlockedDecrement(&device->ref);
	if (ref)
		return ref;
	/* The device's buffers go with it, as on Windows: a program that releases the device first holds pointers it must not use. */
	mixer_close(device);
	EnterCriticalSection(&ds_lock);
	while (device->buffers) {
		buffer *next = device->buffers->next;
		buffer_free(device->buffers);
		device->buffers = next;
	}
	LeaveCriticalSection(&ds_lock);
	FREE(device);
	return 0;
}

static HRESULT WINAPI device_CreateSoundBuffer(IDirectSound8 *iface, const DSBUFFERDESC *desc, IDirectSoundBuffer **out, IUnknown *outer)
{
	device *device = device_from(iface);
	if (!out)
		return DSERR_INVALIDPARAM;
	*out = NULL;
	if (!desc)
		return DSERR_INVALIDPARAM;
	if (outer)
		return DSERR_NOAGGREGATION;
	if (!device->initialized)
		return DSERR_UNINITIALIZED;
	if (desc->dwSize != sizeof(DSBUFFERDESC) && desc->dwSize != sizeof(DSBUFFERDESC1))
		return DSERR_INVALIDPARAM;
	if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) {
		if (desc->lpwfxFormat || desc->dwBufferBytes || (desc->dwFlags & (DSBCAPS_CTRLFX | DSBCAPS_CTRLPOSITIONNOTIFY)))
			return DSERR_INVALIDPARAM;
		EnterCriticalSection(&ds_lock);
		if (!device->primary_made)
			buffer_init_primary(device);
		else if (!device->primary.ref)
			buffer_reset_primary(&device->primary);
		/* A program that asks for the primary buffer again shares the one there is, with the controls asked for last. */
		device->primary.flags = desc->dwFlags | DSBCAPS_LOCSOFTWARE;
		InterlockedIncrement(&device->primary.ref);
		LeaveCriticalSection(&ds_lock);
		*out = (IDirectSoundBuffer *)&device->primary.IDirectSoundBuffer8_iface;
		return DS_OK;
	}
	if ((desc->dwFlags & DSBCAPS_CTRLFX) && !device->is8)
		return DSERR_INVALIDPARAM;
	buffer *made;
	const HRESULT result = buffer_create(device, desc, &made);
	if (FAILED(result))
		return result;
	*out = (IDirectSoundBuffer *)&made->IDirectSoundBuffer8_iface;
	return DS_OK;
}

static HRESULT WINAPI device_GetCaps(IDirectSound8 *iface, DSCAPS *caps)
{
	device *device = device_from(iface);
	if (!device->initialized)
		return DSERR_UNINITIALIZED;
	if (!caps || caps->dwSize < sizeof(*caps))
		return DSERR_INVALIDPARAM;
	const DWORD size = caps->dwSize;
	memset(caps, 0, size);
	caps->dwSize = size;
	caps->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT | DSCAPS_CONTINUOUSRATE |
		DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT;
	caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
	caps->dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
	caps->dwPrimaryBuffers = 1;
	caps->dwUnlockTransferRateHwBuffers = 4096;
	return DS_OK;
}

static HRESULT WINAPI device_DuplicateSoundBuffer(IDirectSound8 *iface, IDirectSoundBuffer *original, IDirectSoundBuffer **out)
{
	device *device = device_from(iface);
	if (!device->initialized)
		return DSERR_UNINITIALIZED;
	if (!original || !out) {
		if (out)
			*out = NULL;
		return DSERR_INVALIDPARAM;
	}
	*out = NULL;
	buffer *source = buffer_from(original), *made;
	if (source->primary || (source->flags & DSBCAPS_CTRLFX))
		return DSERR_INVALIDCALL;
	const HRESULT result = buffer_duplicate(device, source, &made);
	if (FAILED(result))
		return result;
	*out = (IDirectSoundBuffer *)&made->IDirectSoundBuffer8_iface;
	return DS_OK;
}

static HRESULT WINAPI device_SetCooperativeLevel(IDirectSound8 *iface, HWND window, DWORD level)
{
	device *device = device_from(iface);
	if (!device->initialized)
		return DSERR_UNINITIALIZED;
	if (level < DSSCL_NORMAL || level > DSSCL_WRITEPRIMARY)
		return DSERR_INVALIDPARAM;
	device->level = level;
	return DS_OK;
}

static HRESULT WINAPI device_Compact(IDirectSound8 *iface)
{
	device *device = device_from(iface);
	if (!device->initialized)
		return DSERR_UNINITIALIZED;
	return device->level < DSSCL_PRIORITY ? DSERR_PRIOLEVELNEEDED : DS_OK;
}

static HRESULT WINAPI device_GetSpeakerConfig(IDirectSound8 *iface, DWORD *config)
{
	device *device = device_from(iface);
	if (!device->initialized)
		return DSERR_UNINITIALIZED;
	if (!config)
		return DSERR_INVALIDPARAM;
	*config = device->speaker;
	return DS_OK;
}

static HRESULT WINAPI device_SetSpeakerConfig(IDirectSound8 *iface, DWORD config)
{
	device *device = device_from(iface);
	if (!device->initialized)
		return DSERR_UNINITIALIZED;
	if ((config & 0xff) > DSSPEAKER_7POINT1_SURROUND || (config & 0xff) == 0)
		return DSERR_INVALIDPARAM;
	device->speaker = config;
	return DS_OK;
}

static HRESULT WINAPI device_Initialize(IDirectSound8 *iface, const GUID *guid)
{
	device *device = device_from(iface);
	if (device->initialized)
		return DSERR_ALREADYINITIALIZED;
	return device_initialize(device, guid);
}

static HRESULT WINAPI device_VerifyCertification(IDirectSound8 *iface, DWORD *certified)
{
	device *device = device_from(iface);
	if (!device->initialized)
		return DSERR_UNINITIALIZED;
	if (!certified)
		return DSERR_INVALIDPARAM;
	*certified = DS_UNCERTIFIED;
	return DS_OK;
}

static const IDirectSound8Vtbl device_vtbl = {
	device_QueryInterface, device_AddRef, device_Release, device_CreateSoundBuffer, device_GetCaps, device_DuplicateSoundBuffer,
	device_SetCooperativeLevel, device_Compact, device_GetSpeakerConfig, device_SetSpeakerConfig, device_Initialize, device_VerifyCertification,
};

HRESULT device_initialize(device *device, const GUID *guid)
{
	if (!is_render(guid))
		return DSERR_NODRIVER;
	const HRESULT result = mixer_open(device);
	if (FAILED(result))
		return result;
	device->initialized = TRUE;
	return DS_OK;
}

HRESULT device_create(REFIID riid, void **out, BOOL is8)
{
	device *made = ALLOC(sizeof(*made));
	if (!made)
		return E_OUTOFMEMORY;
	made->IDirectSound8_iface.lpVtbl = &device_vtbl;
	made->ref = 1;
	made->is8 = is8;
	made->level = DSSCL_NORMAL;
	made->speaker = DSSPEAKER_COMBINED(DSSPEAKER_STEREO, DSSPEAKER_GEOMETRY_WIDE);
	made->primary_format = (WAVEFORMATEX){WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
	made->listener = (DS3DLISTENER){sizeof(DS3DLISTENER), {0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, DS3D_DEFAULTDISTANCEFACTOR,
		DS3D_DEFAULTROLLOFFFACTOR, DS3D_DEFAULTDOPPLERFACTOR};
	made->listener_pending = made->listener;
	const HRESULT result = IDirectSound8_QueryInterface(&made->IDirectSound8_iface, riid, out);
	IDirectSound8_Release(&made->IDirectSound8_iface);
	return result;
}

/* ----- IDirectSoundCapture ----- */

static capture *capture_from(IDirectSoundCapture *iface) { return CONTAINING_RECORD(iface, capture, IDirectSoundCapture_iface); }

static HRESULT WINAPI capture_QueryInterface(IDirectSoundCapture *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IDirectSoundCapture)) {
		*out = iface;
		IDirectSoundCapture_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI capture_AddRef(IDirectSoundCapture *iface) { return InterlockedIncrement(&capture_from(iface)->ref); }

static ULONG WINAPI capture_Release(IDirectSoundCapture *iface)
{
	capture *capture = capture_from(iface);
	const LONG ref = InterlockedDecrement(&capture->ref);
	if (ref)
		return ref;
	/* A buffer the program still holds outlives its capture object. */
	EnterCriticalSection(&ds_lock);
	if (capture->buffer)
		capture->buffer->capture = NULL;
	LeaveCriticalSection(&ds_lock);
	FREE(capture);
	return 0;
}

static HRESULT WINAPI capture_CreateCaptureBuffer(IDirectSoundCapture *iface, const DSCBUFFERDESC *desc, IDirectSoundCaptureBuffer **out,
	IUnknown *outer)
{
	capture *capture = capture_from(iface);
	if (!out)
		return DSERR_INVALIDPARAM;
	*out = NULL;
	if (outer)
		return DSERR_NOAGGREGATION;
	if (!capture->initialized)
		return DSERR_UNINITIALIZED;
	if (!desc || (desc->dwSize != sizeof(DSCBUFFERDESC) && desc->dwSize != sizeof(DSCBUFFERDESC1)))
		return DSERR_INVALIDPARAM;
	if (capture->buffer)
		return DSERR_ALLOCATED;
	capture_buffer *made;
	const HRESULT result = capture_buffer_create(capture, desc, &made);
	if (FAILED(result))
		return result;
	capture->buffer = made;
	*out = (IDirectSoundCaptureBuffer *)&made->IDirectSoundCaptureBuffer8_iface;
	return DS_OK;
}

static HRESULT WINAPI capture_GetCaps(IDirectSoundCapture *iface, DSCCAPS *caps)
{
	capture *capture = capture_from(iface);
	if (!capture->initialized)
		return DSERR_UNINITIALIZED;
	if (!caps || caps->dwSize < sizeof(*caps))
		return DSERR_INVALIDPARAM;
	caps->dwFlags = 0;
	/* Every WAVE_FORMAT_ rate, width and channel combination from 11 to 96 kHz. */
	caps->dwFormats = 0x000fffff;
	caps->dwChannels = 2;
	return DS_OK;
}

static HRESULT WINAPI capture_Initialize(IDirectSoundCapture *iface, const GUID *guid)
{
	capture *capture = capture_from(iface);
	if (capture->initialized)
		return DSERR_ALREADYINITIALIZED;
	return capture_initialize(capture, guid);
}

static const IDirectSoundCaptureVtbl capture_vtbl = {
	capture_QueryInterface, capture_AddRef, capture_Release, capture_CreateCaptureBuffer, capture_GetCaps, capture_Initialize,
};

HRESULT capture_initialize(capture *capture, const GUID *guid)
{
	if (!is_capture(guid))
		return DSERR_NODRIVER;
	capture->initialized = TRUE;
	return DS_OK;
}

HRESULT capture_create(REFIID riid, void **out)
{
	capture *made = ALLOC(sizeof(*made));
	if (!made)
		return E_OUTOFMEMORY;
	made->IDirectSoundCapture_iface.lpVtbl = &capture_vtbl;
	made->ref = 1;
	const HRESULT result = IDirectSoundCapture_QueryInterface(&made->IDirectSoundCapture_iface, riid, out);
	IDirectSoundCapture_Release(&made->IDirectSoundCapture_iface);
	return result;
}

/* ----- IDirectSoundFullDuplex ----- */

typedef struct duplex {
	IDirectSoundFullDuplex IDirectSoundFullDuplex_iface;
	LONG ref;
	IDirectSound8 *render;
	IDirectSoundCapture *capture;
} duplex;

static duplex *duplex_from(IDirectSoundFullDuplex *iface) { return CONTAINING_RECORD(iface, duplex, IDirectSoundFullDuplex_iface); }

static HRESULT WINAPI duplex_QueryInterface(IDirectSoundFullDuplex *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IDirectSoundFullDuplex)) {
		*out = iface;
		IDirectSoundFullDuplex_AddRef(iface);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI duplex_AddRef(IDirectSoundFullDuplex *iface) { return InterlockedIncrement(&duplex_from(iface)->ref); }

static ULONG WINAPI duplex_Release(IDirectSoundFullDuplex *iface)
{
	duplex *duplex = duplex_from(iface);
	const LONG ref = InterlockedDecrement(&duplex->ref);
	if (ref)
		return ref;
	if (duplex->capture)
		IDirectSoundCapture_Release(duplex->capture);
	if (duplex->render)
		IDirectSound8_Release(duplex->render);
	FREE(duplex);
	return 0;
}

static HRESULT WINAPI duplex_Initialize(IDirectSoundFullDuplex *iface, const GUID *capture_device, const GUID *render_device,
	const DSCBUFFERDESC *capture_desc, const DSBUFFERDESC *render_desc, HWND window, DWORD level, IDirectSoundCaptureBuffer8 **capture_out,
	IDirectSoundBuffer8 **render_out)
{
	duplex *duplex = duplex_from(iface);
	if (!capture_out || !render_out)
		return DSERR_INVALIDPARAM;
	*capture_out = NULL;
	*render_out = NULL;
	if (duplex->render)
		return DSERR_ALREADYINITIALIZED;
	IDirectSoundCaptureBuffer *capture_buffer = NULL;
	IDirectSoundBuffer *render_buffer = NULL;
	HRESULT result = device_create(&IID_IDirectSound8, (void **)&duplex->render, TRUE);
	if (SUCCEEDED(result))
		result = IDirectSound8_Initialize(duplex->render, render_device);
	if (SUCCEEDED(result))
		result = IDirectSound8_SetCooperativeLevel(duplex->render, window, level);
	if (SUCCEEDED(result))
		result = capture_create(&IID_IDirectSoundCapture, (void **)&duplex->capture);
	if (SUCCEEDED(result))
		result = IDirectSoundCapture_Initialize(duplex->capture, capture_device);
	if (SUCCEEDED(result))
		result = IDirectSoundCapture_CreateCaptureBuffer(duplex->capture, capture_desc, &capture_buffer, NULL);
	if (SUCCEEDED(result))
		result = IDirectSound8_CreateSoundBuffer(duplex->render, render_desc, &render_buffer, NULL);
	if (SUCCEEDED(result))
		result = IDirectSoundCaptureBuffer_QueryInterface(capture_buffer, &IID_IDirectSoundCaptureBuffer8, (void **)capture_out);
	if (SUCCEEDED(result))
		result = IDirectSoundBuffer_QueryInterface(render_buffer, &IID_IDirectSoundBuffer8, (void **)render_out);
	if (capture_buffer)
		IDirectSoundCaptureBuffer_Release(capture_buffer);
	if (render_buffer)
		IDirectSoundBuffer_Release(render_buffer);
	if (FAILED(result)) {
		if (*capture_out)
			IDirectSoundCaptureBuffer8_Release(*capture_out);
		if (*render_out)
			IDirectSoundBuffer8_Release(*render_out);
		*capture_out = NULL;
		*render_out = NULL;
		if (duplex->capture)
			IDirectSoundCapture_Release(duplex->capture);
		if (duplex->render)
			IDirectSound8_Release(duplex->render);
		duplex->capture = NULL;
		duplex->render = NULL;
	}
	return result;
}

static const IDirectSoundFullDuplexVtbl duplex_vtbl = {
	duplex_QueryInterface, duplex_AddRef, duplex_Release, duplex_Initialize,
};

static HRESULT duplex_create(REFIID riid, void **out)
{
	duplex *made = ALLOC(sizeof(*made));
	if (!made)
		return E_OUTOFMEMORY;
	made->IDirectSoundFullDuplex_iface.lpVtbl = &duplex_vtbl;
	made->ref = 1;
	const HRESULT result = IDirectSoundFullDuplex_QueryInterface(&made->IDirectSoundFullDuplex_iface, riid, out);
	IDirectSoundFullDuplex_Release(&made->IDirectSoundFullDuplex_iface);
	return result;
}

/* ----- Exports ----- */

static HRESULT create_device(const GUID *guid, IDirectSound8 **out, IUnknown *outer, BOOL is8)
{
	if (!out)
		return DSERR_INVALIDPARAM;
	*out = NULL;
	if (outer)
		return DSERR_NOAGGREGATION;
	IDirectSound8 *made;
	HRESULT result = device_create(is8 ? &IID_IDirectSound8 : &IID_IDirectSound, (void **)&made, is8);
	if (FAILED(result))
		return result;
	result = IDirectSound8_Initialize(made, guid);
	if (FAILED(result)) {
		IDirectSound8_Release(made);
		return result;
	}
	*out = made;
	return DS_OK;
}

HRESULT WINAPI DirectSoundCreate(const GUID *guid, IDirectSound **out, IUnknown *outer)
{
	return create_device(guid, (IDirectSound8 **)out, outer, FALSE);
}

HRESULT WINAPI DirectSoundCreate8(const GUID *guid, IDirectSound8 **out, IUnknown *outer)
{
	return create_device(guid, out, outer, TRUE);
}

static HRESULT create_capture(const GUID *guid, IDirectSoundCapture **out, IUnknown *outer)
{
	if (!out)
		return DSERR_INVALIDPARAM;
	*out = NULL;
	if (outer)
		return DSERR_NOAGGREGATION;
	IDirectSoundCapture *made;
	HRESULT result = capture_create(&IID_IDirectSoundCapture, (void **)&made);
	if (FAILED(result))
		return result;
	result = IDirectSoundCapture_Initialize(made, guid);
	if (FAILED(result)) {
		IDirectSoundCapture_Release(made);
		return result;
	}
	*out = made;
	return DS_OK;
}

HRESULT WINAPI DirectSoundCaptureCreate(const GUID *guid, IDirectSoundCapture **out, IUnknown *outer)
{
	return create_capture(guid, out, outer);
}

HRESULT WINAPI DirectSoundCaptureCreate8(const GUID *guid, IDirectSoundCapture **out, IUnknown *outer)
{
	return create_capture(guid, out, outer);
}

HRESULT WINAPI DirectSoundFullDuplexCreate(const GUID *capture_device, const GUID *render_device, const DSCBUFFERDESC *capture_desc,
	const DSBUFFERDESC *render_desc, HWND window, DWORD level, IDirectSoundFullDuplex **out, IDirectSoundCaptureBuffer8 **capture_out,
	IDirectSoundBuffer8 **render_out, IUnknown *outer)
{
	if (capture_out)
		*capture_out = NULL;
	if (render_out)
		*render_out = NULL;
	if (!out)
		return DSERR_INVALIDPARAM;
	*out = NULL;
	if (outer)
		return DSERR_NOAGGREGATION;
	IDirectSoundFullDuplex *made;
	HRESULT result = duplex_create(&IID_IDirectSoundFullDuplex, (void **)&made);
	if (FAILED(result))
		return result;
	result = IDirectSoundFullDuplex_Initialize(made, capture_device, render_device, capture_desc, render_desc, window, level, capture_out,
		render_out);
	if (FAILED(result)) {
		IDirectSoundFullDuplex_Release(made);
		return result;
	}
	*out = made;
	return DS_OK;
}

/* The primary driver first, with no GUID, then the device itself, as Windows lists them. */
HRESULT WINAPI DirectSoundEnumerateA(LPDSENUMCALLBACKA callback, void *context)
{
	if (!callback)
		return DSERR_INVALIDPARAM;
	GUID device = render_guid;
	if (callback(NULL, "Primary Sound Driver", "", context))
		callback(&device, "Speakers (Raven Audio)", RENDER_ID, context);
	return DS_OK;
}

HRESULT WINAPI DirectSoundEnumerateW(LPDSENUMCALLBACKW callback, void *context)
{
	if (!callback)
		return DSERR_INVALIDPARAM;
	GUID device = render_guid;
	if (callback(NULL, L"Primary Sound Driver", L"", context))
		callback(&device, L"Speakers (Raven Audio)", L"" RENDER_ID, context);
	return DS_OK;
}

HRESULT WINAPI DirectSoundCaptureEnumerateA(LPDSENUMCALLBACKA callback, void *context)
{
	if (!callback)
		return DSERR_INVALIDPARAM;
	GUID device = capture_guid;
	if (callback(NULL, "Primary Sound Capture Driver", "", context))
		callback(&device, "Microphone (Raven Audio)", CAPTURE_ID, context);
	return DS_OK;
}

HRESULT WINAPI DirectSoundCaptureEnumerateW(LPDSENUMCALLBACKW callback, void *context)
{
	if (!callback)
		return DSERR_INVALIDPARAM;
	GUID device = capture_guid;
	if (callback(NULL, L"Primary Sound Capture Driver", L"", context))
		callback(&device, L"Microphone (Raven Audio)", L"" CAPTURE_ID, context);
	return DS_OK;
}

HRESULT WINAPI GetDeviceID(const GUID *source, GUID *destination)
{
	if (!source || !destination)
		return DSERR_INVALIDPARAM;
	if (same_guid(source, &DSDEVID_DefaultPlayback) || same_guid(source, &DSDEVID_DefaultVoicePlayback) || same_guid(source, &render_guid))
		*destination = render_guid;
	else if (same_guid(source, &DSDEVID_DefaultCapture) || same_guid(source, &DSDEVID_DefaultVoiceCapture) || same_guid(source, &capture_guid))
		*destination = capture_guid;
	else
		return DSERR_INVALIDPARAM;
	return DS_OK;
}

/* ----- Class objects ----- */

typedef struct factory {
	IClassFactory IClassFactory_iface;
	const GUID *clsid;
} factory;

static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IClassFactory *iface) { return 2; }
static ULONG WINAPI factory_Release(IClassFactory *iface) { return 1; }

/* An object a class object makes is uninitialized: the program calls Initialize on it, as DirectSoundCreate does. */
static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
	const GUID *clsid = CONTAINING_RECORD(iface, factory, IClassFactory_iface)->clsid;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	if (same_guid(clsid, &CLSID_DirectSound))
		return device_create(riid, out, FALSE);
	if (same_guid(clsid, &CLSID_DirectSound8))
		return device_create(riid, out, TRUE);
	if (same_guid(clsid, &CLSID_DirectSoundFullDuplex))
		return duplex_create(riid, out);
	return capture_create(riid, out);
}

static HRESULT WINAPI factory_LockServer(IClassFactory *iface, BOOL lock) { return S_OK; }

static const IClassFactoryVtbl factory_vtbl = {
	factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer,
};

static factory factories[] = {
	{{&factory_vtbl}, &CLSID_DirectSound}, {{&factory_vtbl}, &CLSID_DirectSound8}, {{&factory_vtbl}, &CLSID_DirectSoundCapture},
	{{&factory_vtbl}, &CLSID_DirectSoundCapture8}, {{&factory_vtbl}, &CLSID_DirectSoundFullDuplex},
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	for (size_t index = 0; index < sizeof(factories) / sizeof(*factories); index++)
		if (same_guid(clsid, factories[index].clsid))
			return factory_QueryInterface(&factories[index].IClassFactory_iface, riid, out);
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(instance);
		InitializeCriticalSection(&ds_lock);
	}
	return TRUE;
}
