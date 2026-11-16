#define INITGUID
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>

/* What the audio resampler transform (CLSID_CResamplerMediaObject) makes of sound at another rate: the count of frames that comes
   out, their values, and the answers to calls in the wrong order. */
DEFINE_GUID(CLSID_Resampler, 0xf447b69e, 0x1884, 0x4a7e, 0x80, 0x55, 0x34, 0x6f, 0x74, 0xd6, 0xed, 0xb3);
DEFINE_GUID(IID_ResamplerProps, 0xe7e9984f, 0xf09f, 0x4da4, 0x90, 0x3f, 0x6e, 0x2e, 0x0e, 0xfe, 0x56, 0xb5);

/* IWMResamplerProps, which the MinGW headers do not declare. */
typedef struct props props;
struct props {
	const struct {
		HRESULT (WINAPI *query)(props *, REFIID, void **);
		ULONG (WINAPI *reference)(props *);
		ULONG (WINAPI *release)(props *);
		HRESULT (WINAPI *half_filter_length)(props *, LONG);
		HRESULT (WINAPI *channel_matrix)(props *, void *);
	} *table;
};

static IMFMediaType *audio(BOOL is_float, UINT32 channels, UINT32 rate)
{
	IMFMediaType *type = NULL;
	const UINT32 bits = is_float ? 32 : 16;
	if (FAILED(MFCreateMediaType(&type)))
		return NULL;
	IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
	IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, is_float ? &MFAudioFormat_Float : &MFAudioFormat_PCM);
	IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, channels);
	IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
	IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * bits / 8);
	IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * bits / 8);
	IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
	return type;
}

static IMFSample *sample_of(DWORD size, IMFMediaBuffer **buffer)
{
	IMFSample *sample = NULL;
	if (FAILED(MFCreateMemoryBuffer(size, buffer)) || FAILED(MFCreateSample(&sample)) || FAILED(IMFSample_AddBuffer(sample, *buffer)))
		return NULL;
	return sample;
}

static BYTE store[1 << 16];

/* Sends `length` bytes in and takes what comes out into the store; the bytes that came out, or -1. */
static int convert(IMFTransform *transform, const void *input, DWORD length, HRESULT *result)
{
	IMFMediaBuffer *in_buffer = NULL, *out_buffer = NULL;
	MFT_OUTPUT_DATA_BUFFER output = {0};
	BYTE *bytes = NULL;
	DWORD status = 0, written = 0;
	IMFSample *sample = sample_of(length, &in_buffer);
	if (!sample || FAILED(IMFMediaBuffer_Lock(in_buffer, &bytes, NULL, NULL)))
		return -1;
	for (DWORD at = 0; at < length; ++at)
		bytes[at] = ((const BYTE *)input)[at];
	IMFMediaBuffer_Unlock(in_buffer);
	IMFMediaBuffer_SetCurrentLength(in_buffer, length);
	if (FAILED(IMFTransform_ProcessInput(transform, 0, sample, 0)))
		return -1;
	IMFSample_Release(sample);
	IMFMediaBuffer_Release(in_buffer);
	output.pSample = sample_of(sizeof store, &out_buffer);
	if (!output.pSample)
		return -1;
	*result = IMFTransform_ProcessOutput(transform, 0, 1, &output, &status);
	if (SUCCEEDED(*result)) {
		if (FAILED(IMFMediaBuffer_Lock(out_buffer, &bytes, NULL, &written)))
			return -1;
		for (DWORD at = 0; at < written; ++at)
			store[at] = bytes[at];
		IMFMediaBuffer_Unlock(out_buffer);
	}
	IMFSample_Release(output.pSample);
	IMFMediaBuffer_Release(out_buffer);
	return (int)written;
}

int raven_entry(void)
{
	IUnknown *unknown = NULL;
	IMFTransform *transform = NULL;
	props *properties = NULL;
	IMFMediaType *type = NULL, *other = NULL;
	IMFMediaBuffer *buffer = NULL;
	MFT_OUTPUT_DATA_BUFFER output = {0};
	DWORD status = 0, inputs = 0, outputs = 0;
	HRESULT result = S_OK;
	static float constant[4410 * 2];
	static SHORT ramp[2000];
	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)))
		return 1;
	/* The class is there, and gives the transform and the resampler's own properties. */
	if (FAILED(CoCreateInstance(&CLSID_Resampler, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&unknown)))
		return 2;
	if (FAILED(IUnknown_QueryInterface(unknown, &IID_IMFTransform, (void **)&transform)))
		return 3;
	if (FAILED(IUnknown_QueryInterface(unknown, &IID_ResamplerProps, (void **)&properties)))
		return 4;
	if (properties->table->half_filter_length(properties, 60) != S_OK || properties->table->half_filter_length(properties, 0) != E_INVALIDARG)
		return 5;
	if (FAILED(IMFTransform_GetStreamCount(transform, &inputs, &outputs)) || inputs != 1 || outputs != 1)
		return 6;

	/* Nothing goes in or out before both types are set. */
	output.pSample = sample_of(64, &buffer);
	if (IMFTransform_ProcessOutput(transform, 0, 1, &output, &status) != MF_E_TRANSFORM_TYPE_NOT_SET)
		return 7;
	if (IMFTransform_ProcessInput(transform, 0, output.pSample, 0) != MF_E_TRANSFORM_TYPE_NOT_SET)
		return 8;
	if (IMFTransform_GetInputCurrentType(transform, 0, &other) != MF_E_TRANSFORM_TYPE_NOT_SET)
		return 9;

	/* A type that is not sound, a second stream, and other channels on the way out are refused. */
	MFCreateMediaType(&other);
	IMFMediaType_SetGUID(other, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
	if (IMFTransform_SetInputType(transform, 0, other, 0) != MF_E_INVALIDMEDIATYPE)
		return 10;
	IMFMediaType_Release(other);
	type = audio(TRUE, 2, 44100);
	if (IMFTransform_SetInputType(transform, 1, type, 0) != MF_E_INVALIDSTREAMNUMBER)
		return 11;
	if (FAILED(IMFTransform_SetInputType(transform, 0, type, 0)))
		return 12;
	IMFMediaType_Release(type);
	type = audio(TRUE, 1, 48000);
	if (IMFTransform_SetOutputType(transform, 0, type, 0) != MF_E_INVALIDMEDIATYPE)
		return 13;
	IMFMediaType_Release(type);
	/* A test of a type sets nothing. */
	type = audio(TRUE, 2, 48000);
	if (FAILED(IMFTransform_SetOutputType(transform, 0, type, MFT_SET_TYPE_TEST_ONLY)) ||
		IMFTransform_GetOutputCurrentType(transform, 0, &other) != MF_E_TRANSFORM_TYPE_NOT_SET)
		return 14;
	if (FAILED(IMFTransform_SetOutputType(transform, 0, type, 0)))
		return 15;
	IMFMediaType_Release(type);
	UINT32 rate = 0;
	if (FAILED(IMFTransform_GetOutputCurrentType(transform, 0, &other)) ||
		FAILED(IMFMediaType_GetUINT32(other, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) || rate != 48000)
		return 16;
	IMFMediaType_Release(other);
	if (FAILED(IMFTransform_ProcessMessage(transform, MFT_MESSAGE_COMMAND_FLUSH, 0)) ||
		FAILED(IMFTransform_ProcessMessage(transform, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
		FAILED(IMFTransform_ProcessMessage(transform, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
		return 17;

	/* With no input there is no output. */
	if (IMFTransform_ProcessOutput(transform, 0, 1, &output, &status) != MF_E_TRANSFORM_NEED_MORE_INPUT)
		return 18;
	IMFSample_Release(output.pSample);
	IMFMediaBuffer_Release(buffer);

	/* A tenth of a second at 44.1 kHz is a tenth of a second at 48 kHz, and a level stays the level it was. */
	for (int frame = 0; frame < 4410; ++frame) {
		constant[frame * 2] = 0.5f;
		constant[frame * 2 + 1] = -0.25f;
	}
	int bytes = convert(transform, constant, sizeof constant, &result);
	if (FAILED(result) || bytes % 8 || bytes / 8 < 4790 || bytes / 8 > 4810)
		return 19;
	const float *frames = (const float *)store;
	for (int frame = 8; frame < bytes / 8; ++frame)
		if (frames[frame * 2] < 0.499f || frames[frame * 2] > 0.501f || frames[frame * 2 + 1] > -0.249f || frames[frame * 2 + 1] < -0.251f)
			return 20;
	/* The second tenth brings the total to two tenths: nothing is lost between calls. */
	const int first = bytes / 8;
	bytes = convert(transform, constant, sizeof constant, &result);
	if (FAILED(result) || first + bytes / 8 < 9590 || first + bytes / 8 > 9600)
		return 21;

	/* 16-bit sound at twice the rate: a ramp stays a ramp, with a step between each two frames. */
	type = audio(FALSE, 1, 22050);
	if (FAILED(IMFTransform_SetInputType(transform, 0, NULL, 0)) || FAILED(IMFTransform_SetOutputType(transform, 0, NULL, 0)) ||
		FAILED(IMFTransform_SetInputType(transform, 0, type, 0)))
		return 22;
	IMFMediaType_Release(type);
	type = audio(FALSE, 1, 44100);
	if (FAILED(IMFTransform_SetOutputType(transform, 0, type, 0)))
		return 23;
	IMFMediaType_Release(type);
	for (int frame = 0; frame < 2000; ++frame)
		ramp[frame] = (SHORT)(frame * 10);
	bytes = convert(transform, ramp, sizeof ramp, &result);
	if (FAILED(result) || bytes / 2 < 3990 || bytes / 2 > 4000)
		return 24;
	const SHORT *samples = (const SHORT *)store;
	for (int frame = 8; frame < bytes / 2; ++frame)
		if (samples[frame] - samples[frame - 1] < 3 || samples[frame] - samples[frame - 1] > 7)
			return 25;

	properties->table->release(properties);
	IMFTransform_Release(transform);
	if (IUnknown_Release(unknown) != 0)
		return 26;
	MFShutdown();
	return 0;
}
