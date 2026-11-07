/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven's page engine: the Chromium Embedded Framework the runtime keeps in the system directory's raven-cef folder, drawing the pages of
 * the runtime's web controls, the WebBrowser control first.
 *
 * The engine starts with the first navigation to an address that is not about:blank. Its browser runs its own message loop on a thread
 * of its own and opens its window as a child of the control's window. Every call back from it reaches the control as a message to that
 * window. Addresses whose scheme Chromium does not know, such as a program's own pluggable protocol, are read through urlmon on the
 * control's thread, so the program's protocol handler serves them as it would for Internet Explorer; such schemes are registered as
 * standard schemes when the engine starts and passed on to its processes. Pages run in this process, where external.c gives them the
 * program's window.external. The engine starts once in a process, so every control of the process shares it. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WEBENGINE_BUILD
#include <windows.h>
#include <objbase.h>
#include <urlmon.h>
#include "engine_cef.h"
#include "include/cef_api_hash.h"
#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_process_handler_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_command_line_capi.h"
#include "include/capi/cef_display_handler_capi.h"
#include "include/capi/cef_life_span_handler_capi.h"
#include "include/capi/cef_load_handler_capi.h"
#include "include/capi/cef_request_handler_capi.h"
#include "include/capi/cef_resource_handler_capi.h"
#include "include/capi/cef_scheme_capi.h"

/* The browser has come: lParam is the browser, with a reference the window takes over. */
#define ENGINE_ATTACHED (WM_APP + 0x54)
/* A request to a program's protocol: lParam is the request, with a reference for the window. */
#define ENGINE_FETCH (WM_APP + 0x55)

static const WCHAR browser_property[] = L"raven.engine.browser";
static const WCHAR pending_property[] = L"raven.engine.pending";
static const WCHAR expected_property[] = L"raven.engine.expected";

struct engine_functions cef;

static enum { engine_unknown, engine_running, engine_missing } state;
static WCHAR schemes[256];
static WCHAR folder[MAX_PATH];

void engine_trace(const char *what, const WCHAR *text)
{
	char line[1024];
	int length = wsprintfA(line, "webengine: %s", what);
	if (text) {
		line[length++] = ' ';
		// A long text is cut, at most three bytes to a character, rather than lost.
		int count = lstrlenW(text);
		const int room = ((int)sizeof(line) - length - 1) / 3;
		if (count > room)
			count = room;
		length += WideCharToMultiByte(CP_UTF8, 0, text, count, line + length, sizeof(line) - length - 1, NULL, NULL);
		line[length] = 0;
	}
	RavenTrace(line);
}

void engine_set_text(cef_string_t *out, const WCHAR *text)
{
	cef_string_utf16_t value = {0};
	cef.string_set((const char16_t *)text, lstrlenW(text), &value, 1);
	*out = value;
}

WCHAR *engine_copy_text(const cef_string_t *text)
{
	WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, ((text ? text->length : 0) + 1) * sizeof(WCHAR));
	if (!copy)
		return NULL;
	if (text && text->length)
		CopyMemory(copy, text->str, text->length * sizeof(WCHAR));
	copy[text ? text->length : 0] = 0;
	return copy;
}

WCHAR *engine_take_userfree(cef_string_userfree_t text)
{
	WCHAR *copy = engine_copy_text(text);
	if (text)
		cef.userfree_free(text);
	return copy;
}

void engine_release(void *object)
{
	if (object)
		((cef_base_ref_counted_t *)object)->release(object);
}

/* The schemes Chromium serves itself; any other one belongs to the program and goes through urlmon. */
static BOOL builtin_scheme(const WCHAR *scheme)
{
	static const WCHAR *const known[] = {L"http", L"https", L"about", L"data", L"file", L"blob", L"javascript", L"chrome", L"devtools", L"ws", L"wss"};
	for (int i = 0; i < ARRAYSIZE(known); i++)
		if (!lstrcmpiW(scheme, known[i]))
			return TRUE;
	return FALSE;
}

/* A scheme the engine registers as the program's own, unless Chromium knows it or it is listed already. */
static void add_scheme(const WCHAR *scheme)
{
	if (!*scheme || builtin_scheme(scheme))
		return;
	const int length = lstrlenW(scheme);
	for (const WCHAR *list = schemes; *list;) {
		const WCHAR *end = list;
		while (*end && *end != L',')
			end++;
		if (end - list == length && CompareStringOrdinal(list, length, scheme, length, TRUE) == CSTR_EQUAL)
			return;
		list = *end ? end + 1 : end;
	}
	const int used = lstrlenW(schemes);
	if (used + length + 2 > ARRAYSIZE(schemes))
		return;
	if (used)
		schemes[used] = L',';
	lstrcpyW(schemes + used + (used ? 1 : 0), scheme);
}

static BOOL scheme_of(const WCHAR *address, WCHAR *scheme, int size)
{
	int length = 0;
	while (address[length] && address[length] != L':' && length < size - 1) {
		const WCHAR character = address[length];
		if (!(character >= L'a' && character <= L'z') && !(character >= L'A' && character <= L'Z') && !(length && ((character >= L'0' && character <= L'9') ||
			character == L'+' || character == L'-' || character == L'.')))
			return FALSE;
		scheme[length] = character;
		length++;
	}
	if (address[length] != L':' || !length)
		return FALSE;
	scheme[length] = 0;
	return TRUE;
}


/* The control's window for a browser: the parent of the browser's own window. */
static HWND control_window(cef_browser_t *browser)
{
	if (!browser)
		return NULL;
	cef_browser_host_t *host = browser->get_host(browser);
	if (!host)
		return NULL;
	HWND window = host->get_window_handle(host);
	engine_release(host);
	return window ? GetParent(window) : NULL;
}

/* A program's protocol answers on the control's thread; Chromium waits for it on its own. */
typedef struct fetch {
	cef_resource_handler_t handler;
	LONG refs;
	struct fetch *next;
	HWND window;
	WCHAR *address;
	BYTE *data;
	DWORD size, offset;
	int status;
	const WCHAR *mime;
	cef_callback_t *callback;
} fetch;

static fetch *fetch_from(void *self)
{
	return CONTAINING_RECORD(self, fetch, handler);
}

static void CEF_CALLBACK fetch_add_ref(cef_base_ref_counted_t *self)
{
	InterlockedIncrement(&fetch_from(self)->refs);
}

static int CEF_CALLBACK fetch_release(cef_base_ref_counted_t *self)
{
	fetch *request = fetch_from(self);
	if (InterlockedDecrement(&request->refs))
		return 0;
	engine_release(request->callback);
	HeapFree(GetProcessHeap(), 0, request->address);
	HeapFree(GetProcessHeap(), 0, request->data);
	HeapFree(GetProcessHeap(), 0, request);
	return 1;
}

static int CEF_CALLBACK fetch_one_ref(cef_base_ref_counted_t *self)
{
	return fetch_from(self)->refs == 1;
}

static int CEF_CALLBACK fetch_some_ref(cef_base_ref_counted_t *self)
{
	return fetch_from(self)->refs >= 1;
}

static int CEF_CALLBACK fetch_open(cef_resource_handler_t *self, cef_request_t *cef_request, int *handle_request, cef_callback_t *callback)
{
	fetch *request = fetch_from(self);
	engine_release(cef_request);
	*handle_request = 0;
	request->callback = callback;
	fetch_add_ref(&self->base);
	if (!request->window || !PostMessageW(request->window, ENGINE_FETCH, 0, (LPARAM)request)) {
		request->callback = NULL;
		fetch_release(&self->base);
		engine_release(callback);
		request->status = 404;
		*handle_request = 1;
	}
	return 1;
}

static const WCHAR *mime_of(const WCHAR *address)
{
	static const struct {
		const WCHAR *extension, *mime;
	} types[] = {{L".htm", L"text/html"}, {L".html", L"text/html"}, {L".js", L"text/javascript"}, {L".css", L"text/css"}, {L".json", L"application/json"},
		{L".svg", L"image/svg+xml"}, {L".png", L"image/png"}, {L".gif", L"image/gif"}, {L".jpg", L"image/jpeg"}, {L".jpeg", L"image/jpeg"},
		{L".ico", L"image/x-icon"}, {L".woff", L"font/woff"}, {L".woff2", L"font/woff2"}, {L".ttf", L"font/ttf"}, {L".xml", L"text/xml"},
		{L".txt", L"text/plain"}, {L".map", L"application/json"}};
	int end = 0;
	while (address[end] && address[end] != L'?' && address[end] != L'#')
		end++;
	for (int i = 0; i < ARRAYSIZE(types); i++) {
		const int length = lstrlenW(types[i].extension);
		if (end >= length && CompareStringOrdinal(address + end - length, length, types[i].extension, length, TRUE) == CSTR_EQUAL)
			return types[i].mime;
	}
	return L"text/html";
}

/* On the control's thread: the whole answer of the program's protocol, then Chromium goes on. */
static void serve(fetch *request)
{
	IStream *stream = NULL;
	engine_trace("program protocol asked for", request->address);
	request->mime = mime_of(request->address);
	request->status = 404;
	if (SUCCEEDED(URLOpenBlockingStreamW(NULL, request->address, &stream, 0, NULL)) && stream) {
		DWORD capacity = 65536, got = 0;
		request->data = HeapAlloc(GetProcessHeap(), 0, capacity);
		while (request->data) {
			if (request->size == capacity) {
				BYTE *larger = HeapReAlloc(GetProcessHeap(), 0, request->data, capacity * 2);
				if (!larger)
					break;
				request->data = larger;
				capacity *= 2;
			}
			const HRESULT status = IStream_Read(stream, request->data + request->size, capacity - request->size, &got);
			request->size += got;
			if (FAILED(status) || !got)
				break;
		}
		IStream_Release(stream);
		request->status = 200;
		char size[48];
		wsprintfA(size, "program protocol gave %lu bytes for", request->size);
		engine_trace(size, request->address);
	} else
		engine_trace("program protocol has nothing at", request->address);
	cef_callback_t *callback = request->callback;
	request->callback = NULL;
	if (callback) {
		callback->cont(callback);
		engine_release(callback);
	}
	fetch_release(&request->handler.base);
}

/* A protocol's bind runs this thread's messages while it waits, and a request that came meanwhile waits its turn instead of starting
 * a bind inside the other. */
static void serve_in_turn(fetch *request)
{
	static fetch *first, *last;
	static BOOL serving;
	static DWORD owner;
	// The queue belongs to the first thread whose controls fetch; controls of another thread serve their requests as they come.
	if (!owner)
		owner = GetCurrentThreadId();
	if (owner != GetCurrentThreadId()) {
		serve(request);
		return;
	}
	request->next = NULL;
	if (last)
		last->next = request;
	else
		first = request;
	last = request;
	if (serving)
		return;
	serving = TRUE;
	while (first) {
		fetch *next = first;
		first = next->next;
		if (!first)
			last = NULL;
		serve(next);
	}
	serving = FALSE;
}

static void CEF_CALLBACK fetch_headers(cef_resource_handler_t *self, cef_response_t *response, int64_t *length, cef_string_t *redirect)
{
	fetch *request = fetch_from(self);
	cef_string_t mime = {0};
	engine_set_text(&mime, request->mime ? request->mime : L"text/html");
	response->set_mime_type(response, &mime);
	cef.string_clear(&mime);
	response->set_status(response, request->status);
	*length = request->status == 200 ? (int64_t)request->size : 0;
	engine_release(response);
}

static int CEF_CALLBACK fetch_skip(cef_resource_handler_t *self, int64_t count, int64_t *skipped, cef_resource_skip_callback_t *callback)
{
	fetch *request = fetch_from(self);
	const DWORD left = request->size - request->offset;
	const DWORD step = count < (int64_t)left ? (DWORD)count : left;
	request->offset += step;
	*skipped = step;
	engine_release(callback);
	return step > 0;
}

static int CEF_CALLBACK fetch_read(cef_resource_handler_t *self, void *out, int wanted, int *read, cef_resource_read_callback_t *callback)
{
	fetch *request = fetch_from(self);
	const DWORD left = request->size - request->offset;
	const DWORD step = (DWORD)wanted < left ? (DWORD)wanted : left;
	CopyMemory(out, request->data + request->offset, step);
	request->offset += step;
	*read = (int)step;
	engine_release(callback);
	return step > 0;
}

static void CEF_CALLBACK fetch_cancel(cef_resource_handler_t *self)
{
}

static cef_resource_handler_t *CEF_CALLBACK factory_create(cef_scheme_handler_factory_t *self, cef_browser_t *browser, cef_frame_t *frame,
	const cef_string_t *scheme, cef_request_t *cef_request)
{
	fetch *request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request));
	if (request) {
		request->handler.base.size = sizeof(request->handler);
		request->handler.base.add_ref = fetch_add_ref;
		request->handler.base.release = fetch_release;
		request->handler.base.has_one_ref = fetch_one_ref;
		request->handler.base.has_at_least_one_ref = fetch_some_ref;
		request->handler.open = fetch_open;
		request->handler.get_response_headers = fetch_headers;
		request->handler.skip = fetch_skip;
		request->handler.read = fetch_read;
		request->handler.cancel = fetch_cancel;
		request->refs = 1;
		request->window = control_window(browser);
		request->address = engine_take_userfree(cef_request->get_url(cef_request));
	}
	engine_release(cef_request);
	engine_release(frame);
	engine_release(browser);
	return request ? &request->handler : NULL;
}

static cef_scheme_handler_factory_t program_protocols = {.base = STATIC_BASE(cef_scheme_handler_factory_t), .create = factory_create};

/* Host names whose pages come from a folder, as a program maps them: http and https requests to such a host read the file at the
 * request's path under the folder. */
static struct {
	WCHAR host[256];
	WCHAR folder[MAX_PATH];
} mappings[16];
static int mapping_count;
static CRITICAL_SECTION mapping_lock;
static INIT_ONCE mapping_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK mapping_init(INIT_ONCE *once, void *parameter, void **context)
{
	InitializeCriticalSection(&mapping_lock);
	return TRUE;
}

static int hex_digit(WCHAR character)
{
	if (character >= L'0' && character <= L'9')
		return character - L'0';
	if (character >= L'a' && character <= L'f')
		return character - L'a' + 10;
	if (character >= L'A' && character <= L'F')
		return character - L'A' + 10;
	return -1;
}

/* The file a mapped address names, or FALSE when the address is not under a mapped host or leaves its folder. */
static BOOL mapped_file(const WCHAR *address, WCHAR *path)
{
	const WCHAR *at = address;
	while (*at && *at != L':')
		at++;
	if (at[0] != L':' || at[1] != L'/' || at[2] != L'/')
		return FALSE;
	const WCHAR *host = at + 3, *end = host;
	while (*end && *end != L'/' && *end != L':' && *end != L'?' && *end != L'#')
		end++;
	const WCHAR *rest = end;
	while (*rest && *rest != L'/' && *rest != L'?' && *rest != L'#')
		rest++;
	BOOL found = FALSE;
	InitOnceExecuteOnce(&mapping_once, mapping_init, NULL, NULL);
	EnterCriticalSection(&mapping_lock);
	for (int i = 0; i < mapping_count && !found; i++)
		if (lstrlenW(mappings[i].host) == end - host &&
			CompareStringOrdinal(host, (int)(end - host), mappings[i].host, (int)(end - host), TRUE) == CSTR_EQUAL) {
			lstrcpyW(path, mappings[i].folder);
			found = TRUE;
		}
	LeaveCriticalSection(&mapping_lock);
	if (!found)
		return FALSE;
	int length = lstrlenW(path);
	for (const WCHAR *part = rest; *part && *part != L'?' && *part != L'#'; part++) {
		WCHAR character = *part;
		if (character == L'%' && hex_digit(part[1]) >= 0 && hex_digit(part[2]) >= 0) {
			character = (WCHAR)(hex_digit(part[1]) * 16 + hex_digit(part[2]));
			part += 2;
		}
		if (character == L'/')
			character = L'\\';
		if (length >= MAX_PATH - 1 || !character)
			return FALSE;
		path[length++] = character;
	}
	path[length] = 0;
	// A path that climbs out of the folder names nothing.
	for (const WCHAR *part = path; *part; part++)
		if (part[0] == L'\\' && part[1] == L'.' && part[2] == L'.' && (!part[3] || part[3] == L'\\'))
			return FALSE;
	return TRUE;
}

static int CEF_CALLBACK file_open(cef_resource_handler_t *self, cef_request_t *cef_request, int *handle_request, cef_callback_t *callback)
{
	engine_release(cef_request);
	engine_release(callback);
	*handle_request = 1;
	return 1;
}

static cef_resource_handler_t *CEF_CALLBACK files_create(cef_scheme_handler_factory_t *self, cef_browser_t *browser, cef_frame_t *frame,
	const cef_string_t *scheme, cef_request_t *cef_request)
{
	WCHAR *address = engine_take_userfree(cef_request->get_url(cef_request));
	WCHAR path[MAX_PATH];
	fetch *request = NULL;
	if (address && mapped_file(address, path))
		request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request));
	if (request) {
		request->handler.base.size = sizeof(request->handler);
		request->handler.base.add_ref = fetch_add_ref;
		request->handler.base.release = fetch_release;
		request->handler.base.has_one_ref = fetch_one_ref;
		request->handler.base.has_at_least_one_ref = fetch_some_ref;
		request->handler.open = file_open;
		request->handler.get_response_headers = fetch_headers;
		request->handler.skip = fetch_skip;
		request->handler.read = fetch_read;
		request->handler.cancel = fetch_cancel;
		request->refs = 1;
		request->address = address;
		address = NULL;
		request->mime = mime_of(path);
		request->status = 404;
		HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
		LARGE_INTEGER size;
		if (file != INVALID_HANDLE_VALUE) {
			if (GetFileSizeEx(file, &size) && size.QuadPart < 0x40000000 &&
				(request->data = HeapAlloc(GetProcessHeap(), 0, size.QuadPart ? (SIZE_T)size.QuadPart : 1)) &&
				ReadFile(file, request->data, (DWORD)size.QuadPart, &request->size, NULL) && request->size == (DWORD)size.QuadPart)
				request->status = 200;
			CloseHandle(file);
		}
		if (request->status != 200)
			engine_trace("mapped folder has nothing at", request->address);
	}
	HeapFree(GetProcessHeap(), 0, address);
	engine_release(cef_request);
	engine_release(frame);
	engine_release(browser);
	// An address under no mapping, such as one of a host mapped only for another window, goes to the network.
	return request ? &request->handler : NULL;
}

static cef_scheme_handler_factory_t folder_files = {.base = STATIC_BASE(cef_scheme_handler_factory_t), .create = files_create};

static void register_mapping(int index)
{
	cef_string_t scheme = {0}, host = {0};
	engine_set_text(&host, mappings[index].host);
	for (int i = 0; i < 2; i++) {
		engine_set_text(&scheme, i ? L"http" : L"https");
		cef.register_factory(&scheme, &host, &folder_files);
		cef.string_clear(&scheme);
	}
	cef.string_clear(&host);
}

BOOL engine_map_folder(const WCHAR *host, const WCHAR *folder_path)
{
	if (!host || !*host || !folder_path || lstrlenW(host) >= 256 || lstrlenW(folder_path) >= MAX_PATH - 1)
		return FALSE;
	InitOnceExecuteOnce(&mapping_once, mapping_init, NULL, NULL);
	EnterCriticalSection(&mapping_lock);
	int index = 0;
	while (index < mapping_count && CompareStringOrdinal(mappings[index].host, -1, host, -1, TRUE) != CSTR_EQUAL)
		index++;
	if (index == ARRAYSIZE(mappings)) {
		LeaveCriticalSection(&mapping_lock);
		return FALSE;
	}
	if (index == mapping_count)
		mapping_count++;
	lstrcpyW(mappings[index].host, host);
	lstrcpyW(mappings[index].folder, folder_path);
	// The folder's path without a closing separator, as the request's path brings its own.
	int length = lstrlenW(mappings[index].folder);
	while (length && (mappings[index].folder[length - 1] == L'\\' || mappings[index].folder[length - 1] == L'/'))
		mappings[index].folder[--length] = 0;
	LeaveCriticalSection(&mapping_lock);
	if (state == engine_running)
		register_mapping(index);
	return TRUE;
}

static void CEF_CALLBACK after_created(cef_life_span_handler_t *self, cef_browser_t *browser)
{
	HWND window = control_window(browser);
	engine_trace(window ? "browser opened" : "browser opened outside a control", NULL);
	if (!window || !PostMessageW(window, ENGINE_ATTACHED, 0, (LPARAM)browser))
		engine_release(browser);
}

static cef_life_span_handler_t life_span = {.base = STATIC_BASE(cef_life_span_handler_t), .on_after_created = after_created};

static void reached(cef_browser_t *browser, cef_frame_t *frame, BOOL failed)
{
	if (frame->is_main(frame)) {
		HWND window = control_window(browser);
		WCHAR *address = engine_take_userfree(frame->get_url(frame));
		engine_trace("page reached", address);
		if (!window || !address || !PostMessageW(window, ENGINE_REACHED, failed, (LPARAM)address))
			HeapFree(GetProcessHeap(), 0, address);
	}
	engine_release(frame);
	engine_release(browser);
}

static void CEF_CALLBACK load_end(cef_load_handler_t *self, cef_browser_t *browser, cef_frame_t *frame, int status)
{
	reached(browser, frame, FALSE);
}

static void CEF_CALLBACK load_error(cef_load_handler_t *self, cef_browser_t *browser, cef_frame_t *frame, cef_errorcode_t error, const cef_string_t *text,
	const cef_string_t *address)
{
	WCHAR *failed = engine_copy_text(address);
	engine_trace("page failed", failed);
	HeapFree(GetProcessHeap(), 0, failed);
	// An aborted load is the start of another, not a failure.
	reached(browser, frame, error != ERR_ABORTED);
}

static cef_load_handler_t loads = {.base = STATIC_BASE(cef_load_handler_t), .on_load_end = load_end, .on_load_error = load_error};

/* A navigation of the main frame asks the control's sinks first, and they may cancel it. */
static int CEF_CALLBACK before_browse(cef_request_handler_t *self, cef_browser_t *browser, cef_frame_t *frame, cef_request_t *request, int gesture,
	int redirect)
{
	int cancel = 0;
	if (frame->is_main(frame)) {
		HWND window = control_window(browser);
		WCHAR *address = engine_take_userfree(request->get_url(request));
		DWORD_PTR answer = 0;
		if (window && address && SendMessageTimeoutW(window, ENGINE_BEFORE, redirect, (LPARAM)address, SMTO_NORMAL, 30000, &answer))
			cancel = answer != 0;
		HeapFree(GetProcessHeap(), 0, address);
	}
	engine_release(request);
	engine_release(frame);
	engine_release(browser);
	return cancel;
}

static cef_request_handler_t requests = {.base = STATIC_BASE(cef_request_handler_t), .on_before_browse = before_browse};

/* What a page writes to its console goes to the runtime's trace, which is where its script errors show. */
static int CEF_CALLBACK console_message(cef_display_handler_t *self, cef_browser_t *browser, cef_log_severity_t level, const cef_string_t *message,
	const cef_string_t *source, int line)
{
	WCHAR *text = engine_copy_text(message), *where = engine_copy_text(source);
	if (text && where) {
		WCHAR *joined = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(text) + lstrlenW(where) + 24) * sizeof(WCHAR));
		if (joined) {
			wsprintfW(joined, L"%s:%d %s", where, line, text);
			engine_trace("page says", joined);
			HeapFree(GetProcessHeap(), 0, joined);
		}
	}
	HeapFree(GetProcessHeap(), 0, text);
	HeapFree(GetProcessHeap(), 0, where);
	engine_release(browser);
	return 0;
}

static cef_display_handler_t display = {.base = STATIC_BASE(cef_display_handler_t), .on_console_message = console_message};

static cef_display_handler_t *CEF_CALLBACK get_display(cef_client_t *self)
{
	return &display;
}

static cef_life_span_handler_t *CEF_CALLBACK get_life_span(cef_client_t *self)
{
	return &life_span;
}

static cef_load_handler_t *CEF_CALLBACK get_load(cef_client_t *self)
{
	return &loads;
}

static cef_request_handler_t *CEF_CALLBACK get_requests(cef_client_t *self)
{
	return &requests;
}

static cef_client_t client = {.base = STATIC_BASE(cef_client_t), .get_display_handler = get_display, .get_life_span_handler = get_life_span, .get_load_handler = get_load,
	.get_request_handler = get_requests};

static void append_switch(cef_command_line_t *line, const WCHAR *name, const WCHAR *value)
{
	cef_string_t key = {0}, text = {0};
	engine_set_text(&key, name);
	if (value) {
		engine_set_text(&text, value);
		line->append_switch_with_value(line, &key, &text);
		cef.string_clear(&text);
	} else
		line->append_switch(line, &key);
	cef.string_clear(&key);
}

/* Pages are drawn in software: the browser composites without a GPU process. They run in the program's own process, as Internet
 * Explorer's do, so that a page's call to window.external reaches the program's object while the page waits for the answer. */
static void CEF_CALLBACK command_line(cef_app_t *self, const cef_string_t *type, cef_command_line_t *line)
{
	if (!type || !type->length) {
		append_switch(line, L"disable-gpu", NULL);
		append_switch(line, L"disable-gpu-compositing", NULL);
		append_switch(line, L"single-process", NULL);
	}
	engine_release(line);
}

static void register_scheme_list(cef_scheme_registrar_t *registrar)
{
	const WCHAR *list = schemes;
	while (*list) {
		const WCHAR *end = list;
		while (*end && *end != L',')
			end++;
		cef_string_t name = {0};
		cef_string_utf16_t value = {0};
		cef.string_set((const char16_t *)list, end - list, &value, 1);
		name = value;
		registrar->add_custom_scheme(registrar, &name, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
		cef.string_clear(&name);
		list = *end ? end + 1 : end;
	}
}

static void CEF_CALLBACK custom_schemes(cef_app_t *self, cef_scheme_registrar_t *registrar)
{
	register_scheme_list(registrar);
}

static void CEF_CALLBACK child_launch(cef_browser_process_handler_t *self, cef_command_line_t *line)
{
	if (*schemes)
		append_switch(line, L"raven-schemes", schemes);
	engine_release(line);
}

static cef_browser_process_handler_t browser_process = {.base = STATIC_BASE(cef_browser_process_handler_t), .on_before_child_process_launch = child_launch};

static cef_browser_process_handler_t *CEF_CALLBACK get_browser_process(cef_app_t *self)
{
	return &browser_process;
}

static cef_render_process_handler_t *CEF_CALLBACK get_render_process(cef_app_t *self)
{
	return &external_pages;
}

static cef_app_t app = {.base = STATIC_BASE(cef_app_t), .on_before_command_line_processing = command_line,
	.on_register_custom_schemes = custom_schemes, .get_browser_process_handler = get_browser_process, .get_render_process_handler = get_render_process};

static BOOL load_engine(void)
{
	if (!GetSystemDirectoryW(folder, MAX_PATH - 32))
		return FALSE;
	lstrcatW(folder, L"\\raven-cef");
	WCHAR path[MAX_PATH];
	wsprintfW(path, L"%s\\libcef.dll", folder);
	HMODULE library = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!library)
		return FALSE;
	cef.api_hash = (void *)GetProcAddress(library, "cef_api_hash");
	cef.initialize = (void *)GetProcAddress(library, "cef_initialize");
	cef.create_browser = (void *)GetProcAddress(library, "cef_browser_host_create_browser");
	cef.string_set = (void *)GetProcAddress(library, "cef_string_utf16_set");
	cef.string_clear = (void *)GetProcAddress(library, "cef_string_utf16_clear");
	cef.userfree_free = (void *)GetProcAddress(library, "cef_string_userfree_utf16_free");
	cef.register_factory = (void *)GetProcAddress(library, "cef_register_scheme_handler_factory");
	cef.dictionary = (void *)GetProcAddress(library, "cef_dictionary_value_create");
	cef.undefined = (void *)GetProcAddress(library, "cef_v8_value_create_undefined");
	cef.null = (void *)GetProcAddress(library, "cef_v8_value_create_null");
	cef.boolean = (void *)GetProcAddress(library, "cef_v8_value_create_bool");
	cef.integer = (void *)GetProcAddress(library, "cef_v8_value_create_int");
	cef.unsigned_integer = (void *)GetProcAddress(library, "cef_v8_value_create_uint");
	cef.number = (void *)GetProcAddress(library, "cef_v8_value_create_double");
	cef.string = (void *)GetProcAddress(library, "cef_v8_value_create_string");
	cef.object = (void *)GetProcAddress(library, "cef_v8_value_create_object");
	cef.function = (void *)GetProcAddress(library, "cef_v8_value_create_function");
	return cef.api_hash && cef.initialize && cef.create_browser && cef.string_set && cef.string_clear && cef.userfree_free && cef.register_factory &&
		cef.dictionary && cef.undefined && cef.null && cef.boolean && cef.integer && cef.unsigned_integer && cef.number && cef.string && cef.object &&
		cef.function;
}

/* The engine of this process, started with the scheme of the address that called for it. */
static BOOL start(const WCHAR *scheme)
{
	if (state == engine_running)
		return TRUE;
	if (state == engine_missing)
		return FALSE;
	state = engine_missing;
	if (!load_engine()) {
		engine_trace("no page engine in", folder);
		return FALSE;
	}
	cef.api_hash(CEF_API_VERSION, 0);
	add_scheme(scheme);
	// The schemes the process serves itself through urlmon's namespaces join the one that started the engine, as Chromium learns custom
	// schemes only when it starts and a control of another kind may start it first.
	DWORD (WINAPI *name_spaces)(WCHAR *, DWORD) = (void *)GetProcAddress(LoadLibraryW(L"urlmon.dll"), "RavenNameSpaces");
	WCHAR served[256];
	if (name_spaces && name_spaces(served, ARRAYSIZE(served)) < ARRAYSIZE(served))
		for (WCHAR *name = served; *name;) {
			WCHAR *end = name;
			while (*end && *end != L',')
				end++;
			const BOOL last = !*end;
			*end = 0;
			add_scheme(name);
			name = last ? end : end + 1;
		}
	if (*schemes)
		engine_trace("page engine serves the schemes", schemes);

	WCHAR text[MAX_PATH], program[MAX_PATH], data[MAX_PATH];
	cef_settings_t settings = {sizeof(settings)};
	settings.no_sandbox = 1;
	settings.multi_threaded_message_loop = 1;
	wsprintfW(text, L"%s\\raven-cefhost.exe", folder);
	engine_set_text(&settings.browser_subprocess_path, text);
	engine_set_text(&settings.resources_dir_path, folder);
	wsprintfW(text, L"%s\\locales", folder);
	engine_set_text(&settings.locales_dir_path, text);
	GetModuleFileNameW(NULL, program, MAX_PATH);
	const WCHAR *name = program;
	for (const WCHAR *at = program; *at; at++)
		if (*at == L'\\')
			name = at + 1;
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", data, MAX_PATH - 64) && !GetTempPathW(MAX_PATH - 64, data))
		return FALSE;
	wsprintfW(text, L"%s\\Raven\\WebBrowser\\%s", data, name);
	engine_set_text(&settings.root_cache_path, text);
	engine_set_text(&settings.cache_path, text);
	wsprintfW(text, L"%s\\Raven\\WebBrowser\\%s.log", data, name);
	engine_set_text(&settings.log_file, text);
	settings.log_severity = LOGSEVERITY_WARNING;
	cef_main_args_t arguments = {GetModuleHandleW(NULL)};
	if (!cef.initialize(&arguments, &settings, &app, NULL)) {
		engine_trace("page engine did not start", NULL);
		return FALSE;
	}
	for (const WCHAR *list = schemes; *list;) {
		const WCHAR *end = list;
		while (*end && *end != L',')
			end++;
		cef_string_t name_text = {0};
		cef.string_set((const char16_t *)list, end - list, &name_text, 1);
		cef.register_factory(&name_text, NULL, &program_protocols);
		cef.string_clear(&name_text);
		list = *end ? end + 1 : end;
	}
	state = engine_running;
	InitOnceExecuteOnce(&mapping_once, mapping_init, NULL, NULL);
	EnterCriticalSection(&mapping_lock);
	for (int i = 0; i < mapping_count; i++)
		register_mapping(i);
	LeaveCriticalSection(&mapping_lock);
	engine_trace("page engine started for", scheme);
	return TRUE;
}

static void load(cef_browser_t *browser, const WCHAR *address)
{
	cef_frame_t *frame = browser->get_main_frame(browser);
	if (!frame)
		return;
	cef_string_t url = {0};
	engine_set_text(&url, address);
	frame->load_url(frame, &url);
	cef.string_clear(&url);
	engine_release(frame);
}

static void replace_pending(HWND window, const WCHAR *address)
{
	WCHAR *old = GetPropW(window, pending_property);
	HeapFree(GetProcessHeap(), 0, old);
	WCHAR *copy = NULL;
	if (address) {
		copy = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(address) + 1) * sizeof(WCHAR));
		if (copy)
			lstrcpyW(copy, address);
	}
	if (copy)
		SetPropW(window, pending_property, copy);
	else
		RemovePropW(window, pending_property);
}

WEBENGINE_API BOOL engine_navigate(HWND window, const WCHAR *address)
{
	WCHAR scheme[32];
	cef_browser_t *browser = GetPropW(window, browser_property);
	if (state != engine_running && (!scheme_of(address, scheme, ARRAYSIZE(scheme)) || !lstrcmpiW(scheme, L"about") || !start(scheme)))
		return FALSE;
	// The navigation the control asked for has had its BeforeNavigate2 already.
	SetPropW(window, expected_property, (HANDLE)((UINT_PTR)GetPropW(window, expected_property) + 1));
	if (browser) {
		load(browser, address);
		return TRUE;
	}
	if (GetPropW(window, pending_property)) {
		replace_pending(window, address);
		return TRUE;
	}
	replace_pending(window, L"");
	RECT area;
	GetClientRect(window, &area);
	cef_window_info_t info = {sizeof(info)};
	info.style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VISIBLE;
	info.parent_window = window;
	info.bounds.width = area.right;
	info.bounds.height = area.bottom;
	info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
	cef_browser_settings_t settings = {sizeof(settings)};
	cef_string_t url = {0}, key = {0};
	engine_set_text(&url, address);
	// Window handles keep to 32 bits, so the control's window fits the dictionary's integers.
	cef_dictionary_value_t *extra = cef.dictionary();
	if (extra) {
		engine_set_text(&key, ENGINE_WINDOW_KEY);
		extra->set_int(extra, &key, (int)(INT_PTR)window);
		cef.string_clear(&key);
	}
	const int created = cef.create_browser(&info, &client, &url, &settings, extra, NULL);
	cef.string_clear(&url);
	if (!created) {
		engine_trace("page engine has no browser for", address);
		replace_pending(window, NULL);
		RemovePropW(window, expected_property);
		return FALSE;
	}
	return TRUE;
}

WEBENGINE_API BOOL engine_execute(HWND window, const WCHAR *script)
{
	cef_browser_t *browser = GetPropW(window, browser_property);
	cef_frame_t *frame = browser ? browser->get_main_frame(browser) : NULL;
	if (!frame)
		return FALSE;
	cef_string_t code = {0}, address = {0};
	engine_set_text(&code, script);
	frame->execute_java_script(frame, &code, &address, 0);
	cef.string_clear(&code);
	engine_release(frame);
	return TRUE;
}

WEBENGINE_API void engine_resize(HWND window)
{
	cef_browser_t *browser = GetPropW(window, browser_property);
	if (!browser)
		return;
	cef_browser_host_t *host = browser->get_host(browser);
	if (!host)
		return;
	HWND child = host->get_window_handle(host);
	RECT area;
	GetClientRect(window, &area);
	if (child)
		SetWindowPos(child, NULL, 0, 0, area.right, area.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
	host->was_resized(host);
	engine_release(host);
}

WEBENGINE_API void engine_close(HWND window)
{
	cef_browser_t *browser = RemovePropW(window, browser_property);
	replace_pending(window, NULL);
	RemovePropW(window, expected_property);
	if (!browser)
		return;
	cef_browser_host_t *host = browser->get_host(browser);
	if (host) {
		host->close_browser(host, 1);
		engine_release(host);
	}
	engine_release(browser);
}

WEBENGINE_API BOOL engine_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
	switch (message) {
	case ENGINE_ATTACHED: {
		cef_browser_t *browser = (cef_browser_t *)lparam;
		if (GetPropW(window, browser_property) || !SetPropW(window, browser_property, browser)) {
			engine_release(browser);
			return TRUE;
		}
		engine_resize(window);
		WCHAR *pending = GetPropW(window, pending_property);
		if (pending && *pending)
			load(browser, pending);
		replace_pending(window, NULL);
		return TRUE;
	}
	case ENGINE_FETCH:
		serve_in_turn((fetch *)lparam);
		return TRUE;
	case ENGINE_DISPATCH:
		external_dispatch(wparam, lparam);
		return TRUE;
	case ENGINE_BEFORE: {
		// Internet Explorer asks nobody about a server's redirect.
		const UINT_PTR expected = (UINT_PTR)GetPropW(window, expected_property);
		if (wparam) {
			*result = FALSE;
			return TRUE;
		}
		if (expected) {
			if (expected > 1)
				SetPropW(window, expected_property, (HANDLE)(expected - 1));
			else
				RemovePropW(window, expected_property);
			*result = FALSE;
			return TRUE;
		}
		return FALSE;
	}
	case WM_SIZE:
		engine_resize(window);
		return FALSE;
	default:
		return FALSE;
	}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
