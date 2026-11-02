/* NetWkstaGetInfo: the local workstation at each level everyone may ask for, by no server name or this computer's own, and its errors. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <lm.h>

static int same_name(const WCHAR *left, const WCHAR *right)
{
	return CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, left, -1, right, -1) == CSTR_EQUAL;
}

static int check(LPWSTR server, const WCHAR *computer, const WCHAR *group)
{
	OSVERSIONINFOW version;
	typedef LONG (WINAPI *version_type)(OSVERSIONINFOW *);
	version_type real_version = (version_type)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	WKSTA_INFO_100 *info100 = NULL;
	WKSTA_INFO_101 *info101 = NULL;
	WKSTA_INFO_102 *info102 = NULL;
	version.dwOSVersionInfoSize = sizeof version;
	if (!real_version || real_version(&version))
		return 1;
	if (NetWkstaGetInfo(server, 100, (LPBYTE *)&info100) != NERR_Success || !info100)
		return 2;
	if (info100->wki100_platform_id != PLATFORM_ID_NT || !same_name(info100->wki100_computername, computer) ||
		!same_name(info100->wki100_langroup, group) || info100->wki100_ver_major != version.dwMajorVersion ||
		info100->wki100_ver_minor != version.dwMinorVersion)
		return 3;
	if (NetApiBufferFree(info100) != NERR_Success)
		return 4;
	if (NetWkstaGetInfo(server, 101, (LPBYTE *)&info101) != NERR_Success || !info101)
		return 5;
	if (info101->wki101_platform_id != PLATFORM_ID_NT || !same_name(info101->wki101_computername, computer) ||
		!same_name(info101->wki101_langroup, group) || info101->wki101_ver_major != version.dwMajorVersion || !info101->wki101_lanroot)
		return 6;
	NetApiBufferFree(info101);
	if (NetWkstaGetInfo(server, 102, (LPBYTE *)&info102) != NERR_Success || !info102)
		return 7;
	if (info102->wki102_platform_id != PLATFORM_ID_NT || !same_name(info102->wki102_computername, computer) ||
		!same_name(info102->wki102_langroup, group) || !info102->wki102_lanroot || info102->wki102_logged_on_users < 1)
		return 8;
	NetApiBufferFree(info102);
	return 0;
}

int raven_entry(void)
{
	WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1], server[MAX_COMPUTERNAME_LENGTH + 3] = L"\\\\", group[] = L"WORKGROUP";
	WCHAR remote[] = L"\\\\RAVEN-NO-SUCH-SERVER";
	DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
	LPWSTR joined = NULL;
	NETSETUP_JOIN_STATUS status;
	LPBYTE buffer = (LPBYTE)1;
	int result;
	if (!GetComputerNameW(computer, &size))
		return 10;
	lstrcatW(server, computer);
	/* The workgroup is the one the join information names. */
	if (NetGetJoinInformation(NULL, &joined, &status) != NERR_Success || status != NetSetupWorkgroupName || !same_name(joined, group))
		return 11;
	NetApiBufferFree(joined);
	if ((result = check(NULL, computer, group)))
		return 20 + result;
	if ((result = check(server, computer, group)))
		return 40 + result;
	if ((result = check(server + 2, computer, group)))
		return 60 + result;
	/* Only the levels every user may read are answered; the buffer is cleared on failure. */
	if (NetWkstaGetInfo(NULL, 99, &buffer) != ERROR_INVALID_LEVEL || buffer)
		return 80;
	if (NetWkstaGetInfo(NULL, 100, NULL) != ERROR_INVALID_PARAMETER)
		return 81;
	buffer = (LPBYTE)1;
	if (NetWkstaGetInfo(remote, 100, &buffer) != ERROR_BAD_NETPATH || buffer)
		return 82;
	return 0;
}
