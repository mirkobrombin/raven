#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0a00
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <winspool.h>
#include <mmsystem.h>
#include <ras.h>
#include <objbase.h>
#include <ctxtcall.h>

#ifndef QS_EVENT
#define QS_EVENT 0x2000
#endif

static int same_bytes(const BYTE *left, const BYTE *right, DWORD size)
{
	for (DWORD at = 0; at < size; ++at)
		if (left[at] != right[at])
			return 0;
	return 1;
}

// What a point-of-sale program asks of the system around a sale: the printers, a sound, the dial-up connections a proxy lookup
// enumerates, the queue its dispatcher watches, hashes it duplicates under a shared provider, a tagged event handle, polled sockets and
// the default context of the multithreaded apartment.
int raven_entry(void)
{
	DWORD needed = 1, returned = 1;
	if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &needed, &returned) || needed || returned) return 1;
	HANDLE printer = (HANDLE)1;
	if (OpenPrinterW(L"Receipt", &printer, NULL) || GetLastError() != ERROR_INVALID_PRINTER_NAME || printer) return 2;
	if (!PlaySoundW(L"SystemAsterisk", NULL, SND_ALIAS | SND_ASYNC) || !sndPlaySoundW(NULL, 0)) return 3;

	RASCONNW connections[1];
	connections[0].dwSize = sizeof(connections[0]);
	DWORD size = sizeof(connections), count = 5;
	if (RasEnumConnectionsW(connections, &size, &count) || count) return 4;

	SetLastError(0);
	GetQueueStatus(QS_ALLINPUT | QS_EVENT);
	if (GetLastError()) return 5;

	HCRYPTPROV provider;
	if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return 6;
	if (!CryptContextAddRef(provider, NULL, 0) || !CryptReleaseContext(provider, 0)) return 7;
	HCRYPTHASH hash, copy;
	if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) || !CryptHashData(hash, (const BYTE *)"ab", 2, 0)) return 8;
	if (!CryptDuplicateHash(hash, NULL, 0, &copy) || !CryptHashData(copy, (const BYTE *)"c", 1, 0) ||
		!CryptHashData(hash, (const BYTE *)"c", 1, 0)) return 9;
	BYTE first[32], second[32];
	DWORD length = 32;
	if (!CryptGetHashParam(hash, HP_HASHVAL, first, &length, 0)) return 10;
	length = 32;
	if (!CryptGetHashParam(copy, HP_HASHVAL, second, &length, 0) || !same_bytes(first, second, 32) || first[0] != 0xba) return 11;
	CryptDestroyHash(copy);
	CryptDestroyHash(hash);
	if (!CryptReleaseContext(provider, 0)) return 12;
	if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) return 13;

	HANDLE event = CreateEventW(NULL, TRUE, TRUE, NULL);
	if (WaitForSingleObject((HANDLE)((ULONG_PTR)event | 1), 0) != WAIT_OBJECT_0) return 14;
	CloseHandle(event);

	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data)) return 15;
	SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int address_size = sizeof(address);
	if (socket_handle == INVALID_SOCKET || bind(socket_handle, (struct sockaddr *)&address, sizeof(address)) ||
		getsockname(socket_handle, (struct sockaddr *)&address, &address_size)) return 16;
	WSAPOLLFD entries[2] = {{socket_handle, POLLWRNORM, 0}, {(SOCKET)0x7ffc, POLLRDNORM, 0}};
	if (WSAPoll(entries, 2, 0) != 2 || !(entries[0].revents & POLLWRNORM) || entries[1].revents != POLLNVAL) return 17;
	entries[0].events = POLLRDNORM;
	if (WSAPoll(entries, 1, 10) != 0 || entries[0].revents) return 18;
	if (sendto(socket_handle, "x", 1, 0, (struct sockaddr *)&address, sizeof(address)) != 1) return 19;
	if (WSAPoll(entries, 1, 1000) != 1 || entries[0].revents != POLLRDNORM) return 20;
	closesocket(socket_handle);
	WSACleanup();

	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 21;
	IContextCallback *context = NULL;
	if (FAILED(CoGetDefaultContext(APTTYPE_MTA, &IID_IContextCallback, (void **)&context)) || !context) return 22;
	context->lpVtbl->Release(context);
	if (CoGetDefaultContext(APTTYPE_STA, &IID_IContextCallback, (void **)&context) != E_INVALIDARG) return 23;
	CoUninitialize();
	return 0;
}
