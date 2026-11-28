/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.UI.Popups: MessageDialog, PopupMenu, UICommand and UICommandSeparator.
 *
 * - A MessageDialog is a task dialog of comctl32 on a thread of the operation's own: its title is the main instruction, its content the
 *   text, one button per command in order, the default command's button the default one. With no command it has a Close button, as on
 *   Windows. Escape and the close box choose the cancel command, and do nothing when the dialog has none. Only one dialog shows at a
 *   time: asking for another while one shows is refused with E_ACCESSDENIED. Canceling the operation closes the dialog.
 * - A PopupMenu is a shortcut menu at a point, or beside a rectangle, of the app's window, in that window's pixels scaled from DIPs; a
 *   separator is a menu separator. It shows from the message loop of the thread that asked for it, the window's own thread, as a
 *   shortcut menu of Windows runs its loop there; one waits to show at a time. Closing it without a choice gives no command. Canceling
 *   the operation does not close the menu: the operation ends canceled when the menu closes.
 * - The command chosen gets its Invoked handler called before the operation completes with it.
 * - Both belong to the window IInitializeWithWindow gave, as desktop apps give it, or else to the app's CoreWindow.
 * - MessageDialogOptions.AcceptUserInputAfterDelay is kept; the buttons take input at once. */
#include "object.h"
#include "../raven-winrt/winrt.h"
#include <commctrl.h>

static const IID iid_dialog = {0x33f59b01, 0x5325, 0x43ab, {0x9a, 0xb3, 0xbd, 0xae, 0x44, 0x0e, 0x41, 0x21}};
static const IID iid_dialog_factory = {0x2d161777, 0xa66f, 0x4ea5, {0xbb, 0x87, 0x79, 0x3f, 0xfa, 0x49, 0x41, 0xf2}};
static const IID iid_menu = {0x4e9bc6dc, 0x880d, 0x47fc, {0xa0, 0xa1, 0x72, 0xb6, 0x39, 0xe6, 0x25, 0x59}};
static const IID iid_command = {0x4ff93a75, 0x4145, 0x47ff, {0xac, 0x7f, 0xdf, 0xf1, 0xc1, 0xfa, 0x5b, 0x0f}};
static const IID iid_command_factory = {0xa21a8189, 0x26b0, 0x4676, {0xae, 0x94, 0x54, 0x04, 0x1b, 0xc1, 0x25, 0xe8}};
/* IVector, IVectorView, IIterable and IIterator of IUICommand, and IAsyncOperation<IUICommand>. */
static const IID iid_command_vector = {0x105139a1, 0xdcb8, 0x5f65, {0x97, 0xef, 0xcb, 0x1b, 0xf0, 0xb7, 0x5f, 0x9d}};
static const IID iid_command_view = {0xed1165e6, 0xf377, 0x5b00, {0x81, 0x72, 0x93, 0xc1, 0xbd, 0x04, 0xde, 0xb4}};
static const IID iid_command_iterable = {0xe63de42b, 0x53c3, 0x5e07, {0x90, 0xd3, 0x98, 0x17, 0x2d, 0x54, 0x54, 0x12}};
static const IID iid_command_iterator = {0xf45db3d3, 0x7299, 0x57ce, {0xa7, 0x3e, 0x29, 0x7c, 0xf0, 0xaf, 0x30, 0x83}};
static const IID iid_command_operation = {0xb8770535, 0x6a4b, 0x52b1, {0xb5, 0x78, 0xf3, 0xcd, 0xc5, 0x00, 0x7a, 0x1f}};
static const struct winrt_items_kind command_list = {&iid_command_view, &iid_command_iterable, &iid_command_iterator, FALSE,
	&iid_command_vector};

/* No command: what a menu closed without a choice gives, and what a dialog with no cancel command does on Escape. */
#define NO_INDEX 0xffffffffu

/* ---- UICommand and UICommandSeparator ---- */

struct command {
	struct object base;
	SRWLOCK lock;
	HSTRING label;
	IUnknown *invoked;
	IInspectable *id;
};

static struct command *command_of(void *iface) { return (struct command *)owner(iface); }

static void destroy_command(struct object *base)
{
	struct command *self = (struct command *)base;
	WindowsDeleteString(self->label);
	release(self->invoked);
	release(self->id);
}

static HRESULT WINAPI command_label(void *iface, HSTRING *out)
{
	struct command *self = command_of(iface);
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = WindowsDuplicateString(self->label, out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

static HRESULT WINAPI set_command_label(void *iface, HSTRING value)
{
	struct command *self = command_of(iface);
	HSTRING copy = NULL;
	const HRESULT result = WindowsDuplicateString(value, &copy);
	if (FAILED(result))
		return result;
	AcquireSRWLockExclusive(&self->lock);
	HSTRING old = self->label;
	self->label = copy;
	ReleaseSRWLockExclusive(&self->lock);
	WindowsDeleteString(old);
	return S_OK;
}

static HRESULT get_field(struct command *self, IUnknown **field, void **out)
{
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	reference(*out = *field);
	ReleaseSRWLockShared(&self->lock);
	return S_OK;
}

static HRESULT put_field(struct command *self, IUnknown **field, IUnknown *value)
{
	reference(value);
	AcquireSRWLockExclusive(&self->lock);
	IUnknown *old = *field;
	*field = value;
	ReleaseSRWLockExclusive(&self->lock);
	release(old);
	return S_OK;
}

static HRESULT WINAPI command_invoked(void *iface, void **out) { return get_field(command_of(iface), &command_of(iface)->invoked, out); }
static HRESULT WINAPI set_command_invoked(void *iface, IUnknown *value) { return put_field(command_of(iface), &command_of(iface)->invoked, value); }
static HRESULT WINAPI command_id(void *iface, void **out) { return get_field(command_of(iface), (IUnknown **)&command_of(iface)->id, out); }
static HRESULT WINAPI set_command_id(void *iface, IUnknown *value) { return put_field(command_of(iface), (IUnknown **)&command_of(iface)->id, value); }

TABLE(command_table, 0, INSPECTABLE, (const void *)command_label, (const void *)set_command_label, (const void *)command_invoked,
	(const void *)set_command_invoked, (const void *)command_id, (const void *)set_command_id);
static const struct face_iid command_iids[] = {{&iid_command, 0}, {NULL, 0}};
static const struct kind command_kind = {"UICommand", L"Windows.UI.Popups.UICommand", command_iids, destroy_command};
static const struct kind separator_kind = {"UICommandSeparator", L"Windows.UI.Popups.UICommandSeparator", command_iids, destroy_command};

static HRESULT command_make(const struct kind *kind, HSTRING label, IUnknown *invoked, IUnknown *id, void **out)
{
	static const void *const *const tables[] = {command_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct command *self = make(sizeof *self, kind, tables, 1);
	if (!self)
		return E_OUTOFMEMORY;
	InitializeSRWLock(&self->lock);
	const HRESULT result = WindowsDuplicateString(label, &self->label);
	if (FAILED(result)) {
		release(face_of(&self->base, 0));
		return result;
	}
	reference(self->invoked = invoked);
	reference(self->id = (IInspectable *)id);
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI command_activate(void *iface, IInspectable **out) { return command_make(&command_kind, NULL, NULL, NULL, (void **)out); }
static HRESULT WINAPI command_create(void *iface, HSTRING label, void **out) { return command_make(&command_kind, label, NULL, NULL, out); }
static HRESULT WINAPI command_create_with(void *iface, HSTRING label, IUnknown *invoked, void **out)
{
	return command_make(&command_kind, label, invoked, NULL, out);
}
static HRESULT WINAPI command_create_with_id(void *iface, HSTRING label, IUnknown *invoked, IUnknown *id, void **out)
{
	return command_make(&command_kind, label, invoked, id, out);
}

TABLE(command_factory_table, 0, INSPECTABLE, (const void *)command_activate);
TABLE(command_statics_table, 1, INSPECTABLE, (const void *)command_create, (const void *)command_create_with,
	(const void *)command_create_with_id);
static const struct face_iid command_class_iids[] = {{&iid_factory, 0}, {&iid_command_factory, 1}, {NULL, 0}};
static const struct kind command_class_kind = {"UICommand", L"Windows.UI.Popups.UICommand", command_class_iids, NULL};
struct object ui_command_class = {{command_factory_table.slots, command_statics_table.slots}, &command_class_kind, 1};

static HRESULT WINAPI separator_activate(void *iface, IInspectable **out) { return command_make(&separator_kind, NULL, NULL, NULL, (void **)out); }

TABLE(separator_factory_table, 0, INSPECTABLE, (const void *)separator_activate);
static const struct face_iid separator_class_iids[] = {{&iid_factory, 0}, {NULL, 0}};
static const struct kind separator_class_kind = {"UICommandSeparator", L"Windows.UI.Popups.UICommandSeparator", separator_class_iids, NULL};
struct object ui_command_separator_class = {{separator_factory_table.slots}, &separator_class_kind, 1};

static BOOL is_separator(IUnknown *command)
{
	return method(command, 0) == (void *)object_query && owner(command)->kind == &separator_kind;
}

/* ---- the commands of a dialog or a menu, taken when it shows ---- */

struct commands {
	IUnknown **items;
	HSTRING *labels;
	UINT32 count;
};

static void commands_free(struct commands *list)
{
	for (UINT32 at = 0; at < list->count; ++at) {
		release(list->items[at]);
		WindowsDeleteString(list->labels[at]);
	}
	FREE(list->items);
	FREE(list->labels);
	list->items = NULL;
	list->labels = NULL;
	list->count = 0;
}

/* The commands in an IVector<IUICommand>, each with its label, which may be any IUICommand of the app's own. */
static HRESULT commands_take(IUnknown *vector, struct commands *list)
{
	IUnknown *view = NULL;
	UINT32 count = 0;
	list->items = NULL;
	list->labels = NULL;
	list->count = 0;
	HRESULT result = ((HRESULT (WINAPI *)(void *, void **))method(vector, 8))(vector, (void **)&view);
	if (SUCCEEDED(result))
		result = ((HRESULT (WINAPI *)(void *, UINT32 *))method(view, 7))(view, &count);
	if (SUCCEEDED(result) && (!(list->items = ALLOC((count ? count : 1) * sizeof *list->items)) ||
		!(list->labels = ALLOC((count ? count : 1) * sizeof *list->labels))))
		result = E_OUTOFMEMORY;
	for (UINT32 at = 0; SUCCEEDED(result) && at < count; ++at) {
		IUnknown *item = NULL;
		if (FAILED(result = ((HRESULT (WINAPI *)(void *, UINT32, void **))method(view, 6))(view, at, (void **)&item)))
			break;
		if (!item)
			continue;
		list->items[list->count] = item;
		((HRESULT (WINAPI *)(void *, HSTRING *))method(item, 6))(item, &list->labels[list->count]);
		++list->count;
	}
	release(view);
	if (FAILED(result))
		commands_free(list);
	return result;
}

/* Calls the chosen command's Invoked handler, then gives the command. */
static HRESULT choose(IUnknown *command, union winrt_value *result)
{
	IUnknown *invoked = NULL;
	if (SUCCEEDED(((HRESULT (WINAPI *)(void *, void **))method(command, 8))(command, (void **)&invoked)) && invoked) {
		((HRESULT (WINAPI *)(void *, void *))method(invoked, 3))(invoked, command);
		release(invoked);
	}
	reference(result->object = command);
	return S_OK;
}

/* The DPI of a window, for DIPs. */
static UINT window_dpi(HWND window)
{
	static UINT (WINAPI *dpi_for_window)(HWND);
	static BOOL looked;
	if (!looked) {
		dpi_for_window = (UINT (WINAPI *)(HWND))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
		looked = TRUE;
	}
	const UINT dpi = dpi_for_window ? dpi_for_window(window) : 0;
	return dpi ? dpi : 96;
}

/* ---- MessageDialog ---- */

struct dialog {
	struct object base;
	SRWLOCK lock;
	HSTRING title, content;
	IUnknown *commands;
	UINT32 default_index, cancel_index;
	INT32 options;
	HWND window;
};

static struct dialog *dialog_of(void *iface) { return (struct dialog *)owner(iface); }
/* Whether a dialog shows. */
static LONG dialog_showing;

static void destroy_dialog(struct object *base)
{
	struct dialog *self = (struct dialog *)base;
	WindowsDeleteString(self->title);
	WindowsDeleteString(self->content);
	release(self->commands);
}

static HRESULT get_text(struct dialog *self, HSTRING *field, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = WindowsDuplicateString(*field, out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

static HRESULT put_text(struct dialog *self, HSTRING *field, HSTRING value)
{
	HSTRING copy = NULL;
	const HRESULT result = WindowsDuplicateString(value, &copy);
	if (FAILED(result))
		return result;
	AcquireSRWLockExclusive(&self->lock);
	HSTRING old = *field;
	*field = copy;
	ReleaseSRWLockExclusive(&self->lock);
	WindowsDeleteString(old);
	return S_OK;
}

static HRESULT WINAPI dialog_title(void *iface, HSTRING *out) { return get_text(dialog_of(iface), &dialog_of(iface)->title, out); }
static HRESULT WINAPI set_dialog_title(void *iface, HSTRING value) { return put_text(dialog_of(iface), &dialog_of(iface)->title, value); }
static HRESULT WINAPI dialog_content(void *iface, HSTRING *out) { return get_text(dialog_of(iface), &dialog_of(iface)->content, out); }
static HRESULT WINAPI set_dialog_content(void *iface, HSTRING value) { return put_text(dialog_of(iface), &dialog_of(iface)->content, value); }

static HRESULT WINAPI dialog_commands(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = dialog_of(iface)->commands);
	return S_OK;
}

static HRESULT get_number(UINT32 *field, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = *(volatile UINT32 *)field;
	return S_OK;
}

static HRESULT WINAPI default_index(void *iface, UINT32 *out) { return get_number(&dialog_of(iface)->default_index, out); }
static HRESULT WINAPI set_default_index(void *iface, UINT32 value) { dialog_of(iface)->default_index = value; return S_OK; }
static HRESULT WINAPI cancel_index(void *iface, UINT32 *out) { return get_number(&dialog_of(iface)->cancel_index, out); }
static HRESULT WINAPI set_cancel_index(void *iface, UINT32 value) { dialog_of(iface)->cancel_index = value; return S_OK; }
static HRESULT WINAPI dialog_options(void *iface, INT32 *out) { return get_number((UINT32 *)&dialog_of(iface)->options, (UINT32 *)out); }
static HRESULT WINAPI set_dialog_options(void *iface, INT32 value) { dialog_of(iface)->options = value; return S_OK; }

/* A dialog as it shows: its texts and commands taken when ShowAsync was called. */
struct showing {
	struct winrt_async *operation;
	struct commands commands;
	HSTRING title, content;
	UINT32 default_index, cancel_index;
	HWND window;
	BOOL canceled;
};

static void free_showing(void *context)
{
	struct showing *self = context;
	commands_free(&self->commands);
	WindowsDeleteString(self->title);
	WindowsDeleteString(self->content);
	FREE(self);
}

/* Button ids: one per command after the ids Windows keeps. */
#define FIRST_BUTTON 100

static HRESULT CALLBACK dialog_callback(HWND window, UINT note, WPARAM wparam, LPARAM lparam, LONG_PTR data)
{
	struct showing *self = (struct showing *)data;
	if (note == TDN_TIMER && !self->canceled && winrt_async_canceled(self->operation)) {
		self->canceled = TRUE;
		SendMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0);
	}
	/* Escape and the close box choose the cancel command, and do nothing when there is none. */
	if (note == TDN_BUTTON_CLICKED && wparam == IDCANCEL && !self->canceled && self->cancel_index >= self->commands.count)
		return S_FALSE;
	return S_OK;
}

static HRESULT run_dialog(struct winrt_async *operation, void *context, union winrt_value *result)
{
	typedef HRESULT (WINAPI *task_dialog_type)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
	struct showing *self = context;
	TASKDIALOGCONFIG config = {sizeof config};
	WCHAR program[MAX_PATH];
	int chosen = 0;
	self->operation = operation;
	HMODULE controls = LoadLibraryW(L"comctl32.dll");
	task_dialog_type task_dialog = controls ? (task_dialog_type)GetProcAddress(controls, "TaskDialogIndirect") : NULL;
	TASKDIALOG_BUTTON *buttons = ALLOC(self->commands.count * sizeof *buttons + sizeof *buttons);
	HRESULT outcome = !task_dialog ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND) : !buttons ? E_OUTOFMEMORY : S_OK;
	if (SUCCEEDED(outcome)) {
		/* The window's title is the program's name, as an app's own dialog has its name there. */
		const DWORD length = GetModuleFileNameW(NULL, program, MAX_PATH);
		WCHAR *name = program;
		for (DWORD at = 0; at < length; ++at)
			if (program[at] == L'\\' || program[at] == L'/')
				name = program + at + 1;
		for (WCHAR *dot = name + lstrlenW(name); dot > name; --dot)
			if (*dot == L'.') {
				*dot = 0;
				break;
			}
		for (UINT32 at = 0; at < self->commands.count; ++at) {
			buttons[at].nButtonID = FIRST_BUTTON + (int)at;
			buttons[at].pszButtonText = WindowsGetStringRawBuffer(self->commands.labels[at], NULL);
		}
		config.hwndParent = self->window;
		config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER | TDF_POSITION_RELATIVE_TO_WINDOW;
		config.pszWindowTitle = name;
		config.pszMainInstruction = WindowsGetStringLen(self->title) ? WindowsGetStringRawBuffer(self->title, NULL) : NULL;
		config.pszContent = WindowsGetStringRawBuffer(self->content, NULL);
		config.cButtons = self->commands.count;
		config.pButtons = buttons;
		config.nDefaultButton = self->default_index < self->commands.count ? FIRST_BUTTON + (int)self->default_index : 0;
		config.pfCallback = dialog_callback;
		config.lpCallbackData = (LONG_PTR)self;
		outcome = task_dialog(&config, &chosen, NULL, NULL);
	}
	FREE(buttons);
	if (controls)
		FreeLibrary(controls);
	InterlockedExchange(&dialog_showing, 0);
	if (FAILED(outcome))
		return outcome;
	if (self->canceled)
		return E_ABORT;
	const UINT32 index = chosen == IDCANCEL ? self->cancel_index : (UINT32)(chosen - FIRST_BUTTON);
	return index < self->commands.count ? choose(self->commands.items[index], result) : S_OK;
}

static HRESULT WINAPI dialog_show(void *iface, void **out)
{
	struct dialog *self = dialog_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (InterlockedCompareExchange(&dialog_showing, 1, 0))
		return E_ACCESSDENIED;
	struct showing *showing = ALLOC(sizeof *showing);
	HRESULT result = showing ? commands_take(self->commands, &showing->commands) : E_OUTOFMEMORY;
	if (SUCCEEDED(result)) {
		AcquireSRWLockShared(&self->lock);
		WindowsDuplicateString(self->title, &showing->title);
		WindowsDuplicateString(self->content, &showing->content);
		ReleaseSRWLockShared(&self->lock);
		showing->default_index = self->default_index;
		showing->cancel_index = self->cancel_index;
		showing->window = self->window ? self->window : application_window();
	}
	/* A dialog with no commands has one that closes it. */
	if (SUCCEEDED(result) && !showing->commands.count) {
		HSTRING close = NULL;
		result = WindowsCreateString(L"Close", 5, &close);
		if (SUCCEEDED(result))
			result = command_make(&command_kind, close, NULL, NULL, (void **)&showing->commands.items[0]);
		if (SUCCEEDED(result)) {
			showing->commands.labels[0] = close;
			showing->commands.count = 1;
			showing->cancel_index = 0;
		} else
			WindowsDeleteString(close);
	}
	if (SUCCEEDED(result))
		return winrt_async_start(&iid_command_operation, FALSE, WINRT_RESULT_OBJECT, run_dialog, showing, free_showing, out);
	if (showing)
		free_showing(showing);
	InterlockedExchange(&dialog_showing, 0);
	return result;
}

static HRESULT WINAPI dialog_initialize(void *iface, HWND window)
{
	if (window && !IsWindow(window))
		return E_INVALIDARG;
	dialog_of(iface)->window = window;
	return S_OK;
}

TABLE(dialog_table, 0, INSPECTABLE, (const void *)dialog_title, (const void *)set_dialog_title, (const void *)dialog_commands,
	(const void *)default_index, (const void *)set_default_index, (const void *)cancel_index, (const void *)set_cancel_index,
	(const void *)dialog_content, (const void *)set_dialog_content, (const void *)dialog_show, (const void *)dialog_options,
	(const void *)set_dialog_options);
TABLE(dialog_window_table, 1, UNKNOWN, (const void *)dialog_initialize);
static const struct face_iid dialog_iids[] = {{&iid_dialog, 0}, {&iid_initialize_with_window, 1}, {NULL, 0}};
static const struct kind dialog_kind = {"MessageDialog", L"Windows.UI.Popups.MessageDialog", dialog_iids, destroy_dialog};

static HRESULT dialog_make(HSTRING content, HSTRING title, void **out)
{
	static const void *const *const tables[] = {dialog_table.slots, dialog_window_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct dialog *self = make(sizeof *self, &dialog_kind, tables, 2);
	if (!self)
		return E_OUTOFMEMORY;
	InitializeSRWLock(&self->lock);
	self->cancel_index = NO_INDEX;
	HRESULT result = WindowsDuplicateString(content, &self->content);
	if (SUCCEEDED(result))
		result = WindowsDuplicateString(title, &self->title);
	if (SUCCEEDED(result))
		result = winrt_vector(&command_list, NULL, 0, (void **)&self->commands);
	if (FAILED(result)) {
		release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI dialog_create(void *iface, HSTRING content, void **out) { return dialog_make(content, NULL, out); }
static HRESULT WINAPI dialog_create_titled(void *iface, HSTRING content, HSTRING title, void **out) { return dialog_make(content, title, out); }

TABLE(dialog_factory_table, 0, FACTORY);
TABLE(dialog_statics_table, 1, INSPECTABLE, (const void *)dialog_create, (const void *)dialog_create_titled);
static const struct face_iid dialog_class_iids[] = {{&iid_factory, 0}, {&iid_dialog_factory, 1}, {NULL, 0}};
static const struct kind dialog_class_kind = {"MessageDialog", L"Windows.UI.Popups.MessageDialog", dialog_class_iids, NULL};
struct object message_dialog_class = {{dialog_factory_table.slots, dialog_statics_table.slots}, &dialog_class_kind, 1};

/* ---- PopupMenu ---- */

struct menu {
	struct object base;
	IUnknown *commands;
	HWND window;
};

static struct menu *menu_of(void *iface) { return (struct menu *)owner(iface); }
static void destroy_menu(struct object *base) { release(((struct menu *)base)->commands); }

static HRESULT WINAPI menu_commands(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = menu_of(iface)->commands);
	return S_OK;
}

struct point {
	FLOAT x, y;
};

struct rect {
	FLOAT x, y, width, height;
};

/* Windows.UI.Popups.Placement. */
enum { PLACE_DEFAULT, PLACE_ABOVE, PLACE_BELOW, PLACE_LEFT, PLACE_RIGHT };

struct tracking {
	struct commands commands;
	HWND window;
	struct rect place;
	INT32 placement;
	/* Set once the menu closed, with the index of the command chosen or NO_INDEX. */
	HANDLE closed;
	UINT32 chosen;
	HRESULT error;
};

static void free_tracking(void *context)
{
	struct tracking *self = context;
	commands_free(&self->commands);
	if (self->closed)
		CloseHandle(self->closed);
	FREE(self);
}

/* The menu that waits to show on the thread that asked for it: one at a time, as a thread has one menu at a time. */
static struct tracking *waiting_menu;
static UINT_PTR waiting_timer;

/* Shows the menu and waits for it to close, on the thread whose loop dispatched the timer. */
static void track(struct tracking *self)
{
	self->chosen = NO_INDEX;
	HMENU menu = IsWindow(self->window) ? CreatePopupMenu() : NULL;
	if (!menu) {
		self->error = IsWindow(self->window) ? HRESULT_FROM_WIN32(GetLastError()) : HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
		return;
	}
	for (UINT32 at = 0; at < self->commands.count; ++at)
		if (is_separator(self->commands.items[at]))
			AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
		else
			AppendMenuW(menu, MF_STRING, at + 1, WindowsGetStringRawBuffer(self->commands.labels[at], NULL));
	/* DIPs of the window's client area to pixels of the screen. */
	const FLOAT scale = (FLOAT)window_dpi(self->window) / 96.0f;
	POINT corner = {(LONG)(self->place.x * scale), (LONG)(self->place.y * scale)};
	POINT end = {(LONG)((self->place.x + self->place.width) * scale), (LONG)((self->place.y + self->place.height) * scale)};
	ClientToScreen(self->window, &corner);
	ClientToScreen(self->window, &end);
	UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON;
	POINT at = corner;
	switch (self->placement) {
	case PLACE_ABOVE:
		flags |= TPM_BOTTOMALIGN | TPM_LEFTALIGN;
		break;
	case PLACE_LEFT:
		flags |= TPM_RIGHTALIGN | TPM_TOPALIGN;
		break;
	case PLACE_RIGHT:
		at.x = end.x;
		flags |= TPM_LEFTALIGN | TPM_TOPALIGN;
		break;
	default:
		/* Beside a rectangle the menu opens below it; at a point, at the point. */
		at.y = end.y;
		flags |= TPM_LEFTALIGN | TPM_TOPALIGN;
	}
	const UINT chosen = (UINT)TrackPopupMenuEx(menu, flags, at.x, at.y, self->window, NULL);
	DestroyMenu(menu);
	if (chosen && chosen <= self->commands.count)
		self->chosen = chosen - 1;
}

static void CALLBACK show_waiting(HWND window, UINT message, UINT_PTR timer, DWORD time)
{
	KillTimer(NULL, timer);
	struct tracking *self = (struct tracking *)InterlockedExchangePointer((void **)&waiting_menu, NULL);
	if (!self)
		return;
	track(self);
	SetEvent(self->closed);
}

/* The operation's own thread waits for the menu to close, then calls the chosen command's handler. */
static HRESULT run_menu(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct tracking *self = context;
	WaitForSingleObject(self->closed, INFINITE);
	if (FAILED(self->error))
		return self->error;
	if (winrt_async_canceled(operation))
		return E_ABORT;
	return self->chosen < self->commands.count ? choose(self->commands.items[self->chosen], result) : S_OK;
}

/* A shortcut menu runs its loop on the thread of the window it belongs to, as on Windows, so the window's own thread asks for it: the
 * menu shows as soon as that thread's message loop goes on, and the operation completes when it closes. */
static HRESULT menu_show(void *iface, struct rect place, INT32 placement, void **out)
{
	struct menu *self = menu_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct tracking *tracking = ALLOC(sizeof *tracking);
	if (!tracking)
		return E_OUTOFMEMORY;
	HRESULT result = commands_take(self->commands, &tracking->commands);
	if (SUCCEEDED(result) && !(tracking->closed = CreateEventW(NULL, TRUE, FALSE, NULL)))
		result = HRESULT_FROM_WIN32(GetLastError());
	if (SUCCEEDED(result) && InterlockedCompareExchangePointer((void **)&waiting_menu, tracking, NULL))
		result = E_ACCESSDENIED;
	if (FAILED(result)) {
		free_tracking(tracking);
		return result;
	}
	tracking->window = self->window ? self->window : application_window();
	tracking->place = place;
	tracking->placement = placement;
	if (!(waiting_timer = SetTimer(NULL, 0, 0, show_waiting))) {
		waiting_menu = NULL;
		result = HRESULT_FROM_WIN32(GetLastError());
		free_tracking(tracking);
		return result;
	}
	result = winrt_async_start(&iid_command_operation, FALSE, WINRT_RESULT_OBJECT, run_menu, tracking, free_tracking, out);
	if (FAILED(result) && InterlockedExchangePointer((void **)&waiting_menu, NULL)) {
		/* The menu never showed: the work, which frees the context, never ran either. */
		KillTimer(NULL, waiting_timer);
	}
	return result;
}

static HRESULT WINAPI menu_show_at(void *iface, struct point point, void **out)
{
	const struct rect place = {point.x, point.y, 0, 0};
	return menu_show(iface, place, PLACE_DEFAULT, out);
}

static HRESULT WINAPI menu_show_beside(void *iface, struct rect selection, void **out) { return menu_show(iface, selection, PLACE_DEFAULT, out); }

static HRESULT WINAPI menu_show_placed(void *iface, struct rect selection, INT32 placement, void **out)
{
	return menu_show(iface, selection, placement, out);
}

static HRESULT WINAPI menu_initialize(void *iface, HWND window)
{
	if (window && !IsWindow(window))
		return E_INVALIDARG;
	menu_of(iface)->window = window;
	return S_OK;
}

TABLE(menu_table, 0, INSPECTABLE, (const void *)menu_commands, (const void *)menu_show_at, (const void *)menu_show_beside,
	(const void *)menu_show_placed);
TABLE(menu_window_table, 1, UNKNOWN, (const void *)menu_initialize);
static const struct face_iid menu_iids[] = {{&iid_menu, 0}, {&iid_initialize_with_window, 1}, {NULL, 0}};
static const struct kind menu_kind = {"PopupMenu", L"Windows.UI.Popups.PopupMenu", menu_iids, destroy_menu};

static HRESULT WINAPI menu_activate(void *iface, IInspectable **out)
{
	static const void *const *const tables[] = {menu_table.slots, menu_window_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct menu *self = make(sizeof *self, &menu_kind, tables, 2);
	if (!self)
		return E_OUTOFMEMORY;
	if (FAILED(winrt_vector(&command_list, NULL, 0, (void **)&self->commands))) {
		release(face_of(&self->base, 0));
		return E_OUTOFMEMORY;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

TABLE(menu_factory_table, 0, INSPECTABLE, (const void *)menu_activate);
static const struct face_iid menu_class_iids[] = {{&iid_factory, 0}, {NULL, 0}};
static const struct kind menu_class_kind = {"PopupMenu", L"Windows.UI.Popups.PopupMenu", menu_class_iids, NULL};
struct object popup_menu_class = {{menu_factory_table.slots}, &menu_class_kind, 1};
