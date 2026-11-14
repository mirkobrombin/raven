// A certificate store handed on is duplicated, and each holder closes its own copy: the store stays open until the last close, as
// the .NET Framework does with the stores MusicBee opens. The exit code names the first check that failed.
#include <windows.h>
#include <wincrypt.h>

int raven_entry(void)
{
	HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
	if (!store)
		return 1;
	HCERTSTORE copy = CertDuplicateStore(store);
	if (!copy)
		return 2;
	if (!CertCloseStore(store, 0))
		return 3;
	/* The copy still names an open store after the first close. */
	if (CertEnumCertificatesInStore(copy, NULL) || GetLastError() != (DWORD)CRYPT_E_NOT_FOUND)
		return 4;
	if (!CertCloseStore(copy, 0))
		return 5;
	return 0;
}
