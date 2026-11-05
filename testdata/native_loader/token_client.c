#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

typedef LONG (WINAPI *open_token)(HANDLE, DWORD, HANDLE *);
typedef LONG (WINAPI *open_token_ex)(HANDLE, DWORD, ULONG, HANDLE *);
typedef LONG (WINAPI *query_process)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef LONG (WINAPI *set_thread)(HANDLE, ULONG, void *, ULONG);
typedef LONG (WINAPI *open_thread_token)(HANDLE, DWORD, BOOL, HANDLE *);
typedef LONG (WINAPI *open_thread_token_ex)(HANDLE, DWORD, BOOL, ULONG, HANDLE *);
typedef LONG (WINAPI *compare_tokens)(HANDLE, HANDLE, BOOLEAN *);

struct basic_information {
	LONG exit_status;
	void *peb;
	ULONG_PTR affinity;
	LONG priority;
	ULONG_PTR pid, parent;
};

static volatile HANDLE transferred_token;
static volatile HANDLE zero_token;

static int test_mandatory_ace(void)
{
	union { ULONGLONG align; BYTE bytes[64]; } storage;
	PACL acl = (PACL)storage.bytes;
	struct { BYTE revision, count, authority[6]; DWORD level; } sid = {1, 1, {0, 0, 0, 0, 0, 16}, 4096};
	SYSTEM_MANDATORY_LABEL_ACE *ace;
	if (!InitializeAcl(acl, sizeof(storage), ACL_REVISION) ||
		!AddMandatoryAce(acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, &sid) ||
		acl->AceCount != 1 || !GetAce(acl, 0, (void **)&ace) || ace->Header.AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE ||
		ace->Header.AceFlags != (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) || ace->Header.AceSize != 20 ||
		ace->Mask != SYSTEM_MANDATORY_LABEL_NO_WRITE_UP || !EqualSid(&ace->SidStart, &sid))
		return 130;
	if (!AddMandatoryAce(acl, ACL_REVISION_DS, INHERITED_ACE, SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP, &sid) ||
		acl->AceCount != 2 || acl->AclRevision != ACL_REVISION_DS || !GetAce(acl, 1, (void **)&ace) ||
		ace->Header.AceFlags != INHERITED_ACE || ace->Mask != SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP)
		return 131;
	if (!InitializeAcl(acl, sizeof(ACL), ACL_REVISION) || AddMandatoryAce(acl, ACL_REVISION, 0, 1, &sid) ||
		GetLastError() != ERROR_ALLOTTED_SPACE_EXCEEDED || acl->AceCount)
		return 132;
	acl->AceCount = 1;
	if (AddMandatoryAce(acl, ACL_REVISION, 0, 1, &sid) || GetLastError() != ERROR_INVALID_ACL || acl->AceCount != 1)
		return 133;
	if (!InitializeAcl(acl, sizeof(storage), ACL_REVISION) || AddMandatoryAce(acl, 3, 0, 1, &sid) ||
		GetLastError() != ERROR_REVISION_MISMATCH || AddMandatoryAce(acl, ACL_REVISION, 0x20, 1, &sid) ||
		GetLastError() != ERROR_INVALID_PARAMETER || AddMandatoryAce(acl, ACL_REVISION, 0, 8, &sid) || GetLastError() != ERROR_INVALID_PARAMETER || acl->AceCount)
		return 134;
	sid.authority[5] = 5;
	if (AddMandatoryAce(acl, ACL_REVISION, 0, 1, &sid) || GetLastError() != ERROR_INVALID_SID || acl->AceCount)
		return 135;
	return 0;
}

static DWORD traversal(HANDLE token)
{
	TOKEN_PRIVILEGES privileges;
	DWORD size;
	if (!GetTokenInformation(token, TokenPrivileges, &privileges, sizeof(privileges), &size)) return 0xffffffff;
	if (!privileges.PrivilegeCount) return 0x100;
	if (privileges.PrivilegeCount != 1 || size != sizeof(privileges) || privileges.Privileges[0].Luid.LowPart != 23 ||
		privileges.Privileges[0].Luid.HighPart) return 0xffffffff;
	return privileges.Privileges[0].Attributes;
}

static BOOL set_traversal(HANDLE token, DWORD attributes)
{
	TOKEN_PRIVILEGES state = {1, {{{23, 0}, 0}}};
	state.Privileges[0].Attributes = attributes;
	return AdjustTokenPrivileges(token, FALSE, &state, 0, NULL, NULL);
}

static int test_privileges(HANDLE source)
{
	HANDLE token, alias, restricted, limited, duplicate;
	LUID luid;
	TOKEN_PRIVILEGES state = {1, {{{23, 0}, 0}}}, previous, saved;
	struct { DWORD count; LUID_AND_ATTRIBUTES entries[2]; } mixed = {2, {{{23, 0}, 0}, {{20, 0}, SE_PRIVILEGE_ENABLED}}};
	DWORD size;
	if (!LookupPrivilegeValueW(NULL, L"SeChangeNotifyPrivilege", &luid) || luid.LowPart != 23 || luid.HighPart || traversal(source) != 3 ||
		!DuplicateTokenEx(source, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &token) ||
		!DuplicateHandle(GetCurrentProcess(), token, GetCurrentProcess(), &alias, 0, FALSE, DUPLICATE_SAME_ACCESS))
		return 110;
	if (AdjustTokenPrivileges(token, FALSE, &state, sizeof(previous) - 1, &previous, &size) ||
		GetLastError() != ERROR_INSUFFICIENT_BUFFER || size != sizeof(previous) || traversal(alias) != 3)
		return 111;
	if (AdjustTokenPrivileges(token, FALSE, &state, sizeof(previous), (TOKEN_PRIVILEGES *)1, &size) ||
		GetLastError() != ERROR_NOACCESS || traversal(alias) != 3 ||
		AdjustTokenPrivileges(token, FALSE, (TOKEN_PRIVILEGES *)1, 0, NULL, NULL) || GetLastError() != ERROR_NOACCESS || traversal(alias) != 3)
		return 112;
	if (!AdjustTokenPrivileges(token, FALSE, &state, sizeof(previous), &previous, &size) || GetLastError() || size != sizeof(previous) ||
		previous.PrivilegeCount != 1 || previous.Privileges[0].Attributes != 3 || traversal(alias) != 1 || traversal(source) != 3)
		return 113;
	saved = previous;
	if (!AdjustTokenPrivileges(token, FALSE, &state, sizeof(previous), &previous, &size) || GetLastError() || size != sizeof(DWORD) || previous.PrivilegeCount ||
		!AdjustTokenPrivileges(token, FALSE, &saved, 0, NULL, NULL) || GetLastError() || traversal(alias) != 3)
		return 114;
	if (!AdjustTokenPrivileges(alias, TRUE, (TOKEN_PRIVILEGES *)1, sizeof(previous), &previous, &size) || GetLastError() ||
		traversal(token) != 1 || previous.PrivilegeCount != 1 || previous.Privileges[0].Attributes != 3 ||
		!AdjustTokenPrivileges(alias, FALSE, &previous, 0, NULL, NULL) || traversal(token) != 3)
		return 115;
	if (!AdjustTokenPrivileges(token, FALSE, (TOKEN_PRIVILEGES *)&mixed, sizeof(previous), &previous, &size) ||
		GetLastError() != ERROR_NOT_ALL_ASSIGNED || previous.PrivilegeCount != 1 || traversal(alias) != 1 ||
		!AdjustTokenPrivileges(token, FALSE, &previous, 0, NULL, NULL) || GetLastError() || traversal(alias) != 3)
		return 116;
	state.Privileges[0].Attributes = SE_PRIVILEGE_REMOVED;
	if (!AdjustTokenPrivileges(token, FALSE, &state, sizeof(previous), &previous, &size) || GetLastError() ||
		previous.PrivilegeCount || size != sizeof(DWORD) || traversal(alias) != 0x100 ||
		!set_traversal(alias, SE_PRIVILEGE_ENABLED) || GetLastError() != ERROR_NOT_ALL_ASSIGNED || traversal(token) != 0x100 || traversal(source) != 3)
		return 117;
	CloseHandle(token);
	CloseHandle(alias);
	if (!CreateRestrictedToken(source, DISABLE_MAX_PRIVILEGE, 0, NULL, 1, (LUID_AND_ATTRIBUTES *)1, 0, NULL, &restricted) || traversal(restricted) != 3)
		return 118;
	CloseHandle(restricted);
	if (!CreateRestrictedToken(source, 0, 0, NULL, 2, mixed.entries, 0, NULL, &restricted) || traversal(restricted) != 0x100 || traversal(source) != 3)
		return 119;
	CloseHandle(restricted);
	if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &limited, TOKEN_QUERY, FALSE, 0))
		return 120;
	if (AdjustTokenPrivileges(limited, FALSE, &state, 0, NULL, NULL))
		return 124;
	if (GetLastError() != ERROR_ACCESS_DENIED)
		return 200 + GetLastError();
	if (CreateRestrictedToken(limited, 0, 0, NULL, 0, NULL, 0, NULL, &restricted) || GetLastError() != ERROR_ACCESS_DENIED)
		return 125;
	CloseHandle(limited);
	if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &limited, TOKEN_DUPLICATE, FALSE, 0) ||
		!CreateRestrictedToken(limited, 0, 0, NULL, 0, NULL, 0, NULL, &restricted) || traversal(restricted) != 0xffffffff || GetLastError() != ERROR_ACCESS_DENIED ||
		!DuplicateTokenEx(restricted, TOKEN_QUERY, NULL, SecurityImpersonation, TokenPrimary, &duplicate) || traversal(duplicate) != 3)
		return 121;
	CloseHandle(duplicate);
	CloseHandle(limited);
	CloseHandle(restricted);
	if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &limited, TOKEN_ADJUST_PRIVILEGES, FALSE, 0) ||
		AdjustTokenPrivileges(limited, FALSE, &state, sizeof(previous), &previous, &size) || GetLastError() != ERROR_ACCESS_DENIED)
		return 122;
	CloseHandle(limited);
	if (CreateRestrictedToken(source, 0x10, 0, NULL, 0, NULL, 0, NULL, &restricted) || GetLastError() != ERROR_INVALID_PARAMETER ||
		CreateRestrictedToken(source, 0, 0, NULL, 1, (LUID_AND_ATTRIBUTES *)1, 0, NULL, &restricted) || GetLastError() != ERROR_NOACCESS ||
		CreateRestrictedToken(source, 0, 0, NULL, 0, NULL, 0, NULL, (HANDLE *)1) || GetLastError() != ERROR_NOACCESS)
		return 123;
	return 0;
}

static DWORD integrity(HANDLE token)
{
	union { ULONGLONG align; BYTE bytes[64]; } buffer;
	DWORD size;
	TOKEN_MANDATORY_LABEL *label = (TOKEN_MANDATORY_LABEL *)buffer.bytes;
	if (!GetTokenInformation(token, TokenIntegrityLevel, &buffer, sizeof(buffer), &size))
		return 0xffffffff;
	return *GetSidSubAuthority(label->Label.Sid, 0);
}

static BOOL set_integrity(HANDLE token, DWORD level)
{
	struct { BYTE revision, count, authority[6]; DWORD level; } sid = {1, 1, {0, 0, 0, 0, 0, 16}, level};
	TOKEN_MANDATORY_LABEL label;
	label.Label.Sid = &sid;
	label.Label.Attributes = SE_GROUP_INTEGRITY;
	return SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label));
}

static DWORD WINAPI thread_entry(void *parameter)
{
	HANDLE token;
	if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) || integrity(token) != (DWORD)(ULONG_PTR)parameter ||
		integrity((HANDLE)(LONG_PTR)-6) != (DWORD)(ULONG_PTR)parameter || !RevertToSelf())
		return 100;
	CloseHandle(token);
	if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) || GetLastError() != ERROR_NO_TOKEN)
		return 101;
	return 0;
}

static DWORD WINAPI retained_thread_entry(void *parameter)
{
	HANDLE token;
	(void)parameter;
	if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) return 102;
	CloseHandle(token);
	return 0;
}

static int test_thread_token(HANDLE primary)
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	set_thread set = (set_thread)GetProcAddress(ntdll, "NtSetInformationThread");
	open_thread_token open = (open_thread_token)GetProcAddress(ntdll, "NtOpenThreadToken");
	open_thread_token_ex open_ex = (open_thread_token_ex)GetProcAddress(ntdll, "NtOpenThreadTokenEx");
	HANDLE token = NULL, observed = NULL, worker, limited, empty = NULL;
	DWORD size, level, flags, code, id;
	if (!set || !open || !open_ex)
		return 40;
	if ((ULONG)open(GetCurrentThread(), TOKEN_QUERY, TRUE, &observed) != 0xc000007c ||
		integrity((HANDLE)(LONG_PTR)-5) != 0xffffffff || GetLastError() != ERROR_NO_TOKEN)
		return 41;
	if (!DuplicateTokenEx(primary, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &token) ||
		!set_integrity(token, 2048) || set(GetCurrentThread(), 5, &token, sizeof(token)))
		return 42;
	if (open_ex(GetCurrentThread(), TOKEN_QUERY, TRUE, 2, &observed) ||
		!GetHandleInformation(observed, &flags) || !(flags & HANDLE_FLAG_INHERIT) || integrity(observed) != 2048 ||
		integrity((HANDLE)(LONG_PTR)-5) != 2048 || integrity((HANDLE)(LONG_PTR)-6) != 2048 || integrity(primary) != 4096 ||
		!GetTokenInformation(observed, TokenImpersonationLevel, &level, sizeof(level), &size) || level != SecurityImpersonation)
		return 43;
	if ((ULONG)set(GetCurrentThread(), 5, &observed, sizeof(observed)) != 0xc0000022 ||
		(ULONG)set(GetCurrentThread(), 5, &primary, sizeof(primary)) != 0xc00000a8 ||
		(ULONG)set(GetCurrentThread(), 5, &token, sizeof(token) - 1) != 0xc0000004 ||
		(ULONG)set(GetCurrentThread(), 5, (void *)1, sizeof(token)) != 0xc0000005 ||
		(ULONG)set(GetCurrentThread(), 0xffffffff, &token, sizeof(token)) != 0xc0000003 ||
		SetThreadToken((HANDLE *)1, token) || GetLastError() != ERROR_NOACCESS)
		return 44;
	if ((ULONG)open(GetCurrentThread(), TOKEN_QUERY, TRUE, (HANDLE *)1) != 0xc0000005 ||
		(ULONG)open_ex(GetCurrentThread(), TOKEN_QUERY, TRUE, 0x200, &empty) != 0xc000000d || empty)
		return 52;
	if (!set_integrity(token, 1024) || integrity(observed) != 1024 || set(GetCurrentThread(), 5, &empty, sizeof(empty)) ||
		integrity((HANDLE)(LONG_PTR)-6) != 4096 || integrity(observed) != 1024)
		return 45;
	CloseHandle(observed);
	worker = CreateThread(NULL, 0, thread_entry, (void *)(ULONG_PTR)512, CREATE_SUSPENDED, &id);
	if (!worker || !SetThreadToken(&worker, token) || !OpenThreadToken(worker, TOKEN_QUERY, TRUE, &observed) ||
		integrity(observed) != 1024 || !set_integrity(token, 512) || integrity(observed) != 512)
		return 46;
	limited = OpenThread(THREAD_QUERY_INFORMATION, FALSE, id);
	if (!limited || (ULONG)set(limited, 5, &token, sizeof(token)) != 0xc0000022)
		return 47;
	CloseHandle(limited);
	CloseHandle(token);
	if (ResumeThread(worker) != 1 || WaitForSingleObject(worker, 15000) != WAIT_OBJECT_0 ||
		!GetExitCodeThread(worker, &code) || code || integrity(observed) != 512)
		return 48;
	CloseHandle(observed);
	CloseHandle(worker);
	if (!DuplicateTokenEx(primary, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &token)) return 53;
	worker = CreateThread(NULL, 0, retained_thread_entry, NULL, CREATE_SUSPENDED, &id);
	if (!worker || !SetThreadToken(&worker, token)) return 54;
	CloseHandle(token);
	if (ResumeThread(worker) != 1 || WaitForSingleObject(worker, 15000) != WAIT_OBJECT_0 ||
		!GetExitCodeThread(worker, &code) || code || OpenThreadToken(worker, TOKEN_QUERY, TRUE, &observed) || GetLastError() != ERROR_NO_TOKEN)
		return 55;
	CloseHandle(worker);
	if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &observed) || GetLastError() != ERROR_NO_TOKEN ||
		!ImpersonateSelf(SecurityIdentification) || OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, FALSE, &observed) ||
		GetLastError() != ERROR_BAD_IMPERSONATION_LEVEL || !OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &observed))
		return 49;
	CloseHandle(observed);
	if (!RevertToSelf() || !ImpersonateSelf(SecurityAnonymous) || OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &observed) ||
		GetLastError() != ERROR_CANT_OPEN_ANONYMOUS || !RevertToSelf())
		return 50;
	if (!ImpersonateLoggedOnUser(primary) || !OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &observed) ||
		integrity(observed) != 4096 || !RevertToSelf())
		return 51;
	CloseHandle(observed);
	return 0;
}

static int verify_dacl(HANDLE token)
{
	DWORD size = 0, index;
	TOKEN_DEFAULT_DACL *data;
	if (GetTokenInformation(token, TokenDefaultDacl, NULL, 0, &size) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
		size != sizeof(TOKEN_DEFAULT_DACL) + 65532)
		return 1;
	data = HeapAlloc(GetProcessHeap(), 0, size);
	if (!data || !GetTokenInformation(token, TokenDefaultDacl, data, size, &size) || !data->DefaultDacl ||
		data->DefaultDacl->AclSize != 65532 || data->DefaultDacl->AceCount)
		return 2;
	for (index = sizeof(ACL); index < 65532; ++index)
		if (((BYTE *)data->DefaultDacl)[index] != (BYTE)(index * 17 + 3))
			return 3;
	HeapFree(GetProcessHeap(), 0, data);
	return 0;
}

static int child_entry(void)
{
	HANDLE token;
	if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) || integrity(token) != 512 ||
		integrity((HANDLE)(LONG_PTR)-6) != 512 || !RevertToSelf() || integrity(token) != 512)
		return 83;
	CloseHandle(token);
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token) || integrity(token) != 4096 || verify_dacl(token) ||
		traversal(token) != 1 || !set_traversal(token, SE_PRIVILEGE_ENABLED) || GetLastError() || traversal(token) != 3)
		return 80;
	if (traversal(transferred_token) != 3 || set_traversal(transferred_token, 0) || GetLastError() != ERROR_ACCESS_DENIED ||
		integrity(transferred_token) != 2222 || set_integrity(transferred_token, 8192) || GetLastError() != ERROR_ACCESS_DENIED)
		return 81;
	if (integrity(zero_token) != 0xffffffff || GetLastError() != ERROR_ACCESS_DENIED)
		return 82;
	CloseHandle(token);
	CloseHandle(transferred_token);
	CloseHandle(zero_token);
	return 0;
}

static int test_child(open_token open, HANDLE own)
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	query_process query = (query_process)GetProcAddress(ntdll, "NtQueryInformationProcess");
	set_thread set = (set_thread)GetProcAddress(ntdll, "NtSetInformationThread");
	STARTUPINFOW startup;
	PROCESS_INFORMATION child;
	struct basic_information information;
	WCHAR command[512];
	ULONG length;
	ULONG_PTR image;
	SIZE_T copied;
	HANDLE first = NULL, second = NULL, sent[2] = {NULL, NULL};
	HANDLE initial = NULL, observed = NULL;
	BYTE *acl;
	TOKEN_DEFAULT_DACL dacl;
	DWORD used, code = 0;
	unsigned index;
	int result = 0;
	for (index = 0; index < sizeof(startup); ++index)
		((BYTE *)&startup)[index] = 0;
	startup.cb = sizeof(startup);
	command[0] = '"';
	used = GetModuleFileNameW(NULL, command + 1, 480);
	if (!query || !set || !used || used >= 480)
		return 30;
	command[++used] = '"'; command[++used] = ' '; command[++used] = '-'; command[++used] = '-'; command[++used] = 'c'; command[++used] = 0;
	if (!set_integrity(own, 8192) || !CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &child))
		return 31;
	if (open(child.hProcess, TOKEN_ALL_ACCESS, &first) || !OpenProcessToken(child.hProcess, TOKEN_QUERY, &second) ||
		!set_integrity(own, 4096) || integrity(first) != 8192 || !set_integrity(first, 4096) || integrity(second) != 4096 || integrity(own) != 4096) {
		result = 32;
		goto done;
	}
	if (traversal(first) != 3 || !set_traversal(first, 0) || GetLastError() || traversal(second) != 1 || traversal(own) != 3) {
		result = 84;
		goto done;
	}
	acl = HeapAlloc(GetProcessHeap(), 0, 65532);
	if (!acl) { result = 33; goto done; }
	for (index = 0; index < 65532; ++index)
		acl[index] = (BYTE)(index * 17 + 3);
	((ACL *)acl)->AclRevision = ACL_REVISION;
	((ACL *)acl)->Sbz1 = 0;
	((ACL *)acl)->AclSize = 65532;
	((ACL *)acl)->AceCount = 0;
	((ACL *)acl)->Sbz2 = 0;
	dacl.DefaultDacl = (ACL *)acl;
	if (!SetTokenInformation(first, TokenDefaultDacl, &dacl, sizeof(dacl)) || verify_dacl(second))
		result = 34;
	HeapFree(GetProcessHeap(), 0, acl);
	if (result) goto done;
	if (!DuplicateHandle(GetCurrentProcess(), own, child.hProcess, &sent[0], TOKEN_QUERY, FALSE, 0) ||
		!DuplicateHandle(GetCurrentProcess(), own, child.hProcess, &sent[1], 0, FALSE, 0) || !set_integrity(own, 2222)) {
		result = 35;
		goto done;
	}
	if (query(child.hProcess, 0, &information, sizeof(information), &length) ||
		!ReadProcessMemory(child.hProcess, (BYTE *)information.peb + 2 * sizeof(void *), &image, sizeof(image), &copied) ||
		!WriteProcessMemory(child.hProcess, (void *)(image + (ULONG_PTR)&transferred_token - (ULONG_PTR)GetModuleHandleW(NULL)),
			&sent[0], sizeof(sent[0]), &copied) || copied != sizeof(sent[0]) ||
		!WriteProcessMemory(child.hProcess, (void *)(image + (ULONG_PTR)&zero_token - (ULONG_PTR)GetModuleHandleW(NULL)),
			&sent[1], sizeof(sent[1]), &copied) || copied != sizeof(sent[1])) {
		result = 36;
		goto done;
	}
	if (!DuplicateTokenEx(own, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &initial) ||
		!set_integrity(initial, 1024) || set(child.hThread, 5, &initial, sizeof(initial)) ||
		!OpenThreadToken(child.hThread, TOKEN_QUERY, TRUE, &observed) || integrity(observed) != 1024 ||
		!SetThreadToken(&child.hThread, NULL)) {
		result = 38;
		goto done;
	}
	CloseHandle(observed);
	observed = NULL;
	if (OpenThreadToken(child.hThread, TOKEN_QUERY, TRUE, &observed) || GetLastError() != ERROR_NO_TOKEN ||
		!SetThreadToken(&child.hThread, initial) || !set_integrity(initial, 512)) {
		result = 39;
		goto done;
	}
	CloseHandle(initial);
	initial = NULL;
	if (ResumeThread(child.hThread) != 1 || WaitForSingleObject(child.hProcess, 15000) != WAIT_OBJECT_0 ||
		!GetExitCodeProcess(child.hProcess, &code) || code || integrity(first) != 4096 || verify_dacl(first) || traversal(first) != 3 || traversal(own) != 3)
		result = code ? (int)code : 37;
done:
	if (result) {
		TerminateProcess(child.hProcess, 99);
		WaitForSingleObject(child.hProcess, 5000);
	}
	if (first) CloseHandle(first);
	if (second) CloseHandle(second);
	if (initial) CloseHandle(initial);
	if (observed) CloseHandle(observed);
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	return result;
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	open_token open = (open_token)GetProcAddress(ntdll, "NtOpenProcessToken");
	open_token_ex open_ex = (open_token_ex)GetProcAddress(ntdll, "NtOpenProcessTokenEx");
	compare_tokens compare = (compare_tokens)GetProcAddress(ntdll, "NtCompareTokens");
	BOOLEAN equal = FALSE;
	DWORD code = 0;
	HANDLE blind = NULL;
	HANDLE first = NULL, second = NULL, copy = NULL, limited, sentinel = (HANDLE)(ULONG_PTR)0x12345678;
	DWORD flags, type, size, previous;
	void *readonly;
	unsigned index;
	int result;
	for (index = 0; line[index]; ++index)
		if (line[index] == '-' && line[index + 1] == '-' && line[index + 2] == 'c')
			return child_entry();
	if (!open || !open_ex)
		return 1;
	result = test_mandatory_ace();
	if (result)
		return result;
	if (open(GetCurrentProcess(), TOKEN_ALL_ACCESS, &first) || open_ex(GetCurrentProcess(), TOKEN_QUERY, 2, &second) ||
		!GetHandleInformation(second, &flags) || !(flags & HANDLE_FLAG_INHERIT) || !set_integrity(first, 4096) ||
		integrity(second) != 4096 || integrity((HANDLE)(LONG_PTR)-4) != 4096)
		return 2;
	if (set_integrity(second, 8192) || GetLastError() != ERROR_ACCESS_DENIED ||
		DuplicateTokenEx(second, TOKEN_QUERY, NULL, SecurityImpersonation, TokenPrimary, &copy) || GetLastError() != ERROR_ACCESS_DENIED)
		return 3;
	if (!DuplicateTokenEx(first, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &copy) ||
		!GetTokenInformation(copy, TokenType, &type, sizeof(type), &size) || type != TokenImpersonation)
		return 4;
	limited = OpenProcess(PROCESS_VM_READ, FALSE, GetCurrentProcessId());
	if (!limited || (ULONG)open(limited, TOKEN_QUERY, &sentinel) != 0xc0000022 || sentinel != (HANDLE)(ULONG_PTR)0x12345678)
		return 5;
	CloseHandle(limited);
	if ((ULONG)open((HANDLE)(ULONG_PTR)0x12345678, TOKEN_QUERY, &sentinel) != 0xc0000008 ||
		(ULONG)open(GetCurrentThread(), TOKEN_QUERY, &sentinel) != 0xc0000008 ||
		(ULONG)open(GetCurrentProcess(), TOKEN_QUERY, NULL) != 0xc0000005 ||
		(ULONG)open(GetCurrentProcess(), TOKEN_QUERY, (HANDLE *)1) != 0xc0000005 || sentinel != (HANDLE)(ULONG_PTR)0x12345678)
		return 6;
	if ((ULONG)open_ex(GetCurrentProcess(), TOKEN_QUERY, 0x200, &sentinel) != 0xc000000d || sentinel != (HANDLE)(ULONG_PTR)0x12345678)
		return 7;
	readonly = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!readonly || !VirtualProtect(readonly, 4096, PAGE_READONLY, &previous) ||
		(ULONG)open(GetCurrentProcess(), TOKEN_QUERY, (HANDLE *)readonly) != 0xc0000005)
		return 8;
	VirtualFree(readonly, 0, MEM_RELEASE);
	if (open(GetCurrentProcess(), READ_CONTROL, &limited) || integrity(limited) != 0xffffffff || GetLastError() != ERROR_ACCESS_DENIED)
		return 9;
	CloseHandle(limited);
	result = test_privileges(first);
	if (result)
		return result;
	result = test_thread_token(first);
	if (result)
		return result;
	result = test_child(open, first);
	if (result)
		return result;
	if (integrity(first) != 2222 || integrity(copy) != 4096 || !set_integrity(copy, 8192) || integrity(first) != 2222)
		return 10;
	if (!compare || compare(first, second, &equal) || !equal || compare(first, copy, &equal) || equal ||
		(ULONG)compare(first, (HANDLE)(ULONG_PTR)0x12345678, &equal) != 0xc0000008)
		return 136;
	if (!DuplicateTokenEx(first, TOKEN_DUPLICATE, NULL, SecurityImpersonation, TokenPrimary, &blind) ||
		(ULONG)compare(first, blind, &equal) != 0xc0000022)
		return 137;
	CloseHandle(blind);
	limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
	if (!limited || !GetExitCodeProcess(limited, &code) || code != STILL_ACTIVE)
		return 138;
	CloseHandle(limited);
	CloseHandle(first);
	CloseHandle(second);
	CloseHandle(copy);
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &first) || integrity(first) != 2222)
		return 11;
	CloseHandle(first);
	return 0;
}
