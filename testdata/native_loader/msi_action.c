#include <windows.h>
#include <msi.h>
#include <msiquery.h>

/* Custom actions of the test package, run by the installer's custom action server: one records a package property in the registry,
   directly and through a temporary row of the package's Registry table; two more mark where the installer runs its rollback and commit
   actions, and the last fails the installation so that the installer rolls back what it did. */

__declspec(dllexport) UINT __stdcall WriteMarker(MSIHANDLE install)
{
	WCHAR value[64];
	DWORD size = 64;
	HKEY key;
	if (MsiGetPropertyW(install, L"CA_VALUE", value, &size) != ERROR_SUCCESS || !size)
		return ERROR_INSTALL_FAILURE;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL))
		return ERROR_INSTALL_FAILURE;
	RegSetValueExW(key, L"FromCustomAction", 0, REG_SZ, (const BYTE *)value, (size + 1) * sizeof(WCHAR));
	RegCloseKey(key);
	/* A temporary row added through a view that never ran, as actions that generate registry entries do: the installer writes it with
	   the package's own. */
	MSIHANDLE database = MsiGetActiveDatabase(install), view = 0, row = MsiCreateRecord(6);
	UINT status = MsiDatabaseOpenViewW(database, L"SELECT * FROM `Registry`", &view);
	MsiRecordSetStringW(row, 1, L"TemporaryRow");
	MsiRecordSetInteger(row, 2, 2);
	MsiRecordSetStringW(row, 3, L"Software\\RavenSample");
	MsiRecordSetStringW(row, 4, L"FromTemporaryRow");
	MsiRecordSetStringW(row, 5, value);
	MsiRecordSetStringW(row, 6, L"Payload");
	if (!status)
		status = MsiViewModify(view, MSIMODIFY_INSERT_TEMPORARY, row);
	MsiCloseHandle(row);
	MsiCloseHandle(view);
	MsiCloseHandle(database);
	return status ? ERROR_INSTALL_FAILURE : ERROR_SUCCESS;
}

/* The rollback and commit actions leave a mark only in the mode the installer runs them in. */
static UINT mark(MSIHANDLE install, MSIRUNMODE mode, const WCHAR *name)
{
	HKEY key;
	if (!MsiGetMode(install, mode))
		return ERROR_INSTALL_FAILURE;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL))
		return ERROR_INSTALL_FAILURE;
	RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)L"yes", sizeof L"yes");
	RegCloseKey(key);
	return ERROR_SUCCESS;
}

__declspec(dllexport) UINT __stdcall RecordRollback(MSIHANDLE install)
{
	return mark(install, MSIRUNMODE_ROLLBACK, L"FromRollback");
}

__declspec(dllexport) UINT __stdcall RecordCommit(MSIHANDLE install)
{
	return mark(install, MSIRUNMODE_COMMIT, L"FromCommit");
}

__declspec(dllexport) UINT __stdcall FailInstall(MSIHANDLE install)
{
	return ERROR_INSTALL_FAILURE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
