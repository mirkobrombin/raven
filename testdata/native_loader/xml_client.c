// Windows.Data.Xml.Dom as an app uses it: a document loaded from text and read through its element, attributes, child nodes and XPath
// (with and without namespaces); nodes created, appended, changed, cloned, imported and removed, and written back by GetXml; text split;
// a malformed document and a DTD refused by default, a DTD and a depth limit taken from XmlLoadSettings; a document loaded from a file and
// saved back to it. The exit code names the first check that failed.
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
typedef HRESULT (WINAPI *string_method)(object *, HSTRING *);
typedef HRESULT (WINAPI *set_string_method)(object *, HSTRING);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *type_method)(object *, INT32 *);
typedef HRESULT (WINAPI *size_method)(object *, UINT32 *);
typedef HRESULT (WINAPI *at_method)(object *, UINT32, object **);
typedef HRESULT (WINAPI *named_method)(object *, HSTRING, object **);
typedef HRESULT (WINAPI *named_string_method)(object *, HSTRING, HSTRING *);
typedef HRESULT (WINAPI *pair_method)(object *, HSTRING, HSTRING);
typedef HRESULT (WINAPI *child_method)(object *, object *, object **);
typedef HRESULT (WINAPI *deep_method)(object *, boolean, object **);
typedef HRESULT (WINAPI *select_ns_method)(object *, HSTRING, object *, object **);
typedef HRESULT (WINAPI *load_with_method)(object *, HSTRING, object *);
typedef HRESULT (WINAPI *import_method)(object *, object *, boolean, object **);
typedef HRESULT (WINAPI *flag_set_method)(object *, boolean);
typedef HRESULT (WINAPI *depth_set_method)(object *, UINT32);

static const GUID node_iid = {0x1c741d59, 0x2122, 0x47d5, {0xa8, 0x56, 0x83, 0xf3, 0xd4, 0x21, 0x48, 0x75}};
static const GUID selector_iid = {0x63dbba8b, 0xd0db, 0x4fe1, {0xb7, 0x45, 0xf9, 0x43, 0x3a, 0xfd, 0xc2, 0x5b}};
static const GUID serializer_iid = {0x5cc5b382, 0xe6dd, 0x4991, {0xab, 0xef, 0x06, 0xd8, 0xd2, 0xe7, 0xbd, 0x0c}};
static const GUID element_iid = {0x2dfb8a1f, 0x6b10, 0x4ef8, {0x9f, 0x83, 0xef, 0xcc, 0xe8, 0xfa, 0xec, 0x37}};
static const GUID attribute_iid = {0xac144aa4, 0xb4f1, 0x4db6, {0xb2, 0x06, 0x8a, 0x22, 0xc3, 0x08, 0xdb, 0x0a}};
static const GUID text_iid = {0xf931a4cb, 0x308d, 0x4760, {0xa1, 0xd5, 0x43, 0xb6, 0x74, 0x50, 0xac, 0x7e}};
static const GUID data_iid = {0x132e42ab, 0x4e36, 0x4df6, {0xb1, 0xc8, 0x0c, 0xe6, 0x2f, 0xd8, 0x8b, 0x26}};
static const GUID document_iid = {0xf7f3a506, 0x1e87, 0x42d6, {0xbc, 0xfb, 0xb8, 0xc8, 0x09, 0xfa, 0x54, 0x94}};
static const GUID document_io_iid = {0x6cd0e74e, 0xee65, 0x4489, {0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37}};
static const GUID document_statics = {0x5543d254, 0xd757, 0x4b79, {0x95, 0x39, 0x23, 0x2b, 0x18, 0xf5, 0x0b, 0xf1}};
static const GUID load_settings_iid = {0x58aa07a8, 0xfed6, 0x46f7, {0xb4, 0xc5, 0xfb, 0x1b, 0xa7, 0x21, 0x08, 0xd6}};
static const GUID node_view_iid = {0x139d959e, 0xe7b5, 0x5cb6, {0xa5, 0x96, 0x4b, 0x54, 0x44, 0x78, 0xda, 0x9b}};
static const GUID value_statics = {0x629bdbc8, 0xd932, 0x4ff4, {0x96, 0xb9, 0x8d, 0x96, 0xc5, 0xc1, 0xe8, 0x58}};
static const GUID property_value_iid = {0x4bd682dd, 0x7554, 0x40e9, {0x9a, 0x9b, 0x82, 0x65, 0x4e, 0xde, 0x7e, 0x62}};
static const GUID file_statics = {0x5984c710, 0xdaf2, 0x43c8, {0x8b, 0xb4, 0xa4, 0xd3, 0xea, 0xcf, 0xd0, 0x3f}};
static const GUID async_info_iid = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

enum { TYPE_CDATA = 4, TYPE_COMMENT = 8 };

struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

static object *documents, *values, *files;

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
	const WCHAR *chars = WindowsGetStringRawBuffer(value, &length);
	const BOOL equal = length == (UINT32)lstrlenW(expected) && CompareStringOrdinal(chars, (int)length, expected, -1, FALSE) == CSTR_EQUAL;
	WindowsDeleteString(value);
	return equal;
}

/* A string through any method that takes nothing and answers one, compared with the text expected. */
static BOOL gives(object *self, const GUID *iid, int place, const WCHAR *expected)
{
	object *face = query(self, iid);
	HSTRING value = NULL;
	const BOOL equal = face && CALL(face, place, string_method)(face, &value) == S_OK && same(value, expected);
	release(face);
	return equal;
}

static BOOL xml_of(object *node, const WCHAR *expected) { return gives(node, &serializer_iid, 6, expected); }
static BOOL inner_text(object *node, const WCHAR *expected) { return gives(node, &serializer_iid, 7, expected); }

/* An Object that is a boxed string, as namespaces and names are, compared with the text expected. */
static BOOL boxed_as(object *value, const WCHAR *expected)
{
	object *box = query(value, &property_value_iid);
	HSTRING string = NULL;
	const BOOL equal = box && CALL(box, 19, string_method)(box, &string) == S_OK && same(string, expected);
	release(box);
	return equal;
}

static object *box(const WCHAR *value)
{
	HSTRING string = text(value);
	object *out = NULL;
	CALL(values, 18, HRESULT (WINAPI *)(object *, HSTRING, object **))(values, string, &out);
	WindowsDeleteString(string);
	return out;
}

static HRESULT load(object *document, const WCHAR *xml)
{
	object *io = query(document, &document_io_iid);
	HSTRING string = text(xml);
	const HRESULT result = io ? CALL(io, 6, set_string_method)(io, string) : E_NOINTERFACE;
	WindowsDeleteString(string);
	release(io);
	return result;
}

static HRESULT load_with(object *document, const WCHAR *xml, object *settings)
{
	object *io = query(document, &document_io_iid);
	HSTRING string = text(xml);
	const HRESULT result = io ? CALL(io, 7, load_with_method)(io, string, settings) : E_NOINTERFACE;
	WindowsDeleteString(string);
	release(io);
	return result;
}

static object *named(object *self, int place, const WCHAR *name)
{
	HSTRING key = text(name);
	object *out = NULL;
	CALL(self, place, named_method)(self, key, &out);
	WindowsDeleteString(key);
	return out;
}

static object *node_of(object *self)
{
	object *node = query(self, &node_iid);
	release(self);
	return node;
}

static object *child_at(object *node, UINT32 index)
{
	object *children = NULL, *child = NULL;
	if (CALL(node, 11, object_method)(node, &children) == S_OK && children)
		CALL(children, 7, at_method)(children, index, &child);
	release(children);
	return child;
}

static UINT32 count_of(object *list)
{
	UINT32 length = 0;
	return list && CALL(list, 6, size_method)(list, &length) == S_OK ? length : (UINT32)-1;
}

static UINT32 child_count(object *node)
{
	object *children = NULL;
	CALL(node, 11, object_method)(node, &children);
	const UINT32 length = count_of(children);
	release(children);
	return length;
}

static INT32 type_of(object *node)
{
	INT32 type = 0;
	return node && CALL(node, 8, type_method)(node, &type) == S_OK ? type : -1;
}

static object *document, *root;

static int check_reading(void)
{
	if (load(document, L"<?xml version=\"1.0\"?><root a=\"1\" xmlns:p=\"urn:p\"><item id=\"x\">one</item><item>two</item><!--c-->"
		L"<p:q p:b=\"2\"/><![CDATA[<raw>]]></root>") != S_OK) return 10;
	object *element = NULL;
	if (CALL(document, 8, object_method)(document, &element) != S_OK || !element) return 11;
	root = node_of(element);
	if (!gives(root, &element_iid, 6, L"root")) return 12;
	object *as_element = query(root, &element_iid);
	HSTRING key = text(L"a"), value = NULL;
	if (CALL(as_element, 7, named_string_method)(as_element, key, &value) != S_OK || !same(value, L"1")) return 13;
	WindowsDeleteString(key);
	key = text(L"missing");
	if (CALL(as_element, 7, named_string_method)(as_element, key, &value) != S_OK || !same(value, L"")) return 14;
	WindowsDeleteString(key);
	release(as_element);

	object *items = named(document, 16, L"item");
	if (count_of(items) != 2) return 15;
	object *view = query(items, &node_view_iid);
	UINT32 size = 0;
	if (!view || CALL(view, 7, size_method)(view, &size) != S_OK || size != 2) return 16;
	object *first = NULL;
	if (CALL(view, 6, at_method)(view, 0, &first) != S_OK || !inner_text(first, L"one")) return 17;
	object *beyond = NULL;
	if (CALL(view, 6, at_method)(view, 2, &beyond) != E_BOUNDS) return 18;
	if (!xml_of(first, L"<item id=\"x\">one</item>")) return 19;
	release(view);
	release(items);

	object *selector = query(root, &selector_iid);
	object *selected = named(selector, 7, L"item");
	if (count_of(selected) != 2) return 20;
	release(selected);
	selected = named(selector, 6, L"item[2]");
	if (!selected || !inner_text(selected, L"two")) return 21;
	release(selected);
	object *namespaces = box(L"xmlns:p='urn:p'"), *qualified = NULL, *name = NULL, *uri = NULL;
	HSTRING xpath = text(L"p:q");
	if (CALL(selector, 8, select_ns_method)(selector, xpath, namespaces, &qualified) != S_OK || !qualified) return 22;
	WindowsDeleteString(xpath);
	if (CALL(qualified, 25, object_method)(qualified, &name) != S_OK || !boxed_as(name, L"q")) return 23;
	if (CALL(qualified, 24, object_method)(qualified, &uri) != S_OK || !boxed_as(uri, L"urn:p")) return 24;
	release(name);
	release(uri);
	release(qualified);
	release(namespaces);
	release(selector);

	if (child_count(root) != 5) return 25;
	object *comment = child_at(root, 2), *section = child_at(root, 4);
	if (type_of(comment) != TYPE_COMMENT || type_of(section) != TYPE_CDATA) return 26;
	if (!gives(section, &data_iid, 6, L"<raw>") || !query(section, &text_iid)) return 27;
	release(comment);
	release(section);

	object *attributes = NULL, *id = NULL;
	if (CALL(first, 16, object_method)(first, &attributes) != S_OK || count_of(attributes) != 1) return 28;
	if (!(id = named(attributes, 8, L"id")) || !gives(id, &attribute_iid, 8, L"x")) return 29;
	release(id);
	release(attributes);
	release(first);
	return 0;
}

static int check_building(void)
{
	object *made = NULL, *appended = NULL, *line = NULL, *removed = NULL;
	HSTRING name = text(L"new"), key = text(L"k"), value = text(L"v&"), content = text(L"t");
	if (CALL(document, 9, named_method)(document, name, &made) != S_OK || !made) return 30;
	object *element = made;
	if (CALL(element, 8, pair_method)(element, key, value) != S_OK) return 31;
	made = query(element, &node_iid);
	if (CALL(root, 22, child_method)(root, made, &appended) != S_OK || !appended) return 32;
	release(appended);
	if (CALL(document, 11, named_method)(document, content, &line) != S_OK) return 33;
	line = node_of(line);
	if (CALL(made, 22, child_method)(made, line, &appended) != S_OK) return 34;
	release(appended);
	if (!xml_of(made, L"<new k=\"v&amp;\">t</new>") || child_count(root) != 6) return 35;
	if (CALL(root, 21, child_method)(root, made, &removed) != S_OK || child_count(root) != 5) return 36;
	release(removed);
	WindowsDeleteString(name);
	WindowsDeleteString(key);
	WindowsDeleteString(value);
	WindowsDeleteString(content);

	object *second = child_at(root, 1), *serializer = query(second, &serializer_iid);
	HSTRING replacement = text(L"zwei");
	if (CALL(serializer, 8, set_string_method)(serializer, replacement) != S_OK || !inner_text(second, L"zwei")) return 37;
	WindowsDeleteString(replacement);
	release(serializer);

	object *copy = NULL, *parent = NULL;
	if (CALL(second, 23, deep_method)(second, TRUE, &copy) != S_OK || !xml_of(copy, L"<item>zwei</item>")) return 38;
	if (CALL(copy, 10, object_method)(copy, &parent) != S_OK || parent) return 39;
	release(copy);

	object *other = activate(L"Windows.Data.Xml.Dom.XmlDocument", &document_iid), *imported = NULL;
	if (!other || CALL(other, 22, import_method)(other, second, TRUE, &imported) != S_OK || !xml_of(imported, L"<item>zwei</item>")) return 40;
	release(imported);
	release(other);

	/* SplitText: the text keeps what is before the offset, the rest follows it as a new node. */
	object *word = child_at(second, 0), *as_text = query(word, &text_iid), *rest = NULL;
	if (!as_text || CALL(as_text, 6, at_method)(as_text, 2, &rest) != S_OK || !rest) return 41;
	if (!gives(word, &data_iid, 6, L"zw") || !gives(rest, &data_iid, 6, L"ei") || child_count(second) != 2) return 42;
	release(rest);
	release(as_text);
	release(word);
	release(element);
	release(made);
	release(line);
	release(second);
	return 0;
}

static int check_loading(void)
{
	object *fresh = activate(L"Windows.Data.Xml.Dom.XmlDocument", &document_iid);
	const HRESULT broken = load(fresh, L"<a><b></a>");
	if (SUCCEEDED(broken) || ((UINT32)broken >> 16) != 0xc00c) return 50;
	const WCHAR *dtd = L"<!DOCTYPE a [<!ENTITY e 'x'>]><a>&e;</a>";
	if (SUCCEEDED(load(fresh, dtd))) return 51;
	object *settings = activate(L"Windows.Data.Xml.Dom.XmlLoadSettings", &load_settings_iid);
	boolean prohibited = FALSE;
	UINT32 depth = 0;
	if (!settings || CALL(settings, 8, HRESULT (WINAPI *)(object *, boolean *))(settings, &prohibited) != S_OK || !prohibited) return 52;
	if (CALL(settings, 6, size_method)(settings, &depth) != S_OK || depth != 256) return 53;
	CALL(settings, 9, flag_set_method)(settings, FALSE);
	if (load_with(fresh, dtd, settings) != S_OK) return 54;
	object *element = NULL;
	if (CALL(fresh, 8, object_method)(fresh, &element) != S_OK || !inner_text(element, L"x")) return 55;
	release(element);
	CALL(settings, 7, depth_set_method)(settings, 2);
	if (SUCCEEDED(load_with(fresh, L"<a><b><c/></b></a>", settings))) return 56;
	if (load_with(fresh, L"<a><b/></a>", settings) != S_OK) return 57;
	release(settings);
	release(fresh);
	return 0;
}

/* Waits for an operation or an action and takes its results. */
static HRESULT finish(object *operation, object **result)
{
	object *info = query(operation, &async_info_iid);
	INT32 status = 0;
	HRESULT error = S_OK;
	for (int tries = 0; info && tries < 1000; ++tries) {
		CALL(info, 7, type_method)(info, &status);
		if (status)
			break;
		Sleep(5);
	}
	if (info && status != 1)
		CALL(info, 8, HRESULT (WINAPI *)(object *, HRESULT *))(info, &error);
	release(info);
	if (status != 1)
		return FAILED(error) ? error : E_FAIL;
	const HRESULT got = result ? CALL(operation, 8, object_method)(operation, result) : CALL(operation, 8, HRESULT (WINAPI *)(object *))(operation);
	release(operation);
	return got;
}

static int check_files(void)
{
	WCHAR path[MAX_PATH];
	DWORD wrote = 0;
	GetTempPathW(MAX_PATH - 32, path);
	lstrcatW(path, L"raven-xml-check.xml");
	HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	const char saved[] = "<doc><v>1</v></doc>";
	if (handle == INVALID_HANDLE_VALUE || !WriteFile(handle, saved, sizeof saved - 1, &wrote, NULL)) return 60;
	CloseHandle(handle);
	object *operation = NULL, *file = NULL, *loaded = NULL;
	HSTRING name = text(path);
	if (CALL(files, 6, named_method)(files, name, &operation) != S_OK || finish(operation, &file) != S_OK || !file) return 61;
	WindowsDeleteString(name);
	if (CALL(documents, 8, child_method)(documents, file, &operation) != S_OK || finish(operation, &loaded) != S_OK || !loaded) return 62;
	object *selector = query(loaded, &selector_iid), *top = named(selector, 6, L"/doc");
	if (!top || !xml_of(top, L"<doc><v>1</v></doc>")) return 63;
	release(top);
	object *v = named(selector, 6, L"/doc/v"), *serializer = query(v, &serializer_iid);
	HSTRING two = text(L"2");
	if (!serializer || CALL(serializer, 8, set_string_method)(serializer, two) != S_OK) return 64;
	WindowsDeleteString(two);
	object *io = query(loaded, &document_io_iid);
	if (CALL(io, 8, child_method)(io, file, &operation) != S_OK || finish(operation, NULL) != S_OK) return 65;
	char back[128] = {0};
	DWORD read = 0;
	handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (handle == INVALID_HANDLE_VALUE || !ReadFile(handle, back, sizeof back - 1, &read, NULL)) return 66;
	CloseHandle(handle);
	DeleteFileW(path);
	BOOL found = FALSE;
	for (DWORD at = 0; at + 8 <= read && !found; ++at)
		found = CompareStringA(LOCALE_INVARIANT, 0, back + at, 8, "<v>2</v>", 8) == CSTR_EQUAL;
	if (!found) return 67;
	release(io);
	release(serializer);
	release(v);
	release(selector);
	release(loaded);
	release(file);
	return 0;
}

int raven_entry(void)
{
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;
	if (!(documents = factory(L"Windows.Data.Xml.Dom.XmlDocument", &document_statics))) return 2;
	if (!(values = factory(L"Windows.Foundation.PropertyValue", &value_statics))) return 3;
	if (!(files = factory(L"Windows.Storage.StorageFile", &file_statics))) return 4;
	if (!(document = activate(L"Windows.Data.Xml.Dom.XmlDocument", &document_iid))) return 5;
	int failed = check_reading();
	if (!failed)
		failed = check_building();
	if (!failed)
		failed = check_loading();
	if (!failed)
		failed = check_files();
	release(root);
	release(document);
	return failed;
}
