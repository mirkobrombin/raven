#include <windows.h>
#include <wincrypt.h>

/* The structures crypt32 hands back for a signed message have the layout of wincrypt.h at the program's width: the signer of
 * signed-message.der (beside this program, made by openssl with serial number 0102...0f10 and a critical basicConstraints) and its
 * certificate are read through their fields as a program reads them. The exit code names the first check that failed. */
static int starts_with(const char *text, const char *prefix)
{
	while (*prefix)
		if (*text++ != *prefix++)
			return 0;
	return 1;
}

static int serial_is_known(const CRYPT_INTEGER_BLOB *serial)
{
	// Little-endian, as CRYPT_INTEGER_BLOB keeps it.
	if (serial->cbData != 16)
		return 0;
	for (DWORD index = 0; index < 16; ++index)
		if (serial->pbData[index] != 16 - index)
			return 0;
	return 1;
}

int raven_entry(void)
{
	WCHAR path[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	while (length && path[length - 1] != L'\\' && path[length - 1] != L'/')
		--length;
	lstrcpyW(path + length, L"signed-message.der");
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	static BYTE message[16384];
	DWORD read = 0;
	if (file == INVALID_HANDLE_VALUE || !ReadFile(file, message, sizeof(message), &read, NULL) || !read)
		return 1;
	CloseHandle(file);
	CRYPT_DATA_BLOB blob = {read, message};
	DWORD encoding = 0, content = 0, format = 0;
	HCERTSTORE store = NULL;
	HCRYPTMSG handle = NULL;
	if (!CryptQueryObject(CERT_QUERY_OBJECT_BLOB, &blob, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED, CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding,
			&content, &format, &store, &handle, NULL) || !handle)
		return 2;
	// The signer: its issuer and serial number, its two algorithms, the signature and the signed attributes.
	DWORD size = 0;
	if (!CryptMsgGetParam(handle, CMSG_SIGNER_INFO_PARAM, 0, NULL, &size) || size < sizeof(CMSG_SIGNER_INFO))
		return 3;
	CMSG_SIGNER_INFO *signer = LocalAlloc(LPTR, size);
	if (!signer || !CryptMsgGetParam(handle, CMSG_SIGNER_INFO_PARAM, 0, signer, &size))
		return 4;
	if (!serial_is_known(&signer->SerialNumber))
		return 5;
	if (!signer->Issuer.cbData || signer->Issuer.cbData > 256 || signer->Issuer.pbData[0] != 0x30)
		return 6;
	if (lstrcmpA(signer->HashAlgorithm.pszObjId, szOID_NIST_sha256))
		return 7;
	if (!starts_with(signer->HashEncryptionAlgorithm.pszObjId, "1.2.840.113549.1.1.") || signer->EncryptedHash.cbData != 256)
		return 8;
	const CRYPT_ATTRIBUTE *digest = NULL;
	for (DWORD index = 0; index < signer->AuthAttrs.cAttr && index < 16; ++index)
		if (!lstrcmpA(signer->AuthAttrs.rgAttr[index].pszObjId, szOID_RSA_messageDigest))
			digest = &signer->AuthAttrs.rgAttr[index];
	if (!digest || digest->cValue != 1 || digest->rgValue[0].cbData != 34 || digest->rgValue[0].pbData[0] != 0x04)
		return 9;
	if (signer->UnauthAttrs.cAttr)
		return 10;
	// The signer's issuer and serial number as a CERT_INFO.
	size = 0;
	if (!CryptMsgGetParam(handle, CMSG_SIGNER_CERT_INFO_PARAM, 0, NULL, &size))
		return 11;
	CERT_INFO *signer_cert = LocalAlloc(LPTR, size);
	if (!signer_cert || !CryptMsgGetParam(handle, CMSG_SIGNER_CERT_INFO_PARAM, 0, signer_cert, &size))
		return 12;
	if (!serial_is_known(&signer_cert->SerialNumber) || signer_cert->Issuer.cbData != signer->Issuer.cbData ||
		signer_cert->Issuer.pbData[0] != 0x30)
		return 13;
	// The certificate the message carries, through its CERT_INFO.
	size = 0;
	if (!CryptMsgGetParam(handle, CMSG_CERT_PARAM, 0, NULL, &size))
		return 14;
	BYTE *encoded = LocalAlloc(LPTR, size);
	if (!encoded || !CryptMsgGetParam(handle, CMSG_CERT_PARAM, 0, encoded, &size))
		return 15;
	const CERT_CONTEXT *certificate = CertCreateCertificateContext(X509_ASN_ENCODING, encoded, size);
	if (!certificate)
		return 16;
	const CERT_INFO *info = certificate->pCertInfo;
	if (!serial_is_known(&info->SerialNumber) || info->Issuer.cbData != signer->Issuer.cbData || info->Subject.pbData[0] != 0x30)
		return 17;
	if (lstrcmpA(info->SignatureAlgorithm.pszObjId, szOID_RSA_SHA256RSA) || !info->NotBefore.dwHighDateTime || !info->NotAfter.dwHighDateTime)
		return 18;
	if (lstrcmpA(info->SubjectPublicKeyInfo.Algorithm.pszObjId, szOID_RSA_RSA) || info->SubjectPublicKeyInfo.PublicKey.cbData != 270 ||
		info->SubjectPublicKeyInfo.PublicKey.pbData[0] != 0x30 || info->SubjectPublicKeyInfo.PublicKey.cUnusedBits)
		return 19;
	const CERT_EXTENSION *constraints = NULL;
	for (DWORD index = 0; index < info->cExtension && index < 16; ++index)
		if (!lstrcmpA(info->rgExtension[index].pszObjId, szOID_BASIC_CONSTRAINTS2))
			constraints = &info->rgExtension[index];
	if (!constraints || !constraints->fCritical || constraints->Value.cbData != 2 || constraints->Value.pbData[0] != 0x30)
		return 20;
	CertFreeCertificateContext(certificate);
	CryptMsgClose(handle);
	CertCloseStore(store, 0);
	return 0;
}
