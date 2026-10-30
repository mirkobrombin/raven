#include <windows.h>

// ntdll's token calls, which the Cygwin runtime makes at start: NtQueryInformationToken answers what GetTokenInformation does and
// reports a short buffer as STATUS_BUFFER_TOO_SMALL with the length needed, and NtAdjustPrivilegesToken answers STATUS_SUCCESS for
// a privilege the token holds and STATUS_NOT_ALL_ASSIGNED for one it lacks. Returns 0, or the number of the check that failed.
typedef LONG NTSTATUS;
__declspec(dllimport) NTSTATUS NTAPI NtQueryInformationToken(HANDLE, TOKEN_INFORMATION_CLASS, void *, ULONG, ULONG *);
__declspec(dllimport) NTSTATUS NTAPI NtAdjustPrivilegesToken(HANDLE, BOOLEAN, TOKEN_PRIVILEGES *, ULONG, TOKEN_PRIVILEGES *, ULONG *);

int raven_entry(void)
{
	HANDLE token;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token))
		return 1;
	ULONG needed = 0;
	if (NtQueryInformationToken(token, TokenUser, NULL, 0, &needed) != (NTSTATUS)0xc0000023 || needed < sizeof(TOKEN_USER))
		return 2;
	static unsigned char user[256];
	if (needed > sizeof user || NtQueryInformationToken(token, TokenUser, user, sizeof user, &needed) != 0 ||
		!IsValidSid(((TOKEN_USER *)user)->User.Sid))
		return 3;
	// SeChangeNotifyPrivilege (LUID 23) is every token's; LUID 9999 names none.
	TOKEN_PRIVILEGES held = {1, {{{23, 0}, SE_PRIVILEGE_ENABLED}}}, lacking = {1, {{{9999, 0}, SE_PRIVILEGE_ENABLED}}};
	if (NtAdjustPrivilegesToken(token, FALSE, &held, 0, NULL, NULL) != 0)
		return 4;
	if (NtAdjustPrivilegesToken(token, FALSE, &lacking, 0, NULL, NULL) != 0x106)
		return 5;
	CloseHandle(token);
	return 0;
}
