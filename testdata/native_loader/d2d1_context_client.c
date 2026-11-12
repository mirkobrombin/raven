// A render target that draws into a WIC bitmap is a device context of every version the factory knows, and its device a device of the
// matching one, as paint.net asks the target it draws its buttons with for the newest of both. The exit code names the first check that
// failed.
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <d2d1_3.h>
#include <wincodec.h>

typedef HRESULT (WINAPI *create_function)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
// The headers declare ID2D1Device6 for C++ alone: its CreateDeviceContext is the method after the twenty of ID2D1Device5.
typedef HRESULT (STDMETHODCALLTYPE *make_context)(void *, D2D1_DEVICE_CONTEXT_OPTIONS, void **);

// ID2D1Properties has no C macros either: its property count is the method after IUnknown's, its type of a property the fourth.
static UINT32 property_count(void *properties) { return ((UINT32 (STDMETHODCALLTYPE *)(void *))(*(void ***)properties)[3])(properties); }
static D2D1_PROPERTY_TYPE property_type(void *properties, UINT32 index)
{
	return ((D2D1_PROPERTY_TYPE (STDMETHODCALLTYPE *)(void *, UINT32))(*(void ***)properties)[6])(properties, index);
}

// The object a registered effect's factory makes, an ID2D1EffectImpl: initialized with a context and a graph, it loads a shader and passes
// its input straight through, as an effect does that has nothing to change; it counts how often it is asked to prepare.
static int initialized, prepared;
static HRESULT STDMETHODCALLTYPE impl_QueryInterface(IUnknown *self, REFIID iid, void **out)
{
	*out = self;
	return S_OK;
}
static ULONG STDMETHODCALLTYPE impl_AddRef(IUnknown *self) { return 1; }
static ULONG STDMETHODCALLTYPE impl_Release(IUnknown *self) { return 1; }
static HRESULT STDMETHODCALLTYPE impl_Initialize(IUnknown *self, void *context, void *graph)
{
	static const GUID shader = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
	static const BYTE code[] = {'D', 'X', 'B', 'C'};
	typedef HRESULT (STDMETHODCALLTYPE *load_shader)(void *, REFGUID, const BYTE *, UINT32);
	typedef BOOL (STDMETHODCALLTYPE *shader_loaded)(void *, REFGUID);
	typedef UINT32 (STDMETHODCALLTYPE *input_count)(void *);
	typedef HRESULT (STDMETHODCALLTYPE *passthrough)(void *, UINT32);
	if (FAILED(((load_shader)(*(void ***)context)[11])(context, &shader, code, sizeof code)) || !((shader_loaded)(*(void ***)context)[14])(context, &shader) ||
		((input_count)(*(void ***)graph)[3])(graph) != 1)
		return E_FAIL;
	/* The newest context makes color contexts from a DXGI color space, as paint.net's color output effect asks for one. */
	static const GUID context2 = {0x577ad2a0, 0x9fc7, 0x4dda, {0x8b, 0x18, 0xda, 0xb8, 0x10, 0x14, 0x00, 0x52}};
	typedef HRESULT (STDMETHODCALLTYPE *from_dxgi)(void *, DXGI_COLOR_SPACE_TYPE, IUnknown **);
	IUnknown *newest = NULL, *color = NULL;
	if (FAILED(IUnknown_QueryInterface((IUnknown *)context, &context2, (void **)&newest)) ||
		FAILED(((from_dxgi)(*(void ***)newest)[25])(newest, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &color)) || !color)
		return E_FAIL;
	IUnknown_Release(color);
	IUnknown_Release(newest);
	++initialized;
	return ((passthrough)(*(void ***)graph)[11])(graph, 0);
}
static HRESULT STDMETHODCALLTYPE impl_PrepareForRender(IUnknown *self, UINT32 change)
{
	++prepared;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE impl_SetGraph(IUnknown *self, void *graph) { return E_NOTIMPL; }
static void *impl_table[] = {impl_QueryInterface, impl_AddRef, impl_Release, impl_Initialize, impl_PrepareForRender, impl_SetGraph};
static struct { void **table; } impl = {impl_table};
static HRESULT CALLBACK make_impl(IUnknown **out)
{
	*out = (IUnknown *)&impl;
	return S_OK;
}

int raven_entry(void)
{
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
		return 1;
	HMODULE library = LoadLibraryExW(L"d2d1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	create_function create = library ? (create_function)GetProcAddress(library, "D2D1CreateFactory") : NULL;
	if (!create)
		return 2;
	ID2D1Factory *factory = NULL;
	if (FAILED(create(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&factory)))
		return 3;
	IWICImagingFactory *imaging = NULL;
	IWICBitmap *pixels = NULL;
	if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&imaging)) ||
		FAILED(IWICImagingFactory_CreateBitmap(imaging, 16, 16, &GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &pixels)))
		return 4;
	D2D1_RENDER_TARGET_PROPERTIES properties = {D2D1_RENDER_TARGET_TYPE_DEFAULT, {DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN}, 0, 0,
		D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT};
	ID2D1RenderTarget *target = NULL;
	if (FAILED(ID2D1Factory_CreateWicBitmapRenderTarget(factory, pixels, &properties, &target)))
		return 5;
	ID2D1DeviceContext5 *context5 = NULL;
	if (FAILED(ID2D1RenderTarget_QueryInterface(target, &IID_ID2D1DeviceContext5, (void **)&context5)))
		return 6;
	ID2D1DeviceContext6 *context6 = NULL;
	if (FAILED(ID2D1RenderTarget_QueryInterface(target, &IID_ID2D1DeviceContext6, (void **)&context6)))
		return 7;
	// Both are the same object, and the newer one answers the older one's methods.
	ID2D1Factory *owner = NULL;
	ID2D1RenderTarget_GetFactory((ID2D1RenderTarget *)context6, &owner);
	if (owner != factory)
		return 8;
	ID2D1Factory_Release(owner);
	// Its device is of the matching version, and makes contexts of that version too.
	ID2D1Device *device = NULL;
	ID2D1Device6 *device6 = NULL;
	ID2D1DeviceContext_GetDevice((ID2D1DeviceContext *)context6, &device);
	if (!device || FAILED(IUnknown_QueryInterface((IUnknown *)device, &IID_ID2D1Device6, (void **)&device6)))
		return 9;
	void *made = NULL;
	if (FAILED((*(make_context **)device6)[20](device6, D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &made)) || !made)
		return 10;
	IUnknown_Release((IUnknown *)made);
	/* A color context for a standard space carries no profile; a custom one keeps the profile it was given, as paint.net's setup
	   makes one before drawing its banner. The headers give C no macros for these, so the methods are called by their slots. */
	typedef HRESULT (STDMETHODCALLTYPE *create_color)(void *, D2D1_COLOR_SPACE, const BYTE *, UINT32, void **);
	typedef D2D1_COLOR_SPACE (STDMETHODCALLTYPE *color_space)(void *);
	typedef UINT32 (STDMETHODCALLTYPE *profile_size)(void *);
	typedef HRESULT (STDMETHODCALLTYPE *get_profile)(void *, BYTE *, UINT32);
	const create_color create_context = (create_color)(*(void ***)context6)[59];
	void *color = NULL;
	if (FAILED(create_context(context6, D2D1_COLOR_SPACE_SRGB, NULL, 0, &color)) || !color ||
		((color_space)(*(void ***)color)[4])(color) != D2D1_COLOR_SPACE_SRGB || ((profile_size)(*(void ***)color)[5])(color) != 0)
		return 11;
	IUnknown_Release((IUnknown *)color);
	static const BYTE profile[] = {1, 2, 3, 4, 5};
	BYTE copy[5] = {0};
	if (FAILED(create_context(context6, D2D1_COLOR_SPACE_CUSTOM, profile, sizeof profile, &color)) ||
		((profile_size)(*(void ***)color)[5])(color) != sizeof profile || ((get_profile)(*(void ***)color)[6])(color, copy, 2) == S_OK ||
		FAILED(((get_profile)(*(void ***)color)[6])(color, copy, sizeof copy)))
		return 12;
	for (int index = 0; index < 5; ++index)
		if (copy[index] != profile[index])
			return 12;
	/* A context from a WIC one: the sRGB of a photo's EXIF data is that standard space, a profile read from memory a custom space with
	   those bytes, as paint.net makes them for the images it opens and for the display. */
	typedef HRESULT (STDMETHODCALLTYPE *from_wic)(void *, IWICColorContext *, void **);
	IWICColorContext *wic = NULL;
	void *converted = NULL;
	if (FAILED(IWICImagingFactory_CreateColorContext(imaging, &wic)) || FAILED(IWICColorContext_InitializeFromExifColorSpace(wic, 1)) ||
		FAILED(((from_wic)(*(void ***)context6)[61])(context6, wic, &converted)) ||
		((color_space)(*(void ***)converted)[4])(converted) != D2D1_COLOR_SPACE_SRGB)
		return 30;
	IUnknown_Release((IUnknown *)converted);
	IWICColorContext_Release(wic);
	if (FAILED(IWICImagingFactory_CreateColorContext(imaging, &wic)) || FAILED(IWICColorContext_InitializeFromMemory(wic, profile, sizeof profile)) ||
		FAILED(((from_wic)(*(void ***)context6)[61])(context6, wic, &converted)) ||
		((color_space)(*(void ***)converted)[4])(converted) != D2D1_COLOR_SPACE_CUSTOM ||
		((profile_size)(*(void ***)converted)[5])(converted) != sizeof profile)
		return 31;
	IUnknown_Release((IUnknown *)converted);
	IWICColorContext_Release(wic);
	/* The Adobe RGB that EXIF data can also name is a custom space whose profile, a valid ICC one, Direct2D supplies itself. */
	static BYTE adobe[2048];
	UINT32 adobe_size = 0;
	if (FAILED(IWICImagingFactory_CreateColorContext(imaging, &wic)) || FAILED(IWICColorContext_InitializeFromExifColorSpace(wic, 2)) ||
		FAILED(((from_wic)(*(void ***)context6)[61])(context6, wic, &converted)) ||
		((color_space)(*(void ***)converted)[4])(converted) != D2D1_COLOR_SPACE_CUSTOM ||
		(adobe_size = ((profile_size)(*(void ***)converted)[5])(converted)) <= 132 || adobe_size > sizeof adobe ||
		FAILED(((get_profile)(*(void ***)converted)[6])(converted, adobe, adobe_size)) || adobe[3] != (BYTE)adobe_size ||
		adobe[12] != 'm' || adobe[16] != 'R' || adobe[20] != 'X' || adobe[36] != 'a' || adobe[39] != 'p')
		return 32;
	IUnknown_Release((IUnknown *)converted);
	IWICColorContext_Release(wic);
	/* Its newer interface says what kind of context it is; one made from a DXGI color space names that space. */
	typedef UINT32 (STDMETHODCALLTYPE *context_type)(void *);
	void *color1 = NULL;
	if (FAILED(IUnknown_QueryInterface((IUnknown *)color, &IID_ID2D1ColorContext1, &color1)) ||
		((context_type)(*(void ***)color1)[7])(color1) != D2D1_COLOR_CONTEXT_TYPE_ICC)
		return 14;
	IUnknown_Release((IUnknown *)color1);
	typedef HRESULT (STDMETHODCALLTYPE *create_dxgi)(void *, DXGI_COLOR_SPACE_TYPE, void **);
	if (FAILED(((create_dxgi)(*(void ***)context6)[117])(context6, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &color1)) ||
		((context_type)(*(void ***)color1)[7])(color1) != D2D1_COLOR_CONTEXT_TYPE_DXGI ||
		((context_type)(*(void ***)color1)[8])(color1) != DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ||
		((color_space)(*(void ***)color1)[4])(color1) != D2D1_COLOR_SPACE_SCRGB)
		return 15;
	IUnknown_Release((IUnknown *)color1);
	IUnknown_Release((IUnknown *)color);
	if (create_context(context6, D2D1_COLOR_SPACE_CUSTOM, NULL, 0, &color) != E_INVALIDARG)
		return 13;
	/* An effect nobody registered is refused, which is when paint.net registers its own; the system's effects are there already. */
	static const CLSID unknown = {0xafd3d302, 0x88ae, 0x3e80, {0xab, 0x6f, 0x5f, 0x77, 0xf0, 0x18, 0x06, 0x40}};
	ID2D1Effect *effect = NULL;
	if (ID2D1DeviceContext_CreateEffect((ID2D1DeviceContext *)context6, &unknown, &effect) != D2DERR_EFFECT_IS_NOT_REGISTERED || effect)
		return 16;
	if (FAILED(ID2D1DeviceContext_CreateEffect((ID2D1DeviceContext *)context6, &CLSID_D2D1GaussianBlur, &effect)) || !effect)
		return 17;
	IUnknown_Release((IUnknown *)effect);
	/* Once registered, the effect is described by its XML, each property with the type the XML gives it, and can be made. */
	ID2D1Factory1 *factory1 = NULL;
	ID2D1Properties *described = NULL;
	if (FAILED(ID2D1Factory_QueryInterface(factory, &IID_ID2D1Factory1, (void **)&factory1)) ||
		ID2D1Factory1_GetEffectProperties(factory1, &unknown, &described) != D2DERR_EFFECT_IS_NOT_REGISTERED)
		return 18;
	static const WCHAR xml[] = L"<?xml version='1.0'?><Effect><Property name='DisplayName' type='string' value='Test'/>"
		L"<Inputs><Input name='Source'/></Inputs><Property name='Space' type='enum'><Property name='Default' type='enum' value='0'/></Property>"
		L"<Property name='Context' type='colorcontext'/></Effect>";
	if (FAILED(ID2D1Factory1_RegisterEffectFromString(factory1, &unknown, xml, NULL, 0, make_impl)) ||
		FAILED(ID2D1Factory1_GetEffectProperties(factory1, &unknown, &described)) || property_count(described) != 2 ||
		property_type(described, 0) != D2D1_PROPERTY_TYPE_ENUM || property_type(described, 1) != D2D1_PROPERTY_TYPE_COLOR_CONTEXT)
		return 19;
	/* Past its own properties each effect has those that describe it: the registered one the display name its XML gives and its class,
	   a system effect the category it is listed under, which paint.net reads to group its own effects with the system's. */
	typedef UINT32 (STDMETHODCALLTYPE *value_size)(void *, UINT32);
	typedef HRESULT (STDMETHODCALLTYPE *get_value)(void *, UINT32, D2D1_PROPERTY_TYPE, BYTE *, UINT32);
	WCHAR text[64] = {0};
	CLSID named = {0};
	if (property_type(described, D2D1_PROPERTY_DISPLAYNAME) != D2D1_PROPERTY_TYPE_STRING ||
		((value_size)(*(void ***)described)[12])(described, D2D1_PROPERTY_DISPLAYNAME) != sizeof L"Test" ||
		FAILED(((get_value)(*(void ***)described)[11])(described, D2D1_PROPERTY_DISPLAYNAME, D2D1_PROPERTY_TYPE_STRING, (BYTE *)text, sizeof text)) ||
		lstrcmpW(text, L"Test") || FAILED(((get_value)(*(void ***)described)[11])(described, D2D1_PROPERTY_CLSID, D2D1_PROPERTY_TYPE_CLSID,
		(BYTE *)&named, sizeof named)) || ((UINT64 *)&named)[0] != ((const UINT64 *)&unknown)[0] ||
		((UINT64 *)&named)[1] != ((const UINT64 *)&unknown)[1])
		return 25;
	IUnknown_Release((IUnknown *)described);
	text[0] = 0;
	if (FAILED(ID2D1Factory1_GetEffectProperties(factory1, &CLSID_D2D1ColorMatrix, &described)) ||
		((value_size)(*(void ***)described)[12])(described, D2D1_PROPERTY_CATEGORY) <= sizeof(WCHAR) ||
		FAILED(((get_value)(*(void ***)described)[11])(described, D2D1_PROPERTY_CATEGORY, D2D1_PROPERTY_TYPE_STRING, (BYTE *)text, sizeof text)) || !text[0])
		return 26;
	IUnknown_Release((IUnknown *)described);
	/* A system effect's properties have their types: color management converts from the color context set as its source, which it
	   holds and gives back as another reference, as paint.net reads it back when it wraps the effect. The source context is its first
	   property and the quality its sixth. */
	typedef HRESULT (STDMETHODCALLTYPE *set_value)(void *, UINT32, D2D1_PROPERTY_TYPE, const BYTE *, UINT32);
	IUnknown *kept = NULL;
	if (FAILED(ID2D1DeviceContext_CreateEffect((ID2D1DeviceContext *)context6, &CLSID_D2D1ColorManagement, &effect)) ||
		property_count(effect) != 6 || property_type(effect, 0) != D2D1_PROPERTY_TYPE_COLOR_CONTEXT ||
		property_type(effect, 5) != D2D1_PROPERTY_TYPE_ENUM ||
		FAILED(((get_value)(*(void ***)effect)[11])(effect, 0, D2D1_PROPERTY_TYPE_COLOR_CONTEXT, (BYTE *)&kept, sizeof kept)) || kept)
		return 27;
	if (FAILED(create_context(context6, D2D1_COLOR_SPACE_SRGB, NULL, 0, &color)) ||
		FAILED(((set_value)(*(void ***)effect)[9])(effect, 0, D2D1_PROPERTY_TYPE_COLOR_CONTEXT, (const BYTE *)&color, sizeof color)) ||
		FAILED(((get_value)(*(void ***)effect)[11])(effect, 0, D2D1_PROPERTY_TYPE_COLOR_CONTEXT, (BYTE *)&kept, sizeof kept)) ||
		kept != (IUnknown *)color || IUnknown_Release(kept) != 2)
		return 28;
	IUnknown_Release((IUnknown *)effect);
	if (IUnknown_Release((IUnknown *)color) != 0)
		return 29;
	if (FAILED(ID2D1DeviceContext_CreateEffect((ID2D1DeviceContext *)context6, &unknown, &effect)) ||
		property_type(effect, 1) != D2D1_PROPERTY_TYPE_COLOR_CONTEXT || initialized != 1)
		return 20;
	/* Drawn, it is first asked to prepare, and then its graph passes the red bitmap given as its input onto the target. */
	static const UINT32 red[4] = {0xffff0000, 0xffff0000, 0xffff0000, 0xffff0000};
	D2D1_BITMAP_PROPERTIES bitmap_properties = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96};
	ID2D1Bitmap *source = NULL;
	D2D1_SIZE_U size = {2, 2};
	if (FAILED(ID2D1RenderTarget_CreateBitmap(target, size, red, 8, &bitmap_properties, &source)))
		return 21;
	typedef void (STDMETHODCALLTYPE *set_input)(void *, UINT32, ID2D1Image *, BOOL);
	typedef void (STDMETHODCALLTYPE *get_output)(void *, ID2D1Image **);
	ID2D1Image *output = NULL;
	((set_input)(*(void ***)effect)[14])(effect, 0, (ID2D1Image *)source, TRUE);
	((get_output)(*(void ***)effect)[18])(effect, &output);
	ID2D1RenderTarget_BeginDraw(target);
	D2D1_COLOR_F white = {1, 1, 1, 1};
	ID2D1RenderTarget_Clear(target, &white);
	D2D1_POINT_2F at = {4, 4};
	ID2D1DeviceContext_DrawImage((ID2D1DeviceContext *)context6, output, &at, NULL, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
	if (FAILED(ID2D1RenderTarget_EndDraw(target, NULL, NULL)))
		return 22;
	UINT32 drawn[16 * 16];
	WICRect whole = {0, 0, 16, 16};
	if (FAILED(IWICBitmap_CopyPixels(pixels, &whole, 64, sizeof drawn, (BYTE *)drawn)) || prepared < 1)
		return 23;
	if ((drawn[5 * 16 + 5] & 0xffffff) != 0xff0000 || (drawn[0] & 0xffffff) != 0xffffff)
		return 24;
	IUnknown_Release((IUnknown *)output);
	IUnknown_Release((IUnknown *)source);
	IUnknown_Release((IUnknown *)effect);
	ID2D1Factory1_Release(factory1);
	IUnknown_Release((IUnknown *)device6);
	IUnknown_Release((IUnknown *)device);
	IUnknown_Release((IUnknown *)context6);
	IUnknown_Release((IUnknown *)context5);
	ID2D1RenderTarget_Release(target);
	IWICBitmap_Release(pixels);
	IWICImagingFactory_Release(imaging);
	ID2D1Factory_Release(factory);
	return 0;
}
