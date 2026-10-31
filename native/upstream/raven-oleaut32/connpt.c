/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * The connection point behind fonts and pictures.
 */
#include "connpt.h"

struct point
{
	IConnectionPoint IConnectionPoint_iface;
	IUnknown *owner;
	IID iid;
	IUnknown **sinks;
	DWORD room;
	DWORD live;
};

static inline struct point *point_of(IConnectionPoint *iface)
{
	return CONTAINING_RECORD(iface, struct point, IConnectionPoint_iface);
}

struct sink_enum
{
	IEnumConnections IEnumConnections_iface;
	LONG ref;
	IUnknown *owner;
	CONNECTDATA *items;
	ULONG count, at;
};

static inline struct sink_enum *enum_of(IEnumConnections *iface)
{
	return CONTAINING_RECORD(iface, struct sink_enum, IEnumConnections_iface);
}

static HRESULT WINAPI enum_QueryInterface(IEnumConnections *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IEnumConnections))
		return E_NOINTERFACE;
	*out = iface;
	IEnumConnections_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI enum_AddRef(IEnumConnections *iface)
{
	return InterlockedIncrement(&enum_of(iface)->ref);
}

static ULONG WINAPI enum_Release(IEnumConnections *iface)
{
	struct sink_enum *object = enum_of(iface);
	ULONG count = InterlockedDecrement(&object->ref);
	if (!count)
	{
		ULONG index;
		for (index = 0; index < object->count; index++)
			IUnknown_Release(object->items[index].pUnk);
		IUnknown_Release(object->owner);
		HeapFree(GetProcessHeap(), 0, object->items);
		HeapFree(GetProcessHeap(), 0, object);
	}
	return count;
}

static HRESULT WINAPI enum_Next(IEnumConnections *iface, ULONG wanted, CONNECTDATA *out, ULONG *fetched)
{
	struct sink_enum *object = enum_of(iface);
	ULONG given = 0;
	if (!out)
		return E_POINTER;
	while (given < wanted && object->at < object->count)
	{
		out[given] = object->items[object->at++];
		IUnknown_AddRef(out[given].pUnk);
		given++;
	}
	if (fetched)
		*fetched = given;
	return given == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Skip(IEnumConnections *iface, ULONG count)
{
	struct sink_enum *object = enum_of(iface);
	if (object->count - object->at < count)
	{
		object->at = object->count;
		return S_FALSE;
	}
	object->at += count;
	return S_OK;
}

static HRESULT WINAPI enum_Reset(IEnumConnections *iface)
{
	enum_of(iface)->at = 0;
	return S_OK;
}

static HRESULT enum_create(IUnknown *owner, const CONNECTDATA *items, ULONG count, ULONG at, IEnumConnections **out);

static HRESULT WINAPI enum_Clone(IEnumConnections *iface, IEnumConnections **out)
{
	struct sink_enum *object = enum_of(iface);
	return enum_create(object->owner, object->items, object->count, object->at, out);
}

static const IEnumConnectionsVtbl enum_vtbl = {
	enum_QueryInterface,
	enum_AddRef,
	enum_Release,
	enum_Next,
	enum_Skip,
	enum_Reset,
	enum_Clone,
};

static HRESULT enum_create(IUnknown *owner, const CONNECTDATA *items, ULONG count, ULONG at, IEnumConnections **out)
{
	struct sink_enum *object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
	ULONG index;
	if (!object)
		return E_OUTOFMEMORY;
	object->items = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (count + 1) * sizeof(*object->items));
	if (!object->items)
	{
		HeapFree(GetProcessHeap(), 0, object);
		return E_OUTOFMEMORY;
	}
	object->IEnumConnections_iface.lpVtbl = &enum_vtbl;
	object->ref = 1;
	object->owner = owner;
	IUnknown_AddRef(owner);
	object->count = count;
	object->at = at;
	for (index = 0; index < count; index++)
	{
		object->items[index] = items[index];
		IUnknown_AddRef(items[index].pUnk);
	}
	*out = &object->IEnumConnections_iface;
	return S_OK;
}

static HRESULT WINAPI point_QueryInterface(IConnectionPoint *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IConnectionPoint))
		return E_NOINTERFACE;
	*out = iface;
	IConnectionPoint_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI point_AddRef(IConnectionPoint *iface)
{
	return IUnknown_AddRef(point_of(iface)->owner);
}

static ULONG WINAPI point_Release(IConnectionPoint *iface)
{
	return IUnknown_Release(point_of(iface)->owner);
}

static HRESULT WINAPI point_GetConnectionInterface(IConnectionPoint *iface, IID *out)
{
	if (!out)
		return E_POINTER;
	*out = point_of(iface)->iid;
	return S_OK;
}

static HRESULT WINAPI point_GetConnectionPointContainer(IConnectionPoint *iface, IConnectionPointContainer **out)
{
	return IUnknown_QueryInterface(point_of(iface)->owner, &IID_IConnectionPointContainer, (void **)out);
}

static HRESULT WINAPI point_Advise(IConnectionPoint *iface, IUnknown *sink, DWORD *cookie)
{
	struct point *object = point_of(iface);
	IUnknown *held = NULL;
	DWORD index;
	if (!sink || !cookie)
		return E_POINTER;
	*cookie = 0;
	if (FAILED(IUnknown_QueryInterface(sink, &object->iid, (void **)&held)))
		return CONNECT_E_CANNOTCONNECT;
	for (index = 0; index < object->room; index++)
		if (!object->sinks[index])
			break;
	if (index == object->room)
	{
		DWORD room = object->room ? object->room * 2 : 4;
		IUnknown **sinks = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, room * sizeof(*sinks));
		if (!sinks)
		{
			IUnknown_Release(held);
			return E_OUTOFMEMORY;
		}
		if (object->room)
			memcpy(sinks, object->sinks, object->room * sizeof(*sinks));
		HeapFree(GetProcessHeap(), 0, object->sinks);
		object->sinks = sinks;
		object->room = room;
	}
	object->sinks[index] = held;
	object->live++;
	*cookie = index + 1;
	return S_OK;
}

static HRESULT WINAPI point_Unadvise(IConnectionPoint *iface, DWORD cookie)
{
	struct point *object = point_of(iface);
	if (!cookie || cookie > object->room || !object->sinks[cookie - 1])
		return CONNECT_E_NOCONNECTION;
	IUnknown_Release(object->sinks[cookie - 1]);
	object->sinks[cookie - 1] = NULL;
	object->live--;
	return S_OK;
}

static HRESULT WINAPI point_EnumConnections(IConnectionPoint *iface, IEnumConnections **out)
{
	struct point *object = point_of(iface);
	CONNECTDATA *items;
	DWORD index, count = 0;
	HRESULT status;
	if (!out)
		return E_POINTER;
	*out = NULL;
	items = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (object->live + 1) * sizeof(*items));
	if (!items)
		return E_OUTOFMEMORY;
	for (index = 0; index < object->room; index++)
		if (object->sinks[index])
		{
			items[count].pUnk = object->sinks[index];
			items[count].dwCookie = index + 1;
			count++;
		}
	status = enum_create((IUnknown *)iface, items, count, 0, out);
	HeapFree(GetProcessHeap(), 0, items);
	return status;
}

static const IConnectionPointVtbl point_vtbl = {
	point_QueryInterface,
	point_AddRef,
	point_Release,
	point_GetConnectionInterface,
	point_GetConnectionPointContainer,
	point_Advise,
	point_Unadvise,
	point_EnumConnections,
};

HRESULT oa_connection_point_create(IUnknown *owner, REFIID iid, IConnectionPoint **out)
{
	struct point *object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
	if (!object)
		return E_OUTOFMEMORY;
	object->IConnectionPoint_iface.lpVtbl = &point_vtbl;
	object->owner = owner;
	object->iid = *iid;
	*out = &object->IConnectionPoint_iface;
	return S_OK;
}

/* The owner destroys its points directly, since their references were never their own. */
void oa_connection_point_each(IConnectionPoint *iface, oa_sink_call call, void *context)
{
	struct point *object = point_of(iface);
	DWORD index;
	for (index = 0; index < object->room; index++)
		if (object->sinks[index])
		{
			IUnknown *sink = object->sinks[index];
			IUnknown_AddRef(sink);
			call(sink, context);
			IUnknown_Release(sink);
		}
}

void oa_connection_point_destroy(IConnectionPoint *iface)
{
	struct point *object = point_of(iface);
	DWORD index;
	for (index = 0; index < object->room; index++)
		if (object->sinks[index])
			IUnknown_Release(object->sinks[index]);
	HeapFree(GetProcessHeap(), 0, object->sinks);
	HeapFree(GetProcessHeap(), 0, object);
}
