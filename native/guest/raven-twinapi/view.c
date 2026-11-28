/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.UI.ViewManagement.ApplicationView and ApplicationViewTitleBar: the app's view as its window on the desktop, for the thread that
 * runs the app's CoreApplicationView.
 *
 * - Sizes are in device-independent pixels, at the DPI of the view's window. The orientation is landscape unless the window is taller
 *   than it is wide; the view is adjacent to an edge of the display when its window touches that edge of the monitor's work area.
 * - Title shows in the caption before the program's name, as "Title - Name".
 * - TryEnterFullScreenMode makes the window a borderless one over its whole monitor, which the desktop shows full screen, and
 *   ExitFullScreenMode puts back its frame and place. TryResizeView sizes the client area unless the view is full screen or maximized,
 *   or the size is under the preferred minimum, which SetPreferredMinSize gives between 192 x 48 and 500 x 500.
 * - PreferredLaunchWindowingMode and PreferredLaunchViewSize apply to the window when the view is made: a size, full screen or
 *   maximized. Raven keeps no view state across launches, so there is no persisted state to clear.
 * - The title bar's colors are the caption's: background and foreground, and their inactive forms while the window is not active;
 *   a color with no opacity is the system's. The caption buttons are drawn by the desktop in the caption's colors; their own colors
 *   are kept and given back.
 * - Only the default view mode is supported: TryEnterViewModeAsync completes with false for any other. TryConsolidateAsync closes the
 *   window, and Consolidated is raised as the window goes, as initiated by the app or by the user.
 * - TerminateAppOnFinalViewClose is kept: the app has one view and ends when its window closes.
 * - CoreApplicationViewTitleBar: a view extended into its title bar has its client area up to the window's top, with the desktop's
 *   caption buttons over its right, whose width is the right inset; the caption's strip, as high as Height, still moves the window.
 *   The title bar is visible unless the view is full screen.
 * - VisibleBounds is the client area on the screen; VisibleBoundsChanged is raised when the window moves or changes size. Screen
 *   capture, system overlays and the desired bounds mode are kept as set. */
#include "object.h"
#include "../raven-winrt/winrt.h"
#include <uxtheme.h>

static const IID iid_view = {0xd222d519, 0x4361, 0x451e, {0x96, 0xc4, 0x60, 0xf4, 0xf9, 0x74, 0x2d, 0xb0}};
static const IID iid_view2 = {0xe876b196, 0xa545, 0x40dc, {0xb5, 0x94, 0x45, 0x0c, 0xba, 0x68, 0xcc, 0x00}};
static const IID iid_view3 = {0x903c9ce5, 0x793a, 0x4fdf, {0xa2, 0xb2, 0xaf, 0x1a, 0xc2, 0x1e, 0x31, 0x08}};
static const IID iid_view4 = {0x15e5cbec, 0x9e0f, 0x46b5, {0xbc, 0x3f, 0x9b, 0xf6, 0x53, 0xe7, 0x4b, 0x5e}};
static const IID iid_view7 = {0xa0369647, 0x5faf, 0x5aa6, {0x9c, 0x38, 0xbe, 0xfb, 0xb1, 0x2a, 0x07, 0x1e}};
static const IID iid_view_statics = {0x010a6306, 0xc433, 0x44e5, {0xa9, 0xf2, 0xbd, 0x84, 0xd4, 0x03, 0x0a, 0x95}};
static const IID iid_view_statics2 = {0xaf338ae5, 0xcf64, 0x423c, {0x85, 0xe5, 0xf3, 0xe7, 0x24, 0x48, 0xfb, 0x23}};
static const IID iid_view_statics3 = {0xa28d7594, 0x8c41, 0x4e13, {0x97, 0x19, 0x51, 0x64, 0x79, 0x6f, 0xe4, 0xc7}};
static const IID iid_view_statics4 = {0x08fd8d33, 0x2611, 0x5336, {0xa3, 0x15, 0xd9, 0x8e, 0x63, 0x66, 0xc9, 0xdb}};
static const IID iid_view_interop = {0xc446fb5d, 0x4793, 0x4896, {0xa8, 0xe2, 0xbe, 0x57, 0xa8, 0xbb, 0x0f, 0x50}};
static const IID iid_title_bar = {0x00924ac0, 0x932b, 0x4a6b, {0x9c, 0x4b, 0xdc, 0x38, 0xc8, 0x24, 0x78, 0xce}};
static const IID iid_consolidated = {0x514449ec, 0x7ea2, 0x4de7, {0xa6, 0xa6, 0x7d, 0xfb, 0xaa, 0xeb, 0xb6, 0xfb}};
static const IID iid_consolidated2 = {0x1c199ecc, 0x6dc1, 0x40f4, {0xaf, 0xee, 0x07, 0xd9, 0xea, 0x29, 0x64, 0x30}};
static const IID iid_core_window_interop = {0x45d64a29, 0xa63e, 0x4cb6, {0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f}};
/* IReference<Windows.UI.Color> and IAsyncOperation<Boolean>. */
static const IID iid_color_reference = {0xab8e5d11, 0xb0c1, 0x5a21, {0x95, 0xae, 0xf1, 0x6b, 0xf3, 0xa3, 0x76, 0x24}};
static const IID iid_boolean_operation = {0xcdb5efb3, 0x5788, 0x509d, {0x9b, 0xe1, 0x71, 0xcc, 0xb8, 0xa3, 0x36, 0x2a}};

struct size {
	FLOAT width, height;
};

struct rect {
	FLOAT x, y, width, height;
};

struct color {
	BYTE a, r, g, b;
};

/* Windows.UI.ViewManagement enumerations. */
enum { LANDSCAPE, PORTRAIT };
enum { STATE_FULL_SCREEN_LANDSCAPE = 0, STATE_FULL_SCREEN_PORTRAIT = 3 };
enum { LAUNCH_AUTO, LAUNCH_VIEW_SIZE, LAUNCH_FULL_SCREEN, LAUNCH_COMPACT_OVERLAY, LAUNCH_MAXIMIZED };
enum { MODE_DEFAULT };

/* The title bar's colors, in the order of IApplicationViewTitleBar's properties. */
enum {
	FOREGROUND, BACKGROUND, BUTTON_FOREGROUND, BUTTON_BACKGROUND, BUTTON_HOVER_FOREGROUND, BUTTON_HOVER_BACKGROUND, BUTTON_PRESSED_FOREGROUND,
	BUTTON_PRESSED_BACKGROUND, INACTIVE_FOREGROUND, INACTIVE_BACKGROUND, BUTTON_INACTIVE_FOREGROUND, BUTTON_INACTIVE_BACKGROUND, COLORS
};

struct title_bar {
	struct object base;
	IUnknown *colors[COLORS];
};

struct view {
	struct object base;
	SRWLOCK lock;
	HSTRING title, persisted;
	struct handlers consolidated, visible_bounds;
	/* What the view was before it went full screen. */
	BOOL full_screen;
	LONG_PTR style, extended;
	WINDOWPLACEMENT placement;
	struct size minimum;
	boolean screen_capture, suppress_overlays;
	INT32 bounds_mode, overlay_mode;
	BOOL app_closing, shown;
};

static struct view *view_of(void *iface) { return (struct view *)owner(iface); }
static struct title_bar *title_bar_of(void *iface) { return (struct title_bar *)owner(iface); }

/* What the app asked for before its view was made, and whether the app ends when its last view closes. */
static INT32 launch_mode;
static struct size launch_size;
static boolean terminate_on_close = TRUE;

static UINT window_dpi(HWND window)
{
	static UINT (WINAPI *dpi_for_window)(HWND);
	static BOOL looked;
	if (!looked) {
		dpi_for_window = (UINT (WINAPI *)(HWND))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
		looked = TRUE;
	}
	const UINT dpi = dpi_for_window && window ? dpi_for_window(window) : 0;
	return dpi ? dpi : 96;
}

static FLOAT to_dips(HWND window, LONG pixels) { return (FLOAT)pixels * 96.0f / (FLOAT)window_dpi(window); }
static LONG to_pixels(HWND window, FLOAT dips) { return (LONG)(dips * (FLOAT)window_dpi(window) / 96.0f + 0.5f); }

/* The program's name, as the core window's caption shows it. */
static void program_name(WCHAR *name, DWORD capacity)
{
	WCHAR path[MAX_PATH];
	const DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	const WCHAR *start = path;
	for (DWORD at = 0; at < length; ++at)
		if (path[at] == L'\\' || path[at] == L'/')
			start = path + at + 1;
	lstrcpynW(name, start, (int)capacity);
	for (WCHAR *dot = name + lstrlenW(name); dot > name; --dot)
		if (*dot == L'.') {
			*dot = 0;
			break;
		}
}

/* ---- ApplicationViewTitleBar ---- */

static HRESULT put_color(void *iface, size_t which, IUnknown *value);
static HRESULT get_color(void *iface, size_t which, void **out)
{
	struct title_bar *self = title_bar_of(iface);
	if (!out)
		return E_POINTER;
	reference(*out = self->colors[which]);
	return S_OK;
}

#define COLOR_METHODS(name, which) \
	static HRESULT WINAPI set_##name(void *iface, IUnknown *value) { return put_color(iface, which, value); } \
	static HRESULT WINAPI get_##name(void *iface, void **out) { return get_color(iface, which, out); }
COLOR_METHODS(foreground, FOREGROUND)
COLOR_METHODS(background, BACKGROUND)
COLOR_METHODS(button_foreground, BUTTON_FOREGROUND)
COLOR_METHODS(button_background, BUTTON_BACKGROUND)
COLOR_METHODS(button_hover_foreground, BUTTON_HOVER_FOREGROUND)
COLOR_METHODS(button_hover_background, BUTTON_HOVER_BACKGROUND)
COLOR_METHODS(button_pressed_foreground, BUTTON_PRESSED_FOREGROUND)
COLOR_METHODS(button_pressed_background, BUTTON_PRESSED_BACKGROUND)
COLOR_METHODS(inactive_foreground, INACTIVE_FOREGROUND)
COLOR_METHODS(inactive_background, INACTIVE_BACKGROUND)
COLOR_METHODS(button_inactive_foreground, BUTTON_INACTIVE_FOREGROUND)
COLOR_METHODS(button_inactive_background, BUTTON_INACTIVE_BACKGROUND)
#define COLOR_SLOTS(name) (const void *)set_##name, (const void *)get_##name

TABLE(title_bar_table, 0, INSPECTABLE, COLOR_SLOTS(foreground), COLOR_SLOTS(background), COLOR_SLOTS(button_foreground),
	COLOR_SLOTS(button_background), COLOR_SLOTS(button_hover_foreground), COLOR_SLOTS(button_hover_background),
	COLOR_SLOTS(button_pressed_foreground), COLOR_SLOTS(button_pressed_background), COLOR_SLOTS(inactive_foreground),
	COLOR_SLOTS(inactive_background), COLOR_SLOTS(button_inactive_foreground), COLOR_SLOTS(button_inactive_background));
static const struct face_iid title_bar_iids[] = {{&iid_title_bar, 0}, {NULL, 0}};
static const struct kind title_bar_kind = {"ApplicationViewTitleBar", L"Windows.UI.ViewManagement.ApplicationViewTitleBar", title_bar_iids, NULL};
static struct title_bar title_bar = {{{title_bar_table.slots}, &title_bar_kind, 1}};

/* A color of the title bar as a COLORREF, or DWMWA_COLOR_DEFAULT when it is not set or has no opacity. */
static COLORREF caption_color(size_t which)
{
	IUnknown *value = title_bar.colors[which];
	struct color color = {0};
	if (!value || ((HRESULT (WINAPI *)(void *, struct color *))method(value, 6))(value, &color) != S_OK || !color.a)
		return 0xffffffff;
	return RGB(color.r, color.g, color.b);
}

/* Gives the caption the colors of the title bar for the window's state. */
static void paint_caption(HWND window, BOOL active)
{
	if (!window)
		return;
	static HRESULT (WINAPI *set_attribute)(HWND, DWORD, const void *, DWORD);
	if (!set_attribute) {
		HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
		if (!dwm || !(set_attribute = (HRESULT (WINAPI *)(HWND, DWORD, const void *, DWORD))GetProcAddress(dwm, "DwmSetWindowAttribute")))
			return;
	}
	/* DWMWA_CAPTION_COLOR and DWMWA_TEXT_COLOR. */
	COLORREF fill = caption_color(active ? BACKGROUND : INACTIVE_BACKGROUND), text = caption_color(active ? FOREGROUND : INACTIVE_FOREGROUND);
	set_attribute(window, 35, &fill, sizeof fill);
	set_attribute(window, 36, &text, sizeof text);
}

static HRESULT put_color(void *iface, size_t which, IUnknown *value)
{
	IUnknown *color = NULL;
	if (value && FAILED(value->lpVtbl->QueryInterface(value, &iid_color_reference, (void **)&color)))
		return E_INVALIDARG;
	IUnknown *old = (IUnknown *)InterlockedExchangePointer((void **)&title_bar_of(iface)->colors[which], color);
	release(old);
	const HWND window = application_window();
	paint_caption(window, window && GetActiveWindow() == window);
	return S_OK;
}

/* ---- CoreApplicationViewTitleBar ---- */

static const IID iid_core_title_bar = {0x006d35e3, 0xe1f1, 0x431b, {0x95, 0x08, 0x29, 0xb9, 0x69, 0x26, 0xac, 0x53}};

struct core_title_bar {
	struct object base;
	boolean extended;
	struct handlers layout, visibility;
};

static struct core_title_bar *core_title_bar_of(void *iface) { return (struct core_title_bar *)owner(iface); }

static UINT system_metric(int index, HWND window)
{
	static int (WINAPI *metric_for_dpi)(int, UINT);
	static BOOL looked;
	if (!looked) {
		metric_for_dpi = (int (WINAPI *)(int, UINT))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
		looked = TRUE;
	}
	return (UINT)(metric_for_dpi ? metric_for_dpi(index, window_dpi(window)) : GetSystemMetrics(index));
}

/* The caption's height in pixels: what the view covers once it is extended into the title bar. */
static LONG caption_pixels(HWND window)
{
	return (LONG)(system_metric(SM_CYCAPTION, window) + system_metric(SM_CYFRAME, window) + system_metric(SM_CXPADDEDBORDER, window));
}

static HRESULT (WINAPI *dwm_function(const char *name))(void)
{
	HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
	return dwm ? (HRESULT (WINAPI *)(void))GetProcAddress(dwm, name) : NULL;
}

/* Gives the caption over the view's top the desktop's caption buttons, or takes it back. */
static void extend_frame(HWND window, BOOL extended)
{
	typedef HRESULT (WINAPI *extend_type)(HWND, const MARGINS *);
	static extend_type extend;
	if (!extend && !(extend = (extend_type)dwm_function("DwmExtendFrameIntoClientArea")))
		return;
	const MARGINS margins = {0, 0, extended ? (int)caption_pixels(window) : 0, 0};
	extend(window, &margins);
	SetWindowPos(window, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static struct view main_view;
static struct core_title_bar core_title_bar;

static HRESULT WINAPI set_core_extended(void *iface, boolean value)
{
	struct core_title_bar *self = core_title_bar_of(iface);
	if (!!self->extended == !!value)
		return S_OK;
	self->extended = value;
	if (application_window())
		extend_frame(application_window(), value);
	handlers_raise(&self->layout, iface, NULL);
	return S_OK;
}

static HRESULT WINAPI core_extended(void *iface, boolean *out) { return answer_flag(out, core_title_bar_of(iface)->extended); }

static HRESULT WINAPI core_left_inset(void *iface, DOUBLE *out)
{
	if (!out)
		return E_POINTER;
	*out = 0;
	return S_OK;
}

/* The caption buttons' width, on the right of the caption. */
static HRESULT WINAPI core_right_inset(void *iface, DOUBLE *out)
{
	typedef HRESULT (WINAPI *attribute_type)(HWND, DWORD, void *, DWORD);
	static attribute_type attribute;
	const HWND window = application_window();
	RECT buttons = {0};
	if (!out)
		return E_POINTER;
	*out = 0;
	if (!attribute)
		attribute = (attribute_type)dwm_function("DwmGetWindowAttribute");
	/* DWMWA_CAPTION_BUTTON_BOUNDS. */
	if (window && attribute && attribute(window, 5, &buttons, sizeof buttons) == S_OK)
		*out = to_dips(window, buttons.right - buttons.left);
	return S_OK;
}

static HRESULT WINAPI core_height(void *iface, DOUBLE *out)
{
	const HWND window = application_window();
	if (!out)
		return E_POINTER;
	*out = window ? to_dips(window, caption_pixels(window)) : 0;
	return S_OK;
}

static HRESULT WINAPI core_add_layout(void *iface, void *handler, INT64 *token) { return handlers_add(&core_title_bar_of(iface)->layout, handler, token); }
static HRESULT WINAPI core_remove_layout(void *iface, INT64 token) { return handlers_remove(&core_title_bar_of(iface)->layout, token); }

/* The title bar shows unless the view is full screen. */
static HRESULT WINAPI core_visible(void *iface, boolean *out);

static HRESULT WINAPI core_add_visibility(void *iface, void *handler, INT64 *token)
{
	return handlers_add(&core_title_bar_of(iface)->visibility, handler, token);
}

static HRESULT WINAPI core_remove_visibility(void *iface, INT64 token) { return handlers_remove(&core_title_bar_of(iface)->visibility, token); }

TABLE(core_title_bar_table, 0, INSPECTABLE, (const void *)set_core_extended, (const void *)core_extended, (const void *)core_left_inset,
	(const void *)core_right_inset, (const void *)core_height, (const void *)core_add_layout, (const void *)core_remove_layout,
	(const void *)core_visible, (const void *)core_add_visibility, (const void *)core_remove_visibility);
static const struct face_iid core_title_bar_iids[] = {{&iid_core_title_bar, 0}, {NULL, 0}};
static const struct kind core_title_bar_kind = {"CoreApplicationViewTitleBar", L"Windows.ApplicationModel.Core.CoreApplicationViewTitleBar",
	core_title_bar_iids, NULL};
static struct core_title_bar core_title_bar = {{{core_title_bar_table.slots}, &core_title_bar_kind, 1}};

HRESULT application_title_bar(void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = face_of(&core_title_bar.base, 0));
	return S_OK;
}

static void title_bar_visibility_changed(void) { handlers_raise(&core_title_bar.visibility, face_of(&core_title_bar.base, 0), NULL); }

/* ---- ApplicationView ---- */

static HWND view_window(void) { return application_window(); }

static HRESULT WINAPI view_orientation(void *iface, INT32 *out)
{
	RECT client = {0};
	if (!out)
		return E_POINTER;
	GetClientRect(view_window(), &client);
	*out = client.bottom - client.top > client.right - client.left ? PORTRAIT : LANDSCAPE;
	return S_OK;
}

/* The work area of the window's monitor, and the window's rectangle. */
static BOOL view_edges(RECT *work, RECT *frame)
{
	const HWND window = view_window();
	MONITORINFO monitor = {sizeof monitor};
	if (!window || !GetWindowRect(window, frame) || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
		return FALSE;
	*work = monitor.rcWork;
	return TRUE;
}

static HRESULT WINAPI view_adjacent_left(void *iface, boolean *out)
{
	RECT work, frame;
	return answer_flag(out, view_edges(&work, &frame) && frame.left <= work.left);
}

static HRESULT WINAPI view_adjacent_right(void *iface, boolean *out)
{
	RECT work, frame;
	return answer_flag(out, view_edges(&work, &frame) && frame.right >= work.right);
}

/* IsFullScreen: whether the view spans the display from its left edge to its right. */
static HRESULT WINAPI view_spans(void *iface, boolean *out)
{
	RECT work, frame;
	return answer_flag(out, view_edges(&work, &frame) && frame.left <= work.left && frame.right >= work.right);
}

static HRESULT WINAPI view_false(void *iface, boolean *out) { return answer_flag(out, FALSE); }

static HRESULT WINAPI view_screen_capture(void *iface, boolean *out) { return answer_flag(out, view_of(iface)->screen_capture); }

static HRESULT WINAPI set_view_screen_capture(void *iface, boolean value)
{
	view_of(iface)->screen_capture = value;
	if (view_window())
		SetWindowDisplayAffinity(view_window(), value ? WDA_NONE : WDA_MONITOR);
	return S_OK;
}

/* Puts "Title - Name" in the caption, or the name alone with no title. */
static void show_title(struct view *self, HWND window)
{
	WCHAR name[MAX_PATH], caption[2 * MAX_PATH];
	program_name(name, MAX_PATH);
	AcquireSRWLockShared(&self->lock);
	const UINT32 length = WindowsGetStringLen(self->title);
	if (length)
		wsprintfW(caption, L"%.*s - %s", (int)(length < MAX_PATH ? length : MAX_PATH - 1), WindowsGetStringRawBuffer(self->title, NULL), name);
	else
		lstrcpyW(caption, name);
	ReleaseSRWLockShared(&self->lock);
	if (window)
		SetWindowTextW(window, caption);
}

static HRESULT WINAPI set_view_title(void *iface, HSTRING value)
{
	struct view *self = view_of(iface);
	HSTRING copy = NULL;
	const HRESULT result = WindowsDuplicateString(value, &copy);
	if (FAILED(result))
		return result;
	AcquireSRWLockExclusive(&self->lock);
	HSTRING old = self->title;
	self->title = copy;
	ReleaseSRWLockExclusive(&self->lock);
	WindowsDeleteString(old);
	show_title(self, view_window());
	return S_OK;
}

static HRESULT WINAPI view_title(void *iface, HSTRING *out)
{
	struct view *self = view_of(iface);
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = WindowsDuplicateString(self->title, out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

/* A view's identifier is its window's, as ApplicationViewInterop gives it for the core window. */
static INT32 id_of(HWND window) { return (INT32)((ULONG_PTR)window & 0x7fffffff); }

static HRESULT WINAPI view_id(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = id_of(view_window());
	return S_OK;
}

static HRESULT WINAPI view_add_consolidated(void *iface, void *handler, INT64 *token)
{
	return handlers_add(&view_of(iface)->consolidated, handler, token);
}

static HRESULT WINAPI view_remove_consolidated(void *iface, INT64 token) { return handlers_remove(&view_of(iface)->consolidated, token); }

static HRESULT WINAPI view_suppress_overlays(void *iface, boolean *out) { return answer_flag(out, view_of(iface)->suppress_overlays); }
static HRESULT WINAPI set_view_suppress_overlays(void *iface, boolean value) { view_of(iface)->suppress_overlays = value; return S_OK; }

static HRESULT WINAPI view_visible_bounds(void *iface, struct rect *out)
{
	const HWND window = view_window();
	RECT client = {0};
	POINT corner = {0, 0};
	if (!out)
		return E_POINTER;
	if (window) {
		GetClientRect(window, &client);
		ClientToScreen(window, &corner);
	}
	*out = (struct rect){to_dips(window, corner.x), to_dips(window, corner.y), to_dips(window, client.right), to_dips(window, client.bottom)};
	return S_OK;
}

static HRESULT WINAPI view_add_visible_bounds(void *iface, void *handler, INT64 *token)
{
	return handlers_add(&view_of(iface)->visible_bounds, handler, token);
}

static HRESULT WINAPI view_remove_visible_bounds(void *iface, INT64 token) { return handlers_remove(&view_of(iface)->visible_bounds, token); }

static HRESULT WINAPI set_view_bounds_mode(void *iface, INT32 mode, boolean *out)
{
	if (mode < 0 || mode > 1)
		return E_INVALIDARG;
	view_of(iface)->bounds_mode = mode;
	return answer_flag(out, TRUE);
}

static HRESULT WINAPI view_bounds_mode(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = view_of(iface)->bounds_mode;
	return S_OK;
}

static HRESULT WINAPI view_title_bar(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = face_of(&title_bar.base, 0));
	return S_OK;
}

static HRESULT WINAPI view_overlay_mode(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = view_of(iface)->overlay_mode;
	return S_OK;
}

static HRESULT WINAPI set_view_overlay_mode(void *iface, INT32 value) { view_of(iface)->overlay_mode = value; return S_OK; }

static HRESULT WINAPI view_full_screen_mode(void *iface, boolean *out) { return answer_flag(out, view_of(iface)->full_screen); }

/* A borderless window over its whole monitor, which the desktop shows full screen. */
static BOOL enter_full_screen(struct view *self, HWND window)
{
	MONITORINFO monitor = {sizeof monitor};
	if (!window || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
		return FALSE;
	if (self->full_screen)
		return TRUE;
	self->placement.length = sizeof self->placement;
	if (!GetWindowPlacement(window, &self->placement))
		return FALSE;
	self->style = GetWindowLongPtrW(window, GWL_STYLE);
	self->extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
	self->full_screen = TRUE;
	title_bar_visibility_changed();
	SetWindowLongPtrW(window, GWL_STYLE, (self->style & ~(LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE)) | WS_POPUP);
	SetWindowLongPtrW(window, GWL_EXSTYLE, self->extended & ~(LONG_PTR)(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
	SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right - monitor.rcMonitor.left,
		monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
	return TRUE;
}

static HRESULT WINAPI view_enter_full_screen(void *iface, boolean *out)
{
	return answer_flag(out, enter_full_screen(view_of(iface), view_window()));
}

static HRESULT WINAPI view_exit_full_screen(void *iface)
{
	struct view *self = view_of(iface);
	const HWND window = view_window();
	if (!self->full_screen || !window)
		return S_OK;
	self->full_screen = FALSE;
	title_bar_visibility_changed();
	SetWindowLongPtrW(window, GWL_STYLE, self->style);
	SetWindowLongPtrW(window, GWL_EXSTYLE, self->extended);
	SetWindowPlacement(window, &self->placement);
	SetWindowPos(window, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
	return S_OK;
}

static HRESULT WINAPI view_show_overlays(void *iface) { return S_OK; }

/* Sizes the window's client area, in DIPs. */
static BOOL resize_client(HWND window, struct size size)
{
	RECT frame = {0, 0, to_pixels(window, size.width), to_pixels(window, size.height)};
	if (!AdjustWindowRectEx(&frame, (DWORD)GetWindowLongPtrW(window, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE)))
		return FALSE;
	return SetWindowPos(window, NULL, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static HRESULT WINAPI view_try_resize(void *iface, struct size size, boolean *out)
{
	struct view *self = view_of(iface);
	const HWND window = view_window();
	if (!out)
		return E_POINTER;
	*out = FALSE;
	if (!window || self->full_screen || IsZoomed(window) || size.width < self->minimum.width || size.height < self->minimum.height ||
		size.width <= 0 || size.height <= 0)
		return S_OK;
	*out = (boolean)resize_client(window, size);
	return S_OK;
}

static HRESULT WINAPI view_set_minimum(void *iface, struct size size)
{
	struct view *self = view_of(iface);
	/* Windows keeps the smallest minimum at 192 x 48 and the largest at 500 x 500. */
	self->minimum.width = size.width < 192 ? 192 : size.width > 500 ? 500 : size.width;
	self->minimum.height = size.height < 48 ? 48 : size.height > 500 ? 500 : size.height;
	return S_OK;
}

static HRESULT WINAPI view_mode(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = MODE_DEFAULT;
	return S_OK;
}

static HRESULT WINAPI view_mode_supported(void *iface, INT32 mode, boolean *out) { return answer_flag(out, mode == MODE_DEFAULT); }

static HRESULT WINAPI view_enter_mode(void *iface, INT32 mode, void **out)
{
	const union winrt_value done = {.boolean = mode == MODE_DEFAULT};
	return winrt_async_done(&iid_boolean_operation, WINRT_RESULT_BOOLEAN, S_OK, &done, out);
}

static HRESULT WINAPI view_enter_mode_with(void *iface, INT32 mode, IUnknown *preferences, void **out)
{
	return view_enter_mode(iface, mode, out);
}

static HRESULT WINAPI view_consolidate(void *iface, void **out)
{
	struct view *self = view_of(iface);
	const HWND window = view_window();
	union winrt_value done = {.boolean = FALSE};
	if (window) {
		self->app_closing = TRUE;
		done.boolean = (boolean)PostMessageW(window, WM_CLOSE, 0, 0);
	}
	return winrt_async_done(&iid_boolean_operation, WINRT_RESULT_BOOLEAN, S_OK, &done, out);
}

static HRESULT WINAPI view_persisted_id(void *iface, HSTRING *out)
{
	struct view *self = view_of(iface);
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = WindowsDuplicateString(self->persisted, out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

static HRESULT WINAPI set_view_persisted_id(void *iface, HSTRING value)
{
	struct view *self = view_of(iface);
	HSTRING copy = NULL;
	const HRESULT result = WindowsDuplicateString(value, &copy);
	if (FAILED(result))
		return result;
	AcquireSRWLockExclusive(&self->lock);
	HSTRING old = self->persisted;
	self->persisted = copy;
	ReleaseSRWLockExclusive(&self->lock);
	WindowsDeleteString(old);
	return S_OK;
}

TABLE(view_table, 0, INSPECTABLE, (const void *)view_orientation, (const void *)view_adjacent_left, (const void *)view_adjacent_right,
	(const void *)view_spans, (const void *)view_false, (const void *)view_screen_capture, (const void *)set_view_screen_capture,
	(const void *)set_view_title, (const void *)view_title, (const void *)view_id, (const void *)view_add_consolidated,
	(const void *)view_remove_consolidated);
TABLE(view2_table, 1, INSPECTABLE, (const void *)view_suppress_overlays, (const void *)set_view_suppress_overlays,
	(const void *)view_visible_bounds, (const void *)view_add_visible_bounds, (const void *)view_remove_visible_bounds,
	(const void *)set_view_bounds_mode, (const void *)view_bounds_mode);
TABLE(view3_table, 2, INSPECTABLE, (const void *)view_title_bar, (const void *)view_overlay_mode, (const void *)set_view_overlay_mode,
	(const void *)view_full_screen_mode, (const void *)view_enter_full_screen, (const void *)view_exit_full_screen,
	(const void *)view_show_overlays, (const void *)view_try_resize, (const void *)view_set_minimum);
TABLE(view4_table, 3, INSPECTABLE, (const void *)view_mode, (const void *)view_mode_supported, (const void *)view_enter_mode,
	(const void *)view_enter_mode_with, (const void *)view_consolidate);
TABLE(view7_table, 4, INSPECTABLE, (const void *)view_persisted_id, (const void *)set_view_persisted_id);
static const struct face_iid view_iids[] = {{&iid_view, 0}, {&iid_view2, 1}, {&iid_view3, 2}, {&iid_view4, 3}, {&iid_view7, 4}, {NULL, 0}};
static const struct kind view_kind = {"ApplicationView", L"Windows.UI.ViewManagement.ApplicationView", view_iids, NULL};
/* The app has one view, on the thread that runs it. */
static HRESULT WINAPI core_visible(void *iface, boolean *out) { return answer_flag(out, !main_view.full_screen); }

static struct view main_view = {{{view_table.slots, view2_table.slots, view3_table.slots, view4_table.slots, view7_table.slots}, &view_kind, 1},
	SRWLOCK_INIT, NULL, NULL, {0}, {0}, FALSE, 0, 0, {0}, {192, 48}, TRUE};

/* ---- ApplicationViewConsolidatedEventArgs ---- */

struct consolidated {
	struct object base;
	boolean by_app;
};

static void free_arguments(struct object *base) {}

static HRESULT WINAPI consolidated_by_user(void *iface, boolean *out) { return answer_flag(out, !((struct consolidated *)owner(iface))->by_app); }
static HRESULT WINAPI consolidated_by_app(void *iface, boolean *out) { return answer_flag(out, ((struct consolidated *)owner(iface))->by_app); }

TABLE(consolidated_table, 0, INSPECTABLE, (const void *)consolidated_by_user);
TABLE(consolidated2_table, 1, INSPECTABLE, (const void *)consolidated_by_app);
static const struct face_iid consolidated_iids[] = {{&iid_consolidated, 0}, {&iid_consolidated2, 1}, {NULL, 0}};
static const struct kind consolidated_kind = {"ApplicationViewConsolidatedEventArgs",
	L"Windows.UI.ViewManagement.ApplicationViewConsolidatedEventArgs", consolidated_iids, free_arguments};

/* ---- the core window's messages that concern the view ---- */

BOOL application_view_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
	struct view *self = &main_view;
	/* A view extended into its title bar has its client area up to the window's top, under the desktop's caption buttons, and the
	 * caption's strip still moves the window and sizes it from its top edge. */
	if (core_title_bar.extended && !self->full_screen) {
		if (message == WM_NCCALCSIZE && wparam) {
			NCCALCSIZE_PARAMS *sizes = (NCCALCSIZE_PARAMS *)lparam;
			const LONG top = sizes->rgrc[0].top;
			*result = DefWindowProcW(window, message, wparam, lparam);
			/* A maximized window reaches past the screen by its frame, which stays out of the view. */
			sizes->rgrc[0].top = top + (IsZoomed(window) ? (LONG)(system_metric(SM_CYFRAME, window) + system_metric(SM_CXPADDEDBORDER, window)) : 0);
			return TRUE;
		}
		if (message == WM_NCHITTEST) {
			typedef BOOL (WINAPI *default_type)(HWND, UINT, WPARAM, LPARAM, LRESULT *);
			static default_type dwm_default;
			if (!dwm_default)
				dwm_default = (default_type)dwm_function("DwmDefWindowProc");
			if (dwm_default && dwm_default(window, message, wparam, lparam, result))
				return TRUE;
			*result = DefWindowProcW(window, message, wparam, lparam);
			POINT at = {(SHORT)LOWORD(lparam), (SHORT)HIWORD(lparam)};
			ScreenToClient(window, &at);
			if (*result == HTCLIENT && at.y >= 0 && at.y < caption_pixels(window))
				*result = at.y < (LONG)system_metric(SM_CYFRAME, window) && !IsZoomed(window) ? HTTOP : HTCAPTION;
			return TRUE;
		}
	}
	switch (message) {
	case WM_CREATE:
		/* What the app asked for before its view was made. */
		if (launch_mode == LAUNCH_VIEW_SIZE && launch_size.width > 0 && launch_size.height > 0)
			resize_client(window, launch_size);
		else if (launch_mode == LAUNCH_FULL_SCREEN)
			enter_full_screen(self, window);
		show_title(self, window);
		if (core_title_bar.extended)
			extend_frame(window, TRUE);
		break;
	case WM_DPICHANGED:
		handlers_raise(&core_title_bar.layout, face_of(&core_title_bar.base, 0), NULL);
		break;
	case WM_SHOWWINDOW:
		if (wparam && launch_mode == LAUNCH_MAXIMIZED && !self->full_screen && !self->shown) {
			self->shown = TRUE;
			PostMessageW(window, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
		}
		break;
	case WM_GETMINMAXINFO: {
		MINMAXINFO *limits = (MINMAXINFO *)lparam;
		RECT frame = {0, 0, to_pixels(window, self->minimum.width), to_pixels(window, self->minimum.height)};
		if (!self->full_screen && AdjustWindowRectEx(&frame, (DWORD)GetWindowLongPtrW(window, GWL_STYLE), FALSE,
				(DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE))) {
			limits->ptMinTrackSize.x = frame.right - frame.left;
			limits->ptMinTrackSize.y = frame.bottom - frame.top;
		}
		break;
	}
	case WM_ACTIVATE:
		paint_caption(window, LOWORD(wparam) != WA_INACTIVE);
		break;
	case WM_SIZE:
	case WM_MOVE:
		if (message == WM_MOVE || wparam != SIZE_MINIMIZED)
			handlers_raise(&self->visible_bounds, face_of(&self->base, 0), NULL);
		break;
	case WM_DESTROY: {
		static const void *const *const tables[] = {consolidated_table.slots, consolidated2_table.slots};
		struct consolidated *arguments = make(sizeof *arguments, &consolidated_kind, tables, 2);
		if (arguments) {
			arguments->by_app = (boolean)self->app_closing;
			handlers_raise(&self->consolidated, face_of(&self->base, 0), face_of(&arguments->base, 0));
			release(face_of(&arguments->base, 0));
		}
		self->app_closing = FALSE;
		self->full_screen = FALSE;
		break;
	}
	}
	return FALSE;
}

/* ---- the class ---- */

static HRESULT WINAPI statics_value(void *iface, INT32 *out)
{
	INT32 orientation = LANDSCAPE;
	if (!out)
		return E_POINTER;
	view_orientation(NULL, &orientation);
	*out = orientation == PORTRAIT ? STATE_FULL_SCREEN_PORTRAIT : STATE_FULL_SCREEN_LANDSCAPE;
	return S_OK;
}

static HRESULT WINAPI statics_unsnap(void *iface, boolean *out) { return answer_flag(out, TRUE); }

/* GetForCurrentView: the view of the thread that runs the app's view, and an error on any other, as on Windows. */
static HRESULT WINAPI statics_for_view(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	IUnknown *core = NULL;
	if (FAILED(application_view((void **)&core)) || !core || !view_window() ||
		GetWindowThreadProcessId(view_window(), NULL) != GetCurrentThreadId()) {
		release(core);
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	release(core);
	reference(*out = face_of(&main_view.base, 0));
	return S_OK;
}

static HRESULT WINAPI statics_terminate(void *iface, boolean *out) { return answer_flag(out, terminate_on_close); }
static HRESULT WINAPI set_statics_terminate(void *iface, boolean value) { terminate_on_close = value; return S_OK; }

static HRESULT WINAPI statics_launch_mode(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = launch_mode;
	return S_OK;
}

static HRESULT WINAPI set_statics_launch_mode(void *iface, INT32 value)
{
	if (value < LAUNCH_AUTO || value > LAUNCH_MAXIMIZED)
		return E_INVALIDARG;
	launch_mode = value;
	return S_OK;
}

static HRESULT WINAPI statics_launch_size(void *iface, struct size *out)
{
	if (!out)
		return E_POINTER;
	*out = launch_size;
	return S_OK;
}

static HRESULT WINAPI set_statics_launch_size(void *iface, struct size value)
{
	if (value.width < 0 || value.height < 0)
		return E_INVALIDARG;
	launch_size = value;
	return S_OK;
}

static HRESULT WINAPI statics_clear_all(void *iface) { return S_OK; }
static HRESULT WINAPI statics_clear(void *iface, HSTRING key) { return S_OK; }

static HRESULT WINAPI interop_view_id(void *iface, IUnknown *core_window, INT32 *out)
{
	IUnknown *interop = NULL;
	HWND window = NULL;
	if (!out)
		return E_POINTER;
	*out = 0;
	if (!core_window || FAILED(core_window->lpVtbl->QueryInterface(core_window, &iid_core_window_interop, (void **)&interop)))
		return E_INVALIDARG;
	const HRESULT result = ((HRESULT (WINAPI *)(void *, HWND *))method(interop, 3))(interop, &window);
	release(interop);
	if (FAILED(result))
		return result;
	*out = id_of(window);
	return S_OK;
}

TABLE(class_factory_table, 0, FACTORY);
TABLE(class_statics_table, 1, INSPECTABLE, (const void *)statics_value, (const void *)statics_unsnap);
TABLE(class_statics2_table, 2, INSPECTABLE, (const void *)statics_for_view, (const void *)statics_terminate, (const void *)set_statics_terminate);
TABLE(class_statics3_table, 3, INSPECTABLE, (const void *)statics_launch_mode, (const void *)set_statics_launch_mode,
	(const void *)statics_launch_size, (const void *)set_statics_launch_size);
TABLE(class_statics4_table, 4, INSPECTABLE, (const void *)statics_clear_all, (const void *)statics_clear);
TABLE(class_interop_table, 5, INSPECTABLE, (const void *)interop_view_id);
static const struct face_iid class_iids[] = {{&iid_factory, 0}, {&iid_view_statics, 1}, {&iid_view_statics2, 2}, {&iid_view_statics3, 3},
	{&iid_view_statics4, 4}, {&iid_view_interop, 5}, {NULL, 0}};
static const struct kind class_kind = {"ApplicationView", L"Windows.UI.ViewManagement.ApplicationView", class_iids, NULL};
struct object application_view_class = {{class_factory_table.slots, class_statics_table.slots, class_statics2_table.slots,
	class_statics3_table.slots, class_statics4_table.slots, class_interop_table.slots}, &class_kind, 1};
