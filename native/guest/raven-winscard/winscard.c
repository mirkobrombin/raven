/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven smart card component: winscard.dll, the interface programs reach card readers through.
 *
 * There are no readers: the smart card resource manager is a Windows service with no counterpart in this runtime, and Windows answers a
 * program on a machine where it is not running with SCARD_E_NO_SERVICE. A program asks for a context first, so it learns that before it
 * looks for a reader; the calls that take a context or a card handle it could not have been given answer that the handle is not valid.
 * The protocol control blocks are the data Windows exports for the three protocols. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef ULONG_PTR SCARDCONTEXT;
typedef ULONG_PTR SCARDHANDLE;

typedef struct {
	DWORD protocol;
	DWORD length;
} SCARD_IO_REQUEST;

/* winerror.h names the answers; SCARD_E_NO_SERVICE among them. */

#define EXPORT __declspec(dllexport)

EXPORT const SCARD_IO_REQUEST g_rgSCardT0Pci = {1, sizeof(SCARD_IO_REQUEST)};
EXPORT const SCARD_IO_REQUEST g_rgSCardT1Pci = {2, sizeof(SCARD_IO_REQUEST)};
EXPORT const SCARD_IO_REQUEST g_rgSCardRawPci = {0x10000, sizeof(SCARD_IO_REQUEST)};

/* Signalled when the service starts, which it never does here. */
static HANDLE started;

EXPORT LONG WINAPI SCardEstablishContext(DWORD scope, LPCVOID reserved1, LPCVOID reserved2, SCARDCONTEXT *context)
{
	if (!context)
		return SCARD_E_INVALID_PARAMETER;
	*context = 0;
	return SCARD_E_NO_SERVICE;
}

EXPORT LONG WINAPI SCardReleaseContext(SCARDCONTEXT context) { return SCARD_E_INVALID_HANDLE; }
EXPORT LONG WINAPI SCardIsValidContext(SCARDCONTEXT context) { return SCARD_E_INVALID_HANDLE; }

EXPORT LONG WINAPI SCardListReadersA(SCARDCONTEXT context, LPCSTR groups, LPSTR readers, LPDWORD length) { return SCARD_E_NO_SERVICE; }
EXPORT LONG WINAPI SCardListReadersW(SCARDCONTEXT context, LPCWSTR groups, LPWSTR readers, LPDWORD length) { return SCARD_E_NO_SERVICE; }
EXPORT LONG WINAPI SCardListReaderGroupsA(SCARDCONTEXT context, LPSTR groups, LPDWORD length) { return SCARD_E_NO_SERVICE; }
EXPORT LONG WINAPI SCardListReaderGroupsW(SCARDCONTEXT context, LPWSTR groups, LPDWORD length) { return SCARD_E_NO_SERVICE; }

EXPORT LONG WINAPI SCardConnectA(SCARDCONTEXT context, LPCSTR reader, DWORD share, DWORD protocols, SCARDHANDLE *card, LPDWORD active)
{
	return SCARD_E_INVALID_HANDLE;
}

EXPORT LONG WINAPI SCardConnectW(SCARDCONTEXT context, LPCWSTR reader, DWORD share, DWORD protocols, SCARDHANDLE *card, LPDWORD active)
{
	return SCARD_E_INVALID_HANDLE;
}

EXPORT LONG WINAPI SCardReconnect(SCARDHANDLE card, DWORD share, DWORD protocols, DWORD initialization, LPDWORD active)
{
	return SCARD_E_INVALID_HANDLE;
}

EXPORT LONG WINAPI SCardDisconnect(SCARDHANDLE card, DWORD disposition) { return SCARD_E_INVALID_HANDLE; }
EXPORT LONG WINAPI SCardBeginTransaction(SCARDHANDLE card) { return SCARD_E_INVALID_HANDLE; }
EXPORT LONG WINAPI SCardEndTransaction(SCARDHANDLE card, DWORD disposition) { return SCARD_E_INVALID_HANDLE; }

EXPORT LONG WINAPI SCardTransmit(SCARDHANDLE card, const SCARD_IO_REQUEST *send_header, const BYTE *send, DWORD send_length,
	SCARD_IO_REQUEST *receive_header, LPBYTE receive, LPDWORD receive_length)
{
	return SCARD_E_INVALID_HANDLE;
}

EXPORT LONG WINAPI SCardControl(SCARDHANDLE card, DWORD code, LPCVOID input, DWORD input_length, LPVOID output, DWORD output_length,
	LPDWORD returned)
{
	return SCARD_E_INVALID_HANDLE;
}

EXPORT LONG WINAPI SCardStatusA(SCARDHANDLE card, LPSTR names, LPDWORD names_length, LPDWORD state, LPDWORD protocol, LPBYTE attributes,
	LPDWORD attributes_length)
{
	return SCARD_E_INVALID_HANDLE;
}

EXPORT LONG WINAPI SCardStatusW(SCARDHANDLE card, LPWSTR names, LPDWORD names_length, LPDWORD state, LPDWORD protocol, LPBYTE attributes,
	LPDWORD attributes_length)
{
	return SCARD_E_INVALID_HANDLE;
}

EXPORT LONG WINAPI SCardGetAttrib(SCARDHANDLE card, DWORD id, LPBYTE attribute, LPDWORD length) { return SCARD_E_INVALID_HANDLE; }

EXPORT LONG WINAPI SCardGetStatusChangeA(SCARDCONTEXT context, DWORD timeout, LPVOID readers, DWORD count) { return SCARD_E_INVALID_HANDLE; }
EXPORT LONG WINAPI SCardGetStatusChangeW(SCARDCONTEXT context, DWORD timeout, LPVOID readers, DWORD count) { return SCARD_E_INVALID_HANDLE; }
EXPORT LONG WINAPI SCardCancel(SCARDCONTEXT context) { return SCARD_E_INVALID_HANDLE; }
EXPORT LONG WINAPI SCardFreeMemory(SCARDCONTEXT context, LPCVOID memory) { return SCARD_E_INVALID_HANDLE; }

EXPORT HANDLE WINAPI SCardAccessStartedEvent(void)
{
	if (!started) {
		HANDLE created = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (created && InterlockedCompareExchangePointer(&started, created, NULL))
			CloseHandle(created);
	}
	return started;
}

EXPORT void WINAPI SCardReleaseStartedEvent(void) {}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
