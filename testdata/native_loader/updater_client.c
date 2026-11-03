#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <sddl.h>
#include <wincrypt.h>

/* What an updater bootstrapper asks of the system before it installs itself: text and path helpers, the security of a registry key,
   a predefined key standing for another one, the shutdown order of its process, and handing off to its installed copy through the
   shell. Built for both widths. */

static int same(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

/* The copy started by the hand-off finds its argument at the end of the command line. */
static int handed_off(void)
{
	const WCHAR *line = GetCommandLineW();
	size_t length = 0;
	while (line[length])
		++length;
	return length >= 8 && same(line + length - 8, L"/handoff");
}

int raven_entry(void)
{
	if (handed_off())
		return 0;
	WCHAR text[] = L"MiXeD\0Tail";
	if (CharLowerBuffW(text, 10) != 10 || !same(text, L"mixed") || !same(text + 6, L"tail"))
		return 1;
	if (CharUpperBuffW(text, 3) != 3 || !same(text, L"MIXed"))
		return 2;

	WCHAR prefix[MAX_PATH];
	if (PathCommonPrefixW(L"C:\\win\\desktop\\temp.txt", L"c:\\win\\tray\\sample.txt", prefix) != 6 || !same(prefix, L"C:\\win"))
		return 3;
	if (PathCommonPrefixW(L"C:\\a", L"C:\\b", prefix) != 3 || !same(prefix, L"C:\\"))
		return 4;
	if (PathCommonPrefixW(L"C:\\same\\dir", L"C:\\SAME\\DIR", NULL) != 11 || PathCommonPrefixW(L"C:\\a", L"D:\\a", NULL))
		return 5;
	if (PathCommonPrefixW(L"\\\\one\\share", L"\\\\two\\share", NULL))
		return 6;
	if (!PathIsPrefixW(L"C:\\Users\\me", L"c:\\users\\ME\\AppData") || !PathIsPrefixW(L"C:\\", L"C:\\Windows") ||
		PathIsPrefixW(L"C:\\Use", L"C:\\Users") || PathIsPrefixW(L"C:\\Users\\me", L"D:\\Users\\me"))
		return 21;

	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Raven\\Updater", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
		return 7;
	DWORD size = 0;
	if (RegGetKeySecurity(key, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, NULL, &size) != ERROR_INSUFFICIENT_BUFFER || !size)
		return 8;
	BYTE descriptor[256];
	DWORD capacity = sizeof(descriptor);
	if (size > capacity || RegGetKeySecurity(key, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, descriptor, &capacity) ||
		capacity != size || !IsValidSecurityDescriptor(descriptor))
		return 9;

	/* While overridden, HKEY_LOCAL_MACHINE is the key given; what is written below it lands there. */
	HKEY machine;
	if (RegOverridePredefKey(HKEY_LOCAL_MACHINE, key))
		return 10;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Inside", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &machine, NULL))
		return 11;
	RegCloseKey(machine);
	if (RegOverridePredefKey(HKEY_LOCAL_MACHINE, NULL))
		return 12;
	if (RegOpenKeyExW(key, L"Inside", 0, KEY_READ, &machine))
		return 13;
	RegCloseKey(machine);
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Inside", 0, KEY_READ, &machine) != ERROR_FILE_NOT_FOUND)
		return 14;

	const WCHAR value[] = L"seven";
	if (RegSetValueExW(key, L"Name", 0, REG_SZ, (const BYTE *)value, sizeof(value)))
		return 15;
	WCHAR read[16];
	DWORD type = 0, length = sizeof(read);
	if (SHQueryValueExW(key, L"Name", NULL, &type, read, &length) || type != REG_SZ || length != sizeof(value) || !same(read, value))
		return 16;
	RegCloseKey(key);
	RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Raven\\Updater");

	DWORD level = 0, flags = 7;
	if (!GetProcessShutdownParameters(&level, &flags) || level != 0x280 || flags)
		return 17;

	/* What a browser installer asks of ntdll about the machine: a desktop of the version it runs. */
	typedef VOID(WINAPI *family_info)(ULONGLONG *, ULONG *, ULONG *);
	const family_info device_family = (family_info)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetDeviceFamilyInfoEnum");
	ULONGLONG packed = 0;
	ULONG family = 0, form = 7;
	if (!device_family)
		return 22;
	device_family(&packed, &family, &form);
	if (family != 3 || form || packed >> 48 != 10)
		return 23;

	/* A capability SID has ten subauthorities; a SID has at most fifteen, and a dash ends nothing. */
	PSID capability = NULL;
	if (!ConvertStringSidToSidW(L"S-1-15-3-1024-1065365936-1281604716-3511738428-1654721687-432734479-3232135806-4053264122-3456934681",
			&capability) || *GetSidSubAuthorityCount(capability) != 10 || *GetSidSubAuthority(capability, 9) != 3456934681U)
		return 24;
	LocalFree(capability);
	if (ConvertStringSidToSidW(L"S-1-5-1-2-3-4-5-6-7-8-9-10-11-12-13-14-15-16", &capability) || GetLastError() != ERROR_INVALID_SID ||
		ConvertStringSidToSidW(L"S-1-5-", &capability) || ConvertStringSidToSidW(L"S-1-5-4294967296", &capability))
		return 25;

	/* The algorithm table a runtime reads to turn a hash's OID into its ALG_ID before it verifies a signature. */
	PCCRYPT_OID_INFO oid_info = CryptFindOIDInfo(CRYPT_OID_INFO_OID_KEY, (void *)szOID_OIWSEC_sha1, CRYPT_HASH_ALG_OID_GROUP_ID);
	if (!oid_info || oid_info->Algid != CALG_SHA1 || oid_info->dwGroupId != CRYPT_HASH_ALG_OID_GROUP_ID || !same(oid_info->pwszName, L"sha1"))
		return 26;
	ALG_ID wanted = CALG_SHA_512;
	oid_info = CryptFindOIDInfo(CRYPT_OID_INFO_ALGID_KEY, &wanted, 0);
	const char *oid = oid_info ? oid_info->pszOID : "";
	const char sha512[] = "2.16.840.1.101.3.4.2.3";
	for (size_t at = 0; at < sizeof(sha512); ++at)
		if (oid[at] != sha512[at])
			return 27;
	oid_info = CryptFindOIDInfo(CRYPT_OID_INFO_NAME_KEY, (void *)L"SHA256RSA", CRYPT_SIGN_ALG_OID_GROUP_ID);
	if (!oid_info || oid_info->Algid != CALG_SHA_256 || oid_info->ExtraInfo.cbData != 4 || *(ALG_ID *)oid_info->ExtraInfo.pbData != CALG_RSA_SIGN)
		return 28;
	if (CryptFindOIDInfo(CRYPT_OID_INFO_OID_KEY, (void *)"1.2.3.4", 0) ||
		CryptFindOIDInfo(CRYPT_OID_INFO_OID_KEY, (void *)szOID_OIWSEC_sha1, CRYPT_PUBKEY_ALG_OID_GROUP_ID))
		return 29;

#ifdef _WIN64
	/* A hash given its value instead of its data, as a runtime does before it verifies a signature over a hash it computed. */
	HCRYPTPROV provider = 0;
	HCRYPTHASH hash = 0;
	BYTE value_set[20], value_read[20];
	DWORD value_size = sizeof(value_read);
	for (size_t at = 0; at < sizeof(value_set); ++at)
		value_set[at] = (BYTE)(at * 7 + 1);
	if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) || !CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash) ||
		!CryptSetHashParam(hash, HP_HASHVAL, value_set, 0) || !CryptGetHashParam(hash, HP_HASHVAL, value_read, &value_size, 0) || value_size != 20)
		return 30;
	for (size_t at = 0; at < sizeof(value_set); ++at)
		if (value_read[at] != value_set[at])
			return 31;
	CryptDestroyHash(hash);
	CryptReleaseContext(provider, 0);
#endif

	WCHAR self[MAX_PATH];
	if (!GetModuleFileNameW(NULL, self, MAX_PATH))
		return 18;
	SHELLEXECUTEINFOW info;
	for (size_t at = 0; at < sizeof(info); ++at)
		((BYTE *)&info)[at] = 0;
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
	info.lpFile = self;
	info.lpParameters = L"/handoff";
	info.nShow = SW_HIDE;
	if (!ShellExecuteExW(&info) || !info.hProcess || (INT_PTR)info.hInstApp <= 32)
		return 19;
	DWORD code = 1;
	if (WaitForSingleObject(info.hProcess, 20000) != WAIT_OBJECT_0 || !GetExitCodeProcess(info.hProcess, &code) || code)
		return 20;
	CloseHandle(info.hProcess);
	return 0;
}
