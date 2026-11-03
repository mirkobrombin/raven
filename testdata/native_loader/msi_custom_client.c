#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msi.h>

/* A package whose DLL custom actions run in the installer's custom action server: the immediate one reads a package property and
   records it in the registry, directly and through a temporary row of the package's Registry table; the deferred one, on request,
   fails the installation, and the installer rolls back the file it had put in place, runs the rollback action and leaves the product
   unregistered. A commit action runs only after the installation that succeeds. */

/* An action library of the other width writes through that width's view of the registry, as a 32-bit one does on 64-bit Windows. */
#ifndef ACTION_VIEW
#define ACTION_VIEW 0
#endif

static const WCHAR product[] = L"{3F6A2B1C-8D4E-4A7B-9C1D-2E3F4A5B6C7D}";
static const WCHAR component[] = L"{6B5A4C3D-2E1F-4A0B-9C8D-7E6F5A4B3C2D}";

static BOOL same(const WCHAR *a, const WCHAR *b)
{
	while (*a && *a == *b)
		++a, ++b;
	return *a == *b;
}

static BOOL marked(const WCHAR *name)
{
	HKEY key;
	BOOL found = FALSE;
	if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_READ | ACTION_VIEW, &key)) {
		found = !RegQueryValueExW(key, name, NULL, NULL, NULL, NULL);
		RegCloseKey(key);
	}
	return found;
}

static void remove_marker(void)
{
	HKEY key;
	if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_SET_VALUE | ACTION_VIEW, &key)) {
		RegDeleteValueW(key, L"FromCustomAction");
		RegDeleteValueW(key, L"FromRollback");
		RegDeleteValueW(key, L"FromCommit");
		RegCloseKey(key);
	}
	if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_SET_VALUE, &key)) {
		RegDeleteValueW(key, L"FromTemporaryRow");
		RegCloseKey(key);
	}
}

int raven_entry(void)
{
	WCHAR package[MAX_PATH], path[MAX_PATH], value[64];
	unsigned cut = GetModuleFileNameW(NULL, package, MAX_PATH);
	while (cut && package[cut - 1] != L'\\')
		--cut;
	const WCHAR name[] = L"custom.msi";
	for (unsigned i = 0; name[i]; ++i)
		package[cut++] = name[i];
	package[cut] = 0;
	MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
	if (MsiQueryProductStateW(product) == INSTALLSTATE_DEFAULT)
		MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT);
	remove_marker();

	UINT result = MsiInstallProductW(package, L"CA_VALUE=from-the-command-line");
	if (result != ERROR_SUCCESS)
		return 100 + (int)(result % 100);
	HKEY key;
	DWORD size = sizeof value, type = 0;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_READ | ACTION_VIEW, &key) ||
		RegQueryValueExW(key, L"FromCustomAction", NULL, &type, (BYTE *)value, &size) || type != REG_SZ || !same(value, L"from-the-command-line"))
		return 2;
	RegCloseKey(key);
	/* The package's registry rows, the temporary one included, go to the view of the package's own width. */
	size = sizeof value;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_READ, &key) ||
		RegQueryValueExW(key, L"FromTemporaryRow", NULL, &type, (BYTE *)value, &size) || type != REG_SZ || !same(value, L"from-the-command-line"))
		return 6;
	RegCloseKey(key);
	/* The installation succeeded: its commit action ran, its rollback action did not. */
	if (!marked(L"FromCommit") || marked(L"FromRollback"))
		return 7;
	size = MAX_PATH;
	if (MsiGetComponentPathW(product, component, path, &size) != INSTALLSTATE_LOCAL)
		return 3;
	if (MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT) != ERROR_SUCCESS || MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN)
		return 4;
	remove_marker();

	/* The deferred action fails after the file was installed: the whole installation is undone. */
	result = MsiInstallProductW(package, L"FAIL=1");
	if (result != ERROR_INSTALL_FAILURE)
		return 200 + (int)(result % 100);
	if (MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
		return 5;
	if (!marked(L"FromRollback") || marked(L"FromCommit"))
		return 8;
	return 0;
}
