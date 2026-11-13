#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mmreg.h>
#include <strmif.h>

static int same_guid(const GUID *left, const GUID *right)
{
	return left->Data1 == right->Data1 && left->Data2 == right->Data2 && left->Data3 == right->Data3 &&
		left->Data4[0] == right->Data4[0] && left->Data4[1] == right->Data4[1] &&
		left->Data4[2] == right->Data4[2] && left->Data4[3] == right->Data4[3] &&
		left->Data4[4] == right->Data4[4] && left->Data4[5] == right->Data4[5] &&
		left->Data4[6] == right->Data4[6] && left->Data4[7] == right->Data4[7];
}

static const GUID media_type_audio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID wave_format_representation = {0x05589f81, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID mf_audio_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID mf_audio_pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID mf_mt_subtype = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
static const GUID mf_audio_channel_mask = {0x55fb5765, 0x644a, 0x4caf, {0x84, 0x79, 0x93, 0x89, 0x83, 0xbb, 0x15, 0x88}};
static const GUID mf_audio_valid_bits = {0xd9bf8d6a, 0x9530, 0x4b7c, {0x9d, 0xdf, 0xff, 0x6f, 0xd5, 0x8b, 0xbd, 0x06}};

/* The multimedia thread registration of avrt.dll, and mfplat.dll's attributes, media types, samples and buffers. */
int raven_entry(void)
{
	if (MFStartup(MF_VERSION, MFSTARTUP_FULL) != S_OK)
		return 35;
	DWORD task_index = 0xdead;
	HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
	if (!task || task_index != 0)
		return 1;
	if (!AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH))
		return 2;
	ULONG responsiveness = 0xdead;
	if (AvQuerySystemResponsiveness(task, &responsiveness) != S_OK || responsiveness != 0)
		return 3;
	if (!AvRevertMmThreadCharacteristics(task))
		return 4;

	IMFSample *sample = NULL;
	if (FAILED(MFCreateSample(&sample)) || !sample)
		return 5;
	IMFMediaBuffer *empty_contiguous = NULL;
	if (IMFSample_ConvertToContiguousBuffer(sample, &empty_contiguous) != E_UNEXPECTED || empty_contiguous)
		return 37;
	LONGLONG sample_time = 0;
	if (IMFSample_SetSampleTime(sample, 123456) != S_OK || IMFSample_GetSampleTime(sample, &sample_time) != S_OK ||
		sample_time != 123456)
		return 6;
	WAVEFORMATEX wave = {WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
	IMFMediaType *type = NULL;
	if (FAILED(MFCreateMediaType(&type)) || !type)
		return 7;
	if (MFInitMediaTypeFromWaveFormatEx(type, &wave, sizeof(wave)) != S_OK)
		return 8;
	IMFMediaType *unsupported_type = NULL;
	struct {
		WAVEFORMATEX format;
		BYTE extra[22];
	} unsupported_wave = {{WAVE_FORMAT_EXTENSIBLE, 2, 48000, 192000, 4, 16, 22}, {0}};
	if (FAILED(MFCreateMediaType(&unsupported_type)) || !unsupported_type)
		return 35;
	if (MFInitMediaTypeFromWaveFormatEx(unsupported_type, &unsupported_wave.format, sizeof(unsupported_wave)) !=
		MF_E_INVALIDMEDIATYPE)
		return 36;
	IMFMediaType_Release(unsupported_type);
	WAVEFORMATEXTENSIBLE valid_extensible = {0};
	valid_extensible.Format = (WAVEFORMATEX){WAVE_FORMAT_EXTENSIBLE, 2, 48000, 384000, 8, 32, 22};
	valid_extensible.Samples.wValidBitsPerSample = 32;
	valid_extensible.dwChannelMask = 3;
	valid_extensible.SubFormat = mf_audio_float;
	IMFMediaType *extensible_type = NULL;
	if (FAILED(MFCreateMediaType(&extensible_type)) || !extensible_type ||
		MFInitMediaTypeFromWaveFormatEx(extensible_type, &valid_extensible.Format, sizeof(valid_extensible)) != S_OK)
		return 38;
	GUID extensible_subtype;
	UINT32 channel_mask = 0, valid_bits = 0, extensible_size = 0;
	if (IMFMediaType_GetGUID(extensible_type, &mf_mt_subtype, &extensible_subtype) != S_OK ||
		!same_guid(&extensible_subtype, &mf_audio_float) ||
		IMFMediaType_GetUINT32(extensible_type, &mf_audio_channel_mask, &channel_mask) != S_OK || channel_mask != 3 ||
		IMFMediaType_GetUINT32(extensible_type, &mf_audio_valid_bits, &valid_bits) != S_OK || valid_bits != 32)
		return 39;
	WAVEFORMATEX *extensible_roundtrip = NULL;
	if (MFCreateWaveFormatExFromMFMediaType(extensible_type, &extensible_roundtrip, &extensible_size, 0) != S_OK ||
		!extensible_roundtrip || extensible_roundtrip->wFormatTag != WAVE_FORMAT_IEEE_FLOAT)
		return 40;
	CoTaskMemFree(extensible_roundtrip);
	IMFMediaType_Release(extensible_type);
	valid_extensible.Format.wBitsPerSample = 16;
	valid_extensible.Format.nBlockAlign = 4;
	valid_extensible.Format.nAvgBytesPerSec = 192000;
	valid_extensible.Samples.wValidBitsPerSample = 16;
	valid_extensible.SubFormat = mf_audio_pcm;
	extensible_type = NULL;
	if (FAILED(MFCreateMediaType(&extensible_type)) || !extensible_type ||
		MFInitMediaTypeFromWaveFormatEx(extensible_type, &valid_extensible.Format, sizeof(valid_extensible)) != S_OK ||
		MFCreateWaveFormatExFromMFMediaType(extensible_type, &extensible_roundtrip, &extensible_size, 0) != S_OK ||
		!extensible_roundtrip || extensible_roundtrip->wFormatTag != WAVE_FORMAT_PCM)
		return 55;
	CoTaskMemFree(extensible_roundtrip);
	if (MFCreateWaveFormatExFromMFMediaType(extensible_type, &extensible_roundtrip, &extensible_size,
			MFWaveFormatExConvertFlag_ForceExtensible) != S_OK || extensible_size != sizeof(WAVEFORMATEXTENSIBLE) ||
		extensible_roundtrip->wFormatTag != WAVE_FORMAT_EXTENSIBLE || extensible_roundtrip->cbSize != 22 ||
		((WAVEFORMATEXTENSIBLE *)extensible_roundtrip)->dwChannelMask != 3 ||
		((WAVEFORMATEXTENSIBLE *)extensible_roundtrip)->Samples.wValidBitsPerSample != 16 ||
		!same_guid(&((WAVEFORMATEXTENSIBLE *)extensible_roundtrip)->SubFormat, &mf_audio_pcm))
		return 56;
	CoTaskMemFree(extensible_roundtrip);
	IMFMediaType_Release(extensible_type);
	GUID major;
	if (IMFMediaType_GetMajorType(type, &major) != S_OK || !same_guid(&major, &media_type_audio))
		return 9;
	WAVEFORMATEX *roundtrip = NULL;
	UINT32 format_size = 0;
	if (MFCreateWaveFormatExFromMFMediaType(type, &roundtrip, &format_size, 0) != S_OK || !roundtrip ||
		format_size < sizeof(*roundtrip) || roundtrip->nSamplesPerSec != wave.nSamplesPerSec ||
		roundtrip->nChannels != wave.nChannels || roundtrip->wBitsPerSample != wave.wBitsPerSample)
		return 10;
	CoTaskMemFree(roundtrip);
	void *representation = NULL;
	const AM_MEDIA_TYPE *media;
	if (IMFMediaType_GetRepresentation(type, wave_format_representation, &representation) != S_OK || !(media = representation) ||
		!same_guid(&media->majortype, &media_type_audio) || !same_guid(&media->subtype, &mf_audio_pcm) ||
		!same_guid(&media->formattype, &wave_format_representation) || media->cbFormat != sizeof(WAVEFORMATEX) ||
		media->lSampleSize != wave.nBlockAlign ||
		((const WAVEFORMATEX *)media->pbFormat)->nSamplesPerSec != wave.nSamplesPerSec ||
		IMFMediaType_FreeRepresentation(type, wave_format_representation, representation) != S_OK)
		return 34;
	if (IMFMediaType_GetRepresentation(type, mf_mt_subtype, &representation) != MF_E_UNSUPPORTED_REPRESENTATION || representation)
		return 57;
	/* A missing key deletes without error, a NULL value only asks whether the key exists, and a blob needs its bytes. */
	GUID index_key;
	UINT32 item_count = 0;
	PROPVARIANT empty_blob = {0};
	empty_blob.vt = VT_BLOB;
	empty_blob.blob.cbSize = 4;
	if (IMFMediaType_DeleteItem(type, &mf_audio_channel_mask) != S_OK ||
		IMFMediaType_GetItem(type, &mf_mt_subtype, NULL) != S_OK ||
		IMFMediaType_GetItem(type, &mf_audio_channel_mask, NULL) != MF_E_ATTRIBUTENOTFOUND ||
		IMFMediaType_GetCount(type, &item_count) != S_OK || !item_count ||
		IMFMediaType_GetItemByIndex(type, item_count, &index_key, NULL) != E_INVALIDARG ||
		IMFMediaType_GetItemByIndex(type, item_count - 1, &index_key, NULL) != S_OK ||
		IMFMediaType_SetItem(type, &mf_audio_channel_mask, &empty_blob) != E_INVALIDARG ||
		IMFMediaType_LockStore(type) != S_OK || IMFMediaType_UnlockStore(type) != S_OK)
		return 58;
	IMFMediaBuffer *buffer = NULL;
	if (MFCreateMemoryBuffer(4096, &buffer) != S_OK || !buffer)
		return 11;
	BYTE *data = NULL;
	DWORD max_length = 0;
	if (IMFMediaBuffer_Lock(buffer, &data, &max_length, NULL) != S_OK || !data || max_length != 4096)
		return 12;
	data[0] = 0x5a;
	if (IMFMediaBuffer_Unlock(buffer) != S_OK || IMFMediaBuffer_SetCurrentLength(buffer, 1) != S_OK)
		return 13;
	if (IMFSample_AddBuffer(sample, buffer) != S_OK || IMFSample_GetTotalLength(sample, &max_length) != S_OK ||
		max_length != 1)
		return 14;
	IMFMediaBuffer *contiguous = NULL;
	if (IMFSample_ConvertToContiguousBuffer(sample, &contiguous) != S_OK || contiguous != buffer)
		return 15;
	IMFMediaBuffer_Release(contiguous);
	IMFMediaBuffer *second = NULL;
	if (MFCreateMemoryBuffer(2, &second) != S_OK || IMFMediaBuffer_Lock(second, &data, NULL, NULL) != S_OK)
		return 41;
	data[0] = 0x11;
	data[1] = 0x22;
	if (IMFMediaBuffer_Unlock(second) != S_OK || IMFMediaBuffer_SetCurrentLength(second, 2) != S_OK ||
		IMFSample_AddBuffer(sample, second) != S_OK ||
		IMFSample_ConvertToContiguousBuffer(sample, &contiguous) != S_OK || contiguous == buffer)
		return 42;
	DWORD sample_buffer_count = 0, contiguous_length = 0;
	if (IMFSample_GetBufferCount(sample, &sample_buffer_count) != S_OK || sample_buffer_count != 1 ||
		IMFMediaBuffer_GetCurrentLength(contiguous, &contiguous_length) != S_OK || contiguous_length != 3 ||
		IMFMediaBuffer_Lock(contiguous, &data, NULL, NULL) != S_OK ||
		data[0] != 0x5a || data[1] != 0x11 || data[2] != 0x22 ||
		IMFMediaBuffer_Unlock(contiguous) != S_OK)
		return 43;
	IMFMediaBuffer_Release(contiguous);
	IMFMediaBuffer_Release(second);
	IMFMediaBuffer_Release(buffer);
	IMFMediaType_Release(type);
	IMFSample_Release(sample);
	IMFMediaBuffer *aligned = NULL;
	if (MFCreateAlignedMemoryBuffer(4096, MF_16_BYTE_ALIGNMENT, &aligned) != S_OK || !aligned)
		return 16;
	if (IMFMediaBuffer_Lock(aligned, &data, NULL, NULL) != S_OK || ((ULONG_PTR)data & 15))
		return 17;
	IMFMediaBuffer_Unlock(aligned);
	IMFMediaBuffer_Release(aligned);
	aligned = (IMFMediaBuffer *)0xdead;
	if (MFCreateAlignedMemoryBuffer(4096, 16, &aligned) != E_INVALIDARG || aligned)
		return 27;
	/* No transform is registered, so an enumeration succeeds and finds none, as paint.net asks whether a video codec is installed. */
	static const GUID video_decoders = {0xd6c02d4b, 0x6833, 0x45b4, {0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91}};
	IMFActivate **activates = (IMFActivate **)0xdead;
	UINT32 count = 0xdead;
	if (MFTEnumEx(video_decoders, MFT_ENUM_FLAG_ALL, NULL, NULL, &activates, &count) != S_OK || activates || count)
		return 59;
	if (MFShutdown() != S_OK || MFShutdown() != MF_E_SHUTDOWN)
		return 60;
	return 0;
}
