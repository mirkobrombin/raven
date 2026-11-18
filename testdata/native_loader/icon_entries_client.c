#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

/* Icon files read through the imaging component. An entry with the 12-byte core header takes its depth and colors from that header, so
   its mask makes the pixels it marks transparent. An entry whose offset and size add up past four gigabytes is refused, though in a
   32-bit program the sum wraps round to a small number. An entry that claims more colors than it holds is read without its mask. The
   exit code names the first check that failed. */

#define PLAIN __attribute__((optimize("no-tree-loop-distribute-patterns")))

static BYTE file[256];
static UINT file_size;

static void put16(UINT at, UINT value)
{
	file[at] = (BYTE)value;
	file[at + 1] = (BYTE)(value >> 8);
}

static void put32(UINT at, UINT value)
{
	put16(at, value & 0xffff);
	put16(at + 2, value >> 16);
}

PLAIN static void clear(void)
{
	for (UINT at = 0; at < sizeof file; ++at)
		file[at] = 0;
}

/* The directory of one entry of two by two pixels, its data `bytes` long at `offset`. */
static void directory(UINT bytes, UINT offset)
{
	put16(2, 1);
	put16(4, 1);
	file[6] = 2;
	file[7] = 2;
	put16(10, 1);
	put16(12, 24);
	put32(14, bytes);
	put32(18, offset);
}

/* A core header (12 bytes), 24-bit rows of eight bytes stored bottom up, then the mask: its rows of four bytes bottom up too, the top
   row's first bit set, so the top left pixel is transparent. The pixels are made so that bytes 14 and 15 of the entry read as 0x2020. */
PLAIN static void core_icon(void)
{
	clear();
	const UINT at = 22;
	directory(12 + 16 + 8, at);
	put32(at, 12);
	put16(at + 4, 2);
	put16(at + 6, 4);
	put16(at + 8, 1);
	put16(at + 10, 24);
	for (UINT index = 0; index < 16; ++index)
		file[at + 12 + index] = 0x20;
	file[at + 12 + 16 + 4] = 0x80;
	file_size = at + 12 + 16 + 8;
}

/* A 40-byte header of an 8-bit entry claiming 0x40000000 colors, with only its 256 colors, rows and mask present. */
PLAIN static void many_colors(void)
{
	clear();
	file_size = sizeof file;
	directory(sizeof file - 22, 22);
	file[12] = 8;
	put32(22, 40);
	put32(26, 2);
	put32(30, 4);
	put16(34, 1);
	put16(36, 8);
	put32(54, 0x40000000);
}

/* 0 when the first frame decodes and its pixels come back, 1 when the file is refused anywhere on the way. */
static int decode(IWICImagingFactory *factory, BYTE *pixels, UINT size)
{
	IWICStream *stream = NULL;
	IWICBitmapDecoder *decoder = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICFormatConverter *converter = NULL;
	int outcome = 1;
	if (IWICImagingFactory_CreateStream(factory, &stream) != S_OK || IWICStream_InitializeFromMemory(stream, file, file_size) != S_OK)
		return 3;
	if (IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder) == S_OK &&
		IWICBitmapDecoder_GetFrame(decoder, 0, &frame) == S_OK && IWICImagingFactory_CreateFormatConverter(factory, &converter) == S_OK &&
		IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL,
			0, WICBitmapPaletteTypeCustom) == S_OK &&
		IWICFormatConverter_CopyPixels(converter, NULL, 8, size, pixels) == S_OK)
		outcome = 0;
	if (converter)
		IWICFormatConverter_Release(converter);
	if (frame)
		IWICBitmapFrameDecode_Release(frame);
	if (decoder)
		IWICBitmapDecoder_Release(decoder);
	IWICStream_Release(stream);
	return outcome;
}

int raven_entry(void)
{
	IWICImagingFactory *factory = NULL;
	BYTE pixels[16];
	int outcome;
	if (FAILED(CoInitialize(NULL)) ||
		CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory) != S_OK)
		return 1;
	core_icon();
	if ((outcome = decode(factory, pixels, sizeof pixels)))
		return 10 + outcome;
	/* Top left transparent, the other three opaque. */
	if (pixels[3] || pixels[7] != 255 || pixels[11] != 255 || pixels[15] != 255)
		return 20;
	core_icon();
	put32(14, 0x80000010);
	put32(18, 0x80000000);
	if ((outcome = decode(factory, pixels, sizeof pixels)) != 1)
		return 30 + outcome;
	many_colors();
	if ((outcome = decode(factory, pixels, sizeof pixels)) > 1)
		return 40 + outcome;
	IWICImagingFactory_Release(factory);
	CoUninitialize();
	return 0;
}
