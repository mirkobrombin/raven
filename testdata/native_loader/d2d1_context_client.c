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

// The object a registered effect's factory makes; nothing here asks more of it than to be an object.
static HRESULT STDMETHODCALLTYPE impl_QueryInterface(IUnknown *self, REFIID iid, void **out)
{
	*out = self;
	return S_OK;
}
static ULONG STDMETHODCALLTYPE impl_AddRef(IUnknown *self) { return 1; }
static ULONG STDMETHODCALLTYPE impl_Release(IUnknown *self) { return 1; }
static IUnknownVtbl impl_table = {impl_QueryInterface, impl_AddRef, impl_Release};
static IUnknown impl = {&impl_table};
static HRESULT CALLBACK make_impl(IUnknown **out)
{
	*out = &impl;
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
	IUnknown_Release((IUnknown *)described);
	if (FAILED(ID2D1DeviceContext_CreateEffect((ID2D1DeviceContext *)context6, &unknown, &effect)) ||
		property_type(effect, 1) != D2D1_PROPERTY_TYPE_COLOR_CONTEXT)
		return 20;
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
