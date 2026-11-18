/* The Windows types and calls the image decoders of windowscodecs.dll use (native/guest/raven-windowscodecs), declared for building those
   decoders on the host for native/fuzz/image_decoders.py. It stands in for the component's own wic.h; image_decoders.c defines what is
   declared here. */
#ifndef RAVEN_FUZZ_WIC_H
#define RAVEN_FUZZ_WIC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t BYTE;
typedef uint16_t USHORT;
typedef uint32_t UINT, DWORD, ULONG, WICColor;
typedef int32_t INT, LONG, HRESULT;
typedef uint64_t UINT64;
typedef size_t SIZE_T;
typedef int BOOL;
#define TRUE 1
#define FALSE 0

typedef struct
{
	uint32_t Data1;
	uint16_t Data2, Data3;
	uint8_t Data4[8];
} GUID;
#define IsEqualGUID(a, b) (!memcmp((a), (b), sizeof(GUID)))

typedef struct
{
	INT X, Y, Width, Height;
} WICRect;

#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define WINCODEC_ERR_UNKNOWNIMAGEFORMAT ((HRESULT)0x88982F07L)
#define WINCODEC_ERR_COMPONENTNOTFOUND ((HRESULT)0x88982F50L)
#define WINCODEC_ERR_BADIMAGE ((HRESULT)0x88982F60L)
#define WINCODEC_ERR_BADHEADER ((HRESULT)0x88982F61L)
#define WINCODEC_ERR_STREAMREAD ((HRESULT)0x88982F72L)
#define FAILED(result) ((HRESULT)(result) < 0)
#define SUCCEEDED(result) ((HRESULT)(result) >= 0)

#define BI_RGB 0
#define BI_RLE8 1
#define BI_RLE4 2
#define BI_BITFIELDS 3

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

/* Each format's Data1 is its number of bits per pixel, which is all image_decoders.c needs of it. */
extern const GUID GUID_WICPixelFormatBlackWhite, GUID_WICPixelFormat1bppIndexed, GUID_WICPixelFormat2bppIndexed,
	GUID_WICPixelFormat4bppIndexed, GUID_WICPixelFormat8bppIndexed, GUID_WICPixelFormat2bppGray, GUID_WICPixelFormat4bppGray,
	GUID_WICPixelFormat8bppGray, GUID_WICPixelFormat16bppGray, GUID_WICPixelFormat16bppBGR555, GUID_WICPixelFormat16bppBGR565,
	GUID_WICPixelFormat24bppBGR, GUID_WICPixelFormat24bppRGB, GUID_WICPixelFormat32bppBGR, GUID_WICPixelFormat32bppBGRA,
	GUID_WICPixelFormat32bppPBGRA, GUID_WICPixelFormat32bppRGBA, GUID_WICPixelFormat48bppRGB, GUID_WICPixelFormat48bppBGR,
	GUID_WICPixelFormat64bppRGBA, GUID_WICPixelFormat64bppBGRA;

typedef struct IWICBitmap IWICBitmap;
typedef struct IWICBitmapSource IWICBitmapSource;
HRESULT IWICBitmapSource_GetSize(IWICBitmapSource *source, UINT *width, UINT *height);
HRESULT IWICBitmapSource_CopyPixels(IWICBitmapSource *source, const WICRect *rect, UINT stride, UINT size, BYTE *pixels);
ULONG IWICBitmapSource_Release(IWICBitmapSource *source);

void *wic_alloc(SIZE_T size);
void wic_free(void *block);
UINT wic_stride(UINT width, UINT bits);

typedef struct image_list image_list;

#endif
