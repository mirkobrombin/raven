#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <msacm.h>

static BOOL CALLBACK count_driver(HACMDRIVERID id, DWORD_PTR instance, DWORD support)
{
	(void)id;
	(void)support;
	++*(DWORD *)instance;
	return TRUE;
}

static BOOL CALLBACK count_tag(HACMDRIVERID id, LPACMFORMATTAGDETAILSW details, DWORD_PTR instance, DWORD support)
{
	(void)id;
	(void)details;
	(void)support;
	++*(DWORD *)instance;
	return TRUE;
}

static BOOL CALLBACK count_format(HACMDRIVERID id, LPACMFORMATDETAILSW details, DWORD_PTR instance, DWORD support)
{
	(void)id;
	(void)details;
	(void)support;
	++*(DWORD *)instance;
	return TRUE;
}

/* The audio compression manager: PCM to MS ADPCM and back, mono and stereo, and what it tells a program about its one driver,
   its format tags and its formats. */
int raven_entry(void)
{
	WAVEFORMATEX pcm = {WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
	BYTE compressed_storage[sizeof(WAVEFORMATEX) + 4 + 7 * sizeof(ADPCMCOEFSET)] = {0};
	WAVEFORMATEX *compressed = (WAVEFORMATEX *)compressed_storage;
	compressed->wFormatTag = WAVE_FORMAT_ADPCM;
	if (acmGetVersion() < 0x04000000 || acmFormatSuggest(NULL, &pcm, compressed, sizeof(compressed_storage),
		ACM_FORMATSUGGESTF_WFORMATTAG) != MMSYSERR_NOERROR || compressed->wFormatTag != WAVE_FORMAT_ADPCM)
		return 18;
	ADPCMWAVEFORMAT *adpcm = (ADPCMWAVEFORMAT *)compressed;
	if (adpcm->wSamplesPerBlock != 500 || adpcm->wNumCoef != 7)
		return 19;
	WAVEFORMATEX suggested_pcm;
	for (DWORD i = 0; i < sizeof(suggested_pcm); ++i)
		((BYTE *)&suggested_pcm)[i] = 0xcc;
	if (acmFormatSuggest(NULL, compressed, &suggested_pcm, sizeof(suggested_pcm), 0) != MMSYSERR_NOERROR ||
		suggested_pcm.wFormatTag != WAVE_FORMAT_PCM || suggested_pcm.wBitsPerSample != 16)
		return 44;
	for (DWORD i = 0; i < sizeof(suggested_pcm); ++i)
		((BYTE *)&suggested_pcm)[i] = 0xcc;
	suggested_pcm.wBitsPerSample = 8;
	if (acmFormatSuggest(NULL, &pcm, &suggested_pcm, sizeof(suggested_pcm), ACM_FORMATSUGGESTF_WBITSPERSAMPLE) !=
		MMSYSERR_NOERROR || suggested_pcm.wBitsPerSample != 8 || suggested_pcm.nBlockAlign != 1)
		return 45;
	suggested_pcm.nChannels = 2;
	if (acmFormatSuggest(NULL, &pcm, &suggested_pcm, sizeof(suggested_pcm), ACM_FORMATSUGGESTF_NCHANNELS) != ACMERR_NOTPOSSIBLE)
		return 46;
	HACMSTREAM encoder = NULL;
	if (acmStreamOpen(&encoder, NULL, &pcm, compressed, NULL, 0, 0, 0) != MMSYSERR_NOERROR || !encoder)
		return 20;
	DWORD encoded_size = 0;
	if (acmStreamSize(encoder, 1000, &encoded_size, ACM_STREAMSIZEF_SOURCE) != MMSYSERR_NOERROR || encoded_size != 256)
		return 21;
	SHORT pcm_samples[500];
	for (DWORD i = 0; i < 500; ++i)
		pcm_samples[i] = (SHORT)((LONG)((i * 97) % 12000) - 6000);
	BYTE encoded[256] = {0};
	ACMSTREAMHEADER encode_header = {0};
	encode_header.cbStruct = sizeof(encode_header);
	encode_header.pbSrc = (LPBYTE)pcm_samples;
	encode_header.cbSrcLength = sizeof(pcm_samples);
	encode_header.pbDst = encoded;
	encode_header.cbDstLength = sizeof(encoded);
	if (acmStreamPrepareHeader(encoder, &encode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamConvert(encoder, &encode_header, ACM_STREAMCONVERTF_BLOCKALIGN) != MMSYSERR_NOERROR ||
		encode_header.cbSrcLengthUsed != sizeof(pcm_samples) || encode_header.cbDstLengthUsed != sizeof(encoded))
		return 22;
	if (acmStreamUnprepareHeader(encoder, &encode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamClose(encoder, 0) != MMSYSERR_NOERROR)
		return 23;
	HACMSTREAM decoder = NULL;
	WAVEFORMATEX decoded_format = pcm;
	if (acmStreamOpen(&decoder, NULL, compressed, &decoded_format, NULL, 0, 0, 0) != MMSYSERR_NOERROR || !decoder)
		return 24;
	DWORD decoded_size = 0;
	if (acmStreamSize(decoder, 0, &decoded_size, ACM_STREAMSIZEF_SOURCE) != MMSYSERR_INVALPARAM ||
		acmStreamSize(decoder, 1500, &decoded_size, ACM_STREAMSIZEF_DESTINATION) != MMSYSERR_NOERROR || decoded_size != 256 ||
		acmStreamSize(decoder, 255, &decoded_size, ACM_STREAMSIZEF_SOURCE) != ACMERR_NOTPOSSIBLE)
		return 47;
	SHORT decoded_samples[500] = {0};
	ACMSTREAMHEADER decode_header = {0};
	decode_header.cbStruct = sizeof(decode_header);
	decode_header.pbSrc = encoded;
	decode_header.cbSrcLength = sizeof(encoded);
	decode_header.pbDst = (LPBYTE)decoded_samples;
	decode_header.cbDstLength = sizeof(decoded_samples);
	if (acmStreamConvert(decoder, &decode_header, ACM_STREAMCONVERTF_BLOCKALIGN) != ACMERR_UNPREPARED)
		return 59;
	if (acmStreamPrepareHeader(decoder, &decode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamConvert(decoder, &decode_header, ACM_STREAMCONVERTF_BLOCKALIGN) != MMSYSERR_NOERROR ||
		decode_header.cbSrcLengthUsed != sizeof(encoded) || decode_header.cbDstLengthUsed != sizeof(decoded_samples))
		return 25;
	/* A block that starts at the largest delta and keeps the code that grows it fastest still decodes. The volatile index keeps the
	 * compiler from calling memset, which this fixture does not link. */
	encoded[1] = encoded[2] = 0xff;
	for (volatile DWORD i = 7; i < sizeof(encoded); ++i)
		encoded[i] = 0x88;
	if (acmStreamConvert(decoder, &decode_header, ACM_STREAMCONVERTF_BLOCKALIGN) != MMSYSERR_NOERROR ||
		decode_header.cbSrcLengthUsed != sizeof(encoded) || decode_header.cbDstLengthUsed != sizeof(decoded_samples) ||
		decoded_samples[499] != -32768)
		return 60;
	if (acmStreamUnprepareHeader(decoder, &decode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamClose(decoder, 0) != MMSYSERR_NOERROR)
		return 26;
	ACMFORMATTAGDETAILSW tag = {0};
	tag.cbStruct = sizeof(tag);
	tag.dwFormatTagIndex = 1;
	if (acmFormatTagDetailsW(NULL, &tag, ACM_FORMATTAGDETAILSF_INDEX) != MMSYSERR_NOERROR ||
		tag.dwFormatTag != WAVE_FORMAT_ADPCM || tag.cStandardFormats == 0)
		return 28;
	tag.dwFormatTag = WAVE_FORMAT_ADPCM;
	if (acmFormatTagDetailsW(NULL, &tag, ACM_FORMATTAGDETAILSF_FORMATTAG) != MMSYSERR_NOERROR ||
		tag.dwFormatTagIndex != 1)
		return 48;
	ACMFORMATDETAILSW format_details = {0};
	BYTE format_storage[sizeof(WAVEFORMATEX) + 4 + 7 * sizeof(ADPCMCOEFSET)] = {0};
	format_details.cbStruct = sizeof(format_details);
	format_details.pwfx = (WAVEFORMATEX *)format_storage;
	format_details.cbwfx = sizeof(format_storage);
	format_details.dwFormatTag = WAVE_FORMAT_PCM;
	format_details.dwFormatIndex = 0;
	if (acmFormatDetailsW(NULL, &format_details, ACM_FORMATDETAILSF_INDEX) != MMSYSERR_NOERROR ||
		format_details.pwfx->wFormatTag != WAVE_FORMAT_PCM || format_details.pwfx->nChannels != 1)
		return 49;
	DWORD enumerated = 0;
	ACMFORMATTAGDETAILSW tag_filter = {0};
	tag_filter.cbStruct = sizeof(tag_filter);
	if (acmFormatTagEnumW(NULL, &tag_filter, count_tag, (DWORD_PTR)&enumerated, 0) != MMSYSERR_NOERROR ||
		enumerated != 2)
		return 50;
	enumerated = 0;
	if (acmFormatEnumW(NULL, &format_details, count_format, (DWORD_PTR)&enumerated, 0) != MMSYSERR_NOERROR ||
		enumerated != 9)
		return 51;
	enumerated = 0;
	if (acmDriverEnum(count_driver, (DWORD_PTR)&enumerated, 0) != MMSYSERR_NOERROR || enumerated != 1)
		return 52;
	HACMDRIVER driver = NULL;
	HACMDRIVERID driver_id = NULL;
	ACMDRIVERDETAILSW driver_details = {0};
	if (acmDriverOpen(&driver, (HACMDRIVERID)1, 0) != MMSYSERR_NOERROR ||
		acmDriverID((HACMOBJ)driver, &driver_id, 0) != MMSYSERR_NOERROR || driver_id != (HACMDRIVERID)1)
		return 53;
	driver_details.cbStruct = sizeof(driver_details);
	if (acmDriverDetailsW(driver_id, &driver_details, 0) != MMSYSERR_NOERROR ||
		driver_details.cFormatTags != 2 || acmDriverClose(driver, 0) != MMSYSERR_NOERROR)
		return 54;
	WAVEFORMATEX stereo_pcm = {WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
	BYTE stereo_compressed_storage[sizeof(WAVEFORMATEX) + 4 + 7 * sizeof(ADPCMCOEFSET)] = {0};
	WAVEFORMATEX *stereo_compressed = (WAVEFORMATEX *)stereo_compressed_storage;
	stereo_compressed->wFormatTag = WAVE_FORMAT_ADPCM;
	if (acmFormatSuggest(NULL, &stereo_pcm, stereo_compressed, sizeof(stereo_compressed_storage),
		ACM_FORMATSUGGESTF_WFORMATTAG) != MMSYSERR_NOERROR ||
		((ADPCMWAVEFORMAT *)stereo_compressed)->wSamplesPerBlock != 244)
		return 29;
	HACMSTREAM stereo_encoder = NULL;
	if (acmStreamOpen(&stereo_encoder, NULL, &stereo_pcm, stereo_compressed, NULL, 0, 0, 0) != MMSYSERR_NOERROR)
		return 30;
	SHORT stereo_samples[244 * 2];
	for (DWORD i = 0; i < 244; ++i) {
		stereo_samples[i * 2] = (SHORT)((LONG)((i * 83) % 10000) - 5000);
		stereo_samples[i * 2 + 1] = (SHORT)((LONG)((i * 61) % 8000) - 4000);
	}
	BYTE stereo_encoded[256] = {0};
	ACMSTREAMHEADER stereo_header = {0};
	stereo_header.cbStruct = sizeof(stereo_header);
	stereo_header.pbSrc = (LPBYTE)stereo_samples;
	stereo_header.cbSrcLength = sizeof(stereo_samples);
	stereo_header.pbDst = stereo_encoded;
	stereo_header.cbDstLength = sizeof(stereo_encoded);
	if (acmStreamPrepareHeader(stereo_encoder, &stereo_header, 0) != MMSYSERR_NOERROR ||
		acmStreamConvert(stereo_encoder, &stereo_header, ACM_STREAMCONVERTF_BLOCKALIGN) != MMSYSERR_NOERROR ||
		stereo_header.cbDstLengthUsed != sizeof(stereo_encoded) ||
		acmStreamUnprepareHeader(stereo_encoder, &stereo_header, 0) != MMSYSERR_NOERROR ||
		acmStreamClose(stereo_encoder, 0) != MMSYSERR_NOERROR)
		return 31;
	HACMSTREAM stereo_decoder = NULL;
	if (acmStreamOpen(&stereo_decoder, NULL, stereo_compressed, &stereo_pcm, NULL, 0, 0, 0) != MMSYSERR_NOERROR)
		return 32;
	SHORT stereo_decoded[244 * 2] = {0};
	ACMSTREAMHEADER stereo_decode_header = {0};
	stereo_decode_header.cbStruct = sizeof(stereo_decode_header);
	stereo_decode_header.pbSrc = stereo_encoded;
	stereo_decode_header.cbSrcLength = sizeof(stereo_encoded);
	stereo_decode_header.pbDst = (LPBYTE)stereo_decoded;
	stereo_decode_header.cbDstLength = sizeof(stereo_decoded);
	if (acmStreamPrepareHeader(stereo_decoder, &stereo_decode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamConvert(stereo_decoder, &stereo_decode_header, ACM_STREAMCONVERTF_BLOCKALIGN) != MMSYSERR_NOERROR ||
		stereo_decode_header.cbDstLengthUsed != sizeof(stereo_decoded) ||
		acmStreamUnprepareHeader(stereo_decoder, &stereo_decode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamClose(stereo_decoder, 0) != MMSYSERR_NOERROR)
		return 33;
	return 0;
}
