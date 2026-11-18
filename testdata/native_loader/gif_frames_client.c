#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

/* A GIF file of two frames read through the imaging component. The codes of the first frame break off with a code that cannot be there,
   in the middle of a data block that holds more bytes after it, a zero and the trailer's 0x3b among them; the second frame is whole. The
   first frame keeps what its codes gave, and the second is still found by the blocks' own lengths, not by where the codes broke off. The
   exit code names the first check that failed. */

static const BYTE file[] = {
	'G', 'I', 'F', '8', '9', 'a', 2, 0, 1, 0, 0x81, 0, 0,
	0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255,
	/* Two by one pixels, literals of two bits: a clear code, the index 1, then 7 where at most 6 can come. */
	0x2c, 0, 0, 0, 0, 2, 0, 1, 0, 0, 2, 4, 0xcc, 0x01, 0x00, 0x3b, 0,
	/* A clear code, the indices 2 and 3, the end code. */
	0x2c, 0, 0, 0, 0, 2, 0, 1, 0, 0, 2, 2, 0xd4, 0x0a, 0,
	0x3b,
};

/* Without the C runtime, so without the memcmp of IsEqualGUID. */
static BOOL same_guid(const GUID *a, const GUID *b)
{
	const BYTE *left = (const BYTE *)a, *right = (const BYTE *)b;
	for (UINT at = 0; at < sizeof(GUID); ++at)
		if (left[at] != right[at])
			return FALSE;
	return TRUE;
}

/* The two indices of `index`'s frame in out; 0 when they came back. */
static int frame(IWICBitmapDecoder *decoder, UINT index, BYTE *out)
{
	IWICBitmapFrameDecode *decoded = NULL;
	WICPixelFormatGUID format;
	UINT width = 0, height = 0;
	int outcome = 1;
	if (IWICBitmapDecoder_GetFrame(decoder, index, &decoded) != S_OK)
		return 2;
	if (IWICBitmapFrameDecode_GetSize(decoded, &width, &height) == S_OK && width == 2 && height == 1 &&
		IWICBitmapFrameDecode_GetPixelFormat(decoded, &format) == S_OK && same_guid(&format, &GUID_WICPixelFormat8bppIndexed) &&
		IWICBitmapFrameDecode_CopyPixels(decoded, NULL, 4, 4, out) == S_OK)
		outcome = 0;
	IWICBitmapFrameDecode_Release(decoded);
	return outcome;
}

int raven_entry(void)
{
	IWICImagingFactory *factory = NULL;
	IWICStream *stream = NULL;
	IWICBitmapDecoder *decoder = NULL;
	BYTE pixels[4];
	UINT count = 0;
	int outcome;
	if (FAILED(CoInitialize(NULL)) ||
		CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory) != S_OK)
		return 1;
	if (IWICImagingFactory_CreateStream(factory, &stream) != S_OK ||
		IWICStream_InitializeFromMemory(stream, (BYTE *)file, sizeof file) != S_OK ||
		IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder) != S_OK)
		return 2;
	if (IWICBitmapDecoder_GetFrameCount(decoder, &count) != S_OK || count != 2)
		return 10 + (int)min(count, 9);
	if ((outcome = frame(decoder, 0, pixels)))
		return 20 + outcome;
	/* The first frame stops after the index its codes gave. */
	if (pixels[0] != 1 || pixels[1])
		return 30;
	if ((outcome = frame(decoder, 1, pixels)))
		return 40 + outcome;
	if (pixels[0] != 2 || pixels[1] != 3)
		return 50;
	IWICBitmapDecoder_Release(decoder);
	IWICStream_Release(stream);
	IWICImagingFactory_Release(factory);
	CoUninitialize();
	return 0;
}
