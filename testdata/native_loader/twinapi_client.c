#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Imported as programs import them, from the Windows Runtime's API set. */
__declspec(dllimport) HRESULT WINAPI RoInitialize(int type);
__declspec(dllimport) HRESULT WINAPI RoGetActivationFactory(void *name, const GUID *iid, void **factory);
__declspec(dllimport) void WINAPI CoTaskMemFree(void *block);
__declspec(dllimport) const WCHAR *WINAPI WindowsGetStringRawBuffer(void *string, UINT32 *length);
__declspec(dllimport) HRESULT WINAPI WindowsDeleteString(void *string);
/* shlwapi's table-driven QueryInterface, which Office's own objects answer through. */
typedef struct {
	const GUID *iid;
	DWORD offset;
} interface_entry;
__declspec(dllimport) HRESULT WINAPI QISearch(void *object, const interface_entry *table, const GUID *iid, void **out);

/* The string header programs built with C++/WinRT write themselves for a reference: flags, length, two unused words, the characters. */
struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

/* An interface pointer seen as its table of methods; the first six are IInspectable's. */
typedef struct {
	void **table;
} object;

#define METHOD(self, index, type) ((type)(self)->table[index])

typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *query_method)(object *, const GUID *, void **);
typedef HRESULT (WINAPI *flag_method)(object *, BYTE *);
typedef HRESULT (WINAPI *number_method)(object *, INT32 *);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *string_method)(object *, void *, object **);
typedef HRESULT (WINAPI *handler_method)(object *, object *);
typedef HRESULT (WINAPI *event_method)(object *, object *, INT64 *);

static const GUID retail_info = {0x0712c6b8, 0x8b92, 0x4f2a, {0x84, 0x99, 0x03, 0x1f, 0x17, 0x98, 0xd6, 0xef}};
static const GUID shared_mode = {0x893df40e, 0xcad6, 0x4d50, {0x8c, 0x49, 0x6f, 0xcf, 0xc0, 0x3e, 0xdb, 0x29}};
static const GUID analytics_info = {0x1d5ee066, 0x188d, 0x5ba9, {0x43, 0x87, 0xac, 0xae, 0xb0, 0xe7, 0xe3, 0x05}};
static const GUID network_information = {0x5074f851, 0x950d, 0x4165, {0x9c, 0x15, 0x36, 0x56, 0x19, 0x48, 0x1e, 0xea}};
static const GUID connection_profile = {0x71ba143c, 0x598e, 0x49d0, {0x84, 0xeb, 0x8f, 0xeb, 0xae, 0xdc, 0xc1, 0x95}};
static const GUID profile_view = {0x6db1b492, 0x3852, 0x5df8, {0xa2, 0x9d, 0x69, 0x44, 0x00, 0x2f, 0x58, 0xd4}};
static const GUID profile_iterable = {0x34dabef9, 0x87d0, 0x5b1c, {0xa7, 0xac, 0x9d, 0x29, 0x0a, 0xde, 0xb0, 0xc8}};
static const GUID online_id = {0x85047792, 0xf634, 0x41e3, {0x96, 0xa4, 0x51, 0x64, 0xe9, 0x02, 0xc7, 0x40}};
static const GUID ticket_request_factory = {0xbebb0a08, 0x9e73, 0x4077, {0x96, 0x14, 0x08, 0x61, 0x4c, 0x0b, 0xc2, 0x45}};
static const GUID web_core = {0x6aca7c92, 0xa581, 0x4479, {0x9c, 0x10, 0x75, 0x2e, 0xff, 0x44, 0xfd, 0x34}};
static const GUID provider_operation = {0x88c66009, 0x12f7, 0x58e2, {0x8d, 0xbe, 0x6e, 0xfc, 0x62, 0x0c, 0x85, 0xba}};
static const GUID async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID core_application = {0x0aacf7a4, 0x5e1d, 0x49df, {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};
static const GUID core_application2 = {0x998681fb, 0x1ab6, 0x4b7f, {0xbe, 0x4a, 0x9a, 0x06, 0x45, 0x22, 0x4c, 0x04}};
static const GUID property_set = {0x8a43ed9f, 0xf4e6, 0x4421, {0xac, 0xf9, 0x1d, 0xab, 0x29, 0x86, 0x82, 0x0c}};
static const GUID property_map = {0x1b0d3570, 0x0877, 0x5ec2, {0x8a, 0x2c, 0x3b, 0x95, 0x39, 0x50, 0x6a, 0xca}};
static const GUID property_iterable = {0xfe2f3d47, 0x5d47, 0x5499, {0x83, 0x74, 0x43, 0x0c, 0x7c, 0xda, 0x02, 0x04}};

static object *statics(const WCHAR *name, const GUID *iid)
{
	struct reference text = {1, (UINT32)lstrlenW(name), {0, 0}, name};
	object *out = NULL;
	return SUCCEEDED(RoGetActivationFactory(&text, iid, (void **)&out)) ? out : NULL;
}

/* A completion handler that records the status it was called with. */
static INT32 completion_status = -1;
static void *completed_operation;

static HRESULT WINAPI handler_query(object *self, const GUID *iid, void **out) { *out = self; return S_OK; }
static ULONG WINAPI handler_count(object *self) { return 1; }

static HRESULT WINAPI handler_invoke(object *self, void *operation, INT32 status)
{
	completed_operation = operation;
	completion_status = status;
	return S_OK;
}

static void *handler_table[] = {(void *)handler_query, (void *)handler_count, (void *)handler_count, (void *)handler_invoke};
static object handler = {handler_table};

/* A map change handler that records the kind of the last change. */
static INT32 last_change = -1;

static HRESULT WINAPI change_invoke(object *self, object *sender, object *args)
{
	return METHOD(args, 6, number_method)(args, &last_change);
}

static void *change_table[] = {(void *)handler_query, (void *)handler_count, (void *)handler_count, (void *)change_invoke};
static object change_handler = {change_table};

typedef HRESULT (WINAPI *lookup_method)(object *, void *, object **);
typedef HRESULT (WINAPI *has_key_method)(object *, void *, BYTE *);
typedef HRESULT (WINAPI *insert_method)(object *, void *, object *, BYTE *);
typedef HRESULT (WINAPI *remove_method)(object *, void *);

/* CoreApplication.Properties: one property set for the process, which keeps values, tells handlers of changes and iterates its pairs. */
static int check_properties(object *core)
{
	object *observable = NULL, *map = NULL, *iterable = NULL, *same = NULL;
	if (METHOD(core, 11, object_method)(core, &observable) != S_OK || !observable) return 32;
	if (METHOD(observable, 0, query_method)(observable, &property_set, (void **)&same) != S_OK) return 33;
	if (METHOD(observable, 0, query_method)(observable, &property_map, (void **)&map) != S_OK) return 34;
	if (METHOD(observable, 0, query_method)(observable, &property_iterable, (void **)&iterable) != S_OK) return 35;
	INT64 token = 0;
	if (METHOD(observable, 6, event_method)(observable, &change_handler, &token) != S_OK || !token) return 36;
	struct reference key = {1, 5, {0, 0}, L"Raven"};
	object *value = NULL;
	if (METHOD(map, 6, lookup_method)(map, &key, &value) != E_BOUNDS || value) return 37;
	BYTE replaced = 1;
	if (METHOD(map, 10, insert_method)(map, &key, &handler, &replaced) != S_OK || replaced || last_change != 1) return 38;
	if (METHOD(map, 10, insert_method)(map, &key, &handler, &replaced) != S_OK || !replaced || last_change != 3) return 39;
	if (METHOD(map, 6, lookup_method)(map, &key, &value) != S_OK || value != &handler) return 40;
	UINT32 size = 0;
	BYTE found = 0;
	if (METHOD(map, 7, number_method)(map, (INT32 *)&size) != S_OK || size != 1) return 41;
	if (METHOD(map, 8, has_key_method)(map, &key, &found) != S_OK || !found) return 42;
	object *iterator = NULL, *pair = NULL;
	if (METHOD(iterable, 6, object_method)(iterable, &iterator) != S_OK || !iterator) return 43;
	if (METHOD(iterator, 6, object_method)(iterator, &pair) != S_OK || !pair) return 44;
	if (METHOD(pair, 7, object_method)(pair, &value) != S_OK || value != &handler) return 45;
	object *view = NULL;
	if (METHOD(map, 9, object_method)(map, &view) != S_OK || !view) return 46;
	if (METHOD(map, 11, remove_method)(map, &key) != S_OK || last_change != 2) return 47;
	if (METHOD(map, 11, remove_method)(map, &key) != E_BOUNDS) return 48;
	size = 0;
	if (METHOD(view, 7, number_method)(view, (INT32 *)&size) != S_OK || size != 1) return 49;
	if (METHOD(observable, 7, HRESULT (WINAPI *)(object *, INT64))(observable, token) != S_OK) return 50;
	METHOD(view, 2, release_method)(view);
	METHOD(pair, 2, release_method)(pair);
	METHOD(iterator, 2, release_method)(iterator);
	return 0;
}

/* The system classes of the Windows Runtime as Office asks them while it starts: the device is no demonstration unit and no shared
 * device; the network is one wired profile whose connectivity matches the adapters; there is no web account provider, and the search
 * for one completes at once with nothing; a desktop process has no package identity but may take events and enable prelaunch, and keeps
 * properties for the whole process. */
/* An object with two interfaces, the second eight bytes in: QISearch finds each by its entry, IUnknown as the first, and nothing else. */
static const GUID first_face = {0x11111111, 0x2222, 0x3333, {0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55}};
static const GUID second_face = {0x66666666, 0x7777, 0x8888, {0x99, 0x99, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa}};
static const GUID unknown_face = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static int check_search(void)
{
	object faces[2] = {{handler_table}, {handler_table}};
	const interface_entry table[] = {{&first_face, 0}, {&second_face, sizeof(object)}, {NULL, 0}};
	void *out = (void *)1;
	if (QISearch(faces, table, &second_face, &out) != S_OK || out != &faces[1]) return 51;
	if (QISearch(faces, table, &unknown_face, &out) != S_OK || out != &faces[0]) return 52;
	if (QISearch(faces, table, &network_information, &out) != E_NOINTERFACE || out) return 53;
	return 0;
}

/* Whether a string a class answered holds the text; the string is freed. */
static BOOL holds(void *string, const WCHAR *text)
{
	UINT32 length = 0;
	const WCHAR *characters = WindowsGetStringRawBuffer(string, &length);
	const BOOL equal = CompareStringOrdinal(characters, (int)length, text, -1, FALSE) == CSTR_EQUAL;
	WindowsDeleteString(string);
	return equal;
}

/* AnalyticsInfo, which winget asks which Windows it runs on: the desktop family, at the version RtlGetVersion and the update build revision
 * give, packed in sixteen bits each and written in decimal, and the desktop form. */
static int check_analytics(void)
{
	object *analytics = statics(L"Windows.System.Profile.AnalyticsInfo", &analytics_info);
	if (!analytics) return 56;
	object *info = NULL;
	void *text = NULL;
	if (METHOD(analytics, 6, object_method)(analytics, &info) != S_OK || !info) return 57;
	if (METHOD(info, 6, object_method)(info, (object **)&text) != S_OK || !holds(text, L"Windows.Desktop")) return 58;
	LONG (WINAPI *get_version)(RTL_OSVERSIONINFOW *) =
		(LONG (WINAPI *)(RTL_OSVERSIONINFOW *))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	RTL_OSVERSIONINFOW version = {sizeof version};
	DWORD revision = 0, size = sizeof revision;
	if (!get_version || get_version(&version)) return 59;
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, NULL, &revision, &size))
		return 60;
	ULONGLONG packed = (ULONGLONG)version.dwMajorVersion << 48 | (ULONGLONG)version.dwMinorVersion << 32 |
		(ULONGLONG)version.dwBuildNumber << 16 | revision;
	WCHAR expected[21];
	int at = 20;
	expected[at] = 0;
	do {
		expected[--at] = (WCHAR)(L'0' + packed % 10);
		packed /= 10;
	} while (packed);
	if (METHOD(info, 7, object_method)(info, (object **)&text) != S_OK || !holds(text, expected + at)) return 61;
	METHOD(info, 2, release_method)(info);
	if (METHOD(analytics, 7, object_method)(analytics, (object **)&text) != S_OK || !holds(text, L"Desktop")) return 62;
	return 0;
}

int raven_entry(void)
{
	/* Programs that use the Windows Runtime use COM as well, as Office does. */
	CoTaskMemFree(NULL);
	if (FAILED(RoInitialize(1))) return 1;

	object *retail = statics(L"Windows.System.Profile.RetailInfo", &retail_info);
	if (!retail) return 2;
	BYTE flag = 1;
	if (METHOD(retail, 6, flag_method)(retail, &flag) != S_OK || flag) return 3;
	object *properties = NULL;
	if (METHOD(retail, 7, object_method)(retail, &properties) != S_OK || !properties) return 4;
	INT32 size = -1;
	if (METHOD(properties, 7, number_method)(properties, &size) != S_OK || size) return 5;
	METHOD(properties, 2, release_method)(properties);
	object *shared = statics(L"Windows.System.Profile.SharedModeSettings", &shared_mode);
	flag = 1;
	if (!shared || METHOD(shared, 6, flag_method)(shared, &flag) != S_OK || flag) return 6;

	object *network = statics(L"Windows.Networking.Connectivity.NetworkInformation", &network_information);
	if (!network) return 7;
	object *profiles = NULL;
	if (METHOD(network, 6, object_method)(network, &profiles) != S_OK || !profiles) return 8;
	object *view = NULL, *iterable = NULL;
	if (METHOD(profiles, 0, query_method)(profiles, &profile_view, (void **)&view) != S_OK) return 9;
	if (METHOD(profiles, 0, query_method)(profiles, &profile_iterable, (void **)&iterable) != S_OK) return 10;
	size = -1;
	if (METHOD(view, 7, number_method)(view, &size) != S_OK || size > 1) return 11;
	object *internet = NULL;
	if (METHOD(network, 7, object_method)(network, &internet) != S_OK || (size == 1) != (internet != NULL)) return 12;
	if (internet) {
		INT32 level = -1;
		if (METHOD(internet, 7, number_method)(internet, &level) != S_OK || level != 3) return 13;
		object *cost = NULL;
		if (METHOD(internet, 9, object_method)(internet, &cost) != S_OK || !cost) return 14;
		INT32 type = -1;
		if (METHOD(cost, 6, number_method)(cost, &type) != S_OK || type != 1) return 15;
		object *again = NULL;
		if (METHOD(internet, 0, query_method)(internet, &connection_profile, (void **)&again) != S_OK) return 16;
	}
	INT64 token = 0;
	if (METHOD(network, 12, event_method)(network, &handler, &token) != S_OK || !token) return 17;
	METHOD(iterable, 2, release_method)(iterable);
	METHOD(view, 2, release_method)(view);
	METHOD(profiles, 2, release_method)(profiles);

	object *web = statics(L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager", &web_core);
	if (!web) return 18;
	struct reference provider_id = {1, 27, {0, 0}, L"https://login.microsoft.com"};
	object *operation = NULL;
	if (METHOD(web, 11, string_method)(web, &provider_id, &operation) != S_OK || !operation) return 19;
	object *typed = NULL, *info = NULL;
	if (METHOD(operation, 0, query_method)(operation, &provider_operation, (void **)&typed) != S_OK) return 21;
	if (METHOD(operation, 0, query_method)(operation, &async_info, (void **)&info) != S_OK) return 22;
	INT32 status = -1;
	if (METHOD(info, 7, number_method)(info, &status) != S_OK || status != 1) return 23;
	if (METHOD(operation, 6, handler_method)(operation, &handler) != S_OK || completion_status != 1 || completed_operation != operation) return 24;
	if (METHOD(operation, 6, handler_method)(operation, &handler) == S_OK) return 25;
	object *provider = (object *)1;
	if (METHOD(operation, 8, object_method)(operation, &provider) != S_OK || provider) return 26;
	METHOD(info, 2, release_method)(info);
	METHOD(typed, 2, release_method)(typed);
	METHOD(operation, 2, release_method)(operation);
	if (!statics(L"Windows.Security.Authentication.OnlineId.OnlineIdSystemAuthenticator", &online_id)) return 27;
	object *requests = statics(L"Windows.Security.Authentication.OnlineId.OnlineIdServiceTicketRequest", &ticket_request_factory);
	struct reference service = {1, 12, {0, 0}, L"ssl.live.com"};
	object *request = NULL;
	void *named = NULL;
	if (!requests || METHOD(requests, 6, HRESULT (WINAPI *)(object *, void *, void *, object **))(requests, &service, NULL, &request) != S_OK)
		return 54;
	if (METHOD(request, 6, object_method)(request, (object **)&named) != S_OK || !named) return 55;
	METHOD(request, 2, release_method)(request);

	object *core = statics(L"Windows.ApplicationModel.Core.CoreApplication", &core_application);
	if (!core) return 28;
	void *id = (void *)1;
	if (METHOD(core, 6, object_method)(core, (object **)&id) != (HRESULT)0x80073d54 || id) return 29;
	token = 0;
	if (METHOD(core, 7, event_method)(core, &handler, &token) != S_OK || !token) return 30;
	object *core2 = statics(L"Windows.ApplicationModel.Core.CoreApplication", &core_application2);
	if (!core2 || METHOD(core2, 12, HRESULT (WINAPI *)(object *, BYTE))(core2, 1) != S_OK) return 31;
	int failure = check_properties(core);
	if (!failure)
		failure = check_search();
	return failure ? failure : check_analytics();
}
