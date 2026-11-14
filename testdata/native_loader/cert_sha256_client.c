// A certificate context names the SHA-256 of its encoded certificate as a property, as the paint.net setup reads the thumbprint of the
// certificate that signed its files. The exit code names the first check that failed.
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>

int raven_entry(void)
{
	HCERTSTORE roots = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT");
	if (!roots)
		return 1;
	PCCERT_CONTEXT root = CertEnumCertificatesInStore(roots, NULL);
	if (!root)
		return 2;
	/* The size first, then the hash itself. */
	DWORD size = 0;
	if (!CertGetCertificateContextProperty(root, CERT_SHA256_HASH_PROP_ID, NULL, &size) || size != 32)
		return 3;
	BYTE named[32], computed[32];
	if (!CertGetCertificateContextProperty(root, CERT_SHA256_HASH_PROP_ID, named, &size) || size != 32)
		return 4;
	BCRYPT_ALG_HANDLE sha256 = NULL;
	if (BCryptOpenAlgorithmProvider(&sha256, BCRYPT_SHA256_ALGORITHM, NULL, 0) ||
		BCryptHash(sha256, NULL, 0, root->pbCertEncoded, root->cbCertEncoded, computed, sizeof computed))
		return 5;
	for (int index = 0; index < 32; index++)
		if (named[index] != computed[index])
			return 6;
	BCryptCloseAlgorithmProvider(sha256, 0);
	CertFreeCertificateContext(root);
	CertCloseStore(roots, 0);
	return 0;
}
