// A certificate chain handed on is duplicated, and each holder frees its own reference, as the .NET Framework does with the chains
// MusicBee asks for. The exit code names the first check that failed.
#include <windows.h>
#include <wincrypt.h>

int raven_entry(void)
{
	HCERTSTORE roots = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT");
	if (!roots)
		return 1;
	PCCERT_CONTEXT root = CertEnumCertificatesInStore(roots, NULL);
	if (!root)
		return 2;
	CERT_CHAIN_PARA para = {sizeof para};
	PCCERT_CHAIN_CONTEXT chain = NULL;
	if (!CertGetCertificateChain(NULL, root, NULL, NULL, &para, 0, NULL, &chain) || !chain)
		return 3;
	/* The duplicate is the same chain. */
	if (CertDuplicateCertificateChain(chain) != chain)
		return 4;
	CertFreeCertificateChain(chain);
	if (chain->cChain != 1 || !chain->rgpChain[0]->cElement || !chain->rgpChain[0]->rgpElement[0]->pCertContext)
		return 5;
	CertFreeCertificateChain(chain);
	CertFreeCertificateContext(root);
	CertCloseStore(roots, 0);
	return 0;
}
