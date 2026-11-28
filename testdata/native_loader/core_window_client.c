// An app of the Windows Runtime as Visual Studio's DirectX template writes one, in C: CoreApplication.Run takes its view source, and the
// view sees, in order, Initialize with its application view, SetWindow with its core window, Load and Run, then Uninitialize. The view's
// Activated event comes while Run processes events, with the arguments of a launch, and the app activates its window there. Run then
// checks the window: its handle, bounds and dispatcher, work posted to the dispatcher, keys, characters, the mouse, a resize, its
// ApplicationView (the launch size asked for before Run, title, sizes, title bar, full screen, view modes), and closing.
// Built with SWAP_CHAIN, it also presents a Direct3D 11 frame through a swap chain made for its core window. Built with MSIX_VIEW, it is the app of a package and also checks
// its identifier there, its package, the folders of its data and its settings, which a second run reads back; built without, that it has neither. The exit code names the first check that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef SWAP_CHAIN
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_2.h>
#endif

__declspec(dllimport) HRESULT WINAPI RoInitialize(int type);
__declspec(dllimport) HRESULT WINAPI RoGetActivationFactory(void *name, const GUID *iid, void **factory);
__declspec(dllimport) HRESULT WINAPI WindowsCreateString(const WCHAR *text, UINT32 length, void **string);
__declspec(dllimport) const WCHAR *WINAPI WindowsGetStringRawBuffer(void *string, UINT32 *length);
__declspec(dllimport) HRESULT WINAPI WindowsDeleteString(void *string);
__declspec(dllimport) LONG WINAPI GetCurrentPackageFamilyName(UINT32 *length, WCHAR *name);
__declspec(dllimport) LONG WINAPI GetCurrentPackagePath(UINT32 *length, WCHAR *path);

/* An interface pointer seen as its table of methods; the first six are IInspectable's. */
typedef struct {
	void **table;
} object;

#define METHOD(self, index, type) ((type)((object *)(self))->table[index])

__declspec(dllimport) HRESULT WINAPI RoActivateInstance(void *name, object **instance);

typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *query_method)(object *, const GUID *, void **);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *event_method)(object *, object *, INT64 *);
typedef HRESULT (WINAPI *number_method)(object *, INT32 *);
typedef HRESULT (WINAPI *flag_method)(object *, BYTE *);

static const GUID core_application = {0x0aacf7a4, 0x5e1d, 0x49df, {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};
static const GUID core_window_static = {0x4d239005, 0x3c2a, 0x41b1, {0x90, 0x22, 0x53, 0x6b, 0xb9, 0xcf, 0x93, 0xb1}};
static const GUID core_window_interop = {0x45d64a29, 0xa63e, 0x4cb6, {0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f}};
static const GUID launch_activated = {0xfbc93e26, 0xa14a, 0x4b4f, {0x82, 0xb0, 0x33, 0xbe, 0xd9, 0x20, 0xaf, 0x52}};
static const GUID async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID display_statics = {0xc6a02a6c, 0xd452, 0x44dc, {0xba, 0x07, 0x96, 0xf3, 0xc6, 0xad, 0xf9, 0xd1}};
static const GUID display4 = {0xc972ce2f, 0x1242, 0x46be, {0xb5, 0x36, 0xe1, 0xaa, 0xfe, 0x9e, 0x7a, 0xcf}};
static const GUID package_statics = {0x4e534bdf, 0x2960, 0x4878, {0x97, 0xa4, 0x96, 0x24, 0xde, 0xb7, 0x2f, 0x2d}};
static const GUID data_statics = {0x5612147b, 0xe843, 0x45e3, {0x94, 0xd8, 0x06, 0x16, 0x9e, 0x3c, 0x8e, 0x17}};
static const GUID storage_item = {0x4207a996, 0xca2f, 0x42f7, {0xbd, 0xe8, 0x8b, 0x10, 0x45, 0x7a, 0x7f, 0x30}};
static const GUID window_event = {0x272b1ef3, 0xc633, 0x4da5, {0xa2, 0x6c, 0xc6, 0xd0, 0xf5, 0x6b, 0x29, 0xda}};
static const GUID view_iid = {0xd222d519, 0x4361, 0x451e, {0x96, 0xc4, 0x60, 0xf4, 0xf9, 0x74, 0x2d, 0xb0}};
static const GUID view2_iid = {0xe876b196, 0xa545, 0x40dc, {0xb5, 0x94, 0x45, 0x0c, 0xba, 0x68, 0xcc, 0x00}};
static const GUID view3_iid = {0x903c9ce5, 0x793a, 0x4fdf, {0xa2, 0xb2, 0xaf, 0x1a, 0xc2, 0x1e, 0x31, 0x08}};
static const GUID view4_iid = {0x15e5cbec, 0x9e0f, 0x46b5, {0xbc, 0x3f, 0x9b, 0xf6, 0x53, 0xe7, 0x4b, 0x5e}};
static const GUID view_statics2 = {0xaf338ae5, 0xcf64, 0x423c, {0x85, 0xe5, 0xf3, 0xe7, 0x24, 0x48, 0xfb, 0x23}};
static const GUID view_statics3 = {0xa28d7594, 0x8c41, 0x4e13, {0x97, 0x19, 0x51, 0x64, 0x79, 0x6f, 0xe4, 0xc7}};
static const GUID view_interop = {0xc446fb5d, 0x4793, 0x4896, {0xa8, 0xe2, 0xbe, 0x57, 0xa8, 0xbb, 0x0f, 0x50}};

struct rect {
	FLOAT x, y, width, height;
};

struct point {
	FLOAT x, y;
};

/* Windows.Foundation.Size, which goes by value as one eight-byte structure. */
struct size {
	FLOAT width, height;
};

/* What the app saw, step by step. */
static int steps, failed;
static object *app_view, *window;
static int activated, sized, keys, characters, presses, visible, closed, dispatched, uninitialized, data_changes;
static struct point pressed_at;
static BYTE left_pressed;
static FLOAT width_seen;
static int bounds_changes, consolidations;

static void fail(int code)
{
	if (!failed)
		failed = code;
}

static void *string(const WCHAR *text)
{
	void *made = NULL;
	WindowsCreateString(text, (UINT32)lstrlenW(text), &made);
	return made;
}

static object *statics(const WCHAR *name, const GUID *iid)
{
	void *class_name = string(name);
	object *found = NULL;
	HRESULT result = RoGetActivationFactory(class_name, iid, (void **)&found);
	WindowsDeleteString(class_name);
	return SUCCEEDED(result) ? found : NULL;
}

/* A delegate: Invoke is its fourth method, after IUnknown's. Each one knows which event it was given to. */
enum { ON_VIEW_ACTIVATED, ON_SIZE, ON_KEY, ON_CHARACTER, ON_PRESS, ON_VISIBILITY, ON_CLOSED, ON_DISPATCHED, ON_DATA_CHANGED, ON_BOUNDS,
	ON_CONSOLIDATED };

struct delegate {
	void **table;
	LONG refs;
	int which;
};

static HRESULT WINAPI delegate_query(struct delegate *self, const GUID *iid, void **out)
{
	*out = self;
	InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG WINAPI delegate_add(struct delegate *self) { return (ULONG)InterlockedIncrement(&self->refs); }
static ULONG WINAPI delegate_release(struct delegate *self) { return (ULONG)InterlockedDecrement(&self->refs); }

static HRESULT WINAPI delegate_invoke(struct delegate *self, object *sender, object *arguments)
{
	switch (self->which) {
	case ON_DATA_CHANGED:
		++data_changes;
		break;
	case ON_VIEW_ACTIVATED: {
		INT32 kind = -1;
		object *launch = NULL, *found = NULL;
		if (sender != app_view || METHOD(arguments, 6, number_method)(arguments, &kind) != S_OK || kind != 0)
			fail(30);
		if (METHOD(arguments, 0, query_method)(arguments, &launch_activated, (void **)&launch) != S_OK)
			fail(31);
		else {
			void *text = NULL;
			if (METHOD(launch, 6, HRESULT (WINAPI *)(object *, void **))(launch, &text) != S_OK)
				fail(32);
			WindowsDeleteString(text);
			METHOD(launch, 2, release_method)(launch);
		}
		/* The template finds the window of its thread and activates it. */
		object *window_statics = statics(L"Windows.UI.Core.CoreWindow", &core_window_static);
		if (!window_statics)
			fail(33);
		else if (METHOD(window_statics, 6, object_method)(window_statics, &found) != S_OK || found != window)
			fail(35);
		if (found) {
			if (METHOD(found, 18, HRESULT (WINAPI *)(object *))(found) != S_OK)
				fail(34);
			METHOD(found, 2, release_method)(found);
		}
		++activated;
		break;
	}
	case ON_SIZE: {
		FLOAT size[2] = {0};
		METHOD(arguments, 6, HRESULT (WINAPI *)(object *, FLOAT *))(arguments, size);
		width_seen = size[0];
		++sized;
		break;
	}
	case ON_KEY: {
		UINT32 key = 0;
		METHOD(arguments, 6, HRESULT (WINAPI *)(object *, UINT32 *))(arguments, &key);
		object *handled = NULL;
		if (key == VK_F5 && METHOD(arguments, 0, query_method)(arguments, &window_event, (void **)&handled) == S_OK) {
			METHOD(handled, 7, HRESULT (WINAPI *)(object *, BYTE))(handled, 1);
			METHOD(handled, 2, release_method)(handled);
			++keys;
		}
		break;
	}
	case ON_CHARACTER: {
		UINT32 code = 0;
		METHOD(arguments, 6, HRESULT (WINAPI *)(object *, UINT32 *))(arguments, &code);
		if (code == 'a')
			++characters;
		break;
	}
	case ON_PRESS: {
		object *point = NULL, *properties = NULL;
		if (METHOD(arguments, 6, object_method)(arguments, &point) == S_OK && point) {
			METHOD(point, 7, HRESULT (WINAPI *)(object *, struct point *))(point, &pressed_at);
			if (METHOD(point, 13, object_method)(point, &properties) == S_OK && properties) {
				METHOD(properties, 16, flag_method)(properties, &left_pressed);
				METHOD(properties, 2, release_method)(properties);
			}
			METHOD(point, 2, release_method)(point);
		}
		++presses;
		break;
	}
	case ON_VISIBILITY: {
		BYTE shown = 0;
		METHOD(arguments, 6, flag_method)(arguments, &shown);
		if (shown)
			++visible;
		break;
	}
	case ON_CLOSED:
		++closed;
		break;
	case ON_BOUNDS:
		++bounds_changes;
		break;
	case ON_CONSOLIDATED:
		++consolidations;
		break;
	}
	return S_OK;
}

/* The dispatcher's work takes no arguments. */
static HRESULT WINAPI dispatched_invoke(struct delegate *self)
{
	++dispatched;
	return S_OK;
}

static void *delegate_table[] = {delegate_query, delegate_add, delegate_release, delegate_invoke};
static void *dispatched_table[] = {delegate_query, delegate_add, delegate_release, dispatched_invoke};
static struct delegate on_view_activated = {delegate_table, 1, ON_VIEW_ACTIVATED}, on_size = {delegate_table, 1, ON_SIZE},
	on_key = {delegate_table, 1, ON_KEY}, on_character = {delegate_table, 1, ON_CHARACTER}, on_press = {delegate_table, 1, ON_PRESS},
	on_visibility = {delegate_table, 1, ON_VISIBILITY}, on_closed = {delegate_table, 1, ON_CLOSED}, on_data_changed = {delegate_table, 1, ON_DATA_CHANGED},
	on_dispatched = {dispatched_table, 1, ON_DISPATCHED}, on_bounds = {delegate_table, 1, ON_BOUNDS},
	on_consolidated = {delegate_table, 1, ON_CONSOLIDATED};

/* A boxed Windows.UI.Color, IReference<Color>, which the title bar keeps. */
struct color {
	BYTE a, r, g, b;
};
static HRESULT WINAPI color_value(object *self, struct color *out)
{
	*out = (struct color){0xff, 0x10, 0x60, 0xa0};
	return S_OK;
}
static void *color_table[] = {delegate_query, delegate_add, delegate_release, NULL, NULL, NULL, color_value};
static struct delegate boxed_color = {color_table, 1, 0};

/* The view's ApplicationView, on the thread that runs the view: the launch size asked for, its title in the caption, its identifier,
 * bounds and sizes, the title bar's colors, full screen and back, and the view modes. */
static int check_application_view(HWND handle)
{
	object *views = statics(L"Windows.UI.ViewManagement.ApplicationView", &view_statics2), *view = NULL, *view2 = NULL, *view3 = NULL,
		*view4 = NULL, *bar = NULL, *got = NULL;
	if (!views || METHOD(views, 6, object_method)(views, &view) != S_OK || !view) return 70;
	if (METHOD(view, 0, query_method)(view, &view2_iid, (void **)&view2) != S_OK || METHOD(view, 0, query_method)(view, &view3_iid,
		(void **)&view3) != S_OK || METHOD(view, 0, query_method)(view, &view4_iid, (void **)&view4) != S_OK) return 71;
	/* The title comes before the program's name in the caption. */
	void *title = string(L"Report");
	if (METHOD(view, 13, HRESULT (WINAPI *)(object *, void *))(view, title) != S_OK) return 72;
	WindowsDeleteString(title);
	WCHAR caption[MAX_PATH];
	GetWindowTextW(handle, caption, MAX_PATH);
	if (caption[0] != L'R' || caption[6] != L' ' || caption[7] != L'-' || caption[8] != L' ') return 73;
	void *read = NULL;
	if (METHOD(view, 14, HRESULT (WINAPI *)(object *, void **))(view, &read) != S_OK || lstrcmpW(WindowsGetStringRawBuffer(read, NULL), L"Report"))
		return 74;
	WindowsDeleteString(read);
	/* Its identifier is the one ApplicationViewInterop gives for the core window. */
	INT32 id = 0, interop_id = -1;
	object *interops = statics(L"Windows.UI.ViewManagement.ApplicationView", &view_interop);
	if (!interops || METHOD(view, 15, number_method)(view, &id) != S_OK ||
		METHOD(interops, 6, HRESULT (WINAPI *)(object *, object *, INT32 *))(interops, window, &interop_id) != S_OK || id != interop_id || !id)
		return 75;
	METHOD(interops, 2, release_method)(interops);
	/* Landscape, with the client area as its visible bounds, which change with the window. */
	INT32 orientation = -1;
	struct rect bounds = {0};
	INT64 bounds_token = 0, consolidated_token = 0;
	if (METHOD(view, 6, number_method)(view, &orientation) != S_OK || orientation != 0) return 76;
	if (METHOD(view2, 8, HRESULT (WINAPI *)(object *, struct rect *))(view2, &bounds) != S_OK || bounds.width != 640.0f || bounds.height != 480.0f)
		return 77;
	if (METHOD(view2, 9, event_method)(view2, (object *)&on_bounds, &bounds_token) != S_OK ||
		METHOD(view, 16, event_method)(view, (object *)&on_consolidated, &consolidated_token) != S_OK) return 78;
	/* TryResizeView sizes the client area, but not under the preferred minimum. */
	typedef HRESULT (WINAPI *resize_method)(object *, struct size, BYTE *);
	typedef HRESULT (WINAPI *size_method)(object *, struct size);
	BYTE done = 0;
	if (METHOD(view3, 13, resize_method)(view3, (struct size){700, 500}, &done) != S_OK || !done) return 79;
	METHOD(view2, 8, HRESULT (WINAPI *)(object *, struct rect *))(view2, &bounds);
	if (bounds.width != 700.0f || bounds.height != 500.0f || !bounds_changes) return 80;
	if (METHOD(view3, 14, size_method)(view3, (struct size){400, 300}) != S_OK ||
		METHOD(view3, 13, resize_method)(view3, (struct size){300, 200}, &done) != S_OK || done) return 81;
	/* The title bar keeps the colors it is given. */
	if (METHOD(view3, 6, object_method)(view3, &bar) != S_OK || !bar) return 82;
	if (METHOD(bar, 8, HRESULT (WINAPI *)(object *, object *))(bar, (object *)&boxed_color) != S_OK ||
		METHOD(bar, 9, object_method)(bar, &got) != S_OK || got != (object *)&boxed_color) return 83;
	METHOD(got, 2, release_method)(got);
	if (METHOD(bar, 8, HRESULT (WINAPI *)(object *, object *))(bar, NULL) != S_OK) return 84;
	/* Full screen: a borderless window over its monitor, and back. */
	BYTE full = 0;
	MONITORINFO monitor = {sizeof monitor};
	RECT frame;
	if (METHOD(view3, 9, flag_method)(view3, &full) != S_OK || full) return 85;
	if (METHOD(view3, 10, flag_method)(view3, &full) != S_OK || !full) return 86;
	GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
	GetWindowRect(handle, &frame);
	if (METHOD(view3, 9, flag_method)(view3, &full) != S_OK || !full || (GetWindowLongW(handle, GWL_STYLE) & WS_CAPTION) ||
		!EqualRect(&frame, &monitor.rcMonitor)) return 87;
	if (METHOD(view3, 11, HRESULT (WINAPI *)(object *))(view3) != S_OK || METHOD(view3, 9, flag_method)(view3, &full) != S_OK || full ||
		(GetWindowLongW(handle, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return 88;
	/* Only the default view mode. */
	BYTE supported = 0, compact = 1;
	if (METHOD(view4, 7, HRESULT (WINAPI *)(object *, INT32, BYTE *))(view4, 0, &supported) != S_OK || !supported ||
		METHOD(view4, 7, HRESULT (WINAPI *)(object *, INT32, BYTE *))(view4, 1, &compact) != S_OK || compact) return 89;
	METHOD(view2, 10, HRESULT (WINAPI *)(object *, INT64))(view2, bounds_token);
	METHOD(bar, 2, release_method)(bar);
	METHOD(view4, 2, release_method)(view4);
	METHOD(view3, 2, release_method)(view3);
	METHOD(view2, 2, release_method)(view2);
	METHOD(view, 2, release_method)(view);
	METHOD(views, 2, release_method)(views);
	return 0;
}

/* The view and its source, each an object of the Windows Runtime with one interface. */
static HRESULT WINAPI app_query(object *self, const GUID *iid, void **out)
{
	*out = self;
	return S_OK;
}

static ULONG WINAPI app_add(object *self) { return 2; }
static ULONG WINAPI app_release(object *self) { return 1; }

static HRESULT WINAPI app_iids(object *self, ULONG *count, GUID **iids)
{
	*count = 0;
	*iids = NULL;
	return S_OK;
}

static HRESULT WINAPI app_name(object *self, void **name)
{
	*name = NULL;
	return S_OK;
}

static HRESULT WINAPI app_trust(object *self, int *level)
{
	*level = 0;
	return S_OK;
}

static HRESULT WINAPI view_initialize(object *self, object *view)
{
	INT64 token;
	if (steps++ != 0)
		fail(10);
	app_view = view;
	if (METHOD(view, 7, event_method)(view, (object *)&on_view_activated, &token) != S_OK)
		fail(11);
	BYTE main = 0;
	if (METHOD(view, 9, flag_method)(view, &main) != S_OK || !main)
		fail(12);
	return S_OK;
}

static HRESULT WINAPI view_set_window(object *self, object *given)
{
	INT64 token;
	object *from_view = NULL;
	if (steps++ != 1)
		fail(13);
	window = given;
	if (METHOD(app_view, 6, object_method)(app_view, &from_view) != S_OK || from_view != given)
		fail(14);
	if (from_view)
		METHOD(from_view, 2, release_method)(from_view);
	if (METHOD(given, 54, event_method)(given, (object *)&on_size, &token) != S_OK ||
		METHOD(given, 34, event_method)(given, (object *)&on_key, &token) != S_OK ||
		METHOD(given, 28, event_method)(given, (object *)&on_character, &token) != S_OK ||
		METHOD(given, 46, event_method)(given, (object *)&on_press, &token) != S_OK ||
		METHOD(given, 56, event_method)(given, (object *)&on_visibility, &token) != S_OK ||
		METHOD(given, 30, event_method)(given, (object *)&on_closed, &token) != S_OK)
		fail(15);
	return S_OK;
}

static HRESULT WINAPI view_load(object *self, void *entry_point)
{
	if (steps++ != 2)
		fail(16);
	return S_OK;
}

/* ProcessEvents with ProcessAllIfPresent, a few times, as a game's loop calls it once a frame. */
static void frames(object *dispatcher, int count)
{
	for (int frame = 0; frame < count; ++frame) {
		METHOD(dispatcher, 7, HRESULT (WINAPI *)(object *, INT32))(dispatcher, 3);
		Sleep(5);
	}
}

static HRESULT WINAPI view_run(object *self)
{
	object *dispatcher = NULL, *interop = NULL;
	if (steps++ != 3)
		fail(17);
	if (METHOD(window, 9, object_method)(window, &dispatcher) != S_OK || !dispatcher) {
		fail(40);
		return S_OK;
	}
	for (int frame = 0; frame < 100 && !activated; ++frame)
		frames(dispatcher, 1);
	if (activated != 1 || !visible)
		fail(41);
	/* ICoreWindowInterop is what a swap chain is made for. */
	HWND handle = NULL;
	if (METHOD(window, 0, query_method)(window, &core_window_interop, (void **)&interop) != S_OK ||
		METHOD(interop, 3, HRESULT (WINAPI *)(object *, HWND *))(interop, &handle) != S_OK || !IsWindow(handle) || !IsWindowVisible(handle))
		fail(42);
	/* The view's display, as the template's device resources read it: DPI, scale, orientation and the screen's pixels. */
	object *displays = statics(L"Windows.Graphics.Display.DisplayInformation", &display_statics), *display = NULL, *display_size = NULL;
	FLOAT dpi = 0;
	INT32 scale = 0, orientation = 0;
	UINT32 screen_width = 0;
	INT64 dpi_token = 0;
	if (!displays || METHOD(displays, 6, object_method)(displays, &display) != S_OK || !display ||
		METHOD(display, 11, HRESULT (WINAPI *)(object *, FLOAT *))(display, &dpi) != S_OK || dpi < 96.0f ||
		METHOD(display, 10, number_method)(display, &scale) != S_OK || scale != (INT32)(dpi * 100 / 96) ||
		METHOD(display, 6, number_method)(display, &orientation) != S_OK || orientation != 1 ||
		METHOD(display, 14, event_method)(display, (object *)&on_closed, &dpi_token) != S_OK || !dpi_token ||
		METHOD(display, 0, query_method)(display, &display4, (void **)&display_size) != S_OK ||
		METHOD(display_size, 6, HRESULT (WINAPI *)(object *, UINT32 *))(display_size, &screen_width) != S_OK || !screen_width)
		fail(55);
	if (display)
		METHOD(display, 15, HRESULT (WINAPI *)(object *, INT64))(display, dpi_token);
	if (display_size)
		METHOD(display_size, 2, release_method)(display_size);
	if (display)
		METHOD(display, 2, release_method)(display);
	struct rect bounds = {0};
	BYTE shown = 0;
	if (METHOD(window, 7, HRESULT (WINAPI *)(object *, struct rect *))(window, &bounds) != S_OK || bounds.width <= 0 || bounds.height <= 0 ||
		METHOD(window, 17, flag_method)(window, &shown) != S_OK || !shown)
		fail(43);
	/* The window has the size the app asked for before Run. */
	if (bounds.width != 500.0f || bounds.height != 400.0f)
		fail(90);
#ifdef SWAP_CHAIN
	/* As the DirectX template makes its swap chain: flip model, two buffers, the size of the window's bounds. */
	static const GUID factory2 = {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
	HMODULE d3d11 = LoadLibraryW(L"d3d11.dll"), dxgi = LoadLibraryW(L"dxgi.dll");
	PFN_D3D11_CREATE_DEVICE create_device = d3d11 ? (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(d3d11, "D3D11CreateDevice") : NULL;
	HRESULT (WINAPI *create_factory)(const GUID *, void **) = dxgi ? (HRESULT (WINAPI *)(const GUID *, void **))(void *)GetProcAddress(dxgi,
		"CreateDXGIFactory1") : NULL;
	ID3D11Device *device = NULL;
	IDXGIFactory2 *factory = NULL;
	IDXGISwapChain1 *chain = NULL;
	if (!create_device || !create_factory || FAILED(create_device(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device,
			NULL, NULL)) || FAILED(create_factory(&factory2, (void **)&factory)))
		fail(60);
	else {
		DXGI_SWAP_CHAIN_DESC1 description = {(UINT)bounds.width, (UINT)bounds.height, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE, {1, 0},
			DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, DXGI_SCALING_NONE, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_ALPHA_MODE_IGNORE, 0};
		HRESULT made = IDXGIFactory2_CreateSwapChainForCoreWindow(factory, (IUnknown *)device, (IUnknown *)window, &description, NULL, &chain);
		if (made == E_NOTIMPL)
			fail(63);
		else if (FAILED(made) || !chain)
			fail(61);
		else if (FAILED(IDXGISwapChain1_Present(chain, 1, 0)))
			fail(62);
		if (chain)
			IDXGISwapChain1_Release(chain);
	}
	if (factory)
		IDXGIFactory2_Release(factory);
	if (device)
		ID3D11Device_Release(device);
#endif
	/* Work posted to the dispatcher runs when the app processes events, and its action completes. */
	object *action = NULL, *info = NULL;
	if (METHOD(dispatcher, 8, HRESULT (WINAPI *)(object *, INT32, object *, object **))(dispatcher, 0, (object *)&on_dispatched, &action) != S_OK ||
		!action || dispatched)
		fail(44);
	frames(dispatcher, 3);
	INT32 status = -1;
	if (dispatched != 1 || !action || METHOD(action, 0, query_method)(action, &async_info, (void **)&info) != S_OK ||
		METHOD(info, 7, number_method)(info, &status) != S_OK || status != 1)
		fail(45);
	/* Keys, a character and the mouse reach the window's events; a handled key is not the window's to answer. */
	PostMessageW(handle, WM_KEYDOWN, VK_F5, 0x003f0001);
	PostMessageW(handle, WM_CHAR, 'a', 0x001e0001);
	PostMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20));
	frames(dispatcher, 3);
	if (keys != 1 || characters != 1)
		fail(46);
	if (presses != 1 || pressed_at.x != 10.0f || pressed_at.y != 20.0f || !left_pressed)
		fail(47);
	/* Resizing the window raises SizeChanged with its new client width. */
	RECT frame = {0, 0, 640, 480};
	AdjustWindowRectEx(&frame, (DWORD)GetWindowLongW(handle, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(handle, GWL_EXSTYLE));
	const int before = sized;
	SetWindowPos(handle, NULL, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER);
	frames(dispatcher, 2);
	if (sized <= before || width_seen != 640.0f)
		fail(48);
	const int view_failed = check_application_view(handle);
	if (view_failed)
		fail(view_failed);
	/* Closing the window ends ProcessUntilQuit. */
	if (METHOD(window, 19, HRESULT (WINAPI *)(object *))(window) != S_OK)
		fail(49);
	METHOD(dispatcher, 7, HRESULT (WINAPI *)(object *, INT32))(dispatcher, 2);
	if (closed != 1 || IsWindow(handle))
		fail(50);
	if (consolidations != 1)
		fail(91);
	if (info)
		METHOD(info, 2, release_method)(info);
	if (action)
		METHOD(action, 2, release_method)(action);
	if (interop)
		METHOD(interop, 2, release_method)(interop);
	METHOD(dispatcher, 2, release_method)(dispatcher);
	return S_OK;
}

static HRESULT WINAPI view_uninitialize(object *self)
{
	if (steps++ != 4)
		fail(18);
	++uninitialized;
	return S_OK;
}

static void *view_table[] = {app_query, app_add, app_release, app_iids, app_name, app_trust, view_initialize, view_set_window, view_load,
	view_run, view_uninitialize};
static object view = {view_table};

static HRESULT WINAPI source_create(object *self, object **out)
{
	*out = &view;
	return S_OK;
}

static void *source_table[] = {app_query, app_add, app_release, app_iids, app_name, app_trust, source_create};
static object source = {source_table};

/* A storage item's path or name, at its place in IStorageItem, compared with what it should be. */
static BOOL item_text(object *folder, int slot, const WCHAR *expected)
{
	object *item = NULL;
	void *text = NULL;
	BOOL same = METHOD(folder, 0, query_method)(folder, &storage_item, (void **)&item) == S_OK &&
		METHOD(item, slot, HRESULT (WINAPI *)(object *, void **))(item, &text) == S_OK && !lstrcmpiW(WindowsGetStringRawBuffer(text, NULL), expected);
	WindowsDeleteString(text);
	if (item)
		METHOD(item, 2, release_method)(item);
	return same;
}

#ifdef MSIX_VIEW
static const GUID value_statics = {0x629bdbc8, 0xd932, 0x4ff4, {0x96, 0xb9, 0x8d, 0x96, 0xc5, 0xc1, 0xe8, 0x58}};
static const GUID property_value = {0x4bd682dd, 0x7554, 0x40e9, {0x9a, 0x9b, 0x82, 0x65, 0x4e, 0xde, 0x7e, 0x62}};
static const GUID string_object_map = {0x1b0d3570, 0x0877, 0x5ec2, {0x8a, 0x2c, 0x3b, 0x95, 0x39, 0x50, 0x6a, 0xca}};

static void *text_of(const WCHAR *text)
{
	void *string = NULL;
	WindowsCreateString(text, lstrlenW(text), &string);
	return string;
}

static void drop(object *self)
{
	if (self)
		METHOD(self, 2, release_method)(self);
}

/* The IMap<String, Object> of a property set or of a container's values. */
static object *map_of(object *values)
{
	object *map = NULL;
	return values && METHOD(values, 0, query_method)(values, &string_object_map, (void **)&map) == S_OK ? map : NULL;
}

static HRESULT insert(object *map, const WCHAR *key, object *value)
{
	void *name = text_of(key);
	BYTE replaced = 0;
	const HRESULT result = METHOD(map, 10, HRESULT (WINAPI *)(object *, void *, object *, BYTE *))(map, name, value, &replaced);
	WindowsDeleteString(name);
	return result;
}

/* The value of a key seen as an IPropertyValue, or NULL. */
static object *lookup(object *map, const WCHAR *key)
{
	void *name = text_of(key);
	object *value = NULL, *box = NULL;
	if (METHOD(map, 6, HRESULT (WINAPI *)(object *, void *, object **))(map, name, &value) == S_OK && value)
		METHOD(value, 0, query_method)(value, &property_value, (void **)&box);
	WindowsDeleteString(name);
	drop(value);
	return box;
}

static INT32 int_at(object *map, const WCHAR *key)
{
	object *box = lookup(map, key);
	INT32 number = -1;
	if (!box || METHOD(box, 11, number_method)(box, &number) != S_OK)
		number = -1;
	drop(box);
	return number;
}

static object *container_named(object *parent, const WCHAR *name, INT32 disposition, HRESULT *result)
{
	void *text = text_of(name);
	object *child = NULL;
	*result = METHOD(parent, 10, HRESULT (WINAPI *)(object *, void *, INT32, object **))(parent, text, disposition, &child);
	WindowsDeleteString(text);
	return child;
}

/* LocalSettings and RoamingSettings. The first run stores values of several kinds, a composite and a container; the run after it, which
 * finds the file the first one left in LocalState, reads them all back. */
static int check_settings(object *data, BOOL again)
{
	object *values_statics = statics(L"Windows.Foundation.PropertyValue", &value_statics);
	object *local = NULL, *roaming = NULL, *values = NULL, *map = NULL, *roaming_map = NULL, *roaming_values = NULL;
	INT32 locality = -1;
	if (!values_statics || METHOD(data, 10, object_method)(data, &local) != S_OK || METHOD(data, 11, object_method)(data, &roaming) != S_OK ||
		METHOD(local, 7, number_method)(local, &locality) != S_OK || locality != 0 ||
		METHOD(roaming, 7, number_method)(roaming, &locality) != S_OK || locality != 1 ||
		METHOD(local, 8, object_method)(local, &values) != S_OK || !(map = map_of(values)) ||
		METHOD(roaming, 8, object_method)(roaming, &roaming_values) != S_OK || !(roaming_map = map_of(roaming_values)))
		return 40;
	HRESULT result;
	if (!again) {
		object *number = NULL, *text = NULL, *list = NULL, *composite = NULL, *composite_map = NULL, *real = NULL, *small = NULL;
		void *hello = text_of(L"hello"), *items[2] = {text_of(L"a"), text_of(L"b")};
		void *composite_name = text_of(L"Windows.Storage.ApplicationDataCompositeValue");
		if (METHOD(values_statics, 10, HRESULT (WINAPI *)(object *, INT32, object **))(values_statics, 42, &number) != S_OK ||
			METHOD(values_statics, 18, HRESULT (WINAPI *)(object *, void *, object **))(values_statics, hello, &text) != S_OK ||
			METHOD(values_statics, 37, HRESULT (WINAPI *)(object *, UINT32, void **, object **))(values_statics, 2, items, &list) != S_OK ||
			METHOD(values_statics, 15, HRESULT (WINAPI *)(object *, double, object **))(values_statics, 1.5, &real) != S_OK ||
			METHOD(values_statics, 7, HRESULT (WINAPI *)(object *, BYTE, object **))(values_statics, 7, &small) != S_OK ||
			RoActivateInstance(composite_name, &composite) != S_OK || !(composite_map = map_of(composite)))
			return 41;
		if (insert(map, L"count", number) != S_OK || insert(map, L"name", text) != S_OK || insert(map, L"list", list) != S_OK ||
			insert(composite_map, L"x", real) != S_OK || insert(map, L"point", composite) != S_OK ||
			insert(roaming_map, L"r", number) != S_OK)
			return 42;
		/* Settings keep boxes and composites only, and a null removes its key. */
		if (insert(map, L"self", local) != (HRESULT)0x8007065e || insert(composite_map, L"nested", composite) != (HRESULT)0x8007065e ||
			insert(map, L"gone", number) != S_OK || insert(map, L"gone", NULL) != S_OK || int_at(map, L"gone") != -1)
			return 43;
		object *child = container_named(local, L"Child", 1, &result);
		if (result != (HRESULT)0x80070490 || child)
			return 44;
		child = container_named(local, L"Child", 0, &result);
		object *child_values = NULL, *child_map = NULL, *doomed = container_named(local, L"Doomed", 0, &result);
		if (!child || !doomed || METHOD(child, 8, object_method)(child, &child_values) != S_OK || !(child_map = map_of(child_values)) ||
			insert(child_map, L"deep", small) != S_OK)
			return 45;
		void *doomed_name = text_of(L"doomed");
		if (METHOD(local, 11, HRESULT (WINAPI *)(object *, void *))(local, doomed_name) != S_OK)
			return 46;
		WindowsDeleteString(doomed_name);
		drop(child_map);
		drop(child_values);
		drop(child);
		drop(doomed);
		drop(composite_map);
		drop(composite);
		drop(real);
		drop(small);
		drop(number);
		drop(text);
		drop(list);
		WindowsDeleteString(hello);
		WindowsDeleteString(items[0]);
		WindowsDeleteString(items[1]);
		WindowsDeleteString(composite_name);
	}
	/* What was stored reads back the same, in this run and in the next. */
	object *name = lookup(map, L"name"), *list = lookup(map, L"list"), *point = NULL, *point_map = NULL, *x = NULL;
	void *text = NULL, **items = NULL;
	UINT32 count = 0;
	double real = 0;
	if (int_at(map, L"count") != 42 || int_at(roaming_map, L"r") != 42 || !name || !list ||
		METHOD(name, 19, HRESULT (WINAPI *)(object *, void **))(name, &text) != S_OK || lstrcmpW(WindowsGetStringRawBuffer(text, NULL), L"hello") ||
		METHOD(list, 37, HRESULT (WINAPI *)(object *, UINT32 *, void ***))(list, &count, &items) != S_OK || count != 2 ||
		lstrcmpW(WindowsGetStringRawBuffer(items[0], NULL), L"a") || lstrcmpW(WindowsGetStringRawBuffer(items[1], NULL), L"b"))
		return again ? 50 : 47;
	void *point_key = text_of(L"point");
	if (METHOD(map, 6, HRESULT (WINAPI *)(object *, void *, object **))(map, point_key, &point) != S_OK || !(point_map = map_of(point)) ||
		!(x = lookup(point_map, L"x")) || METHOD(x, 16, HRESULT (WINAPI *)(object *, double *))(x, &real) != S_OK || real != 1.5)
		return again ? 51 : 48;
	object *child = container_named(local, L"child", 1, &result), *child_values = NULL, *child_map = NULL, *deep = NULL, *children = NULL;
	BYTE small = 0;
	UINT32 size = 0;
	if (!child || METHOD(child, 8, object_method)(child, &child_values) != S_OK || !(child_map = map_of(child_values)) ||
		!(deep = lookup(child_map, L"deep")) || METHOD(deep, 8, HRESULT (WINAPI *)(object *, BYTE *))(deep, &small) != S_OK || small != 7 ||
		METHOD(local, 9, object_method)(local, &children) != S_OK || METHOD(children, 7, HRESULT (WINAPI *)(object *, UINT32 *))(children, &size) != S_OK ||
		size != 1)
		return again ? 52 : 49;
	return 0;
}
#endif

/* The app's package and the folders of its data: a packaged app has both, any other process neither. */
static int check_package(void)
{
	object *packages = statics(L"Windows.ApplicationModel.Package", &package_statics), *package = NULL;
	object *datas = statics(L"Windows.Storage.ApplicationData", &data_statics), *data = NULL;
	if (!packages || !datas)
		return 20;
#ifdef MSIX_VIEW
	WCHAR family[128], path[MAX_PATH], local[MAX_PATH];
	UINT32 length = ARRAYSIZE(family);
	if (GetCurrentPackageFamilyName(&length, family) || (length = MAX_PATH, GetCurrentPackagePath(&length, path)))
		return 21;
	object *id = NULL, *location = NULL;
	void *text = NULL;
	if (METHOD(packages, 6, object_method)(packages, &package) != S_OK || METHOD(package, 6, object_method)(package, &id) != S_OK ||
		METHOD(id, 13, HRESULT (WINAPI *)(object *, void **))(id, &text) != S_OK || lstrcmpW(WindowsGetStringRawBuffer(text, NULL), family))
		return 22;
	WindowsDeleteString(text);
	if (METHOD(package, 7, object_method)(package, &location) != S_OK || !item_text(location, 12, path))
		return 23;
	/* LocalState under the user's local data, named so, and writable through its path. */
	object *folder = NULL;
	length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
	if (!length || length + 32 + lstrlenW(family) >= MAX_PATH)
		return 24;
	lstrcatW(local, L"\\Packages\\");
	lstrcatW(local, family);
	lstrcatW(local, L"\\LocalState");
	if (METHOD(datas, 6, object_method)(datas, &data) != S_OK || METHOD(data, 12, object_method)(data, &folder) != S_OK ||
		!item_text(folder, 12, local) || !item_text(folder, 11, L"LocalState"))
		return 25;
	lstrcatW(local, L"\\saved.txt");
	const int settings = check_settings(data, GetFileAttributesW(local) != INVALID_FILE_ATTRIBUTES);
	if (settings)
		return settings;
	HANDLE file = CreateFileW(local, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written = 0;
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, "saved", 5, &written, NULL) || written != 5)
		return 26;
	CloseHandle(file);
	/* SignalDataChanged reaches the handlers of DataChanged, and a removed one no longer. */
	INT64 token = 0;
	if (METHOD(data, 15, event_method)(data, (object *)&on_data_changed, &token) != S_OK ||
		METHOD(data, 17, HRESULT (WINAPI *)(object *))(data) != S_OK || data_changes != 1 ||
		METHOD(data, 16, HRESULT (WINAPI *)(object *, INT64))(data, token) != S_OK ||
		METHOD(data, 17, HRESULT (WINAPI *)(object *))(data) != S_OK || data_changes != 1)
		return 27;
	METHOD(folder, 2, release_method)(folder);
	METHOD(location, 2, release_method)(location);
	METHOD(id, 2, release_method)(id);
#else
	if (METHOD(packages, 6, object_method)(packages, &package) != (HRESULT)0x80073d54 || package ||
		METHOD(datas, 6, object_method)(datas, &data) != (HRESULT)0x80073d54 || data)
		return 28;
#endif
	return 0;
}

int raven_entry(void)
{
	if (FAILED(RoInitialize(0)))
		return 1;
	object *core = statics(L"Windows.ApplicationModel.Core.CoreApplication", &core_application);
	if (!core)
		return 2;
	object *current = NULL;
	if (METHOD(core, 12, object_method)(core, &current) != (HRESULT)0x80070490 || current)
		return 3;
#ifdef MSIX_VIEW
	/* Started from its package, the app knows its identifier there. */
	void *id = NULL;
	if (METHOD(core, 6, HRESULT (WINAPI *)(object *, void **))(core, &id) != S_OK || lstrcmpW(WindowsGetStringRawBuffer(id, NULL), L"View"))
		return 7;
	WindowsDeleteString(id);
#endif
	/* A thread that runs no view has no display. */
	object *displays = statics(L"Windows.Graphics.Display.DisplayInformation", &display_statics), *display = NULL;
	if (!displays || METHOD(displays, 6, object_method)(displays, &display) == S_OK || display)
		return 8;
	const int package = check_package();
	if (package)
		return package;
	/* A thread that runs no view has no ApplicationView; the launch size asked for now is the window's. */
	object *views = statics(L"Windows.UI.ViewManagement.ApplicationView", &view_statics2), *no_view = NULL;
	if (!views || METHOD(views, 6, object_method)(views, &no_view) != (HRESULT)0x80070490 || no_view)
		return 9;
	object *launch = statics(L"Windows.UI.ViewManagement.ApplicationView", &view_statics3);
	struct size size = {0};
	if (!launch || METHOD(launch, 9, HRESULT (WINAPI *)(object *, struct size))(launch, (struct size){500, 400}) != S_OK ||
		METHOD(launch, 7, HRESULT (WINAPI *)(object *, INT32))(launch, 1) != S_OK ||
		METHOD(launch, 8, HRESULT (WINAPI *)(object *, struct size *))(launch, &size) != S_OK || size.width != 500.0f || size.height != 400.0f)
		return 10;
	if (METHOD(core, 13, HRESULT (WINAPI *)(object *, object *))(core, &source) != S_OK)
		return 4;
	if (failed)
		return failed;
	if (steps != 5 || !uninitialized)
		return 5;
	if (METHOD(core, 12, object_method)(core, &current) != (HRESULT)0x80070490 || current)
		return 6;
	return 0;
}
