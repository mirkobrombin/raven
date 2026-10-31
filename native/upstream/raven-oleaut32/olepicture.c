/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * The standard picture object: a bitmap, icon or metafile with a size in HIMETRIC,
 * and its persistent form.
 */
#include "connpt.h"
#include <wincodec.h>

#define HIMETRIC_PER_INCH 2540

/* On stream a picture is a magic and a byte count, then the file format of its contents. */
#define STREAM_MAGIC 0x0000746c
#define ICON_MAGIC 0x00010000

struct picture
{
	IPicture IPicture_iface;
	IDispatch IDispatch_iface;
	IPersistStream IPersistStream_iface;
	IConnectionPointContainer IConnectionPointContainer_iface;
	LONG ref;
	PICTDESC desc;
	BOOL own;
	OLE_XSIZE_HIMETRIC width;
	OLE_YSIZE_HIMETRIC height;
	OLE_XPOS_PIXELS pixel_width;
	OLE_YPOS_PIXELS pixel_height;
	HDC dc;
	HDC out_dc;
	BOOL keep_format;
	DWORD attributes;
	BOOL has_alpha;
	/* The bytes it was loaded from, kept so a save returns them unchanged. */
	BYTE *data;
	ULONG data_size;
	BOOL loaded_text;
	HBITMAP mask;
	IConnectionPoint *notify;
};

static const IPictureVtbl picture_vtbl;
static const IDispatchVtbl picture_dispatch_vtbl;
static const IPersistStreamVtbl picture_stream_vtbl;
static const IConnectionPointContainerVtbl picture_container_vtbl;

static inline struct picture *picture_of(IPicture *iface)
{
	return CONTAINING_RECORD(iface, struct picture, IPicture_iface);
}

static inline struct picture *picture_of_dispatch(IDispatch *iface)
{
	return CONTAINING_RECORD(iface, struct picture, IDispatch_iface);
}

static inline struct picture *picture_of_stream(IPersistStream *iface)
{
	return CONTAINING_RECORD(iface, struct picture, IPersistStream_iface);
}

static inline struct picture *picture_of_container(IConnectionPointContainer *iface)
{
	return CONTAINING_RECORD(iface, struct picture, IConnectionPointContainer_iface);
}

static LONG screen_dpi(int which)
{
	HDC dc = GetDC(NULL);
	LONG dpi = dc ? GetDeviceCaps(dc, which) : 96;
	if (dc)
		ReleaseDC(NULL, dc);
	return dpi ? dpi : 96;
}

static OLE_XSIZE_HIMETRIC pixels_to_himetric(LONG pixels, int which)
{
	return MulDiv(pixels, HIMETRIC_PER_INCH, screen_dpi(which));
}

static void measure_bitmap(struct picture *picture)
{
	BITMAP info;
	SIZE size;
	if (!GetObjectW(picture->desc.bmp.hbitmap, sizeof(info), &info))
		return;
	picture->pixel_width = info.bmWidth;
	picture->pixel_height = info.bmHeight;
	/* A dimension set on the bitmap wins over the one derived from its pixels. */
	if (GetBitmapDimensionEx(picture->desc.bmp.hbitmap, &size) && size.cx && size.cy)
	{
		picture->width = size.cx * 10;
		picture->height = size.cy * 10;
	}
	else
	{
		picture->width = pixels_to_himetric(info.bmWidth, LOGPIXELSX);
		picture->height = pixels_to_himetric(info.bmHeight, LOGPIXELSY);
	}
}

static void measure_icon(struct picture *picture)
{
	ICONINFO info;
	BITMAP bitmap;
	if (!GetIconInfo(picture->desc.icon.hicon, &info))
		return;
	if (GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap))
	{
		picture->pixel_width = bitmap.bmWidth;
		picture->pixel_height = info.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
		picture->width = pixels_to_himetric(picture->pixel_width, LOGPIXELSX);
		picture->height = pixels_to_himetric(picture->pixel_height, LOGPIXELSY);
	}
	if (info.hbmColor)
		DeleteObject(info.hbmColor);
	if (info.hbmMask)
		DeleteObject(info.hbmMask);
}

static void measure_enhanced(struct picture *picture)
{
	ENHMETAHEADER header;
	if (GetEnhMetaFileHeader(picture->desc.emf.hemf, sizeof(header), &header))
	{
		picture->width = header.rclFrame.right - header.rclFrame.left;
		picture->height = header.rclFrame.bottom - header.rclFrame.top;
	}
}

static void free_contents(struct picture *picture)
{
	if (picture->own)
		switch (picture->desc.picType)
		{
		case PICTYPE_BITMAP:
			DeleteObject(picture->desc.bmp.hbitmap);
			if (picture->mask)
				DeleteObject(picture->mask);
			break;
		case PICTYPE_METAFILE:
			DeleteMetaFile(picture->desc.wmf.hmeta);
			break;
		case PICTYPE_ICON:
			DestroyIcon(picture->desc.icon.hicon);
			break;
		case PICTYPE_ENHMETAFILE:
			DeleteEnhMetaFile(picture->desc.emf.hemf);
			break;
		default:
			break;
		}
	picture->mask = NULL;
	HeapFree(GetProcessHeap(), 0, picture->data);
	picture->data = NULL;
	picture->data_size = 0;
}

static HRESULT WINAPI picture_QueryInterface(IPicture *iface, REFIID riid, void **out)
{
	struct picture *picture = picture_of(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPicture))
		*out = &picture->IPicture_iface;
	else if (IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IPictureDisp))
		*out = &picture->IDispatch_iface;
	else if (IsEqualIID(riid, &IID_IPersist) || IsEqualIID(riid, &IID_IPersistStream))
		*out = &picture->IPersistStream_iface;
	else if (IsEqualIID(riid, &IID_IConnectionPointContainer))
		*out = &picture->IConnectionPointContainer_iface;
	if (!*out)
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI picture_AddRef(IPicture *iface)
{
	return InterlockedIncrement(&picture_of(iface)->ref);
}

static ULONG WINAPI picture_Release(IPicture *iface)
{
	struct picture *picture = picture_of(iface);
	ULONG count = InterlockedDecrement(&picture->ref);
	if (!count)
	{
		free_contents(picture);
		oa_connection_point_destroy(picture->notify);
		HeapFree(GetProcessHeap(), 0, picture);
	}
	return count;
}

static HRESULT WINAPI picture_get_Handle(IPicture *iface, OLE_HANDLE *out)
{
	struct picture *picture = picture_of(iface);
	if (!out)
		return E_POINTER;
	switch (picture->desc.picType)
	{
	case PICTYPE_BITMAP:
		*out = HandleToUlong(picture->desc.bmp.hbitmap);
		break;
	case PICTYPE_METAFILE:
		*out = HandleToUlong(picture->desc.wmf.hmeta);
		break;
	case PICTYPE_ICON:
		*out = HandleToUlong(picture->desc.icon.hicon);
		break;
	case PICTYPE_ENHMETAFILE:
		*out = HandleToUlong(picture->desc.emf.hemf);
		break;
	default:
		*out = 0;
		break;
	}
	return S_OK;
}

static HRESULT WINAPI picture_get_hPal(IPicture *iface, OLE_HANDLE *out)
{
	struct picture *picture = picture_of(iface);
	if (!out)
		return E_POINTER;
	if (picture->desc.picType != PICTYPE_BITMAP)
	{
		*out = 0;
		return E_FAIL;
	}
	*out = HandleToUlong(picture->desc.bmp.hpal);
	return S_OK;
}

static HRESULT WINAPI picture_get_Type(IPicture *iface, SHORT *out)
{
	if (!out)
		return E_POINTER;
	*out = picture_of(iface)->desc.picType;
	return S_OK;
}

static HRESULT WINAPI picture_get_Width(IPicture *iface, OLE_XSIZE_HIMETRIC *out)
{
	if (!out)
		return E_POINTER;
	*out = picture_of(iface)->width;
	return S_OK;
}

static HRESULT WINAPI picture_get_Height(IPicture *iface, OLE_YSIZE_HIMETRIC *out)
{
	if (!out)
		return E_POINTER;
	*out = picture_of(iface)->height;
	return S_OK;
}

static void render_icon(struct picture *picture, HDC dc, LONG x, LONG y, LONG cx, LONG cy)
{
	DrawIconEx(dc, x, y, picture->desc.icon.hicon, cx, cy, 0, NULL, DI_NORMAL);
}

static void render_bitmap(struct picture *picture, HDC dc, LONG x, LONG y, LONG cx, LONG cy, LONG src_x, LONG src_y, LONG src_cx, LONG src_cy)
{
	HDC memory = CreateCompatibleDC(dc);
	HGDIOBJ old;
	if (!memory)
		return;
	old = SelectObject(memory, picture->desc.bmp.hbitmap);
	if (picture->mask)
	{
		HDC mask_dc = CreateCompatibleDC(dc);
		HGDIOBJ old_mask = SelectObject(mask_dc, picture->mask);
		SetBkColor(dc, RGB(255, 255, 255));
		SetTextColor(dc, RGB(0, 0, 0));
		StretchBlt(dc, x, y, cx, cy, mask_dc, src_x, src_y, src_cx, src_cy, SRCAND);
		StretchBlt(dc, x, y, cx, cy, memory, src_x, src_y, src_cx, src_cy, SRCPAINT);
		SelectObject(mask_dc, old_mask);
		DeleteDC(mask_dc);
	}
	else
		StretchBlt(dc, x, y, cx, cy, memory, src_x, src_y, src_cx, src_cy, SRCCOPY);
	SelectObject(memory, old);
	DeleteDC(memory);
}

static HRESULT WINAPI picture_Render(IPicture *iface, HDC dc, LONG x, LONG y, LONG cx, LONG cy, OLE_XPOS_HIMETRIC src_x, OLE_YPOS_HIMETRIC src_y, OLE_XSIZE_HIMETRIC src_cx, OLE_YSIZE_HIMETRIC src_cy, const RECT *bounds)
{
	struct picture *picture = picture_of(iface);
	if (!cx || !cy || !src_cx || !src_cy)
		return CTL_E_INVALIDPROPERTYVALUE;
	if (cx < 0 && picture->desc.picType == PICTYPE_ICON)
		return CTL_E_INVALIDPROPERTYVALUE;
	switch (picture->desc.picType)
	{
	case PICTYPE_UNINITIALIZED:
	case PICTYPE_NONE:
		return S_OK;
	case PICTYPE_BITMAP:
	{
		LONG source_x = MulDiv(src_x, picture->pixel_width, picture->width ? picture->width : 1);
		LONG source_y = MulDiv(picture->height - src_y, picture->pixel_height, picture->height ? picture->height : 1);
		LONG source_cx = MulDiv(src_cx, picture->pixel_width, picture->width ? picture->width : 1);
		LONG source_cy = MulDiv(-src_cy, picture->pixel_height, picture->height ? picture->height : 1);
		render_bitmap(picture, dc, x, y, cx, cy, source_x, source_y, source_cx, source_cy);
		return S_OK;
	}
	case PICTYPE_ICON:
		render_icon(picture, dc, x, y, cx, cy);
		return S_OK;
	case PICTYPE_METAFILE:
	{
		POINT origin, old_origin;
		SIZE extent, old_extent;
		int old_mode = SetMapMode(dc, MM_ANISOTROPIC);
		origin.x = x;
		origin.y = y;
		extent.cx = cx;
		extent.cy = cy;
		SetViewportOrgEx(dc, origin.x, origin.y, &old_origin);
		SetViewportExtEx(dc, extent.cx, extent.cy, &old_extent);
		PlayMetaFile(dc, picture->desc.wmf.hmeta);
		SetViewportOrgEx(dc, old_origin.x, old_origin.y, NULL);
		SetViewportExtEx(dc, old_extent.cx, old_extent.cy, NULL);
		SetMapMode(dc, old_mode);
		return S_OK;
	}
	case PICTYPE_ENHMETAFILE:
	{
		RECT target;
		target.left = x;
		target.top = y;
		target.right = x + cx;
		target.bottom = y + cy;
		PlayEnhMetaFile(dc, picture->desc.emf.hemf, &target);
		return S_OK;
	}
	default:
		return E_NOTIMPL;
	}
}

static HRESULT WINAPI picture_set_hPal(IPicture *iface, OLE_HANDLE palette)
{
	struct picture *picture = picture_of(iface);
	if (picture->desc.picType == PICTYPE_BITMAP)
	{
		picture->desc.bmp.hpal = ULongToHandle(palette);
		return S_OK;
	}
	return E_FAIL;
}

static HRESULT WINAPI picture_get_CurDC(IPicture *iface, HDC *out)
{
	if (!out)
		return E_POINTER;
	*out = picture_of(iface)->dc;
	return S_OK;
}

static HRESULT WINAPI picture_SelectPicture(IPicture *iface, HDC dc, HDC *previous, OLE_HANDLE *selected)
{
	struct picture *picture = picture_of(iface);
	if (picture->desc.picType != PICTYPE_BITMAP)
		return E_FAIL;
	if (previous)
		*previous = picture->dc;
	SelectObject(dc, picture->desc.bmp.hbitmap);
	picture->dc = dc;
	if (selected)
		*selected = HandleToUlong(picture->desc.bmp.hbitmap);
	return S_OK;
}

static HRESULT WINAPI picture_get_KeepOriginalFormat(IPicture *iface, BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = picture_of(iface)->keep_format;
	return S_OK;
}

static HRESULT WINAPI picture_put_KeepOriginalFormat(IPicture *iface, BOOL value)
{
	picture_of(iface)->keep_format = value;
	return S_OK;
}

static HRESULT WINAPI picture_PictureChanged(IPicture *iface)
{
	return S_OK;
}

static HRESULT serialize(struct picture *picture);

static HRESULT WINAPI picture_SaveAsFile(IPicture *iface, IStream *target, BOOL save_copy, LONG *written)
{
	struct picture *picture = picture_of(iface);
	HRESULT status;
	ULONG put = 0;
	if (!target)
		return E_POINTER;
	status = serialize(picture);
	if (FAILED(status))
		return status;
	if (!picture->data)
	{
		if (written)
			*written = 0;
		return S_OK;
	}
	status = IStream_Write(target, picture->data, picture->data_size, &put);
	if (written)
		*written = put;
	return status;
}

static HRESULT WINAPI picture_get_Attributes(IPicture *iface, DWORD *out)
{
	struct picture *picture = picture_of(iface);
	if (!out)
		return E_POINTER;
	*out = 0;
	switch (picture->desc.picType)
	{
	case PICTYPE_BITMAP:
		*out = picture->mask ? PICTURE_TRANSPARENT : 0;
		break;
	case PICTYPE_METAFILE:
	case PICTYPE_ENHMETAFILE:
		*out = PICTURE_TRANSPARENT | PICTURE_SCALABLE;
		break;
	case PICTYPE_ICON:
		*out = PICTURE_TRANSPARENT;
		break;
	default:
		break;
	}
	return S_OK;
}

static const IPictureVtbl picture_vtbl = {
	picture_QueryInterface,
	picture_AddRef,
	picture_Release,
	picture_get_Handle,
	picture_get_hPal,
	picture_get_Type,
	picture_get_Width,
	picture_get_Height,
	picture_Render,
	picture_set_hPal,
	picture_get_CurDC,
	picture_SelectPicture,
	picture_get_KeepOriginalFormat,
	picture_put_KeepOriginalFormat,
	picture_PictureChanged,
	picture_SaveAsFile,
	picture_get_Attributes,
};

/* Decoding: the simple formats directly, everything else through the imaging component. */
static SIZE_T dib_color_bytes(const BITMAPINFOHEADER *header);

/* Keeps the file's own depth: pictures hand out the section itself, not a converted copy. */
static HBITMAP dib_section_from(const BITMAPINFO *info, const BYTE *bits)
{
	void *pixels = NULL;
	HBITMAP bitmap = CreateDIBSection(NULL, info, DIB_RGB_COLORS, &pixels, NULL, 0);
	const BITMAPINFOHEADER *header = &info->bmiHeader;
	SIZE_T line, lines;
	if (!bitmap || !pixels)
		return bitmap;
	line = (((SIZE_T)header->biWidth * header->biBitCount + 31) / 32) * 4;
	lines = header->biHeight < 0 ? -(SIZE_T)header->biHeight : (SIZE_T)header->biHeight;
	memcpy(pixels, bits, line * lines);
	return bitmap;
}

static SIZE_T dib_color_bytes(const BITMAPINFOHEADER *header)
{
	DWORD colors = header->biClrUsed;
	if (header->biSize < sizeof(BITMAPINFOHEADER))
		return 0;
	if (!colors && header->biBitCount <= 8)
		colors = 1u << header->biBitCount;
	return colors * sizeof(RGBQUAD) + (header->biCompression == BI_BITFIELDS ? 12 : 0);
}

static HRESULT load_bmp(struct picture *picture, const BYTE *data, ULONG size)
{
	const BITMAPFILEHEADER *file = (const BITMAPFILEHEADER *)data;
	const BITMAPINFO *info;
	if (size < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) || file->bfOffBits > size)
		return E_FAIL;
	info = (const BITMAPINFO *)(data + sizeof(BITMAPFILEHEADER));
	picture->desc.bmp.hbitmap = dib_section_from(info, data + file->bfOffBits);
	if (!picture->desc.bmp.hbitmap)
		return E_FAIL;
	picture->desc.picType = PICTYPE_BITMAP;
	picture->own = TRUE;
	measure_bitmap(picture);
	return S_OK;
}

/* A headerless device independent bitmap, as older containers store it. */
static HRESULT load_dib(struct picture *picture, const BYTE *data, ULONG size)
{
	const BITMAPINFO *info = (const BITMAPINFO *)data;
	SIZE_T offset;
	if (size < sizeof(BITMAPINFOHEADER))
		return E_FAIL;
	offset = info->bmiHeader.biSize + dib_color_bytes(&info->bmiHeader);
	if (offset > size)
		return E_FAIL;
	picture->desc.bmp.hbitmap = dib_section_from(info, data + offset);
	if (!picture->desc.bmp.hbitmap)
		return E_FAIL;
	picture->desc.picType = PICTYPE_BITMAP;
	picture->own = TRUE;
	measure_bitmap(picture);
	return S_OK;
}

#pragma pack(push, 1)
struct icon_entry
{
	BYTE width, height, colors, reserved;
	WORD x_hotspot, y_hotspot;
	DWORD size, offset;
};

struct icon_header
{
	WORD reserved, type, count;
};

struct placeable_header
{
	DWORD key;
	WORD handle;
	SHORT left, top, right, bottom;
	WORD inch;
	DWORD reserved;
	WORD checksum;
};
#pragma pack(pop)

static HRESULT load_icon(struct picture *picture, const BYTE *data, ULONG size)
{
	const struct icon_header *header = (const struct icon_header *)data;
	const struct icon_entry *entries = (const struct icon_entry *)(header + 1);
	const struct icon_entry *best;
	HICON icon;
	UINT index;
	if (size < sizeof(*header) || !header->count || size < sizeof(*header) + header->count * sizeof(*entries))
		return E_FAIL;
	best = &entries[0];
	/* The first of the standard size, when there is one. */
	for (index = 0; index < header->count; index++)
		if (entries[index].width == 32 && entries[index].height == 32)
		{
			best = &entries[index];
			break;
		}
	if (best->offset > size || best->size > size - best->offset)
		return E_FAIL;
	icon = CreateIconFromResourceEx((BYTE *)data + best->offset, best->size, header->type == 1, 0x30000, best->width, best->height, 0);
	if (!icon)
		return E_FAIL;
	picture->desc.picType = PICTYPE_ICON;
	picture->desc.icon.hicon = icon;
	picture->own = TRUE;
	measure_icon(picture);
	return S_OK;
}

static HRESULT load_enhanced(struct picture *picture, const BYTE *data, ULONG size)
{
	HENHMETAFILE file = SetEnhMetaFileBits(size, data);
	if (!file)
		return E_FAIL;
	picture->desc.picType = PICTYPE_ENHMETAFILE;
	picture->desc.emf.hemf = file;
	picture->own = TRUE;
	measure_enhanced(picture);
	return S_OK;
}

static HRESULT load_placeable(struct picture *picture, const BYTE *data, ULONG size)
{
	const struct placeable_header *header = (const struct placeable_header *)data;
	HMETAFILE file;
	if (size < sizeof(*header) || !header->inch)
		return E_FAIL;
	file = SetMetaFileBitsEx(size - sizeof(*header), data + sizeof(*header));
	if (!file)
		return E_FAIL;
	picture->desc.picType = PICTYPE_METAFILE;
	picture->desc.wmf.hmeta = file;
	picture->own = TRUE;
	picture->width = MulDiv(header->right - header->left, HIMETRIC_PER_INCH, header->inch);
	picture->height = MulDiv(header->bottom - header->top, HIMETRIC_PER_INCH, header->inch);
	return S_OK;
}

/* Formats with a codec: decoded to a top-down 32 bit bitmap, with a mask when pixels are transparent. */
static HRESULT load_decoded(struct picture *picture, const BYTE *data, ULONG size)
{
	IWICImagingFactory *factory = NULL;
	IWICStream *stream = NULL;
	IWICBitmapDecoder *decoder = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICFormatConverter *converter = NULL;
	UINT width = 0, height = 0;
	BITMAPINFOHEADER header;
	BYTE *pixels = NULL;
	HRESULT status;
	BOOL initialized = SUCCEEDED(CoInitialize(NULL));
	HDC dc;
	HBITMAP bitmap = NULL;
	status = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
	if (SUCCEEDED(status))
		status = IWICImagingFactory_CreateStream(factory, &stream);
	if (SUCCEEDED(status))
		status = IWICStream_InitializeFromMemory(stream, (BYTE *)data, size);
	if (SUCCEEDED(status))
		status = IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder);
	if (SUCCEEDED(status))
		status = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
	if (SUCCEEDED(status))
		status = IWICImagingFactory_CreateFormatConverter(factory, &converter);
	if (SUCCEEDED(status))
		status = IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
	if (SUCCEEDED(status))
		status = IWICFormatConverter_GetSize(converter, &width, &height);
	if (SUCCEEDED(status))
	{
		pixels = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)width * height * 4);
		status = pixels ? IWICFormatConverter_CopyPixels(converter, NULL, width * 4, width * height * 4, pixels) : E_OUTOFMEMORY;
	}
	if (SUCCEEDED(status))
	{
		UINT x, y, transparent = 0;
		void *section = NULL;
		memset(&header, 0, sizeof(header));
		header.biSize = sizeof(header);
		header.biWidth = width;
		header.biHeight = height;
		header.biPlanes = 1;
		header.biCompression = BI_RGB;
		for (y = 0; y < height && !transparent; y++)
			for (x = 0; x < width; x++)
				if (pixels[(y * width + x) * 4 + 3] < 128)
				{
					transparent = 1;
					break;
				}
		/* Opaque images need no alpha channel; the section stays bottom-up like a bitmap file. */
		header.biBitCount = transparent ? 32 : 24;
		bitmap = CreateDIBSection(NULL, (BITMAPINFO *)&header, DIB_RGB_COLORS, &section, NULL, 0);
		if (bitmap && section)
		{
			UINT stride = ((width * header.biBitCount + 31) / 32) * 4;
			for (y = 0; y < height; y++)
			{
				const BYTE *from = pixels + (SIZE_T)(height - 1 - y) * width * 4;
				BYTE *to = (BYTE *)section + (SIZE_T)y * stride;
				for (x = 0; x < width; x++)
				{
					to[x * (header.biBitCount / 8)] = from[x * 4];
					to[x * (header.biBitCount / 8) + 1] = from[x * 4 + 1];
					to[x * (header.biBitCount / 8) + 2] = from[x * 4 + 2];
					if (transparent)
						to[x * 4 + 3] = from[x * 4 + 3];
				}
			}
		}
		if (transparent && bitmap)
		{
			dc = CreateCompatibleDC(NULL);
			if (dc)
			{
				HBITMAP mask = CreateBitmap(width, height, 1, 1, NULL);
				HGDIOBJ old = SelectObject(dc, mask);
				for (y = 0; y < height; y++)
					for (x = 0; x < width; x++)
						SetPixel(dc, x, y, pixels[(y * width + x) * 4 + 3] < 128 ? RGB(255, 255, 255) : RGB(0, 0, 0));
				SelectObject(dc, old);
				DeleteDC(dc);
				picture->mask = mask;
			}
		}
		if (!bitmap)
			status = E_FAIL;
	}
	if (SUCCEEDED(status))
	{
		picture->desc.picType = PICTYPE_BITMAP;
		picture->desc.bmp.hbitmap = bitmap;
		picture->own = TRUE;
		measure_bitmap(picture);
	}
	HeapFree(GetProcessHeap(), 0, pixels);
	if (converter)
		IWICFormatConverter_Release(converter);
	if (frame)
		IWICBitmapFrameDecode_Release(frame);
	if (decoder)
		IWICBitmapDecoder_Release(decoder);
	if (stream)
		IWICStream_Release(stream);
	if (factory)
		IWICImagingFactory_Release(factory);
	if (initialized)
		CoUninitialize();
	return status;
}

static HRESULT load_contents(struct picture *picture, const BYTE *data, ULONG size)
{
	HRESULT status;
	if (!size)
	{
		picture->desc.picType = PICTYPE_NONE;
		return S_OK;
	}
	if (size >= 2 && data[0] == 'B' && data[1] == 'M')
		status = load_bmp(picture, data, size);
	else if (size >= 4 && !memcmp(data, "\x00\x00\x01\x00", 4))
		status = load_icon(picture, data, size);
	else if (size >= 4 && !memcmp(data, "\x00\x00\x02\x00", 4))
		status = load_icon(picture, data, size);
	else if (size >= 4 && !memcmp(data, "\xd7\xcd\xc6\x9a", 4))
		status = load_placeable(picture, data, size);
	else if (size >= sizeof(ENHMETAHEADER) && ((const ENHMETAHEADER *)data)->dSignature == ENHMETA_SIGNATURE)
		status = load_enhanced(picture, data, size);
	else if (size >= sizeof(BITMAPINFOHEADER) && *(const DWORD *)data == sizeof(BITMAPINFOHEADER))
		status = load_dib(picture, data, size);
	else
		status = load_decoded(picture, data, size);
	return status;
}

static HRESULT WINAPI stream_QueryInterface(IPersistStream *iface, REFIID riid, void **out)
{
	return picture_QueryInterface(&picture_of_stream(iface)->IPicture_iface, riid, out);
}

static ULONG WINAPI stream_AddRef(IPersistStream *iface)
{
	return picture_AddRef(&picture_of_stream(iface)->IPicture_iface);
}

static ULONG WINAPI stream_Release(IPersistStream *iface)
{
	return picture_Release(&picture_of_stream(iface)->IPicture_iface);
}

static HRESULT WINAPI stream_GetClassID(IPersistStream *iface, CLSID *out)
{
	if (!out)
		return E_POINTER;
	*out = CLSID_StdPicture;
	return S_OK;
}

static HRESULT WINAPI stream_IsDirty(IPersistStream *iface)
{
	return E_NOTIMPL;
}

/* Reads the whole stream: containers often prefix a magic and length, but raw files have neither. */
static HRESULT read_stream(IStream *source, BYTE **data, ULONG *size)
{
	STATSTG stat;
	ULONG room = 0, have = 0, got = 0;
	BYTE *buffer = NULL;
	HRESULT status;
	if (SUCCEEDED(IStream_Stat(source, &stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart && !stat.cbSize.HighPart)
		room = stat.cbSize.LowPart;
	if (!room)
		room = 0x1000;
	buffer = HeapAlloc(GetProcessHeap(), 0, room);
	if (!buffer)
		return E_OUTOFMEMORY;
	for (;;)
	{
		BYTE *grown;
		status = IStream_Read(source, buffer + have, room - have, &got);
		if (FAILED(status))
		{
			HeapFree(GetProcessHeap(), 0, buffer);
			return status;
		}
		have += got;
		if (!got || have < room)
			break;
		room *= 2;
		grown = HeapReAlloc(GetProcessHeap(), 0, buffer, room);
		if (!grown)
		{
			HeapFree(GetProcessHeap(), 0, buffer);
			return E_OUTOFMEMORY;
		}
		buffer = grown;
	}
	*data = buffer;
	*size = have;
	return S_OK;
}

static HRESULT WINAPI stream_Load(IPersistStream *iface, IStream *source)
{
	struct picture *picture = picture_of_stream(iface);
	BYTE *data = NULL, *contents;
	ULONG size = 0, contents_size;
	HRESULT status = read_stream(source, &data, &size);
	if (FAILED(status))
		return status;
	contents = data;
	contents_size = size;
	if (size >= 8 && ((const DWORD *)data)[0] == STREAM_MAGIC)
	{
		DWORD stored = ((const DWORD *)data)[1];
		contents = data + 8;
		contents_size = stored < size - 8 ? stored : size - 8;
	}
	free_contents(picture);
	picture->desc.picType = PICTYPE_UNINITIALIZED;
	status = load_contents(picture, contents, contents_size);
	if (SUCCEEDED(status))
	{
		picture->data = HeapAlloc(GetProcessHeap(), 0, contents_size ? contents_size : 1);
		if (picture->data)
		{
			memcpy(picture->data, contents, contents_size);
			picture->data_size = contents_size;
		}
	}
	HeapFree(GetProcessHeap(), 0, data);
	return status;
}

/* A picture made from a handle has no bytes yet; produce the file form of its type. */
static HRESULT serialize(struct picture *picture)
{
	if (picture->data || picture->desc.picType == PICTYPE_NONE || picture->desc.picType == PICTYPE_UNINITIALIZED)
		return S_OK;
	switch (picture->desc.picType)
	{
	case PICTYPE_BITMAP:
	{
		HBITMAP bitmap = picture->desc.bmp.hbitmap;
		BITMAP info;
		BITMAPFILEHEADER *file;
		BITMAPINFOHEADER *header;
		HDC dc;
		SIZE_T line, total;
		if (!GetObjectW(bitmap, sizeof(info), &info))
			return E_FAIL;
		line = ((SIZE_T)info.bmWidth * 32 + 31) / 32 * 4;
		total = sizeof(*file) + sizeof(*header) + line * info.bmHeight;
		file = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
		if (!file)
			return E_OUTOFMEMORY;
		header = (BITMAPINFOHEADER *)(file + 1);
		header->biSize = sizeof(*header);
		header->biWidth = info.bmWidth;
		header->biHeight = info.bmHeight;
		header->biPlanes = 1;
		header->biBitCount = 32;
		header->biCompression = BI_RGB;
		dc = GetDC(NULL);
		GetDIBits(dc, bitmap, 0, info.bmHeight, header + 1, (BITMAPINFO *)header, DIB_RGB_COLORS);
		ReleaseDC(NULL, dc);
		file->bfType = 0x4d42;
		file->bfSize = (DWORD)total;
		file->bfOffBits = sizeof(*file) + sizeof(*header);
		picture->data = (BYTE *)file;
		picture->data_size = (ULONG)total;
		return S_OK;
	}
	case PICTYPE_ENHMETAFILE:
	{
		UINT size = GetEnhMetaFileBits(picture->desc.emf.hemf, 0, NULL);
		BYTE *bytes = size ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
		if (!bytes)
			return E_FAIL;
		GetEnhMetaFileBits(picture->desc.emf.hemf, size, bytes);
		picture->data = bytes;
		picture->data_size = size;
		return S_OK;
	}
	case PICTYPE_METAFILE:
	{
		UINT size = GetMetaFileBitsEx(picture->desc.wmf.hmeta, 0, NULL);
		struct placeable_header *header;
		WORD *walk;
		WORD sum = 0;
		UINT index;
		BYTE *bytes = size ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + sizeof(*header)) : NULL;
		if (!bytes)
			return E_FAIL;
		header = (struct placeable_header *)bytes;
		header->key = 0x9ac6cdd7;
		header->inch = 1000;
		header->right = (SHORT)MulDiv(picture->width, 1000, HIMETRIC_PER_INCH);
		header->bottom = (SHORT)MulDiv(picture->height, 1000, HIMETRIC_PER_INCH);
		walk = (WORD *)header;
		for (index = 0; index < 10; index++)
			sum ^= walk[index];
		header->checksum = sum;
		GetMetaFileBitsEx(picture->desc.wmf.hmeta, size, bytes + sizeof(*header));
		picture->data = bytes;
		picture->data_size = size + sizeof(*header);
		return S_OK;
	}
	case PICTYPE_ICON:
	{
		ICONINFO info;
		BITMAP color_info, mask_info;
		HDC dc;
		struct icon_header *header;
		struct icon_entry *entry;
		BITMAPINFOHEADER *dib;
		SIZE_T color_line, mask_line, total;
		BYTE *bytes, *mask_at;
		BYTE info_buffer[sizeof(BITMAPINFOHEADER) + 2 * sizeof(RGBQUAD)];
		BITMAPINFO *mask_query = (BITMAPINFO *)info_buffer;
		if (!GetIconInfo(picture->desc.icon.hicon, &info))
			return E_FAIL;
		if (!info.hbmColor || !GetObjectW(info.hbmColor, sizeof(color_info), &color_info) || !GetObjectW(info.hbmMask, sizeof(mask_info), &mask_info))
		{
			if (info.hbmColor)
				DeleteObject(info.hbmColor);
			if (info.hbmMask)
				DeleteObject(info.hbmMask);
			return E_FAIL;
		}
		color_line = (SIZE_T)color_info.bmWidth * 4;
		mask_line = (((SIZE_T)color_info.bmWidth + 31) / 32) * 4;
		total = sizeof(*header) + sizeof(*entry) + sizeof(*dib) + (color_line + mask_line) * color_info.bmHeight;
		bytes = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
		if (!bytes)
		{
			DeleteObject(info.hbmColor);
			DeleteObject(info.hbmMask);
			return E_OUTOFMEMORY;
		}
		header = (struct icon_header *)bytes;
		entry = (struct icon_entry *)(header + 1);
		dib = (BITMAPINFOHEADER *)(entry + 1);
		header->type = 1;
		header->count = 1;
		entry->width = (BYTE)color_info.bmWidth;
		entry->height = (BYTE)color_info.bmHeight;
		entry->x_hotspot = 1;
		entry->y_hotspot = 32;
		entry->size = (DWORD)(total - sizeof(*header) - sizeof(*entry));
		entry->offset = sizeof(*header) + sizeof(*entry);
		dib->biSize = sizeof(*dib);
		dib->biWidth = color_info.bmWidth;
		dib->biHeight = color_info.bmHeight;
		dib->biPlanes = 1;
		dib->biBitCount = 32;
		dib->biCompression = BI_RGB;
		dc = GetDC(NULL);
		GetDIBits(dc, info.hbmColor, 0, color_info.bmHeight, dib + 1, (BITMAPINFO *)dib, DIB_RGB_COLORS);
		dib->biHeight = color_info.bmHeight * 2;
		mask_at = (BYTE *)(dib + 1) + color_line * color_info.bmHeight;
		memset(info_buffer, 0, sizeof(info_buffer));
		mask_query->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		mask_query->bmiHeader.biWidth = color_info.bmWidth;
		mask_query->bmiHeader.biHeight = color_info.bmHeight;
		mask_query->bmiHeader.biPlanes = 1;
		mask_query->bmiHeader.biBitCount = 1;
		mask_query->bmiHeader.biCompression = BI_RGB;
		GetDIBits(dc, info.hbmMask, 0, color_info.bmHeight, mask_at, mask_query, DIB_RGB_COLORS);
		ReleaseDC(NULL, dc);
		DeleteObject(info.hbmColor);
		DeleteObject(info.hbmMask);
		picture->data = bytes;
		picture->data_size = (ULONG)total;
		return S_OK;
	}
	default:
		return E_FAIL;
	}
}

static HRESULT WINAPI stream_Save(IPersistStream *iface, IStream *target, BOOL clear)
{
	struct picture *picture = picture_of_stream(iface);
	DWORD head[2];
	HRESULT status = serialize(picture);
	ULONG put = 0;
	if (FAILED(status))
		return status;
	head[0] = STREAM_MAGIC;
	head[1] = picture->data_size;
	status = IStream_Write(target, head, sizeof(head), &put);
	if (SUCCEEDED(status) && picture->data_size)
		status = IStream_Write(target, picture->data, picture->data_size, &put);
	return status;
}

static HRESULT WINAPI stream_GetSizeMax(IPersistStream *iface, ULARGE_INTEGER *out)
{
	return E_NOTIMPL;
}

static const IPersistStreamVtbl picture_stream_vtbl = {
	stream_QueryInterface,
	stream_AddRef,
	stream_Release,
	stream_GetClassID,
	stream_IsDirty,
	stream_Load,
	stream_Save,
	stream_GetSizeMax,
};

static HRESULT WINAPI container_QueryInterface(IConnectionPointContainer *iface, REFIID riid, void **out)
{
	return picture_QueryInterface(&picture_of_container(iface)->IPicture_iface, riid, out);
}

static ULONG WINAPI container_AddRef(IConnectionPointContainer *iface)
{
	return picture_AddRef(&picture_of_container(iface)->IPicture_iface);
}

static ULONG WINAPI container_Release(IConnectionPointContainer *iface)
{
	return picture_Release(&picture_of_container(iface)->IPicture_iface);
}

static HRESULT WINAPI container_EnumConnectionPoints(IConnectionPointContainer *iface, IEnumConnectionPoints **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI container_FindConnectionPoint(IConnectionPointContainer *iface, REFIID riid, IConnectionPoint **out)
{
	struct picture *picture = picture_of_container(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(riid, &IID_IPropertyNotifySink))
		return CONNECT_E_NOCONNECTION;
	*out = picture->notify;
	IConnectionPoint_AddRef(*out);
	return S_OK;
}

static const IConnectionPointContainerVtbl picture_container_vtbl = {
	container_QueryInterface,
	container_AddRef,
	container_Release,
	container_EnumConnectionPoints,
	container_FindConnectionPoint,
};

static HRESULT WINAPI dispatch_QueryInterface(IDispatch *iface, REFIID riid, void **out)
{
	return picture_QueryInterface(&picture_of_dispatch(iface)->IPicture_iface, riid, out);
}

static ULONG WINAPI dispatch_AddRef(IDispatch *iface)
{
	return picture_AddRef(&picture_of_dispatch(iface)->IPicture_iface);
}

static ULONG WINAPI dispatch_Release(IDispatch *iface)
{
	return picture_Release(&picture_of_dispatch(iface)->IPicture_iface);
}

static HRESULT WINAPI dispatch_GetTypeInfoCount(IDispatch *iface, UINT *out)
{
	if (!out)
		return E_POINTER;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI dispatch_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	ITypeLib *lib = NULL;
	HRESULT status;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (index)
		return DISP_E_BADINDEX;
	status = LoadTypeLib(L"stdole2.tlb", &lib);
	if (FAILED(status))
		return status;
	status = ITypeLib_GetTypeInfoOfGuid(lib, &IID_IPictureDisp, out);
	ITypeLib_Release(lib);
	return status;
}

static HRESULT WINAPI dispatch_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	ITypeInfo *info = NULL;
	HRESULT status = dispatch_GetTypeInfo(iface, 0, lcid, &info);
	if (FAILED(status))
		return status;
	status = ITypeInfo_GetIDsOfNames(info, names, count, ids);
	ITypeInfo_Release(info);
	return status;
}

static HRESULT WINAPI dispatch_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	struct picture *picture = picture_of_dispatch(iface);
	IPicture *object = &picture->IPicture_iface;
	HRESULT status;
	if (!IsEqualIID(riid, &IID_NULL))
		return DISP_E_UNKNOWNNAME;
	if (!params)
		return DISP_E_PARAMNOTOPTIONAL;
	if (params->cArgs && !params->rgvarg)
		return DISP_E_PARAMNOTOPTIONAL;
	if (flags & DISPATCH_PROPERTYGET)
	{
		if (params->cArgs)
			return DISP_E_BADPARAMCOUNT;
		if (!result)
			return DISP_E_PARAMNOTOPTIONAL;
	}
	else if (flags & DISPATCH_PROPERTYPUT)
	{
		if (params->cArgs != 1)
			return DISP_E_BADPARAMCOUNT;
	}
	switch (id)
	{
	case DISPID_PICT_HANDLE:
		if (!(flags & DISPATCH_PROPERTYGET))
			return DISP_E_MEMBERNOTFOUND;
		VariantInit(result);
		V_VT(result) = VT_I4;
		return IPicture_get_Handle(object, (OLE_HANDLE *)&V_I4(result));
	case DISPID_PICT_HPAL:
		if (flags & DISPATCH_PROPERTYGET)
		{
			VariantInit(result);
			V_VT(result) = VT_I4;
			return IPicture_get_hPal(object, (OLE_HANDLE *)&V_I4(result));
		}
		if (flags & DISPATCH_PROPERTYPUT)
		{
			VARIANT value;
			VariantInit(&value);
			status = VariantChangeType(&value, &params->rgvarg[0], 0, VT_I4);
			if (FAILED(status))
				return status;
			return IPicture_set_hPal(object, V_I4(&value));
		}
		return DISP_E_MEMBERNOTFOUND;
	case DISPID_PICT_TYPE:
		if (!(flags & DISPATCH_PROPERTYGET))
			return DISP_E_MEMBERNOTFOUND;
		VariantInit(result);
		V_VT(result) = VT_I2;
		return IPicture_get_Type(object, &V_I2(result));
	case DISPID_PICT_WIDTH:
		if (!(flags & DISPATCH_PROPERTYGET))
			return DISP_E_MEMBERNOTFOUND;
		VariantInit(result);
		V_VT(result) = VT_I4;
		return IPicture_get_Width(object, &V_I4(result));
	case DISPID_PICT_HEIGHT:
		if (!(flags & DISPATCH_PROPERTYGET))
			return DISP_E_MEMBERNOTFOUND;
		VariantInit(result);
		V_VT(result) = VT_I4;
		return IPicture_get_Height(object, &V_I4(result));
	case DISPID_PICT_RENDER:
		if (!(flags & DISPATCH_METHOD))
			return DISP_E_MEMBERNOTFOUND;
	{
		LONG values[10];
		UINT index;
		if (params->cArgs != 10)
			return DISP_E_BADPARAMCOUNT;
		for (index = 0; index < 10; index++)
		{
			if (V_VT(&params->rgvarg[index]) != VT_I4)
				return DISP_E_TYPEMISMATCH;
			values[index] = V_I4(&params->rgvarg[index]);
		}
		return IPicture_Render(object, LongToHandle(values[9]), values[8], values[7], values[6], values[5], values[4], values[3], values[2], values[1], (const RECT *)LongToPtr(values[0]));
	}
	default:
		return DISP_E_MEMBERNOTFOUND;
	}
}

static const IDispatchVtbl picture_dispatch_vtbl = {
	dispatch_QueryInterface,
	dispatch_AddRef,
	dispatch_Release,
	dispatch_GetTypeInfoCount,
	dispatch_GetTypeInfo,
	dispatch_GetIDsOfNames,
	dispatch_Invoke,
};

static HRESULT create_picture(const PICTDESC *desc, BOOL own, struct picture **out)
{
	struct picture *picture = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*picture));
	if (!picture)
		return E_OUTOFMEMORY;
	picture->IPicture_iface.lpVtbl = &picture_vtbl;
	picture->IDispatch_iface.lpVtbl = &picture_dispatch_vtbl;
	picture->IPersistStream_iface.lpVtbl = &picture_stream_vtbl;
	picture->IConnectionPointContainer_iface.lpVtbl = &picture_container_vtbl;
	picture->ref = 1;
	picture->own = own;
	picture->keep_format = TRUE;
	oa_connection_point_create((IUnknown *)&picture->IPicture_iface, &IID_IPropertyNotifySink, &picture->notify);
	if (desc)
	{
		picture->desc = *desc;
		switch (desc->picType)
		{
		case PICTYPE_BITMAP:
			measure_bitmap(picture);
			break;
		case PICTYPE_METAFILE:
			picture->width = desc->wmf.xExt;
			picture->height = desc->wmf.yExt;
			break;
		case PICTYPE_ICON:
			measure_icon(picture);
			break;
		case PICTYPE_ENHMETAFILE:
			measure_enhanced(picture);
			break;
		default:
			break;
		}
	}
	else
		picture->desc.picType = PICTYPE_UNINITIALIZED;
	*out = picture;
	return S_OK;
}

HRESULT WINAPI OleCreatePictureIndirect(PICTDESC *desc, REFIID riid, BOOL own, void **out)
{
	struct picture *picture;
	HRESULT status;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (desc)
	{
		if (desc->cbSizeofstruct != sizeof(PICTDESC) && desc->picType != PICTYPE_UNINITIALIZED && desc->picType != PICTYPE_NONE)
			return E_INVALIDARG;
		if (desc->picType == PICTYPE_UNINITIALIZED)
			return E_UNEXPECTED;
		if (desc->picType < PICTYPE_NONE || desc->picType > PICTYPE_ENHMETAFILE)
			return E_INVALIDARG;
	}
	status = create_picture(desc, own, &picture);
	if (FAILED(status))
		return status;
	status = IPicture_QueryInterface(&picture->IPicture_iface, riid, out);
	IPicture_Release(&picture->IPicture_iface);
	return status;
}

HRESULT WINAPI OleLoadPictureEx(IStream *stream, LONG size, BOOL runmode, REFIID riid, DWORD x, DWORD y, DWORD flags, void **out)
{
	struct picture *picture;
	HRESULT status;
	if (!out)
		return E_POINTER;
	*out = NULL;
	status = create_picture(NULL, TRUE, &picture);
	if (FAILED(status))
		return status;
	status = IPersistStream_Load(&picture->IPersistStream_iface, stream);
	if (SUCCEEDED(status))
		status = IPicture_QueryInterface(&picture->IPicture_iface, riid, out);
	IPicture_Release(&picture->IPicture_iface);
	return status;
}

HRESULT WINAPI OleLoadPicture(IStream *stream, LONG size, BOOL runmode, REFIID riid, void **out)
{
	return OleLoadPictureEx(stream, size, runmode, riid, 0, 0, 0, out);
}

HRESULT WINAPI OleLoadPicturePath(LPOLESTR path, IUnknown *caller, DWORD reserved, OLE_COLOR color, REFIID riid, void **out)
{
	HANDLE file;
	HGLOBAL memory = NULL;
	IStream *stream = NULL;
	LARGE_INTEGER length;
	HRESULT status;
	DWORD got = 0;
	const WCHAR *local = path;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!path)
		return E_INVALIDARG;
	if (!wcsncmp(path, L"file:///", 8))
		local = path + 8;
	else if (wcsstr(path, L"://"))
		return INET_E_RESOURCE_NOT_FOUND;
	file = CreateFileW(local, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());
	if (!GetFileSizeEx(file, &length) || length.QuadPart > 0x7fffffff)
	{
		CloseHandle(file);
		return E_UNEXPECTED;
	}
	memory = GlobalAlloc(GMEM_MOVEABLE, length.QuadPart ? length.QuadPart : 1);
	if (memory)
	{
		void *bytes = GlobalLock(memory);
		ReadFile(file, bytes, (DWORD)length.QuadPart, &got, NULL);
		GlobalUnlock(memory);
	}
	CloseHandle(file);
	if (!memory)
		return E_OUTOFMEMORY;
	status = CreateStreamOnHGlobal(memory, TRUE, &stream);
	if (FAILED(status))
	{
		GlobalFree(memory);
		return status;
	}
	status = OleLoadPicture(stream, got, FALSE, riid, out);
	IStream_Release(stream);
	return status;
}

HRESULT oa_create_std_picture(REFIID riid, void **out)
{
	return OleCreatePictureIndirect(NULL, riid, TRUE, out);
}
