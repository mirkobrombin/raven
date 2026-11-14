#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>

/* What mmdevapi.dll answers to calls out of order and to arguments out of range: the result codes a program tells the cases apart
   by, and no read past a format or count of frames that wrapped. Nothing here needs sound to come out. */
static const WAVEFORMATEX stereo = {WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0};

static IAudioClient *activate(IMMDevice *device)
{
	IAudioClient *client = NULL;
	IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
	return client;
}

int raven_entry(void)
{
	IMMDeviceEnumerator *enumerator = NULL;
	IMMDevice *device = NULL, *other = NULL;
	IMMDeviceCollection *list = NULL;
	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
		return 1;
	if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&enumerator)))
		return 2;
	if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device)))
		return 3;
	/* A role or a state that does not exist. */
	if (IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, (ERole)99, &other) != E_INVALIDARG)
		return 4;
	if (IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eRender, 0x100, &list) != E_INVALIDARG)
		return 5;

	/* A duration whose count of frames does not fit is refused, not cut to a buffer of a frame or none. */
	static const REFERENCE_TIME wrapping[] = {0x7fffffffffffffffll, 894784853334ll, 894784853400ll, -1};
	for (int index = 0; index < 4; ++index) {
		IAudioClient *client = activate(device);
		UINT32 frames = 0;
		if (!client)
			return 6;
		if (SUCCEEDED(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, wrapping[index], 0, &stereo, NULL)) &&
			(FAILED(IAudioClient_GetBufferSize(client, &frames)) || frames < 480))
			return 10 + index;
		IAudioClient_Release(client);
	}

	/* A format at the end of mapped memory whose cbSize is wild: only the format itself is read. */
	BYTE *pages = VirtualAlloc(NULL, 8192, MEM_RESERVE, PAGE_NOACCESS);
	if (!pages || !VirtualAlloc(pages, 4096, MEM_COMMIT, PAGE_READWRITE))
		return 20;
	WAVEFORMATEX *wild = (WAVEFORMATEX *)(pages + 4096 - sizeof(WAVEFORMATEX));
	*wild = stereo;
	wild->cbSize = 0xffff;
	IAudioClient *client = activate(device);
	if (!client || FAILED(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, wild, NULL)))
		return 21;

	/* The services of a stream that plays. */
	IAudioRenderClient *render = NULL;
	void *capture = NULL;
	if (IAudioClient_GetService(client, &IID_IAudioCaptureClient, &capture) != AUDCLNT_E_WRONG_ENDPOINT_TYPE)
		return 30;
	if (FAILED(IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render)))
		return 31;

	/* A buffer released that was never taken, and one released larger than it was taken, which the program still holds. */
	BYTE *data = NULL;
	if (IAudioRenderClient_ReleaseBuffer(render, 1, 0) != AUDCLNT_E_OUT_OF_ORDER)
		return 40;
	if (FAILED(IAudioRenderClient_GetBuffer(render, 100, &data)))
		return 41;
	if (IAudioRenderClient_ReleaseBuffer(render, 101, 0) != AUDCLNT_E_INVALID_SIZE)
		return 42;
	if (IAudioRenderClient_ReleaseBuffer(render, 100, AUDCLNT_BUFFERFLAGS_SILENT) != S_OK)
		return 43;

	/* Started twice, reset while it runs, and reset while the program holds a buffer. */
	if (FAILED(IAudioClient_Start(client)))
		return 50;
	if (IAudioClient_Start(client) != AUDCLNT_E_NOT_STOPPED)
		return 51;
	if (IAudioClient_Reset(client) != AUDCLNT_E_NOT_STOPPED)
		return 52;
	if (FAILED(IAudioClient_Stop(client)))
		return 53;
	if (FAILED(IAudioRenderClient_GetBuffer(render, 1, &data)))
		return 54;
	if (IAudioClient_Reset(client) != AUDCLNT_E_BUFFER_OPERATION_PENDING)
		return 55;
	if (FAILED(IAudioRenderClient_ReleaseBuffer(render, 0, 0)) || FAILED(IAudioClient_Reset(client)))
		return 56;

	/* A volume that is not a number is no volume. */
	ISimpleAudioVolume *simple = NULL;
	IAudioStreamVolume *levels = NULL;
	IAudioEndpointVolume *endpoint = NULL;
	const union { UINT32 bits; float value; } nan = {0x7fc00000};
	if (FAILED(IAudioClient_GetService(client, &IID_ISimpleAudioVolume, (void **)&simple)) ||
		FAILED(IAudioClient_GetService(client, &IID_IAudioStreamVolume, (void **)&levels)) ||
		FAILED(IMMDevice_Activate(device, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&endpoint)))
		return 60;
	if (ISimpleAudioVolume_SetMasterVolume(simple, nan.value, NULL) != E_INVALIDARG)
		return 61;
	if (IAudioStreamVolume_SetChannelVolume(levels, 0, nan.value) != E_INVALIDARG)
		return 62;
	float pair[2] = {1.0f, nan.value};
	if (IAudioStreamVolume_SetAllVolumes(levels, 2, pair) != E_INVALIDARG)
		return 63;
	if (IAudioEndpointVolume_SetMasterVolumeLevelScalar(endpoint, nan.value, NULL) != E_INVALIDARG)
		return 64;
	if (IAudioEndpointVolume_SetMasterVolumeLevel(endpoint, nan.value, NULL) != E_INVALIDARG)
		return 65;
	float level = 0.0f;
	if (FAILED(ISimpleAudioVolume_GetMasterVolume(simple, &level)) || level != 1.0f)
		return 66;
	IAudioClient_Release(client);

	/* A shared stream of a period the engine does not have. */
	IAudioClient3 *shared = NULL;
	if (FAILED(IMMDevice_Activate(device, &IID_IAudioClient3, CLSCTX_ALL, NULL, (void **)&shared)))
		return 70;
	if (IAudioClient3_InitializeSharedAudioStream(shared, 0, 7, &stereo, NULL) != AUDCLNT_E_INVALID_DEVICE_PERIOD)
		return 71;
	if (FAILED(IAudioClient3_InitializeSharedAudioStream(shared, 0, 480, &stereo, NULL)))
		return 72;
	IAudioClient3_Release(shared);
	return 0;
}
