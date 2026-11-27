/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The view of an app of the Windows Runtime: CoreApplication.Run takes the app's view source, makes its view, and gives it a core window
 * that is a window of the desktop, as Windows.UI.dll gives one to every app it starts.
 *
 * - Windows.ApplicationModel.Core.CoreApplicationView: the main view of the thread that runs the app, with its window and dispatcher, and
 *   its Activated event raised once with the arguments of a launch from the start menu.
 * - Windows.UI.Core.CoreWindow: a top-level window, hidden until the app activates it, whose messages become the window's events: size,
 *   visibility, activation, keys and characters, the mouse as a pointer, and closing. Bounds and positions are in device-independent
 *   pixels, as the window's DPI makes them. The window's handle is what ICoreWindowInterop gives.
 * - Windows.UI.Core.CoreDispatcher: the message loop of the window's thread, which runs work posted to it in order with the window's own
 *   messages, and stops when the window is closed.
 * - Windows.UI.Core.CoreCursor: a system cursor by its type, which the window shows while the pointer is over it; no cursor hides it.
 * - Windows.Graphics.Display.DisplayInformation: the view's display, in landscape, at the DPI of the view's window, which is the
 *   monitor's; DpiChanged is raised when the window moves to a monitor of another DPI.
 * - IDXGIFactory2.CreateSwapChainForCoreWindow, when the DXGI the app loads does not make swap chains for core windows: the swap chain is
 *   made for the desktop window behind the core window, with the same description. */
#include "object.h"
#include <dxgi1_2.h>

static const IID iid_application_view = {0x638bb2db, 0x451d, 0x4661, {0xb0, 0x99, 0x41, 0x4f, 0x34, 0xff, 0xb9, 0xf1}};
static const IID iid_application_view2 = {0x68eb7adf, 0x917f, 0x48eb, {0x9a, 0xeb, 0x7d, 0xe5, 0x3e, 0x08, 0x6a, 0xb1}};
static const IID iid_application_view3 = {0x07ebe1b3, 0xa4cf, 0x4550, {0xab, 0x70, 0xb0, 0x7e, 0x85, 0x33, 0x0b, 0xc8}};
static const IID iid_core_window = {0x79b9d5f2, 0x879e, 0x4b89, {0xb7, 0x98, 0x79, 0xe4, 0x75, 0x98, 0x03, 0x0c}};
static const IID iid_core_window2 = {0x7c2b1b85, 0x6917, 0x4361, {0x9c, 0x02, 0x0d, 0x9e, 0x3a, 0x42, 0x0b, 0x95}};
static const IID iid_core_window_interop = {0x45d64a29, 0xa63e, 0x4cb6, {0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f}};
static const IID iid_core_window_static = {0x4d239005, 0x3c2a, 0x41b1, {0x90, 0x22, 0x53, 0x6b, 0xb9, 0xcf, 0x93, 0xb1}};
static const IID iid_dispatcher = {0x60db2fa8, 0xb705, 0x4fde, {0xa7, 0xd6, 0xeb, 0xbb, 0x18, 0x91, 0xd3, 0x9e}};
static const IID iid_dispatcher2 = {0x6f5e63c7, 0xe3aa, 0x4eae, {0xb0, 0xe0, 0xdc, 0xf3, 0x21, 0xca, 0x4b, 0x2f}};
static const IID iid_idle_arguments = {0x98bb6a24, 0xdc1c, 0x43cb, {0xb4, 0xed, 0xd1, 0xc0, 0xeb, 0x23, 0x91, 0xf3}};
static const IID iid_cursor = {0x96893acf, 0x111d, 0x442c, {0x8a, 0x77, 0xb8, 0x79, 0x92, 0xf8, 0xe2, 0xd6}};
static const IID iid_cursor_factory = {0xf6359621, 0xa79d, 0x4ed3, {0x8c, 0x32, 0xa9, 0xef, 0x9d, 0x6b, 0x76, 0xa4}};
static const IID iid_window_event = {0x272b1ef3, 0xc633, 0x4da5, {0xa2, 0x6c, 0xc6, 0xd0, 0xf5, 0x6b, 0x29, 0xda}};
static const IID iid_size_changed = {0x5a200ec7, 0x0426, 0x47dc, {0xb8, 0x6c, 0x6f, 0x47, 0x59, 0x15, 0xe4, 0x51}};
static const IID iid_visibility_changed = {0xbf9918ea, 0xd801, 0x4564, {0xa4, 0x95, 0xb1, 0xe8, 0x4f, 0x8a, 0xd0, 0x85}};
static const IID iid_window_activated = {0x179d65e7, 0x4658, 0x4cb6, {0xaa, 0x13, 0x41, 0xd0, 0x94, 0xea, 0x25, 0x5e}};
static const IID iid_key = {0x5ff5e930, 0x2544, 0x4a17, {0xbd, 0x78, 0x1f, 0x2f, 0xde, 0xbb, 0x10, 0x6b}};
static const IID iid_key2 = {0x583add98, 0x0790, 0x4571, {0x9b, 0x12, 0x64, 0x5e, 0xf9, 0xd7, 0x9e, 0x42}};
static const IID iid_character = {0xc584659f, 0x99b2, 0x4bcc, {0xbd, 0x33, 0x04, 0xe6, 0x3f, 0x42, 0x90, 0x2e}};
static const IID iid_pointer_event = {0x920d9cb1, 0xa5fc, 0x4a21, {0x8c, 0x09, 0x49, 0xdf, 0xe6, 0xff, 0xe2, 0x5f}};
static const IID iid_pointer_point = {0xe995317d, 0x7296, 0x42d9, {0x82, 0x33, 0xc5, 0xbe, 0x73, 0xb7, 0x4a, 0x4a}};
static const IID iid_pointer_properties = {0xc79d8a4b, 0xc163, 0x4ee7, {0x80, 0x3f, 0x67, 0xce, 0x79, 0xf9, 0x97, 0x2d}};
static const IID iid_pointer_device = {0x93c9bafc, 0xebcb, 0x467e, {0x82, 0xc6, 0x27, 0x6f, 0xea, 0xe3, 0x6b, 0x5a}};
static const IID iid_activated = {0xcf651713, 0xcd08, 0x4fd8, {0xb6, 0x97, 0xa2, 0x81, 0xb6, 0x54, 0x4e, 0x2e}};
static const IID iid_launch_activated = {0xfbc93e26, 0xa14a, 0x4b4f, {0x82, 0xb0, 0x33, 0xbe, 0xd9, 0x20, 0xaf, 0x52}};
static const IID iid_prelaunch_activated = {0x0c44717b, 0x19f7, 0x48d6, {0xb0, 0x46, 0xcf, 0x22, 0x82, 0x6e, 0xaa, 0x74}};
static const IID iid_view_activated = {0x930cef4b, 0xb829, 0x40fc, {0x88, 0xf4, 0x85, 0x13, 0xe8, 0xa6, 0x47, 0x38}};
static const IID iid_display_statics = {0xc6a02a6c, 0xd452, 0x44dc, {0xba, 0x07, 0x96, 0xf3, 0xc6, 0xad, 0xf9, 0xd1}};
static const IID iid_display = {0xbed112ae, 0xadc3, 0x4dc9, {0xae, 0x65, 0x85, 0x1f, 0x4d, 0x7d, 0x47, 0x99}};
static const IID iid_display2 = {0x4dcd0021, 0xfad1, 0x4b8e, {0x8e, 0xdf, 0x77, 0x58, 0x87, 0xb8, 0xbf, 0x19}};
static const IID iid_display3 = {0xdb15011d, 0x0f09, 0x4466, {0x8f, 0xf3, 0x11, 0xde, 0x9a, 0x3c, 0x92, 0x9a}};
static const IID iid_display4 = {0xc972ce2f, 0x1242, 0x46be, {0xb5, 0x36, 0xe1, 0xaa, 0xfe, 0x9e, 0x7a, 0xcf}};
static const IID iid_dxgi_factory2 = {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
static const IID iid_async_action = {0x5a648006, 0x843a, 0x4da9, {0x86, 0x5b, 0x9d, 0x26, 0xe5, 0xdf, 0xad, 0x7b}};

/* Windows.Foundation's structures and the core window's key status, as the Windows Runtime lays them out. */
struct rect {
	FLOAT x, y, width, height;
};

struct size {
	FLOAT width, height;
};

struct point {
	FLOAT x, y;
};

struct key_status {
	UINT32 repeat_count, scan_code;
	boolean extended, menu_down, was_down, released;
};

/* A method called before the object is ready for it. */
#define ILLEGAL_CALL ((HRESULT)0x8000000e)

/* What Windows.UI.Core numbers. */
enum {
	PROCESS_ONE_AND_ALL_PENDING,
	PROCESS_ONE_IF_PRESENT,
	PROCESS_UNTIL_QUIT,
	PROCESS_ALL_IF_PRESENT
};

enum {
	ACTIVATED_BY_CODE,
	DEACTIVATED,
	ACTIVATED_BY_POINTER
};

enum {
	ASYNC_STARTED,
	ASYNC_COMPLETED,
	ASYNC_CANCELED,
	ASYNC_ERROR
};

/* Calls through a delegate: its Invoke is the fourth method of its table. */
typedef HRESULT (WINAPI *typed_handler)(void *handler, void *sender, void *arguments);
typedef HRESULT (WINAPI *plain_handler)(void *handler);
typedef HRESULT (WINAPI *one_argument_handler)(void *handler, void *argument);
typedef HRESULT (WINAPI *action_handler)(void *handler, void *action, INT32 status);

static void *method(void *iface, size_t slot) { return (*(void ***)iface)[slot]; }

/* The handlers of one event, each with its token. Raising an event calls a copy of the list, so a handler may add or remove handlers. */
struct handler_entry {
	INT64 token;
	IUnknown *handler;
};

struct handlers {
	struct handler_entry *entries;
	UINT32 count, capacity;
};

static HRESULT handlers_add(struct handlers *list, void *handler, INT64 *token)
{
	if (!handler || !token)
		return E_INVALIDARG;
	if (list->count == list->capacity) {
		const UINT32 capacity = list->capacity ? list->capacity * 2 : 4;
		struct handler_entry *grown = list->entries ? HeapReAlloc(GetProcessHeap(), 0, list->entries, capacity * sizeof *grown)
			: ALLOC(capacity * sizeof *grown);
		if (!grown)
			return E_OUTOFMEMORY;
		list->entries = grown;
		list->capacity = capacity;
	}
	reference(handler);
	list->entries[list->count].handler = handler;
	list->entries[list->count++].token = *token = InterlockedIncrement(&tokens);
	return S_OK;
}

static HRESULT handlers_remove(struct handlers *list, INT64 token)
{
	for (UINT32 index = 0; index < list->count; ++index)
		if (list->entries[index].token == token) {
			release(list->entries[index].handler);
			list->entries[index] = list->entries[--list->count];
			return S_OK;
		}
	return S_OK;
}

static void handlers_raise(struct handlers *list, void *sender, void *arguments)
{
	const UINT32 count = list->count;
	if (!count)
		return;
	IUnknown **called = ALLOC(count * sizeof *called);
	if (!called)
		return;
	for (UINT32 index = 0; index < count; ++index)
		reference(called[index] = list->entries[index].handler);
	for (UINT32 index = 0; index < count; ++index) {
		((typed_handler)method(called[index], 3))(called[index], sender, arguments);
		release(called[index]);
	}
	FREE(called);
}

static void handlers_clear(struct handlers *list)
{
	for (UINT32 index = 0; index < list->count; ++index)
		release(list->entries[index].handler);
	FREE(list->entries);
	list->entries = NULL;
	list->count = list->capacity = 0;
}

/* The pixels of the window's DPI in device-independent pixels, of which there are 96 to an inch. */
static FLOAT dips(HWND window, LONG pixels)
{
	static UINT (WINAPI *dpi_for_window)(HWND);
	static BOOL looked;
	if (!looked) {
		dpi_for_window = (UINT (WINAPI *)(HWND))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
		looked = TRUE;
	}
	const UINT dpi = dpi_for_window && window ? dpi_for_window(window) : 96;
	return (FLOAT)pixels * 96.0f / (FLOAT)(dpi ? dpi : 96);
}

static UINT32 key_modifiers(void)
{
	/* VirtualKeyModifiers: Control, Menu, Shift and Windows. */
	return (GetKeyState(VK_CONTROL) < 0 ? 1 : 0) | (GetKeyState(VK_MENU) < 0 ? 2 : 0) | (GetKeyState(VK_SHIFT) < 0 ? 4 : 0) |
		(GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0 ? 8 : 0);
}

/* Windows.Foundation.IAsyncAction for work posted to a dispatcher: started when it is posted, finished when the work has run. Its
 * completion handler is called when it finishes, or at once when it is set on one that has. */
struct action {
	struct object base;
	LONG status;
	HRESULT error;
	IUnknown *work;
	IUnknown *handler;
	BOOL idle;
};

static void destroy_action(struct object *base)
{
	struct action *self = (struct action *)base;
	release(self->work);
	release(self->handler);
}

static void action_finish(struct action *self, LONG status, HRESULT error)
{
	self->error = error;
	InterlockedExchange(&self->status, status);
	IUnknown *handler = InterlockedCompareExchangePointer((void **)&self->handler, NULL, NULL);
	if (handler)
		((action_handler)method(handler, 3))(handler, face_of(&self->base, 0), status);
}

static HRESULT WINAPI action_set_completed(void *iface, IUnknown *handler)
{
	struct action *self = (struct action *)owner(iface);
	if (!handler)
		return E_INVALIDARG;
	if (InterlockedCompareExchangePointer((void **)&self->handler, handler, NULL))
		return DELEGATE_ASSIGNED;
	reference(handler);
	const LONG status = InterlockedCompareExchange(&self->status, 0, 0);
	if (status != ASYNC_STARTED)
		((action_handler)method(handler, 3))(handler, iface, status);
	return S_OK;
}

static HRESULT WINAPI action_completed(void *iface, IUnknown **out)
{
	struct action *self = (struct action *)owner(iface);
	if (!out)
		return E_POINTER;
	reference(*out = self->handler);
	return S_OK;
}

static HRESULT WINAPI action_results(void *iface)
{
	struct action *self = (struct action *)owner(iface);
	const LONG status = InterlockedCompareExchange(&self->status, 0, 0);
	return status == ASYNC_COMPLETED ? S_OK : status == ASYNC_ERROR ? self->error : ILLEGAL_CALL;
}

static HRESULT WINAPI action_id(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = (UINT32)(ULONG_PTR)owner(iface);
	return S_OK;
}

static HRESULT WINAPI action_status(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = InterlockedCompareExchange(&((struct action *)owner(iface))->status, 0, 0);
	return S_OK;
}

static HRESULT WINAPI action_error(void *iface, HRESULT *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct action *)owner(iface))->error;
	return S_OK;
}

static HRESULT WINAPI action_cancel(void *iface)
{
	InterlockedCompareExchange(&((struct action *)owner(iface))->status, ASYNC_CANCELED, ASYNC_STARTED);
	return S_OK;
}

static HRESULT WINAPI action_close(void *iface) { return S_OK; }

TABLE(action_table, 0, INSPECTABLE, (const void *)action_set_completed, (const void *)action_completed, (const void *)action_results);
TABLE(action_info_table, 1, INSPECTABLE, (const void *)action_id, (const void *)action_status, (const void *)action_error,
	(const void *)action_cancel, (const void *)action_close);
static const struct face_iid action_iids[] = {{&iid_async_action, 0}, {&iid_async_info, 1}, {NULL, 0}};
static const struct kind action_kind = {"IAsyncAction", L"Windows.Foundation.IAsyncAction", action_iids, destroy_action};

/* Windows.UI.Core.IdleDispatchedHandlerArgs: the dispatcher runs idle work only when nothing else waits. */
static HRESULT WINAPI idle_is_idle(void *iface, boolean *out) { return answer_flag(out, GetQueueStatus(QS_ALLINPUT) == 0); }

TABLE(idle_table, 0, INSPECTABLE, (const void *)idle_is_idle);
static const struct face_iid idle_iids[] = {{&iid_idle_arguments, 0}, {NULL, 0}};
static void no_destroy(struct object *base) {}

static const struct kind idle_kind = {"IdleDispatchedHandlerArgs", L"Windows.UI.Core.IdleDispatchedHandlerArgs", idle_iids, no_destroy};

/* Windows.UI.Core.CoreCursor: a system cursor of a type, or a cursor of the app's resources by its number. */
struct cursor {
	struct object base;
	INT32 type;
	UINT32 id;
};

static HRESULT WINAPI cursor_id(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct cursor *)owner(iface))->id;
	return S_OK;
}

static HRESULT WINAPI cursor_type(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct cursor *)owner(iface))->type;
	return S_OK;
}

TABLE(cursor_table, 0, INSPECTABLE, (const void *)cursor_id, (const void *)cursor_type);
static const struct face_iid cursor_iids[] = {{&iid_cursor, 0}, {NULL, 0}};
static const struct kind cursor_kind = {"CoreCursor", L"Windows.UI.Core.CoreCursor", cursor_iids, no_destroy};

static HRESULT WINAPI cursor_create(void *iface, INT32 type, UINT32 id, void **out)
{
	static const void *const *const tables[] = {cursor_table.slots};
	if (!out)
		return E_POINTER;
	if (type < 0 || type > 15)
		return E_INVALIDARG;
	struct cursor *made = make(sizeof *made, &cursor_kind, tables, 1);
	if (!(*out = made ? face_of(&made->base, 0) : NULL))
		return E_OUTOFMEMORY;
	made->type = type;
	made->id = id;
	return S_OK;
}

/* The cursor a CoreCursor stands for: CoreCursorType in order, a custom one from the program's resources. */
static HCURSOR system_cursor(IUnknown *cursor)
{
	static const WORD names[] = {32512, 32515, 0, 32649, 32651, 32513, 32646, 32643, 32645, 32642, 32644, 32648, 32516, 32514, 32671, 32672};
	INT32 type = 0;
	UINT32 id = 0;
	((HRESULT (WINAPI *)(void *, INT32 *))method(cursor, 7))(cursor, &type);
	if (type == 2) {
		((HRESULT (WINAPI *)(void *, UINT32 *))method(cursor, 6))(cursor, &id);
		HCURSOR own = LoadCursorW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(id));
		return own ? own : LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
	}
	return LoadCursorW(NULL, MAKEINTRESOURCEW(type >= 0 && type < 16 && names[type] ? names[type] : 32512));
}

TABLE(cursor_factory_table, 0, FACTORY);
TABLE(cursor_statics_table, 1, INSPECTABLE, (const void *)cursor_create);
static const struct face_iid cursor_factory_iids[] = {{&iid_factory, 0}, {&iid_cursor_factory, 1}, {NULL, 0}};
static const struct kind cursor_factory_kind = {"CoreCursor", L"Windows.UI.Core.CoreCursor", cursor_factory_iids, NULL};
struct object core_cursor_class = {{cursor_factory_table.slots, cursor_statics_table.slots}, &cursor_factory_kind, 1};

/* The window, its dispatcher and its view: one of each for the thread that runs the app. */
struct window;

struct dispatcher {
	struct object base;
	struct window *window;
	DWORD thread;
	BOOL quit;
};

struct window {
	struct object base;
	HWND handle;
	DWORD thread;
	struct dispatcher *dispatcher;
	IUnknown *cursor;
	INT32 flow_direction;
	BOOL closed, pointer_inside, activated;
	UINT32 frame;
	BYTE buttons;
	struct handlers activated_handlers, automation, character, closed_handlers, input_enabled, key_down, key_up, capture_lost, entered,
		exited, moved, pressed, released, touch_hit, wheel, size_changed, visibility;
};

struct display;

struct view {
	struct object base;
	struct window *window;
	struct display *display;
	struct handlers activated, hosted_closing;
};

static UINT run_message, activate_message;
static DWORD current_thread;
static struct view *current_view;

static struct window *window_of(void *iface) { return (struct window *)owner(iface); }

/* Arguments of the window's events: the event's own interface at place 0 and ICoreWindowEventArgs, whose Handled the app sets, at 1. */
struct arguments {
	struct object base;
	boolean handled;
	union {
		struct size size;
		boolean visible;
		INT32 activation;
		struct {
			UINT32 key;
			struct key_status status;
		} key;
		struct {
			IUnknown *point;
			UINT32 modifiers;
		} pointer;
	};
};

static void destroy_arguments(struct object *base) {}

static void destroy_pointer_arguments(struct object *base) { release(((struct arguments *)base)->pointer.point); }

static HRESULT WINAPI arguments_handled(void *iface, boolean *out) { return answer_flag(out, ((struct arguments *)owner(iface))->handled); }

static HRESULT WINAPI arguments_set_handled(void *iface, boolean value)
{
	((struct arguments *)owner(iface))->handled = value;
	return S_OK;
}

static HRESULT WINAPI arguments_size(void *iface, struct size *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct arguments *)owner(iface))->size;
	return S_OK;
}

static HRESULT WINAPI arguments_visible(void *iface, boolean *out) { return answer_flag(out, ((struct arguments *)owner(iface))->visible); }

static HRESULT WINAPI arguments_activation(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct arguments *)owner(iface))->activation;
	return S_OK;
}

static HRESULT WINAPI arguments_key(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct arguments *)owner(iface))->key.key;
	return S_OK;
}

static HRESULT WINAPI arguments_key_status(void *iface, struct key_status *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct arguments *)owner(iface))->key.status;
	return S_OK;
}

/* The keyboard's identifier: the one keyboard of the desktop. */
static HRESULT WINAPI arguments_device(void *iface, HSTRING *out)
{
	static const WCHAR keyboard[] = L"\\\\?\\HID#keyboard";
	if (!out)
		return E_POINTER;
	return WindowsCreateString(keyboard, ARRAYSIZE(keyboard) - 1, out);
}

static HRESULT WINAPI arguments_point(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = ((struct arguments *)owner(iface))->pointer.point);
	return S_OK;
}

static HRESULT WINAPI arguments_modifiers(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct arguments *)owner(iface))->pointer.modifiers;
	return S_OK;
}

static HRESULT WINAPI arguments_intermediate(void *iface, void **out)
{
	answer_null(out);
	return missing("PointerEventArgs.GetIntermediatePoints");
}

TABLE(event_table, 0, INSPECTABLE, (const void *)arguments_handled, (const void *)arguments_set_handled);
TABLE(handled_table, 1, INSPECTABLE, (const void *)arguments_handled, (const void *)arguments_set_handled);
TABLE(size_table, 0, INSPECTABLE, (const void *)arguments_size);
TABLE(visibility_table, 0, INSPECTABLE, (const void *)arguments_visible);
TABLE(activation_table, 0, INSPECTABLE, (const void *)arguments_activation);
TABLE(key_table, 0, INSPECTABLE, (const void *)arguments_key, (const void *)arguments_key_status);
TABLE(key2_table, 2, INSPECTABLE, (const void *)arguments_device);
TABLE(character_table, 0, INSPECTABLE, (const void *)arguments_key, (const void *)arguments_key_status);
TABLE(pointer_table, 0, INSPECTABLE, (const void *)arguments_point, (const void *)arguments_modifiers, (const void *)arguments_intermediate);

static const struct face_iid event_iids[] = {{&iid_window_event, 0}, {NULL, 0}};
static const struct face_iid size_iids[] = {{&iid_size_changed, 0}, {&iid_window_event, 1}, {NULL, 0}};
static const struct face_iid visibility_iids[] = {{&iid_visibility_changed, 0}, {&iid_window_event, 1}, {NULL, 0}};
static const struct face_iid activation_iids[] = {{&iid_window_activated, 0}, {&iid_window_event, 1}, {NULL, 0}};
static const struct face_iid key_iids[] = {{&iid_key, 0}, {&iid_window_event, 1}, {&iid_key2, 2}, {NULL, 0}};
static const struct face_iid character_iids[] = {{&iid_character, 0}, {&iid_window_event, 1}, {NULL, 0}};
static const struct face_iid pointer_iids[] = {{&iid_pointer_event, 0}, {&iid_window_event, 1}, {NULL, 0}};
static const struct kind event_kind = {"CoreWindowEventArgs", L"Windows.UI.Core.CoreWindowEventArgs", event_iids, destroy_arguments};
static const struct kind size_kind = {"WindowSizeChangedEventArgs", L"Windows.UI.Core.WindowSizeChangedEventArgs", size_iids,
	destroy_arguments};
static const struct kind visibility_kind = {"VisibilityChangedEventArgs", L"Windows.UI.Core.VisibilityChangedEventArgs", visibility_iids,
	destroy_arguments};
static const struct kind activation_kind = {"WindowActivatedEventArgs", L"Windows.UI.Core.WindowActivatedEventArgs", activation_iids,
	destroy_arguments};
static const struct kind key_kind = {"KeyEventArgs", L"Windows.UI.Core.KeyEventArgs", key_iids, destroy_arguments};
static const struct kind character_kind = {"CharacterReceivedEventArgs", L"Windows.UI.Core.CharacterReceivedEventArgs", character_iids,
	destroy_arguments};
static const struct kind pointer_kind = {"PointerEventArgs", L"Windows.UI.Core.PointerEventArgs", pointer_iids, destroy_pointer_arguments};

static struct arguments *new_arguments(const struct kind *kind, const void *const *own)
{
	const void *const *const tables[] = {own, handled_table.slots, key2_table.slots};
	return make(sizeof(struct arguments), kind, tables, kind == &key_kind ? 3 : kind == &event_kind ? 1 : 2);
}

/* Raises an event of the window with arguments made for it, and answers whether a handler marked them handled. */
static BOOL raise(struct window *self, struct handlers *list, struct arguments *arguments)
{
	if (!arguments)
		return FALSE;
	reference(face_of(&self->base, 0));
	handlers_raise(list, face_of(&self->base, 0), face_of(&arguments->base, 0));
	const BOOL handled = arguments->handled;
	release(face_of(&arguments->base, 0));
	release(face_of(&self->base, 0));
	return handled;
}

/* Windows.UI.Input.PointerPoint for the mouse, with its PointerPointProperties at place 1, and the mouse as Windows.Devices.Input.
 * PointerDevice. */
struct pointer_point {
	struct object base;
	struct point position;
	UINT32 frame;
	UINT64 timestamp;
	BYTE buttons;
	INT32 wheel;
	boolean horizontal;
	INT32 update;
};

static void destroy_point(struct object *base) {}

static HRESULT WINAPI device_type(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	/* PointerDeviceType.Mouse */
	*out = 2;
	return S_OK;
}

static HRESULT WINAPI device_integrated(void *iface, boolean *out) { return answer_flag(out, FALSE); }

static HRESULT WINAPI device_contacts(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI device_rect(void *iface, struct rect *out)
{
	if (!out)
		return E_POINTER;
	*out = (struct rect){0, 0, (FLOAT)GetSystemMetrics(SM_CXVIRTUALSCREEN), (FLOAT)GetSystemMetrics(SM_CYVIRTUALSCREEN)};
	return S_OK;
}

static HRESULT WINAPI device_usages(void *iface, void **out)
{
	answer_null(out);
	return missing("PointerDevice.SupportedUsages");
}

TABLE(device_table, 0, INSPECTABLE, (const void *)device_type, (const void *)device_integrated, (const void *)device_contacts,
	(const void *)device_rect, (const void *)device_rect, (const void *)device_usages);
static const struct face_iid device_iids[] = {{&iid_pointer_device, 0}, {NULL, 0}};
static const struct kind device_kind = {"PointerDevice", L"Windows.Devices.Input.PointerDevice", device_iids, NULL};
static struct object mouse_device = {{device_table.slots}, &device_kind, 1};

static struct pointer_point *point_of(void *iface) { return (struct pointer_point *)owner(iface); }

static HRESULT WINAPI point_device(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = face_of(&mouse_device, 0);
	return S_OK;
}

static HRESULT WINAPI point_position(void *iface, struct point *out)
{
	if (!out)
		return E_POINTER;
	*out = point_of(iface)->position;
	return S_OK;
}

static HRESULT WINAPI point_id(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI point_frame(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = point_of(iface)->frame;
	return S_OK;
}

static HRESULT WINAPI point_timestamp(void *iface, UINT64 *out)
{
	if (!out)
		return E_POINTER;
	*out = point_of(iface)->timestamp;
	return S_OK;
}

static HRESULT WINAPI point_in_contact(void *iface, boolean *out) { return answer_flag(out, point_of(iface)->buttons != 0); }

static HRESULT WINAPI point_properties(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = face_of(&point_of(iface)->base, 1));
	return S_OK;
}

static HRESULT WINAPI properties_pressure(void *iface, FLOAT *out)
{
	if (!out)
		return E_POINTER;
	*out = point_of(iface)->buttons ? 0.5f : 0.0f;
	return S_OK;
}

static HRESULT WINAPI properties_false(void *iface, boolean *out) { return answer_flag(out, FALSE); }
static HRESULT WINAPI properties_true(void *iface, boolean *out) { return answer_flag(out, TRUE); }

static HRESULT WINAPI properties_zero(void *iface, FLOAT *out)
{
	if (!out)
		return E_POINTER;
	*out = 0;
	return S_OK;
}

static HRESULT WINAPI properties_contact(void *iface, struct rect *out)
{
	if (!out)
		return E_POINTER;
	*out = (struct rect){point_of(iface)->position.x, point_of(iface)->position.y, 0, 0};
	return S_OK;
}

static HRESULT button(void *iface, boolean *out, BYTE mask) { return answer_flag(out, (point_of(iface)->buttons & mask) != 0); }
static HRESULT WINAPI properties_left(void *iface, boolean *out) { return button(iface, out, 1); }
static HRESULT WINAPI properties_right(void *iface, boolean *out) { return button(iface, out, 2); }
static HRESULT WINAPI properties_middle(void *iface, boolean *out) { return button(iface, out, 4); }
static HRESULT WINAPI properties_x1(void *iface, boolean *out) { return button(iface, out, 8); }
static HRESULT WINAPI properties_x2(void *iface, boolean *out) { return button(iface, out, 16); }

static HRESULT WINAPI properties_wheel(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = point_of(iface)->wheel;
	return S_OK;
}

static HRESULT WINAPI properties_horizontal(void *iface, boolean *out) { return answer_flag(out, point_of(iface)->horizontal); }

static HRESULT WINAPI properties_update(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = point_of(iface)->update;
	return S_OK;
}

static HRESULT WINAPI properties_has_usage(void *iface, UINT32 page, UINT32 id, boolean *out) { return answer_flag(out, FALSE); }

static HRESULT WINAPI properties_usage(void *iface, UINT32 page, UINT32 id, INT32 *out)
{
	if (out)
		*out = 0;
	return E_INVALIDARG;
}

TABLE(point_table, 0, INSPECTABLE, (const void *)point_device, (const void *)point_position, (const void *)point_position,
	(const void *)point_id, (const void *)point_frame, (const void *)point_timestamp, (const void *)point_in_contact,
	(const void *)point_properties);
TABLE(properties_table, 1, INSPECTABLE, (const void *)properties_pressure, (const void *)properties_false, (const void *)properties_false,
	(const void *)properties_zero, (const void *)properties_zero, (const void *)properties_zero, (const void *)properties_zero,
	(const void *)properties_contact, (const void *)properties_contact, (const void *)properties_false, (const void *)properties_left,
	(const void *)properties_right, (const void *)properties_middle, (const void *)properties_wheel, (const void *)properties_horizontal,
	(const void *)properties_true, (const void *)properties_true, (const void *)properties_false, (const void *)properties_false,
	(const void *)properties_x1, (const void *)properties_x2, (const void *)properties_update, (const void *)properties_has_usage,
	(const void *)properties_usage);
static const struct face_iid point_iids[] = {{&iid_pointer_point, 0}, {&iid_pointer_properties, 1}, {NULL, 0}};
static const struct kind point_kind = {"PointerPoint", L"Windows.UI.Input.PointerPoint", point_iids, destroy_point};

/* Raises a pointer event for a mouse message. The buttons are the left, right, middle and the two extra ones, in that order of bits. */
static BOOL raise_pointer(struct window *self, struct handlers *list, LPARAM where, INT32 update, INT32 wheel, BOOL horizontal)
{
	static const void *const *const point_tables[] = {point_table.slots, properties_table.slots};
	struct pointer_point *point = make(sizeof *point, &point_kind, point_tables, 2);
	struct arguments *arguments = point ? new_arguments(&pointer_kind, pointer_table.slots) : NULL;
	if (!arguments) {
		if (point)
			release(face_of(&point->base, 0));
		return FALSE;
	}
	POINT at = {(SHORT)LOWORD(where), (SHORT)HIWORD(where)};
	/* The wheel's position is on the screen. */
	if (wheel)
		ScreenToClient(self->handle, &at);
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	point->position = (struct point){dips(self->handle, at.x), dips(self->handle, at.y)};
	point->frame = ++self->frame;
	point->timestamp = (UINT64)(now.QuadPart / (frequency.QuadPart / 1000000 ? frequency.QuadPart / 1000000 : 1));
	point->buttons = self->buttons;
	point->wheel = wheel;
	point->horizontal = (boolean)horizontal;
	point->update = update;
	arguments->pointer.point = face_of(&point->base, 0);
	arguments->pointer.modifiers = key_modifiers();
	return raise(self, list, arguments);
}

static BOOL raise_key(struct window *self, struct handlers *list, const struct kind *kind, const void *const *table, WPARAM key, LPARAM bits,
	BOOL released)
{
	struct arguments *arguments = new_arguments(kind, table);
	if (!arguments)
		return FALSE;
	arguments->key.key = (UINT32)key;
	arguments->key.status = (struct key_status){LOWORD(bits), (bits >> 16) & 0xff, (bits >> 24) & 1, (bits >> 29) & 1, (bits >> 30) & 1,
		(boolean)released};
	return raise(self, list, arguments);
}

/* PointerUpdateKind for a button pressed or released: the left, right, middle and the two extra buttons follow each other. */
static INT32 button_update(BYTE mask, BOOL pressed)
{
	INT32 base = mask == 1 ? 1 : mask == 2 ? 3 : mask == 4 ? 5 : mask == 8 ? 7 : 9;
	return pressed ? base : base + 1;
}

static BYTE message_button(UINT message, WPARAM wparam)
{
	switch (message) {
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: return 1;
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: return 2;
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: return 4;
	default: return HIWORD(wparam) == XBUTTON2 ? 16 : 8;
	}
}

static void window_activate_view(struct window *self);
static void display_dpi_changed(void);

static LRESULT CALLBACK window_procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
	struct window *self = (struct window *)GetWindowLongPtrW(handle, GWLP_USERDATA);
	if (message == WM_NCCREATE) {
		self = ((CREATESTRUCTW *)lparam)->lpCreateParams;
		SetWindowLongPtrW(handle, GWLP_USERDATA, (LONG_PTR)self);
		self->handle = handle;
	}
	if (!self)
		return DefWindowProcW(handle, message, wparam, lparam);
	if (message == run_message && run_message) {
		struct action *action = (struct action *)lparam;
		if (InterlockedCompareExchange(&action->status, 0, 0) == ASYNC_STARTED) {
			HRESULT result;
			if (action->idle) {
				static const void *const *const tables[] = {idle_table.slots};
				struct object *arguments = make(sizeof *arguments, &idle_kind, tables, 1);
				result = ((one_argument_handler)method(action->work, 3))(action->work, arguments ? face_of(arguments, 0) : NULL);
				if (arguments)
					release(face_of(arguments, 0));
			} else
				result = ((plain_handler)method(action->work, 3))(action->work);
			action_finish(action, SUCCEEDED(result) ? ASYNC_COMPLETED : ASYNC_ERROR, result);
		}
		release(face_of(&action->base, 0));
		return 0;
	}
	if (message == activate_message && activate_message) {
		window_activate_view(self);
		return 0;
	}
	switch (message) {
	case WM_SIZE:
		if (wparam != SIZE_MINIMIZED) {
			struct arguments *arguments = new_arguments(&size_kind, size_table.slots);
			if (arguments) {
				arguments->size = (struct size){dips(handle, LOWORD(lparam)), dips(handle, HIWORD(lparam))};
				raise(self, &self->size_changed, arguments);
			}
		}
		break;
	case WM_SHOWWINDOW: {
		struct arguments *arguments = new_arguments(&visibility_kind, visibility_table.slots);
		if (arguments) {
			arguments->visible = wparam != 0;
			raise(self, &self->visibility, arguments);
		}
		break;
	}
	case WM_ACTIVATE: {
		struct arguments *arguments = new_arguments(&activation_kind, activation_table.slots);
		if (arguments) {
			arguments->activation = LOWORD(wparam) == WA_INACTIVE ? DEACTIVATED : LOWORD(wparam) == WA_CLICKACTIVE ? ACTIVATED_BY_POINTER
				: ACTIVATED_BY_CODE;
			raise(self, &self->activated_handlers, arguments);
		}
		break;
	}
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (raise_key(self, &self->key_down, &key_kind, key_table.slots, wparam, lparam, FALSE))
			return 0;
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (raise_key(self, &self->key_up, &key_kind, key_table.slots, wparam, lparam, TRUE))
			return 0;
		break;
	case WM_CHAR:
		if (raise_key(self, &self->character, &character_kind, character_table.slots, wparam, lparam, FALSE))
			return 0;
		break;
	case WM_MOUSEMOVE:
		if (!self->pointer_inside) {
			TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, handle, 0};
			self->pointer_inside = TRUE;
			TrackMouseEvent(&track);
			raise_pointer(self, &self->entered, lparam, 0, 0, FALSE);
		}
		raise_pointer(self, &self->moved, lparam, 0, 0, FALSE);
		return 0;
	case WM_MOUSELEAVE: {
		POINT at = {0, 0};
		GetCursorPos(&at);
		ScreenToClient(handle, &at);
		self->pointer_inside = FALSE;
		raise_pointer(self, &self->exited, MAKELPARAM(at.x, at.y), 0, 0, FALSE);
		return 0;
	}
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN: {
		const BYTE mask = message_button(message, wparam);
		self->buttons |= mask;
		raise_pointer(self, &self->pressed, lparam, button_update(mask, TRUE), 0, FALSE);
		return message == WM_XBUTTONDOWN;
	}
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP:
	case WM_XBUTTONUP: {
		const BYTE mask = message_button(message, wparam);
		self->buttons &= (BYTE)~mask;
		raise_pointer(self, &self->released, lparam, button_update(mask, FALSE), 0, FALSE);
		return message == WM_XBUTTONUP;
	}
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		raise_pointer(self, &self->wheel, lparam, 0, (SHORT)HIWORD(wparam), message == WM_MOUSEHWHEEL);
		return 0;
	case WM_CAPTURECHANGED:
		if ((HWND)lparam != handle) {
			struct arguments *arguments = new_arguments(&event_kind, event_table.slots);
			raise(self, &self->capture_lost, arguments);
		}
		break;
	case WM_SETCURSOR:
		if (LOWORD(lparam) == HTCLIENT) {
			SetCursor(self->cursor ? system_cursor(self->cursor) : NULL);
			return TRUE;
		}
		break;
	case WM_DPICHANGED: {
		const RECT *suggested = (const RECT *)lparam;
		SetWindowPos(handle, NULL, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
			SWP_NOZORDER | SWP_NOACTIVATE);
		display_dpi_changed();
		return 0;
	}
	case WM_CLOSE:
		DestroyWindow(handle);
		return 0;
	case WM_DESTROY: {
		self->closed = TRUE;
		struct arguments *arguments = new_arguments(&event_kind, event_table.slots);
		raise(self, &self->closed_handlers, arguments);
		if (self->dispatcher)
			self->dispatcher->quit = TRUE;
		PostQuitMessage(0);
		break;
	}
	case WM_NCDESTROY:
		SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
		self->handle = NULL;
		break;
	}
	return DefWindowProcW(handle, message, wparam, lparam);
}

/* Windows.UI.Core.CoreDispatcher */
static struct dispatcher *dispatcher_of(void *iface) { return (struct dispatcher *)owner(iface); }

static HRESULT WINAPI dispatcher_access(void *iface, boolean *out) { return answer_flag(out, GetCurrentThreadId() == dispatcher_of(iface)->thread); }

static BOOL pump_one(BOOL wait)
{
	MSG message;
	if (wait ? GetMessageW(&message, NULL, 0, 0) <= 0 : !PeekMessageW(&message, NULL, 0, 0, PM_REMOVE) || message.message == WM_QUIT)
		return FALSE;
	TranslateMessage(&message);
	DispatchMessageW(&message);
	return TRUE;
}

static HRESULT WINAPI dispatcher_process(void *iface, INT32 option)
{
	struct dispatcher *self = dispatcher_of(iface);
	if (GetCurrentThreadId() != self->thread)
		return RPC_E_WRONG_THREAD;
	switch (option) {
	case PROCESS_ONE_AND_ALL_PENDING:
		if (!self->quit && pump_one(TRUE))
			while (!self->quit && pump_one(FALSE))
				;
		return S_OK;
	case PROCESS_ONE_IF_PRESENT:
		if (!self->quit)
			pump_one(FALSE);
		return S_OK;
	case PROCESS_UNTIL_QUIT:
		while (!self->quit && pump_one(TRUE))
			;
		return S_OK;
	case PROCESS_ALL_IF_PRESENT:
		while (!self->quit && pump_one(FALSE))
			;
		return S_OK;
	}
	return E_INVALIDARG;
}

/* Posts work to the window's thread, where it runs in order with the window's messages. */
static HRESULT post(struct dispatcher *self, IUnknown *work, BOOL idle, void **out)
{
	static const void *const *const tables[] = {action_table.slots, action_info_table.slots};
	if (!work || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (!self->window || !self->window->handle)
		return RPC_E_DISCONNECTED;
	struct action *action = make(sizeof *action, &action_kind, tables, 2);
	if (!action)
		return E_OUTOFMEMORY;
	reference(action->work = work);
	action->idle = idle;
	/* One reference for the caller and one for the message, which the window releases when it runs the work. */
	reference(face_of(&action->base, 0));
	if (!PostMessageW(self->window->handle, run_message, 0, (LPARAM)action)) {
		release(face_of(&action->base, 0));
		release(face_of(&action->base, 0));
		return HRESULT_FROM_WIN32(GetLastError());
	}
	*out = face_of(&action->base, 0);
	return S_OK;
}

static HRESULT WINAPI dispatcher_run(void *iface, INT32 priority, IUnknown *work, void **out) { return post(dispatcher_of(iface), work, FALSE, out); }
static HRESULT WINAPI dispatcher_run_idle(void *iface, IUnknown *work, void **out) { return post(dispatcher_of(iface), work, TRUE, out); }

static HRESULT WINAPI dispatcher_try_run(void *iface, INT32 priority, IUnknown *work, void **out)
{
	answer_null(out);
	return missing("CoreDispatcher.TryRunAsync");
}

static HRESULT WINAPI dispatcher_try_run_idle(void *iface, IUnknown *work, void **out)
{
	answer_null(out);
	return missing("CoreDispatcher.TryRunIdleAsync");
}

static void destroy_dispatcher(struct object *base) {}

TABLE(dispatcher_table, 0, INSPECTABLE, (const void *)dispatcher_access, (const void *)dispatcher_process, (const void *)dispatcher_run,
	(const void *)dispatcher_run_idle);
TABLE(dispatcher2_table, 1, INSPECTABLE, (const void *)dispatcher_try_run, (const void *)dispatcher_try_run_idle);
static const struct face_iid dispatcher_iids[] = {{&iid_dispatcher, 0}, {&iid_dispatcher2, 1}, {NULL, 0}};
static const struct kind dispatcher_kind = {"CoreDispatcher", L"Windows.UI.Core.CoreDispatcher", dispatcher_iids, destroy_dispatcher};

/* Windows.UI.Core.CoreWindow */
static void destroy_window(struct object *base)
{
	struct window *self = (struct window *)base;
	if (self->handle) {
		SetWindowLongPtrW(self->handle, GWLP_USERDATA, 0);
		DestroyWindow(self->handle);
	}
	release(self->cursor);
	if (self->dispatcher) {
		self->dispatcher->window = NULL;
		release(face_of(&self->dispatcher->base, 0));
	}
	struct handlers *lists[] = {&self->activated_handlers, &self->automation, &self->character, &self->closed_handlers, &self->input_enabled,
		&self->key_down, &self->key_up, &self->capture_lost, &self->entered, &self->exited, &self->moved, &self->pressed, &self->released,
		&self->touch_hit, &self->wheel, &self->size_changed, &self->visibility};
	for (size_t index = 0; index < ARRAYSIZE(lists); ++index)
		handlers_clear(lists[index]);
}

static HRESULT WINAPI window_automation(void *iface, IInspectable **out) { return answer_null((void **)out); }

static HRESULT WINAPI window_bounds(void *iface, struct rect *out)
{
	struct window *self = window_of(iface);
	RECT client = {0};
	POINT origin = {0, 0};
	if (!out)
		return E_POINTER;
	if (self->handle) {
		GetClientRect(self->handle, &client);
		ClientToScreen(self->handle, &origin);
	}
	*out = (struct rect){dips(self->handle, origin.x), dips(self->handle, origin.y), dips(self->handle, client.right - client.left),
		dips(self->handle, client.bottom - client.top)};
	return S_OK;
}

static HRESULT WINAPI window_custom_properties(void *iface, void **out)
{
	answer_null(out);
	return missing("CoreWindow.CustomProperties");
}

static HRESULT WINAPI window_dispatcher(void *iface, void **out)
{
	struct window *self = window_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->dispatcher)
		reference(*out = face_of(&self->dispatcher->base, 0));
	return S_OK;
}

static HRESULT WINAPI window_flow(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = window_of(iface)->flow_direction;
	return S_OK;
}

static HRESULT WINAPI window_set_flow(void *iface, INT32 value)
{
	window_of(iface)->flow_direction = value;
	return S_OK;
}

static HRESULT WINAPI window_input_enabled(void *iface, boolean *out)
{
	struct window *self = window_of(iface);
	return answer_flag(out, self->handle && IsWindowEnabled(self->handle));
}

static HRESULT WINAPI window_set_input_enabled(void *iface, boolean value)
{
	struct window *self = window_of(iface);
	if (self->handle)
		EnableWindow(self->handle, value);
	return S_OK;
}

static HRESULT WINAPI window_cursor(void *iface, IUnknown **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = window_of(iface)->cursor);
	return S_OK;
}

static HRESULT WINAPI window_set_cursor(void *iface, IUnknown *cursor)
{
	struct window *self = window_of(iface);
	reference(cursor);
	release(self->cursor);
	self->cursor = cursor;
	POINT at;
	if (self->handle && GetCursorPos(&at) && WindowFromPoint(at) == self->handle)
		SetCursor(cursor ? system_cursor(cursor) : NULL);
	return S_OK;
}

static HRESULT WINAPI window_pointer_position(void *iface, struct point *out)
{
	struct window *self = window_of(iface);
	POINT at = {0, 0};
	if (!out)
		return E_POINTER;
	GetCursorPos(&at);
	*out = (struct point){dips(self->handle, at.x), dips(self->handle, at.y)};
	return S_OK;
}

static HRESULT WINAPI window_set_pointer_position(void *iface, struct point value)
{
	struct window *self = window_of(iface);
	const FLOAT scale = dips(self->handle, 96) / 96.0f;
	return SetCursorPos((int)(value.x / scale), (int)(value.y / scale)) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

static HRESULT WINAPI window_visible(void *iface, boolean *out)
{
	struct window *self = window_of(iface);
	return answer_flag(out, self->handle && IsWindowVisible(self->handle) && !IsIconic(self->handle));
}

static HRESULT WINAPI window_activate(void *iface)
{
	struct window *self = window_of(iface);
	if (!self->handle)
		return RPC_E_DISCONNECTED;
	ShowWindow(self->handle, IsIconic(self->handle) ? SW_RESTORE : SW_SHOW);
	SetForegroundWindow(self->handle);
	SetActiveWindow(self->handle);
	SetFocus(self->handle);
	return S_OK;
}

static HRESULT WINAPI window_close(void *iface)
{
	struct window *self = window_of(iface);
	if (self->handle)
		PostMessageW(self->handle, WM_CLOSE, 0, 0);
	return S_OK;
}

/* CoreVirtualKeyStates: down, and locked for a key that toggles. */
static UINT32 key_state(SHORT state, SHORT toggled) { return (state < 0 ? 1 : 0) | (toggled & 1 ? 2 : 0); }

static HRESULT WINAPI window_async_key(void *iface, UINT32 key, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = key_state(GetAsyncKeyState((int)key), GetKeyState((int)key));
	return S_OK;
}

static HRESULT WINAPI window_key(void *iface, UINT32 key, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	const SHORT state = GetKeyState((int)key);
	*out = key_state(state, state);
	return S_OK;
}

static HRESULT WINAPI window_release_capture(void *iface)
{
	ReleaseCapture();
	return S_OK;
}

static HRESULT WINAPI window_set_capture(void *iface)
{
	struct window *self = window_of(iface);
	if (self->handle)
		SetCapture(self->handle);
	return S_OK;
}

#define WINDOW_EVENT(name, list) \
	static HRESULT WINAPI window_add_##name(void *iface, void *handler, INT64 *token) \
	{ \
		return handlers_add(&window_of(iface)->list, handler, token); \
	} \
	static HRESULT WINAPI window_remove_##name(void *iface, INT64 token) { return handlers_remove(&window_of(iface)->list, token); }

WINDOW_EVENT(activated, activated_handlers)
WINDOW_EVENT(automation, automation)
WINDOW_EVENT(character, character)
WINDOW_EVENT(closed, closed_handlers)
WINDOW_EVENT(input_enabled, input_enabled)
WINDOW_EVENT(key_down, key_down)
WINDOW_EVENT(key_up, key_up)
WINDOW_EVENT(capture_lost, capture_lost)
WINDOW_EVENT(entered, entered)
WINDOW_EVENT(exited, exited)
WINDOW_EVENT(moved, moved)
WINDOW_EVENT(pressed, pressed)
WINDOW_EVENT(released, released)
WINDOW_EVENT(touch_hit, touch_hit)
WINDOW_EVENT(wheel, wheel)
WINDOW_EVENT(size_changed, size_changed)
WINDOW_EVENT(visibility, visibility)

#define EVENT_SLOTS(name) (const void *)window_add_##name, (const void *)window_remove_##name

TABLE(window_table, 0, INSPECTABLE, (const void *)window_automation, (const void *)window_bounds, (const void *)window_custom_properties,
	(const void *)window_dispatcher, (const void *)window_flow, (const void *)window_set_flow, (const void *)window_input_enabled,
	(const void *)window_set_input_enabled, (const void *)window_cursor, (const void *)window_set_cursor,
	(const void *)window_pointer_position, (const void *)window_visible, (const void *)window_activate, (const void *)window_close,
	(const void *)window_async_key, (const void *)window_key, (const void *)window_release_capture, (const void *)window_set_capture,
	EVENT_SLOTS(activated), EVENT_SLOTS(automation), EVENT_SLOTS(character), EVENT_SLOTS(closed), EVENT_SLOTS(input_enabled),
	EVENT_SLOTS(key_down), EVENT_SLOTS(key_up), EVENT_SLOTS(capture_lost), EVENT_SLOTS(entered), EVENT_SLOTS(exited), EVENT_SLOTS(moved),
	EVENT_SLOTS(pressed), EVENT_SLOTS(released), EVENT_SLOTS(touch_hit), EVENT_SLOTS(wheel), EVENT_SLOTS(size_changed),
	EVENT_SLOTS(visibility));
TABLE(window2_table, 1, INSPECTABLE, (const void *)window_set_pointer_position);

/* ICoreWindowInterop is a plain COM interface: the window's handle, and whether the app handled the message the window is in. */
static HRESULT WINAPI interop_handle(void *iface, HWND *out)
{
	if (!out)
		return E_POINTER;
	*out = window_of(iface)->handle;
	return S_OK;
}

static HRESULT WINAPI interop_handled(void *iface, boolean value) { return S_OK; }

TABLE(interop_table, 2, (const void *)object_query, (const void *)object_reference, (const void *)object_release, (const void *)interop_handle,
	(const void *)interop_handled);
static const struct face_iid window_iids[] = {{&iid_core_window, 0}, {&iid_core_window2, 1}, {&iid_core_window_interop, 2}, {NULL, 0}};
static const struct kind window_kind = {"CoreWindow", L"Windows.UI.Core.CoreWindow", window_iids, destroy_window};

/* The window's title: the name of the program without its extension. */
static void program_title(WCHAR *title, DWORD capacity)
{
	WCHAR path[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH), start = length, end = length;
	while (start && path[start - 1] != L'\\' && path[start - 1] != L'/')
		--start;
	for (DWORD at = start; at < length; ++at)
		if (path[at] == L'.')
			end = at;
	DWORD used = 0;
	for (DWORD at = start; at < end && used + 1 < capacity; ++at)
		title[used++] = path[at];
	title[used] = 0;
}

static struct window *new_window(void)
{
	static const void *const *const window_tables[] = {window_table.slots, window2_table.slots, interop_table.slots};
	static const void *const *const dispatcher_tables[] = {dispatcher_table.slots, dispatcher2_table.slots};
	static ATOM atom;
	if (!atom) {
		WNDCLASSEXW type = {sizeof type};
		type.lpfnWndProc = window_procedure;
		type.hInstance = GetModuleHandleW(NULL);
		type.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
		type.lpszClassName = L"Windows.UI.Core.CoreWindow";
		if (!(atom = RegisterClassExW(&type)))
			return NULL;
		run_message = RegisterWindowMessageW(L"Raven.CoreDispatcher.Run");
		activate_message = RegisterWindowMessageW(L"Raven.CoreApplicationView.Activate");
	}
	struct window *self = make(sizeof *self, &window_kind, window_tables, 3);
	struct dispatcher *dispatcher = self ? make(sizeof *dispatcher, &dispatcher_kind, dispatcher_tables, 2) : NULL;
	if (!dispatcher) {
		if (self)
			release(face_of(&self->base, 0));
		return NULL;
	}
	self->dispatcher = dispatcher;
	dispatcher->window = self;
	self->thread = dispatcher->thread = GetCurrentThreadId();
	static const void *const *const cursor_tables[] = {cursor_table.slots};
	struct cursor *arrow = make(sizeof *arrow, &cursor_kind, cursor_tables, 1);
	if (arrow)
		self->cursor = face_of(&arrow->base, 0);
	WCHAR title[MAX_PATH];
	program_title(title, MAX_PATH);
	if (!CreateWindowExW(WS_EX_APPWINDOW, (const WCHAR *)(ULONG_PTR)atom, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			CW_USEDEFAULT, NULL, NULL, GetModuleHandleW(NULL), self)) {
		release(face_of(&dispatcher->base, 0));
		self->dispatcher = NULL;
		release(face_of(&self->base, 0));
		return NULL;
	}
	return self;
}

/* CoreWindow.GetForCurrentThread: the window of the app's view on its own thread, and none on any other. */
static HRESULT WINAPI window_for_thread(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (current_view && current_thread == GetCurrentThreadId() && current_view->window)
		reference(*out = face_of(&current_view->window->base, 0));
	return S_OK;
}

TABLE(window_factory_table, 0, FACTORY);
TABLE(window_statics_table, 1, INSPECTABLE, (const void *)window_for_thread);
static const struct face_iid window_factory_iids[] = {{&iid_factory, 0}, {&iid_core_window_static, 1}, {NULL, 0}};
static const struct kind window_factory_kind = {"CoreWindow", L"Windows.UI.Core.CoreWindow", window_factory_iids, NULL};
struct object core_window_class = {{window_factory_table.slots, window_statics_table.slots}, &window_factory_kind, 1};

/* The arguments of a launch: Windows.ApplicationModel.Activation.LaunchActivatedEventArgs for an app started from its tile, with the
 * text its command line passes after the program's name. */
struct launch {
	struct object base;
	HSTRING arguments;
};

static void destroy_launch(struct object *base) { WindowsDeleteString(((struct launch *)base)->arguments); }

static HRESULT WINAPI launch_kind(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	/* ActivationKind.Launch */
	*out = 0;
	return S_OK;
}

static HRESULT WINAPI launch_previous(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	/* ApplicationExecutionState.NotRunning */
	*out = 0;
	return S_OK;
}

static HRESULT WINAPI launch_splash(void *iface, void **out) { return answer_null(out); }

static HRESULT WINAPI launch_arguments(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(((struct launch *)owner(iface))->arguments, out);
}

static HRESULT WINAPI launch_tile(void *iface, HSTRING *out)
{
	static const WCHAR tile[] = L"App";
	if (!out)
		return E_POINTER;
	return WindowsCreateString(tile, ARRAYSIZE(tile) - 1, out);
}

static HRESULT WINAPI launch_prelaunched(void *iface, boolean *out) { return answer_flag(out, FALSE); }

static HRESULT WINAPI launch_view(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = 1;
	return S_OK;
}

TABLE(launch_table, 0, INSPECTABLE, (const void *)launch_kind, (const void *)launch_previous, (const void *)launch_splash);
TABLE(launch_text_table, 1, INSPECTABLE, (const void *)launch_arguments, (const void *)launch_tile);
TABLE(launch_prelaunch_table, 2, INSPECTABLE, (const void *)launch_prelaunched);
TABLE(launch_view_table, 3, INSPECTABLE, (const void *)launch_view);
static const struct face_iid launch_iids[] = {{&iid_activated, 0}, {&iid_launch_activated, 1}, {&iid_prelaunch_activated, 2},
	{&iid_view_activated, 3}, {NULL, 0}};
static const struct kind launch_kind_of = {"LaunchActivatedEventArgs", L"Windows.ApplicationModel.Activation.LaunchActivatedEventArgs",
	launch_iids, destroy_launch};

static const WCHAR *command_tail(void)
{
	const WCHAR *line = GetCommandLineW();
	BOOL quoted = FALSE;
	for (; *line && (quoted || (*line != L' ' && *line != L'\t')); ++line)
		if (*line == L'"')
			quoted = !quoted;
	while (*line == L' ' || *line == L'\t')
		++line;
	return line;
}

/* Windows.ApplicationModel.Core.CoreApplicationView */
static struct view *view_of(void *iface) { return (struct view *)owner(iface); }

static void destroy_view(struct object *base)
{
	struct view *self = (struct view *)base;
	if (self->display)
		release(face_of((struct object *)self->display, 0));
	if (self->window)
		release(face_of(&self->window->base, 0));
	handlers_clear(&self->activated);
	handlers_clear(&self->hosted_closing);
}

static HRESULT WINAPI view_window(void *iface, void **out)
{
	struct view *self = view_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->window)
		reference(*out = face_of(&self->window->base, 0));
	return S_OK;
}

static HRESULT WINAPI view_add_activated(void *iface, void *handler, INT64 *token) { return handlers_add(&view_of(iface)->activated, handler, token); }
static HRESULT WINAPI view_remove_activated(void *iface, INT64 token) { return handlers_remove(&view_of(iface)->activated, token); }
static HRESULT WINAPI view_main(void *iface, boolean *out) { return answer_flag(out, TRUE); }
static HRESULT WINAPI view_false(void *iface, boolean *out) { return answer_flag(out, FALSE); }

static HRESULT WINAPI view_dispatcher(void *iface, void **out)
{
	struct view *self = view_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->window && self->window->dispatcher)
		reference(*out = face_of(&self->window->dispatcher->base, 0));
	return S_OK;
}

static HRESULT WINAPI view_title_bar(void *iface, void **out)
{
	answer_null(out);
	return missing("CoreApplicationView.TitleBar");
}

static HRESULT WINAPI view_add_hosted_closing(void *iface, void *handler, INT64 *token)
{
	return handlers_add(&view_of(iface)->hosted_closing, handler, token);
}

static HRESULT WINAPI view_remove_hosted_closing(void *iface, INT64 token) { return handlers_remove(&view_of(iface)->hosted_closing, token); }

TABLE(view_table, 0, INSPECTABLE, (const void *)view_window, (const void *)view_add_activated, (const void *)view_remove_activated,
	(const void *)view_main, (const void *)view_false);
TABLE(view2_table, 1, INSPECTABLE, (const void *)view_dispatcher);
TABLE(view3_table, 2, INSPECTABLE, (const void *)view_false, (const void *)view_title_bar, (const void *)view_add_hosted_closing,
	(const void *)view_remove_hosted_closing);
static const struct face_iid view_iids[] = {{&iid_application_view, 0}, {&iid_application_view2, 1}, {&iid_application_view3, 2}, {NULL, 0}};
static const struct kind view_kind = {"CoreApplicationView", L"Windows.ApplicationModel.Core.CoreApplicationView", view_iids, destroy_view};

/* The view's Activated event, raised once from the window's messages so the app sees it while it processes events, as on Windows. */
static void window_activate_view(struct window *window)
{
	static const void *const *const tables[] = {launch_table.slots, launch_text_table.slots, launch_prelaunch_table.slots,
		launch_view_table.slots};
	struct view *view = current_view;
	if (!view || view->window != window || window->activated)
		return;
	window->activated = TRUE;
	struct launch *launch = make(sizeof *launch, &launch_kind_of, tables, 4);
	if (!launch)
		return;
	const WCHAR *tail = command_tail();
	WindowsCreateString(tail, (UINT32)lstrlenW(tail), &launch->arguments);
	reference(face_of(&view->base, 0));
	handlers_raise(&view->activated, face_of(&view->base, 0), face_of(&launch->base, 0));
	release(face_of(&launch->base, 0));
	release(face_of(&view->base, 0));
}

/* CoreApplication.GetCurrentView and MainView: the view of the running app. */
HRESULT application_view(void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!current_view)
		return NOT_FOUND;
	reference(*out = face_of(&current_view->base, 0));
	return S_OK;
}

/* CoreApplication.Exit: the app's window closes, which ends the processing of its events. */
HRESULT application_exit(void)
{
	if (current_view && current_view->window && current_view->window->handle) {
		PostMessageW(current_view->window->handle, WM_CLOSE, 0, 0);
		return S_OK;
	}
	ExitProcess(0);
}

/* Windows.Graphics.Display.DisplayInformation of the running view. */
struct display {
	struct object base;
	struct handlers orientation, dpi, stereo, color;
};

static struct handlers contents_invalidated;

static void destroy_display(struct object *base)
{
	struct display *self = (struct display *)base;
	handlers_clear(&self->orientation);
	handlers_clear(&self->dpi);
	handlers_clear(&self->stereo);
	handlers_clear(&self->color);
}

static struct display *display_of(void *iface) { return (struct display *)owner(iface); }

static HWND display_window(void) { return current_view && current_view->window ? current_view->window->handle : NULL; }

static UINT display_dpi(void)
{
	static UINT (WINAPI *dpi_for_window)(HWND);
	static BOOL looked;
	if (!looked) {
		dpi_for_window = (UINT (WINAPI *)(HWND))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
		looked = TRUE;
	}
	const HWND window = display_window();
	const UINT dpi = dpi_for_window && window ? dpi_for_window(window) : 96;
	return dpi ? dpi : 96;
}

static HRESULT WINAPI display_orientation(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	/* DisplayOrientations.Landscape */
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI display_scale(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	/* ResolutionScale counts in percent. */
	*out = (INT32)(display_dpi() * 100 / 96);
	return S_OK;
}

static HRESULT WINAPI display_logical_dpi(void *iface, FLOAT *out)
{
	if (!out)
		return E_POINTER;
	*out = (FLOAT)display_dpi();
	return S_OK;
}

static HRESULT WINAPI display_raw_pixels_per_view_pixel(void *iface, DOUBLE *out)
{
	if (!out)
		return E_POINTER;
	*out = display_dpi() / 96.0;
	return S_OK;
}

static HRESULT WINAPI display_no_stereo(void *iface, boolean *out) { return answer_flag(out, FALSE); }

static HRESULT WINAPI display_color_profile(void *iface, void **out)
{
	answer_null(out);
	return missing("DisplayInformation.GetColorProfileAsync");
}

/* The display's diagonal is not known: the property is null, as for a display that reports no size. */
static HRESULT WINAPI display_diagonal(void *iface, void **out) { return answer_null(out); }

static void monitor_size(UINT32 *width, UINT32 *height)
{
	MONITORINFO info = {sizeof info};
	const HWND window = display_window();
	if (window && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) {
		*width = (UINT32)(info.rcMonitor.right - info.rcMonitor.left);
		*height = (UINT32)(info.rcMonitor.bottom - info.rcMonitor.top);
	} else {
		*width = (UINT32)GetSystemMetrics(SM_CXSCREEN);
		*height = (UINT32)GetSystemMetrics(SM_CYSCREEN);
	}
}

static HRESULT WINAPI display_width(void *iface, UINT32 *out)
{
	UINT32 height;
	if (!out)
		return E_POINTER;
	monitor_size(out, &height);
	return S_OK;
}

static HRESULT WINAPI display_height(void *iface, UINT32 *out)
{
	UINT32 width;
	if (!out)
		return E_POINTER;
	monitor_size(&width, out);
	return S_OK;
}

#define DISPLAY_EVENT(name) \
	static HRESULT WINAPI display_add_##name(void *iface, void *handler, INT64 *token) \
	{ \
		return handlers_add(&display_of(iface)->name, handler, token); \
	} \
	static HRESULT WINAPI display_remove_##name(void *iface, INT64 token) { return handlers_remove(&display_of(iface)->name, token); }

DISPLAY_EVENT(orientation)
DISPLAY_EVENT(dpi)
DISPLAY_EVENT(stereo)
DISPLAY_EVENT(color)

TABLE(display_table, 0, INSPECTABLE, (const void *)display_orientation, (const void *)display_orientation,
	(const void *)display_add_orientation, (const void *)display_remove_orientation, (const void *)display_scale,
	(const void *)display_logical_dpi, (const void *)display_logical_dpi, (const void *)display_logical_dpi, (const void *)display_add_dpi,
	(const void *)display_remove_dpi, (const void *)display_no_stereo, (const void *)display_add_stereo, (const void *)display_remove_stereo,
	(const void *)display_color_profile, (const void *)display_add_color, (const void *)display_remove_color);
TABLE(display2_table, 1, INSPECTABLE, (const void *)display_raw_pixels_per_view_pixel);
TABLE(display3_table, 2, INSPECTABLE, (const void *)display_diagonal);
TABLE(display4_table, 3, INSPECTABLE, (const void *)display_width, (const void *)display_height);
static const struct face_iid display_iids[] = {{&iid_display, 0}, {&iid_display2, 1}, {&iid_display3, 2}, {&iid_display4, 3}, {NULL, 0}};
static const struct kind display_kind = {"DisplayInformation", L"Windows.Graphics.Display.DisplayInformation", display_iids, destroy_display};

static void display_dpi_changed(void)
{
	struct display *display = current_view ? current_view->display : NULL;
	if (!display)
		return;
	void *face = face_of(&display->base, 0);
	reference(face);
	handlers_raise(&display->dpi, face, NULL);
	release(face);
}

/* DisplayInformation.GetForCurrentView: the one object of the view, on the view's thread. */
static HRESULT WINAPI display_for_view(void *iface, void **out)
{
	static const void *const *const tables[] = {display_table.slots, display2_table.slots, display3_table.slots, display4_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!current_view || current_thread != GetCurrentThreadId())
		return NOT_FOUND;
	if (!current_view->display && !(current_view->display = make(sizeof(struct display), &display_kind, tables, 4)))
		return E_OUTOFMEMORY;
	reference(*out = face_of(&current_view->display->base, 0));
	return S_OK;
}

static HRESULT WINAPI display_rotation(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	/* DisplayOrientations.None: the app has no preference. */
	*out = 0;
	return S_OK;
}

static HRESULT WINAPI display_set_rotation(void *iface, INT32 value) { return S_OK; }

static HRESULT WINAPI display_add_invalidated(void *iface, void *handler, INT64 *token) { return handlers_add(&contents_invalidated, handler, token); }
static HRESULT WINAPI display_remove_invalidated(void *iface, INT64 token) { return handlers_remove(&contents_invalidated, token); }

TABLE(display_factory_table, 0, FACTORY);
TABLE(display_statics_table, 1, INSPECTABLE, (const void *)display_for_view, (const void *)display_rotation, (const void *)display_set_rotation,
	(const void *)display_add_invalidated, (const void *)display_remove_invalidated);
static const struct face_iid display_factory_iids[] = {{&iid_factory, 0}, {&iid_display_statics, 1}, {NULL, 0}};
static const struct kind display_factory_kind = {"DisplayInformation", L"Windows.Graphics.Display.DisplayInformation", display_factory_iids,
	NULL};
struct object display_information_class = {{display_factory_table.slots, display_statics_table.slots}, &display_factory_kind, 1};

/* A swap chain for a core window is one for its desktop window. */
static HRESULT STDMETHODCALLTYPE core_window_swap_chain(IDXGIFactory2 *factory, IUnknown *device, IUnknown *window,
	const DXGI_SWAP_CHAIN_DESC1 *description, IDXGIOutput *output, IDXGISwapChain1 **out)
{
	IUnknown *interop = NULL;
	HWND handle = NULL;
	if (out)
		*out = NULL;
	if (!device || !window || !description || !out)
		return DXGI_ERROR_INVALID_CALL;
	if (FAILED(window->lpVtbl->QueryInterface(window, &iid_core_window_interop, (void **)&interop)))
		return DXGI_ERROR_INVALID_CALL;
	((HRESULT (WINAPI *)(void *, HWND *))method(interop, 3))(interop, &handle);
	release(interop);
	return factory->lpVtbl->CreateSwapChainForHwnd(factory, device, handle, description, NULL, output, out);
}

/* The DXGI an app of the Windows Runtime loads answers a core window with its own swap chain, or with E_NOTIMPL before it looks at
 * its arguments, as DXVK does. Its factories share one table of methods, whose entry for core windows then makes the swap chain for the
 * desktop window; the library stays loaded while the table is in use. */
static void serve_core_window_swap_chains(void)
{
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	HRESULT (WINAPI *create)(REFIID, void **) = dxgi ? (HRESULT (WINAPI *)(REFIID, void **))GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
	IDXGIFactory2 *factory = NULL;
	if (!create || FAILED(create(&iid_dxgi_factory2, (void **)&factory)) || !factory)
		return;
	IDXGISwapChain1 *probe = NULL;
	if (factory->lpVtbl->CreateSwapChainForCoreWindow(factory, NULL, NULL, NULL, NULL, &probe) == E_NOTIMPL) {
		void **slot = (void **)&factory->lpVtbl->CreateSwapChainForCoreWindow;
		DWORD protection;
		if (VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &protection)) {
			*slot = (void *)core_window_swap_chain;
			VirtualProtect(slot, sizeof *slot, protection, &protection);
			trace("CreateSwapChainForCoreWindow", " is served by the core window's desktop window");
		}
	}
	factory->lpVtbl->Release(factory);
}

/* CoreApplication.Run: the view source makes the app's view, which is initialized with its application view, given its window, loaded,
 * activated and run, then uninitialized; IFrameworkViewSource.CreateView, and IFrameworkView's Initialize, SetWindow, Load, Run and
 * Uninitialize are the methods after IInspectable's in that order. */
HRESULT application_run(IUnknown *source)
{
	static const void *const *const tables[] = {view_table.slots, view2_table.slots, view3_table.slots};
	if (!source)
		return E_INVALIDARG;
	if (current_view)
		return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
	IUnknown *app = NULL;
	HRESULT result = ((HRESULT (WINAPI *)(void *, IUnknown **))method(source, 6))(source, &app);
	if (FAILED(result))
		return result;
	if (!app)
		return E_POINTER;
	struct view *view = make(sizeof *view, &view_kind, tables, 3);
	if (!view || !(view->window = new_window())) {
		if (view)
			release(face_of(&view->base, 0));
		release(app);
		return E_OUTOFMEMORY;
	}
	current_view = view;
	current_thread = GetCurrentThreadId();
	serve_core_window_swap_chains();
	result = ((HRESULT (WINAPI *)(void *, void *))method(app, 6))(app, face_of(&view->base, 0));
	if (SUCCEEDED(result))
		result = ((HRESULT (WINAPI *)(void *, void *))method(app, 7))(app, face_of(&view->window->base, 0));
	if (SUCCEEDED(result)) {
		HSTRING entry = NULL;
		static const WCHAR point[] = L"App";
		WindowsCreateString(point, ARRAYSIZE(point) - 1, &entry);
		result = ((HRESULT (WINAPI *)(void *, HSTRING))method(app, 8))(app, entry);
		WindowsDeleteString(entry);
	}
	if (SUCCEEDED(result)) {
		PostMessageW(view->window->handle, activate_message, 0, 0);
		result = ((HRESULT (WINAPI *)(void *))method(app, 9))(app);
	}
	((HRESULT (WINAPI *)(void *))method(app, 10))(app);
	release(app);
	if (view->window->handle)
		DestroyWindow(view->window->handle);
	current_view = NULL;
	release(face_of(&view->base, 0));
	return result;
}
