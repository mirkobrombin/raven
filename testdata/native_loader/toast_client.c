// Windows.UI.Notifications as a desktop app uses it: a template filled in and shown, a toast made from its own XML and shown, one that
// asks for no popup, and what the classes refuse. The script that runs this program checks what reached the desktop. The exit code names
// the first check that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <winstring.h>

typedef struct object object;
struct object {
	void **table;
};
#define CALL(self, place, type) ((type)(self)->table[place])

typedef HRESULT (WINAPI *query_method)(object *, const GUID *, object **);
typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *give_method)(object *, object *);
typedef HRESULT (WINAPI *make_method)(object *, object *, object **);
typedef HRESULT (WINAPI *template_method)(object *, INT32, object **);
typedef HRESULT (WINAPI *named_method)(object *, HSTRING, object **);
typedef HRESULT (WINAPI *at_method)(object *, UINT32, object **);
typedef HRESULT (WINAPI *size_method)(object *, UINT32 *);
typedef HRESULT (WINAPI *string_method)(object *, HSTRING *);
typedef HRESULT (WINAPI *set_string_method)(object *, HSTRING);
typedef HRESULT (WINAPI *number_method)(object *, INT32 *);
typedef HRESULT (WINAPI *flag_method)(object *, boolean *);
typedef HRESULT (WINAPI *set_flag_method)(object *, boolean);

static const GUID manager_statics = {0x50ac103f, 0xd235, 0x4598, {0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4}};
static const GUID toast_factory = {0x04124b20, 0x82c6, 0x4229, {0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53}};
static const GUID toast_iid = {0x997e2675, 0x059e, 0x4e60, {0x8b, 0x06, 0x17, 0x60, 0x91, 0x7c, 0x8b, 0x80}};
static const GUID toast2_iid = {0x9dfb9fd1, 0x143a, 0x490e, {0x90, 0xbf, 0xb9, 0xfb, 0xa7, 0x13, 0x2d, 0xe7}};
static const GUID selector_iid = {0x63dbba8b, 0xd0db, 0x4fe1, {0xb7, 0x45, 0xf9, 0x43, 0x3a, 0xfd, 0xc2, 0x5b}};
static const GUID serializer_iid = {0x5cc5b382, 0xe6dd, 0x4991, {0xab, 0xef, 0x06, 0xd8, 0xd2, 0xe7, 0xbd, 0x0c}};
static const GUID document_io_iid = {0x6cd0e74e, 0xee65, 0x4489, {0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37}};
static const GUID unknown_iid = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/* ToastTemplateType. */
enum { TOAST_IMAGE_AND_TEXT01 = 0, TOAST_TEXT02 = 5 };

struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

static object *factory(const WCHAR *name, const GUID *iid)
{
	struct reference reference = {1, (UINT32)lstrlenW(name), {0, 0}, name};
	object *out = NULL;
	return RoGetActivationFactory((HSTRING)&reference, iid, (void **)&out) == S_OK ? out : NULL;
}

static object *activate(const WCHAR *name, const GUID *iid)
{
	struct reference reference = {1, (UINT32)lstrlenW(name), {0, 0}, name};
	object *made = NULL, *out = NULL;
	if (RoActivateInstance((HSTRING)&reference, (IInspectable **)&made) != S_OK || !made)
		return NULL;
	CALL(made, 0, query_method)(made, iid, &out);
	CALL(made, 2, release_method)(made);
	return out;
}

static void release(object *self)
{
	if (self)
		CALL(self, 2, release_method)(self);
}

static object *query(object *self, const GUID *iid)
{
	object *out = NULL;
	return self && CALL(self, 0, query_method)(self, iid, &out) == S_OK ? out : NULL;
}

static HSTRING text(const WCHAR *value)
{
	HSTRING made = NULL;
	WindowsCreateString(value, lstrlenW(value), &made);
	return made;
}

static BOOL same(HSTRING value, const WCHAR *expected)
{
	UINT32 length = 0;
	const WCHAR *raw = WindowsGetStringRawBuffer(value, &length);
	return length == (UINT32)lstrlenW(expected) && CompareStringOrdinal(raw, (int)length, expected, -1, FALSE) == CSTR_EQUAL;
}

/* The nodes an XPath expression selects in a node, through IXmlNodeSelector.SelectNodes. */
static object *select(object *node, const WCHAR *xpath)
{
	object *selector = query(node, &selector_iid), *list = NULL;
	HSTRING expression = text(xpath);
	if (selector)
		CALL(selector, 7, named_method)(selector, expression, &list);
	WindowsDeleteString(expression);
	release(selector);
	return list;
}

static UINT32 count(object *node, const WCHAR *xpath)
{
	object *list = select(node, xpath);
	UINT32 length = 0;
	if (list)
		CALL(list, 6, size_method)(list, &length);
	release(list);
	return length;
}

/* The inner text of the first node selected, through IXmlNodeSerializer, or sets it. */
static BOOL inner_text(object *node, const WCHAR *xpath, const WCHAR *expected, const WCHAR *replacement)
{
	object *list = select(node, xpath), *item = NULL, *serializer = NULL;
	HSTRING value = NULL;
	BOOL result = FALSE;
	if (list && CALL(list, 7, at_method)(list, 0, &item) == S_OK && (serializer = query(item, &serializer_iid))) {
		if (replacement) {
			value = text(replacement);
			result = CALL(serializer, 8, set_string_method)(serializer, value) == S_OK;
		} else {
			result = CALL(serializer, 7, string_method)(serializer, &value) == S_OK && same(value, expected);
		}
	}
	WindowsDeleteString(value);
	release(serializer);
	release(item);
	release(list);
	return result;
}

static object *toast_of(object *toasts, object *content)
{
	object *toast = NULL;
	return CALL(toasts, 6, make_method)(toasts, content, &toast) == S_OK ? toast : NULL;
}

int raven_entry(void)
{
	object *manager, *toasts, *notifier = NULL, *content = NULL, *toast, *options, *given = NULL;
	HSTRING value = NULL;
	INT32 setting = -1;
	boolean flag = TRUE;
	if (RoInitialize(RO_INIT_MULTITHREADED) < 0)
		return 1;
	if (!(manager = factory(L"Windows.UI.Notifications.ToastNotificationManager", &manager_statics)))
		return 2;
	if (!(toasts = factory(L"Windows.UI.Notifications.ToastNotification", &toast_factory)))
		return 3;
	// The templates: a binding named after the template, with the texts and the image of its layout.
	if (CALL(manager, 8, template_method)(manager, TOAST_TEXT02, &content) != S_OK || !content)
		return 4;
	if (count(content, L"/toast/visual/binding/text") != 2 || count(content, L"/toast/visual/binding/image") != 0 ||
		!inner_text(content, L"/toast/visual/binding/@template", L"ToastText02", NULL))
		return 5;
	object *imaged = NULL;
	if (CALL(manager, 8, template_method)(manager, TOAST_IMAGE_AND_TEXT01, &imaged) != S_OK || count(imaged, L"//text") != 1 ||
		count(imaged, L"/toast/visual/binding/image[@id='1']") != 1)
		return 6;
	release(imaged);
	if (CALL(manager, 8, template_method)(manager, 8, &imaged) != E_INVALIDARG)
		return 7;
	// The template filled in and shown.
	if (!inner_text(content, L"//text[@id='1']", NULL, L"Template title") || !inner_text(content, L"//text[@id='2']", NULL, L"Template text"))
		return 8;
	if (CALL(manager, 6, object_method)(manager, &notifier) != S_OK || !notifier)
		return 9;
	if (CALL(notifier, 8, number_method)(notifier, &setting) != S_OK || setting != 0)
		return 10;
	if (!(toast = toast_of(toasts, content)))
		return 11;
	if (CALL(toast, 6, object_method)(toast, &given) != S_OK || !given)
		return 12;
	object *left = query(given, &unknown_iid), *right = query(content, &unknown_iid);
	if (!left || left != right)
		return 13;
	release(left);
	release(right);
	release(given);
	if (CALL(notifier, 6, give_method)(notifier, toast) != S_OK)
		return 14;
	release(toast);
	release(content);
	// A toast of the app's own XML, three texts: the title and two lines.
	if (!(content = activate(L"Windows.Data.Xml.Dom.XmlDocument", &document_io_iid)))
		return 15;
	value = text(L"<toast><visual><binding template=\"ToastGeneric\"><text>Title</text><text>Line one</text><text>Line two</text>"
		L"</binding></visual></toast>");
	if (CALL(content, 6, set_string_method)(content, value) != S_OK)
		return 16;
	WindowsDeleteString(value);
	value = NULL;
	if (!(toast = toast_of(toasts, content)) || !(options = query(toast, &toast2_iid)))
		return 17;
	value = text(L"build-42");
	if (CALL(options, 6, set_string_method)(options, value) != S_OK)
		return 18;
	WindowsDeleteString(value);
	value = NULL;
	if (CALL(options, 7, string_method)(options, &value) != S_OK || !same(value, L"build-42"))
		return 19;
	WindowsDeleteString(value);
	if (CALL(options, 11, flag_method)(options, &flag) != S_OK || flag)
		return 20;
	if (CALL(notifier, 6, give_method)(notifier, toast) != S_OK)
		return 21;
	// One that asks for no popup is not shown.
	if (CALL(options, 10, set_flag_method)(options, TRUE) != S_OK || CALL(notifier, 6, give_method)(notifier, toast) != S_OK)
		return 22;
	if (CALL(notifier, 7, give_method)(notifier, toast) != S_OK)
		return 23;
	release(options);
	release(toast);
	// A toast needs content, and a notifier shows only toasts.
	if (CALL(toasts, 6, make_method)(toasts, NULL, &toast) != E_INVALIDARG)
		return 24;
	if (CALL(notifier, 6, give_method)(notifier, NULL) != E_INVALIDARG || CALL(notifier, 6, give_method)(notifier, content) != E_INVALIDARG)
		return 25;
	release(content);
	release(notifier);
	release(toasts);
	release(manager);
	return 0;
}
