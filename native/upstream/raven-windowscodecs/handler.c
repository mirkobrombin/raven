/* SPDX-License-Identifier: MIT */
#include "wic.h"

/* What the registry says about a metadata handler. Each class has one object, kept for the life of the library. */
typedef struct
{
	IWICMetadataReaderInfo iface;
	const CLSID *clsid;
	const GUID *format;
	const WCHAR *name;
	BOOL writer;
} handler;

static handler *handler_of(IWICMetadataReaderInfo *iface)
{
	return WIC_CONTAINING(iface, handler, iface);
}

/* The blocks each container carries. */
static UINT containers_of(const GUID *format, const GUID **out)
{
	static const GUID *const gif = &GUID_ContainerFormatGif, *const png = &GUID_ContainerFormatPng, *const jpeg = &GUID_ContainerFormatJpeg, *const tiff = &GUID_ContainerFormatTiff;
	if (IsEqualGUID(format, &GUID_MetadataFormatLSD) || IsEqualGUID(format, &GUID_MetadataFormatIMD) || IsEqualGUID(format, &GUID_MetadataFormatGCE)
		|| IsEqualGUID(format, &GUID_MetadataFormatAPE) || IsEqualGUID(format, &GUID_MetadataFormatGifComment)) {
		out[0] = gif;
		return 1;
	}
	if (IsEqualGUID(format, &GUID_MetadataFormatChunktEXt) || IsEqualGUID(format, &GUID_MetadataFormatChunkgAMA) || IsEqualGUID(format, &GUID_MetadataFormatChunkcHRM)
		|| IsEqualGUID(format, &GUID_MetadataFormatChunkhIST) || IsEqualGUID(format, &GUID_MetadataFormatChunktIME) || IsEqualGUID(format, &GUID_MetadataFormatChunkbKGD)) {
		out[0] = png;
		return 1;
	}
	if (IsEqualGUID(format, &GUID_MetadataFormatApp0) || IsEqualGUID(format, &GUID_MetadataFormatApp1)) {
		out[0] = jpeg;
		return 1;
	}
	if (IsEqualGUID(format, &GUID_MetadataFormatIfd)) {
		out[0] = tiff;
		out[1] = &GUID_MetadataFormatApp1;
		return 2;
	}
	if (IsEqualGUID(format, &GUID_MetadataFormatExif) || IsEqualGUID(format, &GUID_MetadataFormatGps)) {
		out[0] = &GUID_MetadataFormatIfd;
		return 1;
	}
	return 0;
}

static HRESULT WINAPI handler_QueryInterface(IWICMetadataReaderInfo *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICComponentInfo) || IsEqualGUID(riid, &IID_IWICMetadataHandlerInfo)
		|| IsEqualGUID(riid, handler_of(iface)->writer ? &IID_IWICMetadataWriterInfo : &IID_IWICMetadataReaderInfo)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI handler_AddRef(IWICMetadataReaderInfo *iface)
{
	return 2;
}

static ULONG WINAPI handler_Release(IWICMetadataReaderInfo *iface)
{
	return 1;
}

static HRESULT WINAPI handler_GetComponentType(IWICMetadataReaderInfo *iface, WICComponentType *type)
{
	if (!type)
		return E_INVALIDARG;
	*type = handler_of(iface)->writer ? WICMetadataWriter : WICMetadataReader;
	return S_OK;
}

static HRESULT WINAPI handler_GetCLSID(IWICMetadataReaderInfo *iface, CLSID *clsid)
{
	if (!clsid)
		return E_INVALIDARG;
	*clsid = *handler_of(iface)->clsid;
	return S_OK;
}

static HRESULT WINAPI handler_GetSigningStatus(IWICMetadataReaderInfo *iface, DWORD *status)
{
	if (!status)
		return E_INVALIDARG;
	*status = WICComponentSigned;
	return S_OK;
}

static HRESULT copy_text(const WCHAR *text, UINT room, WCHAR *out, UINT *actual)
{
	const UINT length = lstrlenW(text) + 1;
	if (!actual || (room && !out))
		return E_INVALIDARG;
	*actual = length;
	if (!room)
		return S_OK;
	if (room < length)
		return WINCODEC_ERR_INSUFFICIENTBUFFER;
	memcpy(out, text, length * sizeof(WCHAR));
	return S_OK;
}

static HRESULT WINAPI handler_GetAuthor(IWICMetadataReaderInfo *iface, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(L"", room, out, actual);
}

static HRESULT WINAPI handler_GetVendorGUID(IWICMetadataReaderInfo *iface, GUID *vendor)
{
	if (!vendor)
		return E_INVALIDARG;
	*vendor = GUID_VendorMicrosoft;
	return S_OK;
}

static HRESULT WINAPI handler_GetVersion(IWICMetadataReaderInfo *iface, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(L"1.0.0.0", room, out, actual);
}

static HRESULT WINAPI handler_GetSpecVersion(IWICMetadataReaderInfo *iface, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(L"1.0.0.0", room, out, actual);
}

static HRESULT WINAPI handler_GetFriendlyName(IWICMetadataReaderInfo *iface, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(handler_of(iface)->name, room, out, actual);
}

static HRESULT WINAPI handler_GetMetadataFormat(IWICMetadataReaderInfo *iface, GUID *format)
{
	if (!format)
		return E_INVALIDARG;
	*format = *handler_of(iface)->format;
	return S_OK;
}

static HRESULT WINAPI handler_GetContainerFormats(IWICMetadataReaderInfo *iface, UINT room, GUID *out, UINT *actual)
{
	const GUID *list[4];
	const UINT count = containers_of(handler_of(iface)->format, list);
	UINT index;
	if (!actual)
		return E_INVALIDARG;
	if (!out) {
		*actual = count;
		return S_OK;
	}
	for (index = 0; index < count && index < room; index++)
		out[index] = *list[index];
	*actual = index;
	return S_OK;
}

static HRESULT WINAPI handler_GetDeviceManufacturer(IWICMetadataReaderInfo *iface, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(L"", room, out, actual);
}

static HRESULT WINAPI handler_GetDeviceModels(IWICMetadataReaderInfo *iface, UINT room, WCHAR *out, UINT *actual)
{
	return copy_text(L"", room, out, actual);
}

static HRESULT WINAPI handler_DoesRequireFullStream(IWICMetadataReaderInfo *iface, BOOL *answer)
{
	if (!answer)
		return E_INVALIDARG;
	*answer = TRUE;
	return S_OK;
}

static HRESULT WINAPI handler_DoesSupportPadding(IWICMetadataReaderInfo *iface, BOOL *answer)
{
	if (!answer)
		return E_INVALIDARG;
	*answer = FALSE;
	return S_OK;
}

static HRESULT WINAPI handler_DoesRequireFixedSize(IWICMetadataReaderInfo *iface, BOOL *answer)
{
	if (!answer)
		return E_INVALIDARG;
	*answer = FALSE;
	return S_OK;
}

/* Readers are chosen by format here, never by byte patterns, so no container lists any. */
static HRESULT WINAPI handler_GetPatterns(IWICMetadataReaderInfo *iface, REFGUID container, UINT room, WICMetadataPattern *out, UINT *count, UINT *actual)
{
	const GUID *list[4];
	const UINT total = containers_of(handler_of(iface)->format, list);
	UINT index;
	if (!container || !count || !actual)
		return E_INVALIDARG;
	for (index = 0; index < total; index++)
		if (IsEqualGUID(container, list[index])) {
			*count = 0;
			*actual = 0;
			return S_OK;
		}
	return WINCODEC_ERR_COMPONENTNOTFOUND;
}

static HRESULT WINAPI handler_MatchesPattern(IWICMetadataReaderInfo *iface, REFGUID container, IStream *stream, BOOL *matches)
{
	if (!matches)
		return E_INVALIDARG;
	*matches = FALSE;
	return S_OK;
}

static HRESULT WINAPI handler_CreateInstance(IWICMetadataReaderInfo *iface, IWICMetadataReader **out)
{
	if (!out)
		return E_INVALIDARG;
	return CoCreateInstance(handler_of(iface)->clsid, NULL, CLSCTX_INPROC_SERVER, handler_of(iface)->writer ? &IID_IWICMetadataWriter : &IID_IWICMetadataReader, (void **)out);
}

static const IWICMetadataReaderInfoVtbl handler_vtbl = {
	handler_QueryInterface, handler_AddRef, handler_Release, handler_GetComponentType, handler_GetCLSID, handler_GetSigningStatus, handler_GetAuthor,
	handler_GetVendorGUID, handler_GetVersion, handler_GetSpecVersion, handler_GetFriendlyName, handler_GetMetadataFormat, handler_GetContainerFormats,
	handler_GetDeviceManufacturer, handler_GetDeviceModels, handler_DoesRequireFullStream, handler_DoesSupportPadding, handler_DoesRequireFixedSize,
	handler_GetPatterns, handler_MatchesPattern, handler_CreateInstance,
};

/* A writer's table has two calls after the shared ones: its header, and making one. */
static HRESULT WINAPI handler_GetHeader(IWICMetadataReaderInfo *iface, REFGUID container, UINT room, WICMetadataHeader *out, UINT *actual)
{
	if (!container || !actual)
		return E_INVALIDARG;
	return WINCODEC_ERR_COMPONENTNOTFOUND;
}

HRESULT wic_handler_info_create(REFCLSID clsid, IWICComponentInfo **out)
{
	static handler made[64];
	static const IWICMetadataReaderInfoVtbl writer_vtbl = {
		handler_QueryInterface, handler_AddRef, handler_Release, handler_GetComponentType, handler_GetCLSID, handler_GetSigningStatus, handler_GetAuthor,
		handler_GetVendorGUID, handler_GetVersion, handler_GetSpecVersion, handler_GetFriendlyName, handler_GetMetadataFormat, handler_GetContainerFormats,
		handler_GetDeviceManufacturer, handler_GetDeviceModels, handler_DoesRequireFullStream, handler_DoesSupportPadding, handler_DoesRequireFixedSize,
		(void *)handler_GetHeader, (void *)handler_CreateInstance,
	};
	const GUID *format;
	BOOL writer;
	const WCHAR *name = wic_metadata_name(clsid, &format, &writer);
	const CLSID *known;
	UINT index;
	if (!name)
		return WINCODEC_ERR_COMPONENTNOTFOUND;
	for (index = 0; (known = wic_metadata_class_at(index)) && index < ARRAYSIZE(made); index++)
		if (IsEqualGUID(known, clsid)) {
			handler *self = &made[index];
			self->clsid = known;
			self->format = format;
			self->name = name;
			self->writer = writer;
			self->iface.lpVtbl = writer ? &writer_vtbl : &handler_vtbl;
			*out = (IWICComponentInfo *)&self->iface;
			return S_OK;
		}
	return WINCODEC_ERR_COMPONENTNOTFOUND;
}
