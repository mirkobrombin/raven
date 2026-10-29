#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <objidl.h>
#include <shlwapi.h>
#include "appxpackaging.h"

/* The generated header has no method macros: a call goes through the vtable. */
#define CALL(object, method, ...) ((object)->lpVtbl->method((object), ##__VA_ARGS__))

static WCHAR directory[MAX_PATH];

/* A fixture beside the client, which appx_fixtures.py wrote. */
static IStream *open_fixture(const WCHAR *name)
{
	WCHAR path[MAX_PATH];
	lstrcpyW(path, directory);
	lstrcatW(path, name);
	IStream *stream = NULL;
	return SUCCEEDED(SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, NULL, &stream)) ? stream : NULL;
}

static HRESULT open_package(IAppxFactory *factory, const WCHAR *name, IAppxPackageReader **reader)
{
	*reader = NULL;
	IStream *stream = open_fixture(name);
	if (!stream)
		return E_FAIL;
	const HRESULT result = CALL(factory, CreatePackageReader, stream, reader);
	CALL(stream, Release);
	return result;
}

/* Whether a string the API returned is the expected one; it is freed either way. */
static BOOL is(LPWSTR value, const WCHAR *expected)
{
	const BOOL same = value && expected ? !lstrcmpW(value, expected) : value == expected;
	CoTaskMemFree(value);
	return same;
}

/* The bytes of Public\index.db, as appx_fixtures.py makes them. */
static BYTE pattern(UINT32 index) { return (BYTE)((index % 251) ^ ((index >> 9) & 0xff)); }

#define INDEX_SIZE (2 * 65536 + 12345)

/* The packaging API through COM: a package's manifest, files and block map; ZIP64 records; packages and bundles told apart; and content
 * that differs from the block map refused where it is read. */
int raven_entry(void)
{
	GetModuleFileNameW(NULL, directory, MAX_PATH);
	for (WCHAR *at = directory + lstrlenW(directory); at > directory && at[-1] != '\\'; --at)
		at[-1] = 0;
	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 1;
	IAppxFactory *factory = NULL;
	IAppxBundleFactory *bundles = NULL;
	if (CoCreateInstance(&CLSID_AppxFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IAppxFactory, (void **)&factory) != S_OK) return 2;
	if (CoCreateInstance(&CLSID_AppxBundleFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IAppxBundleFactory, (void **)&bundles) != S_OK) return 3;

	/* A package is not a bundle, and a bundle is not a package: what tells a program which of the two readers to use. */
	IStream *stream = open_fixture(L"appx-good.msix");
	IAppxBundleReader *bundle = NULL;
	if (!stream || CALL(bundles, CreateBundleReader, stream, &bundle) != APPX_E_MISSING_REQUIRED_FILE || bundle) return 4;
	CALL(stream, Release);
	IAppxPackageReader *reader = NULL;
	if (open_package(factory, L"appx-bundle.msixbundle", &reader) != APPX_E_MISSING_REQUIRED_FILE || reader) return 5;
	/* Every part other than the footprint files is in the block map. */
	if (open_package(factory, L"appx-unlisted.msix", &reader) != APPX_E_INVALID_BLOCKMAP || reader) return 6;

	if (open_package(factory, L"appx-good.msix", &reader) != S_OK) return 7;
	/* The identity, and the names that follow from it: the publisher ID of this publisher is the one Microsoft's packages carry. */
	IAppxManifestReader *manifest = NULL;
	IAppxManifestPackageId *id = NULL;
	LPWSTR text = NULL;
	UINT64 version = 0;
	APPX_PACKAGE_ARCHITECTURE architecture = 99;
	BOOL same = FALSE;
	if (CALL(reader, GetManifest, &manifest) != S_OK || CALL(manifest, GetPackageId, &id) != S_OK) return 8;
	if (CALL(id, GetName, &text) != S_OK || !is(text, L"Raven.Test")) return 9;
	if (CALL(id, GetVersion, &version) != S_OK || version != 0x0001000200030004ull) return 10;
	if (CALL(id, GetArchitecture, &architecture) != S_OK || architecture != APPX_PACKAGE_ARCHITECTURE_X64) return 11;
	if (CALL(id, GetPackageFullName, &text) != S_OK || !is(text, L"Raven.Test_1.2.3.4_x64__8wekyb3d8bbwe")) return 12;
	if (CALL(id, GetPackageFamilyName, &text) != S_OK || !is(text, L"Raven.Test_8wekyb3d8bbwe")) return 13;
	if (CALL(id, GetResourceId, &text) != S_OK || !is(text, NULL)) return 14;
	if (CALL(id, ComparePublisher, L"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US", &same) != S_OK || !same) return 15;
	if (CALL(id, ComparePublisher, L"CN=Someone Else", &same) != S_OK || same) return 16;
	CALL(id, Release);
	/* Properties, dependencies, the target device family, capabilities and applications. */
	IAppxManifestProperties *properties = NULL;
	BOOL flag = TRUE;
	if (CALL(manifest, GetProperties, &properties) != S_OK) return 17;
	if (CALL(properties, GetStringValue, L"DisplayName", &text) != S_OK || !is(text, L"Raven test package")) return 18;
	if (CALL(properties, GetBoolValue, L"Framework", &flag) != S_OK || flag) return 19;
	if (CALL(properties, GetStringValue, L"Nonsense", &text) != E_INVALIDARG) return 20;
	CALL(properties, Release);
	IAppxManifestPackageDependenciesEnumerator *dependencies = NULL;
	IAppxManifestPackageDependency *dependency = NULL;
	BOOL has = FALSE;
	if (CALL(manifest, GetPackageDependencies, &dependencies) != S_OK || CALL(dependencies, GetHasCurrent, &has) != S_OK || !has) return 21;
	if (CALL(dependencies, GetCurrent, &dependency) != S_OK || CALL(dependency, GetName, &text) != S_OK || !is(text, L"Microsoft.VCLibs.140.00.UWPDesktop"))
		return 22;
	if (CALL(dependency, GetMinVersion, &version) != S_OK || version != 0x000e00005e990000ull) return 23;
	CALL(dependency, Release);
	if (CALL(dependencies, MoveNext, &has) != S_OK || has || CALL(dependencies, GetCurrent, &dependency) == S_OK) return 24;
	CALL(dependencies, Release);
	IAppxManifestReader3 *manifest3 = NULL;
	IAppxManifestTargetDeviceFamiliesEnumerator *families = NULL;
	IAppxManifestTargetDeviceFamily *family = NULL;
	if (CALL(manifest, QueryInterface, &IID_IAppxManifestReader3, (void **)&manifest3) != S_OK) return 25;
	if (CALL(manifest3, GetTargetDeviceFamilies, &families) != S_OK || CALL(families, GetCurrent, &family) != S_OK) return 26;
	if (CALL(family, GetName, &text) != S_OK || !is(text, L"Windows.Desktop")) return 27;
	if (CALL(family, GetMinVersion, &version) != S_OK || version != 0x000a000045630000ull) return 28;
	CALL(family, Release);
	CALL(families, Release);
	CALL(manifest3, Release);
	APPX_CAPABILITIES capabilities = 0;
	if (CALL(manifest, GetCapabilities, &capabilities) != S_OK || capabilities != APPX_CAPABILITY_INTERNET_CLIENT) return 29;
	IAppxManifestResourcesEnumerator *resources = NULL;
	if (CALL(manifest, GetResources, &resources) != S_OK || CALL(resources, GetCurrent, &text) != S_OK || !is(text, L"en-us")) return 30;
	CALL(resources, Release);
	IAppxManifestApplicationsEnumerator *applications = NULL;
	IAppxManifestApplication *application = NULL;
	if (CALL(manifest, GetApplications, &applications) != S_OK || CALL(applications, GetCurrent, &application) != S_OK) return 31;
	if (CALL(application, GetAppUserModelId, &text) != S_OK || !is(text, L"Raven.Test_8wekyb3d8bbwe!App")) return 32;
	if (CALL(application, GetStringValue, L"Executable", &text) != S_OK || !is(text, L"raven.exe")) return 33;
	if (CALL(application, GetStringValue, L"DisplayName", &text) != S_OK || !is(text, L"Raven")) return 34;
	CALL(application, Release);
	CALL(applications, Release);
	CALL(manifest, Release);

	/* A deflated payload file of three blocks, read whole, from within its second block, and through a clone. */
	IAppxFile *file = NULL;
	UINT64 size = 0;
	APPX_COMPRESSION_OPTION compression = 99;
	if (CALL(reader, GetPayloadFile, L"Public\\index.db", &file) != S_OK) return 35;
	if (CALL(file, GetName, &text) != S_OK || !is(text, L"Public\\index.db")) return 36;
	if (CALL(file, GetSize, &size) != S_OK || size != INDEX_SIZE) return 37;
	if (CALL(file, GetCompressionOption, &compression) != S_OK || compression != APPX_COMPRESSION_OPTION_NORMAL) return 38;
	if (CALL(file, GetContentType, &text) != S_OK || !is(text, L"application/octet-stream")) return 39;
	IStream *content = NULL;
	static BYTE buffer[INDEX_SIZE + 16];
	ULONG done = 0;
	if (CALL(file, GetStream, &content) != S_OK) return 40;
	if (CALL(content, Read, buffer, sizeof buffer, &done) != S_FALSE || done != INDEX_SIZE) return 41;
	for (UINT32 index = 0; index < INDEX_SIZE; ++index)
		if (buffer[index] != pattern(index)) return 42;
	LARGE_INTEGER move;
	move.QuadPart = 65536 + 4000;
	if (CALL(content, Seek, move, STREAM_SEEK_SET, NULL) != S_OK || CALL(content, Read, buffer, 100, &done) != S_OK || done != 100) return 43;
	for (UINT32 index = 0; index < 100; ++index)
		if (buffer[index] != pattern(65536 + 4000 + index)) return 44;
	IStream *clone = NULL;
	if (CALL(content, Clone, &clone) != S_OK || CALL(clone, Read, buffer, 10, &done) != S_OK || buffer[0] != pattern(65536 + 4100)) return 45;
	CALL(clone, Release);
	STATSTG stat;
	if (CALL(content, Stat, &stat, STATFLAG_NONAME) != S_OK || stat.cbSize.QuadPart != INDEX_SIZE) return 46;
	CALL(file, Release);
	/* The same file named with '/'; a stored file whose part name is percent-encoded; names that are not payload files. */
	if (CALL(reader, GetPayloadFile, L"public/INDEX.db", &file) != S_OK) return 47;
	CALL(file, Release);
	if (CALL(reader, GetPayloadFile, L"Docs\\read me.txt", &file) != S_OK || CALL(file, GetStream, &clone) != S_OK) return 48;
	if (CALL(clone, Read, buffer, sizeof buffer, &done) != S_FALSE || done != 5 || buffer[0] != 'h' || buffer[4] != 'o') return 49;
	if (CALL(file, GetCompressionOption, &compression) != S_OK || compression != APPX_COMPRESSION_OPTION_NONE) return 50;
	CALL(clone, Release);
	CALL(file, Release);
	if (SUCCEEDED(CALL(reader, GetPayloadFile, L"AppxManifest.xml", &file)) || SUCCEEDED(CALL(reader, GetPayloadFile, L"missing.txt", &file))) return 51;
	IAppxFilesEnumerator *files = NULL;
	UINT32 count = 0;
	if (CALL(reader, GetPayloadFiles, &files) != S_OK) return 52;
	for (CALL(files, GetHasCurrent, &has); has; CALL(files, MoveNext, &has))
		++count;
	if (count != 3) return 53;
	CALL(files, Release);
	/* Footprint files: the manifest is there with its content type, an unsigned package has no signature. */
	if (CALL(reader, GetFootprintFile, APPX_FOOTPRINT_FILE_TYPE_MANIFEST, &file) != S_OK) return 54;
	if (CALL(file, GetContentType, &text) != S_OK || !is(text, L"application/vnd.ms-appx.manifest+xml")) return 55;
	CALL(file, Release);
	if (CALL(reader, GetFootprintFile, APPX_FOOTPRINT_FILE_TYPE_BLOCKMAP, &file) != S_OK || CALL(file, GetContentType, &text) != S_OK ||
		!is(text, L"application/vnd.ms-appx.blockmap+xml"))
		return 56;
	CALL(file, Release);
	if (SUCCEEDED(CALL(reader, GetFootprintFile, APPX_FOOTPRINT_FILE_TYPE_SIGNATURE, &file))) return 57;
	/* The block map: a file's blocks, its local header, and a stream checked against it. */
	IAppxBlockMapReader *map = NULL;
	IAppxBlockMapFile *mapped = NULL;
	IAppxBlockMapBlocksEnumerator *blocks = NULL;
	UINT32 header = 0;
	if (CALL(reader, GetBlockMap, &map) != S_OK || CALL(map, GetFile, L"Public\\index.db", &mapped) != S_OK) return 58;
	if (CALL(mapped, GetLocalFileHeaderSize, &header) != S_OK || header != 30 + 15) return 59;
	if (CALL(mapped, GetBlocks, &blocks) != S_OK) return 60;
	count = 0;
	for (CALL(blocks, GetHasCurrent, &has); has; CALL(blocks, MoveNext, &has)) {
		IAppxBlockMapBlock *block = NULL;
		UINT32 hash_size = 0, stored = 0;
		BYTE *hash = NULL;
		if (CALL(blocks, GetCurrent, &block) != S_OK || CALL(block, GetHash, &hash_size, &hash) != S_OK || hash_size != 32) return 61;
		if (CALL(block, GetCompressedSize, &stored) != S_OK || !stored) return 62;
		CoTaskMemFree(hash);
		CALL(block, Release);
		++count;
	}
	if (count != 3) return 63;
	CALL(blocks, Release);
	if (CALL(mapped, ValidateFileHash, content, &same) != S_OK || !same) return 65;
	CALL(content, Release);
	CALL(mapped, Release);
	CALL(map, Release);
	if (CALL(reader, Release)) return 66;

	/* A manifest read on its own. */
	stream = open_fixture(L"appx-manifest.xml");
	if (!stream || CALL(factory, CreateManifestReader, stream, &manifest) != S_OK || CALL(manifest, GetPackageId, &id) != S_OK) return 67;
	if (CALL(id, GetPackageFamilyName, &text) != S_OK || !is(text, L"Raven.Test_8wekyb3d8bbwe")) return 68;
	CALL(id, Release);
	CALL(manifest, Release);
	CALL(stream, Release);

	/* The same package with ZIP64 records. */
	if (open_package(factory, L"appx-zip64.msix", &reader) != S_OK || CALL(reader, GetPayloadFile, L"Public\\index.db", &file) != S_OK) return 69;
	if (CALL(file, GetStream, &content) != S_OK || CALL(content, Read, buffer, sizeof buffer, &done) != S_FALSE || done != INDEX_SIZE) return 70;
	if (buffer[INDEX_SIZE - 1] != pattern(INDEX_SIZE - 1)) return 71;
	CALL(content, Release);
	CALL(file, Release);
	CALL(reader, Release);

	/* Content that differs from the block map opens, and fails where it is read: the deflated file's first block reads, its second does
	 * not; the stored file fails at once. */
	if (open_package(factory, L"appx-tampered.msix", &reader) != S_OK) return 72;
	if (CALL(reader, GetPayloadFile, L"Public\\index.db", &file) != S_OK || CALL(file, GetStream, &content) != S_OK) return 73;
	if (CALL(content, Read, buffer, 65536, &done) != S_OK || done != 65536) return 74;
	if (CALL(content, Read, buffer, 100, &done) != APPX_E_CORRUPT_CONTENT) return 75;
	CALL(content, Release);
	CALL(file, Release);
	if (CALL(reader, GetPayloadFile, L"Assets\\Logo.png", &file) != S_OK || CALL(file, GetStream, &content) != S_OK) return 76;
	if (CALL(content, Read, buffer, 4, &done) != APPX_E_CORRUPT_CONTENT) return 77;
	CALL(content, Release);
	CALL(file, Release);
	CALL(reader, Release);

	CALL(bundles, Release);
	CALL(factory, Release);
	CoUninitialize();
	return 0;
}
