#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winscard.h>

/* A library that reads a smart card, as VLC's ARIB conditional access plugin does, imports winscard.dll and its protocol control blocks.
   With no card service running, a context is refused with SCARD_E_NO_SERVICE before any reader is looked for, and the control blocks
   name their protocols. */
int raven_entry(void)
{
	SCARDCONTEXT context = 0xdead;
	if (SCardEstablishContext(SCARD_SCOPE_USER, NULL, NULL, &context) != SCARD_E_NO_SERVICE || context)
		return 1;
	char readers[64];
	DWORD length = sizeof readers;
	if (SCardListReadersA(context, NULL, readers, &length) != SCARD_E_NO_SERVICE)
		return 2;
	SCARDHANDLE card = 0;
	DWORD protocol = 0;
	if (SCardConnectA(context, "reader", SCARD_SHARE_SHARED, SCARD_PROTOCOL_T1, &card, &protocol) == SCARD_S_SUCCESS)
		return 3;
	if (SCardReleaseContext(context) == SCARD_S_SUCCESS)
		return 4;
	if (SCARD_PCI_T0->dwProtocol != SCARD_PROTOCOL_T0 || SCARD_PCI_T1->dwProtocol != SCARD_PROTOCOL_T1 ||
		SCARD_PCI_T1->cbPciLength != sizeof(SCARD_IO_REQUEST))
		return 5;
	HANDLE started = SCardAccessStartedEvent();
	if (!started || WaitForSingleObject(started, 0) != WAIT_TIMEOUT)
		return 6;
	SCardReleaseStartedEvent();
	return 0;
}
