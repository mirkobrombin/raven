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
		&count, &resume, ENUM_GROUP))
		return 111;
	if (GetLastError() != ERROR_MORE_DATA)
		return 112;
	if (!needed)
		return 113;
	if (count)
		return 114;
	BYTE *buffer = HeapAlloc(GetProcessHeap(), 0, needed);
	if (!buffer)
		return 12;
	const DWORD capacity = needed;
	if (!EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer, capacity,
		&needed, &count, &resume, ENUM_GROUP) || count != 2 || resume)
		return 13;
	ENUM_SERVICE_STATUS_PROCESSW *items = (ENUM_SERVICE_STATUS_PROCESSW *)buffer;
	const BOOL first_is_a = !lstrcmpW(items[0].lpServiceName, L"RavenEnumA");
	const BOOL first_is_b = !lstrcmpW(items[0].lpServiceName, L"RavenEnumB");
	if ((!first_is_a && !first_is_b) ||
		lstrcmpW(items[0].lpDisplayName, first_is_a ? L"Raven enum A" : L"Raven enum B") ||
		lstrcmpW(items[1].lpServiceName, first_is_a ? L"RavenEnumB" : L"RavenEnumA") ||
		lstrcmpW(items[1].lpDisplayName, first_is_a ? L"Raven enum B" : L"Raven enum A") ||
		items[0].ServiceStatusProcess.dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
		items[0].ServiceStatusProcess.dwCurrentState != SERVICE_STOPPED ||
		items[1].ServiceStatusProcess.dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
		items[1].ServiceStatusProcess.dwCurrentState != SERVICE_STOPPED)
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
	if (!EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer, remaining,
		&remaining, &count, &resume, ENUM_GROUP) || count != 1 || resume ||
		lstrcmpW(((ENUM_SERVICE_STATUS_PROCESSW *)buffer)->lpServiceName, first_is_a ? L"RavenEnumB" : L"RavenEnumA"))
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
	const BOOL first_is_a = !lstrcmpA(items[0].lpServiceName, "RavenEnumA");
	const BOOL first_is_b = !lstrcmpA(items[0].lpServiceName, "RavenEnumB");
	if ((!first_is_a && !first_is_b) ||
		lstrcmpA(items[0].lpDisplayName, first_is_a ? "Raven enum A" : "Raven enum B") ||
		lstrcmpA(items[1].lpServiceName, first_is_a ? "RavenEnumB" : "RavenEnumA") ||
		lstrcmpA(items[1].lpDisplayName, first_is_a ? "Raven enum B" : "Raven enum A") ||
		items[0].ServiceStatusProcess.dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
		items[0].ServiceStatusProcess.dwCurrentState != SERVICE_STOPPED ||
		items[1].ServiceStatusProcess.dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
		items[1].ServiceStatusProcess.dwCurrentState != SERVICE_STOPPED)
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
