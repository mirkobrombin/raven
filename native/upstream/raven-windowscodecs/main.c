/* SPDX-License-Identifier: MIT */
#include "wic.h"
#include <wchar.h>

static LONG locks;
static HINSTANCE this_module;

/* Metadata helpers nothing has needed end here: they stop the caller rather than answering with a guess. */
void wic_unavailable(void)
{
	RaiseException(0xC0000002, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

static HRESULT create_converter(REFIID riid, void **out)
{
	IWICFormatConverter *made;
	HRESULT result = wic_converter_create(&made);
	if (SUCCEEDED(result)) {
		result = IWICFormatConverter_QueryInterface(made, riid, out);
		IWICFormatConverter_Release(made);
	}
	return result;
}

typedef struct
{
	IClassFactory iface;
	const CLSID *clsid;
} class_factory;

static HRESULT WINAPI class_QueryInterface(IClassFactory *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IClassFactory)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI class_AddRef(IClassFactory *self)
{
	return 2;
}

static ULONG WINAPI class_Release(IClassFactory *self)
{
	return 1;
}

static HRESULT WINAPI class_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
	const CLSID *clsid = WIC_CONTAINING(iface, class_factory, iface)->clsid;
	BOOL encoder = FALSE;
	const wic_codec *codec;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	if (IsEqualGUID(clsid, &CLSID_WICImagingFactory))
		return wic_factory_create(riid, out);
	if (IsEqualGUID(clsid, &CLSID_WICDefaultFormatConverter))
		return create_converter(riid, out);
	if ((codec = wic_codec_by_class(clsid, &encoder))) {
		IUnknown *made;
		HRESULT result = encoder ? wic_encoder_create(codec, (IWICBitmapEncoder **)&made) : wic_decoder_create(codec, (IWICBitmapDecoder **)&made);
		if (SUCCEEDED(result)) {
			result = IUnknown_QueryInterface(made, riid, out);
			IUnknown_Release(made);
		}
		return result;
	}
	{
		HRESULT result;
		if (wic_metadata_class(clsid, riid, out, &result))
			return result;
	}
	return CLASS_E_CLASSNOTAVAILABLE;
}

static HRESULT WINAPI class_LockServer(IClassFactory *self, BOOL lock)
{
	if (lock)
		InterlockedIncrement(&locks);
	else
		InterlockedDecrement(&locks);
	return S_OK;
}

static const IClassFactoryVtbl class_vtbl = {
	class_QueryInterface, class_AddRef, class_Release, class_CreateInstance, class_LockServer,
};

static class_factory factories[] = {
	{{&class_vtbl}, &CLSID_WICImagingFactory},
	{{&class_vtbl}, &CLSID_WICDefaultFormatConverter},
	{{&class_vtbl}, &CLSID_WICBmpDecoder},
	{{&class_vtbl}, &CLSID_WICBmpEncoder},
	{{&class_vtbl}, &CLSID_WICPngDecoder},
	{{&class_vtbl}, &CLSID_WICPngEncoder},
	{{&class_vtbl}, &CLSID_WICIcoDecoder},
	{{&class_vtbl}, &CLSID_WICGifDecoder},
	{{&class_vtbl}, &CLSID_WICJpegDecoder},
	{{&class_vtbl}, &CLSID_WICJpegEncoder},
	{{&class_vtbl}, &CLSID_WICTiffDecoder},
	{{&class_vtbl}, &CLSID_WICTiffEncoder},
	{{&class_vtbl}, &CLSID_WICGifEncoder},
	{{&class_vtbl}, &CLSID_WICUnknownMetadataReader},
	{{&class_vtbl}, &CLSID_WICUnknownMetadataWriter},
	{{&class_vtbl}, &CLSID_WICPngTextMetadataReader},
	{{&class_vtbl}, &CLSID_WICPngTextMetadataWriter},
	{{&class_vtbl}, &CLSID_WICPngGamaMetadataReader},
	{{&class_vtbl}, &CLSID_WICPngGamaMetadataWriter},
	{{&class_vtbl}, &CLSID_WICPngChrmMetadataReader},
	{{&class_vtbl}, &CLSID_WICPngChrmMetadataWriter},
	{{&class_vtbl}, &CLSID_WICPngHistMetadataReader},
	{{&class_vtbl}, &CLSID_WICPngHistMetadataWriter},
	{{&class_vtbl}, &CLSID_WICPngTimeMetadataReader},
	{{&class_vtbl}, &CLSID_WICPngTimeMetadataWriter},
	{{&class_vtbl}, &CLSID_WICPngBkgdMetadataReader},
	{{&class_vtbl}, &CLSID_WICPngBkgdMetadataWriter},
	{{&class_vtbl}, &CLSID_WICIfdMetadataReader},
	{{&class_vtbl}, &CLSID_WICIfdMetadataWriter},
	{{&class_vtbl}, &CLSID_WICExifMetadataReader},
	{{&class_vtbl}, &CLSID_WICExifMetadataWriter},
	{{&class_vtbl}, &CLSID_WICGpsMetadataReader},
	{{&class_vtbl}, &CLSID_WICGpsMetadataWriter},
	{{&class_vtbl}, &CLSID_WICLSDMetadataReader},
	{{&class_vtbl}, &CLSID_WICLSDMetadataWriter},
	{{&class_vtbl}, &CLSID_WICIMDMetadataReader},
	{{&class_vtbl}, &CLSID_WICIMDMetadataWriter},
	{{&class_vtbl}, &CLSID_WICGCEMetadataReader},
	{{&class_vtbl}, &CLSID_WICGCEMetadataWriter},
	{{&class_vtbl}, &CLSID_WICAPEMetadataReader},
	{{&class_vtbl}, &CLSID_WICAPEMetadataWriter},
	{{&class_vtbl}, &CLSID_WICGifCommentMetadataReader},
	{{&class_vtbl}, &CLSID_WICGifCommentMetadataWriter},
	{{&class_vtbl}, &CLSID_WICApp0MetadataReader},
	{{&class_vtbl}, &CLSID_WICApp0MetadataWriter},
	{{&class_vtbl}, &CLSID_WICApp1MetadataReader},
	{{&class_vtbl}, &CLSID_WICApp1MetadataWriter},
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(factories); index++)
		if (IsEqualGUID(clsid, factories[index].clsid))
			return class_QueryInterface(&factories[index].iface, riid, out);
	if (out)
		*out = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return locks ? S_FALSE : S_OK;
}

static LONG set_text(HKEY key, const WCHAR *name, const WCHAR *value)
{
	return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)(wcslen(value) + 1) * sizeof(WCHAR));
}

HRESULT WINAPI DllRegisterServer(void)
{
	WCHAR module[MAX_PATH], guid[40], path[64];
	UINT index;
	HKEY key, server;
	GetModuleFileNameW(this_module, module, ARRAYSIZE(module));
	for (index = 0; index < ARRAYSIZE(factories); index++) {
		StringFromGUID2(factories[index].clsid, guid, ARRAYSIZE(guid));
		lstrcpyW(path, L"CLSID\\");
		lstrcatW(path, guid);
		if (RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
			return E_FAIL;
		if (!RegCreateKeyExW(key, L"InProcServer32", 0, NULL, 0, KEY_WRITE, NULL, &server, NULL)) {
			set_text(server, NULL, module);
			set_text(server, L"ThreadingModel", L"Both");
			RegCloseKey(server);
		}
		RegCloseKey(key);
	}
	return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	WCHAR guid[40], path[96];
	UINT index;
	for (index = 0; index < ARRAYSIZE(factories); index++) {
		StringFromGUID2(factories[index].clsid, guid, ARRAYSIZE(guid));
		lstrcpyW(path, L"CLSID\\");
		lstrcatW(path, guid);
		lstrcatW(path, L"\\InProcServer32");
		RegDeleteKeyW(HKEY_CLASSES_ROOT, path);
		path[6 + 38] = 0;
		RegDeleteKeyW(HKEY_CLASSES_ROOT, path);
	}
	return S_OK;
}

HRESULT WINAPI WICCreateImagingFactory_Proxy(UINT version, IWICImagingFactory **out)
{
	if (!out)
		return E_INVALIDARG;
	return wic_factory_create(&IID_IWICImagingFactory, (void **)out);
}

HRESULT WINAPI WICCreateColorContext_Proxy(IWICImagingFactory *factory, IWICColorContext **out)
{
	return IWICImagingFactory_CreateColorContext(factory, out);
}

/* The frame takes the source's format if it can, and otherwise answers the one it will write, which the source is then converted to. */
HRESULT WINAPI WICSetEncoderFormat_Proxy(IWICBitmapSource *source, IWICPalette *palette, IWICBitmapFrameEncode *frame, IWICBitmapSource **out)
{
	GUID wanted, taken;
	HRESULT result;
	if (!source || !frame || !out)
		return E_INVALIDARG;
	if (FAILED(result = IWICBitmapSource_GetPixelFormat(source, &wanted)))
		return result;
	taken = wanted;
	if (FAILED(result = IWICBitmapFrameEncode_SetPixelFormat(frame, &taken)))
		return result;
	if (palette)
		IWICBitmapFrameEncode_SetPalette(frame, palette);
	return WICConvertBitmapSource(&taken, source, out);
}

HRESULT WINAPI WICCreateBitmapFromSectionEx(UINT width, UINT height, REFWICPixelFormatGUID format, HANDLE section, UINT stride, UINT offset,
	WICSectionAccessLevel access, IWICBitmap **out)
{
	const SIZE_T size = (SIZE_T)stride * height;
	const BYTE *view;
	HRESULT result;
	if (!section || !out)
		return E_INVALIDARG;
	/* The bitmap is a copy of what the section holds at this moment. */
	if (!(view = MapViewOfFile(section, FILE_MAP_READ, 0, offset, size)))
		return HRESULT_FROM_WIN32(GetLastError());
	result = wic_bitmap_from_rows(width, height, format, stride, (UINT)size, view, out);
	UnmapViewOfFile(view);
	return result;
}

HRESULT WINAPI WICCreateBitmapFromSection(UINT width, UINT height, REFWICPixelFormatGUID format, HANDLE section, UINT stride, UINT offset, IWICBitmap **out)
{
	return WICCreateBitmapFromSectionEx(width, height, format, section, stride, offset, WICSectionAccessLevelRead, out);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		this_module = instance;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}
