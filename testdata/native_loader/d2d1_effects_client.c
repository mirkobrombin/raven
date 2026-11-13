// Effects drawn through DrawImage give pixels: a program's own effect runs the pixel shader its draw transform names over the inputs it asks
// for, as paint.net draws its canvas, its checkerboard and its color conversions; a system effect does its own step; a graph of several nodes
// runs them in order. A target over a half-float texture holds what is drawn as half floats, and a command list covers what was drawn into
// it. The exit code names the first check that failed.
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <d2d1_3.h>
#include <d2d1effectauthor.h>
#include <d3dcommon.h>
#include <d3d11.h>
#include <wincodec.h>

// ID2D1Effect has no C macros: its output is the method after the eighteen of ID2D1Properties and its own first four.
static ID2D1Image *output_of(ID2D1Effect *effect)
{
	ID2D1Image *image = NULL;
	((void (STDMETHODCALLTYPE *)(void *, ID2D1Image **))(*(void ***)effect)[18])(effect, &image);
	return image;
}

typedef HRESULT (WINAPI *create_device_function)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
	ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *create_function)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
typedef HRESULT (WINAPI *compile_function)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *, const char *,
	const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

static const GUID generator_shader = {0x6e2f0c11, 0x1d2a, 0x4b4e, {0x9a, 0x51, 0x2b, 0x70, 0x0c, 0x3d, 0x11, 0x01}};
static const GUID swap_shader = {0x6e2f0c11, 0x1d2a, 0x4b4e, {0x9a, 0x51, 0x2b, 0x70, 0x0c, 0x3d, 0x11, 0x02}};
static const CLSID generator_effect = {0x6e2f0c11, 0x1d2a, 0x4b4e, {0x9a, 0x51, 0x2b, 0x70, 0x0c, 0x3d, 0x22, 0x01}};
static const CLSID swap_effect = {0x6e2f0c11, 0x1d2a, 0x4b4e, {0x9a, 0x51, 0x2b, 0x70, 0x0c, 0x3d, 0x22, 0x02}};
static const CLSID chain_effect = {0x6e2f0c11, 0x1d2a, 0x4b4e, {0x9a, 0x51, 0x2b, 0x70, 0x0c, 0x3d, 0x22, 0x03}};

// Green left of x = 2 and blue from there, by the position Direct2D gives each pixel and the two colors the transform sets.
static const char generator_source[] =
	"cbuffer colors : register(b0) { float4 first; float4 second; };\n"
	"float4 main(float4 position : SV_POSITION, float4 scene : SCENE_POSITION) : SV_TARGET { return lerp(first, second, step(2, scene.x)); }\n";
// The input with its red and blue swapped, read where the texture coordinate of the input places the pixel.
static const char swap_source[] =
	"Texture2D input : register(t0);\n"
	"SamplerState sampled : register(s0);\n"
	"float4 main(float4 position : SV_POSITION, float4 scene : SCENE_POSITION, float4 texel : TEXCOORD0) : SV_TARGET\n"
	"{ return input.Sample(sampled, texel.xy).bgra; }\n";

static ID3DBlob *generator_code, *swap_code;

// A draw transform: the generator covers four pixels from the origin and reads nothing, the swap covers and reads what its input covers.
struct transform {
	void **table;
	int generator;
};

static HRESULT STDMETHODCALLTYPE transform_QueryInterface(void *self, REFIID iid, void **out)
{
	*out = self;
	return S_OK;
}
static ULONG STDMETHODCALLTYPE transform_AddRef(void *self) { return 1; }
static ULONG STDMETHODCALLTYPE transform_Release(void *self) { return 1; }
static UINT32 STDMETHODCALLTYPE transform_GetInputCount(void *self) { return ((struct transform *)self)->generator ? 0 : 1; }
static HRESULT STDMETHODCALLTYPE transform_MapOutputRectToInputRects(void *self, const D2D1_RECT_L *output, D2D1_RECT_L *inputs, UINT32 count)
{
	/* paint.net's generators leave this unimplemented: with no inputs there is nothing to ask for, and Direct2D does not ask. */
	if (!count)
		return E_NOTIMPL;
	inputs[0] = *output;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE transform_MapInputRectsToOutputRect(void *self, const D2D1_RECT_L *inputs, const D2D1_RECT_L *opaque,
	UINT32 count, D2D1_RECT_L *output, D2D1_RECT_L *output_opaque)
{
	static const D2D1_RECT_L square = {0, 0, 4, 4}, none = {0, 0, 0, 0};
	*output = ((struct transform *)self)->generator ? square : inputs[0];
	*output_opaque = none;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE transform_MapInvalidRect(void *self, UINT32 index, D2D1_RECT_L invalid, D2D1_RECT_L *output)
{
	*output = invalid;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE transform_SetDrawInfo(void *self, void *info)
{
	typedef HRESULT (STDMETHODCALLTYPE *set_constants)(void *, const BYTE *, UINT32);
	typedef HRESULT (STDMETHODCALLTYPE *set_shader)(void *, REFGUID, D2D1_PIXEL_OPTIONS);
	static const float colors[8] = {0, 1, 0, 1, 0, 0, 1, 1};
	if (!((struct transform *)self)->generator)
		return ((set_shader)(*(void ***)info)[10])(info, &swap_shader, D2D1_PIXEL_OPTIONS_NONE);
	if (FAILED(((set_constants)(*(void ***)info)[7])(info, (const BYTE *)colors, sizeof colors)))
		return E_FAIL;
	return ((set_shader)(*(void ***)info)[10])(info, &generator_shader, D2D1_PIXEL_OPTIONS_NONE);
}
static void *transform_table[] = {transform_QueryInterface, transform_AddRef, transform_Release, transform_GetInputCount,
	transform_MapOutputRectToInputRects, transform_MapInputRectsToOutputRect, transform_MapInvalidRect, transform_SetDrawInfo};
static struct transform generator_transform = {transform_table, 1}, swap_transform = {transform_table, 0};

// What the graph is given for each transform, as .NET gives it: a node of four methods, from which the transform is a query away.
struct face {
	void **table;
	struct transform *transform;
};
static int same_iid(REFIID a, REFIID b) { return ((const UINT64 *)a)[0] == ((const UINT64 *)b)[0] && ((const UINT64 *)a)[1] == ((const UINT64 *)b)[1]; }
static HRESULT STDMETHODCALLTYPE face_QueryInterface(void *self, REFIID iid, void **out)
{
	*out = same_iid(iid, &IID_ID2D1DrawTransform) || same_iid(iid, &IID_ID2D1Transform) ? (void *)((struct face *)self)->transform : self;
	return S_OK;
}
static UINT32 STDMETHODCALLTYPE face_GetInputCount(void *self) { return transform_GetInputCount(((struct face *)self)->transform); }
static void *face_table[] = {face_QueryInterface, transform_AddRef, transform_Release, face_GetInputCount};
static struct face generator_face = {face_table, &generator_transform}, swap_face = {face_table, &swap_transform};

// The effects' objects: each loads its shader and places its transforms. The chain premultiplies its input with the system's effect and
// swaps the result, two nodes joined to each other and to the effect's input.
struct impl {
	void **table;
	int kind;
};
enum { GENERATOR, SWAP, CHAIN };

static HRESULT STDMETHODCALLTYPE impl_QueryInterface(void *self, REFIID iid, void **out)
{
	*out = self;
	return S_OK;
}
static ULONG STDMETHODCALLTYPE impl_AddRef(void *self) { return 1; }
static ULONG STDMETHODCALLTYPE impl_Release(void *self) { return 1; }
static HRESULT STDMETHODCALLTYPE impl_Initialize(void *self, void *context, void *graph)
{
	typedef HRESULT (STDMETHODCALLTYPE *load_shader)(void *, REFGUID, const BYTE *, UINT32);
	typedef HRESULT (STDMETHODCALLTYPE *create_effect)(void *, REFCLSID, ID2D1Effect **);
	typedef HRESULT (STDMETHODCALLTYPE *node_from_effect)(void *, ID2D1Effect *, void **);
	typedef HRESULT (STDMETHODCALLTYPE *one_node)(void *, void *);
	typedef HRESULT (STDMETHODCALLTYPE *connect_nodes)(void *, void *, void *, UINT32);
	typedef HRESULT (STDMETHODCALLTYPE *connect_input)(void *, UINT32, void *, UINT32);
	const load_shader load = (load_shader)(*(void ***)context)[11];
	if (FAILED(load(context, &generator_shader, ID3D10Blob_GetBufferPointer(generator_code), (UINT32)ID3D10Blob_GetBufferSize(generator_code))) ||
		FAILED(load(context, &swap_shader, ID3D10Blob_GetBufferPointer(swap_code), (UINT32)ID3D10Blob_GetBufferSize(swap_code))))
		return E_FAIL;
	switch (((struct impl *)self)->kind) {
	case GENERATOR:
		return ((one_node)(*(void ***)graph)[4])(graph, &generator_face);
	case SWAP:
		return ((one_node)(*(void ***)graph)[4])(graph, &swap_face);
	default: {
		ID2D1Effect *premultiply = NULL;
		void *node = NULL;
		if (FAILED(((create_effect)(*(void ***)context)[4])(context, &CLSID_D2D1Premultiply, &premultiply)) ||
			FAILED(((node_from_effect)(*(void ***)context)[6])(context, premultiply, &node)) ||
			FAILED(((one_node)(*(void ***)graph)[5])(graph, node)) || FAILED(((one_node)(*(void ***)graph)[5])(graph, &swap_face)) ||
			FAILED(((connect_input)(*(void ***)graph)[9])(graph, 0, node, 0)) ||
			FAILED(((connect_nodes)(*(void ***)graph)[8])(graph, node, &swap_face, 0)) ||
			FAILED(((one_node)(*(void ***)graph)[7])(graph, &swap_face)))
			return E_FAIL;
		IUnknown_Release((IUnknown *)node);
		IUnknown_Release((IUnknown *)premultiply);
		return S_OK;
	}
	}
}
static HRESULT STDMETHODCALLTYPE impl_PrepareForRender(void *self, UINT32 change) { return S_OK; }
static HRESULT STDMETHODCALLTYPE impl_SetGraph(void *self, void *graph) { return E_NOTIMPL; }
static void *impl_table[] = {impl_QueryInterface, impl_AddRef, impl_Release, impl_Initialize, impl_PrepareForRender, impl_SetGraph};
static struct impl impls[3] = {{impl_table, GENERATOR}, {impl_table, SWAP}, {impl_table, CHAIN}};
static HRESULT CALLBACK make_generator(IUnknown **out) { *out = (IUnknown *)&impls[GENERATOR]; return S_OK; }
static HRESULT CALLBACK make_swap(IUnknown **out) { *out = (IUnknown *)&impls[SWAP]; return S_OK; }
static HRESULT CALLBACK make_chain(IUnknown **out) { *out = (IUnknown *)&impls[CHAIN]; return S_OK; }

// Whether a channel is near the value expected, as the conversions round.
static int near_value(UINT32 pixel, int shift, int expected) { int value = (int)((pixel >> shift) & 255); return value >= expected - 3 && value <= expected + 3; }
static int pixel_is(UINT32 pixel, int red, int green, int blue)
{
	return near_value(pixel, 16, red) && near_value(pixel, 8, green) && near_value(pixel, 0, blue);
}

int raven_entry(void)
{
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
		return 1;
	HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
	compile_function compile = compiler ? (compile_function)GetProcAddress(compiler, "D3DCompile") : NULL;
	if (!compile || FAILED(compile(generator_source, sizeof generator_source - 1, "generator", NULL, NULL, "main", "ps_4_0", 0, 0, &generator_code, NULL)) ||
		FAILED(compile(swap_source, sizeof swap_source - 1, "swap", NULL, NULL, "main", "ps_4_0", 0, 0, &swap_code, NULL)))
		return 2;
	HMODULE library = LoadLibraryExW(L"d2d1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	create_function create = library ? (create_function)GetProcAddress(library, "D2D1CreateFactory") : NULL;
	ID2D1Factory1 *factory = NULL;
	if (!create || FAILED(create(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory1, NULL, (void **)&factory)))
		return 3;
	IWICImagingFactory *imaging = NULL;
	IWICBitmap *pixels = NULL;
	if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&imaging)) ||
		FAILED(IWICImagingFactory_CreateBitmap(imaging, 16, 16, &GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &pixels)))
		return 4;
	D2D1_RENDER_TARGET_PROPERTIES properties = {D2D1_RENDER_TARGET_TYPE_DEFAULT, {DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN}, 0, 0,
		D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT};
	ID2D1RenderTarget *target = NULL;
	ID2D1DeviceContext *context = NULL;
	if (FAILED(ID2D1Factory_CreateWicBitmapRenderTarget((ID2D1Factory *)factory, pixels, &properties, &target)) ||
		FAILED(ID2D1RenderTarget_QueryInterface(target, &IID_ID2D1DeviceContext, (void **)&context)))
		return 5;
	static const WCHAR generator_xml[] = L"<?xml version='1.0'?><Effect><Property name='DisplayName' type='string' value='Generator'/>"
		L"<Inputs/></Effect>";
	static const WCHAR one_input_xml[] = L"<?xml version='1.0'?><Effect><Property name='DisplayName' type='string' value='One input'/>"
		L"<Inputs><Input name='Source'/></Inputs></Effect>";
	if (FAILED(ID2D1Factory1_RegisterEffectFromString(factory, &generator_effect, generator_xml, NULL, 0, make_generator)) ||
		FAILED(ID2D1Factory1_RegisterEffectFromString(factory, &swap_effect, one_input_xml, NULL, 0, make_swap)) ||
		FAILED(ID2D1Factory1_RegisterEffectFromString(factory, &chain_effect, one_input_xml, NULL, 0, make_chain)))
		return 6;
	ID2D1Effect *generator = NULL, *swap = NULL, *premultiply = NULL, *chain = NULL;
	if (FAILED(ID2D1DeviceContext_CreateEffect(context, &generator_effect, &generator)) ||
		FAILED(ID2D1DeviceContext_CreateEffect(context, &swap_effect, &swap)) ||
		FAILED(ID2D1DeviceContext_CreateEffect(context, &CLSID_D2D1Premultiply, &premultiply)) ||
		FAILED(ID2D1DeviceContext_CreateEffect(context, &chain_effect, &chain)))
		return 7;
	/* An opaque red bitmap for the swap, and a red of half coverage kept unpremultiplied, which the premultiply makes a dark red. */
	static const UINT32 red[4] = {0xffff0000, 0xffff0000, 0xffff0000, 0xffff0000};
	static const UINT32 straight[4] = {0x80ff0000, 0x80ff0000, 0x80ff0000, 0x80ff0000};
	D2D1_BITMAP_PROPERTIES bitmap_properties = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96};
	ID2D1Bitmap *opaque = NULL, *unpremultiplied = NULL;
	D2D1_SIZE_U size = {2, 2};
	if (FAILED(ID2D1RenderTarget_CreateBitmap(target, size, red, 8, &bitmap_properties, &opaque)) ||
		FAILED(ID2D1RenderTarget_CreateBitmap(target, size, straight, 8, &bitmap_properties, &unpremultiplied)))
		return 8;
	typedef void (STDMETHODCALLTYPE *set_input)(void *, UINT32, ID2D1Image *, BOOL);
	((set_input)(*(void ***)swap)[14])(swap, 0, (ID2D1Image *)opaque, TRUE);
	((set_input)(*(void ***)premultiply)[14])(premultiply, 0, (ID2D1Image *)unpremultiplied, TRUE);
	((set_input)(*(void ***)chain)[14])(chain, 0, (ID2D1Image *)unpremultiplied, TRUE);
	/* What each covers: the generator its four pixels, the swap its input's two. */
	D2D1_RECT_F bounds;
	ID2D1Image *output = output_of(generator);
	if (FAILED(ID2D1DeviceContext_GetImageLocalBounds(context, output, &bounds)) || bounds.right != 4 || bounds.bottom != 4)
		return 9;
	IUnknown_Release((IUnknown *)output);
	/* An output outlives the effect the program let go of, and still draws what the effect makes. */
	ID2D1Effect *orphan = NULL;
	if (FAILED(ID2D1DeviceContext_CreateEffect(context, &swap_effect, &orphan)))
		return 16;
	((set_input)(*(void ***)orphan)[14])(orphan, 0, (ID2D1Image *)opaque, TRUE);
	ID2D1Image *orphan_output = output_of(orphan);
	IUnknown_Release((IUnknown *)orphan);
	ID2D1RenderTarget_BeginDraw(target);
	D2D1_COLOR_F white = {1, 1, 1, 1};
	ID2D1RenderTarget_Clear(target, &white);
	const D2D1_POINT_2F places[4] = {{0, 0}, {8, 0}, {0, 8}, {8, 8}};
	ID2D1Effect *drawn[4] = {generator, swap, premultiply, chain};
	for (int index = 0; index < 4; ++index) {
		output = output_of(drawn[index]);
		ID2D1DeviceContext_DrawImage(context, output, &places[index], NULL, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
			D2D1_COMPOSITE_MODE_SOURCE_OVER);
		IUnknown_Release((IUnknown *)output);
	}
	const D2D1_POINT_2F orphan_place = {12, 4};
	ID2D1DeviceContext_DrawImage(context, orphan_output, &orphan_place, NULL, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
		D2D1_COMPOSITE_MODE_SOURCE_OVER);
	IUnknown_Release((IUnknown *)orphan_output);
	if (FAILED(ID2D1RenderTarget_EndDraw(target, NULL, NULL)))
		return 10;
	UINT32 result[16 * 16];
	WICRect whole = {0, 0, 16, 16};
	if (FAILED(IWICBitmap_CopyPixels(pixels, &whole, 64, sizeof result, (BYTE *)result)))
		return 11;
	/* The generator's two colors on each side of x = 2, nothing past its four pixels. */
	if (!pixel_is(result[1 * 16 + 1], 0, 255, 0) || !pixel_is(result[1 * 16 + 3], 0, 0, 255) || !pixel_is(result[1 * 16 + 5], 255, 255, 255) ||
		!pixel_is(result[5 * 16 + 1], 255, 255, 255))
		return 12;
	/* The swap turns its red input blue, over the input's two pixels only. */
	if (!pixel_is(result[1 * 16 + 9], 0, 0, 255) || !pixel_is(result[1 * 16 + 11], 255, 255, 255))
		return 13;
	/* Half a red over white is full red and half green and blue once premultiplied. */
	if (!pixel_is(result[9 * 16 + 1], 255, 127, 127))
		return 14;
	/* The chain premultiplies and then swaps: half a blue over white. */
	if (!pixel_is(result[9 * 16 + 9], 127, 127, 255))
		return 15;
	if (!pixel_is(result[5 * 16 + 13], 0, 0, 255))
		return 17;
	/* A target over a texture of half floats, as paint.net draws its canvas in linear light, holds what is drawn as those floats: a
	   clear to 0.5 leaves halves near 0x3800. */
	HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
	create_device_function create_device = d3d11 ? (create_device_function)GetProcAddress(d3d11, "D3D11CreateDevice") : NULL;
	ID3D11Device *device3d = NULL;
	ID3D11DeviceContext *immediate = NULL;
	IDXGIDevice *dxgi = NULL;
	if (!create_device || FAILED(create_device(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
		&device3d, NULL, &immediate)) || FAILED(ID3D11Device_QueryInterface(device3d, &IID_IDXGIDevice, (void **)&dxgi)))
		return 18;
	D3D11_TEXTURE2D_DESC texture_desc = {8, 8, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
		D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
	ID3D11Texture2D *texture = NULL, *staging = NULL;
	IDXGISurface *surface = NULL;
	ID2D1Device *device2d = NULL;
	ID2D1DeviceContext *surface_context = NULL;
	ID2D1Bitmap1 *surface_bitmap = NULL;
	D2D1_BITMAP_PROPERTIES1 surface_properties = {{DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96,
		D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, NULL};
	if (FAILED(ID3D11Device_CreateTexture2D(device3d, &texture_desc, NULL, &texture)) ||
		FAILED(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGISurface, (void **)&surface)) ||
		FAILED(ID2D1Factory1_CreateDevice(factory, dxgi, &device2d)) ||
		FAILED(ID2D1Device_CreateDeviceContext(device2d, D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &surface_context)) ||
		FAILED(ID2D1DeviceContext_CreateBitmapFromDxgiSurface(surface_context, surface, &surface_properties, &surface_bitmap)))
		return 19;
	ID2D1DeviceContext_SetTarget(surface_context, (ID2D1Image *)surface_bitmap);
	ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)surface_context);
	D2D1_COLOR_F half_grey = {0.5f, 0.5f, 0.5f, 1};
	ID2D1RenderTarget_Clear((ID2D1RenderTarget *)surface_context, &half_grey);
	if (FAILED(ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)surface_context, NULL, NULL)))
		return 20;
	texture_desc.Usage = D3D11_USAGE_STAGING;
	texture_desc.BindFlags = 0;
	texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(ID3D11Device_CreateTexture2D(device3d, &texture_desc, NULL, &staging)))
		return 21;
	ID3D11DeviceContext_CopyResource(immediate, (ID3D11Resource *)staging, (ID3D11Resource *)texture);
	if (FAILED(ID3D11DeviceContext_Map(immediate, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)))
		return 22;
	const UINT16 *halves = (const UINT16 *)((const BYTE *)mapped.pData + 4 * mapped.RowPitch) + 4 * 4;
	const BOOL grey_halves = halves[0] >= 0x37f0 && halves[0] <= 0x3810 && halves[1] >= 0x37f0 && halves[1] <= 0x3810 && halves[3] == 0x3c00;
	ID3D11DeviceContext_Unmap(immediate, (ID3D11Resource *)staging, 0);
	if (!grey_halves)
		return 23;
	/* A command list covers what was drawn into it, however far from the target it was recorded through: paint.net records the tiles
	   of its document into one and scales it to the window by those bounds. */
	ID2D1Image *previous_target = NULL;
	ID2D1CommandList *list = NULL;
	ID2D1SolidColorBrush *red_brush = NULL;
	D2D1_COLOR_F red_color = {1, 0, 0, 1};
	D2D1_RECT_F square = {2, 3, 6, 7};
	ID2D1DeviceContext_GetTarget(context, &previous_target);
	if (FAILED(ID2D1DeviceContext_CreateCommandList(context, &list)) ||
		FAILED(ID2D1RenderTarget_CreateSolidColorBrush((ID2D1RenderTarget *)context, &red_color, NULL, &red_brush)))
		return 24;
	/* Past the 16 by 16 target the list was recorded through, inside a clip as paint.net draws its tiles: the clip still holds where
	   the list grew, so the bitmap stops at it. The clip paint.net pushes on its window before it records stays with the window. */
	D2D1_RECT_F far_place = {20, 30, 50, 60}, document = {0, 0, 40, 40}, window_clip = {0, 0, 16, 16};
	ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)context);
	ID2D1RenderTarget_PushAxisAlignedClip((ID2D1RenderTarget *)context, &window_clip, D2D1_ANTIALIAS_MODE_ALIASED);
	ID2D1DeviceContext_SetTarget(context, (ID2D1Image *)list);
	ID2D1RenderTarget_PushAxisAlignedClip((ID2D1RenderTarget *)context, &document, D2D1_ANTIALIAS_MODE_ALIASED);
	ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget *)context, &square, (ID2D1Brush *)red_brush);
	ID2D1RenderTarget_DrawBitmap((ID2D1RenderTarget *)context, opaque, &far_place, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, NULL);
	ID2D1RenderTarget_PopAxisAlignedClip((ID2D1RenderTarget *)context);
	ID2D1DeviceContext_SetTarget(context, previous_target);
	ID2D1RenderTarget_PopAxisAlignedClip((ID2D1RenderTarget *)context);
	if (FAILED(ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)context, NULL, NULL)) || FAILED(((HRESULT (STDMETHODCALLTYPE *)(void *))(*(void ***)list)[5])(list)))
		return 24;
	if (FAILED(ID2D1DeviceContext_GetImageLocalBounds(context, (ID2D1Image *)list, &bounds)) || bounds.left != 2 || bounds.top != 3 ||
		bounds.right != 40 || bounds.bottom != 40)
		return 25;
	((void (STDMETHODCALLTYPE *)(void *, UINT32, ID2D1Image *, BOOL))(*(void ***)swap)[14])(swap, 0, (ID2D1Image *)list, TRUE);
	output = output_of(swap);
	if (FAILED(ID2D1DeviceContext_GetImageLocalBounds(context, output, &bounds)) || bounds.left != 2 || bounds.top != 3 || bounds.right != 40 ||
		bounds.bottom != 40)
		return 26;
	IUnknown_Release((IUnknown *)red_brush);
	IUnknown_Release((IUnknown *)list);
	if (previous_target)
		IUnknown_Release((IUnknown *)previous_target);
	IUnknown_Release((IUnknown *)surface_bitmap);
	IUnknown_Release((IUnknown *)surface_context);
	IUnknown_Release((IUnknown *)device2d);
	IUnknown_Release((IUnknown *)surface);
	ID3D11Texture2D_Release(staging);
	ID3D11Texture2D_Release(texture);
	IUnknown_Release((IUnknown *)dxgi);
	ID3D11DeviceContext_Release(immediate);
	ID3D11Device_Release(device3d);
	IUnknown_Release((IUnknown *)chain);
	IUnknown_Release((IUnknown *)premultiply);
	IUnknown_Release((IUnknown *)swap);
	IUnknown_Release((IUnknown *)generator);
	ID2D1Bitmap_Release(unpremultiplied);
	ID2D1Bitmap_Release(opaque);
	IUnknown_Release((IUnknown *)context);
	ID2D1RenderTarget_Release(target);
	IWICBitmap_Release(pixels);
	IWICImagingFactory_Release(imaging);
	ID2D1Factory1_Release(factory);
	return 0;
}
