// Calls the EA app installer's bootstrapper makes: it writes a hash as hexadecimal text, reads Base64 back, imports a certificate's
// RSA key, lists the entries of an access list, asks for the computer's name and the region in ANSI, and before it starts its elevated
// part compares the final path of each folder it walks with the one it built. The exit code names the first check that failed.
#include <windows.h>
#include <wincrypt.h>
#include <aclapi.h>

static int same(const void *left, const void *right, DWORD size)
{
	const BYTE *a = left, *b = right;
	for (DWORD at = 0; at < size; ++at)
		if (a[at] != b[at])
			return 0;
	return 1;
}

static int text(void)
{
	static const BYTE bytes[] = {0xde, 0xad, 0xbe, 0xef, 0x01};
	char hex[16];
	DWORD length = 0;
	/* The length asked counts the terminator; the one written after a copy does not. */
	if (!CryptBinaryToStringA(bytes, sizeof bytes, CRYPT_STRING_HEXRAW | CRYPT_STRING_NOCRLF, NULL, &length) || length != 11)
		return 10;
	length = sizeof hex;
	if (!CryptBinaryToStringA(bytes, sizeof bytes, CRYPT_STRING_HEXRAW | CRYPT_STRING_NOCRLF, hex, &length) || length != 10 ||
		!same(hex, "deadbeef01", 11))
		return 11;
	length = 4;
	if (CryptBinaryToStringA(bytes, sizeof bytes, CRYPT_STRING_HEXRAW | CRYPT_STRING_NOCRLF, hex, &length) || GetLastError() != ERROR_MORE_DATA)
		return 12;
	WCHAR base64[64];
	length = 64;
	if (!CryptBinaryToStringW(bytes, sizeof bytes, CRYPT_STRING_BASE64, base64, &length) || length != 10 ||
		!same(base64, L"3q2+7wE=\r\n", 11 * sizeof(WCHAR)))
		return 13;
	BYTE back[8];
	DWORD size = sizeof back, skipped = 9, form = 9;
	if (!CryptStringToBinaryW(base64, 0, CRYPT_STRING_BASE64, back, &size, &skipped, &form) || size != sizeof bytes ||
		!same(back, bytes, sizeof bytes) || skipped || form != CRYPT_STRING_BASE64)
		return 14;
	static const char framed[] = "-----BEGIN CERTIFICATE-----\r\n3q2+7wE=\r\n-----END CERTIFICATE-----\r\n";
	size = 0;
	if (!CryptStringToBinaryA(framed, 0, CRYPT_STRING_BASE64_ANY, NULL, &size, NULL, &form) || size != sizeof bytes ||
		form != CRYPT_STRING_BASE64HEADER)
		return 15;
	if (CryptStringToBinaryA("not base64!", 0, CRYPT_STRING_BASE64, back, &size, NULL, NULL))
		return 16;
	return 0;
}

static int key(void)
{
	/* A DER RSAPublicKey: a 512-bit modulus (with the zero byte that keeps it positive) and the exponent 65537. */
	BYTE der[2 + 3 + 64 + 5] = {0x30, sizeof der - 2, 0x02, 0x41, 0x00};
	for (int at = 0; at < 64; ++at)
		der[5 + at] = (BYTE)(at == 0 ? 0xc5 : at * 7 + 1);
	BYTE *exponent = der + 5 + 64;
	exponent[0] = 0x02, exponent[1] = 0x03, exponent[2] = 0x01, exponent[3] = 0x00, exponent[4] = 0x01;
	CERT_PUBLIC_KEY_INFO info = {{szOID_RSA_RSA, {0, NULL}}, {sizeof der, der, 0}};
	HCRYPTPROV provider = 0;
	HCRYPTKEY imported = 0;
	if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
		return 20;
	if (!CryptImportPublicKeyInfo(provider, X509_ASN_ENCODING, &info, &imported))
		return 21;
	BYTE blob[128];
	DWORD size = sizeof blob;
	if (!CryptExportKey(imported, 0, PUBLICKEYBLOB, 0, blob, &size) || size != 20 + 64)
		return 22;
	const RSAPUBKEY *rsa = (const RSAPUBKEY *)(blob + sizeof(BLOBHEADER));
	if (blob[0] != PUBLICKEYBLOB || rsa->magic != 0x31415352 || rsa->bitlen != 512 || rsa->pubexp != 65537)
		return 23;
	/* The blob keeps the modulus least significant byte first. */
	for (int at = 0; at < 64; ++at)
		if (blob[20 + at] != der[5 + 63 - at])
			return 24;
	info.Algorithm.pszObjId = "1.2.840.10045.2.1";
	HCRYPTKEY other = 0;
	if (CryptImportPublicKeyInfo(provider, X509_ASN_ENCODING, &info, &other))
		return 25;
	CryptDestroyKey(imported);
	CryptReleaseContext(provider, 0);
	return 0;
}

static int entries(void)
{
	SID_IDENTIFIER_AUTHORITY world = {SECURITY_WORLD_SID_AUTHORITY};
	PSID everyone = NULL;
	if (!AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone))
		return 30;
	EXPLICIT_ACCESS_W given[2] = {0};
	given[0].grfAccessPermissions = GENERIC_READ;
	given[0].grfAccessMode = GRANT_ACCESS;
	given[0].grfInheritance = CONTAINER_INHERIT_ACE;
	given[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	given[0].Trustee.ptstrName = (LPWSTR)everyone;
	given[1] = given[0];
	given[1].grfAccessPermissions = DELETE;
	given[1].grfAccessMode = DENY_ACCESS;
	given[1].grfInheritance = NO_INHERITANCE;
	PACL acl = NULL;
	if (SetEntriesInAclW(2, given, NULL, &acl) != ERROR_SUCCESS)
		return 31;
	ULONG count = 0;
	EXPLICIT_ACCESS_W *found = NULL;
	if (GetExplicitEntriesFromAclW(acl, &count, &found) != ERROR_SUCCESS || count != 2 || !found)
		return 32;
	for (ULONG index = 0; index < 2; ++index) {
		const EXPLICIT_ACCESS_W *entry = found + index;
		if (entry->grfAccessPermissions != given[index].grfAccessPermissions || entry->grfAccessMode != given[index].grfAccessMode ||
			entry->grfInheritance != given[index].grfInheritance || entry->Trustee.TrusteeForm != TRUSTEE_IS_SID ||
			!EqualSid((PSID)entry->Trustee.ptstrName, everyone))
			return 33;
	}
	LocalFree(found);
	LocalFree(acl);
	FreeSid(everyone);
	return 0;
}

static int names(void)
{
	char narrow[64];
	WCHAR wide[64];
	DWORD narrow_length = sizeof narrow, wide_length = 64;
	if (!GetComputerNameExA(ComputerNameDnsHostname, narrow, &narrow_length) ||
		!GetComputerNameExW(ComputerNameDnsHostname, wide, &wide_length) || narrow_length != wide_length || !narrow_length)
		return 40;
	for (DWORD at = 0; at < narrow_length; ++at)
		if ((WCHAR)(BYTE)narrow[at] != wide[at])
			return 41;
	char region[8];
	if (GetGeoInfoA(244, GEO_ISO2, region, sizeof region, 0) != 3 || !same(region, "US", 3))
		return 42;
	return 0;
}

static int length_of(const WCHAR *text)
{
	int length = 0;
	while (text[length])
		++length;
	return length;
}

static int final_paths(void)
{
	WCHAR temp[MAX_PATH], final[MAX_PATH + 8];
	DWORD length = GetTempPathW(MAX_PATH, temp);
	if (!length || temp[length - 1] != '\\')
		return 50;
	/* The folder opened by a name that ends in a backslash: its final path has none. */
	HANDLE folder = CreateFileW(temp, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (folder == INVALID_HANDLE_VALUE)
		return 51;
	DWORD got = GetFinalPathNameByHandleW(folder, final, MAX_PATH + 8, VOLUME_NAME_DOS);
	if (got != length - 1 + 4 || got != (DWORD)length_of(final) || !same(final, L"\\\\?\\", 4 * sizeof(WCHAR)) ||
		!same(final + 4, temp, (length - 1) * sizeof(WCHAR)))
		return 52;
	/* Too small a buffer is told the length with the terminator. */
	if (GetFinalPathNameByHandleW(folder, final, 4, VOLUME_NAME_DOS) != got + 1)
		return 53;
	if (GetFinalPathNameByHandleW(folder, final, MAX_PATH + 8, VOLUME_NAME_NONE) != length - 1 - 2 || !same(final, temp + 2, (length - 3) * sizeof(WCHAR)))
		return 54;
	char narrow[MAX_PATH + 8];
	if (GetFinalPathNameByHandleA(folder, narrow, sizeof narrow, VOLUME_NAME_DOS) != got || narrow[4] != (char)temp[0])
		return 55;
	CloseHandle(folder);
	/* A drive's root keeps its backslash. */
	HANDLE root = CreateFileW(L"C:\\", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (root == INVALID_HANDLE_VALUE)
		return 56;
	if (GetFinalPathNameByHandleW(root, final, MAX_PATH, VOLUME_NAME_DOS) != 7 || !same(final, L"\\\\?\\C:\\", 8 * sizeof(WCHAR)))
		return 57;
	CloseHandle(root);
	return 0;
}

int raven_entry(void)
{
	int failed;
	if ((failed = text()) || (failed = key()) || (failed = entries()) || (failed = names()) || (failed = final_paths()))
		return failed;
	return 0;
}
