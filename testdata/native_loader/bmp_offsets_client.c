#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

/* A BMP file whose pixel offset points past its end is refused, however far past: in a 32-bit program an offset near four gigabytes
   wraps the address of the pixels round to below the file, where they must not be read from. A file whose offset and rows fit is read
   whole. The exit code names the first check that failed. */

#define PLAIN __attribute__((optimize("no-tree-loop-distribute-patterns")))

static BYTE file[14 + 40 + 16];

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

/* Two by two pixels of 24 bits, the rows padded to eight bytes, stored bottom up, with the pixels at `offset`. */
PLAIN static void write_bmp(UINT offset)
{
	for (UINT at = 0; at < sizeof file; ++at)
		file[at] = 0;
	file[0] = 'B';
	file[1] = 'M';
	put32(2, sizeof file);
	put32(10, offset);
	put32(14, 40);
	put32(18, 2);
	put32(22, 2);
	put16(26, 1);
	put16(28, 24);
	for (UINT at = 0; at < 16; ++at)
		file[54 + at] = (BYTE)(at * 13 + 1);
}

/* 0 when the file decodes and its pixels come back, 1 when it is refused anywhere on the way, 2 when its pixels differ. */
static int decode(IWICImagingFactory *factory)
{
	IWICStream *stream = NULL;
	IWICBitmapDecoder *decoder = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	BYTE pixels[16];
	int outcome = 1;
	if (IWICImagingFactory_CreateStream(factory, &stream) != S_OK || IWICStream_InitializeFromMemory(stream, file, sizeof file) != S_OK)
		return 3;
	if (IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder) == S_OK &&
		IWICBitmapDecoder_GetFrame(decoder, 0, &frame) == S_OK && IWICBitmapFrameDecode_CopyPixels(frame, NULL, 8, sizeof pixels, pixels) == S_OK) {
		outcome = 0;
		/* The top row is the second stored. */
		for (UINT x = 0; x < 6; ++x)
			if (pixels[x] != file[54 + 8 + x] || pixels[8 + x] != file[54 + x])
				outcome = 2;
	}
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
	int outcome;
	if (FAILED(CoInitialize(NULL)) ||
		CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory) != S_OK)
		return 1;
	write_bmp(54);
	if ((outcome = decode(factory)))
		return 10 + outcome;
	/* Offsets past the end, from just past it to the last ones that wrap. */
	static const UINT past[] = {sizeof file + 1, 0x7fffffff, 0xfffff000, 0xffffffff};
	for (UINT index = 0; index < sizeof past / sizeof past[0]; ++index) {
		write_bmp(past[index]);
		if ((outcome = decode(factory)) != 1)
			return 20 + 10 * (int)index + outcome;
	}
	/* Pixels that start inside the file but whose rows run past its end. */
	write_bmp(sizeof file - 8);
	if ((outcome = decode(factory)) != 1)
		return 70 + outcome;
	IWICImagingFactory_Release(factory);
	CoUninitialize();
	return 0;
}
