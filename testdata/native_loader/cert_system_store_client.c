// CertOpenSystemStore opens a system store by its name in either case, as the EA app installer opens the root store to check its
// downloads. The ANSI and wide forms open the same store, and the personal store opens empty. The exit code names the first check
// that failed.
#include <windows.h>
#include <wincrypt.h>

static int count(HCERTSTORE store)
{
	int certificates = 0;
	for (PCCERT_CONTEXT context = NULL; (context = CertEnumCertificatesInStore(store, context));)
		++certificates;
	return certificates;
}

int raven_entry(void)
{
	HCERTSTORE narrow = CertOpenSystemStoreA(0, "ROOT");
	if (!narrow)
		return 1;
	HCERTSTORE wide = CertOpenSystemStoreW(0, L"Root");
	if (!wide)
		return 2;
	if (count(narrow) != count(wide))
		return 3;
	HCERTSTORE personal = CertOpenSystemStoreA(0, "MY");
	if (!personal)
		return 4;
	if (count(personal))
		return 5;
	if (!CertCloseStore(narrow, 0) || !CertCloseStore(wide, 0) || !CertCloseStore(personal, 0))
		return 6;
	return 0;
}
