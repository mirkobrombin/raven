/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven audio resampler component: resampledmo.dll, the Media Foundation transform a program changes the sample rate of its sound
 * with (CLSID_CResamplerMediaObject). A WASAPI program that mixes at one rate and plays at another creates it, and some create it
 * before they know whether they need it.
 *
 * One input and one output stream of 16-bit PCM or 32-bit float, with the same channels on both sides. */
#define INITGUID
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <string.h>

DEFINE_GUID(CLSID_RavenResampler, 0xf447b69e, 0x1884, 0x4a7e, 0x80, 0x55, 0x34, 0x6f, 0x74, 0xd6, 0xed, 0xb3);
DEFINE_GUID(IID_RavenResamplerProps, 0xe7e9984f, 0xf09f, 0x4da4, 0x90, 0x3f, 0x6e, 0x2e, 0x0e, 0xfe, 0x56, 0xb5);

/* IWMResamplerProps, which the MinGW headers do not declare. */
typedef struct props props;
typedef struct props_table {
	HRESULT (WINAPI *query)(props *, REFIID, void **);
	ULONG (WINAPI *reference)(props *);
	ULONG (WINAPI *release)(props *);
	HRESULT (WINAPI *half_filter_length)(props *, LONG);
	HRESULT (WINAPI *channel_matrix)(props *, void *);
} props_table;
struct props { const props_table *table; };

enum { MAX_CHANNELS = 8, MAX_RATE = 384000 };

typedef struct format {
	UINT32 rate, channels, block;
	BOOL is_float;
} format;

typedef struct resampler {
	IMFTransform transform;
	props properties;
	LONG ref;
	CRITICAL_SECTION lock;
	IMFMediaType *types[2];
	format formats[2];
	/* The input not yet used, as float frames. The first frame is the one before the position, which the curve needs. */
	float *frames;
	UINT32 count, capacity;
	/* Where the next output frame falls in the frames, and how many frames went out since the last flush. */
	double position;
	UINT64 written;
} resampler;

static volatile LONG objects;

static resampler *from_props(props *iface) { return CONTAINING_RECORD(iface, resampler, properties); }
static resampler *from_transform(IMFTransform *iface) { return CONTAINING_RECORD(iface, resampler, transform); }

static void reset(resampler *self)
{
	/* One silent frame stands for the sound before the stream. */
	if (self->frames)
		memset(self->frames, 0, sizeof(float) * MAX_CHANNELS);
	self->count = self->frames ? 1 : 0;
	self->position = 1;
	self->written = 0;
}

/* Whether the frames hold the four points around the position. */
static BOOL ready(const resampler *self) { return (UINT32)self->position + 2 < self->count; }

static HRESULT read_format(IMFMediaType *type, format *out)
{
	GUID major, subtype;
	UINT32 bits = 0;
	if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major)) || !IsEqualGUID(&major, &MFMediaType_Audio) ||
		FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)) ||
		FAILED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &out->channels)) ||
		FAILED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &out->rate)) ||
		FAILED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)))
		return MF_E_INVALIDMEDIATYPE;
	out->is_float = IsEqualGUID(&subtype, &MFAudioFormat_Float);
	/* shortcut: 16-bit PCM and 32-bit float only, add 8, 24 and 32-bit PCM when a program asks for them. */
	if ((!out->is_float && !IsEqualGUID(&subtype, &MFAudioFormat_PCM)) || bits != (out->is_float ? 32U : 16U) || !out->channels ||
		out->channels > MAX_CHANNELS || !out->rate || out->rate > MAX_RATE)
		return MF_E_INVALIDMEDIATYPE;
	out->block = out->channels * bits / 8;
	return S_OK;
}

/* ---- IWMResamplerProps. ---- */

static HRESULT WINAPI props_query(props *iface, REFIID iid, void **out) { return IMFTransform_QueryInterface(&from_props(iface)->transform, iid, out); }
static ULONG WINAPI props_reference(props *iface) { return IMFTransform_AddRef(&from_props(iface)->transform); }
static ULONG WINAPI props_release(props *iface) { return IMFTransform_Release(&from_props(iface)->transform); }
/* The quality a program asks for, from 1 to 60. The curve here has one quality. */
static HRESULT WINAPI props_half_filter_length(props *iface, LONG length) { return length < 1 || length > 60 ? E_INVALIDARG : S_OK; }
static HRESULT WINAPI props_channel_matrix(props *iface, void *matrix) { return E_NOTIMPL; }
static const props_table properties_table = {props_query, props_reference, props_release, props_half_filter_length, props_channel_matrix};

/* ---- IMFTransform. ---- */

static HRESULT WINAPI transform_query(IMFTransform *iface, REFIID iid, void **out)
{
	resampler *self = from_transform(iface);
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMFTransform))
		*out = &self->transform;
	else if (IsEqualGUID(iid, &IID_RavenResamplerProps))
		*out = &self->properties;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->ref);
	return S_OK;
}

static ULONG WINAPI transform_reference(IMFTransform *iface) { return InterlockedIncrement(&from_transform(iface)->ref); }

static ULONG WINAPI transform_release(IMFTransform *iface)
{
	resampler *self = from_transform(iface);
	const ULONG left = InterlockedDecrement(&self->ref);
	if (!left) {
		for (int side = 0; side < 2; ++side)
			if (self->types[side])
				IMFMediaType_Release(self->types[side]);
		HeapFree(GetProcessHeap(), 0, self->frames);
		DeleteCriticalSection(&self->lock);
		HeapFree(GetProcessHeap(), 0, self);
		InterlockedDecrement(&objects);
	}
	return left;
}

static HRESULT WINAPI transform_stream_limits(IMFTransform *iface, DWORD *input_minimum, DWORD *input_maximum, DWORD *output_minimum,
	DWORD *output_maximum)
{
	if (!input_minimum || !input_maximum || !output_minimum || !output_maximum)
		return E_POINTER;
	*input_minimum = *input_maximum = *output_minimum = *output_maximum = 1;
	return S_OK;
}

static HRESULT WINAPI transform_stream_count(IMFTransform *iface, DWORD *inputs, DWORD *outputs)
{
	if (!inputs || !outputs)
		return E_POINTER;
	*inputs = *outputs = 1;
	return S_OK;
}

static HRESULT WINAPI transform_stream_ids(IMFTransform *iface, DWORD inputs, DWORD *input, DWORD outputs, DWORD *output) { return E_NOTIMPL; }

static HRESULT WINAPI transform_input_info(IMFTransform *iface, DWORD stream, MFT_INPUT_STREAM_INFO *info)
{
	resampler *self = from_transform(iface);
	if (!info)
		return E_POINTER;
	if (stream)
		return MF_E_INVALIDSTREAMNUMBER;
	if (!self->types[0])
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	memset(info, 0, sizeof *info);
	info->cbSize = self->formats[0].block;
	return S_OK;
}

static HRESULT WINAPI transform_output_info(IMFTransform *iface, DWORD stream, MFT_OUTPUT_STREAM_INFO *info)
{
	resampler *self = from_transform(iface);
	if (!info)
		return E_POINTER;
	if (stream)
		return MF_E_INVALIDSTREAMNUMBER;
	if (!self->types[1])
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	memset(info, 0, sizeof *info);
	info->cbSize = self->formats[1].block;
	return S_OK;
}

static HRESULT WINAPI transform_attributes(IMFTransform *iface, IMFAttributes **attributes) { return E_NOTIMPL; }
static HRESULT WINAPI transform_stream_attributes(IMFTransform *iface, DWORD stream, IMFAttributes **attributes) { return E_NOTIMPL; }
static HRESULT WINAPI transform_delete_input(IMFTransform *iface, DWORD stream) { return E_NOTIMPL; }
static HRESULT WINAPI transform_add_inputs(IMFTransform *iface, DWORD count, DWORD *streams) { return E_NOTIMPL; }

/* The types a side takes: float first, then PCM. A caller fills in the channels and the rate. */
static HRESULT WINAPI transform_available_type(IMFTransform *iface, DWORD stream, DWORD index, IMFMediaType **type)
{
	if (!type)
		return E_POINTER;
	*type = NULL;
	if (stream)
		return MF_E_INVALIDSTREAMNUMBER;
	if (index > 1)
		return MF_E_NO_MORE_TYPES;
	HRESULT result = MFCreateMediaType(type);
	if (FAILED(result))
		return result;
	IMFMediaType_SetGUID(*type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
	IMFMediaType_SetGUID(*type, &MF_MT_SUBTYPE, index ? &MFAudioFormat_PCM : &MFAudioFormat_Float);
	IMFMediaType_SetUINT32(*type, &MF_MT_AUDIO_BITS_PER_SAMPLE, index ? 16 : 32);
	return S_OK;
}

static HRESULT set_type(resampler *self, int side, DWORD stream, IMFMediaType *type, DWORD flags)
{
	format parsed = {0};
	IMFMediaType *copy = NULL;
	if (stream)
		return MF_E_INVALIDSTREAMNUMBER;
	if (flags & ~(DWORD)MFT_SET_TYPE_TEST_ONLY)
		return E_INVALIDARG;
	if (type) {
		HRESULT result = read_format(type, &parsed);
		if (FAILED(result))
			return result;
		if (self->types[!side] && self->formats[!side].channels != parsed.channels)
			return MF_E_INVALIDMEDIATYPE;
		if (flags & MFT_SET_TYPE_TEST_ONLY)
			return S_OK;
		/* A copy: the program may change or release its type after the call. */
		if (FAILED(result = MFCreateMediaType(&copy)))
			return result;
		if (FAILED(result = IMFMediaType_CopyAllItems(type, (IMFAttributes *)copy))) {
			IMFMediaType_Release(copy);
			return result;
		}
	} else if (flags & MFT_SET_TYPE_TEST_ONLY)
		return S_OK;
	EnterCriticalSection(&self->lock);
	if (self->types[side])
		IMFMediaType_Release(self->types[side]);
	self->types[side] = copy;
	self->formats[side] = parsed;
	reset(self);
	LeaveCriticalSection(&self->lock);
	return S_OK;
}

static HRESULT WINAPI transform_set_input(IMFTransform *iface, DWORD stream, IMFMediaType *type, DWORD flags)
{
	return set_type(from_transform(iface), 0, stream, type, flags);
}

static HRESULT WINAPI transform_set_output(IMFTransform *iface, DWORD stream, IMFMediaType *type, DWORD flags)
{
	return set_type(from_transform(iface), 1, stream, type, flags);
}

static HRESULT current_type(resampler *self, int side, DWORD stream, IMFMediaType **type)
{
	if (!type)
		return E_POINTER;
	*type = NULL;
	if (stream)
		return MF_E_INVALIDSTREAMNUMBER;
	if (!self->types[side])
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	HRESULT result = MFCreateMediaType(type);
	if (SUCCEEDED(result) && FAILED(result = IMFMediaType_CopyAllItems(self->types[side], (IMFAttributes *)*type))) {
		IMFMediaType_Release(*type);
		*type = NULL;
	}
	return result;
}

static HRESULT WINAPI transform_input_type(IMFTransform *iface, DWORD stream, IMFMediaType **type)
{
	return current_type(from_transform(iface), 0, stream, type);
}

static HRESULT WINAPI transform_output_type(IMFTransform *iface, DWORD stream, IMFMediaType **type)
{
	return current_type(from_transform(iface), 1, stream, type);
}

/* The most input held: a second of it. More is refused until the program takes output. */
static BOOL accepts(const resampler *self) { return self->count <= self->formats[0].rate; }

static HRESULT WINAPI transform_input_status(IMFTransform *iface, DWORD stream, DWORD *flags)
{
	resampler *self = from_transform(iface);
	if (!flags)
		return E_POINTER;
	if (stream)
		return MF_E_INVALIDSTREAMNUMBER;
	if (!self->types[0] || !self->types[1])
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	*flags = accepts(self) ? MFT_INPUT_STATUS_ACCEPT_DATA : 0;
	return S_OK;
}

static HRESULT WINAPI transform_output_status(IMFTransform *iface, DWORD *flags)
{
	resampler *self = from_transform(iface);
	if (!flags)
		return E_POINTER;
	if (!self->types[0] || !self->types[1])
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	*flags = ready(self) ? MFT_OUTPUT_STATUS_SAMPLE_READY : 0;
	return S_OK;
}

static HRESULT WINAPI transform_output_bounds(IMFTransform *iface, LONGLONG lower, LONGLONG upper) { return E_NOTIMPL; }
static HRESULT WINAPI transform_event(IMFTransform *iface, DWORD stream, IMFMediaEvent *event) { return E_NOTIMPL; }

static HRESULT WINAPI transform_message(IMFTransform *iface, MFT_MESSAGE_TYPE message, ULONG_PTR parameter)
{
	resampler *self = from_transform(iface);
	if (message == MFT_MESSAGE_COMMAND_FLUSH) {
		EnterCriticalSection(&self->lock);
		reset(self);
		LeaveCriticalSection(&self->lock);
	}
	return S_OK;
}

static HRESULT take_input(resampler *self, const BYTE *bytes, DWORD length)
{
	const format *in = &self->formats[0];
	const UINT32 added = length / in->block;
	if (!accepts(self))
		return MF_E_NOTACCEPTING;
	if (self->count + added > self->capacity || !self->frames) {
		const UINT32 capacity = self->count + added + 1;
		float *grown = self->frames ? HeapReAlloc(GetProcessHeap(), 0, self->frames, sizeof(float) * MAX_CHANNELS * capacity) :
			HeapAlloc(GetProcessHeap(), 0, sizeof(float) * MAX_CHANNELS * capacity);
		if (!grown)
			return E_OUTOFMEMORY;
		const BOOL first = !self->frames;
		self->frames = grown;
		self->capacity = capacity;
		if (first)
			reset(self);
	}
	for (UINT32 frame = 0; frame < added; ++frame)
		for (UINT32 channel = 0; channel < in->channels; ++channel) {
			const UINT32 at = frame * in->channels + channel;
			float value;
			if (in->is_float)
				memcpy(&value, bytes + at * 4, 4);
			else {
				SHORT sample;
				memcpy(&sample, bytes + at * 2, 2);
				value = sample / 32768.0f;
			}
			self->frames[(self->count + frame) * MAX_CHANNELS + channel] = value;
		}
	self->count += added;
	return S_OK;
}

static HRESULT WINAPI transform_process_input(IMFTransform *iface, DWORD stream, IMFSample *sample, DWORD flags)
{
	resampler *self = from_transform(iface);
	IMFMediaBuffer *buffer = NULL;
	BYTE *bytes = NULL;
	DWORD length = 0;
	if (!sample)
		return E_POINTER;
	if (stream)
		return MF_E_INVALIDSTREAMNUMBER;
	if (flags)
		return E_INVALIDARG;
	if (!self->types[0] || !self->types[1])
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	HRESULT result = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
	if (FAILED(result))
		return result;
	if (SUCCEEDED(result = IMFMediaBuffer_Lock(buffer, &bytes, NULL, &length))) {
		EnterCriticalSection(&self->lock);
		result = take_input(self, bytes, length);
		LeaveCriticalSection(&self->lock);
		IMFMediaBuffer_Unlock(buffer);
	}
	IMFMediaBuffer_Release(buffer);
	return result;
}

/* Writes up to `room` output frames and returns how many.
 * shortcut: a cubic curve through four frames with no low-pass filter, so a much lower output rate aliases; use a windowed sinc when a
 * program needs that quality. */
static UINT32 give_output(resampler *self, BYTE *bytes, UINT32 room)
{
	const format *out = &self->formats[1];
	const double step = (double)self->formats[0].rate / out->rate;
	UINT32 done = 0;
	for (; done < room && ready(self); ++done, self->position += step) {
		const UINT32 base = (UINT32)self->position;
		const float t = (float)(self->position - base);
		for (UINT32 channel = 0; channel < out->channels; ++channel) {
			const float *at = self->frames + base * MAX_CHANNELS + channel;
			const float before = at[-MAX_CHANNELS], here = at[0], next = at[MAX_CHANNELS], after = at[2 * MAX_CHANNELS];
			float value = here + 0.5f * t * (next - before + t * (2 * before - 5 * here + 4 * next - after +
				t * (3 * (here - next) + after - before)));
			const UINT32 index = done * out->channels + channel;
			if (out->is_float)
				memcpy(bytes + index * 4, &value, 4);
			else {
				value = value > 1 ? 1 : value < -1 ? -1 : value;
				const SHORT sample = (SHORT)(value * 32767.0f);
				memcpy(bytes + index * 2, &sample, 2);
			}
		}
	}
	/* Drops the frames before the one the curve still reads. */
	/* A step past the end leaves a position in frames that have yet to come. */
	const UINT32 drop = (UINT32)self->position - 1 < self->count ? (UINT32)self->position - 1 : self->count;
	if (drop) {
		memmove(self->frames, self->frames + drop * MAX_CHANNELS, sizeof(float) * MAX_CHANNELS * (self->count - drop));
		self->count -= drop;
		self->position -= drop;
	}
	return done;
}

static HRESULT WINAPI transform_process_output(IMFTransform *iface, DWORD flags, DWORD count, MFT_OUTPUT_DATA_BUFFER *samples, DWORD *status)
{
	resampler *self = from_transform(iface);
	IMFMediaBuffer *buffer = NULL;
	BYTE *bytes = NULL;
	DWORD room = 0;
	if (!samples || !status)
		return E_POINTER;
	if (flags || count != 1)
		return E_INVALIDARG;
	if (!self->types[0] || !self->types[1])
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	/* The program brings the sample the output goes into. */
	if (!samples[0].pSample)
		return E_INVALIDARG;
	*status = 0;
	samples[0].dwStatus = 0;
	HRESULT result = IMFSample_ConvertToContiguousBuffer(samples[0].pSample, &buffer);
	if (FAILED(result))
		return result;
	if (SUCCEEDED(result = IMFMediaBuffer_Lock(buffer, &bytes, &room, NULL))) {
		EnterCriticalSection(&self->lock);
		const format *out = &self->formats[1];
		const UINT64 start = self->written;
		const UINT32 done = give_output(self, bytes, room / out->block);
		self->written += done;
		const BOOL more = ready(self);
		LeaveCriticalSection(&self->lock);
		IMFMediaBuffer_Unlock(buffer);
		if (!done)
			result = MF_E_TRANSFORM_NEED_MORE_INPUT;
		else {
			IMFMediaBuffer_SetCurrentLength(buffer, done * out->block);
			IMFSample_SetSampleTime(samples[0].pSample, (LONGLONG)(start * 10000000ULL / out->rate));
			IMFSample_SetSampleDuration(samples[0].pSample, (LONGLONG)(done * 10000000ULL / out->rate));
			if (more)
				samples[0].dwStatus = MFT_OUTPUT_DATA_BUFFER_INCOMPLETE;
		}
	}
	IMFMediaBuffer_Release(buffer);
	return result;
}

static IMFTransformVtbl transform_table = {transform_query, transform_reference, transform_release, transform_stream_limits,
	transform_stream_count, transform_stream_ids, transform_input_info, transform_output_info, transform_attributes,
	transform_stream_attributes, transform_stream_attributes, transform_delete_input, transform_add_inputs, transform_available_type,
	transform_available_type, transform_set_input, transform_set_output, transform_input_type, transform_output_type,
	transform_input_status, transform_output_status, transform_output_bounds, transform_event, transform_message,
	transform_process_input, transform_process_output};

/* ---- The class factory. ---- */

static HRESULT WINAPI factory_query(IClassFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_reference(IClassFactory *iface) { return 2; }
static ULONG WINAPI factory_release(IClassFactory *iface) { return 1; }

static HRESULT WINAPI factory_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	resampler *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->transform.lpVtbl = &transform_table;
	self->properties.table = &properties_table;
	self->ref = 1;
	self->position = 1;
	InitializeCriticalSection(&self->lock);
	InterlockedIncrement(&objects);
	const HRESULT result = transform_query(&self->transform, iid, out);
	transform_release(&self->transform);
	return result;
}

static HRESULT WINAPI factory_lock(IClassFactory *iface, BOOL lock) { return S_OK; }

static IClassFactoryVtbl factory_table = {factory_query, factory_reference, factory_release, factory_create, factory_lock};
static IClassFactory factory = {&factory_table};

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualGUID(clsid, &CLSID_RavenResampler))
		return CLASS_E_CLASSNOTAVAILABLE;
	return factory_query(&factory, iid, out);
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
