#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <winstring.h>
#include <activation.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.ui.composition.h>
#include <windows.ui.composition.interop.h>
#include "../../native/guest/raven-composition/composition_interfaces.h"

typedef HRESULT (WINAPI *initialize_function)(int);
typedef HRESULT (WINAPI *activate_function)(HSTRING, IInspectable **);
typedef HRESULT (WINAPI *create_device_function)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
	ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

/* The string header programs built with C++/WinRT write themselves for a reference: flags, length, two unused words, the characters. */
struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

typedef struct ICompositorDesktopInteropVtbl desktop_table;
typedef struct {
	const desktop_table *lpVtbl;
} desktop_object;

static void pump(DWORD milliseconds)
{
	const DWORD started = GetTickCount();
	MSG message;
	while (GetTickCount() - started < milliseconds) {
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		Sleep(5);
	}
}

static BOOL close_to(int value, int wanted)
{
	return value - wanted <= 3 && wanted - value <= 3;
}

static BOOL matches(COLORREF value, BYTE red, BYTE green, BYTE blue)
{
	return close_to(GetRValue(value), red) && close_to(GetGValue(value), green) && close_to(GetBValue(value), blue);
}

/* A swap chain of half floats made for composition holds scRGB, linear light, as paint.net draws its canvas into one: shown in a sprite
 * visual, a linear 0.214 is the middle grey of sRGB on the screen, and each frame presented after shows without a commit. The exit code
 * names the first check that failed. */
int raven_entry(void)
{
	static const WCHAR compositor_name[] = L"Windows.UI.Composition.Compositor";
	HMODULE runtime = LoadLibraryW(L"combase.dll");
	initialize_function RoInitialize = runtime ? (initialize_function)GetProcAddress(runtime, "RoInitialize") : NULL;
	activate_function RoActivateInstance = runtime ? (activate_function)GetProcAddress(runtime, "RoActivateInstance") : NULL;
	if (!RoInitialize || !RoActivateInstance || FAILED(RoInitialize(0))) return 1;
	struct reference class_name = {1, ARRAYSIZE(compositor_name) - 1, {0, 0}, compositor_name};
	IInspectable *object = NULL;
	__x_ABI_CWindows_CUI_CComposition_CICompositor *compositor = NULL;
	__x_ABI_CWindows_CUI_CComposition_CICompositorInterop *interop = NULL;
	desktop_object *desktop = NULL;
	if (RoActivateInstance((HSTRING)&class_name, &object) != S_OK ||
		FAILED(IInspectable_QueryInterface(object, &IID___x_ABI_CWindows_CUI_CComposition_CICompositor, (void **)&compositor)) ||
		FAILED(IInspectable_QueryInterface(object, &IID___x_ABI_CWindows_CUI_CComposition_CICompositorInterop, (void **)&interop)) ||
		FAILED(IInspectable_QueryInterface(object, &IID_ICompositorDesktopInterop, (void **)&desktop)))
		return 2;

	HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
	create_device_function create = d3d11 ? (create_device_function)GetProcAddress(d3d11, "D3D11CreateDevice") : NULL;
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	IDXGIDevice *dxgi = NULL;
	IDXGIAdapter *adapter = NULL;
	IDXGIFactory2 *factory = NULL;
	if (!create || FAILED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &device, NULL,
		&context)))
		return 3;
	if (FAILED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi)) || FAILED(IDXGIDevice_GetAdapter(dxgi, &adapter)) ||
		FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory)))
		return 4;
	DXGI_SWAP_CHAIN_DESC1 desc = {16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 3, DXGI_SCALING_STRETCH,
		DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_ALPHA_MODE_PREMULTIPLIED, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT};
	IDXGISwapChain1 *swap_chain = NULL;
	if (FAILED(IDXGIFactory2_CreateSwapChainForComposition(factory, (IUnknown *)device, &desc, NULL, &swap_chain)))
		return 5;
	/* Every pixel the linear light of sRGB's middle grey. */
	ID3D11Texture2D *buffer = NULL;
	ID3D11RenderTargetView *view = NULL;
	if (FAILED(IDXGISwapChain1_GetBuffer(swap_chain, 0, &IID_ID3D11Texture2D, (void **)&buffer)) ||
		FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)buffer, NULL, &view)))
		return 6;
	static const FLOAT grey[4] = {0.214041f, 0.214041f, 0.214041f, 1};
	ID3D11DeviceContext_ClearRenderTargetView(context, view, grey);
	if (FAILED(IDXGISwapChain1_Present(swap_chain, 0, 0)))
		return 7;

	WNDCLASSW type = {0};
	type.lpfnWndProc = DefWindowProcW;
	type.hInstance = GetModuleHandleW(NULL);
	type.hbrBackground = GetStockObject(WHITE_BRUSH);
	type.lpszClassName = L"CompositionSwapChain";
	if (!RegisterClassW(&type)) return 8;
	HWND window = CreateWindowExW(0, type.lpszClassName, L"swap chain", WS_POPUP | WS_VISIBLE, 40, 40, 48, 48, NULL, NULL, NULL, NULL);
	if (!window) return 9;
	UpdateWindow(window);
	IInspectable *desktop_target = NULL;
	__x_ABI_CWindows_CUI_CComposition_CICompositionTarget *target = NULL;
	if (FAILED(desktop->lpVtbl->CreateDesktopWindowTarget(desktop, window, FALSE, (void **)&desktop_target)) ||
		FAILED(IInspectable_QueryInterface(desktop_target, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionTarget, (void **)&target)))
		return 10;
	__x_ABI_CWindows_CUI_CComposition_CICompositionSurface *surface = NULL;
	__x_ABI_CWindows_CUI_CComposition_CICompositionSurfaceBrush *surface_brush = NULL;
	__x_ABI_CWindows_CUI_CComposition_CICompositionBrush *brush = NULL;
	__x_ABI_CWindows_CUI_CComposition_CISpriteVisual *sprite = NULL;
	__x_ABI_CWindows_CUI_CComposition_CIVisual *visual = NULL;
	if (FAILED(interop->lpVtbl->CreateCompositionSurfaceForSwapChain(interop, (IUnknown *)swap_chain, &surface)) ||
		FAILED(compositor->lpVtbl->CreateSurfaceBrushWithSurface(compositor, surface, &surface_brush)) ||
		FAILED(surface_brush->lpVtbl->QueryInterface(surface_brush, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionBrush, (void **)&brush)))
		return 11;
	if (FAILED(compositor->lpVtbl->CreateSpriteVisual(compositor, &sprite)) || FAILED(sprite->lpVtbl->put_Brush(sprite, brush)) ||
		FAILED(sprite->lpVtbl->QueryInterface(sprite, &IID___x_ABI_CWindows_CUI_CComposition_CIVisual, (void **)&visual)))
		return 12;
	__x_ABI_CWindows_CFoundation_CNumerics_CVector2 size = {16, 16};
	__x_ABI_CWindows_CFoundation_CNumerics_CVector3 offset = {8, 8, 0};
	if (FAILED(visual->lpVtbl->put_Size(visual, size)) || FAILED(visual->lpVtbl->put_Offset(visual, offset)) ||
		FAILED(target->lpVtbl->put_Root(target, visual)))
		return 13;
	/* A compositor of its own commits by itself, once the thread is back at its messages. */
	pump(300);
	HDC dc = GetDC(window);
	const COLORREF shown = GetPixel(dc, 12, 12), outside = GetPixel(dc, 30, 30);
	ReleaseDC(window, dc);
	if (!matches(outside, 255, 255, 255)) return 14;
	if (!matches(shown, 128, 128, 128)) return 15;
	/* Frames presented later show by themselves, with nothing committed, as the desktop compositor takes each frame a swap chain
	   presents with the area it changed, whichever of its three buffers the Direct3D libraries keep it in. */
	static const FLOAT black[4] = {0, 0, 0, 1};
	ID3D11DeviceContext_ClearRenderTargetView(context, view, black);
	if (FAILED(IDXGISwapChain1_Present(swap_chain, 0, 0)))
		return 16;
	pump(300);
	dc = GetDC(window);
	const COLORREF later = GetPixel(dc, 12, 12);
	ReleaseDC(window, dc);
	if (!matches(later, 0, 0, 0)) return 17;
	static const FLOAT white[4] = {1, 1, 1, 1};
	for (UINT frame = 0; frame < 3; ++frame) {
		const FLOAT *color = frame % 2 ? black : white;
		ID3D11DeviceContext_ClearRenderTargetView(context, view, color);
		RECT dirty = {0, 0, 16, 16};
		DXGI_PRESENT_PARAMETERS parameters = {1, &dirty, NULL, NULL};
		if (FAILED(IDXGISwapChain1_Present1(swap_chain, 1, 0, &parameters)))
			return 18;
		pump(200);
		dc = GetDC(window);
		const COLORREF now = GetPixel(dc, 12, 12);
		ReleaseDC(window, dc);
		if (!matches(now, frame % 2 ? 0 : 255, frame % 2 ? 0 : 255, frame % 2 ? 0 : 255)) return 19;
	}

	visual->lpVtbl->Release(visual);
	sprite->lpVtbl->Release(sprite);
	brush->lpVtbl->Release(brush);
	surface_brush->lpVtbl->Release(surface_brush);
	surface->lpVtbl->Release(surface);
	target->lpVtbl->Release(target);
	IInspectable_Release(desktop_target);
	DestroyWindow(window);
	ID3D11RenderTargetView_Release(view);
	ID3D11Texture2D_Release(buffer);
	IDXGISwapChain1_Release(swap_chain);
	IDXGIFactory2_Release(factory);
	IDXGIAdapter_Release(adapter);
	IDXGIDevice_Release(dxgi);
	ID3D11DeviceContext_Release(context);
	ID3D11Device_Release(device);
	desktop->lpVtbl->Release(desktop);
	interop->lpVtbl->Release(interop);
	compositor->lpVtbl->Release(compositor);
	IInspectable_Release(object);
	return 0;
}
