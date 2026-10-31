/* SPDX-License-Identifier: MIT */
#include "wic.h"

/* The four kinds of component information share their first methods, so one object serves them all, each kind with its own table. */
typedef struct
{
	const void *const *table;
	LONG references;
	WICComponentType type;
	const wic_codec *codec;
	GUID format;
	const WCHAR *name;
	const GUID *clsid;
} info;

static HRESULT copy_text(const WCHAR *text, UINT room, WCHAR *out, UINT *actual)
{
	const UINT length = text ? lstrlenW(text) + 1 : 1;
	if (!actual)
		return E_INVALIDARG;
	*actual = length;
	if (room && !out)
		return E_INVALIDARG;
	if (!room)
		return S_OK;
	if (room < length)
		return WINCODEC_ERR_INSUFFICIENTBUFFER;
	if (text)
		lstrcpyW(out, text);
	else
		out[0] = 0;
	return S_OK;
}

static HRESULT WINAPI info_QueryInterface(info *self, REFIID riid, void **out)
{
	BOOL wanted = IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICComponentInfo);
	if (!out)
		return E_INVALIDARG;
	switch (self->type) {
	case WICDecoder:
		wanted |= IsEqualGUID(riid, &IID_IWICBitmapCodecInfo) || IsEqualGUID(riid, &IID_IWICBitmapDecoderInfo);
		break;
	case WICEncoder:
		wanted |= IsEqualGUID(riid, &IID_IWICBitmapCodecInfo) || IsEqualGUID(riid, &IID_IWICBitmapEncoderInfo);
		break;
	case WICPixelFormat:
		wanted |= IsEqualGUID(riid, &IID_IWICPixelFormatInfo) || IsEqualGUID(riid, &IID_IWICPixelFormatInfo2);
		break;
	default:
		wanted |= IsEqualGUID(riid, &IID_IWICFormatConverterInfo);
		break;
	}
	if (!wanted) {
		*out = NULL;
		return E_NOINTERFACE;
	}
	*out = self;
	InterlockedIncrement(&self->references);
	return S_OK;
}

static ULONG WINAPI info_AddRef(info *self)
{
	return InterlockedIncrement(&self->references);
}

static ULONG WINAPI info_Release(info *self)
{
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left)
		wic_free(self);
	return left;
}

static HRESULT WINAPI info_GetComponentType(info *self, WICComponentType *type)
{
	if (!type)
		return E_INVALIDARG;
	*type = self->type;
	return S_OK;
}

static HRESULT WINAPI info_GetCLSID(info *self, CLSID *clsid)
{
	if (!clsid)
		return E_INVALIDARG;
	*clsid = *self->clsid;
	return S_OK;
}

static HRESULT WINAPI info_GetSigningStatus(info *self, DWORD *status)
{
	if (!status)
		return E_INVALIDARG;
	*status = WICComponentSigned;
	return S_OK;
}

static HRESULT WINAPI info_GetAuthor(info *self, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(L"Raven", room, out, actual);
}

static HRESULT WINAPI info_GetVendorGUID(info *self, GUID *vendor)
{
	if (!vendor)
		return E_INVALIDARG;
	*vendor = GUID_VendorMicrosoft;
	return S_OK;
}

static HRESULT WINAPI info_GetVersion(info *self, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(L"1.0.0.0", room, out, actual);
}

static HRESULT WINAPI info_GetFriendlyName(info *self, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(self->name, room, out, actual);
}

static HRESULT WINAPI codec_GetContainerFormat(info *self, GUID *format)
{
	if (!format)
		return E_INVALIDARG;
	*format = *self->codec->container;
	return S_OK;
}

static HRESULT WINAPI codec_GetPixelFormats(info *self, UINT room, GUID *formats, UINT *actual)
{
	static const GUID *const common[] = {&GUID_WICPixelFormat8bppIndexed, &GUID_WICPixelFormat24bppBGR, &GUID_WICPixelFormat32bppBGR, &GUID_WICPixelFormat32bppBGRA};
	UINT index;
	if (!actual)
		return E_INVALIDARG;
	if (!formats) {
		if (room)
			return E_INVALIDARG;
		*actual = ARRAYSIZE(common);
		return S_OK;
	}
	*actual = min(room, ARRAYSIZE(common));
	for (index = 0; index < *actual; index++)
		formats[index] = *common[index];
	return S_OK;
}

static HRESULT WINAPI codec_GetDeviceManufacturer(info *self, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(NULL, room, out, actual);
}

static HRESULT WINAPI codec_GetMimeTypes(info *self, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(self->codec->mime_types, room, out, actual);
}

static HRESULT WINAPI codec_GetFileExtensions(info *self, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(self->codec->extensions, room, out, actual);
}

static HRESULT answer(BOOL *out, BOOL value)
{
	if (!out)
		return E_INVALIDARG;
	*out = value;
	return S_OK;
}

static HRESULT WINAPI codec_DoesSupportAnimation(info *self, BOOL *out)
{
	return answer(out, IsEqualGUID(self->codec->container, &GUID_ContainerFormatGif));
}

static HRESULT WINAPI codec_DoesSupportChromakey(info *self, BOOL *out)
{
	return answer(out, IsEqualGUID(self->codec->container, &GUID_ContainerFormatGif));
}

static HRESULT WINAPI codec_DoesSupportLossless(info *self, BOOL *out)
{
	return answer(out, TRUE);
}

static HRESULT WINAPI codec_DoesSupportMultiframe(info *self, BOOL *out)
{
	return answer(out, IsEqualGUID(self->codec->container, &GUID_ContainerFormatGif) || IsEqualGUID(self->codec->container, &GUID_ContainerFormatIco));
}

static HRESULT WINAPI codec_MatchesMimeType(info *self, LPCWSTR type, BOOL *out)
{
	const WCHAR *at = self->codec->mime_types;
	const int length = type ? lstrlenW(type) : 0;
	if (!type || !out)
		return E_INVALIDARG;
	*out = FALSE;
	while (at && *at) {
		const WCHAR *end = at;
		while (*end && *end != L',')
			end++;
		if (end - at == length && CompareStringOrdinal(at, length, type, length, TRUE) == CSTR_EQUAL)
			*out = TRUE;
		at = *end ? end + 1 : end;
	}
	return S_OK;
}

static HRESULT WINAPI decoder_GetPatterns(info *self, UINT room, WICBitmapPattern *patterns, UINT *count, UINT *actual)
{
	const UINT size = self->codec->pattern_size, needed = sizeof(WICBitmapPattern) + size * 2;
	if (!actual)
		return E_INVALIDARG;
	*actual = needed;
	if (count)
		*count = 1;
	if (!room)
		return S_OK;
	if (!patterns)
		return E_INVALIDARG;
	if (room < needed)
		return WINCODEC_ERR_INSUFFICIENTBUFFER;
	{
		BYTE *bytes = (BYTE *)(patterns + 1);
		patterns->Position.QuadPart = 0;
		patterns->Length = size;
		patterns->Pattern = bytes;
		patterns->Mask = bytes + size;
		patterns->EndOfStream = FALSE;
		memcpy(bytes, self->codec->pattern, size);
		memset(bytes + size, 0xff, size);
	}
	return S_OK;
}

static HRESULT WINAPI decoder_MatchesPattern(info *self, IStream *stream, BOOL *out)
{
	BYTE head[16];
	ULONG got = 0;
	LARGE_INTEGER start = {{0, 0}};
	HRESULT result;
	if (!stream || !out)
		return E_INVALIDARG;
	if (FAILED(result = IStream_Seek(stream, start, STREAM_SEEK_SET, NULL)) || FAILED(result = IStream_Read(stream, head, sizeof(head), &got)))
		return result;
	*out = wic_codec_matches(self->codec, head, got);
	return S_OK;
}

static HRESULT WINAPI decoder_CreateInstance(info *self, IWICBitmapDecoder **out)
{
	if (!out)
		return E_INVALIDARG;
	return wic_decoder_create(self->codec, out);
}

static HRESULT WINAPI encoder_CreateInstance(info *self, IWICBitmapEncoder **out)
{
	if (!out)
		return E_INVALIDARG;
	return wic_encoder_create(self->codec, out);
}

static const void *const decoder_table[] = {
	info_QueryInterface, info_AddRef, info_Release, info_GetComponentType, info_GetCLSID, info_GetSigningStatus, info_GetAuthor, info_GetVendorGUID,
	info_GetVersion, info_GetVersion, info_GetFriendlyName, codec_GetContainerFormat, codec_GetPixelFormats, info_GetVersion, codec_GetDeviceManufacturer,
	codec_GetDeviceManufacturer, codec_GetMimeTypes, codec_GetFileExtensions, codec_DoesSupportAnimation, codec_DoesSupportChromakey, codec_DoesSupportLossless,
	codec_DoesSupportMultiframe, codec_MatchesMimeType, decoder_GetPatterns, decoder_MatchesPattern, decoder_CreateInstance,
};

static const void *const encoder_table[] = {
	info_QueryInterface, info_AddRef, info_Release, info_GetComponentType, info_GetCLSID, info_GetSigningStatus, info_GetAuthor, info_GetVendorGUID,
	info_GetVersion, info_GetVersion, info_GetFriendlyName, codec_GetContainerFormat, codec_GetPixelFormats, info_GetVersion, codec_GetDeviceManufacturer,
	codec_GetDeviceManufacturer, codec_GetMimeTypes, codec_GetFileExtensions, codec_DoesSupportAnimation, codec_DoesSupportChromakey, codec_DoesSupportLossless,
	codec_DoesSupportMultiframe, codec_MatchesMimeType, encoder_CreateInstance,
};

HRESULT wic_codec_info_create(const wic_codec *codec, BOOL encoder, void *out)
{
	/* A codec has one information object, which lives as long as the library. */
	static info *known[8][2];
	info *self;
	UINT index = 0;
	while (wic_codec_at(index) != codec)
		index++;
	if (known[index][encoder]) {
		InterlockedIncrement(&known[index][encoder]->references);
		*(void **)out = known[index][encoder];
		return S_OK;
	}
	if (!(self = wic_alloc(sizeof(*self))))
		return E_OUTOFMEMORY;
	self->table = encoder ? encoder_table : decoder_table;
	self->references = 1;
	self->type = encoder ? WICEncoder : WICDecoder;
	self->codec = codec;
	self->name = encoder ? codec->encoder_name : codec->decoder_name;
	self->clsid = encoder ? codec->encoder_class : codec->decoder_class;
	self->references = 2;
	known[index][encoder] = self;
	*(void **)out = self;
	return S_OK;
}

static HRESULT WINAPI format_GetFormatGUID(info *self, GUID *format)
{
	if (!format)
		return E_INVALIDARG;
	*format = self->format;
	return S_OK;
}

static HRESULT WINAPI format_GetColorContext(info *self, IWICColorContext **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI format_GetBitsPerPixel(info *self, UINT *bits)
{
	if (!bits)
		return E_INVALIDARG;
	*bits = wic_format_bits(&self->format);
	return S_OK;
}

static BOOL format_has_alpha(const GUID *format)
{
	static const GUID *const alpha[] = {&GUID_WICPixelFormat8bppAlpha, &GUID_WICPixelFormat16bppBGRA5551, &GUID_WICPixelFormat32bppBGRA, &GUID_WICPixelFormat32bppRGBA,
		&GUID_WICPixelFormat32bppPBGRA, &GUID_WICPixelFormat32bppPRGBA, &GUID_WICPixelFormat64bppRGBA, &GUID_WICPixelFormat64bppBGRA, &GUID_WICPixelFormat64bppPRGBA,
		&GUID_WICPixelFormat64bppPBGRA, &GUID_WICPixelFormat128bppRGBAFloat, &GUID_WICPixelFormat128bppPRGBAFloat};
	UINT index;
	for (index = 0; index < ARRAYSIZE(alpha); index++)
		if (IsEqualGUID(format, alpha[index]))
			return TRUE;
	return FALSE;
}

static UINT format_channels(const GUID *format)
{
	const UINT bits = wic_format_bits(format);
	if (format_has_alpha(format))
		return IsEqualGUID(format, &GUID_WICPixelFormat8bppAlpha) ? 1 : 4;
	if (IsEqualGUID(format, &GUID_WICPixelFormat32bppCMYK) || IsEqualGUID(format, &GUID_WICPixelFormat64bppCMYK))
		return 4;
	if (IsEqualGUID(format, &GUID_WICPixelFormat16bppGray) || IsEqualGUID(format, &GUID_WICPixelFormat16bppGrayFixedPoint) || IsEqualGUID(format, &GUID_WICPixelFormat16bppGrayHalf)
		|| IsEqualGUID(format, &GUID_WICPixelFormat32bppGrayFloat) || IsEqualGUID(format, &GUID_WICPixelFormat32bppGrayFixedPoint))
		return 1;
	return bits <= 8 ? 1 : 3;
}

static HRESULT WINAPI format_GetChannelCount(info *self, UINT *count)
{
	if (!count)
		return E_INVALIDARG;
	*count = format_channels(&self->format);
	return S_OK;
}

/* A pixel format has no version of its own. */
static HRESULT WINAPI format_GetVersion(info *self, UINT room, WCHAR *out, UINT *actual)
{
	if (!actual)
		return E_INVALIDARG;
	if (room && out)
		out[0] = 0;
	*actual = 0;
	return S_OK;
}

static HRESULT WINAPI format_GetChannelMask(info *self, UINT channel, UINT room, BYTE *mask, UINT *actual)
{
	const UINT bits = wic_format_bits(&self->format), channels = format_channels(&self->format), each = bits / channels, bytes = (bits + 7) / 8;
	UINT bit;
	if (!actual || channel >= channels)
		return E_INVALIDARG;
	*actual = bytes;
	if (!room)
		return S_OK;
	if (!mask)
		return E_INVALIDARG;
	if (room < bytes)
		return E_INVALIDARG;
	memset(mask, 0, bytes);
	for (bit = each * channel; bit < each * (channel + 1); bit++)
		mask[bit / 8] |= 1 << (bit % 8);
	return S_OK;
}

static HRESULT WINAPI format_SupportsTransparency(info *self, BOOL *out)
{
	return answer(out, format_has_alpha(&self->format));
}

static HRESULT WINAPI format_GetNumericRepresentation(info *self, WICPixelFormatNumericRepresentation *out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(&self->format, &GUID_WICPixelFormat1bppIndexed) || IsEqualGUID(&self->format, &GUID_WICPixelFormat2bppIndexed)
		|| IsEqualGUID(&self->format, &GUID_WICPixelFormat4bppIndexed) || IsEqualGUID(&self->format, &GUID_WICPixelFormat8bppIndexed))
		*out = WICPixelFormatNumericRepresentationIndexed;
	else if (wic_format_bits(&self->format) >= 96 || IsEqualGUID(&self->format, &GUID_WICPixelFormat32bppGrayFloat))
		*out = WICPixelFormatNumericRepresentationFloat;
	else
		*out = WICPixelFormatNumericRepresentationUnsignedInteger;
	return S_OK;
}

static const void *const format_table[] = {
	info_QueryInterface, info_AddRef, info_Release, info_GetComponentType, info_GetCLSID, info_GetSigningStatus, info_GetAuthor, info_GetVendorGUID,
	format_GetVersion, format_GetVersion, info_GetFriendlyName, format_GetFormatGUID, format_GetColorContext, format_GetBitsPerPixel, format_GetChannelCount,
	format_GetChannelMask, format_SupportsTransparency, format_GetNumericRepresentation,
};

static HRESULT WINAPI converter_GetPixelFormats(info *self, UINT room, GUID *formats, UINT *actual)
{
	UINT count = 0;
	const GUID *format;
	if (!actual)
		return E_INVALIDARG;
	while ((format = wic_format_at(count))) {
		if (room && formats && count < room)
			formats[count] = *format;
		count++;
	}
	*actual = count;
	return room && room < count ? WINCODEC_ERR_INSUFFICIENTBUFFER : S_OK;
}

static HRESULT WINAPI converter_CreateInstance(info *self, IWICFormatConverter **out)
{
	if (!out)
		return E_INVALIDARG;
	return wic_converter_create(out);
}

static const void *const converter_table[] = {
	info_QueryInterface, info_AddRef, info_Release, info_GetComponentType, info_GetCLSID, info_GetSigningStatus, info_GetAuthor, info_GetVendorGUID,
	info_GetVersion, info_GetVersion, info_GetFriendlyName, converter_GetPixelFormats, converter_CreateInstance,
};

HRESULT wic_component_info_create(REFCLSID clsid, IWICComponentInfo **out)
{
	BOOL encoder = FALSE;
	const wic_codec *codec = wic_codec_by_class(clsid, &encoder);
	info *self;
	if (codec)
		return wic_codec_info_create(codec, encoder, out);
	{
		const GUID *handled;
		BOOL writer;
		if (wic_metadata_name(clsid, &handled, &writer))
			return wic_handler_info_create(clsid, out);
	}
	if (!IsEqualGUID(clsid, &CLSID_WICDefaultFormatConverter) && !wic_format_bits(clsid))
		return WINCODEC_ERR_COMPONENTNOTFOUND;
	if (!(self = wic_alloc(sizeof(*self))))
		return E_OUTOFMEMORY;
	self->references = 1;
	if (IsEqualGUID(clsid, &CLSID_WICDefaultFormatConverter)) {
		self->table = converter_table;
		self->type = WICPixelFormatConverter;
		self->name = L"Default Format Converter";
		self->clsid = &CLSID_WICDefaultFormatConverter;
	}
	else {
		self->table = format_table;
		self->type = WICPixelFormat;
		self->format = *clsid;
		self->name = L"Pixel Format";
		self->clsid = &self->format;
	}
	*out = (IWICComponentInfo *)self;
	return S_OK;
}
