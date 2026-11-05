#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msi.h>

/* A package whose VBScript and JScript custom actions run in the installer: one in each language works through the language and the
   session and leaves its findings in a property the package writes to the registry, one inline script in each does the same, a deferred
   one checks the data it was handed. A script that answers failure, and one stopped by an error it does not handle, fail the
   installation. */

static const WCHAR product[] = L"{7C2E9A41-5B3D-4F6E-8A1C-0D9B8E7F6A51}";

static BOOL value_is(const WCHAR *name, const WCHAR *expected)
{
	WCHAR value[128];
	DWORD size = sizeof value, type = 0;
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenScript", 0, KEY_READ, &key))
		return FALSE;
	BOOL same = !RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &size) && type == REG_SZ && !lstrcmpW(value, expected);
	RegCloseKey(key);
	return same;
}

int raven_entry(void)
{
	WCHAR package[MAX_PATH];
	unsigned cut = GetModuleFileNameW(NULL, package, MAX_PATH);
	while (cut && package[cut - 1] != L'\\')
		--cut;
	const WCHAR name[] = L"script.msi";
	for (unsigned i = 0; name[i]; ++i)
		package[cut++] = name[i];
	package[cut] = 0;
	MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
	if (MsiQueryProductStateW(product) == INSTALLSTATE_DEFAULT)
		MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT);

	UINT result = MsiInstallProductW(package, NULL);
	if (result != ERROR_SUCCESS)
		return 100 + (int)(result % 100);
	if (!value_is(L"Result", L"RAVEN-12-twelve-caught-condition-a+B+c-bcd-3-9-42"))
		return 2;
	if (!value_is(L"Inline", L"inline 42"))
		return 3;
	if (!value_is(L"JScript", L"RAVEN-12-caught-condition-1+2+3"))
		return 6;
	if (!value_is(L"JInline", L"inline 42"))
		return 7;
	if (MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT) != ERROR_SUCCESS)
		return 4;

	result = MsiInstallProductW(package, L"FAIL=1");
	if (result != ERROR_INSTALL_FAILURE)
		return 200 + (int)(result % 100);
	result = MsiInstallProductW(package, L"BROKEN=1");
	if (result != ERROR_INSTALL_FAILURE)
		return 300 + (int)(result % 100);
	if (MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN)
		return 5;
	return 0;
}
