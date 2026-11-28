// Windows.UI.Popups as a desktop app uses it, with the window it gives through IInitializeWithWindow: UICommand and its factory, a
// separator, a MessageDialog whose button and Escape choose commands, its single showing and canceling it, and a PopupMenu that shows
// from the window's own message loop and closes with EndMenu without a choice. The exit code names the first check that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <winstring.h>

typedef struct object object;
struct object {
	void **table;
};
#define CALL(self, place, type) ((type)(self)->table[place])

typedef HRESULT (WINAPI *query_method)(object *, const GUID *, void **);
typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *put_object_method)(object *, object *);
typedef HRESULT (WINAPI *string_method)(object *, HSTRING *);
typedef HRESULT (WINAPI *put_string_method)(object *, HSTRING);
typedef HRESULT (WINAPI *number_method)(object *, UINT32);
typedef HRESULT (WINAPI *get_number_method)(object *, UINT32 *);
typedef HRESULT (WINAPI *status_method)(object *, int *);
typedef HRESULT (WINAPI *plain_method)(object *);
typedef HRESULT (WINAPI *window_method)(object *, HWND);
typedef HRESULT (WINAPI *create_method)(object *, HSTRING, object **);
typedef HRESULT (WINAPI *create_titled_method)(object *, HSTRING, HSTRING, object **);
typedef HRESULT (WINAPI *create_with_id_method)(object *, HSTRING, object *, object *, object **);
struct point {
	FLOAT x, y;
};
typedef HRESULT (WINAPI *show_at_method)(object *, struct point, object **);

static const GUID command = {0x4ff93a75, 0x4145, 0x47ff, {0xac, 0x7f, 0xdf, 0xf1, 0xc1, 0xfa, 0x5b, 0x0f}};
static const GUID command_factory = {0xa21a8189, 0x26b0, 0x4676, {0xae, 0x94, 0x54, 0x04, 0x1b, 0xc1, 0x25, 0xe8}};
static const GUID dialog_factory = {0x2d161777, 0xa66f, 0x4ea5, {0xbb, 0x87, 0x79, 0x3f, 0xfa, 0x49, 0x41, 0xf2}};
static const GUID menu = {0x4e9bc6dc, 0x880d, 0x47fc, {0xa0, 0xa1, 0x72, 0xb6, 0x39, 0xe6, 0x25, 0x59}};
static const GUID initialize_with_window = {0x3e68d4bd, 0x7135, 0x4d10, {0x80, 0x18, 0x9f, 0xb6, 0xd9, 0xf3, 0x3f, 0xa1}};
static const GUID async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

static HSTRING text(const WCHAR *value)
{
	HSTRING made = NULL;
	WindowsCreateString(value, lstrlenW(value), &made);
	return made;
}

static BOOL same(HSTRING value, const WCHAR *expected)
{
	const BOOL equal = !lstrcmpW(WindowsGetStringRawBuffer(value, NULL), expected);
	WindowsDeleteString(value);
	return equal;
}

static void release(object *self)
{
	if (self)
		CALL(self, 2, release_method)(self);
}

static object *query(object *self, const GUID *iid)
{
	object *out = NULL;
	return self && CALL(self, 0, query_method)(self, iid, (void **)&out) == S_OK ? out : NULL;
}

static object *factory(const WCHAR *name, const GUID *iid)
{
	struct reference reference = {1, (UINT32)lstrlenW(name), {0, 0}, name};
	object *out = NULL;
	return RoGetActivationFactory((HSTRING)&reference, iid, (void **)&out) == S_OK ? out : NULL;
}

static object *activate(const WCHAR *name, const GUID *iid)
{
	struct reference reference = {1, (UINT32)lstrlenW(name), {0, 0}, name};
	object *instance = NULL;
	if (RoActivateInstance((HSTRING)&reference, (IInspectable **)&instance) != S_OK)
		return NULL;
	object *face = query(instance, iid);
	release(instance);
	return face;
}

/* The UICommandInvokedHandler: counts its calls and keeps the command it was given. */
static LONG invoked_count;
static object *invoked_with;
static HRESULT WINAPI handler_query(object *self, const GUID *iid, void **out) { *out = self; return S_OK; }
static ULONG WINAPI handler_count(object *self) { return 1; }
static HRESULT WINAPI handler_invoke(object *self, object *chosen)
{
	++invoked_count;
	invoked_with = chosen;
	return S_OK;
}
static void *handler_table[] = {(void *)handler_query, (void *)handler_count, (void *)handler_count, (void *)handler_invoke};
static object handler = {handler_table};

/* The operation's status once it ended, waiting up to ten seconds while pumping this thread's messages. */
static int ended(object *operation)
{
	object *info = query(operation, &async_info);
	int status = 0;
	for (int round = 0; info && round < 1000 && CALL(info, 7, status_method)(info, &status) == S_OK && !status; ++round) {
		MSG message;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&message);
		Sleep(10);
	}
	release(info);
	return status;
}

/* The task dialog of this process that is not the owner window: its title is the program's name. */
static HWND find_dialog(HWND owner)
{
	WCHAR program[MAX_PATH];
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
	for (int round = 0; round < 500; ++round) {
		HWND found = FindWindowW(NULL, name);
		if (found && found != owner)
			return found;
		Sleep(10);
	}
	return NULL;
}

/* Closes the menu from the window's own thread, when another thread asks while the menu's loop runs. */
#define CLOSE_MENU (WM_APP + 1)
static LRESULT CALLBACK owner_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == CLOSE_MENU) {
		EndMenu();
		return 1;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static DWORD WINAPI close_menu_later(void *window)
{
	Sleep(500);
	SendMessageW((HWND)window, CLOSE_MENU, 0, 0);
	return 0;
}

static int check_commands(object **first, object **second)
{
	object *commands = factory(L"Windows.UI.Popups.UICommand", &command_factory), *id = NULL, *got = NULL;
	if (!commands) return 10;
	HSTRING label = text(L"First");
	if (CALL(commands, 6, create_method)(commands, label, first) != S_OK || !*first) return 11;
	WindowsDeleteString(label);
	HSTRING value = NULL;
	if (CALL(*first, 6, string_method)(*first, &value) != S_OK || !same(value, L"First")) return 12;
	label = text(L"Renamed");
	if (CALL(*first, 7, put_string_method)(*first, label) != S_OK) return 13;
	WindowsDeleteString(label);
	if (CALL(*first, 6, string_method)(*first, &value) != S_OK || !same(value, L"Renamed")) return 14;
	/* The id is any object the app gives; a command made by the factory with a handler and an id keeps both. */
	label = text(L"Second");
	id = activate(L"Windows.UI.Popups.UICommand", &command);
	if (!id) return 15;
	if (CALL(commands, 8, create_with_id_method)(commands, label, &handler, id, second) != S_OK || !*second) return 16;
	WindowsDeleteString(label);
	if (CALL(*second, 10, object_method)(*second, &got) != S_OK || !got) return 17;
	release(got);
	if (CALL(*second, 8, object_method)(*second, &got) != S_OK || got != &handler) return 18;
	if (CALL(*first, 9, put_object_method)(*first, &handler) != S_OK) return 19;
	release(id);
	release(commands);
	object *separator = activate(L"Windows.UI.Popups.UICommandSeparator", &command);
	if (!separator) return 20;
	release(separator);
	return 0;
}

static object *make_dialog(object *first, object *second, HWND window)
{
	object *dialogs = factory(L"Windows.UI.Popups.MessageDialog", &dialog_factory), *made = NULL, *list = NULL;
	HSTRING content = text(L"Save the changes?"), title = text(L"Raven");
	if (dialogs)
		CALL(dialogs, 7, create_titled_method)(dialogs, content, title, &made);
	WindowsDeleteString(content);
	WindowsDeleteString(title);
	release(dialogs);
	object *initialize = query(made, &initialize_with_window);
	if (!initialize || CALL(initialize, 3, window_method)(initialize, window) != S_OK) {
		release(made);
		made = NULL;
	}
	release(initialize);
	if (first && made && (CALL(made, 8, object_method)(made, &list) != S_OK || CALL(list, 13, put_object_method)(list, first) != S_OK ||
		CALL(list, 13, put_object_method)(list, second) != S_OK)) {
		release(made);
		made = NULL;
	}
	release(list);
	return made;
}

static int check_dialog(object *first, object *second, HWND window)
{
	object *shown = make_dialog(first, second, window), *operation = NULL, *other = NULL, *result = NULL;
	if (!shown) return 30;
	HSTRING value = NULL;
	if (CALL(shown, 6, string_method)(shown, &value) != S_OK || !same(value, L"Raven")) return 31;
	if (CALL(shown, 13, string_method)(shown, &value) != S_OK || !same(value, L"Save the changes?")) return 32;
	UINT32 index = 0;
	if (CALL(shown, 11, get_number_method)(shown, &index) != S_OK || index != 0xffffffffu) return 33;
	if (CALL(shown, 10, number_method)(shown, 1) != S_OK || CALL(shown, 12, number_method)(shown, 0) != S_OK) return 34;
	if (CALL(shown, 15, object_method)(shown, &operation) != S_OK || !operation) return 35;
	/* One dialog shows at a time. */
	if (CALL(shown, 15, object_method)(shown, &other) != E_ACCESSDENIED || other) return 36;
	HWND box = find_dialog(window);
	if (!box) return 37;
	/* The second command's button: its handler runs, then the operation gives the command. */
	SendMessageW(box, WM_COMMAND, 101, 0);
	if (ended(operation) != 1) return 38;
	if (CALL(operation, 8, object_method)(operation, &result) != S_OK || result != second) return 39;
	if (invoked_count != 1 || invoked_with != second) return 40;
	release(result);
	release(operation);
	/* Escape chooses the cancel command. */
	if (CALL(shown, 15, object_method)(shown, &operation) != S_OK || !operation) return 41;
	if (!(box = find_dialog(window))) return 42;
	SendMessageW(box, WM_COMMAND, IDCANCEL, 0);
	if (ended(operation) != 1) return 43;
	if (CALL(operation, 8, object_method)(operation, &result) != S_OK || result != first) return 44;
	if (invoked_count != 2 || invoked_with != first) return 45;
	release(result);
	release(operation);
	release(shown);
	/* A dialog with no commands shows a Close button; canceling the operation closes it. */
	if (!(shown = make_dialog(NULL, NULL, window))) return 46;
	if (CALL(shown, 15, object_method)(shown, &operation) != S_OK || !operation) return 47;
	if (!find_dialog(window)) return 48;
	object *info = query(operation, &async_info);
	if (!info || CALL(info, 9, plain_method)(info) != S_OK) return 49;
	release(info);
	if (ended(operation) != 2) return 50;
	release(operation);
	release(shown);
	return 0;
}

static int check_menu(object *first, object *second, HWND window)
{
	object *shown = activate(L"Windows.UI.Popups.PopupMenu", &menu), *list = NULL, *operation = NULL, *other = NULL, *result = NULL;
	if (!shown) return 60;
	object *separator = activate(L"Windows.UI.Popups.UICommandSeparator", &command);
	if (CALL(shown, 6, object_method)(shown, &list) != S_OK || CALL(list, 13, put_object_method)(list, first) != S_OK ||
		CALL(list, 13, put_object_method)(list, separator) != S_OK || CALL(list, 13, put_object_method)(list, second) != S_OK) return 61;
	release(separator);
	release(list);
	object *initialize = query(shown, &initialize_with_window);
	if (!initialize || CALL(initialize, 3, window_method)(initialize, window) != S_OK) return 62;
	release(initialize);
	const struct point at = {10, 10};
	if (CALL(shown, 7, show_at_method)(shown, at, &operation) != S_OK || !operation) return 63;
	/* One menu waits to show at a time. */
	if (CALL(shown, 7, show_at_method)(shown, at, &other) != E_ACCESSDENIED || other) return 64;
	HANDLE closer = CreateThread(NULL, 0, close_menu_later, window, 0, NULL);
	if (!closer) return 65;
	/* The menu shows from this thread's loop and ends without a choice. */
	if (ended(operation) != 1) return 66;
	WaitForSingleObject(closer, INFINITE);
	CloseHandle(closer);
	if (CALL(operation, 8, object_method)(operation, &result) != S_OK || result) return 67;
	if (invoked_count != 2) return 68;
	release(operation);
	release(shown);
	return 0;
}

int raven_entry(void)
{
	if (FAILED(RoInitialize(RO_INIT_SINGLETHREADED))) return 1;
	WNDCLASSW type = {0};
	type.lpfnWndProc = owner_procedure;
	type.lpszClassName = L"RavenPopupsOwner";
	if (!RegisterClassW(&type)) return 2;
	HWND window = CreateWindowExW(0, L"RavenPopupsOwner", L"owner", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 300, 200, NULL, NULL, NULL, NULL);
	if (!window) return 3;
	object *first = NULL, *second = NULL;
	int failed = check_commands(&first, &second);
	if (!failed)
		failed = check_dialog(first, second, window);
	if (!failed)
		failed = check_menu(first, second, window);
	release(first);
	release(second);
	DestroyWindow(window);
	return failed;
}
