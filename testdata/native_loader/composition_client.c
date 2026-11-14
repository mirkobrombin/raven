#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <winstring.h>
#include <activation.h>
#include <windows.ui.composition.h>
#include "../../native/guest/raven-composition/composition_interfaces.h"

typedef HRESULT (WINAPI *initialize_function)(int);
typedef HRESULT (WINAPI *activate_function)(HSTRING, IInspectable **);
typedef DWORD (WINAPI *clock_function)(UINT, const HANDLE *, DWORD);

/* The string header programs built with C++/WinRT write themselves for a reference: flags, length, two unused words, the characters. */
struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

typedef struct ICompositorControllerVtbl controller_table;
typedef struct {
	const controller_table *lpVtbl;
} controller_object;
typedef struct ICompositorDesktopInteropVtbl desktop_table;
typedef struct {
	const desktop_table *lpVtbl;
} desktop_object;

/* The CommitNeeded handler: an event handler of the controller and an object, which counts the times it is raised. */
static LONG raised;
static HRESULT STDMETHODCALLTYPE handler_query(IUnknown *self, REFIID iid, void **out)
{
	*out = self;
	return S_OK;
}
static ULONG STDMETHODCALLTYPE handler_reference(IUnknown *self) { return 2; }
static HRESULT STDMETHODCALLTYPE handler_invoke(IUnknown *self, void *sender, void *argument)
{
	InterlockedIncrement(&raised);
	return S_OK;
}
static void *handler_table[] = {handler_query, handler_reference, handler_reference, handler_invoke};
static struct {
	void **table;
} handler = {handler_table};

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
	return value - wanted <= 2 && wanted - value <= 2;
}

static BOOL matches(COLORREF value, BYTE red, BYTE green, BYTE blue)
{
	return close_to(GetRValue(value), red) && close_to(GetGValue(value), green) && close_to(GetBValue(value), blue);
}

static BOOL contains(const char *text, const char *part)
{
	for (; *text; ++text) {
		const char *at = text, *wanted = part;
		while (*wanted && *at == *wanted)
			++at, ++wanted;
		if (!*wanted)
			return TRUE;
	}
	return FALSE;
}

static IInspectable *activate(activate_function RoActivateInstance, const WCHAR *name, UINT32 length)
{
	struct reference class_name = {1, length, {0, 0}, name};
	IInspectable *object = NULL;
	return RoActivateInstance((HSTRING)&class_name, &object) == S_OK ? object : NULL;
}

/* The visual layer of the Windows Runtime as paint.net uses it in a desktop window: a compositor controller announces changes through
 * CommitNeeded, a desktop window target shows a tree of sprite visuals filled with colours, faded and trimmed by an inset clip, and
 * a commit draws them over what the window painted. The compositor clock ticks with the frames. */
int raven_entry(void)
{
	static const WCHAR controller_name[] = L"Windows.UI.Composition.Core.CompositorController";
	static const WCHAR compositor_name[] = L"Windows.UI.Composition.Compositor";
	HMODULE runtime = LoadLibraryW(L"combase.dll");
	initialize_function RoInitialize = runtime ? (initialize_function)GetProcAddress(runtime, "RoInitialize") : NULL;
	activate_function RoActivateInstance = runtime ? (activate_function)GetProcAddress(runtime, "RoActivateInstance") : NULL;
	if (!RoInitialize || !RoActivateInstance) return 30;
	if (FAILED(RoInitialize(0))) return 1;
	IInspectable *object = activate(RoActivateInstance, controller_name, ARRAYSIZE(controller_name) - 1);
	if (!object) return 2;
	controller_object *controller = NULL;
	if (FAILED(IInspectable_QueryInterface(object, &IID_ICompositorController, (void **)&controller))) return 3;
	EventRegistrationToken token = {0};
	if (FAILED(controller->lpVtbl->add_CommitNeeded(controller, &handler, &token)) || !token.value) return 4;
	__x_ABI_CWindows_CUI_CComposition_CICompositor *compositor = NULL;
	if (FAILED(controller->lpVtbl->get_Compositor(controller, (void **)&compositor)) || !compositor) return 5;
	desktop_object *desktop = NULL;
	if (FAILED(compositor->lpVtbl->QueryInterface(compositor, &IID_ICompositorDesktopInterop, (void **)&desktop))) return 6;

	WNDCLASSW type = {0};
	type.lpfnWndProc = DefWindowProcW;
	type.hInstance = GetModuleHandleW(NULL);
	type.hbrBackground = GetStockObject(WHITE_BRUSH);
	type.lpszClassName = L"CompositionClient";
	if (!RegisterClassW(&type)) return 7;
	HWND window = CreateWindowExW(0, type.lpszClassName, L"composition", WS_POPUP | WS_VISIBLE, 40, 40, 64, 64, NULL, NULL, NULL, NULL);
	if (!window) return 8;
	UpdateWindow(window);
	__x_ABI_CWindows_CUI_CComposition_CICompositionTarget *target = NULL;
	IInspectable *desktop_target = NULL;
	if (FAILED(desktop->lpVtbl->CreateDesktopWindowTarget(desktop, window, FALSE, (void **)&desktop_target)) || !desktop_target) return 9;
	if (SUCCEEDED(desktop->lpVtbl->CreateDesktopWindowTarget(desktop, window, FALSE, (void **)&object))) return 10;
	if (FAILED(IInspectable_QueryInterface(desktop_target, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionTarget, (void **)&target)))
		return 11;

	/* A container root holding a red square at (8, 8), a blue one at half opacity at (32, 32), and a green one trimmed by an inset clip
	 * of four pixels on each side at (8, 40). */
	__x_ABI_CWindows_CUI_CComposition_CIContainerVisual *root = NULL;
	__x_ABI_CWindows_CUI_CComposition_CIVisual *root_visual = NULL;
	__x_ABI_CWindows_CUI_CComposition_CIVisualCollection *children = NULL;
	if (FAILED(compositor->lpVtbl->CreateContainerVisual(compositor, &root))) return 12;
	if (FAILED(root->lpVtbl->QueryInterface(root, &IID___x_ABI_CWindows_CUI_CComposition_CIVisual, (void **)&root_visual))) return 13;
	if (FAILED(root->lpVtbl->get_Children(root, &children))) return 14;
	const struct {
		__x_ABI_CWindows_CUI_CColor color;
		float x, y, opacity, inset;
	} squares[] = {{{255, 255, 0, 0}, 8, 8, 1, 0}, {{255, 0, 0, 255}, 32, 32, 0.5f, 0}, {{255, 0, 255, 0}, 8, 40, 1, 4}};
	for (UINT index = 0; index < ARRAYSIZE(squares); ++index) {
		__x_ABI_CWindows_CUI_CComposition_CISpriteVisual *sprite = NULL;
		__x_ABI_CWindows_CUI_CComposition_CIVisual *visual = NULL;
		__x_ABI_CWindows_CUI_CComposition_CICompositionColorBrush *color = NULL;
		__x_ABI_CWindows_CUI_CComposition_CICompositionBrush *brush = NULL;
		if (FAILED(compositor->lpVtbl->CreateSpriteVisual(compositor, &sprite))) return 15;
		if (FAILED(compositor->lpVtbl->CreateColorBrushWithColor(compositor, squares[index].color, &color))) return 16;
		if (FAILED(color->lpVtbl->QueryInterface(color, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionBrush, (void **)&brush))) return 17;
		if (FAILED(sprite->lpVtbl->put_Brush(sprite, brush))) return 18;
		if (FAILED(sprite->lpVtbl->QueryInterface(sprite, &IID___x_ABI_CWindows_CUI_CComposition_CIVisual, (void **)&visual))) return 19;
		__x_ABI_CWindows_CFoundation_CNumerics_CVector2 size = {16, 16};
		__x_ABI_CWindows_CFoundation_CNumerics_CVector3 offset = {squares[index].x, squares[index].y, 0};
		if (FAILED(visual->lpVtbl->put_Size(visual, size)) || FAILED(visual->lpVtbl->put_Offset(visual, offset)) ||
			FAILED(visual->lpVtbl->put_Opacity(visual, squares[index].opacity)))
			return 20;
		if (squares[index].inset) {
			__x_ABI_CWindows_CUI_CComposition_CIInsetClip *inset = NULL;
			__x_ABI_CWindows_CUI_CComposition_CICompositionClip *clip = NULL;
			const float by = squares[index].inset;
			if (FAILED(compositor->lpVtbl->CreateInsetClipWithInsets(compositor, by, by, by, by, &inset))) return 21;
			if (FAILED(inset->lpVtbl->QueryInterface(inset, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionClip, (void **)&clip))) return 22;
			if (FAILED(visual->lpVtbl->put_Clip(visual, clip))) return 23;
			clip->lpVtbl->Release(clip);
			inset->lpVtbl->Release(inset);
		}
		if (FAILED(children->lpVtbl->InsertAtTop(children, visual))) return 24;
		/* A visual has one parent. */
		if (SUCCEEDED(children->lpVtbl->InsertAtTop(children, visual))) return 25;
		visual->lpVtbl->Release(visual);
		brush->lpVtbl->Release(brush);
		color->lpVtbl->Release(color);
		sprite->lpVtbl->Release(sprite);
	}
	INT32 count = 0;
	if (FAILED(children->lpVtbl->get_Count(children, &count)) || count != 3) return 26;
	if (FAILED(target->lpVtbl->put_Root(target, root_visual))) return 27;

	/* The changes ask for a commit once the thread is back at its messages; nothing is drawn until the program commits. */
	pump(200);
	if (raised != 1) return 28;
	HDC dc = GetDC(window);
	if (!matches(GetPixel(dc, 12, 12), 255, 255, 255)) return 29;
	if (FAILED(controller->lpVtbl->Commit(controller))) return 31;
	const COLORREF red = GetPixel(dc, 12, 12), blue = GetPixel(dc, 36, 36), outside = GetPixel(dc, 2, 2);
	const COLORREF trimmed = GetPixel(dc, 9, 41), kept = GetPixel(dc, 16, 48);
	if (!matches(red, 255, 0, 0)) return 32;
	if (!matches(blue, 127, 127, 255)) return 33;
	if (!matches(outside, 255, 255, 255)) return 34;
	if (!matches(trimmed, 255, 255, 255) || !matches(kept, 0, 255, 0)) return 35;
	/* What the window paints itself goes under the visuals: they are drawn over it again. */
	InvalidateRect(window, NULL, TRUE);
	UpdateWindow(window);
	if (!matches(GetPixel(dc, 12, 12), 255, 0, 0)) return 36;
	ReleaseDC(window, dc);
	if (FAILED(controller->lpVtbl->remove_CommitNeeded(controller, token))) return 37;

	/* A compositor made on its own commits by itself. */
	IInspectable *alone = activate(RoActivateInstance, compositor_name, ARRAYSIZE(compositor_name) - 1);
	__x_ABI_CWindows_CUI_CComposition_CICompositor *second = NULL;
	if (!alone || FAILED(IInspectable_QueryInterface(alone, &IID___x_ABI_CWindows_CUI_CComposition_CICompositor, (void **)&second))) return 38;

	/* The compositor clock: a tick within a frame, or the handle that is set first. */
	HMODULE dcomp = LoadLibraryW(L"dcomp.dll");
	clock_function wait = dcomp ? (clock_function)GetProcAddress(dcomp, "DCompositionWaitForCompositorClock") : NULL;
	if (!wait) return 39;
	DWORD started = GetTickCount();
	if (wait(0, NULL, 1000) != WAIT_OBJECT_0 || GetTickCount() - started > 200) return 40;
	HANDLE event = CreateEventW(NULL, TRUE, TRUE, NULL);
	if (wait(1, &event, 1000) != WAIT_OBJECT_0) return 41;
	if (wait(0, NULL, 0) != WAIT_TIMEOUT && wait(0, NULL, 0) != WAIT_OBJECT_0) return 42;

	/* Swap chains made for composition are drawn by the runtime from their buffers, so the Direct3D libraries are told to make them
	 * without a window of their own. */
	char config[256];
	if (!GetEnvironmentVariableA("DXVK_CONFIG", config, sizeof config) || !contains(config, "dxgi.enableDummyCompositionSwapchain = True"))
		return 43;

	second->lpVtbl->Release(second);
	IInspectable_Release(alone);
	children->lpVtbl->Release(children);
	root_visual->lpVtbl->Release(root_visual);
	root->lpVtbl->Release(root);
	target->lpVtbl->Release(target);
	IInspectable_Release(desktop_target);
	DestroyWindow(window);
	desktop->lpVtbl->Release(desktop);
	compositor->lpVtbl->Release(compositor);
	controller->lpVtbl->Release(controller);
	return 0;
}
