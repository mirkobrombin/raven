/* SPDX-License-Identifier: MIT */
#include "rpc_own.h"

// COM proxies and stubs on the tables MIDL generates. A proxy's "this" is the slot holding its method table, with the
// proxy buffer around it, as generated proxies and the public header expect.

static inline const CInterfaceProxyHeader *proxy_header_of(const void *vtable)
{
	return (const CInterfaceProxyHeader *)((const BYTE *)vtable - offsetof(CInterfaceProxyVtbl, Vtbl));
}

typedef struct proxy_buffer
{
	const IRpcProxyBufferVtbl *buffer_vtbl;
	const void *vtable;		// what clients of the spec see as the object
	IUnknown *base;			// right after: the delegation thunks read it by this position
	LONG refs;
	IUnknown *outer;
	IRpcChannelBuffer *channel;
	IPSFactoryBuffer *factory;
	const IID *iid;
	IRpcProxyBuffer *base_buffer;
} proxy_buffer;

#define PROXY_OF_THIS(object) CONTAINING_RECORD((object), proxy_buffer, vtable)
#define PROXY_OF_BUFFER(object) CONTAINING_RECORD((object), proxy_buffer, buffer_vtbl)

// ---- the channel, from the generated code's side ----

void RPC_ENTRY NdrProxyInitialize(void *object, PRPC_MESSAGE rpc, PMIDL_STUB_MESSAGE m, PMIDL_STUB_DESC desc, unsigned int proc)
{
	proxy_buffer *proxy = PROXY_OF_THIS(object);
	NdrClientInitializeNew(rpc, m, desc, proc);
	m->pRpcChannelBuffer = proxy->channel;
	if (!proxy->channel)
		RpcRaiseException(CO_E_OBJNOTCONNECTED);
	IRpcChannelBuffer_AddRef(proxy->channel);
	DWORD context = MSHCTX_LOCAL;
	void *context_data = NULL;
	IRpcChannelBuffer_GetDestCtx(proxy->channel, &context, &context_data);
	m->dwDestContext = context;
	m->pvDestContext = context_data;
}

void RPC_ENTRY NdrProxyGetBuffer(void *object, PMIDL_STUB_MESSAGE m)
{
	proxy_buffer *proxy = PROXY_OF_THIS(object);
	m->RpcMsg->BufferLength = m->BufferLength;
	m->RpcMsg->DataRepresentation = NDR_LOCAL_DATA_REPRESENTATION;
	HRESULT hr = IRpcChannelBuffer_GetBuffer(m->pRpcChannelBuffer, (RPCOLEMESSAGE *)m->RpcMsg, proxy->iid);
	if (FAILED(hr))
		RpcRaiseException(hr);
	m->fBufferValid = TRUE;
	m->BufferStart = m->RpcMsg->Buffer;
	m->BufferEnd = m->BufferStart + m->BufferLength;
	m->Buffer = m->BufferStart;
}

void RPC_ENTRY NdrProxySendReceive(void *object, PMIDL_STUB_MESSAGE m)
{
	(void)object;
	if (!m->pRpcChannelBuffer)
		RpcRaiseException(CO_E_OBJNOTCONNECTED);
	ULONG status = 0;
	HRESULT hr = IRpcChannelBuffer_SendReceive(m->pRpcChannelBuffer, (RPCOLEMESSAGE *)m->RpcMsg, &status);
	m->dwStubPhase = PROXY_UNMARSHAL;
	m->BufferLength = m->RpcMsg->BufferLength;
	m->BufferStart = m->RpcMsg->Buffer;
	m->BufferEnd = m->BufferStart + m->BufferLength;
	m->Buffer = m->BufferStart;
	if (FAILED(hr))
		RpcRaiseException(hr == RPC_E_FAULT ? (HRESULT)status : hr);
}

void RPC_ENTRY NdrProxyFreeBuffer(void *object, PMIDL_STUB_MESSAGE m)
{
	(void)object;
	if (!m->pRpcChannelBuffer)
		return;
	if (m->fBufferValid) {
		IRpcChannelBuffer_FreeBuffer(m->pRpcChannelBuffer, (RPCOLEMESSAGE *)m->RpcMsg);
		m->fBufferValid = FALSE;
	}
	IRpcChannelBuffer_Release(m->pRpcChannelBuffer);
	m->pRpcChannelBuffer = NULL;
}

HRESULT RPC_ENTRY NdrProxyErrorHandler(DWORD code)
{
	return (LONG)code < 0 ? (HRESULT)code : HRESULT_FROM_WIN32(code);
}

void RPC_ENTRY NdrStubInitialize(PRPC_MESSAGE rpc, PMIDL_STUB_MESSAGE m, PMIDL_STUB_DESC desc, IRpcChannelBuffer *channel)
{
	NdrServerInitializeNew(rpc, m, desc);
	m->pRpcChannelBuffer = channel;
	IRpcChannelBuffer_AddRef(channel);
	DWORD context = MSHCTX_LOCAL;
	void *context_data = NULL;
	IRpcChannelBuffer_GetDestCtx(channel, &context, &context_data);
	m->dwDestContext = context;
	m->pvDestContext = context_data;
}

void RPC_ENTRY NdrStubGetBuffer(IRpcStubBuffer *stub, IRpcChannelBuffer *channel, PMIDL_STUB_MESSAGE m)
{
	const CInterfaceStubHeader *header = (const CInterfaceStubHeader *)((const BYTE *)((CStdStubBuffer *)stub)->lpVtbl - offsetof(CInterfaceStubVtbl, Vtbl));
	m->RpcMsg->BufferLength = m->BufferLength;
	HRESULT hr = IRpcChannelBuffer_GetBuffer(channel, (RPCOLEMESSAGE *)m->RpcMsg, header->piid);
	if (FAILED(hr))
		RpcRaiseException(hr);
	m->Buffer = m->RpcMsg->Buffer;
	m->fBufferValid = TRUE;
}

// ---- spec pointers on the wire: a counted block of what CoMarshalInterface writes ----

void ndr_interface(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const IID *iid)
{
	void **slot = (void **)memory;
	ndr_op plain = op == NDR_MEMSIZE ? NDR_SKIP : op;
	if (op == NDR_FREE) {
		if (slot && *slot)
			IUnknown_Release((IUnknown *)*slot);
		return;
	}
	DWORD context = m->dwDestContext;
	if (op == NDR_SIZE) {
		ULONG size = 0;
		m->BufferLength = (m->BufferLength + 3) & ~3u;
		m->BufferLength += 8;
		if (*slot && SUCCEEDED(CoGetMarshalSizeMax(&size, iid ? iid : &IID_IUnknown, *slot, context, m->pvDestContext, MSHLFLAGS_NORMAL)))
			m->BufferLength += size;
		return;
	}
	m->Buffer = (BYTE *)(((ULONG_PTR)m->Buffer + 3) & ~(ULONG_PTR)3);
	if (op == NDR_MARSHAL) {
		HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, 0);
		IStream *stream = NULL;
		ULONG written = 0;
		if (block && SUCCEEDED(CreateStreamOnHGlobal(block, TRUE, &stream))) {
			HRESULT hr = *slot ? CoMarshalInterface(stream, iid ? iid : &IID_IUnknown, *slot, context, m->pvDestContext, MSHLFLAGS_NORMAL) : S_OK;
			if (FAILED(hr)) {
				IStream_Release(stream);
				RpcRaiseException(hr);
			}
			ULARGE_INTEGER end;
			LARGE_INTEGER zero = {{0, 0}};
			IStream_Seek(stream, zero, STREAM_SEEK_CUR, &end);
			written = (ULONG)end.QuadPart;
			((ULONG *)m->Buffer)[0] = written;
			((ULONG *)m->Buffer)[1] = written;
			if (written) {
				BYTE *data = GlobalLock(block);
				memcpy(m->Buffer + 8, data, written);
				GlobalUnlock(block);
			}
			m->Buffer += 8 + written;
			IStream_Release(stream);
		} else {
			RpcRaiseException(RPC_S_OUT_OF_MEMORY);
		}
		return;
	}
	if (m->BufferEnd && m->Buffer + 8 > m->BufferEnd)
		RpcRaiseException(RPC_X_BAD_STUB_DATA);
	ULONG size = ((ULONG *)m->Buffer)[1];
	if (m->BufferEnd && size > (ULONG_PTR)(m->BufferEnd - m->Buffer - 8))
		RpcRaiseException(RPC_X_BAD_STUB_DATA);
	m->Buffer += 8;
	if (plain == NDR_SKIP) {
		if (op == NDR_MEMSIZE)
			m->MemorySize += sizeof(void *);
		m->Buffer += size;
		return;
	}
	*slot = NULL;
	if (size) {
		HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, size);
		IStream *stream = NULL;
		if (!block)
			RpcRaiseException(RPC_S_OUT_OF_MEMORY);
		BYTE *data = GlobalLock(block);
		memcpy(data, m->Buffer, size);
		GlobalUnlock(block);
		if (FAILED(CreateStreamOnHGlobal(block, TRUE, &stream)))
			RpcRaiseException(RPC_S_OUT_OF_MEMORY);
		HRESULT hr = CoUnmarshalInterface(stream, iid ? iid : &IID_IUnknown, slot);
		IStream_Release(stream);
		if (FAILED(hr))
			RpcRaiseException(hr);
		m->Buffer += size;
	}
}

// ---- proxy object ----

static HRESULT STDMETHODCALLTYPE buffer_query(IRpcProxyBuffer *iface, REFIID iid, void **out)
{
	proxy_buffer *self = PROXY_OF_BUFFER(iface);
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IRpcProxyBuffer)) {
		*out = iface;
		InterlockedIncrement(&self->refs);
		return S_OK;
	}
	if (IsEqualIID(iid, self->iid)) {
		*out = &self->vtable;
		InterlockedIncrement(&self->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE buffer_add_ref(IRpcProxyBuffer *iface)
{
	return (ULONG)InterlockedIncrement(&PROXY_OF_BUFFER(iface)->refs);
}

static ULONG STDMETHODCALLTYPE buffer_release(IRpcProxyBuffer *iface)
{
	proxy_buffer *self = PROXY_OF_BUFFER(iface);
	LONG refs = InterlockedDecrement(&self->refs);
	if (refs)
		return (ULONG)refs;
	if (self->channel)
		IRpcChannelBuffer_Release(self->channel);
	if (self->base_buffer) {
		IRpcProxyBuffer_Disconnect(self->base_buffer);
		IRpcProxyBuffer_Release(self->base_buffer);
	}
	if (self->base)
		IUnknown_Release(self->base);
	IPSFactoryBuffer_Release(self->factory);
	rpc_free(self);
	return 0;
}

static HRESULT STDMETHODCALLTYPE buffer_connect(IRpcProxyBuffer *iface, IRpcChannelBuffer *channel)
{
	proxy_buffer *self = PROXY_OF_BUFFER(iface);
	if (self->channel)
		IRpcChannelBuffer_Release(self->channel);
	self->channel = channel;
	IRpcChannelBuffer_AddRef(channel);
	return self->base_buffer ? IRpcProxyBuffer_Connect(self->base_buffer, channel) : S_OK;
}

static void STDMETHODCALLTYPE buffer_disconnect(IRpcProxyBuffer *iface)
{
	proxy_buffer *self = PROXY_OF_BUFFER(iface);
	if (self->base_buffer)
		IRpcProxyBuffer_Disconnect(self->base_buffer);
	if (self->channel)
		IRpcChannelBuffer_Release(self->channel);
	self->channel = NULL;
}

static const IRpcProxyBufferVtbl proxy_buffer_vtbl = {buffer_query, buffer_add_ref, buffer_release, buffer_connect, buffer_disconnect};

// The three methods every spec starts with, as generated tables name them.
HRESULT STDMETHODCALLTYPE IUnknown_QueryInterface_Proxy(IUnknown *iface, REFIID iid, void **out)
{
	return IUnknown_QueryInterface(PROXY_OF_THIS(iface)->outer, iid, out);
}

ULONG STDMETHODCALLTYPE IUnknown_AddRef_Proxy(IUnknown *iface)
{
	return IUnknown_AddRef(PROXY_OF_THIS(iface)->outer);
}

ULONG STDMETHODCALLTYPE IUnknown_Release_Proxy(IUnknown *iface)
{
	return IUnknown_Release(PROXY_OF_THIS(iface)->outer);
}

// The file's table version tells whether its proxy tables start with the stubless information;
// the interface's identifier always comes right before the methods.
static void **proxy_methods(const ProxyFileInfo *file, ULONG index)
{
	BYTE *table = (BYTE *)file->pProxyVtblList[index];
	return (void **)(table + (file->TableVersion > 1 ? 2 : 1) * sizeof(void *));
}

static HRESULT create_proxy(const ProxyFileInfo *info, ULONG index, IPSFactoryBuffer *factory, IUnknown *outer, REFIID iid, IRpcProxyBuffer **out_buffer, void **out_interface)
{
	void **methods = proxy_methods(info, index);
	const IID *base_iid = info->pDelegatedIIDs ? info->pDelegatedIIDs[index] : NULL;
	proxy_buffer *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->buffer_vtbl = &proxy_buffer_vtbl;
	self->vtable = methods;
	self->refs = 1;
	// With nobody aggregating it, the buffer's own counting stands for the whole proxy.
	self->outer = outer ? outer : (IUnknown *)&self->buffer_vtbl;
	self->factory = factory;
	self->iid = (const IID *)methods[-1];
	IPSFactoryBuffer_AddRef(factory);
	if (base_iid && !IsEqualIID(base_iid, &IID_IUnknown)) {
		IPSFactoryBuffer *base_factory = NULL;
		CLSID clsid;
		HRESULT hr = CoGetPSClsid(base_iid, &clsid);
		if (SUCCEEDED(hr))
			hr = CoGetClassObject(&clsid, CLSCTX_INPROC_SERVER, NULL, &IID_IPSFactoryBuffer, (void **)&base_factory);
		if (SUCCEEDED(hr)) {
			hr = IPSFactoryBuffer_CreateProxy(base_factory, self->outer, base_iid, &self->base_buffer, (void **)&self->base);
			IPSFactoryBuffer_Release(base_factory);
		}
		if (FAILED(hr)) {
			buffer_release((IRpcProxyBuffer *)self);
			return hr;
		}
	}
	(void)iid;
	*out_buffer = (IRpcProxyBuffer *)self;
	*out_interface = &self->vtable;
	IUnknown_AddRef((IUnknown *)*out_interface);
	return S_OK;
}

// ---- stub object ----

#define STUB_HEADER_OF(stub) ((const CInterfaceStubHeader *)((const BYTE *)(stub)->lpVtbl - offsetof(CInterfaceStubVtbl, Vtbl)))

HRESULT STDMETHODCALLTYPE CStdStubBuffer_QueryInterface(IRpcStubBuffer *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IRpcStubBuffer)) {
		IRpcStubBuffer_AddRef(iface);
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CStdStubBuffer_AddRef(IRpcStubBuffer *iface)
{
	return (ULONG)InterlockedIncrement(&((CStdStubBuffer *)iface)->RefCount);
}

ULONG STDMETHODCALLTYPE NdrCStdStubBuffer_Release(IRpcStubBuffer *iface, IPSFactoryBuffer *factory)
{
	CStdStubBuffer *self = (CStdStubBuffer *)iface;
	LONG refs = InterlockedDecrement(&self->RefCount);
	if (refs)
		return (ULONG)refs;
	IRpcStubBuffer_Disconnect(iface);
	rpc_free(self);
	IPSFactoryBuffer_Release(factory);
	return 0;
}

// A stub with a base keeps it just ahead of itself, along with the table of forwarding thunks.
typedef struct delegating_stub
{
	IUnknownVtbl *delegate_vtable;	// the object base stubs think they serve: its methods forward to the real one
	void *server;			// the slot the base stub uses as object
	IRpcStubBuffer *base;
	CStdStubBuffer stub;
} delegating_stub;

ULONG STDMETHODCALLTYPE NdrCStdStubBuffer2_Release(IRpcStubBuffer *iface, IPSFactoryBuffer *factory)
{
	CStdStubBuffer *stub = (CStdStubBuffer *)iface;
	delegating_stub *self = CONTAINING_RECORD(stub, delegating_stub, stub);
	LONG refs = InterlockedDecrement(&stub->RefCount);
	if (refs)
		return (ULONG)refs;
	IRpcStubBuffer_Disconnect(iface);
	if (self->base)
		IRpcStubBuffer_Release(self->base);
	rpc_free(self);
	IPSFactoryBuffer_Release(factory);
	return 0;
}

HRESULT STDMETHODCALLTYPE CStdStubBuffer_Connect(IRpcStubBuffer *iface, IUnknown *server)
{
	CStdStubBuffer *self = (CStdStubBuffer *)iface;
	void *object = NULL;
	HRESULT hr = IUnknown_QueryInterface(server, STUB_HEADER_OF(self)->piid, &object);
	void *old = InterlockedExchangePointer(&self->pvServerObject, object);
	if (old)
		IUnknown_Release((IUnknown *)old);
	return hr;
}

void STDMETHODCALLTYPE CStdStubBuffer_Disconnect(IRpcStubBuffer *iface)
{
	CStdStubBuffer *self = (CStdStubBuffer *)iface;
	void *old = InterlockedExchangePointer(&self->pvServerObject, NULL);
	if (old)
		IUnknown_Release((IUnknown *)old);
}

HRESULT STDMETHODCALLTYPE CStdStubBuffer2_Connect(IRpcStubBuffer *iface, IUnknown *server)
{
	CStdStubBuffer *stub = (CStdStubBuffer *)iface;
	delegating_stub *self = CONTAINING_RECORD(stub, delegating_stub, stub);
	HRESULT hr = CStdStubBuffer_Connect(iface, server);
	if (SUCCEEDED(hr) && self->base)
		hr = IRpcStubBuffer_Connect(self->base, (IUnknown *)&self->delegate_vtable);
	return hr;
}

void STDMETHODCALLTYPE CStdStubBuffer2_Disconnect(IRpcStubBuffer *iface)
{
	CStdStubBuffer *stub = (CStdStubBuffer *)iface;
	delegating_stub *self = CONTAINING_RECORD(stub, delegating_stub, stub);
	if (self->base)
		IRpcStubBuffer_Disconnect(self->base);
	CStdStubBuffer_Disconnect(iface);
}

HRESULT STDMETHODCALLTYPE CStdStubBuffer_Invoke(IRpcStubBuffer *iface, RPCOLEMESSAGE *message, IRpcChannelBuffer *channel)
{
	CStdStubBuffer *self = (CStdStubBuffer *)iface;
	const CInterfaceStubHeader *header = STUB_HEADER_OF(self);
	ULONG proc = message->iMethod & 0x7fff;
	DWORD phase = STUB_UNMARSHAL;
	if (header->pDispatchTable) {
		header->pDispatchTable[proc](iface, channel, (PRPC_MESSAGE)message, &phase);
		return S_OK;
	}
	return NdrStubCall2((struct IRpcStubBuffer *)iface, (struct IRpcChannelBuffer *)channel, (PRPC_MESSAGE)message, &phase);
}

void __RPC_STUB NdrStubForwardingFunction(IRpcStubBuffer *iface, IRpcChannelBuffer *channel, PRPC_MESSAGE message, DWORD *phase)
{
	(void)phase;
	delegating_stub *self = CONTAINING_RECORD((CStdStubBuffer *)iface, delegating_stub, stub);
	HRESULT hr = IRpcStubBuffer_Invoke(self->base, (RPCOLEMESSAGE *)message, channel);
	if (FAILED(hr))
		RpcRaiseException(hr);
}

IRpcStubBuffer *STDMETHODCALLTYPE CStdStubBuffer_IsIIDSupported(IRpcStubBuffer *iface, REFIID iid)
{
	CStdStubBuffer *self = (CStdStubBuffer *)iface;
	if (!IsEqualIID(STUB_HEADER_OF(self)->piid, iid))
		return NULL;
	IRpcStubBuffer_AddRef(iface);
	return iface;
}

ULONG STDMETHODCALLTYPE CStdStubBuffer_CountRefs(IRpcStubBuffer *iface)
{
	return ((CStdStubBuffer *)iface)->pvServerObject ? 1 : 0;
}

static ULONG STDMETHODCALLTYPE delegating_count_refs(IRpcStubBuffer *iface)
{
	delegating_stub *self = CONTAINING_RECORD((CStdStubBuffer *)iface, delegating_stub, stub);
	return CStdStubBuffer_CountRefs(iface) + (self->base ? IRpcStubBuffer_CountRefs(self->base) : 0);
}

HRESULT STDMETHODCALLTYPE CStdStubBuffer_DebugServerQueryInterface(IRpcStubBuffer *iface, void **out)
{
	*out = ((CStdStubBuffer *)iface)->pvServerObject;
	return *out ? S_OK : CO_E_OBJNOTCONNECTED;
}

void STDMETHODCALLTYPE CStdStubBuffer_DebugServerRelease(IRpcStubBuffer *iface, void *object)
{
	(void)iface; (void)object;
}

static const IRpcStubBufferVtbl stub_vtbl_parts = {
	CStdStubBuffer_QueryInterface, CStdStubBuffer_AddRef, NULL, CStdStubBuffer_Connect, CStdStubBuffer_Disconnect,
	CStdStubBuffer_Invoke, CStdStubBuffer_IsIIDSupported, CStdStubBuffer_CountRefs,
	CStdStubBuffer_DebugServerQueryInterface, CStdStubBuffer_DebugServerRelease,
};

// A base stub serves this little object; whatever it calls runs on the real server object through forwarding thunks.
static HRESULT create_stub(const ProxyFileInfo *info, ULONG index, IPSFactoryBuffer *factory, IUnknown *server, IRpcStubBuffer **out)
{
	const CInterfaceStubVtbl *table = info->pStubVtblList[index];
	const IID *base_iid = info->pDelegatedIIDs ? info->pDelegatedIIDs[index] : NULL;
	delegating_stub *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->stub.lpVtbl = (IRpcStubBufferVtbl *)&table->Vtbl;
	self->stub.RefCount = 1;
	if (base_iid && !IsEqualIID(base_iid, &IID_IUnknown)) {
		self->delegate_vtable = rpc_delegate_vtable();
		CLSID clsid;
		IPSFactoryBuffer *base_factory = NULL;
		HRESULT hr = CoGetPSClsid(base_iid, &clsid);
		if (SUCCEEDED(hr))
			hr = CoGetClassObject(&clsid, CLSCTX_INPROC_SERVER, NULL, &IID_IPSFactoryBuffer, (void **)&base_factory);
		if (SUCCEEDED(hr)) {
			hr = IPSFactoryBuffer_CreateStub(base_factory, base_iid, NULL, &self->base);
			IPSFactoryBuffer_Release(base_factory);
		}
		if (FAILED(hr)) {
			rpc_free(self);
			return hr;
		}
	}
	IPSFactoryBuffer_AddRef(factory);
	if (server) {
		HRESULT hr = IRpcStubBuffer_Connect((IRpcStubBuffer *)&self->stub, server);
		if (FAILED(hr)) {
			IRpcStubBuffer_Release((IRpcStubBuffer *)&self->stub);
			return hr;
		}
	}
	*out = (IRpcStubBuffer *)&self->stub;
	return S_OK;
}

// ---- the proxy file's factory ----

static BOOL find_iid(const CStdPSFactoryBuffer *factory, REFIID iid, const ProxyFileInfo **out, ULONG *index)
{
	for (const ProxyFileInfo *const *file = factory->pProxyFileList; *file; file++) {
		for (ULONG i = 0; i < (*file)->TableSize; i++) {
			if (IsEqualIID((*file)->pStubVtblList[i]->header.piid, iid)) {
				*out = *file;
				*index = i;
				return TRUE;
			}
		}
	}
	return FALSE;
}

static HRESULT STDMETHODCALLTYPE factory_query(IPSFactoryBuffer *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IPSFactoryBuffer)) {
		*out = iface;
		IPSFactoryBuffer_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE factory_add_ref(IPSFactoryBuffer *iface)
{
	return (ULONG)InterlockedIncrement(&((CStdPSFactoryBuffer *)iface)->RefCount);
}

static ULONG STDMETHODCALLTYPE factory_release(IPSFactoryBuffer *iface)
{
	return (ULONG)InterlockedDecrement(&((CStdPSFactoryBuffer *)iface)->RefCount);
}

static HRESULT STDMETHODCALLTYPE factory_create_proxy(IPSFactoryBuffer *iface, IUnknown *outer, REFIID iid, IRpcProxyBuffer **buffer, void **out)
{
	const ProxyFileInfo *file;
	ULONG index;
	if (!find_iid((CStdPSFactoryBuffer *)iface, iid, &file, &index))
		return E_NOINTERFACE;
	return create_proxy(file, index, iface, outer, iid, buffer, out);
}

static HRESULT STDMETHODCALLTYPE factory_create_stub(IPSFactoryBuffer *iface, REFIID iid, IUnknown *server, IRpcStubBuffer **out)
{
	const ProxyFileInfo *file;
	ULONG index;
	if (!find_iid((CStdPSFactoryBuffer *)iface, iid, &file, &index))
		return E_NOINTERFACE;
	return create_stub(file, index, iface, server, out);
}

static const IPSFactoryBufferVtbl factory_vtbl = {factory_query, factory_add_ref, factory_release, factory_create_proxy, factory_create_stub};

// Tables come from the generator with holes that only the runtime can fill.
static void fill_tables(const ProxyFileInfo *file)
{
	for (ULONG i = 0; i < file->TableSize; i++) {
		CInterfaceStubVtbl *stub = (CInterfaceStubVtbl *)file->pStubVtblList[i];
		BOOL delegates = file->pDelegatedIIDs && file->pDelegatedIIDs[i];
		void **methods = proxy_methods(file, i);
		ULONG count = stub->header.DispatchTableCount;
		DWORD old;
		VirtualProtect(methods, count * sizeof(void *), PAGE_READWRITE, &old);
		for (ULONG m = 3; m < count; m++) {
			if (methods[m] == (void *)(LONG_PTR)-1)
				methods[m] = rpc_stubless_thunk(m);
			else if (!methods[m])
				methods[m] = delegates ? rpc_delegate_thunk(m) : rpc_stubless_thunk(m);
		}
		VirtualProtect(stub, sizeof(*stub), PAGE_READWRITE, &old);
		IRpcStubBufferVtbl *vtbl = &stub->Vtbl;
		#define FILL(name, value) if (!vtbl->name) vtbl->name = (value)
		FILL(QueryInterface, stub_vtbl_parts.QueryInterface);
		FILL(AddRef, stub_vtbl_parts.AddRef);
		FILL(Connect, delegates ? CStdStubBuffer2_Connect : stub_vtbl_parts.Connect);
		FILL(Disconnect, delegates ? CStdStubBuffer2_Disconnect : stub_vtbl_parts.Disconnect);
		FILL(Invoke, stub_vtbl_parts.Invoke);
		FILL(IsIIDSupported, stub_vtbl_parts.IsIIDSupported);
		FILL(CountRefs, delegates ? delegating_count_refs : stub_vtbl_parts.CountRefs);
		FILL(DebugServerQueryInterface, stub_vtbl_parts.DebugServerQueryInterface);
		FILL(DebugServerRelease, stub_vtbl_parts.DebugServerRelease);
		#undef FILL
	}
}

HRESULT RPC_ENTRY NdrDllGetClassObject(REFCLSID clsid, REFIID iid, void **out, const ProxyFileInfo **const files, const CLSID *file_clsid, CStdPSFactoryBuffer *factory)
{
	*out = NULL;
	if (!factory->lpVtbl) {
		factory->lpVtbl = (IPSFactoryBufferVtbl *)&factory_vtbl;
		factory->RefCount = 0;
		factory->pProxyFileList = files;
		for (const ProxyFileInfo *const *file = files; *file; file++)
			fill_tables(*file);
	}
	// The factory answers to its own class and to the identifier of any interface it covers.
	BOOL match = file_clsid ? IsEqualCLSID(clsid, file_clsid) : FALSE;
	for (const ProxyFileInfo **file = files; !match && *file; file++)
		for (ULONG i = 0; !match && i < (*file)->TableSize; i++)
			match = IsEqualGUID(clsid, (*file)->pStubVtblList[i]->header.piid);
	if (!match)
		return CLASS_E_CLASSNOTAVAILABLE;
	return IPSFactoryBuffer_QueryInterface((IPSFactoryBuffer *)factory, iid, out);
}

HRESULT RPC_ENTRY NdrDllCanUnloadNow(CStdPSFactoryBuffer *factory)
{
	return factory->RefCount ? S_FALSE : S_OK;
}

// ---- registry entries for a proxy file ----

static void guid_text(const GUID *guid, WCHAR *out)
{
	StringFromGUID2(guid, out, 39);
}

HRESULT RPC_ENTRY NdrDllRegisterProxy(HMODULE module, const ProxyFileInfo **files, const CLSID *clsid)
{
	const CLSID *use = clsid;
	if (!files || !files[0] || !files[0]->TableSize)
		return E_INVALIDARG;
	if (!use)
		use = files[0]->pStubVtblList[0]->header.piid;
	WCHAR clsid_text[39], module_path[MAX_PATH], key_path[128];
	guid_text(use, clsid_text);
	GetModuleFileNameW(module, module_path, MAX_PATH);
	wsprintfW(key_path, L"CLSID\\%s\\InProcServer32", clsid_text);
	HKEY key;
	if (!RegCreateKeyExW(HKEY_CLASSES_ROOT, key_path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL)) {
		RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)module_path, (lstrlenW(module_path) + 1) * sizeof(WCHAR));
		RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Both", sizeof(L"Both"));
		RegCloseKey(key);
	}
	for (const ProxyFileInfo *const *file = files; *file; file++) {
		for (ULONG i = 0; i < (*file)->TableSize; i++) {
			WCHAR iid_text[39], methods[16];
			const CInterfaceStubVtbl *stub = (*file)->pStubVtblList[i];
			guid_text(stub->header.piid, iid_text);
			wsprintfW(key_path, L"Interface\\%s", iid_text);
			if (RegCreateKeyExW(HKEY_CLASSES_ROOT, key_path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
				continue;
			if ((*file)->pNamesArray && (*file)->pNamesArray[i]) {
				WCHAR *name = rpc_widen((*file)->pNamesArray[i]);
				RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)name, (lstrlenW(name) + 1) * sizeof(WCHAR));
				rpc_free(name);
			}
			HKEY sub;
			if (!RegCreateKeyExW(key, L"ProxyStubClsid32", 0, NULL, 0, KEY_WRITE, NULL, &sub, NULL)) {
				RegSetValueExW(sub, NULL, 0, REG_SZ, (const BYTE *)clsid_text, sizeof(clsid_text));
				RegCloseKey(sub);
			}
			wsprintfW(methods, L"%u", (UINT)stub->header.DispatchTableCount);
			if (!RegCreateKeyExW(key, L"NumMethods", 0, NULL, 0, KEY_WRITE, NULL, &sub, NULL)) {
				RegSetValueExW(sub, NULL, 0, REG_SZ, (const BYTE *)methods, (lstrlenW(methods) + 1) * sizeof(WCHAR));
				RegCloseKey(sub);
			}
			RegCloseKey(key);
		}
	}
	return S_OK;
}

HRESULT RPC_ENTRY NdrDllUnregisterProxy(HMODULE module, const ProxyFileInfo **files, const CLSID *clsid)
{
	(void)module;
	if (!files || !files[0] || !files[0]->TableSize)
		return E_INVALIDARG;
	const CLSID *use = clsid ? clsid : files[0]->pStubVtblList[0]->header.piid;
	WCHAR text[39], key_path[128];
	guid_text(use, text);
	wsprintfW(key_path, L"CLSID\\%s\\InProcServer32", text);
	RegDeleteKeyW(HKEY_CLASSES_ROOT, key_path);
	wsprintfW(key_path, L"CLSID\\%s", text);
	RegDeleteKeyW(HKEY_CLASSES_ROOT, key_path);
	for (const ProxyFileInfo *const *file = files; *file; file++) {
		for (ULONG i = 0; i < (*file)->TableSize; i++) {
			guid_text((*file)->pStubVtblList[i]->header.piid, text);
			wsprintfW(key_path, L"Interface\\%s\\ProxyStubClsid32", text);
			RegDeleteKeyW(HKEY_CLASSES_ROOT, key_path);
			wsprintfW(key_path, L"Interface\\%s\\NumMethods", text);
			RegDeleteKeyW(HKEY_CLASSES_ROOT, key_path);
			wsprintfW(key_path, L"Interface\\%s", text);
			RegDeleteKeyW(HKEY_CLASSES_ROOT, key_path);
		}
	}
	return S_OK;
}

HRESULT WINAPI CreateProxyFromTypeInfo(ITypeInfo *info, IUnknown *outer, REFIID iid, IRpcProxyBuffer **proxy, void **out)
{
	(void)info; (void)outer; (void)iid; (void)proxy; (void)out;
	return E_NOTIMPL;
}

HRESULT WINAPI CreateStubFromTypeInfo(ITypeInfo *info, REFIID iid, IUnknown *server, IRpcStubBuffer **out)
{
	(void)info; (void)iid; (void)server; (void)out;
	return E_NOTIMPL;
}

// ---- delegation thunks ----

#define DELEGATE_COUNT 512

// A proxy method served by the base spec: same slot, on the base proxy kept right behind the method table slot.
// A stub's base calls its object through the second family, which finds the real server object the same way.
#ifdef _WIN64
__asm__(
	".text\n"
	".p2align 4\n"
	".globl rpc_delegate_thunks\n"
	"rpc_delegate_thunks:\n"
	".set rpc_thunk_index, 0\n"
	".rept 512\n"
	"	movq 8(%rcx), %rcx\n"
	"	movq (%rcx), %rax\n"
	"	jmp *(rpc_thunk_index*8)(%rax)\n"
	"	.p2align 4\n"
	"	.set rpc_thunk_index, rpc_thunk_index + 1\n"
	".endr\n"
	".globl rpc_forward_thunks\n"
	"rpc_forward_thunks:\n"
	".set rpc_thunk_index, 0\n"
	".rept 512\n"
	"	movq 8(%rcx), %rcx\n"
	"	movq (%rcx), %rax\n"
	"	jmp *(rpc_thunk_index*8)(%rax)\n"
	"	.p2align 4\n"
	"	.set rpc_thunk_index, rpc_thunk_index + 1\n"
	".endr\n");
#else
__asm__(
	".text\n"
	".p2align 4\n"
	".globl _rpc_delegate_thunks\n"
	"_rpc_delegate_thunks:\n"
	".set rpc_thunk_index, 0\n"
	".rept 512\n"
	"	movl 4(%esp), %eax\n"
	"	movl 4(%eax), %eax\n"
	"	movl %eax, 4(%esp)\n"
	"	movl (%eax), %eax\n"
	"	jmp *(rpc_thunk_index*4)(%eax)\n"
	"	.p2align 4\n"
	"	.set rpc_thunk_index, rpc_thunk_index + 1\n"
	".endr\n"
	".globl _rpc_forward_thunks\n"
	"_rpc_forward_thunks:\n"
	".set rpc_thunk_index, 0\n"
	".rept 512\n"
	"	movl 4(%esp), %eax\n"
	"	movl 4(%eax), %eax\n"
	"	movl %eax, 4(%esp)\n"
	"	movl (%eax), %eax\n"
	"	jmp *(rpc_thunk_index*4)(%eax)\n"
	"	.p2align 4\n"
	"	.set rpc_thunk_index, rpc_thunk_index + 1\n"
	".endr\n");
#endif

extern const BYTE rpc_delegate_thunks[], rpc_forward_thunks[];

void *rpc_delegate_thunk(ULONG index)
{
	return index < DELEGATE_COUNT ? (void *)(rpc_delegate_thunks + index * 16) : NULL;
}

void *rpc_delegate_vtable(void)
{
	static void *table[DELEGATE_COUNT];
	if (!table[0])
		for (INT i = DELEGATE_COUNT - 1; i >= 0; i--)
			table[i] = (void *)(rpc_forward_thunks + i * 16);
	return table;
}
