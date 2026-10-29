/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven packaging component: the AppxFactory and AppxBundleFactory classes of the packaging API, which installers and package managers
 * (winget among them) open .appx and .msix packages with, to read their manifest and extract their files.
 *
 * A package reader checks, as it opens a package, that it is a ZIP archive with a manifest, a block map and content types, and that the
 * block map lists every payload file with its size; a file's content is then checked block by block against the block map as it is read
 * (APPX_E_CORRUPT_CONTENT when it differs). The signature is handed out as the footprint file it is; whether it is trusted is for
 * WinVerifyTrust to say, as on Windows.
 *
 * Not provided yet (E_NOTIMPL): writing packages and bundles, reading bundles (a bundle reader answers APPX_E_MISSING_REQUIRED_FILE for a
 * package, as Windows does, and E_NOTIMPL for a bundle), block maps validated against a signature, IAppxDigestProvider, IAppxFactory2 and
 * the encryption, editor and diagnostics classes. */
#define INITGUID
#include "appx.h"

LONG appx_objects;

typedef struct package package;

typedef struct package_file {
	IAppxFile iface;
	LONG refs;
	package *owner;
	const WCHAR *name; /* as GetName gives it */
	appx_entry *entry;
	appx_mapped_file *mapped; /* NULL for a footprint file the block map does not list */
} package_file;

struct package {
	IAppxPackageReader iface;
	LONG refs;
	appx_archive *archive;
	appx_blockmap *map;
	appx_document types; /* [Content_Types].xml */
	BYTE *map_data;
	UINT64 map_size;
};

static void package_hold(package *self) { InterlockedIncrement(&self->refs); }
static void package_let_go(package *self) { self->iface.lpVtbl->Release(&self->iface); }

/* ---- Content types ([Content_Types].xml, ECMA-376 part 2): an override for the part, else the default for its extension. ---- */

static const WCHAR *content_type(package *self, const WCHAR *name)
{
	const appx_node *root = self->types.root;
	for (const appx_node *node = appx_child(root, L"Override", NULL); node; node = appx_child(root, L"Override", node)) {
		const WCHAR *part = appx_attribute_value(node, L"PartName");
		if (part && appx_part_matches(part, name))
			return appx_attribute_value(node, L"ContentType");
	}
	const WCHAR *extension = NULL;
	for (const WCHAR *at = name; *at; ++at)
		if (*at == '.')
			extension = at + 1;
		else if (*at == '\\' || *at == '/')
			extension = NULL;
	for (const appx_node *node = appx_child(root, L"Default", NULL); extension && node; node = appx_child(root, L"Default", node)) {
		const WCHAR *candidate = appx_attribute_value(node, L"Extension");
		if (candidate && appx_equal_nocase(candidate, extension))
			return appx_attribute_value(node, L"ContentType");
	}
	return NULL;
}

/* ---- Files ---- */

static void file_destroy(package_file *self)
{
	package_let_go(self->owner);
	FREE(self);
}

APPX_UNKNOWN(file, IAppxFile, package_file, appx_same(iid, &IID_IAppxFile), file_destroy)

static HRESULT WINAPI file_compression(IAppxFile *iface, APPX_COMPRESSION_OPTION *option)
{
	if (!option)
		return E_POINTER;
	*option = ((package_file *)iface)->entry->method ? APPX_COMPRESSION_OPTION_NORMAL : APPX_COMPRESSION_OPTION_NONE;
	return S_OK;
}

static HRESULT WINAPI file_content_type(IAppxFile *iface, LPWSTR *type)
{
	package_file *self = (package_file *)iface;
	return appx_task_string(content_type(self->owner, self->name), type);
}

static HRESULT WINAPI file_name(IAppxFile *iface, LPWSTR *name) { return appx_task_string(((package_file *)iface)->name, name); }

static HRESULT WINAPI file_size(IAppxFile *iface, UINT64 *size)
{
	if (!size)
		return E_POINTER;
	*size = ((package_file *)iface)->entry->size;
	return S_OK;
}

/* A file the block map lists streams its blocks as they are read and checked; a footprint file is read whole. */
static HRESULT WINAPI file_stream(IAppxFile *iface, IStream **out)
{
	package_file *self = (package_file *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->mapped)
		return appx_file_stream(self->owner->archive, self->owner->map, self->mapped, out);
	BYTE *data;
	UINT64 size;
	HRESULT result = appx_archive_extract(self->owner->archive, self->entry, &data, &size);
	return SUCCEEDED(result) ? appx_memory_stream(data, size, out) : result;
}

static const IAppxFileVtbl file_table = {file_query, file_add_ref, file_release, file_compression, file_content_type, file_name, file_size, file_stream};

static HRESULT make_file(package *owner, const WCHAR *name, appx_entry *entry, appx_mapped_file *mapped, IAppxFile **out)
{
	package_file *self = ALLOC(sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &file_table;
	self->refs = 1;
	package_hold(self->owner = owner);
	self->name = name;
	self->entry = entry;
	self->mapped = mapped;
	InterlockedIncrement(&appx_objects);
	*out = &self->iface;
	return S_OK;
}

/* ---- The package reader ---- */

static void package_destroy(package *self)
{
	appx_document_free(&self->types);
	FREE(self->map_data);
	if (self->map)
		appx_blockmap_release(self->map);
	if (self->archive)
		appx_archive_release(self->archive);
	FREE(self);
}

APPX_UNKNOWN(package, IAppxPackageReader, package, appx_same(iid, &IID_IAppxPackageReader), package_destroy)

static HRESULT WINAPI package_block_map(IAppxPackageReader *iface, IAppxBlockMapReader **out)
{
	package *self = (package *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	BYTE *copy = ALLOC(self->map_size ? self->map_size : 1);
	if (!copy)
		return E_OUTOFMEMORY;
	memcpy(copy, self->map_data, self->map_size);
	IStream *stream;
	HRESULT result = appx_memory_stream(copy, self->map_size, &stream);
	if (FAILED(result))
		return result;
	result = appx_blockmap_reader(self->map, stream, out);
	IStream_Release(stream);
	return result;
}

/* The payload files: what the block map lists other than the manifest and the metadata folder. */
static BOOL payload(const appx_mapped_file *file)
{
	return !appx_equal_nocase(file->name, L"AppxManifest.xml") && !(lstrlenW(file->name) > 13 &&
		(CompareStringOrdinal(file->name, 13, L"AppxMetadata\\", 13, TRUE) == CSTR_EQUAL ||
			CompareStringOrdinal(file->name, 13, L"AppxMetadata/", 13, TRUE) == CSTR_EQUAL));
}

static HRESULT WINAPI package_footprint(IAppxPackageReader *iface, APPX_FOOTPRINT_FILE_TYPE type, IAppxFile **out)
{
	package *self = (package *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	static const WCHAR *const names[] = {L"AppxManifest.xml", L"AppxBlockMap.xml", L"AppxSignature.p7x", L"AppxMetadata\\CodeIntegrity.cat",
		L"AppxMetadata\\ContentGroupMap.xml"};
	if (type < APPX_FOOTPRINT_FILE_TYPE_MANIFEST || type > APPX_FOOTPRINT_FILE_TYPE_CONTENTGROUPMAP)
		return E_INVALIDARG;
	appx_entry *entry = appx_archive_find(self->archive, names[type]);
	if (!entry)
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	return make_file(self, names[type], entry, appx_blockmap_find(self->map, names[type]), out);
}

static HRESULT WINAPI package_payload_file(IAppxPackageReader *iface, LPCWSTR name, IAppxFile **out)
{
	package *self = (package *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!name || !*name)
		return E_INVALIDARG;
	appx_mapped_file *file = appx_blockmap_find(self->map, name);
	if (!file || !payload(file))
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	return make_file(self, file->name, file->entry, file, out);
}

static HRESULT WINAPI package_payload_files(IAppxPackageReader *iface, IAppxFilesEnumerator **out)
{
	package *self = (package *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	IUnknown **items = ALLOC(sizeof *items * (self->map->count ? self->map->count : 1));
	if (!items)
		return E_OUTOFMEMORY;
	HRESULT result = S_OK;
	UINT32 made = 0;
	for (UINT32 index = 0; index < self->map->count; ++index) {
		appx_mapped_file *file = &self->map->files[index];
		if (!payload(file))
			continue;
		if (FAILED(result = make_file(self, file->name, file->entry, file, (IAppxFile **)&items[made])))
			break;
		++made;
	}
	if (SUCCEEDED(result))
		result = appx_enumerator(&IID_IAppxFilesEnumerator, items, made, (void **)out);
	for (UINT32 index = 0; index < made; ++index)
		IUnknown_Release(items[index]);
	FREE(items);
	return result;
}

/* The manifest, read through the block map's check as any file it lists. */
static HRESULT WINAPI package_manifest(IAppxPackageReader *iface, IAppxManifestReader **out)
{
	package *self = (package *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	appx_mapped_file *file = appx_blockmap_find(self->map, L"AppxManifest.xml");
	if (!file)
		return APPX_E_MISSING_REQUIRED_FILE;
	IStream *stream;
	HRESULT result = appx_file_stream(self->archive, self->map, file, &stream);
	if (FAILED(result))
		return result;
	BYTE *data;
	UINT64 size;
	result = appx_read_stream(stream, &data, &size);
	IStream_Release(stream);
	return SUCCEEDED(result) ? appx_manifest_reader(data, size, out) : result;
}

static const IAppxPackageReaderVtbl package_table = {package_query, package_add_ref, package_release, package_block_map, package_footprint,
	package_payload_file, package_payload_files, package_manifest};

/* Reads a footprint part whole, APPX_E_MISSING_REQUIRED_FILE when the package lacks it. */
static HRESULT required_part(appx_archive *archive, const WCHAR *name, BYTE **data, UINT64 *size)
{
	appx_entry *entry = appx_archive_find(archive, name);
	return entry ? appx_archive_extract(archive, entry, data, size) : APPX_E_MISSING_REQUIRED_FILE;
}

HRESULT appx_package_reader(IStream *stream, IAppxPackageReader **out)
{
	*out = NULL;
	package *self = ALLOC(sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &package_table;
	self->refs = 1;
	HRESULT result = appx_archive_open(stream, &self->archive);
	/* A bundle is a ZIP archive too, without a package manifest: that is how a reader tells one from the other. */
	if (SUCCEEDED(result) && !appx_archive_find(self->archive, L"AppxManifest.xml"))
		result = APPX_E_MISSING_REQUIRED_FILE;
	if (SUCCEEDED(result))
		result = required_part(self->archive, L"AppxBlockMap.xml", &self->map_data, &self->map_size);
	if (SUCCEEDED(result))
		result = appx_blockmap_read(self->map_data, self->map_size, self->archive, &self->map);
	if (SUCCEEDED(result) && !appx_blockmap_find(self->map, L"AppxManifest.xml"))
		result = APPX_E_INVALID_BLOCKMAP;
	BYTE *types = NULL;
	UINT64 types_size = 0;
	if (SUCCEEDED(result))
		result = required_part(self->archive, L"[Content_Types].xml", &types, &types_size);
	if (SUCCEEDED(result) && FAILED(appx_parse(types, types_size, &self->types)))
		result = APPX_E_CORRUPT_CONTENT;
	FREE(types);
	if (FAILED(result)) {
		package_destroy(self);
		return result;
	}
	InterlockedIncrement(&appx_objects);
	*out = &self->iface;
	return S_OK;
}

HRESULT appx_bundle_reader(IStream *stream, IAppxBundleReader **out)
{
	*out = NULL;
	appx_archive *archive;
	HRESULT result = appx_archive_open(stream, &archive);
	if (FAILED(result))
		return result;
	result = appx_archive_find(archive, L"AppxMetadata/AppxBundleManifest.xml") ? E_NOTIMPL : APPX_E_MISSING_REQUIRED_FILE;
	appx_archive_release(archive);
	return result;
}

/* ---- The factories ---- */

static HRESULT WINAPI factory_query(IAppxFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (appx_same(iid, &IID_IUnknown) || appx_same(iid, &IID_IAppxFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_add_ref(IAppxFactory *iface) { return 2; }
static ULONG WINAPI factory_release(IAppxFactory *iface) { return 1; }

static HRESULT WINAPI factory_package_writer(IAppxFactory *iface, IStream *stream, const APPX_PACKAGE_SETTINGS *settings, IAppxPackageWriter **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI factory_package_reader(IAppxFactory *iface, IStream *stream, IAppxPackageReader **out)
{
	if (!stream || !out)
		return E_POINTER;
	return appx_package_reader(stream, out);
}

static HRESULT WINAPI factory_manifest_reader(IAppxFactory *iface, IStream *stream, IAppxManifestReader **out)
{
	if (!stream || !out)
		return E_POINTER;
	*out = NULL;
	BYTE *data;
	UINT64 size;
	HRESULT result = appx_read_stream(stream, &data, &size);
	return SUCCEEDED(result) ? appx_manifest_reader(data, size, out) : result;
}

static HRESULT WINAPI factory_block_map_reader(IAppxFactory *iface, IStream *stream, IAppxBlockMapReader **out)
{
	if (!stream || !out)
		return E_POINTER;
	*out = NULL;
	BYTE *data;
	UINT64 size;
	HRESULT result = appx_read_stream(stream, &data, &size);
	if (FAILED(result))
		return result;
	appx_blockmap *map;
	result = appx_blockmap_read(data, size, NULL, &map);
	FREE(data);
	if (FAILED(result))
		return result;
	result = appx_blockmap_reader(map, stream, out);
	appx_blockmap_release(map);
	return result;
}

static HRESULT WINAPI factory_validated_block_map_reader(IAppxFactory *iface, IStream *stream, LPCWSTR signature, IAppxBlockMapReader **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static const IAppxFactoryVtbl factory_table = {factory_query, factory_add_ref, factory_release, factory_package_writer, factory_package_reader,
	factory_manifest_reader, factory_block_map_reader, factory_validated_block_map_reader};
static IAppxFactory factory = {&factory_table};

static HRESULT WINAPI bundle_factory_query(IAppxBundleFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (appx_same(iid, &IID_IUnknown) || appx_same(iid, &IID_IAppxBundleFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI bundle_factory_add_ref(IAppxBundleFactory *iface) { return 2; }
static ULONG WINAPI bundle_factory_release(IAppxBundleFactory *iface) { return 1; }

static HRESULT WINAPI bundle_factory_writer(IAppxBundleFactory *iface, IStream *stream, UINT64 version, IAppxBundleWriter **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI bundle_factory_reader(IAppxBundleFactory *iface, IStream *stream, IAppxBundleReader **out)
{
	if (!stream || !out)
		return E_POINTER;
	return appx_bundle_reader(stream, out);
}

static HRESULT WINAPI bundle_factory_manifest_reader(IAppxBundleFactory *iface, IStream *stream, IAppxBundleManifestReader **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static const IAppxBundleFactoryVtbl bundle_factory_table = {bundle_factory_query, bundle_factory_add_ref, bundle_factory_release, bundle_factory_writer,
	bundle_factory_reader, bundle_factory_manifest_reader};
static IAppxBundleFactory bundle_factory = {&bundle_factory_table};

/* ---- The class objects: each factory class is one object for the process. ---- */

typedef struct class_object {
	IClassFactory iface;
	IUnknown *instance;
} class_object;

static HRESULT WINAPI class_query(IClassFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (appx_same(iid, &IID_IUnknown) || appx_same(iid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI class_add_ref(IClassFactory *iface) { return 2; }
static ULONG WINAPI class_release(IClassFactory *iface) { return 1; }

static HRESULT WINAPI class_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	IUnknown *instance = ((class_object *)iface)->instance;
	return instance->lpVtbl->QueryInterface(instance, iid, out);
}

static HRESULT WINAPI class_lock(IClassFactory *iface, BOOL lock) { return S_OK; }

static IClassFactoryVtbl class_table = {class_query, class_add_ref, class_release, class_create, class_lock};
static class_object factory_class = {{&class_table}, (IUnknown *)&factory};
static class_object bundle_factory_class = {{&class_table}, (IUnknown *)&bundle_factory};

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (appx_same(clsid, &CLSID_AppxFactory))
		return class_query(&factory_class.iface, iid, out);
	if (appx_same(clsid, &CLSID_AppxBundleFactory))
		return class_query(&bundle_factory_class.iface, iid, out);
	return CLASS_E_CLASSNOTAVAILABLE;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return appx_objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
