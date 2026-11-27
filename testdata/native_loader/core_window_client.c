// An app of the Windows Runtime as Visual Studio's DirectX template writes one, in C: CoreApplication.Run takes its view source, and the
// view sees, in order, Initialize with its application view, SetWindow with its core window, Load and Run, then Uninitialize. The view's
// Activated event comes while Run processes events, with the arguments of a launch, and the app activates its window there. Run then
// checks the window: its handle, bounds and dispatcher, work posted to the dispatcher, keys, characters, the mouse, a resize, and closing.
// Built with MSIX_VIEW, it is the app of a package and also checks its identifier there. The exit code names the first check that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllimport) HRESULT WINAPI RoInitialize(int type);
__declspec(dllimport) HRESULT WINAPI RoGetActivationFactory(void *name, const GUID *iid, void **factory);
__declspec(dllimport) HRESULT WINAPI WindowsCreateString(const WCHAR *text, UINT32 length, void **string);
__declspec(dllimport) const WCHAR *WINAPI WindowsGetStringRawBuffer(void *string, UINT32 *length);
__declspec(dllimport) HRESULT WINAPI WindowsDeleteString(void *string);

/* An interface pointer seen as its table of methods; the first six are IInspectable's. */
typedef struct {
	void **table;
} object;

#define METHOD(self, index, type) ((type)((object *)(self))->table[index])

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
static const GUID window_event = {0x272b1ef3, 0xc633, 0x4da5, {0xa2, 0x6c, 0xc6, 0xd0, 0xf5, 0x6b, 0x29, 0xda}};

struct rect {
	FLOAT x, y, width, height;
};

struct point {
	FLOAT x, y;
};

/* What the app saw, step by step. */
static int steps, failed;
static object *app_view, *window;
static int activated, sized, keys, characters, presses, visible, closed, dispatched, uninitialized;
static struct point pressed_at;
static BYTE left_pressed;
static FLOAT width_seen;

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
enum { ON_VIEW_ACTIVATED, ON_SIZE, ON_KEY, ON_CHARACTER, ON_PRESS, ON_VISIBILITY, ON_CLOSED, ON_DISPATCHED };

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
	on_visibility = {delegate_table, 1, ON_VISIBILITY}, on_closed = {delegate_table, 1, ON_CLOSED},
	on_dispatched = {dispatched_table, 1, ON_DISPATCHED};

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
	struct rect bounds = {0};
	BYTE shown = 0;
	if (METHOD(window, 7, HRESULT (WINAPI *)(object *, struct rect *))(window, &bounds) != S_OK || bounds.width <= 0 || bounds.height <= 0 ||
		METHOD(window, 17, flag_method)(window, &shown) != S_OK || !shown)
		fail(43);
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
	/* Closing the window ends ProcessUntilQuit. */
	if (METHOD(window, 19, HRESULT (WINAPI *)(object *))(window) != S_OK)
		fail(49);
	METHOD(dispatcher, 7, HRESULT (WINAPI *)(object *, INT32))(dispatcher, 2);
	if (closed != 1 || IsWindow(handle))
		fail(50);
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
