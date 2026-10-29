#include <winsock2.h>
#include <windows.h>
#include <winternl.h>

// The AFD readiness poll as mio and wepoll use it: \Device\Afd opened with NtCreateFile and joined to a completion port; a poll of a
// listening socket completes with AFD_POLL_ACCEPT once a client connects, its packet carrying the APC context; a second poll that
// nothing satisfies ends as cancelled after NtCancelIoFileEx. Returns 0, or the number of the check that failed.
NTSTATUS NTAPI NtCancelIoFileEx(HANDLE, PIO_STATUS_BLOCK, PIO_STATUS_BLOCK);

typedef struct {
	LARGE_INTEGER timeout;
	ULONG count, exclusive;
	struct {
		HANDLE socket;
		ULONG events;
		NTSTATUS status;
	} handles[1];
} PollInfo;

#define IOCTL_AFD_POLL 0x00012024
#define AFD_POLL_RECEIVE 0x1
#define AFD_POLL_ACCEPT 0x80

int raven_entry(void)
{
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data))
		return 1;
	HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	WCHAR text[] = L"\\Device\\Afd\\Test";
	UNICODE_STRING name = {sizeof text - 2, sizeof text, text};
	static OBJECT_ATTRIBUTES attributes;
	attributes.Length = sizeof attributes;
	attributes.ObjectName = &name;
	IO_STATUS_BLOCK opened;
	HANDLE afd;
	if (NtCreateFile(&afd, SYNCHRONIZE, &attributes, &opened, NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0))
		return 2;
	if (CreateIoCompletionPort(afd, port, 7, 0) != port || !SetFileCompletionNotificationModes(afd, FILE_SKIP_SET_EVENT_ON_HANDLE))
		return 3;
	SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in address = {AF_INET};
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int size = sizeof address;
	if (bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, 4) || getsockname(listener, (struct sockaddr *)&address, &size))
		return 4;
	SOCKET base = INVALID_SOCKET;
	DWORD returned = 0;
	if (WSAIoctl(listener, 0x48000022, NULL, 0, &base, sizeof base, &returned, NULL, NULL) || base != listener)
		return 5;
	static PollInfo poll;
	static IO_STATUS_BLOCK status;
	poll.timeout.QuadPart = 0x7fffffffffffffffLL;
	poll.count = 1;
	poll.handles[0].socket = (HANDLE)listener;
	poll.handles[0].events = AFD_POLL_ACCEPT;
	NTSTATUS result = NtDeviceIoControlFile(afd, NULL, NULL, &status, &status, IOCTL_AFD_POLL, &poll, sizeof poll, &poll, sizeof poll);
	if (result != STATUS_PENDING && result != 0)
		return 6;
	SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (connect(client, (struct sockaddr *)&address, sizeof address))
		return 7;
	DWORD bytes;
	ULONG_PTR key;
	OVERLAPPED *context;
	if (!GetQueuedCompletionStatus(port, &bytes, &key, &context, 10000) || key != 7 || context != (OVERLAPPED *)&status)
		return 8;
	if (status.Status || poll.count != 1 || !(poll.handles[0].events & AFD_POLL_ACCEPT))
		return 9;
	// A poll for data on the listener, which never comes, is cancelled.
	poll.count = 1;
	poll.handles[0].socket = (HANDLE)listener;
	poll.handles[0].events = AFD_POLL_RECEIVE;
	result = NtDeviceIoControlFile(afd, NULL, NULL, &status, &status, IOCTL_AFD_POLL, &poll, sizeof poll, &poll, sizeof poll);
	if (result != STATUS_PENDING)
		return 10;
	IO_STATUS_BLOCK cancelled;
	if (NtCancelIoFileEx(afd, &status, &cancelled))
		return 11;
	if (GetQueuedCompletionStatus(port, &bytes, &key, &context, 10000) || GetLastError() != ERROR_OPERATION_ABORTED || context != (OVERLAPPED *)&status)
		return 12;
	if (status.Status != (NTSTATUS)0xC0000120L)
		return 13;
	closesocket(client);
	closesocket(listener);
	CloseHandle(afd);
	return 0;
}
