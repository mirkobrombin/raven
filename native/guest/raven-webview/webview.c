/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* EmbeddedBrowserWebView.dll, Raven's WebView2 runtime: what a program's WebView2Loader.dll loads from the folder the EdgeUpdate client
 * state names, built on the public WebView2 headers and drawing its pages with raven-webengine.dll, the engine the WebBrowser control
 * shares in the process.
 *
 * The loader calls CreateWebViewEnvironmentWithOptionsInternal and every object follows from the environment it completes with. A
 * controller is a child window of the program's window, which the page engine opens its browser in; the engine's messages to that window
 * become the WebView2 events. Completions and events reach the program on the thread that created the environment, through its window
 * messages, as WebView2 delivers them. chrome.webview reaches pages as a script every document runs first, which talks to the window
 * through the engine's __ravenPost.
 *
 * Members the runtime does not implement answer E_NOTIMPL and leave their interface and slot in the runtime's trace. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include "WebView2.h"
#include "webengine.h"

#ifndef RAVEN_WEBVIEW_VERSION
#error RAVEN_WEBVIEW_VERSION must name the browser version of the runtime
#endif

__declspec(dllimport) void WINAPI RavenTrace(const char *text);

static HINSTANCE module;

static void trace(const char *what, const WCHAR *text)
{
	char line[1024];
	int length = wsprintfA(line, "webview: %s", what);
	if (text) {
		line[length++] = ' ';
		// A long text is cut, at most three bytes to a character, rather than lost.
		int count = lstrlenW(text);
		const int room = ((int)sizeof(line) - length - 1) / 3;
		if (count > room)
			count = room;
		length += WideCharToMultiByte(CP_UTF8, 0, text, count, line + length, (int)sizeof(line) - length - 1, NULL, NULL);
	}
	line[length] = 0;
	RavenTrace(line);
}

static void trace_iid(const char *what, REFIID iid)
{
	char line[160];
	wsprintfA(line, "webview: %s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", what, (unsigned long)iid->Data1, iid->Data2, iid->Data3,
		iid->Data4[0], iid->Data4[1], iid->Data4[2], iid->Data4[3], iid->Data4[4], iid->Data4[5], iid->Data4[6], iid->Data4[7]);
	RavenTrace(line);
}

/* Members without an implementation. Every interface takes its own run of stubs, so the trace can name the slot a program called. */
#define STUB(n) static HRESULT STDMETHODCALLTYPE stub_##n(void) { return unimplemented(#n); }
#define STUB_10(n) STUB(n##0) STUB(n##1) STUB(n##2) STUB(n##3) STUB(n##4) STUB(n##5) STUB(n##6) STUB(n##7) STUB(n##8) STUB(n##9)
#define STUB_100(n) STUB_10(n##0) STUB_10(n##1) STUB_10(n##2) STUB_10(n##3) STUB_10(n##4) STUB_10(n##5) STUB_10(n##6) STUB_10(n##7) \
	STUB_10(n##8) STUB_10(n##9)
#define STUB_REF(n) stub_##n,
#define STUB_REF_10(n) STUB_REF(n##0) STUB_REF(n##1) STUB_REF(n##2) STUB_REF(n##3) STUB_REF(n##4) STUB_REF(n##5) STUB_REF(n##6) \
	STUB_REF(n##7) STUB_REF(n##8) STUB_REF(n##9)
#define STUB_REF_100(n) STUB_REF_10(n##0) STUB_REF_10(n##1) STUB_REF_10(n##2) STUB_REF_10(n##3) STUB_REF_10(n##4) STUB_REF_10(n##5) \
	STUB_REF_10(n##6) STUB_REF_10(n##7) STUB_REF_10(n##8) STUB_REF_10(n##9)

static struct {
	const char *name;
	int first, count;
} runs[24];
static int run_count, next_stub;

static HRESULT unimplemented(const char *digits)
{
	int index = 0;
	while (*digits)
		index = index * 10 + (*digits++ - '0');
	index -= 100;
	for (int i = 0; i < run_count; i++)
		if (index >= runs[i].first && index < runs[i].first + runs[i].count) {
			char line[128];
			wsprintfA(line, "webview: unimplemented %s slot %d", runs[i].name, index - runs[i].first);
			RavenTrace(line);
			return E_NOTIMPL;
		}
	RavenTrace("webview: unimplemented member");
	return E_NOTIMPL;
}

STUB_100(1) STUB_100(2) STUB_100(3) STUB_100(4)

static void *const stubs[] = {STUB_REF_100(1) STUB_REF_100(2) STUB_REF_100(3) STUB_REF_100(4)};

/* Fills a table of an interface's members with stubs, past the three of IUnknown, which every object sets itself. */
static void stub_table(void *table, size_t size, const char *name)
{
	void **slots = table;
	const int count = (int)(size / sizeof(void *));
	if (run_count == ARRAYSIZE(runs) || next_stub + count > (int)ARRAYSIZE(stubs))
		return;
	runs[run_count].name = name;
	runs[run_count].first = next_stub;
	runs[run_count++].count = count;
	for (int i = 0; i < count; i++)
		slots[i] = stubs[next_stub + i];
	next_stub += count;
}

/* Strings. */
static WCHAR *heap_copy(const WCHAR *text)
{
	if (!text)
		return NULL;
	WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(text) + 1) * sizeof(WCHAR));
	if (copy)
		lstrcpyW(copy, text);
	return copy;
}

static HRESULT co_copy(const WCHAR *text, LPWSTR *out)
{
	if (!out)
		return E_POINTER;
	if (!text)
		text = L"";
	*out = CoTaskMemAlloc((lstrlenW(text) + 1) * sizeof(WCHAR));
	if (!*out)
		return E_OUTOFMEMORY;
	lstrcpyW(*out, text);
	return S_OK;
}

/* A growing text, for the scripts the runtime writes. */
typedef struct text {
	WCHAR *data;
	int length, size;
	BOOL failed;
} text;

static void append(text *out, const WCHAR *part, int length)
{
	if (length < 0)
		length = lstrlenW(part);
	if (out->failed)
		return;
	if (out->length + length + 1 > out->size) {
		int size = out->size ? out->size : 256;
		while (out->length + length + 1 > size)
			size *= 2;
		WCHAR *data = out->data ? HeapReAlloc(GetProcessHeap(), 0, out->data, size * sizeof(WCHAR)) : HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR));
		if (!data) {
			out->failed = TRUE;
			return;
		}
		out->data = data;
		out->size = size;
	}
	CopyMemory(out->data + out->length, part, length * sizeof(WCHAR));
	out->length += length;
	out->data[out->length] = 0;
}

static WCHAR *finish(text *out)
{
	if (out->failed || !out->data) {
		HeapFree(GetProcessHeap(), 0, out->data);
		return NULL;
	}
	return out->data;
}

/* A string as a JSON and script string literal. */
static void append_quoted(text *out, const WCHAR *value)
{
	static const WCHAR digits[] = L"0123456789abcdef";
	append(out, L"\"", 1);
	for (const WCHAR *at = value; *at; at++) {
		const WCHAR character = *at;
		if (character == L'"' || character == L'\\') {
			WCHAR pair[2] = {L'\\', character};
			append(out, pair, 2);
		} else if (character < 0x20 || character == 0x2028 || character == 0x2029) {
			WCHAR escape[6] = {L'\\', L'u', digits[character >> 12], digits[(character >> 8) & 15], digits[(character >> 4) & 15], digits[character & 15]};
			append(out, escape, 6);
		} else {
			append(out, at, 1);
		}
	}
	append(out, L"\"", 1);
}

static int hex_value(WCHAR character)
{
	if (character >= L'0' && character <= L'9')
		return character - L'0';
	if (character >= L'a' && character <= L'f')
		return character - L'a' + 10;
	if (character >= L'A' && character <= L'F')
		return character - L'A' + 10;
	return -1;
}

/* The string a JSON string literal holds, or NULL when the JSON is not one. */
static WCHAR *json_string(const WCHAR *json)
{
	const int length = lstrlenW(json);
	if (length < 2 || json[0] != L'"' || json[length - 1] != L'"')
		return NULL;
	WCHAR *value = HeapAlloc(GetProcessHeap(), 0, length * sizeof(WCHAR)), *out = value;
	if (!value)
		return NULL;
	for (int i = 1; i < length - 1; i++) {
		WCHAR character = json[i];
		if (character == L'\\' && i + 1 < length - 1) {
			character = json[++i];
			switch (character) {
			case L'n': character = L'\n'; break;
			case L'r': character = L'\r'; break;
			case L't': character = L'\t'; break;
			case L'b': character = L'\b'; break;
			case L'f': character = L'\f'; break;
			case L'u': {
				int code = 0;
				for (int j = 1; j <= 4; j++) {
					const int digit = i + j < length - 1 ? hex_value(json[i + j]) : -1;
					if (digit < 0) {
						HeapFree(GetProcessHeap(), 0, value);
						return NULL;
					}
					code = code * 16 + digit;
				}
				i += 4;
				character = (WCHAR)code;
				break;
			}
			default:
				break;
			}
		}
		*out++ = character;
	}
	*out = 0;
	return value;
}

/* A page given as a string, as a data address the engine loads. */
static WCHAR *data_address(const WCHAR *html)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const int bytes = WideCharToMultiByte(CP_UTF8, 0, html, -1, NULL, 0, NULL, NULL) - 1;
	unsigned char *utf8 = bytes >= 0 ? HeapAlloc(GetProcessHeap(), 0, bytes + 1) : NULL;
	static const WCHAR prefix[] = L"data:text/html;charset=utf-8;base64,";
	WCHAR *address = utf8 ? HeapAlloc(GetProcessHeap(), 0, (ARRAYSIZE(prefix) + (bytes + 2) / 3 * 4 + 1) * sizeof(WCHAR)) : NULL;
	if (!address) {
		HeapFree(GetProcessHeap(), 0, utf8);
		return NULL;
	}
	WideCharToMultiByte(CP_UTF8, 0, html, -1, (char *)utf8, bytes + 1, NULL, NULL);
	lstrcpyW(address, prefix);
	WCHAR *out = address + ARRAYSIZE(prefix) - 1;
	for (int i = 0; i < bytes; i += 3) {
		const unsigned value = utf8[i] << 16 | (i + 1 < bytes ? utf8[i + 1] << 8 : 0) | (i + 2 < bytes ? utf8[i + 2] : 0);
		*out++ = alphabet[value >> 18];
		*out++ = alphabet[(value >> 12) & 63];
		*out++ = i + 1 < bytes ? alphabet[(value >> 6) & 63] : L'=';
		*out++ = i + 2 < bytes ? alphabet[value & 63] : L'=';
	}
	*out = 0;
	HeapFree(GetProcessHeap(), 0, utf8);
	return address;
}

/* The page engine, from the system directory. */
static struct {
	engine_navigate_function navigate;
	engine_message_function message;
	engine_window_function resize, close;
	engine_execute_function execute;
	engine_map_folder_function map_folder;
} engine;

static BOOL load_engine(void)
{
	static LONG tried;
	if (InterlockedExchange(&tried, 1))
		return engine.navigate != NULL;
	WCHAR path[MAX_PATH];
	const UINT length = GetSystemDirectoryW(path, MAX_PATH - 24);
	if (!length || length >= MAX_PATH - 24)
		return FALSE;
	lstrcatW(path, L"\\raven-webengine.dll");
	HMODULE library = LoadLibraryW(path);
	if (!library) {
		trace("no page engine at", path);
		return FALSE;
	}
	engine.message = (engine_message_function)GetProcAddress(library, "engine_message");
	engine.resize = (engine_window_function)GetProcAddress(library, "engine_resize");
	engine.close = (engine_window_function)GetProcAddress(library, "engine_close");
	engine.execute = (engine_execute_function)GetProcAddress(library, "engine_execute");
	engine.map_folder = (engine_map_folder_function)GetProcAddress(library, "engine_map_folder");
	engine_navigate_function navigate = (engine_navigate_function)GetProcAddress(library, "engine_navigate");
	if (!engine.message || !engine.resize || !engine.close || !engine.execute || !engine.map_folder || !navigate)
		return FALSE;
	engine.navigate = navigate;
	return TRUE;
}

/* Calls on a handler's Invoke, the member after IUnknown's that every WebView2 handler has. */
typedef HRESULT(STDMETHODCALLTYPE *event_invoke)(IUnknown *handler, IUnknown *sender, IUnknown *arguments);
typedef HRESULT(STDMETHODCALLTYPE *completion_invoke)(IUnknown *handler, HRESULT status, const void *result);

static HRESULT call_event(IUnknown *handler, IUnknown *sender, IUnknown *arguments)
{
	return ((event_invoke)((void **)handler->lpVtbl)[3])(handler, sender, arguments);
}

static HRESULT call_completion(IUnknown *handler, HRESULT status, const void *result)
{
	return ((completion_invoke)((void **)handler->lpVtbl)[3])(handler, status, result);
}

static const IID unknown_iid = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static BOOL is_iid(REFIID iid, const IID *const *list, int count)
{
	if (IsEqualIID(iid, &unknown_iid))
		return TRUE;
	for (int i = 0; i < count; i++)
		if (IsEqualIID(iid, list[i]))
			return TRUE;
	return FALSE;
}

/* Completions run later, from the window messages of the thread that asked: the handler, its status and a result that is an object
 * (with a reference the job holds) or a string. */
#define VIEW_JOB (WM_APP + 0x60)
#define VIEW_BLANK (WM_APP + 0x61)

typedef struct job {
	IUnknown *handler, *object;
	HRESULT status;
	WCHAR *text;
} job;

static BOOL post_job(HWND window, IUnknown *handler, HRESULT status, IUnknown *object, const WCHAR *text)
{
	if (!handler)
		return TRUE;
	job *work = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*work));
	if (!work)
		return FALSE;
	work->handler = handler;
	work->object = object;
	work->status = status;
	work->text = heap_copy(text);
	IUnknown_AddRef(handler);
	if (object)
		IUnknown_AddRef(object);
	if (PostMessageW(window, VIEW_JOB, 0, (LPARAM)work))
		return TRUE;
	IUnknown_Release(handler);
	if (object)
		IUnknown_Release(object);
	HeapFree(GetProcessHeap(), 0, work->text);
	HeapFree(GetProcessHeap(), 0, work);
	return FALSE;
}

static void run_job(job *work)
{
	const HRESULT answer = call_completion(work->handler, work->status, work->object ? (const void *)work->object : (const void *)work->text);
	if (FAILED(answer)) {
		char line[64];
		wsprintfA(line, "webview: completion handler failed with %08lX", (unsigned long)answer);
		RavenTrace(line);
	}
	IUnknown_Release(work->handler);
	if (work->object)
		IUnknown_Release(work->object);
	HeapFree(GetProcessHeap(), 0, work->text);
	HeapFree(GetProcessHeap(), 0, work);
}

/* Event arguments: one kind of object for every event the runtime raises, its table chosen by the event. */
typedef struct arguments {
	const void *table;
	LONG refs;
	const IID *const *iids;
	int iid_count;
	WCHAR *uri, *json;
	UINT64 navigation;
	BOOL flag, cancel;
	int status;
} arguments;

static HRESULT STDMETHODCALLTYPE arguments_query(void *self, REFIID iid, void **out)
{
	arguments *object = self;
	if (!out)
		return E_POINTER;
	if (!is_iid(iid, object->iids, object->iid_count)) {
		trace_iid("event arguments lack", iid);
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&object->refs);
	*out = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE arguments_add_ref(void *self)
{
	return InterlockedIncrement(&((arguments *)self)->refs);
}

static ULONG STDMETHODCALLTYPE arguments_release(void *self)
{
	arguments *object = self;
	const LONG refs = InterlockedDecrement(&object->refs);
	if (!refs) {
		HeapFree(GetProcessHeap(), 0, object->uri);
		HeapFree(GetProcessHeap(), 0, object->json);
		HeapFree(GetProcessHeap(), 0, object);
	}
	return refs;
}

static HRESULT STDMETHODCALLTYPE arguments_uri(void *self, LPWSTR *uri)
{
	return co_copy(((arguments *)self)->uri, uri);
}

static HRESULT STDMETHODCALLTYPE arguments_flag(void *self, BOOL *value)
{
	if (!value)
		return E_POINTER;
	*value = ((arguments *)self)->flag;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE arguments_false(void *self, BOOL *value)
{
	if (!value)
		return E_POINTER;
	*value = FALSE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE arguments_get_cancel(void *self, BOOL *value)
{
	if (!value)
		return E_POINTER;
	*value = ((arguments *)self)->cancel;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE arguments_put_cancel(void *self, BOOL value)
{
	((arguments *)self)->cancel = value;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE arguments_navigation(void *self, UINT64 *value)
{
	if (!value)
		return E_POINTER;
	*value = ((arguments *)self)->navigation;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE arguments_status(void *self, int *value)
{
	if (!value)
		return E_POINTER;
	*value = ((arguments *)self)->status;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE arguments_http_status(void *self, int *value)
{
	if (!value)
		return E_POINTER;
	*value = ((arguments *)self)->flag ? 200 : 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE arguments_json(void *self, LPWSTR *value)
{
	return co_copy(((arguments *)self)->json, value);
}

static HRESULT STDMETHODCALLTYPE arguments_string(void *self, LPWSTR *value)
{
	if (!value)
		return E_POINTER;
	*value = NULL;
	WCHAR *string = json_string(((arguments *)self)->json);
	if (!string)
		return E_INVALIDARG;
	const HRESULT status = co_copy(string, value);
	HeapFree(GetProcessHeap(), 0, string);
	return status;
}

static HRESULT STDMETHODCALLTYPE arguments_navigation_kind(void *self, COREWEBVIEW2_NAVIGATION_KIND *value)
{
	if (!value)
		return E_POINTER;
	*value = COREWEBVIEW2_NAVIGATION_KIND_NEW_DOCUMENT;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE arguments_empty(void *self, LPWSTR *value)
{
	return co_copy(L"", value);
}

static HRESULT STDMETHODCALLTYPE arguments_accept(void *self, LPCWSTR value)
{
	return S_OK;
}

static ICoreWebView2NavigationStartingEventArgs3Vtbl starting_table;
static ICoreWebView2NavigationCompletedEventArgs2Vtbl completed_table;
static ICoreWebView2ContentLoadingEventArgsVtbl loading_table;
static ICoreWebView2SourceChangedEventArgsVtbl source_table;
static ICoreWebView2DOMContentLoadedEventArgsVtbl loaded_table;
static ICoreWebView2WebMessageReceivedEventArgs2Vtbl message_table;

static const IID *const starting_iids[] = {&IID_ICoreWebView2NavigationStartingEventArgs, &IID_ICoreWebView2NavigationStartingEventArgs2,
	&IID_ICoreWebView2NavigationStartingEventArgs3};
static const IID *const completed_iids[] = {&IID_ICoreWebView2NavigationCompletedEventArgs, &IID_ICoreWebView2NavigationCompletedEventArgs2};
static const IID *const loading_iids[] = {&IID_ICoreWebView2ContentLoadingEventArgs};
static const IID *const source_iids[] = {&IID_ICoreWebView2SourceChangedEventArgs};
static const IID *const loaded_iids[] = {&IID_ICoreWebView2DOMContentLoadedEventArgs};
static const IID *const message_iids[] = {&IID_ICoreWebView2WebMessageReceivedEventArgs, &IID_ICoreWebView2WebMessageReceivedEventArgs2};

#define ARGUMENTS_BASE(table) \
	table.QueryInterface = (void *)arguments_query; \
	table.AddRef = (void *)arguments_add_ref; \
	table.Release = (void *)arguments_release

static void arguments_tables(void)
{
	stub_table(&starting_table, sizeof(starting_table), "NavigationStartingEventArgs");
	ARGUMENTS_BASE(starting_table);
	starting_table.get_Uri = (void *)arguments_uri;
	starting_table.get_IsUserInitiated = (void *)arguments_false;
	starting_table.get_IsRedirected = (void *)arguments_flag;
	starting_table.get_Cancel = (void *)arguments_get_cancel;
	starting_table.put_Cancel = (void *)arguments_put_cancel;
	starting_table.get_NavigationId = (void *)arguments_navigation;
	starting_table.get_AdditionalAllowedFrameAncestors = (void *)arguments_empty;
	starting_table.put_AdditionalAllowedFrameAncestors = (void *)arguments_accept;
	starting_table.get_NavigationKind = (void *)arguments_navigation_kind;

	stub_table(&completed_table, sizeof(completed_table), "NavigationCompletedEventArgs");
	ARGUMENTS_BASE(completed_table);
	completed_table.get_IsSuccess = (void *)arguments_flag;
	completed_table.get_WebErrorStatus = (void *)arguments_status;
	completed_table.get_NavigationId = (void *)arguments_navigation;
	completed_table.get_HttpStatusCode = (void *)arguments_http_status;

	stub_table(&loading_table, sizeof(loading_table), "ContentLoadingEventArgs");
	ARGUMENTS_BASE(loading_table);
	loading_table.get_IsErrorPage = (void *)arguments_flag;
	loading_table.get_NavigationId = (void *)arguments_navigation;

	stub_table(&source_table, sizeof(source_table), "SourceChangedEventArgs");
	ARGUMENTS_BASE(source_table);
	source_table.get_IsNewDocument = (void *)arguments_flag;

	stub_table(&loaded_table, sizeof(loaded_table), "DOMContentLoadedEventArgs");
	ARGUMENTS_BASE(loaded_table);
	loaded_table.get_NavigationId = (void *)arguments_navigation;

	stub_table(&message_table, sizeof(message_table), "WebMessageReceivedEventArgs");
	ARGUMENTS_BASE(message_table);
	message_table.get_Source = (void *)arguments_uri;
	message_table.get_WebMessageAsJson = (void *)arguments_json;
	message_table.TryGetWebMessageAsString = (void *)arguments_string;
}

static arguments *new_arguments(const void *table, const IID *const *iids, int iid_count, const WCHAR *uri, UINT64 navigation)
{
	arguments *object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
	if (!object)
		return NULL;
	object->table = table;
	object->refs = 1;
	object->iids = iids;
	object->iid_count = iid_count;
	object->uri = heap_copy(uri);
	object->navigation = navigation;
	return object;
}

/* The events of a view, by the names WebView2 gives them. */
#define VIEW_EVENTS(X) X(NavigationStarting) X(ContentLoading) X(SourceChanged) X(HistoryChanged) X(NavigationCompleted) \
	X(FrameNavigationStarting) X(FrameNavigationCompleted) X(ScriptDialogOpening) X(PermissionRequested) X(ProcessFailed) X(WebMessageReceived) \
	X(NewWindowRequested) X(DocumentTitleChanged) X(ContainsFullScreenElementChanged) X(WebResourceRequested) X(WindowCloseRequested) \
	X(WebResourceResponseReceived) X(DOMContentLoaded) X(FrameCreated) X(DownloadStarting) X(ClientCertificateRequested) X(IsMutedChanged) \
	X(IsDocumentPlayingAudioChanged) X(IsDefaultDownloadDialogOpenChanged) X(BasicAuthenticationRequested) X(ContextMenuRequested) \
	X(StatusBarTextChanged) X(ServerCertificateErrorDetected) X(FaviconChanged) X(LaunchingExternalUriScheme)
#define CONTROLLER_EVENTS(X) X(ZoomFactorChanged) X(MoveFocusRequested) X(GotFocus) X(LostFocus) X(AcceleratorKeyPressed) \
	X(RasterizationScaleChanged)
#define EVENT_NAME(name) event_##name,
enum { VIEW_EVENTS(EVENT_NAME) CONTROLLER_EVENTS(EVENT_NAME) EVENTS };

/* The view's settings, with WebView2's defaults. */
#define VIEW_SETTINGS(X) X(IsScriptEnabled, TRUE) X(IsWebMessageEnabled, TRUE) X(AreDefaultScriptDialogsEnabled, TRUE) \
	X(IsStatusBarEnabled, TRUE) X(AreDevToolsEnabled, TRUE) X(AreDefaultContextMenusEnabled, TRUE) X(AreHostObjectsAllowed, TRUE) \
	X(IsZoomControlEnabled, TRUE) X(IsBuiltInErrorPageEnabled, TRUE) X(AreBrowserAcceleratorKeysEnabled, TRUE) \
	X(IsPasswordAutosaveEnabled, FALSE) X(IsGeneralAutofillEnabled, TRUE) X(IsPinchZoomEnabled, TRUE) X(IsSwipeNavigationEnabled, TRUE) \
	X(IsReputationCheckingRequired, TRUE)
#define SETTING_NAME(name, initial) setting_##name,
enum { VIEW_SETTINGS(SETTING_NAME) SETTINGS };

#define HANDLERS 8
#define DOCUMENT_SCRIPTS 32
#define SCRIPT_RUNS 64

typedef struct environment environment;

/* A controller, its CoreWebView2 and the settings of both: one object with a reference count they share. */
typedef struct view {
	ICoreWebView2Controller4 controller;
	ICoreWebView2_22 webview;
	ICoreWebView2Settings8 settings;
	ICoreWebView2Profile8 profile;
	LONG refs;
	environment *owner;
	HWND window;
	RECT bounds;
	BOOL visible, closed;
	double zoom, scale;
	COREWEBVIEW2_COLOR background;
	BOOL flags[SETTINGS];
	WCHAR *user_agent, *source, *downloads;
	COREWEBVIEW2_PREFERRED_COLOR_SCHEME color_scheme;
	UINT64 navigation;
	struct {
		INT64 token;
		IUnknown *handler;
	} events[EVENTS][HANDLERS];
	struct {
		UINT id;
		WCHAR *code;
	} scripts[DOCUMENT_SCRIPTS];
	struct {
		UINT id;
		IUnknown *handler;
	} pending[SCRIPT_RUNS];
} view;

struct environment {
	ICoreWebView2Environment13 face;
	LONG refs;
	HWND messages;
	WCHAR *data_folder;
};

static LONG next_token, next_script;

#define VIEW_OF(self, member) ((view *)((char *)(self) - offsetof(view, member)))

static ULONG view_add_ref(view *object)
{
	return InterlockedIncrement(&object->refs);
}

static void view_close(view *object);

static ULONG view_release(view *object)
{
	const LONG refs = InterlockedDecrement(&object->refs);
	if (refs)
		return refs;
	view_close(object);
	for (int i = 0; i < EVENTS; i++)
		for (int j = 0; j < HANDLERS; j++)
			if (object->events[i][j].handler)
				IUnknown_Release(object->events[i][j].handler);
	for (int i = 0; i < DOCUMENT_SCRIPTS; i++)
		HeapFree(GetProcessHeap(), 0, object->scripts[i].code);
	for (int i = 0; i < SCRIPT_RUNS; i++)
		if (object->pending[i].handler)
			IUnknown_Release(object->pending[i].handler);
	HeapFree(GetProcessHeap(), 0, object->user_agent);
	HeapFree(GetProcessHeap(), 0, object->downloads);
	HeapFree(GetProcessHeap(), 0, object->source);
	ICoreWebView2Environment13_Release(&object->owner->face);
	HeapFree(GetProcessHeap(), 0, object);
	return 0;
}

static HRESULT subscribe(view *object, int event, IUnknown *handler, EventRegistrationToken *token)
{
	if (!handler || !token)
		return E_POINTER;
	for (int i = 0; i < HANDLERS; i++)
		if (!object->events[event][i].handler) {
			IUnknown_AddRef(handler);
			object->events[event][i].handler = handler;
			object->events[event][i].token = token->value = InterlockedIncrement(&next_token);
			return S_OK;
		}
	return E_OUTOFMEMORY;
}

static HRESULT unsubscribe(view *object, int event, EventRegistrationToken token)
{
	for (int i = 0; i < HANDLERS; i++)
		if (object->events[event][i].handler && object->events[event][i].token == token.value) {
			IUnknown *handler = object->events[event][i].handler;
			object->events[event][i].handler = NULL;
			IUnknown_Release(handler);
		}
	return S_OK;
}

/* Raises an event; a handler may subscribe or unsubscribe others while it runs. */
static void raise_event(view *object, int event, IUnknown *sender, IUnknown *details)
{
	IUnknown *handlers[HANDLERS];
	int count = 0;
	for (int i = 0; i < HANDLERS; i++)
		if (object->events[event][i].handler) {
			handlers[count] = object->events[event][i].handler;
			IUnknown_AddRef(handlers[count]);
			count++;
		}
	for (int i = 0; i < count; i++) {
		call_event(handlers[i], sender, details);
		IUnknown_Release(handlers[i]);
	}
}

static void raise_with(view *object, int event, IUnknown *sender, arguments *details)
{
	if (!details)
		return;
	raise_event(object, event, sender, (IUnknown *)details);
	arguments_release(details);
}

#define VIEW_SENDER(object) ((IUnknown *)&(object)->webview)
#define CONTROLLER_SENDER(object) ((IUnknown *)&(object)->controller)

/* The script every document of a view runs first: chrome.webview, then the program's own scripts, each on its own. */
static const WCHAR bootstrap[] =
	L"(function(){var post=__ravenPost;var target=new EventTarget();var view={"
	L"postMessage:function(message){post('m'+JSON.stringify(message));},"
	L"addEventListener:function(type,listener,options){target.addEventListener(type,listener,options);},"
	L"removeEventListener:function(type,listener,options){target.removeEventListener(type,listener,options);},"
	L"hostObjects:{},"
	L"__deliver:function(data){target.dispatchEvent(new MessageEvent('message',{data:data}));},"
	L"__result:function(id,run){var value;try{value=JSON.stringify(run());}catch(error){value=undefined;}"
	L"post('r'+id+':'+(value===undefined?'null':value));}};"
	L"Object.defineProperty(view,'__deliver',{enumerable:false});Object.defineProperty(view,'__result',{enumerable:false});"
	L"if(!window.chrome)window.chrome={};window.chrome.webview=view;})();\n";

static WCHAR *document_script(view *object)
{
	text script = {0};
	append(&script, bootstrap, -1);
	for (int i = 0; i < DOCUMENT_SCRIPTS; i++)
		if (object->scripts[i].code) {
			append(&script, L"try{(0,eval)(", -1);
			append_quoted(&script, object->scripts[i].code);
			append(&script, L");}catch(error){console.error(error);}\n", -1);
		}
	return finish(&script);
}

static void complete_run(view *object, UINT id, const WCHAR *json)
{
	for (int i = 0; i < SCRIPT_RUNS; i++)
		if (object->pending[i].handler && object->pending[i].id == id) {
			IUnknown *handler = object->pending[i].handler;
			object->pending[i].handler = NULL;
			call_completion(handler, S_OK, json);
			IUnknown_Release(handler);
			return;
		}
}

static void posted(view *object, const WCHAR *message)
{
	if (message[0] == L'm' && object->flags[setting_IsWebMessageEnabled]) {
		arguments *details = new_arguments(&message_table, message_iids, ARRAYSIZE(message_iids), object->source, 0);
		if (details)
			details->json = heap_copy(message + 1);
		raise_with(object, event_WebMessageReceived, VIEW_SENDER(object), details);
	} else if (message[0] == L'r') {
		UINT id = 0;
		const WCHAR *at = message + 1;
		while (*at >= L'0' && *at <= L'9')
			id = id * 10 + (*at++ - L'0');
		if (*at == L':')
			complete_run(object, id, at + 1);
	}
}

static void reached(view *object, WCHAR *address, BOOL failed)
{
	HeapFree(GetProcessHeap(), 0, object->source);
	object->source = address;
	trace(failed ? "navigation failed at" : "navigation completed at", address);
	view_add_ref(object);
	arguments *details = new_arguments(&loading_table, loading_iids, ARRAYSIZE(loading_iids), NULL, object->navigation);
	if (details)
		details->flag = failed;
	raise_with(object, event_ContentLoading, VIEW_SENDER(object), details);
	details = new_arguments(&source_table, source_iids, ARRAYSIZE(source_iids), NULL, object->navigation);
	if (details)
		details->flag = TRUE;
	raise_with(object, event_SourceChanged, VIEW_SENDER(object), details);
	raise_event(object, event_HistoryChanged, VIEW_SENDER(object), NULL);
	if (!failed)
		raise_with(object, event_DOMContentLoaded, VIEW_SENDER(object),
			new_arguments(&loaded_table, loaded_iids, ARRAYSIZE(loaded_iids), NULL, object->navigation));
	details = new_arguments(&completed_table, completed_iids, ARRAYSIZE(completed_iids), NULL, object->navigation);
	if (details) {
		details->flag = !failed;
		details->status = failed ? COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED : COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
	}
	raise_with(object, event_NavigationCompleted, VIEW_SENDER(object), details);
	view_release(object);
}

static BOOL starting(view *object, const WCHAR *address, BOOL redirect)
{
	if (!redirect)
		object->navigation++;
	trace("navigation starting to", address);
	arguments *details = new_arguments(&starting_table, starting_iids, ARRAYSIZE(starting_iids), address, object->navigation);
	if (!details)
		return FALSE;
	details->flag = redirect;
	view_add_ref(object);
	raise_event(object, event_NavigationStarting, VIEW_SENDER(object), (IUnknown *)details);
	const BOOL cancel = details->cancel;
	arguments_release(details);
	view_release(object);
	return cancel;
}

static LRESULT CALLBACK view_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	view *object = (view *)GetWindowLongPtrW(window, GWLP_USERDATA);
	LRESULT result = 0;
	switch (message) {
	case VIEW_JOB:
		run_job((job *)lparam);
		return 0;
	case VIEW_BLANK:
		if (object && !starting(object, L"about:blank", FALSE))
			reached(object, heap_copy(L"about:blank"), FALSE);
		return 0;
	case ENGINE_BEFORE: {
		const BOOL cancel = object ? starting(object, (const WCHAR *)lparam, (BOOL)wparam) : FALSE;
		// The engine counts the navigations it was asked for.
		engine.message(window, message, wparam, lparam, &result);
		return cancel;
	}
	case ENGINE_REACHED:
		if (object)
			reached(object, (WCHAR *)lparam, (BOOL)wparam);
		else
			HeapFree(GetProcessHeap(), 0, (WCHAR *)lparam);
		return 0;
	case ENGINE_EXTERNAL:
		return 0;
	case ENGINE_SCRIPTS:
		return object && object->flags[setting_IsScriptEnabled] ? (LRESULT)document_script(object) : 0;
	case ENGINE_POSTED:
		if (object)
			posted(object, (const WCHAR *)lparam);
		HeapFree(GetProcessHeap(), 0, (WCHAR *)lparam);
		return 0;
	case WM_SETFOCUS:
		if (object)
			raise_event(object, event_GotFocus, CONTROLLER_SENDER(object), NULL);
		break;
	case WM_KILLFOCUS:
		if (object)
			raise_event(object, event_LostFocus, CONTROLLER_SENDER(object), NULL);
		break;
	default:
		break;
	}
	if (engine.navigate && engine.message(window, message, wparam, lparam, &result))
		return result;
	return DefWindowProcW(window, message, wparam, lparam);
}

static void view_close(view *object)
{
	if (object->closed)
		return;
	object->closed = TRUE;
	if (object->window) {
		if (engine.navigate)
			engine.close(object->window);
		SetWindowLongPtrW(object->window, GWLP_USERDATA, 0);
		DestroyWindow(object->window);
		object->window = NULL;
	}
}

/* ICoreWebView2Settings. */
static const IID *const settings_iids[] = {&IID_ICoreWebView2Settings, &IID_ICoreWebView2Settings2, &IID_ICoreWebView2Settings3,
	&IID_ICoreWebView2Settings4, &IID_ICoreWebView2Settings5, &IID_ICoreWebView2Settings6, &IID_ICoreWebView2Settings7,
	&IID_ICoreWebView2Settings8};

static HRESULT STDMETHODCALLTYPE settings_query(ICoreWebView2Settings8 *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!is_iid(iid, settings_iids, ARRAYSIZE(settings_iids))) {
		trace_iid("settings lack", iid);
		*out = NULL;
		return E_NOINTERFACE;
	}
	view_add_ref(VIEW_OF(self, settings));
	*out = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE settings_add_ref(ICoreWebView2Settings8 *self)
{
	return view_add_ref(VIEW_OF(self, settings));
}

static ULONG STDMETHODCALLTYPE settings_release(ICoreWebView2Settings8 *self)
{
	return view_release(VIEW_OF(self, settings));
}

#define SETTING_MEMBERS(name, initial) \
	static HRESULT STDMETHODCALLTYPE settings_get_##name(ICoreWebView2Settings8 *self, BOOL *value) \
	{ \
		if (!value) \
			return E_POINTER; \
		*value = VIEW_OF(self, settings)->flags[setting_##name]; \
		return S_OK; \
	} \
	static HRESULT STDMETHODCALLTYPE settings_put_##name(ICoreWebView2Settings8 *self, BOOL value) \
	{ \
		VIEW_OF(self, settings)->flags[setting_##name] = value != FALSE; \
		return S_OK; \
	}
VIEW_SETTINGS(SETTING_MEMBERS)

static HRESULT STDMETHODCALLTYPE settings_get_user_agent(ICoreWebView2Settings8 *self, LPWSTR *value)
{
	const WCHAR *agent = VIEW_OF(self, settings)->user_agent;
	return co_copy(agent ? agent : L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" RAVEN_WEBVIEW_VERSION
		L" Safari/537.36", value);
}

static HRESULT STDMETHODCALLTYPE settings_put_user_agent(ICoreWebView2Settings8 *self, LPCWSTR value)
{
	view *object = VIEW_OF(self, settings);
	HeapFree(GetProcessHeap(), 0, object->user_agent);
	object->user_agent = heap_copy(value);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE settings_get_pdf_items(ICoreWebView2Settings8 *self, COREWEBVIEW2_PDF_TOOLBAR_ITEMS *value)
{
	if (!value)
		return E_POINTER;
	*value = COREWEBVIEW2_PDF_TOOLBAR_ITEMS_NONE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE settings_put_pdf_items(ICoreWebView2Settings8 *self, COREWEBVIEW2_PDF_TOOLBAR_ITEMS value)
{
	return S_OK;
}

static ICoreWebView2Settings8Vtbl settings_table;

static void settings_members(void)
{
	stub_table(&settings_table, sizeof(settings_table), "Settings");
	settings_table.QueryInterface = settings_query;
	settings_table.AddRef = settings_add_ref;
	settings_table.Release = settings_release;
#define SETTING_TABLE(name, initial) \
	settings_table.get_##name = settings_get_##name; \
	settings_table.put_##name = settings_put_##name;
	VIEW_SETTINGS(SETTING_TABLE)
	settings_table.get_UserAgent = settings_get_user_agent;
	settings_table.put_UserAgent = settings_put_user_agent;
	settings_table.get_HiddenPdfToolbarItems = settings_get_pdf_items;
	settings_table.put_HiddenPdfToolbarItems = settings_put_pdf_items;
}

/* ICoreWebView2. */
static const IID *const webview_iids[] = {&IID_ICoreWebView2, &IID_ICoreWebView2_2, &IID_ICoreWebView2_3, &IID_ICoreWebView2_4,
	&IID_ICoreWebView2_5, &IID_ICoreWebView2_6, &IID_ICoreWebView2_7, &IID_ICoreWebView2_8, &IID_ICoreWebView2_9, &IID_ICoreWebView2_10,
	&IID_ICoreWebView2_11, &IID_ICoreWebView2_12, &IID_ICoreWebView2_13, &IID_ICoreWebView2_14, &IID_ICoreWebView2_15, &IID_ICoreWebView2_16,
	&IID_ICoreWebView2_17, &IID_ICoreWebView2_18, &IID_ICoreWebView2_19, &IID_ICoreWebView2_20, &IID_ICoreWebView2_21, &IID_ICoreWebView2_22};

#define WEBVIEW_OF(self) VIEW_OF(self, webview)

static HRESULT STDMETHODCALLTYPE webview_query(ICoreWebView2_22 *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!is_iid(iid, webview_iids, ARRAYSIZE(webview_iids))) {
		trace_iid("CoreWebView2 lacks", iid);
		*out = NULL;
		return E_NOINTERFACE;
	}
	view_add_ref(WEBVIEW_OF(self));
	*out = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE webview_add_ref(ICoreWebView2_22 *self)
{
	return view_add_ref(WEBVIEW_OF(self));
}

static ULONG STDMETHODCALLTYPE webview_release(ICoreWebView2_22 *self)
{
	return view_release(WEBVIEW_OF(self));
}

static HRESULT STDMETHODCALLTYPE webview_get_settings(ICoreWebView2_22 *self, ICoreWebView2Settings **settings)
{
	if (!settings)
		return E_POINTER;
	view *object = WEBVIEW_OF(self);
	view_add_ref(object);
	*settings = (ICoreWebView2Settings *)&object->settings;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_get_source(ICoreWebView2_22 *self, LPWSTR *uri)
{
	const WCHAR *source = WEBVIEW_OF(self)->source;
	return co_copy(source ? source : L"about:blank", uri);
}

static HRESULT go_to(view *object, const WCHAR *address)
{
	if (object->closed)
		return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
	if (!lstrcmpiW(address, L"about:blank") || !load_engine() || !engine.navigate(object->window, address)) {
		if (lstrcmpiW(address, L"about:blank"))
			trace("no page engine for", address);
		PostMessageW(object->window, VIEW_BLANK, 0, 0);
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_navigate(ICoreWebView2_22 *self, LPCWSTR uri)
{
	if (!uri)
		return E_INVALIDARG;
	trace("navigate", uri);
	return go_to(WEBVIEW_OF(self), uri);
}

static HRESULT STDMETHODCALLTYPE webview_navigate_to_string(ICoreWebView2_22 *self, LPCWSTR html)
{
	if (!html)
		return E_INVALIDARG;
	WCHAR *address = data_address(html);
	if (!address)
		return E_OUTOFMEMORY;
	trace("navigate to a string", NULL);
	const HRESULT status = go_to(WEBVIEW_OF(self), address);
	HeapFree(GetProcessHeap(), 0, address);
	return status;
}

#define VIEW_EVENT_MEMBERS(name) \
	static HRESULT STDMETHODCALLTYPE webview_add_##name(ICoreWebView2_22 *self, IUnknown *handler, EventRegistrationToken *token) \
	{ \
		return subscribe(WEBVIEW_OF(self), event_##name, handler, token); \
	} \
	static HRESULT STDMETHODCALLTYPE webview_remove_##name(ICoreWebView2_22 *self, EventRegistrationToken token) \
	{ \
		return unsubscribe(WEBVIEW_OF(self), event_##name, token); \
	}
VIEW_EVENTS(VIEW_EVENT_MEMBERS)

static HRESULT STDMETHODCALLTYPE webview_add_script(ICoreWebView2_22 *self, LPCWSTR script, IUnknown *handler)
{
	view *object = WEBVIEW_OF(self);
	if (!script)
		return E_INVALIDARG;
	for (int i = 0; i < DOCUMENT_SCRIPTS; i++)
		if (!object->scripts[i].code) {
			if (!(object->scripts[i].code = heap_copy(script)))
				return E_OUTOFMEMORY;
			object->scripts[i].id = InterlockedIncrement(&next_script);
			WCHAR id[16];
			wsprintfW(id, L"%u", object->scripts[i].id);
			post_job(object->owner->messages, handler, S_OK, NULL, id);
			return S_OK;
		}
	return E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE webview_remove_script(ICoreWebView2_22 *self, LPCWSTR id)
{
	view *object = WEBVIEW_OF(self);
	UINT number = 0;
	for (const WCHAR *at = id; at && *at >= L'0' && *at <= L'9'; at++)
		number = number * 10 + (*at - L'0');
	for (int i = 0; i < DOCUMENT_SCRIPTS; i++)
		if (object->scripts[i].code && object->scripts[i].id == number) {
			HeapFree(GetProcessHeap(), 0, object->scripts[i].code);
			object->scripts[i].code = NULL;
		}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_execute(ICoreWebView2_22 *self, LPCWSTR script, IUnknown *handler)
{
	view *object = WEBVIEW_OF(self);
	if (!script)
		return E_INVALIDARG;
	static LONG next_run;
	const UINT id = (UINT)InterlockedIncrement(&next_run);
	text code = {0};
	WCHAR head[96];
	wsprintfW(head, L"window.chrome&&window.chrome.webview&&window.chrome.webview.__result(%u,function(){return(0,eval)(", id);
	append(&code, head, -1);
	append_quoted(&code, script);
	append(&code, L");});", -1);
	WCHAR *run = finish(&code);
	if (!run)
		return E_OUTOFMEMORY;
	int slot = -1;
	for (int i = 0; handler && i < SCRIPT_RUNS && slot < 0; i++)
		if (!object->pending[i].handler)
			slot = i;
	const BOOL started = (!handler || slot >= 0) && engine.navigate && !object->closed && engine.execute(object->window, run);
	HeapFree(GetProcessHeap(), 0, run);
	if (!started)
		return post_job(object->owner->messages, handler, S_OK, NULL, L"null") ? S_OK : E_OUTOFMEMORY;
	if (handler) {
		IUnknown_AddRef(handler);
		object->pending[slot].handler = handler;
		object->pending[slot].id = id;
	}
	return S_OK;
}

static HRESULT post_to_page(view *object, const WCHAR *json)
{
	if (!object->flags[setting_IsWebMessageEnabled])
		return S_OK;
	text code = {0};
	append(&code, L"window.chrome&&window.chrome.webview&&window.chrome.webview.__deliver(", -1);
	append(&code, json, -1);
	append(&code, L");", -1);
	WCHAR *run = finish(&code);
	if (!run)
		return E_OUTOFMEMORY;
	if (engine.navigate && !object->closed)
		engine.execute(object->window, run);
	HeapFree(GetProcessHeap(), 0, run);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_post_json(ICoreWebView2_22 *self, LPCWSTR json)
{
	return json ? post_to_page(WEBVIEW_OF(self), json) : E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE webview_post_string(ICoreWebView2_22 *self, LPCWSTR message)
{
	if (!message)
		return E_INVALIDARG;
	text json = {0};
	append_quoted(&json, message);
	WCHAR *quoted = finish(&json);
	if (!quoted)
		return E_OUTOFMEMORY;
	const HRESULT status = post_to_page(WEBVIEW_OF(self), quoted);
	HeapFree(GetProcessHeap(), 0, quoted);
	return status;
}

static HRESULT STDMETHODCALLTYPE webview_reload(ICoreWebView2_22 *self)
{
	view *object = WEBVIEW_OF(self);
	return object->source ? go_to(object, object->source) : S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_process_id(ICoreWebView2_22 *self, UINT32 *value)
{
	if (!value)
		return E_POINTER;
	*value = GetCurrentProcessId();
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_false(ICoreWebView2_22 *self, BOOL *value)
{
	if (!value)
		return E_POINTER;
	*value = FALSE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_nothing(ICoreWebView2_22 *self)
{
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_empty(ICoreWebView2_22 *self, LPWSTR *value)
{
	return co_copy(L"", value);
}

static HRESULT STDMETHODCALLTYPE webview_accept_bool(ICoreWebView2_22 *self, BOOL value)
{
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_resource_filter(ICoreWebView2_22 *self, LPCWSTR uri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT context)
{
	trace("resource filter not applied:", uri);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_resource_filter_kinds(ICoreWebView2_22 *self, LPCWSTR uri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT context,
	COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS kinds)
{
	return webview_resource_filter(self, uri, context);
}

static HRESULT STDMETHODCALLTYPE webview_get_environment(ICoreWebView2_22 *self, ICoreWebView2Environment **value)
{
	if (!value)
		return E_POINTER;
	environment *owner = WEBVIEW_OF(self)->owner;
	ICoreWebView2Environment13_AddRef(&owner->face);
	*value = (ICoreWebView2Environment *)&owner->face;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_folder_mapping(ICoreWebView2_22 *self, LPCWSTR host, LPCWSTR folder, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND kind)
{
	if (!host || !folder)
		return E_INVALIDARG;
	// The page engine serves the folder's files to every page of the process; how far other origins may reach them is its own rule.
	if (!load_engine() || !engine.map_folder(host, folder)) {
		trace("folder mapping not applied for host", host);
		return S_OK;
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_clear_mapping(ICoreWebView2_22 *self, LPCWSTR host)
{
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_add_host_object(ICoreWebView2_22 *self, LPCWSTR name, VARIANT *object)
{
	trace("host object not given to pages:", name);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE webview_remove_host_object(ICoreWebView2_22 *self, LPCWSTR name)
{
	return S_OK;
}

/* ICoreWebView2Profile: the one profile of the view, kept in the environment's data folder. Browsing data lives with the page engine,
 * so clearing it completes at once. */
static const IID *const profile_iids[] = {&IID_ICoreWebView2Profile, &IID_ICoreWebView2Profile2, &IID_ICoreWebView2Profile3,
	&IID_ICoreWebView2Profile4, &IID_ICoreWebView2Profile5, &IID_ICoreWebView2Profile6, &IID_ICoreWebView2Profile7,
	&IID_ICoreWebView2Profile8};

static HRESULT STDMETHODCALLTYPE profile_query(ICoreWebView2Profile8 *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!is_iid(iid, profile_iids, ARRAYSIZE(profile_iids))) {
		trace_iid("profile lacks", iid);
		*out = NULL;
		return E_NOINTERFACE;
	}
	view_add_ref(VIEW_OF(self, profile));
	*out = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE profile_add_ref(ICoreWebView2Profile8 *self)
{
	return view_add_ref(VIEW_OF(self, profile));
}

static ULONG STDMETHODCALLTYPE profile_release(ICoreWebView2Profile8 *self)
{
	return view_release(VIEW_OF(self, profile));
}

static HRESULT STDMETHODCALLTYPE profile_name(ICoreWebView2Profile8 *self, LPWSTR *value)
{
	return co_copy(L"Default", value);
}

static HRESULT STDMETHODCALLTYPE profile_private(ICoreWebView2Profile8 *self, BOOL *value)
{
	if (!value)
		return E_POINTER;
	*value = FALSE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE profile_path(ICoreWebView2Profile8 *self, LPWSTR *value)
{
	const WCHAR *folder = VIEW_OF(self, profile)->owner->data_folder;
	WCHAR path[MAX_PATH];
	if (lstrlenW(folder) + 20 >= MAX_PATH)
		return E_FAIL;
	lstrcpyW(path, folder);
	lstrcatW(path, L"\\EBWebView\\Default");
	return co_copy(path, value);
}

static HRESULT STDMETHODCALLTYPE profile_get_downloads(ICoreWebView2Profile8 *self, LPWSTR *value)
{
	const WCHAR *chosen = VIEW_OF(self, profile)->downloads;
	WCHAR path[MAX_PATH];
	if (chosen)
		return co_copy(chosen, value);
	const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH - 12);
	if (!length || length >= MAX_PATH - 12)
		return co_copy(L"", value);
	lstrcatW(path, L"\\Downloads");
	return co_copy(path, value);
}

static HRESULT STDMETHODCALLTYPE profile_put_downloads(ICoreWebView2Profile8 *self, LPCWSTR value)
{
	view *object = VIEW_OF(self, profile);
	if (!value)
		return E_INVALIDARG;
	WCHAR *copy = heap_copy(value);
	if (!copy)
		return E_OUTOFMEMORY;
	HeapFree(GetProcessHeap(), 0, object->downloads);
	object->downloads = copy;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE profile_get_scheme(ICoreWebView2Profile8 *self, COREWEBVIEW2_PREFERRED_COLOR_SCHEME *value)
{
	if (!value)
		return E_POINTER;
	*value = VIEW_OF(self, profile)->color_scheme;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE profile_put_scheme(ICoreWebView2Profile8 *self, COREWEBVIEW2_PREFERRED_COLOR_SCHEME value)
{
	VIEW_OF(self, profile)->color_scheme = value;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE profile_clear(ICoreWebView2Profile8 *self, COREWEBVIEW2_BROWSING_DATA_KINDS kinds, IUnknown *handler)
{
	return post_job(VIEW_OF(self, profile)->owner->messages, handler, S_OK, NULL, NULL) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE profile_clear_range(ICoreWebView2Profile8 *self, COREWEBVIEW2_BROWSING_DATA_KINDS kinds, double start,
	double end, IUnknown *handler)
{
	return profile_clear(self, kinds, handler);
}

static HRESULT STDMETHODCALLTYPE profile_clear_all(ICoreWebView2Profile8 *self, IUnknown *handler)
{
	return profile_clear(self, 0, handler);
}

/* Permissions are the page engine's to grant: a state a program sets for an origin is taken, and the change completes at once. */
static HRESULT STDMETHODCALLTYPE profile_set_permission(ICoreWebView2Profile8 *self, COREWEBVIEW2_PERMISSION_KIND kind, LPCWSTR origin,
	COREWEBVIEW2_PERMISSION_STATE state, IUnknown *handler)
{
	if (!origin)
		return E_INVALIDARG;
	return post_job(VIEW_OF(self, profile)->owner->messages, handler, S_OK, NULL, NULL) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE profile_get_tracking(ICoreWebView2Profile8 *self, COREWEBVIEW2_TRACKING_PREVENTION_LEVEL *value)
{
	if (!value)
		return E_POINTER;
	*value = COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_BALANCED;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE profile_put_tracking(ICoreWebView2Profile8 *self, COREWEBVIEW2_TRACKING_PREVENTION_LEVEL value)
{
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE profile_get_off(ICoreWebView2Profile8 *self, BOOL *value)
{
	if (!value)
		return E_POINTER;
	*value = FALSE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE profile_put_flag(ICoreWebView2Profile8 *self, BOOL value)
{
	return S_OK;
}

/* The profile is never deleted, so its Deleted event takes handlers and is never raised. */
static HRESULT STDMETHODCALLTYPE profile_add_deleted(ICoreWebView2Profile8 *self, IUnknown *handler, EventRegistrationToken *token)
{
	static LONG next_token;
	if (!handler || !token)
		return E_INVALIDARG;
	token->value = InterlockedIncrement(&next_token);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE profile_remove_deleted(ICoreWebView2Profile8 *self, EventRegistrationToken token)
{
	return S_OK;
}

static ICoreWebView2Profile8Vtbl profile_table;

static void profile_members(void)
{
	stub_table(&profile_table, sizeof(profile_table), "Profile");
	profile_table.QueryInterface = profile_query;
	profile_table.AddRef = profile_add_ref;
	profile_table.Release = profile_release;
	profile_table.get_ProfileName = profile_name;
	profile_table.get_IsInPrivateModeEnabled = profile_private;
	profile_table.get_ProfilePath = profile_path;
	profile_table.get_DefaultDownloadFolderPath = profile_get_downloads;
	profile_table.put_DefaultDownloadFolderPath = profile_put_downloads;
	profile_table.get_PreferredColorScheme = profile_get_scheme;
	profile_table.put_PreferredColorScheme = profile_put_scheme;
	profile_table.ClearBrowsingData = (void *)profile_clear;
	profile_table.ClearBrowsingDataInTimeRange = (void *)profile_clear_range;
	profile_table.ClearBrowsingDataAll = (void *)profile_clear_all;
	profile_table.SetPermissionState = (void *)profile_set_permission;
	profile_table.get_PreferredTrackingPreventionLevel = profile_get_tracking;
	profile_table.put_PreferredTrackingPreventionLevel = profile_put_tracking;
	profile_table.get_IsPasswordAutosaveEnabled = profile_get_off;
	profile_table.put_IsPasswordAutosaveEnabled = profile_put_flag;
	profile_table.get_IsGeneralAutofillEnabled = profile_get_off;
	profile_table.put_IsGeneralAutofillEnabled = profile_put_flag;
	profile_table.add_Deleted = (void *)profile_add_deleted;
	profile_table.remove_Deleted = profile_remove_deleted;
}

static HRESULT STDMETHODCALLTYPE webview_get_profile(ICoreWebView2_22 *self, ICoreWebView2Profile **profile)
{
	if (!profile)
		return E_POINTER;
	view *object = WEBVIEW_OF(self);
	view_add_ref(object);
	*profile = (ICoreWebView2Profile *)&object->profile;
	return S_OK;
}

/* A DevTools protocol method: the page engine takes none from programs, so each answers with an empty result. */
static HRESULT STDMETHODCALLTYPE webview_devtools(ICoreWebView2_22 *self, LPCWSTR method, LPCWSTR parameters, IUnknown *handler)
{
	if (!method)
		return E_INVALIDARG;
	trace("DevTools method answered empty:", method);
	return post_job(WEBVIEW_OF(self)->owner->messages, handler, S_OK, NULL, L"{}") ? S_OK : E_OUTOFMEMORY;
}

/* The receiver of a DevTools protocol event: the page engine raises none to programs, so a handler is taken and never called. One
 * receiver, living as long as the runtime, serves every event name. */
static HRESULT STDMETHODCALLTYPE receiver_query(ICoreWebView2DevToolsProtocolEventReceiver *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!IsEqualIID(iid, &unknown_iid) && !IsEqualIID(iid, &IID_ICoreWebView2DevToolsProtocolEventReceiver)) {
		trace_iid("DevTools event receiver lacks", iid);
		*out = NULL;
		return E_NOINTERFACE;
	}
	*out = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE receiver_add_ref(ICoreWebView2DevToolsProtocolEventReceiver *self)
{
	return 2;
}

static ULONG STDMETHODCALLTYPE receiver_release(ICoreWebView2DevToolsProtocolEventReceiver *self)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE receiver_add(ICoreWebView2DevToolsProtocolEventReceiver *self, IUnknown *handler, EventRegistrationToken *token)
{
	static LONG next_token;
	if (!handler || !token)
		return E_INVALIDARG;
	token->value = InterlockedIncrement(&next_token);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE receiver_remove(ICoreWebView2DevToolsProtocolEventReceiver *self, EventRegistrationToken token)
{
	return S_OK;
}

static ICoreWebView2DevToolsProtocolEventReceiverVtbl receiver_table = {receiver_query, receiver_add_ref, receiver_release, (void *)receiver_add,
	receiver_remove};
static ICoreWebView2DevToolsProtocolEventReceiver receiver = {&receiver_table};

static HRESULT STDMETHODCALLTYPE webview_devtools_receiver(ICoreWebView2_22 *self, LPCWSTR event, ICoreWebView2DevToolsProtocolEventReceiver **out)
{
	if (!event || !out)
		return E_INVALIDARG;
	trace("DevTools event never raised:", event);
	*out = &receiver;
	return S_OK;
}

static ICoreWebView2_22Vtbl webview_table;

static void webview_members(void)
{
	stub_table(&webview_table, sizeof(webview_table), "CoreWebView2");
	webview_table.QueryInterface = webview_query;
	webview_table.AddRef = webview_add_ref;
	webview_table.Release = webview_release;
	webview_table.get_Settings = webview_get_settings;
	webview_table.get_Profile = webview_get_profile;
	webview_table.CallDevToolsProtocolMethod = (void *)webview_devtools;
	webview_table.GetDevToolsProtocolEventReceiver = webview_devtools_receiver;
	webview_table.get_Source = webview_get_source;
	webview_table.Navigate = webview_navigate;
	webview_table.NavigateToString = webview_navigate_to_string;
#define VIEW_EVENT_TABLE(name) \
	webview_table.add_##name = (void *)webview_add_##name; \
	webview_table.remove_##name = webview_remove_##name;
	VIEW_EVENTS(VIEW_EVENT_TABLE)
	webview_table.AddScriptToExecuteOnDocumentCreated = (void *)webview_add_script;
	webview_table.RemoveScriptToExecuteOnDocumentCreated = webview_remove_script;
	webview_table.ExecuteScript = (void *)webview_execute;
	webview_table.Reload = webview_reload;
	webview_table.PostWebMessageAsJson = webview_post_json;
	webview_table.PostWebMessageAsString = webview_post_string;
	webview_table.get_BrowserProcessId = webview_process_id;
	webview_table.get_CanGoBack = webview_false;
	webview_table.get_CanGoForward = webview_false;
	webview_table.GoBack = webview_nothing;
	webview_table.GoForward = webview_nothing;
	webview_table.Stop = webview_nothing;
	webview_table.get_DocumentTitle = webview_empty;
	webview_table.AddHostObjectToScript = webview_add_host_object;
	webview_table.RemoveHostObjectFromScript = webview_remove_host_object;
	webview_table.get_ContainsFullScreenElement = webview_false;
	webview_table.AddWebResourceRequestedFilter = webview_resource_filter;
	webview_table.RemoveWebResourceRequestedFilter = webview_resource_filter;
	webview_table.AddWebResourceRequestedFilterWithRequestSourceKinds = webview_resource_filter_kinds;
	webview_table.RemoveWebResourceRequestedFilterWithRequestSourceKinds = webview_resource_filter_kinds;
	webview_table.get_Environment = webview_get_environment;
	webview_table.get_IsSuspended = webview_false;
	webview_table.Resume = webview_nothing;
	webview_table.SetVirtualHostNameToFolderMapping = webview_folder_mapping;
	webview_table.ClearVirtualHostNameToFolderMapping = webview_clear_mapping;
	webview_table.get_IsMuted = webview_false;
	webview_table.put_IsMuted = webview_accept_bool;
	webview_table.get_IsDocumentPlayingAudio = webview_false;
	webview_table.get_IsDefaultDownloadDialogOpen = webview_false;
	webview_table.get_StatusBarText = webview_empty;
	webview_table.get_FaviconUri = webview_empty;
}

/* ICoreWebView2Controller. */
static const IID *const controller_iids[] = {&IID_ICoreWebView2Controller, &IID_ICoreWebView2Controller2, &IID_ICoreWebView2Controller3,
	&IID_ICoreWebView2Controller4};

#define CONTROLLER_OF(self) VIEW_OF(self, controller)

static HRESULT STDMETHODCALLTYPE controller_query(ICoreWebView2Controller4 *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!is_iid(iid, controller_iids, ARRAYSIZE(controller_iids))) {
		trace_iid("controller lacks", iid);
		*out = NULL;
		return E_NOINTERFACE;
	}
	view_add_ref(CONTROLLER_OF(self));
	*out = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE controller_add_ref(ICoreWebView2Controller4 *self)
{
	return view_add_ref(CONTROLLER_OF(self));
}

static ULONG STDMETHODCALLTYPE controller_release(ICoreWebView2Controller4 *self)
{
	return view_release(CONTROLLER_OF(self));
}

static HRESULT STDMETHODCALLTYPE controller_get_visible(ICoreWebView2Controller4 *self, BOOL *value)
{
	if (!value)
		return E_POINTER;
	*value = CONTROLLER_OF(self)->visible;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_put_visible(ICoreWebView2Controller4 *self, BOOL value)
{
	view *object = CONTROLLER_OF(self);
	object->visible = value != FALSE;
	if (object->window)
		ShowWindow(object->window, object->visible ? SW_SHOWNA : SW_HIDE);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_get_bounds(ICoreWebView2Controller4 *self, RECT *value)
{
	if (!value)
		return E_POINTER;
	*value = CONTROLLER_OF(self)->bounds;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_put_bounds(ICoreWebView2Controller4 *self, RECT value)
{
	view *object = CONTROLLER_OF(self);
	object->bounds = value;
	if (object->window)
		SetWindowPos(object->window, NULL, value.left, value.top, value.right - value.left, value.bottom - value.top, SWP_NOZORDER | SWP_NOACTIVATE);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_get_zoom(ICoreWebView2Controller4 *self, double *value)
{
	if (!value)
		return E_POINTER;
	*value = CONTROLLER_OF(self)->zoom;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_put_zoom(ICoreWebView2Controller4 *self, double value)
{
	CONTROLLER_OF(self)->zoom = value;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_bounds_and_zoom(ICoreWebView2Controller4 *self, RECT bounds, double zoom)
{
	controller_put_zoom(self, zoom);
	return controller_put_bounds(self, bounds);
}

static HRESULT STDMETHODCALLTYPE controller_move_focus(ICoreWebView2Controller4 *self, COREWEBVIEW2_MOVE_FOCUS_REASON reason)
{
	view *object = CONTROLLER_OF(self);
	if (!object->window)
		return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
	HWND child = GetWindow(object->window, GW_CHILD);
	SetFocus(child ? child : object->window);
	return S_OK;
}

#define CONTROLLER_EVENT_MEMBERS(name) \
	static HRESULT STDMETHODCALLTYPE controller_add_##name(ICoreWebView2Controller4 *self, IUnknown *handler, EventRegistrationToken *token) \
	{ \
		return subscribe(CONTROLLER_OF(self), event_##name, handler, token); \
	} \
	static HRESULT STDMETHODCALLTYPE controller_remove_##name(ICoreWebView2Controller4 *self, EventRegistrationToken token) \
	{ \
		return unsubscribe(CONTROLLER_OF(self), event_##name, token); \
	}
CONTROLLER_EVENTS(CONTROLLER_EVENT_MEMBERS)

static HRESULT STDMETHODCALLTYPE controller_get_parent(ICoreWebView2Controller4 *self, HWND *value)
{
	if (!value)
		return E_POINTER;
	view *object = CONTROLLER_OF(self);
	*value = object->window ? GetParent(object->window) : NULL;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_put_parent(ICoreWebView2Controller4 *self, HWND value)
{
	view *object = CONTROLLER_OF(self);
	if (!object->window)
		return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
	if (!IsWindow(value))
		return E_INVALIDARG;
	SetParent(object->window, value);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_position_changed(ICoreWebView2Controller4 *self)
{
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_close(ICoreWebView2Controller4 *self)
{
	view_close(CONTROLLER_OF(self));
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_get_webview(ICoreWebView2Controller4 *self, ICoreWebView2 **value)
{
	if (!value)
		return E_POINTER;
	view *object = CONTROLLER_OF(self);
	if (object->closed) {
		*value = NULL;
		return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
	}
	view_add_ref(object);
	*value = (ICoreWebView2 *)&object->webview;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_get_background(ICoreWebView2Controller4 *self, COREWEBVIEW2_COLOR *value)
{
	if (!value)
		return E_POINTER;
	*value = CONTROLLER_OF(self)->background;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_put_background(ICoreWebView2Controller4 *self, COREWEBVIEW2_COLOR value)
{
	CONTROLLER_OF(self)->background = value;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_get_scale(ICoreWebView2Controller4 *self, double *value)
{
	if (!value)
		return E_POINTER;
	*value = CONTROLLER_OF(self)->scale;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_put_scale(ICoreWebView2Controller4 *self, double value)
{
	CONTROLLER_OF(self)->scale = value;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_get_true(ICoreWebView2Controller4 *self, BOOL *value)
{
	if (!value)
		return E_POINTER;
	*value = TRUE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_accept_bool(ICoreWebView2Controller4 *self, BOOL value)
{
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_get_bounds_mode(ICoreWebView2Controller4 *self, COREWEBVIEW2_BOUNDS_MODE *value)
{
	if (!value)
		return E_POINTER;
	*value = COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_put_bounds_mode(ICoreWebView2Controller4 *self, COREWEBVIEW2_BOUNDS_MODE value)
{
	return S_OK;
}

static ICoreWebView2Controller4Vtbl controller_table;

static void controller_members(void)
{
	stub_table(&controller_table, sizeof(controller_table), "Controller");
	controller_table.QueryInterface = controller_query;
	controller_table.AddRef = controller_add_ref;
	controller_table.Release = controller_release;
	controller_table.get_IsVisible = controller_get_visible;
	controller_table.put_IsVisible = controller_put_visible;
	controller_table.get_Bounds = controller_get_bounds;
	controller_table.put_Bounds = controller_put_bounds;
	controller_table.get_ZoomFactor = controller_get_zoom;
	controller_table.put_ZoomFactor = controller_put_zoom;
	controller_table.SetBoundsAndZoomFactor = controller_bounds_and_zoom;
	controller_table.MoveFocus = controller_move_focus;
#define CONTROLLER_EVENT_TABLE(name) \
	controller_table.add_##name = (void *)controller_add_##name; \
	controller_table.remove_##name = controller_remove_##name;
	CONTROLLER_EVENTS(CONTROLLER_EVENT_TABLE)
	controller_table.get_ParentWindow = controller_get_parent;
	controller_table.put_ParentWindow = controller_put_parent;
	controller_table.NotifyParentWindowPositionChanged = controller_position_changed;
	controller_table.Close = controller_close;
	controller_table.get_CoreWebView2 = controller_get_webview;
	controller_table.get_DefaultBackgroundColor = controller_get_background;
	controller_table.put_DefaultBackgroundColor = controller_put_background;
	controller_table.get_RasterizationScale = controller_get_scale;
	controller_table.put_RasterizationScale = controller_put_scale;
	controller_table.get_ShouldDetectMonitorScaleChanges = controller_get_true;
	controller_table.put_ShouldDetectMonitorScaleChanges = controller_accept_bool;
	controller_table.get_BoundsMode = controller_get_bounds_mode;
	controller_table.put_BoundsMode = controller_put_bounds_mode;
	controller_table.get_AllowExternalDrop = controller_get_true;
	controller_table.put_AllowExternalDrop = controller_accept_bool;
}

static const WCHAR view_class[] = L"Raven WebView2";

static view *new_view(environment *owner, HWND parent)
{
	view *object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
	if (!object)
		return NULL;
	object->controller.lpVtbl = &controller_table;
	object->webview.lpVtbl = &webview_table;
	object->settings.lpVtbl = &settings_table;
	object->profile.lpVtbl = &profile_table;
	object->refs = 1;
	object->owner = owner;
	ICoreWebView2Environment13_AddRef(&owner->face);
	object->visible = TRUE;
	object->zoom = object->scale = 1.0;
	object->background = (COREWEBVIEW2_COLOR){255, 255, 255, 255};
#define SETTING_INITIAL(name, initial) object->flags[setting_##name] = initial;
	VIEW_SETTINGS(SETTING_INITIAL)
	object->window = CreateWindowExW(0, view_class, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, NULL,
		module, NULL);
	if (!object->window) {
		object->closed = TRUE;
		view_release(object);
		return NULL;
	}
	SetWindowLongPtrW(object->window, GWLP_USERDATA, (LONG_PTR)object);
	return object;
}

/* ICoreWebView2ControllerOptions. */
typedef struct options {
	ICoreWebView2ControllerOptions2 face;
	LONG refs;
	BOOL private_mode;
	WCHAR *profile, *locale;
} options;

static const IID *const options_iids[] = {&IID_ICoreWebView2ControllerOptions, &IID_ICoreWebView2ControllerOptions2};

static HRESULT STDMETHODCALLTYPE options_query(ICoreWebView2ControllerOptions2 *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!is_iid(iid, options_iids, ARRAYSIZE(options_iids))) {
		trace_iid("controller options lack", iid);
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&((options *)self)->refs);
	*out = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE options_add_ref(ICoreWebView2ControllerOptions2 *self)
{
	return InterlockedIncrement(&((options *)self)->refs);
}

static ULONG STDMETHODCALLTYPE options_release(ICoreWebView2ControllerOptions2 *self)
{
	options *object = (options *)self;
	const LONG refs = InterlockedDecrement(&object->refs);
	if (!refs) {
		HeapFree(GetProcessHeap(), 0, object->profile);
		HeapFree(GetProcessHeap(), 0, object->locale);
		HeapFree(GetProcessHeap(), 0, object);
	}
	return refs;
}

static HRESULT STDMETHODCALLTYPE options_get_profile(ICoreWebView2ControllerOptions2 *self, LPWSTR *value)
{
	return co_copy(((options *)self)->profile, value);
}

static HRESULT STDMETHODCALLTYPE options_put_profile(ICoreWebView2ControllerOptions2 *self, LPCWSTR value)
{
	options *object = (options *)self;
	HeapFree(GetProcessHeap(), 0, object->profile);
	object->profile = heap_copy(value);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE options_get_private(ICoreWebView2ControllerOptions2 *self, BOOL *value)
{
	if (!value)
		return E_POINTER;
	*value = ((options *)self)->private_mode;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE options_put_private(ICoreWebView2ControllerOptions2 *self, BOOL value)
{
	((options *)self)->private_mode = value;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE options_get_locale(ICoreWebView2ControllerOptions2 *self, LPWSTR *value)
{
	return co_copy(((options *)self)->locale, value);
}

static HRESULT STDMETHODCALLTYPE options_put_locale(ICoreWebView2ControllerOptions2 *self, LPCWSTR value)
{
	options *object = (options *)self;
	HeapFree(GetProcessHeap(), 0, object->locale);
	object->locale = heap_copy(value);
	return S_OK;
}

static ICoreWebView2ControllerOptions2Vtbl options_table;

static void options_members(void)
{
	stub_table(&options_table, sizeof(options_table), "ControllerOptions");
	options_table.QueryInterface = options_query;
	options_table.AddRef = options_add_ref;
	options_table.Release = options_release;
	options_table.get_ProfileName = options_get_profile;
	options_table.put_ProfileName = options_put_profile;
	options_table.get_IsInPrivateModeEnabled = options_get_private;
	options_table.put_IsInPrivateModeEnabled = options_put_private;
	options_table.get_ScriptLocale = options_get_locale;
	options_table.put_ScriptLocale = options_put_locale;
}

/* ICoreWebView2Environment. */
static const IID *const environment_iids[] = {&IID_ICoreWebView2Environment, &IID_ICoreWebView2Environment2, &IID_ICoreWebView2Environment3,
	&IID_ICoreWebView2Environment4, &IID_ICoreWebView2Environment5, &IID_ICoreWebView2Environment6, &IID_ICoreWebView2Environment7,
	&IID_ICoreWebView2Environment8, &IID_ICoreWebView2Environment9, &IID_ICoreWebView2Environment10, &IID_ICoreWebView2Environment11,
	&IID_ICoreWebView2Environment12, &IID_ICoreWebView2Environment13};

static HRESULT STDMETHODCALLTYPE environment_query(ICoreWebView2Environment13 *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!is_iid(iid, environment_iids, ARRAYSIZE(environment_iids))) {
		trace_iid("environment lacks", iid);
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&((environment *)self)->refs);
	*out = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE environment_add_ref(ICoreWebView2Environment13 *self)
{
	return InterlockedIncrement(&((environment *)self)->refs);
}

static ULONG STDMETHODCALLTYPE environment_release(ICoreWebView2Environment13 *self)
{
	environment *object = (environment *)self;
	const LONG refs = InterlockedDecrement(&object->refs);
	if (!refs) {
		// Completions still queued for the messages window run first.
		PostMessageW(object->messages, WM_CLOSE, 0, 0);
		HeapFree(GetProcessHeap(), 0, object->data_folder);
		HeapFree(GetProcessHeap(), 0, object);
	}
	return refs;
}

static HRESULT create_controller(environment *owner, HWND parent, IUnknown *handler)
{
	if (!IsWindow(parent))
		return E_INVALIDARG;
	view *object = new_view(owner, parent);
	trace(object ? "controller created" : "controller not created", NULL);
	if (!object)
		return post_job(owner->messages, handler, E_FAIL, NULL, NULL) ? S_OK : E_OUTOFMEMORY;
	const BOOL posted_job = post_job(owner->messages, handler, S_OK, (IUnknown *)&object->controller, NULL);
	view_release(object);
	return posted_job ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE environment_create_controller(ICoreWebView2Environment13 *self, HWND parent, IUnknown *handler)
{
	return create_controller((environment *)self, parent, handler);
}

static HRESULT STDMETHODCALLTYPE environment_create_controller_with_options(ICoreWebView2Environment13 *self, HWND parent, IUnknown *choices,
	IUnknown *handler)
{
	return create_controller((environment *)self, parent, handler);
}

static HRESULT STDMETHODCALLTYPE environment_create_options(ICoreWebView2Environment13 *self, ICoreWebView2ControllerOptions **out)
{
	if (!out)
		return E_POINTER;
	options *object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
	if (!object)
		return E_OUTOFMEMORY;
	object->face.lpVtbl = &options_table;
	object->refs = 1;
	*out = (ICoreWebView2ControllerOptions *)&object->face;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE environment_version(ICoreWebView2Environment13 *self, LPWSTR *value)
{
	return co_copy(RAVEN_WEBVIEW_VERSION, value);
}

static HRESULT STDMETHODCALLTYPE environment_data_folder(ICoreWebView2Environment13 *self, LPWSTR *value)
{
	return co_copy(((environment *)self)->data_folder, value);
}

static HRESULT STDMETHODCALLTYPE environment_add_ignored(ICoreWebView2Environment13 *self, IUnknown *handler, EventRegistrationToken *token)
{
	if (!handler || !token)
		return E_POINTER;
	token->value = InterlockedIncrement(&next_token);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE environment_remove_ignored(ICoreWebView2Environment13 *self, EventRegistrationToken token)
{
	return S_OK;
}

static ICoreWebView2Environment13Vtbl environment_table;

static void environment_members(void)
{
	stub_table(&environment_table, sizeof(environment_table), "Environment");
	environment_table.QueryInterface = environment_query;
	environment_table.AddRef = environment_add_ref;
	environment_table.Release = environment_release;
	environment_table.CreateCoreWebView2Controller = (void *)environment_create_controller;
	environment_table.CreateCoreWebView2ControllerWithOptions = (void *)environment_create_controller_with_options;
	environment_table.CreateCoreWebView2ControllerOptions = environment_create_options;
	environment_table.get_BrowserVersionString = environment_version;
	environment_table.get_UserDataFolder = environment_data_folder;
	environment_table.add_NewBrowserVersionAvailable = (void *)environment_add_ignored;
	environment_table.remove_NewBrowserVersionAvailable = environment_remove_ignored;
	environment_table.add_BrowserProcessExited = (void *)environment_add_ignored;
	environment_table.remove_BrowserProcessExited = environment_remove_ignored;
	environment_table.add_ProcessInfosChanged = (void *)environment_add_ignored;
	environment_table.remove_ProcessInfosChanged = environment_remove_ignored;
}

static BOOL prepare(void)
{
	static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	BOOL pending;
	if (!InitOnceBeginInitialize(&once, 0, &pending, NULL))
		return FALSE;
	if (!pending)
		return TRUE;
	arguments_tables();
	settings_members();
	profile_members();
	webview_members();
	controller_members();
	options_members();
	environment_members();
	WNDCLASSW description = {0};
	description.lpfnWndProc = view_procedure;
	description.hInstance = module;
	description.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	description.lpszClassName = view_class;
	const BOOL registered = RegisterClassW(&description) != 0;
	InitOnceComplete(&once, registered ? 0 : INIT_ONCE_INIT_FAILED, NULL);
	return registered;
}

/* What the loader calls: an environment for the calling thread, given to the handler from that thread's messages. */
__declspec(dllexport) HRESULT STDMETHODCALLTYPE CreateWebViewEnvironmentWithOptionsInternal(int checked, int runtime_type, LPCWSTR data_folder,
	IUnknown *choices, IUnknown *handler)
{
	if (!handler)
		return E_POINTER;
	trace("environment asked, data in", data_folder);
	if (!prepare())
		return E_FAIL;
	// The loader may unload the runtime once this returns; the objects it hands out need it for the life of the process.
	HMODULE pinned;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)module, &pinned);
	environment *object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
	if (!object)
		return E_OUTOFMEMORY;
	object->face.lpVtbl = &environment_table;
	object->refs = 1;
	object->data_folder = heap_copy(data_folder ? data_folder : L"");
	object->messages = CreateWindowExW(0, view_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, module, NULL);
	if (!object->messages) {
		HeapFree(GetProcessHeap(), 0, object->data_folder);
		HeapFree(GetProcessHeap(), 0, object);
		return HRESULT_FROM_WIN32(GetLastError());
	}
	const BOOL posted_job = post_job(object->messages, handler, S_OK, (IUnknown *)&object->face, NULL);
	ICoreWebView2Environment13_Release(&object->face);
	return posted_job ? S_OK : E_OUTOFMEMORY;
}

__declspec(dllexport) HRESULT STDMETHODCALLTYPE DllCanUnloadNow(void)
{
	return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		module = instance;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}
