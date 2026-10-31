/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * Class objects for the standard font and picture.
 */
#include "oa.h"

HRESULT oa_create_std_font(REFIID riid, void **out);
HRESULT oa_create_std_picture(REFIID riid, void **out);

struct factory
{
	IClassFactory IClassFactory_iface;
	HRESULT (*create)(REFIID riid, void **out);
};

static inline struct factory *factory_of(IClassFactory *iface)
{
	return CONTAINING_RECORD(iface, struct factory, IClassFactory_iface);
}

static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IClassFactory))
		return E_NOINTERFACE;
	*out = iface;
	return S_OK;
}

static ULONG WINAPI factory_AddRef(IClassFactory *iface)
{
	return 2;
}

static ULONG WINAPI factory_Release(IClassFactory *iface)
{
	return 1;
}

static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return factory_of(iface)->create(riid, out);
}

static HRESULT WINAPI factory_LockServer(IClassFactory *iface, BOOL lock)
{
	return S_OK;
}

static const IClassFactoryVtbl factory_vtbl = {
	factory_QueryInterface,
	factory_AddRef,
	factory_Release,
	factory_CreateInstance,
	factory_LockServer,
};

static struct factory font_factory = {{&factory_vtbl}, oa_create_std_font};
static struct factory picture_factory = {{&factory_vtbl}, oa_create_std_picture};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualCLSID(clsid, &CLSID_StdFont))
		return IClassFactory_QueryInterface(&font_factory.IClassFactory_iface, riid, out);
	if (IsEqualCLSID(clsid, &CLSID_StdPicture))
		return IClassFactory_QueryInterface(&picture_factory.IClassFactory_iface, riid, out);
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return S_FALSE;
}
