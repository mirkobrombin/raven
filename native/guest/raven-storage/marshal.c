/* Objects across apartments and processes: the marshaled form follows the OBJREF layout of [MS-DCOM], the
   calls themselves travel over a named pipe of the serving process with a framing of this component. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <ocidl.h>
#include <shobjidl.h>

#define OBJREF_SIGNATURE 0x574f454d
#define OBJREF_STANDARD 1
#define OBJREF_CUSTOM 4
#define WM_RAVEN_CALL (WM_USER + 0x4d4)

enum { OP_CALL, OP_QUERY, OP_RELEASE, OP_CREATE, OP_METHOD, OP_ADDREF };

typedef struct {
	DWORD size, op, method, representation;
	HRESULT status;
	GUID ipid, iid;
} frame_head;

typedef struct {
	DWORD signature, flags;
	GUID iid;
	DWORD std_flags, public_refs;
	ULONGLONG oxid, oid;
	GUID ipid;
	USHORT entries, security;
} objref_standard;

typedef struct {
	DWORD signature, flags;
	GUID iid, clsid;
	DWORD extension, size;
} objref_custom;

typedef struct export_entry {
	struct export_entry *next;
	IUnknown *object, *identity;
	IRpcStubBuffer *stub;
	GUID ipid, iid;
	HWND window;
	DWORD thread;
	LONG refs;
} export_entry;

static CRITICAL_SECTION lock;
static LONG lock_state;
static export_entry *exports;
static LONG listening;
static ATOM window_class;
static DWORD context_index = TLS_OUT_OF_INDEXES;

static void lock_ready(void)
{
	if (InterlockedCompareExchange(&lock_state, 1, 0) == 0) {
		InitializeCriticalSection(&lock);
		context_index = TlsAlloc();
		InterlockedExchange(&lock_state, 2);
	}
	while (lock_state != 2)
		Sleep(0);
}

static void *take(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}

static void give(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

static void pipe_name(WCHAR *name, DWORD process)
{
	wsprintfW(name, L"\\\\.\\pipe\\raven-com-%lu", process);
}

/* System interfaces have their marshaling in system libraries that do not exist here, so a table describes them.
   One letter per argument, in order:
   d  a value of pointer size or less, window handles included
   r  [in] RECT    m  [in] MSG    f  [in] LOGFONTW    w  [in] text
   o  [out] a value of pointer size or less
   S  [in] IStream    U  [in] IUnknown    Q  [in] IID followed by [out, iid_is] pointer */
typedef struct {
	const IID *iid;
	const char *methods[8];
} interface_shape;

static const IID shape_preview = {0x8895b1c6, 0xb41f, 0x4c1c, {0xa5, 0x62, 0x0d, 0x56, 0x42, 0x50, 0x83, 0x6f}};
static const IID shape_visuals = {0x196bf9a5, 0xb346, 0x4ef0, {0xaa, 0x1e, 0x5d, 0xcd, 0xb7, 0x67, 0x68, 0xb1}};
static const IID shape_stream_init = {0xb824b49d, 0x22ac, 0x4161, {0xac, 0x8a, 0x99, 0x16, 0xe8, 0xfa, 0x3f, 0x7f}};
static const IID shape_file_init = {0xb7d14566, 0x0509, 0x4cce, {0xa7, 0x1f, 0x0a, 0x55, 0x42, 0x33, 0xbd, 0x9b}};

static const interface_shape shapes[] = {
	{ &shape_preview, { "dr", "r", "", "", "", "o", "m" } },
	{ &shape_visuals, { "d", "f", "d" } },
	{ &shape_stream_init, { "Sd" } },
	{ &shape_file_init, { "wd" } },
	{ &IID_IObjectWithSite, { "U", "Q" } },
	{ &IID_IOleWindow, { "o", "d" } },
};

static const interface_shape *shape_of(REFIID iid)
{
	for (unsigned i = 0; i < ARRAYSIZE(shapes); i++)
		if (IsEqualIID(iid, shapes[i].iid))
			return &shapes[i];
	return NULL;
}

/* Interface proxies and stubs come from the factory the registry names for the interface. */
static HRESULT ps_factory(REFIID iid, IPSFactoryBuffer **out)
{
	WCHAR key[128], text[64], path[MAX_PATH], guid[40];
	StringFromGUID2(iid, guid, 40);
	wsprintfW(key, L"Interface\\%s\\ProxyStubClsid32", guid);
	LONG size = sizeof text;
	if (RegQueryValueW(HKEY_CLASSES_ROOT, key, text, &size))
		return REGDB_E_IIDNOTREG;
	wsprintfW(key, L"CLSID\\%s\\InprocServer32", text);
	size = sizeof path;
	if (RegQueryValueW(HKEY_CLASSES_ROOT, key, path, &size))
		return REGDB_E_CLASSNOTREG;
	HMODULE module = LoadLibraryW(path);
	if (!module)
		return HRESULT_FROM_WIN32(GetLastError());
	HRESULT (WINAPI *get)(REFCLSID, REFIID, void **) = (void *)GetProcAddress(module, "DllGetClassObject");
	CLSID clsid;
	if (!get || CLSIDFromString(text, &clsid))
		return REGDB_E_CLASSNOTREG;
	return get(&clsid, &IID_IPSFactoryBuffer, (void **)out);
}

static BOOL builtin_interface(REFIID iid)
{
	return IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory) || shape_of(iid);
}

HRESULT WINAPI CoGetApartmentType(APTTYPE *type, APTTYPEQUALIFIER *qualifier);
static HRESULT marshal_to_memory(IUnknown *object, REFIID iid, void **data, DWORD *size);
static LRESULT CALLBACK call_window(HWND window, UINT message, WPARAM wide, LPARAM data);

/* The apartment's window: calls for a single threaded apartment run on its thread, between its messages. */
static HWND apartment_window(void)
{
	APTTYPE type;
	APTTYPEQUALIFIER qualifier;
	if (FAILED(CoGetApartmentType(&type, &qualifier)) || type == APTTYPE_MTA)
		return NULL;
	if (!window_class) {
		WNDCLASSW info = {0};
		info.lpfnWndProc = call_window;
		info.lpszClassName = L"OleMainThreadWndClass";
		info.hInstance = GetModuleHandleW(NULL);
		window_class = RegisterClassW(&info);
	}
	for (export_entry *entry = exports; entry; entry = entry->next)
		if (entry->thread == GetCurrentThreadId() && entry->window)
			return entry->window;
	return CreateWindowExW(0, L"OleMainThreadWndClass", L"OleMainThreadWndName", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
}

static DWORD WINAPI listen_thread(void *unused);

static HRESULT export_interface(IUnknown *object, REFIID iid, GUID *ipid)
{
	IUnknown *identity = NULL, *wanted = NULL;
	HRESULT hr = IUnknown_QueryInterface(object, &IID_IUnknown, (void **)&identity);
	if (FAILED(hr))
		return hr;
	hr = IUnknown_QueryInterface(object, iid, (void **)&wanted);
	if (FAILED(hr)) {
		IUnknown_Release(identity);
		return hr;
	}
	lock_ready();
	EnterCriticalSection(&lock);
	for (export_entry *entry = exports; entry; entry = entry->next)
		if (entry->identity == identity && IsEqualIID(&entry->iid, iid) && entry->thread == GetCurrentThreadId()) {
			entry->refs++;
			*ipid = entry->ipid;
			LeaveCriticalSection(&lock);
			IUnknown_Release(wanted);
			IUnknown_Release(identity);
			return S_OK;
		}
	IRpcStubBuffer *stub = NULL;
	if (!builtin_interface(iid)) {
		IPSFactoryBuffer *factory;
		hr = ps_factory(iid, &factory);
		if (SUCCEEDED(hr)) {
			hr = IPSFactoryBuffer_CreateStub(factory, iid, wanted, &stub);
			IPSFactoryBuffer_Release(factory);
		}
		if (FAILED(hr)) {
			LeaveCriticalSection(&lock);
			IUnknown_Release(wanted);
			IUnknown_Release(identity);
			return hr;
		}
	}
	export_entry *entry = take(sizeof *entry);
	entry->object = wanted;
	entry->identity = identity;
	entry->stub = stub;
	entry->iid = *iid;
	entry->thread = GetCurrentThreadId();
	entry->window = apartment_window();
	entry->refs = 1;
	CoCreateGuid(&entry->ipid);
	entry->next = exports;
	exports = entry;
	*ipid = entry->ipid;
	LeaveCriticalSection(&lock);
	if (InterlockedCompareExchange(&listening, 1, 0) == 0)
		CloseHandle(CreateThread(NULL, 0, listen_thread, NULL, 0, NULL));
	return S_OK;
}

static export_entry *export_find(const GUID *ipid)
{
	for (export_entry *entry = exports; entry; entry = entry->next)
		if (IsEqualGUID(&entry->ipid, ipid))
			return entry;
	return NULL;
}

static void export_release(const GUID *ipid)
{
	lock_ready();
	EnterCriticalSection(&lock);
	for (export_entry **link = &exports; *link; link = &(*link)->next) {
		export_entry *entry = *link;
		if (!IsEqualGUID(&entry->ipid, ipid))
			continue;
		if (--entry->refs > 0)
			break;
		*link = entry->next;
		LeaveCriticalSection(&lock);
		if (entry->stub) {
			IRpcStubBuffer_Disconnect(entry->stub);
			IRpcStubBuffer_Release(entry->stub);
		}
		IUnknown_Release(entry->object);
		IUnknown_Release(entry->identity);
		give(entry);
		return;
	}
	LeaveCriticalSection(&lock);
}

/* Both directions of a channel: the client's sends a frame and waits; the server's only hands out the reply buffer. */
typedef struct {
	IRpcChannelBuffer iface;
	LONG refs;
	DWORD process;
	GUID ipid, iid;
} channel_object;

static inline channel_object *channel_of(IRpcChannelBuffer *iface)
{
	return CONTAINING_RECORD(iface, channel_object, iface);
}

static HRESULT WINAPI channel_query(IRpcChannelBuffer *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IRpcChannelBuffer)) {
		*out = iface;
		IRpcChannelBuffer_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI channel_addref(IRpcChannelBuffer *iface)
{
	return InterlockedIncrement(&channel_of(iface)->refs);
}

static ULONG WINAPI channel_release(IRpcChannelBuffer *iface)
{
	const LONG left = InterlockedDecrement(&channel_of(iface)->refs);
	if (!left)
		give(channel_of(iface));
	return left;
}

static HRESULT WINAPI channel_get_buffer(IRpcChannelBuffer *iface, RPCOLEMESSAGE *message, REFIID iid)
{
	(void)iface, (void)iid;
	message->Buffer = take(message->cbBuffer);
	return message->Buffer ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI channel_free_buffer(IRpcChannelBuffer *iface, RPCOLEMESSAGE *message)
{
	(void)iface;
	give(message->Buffer);
	message->Buffer = NULL;
	return S_OK;
}

static HRESULT WINAPI channel_get_context(IRpcChannelBuffer *iface, DWORD *context, void **data)
{
	(void)iface;
	*context = MSHCTX_LOCAL;
	if (data)
		*data = NULL;
	return S_OK;
}

static HRESULT WINAPI channel_is_connected(IRpcChannelBuffer *iface)
{
	(void)iface;
	return S_OK;
}

/* Reads and writes on a connection the client opened overlapped: while one waits, calls for this thread's own
   objects keep running, or two apartments calling each other would never finish. */
static BOOL transfer(HANDLE pipe, void *data, DWORD size, BOOL writing)
{
	DWORD done = 0;
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	BOOL result = TRUE;
	while (result && done < size) {
		OVERLAPPED overlapped = {0};
		overlapped.hEvent = event;
		ResetEvent(event);
		DWORD moved = 0;
		BOOL ok = writing ? WriteFile(pipe, (BYTE *)data + done, size - done, NULL, &overlapped) : ReadFile(pipe, (BYTE *)data + done, size - done, NULL, &overlapped);
		if (!ok && GetLastError() != ERROR_IO_PENDING) {
			result = FALSE;
			break;
		}
		for (;;) {
			const DWORD waited = MsgWaitForMultipleObjects(1, &event, FALSE, INFINITE, QS_POSTMESSAGE | QS_SENDMESSAGE);
			if (waited == WAIT_OBJECT_0)
				break;
			MSG message;
			while (PeekMessageW(&message, NULL, WM_RAVEN_CALL, WM_RAVEN_CALL, PM_REMOVE))
				DispatchMessageW(&message);
		}
		if (!GetOverlappedResult(pipe, &overlapped, &moved, FALSE) || !moved)
			result = FALSE;
		done += moved;
	}
	CloseHandle(event);
	return result;
}

/* One exchange. A connection serves one call, so a call made while another waits never shares it. */
static HRESULT exchange(DWORD process, frame_head *head, const void *payload, void **reply)
{
	WCHAR name[64];
	pipe_name(name, process);
	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 50 && pipe == INVALID_HANDLE_VALUE; attempt++) {
		pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (pipe == INVALID_HANDLE_VALUE)
			Sleep(20);
	}
	if (pipe == INVALID_HANDLE_VALUE)
		return RPC_E_SERVER_DIED;
	HRESULT hr = RPC_E_DISCONNECTED;
	if (transfer(pipe, head, sizeof *head, TRUE) && (!head->size || transfer(pipe, (void *)payload, head->size, TRUE)) && transfer(pipe, head, sizeof *head, FALSE)) {
		void *data = take(head->size);
		if (!head->size || transfer(pipe, data, head->size, FALSE)) {
			*reply = data;
			hr = S_OK;
		} else
			give(data);
	}
	CloseHandle(pipe);
	return hr;
}

static HRESULT WINAPI channel_send_receive(IRpcChannelBuffer *iface, RPCOLEMESSAGE *message, ULONG *status)
{
	channel_object *channel = channel_of(iface);
	frame_head head = {0};
	head.size = message->cbBuffer;
	head.op = OP_CALL;
	head.method = message->iMethod;
	head.representation = message->dataRepresentation;
	head.ipid = channel->ipid;
	head.iid = channel->iid;
	void *reply = NULL;
	const HRESULT hr = exchange(channel->process, &head, message->Buffer, &reply);
	if (status)
		*status = 0;
	if (FAILED(hr))
		return hr;
	give(message->Buffer);
	message->Buffer = reply;
	message->cbBuffer = head.size;
	message->dataRepresentation = head.representation;
	if (FAILED(head.status)) {
		if (status)
			*status = head.status;
		return RPC_E_SERVERFAULT;
	}
	return S_OK;
}

static const IRpcChannelBufferVtbl channel_table = {
	channel_query, channel_addref, channel_release, channel_get_buffer, channel_send_receive,
	channel_free_buffer, channel_get_context, channel_is_connected,
};

static channel_object *channel_create(DWORD process, const GUID *ipid, const GUID *iid)
{
	channel_object *channel = take(sizeof *channel);
	channel->iface.lpVtbl = &channel_table;
	channel->refs = 1;
	channel->process = process;
	if (ipid)
		channel->ipid = *ipid;
	if (iid)
		channel->iid = *iid;
	return channel;
}

/* The serving side. */
typedef struct {
	frame_head head;
	void *payload, *reply;
	HANDLE done;
	DWORD caller;
} pending_call;

/* The call a thread serves for another apartment, which CoGetCallContext describes: its object answers for the client. The client
 * runs as the same user here, so impersonating it changes nothing. */
typedef struct {
	IServerSecurity iface;
	LONG refs;
	DWORD caller;
	BOOL impersonating;
} call_context;


static call_context *current_call(void)
{
	return context_index == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue(context_index);
}

static HRESULT WINAPI context_query(IServerSecurity *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IServerSecurity)) {
		*out = iface;
		IServerSecurity_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI context_addref(IServerSecurity *iface)
{
	return InterlockedIncrement(&CONTAINING_RECORD(iface, call_context, iface)->refs);
}

static ULONG WINAPI context_release(IServerSecurity *iface)
{
	call_context *context = CONTAINING_RECORD(iface, call_context, iface);
	const LONG left = InterlockedDecrement(&context->refs);
	if (!left)
		give(context);
	return left;
}

/* Calls between processes of one user are authenticated by the system at packet privacy, the client asking to be identified. */
static HRESULT WINAPI context_blanket(IServerSecurity *iface, DWORD *authentication, DWORD *authorization, OLECHAR **principal, DWORD *level,
	DWORD *impersonation, void **privileges, DWORD *capabilities)
{
	(void)iface;
	if (authentication)
		*authentication = RPC_C_AUTHN_WINNT;
	if (authorization)
		*authorization = RPC_C_AUTHZ_NONE;
	if (principal)
		*principal = NULL;
	if (level)
		*level = RPC_C_AUTHN_LEVEL_PKT_PRIVACY;
	if (impersonation)
		*impersonation = RPC_C_IMP_LEVEL_IDENTIFY;
	if (privileges)
		*privileges = NULL;
	if (capabilities)
		*capabilities = EOAC_NONE;
	return S_OK;
}

static HRESULT WINAPI context_impersonate(IServerSecurity *iface)
{
	CONTAINING_RECORD(iface, call_context, iface)->impersonating = TRUE;
	return S_OK;
}

static HRESULT WINAPI context_revert(IServerSecurity *iface)
{
	CONTAINING_RECORD(iface, call_context, iface)->impersonating = FALSE;
	return S_OK;
}

static BOOL WINAPI context_impersonating(IServerSecurity *iface)
{
	return CONTAINING_RECORD(iface, call_context, iface)->impersonating;
}

static const IServerSecurityVtbl context_table = {context_query, context_addref, context_release, context_blanket, context_impersonate,
	context_revert, context_impersonating};

HRESULT WINAPI CoGetCallContext(REFIID iid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	call_context *context = current_call();
	return context ? context_query(&context->iface, iid, out) : RPC_E_CALL_COMPLETE;
}

HRESULT WINAPI CoImpersonateClient(void)
{
	call_context *context = current_call();
	return context ? context_impersonate(&context->iface) : RPC_E_CALL_COMPLETE;
}

HRESULT WINAPI CoRevertToSelf(void)
{
	call_context *context = current_call();
	return context ? context_revert(&context->iface) : RPC_E_CALL_COMPLETE;
}

HRESULT WINAPI CoQueryClientBlanket(DWORD *authentication, DWORD *authorization, OLECHAR **principal, DWORD *level, DWORD *impersonation,
	void **privileges, DWORD *capabilities)
{
	call_context *context = current_call();
	return context ? context_blanket(&context->iface, authentication, authorization, principal, level, impersonation, privileges, capabilities)
		: RPC_E_CALL_COMPLETE;
}

static HRESULT marshal_to_memory(IUnknown *object, REFIID iid, void **data, DWORD *size)
{
	IStream *stream;
	HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
	if (FAILED(hr))
		return hr;
	hr = CoMarshalInterface(stream, iid, object, MSHCTX_LOCAL, NULL, MSHLFLAGS_NORMAL);
	if (SUCCEEDED(hr)) {
		HGLOBAL global;
		GetHGlobalFromStream(stream, &global);
		STATSTG info;
		IStream_Stat(stream, &info, STATFLAG_NONAME);
		*size = info.cbSize.LowPart;
		*data = take(*size);
		CopyMemory(*data, GlobalLock(global), *size);
		GlobalUnlock(global);
	}
	IStream_Release(stream);
	return hr;
}

/* Calling a vtable slot with arguments of pointer size works the same on both architectures. */
static HRESULT call_slot(void *object, unsigned slot, const ULONG_PTR *a, unsigned count)
{
	void **table = *(void ***)object;
	void *target = table[slot];
	switch (count) {
	case 0: return ((HRESULT (STDMETHODCALLTYPE *)(void *))target)(object);
	case 1: return ((HRESULT (STDMETHODCALLTYPE *)(void *, ULONG_PTR))target)(object, a[0]);
	case 2: return ((HRESULT (STDMETHODCALLTYPE *)(void *, ULONG_PTR, ULONG_PTR))target)(object, a[0], a[1]);
	case 3: return ((HRESULT (STDMETHODCALLTYPE *)(void *, ULONG_PTR, ULONG_PTR, ULONG_PTR))target)(object, a[0], a[1], a[2]);
	}
	return E_NOTIMPL;
}

static HRESULT unmarshal_bytes(const void *data, DWORD size, REFIID iid, void **out)
{
	IStream *stream;
	HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
	if (FAILED(hr))
		return hr;
	LARGE_INTEGER start = {0};
	IStream_Write(stream, data, size, NULL);
	IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
	hr = CoUnmarshalInterface(stream, iid, out);
	IStream_Release(stream);
	return hr;
}

/* Arguments are laid out one after another, each counted block with its size first. */
static HRESULT serve_method(export_entry *entry, DWORD method, const BYTE *in, DWORD in_size, void **reply, DWORD *reply_size)
{
	const interface_shape *shape = shape_of(&entry->iid);
	if (!shape || method >= ARRAYSIZE(shape->methods) || !shape->methods[method])
		return RPC_E_INVALID_DATA;
	const char *signature = shape->methods[method];
	ULONG_PTR a[4] = {0}, out_values[4] = {0};
	IUnknown *held[4] = {0};
	void *out_pointer = NULL;
	IID out_iid;
	BOOL pointer_out = FALSE;
	unsigned count = 0, position = 0;
	const BYTE *at = in, *end = in + in_size;
	for (const char *kind = signature; *kind; kind++, position++) {
		if (*kind == 'o') {
			a[count++] = (ULONG_PTR)&out_values[position];
			continue;
		}
		if ((SIZE_T)(end - at) < sizeof(DWORD))
			return RPC_E_INVALID_DATA;
		const DWORD size = *(const DWORD *)at;
		at += sizeof(DWORD);
		if ((SIZE_T)(end - at) < size)
			return RPC_E_INVALID_DATA;
		switch (*kind) {
		case 'd':
			a[count++] = *(const ULONG_PTR *)at;
			break;
		case 'S':
		case 'U':
			if (size) {
				const HRESULT hr = unmarshal_bytes(at, size, *kind == 'S' ? &IID_IStream : &IID_IUnknown, (void **)&held[position]);
				if (FAILED(hr))
					return hr;
			}
			a[count++] = (ULONG_PTR)held[position];
			break;
		case 'Q':
			out_iid = *(const IID *)at;
			a[count++] = (ULONG_PTR)&out_iid;
			a[count++] = (ULONG_PTR)&out_pointer;
			pointer_out = TRUE;
			break;
		default:
			a[count++] = size ? (ULONG_PTR)at : 0;
			break;
		}
		at += size;
	}
	const HRESULT status = call_slot(entry->object, method + 3, a, count);
	for (unsigned i = 0; i < 4; i++)
		if (held[i])
			IUnknown_Release(held[i]);
	void *marshaled = NULL;
	DWORD marshaled_size = 0;
	if (SUCCEEDED(status) && pointer_out && out_pointer) {
		marshal_to_memory(out_pointer, &out_iid, &marshaled, &marshaled_size);
		IUnknown_Release((IUnknown *)out_pointer);
	}
	*reply_size = sizeof out_values + marshaled_size;
	*reply = take(*reply_size);
	CopyMemory(*reply, out_values, sizeof out_values);
	if (marshaled) {
		CopyMemory((BYTE *)*reply + sizeof out_values, marshaled, marshaled_size);
		give(marshaled);
	}
	return status;
}

static void serve_call_in_context(pending_call *call);

/* A call runs with its context in effect; one served while another waits nests inside it. */
static void serve_call(pending_call *call)
{
	lock_ready();
	call_context *context = take(sizeof *context);
	context->iface.lpVtbl = (IServerSecurityVtbl *)&context_table;
	context->refs = 1;
	context->caller = call->caller;
	call_context *outer = current_call();
	TlsSetValue(context_index, context);
	serve_call_in_context(call);
	TlsSetValue(context_index, outer);
	context_release(&context->iface);
}

static void serve_call_in_context(pending_call *call)
{
	frame_head *head = &call->head;
	lock_ready();
	EnterCriticalSection(&lock);
	export_entry *entry = export_find(&head->ipid);
	LeaveCriticalSection(&lock);
	void *reply = NULL;
	DWORD reply_size = 0;
	HRESULT status = RPC_E_DISCONNECTED;
	if (entry && head->op == OP_QUERY) {
		GUID ipid;
		status = export_interface(entry->object, (const IID *)call->payload, &ipid);
		if (SUCCEEDED(status)) {
			reply = take(sizeof ipid);
			*(GUID *)reply = ipid;
			reply_size = sizeof ipid;
		}
	} else if (entry && head->op == OP_ADDREF) {
		InterlockedIncrement(&entry->refs);
		status = S_OK;
	} else if (entry && head->op == OP_RELEASE) {
		export_release(&head->ipid);
		status = S_OK;
	} else if (entry && head->op == OP_CREATE) {
		IUnknown *made = NULL;
		const IID *wanted = call->payload;
		status = IClassFactory_CreateInstance((IClassFactory *)entry->object, NULL, wanted, (void **)&made);
		if (SUCCEEDED(status)) {
			status = marshal_to_memory(made, wanted, &reply, &reply_size);
			IUnknown_Release(made);
		}
	} else if (entry && head->op == OP_METHOD) {
		status = serve_method(entry, head->method, call->payload, head->size, &reply, &reply_size);
	} else if (entry && entry->stub) {
		RPCOLEMESSAGE message = {0};
		message.Buffer = call->payload;
		message.cbBuffer = head->size;
		message.iMethod = head->method;
		message.dataRepresentation = head->representation;
		channel_object *channel = channel_create(GetCurrentProcessId(), &head->ipid, &head->iid);
		status = IRpcStubBuffer_Invoke(entry->stub, &message, &channel->iface);
		IRpcChannelBuffer_Release(&channel->iface);
		if (SUCCEEDED(status)) {
			reply = message.Buffer;
			reply_size = message.cbBuffer;
			head->representation = message.dataRepresentation;
			if (reply == call->payload) {
				reply = NULL;
				reply_size = 0;
			}
		}
	}
	head->status = status;
	head->size = reply_size;
	call->reply = reply;
}

static LRESULT CALLBACK call_window(HWND window, UINT message, WPARAM wide, LPARAM data)
{
	if (message == WM_RAVEN_CALL) {
		pending_call *call = (pending_call *)data;
		serve_call(call);
		SetEvent(call->done);
		return 0;
	}
	return DefWindowProcW(window, message, wide, data);
}

static BOOL pipe_read(HANDLE pipe, void *data, DWORD size)
{
	DWORD done = 0;
	while (done < size) {
		DWORD moved = 0;
		if (!ReadFile(pipe, (BYTE *)data + done, size - done, &moved, NULL) || !moved)
			return FALSE;
		done += moved;
	}
	return TRUE;
}

static BOOL pipe_write(HANDLE pipe, const void *data, DWORD size)
{
	DWORD moved = 0;
	return !size || (WriteFile(pipe, data, size, &moved, NULL) && moved == size);
}

static DWORD WINAPI connection_thread(void *argument)
{
	HANDLE pipe = argument;
	pending_call call = {0};
	GetNamedPipeClientProcessId(pipe, &call.caller);
	if (pipe_read(pipe, &call.head, sizeof call.head)) {
		call.payload = take(call.head.size);
		if (!call.head.size || pipe_read(pipe, call.payload, call.head.size)) {
			lock_ready();
			EnterCriticalSection(&lock);
			export_entry *entry = export_find(&call.head.ipid);
			HWND window = entry ? entry->window : NULL;
			LeaveCriticalSection(&lock);
			if (window) {
				call.done = CreateEventW(NULL, TRUE, FALSE, NULL);
				if (PostMessageW(window, WM_RAVEN_CALL, 0, (LPARAM)&call))
					WaitForSingleObject(call.done, INFINITE);
				else
					call.head.status = RPC_E_SERVER_DIED;
				CloseHandle(call.done);
			} else {
				CoInitializeEx(NULL, COINIT_MULTITHREADED);
				serve_call(&call);
				CoUninitialize();
			}
			if (pipe_write(pipe, &call.head, sizeof call.head))
				pipe_write(pipe, call.reply, call.head.size);
			FlushFileBuffers(pipe);
			give(call.reply);
		}
		give(call.payload);
	}
	DisconnectNamedPipe(pipe);
	CloseHandle(pipe);
	return 0;
}

static DWORD WINAPI listen_thread(void *unused)
{
	(void)unused;
	WCHAR name[64];
	pipe_name(name, GetCurrentProcessId());
	for (;;) {
		HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, NULL);
		if (pipe == INVALID_HANDLE_VALUE)
			return 1;
		if (ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
			HANDLE thread = CreateThread(NULL, 0, connection_thread, pipe, 0, NULL);
			if (thread)
				CloseHandle(thread);
			else
				CloseHandle(pipe);
		} else
			CloseHandle(pipe);
	}
}

/* The client side: one object per remote identity; each interface on it is a proxy with its own channel. */
typedef struct proxied {
	struct proxied *next;
	GUID ipid, iid;
	IUnknown *pointer;
	IRpcProxyBuffer *buffer;
	channel_object *channel;
} proxied;

typedef struct {
	IUnknown outer;
	LONG refs;
	DWORD process;
	proxied *interfaces;
	CRITICAL_SECTION guard;
} proxy_manager;

typedef struct {
	IClassFactory iface;
	proxy_manager *manager;
	GUID ipid;
} factory_proxy;

typedef struct {
	const void *const *table;
	proxy_manager *manager;
	const interface_shape *shape;
	GUID ipid;
} shape_proxy;

static inline proxy_manager *manager_of(IUnknown *iface)
{
	return CONTAINING_RECORD(iface, proxy_manager, outer);
}

static HRESULT manager_add(proxy_manager *manager, REFIID iid, const GUID *ipid, void **out);

static ULONG WINAPI manager_addref(IUnknown *iface)
{
	return InterlockedIncrement(&manager_of(iface)->refs);
}

static ULONG WINAPI manager_release(IUnknown *iface)
{
	proxy_manager *manager = manager_of(iface);
	const LONG left = InterlockedDecrement(&manager->refs);
	if (left)
		return left;
	manager->refs = 1 << 20;
	for (proxied *item = manager->interfaces; item;) {
		proxied *next = item->next;
		frame_head head = {0};
		head.op = OP_RELEASE;
		head.ipid = item->ipid;
		void *reply = NULL;
		if (SUCCEEDED(exchange(manager->process, &head, NULL, &reply)))
			give(reply);
		if (item->buffer) {
			IRpcProxyBuffer_Disconnect(item->buffer);
			IRpcProxyBuffer_Release(item->buffer);
		}
		if (item->channel)
			IRpcChannelBuffer_Release(&item->channel->iface);
		if (IsEqualIID(&item->iid, &IID_IClassFactory))
			give(item->pointer);
		else if (shape_of(&item->iid)) {
			give((void *)((shape_proxy *)item->pointer)->table);
			give(item->pointer);
		}
		give(item);
		item = next;
	}
	DeleteCriticalSection(&manager->guard);
	give(manager);
	return 0;
}

static HRESULT WINAPI manager_query(IUnknown *iface, REFIID iid, void **out)
{
	proxy_manager *manager = manager_of(iface);
	*out = NULL;
	if (IsEqualIID(iid, &IID_IUnknown)) {
		*out = iface;
		manager_addref(iface);
		return S_OK;
	}
	EnterCriticalSection(&manager->guard);
	for (proxied *item = manager->interfaces; item; item = item->next)
		if (IsEqualIID(&item->iid, iid)) {
			*out = item->pointer;
			manager_addref(iface);
			LeaveCriticalSection(&manager->guard);
			return S_OK;
		}
	const GUID *any = manager->interfaces ? &manager->interfaces->ipid : NULL;
	GUID known = {0};
	if (any)
		known = *any;
	LeaveCriticalSection(&manager->guard);
	if (!any)
		return E_NOINTERFACE;
	frame_head head = {0};
	head.op = OP_QUERY;
	head.size = sizeof(IID);
	head.ipid = known;
	void *reply = NULL;
	HRESULT hr = exchange(manager->process, &head, iid, &reply);
	if (FAILED(hr))
		return hr;
	if (FAILED(head.status)) {
		give(reply);
		return head.status;
	}
	hr = manager_add(manager, iid, (const GUID *)reply, out);
	give(reply);
	return hr;
}

static const IUnknownVtbl manager_table = { manager_query, manager_addref, manager_release };

static inline factory_proxy *factory_of(IClassFactory *iface)
{
	return CONTAINING_RECORD(iface, factory_proxy, iface);
}

static HRESULT WINAPI factory_query(IClassFactory *iface, REFIID iid, void **out)
{
	return manager_query(&factory_of(iface)->manager->outer, iid, out);
}

static ULONG WINAPI factory_addref(IClassFactory *iface)
{
	return manager_addref(&factory_of(iface)->manager->outer);
}

static ULONG WINAPI factory_release(IClassFactory *iface)
{
	return manager_release(&factory_of(iface)->manager->outer);
}

static HRESULT WINAPI factory_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	factory_proxy *factory = factory_of(iface);
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	frame_head head = {0};
	head.op = OP_CREATE;
	head.size = sizeof(IID);
	head.ipid = factory->ipid;
	void *reply = NULL;
	HRESULT hr = exchange(factory->manager->process, &head, iid, &reply);
	if (FAILED(hr))
		return hr;
	if (FAILED(head.status)) {
		give(reply);
		return head.status;
	}
	IStream *stream;
	hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
	if (SUCCEEDED(hr)) {
		LARGE_INTEGER start = {0};
		IStream_Write(stream, reply, head.size, NULL);
		IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
		hr = CoUnmarshalInterface(stream, iid, out);
		IStream_Release(stream);
	}
	give(reply);
	return hr;
}

static HRESULT WINAPI factory_lock(IClassFactory *iface, BOOL locking)
{
	(void)iface, (void)locking;
	return S_OK;
}

static const IClassFactoryVtbl factory_table = { factory_query, factory_addref, factory_release, factory_create, factory_lock };

/* A proxy built from a shape: every slot funnels into shape_call with its number and arguments. */
static void block_append(BYTE **data, DWORD *size, const void *block, DWORD length)
{
	BYTE *grown = take(*size + sizeof(DWORD) + length);
	if (*data)
		CopyMemory(grown, *data, *size);
	CopyMemory(grown + *size, &length, sizeof length);
	if (length)
		CopyMemory(grown + *size + sizeof length, block, length);
	give(*data);
	*data = grown;
	*size += sizeof length + length;
}

static HRESULT append_interface(BYTE **data, DWORD *size, IUnknown *object, REFIID iid)
{
	void *bytes = NULL;
	DWORD length = 0;
	if (object) {
		const HRESULT hr = marshal_to_memory(object, iid, &bytes, &length);
		if (FAILED(hr))
			return hr;
	}
	block_append(data, size, bytes, length);
	give(bytes);
	return S_OK;
}

static HRESULT shape_call(shape_proxy *proxy, unsigned slot, const ULONG_PTR *a)
{
	const char *signature = proxy->shape->methods[slot - 3];
	BYTE *payload = NULL;
	DWORD payload_size = 0;
	unsigned position = 0;
	HRESULT hr = S_OK;
	const ULONG_PTR *arg = a;
	for (const char *kind = signature; *kind && SUCCEEDED(hr); kind++, position++, arg++) {
		switch (*kind) {
		case 'd': block_append(&payload, &payload_size, arg, sizeof *arg); break;
		case 'r': block_append(&payload, &payload_size, (void *)*arg, *arg ? sizeof(RECT) : 0); break;
		case 'm': block_append(&payload, &payload_size, (void *)*arg, *arg ? sizeof(MSG) : 0); break;
		case 'f': block_append(&payload, &payload_size, (void *)*arg, *arg ? sizeof(LOGFONTW) : 0); break;
		case 'w': block_append(&payload, &payload_size, (void *)*arg, *arg ? (lstrlenW((const WCHAR *)*arg) + 1) * sizeof(WCHAR) : 0); break;
		case 'S': hr = append_interface(&payload, &payload_size, (IUnknown *)*arg, &IID_IStream); break;
		case 'U': hr = append_interface(&payload, &payload_size, (IUnknown *)*arg, &IID_IUnknown); break;
		case 'Q': block_append(&payload, &payload_size, (void *)*arg, sizeof(IID)); arg++; break;
		default: break;
		}
	}
	if (FAILED(hr)) {
		give(payload);
		return hr;
	}
	frame_head head = {0};
	head.op = OP_METHOD;
	head.method = slot - 3;
	head.size = payload_size;
	head.ipid = proxy->ipid;
	void *reply = NULL;
	hr = exchange(proxy->manager->process, &head, payload, &reply);
	give(payload);
	if (FAILED(hr))
		return hr;
	const ULONG_PTR *values = reply;
	arg = a;
	position = 0;
	for (const char *kind = signature; *kind; kind++, position++, arg++) {
		if (*kind == 'o' && *arg)
			*(ULONG_PTR *)*arg = head.size >= sizeof(ULONG_PTR) * 4 ? values[position] : 0;
		else if (*kind == 'Q') {
			const IID *iid = (const IID *)arg[0];
			void **out = (void **)arg[1];
			arg++;
			*out = NULL;
			if (SUCCEEDED(head.status) && head.size > sizeof(ULONG_PTR) * 4)
				hr = unmarshal_bytes((BYTE *)reply + sizeof(ULONG_PTR) * 4, head.size - sizeof(ULONG_PTR) * 4, iid, out);
		}
	}
	give(reply);
	return FAILED(hr) ? hr : head.status;
}

static HRESULT WINAPI shape_query(shape_proxy *proxy, REFIID iid, void **out)
{
	return manager_query(&proxy->manager->outer, iid, out);
}

static ULONG WINAPI shape_addref(shape_proxy *proxy)
{
	return manager_addref(&proxy->manager->outer);
}

static ULONG WINAPI shape_release(shape_proxy *proxy)
{
	return manager_release(&proxy->manager->outer);
}

#define SHAPE_SLOT(n) \
	static HRESULT WINAPI slot##n##_0(shape_proxy *p) { ULONG_PTR a[4] = {0}; return shape_call(p, n, a); } \
	static HRESULT WINAPI slot##n##_1(shape_proxy *p, ULONG_PTR x) { ULONG_PTR a[4] = {x}; return shape_call(p, n, a); } \
	static HRESULT WINAPI slot##n##_2(shape_proxy *p, ULONG_PTR x, ULONG_PTR y) { ULONG_PTR a[4] = {x, y}; return shape_call(p, n, a); }
SHAPE_SLOT(3) SHAPE_SLOT(4) SHAPE_SLOT(5) SHAPE_SLOT(6) SHAPE_SLOT(7) SHAPE_SLOT(8) SHAPE_SLOT(9)
#define SHAPE_ROW(n) { (void *)slot##n##_0, (void *)slot##n##_1, (void *)slot##n##_2 }
static void *const slot_functions[7][3] = { SHAPE_ROW(3), SHAPE_ROW(4), SHAPE_ROW(5), SHAPE_ROW(6), SHAPE_ROW(7), SHAPE_ROW(8), SHAPE_ROW(9) };

static shape_proxy *shape_proxy_create(proxy_manager *manager, const interface_shape *shape, const GUID *ipid)
{
	shape_proxy *proxy = take(sizeof *proxy);
	const void **table = take(sizeof(void *) * 10);
	table[0] = (void *)shape_query;
	table[1] = (void *)shape_addref;
	table[2] = (void *)shape_release;
	for (unsigned i = 0; i < ARRAYSIZE(shape->methods) - 1 && shape->methods[i]; i++) {
		unsigned arity = 0;
		for (const char *kind = shape->methods[i]; *kind; kind++)
			arity += *kind == 'Q' ? 2 : 1;
		table[i + 3] = slot_functions[i][arity];
	}
	proxy->table = table;
	proxy->manager = manager;
	proxy->shape = shape;
	proxy->ipid = *ipid;
	return proxy;
}

static HRESULT manager_add(proxy_manager *manager, REFIID iid, const GUID *ipid, void **out)
{
	proxied *item = take(sizeof *item);
	item->ipid = *ipid;
	item->iid = *iid;
	HRESULT hr = S_OK;
	BOOL counted = FALSE;
	if (IsEqualIID(iid, &IID_IUnknown))
		item->pointer = &manager->outer;
	else if (IsEqualIID(iid, &IID_IClassFactory)) {
		factory_proxy *factory = take(sizeof *factory);
		factory->iface.lpVtbl = &factory_table;
		factory->manager = manager;
		factory->ipid = *ipid;
		item->pointer = (IUnknown *)&factory->iface;
	} else if (shape_of(iid)) {
		item->pointer = (IUnknown *)shape_proxy_create(manager, shape_of(iid), ipid);
	} else {
		IPSFactoryBuffer *factory;
		hr = ps_factory(iid, &factory);
		if (SUCCEEDED(hr)) {
			void *pointer = NULL;
			hr = IPSFactoryBuffer_CreateProxy(factory, &manager->outer, iid, &item->buffer, &pointer);
			IPSFactoryBuffer_Release(factory);
			item->pointer = pointer;
			/* The interface a proxy hands back already holds a reference on the manager: it is the caller's. */
			counted = SUCCEEDED(hr) && pointer;
			if (SUCCEEDED(hr)) {
				item->channel = channel_create(manager->process, ipid, iid);
				hr = IRpcProxyBuffer_Connect(item->buffer, &item->channel->iface);
			}
		}
		if (FAILED(hr)) {
			if (counted)
				manager_release(&manager->outer);
			if (item->buffer)
				IRpcProxyBuffer_Release(item->buffer);
			if (item->channel)
				IRpcChannelBuffer_Release(&item->channel->iface);
			give(item);
			return hr;
		}
	}
	EnterCriticalSection(&manager->guard);
	item->next = manager->interfaces;
	manager->interfaces = item;
	LeaveCriticalSection(&manager->guard);
	*out = item->pointer;
	if (!counted)
		manager_addref(&manager->outer);
	return S_OK;
}

static HRESULT unmarshal_standard(const objref_standard *reference, REFIID iid, void **out)
{
	const DWORD process = (DWORD)reference->oxid;
	if (process == GetCurrentProcessId()) {
		lock_ready();
		EnterCriticalSection(&lock);
		export_entry *entry = export_find(&reference->ipid);
		/* The same apartment gets the object itself. */
		if (entry && entry->thread == GetCurrentThreadId()) {
			HRESULT hr = IUnknown_QueryInterface(entry->object, iid, out);
			LeaveCriticalSection(&lock);
			return hr;
		}
		LeaveCriticalSection(&lock);
	}
	/* Data marshaled for a table can be read many times and keeps its own reference: each reader needs one of its own. */
	if (reference->std_flags & (MSHLFLAGS_TABLESTRONG | MSHLFLAGS_TABLEWEAK)) {
		frame_head head = {0};
		head.op = OP_ADDREF;
		head.ipid = reference->ipid;
		void *reply = NULL;
		const HRESULT status = exchange(process, &head, NULL, &reply);
		give(reply);
		if (FAILED(status) || FAILED(head.status))
			return FAILED(status) ? status : head.status;
	}
	proxy_manager *manager = take(sizeof *manager);
	manager->outer.lpVtbl = &manager_table;
	manager->refs = 1;
	manager->process = process;
	InitializeCriticalSection(&manager->guard);
	void *first = NULL;
	HRESULT hr = manager_add(manager, &reference->iid, &reference->ipid, &first);
	if (SUCCEEDED(hr)) {
		manager_release(&manager->outer);
		hr = manager_query(&manager->outer, iid, out);
	}
	manager_release(&manager->outer);
	return hr;
}

HRESULT WINAPI CoMarshalInterface(IStream *stream, REFIID iid, IUnknown *object, DWORD context, void *context_data, DWORD flags)
{
	if (!stream || !object)
		return E_INVALIDARG;
	IMarshal *custom = NULL;
	if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IMarshal, (void **)&custom))) {
		objref_custom reference = {0};
		reference.signature = OBJREF_SIGNATURE;
		reference.flags = OBJREF_CUSTOM;
		reference.iid = *iid;
		HRESULT hr = IMarshal_GetUnmarshalClass(custom, iid, object, context, context_data, flags, &reference.clsid);
		if (SUCCEEDED(hr))
			hr = IMarshal_GetMarshalSizeMax(custom, iid, object, context, context_data, flags, &reference.size);
		if (SUCCEEDED(hr))
			hr = IStream_Write(stream, &reference, sizeof reference, NULL);
		if (SUCCEEDED(hr))
			hr = IMarshal_MarshalInterface(custom, stream, iid, object, context, context_data, flags);
		IMarshal_Release(custom);
		return hr;
	}
	objref_standard reference = {0};
	reference.signature = OBJREF_SIGNATURE;
	reference.flags = OBJREF_STANDARD;
	reference.iid = *iid;
	reference.public_refs = 1;
	reference.std_flags = flags;
	reference.oxid = GetCurrentProcessId();
	reference.oid = (ULONG_PTR)object;
	const HRESULT hr = export_interface(object, iid, &reference.ipid);
	return FAILED(hr) ? hr : IStream_Write(stream, &reference, sizeof reference, NULL);
}

HRESULT WINAPI CoUnmarshalInterface(IStream *stream, REFIID iid, void **out)
{
	if (!stream || !out)
		return E_INVALIDARG;
	*out = NULL;
	DWORD start[2];
	ULONG got = 0;
	HRESULT hr = IStream_Read(stream, start, sizeof start, &got);
	if (FAILED(hr) || got != sizeof start || start[0] != OBJREF_SIGNATURE)
		return RPC_E_INVALID_OBJREF;
	const IID *wanted = iid;
	if (start[1] == OBJREF_STANDARD) {
		objref_standard reference;
		reference.signature = start[0];
		reference.flags = start[1];
		hr = IStream_Read(stream, &reference.iid, sizeof reference - sizeof start, &got);
		if (FAILED(hr) || got != sizeof reference - sizeof start)
			return RPC_E_INVALID_OBJREF;
		if (IsEqualIID(wanted, &IID_NULL))
			wanted = &reference.iid;
		hr = unmarshal_standard(&reference, wanted, out);
		return hr;
	}
	if (start[1] == OBJREF_CUSTOM) {
		objref_custom reference;
		hr = IStream_Read(stream, &reference.iid, sizeof reference - sizeof start, &got);
		if (FAILED(hr) || got != sizeof reference - sizeof start)
			return RPC_E_INVALID_OBJREF;
		if (IsEqualIID(wanted, &IID_NULL))
			wanted = &reference.iid;
		IMarshal *custom;
		hr = CoCreateInstance(&reference.clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMarshal, (void **)&custom);
		if (FAILED(hr))
			return hr;
		hr = IMarshal_UnmarshalInterface(custom, stream, wanted, out);
		IMarshal_Release(custom);
		return hr;
	}
	return RPC_E_INVALID_OBJREF;
}

HRESULT WINAPI CoGetMarshalSizeMax(DWORD *size, REFIID iid, IUnknown *object, DWORD context, void *context_data, DWORD flags)
{
	if (!size || !object)
		return E_INVALIDARG;
	*size = sizeof(objref_standard);
	IMarshal *custom = NULL;
	if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IMarshal, (void **)&custom))) {
		DWORD inner = 0;
		const HRESULT hr = IMarshal_GetMarshalSizeMax(custom, iid, object, context, context_data, flags, &inner);
		IMarshal_Release(custom);
		if (FAILED(hr))
			return hr;
		*size = sizeof(objref_custom) + inner;
	}
	return S_OK;
}

HRESULT WINAPI CoReleaseMarshalData(IStream *stream)
{
	objref_standard reference;
	ULONG got = 0;
	const HRESULT hr = IStream_Read(stream, &reference, sizeof reference, &got);
	if (FAILED(hr) || got != sizeof reference || reference.signature != OBJREF_SIGNATURE)
		return RPC_E_INVALID_OBJREF;
	if (reference.flags == OBJREF_STANDARD && (DWORD)reference.oxid == GetCurrentProcessId())
		export_release(&reference.ipid);
	return S_OK;
}

HRESULT WINAPI CoMarshalInterThreadInterfaceInStream(REFIID iid, IUnknown *object, IStream **out)
{
	if (!out)
		return E_INVALIDARG;
	HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, out);
	if (FAILED(hr))
		return hr;
	hr = CoMarshalInterface(*out, iid, object, MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL);
	if (FAILED(hr)) {
		IStream_Release(*out);
		*out = NULL;
		return hr;
	}
	LARGE_INTEGER start = {0};
	return IStream_Seek(*out, start, STREAM_SEEK_SET, NULL);
}

HRESULT WINAPI CoGetInterfaceAndReleaseStream(IStream *stream, REFIID iid, void **out)
{
	if (!stream)
		return E_INVALIDARG;
	const HRESULT hr = CoUnmarshalInterface(stream, iid, out);
	IStream_Release(stream);
	return hr;
}

/* Everything this apartment still offers for an object stops answering. */
HRESULT WINAPI CoDisconnectObject(IUnknown *object, DWORD reserved)
{
	(void)reserved;
	if (!object)
		return E_INVALIDARG;
	IUnknown *identity = NULL;
	const HRESULT hr = IUnknown_QueryInterface(object, &IID_IUnknown, (void **)&identity);
	if (FAILED(hr))
		return hr;
	lock_ready();
	for (;;) {
		GUID ipid;
		BOOL found = FALSE;
		EnterCriticalSection(&lock);
		for (export_entry *entry = exports; entry; entry = entry->next)
			if (entry->identity == identity) {
				entry->refs = 1;
				ipid = entry->ipid;
				found = TRUE;
				break;
			}
		LeaveCriticalSection(&lock);
		if (!found)
			break;
		export_release(&ipid);
	}
	IUnknown_Release(identity);
	return S_OK;
}

/* Libraries stay loaded here until the process ends. */
void WINAPI CoFreeUnusedLibraries(void)
{
}

void WINAPI CoFreeUnusedLibrariesEx(DWORD delay, DWORD reserved)
{
	(void)delay, (void)reserved;
}
