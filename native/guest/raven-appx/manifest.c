/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The manifest of a package (AppxManifest.xml): its identity, from which the package's full and family names follow, its properties,
 * dependencies, target device families, capabilities, resources, prerequisites and applications. Elements are found by their local
 * names, whichever of the manifest schemas (2010 to Windows 10's foundation and uap namespaces) they come from; the manifest is not
 * validated against the schemas as Windows does.
 *
 * Not provided yet (E_NOTIMPL): qualified resources (IAppxManifestReader2), capabilities by class (IAppxManifestReader3), and the
 * readers 4 to 7 (optional package information, main and driver dependencies, host runtimes, IsNonQualifiedResourcePackage). */
#include "appx.h"

static const WCHAR *const package_namespaces[] = {L"http://schemas.microsoft.com/appx/2010/manifest",
	L"http://schemas.microsoft.com/appx/manifest/foundation/windows10"};

typedef struct manifest {
	IAppxManifestReader3 iface;
	LONG refs;
	appx_document document;
	BYTE *data;
	UINT64 size;
	const appx_node *identity;
	UINT64 version;
	WCHAR full_name[256], family_name[256];
} manifest;

/* The objects a manifest hands out hold a reference to it: their strings are its own. */
static void hold(manifest *owner) { InterlockedIncrement(&owner->refs); }
static void let_go(manifest *owner) { owner->iface.lpVtbl->Release(&owner->iface); }

/* A version of four parts, each at most 65535 (major.minor.build.revision), packed in 64 bits as PACKAGE_VERSION is. */
static BOOL parse_version(const WCHAR *text, UINT64 *version)
{
	if (!text)
		return FALSE;
	UINT64 result = 0;
	for (int part = 0; part < 4; ++part) {
		UINT32 value = 0;
		const WCHAR *start = text;
		while (*text >= '0' && *text <= '9') {
			value = value * 10 + (*text++ - '0');
			if (value > 0xffff)
				return FALSE;
		}
		if (text == start || (part < 3 ? *text != '.' : *text != 0))
			return FALSE;
		if (part < 3)
			++text;
		result = result << 16 | value;
	}
	*version = result;
	return TRUE;
}

static const struct {
	const WCHAR *name;
	APPX_PACKAGE_ARCHITECTURE architecture;
	APPX_PACKAGE_ARCHITECTURE2 architecture2;
} architectures[] = {{L"x86", APPX_PACKAGE_ARCHITECTURE_X86, APPX_PACKAGE_ARCHITECTURE2_X86},
	{L"x64", APPX_PACKAGE_ARCHITECTURE_X64, APPX_PACKAGE_ARCHITECTURE2_X64}, {L"arm", APPX_PACKAGE_ARCHITECTURE_ARM, APPX_PACKAGE_ARCHITECTURE2_ARM},
	{L"arm64", APPX_PACKAGE_ARCHITECTURE_ARM64, APPX_PACKAGE_ARCHITECTURE2_ARM64},
	{L"neutral", APPX_PACKAGE_ARCHITECTURE_NEUTRAL, APPX_PACKAGE_ARCHITECTURE2_NEUTRAL},
	/* An x86 package for ARM64 machines: x86 to callers that know only the first set. */
	{L"x86a64", APPX_PACKAGE_ARCHITECTURE_X86, APPX_PACKAGE_ARCHITECTURE2_X86_ON_ARM64}};

static int architecture_index(const WCHAR *name)
{
	for (int index = 0; index < (int)ARRAYSIZE(architectures); ++index)
		if (!lstrcmpW(name ? name : L"neutral", architectures[index].name))
			return index;
	return -1;
}

HRESULT appx_publisher_id(const WCHAR *publisher, WCHAR id[14])
{
	static const char alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
	const appx_hash sha256 = {BCRYPT_SHA256_ALG_HANDLE, 32};
	BYTE digest[32];
	HRESULT result = appx_hash_data(&sha256, (const BYTE *)publisher, lstrlenW(publisher) * sizeof(WCHAR), digest);
	if (FAILED(result))
		return result;
	UINT64 bits = 0;
	for (int index = 0; index < 8; ++index)
		bits = bits << 8 | digest[index];
	/* 64 bits are 13 characters of five bits, the last padded with a zero bit. */
	for (int index = 0; index < 13; ++index) {
		const int shift = 64 - 5 * (index + 1);
		id[index] = alphabet[(shift >= 0 ? bits >> shift : bits << -shift) & 31];
	}
	id[13] = 0;
	return S_OK;
}

static void append(WCHAR *target, SIZE_T capacity, const WCHAR *text)
{
	SIZE_T length = lstrlenW(target);
	while (*text && length + 1 < capacity)
		target[length++] = *text++;
	target[length] = 0;
}

/* ---- The package identity ---- */

typedef struct identity {
	IAppxManifestPackageId2 iface;
	LONG refs;
	manifest *owner;
	const appx_node *node;
	const WCHAR *full_name, *family_name;
	UINT64 version;
} identity;

static void identity_destroy(identity *self)
{
	let_go(self->owner);
	FREE(self);
}

APPX_UNKNOWN(identity, IAppxManifestPackageId2, identity, appx_same(iid, &IID_IAppxManifestPackageId) || appx_same(iid, &IID_IAppxManifestPackageId2),
	identity_destroy)

static identity *from_identity(IAppxManifestPackageId2 *iface) { return (identity *)iface; }

static HRESULT WINAPI identity_name(IAppxManifestPackageId2 *iface, LPWSTR *name)
{
	return appx_task_string(appx_attribute_value(from_identity(iface)->node, L"Name"), name);
}

static HRESULT WINAPI identity_architecture(IAppxManifestPackageId2 *iface, APPX_PACKAGE_ARCHITECTURE *architecture)
{
	if (!architecture)
		return E_POINTER;
	*architecture = architectures[architecture_index(appx_attribute_value(from_identity(iface)->node, L"ProcessorArchitecture"))].architecture;
	return S_OK;
}

static HRESULT WINAPI identity_publisher(IAppxManifestPackageId2 *iface, LPWSTR *publisher)
{
	return appx_task_string(appx_attribute_value(from_identity(iface)->node, L"Publisher"), publisher);
}

static HRESULT WINAPI identity_version(IAppxManifestPackageId2 *iface, UINT64 *version)
{
	if (!version)
		return E_POINTER;
	*version = from_identity(iface)->version;
	return S_OK;
}

/* A package without a resource ID gives NULL. */
static HRESULT WINAPI identity_resource(IAppxManifestPackageId2 *iface, LPWSTR *resource)
{
	return appx_task_string(appx_attribute_value(from_identity(iface)->node, L"ResourceId"), resource);
}

static HRESULT WINAPI identity_compare_publisher(IAppxManifestPackageId2 *iface, LPCWSTR other, BOOL *same)
{
	if (!other || !same)
		return E_POINTER;
	*same = !lstrcmpW(appx_attribute_value(from_identity(iface)->node, L"Publisher"), other);
	return S_OK;
}

static HRESULT WINAPI identity_full_name(IAppxManifestPackageId2 *iface, LPWSTR *name)
{
	return appx_task_string(from_identity(iface)->full_name, name);
}

static HRESULT WINAPI identity_family_name(IAppxManifestPackageId2 *iface, LPWSTR *name)
{
	return appx_task_string(from_identity(iface)->family_name, name);
}

static HRESULT WINAPI identity_architecture2(IAppxManifestPackageId2 *iface, APPX_PACKAGE_ARCHITECTURE2 *architecture)
{
	if (!architecture)
		return E_POINTER;
	*architecture = architectures[architecture_index(appx_attribute_value(from_identity(iface)->node, L"ProcessorArchitecture"))].architecture2;
	return S_OK;
}

static const IAppxManifestPackageId2Vtbl identity_table = {identity_query, identity_add_ref, identity_release, identity_name, identity_architecture,
	identity_publisher, identity_version, identity_resource, identity_compare_publisher, identity_full_name, identity_family_name,
	identity_architecture2};

/* ---- Properties ---- */

typedef struct properties {
	IAppxManifestProperties iface;
	LONG refs;
	manifest *owner;
	const appx_node *node;
} properties;

static void properties_destroy(properties *self)
{
	let_go(self->owner);
	FREE(self);
}

APPX_UNKNOWN(properties, IAppxManifestProperties, properties, appx_same(iid, &IID_IAppxManifestProperties), properties_destroy)

/* The text of a property element, trimmed of the white space around it. */
static const WCHAR *property_text(const appx_node *node, const WCHAR *name, WCHAR *buffer, SIZE_T capacity)
{
	const appx_node *element = appx_child(node, name, NULL);
	if (!element || !element->text)
		return NULL;
	const WCHAR *text = element->text;
	while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
		++text;
	SIZE_T length = lstrlenW(text);
	while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r' || text[length - 1] == '\n'))
		--length;
	if (length >= capacity)
		length = capacity - 1;
	memcpy(buffer, text, length * sizeof(WCHAR));
	buffer[length] = 0;
	return buffer;
}

static HRESULT WINAPI properties_bool(IAppxManifestProperties *iface, LPCWSTR name, BOOL *value)
{
	properties *self = (properties *)iface;
	if (!name || !value)
		return E_POINTER;
	*value = FALSE;
	if (lstrcmpW(name, L"Framework") && lstrcmpW(name, L"ResourcePackage"))
		return E_INVALIDARG;
	WCHAR buffer[16];
	const WCHAR *text = property_text(self->node, name, buffer, ARRAYSIZE(buffer));
	*value = text && (!lstrcmpW(text, L"true") || !lstrcmpW(text, L"1"));
	return S_OK;
}

static HRESULT WINAPI properties_string(IAppxManifestProperties *iface, LPCWSTR name, LPWSTR *value)
{
	properties *self = (properties *)iface;
	if (!name || !value)
		return E_POINTER;
	*value = NULL;
	if (lstrcmpW(name, L"DisplayName") && lstrcmpW(name, L"PublisherDisplayName") && lstrcmpW(name, L"Description") && lstrcmpW(name, L"Logo"))
		return E_INVALIDARG;
	const appx_node *element = appx_child(self->node, name, NULL);
	if (!element || !element->text)
		return S_OK;
	SIZE_T capacity = lstrlenW(element->text) + 1;
	WCHAR *buffer = ALLOC(capacity * sizeof(WCHAR));
	if (!buffer)
		return E_OUTOFMEMORY;
	const HRESULT result = appx_task_string(property_text(self->node, name, buffer, capacity), value);
	FREE(buffer);
	return result;
}

static const IAppxManifestPropertiesVtbl properties_table = {properties_query, properties_add_ref, properties_release, properties_bool,
	properties_string};

/* ---- Dependencies and target device families ---- */

/* A package dependency (Name, Publisher, MinVersion) and a target device family (Name, MinVersion, MaxVersionTested) are both an element
 * read for its attributes; one object serves the two interfaces. */
typedef struct dependency {
	union {
		IAppxManifestPackageDependency package;
		IAppxManifestTargetDeviceFamily family;
	} iface;
	LONG refs;
	manifest *owner;
	const appx_node *node;
	IID iid;
} dependency;

static void dependency_destroy(dependency *self)
{
	let_go(self->owner);
	FREE(self);
}

APPX_UNKNOWN(package_dependency, IAppxManifestPackageDependency, dependency, appx_same(iid, &((dependency *)iface)->iid), dependency_destroy)

static HRESULT dependency_string(IUnknown *iface, const WCHAR *name, LPWSTR *out)
{
	return appx_task_string(appx_attribute_value(((dependency *)iface)->node, name), out);
}

static HRESULT dependency_version(IUnknown *iface, const WCHAR *name, UINT64 *out)
{
	if (!out)
		return E_POINTER;
	*out = 0;
	const WCHAR *text = appx_attribute_value(((dependency *)iface)->node, name);
	return !text || parse_version(text, out) ? S_OK : APPX_E_INVALID_MANIFEST;
}

static HRESULT WINAPI package_dependency_name(IAppxManifestPackageDependency *iface, LPWSTR *name) { return dependency_string((IUnknown *)iface, L"Name", name); }
static HRESULT WINAPI package_dependency_publisher(IAppxManifestPackageDependency *iface, LPWSTR *publisher)
{
	return dependency_string((IUnknown *)iface, L"Publisher", publisher);
}
static HRESULT WINAPI package_dependency_min_version(IAppxManifestPackageDependency *iface, UINT64 *version)
{
	return dependency_version((IUnknown *)iface, L"MinVersion", version);
}

static const IAppxManifestPackageDependencyVtbl package_dependency_table = {package_dependency_query, package_dependency_add_ref,
	package_dependency_release, package_dependency_name, package_dependency_publisher, package_dependency_min_version};

static HRESULT WINAPI family_query(IAppxManifestTargetDeviceFamily *iface, REFIID iid, void **out)
{
	return package_dependency_query((IAppxManifestPackageDependency *)iface, iid, out);
}
static ULONG WINAPI family_add_ref(IAppxManifestTargetDeviceFamily *iface) { return package_dependency_add_ref((IAppxManifestPackageDependency *)iface); }
static ULONG WINAPI family_release(IAppxManifestTargetDeviceFamily *iface) { return package_dependency_release((IAppxManifestPackageDependency *)iface); }
static HRESULT WINAPI family_name(IAppxManifestTargetDeviceFamily *iface, LPWSTR *name) { return dependency_string((IUnknown *)iface, L"Name", name); }
static HRESULT WINAPI family_min_version(IAppxManifestTargetDeviceFamily *iface, UINT64 *version)
{
	return dependency_version((IUnknown *)iface, L"MinVersion", version);
}
static HRESULT WINAPI family_max_version(IAppxManifestTargetDeviceFamily *iface, UINT64 *version)
{
	return dependency_version((IUnknown *)iface, L"MaxVersionTested", version);
}

static const IAppxManifestTargetDeviceFamilyVtbl family_table = {family_query, family_add_ref, family_release, family_name, family_min_version,
	family_max_version};

/* ---- Applications ---- */

typedef struct application {
	IAppxManifestApplication iface;
	LONG refs;
	manifest *owner;
	const appx_node *node;
} application;

static void application_destroy(application *self)
{
	let_go(self->owner);
	FREE(self);
}

APPX_UNKNOWN(application, IAppxManifestApplication, application, appx_same(iid, &IID_IAppxManifestApplication), application_destroy)

/* An application's attributes, and those of its visual elements (DisplayName, Description, BackgroundColor, the logos...). */
static HRESULT WINAPI application_string(IAppxManifestApplication *iface, LPCWSTR name, LPWSTR *value)
{
	application *self = (application *)iface;
	if (!name || !value)
		return E_POINTER;
	const WCHAR *text = appx_attribute_value(self->node, name);
	if (!text)
		text = appx_attribute_value(appx_child(self->node, L"VisualElements", NULL), name);
	return appx_task_string(text, value);
}

/* The application user model ID: the package family name, '!', the application's ID. */
static HRESULT WINAPI application_model_id(IAppxManifestApplication *iface, LPWSTR *value)
{
	application *self = (application *)iface;
	if (!value)
		return E_POINTER;
	WCHAR id[512];
	id[0] = 0;
	append(id, ARRAYSIZE(id), self->owner->family_name);
	append(id, ARRAYSIZE(id), L"!");
	const WCHAR *application_id = appx_attribute_value(self->node, L"Id");
	append(id, ARRAYSIZE(id), application_id ? application_id : L"");
	return appx_task_string(id, value);
}

static const IAppxManifestApplicationVtbl application_table = {application_query, application_add_ref, application_release, application_string,
	application_model_id};

/* ---- The reader ---- */

static manifest *from_reader(IAppxManifestReader3 *iface) { return (manifest *)iface; }

static void manifest_destroy(manifest *self)
{
	appx_document_free(&self->document);
	FREE(self->data);
	FREE(self);
}

APPX_UNKNOWN(manifest, IAppxManifestReader3, manifest,
	appx_same(iid, &IID_IAppxManifestReader) || appx_same(iid, &IID_IAppxManifestReader2) || appx_same(iid, &IID_IAppxManifestReader3), manifest_destroy)

static HRESULT WINAPI manifest_package_id(IAppxManifestReader3 *iface, IAppxManifestPackageId **out)
{
	manifest *self = from_reader(iface);
	if (!out)
		return E_POINTER;
	identity *made = ALLOC(sizeof *made);
	if (!made)
		return E_OUTOFMEMORY;
	made->iface.lpVtbl = &identity_table;
	made->refs = 1;
	hold(made->owner = self);
	made->node = self->identity;
	made->full_name = self->full_name;
	made->family_name = self->family_name;
	made->version = self->version;
	InterlockedIncrement(&appx_objects);
	*out = (IAppxManifestPackageId *)&made->iface;
	return S_OK;
}

static HRESULT WINAPI manifest_properties(IAppxManifestReader3 *iface, IAppxManifestProperties **out)
{
	manifest *self = from_reader(iface);
	if (!out)
		return E_POINTER;
	properties *made = ALLOC(sizeof *made);
	if (!made)
		return E_OUTOFMEMORY;
	made->iface.lpVtbl = &properties_table;
	made->refs = 1;
	hold(made->owner = self);
	made->node = appx_child(self->document.root, L"Properties", NULL);
	InterlockedIncrement(&appx_objects);
	*out = &made->iface;
	return S_OK;
}

/* An enumerator over the child elements of a parent's that have a name, each as an object made by `make`. */
static HRESULT enumerate(manifest *self, const appx_node *parent, const WCHAR *name, const IID *iid, HRESULT (*make)(manifest *, const appx_node *, IUnknown **),
	void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	UINT32 count = 0;
	for (const appx_node *node = appx_child(parent, name, NULL); node; node = appx_child(parent, name, node))
		++count;
	IUnknown **items = ALLOC(sizeof *items * (count ? count : 1));
	if (!items)
		return E_OUTOFMEMORY;
	HRESULT result = S_OK;
	UINT32 made = 0;
	for (const appx_node *node = appx_child(parent, name, NULL); node && SUCCEEDED(result = make(self, node, &items[made])); node = appx_child(parent, name, node))
		++made;
	if (SUCCEEDED(result))
		result = appx_enumerator(iid, items, made, out);
	for (UINT32 index = 0; index < made; ++index)
		IUnknown_Release(items[index]);
	FREE(items);
	return result;
}

static HRESULT make_dependency(manifest *self, const appx_node *node, const IID *iid, const void *table, IUnknown **out)
{
	dependency *made = ALLOC(sizeof *made);
	if (!made)
		return E_OUTOFMEMORY;
	made->iface.package.lpVtbl = (const IAppxManifestPackageDependencyVtbl *)table;
	made->refs = 1;
	hold(made->owner = self);
	made->node = node;
	made->iid = *iid;
	InterlockedIncrement(&appx_objects);
	*out = (IUnknown *)&made->iface;
	return S_OK;
}

static HRESULT make_package_dependency(manifest *self, const appx_node *node, IUnknown **out)
{
	return make_dependency(self, node, &IID_IAppxManifestPackageDependency, &package_dependency_table, out);
}

static HRESULT make_family(manifest *self, const appx_node *node, IUnknown **out)
{
	return make_dependency(self, node, &IID_IAppxManifestTargetDeviceFamily, &family_table, out);
}

static HRESULT make_application(manifest *self, const appx_node *node, IUnknown **out)
{
	application *made = ALLOC(sizeof *made);
	if (!made)
		return E_OUTOFMEMORY;
	made->iface.lpVtbl = &application_table;
	made->refs = 1;
	hold(made->owner = self);
	made->node = node;
	InterlockedIncrement(&appx_objects);
	*out = (IUnknown *)&made->iface;
	return S_OK;
}

static HRESULT WINAPI manifest_dependencies(IAppxManifestReader3 *iface, IAppxManifestPackageDependenciesEnumerator **out)
{
	manifest *self = from_reader(iface);
	return enumerate(self, appx_child(self->document.root, L"Dependencies", NULL), L"PackageDependency", &IID_IAppxManifestPackageDependenciesEnumerator,
		make_package_dependency, (void **)out);
}

/* The general capabilities of Windows 8, as flags. */
static HRESULT WINAPI manifest_capabilities(IAppxManifestReader3 *iface, APPX_CAPABILITIES *out)
{
	manifest *self = from_reader(iface);
	if (!out)
		return E_POINTER;
	static const struct {
		const WCHAR *name;
		APPX_CAPABILITIES flag;
	} known[] = {{L"internetClient", APPX_CAPABILITY_INTERNET_CLIENT}, {L"internetClientServer", APPX_CAPABILITY_INTERNET_CLIENT_SERVER},
		{L"privateNetworkClientServer", APPX_CAPABILITY_PRIVATE_NETWORK_CLIENT_SERVER}, {L"documentsLibrary", APPX_CAPABILITY_DOCUMENTS_LIBRARY},
		{L"picturesLibrary", APPX_CAPABILITY_PICTURES_LIBRARY}, {L"videosLibrary", APPX_CAPABILITY_VIDEOS_LIBRARY},
		{L"musicLibrary", APPX_CAPABILITY_MUSIC_LIBRARY}, {L"enterpriseAuthentication", APPX_CAPABILITY_ENTERPRISE_AUTHENTICATION},
		{L"sharedUserCertificates", APPX_CAPABILITY_SHARED_USER_CERTIFICATES}, {L"removableStorage", APPX_CAPABILITY_REMOVABLE_STORAGE},
		{L"appointments", APPX_CAPABILITY_APPOINTMENTS}, {L"contacts", APPX_CAPABILITY_CONTACTS}};
	APPX_CAPABILITIES flags = 0;
	const appx_node *capabilities = appx_child(self->document.root, L"Capabilities", NULL);
	for (const appx_node *node = appx_child(capabilities, L"Capability", NULL); node; node = appx_child(capabilities, L"Capability", node)) {
		const WCHAR *name = appx_attribute_value(node, L"Name");
		for (UINT32 index = 0; name && index < ARRAYSIZE(known); ++index)
			if (!lstrcmpW(name, known[index].name))
				flags |= known[index].flag;
	}
	*out = flags;
	return S_OK;
}

/* The values of an attribute of a parent's child elements that have a name, as a string enumerator. */
static HRESULT enumerate_strings(manifest *self, const appx_node *parent, const WCHAR *name, const WCHAR *attribute, const IID *iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	UINT32 count = 0;
	for (const appx_node *node = appx_child(parent, name, NULL); node; node = appx_child(parent, name, node))
		++count;
	const WCHAR **strings = ALLOC(sizeof *strings * (count ? count : 1));
	if (!strings)
		return E_OUTOFMEMORY;
	UINT32 kept = 0;
	for (const appx_node *node = appx_child(parent, name, NULL); node; node = appx_child(parent, name, node))
		if ((strings[kept] = appx_attribute_value(node, attribute)))
			++kept;
	const HRESULT result = appx_string_enumerator(iid, (IUnknown *)&self->iface, strings, kept, out);
	FREE(strings);
	return result;
}

static HRESULT WINAPI manifest_resources(IAppxManifestReader3 *iface, IAppxManifestResourcesEnumerator **out)
{
	manifest *self = from_reader(iface);
	return enumerate_strings(self, appx_child(self->document.root, L"Resources", NULL), L"Resource", L"Language", &IID_IAppxManifestResourcesEnumerator,
		(void **)out);
}

static HRESULT WINAPI manifest_device_capabilities(IAppxManifestReader3 *iface, IAppxManifestDeviceCapabilitiesEnumerator **out)
{
	manifest *self = from_reader(iface);
	return enumerate_strings(self, appx_child(self->document.root, L"Capabilities", NULL), L"DeviceCapability", L"Name",
		&IID_IAppxManifestDeviceCapabilitiesEnumerator, (void **)out);
}

/* OSMinVersion and OSMaxVersionTested of the Windows 8 manifest's prerequisites; versions there may have two to four parts. */
static HRESULT WINAPI manifest_prerequisite(IAppxManifestReader3 *iface, LPCWSTR name, UINT64 *value)
{
	manifest *self = from_reader(iface);
	if (!name || !value)
		return E_POINTER;
	*value = 0;
	if (lstrcmpW(name, L"OSMinVersion") && lstrcmpW(name, L"OSMaxVersionTested"))
		return E_INVALIDARG;
	WCHAR buffer[64];
	const WCHAR *text = property_text(appx_child(self->document.root, L"Prerequisites", NULL), name, buffer, ARRAYSIZE(buffer));
	if (!text)
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	WCHAR padded[80];
	padded[0] = 0;
	append(padded, ARRAYSIZE(padded), text);
	int dots = 0;
	for (const WCHAR *at = text; *at; ++at)
		dots += *at == '.';
	for (; dots < 3; ++dots)
		append(padded, ARRAYSIZE(padded), L".0");
	return parse_version(padded, value) ? S_OK : APPX_E_INVALID_MANIFEST;
}

static HRESULT WINAPI manifest_applications(IAppxManifestReader3 *iface, IAppxManifestApplicationsEnumerator **out)
{
	manifest *self = from_reader(iface);
	return enumerate(self, appx_child(self->document.root, L"Applications", NULL), L"Application", &IID_IAppxManifestApplicationsEnumerator,
		make_application, (void **)out);
}

static HRESULT WINAPI manifest_stream(IAppxManifestReader3 *iface, IStream **out)
{
	manifest *self = from_reader(iface);
	if (!out)
		return E_POINTER;
	BYTE *copy = ALLOC(self->size ? self->size : 1);
	if (!copy)
		return E_OUTOFMEMORY;
	memcpy(copy, self->data, self->size);
	return appx_memory_stream(copy, self->size, out);
}

static HRESULT WINAPI manifest_qualified_resources(IAppxManifestReader3 *iface, IAppxManifestQualifiedResourcesEnumerator **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI manifest_capabilities_by_class(IAppxManifestReader3 *iface, APPX_CAPABILITY_CLASS_TYPE type, IAppxManifestCapabilitiesEnumerator **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI manifest_target_device_families(IAppxManifestReader3 *iface, IAppxManifestTargetDeviceFamiliesEnumerator **out)
{
	manifest *self = from_reader(iface);
	return enumerate(self, appx_child(self->document.root, L"Dependencies", NULL), L"TargetDeviceFamily", &IID_IAppxManifestTargetDeviceFamiliesEnumerator,
		make_family, (void **)out);
}

static const IAppxManifestReader3Vtbl manifest_table = {manifest_query, manifest_add_ref, manifest_release, manifest_package_id, manifest_properties,
	manifest_dependencies, manifest_capabilities, manifest_resources, manifest_device_capabilities, manifest_prerequisite, manifest_applications,
	manifest_stream, manifest_qualified_resources, manifest_capabilities_by_class, manifest_target_device_families};

/* The identity a manifest must have: a name, a publisher and a version of four parts, and an architecture Windows knows. The full name is
 * Name_Version_Architecture_ResourceId_PublisherId, the family name Name_PublisherId. */
static HRESULT read_identity(manifest *self)
{
	const appx_node *root = self->document.root;
	BOOL known = FALSE;
	for (UINT32 index = 0; index < ARRAYSIZE(package_namespaces); ++index)
		known |= !lstrcmpW(root->uri, package_namespaces[index]);
	if (lstrcmpW(root->name, L"Package") || !known || !(self->identity = appx_child(root, L"Identity", NULL)))
		return APPX_E_INVALID_MANIFEST;
	const WCHAR *name = appx_attribute_value(self->identity, L"Name"), *publisher = appx_attribute_value(self->identity, L"Publisher");
	const int architecture = architecture_index(appx_attribute_value(self->identity, L"ProcessorArchitecture"));
	if (!name || !*name || !publisher || !*publisher || architecture < 0 || !parse_version(appx_attribute_value(self->identity, L"Version"), &self->version))
		return APPX_E_INVALID_MANIFEST;
	WCHAR id[14];
	HRESULT result = appx_publisher_id(publisher, id);
	if (FAILED(result))
		return result;
	WCHAR version[32];
	version[0] = 0;
	for (int part = 3; part >= 0; --part) {
		WCHAR digits[8], *at = digits + ARRAYSIZE(digits) - 1;
		UINT value = (UINT)(self->version >> 16 * part & 0xffff);
		*at = 0;
		do
			*--at = (WCHAR)('0' + value % 10);
		while (value /= 10);
		append(version, ARRAYSIZE(version), at);
		if (part)
			append(version, ARRAYSIZE(version), L".");
	}
	const WCHAR *resource = appx_attribute_value(self->identity, L"ResourceId");
	const WCHAR *parts[] = {name, L"_", version, L"_", architectures[architecture].name, L"_", resource ? resource : L"", L"_", id};
	for (UINT32 index = 0; index < ARRAYSIZE(parts); ++index)
		append(self->full_name, ARRAYSIZE(self->full_name), parts[index]);
	append(self->family_name, ARRAYSIZE(self->family_name), name);
	append(self->family_name, ARRAYSIZE(self->family_name), L"_");
	append(self->family_name, ARRAYSIZE(self->family_name), id);
	return S_OK;
}

HRESULT appx_manifest_reader(BYTE *data, UINT64 size, IAppxManifestReader **out)
{
	*out = NULL;
	manifest *self = ALLOC(sizeof *self);
	if (!self) {
		FREE(data);
		return E_OUTOFMEMORY;
	}
	self->iface.lpVtbl = &manifest_table;
	self->refs = 1;
	self->data = data;
	self->size = size;
	HRESULT result = appx_parse(data, size, &self->document);
	if (FAILED(result))
		result = APPX_E_INVALID_MANIFEST;
	else
		result = read_identity(self);
	if (FAILED(result)) {
		manifest_destroy(self);
		return result;
	}
	InterlockedIncrement(&appx_objects);
	*out = (IAppxManifestReader *)&self->iface;
	return S_OK;
}
