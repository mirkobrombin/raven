/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <msacm.h>
#include <stdint.h>
#include <string.h>

typedef struct acm_stream {
	WAVEFORMATEX source, destination;
	DWORD source_align, destination_align;
	DWORD source_frames_per_block, destination_frames_per_block;
	ADPCMCOEFSET source_coefficients[32], destination_coefficients[32];
	WORD source_coefficient_count, destination_coefficient_count;
} acm_stream;

static const int adaptation[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
static const ADPCMCOEFSET standard_coefficients[] = {
	{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}
};

static BOOL pcm_format(const WAVEFORMATEX *format)
{
	return format && format->wFormatTag == WAVE_FORMAT_PCM && format->nChannels &&
		(format->wBitsPerSample == 8 || format->wBitsPerSample == 16) &&
		format->nBlockAlign == format->nChannels * (format->wBitsPerSample / 8) &&
		format->nSamplesPerSec && format->nSamplesPerSec <= UINT32_MAX / format->nBlockAlign &&
		format->nAvgBytesPerSec == format->nSamplesPerSec * format->nBlockAlign;
}

static BOOL adpcm_format(const WAVEFORMATEX *format, WORD *samples_per_block, WORD *coefficient_count, ADPCMCOEFSET *coefficients)
{
	if (!format || format->wFormatTag != WAVE_FORMAT_ADPCM || !format->nChannels || format->nChannels > 2 ||
		format->wBitsPerSample != 4 || format->cbSize < 4)
		return FALSE;
	const ADPCMWAVEFORMAT *adpcm = (const ADPCMWAVEFORMAT *)format;
	const DWORD required = 4 + (DWORD)adpcm->wNumCoef * sizeof(ADPCMCOEFSET);
	if (!adpcm->wNumCoef || adpcm->wNumCoef > 32 || format->cbSize < required ||
		adpcm->wSamplesPerBlock < 2 || format->nBlockAlign <= 7 * format->nChannels)
		return FALSE;
	const DWORD expected = ((format->nBlockAlign - 7 * format->nChannels) * 2 / format->nChannels) + 2;
	if (adpcm->wSamplesPerBlock != expected)
		return FALSE;
	*samples_per_block = adpcm->wSamplesPerBlock;
	*coefficient_count = adpcm->wNumCoef;
	memcpy(coefficients, adpcm->aCoef, adpcm->wNumCoef * sizeof(*coefficients));
	return TRUE;
}

static BOOL supported_format(const WAVEFORMATEX *format)
{
	WORD samples, count;
	ADPCMCOEFSET coefficients[32];
	return pcm_format(format) || adpcm_format(format, &samples, &count, coefficients);
}

static BOOL supported_pair(const WAVEFORMATEX *source, const WAVEFORMATEX *destination)
{
	WORD samples, count;
	ADPCMCOEFSET coefficients[32];
	/* shortcut: no resampling or channel conversion, add them when a program converts between rates or channel counts. */
	return source->nChannels == destination->nChannels && source->nSamplesPerSec == destination->nSamplesPerSec &&
		(pcm_format(source) ? supported_format(destination) :
			adpcm_format(source, &samples, &count, coefficients) && pcm_format(destination));
}

static DWORD format_bytes(const WAVEFORMATEX *format)
{
	return sizeof(WAVEFORMATEX) + format->cbSize;
}

static BOOL init_stream(acm_stream *stream, const WAVEFORMATEX *source, const WAVEFORMATEX *destination)
{
	if (!supported_pair(source, destination))
		return FALSE;
	memcpy(&stream->source, source, sizeof(WAVEFORMATEX));
	memcpy(&stream->destination, destination, sizeof(WAVEFORMATEX));
	stream->source_align = source->nBlockAlign;
	stream->destination_align = destination->nBlockAlign;
	stream->source_frames_per_block = 1;
	stream->destination_frames_per_block = 1;
	if (source->wFormatTag == WAVE_FORMAT_ADPCM) {
		WORD frames;
		if (!adpcm_format(source, &frames, &stream->source_coefficient_count, stream->source_coefficients))
			return FALSE;
		stream->source_frames_per_block = frames;
	}
	if (destination->wFormatTag == WAVE_FORMAT_ADPCM) {
		WORD frames;
		if (!adpcm_format(destination, &frames, &stream->destination_coefficient_count, stream->destination_coefficients))
			return FALSE;
		stream->destination_frames_per_block = frames;
	}
	return TRUE;
}

/* The bound keeps the next multiplication in range for a block that starts with a huge delta. */
static LONG adapt_delta(int code, LONG delta)
{
	delta = adaptation[code] * delta / 256;
	return delta < 16 ? 16 : delta > INT32_MAX / 768 ? INT32_MAX / 768 : delta;
}

static LONG clamp_sample(LONG sample)
{
	return sample < -32768 ? -32768 : sample > 32767 ? 32767 : sample;
}

static SHORT read_sample(const BYTE *data, DWORD bits)
{
	if (bits == 8)
		return (SHORT)(((LONG)data[0] - 128) << 8);
	return (SHORT)(data[0] | ((WORD)data[1] << 8));
}

static void write_sample(BYTE *data, DWORD bits, SHORT sample)
{
	if (bits == 8)
		data[0] = (BYTE)(((LONG)sample + 32768) >> 8);
	else {
		data[0] = (BYTE)sample;
		data[1] = (BYTE)((WORD)sample >> 8);
	}
}

static DWORD convert_pcm(const acm_stream *stream, const BYTE *source, DWORD source_bytes, BYTE *destination, DWORD destination_bytes)
{
	const DWORD frame_size = stream->source.nChannels * (stream->source.wBitsPerSample / 8);
	const DWORD frames = source_bytes / frame_size < destination_bytes / stream->destination.nBlockAlign ?
		source_bytes / frame_size : destination_bytes / stream->destination.nBlockAlign;
	for (DWORD frame = 0; frame < frames; ++frame)
		for (DWORD channel = 0; channel < stream->source.nChannels; ++channel) {
			const SHORT sample = read_sample(source + frame * frame_size + channel * (stream->source.wBitsPerSample / 8),
				stream->source.wBitsPerSample);
			write_sample(destination + frame * stream->destination.nBlockAlign +
				channel * (stream->destination.wBitsPerSample / 8), stream->destination.wBitsPerSample, sample);
		}
	return frames * stream->destination.nBlockAlign;
}

static DWORD decode_adpcm_block(const acm_stream *stream, const BYTE *block, BYTE *output, DWORD output_capacity)
{
	const DWORD channels = stream->source.nChannels;
	const DWORD frames = stream->source_frames_per_block;
	if (output_capacity < frames * stream->destination.nBlockAlign)
		return 0;
	BYTE predictor[2];
	LONG delta[2], previous[2], previous2[2];
	for (DWORD channel = 0; channel < channels; ++channel) {
		predictor[channel] = block[channel];
		if (predictor[channel] >= stream->source_coefficient_count)
			return 0;
		const BYTE *delta_value = block + channels + channel * 2;
		const BYTE *sample1 = block + channels * 3 + channel * 2;
		const BYTE *sample2 = block + channels * 5 + channel * 2;
		delta[channel] = delta_value[0] | ((WORD)delta_value[1] << 8);
		if (delta[channel] < 16)
			delta[channel] = 16;
		previous[channel] = (SHORT)(sample1[0] | ((WORD)sample1[1] << 8));
		previous2[channel] = (SHORT)(sample2[0] | ((WORD)sample2[1] << 8));
	}
	DWORD frame = 0;
	const DWORD sample_bytes = stream->destination.wBitsPerSample / 8;
	for (DWORD channel = 0; channel < channels; ++channel)
		write_sample(output + channel * sample_bytes, stream->destination.wBitsPerSample, (SHORT)previous2[channel]);
	++frame;
	for (DWORD channel = 0; channel < channels; ++channel)
		write_sample(output + frame * stream->destination.nBlockAlign + channel * (stream->destination.wBitsPerSample / 8),
			stream->destination.wBitsPerSample, (SHORT)previous[channel]);
	++frame;
	const BYTE *encoded = block + channels * 7;
	for (DWORD position = 0; frame < frames; ++position) {
		const BYTE value = encoded[position];
		const DWORD nibbles = channels == 1 ? 2 : channels;
		for (DWORD part = 0; part < nibbles && frame < frames; ++part) {
			const DWORD channel = channels == 1 ? 0 : part;
			const int code = part == 0 ? (value >> 4) & 15 : value & 15;
			const int signed_code = code & 8 ? code - 16 : code;
			const ADPCMCOEFSET coefficient = stream->source_coefficients[predictor[channel]];
			LONG sample = (previous[channel] * coefficient.iCoef1 + previous2[channel] * coefficient.iCoef2) / 256 +
				signed_code * delta[channel];
			sample = clamp_sample(sample);
			previous2[channel] = previous[channel];
			previous[channel] = sample;
			delta[channel] = adapt_delta(code, delta[channel]);
			write_sample(output + frame * stream->destination.nBlockAlign + channel * sample_bytes,
				stream->destination.wBitsPerSample, (SHORT)sample);
			if (channels == 1)
				++frame;
		}
		if (channels > 1)
			++frame;
	}
	return frames * stream->destination.nBlockAlign;
}

static int choose_predictor(const acm_stream *stream, SHORT sample0, SHORT sample1, SHORT sample2)
{
	int best = 0;
	LONG best_error = INT32_MAX;
	for (WORD i = 0; i < stream->destination_coefficient_count; ++i) {
		const ADPCMCOEFSET coefficient = stream->destination_coefficients[i];
		const LONG predicted = (sample1 * coefficient.iCoef1 + sample0 * coefficient.iCoef2) / 256;
		LONG error = (LONG)sample2 - predicted;
		if (error < 0)
			error = -error;
		if (error < best_error) {
			best = i;
			best_error = error;
		}
	}
	return best;
}

static BYTE encode_nibble(const acm_stream *stream, DWORD channel, SHORT target, BYTE predictor[], LONG delta[],
	LONG previous[], LONG previous2[])
{
	const ADPCMCOEFSET coefficient = stream->destination_coefficients[predictor[channel]];
	const LONG predicted = (previous[channel] * coefficient.iCoef1 + previous2[channel] * coefficient.iCoef2) / 256;
	LONG difference = (LONG)target - predicted;
	LONG quantized = difference >= 0 ? (difference + delta[channel] / 2) / delta[channel] :
		(difference - delta[channel] / 2) / delta[channel];
	if (quantized < -8)
		quantized = -8;
	if (quantized > 7)
		quantized = 7;
	const int code = (int)quantized & 15;
	LONG sample = clamp_sample(predicted + quantized * delta[channel]);
	previous2[channel] = previous[channel];
	previous[channel] = sample;
	delta[channel] = adapt_delta(code, delta[channel]);
	return (BYTE)code;
}

static DWORD encode_adpcm_block(const acm_stream *stream, const BYTE *input, BYTE *block, DWORD input_bytes)
{
	const DWORD channels = stream->destination.nChannels;
	const DWORD frames = stream->destination_frames_per_block;
	const DWORD frame_size = stream->source.nBlockAlign;
	if (input_bytes < frames * frame_size)
		return 0;
	BYTE predictor[2];
	LONG delta[2], previous[2], previous2[2];
	for (DWORD channel = 0; channel < channels; ++channel) {
		const SHORT first = read_sample(input + channel * (stream->source.wBitsPerSample / 8), stream->source.wBitsPerSample);
		const SHORT second = read_sample(input + frame_size + channel * (stream->source.wBitsPerSample / 8), stream->source.wBitsPerSample);
		const SHORT third = read_sample(input + 2 * frame_size + channel * (stream->source.wBitsPerSample / 8), stream->source.wBitsPerSample);
		predictor[channel] = (BYTE)choose_predictor(stream, first, second, third);
		delta[channel] = second > first ? second - first : first - second;
		if (delta[channel] < 16)
			delta[channel] = 16;
		if (delta[channel] > 32767)
			delta[channel] = 32767;
		previous[channel] = second;
		previous2[channel] = first;
		block[channel] = predictor[channel];
		BYTE *delta_value = block + channels + channel * 2;
		BYTE *sample1 = block + channels * 3 + channel * 2;
		BYTE *sample2 = block + channels * 5 + channel * 2;
		delta_value[0] = (BYTE)delta[channel];
		delta_value[1] = (BYTE)(delta[channel] >> 8);
		sample1[0] = (BYTE)second;
		sample1[1] = (BYTE)((WORD)second >> 8);
		sample2[0] = (BYTE)first;
		sample2[1] = (BYTE)((WORD)first >> 8);
	}
	BYTE *encoded = block + channels * 7;
	DWORD frame = 2, byte_index = 0;
	while (frame < frames) {
		BYTE value = 0;
		if (channels == 1) {
			value = (BYTE)(encode_nibble(stream, 0,
				read_sample(input + frame * frame_size, stream->source.wBitsPerSample),
				predictor, delta, previous, previous2) << 4);
			++frame;
			if (frame < frames) {
				value |= encode_nibble(stream, 0, read_sample(input + frame * frame_size, stream->source.wBitsPerSample),
					predictor, delta, previous, previous2);
				++frame;
			}
		} else {
			for (DWORD channel = 0; channel < channels; ++channel)
				value |= encode_nibble(stream, channel,
					read_sample(input + frame * frame_size + channel * (stream->source.wBitsPerSample / 8),
						stream->source.wBitsPerSample), predictor, delta, previous, previous2) << (channel ? 0 : 4);
			++frame;
		}
		encoded[byte_index++] = value;
	}
	return stream->destination.nBlockAlign;
}

static DWORD stream_convert(acm_stream *stream, ACMSTREAMHEADER *header, DWORD flags)
{
	if (!header || header->cbStruct < sizeof(*header) || !header->pbSrc || !header->pbDst)
		return MMSYSERR_INVALPARAM;
	if (!(header->fdwStatus & ACMSTREAMHEADER_STATUSF_PREPARED))
		return ACMERR_UNPREPARED;
	if (header->fdwStatus & ACMSTREAMHEADER_STATUSF_INQUEUE)
		return ACMERR_BUSY;
	header->cbSrcLengthUsed = 0;
	header->cbDstLengthUsed = 0;
	if (stream->source.wFormatTag == WAVE_FORMAT_PCM && stream->destination.wFormatTag == WAVE_FORMAT_PCM) {
		const DWORD bytes = convert_pcm(stream, header->pbSrc, header->cbSrcLength, header->pbDst, header->cbDstLength);
		header->cbSrcLengthUsed = bytes / stream->destination.nBlockAlign * stream->source.nBlockAlign;
		header->cbDstLengthUsed = bytes;
	} else if (stream->source.wFormatTag == WAVE_FORMAT_ADPCM) {
		DWORD source_offset = 0, destination_offset = 0;
		while (header->cbSrcLength - source_offset >= stream->source.nBlockAlign &&
			header->cbDstLength - destination_offset >= stream->source_frames_per_block * stream->destination.nBlockAlign) {
			const DWORD written = decode_adpcm_block(stream, header->pbSrc + source_offset, header->pbDst + destination_offset,
				header->cbDstLength - destination_offset);
			if (!written)
				return ACMERR_NOTPOSSIBLE;
			source_offset += stream->source.nBlockAlign;
			destination_offset += written;
		}
		header->cbSrcLengthUsed = source_offset;
		header->cbDstLengthUsed = destination_offset;
	} else {
		DWORD source_offset = 0, destination_offset = 0;
		const DWORD source_block = stream->destination_frames_per_block * stream->source.nBlockAlign;
		while (header->cbSrcLength - source_offset >= source_block &&
			header->cbDstLength - destination_offset >= stream->destination.nBlockAlign) {
			const DWORD written = encode_adpcm_block(stream, header->pbSrc + source_offset,
				header->pbDst + destination_offset, header->cbSrcLength - source_offset);
			if (!written)
				return ACMERR_NOTPOSSIBLE;
			source_offset += source_block;
			destination_offset += written;
		}
		header->cbSrcLengthUsed = source_offset;
		header->cbDstLengthUsed = destination_offset;
	}
	/* shortcut: BLOCKALIGN, START and END are ignored because every call converts whole blocks and keeps no state. */
	(void)flags;
	header->fdwStatus |= ACMSTREAMHEADER_STATUSF_DONE;
	return MMSYSERR_NOERROR;
}

__declspec(dllexport) DWORD WINAPI acmGetVersion(void) { return 0x04000000; }

__declspec(dllexport) MMRESULT WINAPI acmMetrics(HACMOBJ object, UINT metric, LPVOID value)
{
	(void)object;
	if (!value)
		return MMSYSERR_INVALPARAM;
	DWORD result;
	switch (metric) {
	case ACM_METRIC_COUNT_DRIVERS:
	case ACM_METRIC_COUNT_CODECS:
	case ACM_METRIC_COUNT_CONVERTERS:
	case ACM_METRIC_COUNT_LOCAL_DRIVERS:
	case ACM_METRIC_COUNT_LOCAL_CODECS:
	case ACM_METRIC_COUNT_LOCAL_CONVERTERS:
		result = 1;
		break;
	case ACM_METRIC_COUNT_FILTERS:
	case ACM_METRIC_COUNT_LOCAL_FILTERS:
	case ACM_METRIC_COUNT_DISABLED:
	case ACM_METRIC_COUNT_LOCAL_DISABLED:
	case ACM_METRIC_COUNT_HARDWARE:
	case ACM_METRIC_HARDWARE_WAVE_INPUT:
	case ACM_METRIC_HARDWARE_WAVE_OUTPUT:
		result = 0;
		break;
	case ACM_METRIC_MAX_SIZE_FORMAT:
		result = sizeof(WAVEFORMATEX) + 4 + sizeof(standard_coefficients);
		break;
	case ACM_METRIC_MAX_SIZE_FILTER:
		result = 0;
		break;
	default:
		return MMSYSERR_INVALPARAM;
	}
	*(DWORD *)value = result;
	return MMSYSERR_NOERROR;
}

static MMRESULT format_tag_details(DWORD tag, DWORD index, DWORD query, DWORD *out_tag, DWORD *size, DWORD *support,
	DWORD *formats)
{
	DWORD requested = tag;
	if (query == ACM_FORMATTAGDETAILSF_INDEX) {
		if (index == 0)
			requested = WAVE_FORMAT_PCM;
		else if (index == 1)
			requested = WAVE_FORMAT_ADPCM;
		else
			return ACMERR_NOTPOSSIBLE;
	} else if (query == ACM_FORMATTAGDETAILSF_FORMATTAG) {
		if (requested != WAVE_FORMAT_PCM && requested != WAVE_FORMAT_ADPCM)
			return ACMERR_NOTPOSSIBLE;
	} else if (query == ACM_FORMATTAGDETAILSF_LARGESTSIZE) {
		requested = WAVE_FORMAT_ADPCM;
	} else {
		return MMSYSERR_INVALPARAM;
	}
	if (out_tag)
		*out_tag = requested;
	if (size)
		*size = requested == WAVE_FORMAT_ADPCM ? sizeof(WAVEFORMATEX) + 4 + sizeof(standard_coefficients) : sizeof(WAVEFORMATEX);
	if (support)
		*support = ACMDRIVERDETAILS_SUPPORTF_CODEC | ACMDRIVERDETAILS_SUPPORTF_CONVERTER;
	if (formats)
		*formats = requested == WAVE_FORMAT_ADPCM ? 3 : 6;
	return MMSYSERR_NOERROR;
}

static DWORD format_tag_index(DWORD tag)
{
	return tag == WAVE_FORMAT_PCM ? 0 : 1;
}

static void copy_ascii_a(char *target, DWORD target_count, const char *source)
{
	if (!target_count)
		return;
	DWORD i = 0;
	for (; i + 1 < target_count && source[i]; ++i)
		target[i] = source[i];
	target[i] = 0;
}

static void copy_ascii_w(WCHAR *target, DWORD target_count, const char *source)
{
	if (!target_count)
		return;
	DWORD i = 0;
	for (; i + 1 < target_count && source[i]; ++i)
		target[i] = (WCHAR)(BYTE)source[i];
	target[i] = 0;
}

#define DEFINE_TAG_DETAILS(suffix, copy_fn) \
__declspec(dllexport) MMRESULT WINAPI acmFormatTagDetails##suffix(HACMDRIVER driver, LPACMFORMATTAGDETAILS##suffix details, DWORD flags) \
{ \
	(void)driver; \
	if (!details || details->cbStruct < sizeof(*details)) return MMSYSERR_INVALPARAM; \
	DWORD tag = details->dwFormatTag, size, support, formats; \
	MMRESULT result = format_tag_details(tag, details->dwFormatTagIndex, flags & ACM_FORMATTAGDETAILSF_QUERYMASK, \
		&tag, &size, &support, &formats); \
	if (result) return result; \
	details->dwFormatTag = tag; details->cbFormatSize = size; details->fdwSupport = support; details->cStandardFormats = formats; \
	details->dwFormatTagIndex = format_tag_index(tag); \
	copy_fn(details->szFormatTag, ACMFORMATTAGDETAILS_FORMATTAG_CHARS, tag == WAVE_FORMAT_PCM ? "PCM" : "Microsoft ADPCM"); \
	return MMSYSERR_NOERROR; \
}

DEFINE_TAG_DETAILS(A, copy_ascii_a)
DEFINE_TAG_DETAILS(W, copy_ascii_w)

static BOOL build_adpcm_format(LPWAVEFORMATEX output, DWORD capacity, DWORD channels, DWORD rate)
{
	const DWORD coefficient_bytes = sizeof(standard_coefficients);
	const DWORD needed = sizeof(WAVEFORMATEX) + 4 + coefficient_bytes;
	if (capacity < needed || !channels || channels > 2 || rate > UINT32_MAX / 256)
		return FALSE;
	const DWORD block_align = 256;
	const DWORD samples = ((block_align - 7 * channels) * 2 / channels) + 2;
	memset(output, 0, needed);
	output->wFormatTag = WAVE_FORMAT_ADPCM;
	output->nChannels = (WORD)channels;
	output->nSamplesPerSec = rate;
	output->nBlockAlign = (WORD)block_align;
	output->wBitsPerSample = 4;
	output->nAvgBytesPerSec = rate * block_align / samples;
	output->cbSize = (WORD)(4 + coefficient_bytes);
	ADPCMWAVEFORMAT *adpcm = (ADPCMWAVEFORMAT *)output;
	adpcm->wSamplesPerBlock = (WORD)samples;
	adpcm->wNumCoef = sizeof(standard_coefficients) / sizeof(standard_coefficients[0]);
	memcpy(adpcm->aCoef, standard_coefficients, coefficient_bytes);
	return TRUE;
}

static BOOL standard_format(DWORD tag, DWORD index, LPWAVEFORMATEX format, DWORD capacity)
{
	static const struct {
		WORD channels;
		DWORD rate;
		WORD bits;
	} pcm_formats[] = {
		{1, 22050, 8}, {1, 22050, 16}, {1, 44100, 16},
		{2, 22050, 8}, {2, 44100, 16}, {2, 48000, 16}
	};
	if (tag == WAVE_FORMAT_PCM && index < sizeof(pcm_formats) / sizeof(pcm_formats[0])) {
		const WORD channels = pcm_formats[index].channels, bits = pcm_formats[index].bits;
		const DWORD alignment = channels * (bits / 8), rate = pcm_formats[index].rate;
		if (capacity < sizeof(WAVEFORMATEX))
			return FALSE;
		memset(format, 0, sizeof(*format));
		format->wFormatTag = WAVE_FORMAT_PCM;
		format->nChannels = channels;
		format->nSamplesPerSec = rate;
		format->wBitsPerSample = bits;
		format->nBlockAlign = (WORD)alignment;
		format->nAvgBytesPerSec = rate * alignment;
		return TRUE;
	}
	if (tag == WAVE_FORMAT_ADPCM && index < 3) {
		static const WORD channels[] = {1, 1, 2};
		static const DWORD rates[] = {22050, 44100, 44100};
		return build_adpcm_format(format, capacity, channels[index], rates[index]);
	}
	return FALSE;
}

#define DEFINE_FORMAT_DETAILS(suffix, copy_fn) \
__declspec(dllexport) MMRESULT WINAPI acmFormatDetails##suffix(HACMDRIVER driver, LPACMFORMATDETAILS##suffix details, DWORD flags) \
{ \
	(void)driver; \
	if (!details || details->cbStruct < sizeof(*details) || !details->pwfx || flags & ~ACM_FORMATDETAILSF_QUERYMASK) \
		return MMSYSERR_INVALPARAM; \
	DWORD tag = details->dwFormatTag, index = details->dwFormatIndex; \
	if ((flags & ACM_FORMATDETAILSF_QUERYMASK) == ACM_FORMATDETAILSF_FORMAT) { \
		if (!supported_format(details->pwfx)) return ACMERR_NOTPOSSIBLE; \
		tag = details->pwfx->wFormatTag; \
		BOOL found = FALSE; \
		for (DWORD candidate = 0; candidate < (tag == WAVE_FORMAT_PCM ? 6u : tag == WAVE_FORMAT_ADPCM ? 3u : 0u); ++candidate) { \
			BYTE storage[sizeof(WAVEFORMATEX) + 4 + sizeof(standard_coefficients)] = {0}; \
			if (standard_format(tag, candidate, (LPWAVEFORMATEX)storage, sizeof(storage)) && \
				details->pwfx->nChannels == ((LPWAVEFORMATEX)storage)->nChannels && \
				details->pwfx->nSamplesPerSec == ((LPWAVEFORMATEX)storage)->nSamplesPerSec && \
				details->pwfx->wBitsPerSample == ((LPWAVEFORMATEX)storage)->wBitsPerSample) { \
				index = candidate; found = TRUE; break; \
			} \
		} \
		if (!found) return ACMERR_NOTPOSSIBLE; \
	} else if ((flags & ACM_FORMATDETAILSF_QUERYMASK) != ACM_FORMATDETAILSF_INDEX) return MMSYSERR_INVALPARAM; \
	const DWORD count = tag == WAVE_FORMAT_PCM ? 6 : tag == WAVE_FORMAT_ADPCM ? 3 : 0; \
	BYTE storage[sizeof(WAVEFORMATEX) + 4 + sizeof(standard_coefficients)] = {0}; \
	if (index >= count || !standard_format(tag, index, (LPWAVEFORMATEX)storage, sizeof(storage))) return ACMERR_NOTPOSSIBLE; \
	const DWORD bytes = format_bytes((LPWAVEFORMATEX)storage); \
	if (details->cbwfx < bytes) return ACMERR_NOTPOSSIBLE; \
	memcpy(details->pwfx, storage, bytes); \
	details->dwFormatTag = tag; details->dwFormatIndex = index; \
	details->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC | ACMDRIVERDETAILS_SUPPORTF_CONVERTER; \
	copy_fn(details->szFormat, ACMFORMATDETAILS_FORMAT_CHARS, tag == WAVE_FORMAT_PCM ? "PCM" : "Microsoft ADPCM"); \
	return MMSYSERR_NOERROR; \
}

DEFINE_FORMAT_DETAILS(A, copy_ascii_a)
DEFINE_FORMAT_DETAILS(W, copy_ascii_w)

#define DEFINE_TAG_ENUM(suffix) \
__declspec(dllexport) MMRESULT WINAPI acmFormatTagEnum##suffix(HACMDRIVER driver, LPACMFORMATTAGDETAILS##suffix details, \
	ACMFORMATTAGENUMCB##suffix callback, DWORD_PTR instance, DWORD flags) \
{ \
	(void)driver; \
	if (!details || !callback || (flags & ~ACM_FORMATTAGDETAILSF_QUERYMASK)) \
		return MMSYSERR_INVALPARAM; \
	for (DWORD index = 0; index < 2; ++index) { \
		ACMFORMATTAGDETAILS##suffix current = {0}; \
		current.cbStruct = sizeof(current); \
		current.dwFormatTagIndex = index; \
		if (acmFormatTagDetails##suffix(NULL, &current, ACM_FORMATTAGDETAILSF_INDEX) != MMSYSERR_NOERROR) \
			return ACMERR_NOTPOSSIBLE; \
		if (callback((HACMDRIVERID)1, &current, instance, current.fdwSupport) == FALSE) \
			break; \
	} \
	return MMSYSERR_NOERROR; \
}

DEFINE_TAG_ENUM(A)
DEFINE_TAG_ENUM(W)

/* shortcut: CONVERT, SUGGEST, INPUT and OUTPUT do not narrow the list, filter by the format in pwfx when a program relies on them. */
#define DEFINE_FORMAT_ENUM(suffix, callback_type, details_fn) \
__declspec(dllexport) MMRESULT WINAPI acmFormatEnum##suffix(HACMDRIVER driver, LPACMFORMATDETAILS##suffix filter, \
	callback_type callback, DWORD_PTR instance, DWORD flags) \
{ \
	(void)driver; \
	if (!filter || !callback || (flags & ~(ACM_FORMATENUMF_WFORMATTAG | ACM_FORMATENUMF_NCHANNELS | \
		ACM_FORMATENUMF_NSAMPLESPERSEC | ACM_FORMATENUMF_WBITSPERSAMPLE | ACM_FORMATENUMF_INPUT | \
		ACM_FORMATENUMF_OUTPUT | ACM_FORMATENUMF_CONVERT | ACM_FORMATENUMF_SUGGEST | ACM_FORMATENUMF_HARDWARE))) \
		return MMSYSERR_INVALPARAM; \
	const DWORD tags[] = {WAVE_FORMAT_PCM, WAVE_FORMAT_ADPCM}; \
	for (DWORD t = 0; t < 2; ++t) { \
		const DWORD tag = tags[t]; \
		if ((flags & ACM_FORMATENUMF_WFORMATTAG) && filter->dwFormatTag != tag) continue; \
		const DWORD count = tag == WAVE_FORMAT_PCM ? 6 : 3; \
		for (DWORD index = 0; index < count; ++index) { \
			BYTE storage[sizeof(WAVEFORMATEX) + 4 + sizeof(standard_coefficients)] = {0}; \
			ACMFORMATDETAILS##suffix current = {0}; \
			current.cbStruct = sizeof(current); current.dwFormatTag = tag; current.dwFormatIndex = index; \
			current.pwfx = (LPWAVEFORMATEX)storage; current.cbwfx = sizeof(storage); \
			if (details_fn(NULL, &current, ACM_FORMATDETAILSF_INDEX) != MMSYSERR_NOERROR) return ACMERR_NOTPOSSIBLE; \
			if ((flags & ACM_FORMATENUMF_NCHANNELS) && filter->pwfx && current.pwfx->nChannels != filter->pwfx->nChannels) continue; \
			if ((flags & ACM_FORMATENUMF_NSAMPLESPERSEC) && filter->pwfx && current.pwfx->nSamplesPerSec != filter->pwfx->nSamplesPerSec) continue; \
			if ((flags & ACM_FORMATENUMF_WBITSPERSAMPLE) && filter->pwfx && current.pwfx->wBitsPerSample != filter->pwfx->wBitsPerSample) continue; \
			if (callback((HACMDRIVERID)1, &current, instance, current.fdwSupport) == FALSE) return MMSYSERR_NOERROR; \
		} \
	} \
	return MMSYSERR_NOERROR; \
}

DEFINE_FORMAT_ENUM(A, ACMFORMATENUMCBA, acmFormatDetailsA)
DEFINE_FORMAT_ENUM(W, ACMFORMATENUMCBW, acmFormatDetailsW)

__declspec(dllexport) MMRESULT WINAPI acmDriverEnum(ACMDRIVERENUMCB callback, DWORD_PTR instance, DWORD flags)
{
	if (!callback || (flags & ~(ACM_DRIVERENUMF_NOLOCAL | ACM_DRIVERENUMF_DISABLED)))
		return MMSYSERR_INVALPARAM;
	if (flags & ACM_DRIVERENUMF_NOLOCAL)
		return MMSYSERR_NOERROR;
	callback((HACMDRIVERID)1, instance, ACMDRIVERDETAILS_SUPPORTF_CODEC | ACMDRIVERDETAILS_SUPPORTF_CONVERTER |
		ACMDRIVERDETAILS_SUPPORTF_LOCAL);
	return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI acmDriverOpen(LPHACMDRIVER driver, HACMDRIVERID id, DWORD flags)
{
	if (!driver || id != (HACMDRIVERID)1 || flags)
		return MMSYSERR_INVALPARAM;
	*driver = (HACMDRIVER)(ULONG_PTR)1;
	return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI acmDriverClose(HACMDRIVER driver, DWORD flags)
{
	return driver == (HACMDRIVER)(ULONG_PTR)1 && !flags ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

__declspec(dllexport) MMRESULT WINAPI acmDriverID(HACMOBJ object, LPHACMDRIVERID id, DWORD flags)
{
	if (!id || flags)
		return MMSYSERR_INVALPARAM;
	if (object && object != (HACMOBJ)(ULONG_PTR)1)
		return MMSYSERR_INVALHANDLE;
	*id = (HACMDRIVERID)1;
	return MMSYSERR_NOERROR;
}

#define DEFINE_DRIVER_DETAILS(suffix, copy_fn) \
__declspec(dllexport) MMRESULT WINAPI acmDriverDetails##suffix(HACMDRIVERID id, LPACMDRIVERDETAILS##suffix details, DWORD flags) \
{ \
	if (id != (HACMDRIVERID)1 || !details || details->cbStruct < sizeof(*details) || flags) return MMSYSERR_INVALPARAM; \
	details->fccType = ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC; details->fccComp = ACMDRIVERDETAILS_FCCCOMP_UNDEFINED; \
	details->wMid = 0; details->wPid = 0; details->vdwACM = 0x04000000; details->vdwDriver = 0x00010000; \
	details->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC | ACMDRIVERDETAILS_SUPPORTF_CONVERTER | ACMDRIVERDETAILS_SUPPORTF_LOCAL; \
	details->cFormatTags = 2; details->cFilterTags = 0; details->hicon = NULL; \
	copy_fn(details->szShortName, ACMDRIVERDETAILS_SHORTNAME_CHARS, "Raven ACM"); \
	copy_fn(details->szLongName, ACMDRIVERDETAILS_LONGNAME_CHARS, "Raven PCM and Microsoft ADPCM Converter"); \
	copy_fn(details->szCopyright, ACMDRIVERDETAILS_COPYRIGHT_CHARS, "Raven"); \
	copy_fn(details->szLicensing, ACMDRIVERDETAILS_LICENSING_CHARS, ""); \
	copy_fn(details->szFeatures, ACMDRIVERDETAILS_FEATURES_CHARS, "PCM and Microsoft ADPCM conversion"); \
	return MMSYSERR_NOERROR; \
}

DEFINE_DRIVER_DETAILS(A, copy_ascii_a)
DEFINE_DRIVER_DETAILS(W, copy_ascii_w)

__declspec(dllexport) MMRESULT WINAPI acmFormatSuggest(HACMDRIVER driver, LPWAVEFORMATEX source, LPWAVEFORMATEX destination,
	DWORD destination_size, DWORD flags)
{
	(void)driver;
	if (!source || !destination || flags & ~ACM_FORMATSUGGESTF_TYPEMASK ||
		(flags & ACM_FORMATSUGGESTF_TYPEMASK & ~(ACM_FORMATSUGGESTF_WFORMATTAG | ACM_FORMATSUGGESTF_NCHANNELS |
			ACM_FORMATSUGGESTF_NSAMPLESPERSEC | ACM_FORMATSUGGESTF_WBITSPERSAMPLE)))
		return MMSYSERR_INVALPARAM;
	if (!supported_format(source))
		return ACMERR_NOTPOSSIBLE;
	const DWORD requested_tag = flags & ACM_FORMATSUGGESTF_WFORMATTAG ? destination->wFormatTag : WAVE_FORMAT_PCM;
	const WORD requested_channels = flags & ACM_FORMATSUGGESTF_NCHANNELS ? destination->nChannels : source->nChannels;
	const DWORD requested_rate = flags & ACM_FORMATSUGGESTF_NSAMPLESPERSEC ? destination->nSamplesPerSec : source->nSamplesPerSec;
	const WORD requested_bits = flags & ACM_FORMATSUGGESTF_WBITSPERSAMPLE ? destination->wBitsPerSample : 0;
	if (requested_channels != source->nChannels || requested_rate != source->nSamplesPerSec)
		return ACMERR_NOTPOSSIBLE;
	if (requested_tag == WAVE_FORMAT_ADPCM) {
		if (!pcm_format(source) || (requested_bits && requested_bits != 4))
			return ACMERR_NOTPOSSIBLE;
		if (!build_adpcm_format(destination, destination_size, source->nChannels, source->nSamplesPerSec))
			return ACMERR_NOTPOSSIBLE;
		return MMSYSERR_NOERROR;
	}
	if (requested_tag != WAVE_FORMAT_PCM)
		return ACMERR_NOTPOSSIBLE;
	if (destination_size < sizeof(WAVEFORMATEX))
		return MMSYSERR_INVALPARAM;
	WAVEFORMATEX suggested = *source;
	suggested.wFormatTag = WAVE_FORMAT_PCM;
	suggested.wBitsPerSample = requested_bits ? requested_bits :
		source->wFormatTag == WAVE_FORMAT_ADPCM ? 16 : source->wBitsPerSample;
	if (suggested.wBitsPerSample != 8 && suggested.wBitsPerSample != 16)
		return ACMERR_NOTPOSSIBLE;
	suggested.nBlockAlign = suggested.nChannels * (suggested.wBitsPerSample / 8);
	if (suggested.nSamplesPerSec > UINT32_MAX / suggested.nBlockAlign)
		return ACMERR_NOTPOSSIBLE;
	suggested.nAvgBytesPerSec = suggested.nSamplesPerSec * suggested.nBlockAlign;
	suggested.cbSize = 0;
	*destination = suggested;
	return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI acmStreamOpen(LPHACMSTREAM out, HACMDRIVER driver, LPWAVEFORMATEX source,
	LPWAVEFORMATEX destination, LPWAVEFILTER filter, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
	(void)driver;
	(void)filter;
	(void)callback;
	(void)instance;
	if (!source || !destination || flags & ~(ACM_STREAMOPENF_QUERY | ACM_STREAMOPENF_NONREALTIME))
		return MMSYSERR_INVALPARAM;
	if (!supported_pair(source, destination))
		return ACMERR_NOTPOSSIBLE;
	if (flags & ACM_STREAMOPENF_QUERY)
		return MMSYSERR_NOERROR;
	if (!out)
		return MMSYSERR_INVALPARAM;
	*out = NULL;
	acm_stream *stream = LocalAlloc(LPTR, sizeof(*stream));
	if (!stream)
		return MMSYSERR_NOMEM;
	if (!init_stream(stream, source, destination)) {
		LocalFree(stream);
		return ACMERR_NOTPOSSIBLE;
	}
	*out = (HACMSTREAM)stream;
	return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI acmStreamClose(HACMSTREAM handle, DWORD flags)
{
	(void)flags;
	if (!handle)
		return MMSYSERR_INVALHANDLE;
	LocalFree(handle);
	return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI acmStreamPrepareHeader(HACMSTREAM handle, LPACMSTREAMHEADER header, DWORD flags)
{
	(void)flags;
	if (!handle || !header || header->cbStruct < sizeof(*header))
		return MMSYSERR_INVALPARAM;
	if (header->fdwStatus & ACMSTREAMHEADER_STATUSF_INQUEUE)
		return ACMERR_BUSY;
	header->fdwStatus |= ACMSTREAMHEADER_STATUSF_PREPARED;
	return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI acmStreamUnprepareHeader(HACMSTREAM handle, LPACMSTREAMHEADER header, DWORD flags)
{
	(void)flags;
	if (!handle || !header || header->cbStruct < sizeof(*header))
		return MMSYSERR_INVALPARAM;
	if (!(header->fdwStatus & ACMSTREAMHEADER_STATUSF_PREPARED))
		return ACMERR_UNPREPARED;
	if (header->fdwStatus & ACMSTREAMHEADER_STATUSF_INQUEUE)
		return ACMERR_BUSY;
	header->fdwStatus &= ~ACMSTREAMHEADER_STATUSF_PREPARED;
	return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI acmStreamConvert(HACMSTREAM handle, LPACMSTREAMHEADER header, DWORD flags)
{
	return handle ? stream_convert((acm_stream *)handle, header, flags) : MMSYSERR_INVALHANDLE;
}

/* A stream keeps no state between conversions, so there is nothing to reset. */
__declspec(dllexport) MMRESULT WINAPI acmStreamReset(HACMSTREAM handle, DWORD flags)
{
	(void)flags;
	return handle ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

__declspec(dllexport) MMRESULT WINAPI acmStreamSize(HACMSTREAM handle, DWORD input, LPDWORD output, DWORD flags)
{
	if (!handle || !output)
		return MMSYSERR_INVALPARAM;
	if (!input)
		return MMSYSERR_INVALPARAM;
	acm_stream *stream = (acm_stream *)handle;
	const DWORD query = flags & ACM_STREAMSIZEF_QUERYMASK;
	if (query == ACM_STREAMSIZEF_SOURCE) {
		const DWORD source_blocks = input / stream->source_align;
		if (source_blocks > UINT32_MAX / stream->source_frames_per_block)
			return MMSYSERR_INVALPARAM;
		const DWORD frames = source_blocks * stream->source_frames_per_block;
		const DWORD destination_blocks = frames / stream->destination_frames_per_block;
		if (destination_blocks > UINT32_MAX / stream->destination_align)
			return MMSYSERR_INVALPARAM;
		const DWORD bytes = destination_blocks * stream->destination_align;
		if (!bytes)
			return ACMERR_NOTPOSSIBLE;
		*output = bytes;
		return MMSYSERR_NOERROR;
	}
	if (query == ACM_STREAMSIZEF_DESTINATION) {
		const DWORD destination_blocks = input / stream->destination_align;
		if (destination_blocks > UINT32_MAX / stream->destination_frames_per_block)
			return MMSYSERR_INVALPARAM;
		const DWORD frames = destination_blocks * stream->destination_frames_per_block;
		const DWORD source_blocks = frames / stream->source_frames_per_block;
		if (source_blocks > UINT32_MAX / stream->source_align)
			return MMSYSERR_INVALPARAM;
		const DWORD bytes = source_blocks * stream->source_align;
		if (!bytes)
			return ACMERR_NOTPOSSIBLE;
		*output = bytes;
		return MMSYSERR_NOERROR;
	}
	return MMSYSERR_INVALPARAM;
}

__declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	(void)instance;
	(void)reason;
	(void)reserved;
	return TRUE;
}
