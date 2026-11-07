/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven deployment component: the Windows.Management.Deployment.PackageManager class of the Windows Runtime, which programs ask which
 * packaged applications are installed, before they install a desktop version of one.
 *
 * Raven installs no packaged applications, so every query finds none: a collection of packages or users is empty, and a package asked
 * for by its full name is null, as Windows answers when that package is absent. Deploying, staging or removing packages is not
 * supported. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winstring.h>
#include <activation.h>
#include <windows.management.deployment.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

typedef __x_ABI_CWindows_CManagement_CDeployment_CIPackageManager package_manager;
typedef __FIIterable_1_Windows__CApplicationModel__CPackage package_iterable;
typedef __FIAsyncOperationWithProgress_2_Windows__CManagement__CDeployment__CDeploymentResult_DeploymentProgress deployment_operation;

static const WCHAR class_name[] = L"Windows.Management.Deployment.PackageManager";
static LONG objects;

/* The Windows Runtime methods every object has; the interfaces here are not enumerated and all run with full trust. */
static HRESULT inspectable_iids(ULONG *count, IID **iids)
{
	if (!count || !iids)
		return E_POINTER;
	*count = 0;
	*iids = NULL;
	return S_OK;
}

static HRESULT inspectable_trust(TrustLevel *level)
{
	if (!level)
		return E_POINTER;
	*level = BaseTrust;
	return S_OK;
}

/* An empty collection, of packages or of users, and the iterator it hands out; both have the same methods for every element type, as
 * none is ever returned. */
struct empty {
	const void *table;
	LONG refs;
	const IID *kind;
	const WCHAR *name;
};

typedef struct empty empty;

static HRESULT WINAPI empty_query(empty *self, REFIID iid, void **out);
static ULONG WINAPI empty_reference(empty *self) { return (ULONG)InterlockedIncrement(&self->refs); }

static ULONG WINAPI empty_release(empty *self)
{
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return (ULONG)refs;
}

static HRESULT WINAPI empty_iids(empty *self, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI empty_name(empty *self, HSTRING *name) { return WindowsCreateString(self->name, lstrlenW(self->name), name); }
static HRESULT WINAPI empty_trust(empty *self, TrustLevel *level) { return inspectable_trust(level); }

static HRESULT make_empty(BOOL iterator, const IID *kind, const WCHAR *name, void **out);

static HRESULT WINAPI empty_first(empty *self, void **out)
{
	static const struct { const IID *iterable, *iterator; } pairs[] = {
		{&IID___FIIterable_1_Windows__CApplicationModel__CPackage, &IID___FIIterator_1_Windows__CApplicationModel__CPackage},
		{&IID___FIIterable_1_Windows__CManagement__CDeployment__CPackageUserInformation,
			&IID___FIIterator_1_Windows__CManagement__CDeployment__CPackageUserInformation}};
	static const WCHAR *names[] = {L"Windows.Foundation.Collections.IIterator`1<Windows.ApplicationModel.Package>",
		L"Windows.Foundation.Collections.IIterator`1<Windows.Management.Deployment.PackageUserInformation>"};
	if (!out)
		return E_POINTER;
	const int index = IsEqualIID(self->kind, pairs[0].iterable) ? 0 : 1;
	return make_empty(TRUE, pairs[index].iterator, names[index], out);
}

static HRESULT WINAPI empty_current(empty *self, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return E_BOUNDS;
}

static HRESULT WINAPI empty_has_current(empty *self, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = FALSE;
	return S_OK;
}

static HRESULT WINAPI empty_many(empty *self, UINT32 capacity, void **items, UINT32 *count)
{
	if (!count)
		return E_POINTER;
	*count = 0;
	return S_OK;
}

static const struct {
	void *query, *reference, *release, *iids, *name, *trust, *first;
} iterable_table = {empty_query, empty_reference, empty_release, empty_iids, empty_name, empty_trust, empty_first};

static const struct {
	void *query, *reference, *release, *iids, *name, *trust, *current, *has_current, *next, *many;
} iterator_table = {empty_query, empty_reference, empty_release, empty_iids, empty_name, empty_trust, empty_current, empty_has_current,
	empty_has_current, empty_many};

static HRESULT WINAPI empty_query(empty *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject) ||
		IsEqualIID(iid, self->kind)) {
		empty_reference(self);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT make_empty(BOOL iterator, const IID *kind, const WCHAR *name, void **out)
{
	empty *self = ALLOC(sizeof *self);
	*out = NULL;
	if (!self)
		return E_OUTOFMEMORY;
	self->table = iterator ? (const void *)&iterator_table : (const void *)&iterable_table;
	self->refs = 1;
	self->kind = kind;
	self->name = name;
	InterlockedIncrement(&objects);
	*out = self;
	return S_OK;
}

static HRESULT no_packages(package_iterable **out)
{
	if (!out)
		return E_POINTER;
	return make_empty(FALSE, &IID___FIIterable_1_Windows__CApplicationModel__CPackage,
		L"Windows.Foundation.Collections.IIterable`1<Windows.ApplicationModel.Package>", (void **)out);
}

/* The package manager. */
struct manager {
	package_manager iface;
	LONG refs;
};

static HRESULT WINAPI manager_query(package_manager *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject) ||
		IsEqualIID(iid, &IID___x_ABI_CWindows_CManagement_CDeployment_CIPackageManager)) {
		__x_ABI_CWindows_CManagement_CDeployment_CIPackageManager_AddRef(iface);
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI manager_reference(package_manager *iface) { return (ULONG)InterlockedIncrement(&((struct manager *)iface)->refs); }

static ULONG WINAPI manager_release(package_manager *iface)
{
	struct manager *self = (struct manager *)iface;
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return (ULONG)refs;
}

static HRESULT WINAPI manager_iids(package_manager *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI manager_name(package_manager *iface, HSTRING *name) { return WindowsCreateString(class_name, lstrlenW(class_name), name); }
static HRESULT WINAPI manager_trust(package_manager *iface, TrustLevel *level) { return inspectable_trust(level); }

static HRESULT unsupported(deployment_operation **operation)
{
	if (operation)
		*operation = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI manager_add(package_manager *iface, __x_ABI_CWindows_CFoundation_CIUriRuntimeClass *uri,
	__FIIterable_1_Windows__CFoundation__CUri *dependencies, __x_ABI_CWindows_CManagement_CDeployment_CDeploymentOptions options,
	deployment_operation **operation)
{
	return unsupported(operation);
}

static HRESULT WINAPI manager_remove(package_manager *iface, HSTRING name, deployment_operation **operation) { return unsupported(operation); }

static HRESULT WINAPI manager_stage(package_manager *iface, __x_ABI_CWindows_CFoundation_CIUriRuntimeClass *uri,
	__FIIterable_1_Windows__CFoundation__CUri *dependencies, deployment_operation **operation)
{
	return unsupported(operation);
}

static HRESULT WINAPI manager_packages(package_manager *iface, package_iterable **packages) { return no_packages(packages); }
static HRESULT WINAPI manager_packages_one(package_manager *iface, HSTRING first, package_iterable **packages) { return no_packages(packages); }

static HRESULT WINAPI manager_packages_two(package_manager *iface, HSTRING first, HSTRING second, package_iterable **packages)
{
	return no_packages(packages);
}

static HRESULT WINAPI manager_packages_three(package_manager *iface, HSTRING sid, HSTRING name, HSTRING publisher, package_iterable **packages)
{
	return no_packages(packages);
}

static HRESULT WINAPI manager_users(package_manager *iface, HSTRING name, __FIIterable_1_Windows__CManagement__CDeployment__CPackageUserInformation **users)
{
	if (!users)
		return E_POINTER;
	return make_empty(FALSE, &IID___FIIterable_1_Windows__CManagement__CDeployment__CPackageUserInformation,
		L"Windows.Foundation.Collections.IIterable`1<Windows.Management.Deployment.PackageUserInformation>", (void **)users);
}

static HRESULT WINAPI manager_state(package_manager *iface, HSTRING name, __x_ABI_CWindows_CManagement_CDeployment_CPackageState state)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI manager_package(package_manager *iface, HSTRING name, __x_ABI_CWindows_CApplicationModel_CIPackage **package)
{
	if (!package)
		return E_POINTER;
	*package = NULL;
	return S_OK;
}

static HRESULT WINAPI manager_cleanup(package_manager *iface, HSTRING name, HSTRING sid, deployment_operation **operation)
{
	return unsupported(operation);
}

static HRESULT WINAPI manager_user_package(package_manager *iface, HSTRING sid, HSTRING name, __x_ABI_CWindows_CApplicationModel_CIPackage **package)
{
	return manager_package(iface, name, package);
}

static const __x_ABI_CWindows_CManagement_CDeployment_CIPackageManagerVtbl manager_table = {manager_query, manager_reference, manager_release,
	manager_iids, manager_name, manager_trust, manager_add, manager_add, manager_remove, manager_stage, manager_add, manager_packages,
	manager_packages_one, manager_packages_two, manager_packages_three, manager_users, manager_state, manager_package, manager_cleanup,
	manager_packages_one, manager_packages_two, manager_user_package};

/* The activation factory of the class: each activation makes a new manager. */
static HRESULT WINAPI factory_query(IActivationFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject) ||
		IsEqualIID(iid, &IID_IActivationFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_reference(IActivationFactory *iface) { return 2; }
static ULONG WINAPI factory_release(IActivationFactory *iface) { return 1; }
static HRESULT WINAPI factory_iids(IActivationFactory *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI factory_name(IActivationFactory *iface, HSTRING *name) { return WindowsCreateString(class_name, lstrlenW(class_name), name); }
static HRESULT WINAPI factory_trust(IActivationFactory *iface, TrustLevel *level) { return inspectable_trust(level); }

static HRESULT WINAPI factory_activate(IActivationFactory *iface, IInspectable **out)
{
	struct manager *self;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!(self = ALLOC(sizeof *self)))
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = (__x_ABI_CWindows_CManagement_CDeployment_CIPackageManagerVtbl *)&manager_table;
	self->refs = 1;
	InterlockedIncrement(&objects);
	*out = (IInspectable *)&self->iface;
	return S_OK;
}

static IActivationFactoryVtbl factory_table = {factory_query, factory_reference, factory_release, factory_iids, factory_name, factory_trust,
	factory_activate};
static IActivationFactory factory = {&factory_table};

__declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
	UINT32 length = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
	if (length != ARRAYSIZE(class_name) - 1 || CompareStringOrdinal(text, (int)length, class_name, -1, FALSE) != CSTR_EQUAL)
		return CLASS_E_CLASSNOTAVAILABLE;
	*out = &factory;
	return S_OK;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
