/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven network list component: the NetworkListManager class of netprofm.dll, which programs ask whether the computer is connected
 * before they reach a service, which networks it is connected to, and what a connection costs.
 *
 * The computer is connected when it has an IPv4 address beyond loopback, and such a connection counts as one to the Internet: Raven does
 * not probe a server as the connectivity indicator of Windows does. A connected computer has one network, private and outside a domain,
 * with one connection whose cost is unrestricted. Connectivity is read when asked, and the events a program registers for are accepted
 * but never raised. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <oleauto.h>
#include <ocidl.h>
#include <olectl.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <stddef.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

DEFINE_GUID(network_id, 0x52415645, 0x4e00, 0x4e45, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01);
DEFINE_GUID(connection_id, 0x52415645, 0x4e00, 0x4e45, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02);

static LONG objects;

static BOOL same(REFIID left, REFIID right)
{
	const DWORD *a = (const DWORD *)left, *b = (const DWORD *)right;
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* Whether the computer has an IPv4 address beyond loopback, the adapters the helper library lists. */
static NLM_CONNECTIVITY connectivity(void)
{
	ULONG size = 0;
	if (GetAdaptersInfo(NULL, &size) != ERROR_BUFFER_OVERFLOW || !size)
		return NLM_CONNECTIVITY_DISCONNECTED;
	IP_ADAPTER_INFO *adapters = ALLOC(size);
	const BOOL connected = adapters && GetAdaptersInfo(adapters, &size) == NO_ERROR;
	FREE(adapters);
	return connected ? NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV4_LOCALNETWORK : NLM_CONNECTIVITY_DISCONNECTED;
}

static HRESULT connected_flag(VARIANT_BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = connectivity() ? VARIANT_TRUE : VARIANT_FALSE;
	return S_OK;
}

static HRESULT connectivity_value(NLM_CONNECTIVITY *out)
{
	if (!out)
		return E_POINTER;
	*out = connectivity();
	return S_OK;
}

/* The automation methods every interface here inherits: no type information is published. */
#define DISPATCH_METHODS(prefix, type) \
	static HRESULT WINAPI prefix##_count(type *self, UINT *count) \
	{ \
		if (!count) \
			return E_POINTER; \
		*count = 0; \
		return S_OK; \
	} \
	static HRESULT WINAPI prefix##_type(type *self, UINT index, LCID locale, ITypeInfo **info) { return E_NOTIMPL; } \
	static HRESULT WINAPI prefix##_names(type *self, REFIID iid, LPOLESTR *names, UINT count, LCID locale, DISPID *ids) { return E_NOTIMPL; } \
	static HRESULT WINAPI prefix##_invoke(type *self, DISPID id, REFIID iid, LCID locale, WORD flags, DISPPARAMS *parameters, VARIANT *result, \
		EXCEPINFO *exception, UINT *argument) \
	{ \
		return E_NOTIMPL; \
	}

/* The network and its connection, which live as long as the process. */
static INetwork network;
static INetworkConnection connection;

static HRESULT WINAPI network_query(INetwork *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IDispatch) || same(iid, &IID_INetwork)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI network_reference(INetwork *self) { return 2; }
static ULONG WINAPI network_release(INetwork *self) { return 1; }
DISPATCH_METHODS(network, INetwork)

static HRESULT WINAPI network_name(INetwork *self, BSTR *name)
{
	if (!name)
		return E_POINTER;
	return (*name = SysAllocString(L"Network")) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI network_set_text(INetwork *self, BSTR text) { return E_ACCESSDENIED; }

static HRESULT WINAPI network_id_value(INetwork *self, GUID *id)
{
	if (!id)
		return E_POINTER;
	*id = network_id;
	return S_OK;
}

static HRESULT WINAPI network_domain(INetwork *self, NLM_DOMAIN_TYPE *type)
{
	if (!type)
		return E_POINTER;
	*type = NLM_DOMAIN_TYPE_NON_DOMAIN_NETWORK;
	return S_OK;
}

static HRESULT make_enumerator(const IID *kind, IUnknown *item, void **out);

static HRESULT WINAPI network_connections(INetwork *self, IEnumNetworkConnections **out)
{
	if (!out)
		return E_POINTER;
	return make_enumerator(&IID_IEnumNetworkConnections, connectivity() ? (IUnknown *)&connection : NULL, (void **)out);
}

static HRESULT WINAPI network_times(INetwork *self, DWORD *created_low, DWORD *created_high, DWORD *connected_low, DWORD *connected_high)
{
	FILETIME now;
	if (!created_low || !created_high || !connected_low || !connected_high)
		return E_POINTER;
	GetSystemTimeAsFileTime(&now);
	*created_low = *connected_low = now.dwLowDateTime;
	*created_high = *connected_high = now.dwHighDateTime;
	return S_OK;
}

static HRESULT WINAPI network_internet(INetwork *self, VARIANT_BOOL *out) { return connected_flag(out); }
static HRESULT WINAPI network_connected(INetwork *self, VARIANT_BOOL *out) { return connected_flag(out); }
static HRESULT WINAPI network_connectivity(INetwork *self, NLM_CONNECTIVITY *out) { return connectivity_value(out); }

static HRESULT WINAPI network_category(INetwork *self, NLM_NETWORK_CATEGORY *category)
{
	if (!category)
		return E_POINTER;
	*category = NLM_NETWORK_CATEGORY_PRIVATE;
	return S_OK;
}

static HRESULT WINAPI network_set_category(INetwork *self, NLM_NETWORK_CATEGORY category) { return E_ACCESSDENIED; }

static INetworkVtbl network_table = {network_query, network_reference, network_release, network_count, network_type, network_names,
	network_invoke, network_name, network_set_text, network_name, network_set_text, network_id_value, network_domain, network_connections,
	network_times, network_internet, network_connected, network_connectivity, network_category, network_set_category};
static INetwork network = {&network_table};

static HRESULT WINAPI connection_query(INetworkConnection *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IDispatch) || same(iid, &IID_INetworkConnection)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI connection_reference(INetworkConnection *self) { return 2; }
static ULONG WINAPI connection_release(INetworkConnection *self) { return 1; }
DISPATCH_METHODS(connection, INetworkConnection)

static HRESULT WINAPI connection_network(INetworkConnection *self, INetwork **out)
{
	if (!out)
		return E_POINTER;
	*out = &network;
	return S_OK;
}

static HRESULT WINAPI connection_internet(INetworkConnection *self, VARIANT_BOOL *out) { return connected_flag(out); }
static HRESULT WINAPI connection_connected(INetworkConnection *self, VARIANT_BOOL *out) { return connected_flag(out); }
static HRESULT WINAPI connection_connectivity(INetworkConnection *self, NLM_CONNECTIVITY *out) { return connectivity_value(out); }

static HRESULT WINAPI connection_id_value(INetworkConnection *self, GUID *id)
{
	if (!id)
		return E_POINTER;
	*id = connection_id;
	return S_OK;
}

static HRESULT WINAPI connection_domain(INetworkConnection *self, NLM_DOMAIN_TYPE *type) { return network_domain(&network, type); }

static INetworkConnectionVtbl connection_table = {connection_query, connection_reference, connection_release, connection_count,
	connection_type, connection_names, connection_invoke, connection_network, connection_internet, connection_connected,
	connection_connectivity, connection_id_value, connection_id_value, connection_domain};
static INetworkConnection connection = {&connection_table};

/* An enumeration of networks or of connections, holding the one item or none; both kinds have the same methods. */
typedef struct enumerator {
	const void *table;
	LONG refs;
	const IID *kind;
	IUnknown *item;
	ULONG position;
} enumerator;

static HRESULT WINAPI enumerator_query(enumerator *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IDispatch) || same(iid, self->kind)) {
		InterlockedIncrement(&self->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI enumerator_reference(enumerator *self) { return (ULONG)InterlockedIncrement(&self->refs); }

static ULONG WINAPI enumerator_release(enumerator *self)
{
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return (ULONG)refs;
}

DISPATCH_METHODS(enumerator, enumerator)

static HRESULT WINAPI enumerator_variants(enumerator *self, IEnumVARIANT **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI enumerator_next(enumerator *self, ULONG count, IUnknown **items, ULONG *fetched)
{
	ULONG done = 0;
	if (!items)
		return E_POINTER;
	if (count > 1 && !fetched)
		return E_INVALIDARG;
	if (count && self->item && !self->position) {
		items[done++] = self->item;
		self->position = 1;
	}
	if (fetched)
		*fetched = done;
	return done == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI enumerator_skip(enumerator *self, ULONG count)
{
	const ULONG left = self->item && !self->position ? 1 : 0;
	if (count && left)
		self->position = 1;
	return count <= left ? S_OK : S_FALSE;
}

static HRESULT WINAPI enumerator_reset(enumerator *self)
{
	self->position = 0;
	return S_OK;
}

static HRESULT WINAPI enumerator_clone(enumerator *self, void **out)
{
	if (!out)
		return E_POINTER;
	HRESULT result = make_enumerator(self->kind, self->item, out);
	if (SUCCEEDED(result))
		((enumerator *)*out)->position = self->position;
	return result;
}

static const struct {
	void *query, *reference, *release, *count, *type, *names, *invoke, *variants, *next, *skip, *reset, *clone;
} enumerator_table = {enumerator_query, enumerator_reference, enumerator_release, enumerator_count, enumerator_type, enumerator_names,
	enumerator_invoke, enumerator_variants, enumerator_next, enumerator_skip, enumerator_reset, enumerator_clone};

static HRESULT make_enumerator(const IID *kind, IUnknown *item, void **out)
{
	enumerator *self = ALLOC(sizeof *self);
	*out = NULL;
	if (!self)
		return E_OUTOFMEMORY;
	self->table = &enumerator_table;
	self->refs = 1;
	self->kind = kind;
	self->item = item;
	InterlockedIncrement(&objects);
	*out = self;
	return S_OK;
}

/* A connection point for one of the event interfaces: registrations are accepted and given a cookie, and no event is raised. */
typedef struct point {
	IConnectionPoint iface;
	LONG refs;
	IID kind;
	IConnectionPointContainer *container;
} point;

static LONG cookies;

static HRESULT WINAPI point_query(IConnectionPoint *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IConnectionPoint)) {
		IConnectionPoint_AddRef(iface);
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI point_reference(IConnectionPoint *iface) { return (ULONG)InterlockedIncrement(&((point *)iface)->refs); }

static ULONG WINAPI point_release(IConnectionPoint *iface)
{
	point *self = (point *)iface;
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		IConnectionPointContainer_Release(self->container);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return (ULONG)refs;
}

static HRESULT WINAPI point_interface(IConnectionPoint *iface, IID *iid)
{
	if (!iid)
		return E_POINTER;
	*iid = ((point *)iface)->kind;
	return S_OK;
}

static HRESULT WINAPI point_container(IConnectionPoint *iface, IConnectionPointContainer **out)
{
	if (!out)
		return E_POINTER;
	*out = ((point *)iface)->container;
	IConnectionPointContainer_AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI point_advise(IConnectionPoint *iface, IUnknown *sink, DWORD *cookie)
{
	IUnknown *events = NULL;
	if (!sink || !cookie)
		return E_POINTER;
	*cookie = 0;
	if (FAILED(IUnknown_QueryInterface(sink, &((point *)iface)->kind, (void **)&events)) || !events)
		return CONNECT_E_CANNOTCONNECT;
	IUnknown_Release(events);
	*cookie = (DWORD)InterlockedIncrement(&cookies);
	return S_OK;
}

static HRESULT WINAPI point_unadvise(IConnectionPoint *iface, DWORD cookie) { return cookie ? S_OK : CONNECT_E_NOCONNECTION; }

static HRESULT WINAPI point_connections(IConnectionPoint *iface, IEnumConnections **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static IConnectionPointVtbl point_table = {point_query, point_reference, point_release, point_interface, point_container, point_advise,
	point_unadvise, point_connections};

/* The manager. */
typedef struct manager {
	INetworkListManager iface;
	INetworkCostManager cost;
	IConnectionPointContainer points;
	LONG refs;
} manager;

static manager *from_cost(INetworkCostManager *iface) { return (manager *)((char *)iface - offsetof(manager, cost)); }
static manager *from_points(IConnectionPointContainer *iface) { return (manager *)((char *)iface - offsetof(manager, points)); }

static HRESULT WINAPI manager_query(INetworkListManager *iface, REFIID iid, void **out)
{
	manager *self = (manager *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IDispatch) || same(iid, &IID_INetworkListManager))
		*out = &self->iface;
	else if (same(iid, &IID_INetworkCostManager))
		*out = &self->cost;
	else if (same(iid, &IID_IConnectionPointContainer))
		*out = &self->points;
	else
		return E_NOINTERFACE;
	InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG WINAPI manager_reference(INetworkListManager *iface) { return (ULONG)InterlockedIncrement(&((manager *)iface)->refs); }

static ULONG WINAPI manager_release(INetworkListManager *iface)
{
	manager *self = (manager *)iface;
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return (ULONG)refs;
}

DISPATCH_METHODS(manager, INetworkListManager)

static HRESULT WINAPI manager_networks(INetworkListManager *iface, NLM_ENUM_NETWORK flags, IEnumNetworks **out)
{
	if (!out)
		return E_POINTER;
	/* The one network is connected whenever it exists, so it is never among the disconnected ones. */
	const BOOL listed = connectivity() && (flags & NLM_ENUM_NETWORK_CONNECTED);
	return make_enumerator(&IID_IEnumNetworks, listed ? (IUnknown *)&network : NULL, (void **)out);
}

static HRESULT WINAPI manager_network(INetworkListManager *iface, GUID id, INetwork **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!connectivity() || !same(&id, &network_id))
		return E_INVALIDARG;
	*out = &network;
	return S_OK;
}

static HRESULT WINAPI manager_connections(INetworkListManager *iface, IEnumNetworkConnections **out)
{
	return network_connections(&network, out);
}

static HRESULT WINAPI manager_connection(INetworkListManager *iface, GUID id, INetworkConnection **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!connectivity() || !same(&id, &connection_id))
		return E_INVALIDARG;
	*out = &connection;
	return S_OK;
}

static HRESULT WINAPI manager_internet(INetworkListManager *iface, VARIANT_BOOL *out) { return connected_flag(out); }
static HRESULT WINAPI manager_connected(INetworkListManager *iface, VARIANT_BOOL *out) { return connected_flag(out); }
static HRESULT WINAPI manager_connectivity(INetworkListManager *iface, NLM_CONNECTIVITY *out) { return connectivity_value(out); }

static INetworkListManagerVtbl manager_table = {manager_query, manager_reference, manager_release, manager_count, manager_type,
	manager_names, manager_invoke, manager_networks, manager_network, manager_connections, manager_connection, manager_internet,
	manager_connected, manager_connectivity};

static HRESULT WINAPI cost_query(INetworkCostManager *iface, REFIID iid, void **out)
{
	return manager_query(&from_cost(iface)->iface, iid, out);
}

static ULONG WINAPI cost_reference(INetworkCostManager *iface) { return manager_reference(&from_cost(iface)->iface); }
static ULONG WINAPI cost_release(INetworkCostManager *iface) { return manager_release(&from_cost(iface)->iface); }

static HRESULT WINAPI cost_value(INetworkCostManager *iface, DWORD *cost, NLM_SOCKADDR *destination)
{
	if (!cost)
		return E_POINTER;
	*cost = NLM_CONNECTION_COST_UNRESTRICTED;
	return S_OK;
}

/* No data plan is known: every figure is the unknown value and every time is zero. */
static HRESULT WINAPI cost_plan(INetworkCostManager *iface, NLM_DATAPLAN_STATUS *status, NLM_SOCKADDR *destination)
{
	if (!status)
		return E_POINTER;
	ZeroMemory(status, sizeof *status);
	status->InterfaceGuid = connection_id;
	status->UsageData.UsageInMegabytes = NLM_UNKNOWN_DATAPLAN_STATUS;
	status->DataLimitInMegabytes = NLM_UNKNOWN_DATAPLAN_STATUS;
	status->InboundBandwidthInKbps = NLM_UNKNOWN_DATAPLAN_STATUS;
	status->OutboundBandwidthInKbps = NLM_UNKNOWN_DATAPLAN_STATUS;
	status->MaxTransferSizeInMegabytes = NLM_UNKNOWN_DATAPLAN_STATUS;
	return S_OK;
}

static HRESULT WINAPI cost_destinations(INetworkCostManager *iface, UINT32 count, NLM_SOCKADDR *addresses, VARIANT_BOOL append)
{
	return count && !addresses ? E_POINTER : S_OK;
}

static INetworkCostManagerVtbl cost_table = {cost_query, cost_reference, cost_release, cost_value, cost_plan, cost_destinations};

static HRESULT WINAPI points_query(IConnectionPointContainer *iface, REFIID iid, void **out)
{
	return manager_query(&from_points(iface)->iface, iid, out);
}

static ULONG WINAPI points_reference(IConnectionPointContainer *iface) { return manager_reference(&from_points(iface)->iface); }
static ULONG WINAPI points_release(IConnectionPointContainer *iface) { return manager_release(&from_points(iface)->iface); }

static HRESULT WINAPI points_enumerate(IConnectionPointContainer *iface, IEnumConnectionPoints **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI points_find(IConnectionPointContainer *iface, REFIID iid, IConnectionPoint **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!same(iid, &IID_INetworkListManagerEvents) && !same(iid, &IID_INetworkEvents) && !same(iid, &IID_INetworkConnectionEvents) &&
		!same(iid, &IID_INetworkCostManagerEvents))
		return CONNECT_E_NOCONNECTION;
	point *created = ALLOC(sizeof *created);
	if (!created)
		return E_OUTOFMEMORY;
	created->iface.lpVtbl = &point_table;
	created->refs = 1;
	created->kind = *iid;
	created->container = iface;
	IConnectionPointContainer_AddRef(iface);
	InterlockedIncrement(&objects);
	*out = &created->iface;
	return S_OK;
}

static IConnectionPointContainerVtbl points_table = {points_query, points_reference, points_release, points_enumerate, points_find};

/* The class factory. */
static HRESULT WINAPI factory_query(IClassFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_reference(IClassFactory *iface) { return 2; }
static ULONG WINAPI factory_release(IClassFactory *iface) { return 1; }

static HRESULT WINAPI factory_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	manager *self = ALLOC(sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &manager_table;
	self->cost.lpVtbl = &cost_table;
	self->points.lpVtbl = &points_table;
	self->refs = 1;
	InterlockedIncrement(&objects);
	HRESULT result = manager_query(&self->iface, iid, out);
	manager_release(&self->iface);
	return result;
}

static HRESULT WINAPI factory_lock(IClassFactory *iface, BOOL lock) { return S_OK; }

static IClassFactoryVtbl factory_table = {factory_query, factory_reference, factory_release, factory_create, factory_lock};
static IClassFactory factory = {&factory_table};

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!same(clsid, &CLSID_NetworkListManager))
		return CLASS_E_CLASSNOTAVAILABLE;
	return factory_query(&factory, iid, out);
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
