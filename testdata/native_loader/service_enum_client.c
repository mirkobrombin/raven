#include <windows.h>

#define ENUM_GROUP L"RavenEnumGroup"

static int create_service(SC_HANDLE manager, LPCWSTR name, LPCWSTR display)
{
	SC_HANDLE service = CreateServiceW(manager, name, display, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
		SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, L"C:\\Windows\\System32\\svchost.exe", ENUM_GROUP,
		NULL, NULL, NULL, NULL);
	if (!service)
		return 10;
	CloseServiceHandle(service);
	return 0;
}

static int enumerate_w(SC_HANDLE manager)
{
	DWORD needed = 0, count = 0, resume = 0;
	if (EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &needed,
		&count, &resume, ENUM_GROUP) || GetLastError() != ERROR_MORE_DATA || !needed || count)
		return 11;
	BYTE *buffer = HeapAlloc(GetProcessHeap(), 0, needed);
	if (!buffer)
		return 12;
	const DWORD capacity = needed;
	if (!EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer, capacity,
		&needed, &count, &resume, ENUM_GROUP) || count != 2 || resume)
		return 13;
	ENUM_SERVICE_STATUS_PROCESSW *items = (ENUM_SERVICE_STATUS_PROCESSW *)buffer;
	if (lstrcmpW(items[0].lpServiceName, L"RavenEnumA") || lstrcmpW(items[0].lpDisplayName, L"Raven enum A") ||
		lstrcmpW(items[1].lpServiceName, L"RavenEnumB") || lstrcmpW(items[1].lpDisplayName, L"Raven enum B") ||
		items[0].ServiceStatusProcess.dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
		items[0].ServiceStatusProcess.dwCurrentState != SERVICE_STOPPED)
		return 14;
	if (!EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_KERNEL_DRIVER, SERVICE_STATE_ALL, buffer, capacity,
		&needed, &count, &resume, ENUM_GROUP) || count || resume)
		return 15;
	if (!EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, buffer, capacity,
		&needed, &count, &resume, ENUM_GROUP) || count || resume)
		return 16;

	/* Return one entry, then resume at the second entry with the size the first call reports. */
	resume = 0;
	const DWORD one = (DWORD)(sizeof(ENUM_SERVICE_STATUS_PROCESSW) + (lstrlenW(L"RavenEnumA") + 1) * sizeof(WCHAR) +
		(lstrlenW(L"Raven enum A") + 1) * sizeof(WCHAR));
	if (EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer, one, &needed,
		&count, &resume, ENUM_GROUP) || GetLastError() != ERROR_MORE_DATA || count != 1 || !resume)
		return 17;
	DWORD remaining = needed;
	if (!EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer, capacity,
		&remaining, &count, &resume, ENUM_GROUP) || count != 1 || resume ||
		lstrcmpW(((ENUM_SERVICE_STATUS_PROCESSW *)buffer)->lpServiceName, L"RavenEnumB"))
		return 18;
	HeapFree(GetProcessHeap(), 0, buffer);
	return 0;
}

static int enumerate_a(SC_HANDLE manager)
{
	DWORD needed = 0, count = 0, resume = 0;
	if (EnumServicesStatusExA(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &needed,
		&count, &resume, "RavenEnumGroup") || GetLastError() != ERROR_MORE_DATA || !needed || count)
		return 30;
	BYTE *buffer = HeapAlloc(GetProcessHeap(), 0, needed);
	if (!buffer)
		return 31;
	const DWORD capacity = needed;
	if (!EnumServicesStatusExA(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer, capacity,
		&needed, &count, &resume, "RavenEnumGroup") || count != 2 || resume)
		return 32;
	ENUM_SERVICE_STATUS_PROCESSA *items = (ENUM_SERVICE_STATUS_PROCESSA *)buffer;
	if (lstrcmpA(items[0].lpServiceName, "RavenEnumA") || lstrcmpA(items[0].lpDisplayName, "Raven enum A") ||
		lstrcmpA(items[1].lpServiceName, "RavenEnumB") || lstrcmpA(items[1].lpDisplayName, "Raven enum B") ||
		items[0].ServiceStatusProcess.dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
		items[0].ServiceStatusProcess.dwCurrentState != SERVICE_STOPPED)
		return 33;
	if (!EnumServicesStatusExA(manager, SC_ENUM_PROCESS_INFO, SERVICE_KERNEL_DRIVER, SERVICE_STATE_ALL, buffer, capacity,
		&needed, &count, &resume, "RavenEnumGroup") || count || resume)
		return 34;
	if (!EnumServicesStatusExA(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, buffer, capacity,
		&needed, &count, &resume, "RavenEnumGroup") || count || resume)
		return 35;
	HeapFree(GetProcessHeap(), 0, buffer);
	return 0;
}

int raven_entry(void)
{
	SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (!manager)
		return 1;
	int result = create_service(manager, L"RavenEnumA", L"Raven enum A");
	if (!result)
		result = create_service(manager, L"RavenEnumB", L"Raven enum B");
	if (!result)
		result = enumerate_w(manager);
	if (!result)
		result = enumerate_a(manager);
	SC_HANDLE first = OpenServiceW(manager, L"RavenEnumA", DELETE);
	SC_HANDLE second = OpenServiceW(manager, L"RavenEnumB", DELETE);
	if (first) {
		DeleteService(first);
		CloseServiceHandle(first);
	}
	if (second) {
		DeleteService(second);
		CloseServiceHandle(second);
	}
	CloseServiceHandle(manager);
	return result;
}
