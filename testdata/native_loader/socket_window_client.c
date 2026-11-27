#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <windows.h>

/* WSAAsyncSelect, as Qt's socket notifiers use it: a listening socket and a client on the loopback post their network events to a window,
   each as one message with the socket and WSAMAKESELECTREPLY(event, error). The listener reports the connection to accept, the client its
   connection and that it can write, then the data the server sends and the server closing. A read enables the read event again, so data
   sent after it is reported once more. A handle that is no socket is refused. The exit code names the first check that failed. */

#define LISTENER_MESSAGE (WM_USER + 1)
#define CLIENT_MESSAGE (WM_USER + 2)

static HWND window;

/* Waits up to two seconds for a message of this kind about this socket that carries the event; anything else is set aside. */
static int wait_event(UINT message, SOCKET socket_handle, WORD event, WORD *error)
{
	const DWORD until = GetTickCount() + 2000;
	MSG got;
	while (GetTickCount() < until) {
		while (PeekMessageW(&got, window, message, message, PM_REMOVE))
			if (got.wParam == (WPARAM)socket_handle && WSAGETSELECTEVENT(got.lParam) == event) {
				if (error)
					*error = WSAGETSELECTERROR(got.lParam);
				return 1;
			}
		Sleep(5);
	}
	return 0;
}

int raven_entry(void)
{
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data))
		return 1;
	window = CreateWindowExW(0, L"STATIC", L"sockets", 0, 0, 0, 10, 10, HWND_MESSAGE, NULL, NULL, NULL);
	if (!window)
		return 2;
	if (WSAAsyncSelect((SOCKET)0x7ff0, window, LISTENER_MESSAGE, FD_ACCEPT) != SOCKET_ERROR || WSAGetLastError() != WSAENOTSOCK)
		return 3;
	SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP), client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in address = {0};
	int length = sizeof address;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (listener == INVALID_SOCKET || client == INVALID_SOCKET || bind(listener, (struct sockaddr *)&address, sizeof address) ||
		listen(listener, 1) || getsockname(listener, (struct sockaddr *)&address, &length))
		return 4;
	if (WSAAsyncSelect(listener, window, LISTENER_MESSAGE, FD_ACCEPT) ||
		WSAAsyncSelect(client, window, CLIENT_MESSAGE, FD_CONNECT | FD_WRITE | FD_READ | FD_CLOSE))
		return 5;
	/* The selection made the client non-blocking: the connection completes later and is reported. */
	if (connect(client, (struct sockaddr *)&address, sizeof address) != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK)
		return 6;
	WORD error = 1;
	if (!wait_event(LISTENER_MESSAGE, listener, FD_ACCEPT, &error) || error)
		return 7;
	SOCKET server = accept(listener, NULL, NULL);
	if (server == INVALID_SOCKET)
		return 8;
	error = 1;
	if (!wait_event(CLIENT_MESSAGE, client, FD_CONNECT, &error) || error)
		return 9;
	if (!wait_event(CLIENT_MESSAGE, client, FD_WRITE, NULL))
		return 10;
	/* The accepted socket has the listener's selection; it is changed to report nothing. */
	if (WSAAsyncSelect(server, window, 0, 0))
		return 11;
	char text[8] = {0};
	if (send(server, "one", 3, 0) != 3 || !wait_event(CLIENT_MESSAGE, client, FD_READ, NULL) || recv(client, text, sizeof text, 0) != 3 ||
		text[0] != 'o')
		return 12;
	if (send(server, "two", 3, 0) != 3 || !wait_event(CLIENT_MESSAGE, client, FD_READ, NULL) || recv(client, text, sizeof text, 0) != 3 ||
		text[0] != 't')
		return 13;
	closesocket(server);
	error = 1;
	if (!wait_event(CLIENT_MESSAGE, client, FD_CLOSE, &error) || error)
		return 14;
	closesocket(client);
	closesocket(listener);
	DestroyWindow(window);
	WSACleanup();
	return 0;
}
