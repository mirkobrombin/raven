// Windows.ApplicationModel.DataTransfer as an app uses it: a DataPackage with text, properties and a format a provider gives, put on
// the clipboard with Clipboard.SetContent and read back through Win32 and Clipboard.GetContent; text another program put there; the
// CF_HTML text HtmlFormatHelper makes and reads; a format the clipboard does not have; and ContentChanged when the clipboard changes.
// The exit code names the first check that failed.
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
typedef HRESULT (WINAPI *string_object_method)(object *, HSTRING, object **);
typedef HRESULT (WINAPI *string_string_method)(object *, HSTRING, HSTRING *);
typedef HRESULT (WINAPI *string_put_method)(object *, HSTRING, object *);
typedef HRESULT (WINAPI *contains_method)(object *, HSTRING, boolean *);
typedef HRESULT (WINAPI *status_method)(object *, int *);
typedef HRESULT (WINAPI *add_method)(object *, object *, INT64 *);
typedef HRESULT (WINAPI *remove_method)(object *, INT64);

static const GUID formats_statics = {0x7ed681a1, 0xa880, 0x40c9, {0xb4, 0xed, 0x0b, 0xee, 0x1e, 0x15, 0xf5, 0x49}};
static const GUID clipboard_statics = {0xc627e291, 0x34e2, 0x4963, {0x8e, 0xed, 0x93, 0xcb, 0xb0, 0xea, 0x3d, 0x70}};
static const GUID html_statics = {0xe22e7749, 0xdd70, 0x446f, {0xae, 0xfc, 0x61, 0xce, 0xe5, 0x9f, 0x65, 0x5e}};
static const GUID value_statics = {0x629bdbc8, 0xd932, 0x4ff4, {0x96, 0xb9, 0x8d, 0x96, 0xc5, 0xc1, 0xe8, 0x58}};
static const GUID data_package = {0x61ebf5c7, 0xefea, 0x4346, {0x95, 0x54, 0x98, 0x1d, 0x7e, 0x19, 0x8f, 0xfe}};
static const GUID property_map = {0x1b0d3570, 0x0877, 0x5ec2, {0x8a, 0x2c, 0x3b, 0x95, 0x39, 0x50, 0x6a, 0xca}};
static const GUID property_value = {0x4bd682dd, 0x7554, 0x40e9, {0x9a, 0x9b, 0x82, 0x65, 0x4e, 0xde, 0x7e, 0x62}};
static const GUID async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static object *values;

static void release(object *self)
{
	if (self)
		CALL(self, 2, release_method)(self);
}

static HSTRING text_of(const WCHAR *text)
{
	HSTRING made = NULL;
	WindowsCreateString(text, lstrlenW(text), &made);
	return made;
}

static BOOL equal(HSTRING string, const WCHAR *text)
{
	UINT32 length = 0;
	const WCHAR *characters = WindowsGetStringRawBuffer(string, &length);
	return length == (UINT32)lstrlenW(text) && CompareStringOrdinal(characters, (int)length, text, -1, FALSE) == CSTR_EQUAL;
}

static object *statics(const WCHAR *name, const GUID *iid)
{
	HSTRING text = text_of(name);
	object *out = NULL;
	RoGetActivationFactory(text, iid, (void **)&out);
	WindowsDeleteString(text);
	return out;
}

/* Waits for an operation: its result into *result, and with error its error code into *error. */
static HRESULT finish(object *operation, void *result, HRESULT *error)
{
	object *info = NULL;
	int status = 0;
	if (!operation || CALL(operation, 0, query_method)(operation, &async_info, (void **)&info) != S_OK)
		return E_FAIL;
	while (CALL(info, 7, status_method)(info, &status) == S_OK && status == 0)
		Sleep(10);
	if (error)
		CALL(info, 8, status_method)(info, (int *)error);
	release(info);
	const HRESULT done = status != 1 ? E_FAIL : CALL(operation, 8, object_method)(operation, (object **)result);
	release(operation);
	return done;
}

/* A delegate: a DataProviderHandler that sets its text on the request, or an EventHandler that counts its calls. */
struct handler {
	void **table;
	LONG refs, calls;
	const WCHAR *text;
};

static HRESULT WINAPI handler_query(struct handler *self, const GUID *iid, void **out)
{
	*out = self;
	InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG WINAPI handler_reference(struct handler *self) { return (ULONG)InterlockedIncrement(&self->refs); }
static ULONG WINAPI handler_release(struct handler *self) { return (ULONG)InterlockedDecrement(&self->refs); }

static HRESULT WINAPI provide(struct handler *self, object *request)
{
	object *box = NULL;
	HSTRING text = text_of(self->text);
	InterlockedIncrement(&self->calls);
	/* PropertyValue.CreateString, then DataProviderRequest.SetData. */
	if (CALL(values, 18, string_object_method)(values, text, &box) == S_OK)
		CALL(request, 9, put_object_method)(request, box);
	release(box);
	WindowsDeleteString(text);
	return S_OK;
}

static HRESULT WINAPI changed(struct handler *self, object *sender, object *args)
{
	InterlockedIncrement(&self->calls);
	return S_OK;
}

static void *provider_table[] = {handler_query, handler_reference, handler_release, provide};
static void *changed_table[] = {handler_query, handler_reference, handler_release, changed};

static BOOL put_text(const WCHAR *text)
{
	const SIZE_T size = (lstrlenW(text) + 1) * sizeof(WCHAR);
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
	if (!memory || !OpenClipboard(NULL))
		return FALSE;
	WCHAR *data = GlobalLock(memory);
	for (SIZE_T at = 0; at < size / sizeof(WCHAR); ++at)
		data[at] = text[at];
	GlobalUnlock(memory);
	EmptyClipboard();
	const BOOL done = SetClipboardData(CF_UNICODETEXT, memory) != NULL;
	CloseClipboard();
	return done;
}

static BOOL clipboard_has(const WCHAR *text)
{
	BOOL found = FALSE;
	if (!OpenClipboard(NULL))
		return FALSE;
	HANDLE memory = GetClipboardData(CF_UNICODETEXT);
	const WCHAR *data = memory ? GlobalLock(memory) : NULL;
	if (data) {
		found = CompareStringOrdinal(data, -1, text, -1, FALSE) == CSTR_EQUAL;
		GlobalUnlock(memory);
	}
	CloseClipboard();
	return found;
}

/* Whether a view's text, through GetTextAsync, is text. */
static BOOL view_text(object *view, const WCHAR *text)
{
	object *operation = NULL;
	HSTRING got = NULL;
	CALL(view, 12, object_method)(view, &operation);
	const BOOL same = finish(operation, &got, NULL) == S_OK && equal(got, text);
	WindowsDeleteString(got);
	return same;
}

int raven_entry(void)
{
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;
	object *formats = statics(L"Windows.ApplicationModel.DataTransfer.StandardDataFormats", &formats_statics);
	object *clipboard = statics(L"Windows.ApplicationModel.DataTransfer.Clipboard", &clipboard_statics);
	object *html = statics(L"Windows.ApplicationModel.DataTransfer.HtmlFormatHelper", &html_statics);
	values = statics(L"Windows.Foundation.PropertyValue", &value_statics);
	if (!formats || !clipboard || !html || !values) return 2;

	/* The standard formats are named as on Windows. */
	HSTRING text_format = NULL, html_format = NULL;
	if (CALL(formats, 6, string_method)(formats, &text_format) != S_OK || !equal(text_format, L"Text")) return 10;
	if (CALL(formats, 8, string_method)(formats, &html_format) != S_OK || !equal(html_format, L"HTML Format")) return 11;

	/* A package with text, a title and a format a provider gives. */
	HSTRING package_name = text_of(L"Windows.ApplicationModel.DataTransfer.DataPackage");
	object *instance = NULL, *package = NULL, *properties = NULL, *map = NULL, *value = NULL, *box = NULL;
	if (RoActivateInstance(package_name, (IInspectable **)&instance) != S_OK ||
		CALL(instance, 0, query_method)(instance, &data_package, (void **)&package) != S_OK)
		return 20;
	release(instance);
	HSTRING hello = text_of(L"hello from the package"), title = text_of(L"A title"), key = text_of(L"Title"), got = NULL;
	if (CALL(package, 16, put_string_method)(package, hello) != S_OK) return 21;
	if (CALL(package, 7, object_method)(package, &properties) != S_OK || !properties) return 22;
	if (CALL(properties, 7, put_string_method)(properties, title) != S_OK) return 23;
	if (CALL(properties, 6, string_method)(properties, &got) != S_OK || !equal(got, L"A title")) return 24;
	WindowsDeleteString(got);
	/* The properties are a map whose key Title holds the title. */
	if (CALL(properties, 0, query_method)(properties, &property_map, (void **)&map) != S_OK) return 25;
	if (CALL(map, 6, string_object_method)(map, key, &value) != S_OK ||
		CALL(value, 0, query_method)(value, &property_value, (void **)&box) != S_OK ||
		CALL(box, 19, string_method)(box, &got) != S_OK || !equal(got, L"A title"))
		return 26;
	WindowsDeleteString(got);
	release(box);
	release(value);
	struct handler provider = {provider_table, 1, 0, L"provided later"};
	HSTRING custom = text_of(L"RavenCustom");
	if (CALL(package, 15, string_put_method)(package, custom, (object *)&provider) != S_OK) return 27;

	/* SetContent puts the text on the clipboard Win32 reads, and asks the provider. */
	if (CALL(clipboard, 7, put_object_method)(clipboard, package) != S_OK) return 30;
	if (!clipboard_has(L"hello from the package")) return 31;
	if (!provider.calls) return 32;

	/* GetContent gives the package back: its text, its title and the provider's format. */
	object *view = NULL, *view_properties = NULL, *operation = NULL;
	boolean has = FALSE;
	if (CALL(clipboard, 6, object_method)(clipboard, &view) != S_OK || !view) return 40;
	if (CALL(view, 10, contains_method)(view, text_format, &has) != S_OK || !has) return 41;
	if (!view_text(view, L"hello from the package")) return 42;
	if (CALL(view, 6, object_method)(view, &view_properties) != S_OK || !view_properties ||
		CALL(view_properties, 6, string_method)(view_properties, &got) != S_OK || !equal(got, L"A title"))
		return 43;
	WindowsDeleteString(got);
	release(view_properties);
	CALL(view, 13, string_object_method)(view, custom, &operation);
	if (finish(operation, &got, NULL) != S_OK || !equal(got, L"provided later")) return 44;
	WindowsDeleteString(got);
	/* A format the package does not have ends the operation in error. */
	HRESULT error = S_OK;
	CALL(view, 17, object_method)(view, &operation);
	if (finish(operation, &got, &error) == S_OK || error != (HRESULT)0x80040064) return 45;
	release(view);

	/* Text another program put on the clipboard is what GetContent reads. */
	if (!put_text(L"put there by Win32")) return 50;
	if (CALL(clipboard, 6, object_method)(clipboard, &view) != S_OK || !view) return 51;
	if (CALL(view, 10, contains_method)(view, text_format, &has) != S_OK || !has) return 52;
	if (!view_text(view, L"put there by Win32")) return 53;
	release(view);

	/* HtmlFormatHelper makes CF_HTML text and finds the fragment in it again. */
	HSTRING fragment = text_of(L"<b>caf\x00e9</b>"), made = NULL, back = NULL;
	if (CALL(html, 7, string_string_method)(html, fragment, &made) != S_OK) return 60;
	if (CALL(html, 6, string_string_method)(html, made, &back) != S_OK || !equal(back, L"<b>caf\x00e9</b>")) return 61;
	WindowsDeleteString(back);

	/* ContentChanged hears a change of the clipboard. */
	struct handler counter = {changed_table, 1, 0, NULL};
	INT64 token = 0;
	if (CALL(clipboard, 10, add_method)(clipboard, (object *)&counter, &token) != S_OK) return 70;
	if (!put_text(L"changed")) return 71;
	for (int tries = 0; tries < 300 && !counter.calls; ++tries)
		Sleep(10);
	if (!counter.calls) return 72;
	if (CALL(clipboard, 11, remove_method)(clipboard, token) != S_OK) return 73;

	release(map);
	release(properties);
	release(package);
	release(values);
	release(html);
	release(clipboard);
	release(formats);
	return 0;
}
