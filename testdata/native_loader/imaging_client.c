#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

/* Pictures as installer bootstrappers draw their themes: a PNG file decoded through the imaging component, converted to premultiplied
   BGRA and copied out; then the same file opened through GDI+ and turned into a bitmap handle. A picture half red and half blue also
   goes through the TIFF and JPEG encoders and back, which TIFF keeps exactly and JPEG within the loss of its compression. */

typedef struct {
	UINT32 version;
	void *debug;
	BOOL no_thread;
	BOOL no_codecs;
} StartupInput;

int WINAPI GdiplusStartup(ULONG_PTR *token, const StartupInput *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int WINAPI GdipCreateBitmapFromFile(const WCHAR *path, void **bitmap);
int WINAPI GdipGetImageWidth(void *image, UINT *width);
int WINAPI GdipGetImageHeight(void *image, UINT *height);
int WINAPI GdipCreateHBITMAPFromBitmap(void *bitmap, HBITMAP *handle, DWORD background);
int WINAPI GdipDisposeImage(void *image);

/* Three pixels over two rows, RGBA: red, green, blue; white, black and a half transparent dark blue. */
static const BYTE picture[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x9d, 0x74, 0x66, 0x1a, 0x00, 0x00, 0x00, 0x1a, 0x49, 0x44, 0x41, 0x54, 0x78,
	0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x0c, 0x19, 0xfe, 0x03, 0x49, 0x20, 0x60, 0x00, 0xb2, 0xb8, 0x44, 0xe4, 0x1a, 0x00, 0xa1, 0x99,
	0x0b, 0xb2, 0x50, 0xb3, 0xac, 0x21, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

/* The 16x16 picture encoded in the container and decoded again; 0 when every pixel comes back within the tolerance. */
static int round_trip(IWICImagingFactory *factory, const GUID *container, int tolerance)
{
	static BYTE source[16 * 16 * 3], back[16 * 16 * 3];
	IWICBitmap *bitmap = NULL;
	IStream *stream = NULL;
	IWICBitmapEncoder *encoder = NULL;
	IWICBitmapFrameEncode *frame = NULL;
	IPropertyBag2 *options = NULL;
	IWICBitmapDecoder *decoder = NULL;
	IWICBitmapFrameDecode *decoded = NULL;
	IWICBitmapSource *converted = NULL;
	WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
	LARGE_INTEGER start = {0};
	UINT width = 0, height = 0;
	int at;
	for (at = 0; at < 16 * 16; ++at) {
		const BOOL left = at % 16 < 8;
		source[at * 3] = left ? 0 : 255;
		source[at * 3 + 1] = 0;
		source[at * 3 + 2] = left ? 255 : 0;
	}
	if (IWICImagingFactory_CreateBitmapFromMemory(factory, 16, 16, &GUID_WICPixelFormat24bppBGR, 16 * 3, sizeof source, source, &bitmap) != S_OK)
		return 1;
	if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK || IWICImagingFactory_CreateEncoder(factory, container, NULL, &encoder) != S_OK)
		return 2;
	if (IWICBitmapEncoder_Initialize(encoder, stream, WICBitmapEncoderNoCache) != S_OK ||
		IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &options) != S_OK || IWICBitmapFrameEncode_Initialize(frame, options) != S_OK)
		return 3;
	if (IWICBitmapFrameEncode_SetSize(frame, 16, 16) != S_OK || IWICBitmapFrameEncode_SetPixelFormat(frame, &format) != S_OK ||
		IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)bitmap, NULL) != S_OK)
		return 4;
	if (IWICBitmapFrameEncode_Commit(frame) != S_OK || IWICBitmapEncoder_Commit(encoder) != S_OK)
		return 5;
	if (IStream_Seek(stream, start, STREAM_SEEK_SET, NULL) != S_OK ||
		IWICImagingFactory_CreateDecoderFromStream(factory, stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder) != S_OK)
		return 6;
	if (IWICBitmapDecoder_GetFrame(decoder, 0, &decoded) != S_OK || IWICBitmapFrameDecode_GetSize(decoded, &width, &height) != S_OK ||
		width != 16 || height != 16)
		return 7;
	if (WICConvertBitmapSource(&GUID_WICPixelFormat24bppBGR, (IWICBitmapSource *)decoded, &converted) != S_OK ||
		IWICBitmapSource_CopyPixels(converted, NULL, 16 * 3, sizeof back, back) != S_OK)
		return 8;
	/* A lossy container blurs the colour where red meets blue, so only the columns away from that edge are held to the tolerance. */
	for (at = 0; at < (int)sizeof back; ++at)
		if ((!tolerance || at / 3 % 16 < 5 || at / 3 % 16 > 10) && (back[at] - source[at] > tolerance || source[at] - back[at] > tolerance))
			return 9;
	IWICBitmapSource_Release(converted);
	IWICBitmapFrameDecode_Release(decoded);
	IWICBitmapDecoder_Release(decoder);
	if (options)
		IPropertyBag2_Release(options);
	IWICBitmapFrameEncode_Release(frame);
	IWICBitmapEncoder_Release(encoder);
	IStream_Release(stream);
	IWICBitmap_Release(bitmap);
	return 0;
}

int raven_entry(void)
{
	WCHAR path[MAX_PATH];
	DWORD written;
	IWICImagingFactory *factory = NULL;
	IWICBitmapDecoder *decoder = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICBitmapSource *converted = NULL;
	UINT width = 0, height = 0;
	BYTE pixels[3 * 2 * 4];
	GetTempPathW(MAX_PATH, path);
	lstrcatW(path, L"logo.png");
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, picture, sizeof picture, &written, NULL) || !CloseHandle(file))
		return 1;
	if (FAILED(CoInitialize(NULL)))
		return 2;
	if (CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory) != S_OK)
		return 3;
	if (IWICImagingFactory_CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder) != S_OK)
		return 4;
	if (IWICBitmapDecoder_GetFrame(decoder, 0, &frame) != S_OK)
		return 5;
	if (IWICBitmapFrameDecode_GetSize(frame, &width, &height) != S_OK || width != 3 || height != 2)
		return 6;
	if (WICConvertBitmapSource(&GUID_WICPixelFormat32bppPBGRA, (IWICBitmapSource *)frame, &converted) != S_OK)
		return 7;
	if (IWICBitmapSource_CopyPixels(converted, NULL, 3 * 4, sizeof pixels, pixels) != S_OK)
		return 8;
	/* Red first, as blue, green, red and alpha; black in the middle of the second row; the last pixel keeps half its opacity. */
	if (pixels[0] != 0 || pixels[1] != 0 || pixels[2] != 255 || pixels[3] != 255 || pixels[16] || pixels[17] || pixels[18] ||
		pixels[19] != 255 || pixels[23] != 128)
		return 9;
	IWICBitmapSource_Release(converted);
	IWICBitmapFrameDecode_Release(frame);
	IWICBitmapDecoder_Release(decoder);
	int failed;
	if ((failed = round_trip(factory, &GUID_ContainerFormatTiff, 0)))
		return 20 + failed;
	if ((failed = round_trip(factory, &GUID_ContainerFormatJpeg, 24)))
		return 30 + failed;
	IWICImagingFactory_Release(factory);
	ULONG_PTR token;
	StartupInput input = {1, NULL, FALSE, FALSE};
	void *bitmap = NULL;
	HBITMAP handle = NULL;
	BITMAP described;
	if (GdiplusStartup(&token, &input, NULL))
		return 10;
	if (GdipCreateBitmapFromFile(path, &bitmap) || !bitmap)
		return 11;
	if (GdipGetImageWidth(bitmap, &width) || GdipGetImageHeight(bitmap, &height) || width != 3 || height != 2)
		return 12;
	if (GdipCreateHBITMAPFromBitmap(bitmap, &handle, 0) || !handle)
		return 13;
	if (GetObjectW(handle, sizeof described, &described) != sizeof described || described.bmWidth != 3 || described.bmHeight != 2)
		return 14;
	DeleteObject(handle);
	GdipDisposeImage(bitmap);
	GdiplusShutdown(token);
	DeleteFileW(path);
	CoUninitialize();
	return 0;
}
