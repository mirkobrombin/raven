#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msi.h>
#include <msiquery.h>

/* A Windows Installer package installed without a user interface, as setup programs and redistributables run it, and removed again:
   the product is registered, its file is in place and readable, its registry value is written, and removal takes all of it away. */

static const WCHAR product[] = L"{7B1E3C52-9A4D-4F60-8C2E-1D5A6B7C8D90}";
static const WCHAR component[] = L"{0C8A1E44-6D2B-4F7A-8B3E-5A9C1D2E3F40}";

static void beside(WCHAR *target, const WCHAR *name)
{
	unsigned cut = GetModuleFileNameW(NULL, target, MAX_PATH);
	while (cut && target[cut - 1] != L'\\')
		--cut;
	for (unsigned i = 0; name[i]; ++i)
		target[cut++] = name[i];
	target[cut] = 0;
}

static BOOL same(const char *a, const char *b, DWORD count)
{
	for (DWORD i = 0; i < count; ++i)
		if (a[i] != b[i])
			return FALSE;
	return TRUE;
}

int raven_entry(void)
{
	WCHAR package[MAX_PATH], path[MAX_PATH];
	beside(package, L"basic.msi");
	MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
	/* A prefix used before starts without the product. */
	if (MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN)
		MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT);
	if (MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN)
		return 1;
	/* Advertised: published with the feature on offer; removal forgets it, a configure installs it. */
	if (MsiAdvertiseProductW(package, L"x.aas", NULL, 0) != ERROR_CALL_NOT_IMPLEMENTED)
		return 20;
	if (MsiAdvertiseProductW(package, (LPCWSTR)0, NULL, 0) != ERROR_SUCCESS)
		return 21;
	if (MsiQueryProductStateW(product) != INSTALLSTATE_ADVERTISED)
		return 22;
	if (MsiQueryFeatureStateW(product, L"Main") != INSTALLSTATE_ADVERTISED)
		return 23;
	if (MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT) != ERROR_SUCCESS)
		return 24;
	if (MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN)
		return 25;
	if (MsiAdvertiseProductW(package, (LPCWSTR)0, NULL, 0) != ERROR_SUCCESS)
		return 26;
	if (MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_DEFAULT) != ERROR_SUCCESS)
		return 27;
	if (MsiQueryProductStateW(product) != INSTALLSTATE_DEFAULT || MsiQueryFeatureStateW(product, L"Main") != INSTALLSTATE_LOCAL)
		return 28;
	/* Each use of an installed feature is counted, with the date of the last one. */
	DWORD before = MAXDWORD, uses = 0;
	WORD last = 0;
	if (MsiGetFeatureUsageW(product, L"Main", &before, &last) != ERROR_SUCCESS || before == MAXDWORD)
		return 33;
	if (MsiUseFeatureW(product, L"Main") != INSTALLSTATE_LOCAL || MsiUseFeatureW(product, L"Main") != INSTALLSTATE_LOCAL)
		return 34;
	SYSTEMTIME today;
	GetLocalTime(&today);
	if (MsiGetFeatureUsageW(product, L"Main", &uses, &last) != ERROR_SUCCESS || uses != before + 2 || (last & 31) != today.wDay ||
		MsiGetFeatureUsageW(product, L"Absent", &uses, &last) != ERROR_UNKNOWN_FEATURE)
		return 35;
	if (MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT) != ERROR_SUCCESS)
		return 29;
	/* After costing, a directory is the property of its name: text like [INSTALLDIR] depends on it. */
	MSIHANDLE session = 0;
	if (MsiOpenPackageW(package, &session) != ERROR_SUCCESS)
		return 30;
	WCHAR folder[MAX_PATH];
	DWORD folder_size = MAX_PATH;
	if (MsiDoActionW(session, L"CostInitialize") != ERROR_SUCCESS || MsiDoActionW(session, L"CostFinalize") != ERROR_SUCCESS)
		return 31;
	if (MsiGetPropertyW(session, L"INSTALLDIR", folder, &folder_size) != ERROR_SUCCESS || folder_size < 13 ||
		lstrcmpiW(folder + folder_size - 13, L"\\RavenSample\\"))
		return 32;
	MsiCloseHandle(session);
	UINT result = MsiInstallProductW(package, L"");
	if (result != ERROR_SUCCESS)
		return 100 + (int)(result % 100);
	if (MsiQueryProductStateW(product) != INSTALLSTATE_DEFAULT)
		return 2;
	DWORD size = MAX_PATH;
	if (MsiGetComponentPathW(product, component, path, &size) != INSTALLSTATE_LOCAL)
		return 3;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	char text[64];
	DWORD read = 0;
	if (file == INVALID_HANDLE_VALUE || !ReadFile(file, text, sizeof text, &read, NULL) || read != 36 ||
		!same(text, "Installed by the Windows Installer.\n", 36))
		return 4;
	CloseHandle(file);
	HKEY key;
	DWORD value = 0, type = 0;
	size = sizeof value;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_READ, &key) ||
		RegQueryValueExW(key, L"Installed", NULL, &type, (BYTE *)&value, &size) || type != REG_DWORD || value != 1)
		return 5;
	RegCloseKey(key);
	size = MAX_PATH;
	WCHAR version[32];
	if (MsiGetProductInfoW(product, L"VersionString", version, &size) != ERROR_SUCCESS || size != 5)
		return 6;

	if (MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT) != ERROR_SUCCESS)
		return 7;
	if (MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
		return 8;
	if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_READ, &key) &&
		!RegQueryValueExW(key, L"Installed", NULL, NULL, NULL, NULL))
		return 9;
	/* Gone from the list of products too, although its use was counted. */
	WCHAR listed[39];
	for (DWORD index = 0; MsiEnumProductsW(index, listed) == ERROR_SUCCESS; index++)
		if (!lstrcmpiW(listed, product))
			return 36;
	return 0;
}
