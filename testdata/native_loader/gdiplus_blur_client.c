// The blur effect of GDI+ 1.1 softens a bitmap in place and while it is drawn, as MusicBee blurs the cover behind its player bar. A
// square half black and half white turns grey where the halves meet and keeps its colours away from there. The exit code names the
// first check that failed.
#include <windows.h>

typedef struct {
	UINT32 version;
	void *debug;
	BOOL no_thread;
	BOOL no_codecs;
} StartupInput;

typedef struct {
	float radius;
	BOOL expand_edge;
} BlurParams;

int WINAPI GdiplusStartup(ULONG_PTR *token, const StartupInput *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int WINAPI GdipCreateBitmapFromScan0(INT width, INT height, INT stride, INT format, BYTE *bits, void **bitmap);
int WINAPI GdipBitmapSetPixel(void *bitmap, INT x, INT y, DWORD color);
int WINAPI GdipBitmapGetPixel(void *bitmap, INT x, INT y, DWORD *color);
int WINAPI GdipGetImageGraphicsContext(void *image, void **graphics);
int WINAPI GdipDeleteGraphics(void *graphics);
int WINAPI GdipDisposeImage(void *image);

typedef int(WINAPI *create_effect)(GUID guid, void **effect);
typedef int(WINAPI *delete_effect)(void *effect);
typedef int(WINAPI *set_parameters)(void *effect, const void *data, UINT size);
typedef int(WINAPI *parameter_size)(void *effect, UINT *size);
typedef int(WINAPI *apply_effect)(void *bitmap, void *effect, RECT *roi, BOOL use_aux, void **aux, INT *aux_size);
typedef int(WINAPI *draw_image_fx)(void *graphics, void *image, void *source, void *matrix, void *effect, void *attributes, INT unit);

static const GUID blur = {0x633c80a4, 0x1843, 0x482b, {0x9e, 0xf2, 0xbe, 0x28, 0x34, 0xc5, 0xfd, 0xd4}};

static void *square(void)
{
	void *bitmap = NULL;
	if (GdipCreateBitmapFromScan0(16, 16, 0, 0x26200a, NULL, &bitmap) || !bitmap)
		return NULL;
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
			GdipBitmapSetPixel(bitmap, x, y, x < 8 ? 0xff000000 : 0xffffffff);
	return bitmap;
}

// Away from the edge between them the halves keep their colours, up to the bitmap's own sides, which the blur does not fade; beside
// the edge both turn grey.
static int blurred(void *bitmap)
{
	DWORD dark = 0, light = 0, left = 0, right = 0;
	if (GdipBitmapGetPixel(bitmap, 0, 0, &dark) || GdipBitmapGetPixel(bitmap, 15, 15, &light) || GdipBitmapGetPixel(bitmap, 7, 0, &left) ||
		GdipBitmapGetPixel(bitmap, 8, 15, &right))
		return 0;
	BYTE l = (BYTE)left, r = (BYTE)right;
	return dark == 0xff000000 && light == 0xffffffff && (left >> 24) == 255 && (right >> 24) == 255 && l > 40 && l < 128 && r > 128 &&
		r < 215;
}

int raven_entry(void)
{
	ULONG_PTR token;
	StartupInput input = {1, NULL, FALSE, FALSE};
	if (GdiplusStartup(&token, &input, NULL))
		return 1;
	HMODULE gdiplus = GetModuleHandleA("gdiplus.dll");
	create_effect create = (create_effect)GetProcAddress(gdiplus, "GdipCreateEffect");
	delete_effect destroy = (delete_effect)GetProcAddress(gdiplus, "GdipDeleteEffect");
	set_parameters set = (set_parameters)GetProcAddress(gdiplus, "GdipSetEffectParameters");
	parameter_size size_of = (parameter_size)GetProcAddress(gdiplus, "GdipGetEffectParameterSize");
	apply_effect apply = (apply_effect)GetProcAddress(gdiplus, "GdipBitmapApplyEffect");
	draw_image_fx draw = (draw_image_fx)GetProcAddress(gdiplus, "GdipDrawImageFX");
	if (!create || !destroy || !set || !size_of || !apply || !draw)
		return 2;
	void *effect = NULL;
	if (create(blur, &effect) || !effect)
		return 3;
	BlurParams params = {3.0f, FALSE};
	UINT size = 0;
	if (set(effect, &params, sizeof params) || size_of(effect, &size) || size != sizeof params)
		return 4;
	void *bitmap = square();
	if (!bitmap)
		return 5;
	if (apply(bitmap, effect, NULL, FALSE, NULL, NULL))
		return 6;
	if (!blurred(bitmap))
		return 7;
	// The source draws over a copy of itself, as MusicBee blurs a bitmap through its own graphics: the faded ends land on the original.
	void *source = square(), *target = square(), *graphics = NULL;
	if (!source || !target || GdipGetImageGraphicsContext(target, &graphics) || !graphics)
		return 8;
	if (draw(graphics, source, NULL, NULL, effect, NULL, 2))
		return 9;
	if (!blurred(target))
		return 10;
	GdipDeleteGraphics(graphics);
	GdipDisposeImage(target);
	GdipDisposeImage(source);
	GdipDisposeImage(bitmap);
	destroy(effect);
	GdiplusShutdown(token);
	return 0;
}
