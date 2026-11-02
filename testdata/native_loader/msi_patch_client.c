#define UNICODE
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <msi.h>
#include <msiquery.h>

/* What maintenance asks of the installer. A patch is built here from two versions of a package, applied to the installed
   product, listed and taken away; a package's searches read the machine; features answer in a session; the source list reads
   back; an administrative image lays the files out and needs no cabinet to install from. */

static const WCHAR product[] = L"{7B1E3C52-9A4D-4F60-8C2E-1D5A6B7C8D90}";
static const WCHAR patch_code[] = L"{5C7A9D10-3B4E-4A21-9F86-2E1D0C9B8A70}";
static const WCHAR payload_component[] = L"{0C8A1E44-6D2B-4F7A-8B3E-5A9C1D2E3F40}";
static const char patched_text[] = "Replaced by a patch.\r\n";

static void join(WCHAR *out, const WCHAR *first, const WCHAR *second)
{
	lstrcpyW(out, first);
	lstrcatW(out, second);
}

static void beside(WCHAR *target, const WCHAR *name)
{
	unsigned cut = GetModuleFileNameW(NULL, target, MAX_PATH);
	while (cut && target[cut - 1] != L'\\')
		--cut;
	lstrcpyW(target + cut, name);
}

static BOOL run_sql(MSIHANDLE database, const WCHAR *query)
{
	MSIHANDLE view = 0;
	if (MsiDatabaseOpenViewW(database, query, &view))
		return FALSE;
	UINT status = MsiViewExecute(view, 0);
	MsiCloseHandle(view);
	return status == ERROR_SUCCESS;
}

// The registry row of the second version, on the component the package already has.
static BOOL add_registry_row(MSIHANDLE database)
{
	MSIHANDLE view = 0, row = 0;
	WCHAR component[80], query[400];
	DWORD size = ARRAYSIZE(component);
	if (MsiDatabaseOpenViewW(database, L"SELECT `Component_` FROM `Registry`", &view) || MsiViewExecute(view, 0) || MsiViewFetch(view, &row) ||
		MsiRecordGetStringW(row, 1, component, &size))
		return FALSE;
	MsiCloseHandle(row);
	MsiCloseHandle(view);
	lstrcpyW(query, L"INSERT INTO `Registry` (`Registry`, `Root`, `Key`, `Name`, `Value`, `Component_`) VALUES ('Patched', 2, 'Software\\RavenSample', 'Patched', '#7', '");
	lstrcatW(query, component);
	lstrcatW(query, L"')");
	return run_sql(database, query);
}

static BYTE *put_u32(BYTE *at, DWORD value)
{
	for (int i = 0; i < 4; i++)
		*at++ = (BYTE)(value >> (i * 8));
	return at;
}

static BYTE *put_u16(BYTE *at, WORD value)
{
	*at++ = (BYTE)value;
	*at++ = (BYTE)(value >> 8);
	return at;
}

// One stored file, by the published cabinet layout: header, folder, file entry and a single data block.
static DWORD build_cabinet(BYTE *out)
{
	static const char member[] = "Hello";
	const DWORD length = sizeof patched_text - 1, entries = 36 + 8, data = entries + 16 + sizeof member;
	BYTE *at = put_u32(out, 0x4643534d);
	at = put_u32(at, 0);
	at = put_u32(at, data + 8 + length);
	at = put_u32(at, 0);
	at = put_u32(at, entries);
	at = put_u32(at, 0);
	*at++ = 3;
	*at++ = 1;
	at = put_u16(at, 1);
	at = put_u16(at, 1);
	at = put_u16(at, 0);
	at = put_u16(at, 0);
	at = put_u16(at, 0);
	at = put_u32(at, data);
	at = put_u16(at, 1);
	at = put_u16(at, 0);
	at = put_u32(at, length);
	at = put_u32(at, 0);
	at = put_u16(at, 0);
	at = put_u16(at, 0x5821);
	at = put_u16(at, 0);
	at = put_u16(at, FILE_ATTRIBUTE_NORMAL);
	for (DWORD i = 0; i < sizeof member; i++)
		*at++ = member[i];
	at = put_u32(at, 0);
	at = put_u16(at, (WORD)length);
	at = put_u16(at, (WORD)length);
	for (DWORD i = 0; i < length; i++)
		*at++ = patched_text[i];
	return (DWORD)(at - out);
}

// The file the installed product keeps its payload in holds what a cabinet of the patch said, or does not.
static int payload_patched(void)
{
	WCHAR path[MAX_PATH];
	DWORD size = ARRAYSIZE(path), got = 0;
	if (MsiGetComponentPathW(product, payload_component, path, &size) != INSTALLSTATE_LOCAL)
		return -1;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return -1;
	char text[64];
	BOOL read = ReadFile(file, text, sizeof text, &got, NULL);
	CloseHandle(file);
	if (!read)
		return -1;
	if (got != sizeof patched_text - 1)
		return 0;
	for (DWORD i = 0; i < got; i++)
		if (text[i] != patched_text[i])
			return 0;
	return 1;
}

// A patch is a compound file: a summary naming its product and transforms, and each transform a storage inside.
static int build_patch(const WCHAR *path, const WCHAR *transform)
{
	IStorage *root = NULL, *from = NULL, *inside = NULL;
	if (FAILED(StgCreateDocfile(path, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &root)))
		return 1;
	if (FAILED(StgOpenStorage(transform, NULL, STGM_READ | STGM_SHARE_DENY_WRITE, NULL, 0, &from)))
		return 2;
	if (FAILED(IStorage_CreateStorage(root, L"Upgrade", STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &inside)))
		return 3;
	if (FAILED(IStorage_CopyTo(from, 0, NULL, NULL, inside)))
		return 4;
	IStorage_Release(inside);
	IStorage_Release(from);
	static BYTE cabinet[256];
	DWORD cabinet_size = build_cabinet(cabinet);
	IStream *stream = NULL;
	if (FAILED(IStorage_CreateStream(root, L"patchcab", STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stream)))
		return 8;
	if (FAILED(IStream_Write(stream, cabinet, cabinet_size, NULL)))
		return 9;
	IStream_Release(stream);
	IStorage_Commit(root, 0);
	IStorage_Release(root);
	MSIHANDLE summary = 0;
	FILETIME none = { 0, 0 };
	if (MsiGetSummaryInformationW(0, path, 4, &summary))
		return 5;
	if (MsiSummaryInfoSetPropertyW(summary, 2, VT_LPSTR, 0, &none, L"Sample patch") ||
		MsiSummaryInfoSetPropertyW(summary, 7, VT_LPSTR, 0, &none, product) ||
		MsiSummaryInfoSetPropertyW(summary, 8, VT_LPSTR, 0, &none, L":Upgrade") ||
		MsiSummaryInfoSetPropertyW(summary, 9, VT_LPSTR, 0, &none, patch_code))
		return 6;
	if (MsiSummaryInfoPersist(summary))
		return 7;
	MsiCloseHandle(summary);
	return 0;
}

static DWORD read_patched(void)
{
	HKEY key;
	DWORD value = 0, size = sizeof value;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_READ, &key))
		return 0;
	RegQueryValueExW(key, L"Patched", NULL, NULL, (BYTE *)&value, &size);
	RegCloseKey(key);
	return value;
}

static int check_patching(const WCHAR *package, const WCHAR *scratch)
{
	WCHAR second[MAX_PATH], transform[MAX_PATH], patch[MAX_PATH];
	join(second, scratch, L"second.msi");
	join(transform, scratch, L"second.mst");
	join(patch, scratch, L"sample.msp");
	if (!CopyFileW(package, second, FALSE))
		return 1;
	SetFileAttributesW(second, FILE_ATTRIBUTE_NORMAL);
	MSIHANDLE base = 0, changed = 0;
	if (MsiOpenDatabaseW(package, MSIDBOPEN_READONLY, &base) || MsiOpenDatabaseW(second, MSIDBOPEN_TRANSACT, &changed))
		return 2;
	if (!add_registry_row(changed) || !run_sql(changed, L"INSERT INTO `Media` (`DiskId`, `LastSequence`, `Cabinet`) VALUES (2, 100, '#patchcab')") ||
		!run_sql(changed, L"UPDATE `File` SET `Sequence` = 100 WHERE `File` = 'Hello'") || MsiDatabaseCommit(changed))
		return 3;
	if (MsiDatabaseGenerateTransformW(changed, base, transform, 0, 0))
		return 4;
	MsiCloseHandle(changed);
	MsiCloseHandle(base);
	int built = build_patch(patch, transform);
	if (built)
		return 10 + built;

	if (MsiApplyPatchW(patch, NULL, INSTALLTYPE_DEFAULT, L"") != ERROR_PATCH_TARGET_NOT_FOUND)
		return 20;
	if (MsiInstallProductW(package, L"") != ERROR_SUCCESS)
		return 21;
	// A value no removal could take away if an earlier run stopped halfway.
	HKEY stale;
	if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_SET_VALUE, &stale)) {
		RegDeleteValueW(stale, L"Patched");
		RegCloseKey(stale);
	}
	if (read_patched())
		return 22;
	if (MsiApplyPatchW(patch, NULL, INSTALLTYPE_DEFAULT, L"") != ERROR_SUCCESS)
		return 23;
	if (read_patched() != 7)
		return 24;
	if (payload_patched() != 1)
		return 41;

	WCHAR guid[39], transforms[256], text[MAX_PATH];
	DWORD size = ARRAYSIZE(transforms);
	if (MsiEnumPatchesW(product, 0, guid, transforms, &size) != ERROR_SUCCESS || lstrcmpiW(guid, patch_code) || lstrcmpW(transforms, L":Upgrade"))
		return 25;
	size = ARRAYSIZE(transforms);
	if (MsiEnumPatchesW(product, 1, guid, transforms, &size) != ERROR_NO_MORE_ITEMS)
		return 26;
	size = ARRAYSIZE(text);
	if (MsiGetPatchInfoExW(patch_code, product, NULL, MSIINSTALLCONTEXT_MACHINE, L"State", text, &size) || lstrcmpW(text, L"1"))
		return 27;
	size = ARRAYSIZE(text);
	if (MsiGetPatchInfoW(patch_code, L"LocalPackage", text, &size) || GetFileAttributesW(text) == INVALID_FILE_ATTRIBUTES)
		return 28;
	WCHAR cached[MAX_PATH];
	lstrcpyW(cached, text);
	size = ARRAYSIZE(text);
	if (MsiGetPatchInfoExW(patch_code, product, NULL, MSIINSTALLCONTEXT_MACHINE, L"DisplayName", text, &size) || lstrcmpW(text, L"Sample patch"))
		return 29;

	// Later maintenance runs the kept patch again: a repair keeps the registry row only the patch knows.
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_SET_VALUE, &key))
		return 30;
	RegDeleteValueW(key, L"Patched");
	RegCloseKey(key);
	if (MsiReinstallProductW(product, REINSTALLMODE_MACHINEDATA | REINSTALLMODE_FILEOLDERVERSION) != ERROR_SUCCESS)
		return 31;
	if (read_patched() != 7)
		return 32;

	// The source list reads back what publishing wrote.
	size = ARRAYSIZE(text);
	if (MsiSourceListGetInfoW(product, NULL, MSIINSTALLCONTEXT_MACHINE, MSICODE_PRODUCT, L"PackageName", text, &size) || lstrcmpiW(text, L"basic.msi"))
		return 33;
	size = ARRAYSIZE(text);
	if (MsiSourceListEnumSourcesW(product, NULL, MSIINSTALLCONTEXT_MACHINE, MSICODE_PRODUCT | MSISOURCETYPE_NETWORK, 0, text, &size))
		return 34;
	if (MsiSourceListAddSourceExW(product, NULL, MSIINSTALLCONTEXT_MACHINE, MSICODE_PRODUCT | MSISOURCETYPE_NETWORK, L"C:\\second\\", 0))
		return 35;
	size = ARRAYSIZE(text);
	if (MsiSourceListEnumSourcesW(product, NULL, MSIINSTALLCONTEXT_MACHINE, MSICODE_PRODUCT | MSISOURCETYPE_NETWORK, 1, text, &size) || lstrcmpW(text, L"C:\\second\\"))
		return 36;

	if (MsiRemovePatchesW(patch_code, product, INSTALLTYPE_SINGLE_INSTANCE, L"") != ERROR_SUCCESS)
		return 37;
	size = ARRAYSIZE(transforms);
	if (MsiEnumPatchesW(product, 0, guid, transforms, &size) != ERROR_NO_MORE_ITEMS)
		return 38;
	if (GetFileAttributesW(cached) != INVALID_FILE_ATTRIBUTES)
		return 39;
	if (payload_patched() != 0)
		return 42;
	if (MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT) != ERROR_SUCCESS)
		return 40;
	return 0;
}

// A copy of the package with searches of each kind; the session finds what they name.
static int check_searches(const WCHAR *package, const WCHAR *scratch)
{
	WCHAR copy[MAX_PATH], below[MAX_PATH], target[MAX_PATH];
	join(copy, scratch, L"search.msi");
	join(below, scratch, L"found-dir\\");
	join(target, below, L"search-target.txt");
	CreateDirectoryW(below, NULL);
	HANDLE made = CreateFileW(target, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD put = 0;
	if (made == INVALID_HANDLE_VALUE || !WriteFile(made, "found", 5, &put, NULL))
		return 20;
	CloseHandle(made);
	if (!CopyFileW(package, copy, FALSE))
		return 1;
	SetFileAttributesW(copy, FILE_ATTRIBUTE_NORMAL);
	MSIHANDLE database = 0, session = 0;
	if (MsiOpenDatabaseW(copy, MSIDBOPEN_TRANSACT, &database))
		return 2;
	static const WCHAR *const statements[] = {
		L"CREATE TABLE `AppSearch` (`Property` CHAR(72) NOT NULL, `Signature_` CHAR(72) NOT NULL PRIMARY KEY `Property`, `Signature_`)",
		L"CREATE TABLE `Signature` (`Signature` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL, `MinVersion` CHAR(20), `MaxVersion` CHAR(20), `MinSize` LONG, `MaxSize` LONG, `MinDate` LONG, `MaxDate` LONG, `Languages` CHAR(255) PRIMARY KEY `Signature`)",
		L"CREATE TABLE `DrLocator` (`Signature_` CHAR(72) NOT NULL, `Parent` CHAR(72), `Path` CHAR(255), `Depth` SHORT PRIMARY KEY `Signature_`, `Parent`, `Path`)",
		L"CREATE TABLE `RegLocator` (`Signature_` CHAR(72) NOT NULL, `Root` SHORT NOT NULL, `Key` CHAR(255) NOT NULL, `Name` CHAR(255), `Type` SHORT PRIMARY KEY `Signature_`)",
		L"INSERT INTO `AppSearch` (`Property`, `Signature_`) VALUES ('FOUND_FILE', 'sysfile')",
		L"INSERT INTO `AppSearch` (`Property`, `Signature_`) VALUES ('FOUND_DIR', 'sysdir')",
		L"INSERT INTO `AppSearch` (`Property`, `Signature_`) VALUES ('NOT_FOUND', 'absent')",
		L"INSERT INTO `AppSearch` (`Property`, `Signature_`) VALUES ('FOUND_REG', 'rawvalue')",
		L"INSERT INTO `Signature` (`Signature`, `FileName`, `MinSize`) VALUES ('sysfile', 'search-target.txt', 1)",
		L"INSERT INTO `Signature` (`Signature`, `FileName`) VALUES ('absent', 'no-such-file.bin')",
		L"INSERT INTO `DrLocator` (`Signature_`, `Path`, `Depth`) VALUES ('sysfile', '[TempFolder]', 2)",
		L"INSERT INTO `DrLocator` (`Signature_`, `Path`, `Depth`) VALUES ('sysdir', '[TempFolder]found-dir', 0)",
		L"INSERT INTO `DrLocator` (`Signature_`, `Path`, `Depth`) VALUES ('absent', '[TempFolder]', 2)",
		L"INSERT INTO `RegLocator` (`Signature_`, `Root`, `Key`, `Name`, `Type`) VALUES ('rawvalue', 2, 'Software\\Microsoft\\Windows NT\\CurrentVersion', 'CurrentVersion', 18)",
	};
	for (unsigned i = 0; i < ARRAYSIZE(statements); i++)
		if (!run_sql(database, statements[i]) && i >= 4) // a package may have the empty tables already
			return 100 + i;
	if (MsiDatabaseCommit(database))
		return 3;
	MsiCloseHandle(database);
	if (MsiOpenPackageW(copy, &session))
		return 4;
	if (MsiDoActionW(session, L"AppSearch"))
		return 5;
	WCHAR text[MAX_PATH];
	DWORD size = ARRAYSIZE(text);
	if (MsiGetPropertyW(session, L"FOUND_FILE", text, &size) || lstrcmpiW(text, target))
		return 6;
	size = ARRAYSIZE(text);
	if (MsiGetPropertyW(session, L"FOUND_DIR", text, &size) || lstrcmpiW(text, below))
		return 7;
	size = ARRAYSIZE(text);
	if (MsiGetPropertyW(session, L"NOT_FOUND", text, &size) || size)
		return 8;
	size = ARRAYSIZE(text);
	if (MsiGetPropertyW(session, L"FOUND_REG", text, &size) || !size)
		return 9;

	// Features, in the session that has them.
	if (MsiDoActionW(session, L"CostInitialize") || MsiDoActionW(session, L"FileCost") || MsiDoActionW(session, L"CostFinalize"))
		return 10;
	DWORD attributes = 0, states = 0, title_size = ARRAYSIZE(text);
	if (MsiGetFeatureInfoW(session, L"Main", &attributes, text, &title_size, NULL, NULL))
		return 11;
	if (MsiGetFeatureInfoW(session, L"Nothing", &attributes, NULL, NULL, NULL, NULL) != ERROR_UNKNOWN_FEATURE)
		return 12;
	if (MsiGetFeatureValidStatesW(session, L"Main", &states) || !(states & (1 << INSTALLSTATE_LOCAL)))
		return 13;
	INT cost = -1;
	if (MsiGetFeatureCostW(session, L"Main", MSICOSTTREE_SELFONLY, INSTALLSTATE_LOCAL, &cost) || cost < 1)
		return 14;
	WCHAR drive[8];
	INT temporary = -1;
	DWORD drive_size = ARRAYSIZE(drive);
	if (MsiEnumComponentCostsW(session, NULL, 0, INSTALLSTATE_LOCAL, drive, &drive_size, &cost, &temporary) || drive_size != 2)
		return 15;
	size = ARRAYSIZE(text);
	if (MsiGetPropertyW(session, L"OutOfDiskSpace", text, &size) || lstrcmpW(text, L"0"))
		return 16;
	MsiCloseHandle(session);
	return 0;
}

// An image: the database beside the files unpacked along their tree, and marked as such.
static int check_admin_image(const WCHAR *package, const WCHAR *scratch)
{
	WCHAR copy[MAX_PATH], image[MAX_PATH], line[MAX_PATH + 40], in_image[MAX_PATH];
	join(copy, scratch, L"admin.msi");
	join(image, scratch, L"image");
	if (!CopyFileW(package, copy, FALSE))
		return 1;
	SetFileAttributesW(copy, FILE_ATTRIBUTE_NORMAL);
	MSIHANDLE database = 0;
	if (MsiOpenDatabaseW(copy, MSIDBOPEN_TRANSACT, &database))
		return 2;
	static const WCHAR *const rows[] = {
		L"CREATE TABLE `AdminExecuteSequence` (`Action` CHAR(72) NOT NULL, `Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)",
		L"DELETE FROM `AdminExecuteSequence`",
		L"INSERT INTO `AdminExecuteSequence` (`Action`, `Sequence`) VALUES ('CostInitialize', 800)",
		L"INSERT INTO `AdminExecuteSequence` (`Action`, `Sequence`) VALUES ('FileCost', 900)",
		L"INSERT INTO `AdminExecuteSequence` (`Action`, `Sequence`) VALUES ('CostFinalize', 1000)",
		L"INSERT INTO `AdminExecuteSequence` (`Action`, `Sequence`) VALUES ('InstallValidate', 1400)",
		L"INSERT INTO `AdminExecuteSequence` (`Action`, `Sequence`) VALUES ('InstallInitialize', 1500)",
		L"INSERT INTO `AdminExecuteSequence` (`Action`, `Sequence`) VALUES ('InstallAdminPackage', 3900)",
		L"INSERT INTO `AdminExecuteSequence` (`Action`, `Sequence`) VALUES ('InstallFiles', 4000)",
		L"INSERT INTO `AdminExecuteSequence` (`Action`, `Sequence`) VALUES ('InstallFinalize', 6600)",
	};
	for (unsigned i = 0; i < ARRAYSIZE(rows); i++)
		run_sql(database, rows[i]);
	if (MsiDatabaseCommit(database))
		return 3;
	MsiCloseHandle(database);
	join(line, L"ACTION=ADMIN TARGETDIR=", image);
	UINT status = MsiInstallProductW(copy, line);
	if (status)
		return 1000 + status;
	join(in_image, image, L"\\admin.msi");
	if (GetFileAttributesW(in_image) == INVALID_FILE_ATTRIBUTES)
		return 4;
	MSIHANDLE summary = 0;
	INT words = 0;
	UINT type = 0;
	FILETIME time;
	DWORD room = 0;
	if (MsiGetSummaryInformationW(0, in_image, 0, &summary) || MsiSummaryInfoGetPropertyW(summary, 15, &type, &words, &time, NULL, &room) || (words & 6) != 4)
		return 5;
	MsiCloseHandle(summary);
	// The image installs by itself.
	if (MsiInstallProductW(in_image, L"") != ERROR_SUCCESS)
		return 6;
	if (MsiQueryProductStateW(product) != INSTALLSTATE_DEFAULT)
		return 7;
	if (MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT) != ERROR_SUCCESS)
		return 8;
	return 0;
}

int raven_entry(void)
{
	WCHAR package[MAX_PATH], scratch[MAX_PATH];
	beside(package, L"basic.msi");
	GetTempPathW(MAX_PATH, scratch);
	MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
	if (MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN)
		MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT);
	int result = check_searches(package, scratch);
	if (result)
		return 1000 + result;
	result = check_patching(package, scratch);
	if (result)
		return 2000 + result;
	result = check_admin_image(package, scratch);
	if (result)
		return 4000 + result;
	return 0;
}
