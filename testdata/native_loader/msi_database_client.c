#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

static BOOL same(const WCHAR *a, const WCHAR *b)
{
	while (*a && *a == *b)
		++a, ++b;
	return *a == *b;
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
	return 0;
}
