#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <msi.h>
#include <msiquery.h>

/* A Windows Installer package read as a database: its tables, its summary and a property, as setup programs read them. */

static void beside(WCHAR *target, const WCHAR *name)
{
	unsigned cut = GetModuleFileNameW(NULL, target, MAX_PATH);
	while (cut && target[cut - 1] != L'\\')
		--cut;
	for (unsigned i = 0; name[i]; ++i)
		target[cut++] = name[i];
	target[cut] = 0;
}

static int name_digit(WCHAR c)
{
	if (c >= L'0' && c <= L'9')
		return c - L'0';
	if (c >= L'A' && c <= L'Z')
		return c - L'A' + 10;
	if (c >= L'a' && c <= L'z')
		return c - L'a' + 36;
	return c == L'.' ? 62 : c == L'_' ? 63 : -1;
}

static BOOL same(const WCHAR *a, const WCHAR *b)
{
	while (*a && *a == *b)
		++a, ++b;
	return *a == *b;
}

/* A stream name of a table as packages store it: a marker, then the name packed two characters to a unit. */
static void table_stream(const WCHAR *name, WCHAR *out)
{
	*out++ = 0x4840;
	while (*name) {
		int first = name_digit(*name), second = name[1] ? name_digit(name[1]) : -1;
		if (first < 0)
			*out++ = *name++;
		else if (second >= 0) {
			*out++ = 0x3800 + first + (second << 6);
			name += 2;
		} else {
			*out++ = 0x4800 + first;
			++name;
		}
	}
	*out = 0;
}

static BOOL put_stream(IStorage *storage, const WCHAR *name, const void *data, ULONG size)
{
	WCHAR encoded[32];
	IStream *stream;
	ULONG done = 0;
	table_stream(name, encoded);
	if (FAILED(IStorage_CreateStream(storage, encoded, STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stream)))
		return FALSE;
	HRESULT hr = IStream_Write(stream, data, size, &done);
	IStream_Release(stream);
	return SUCCEEDED(hr) && done == size;
}

/* A package with one table T of one string column A whose one row is the fourth string of the pool given, and the summary
   information stream given, if any. */
static BOOL write_package(WCHAR *path, const WORD *pool, ULONG pool_size, const BYTE *summary, ULONG summary_size)
{
	static const GUID database = {0x000c1084, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
	static const char data[] = "TAabc";
	static const WORD tables[] = {1}, columns[] = {1, 0x8001, 2, 0x8000 | 0x2d08}, rows[] = {4};
	WCHAR directory[MAX_PATH];
	IStorage *storage;
	IStream *stream;
	ULONG done = 0;
	GetTempPathW(MAX_PATH, directory);
	GetTempFileNameW(directory, L"rvn", 0, path);
	if (FAILED(StgCreateDocfile(path, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &storage)))
		return FALSE;
	BOOL written = SUCCEEDED(IStorage_SetClass(storage, &database)) && put_stream(storage, L"_StringPool", pool, pool_size) &&
		put_stream(storage, L"_StringData", data, 5) && put_stream(storage, L"_Tables", tables, sizeof tables) &&
		put_stream(storage, L"_Columns", columns, sizeof columns) && put_stream(storage, L"T", rows, sizeof rows);
	if (written && summary) {
		written = SUCCEEDED(IStorage_CreateStream(storage, L"\005SummaryInformation", STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stream));
		if (written) {
			written = SUCCEEDED(IStream_Write(stream, summary, summary_size, &done)) && done == summary_size;
			IStream_Release(stream);
		}
	}
	IStorage_Release(storage);
	return written;
}

/* A string pool with a long entry claiming 4 GB between "A" and "abc". The entry is refused alone: the strings after it still come
   from where they are. */
static int long_pool_entry(void)
{
	static const WORD pool[] = {0, 0, 1, 1, 1, 1, 0, 0xffff, 0xffff, 0, 3, 1};
	WCHAR path[MAX_PATH], text[16];
	if (!write_package(path, pool, sizeof pool, NULL, 0))
		return 97;
	MSIHANDLE database_handle = 0, view = 0, record = 0;
	DWORD size = 16;
	int result = 0;
	if (MsiOpenDatabaseW(path, (LPCWSTR)MSIDBOPEN_READONLY, &database_handle) != ERROR_SUCCESS ||
		MsiDatabaseOpenViewW(database_handle, L"SELECT `A` FROM `T`", &view) != ERROR_SUCCESS || MsiViewExecute(view, 0) != ERROR_SUCCESS ||
		MsiViewFetch(view, &record) != ERROR_SUCCESS)
		result = 98;
	else if (MsiRecordGetStringW(record, 1, text, &size) != ERROR_SUCCESS || !same(text, L"abc"))
		result = 99;
	MsiCloseHandle(record);
	MsiCloseHandle(view);
	MsiCloseHandle(database_handle);
	DeleteFileW(path);
	return result;
}

/* A summary information stream whose section offset is near 4 GB, so that adding the section header to it wraps: the section is
   refused and the summary reads as empty. */
static int far_summary_section(void)
{
	static const WORD pool[] = {0, 0, 1, 1, 1, 1, 0, 0, 3, 1};
	static BYTE summary[48];
	static const BYTE format[16] = {0xe0, 0x85, 0x9f, 0xf2, 0xf9, 0x4f, 0x68, 0x10, 0xab, 0x91, 0x08, 0x00, 0x2b, 0x27, 0xb3, 0xd9};
	WCHAR path[MAX_PATH];
	summary[0] = 0xfe;
	summary[1] = 0xff;
	for (int i = 0; i < 16; ++i)
		summary[28 + i] = format[i];
	summary[44] = 0xfc;
	summary[45] = summary[46] = summary[47] = 0xff;
	if (!write_package(path, pool, sizeof pool, summary, sizeof summary))
		return 100;
	MSIHANDLE database = 0, handle = 0;
	UINT count = 1;
	int result = 0;
	if (MsiOpenDatabaseW(path, (LPCWSTR)MSIDBOPEN_READONLY, &database) != ERROR_SUCCESS ||
		MsiGetSummaryInformationW(database, NULL, 0, &handle) != ERROR_SUCCESS || MsiSummaryInfoGetPropertyCount(handle, &count) != ERROR_SUCCESS)
		result = 101;
	else if (count)
		result = 102;
	MsiCloseHandle(handle);
	MsiCloseHandle(database);
	DeleteFileW(path);
	return result;
}

/* A transform deleting the row of T whose key is 5000 characters long, applied only to view what it changes: the row shows in the
   view whole, however long its key. */
static int long_transform_key(void)
{
	static const GUID transform = {0x000c1082, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
	static const WORD package_pool[] = {0, 0, 1, 1, 1, 1, 0, 0, 3, 1}, pool[] = {0, 0, 5000, 1}, change[] = {0, 1};
	static char key[5000];
	static WCHAR row[5040];
	WCHAR package[MAX_PATH], path[MAX_PATH], directory[MAX_PATH];
	IStorage *storage;
	for (int i = 0; i < 5000; ++i)
		key[i] = 'k';
	if (!write_package(package, package_pool, sizeof package_pool, NULL, 0))
		return 103;
	GetTempPathW(MAX_PATH, directory);
	GetTempFileNameW(directory, L"rvn", 0, path);
	if (FAILED(StgCreateDocfile(path, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &storage)))
		return 103;
	BOOL written = SUCCEEDED(IStorage_SetClass(storage, &transform)) && put_stream(storage, L"_StringPool", pool, sizeof pool) &&
		put_stream(storage, L"_StringData", key, sizeof key) && put_stream(storage, L"T", change, sizeof change);
	IStorage_Release(storage);
	if (!written)
		return 103;
	MSIHANDLE database = 0, view = 0, record = 0;
	DWORD size = 5040;
	int result = 0;
	if (MsiOpenDatabaseW(package, (LPCWSTR)MSIDBOPEN_READONLY, &database) != ERROR_SUCCESS ||
		MsiDatabaseApplyTransformW(database, path, 0x100 /* MSITRANSFORM_ERROR_VIEWTRANSFORM */) != ERROR_SUCCESS)
		result = 104;
	else if (MsiDatabaseOpenViewW(database, L"SELECT `Row` FROM `_TransformView` WHERE `Column` = 'DELETE'", &view) != ERROR_SUCCESS ||
		MsiViewExecute(view, 0) != ERROR_SUCCESS || MsiViewFetch(view, &record) != ERROR_SUCCESS)
		result = 105;
	else if (MsiRecordGetStringW(record, 1, row, &size) != ERROR_SUCCESS || size != 5000 || row[0] != L'k' || row[4999] != L'k')
		result = 106;
	MsiCloseHandle(record);
	MsiCloseHandle(view);
	MsiCloseHandle(database);
	DeleteFileW(path);
	DeleteFileW(package);
	return result;
}

int raven_entry(void)
{
	WCHAR package[MAX_PATH], text[64];
	beside(package, L"basic.msi");
	MSIHANDLE database = 0, view = 0, record = 0;
	if (MsiOpenDatabaseW(package, (LPCWSTR)MSIDBOPEN_READONLY, &database) != ERROR_SUCCESS)
		return 1;
	if (MsiDatabaseOpenViewW(database, L"SELECT `Value` FROM `Property` WHERE `Property` = 'ProductName'", &view) != ERROR_SUCCESS)
		return 2;
	if (MsiViewExecute(view, 0) != ERROR_SUCCESS || MsiViewFetch(view, &record) != ERROR_SUCCESS)
		return 3;
	DWORD size = 64;
	if (MsiRecordGetStringW(record, 1, text, &size) != ERROR_SUCCESS || !same(text, L"Raven Sample"))
		return 4;
	MsiCloseHandle(record);
	MsiViewClose(view);
	MsiCloseHandle(view);
	/* The file the package installs, from the File table. */
	if (MsiDatabaseOpenViewW(database, L"SELECT `FileName` FROM `File`", &view) != ERROR_SUCCESS || MsiViewExecute(view, 0) != ERROR_SUCCESS ||
		MsiViewFetch(view, &record) != ERROR_SUCCESS)
		return 5;
	size = 64;
	if (MsiRecordGetStringW(record, 1, text, &size) != ERROR_SUCCESS || !same(text, L"hello.txt"))
		return 6;
	MsiCloseHandle(record);
	MsiCloseHandle(view);
	/* The summary information stream names the package's platform. */
	MSIHANDLE summary = 0;
	UINT type = 0;
	INT number = 0;
	FILETIME time;
	size = 64;
	if (MsiGetSummaryInformationW(database, NULL, 0, &summary) != ERROR_SUCCESS ||
		MsiSummaryInfoGetPropertyW(summary, 7 /* PID_TEMPLATE */, &type, &number, &time, text, &size) != ERROR_SUCCESS || type != 30 /* VT_LPSTR */)
		return 7;
	MsiCloseHandle(summary);
	MsiCloseHandle(database);
	/* The file hash is MD5: this is the known digest of "abc". */
	{
		WCHAR hash_path[MAX_PATH], hash_directory[MAX_PATH];
		DWORD written;
		MSIFILEHASHINFO info;
		GetTempPathW(MAX_PATH, hash_directory);
		GetTempFileNameW(hash_directory, L"rvn", 0, hash_path);
		HANDLE hash_file = CreateFileW(hash_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		WriteFile(hash_file, "abc", 3, &written, NULL);
		CloseHandle(hash_file);
		info.dwFileHashInfoSize = sizeof info;
		UINT hashed = MsiGetFileHashW(hash_path, 0, &info);
		DeleteFileW(hash_path);
		if (hashed != ERROR_SUCCESS || info.dwData[0] != 0x98500190 || info.dwData[3] != 0x727fe128)
			return 90;
	}
	if (MsiVerifyDiskSpace(0) != ERROR_INVALID_HANDLE)
		return 94;
	if (MsiCollectUserInfoW(L"{11111111-2222-3333-4444-555555555555}") != ERROR_UNKNOWN_PRODUCT || MsiCollectUserInfoW(NULL) != ERROR_INVALID_PARAMETER)
		return 95;
	/* A Darwin shortcut laid out as MS-SHLLINK says: a header, then the extra data block with the descriptor in its Unicode field. */
	{
		static const WCHAR descriptor[] = L"zn=BV3!!!!4!!!!MKKSkWord>m[8Q(!7fN4[M5LC!'6LC";
		static BYTE link[0x4c + 0x314 + 4];
		WCHAR link_path[MAX_PATH], link_directory[MAX_PATH], product[39], feature[64], component[39];
		DWORD written;
		*(DWORD *)link = 0x4c;
		*(DWORD *)(link + 0x14) = 0x1000 | 0x80;
		*(DWORD *)(link + 0x4c) = 0x314;
		*(DWORD *)(link + 0x50) = 0xa0000006;
		lstrcpyW((WCHAR *)(link + 0x4c + 0x10c), descriptor);
		GetTempPathW(MAX_PATH, link_directory);
		GetTempFileNameW(link_directory, L"rvn", 0, link_path);
		HANDLE link_file = CreateFileW(link_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		WriteFile(link_file, link, sizeof link, &written, NULL);
		CloseHandle(link_file);
		UINT target = MsiGetShortcutTargetW(link_path, product, feature, component);
		DeleteFileW(link_path);
		if (target != ERROR_SUCCESS)
			return 91;
		if (lstrcmpW(product, L"{90160000-000F-0000-1000-0000000FF1CE}") || lstrcmpW(feature, L"Word") || lstrcmpW(component, L"{11111111-2222-3333-4444-555555555555}"))
			return 92;
		if (MsiGetShortcutTargetW(L"C:\\raven-no-such.lnk", product, feature, component) == ERROR_SUCCESS)
			return 93;
	}
	{
		/* A group in braces naming no property stays as written, braces and all; one that does loses its braces. */
		MSIHANDLE record = MsiCreateRecord(2);
		WCHAR text[96];
		DWORD length = 96;
		MsiRecordSetStringW(record, 0, L"CLSID\\{E2384F4D-5F0A-4CD4-B5D1-1164BA9E4481}{ v[1]}");
		MsiRecordSetStringW(record, 1, L"1");
		if (MsiFormatRecordW(0, record, text, &length) != ERROR_SUCCESS ||
			lstrcmpW(text, L"CLSID\\{E2384F4D-5F0A-4CD4-B5D1-1164BA9E4481} v1"))
			return 96;
		MsiCloseHandle(record);
	}
	int result = long_pool_entry();
	if (!result)
		result = far_summary_section();
	return result ? result : long_transform_key();
}
