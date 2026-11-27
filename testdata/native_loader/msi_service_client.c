// The Windows Installer service started as installers start it before they hand it a package: by name through the service
// manager, which runs msiexec /V from the command the registry gives with %SystemRoot%. It reports that it runs, and it stops when
// told to. The exit code names the first check that failed.
#include <windows.h>

static DWORD wait_for(SC_HANDLE service, DWORD state)
{
	SERVICE_STATUS status;
	for (int tries = 0; tries < 300; ++tries) {
		if (!QueryServiceStatus(service, &status))
			return 0;
		if (status.dwCurrentState == state)
			return state;
		Sleep(100);
	}
	return status.dwCurrentState;
}

int raven_entry(void)
{
	SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (!manager)
		return 1;
	SC_HANDLE service = OpenServiceW(manager, L"MSIServer", SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
	if (!service)
		return 2;
	SERVICE_STATUS status;
	if (!QueryServiceStatus(service, &status) || status.dwCurrentState != SERVICE_STOPPED)
		return 3;
	if (!StartServiceW(service, 0, NULL))
		return 4;
	if (wait_for(service, SERVICE_RUNNING) != SERVICE_RUNNING)
		return 5;
	if (!ControlService(service, SERVICE_CONTROL_STOP, &status))
		return 6;
	if (wait_for(service, SERVICE_STOPPED) != SERVICE_STOPPED)
		return 7;
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	return 0;
}
