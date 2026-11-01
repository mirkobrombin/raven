/* Named and anonymous pipes: overlapped transfers that complete through a port, cancellation, message mode and a blocking read on a thread. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const WCHAR name[] = L"\\\\.\\pipe\\raven-pipe-contract";
static const WCHAR message_name[] = L"\\\\.\\pipe\\raven-pipe-messages";

static HANDLE blocked_server;
static volatile LONG blocked_result;

static DWORD WINAPI blocked_reader(void *parameter)
{
	char buffer[8];
	DWORD count = 0;
	if (!ReadFile(blocked_server, buffer, sizeof(buffer), &count, NULL) || count != 4 || buffer[0] != 'w' || buffer[3] != 'e')
		blocked_result = -1;
	else
		blocked_result = 1;
	return 0;
}

static int same(const char *left, const char *right, int length)
{
	int index;
	for (index = 0; index < length; ++index)
		if (left[index] != right[index])
			return 0;
	return 1;
}

int raven_entry(void)
{
	HANDLE server, client, port, second, read_end, write_end, thread, event;
	OVERLAPPED connect_io = {0}, read_io = {0}, write_io = {0}, other_io = {0};
	char buffer[64];
	DWORD count, flags;
	ULONG_PTR key;
	OVERLAPPED *finished;
	DWORD error;

	server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
	if (server == INVALID_HANDLE_VALUE)
		return 1;
	/* A second first instance of a name in use is refused. */
	second = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE, 1, 4096, 4096, 0, NULL);
	if (second != INVALID_HANDLE_VALUE || GetLastError() != ERROR_ACCESS_DENIED)
		return 2;
	if (!WaitNamedPipeW(name, 100))
		return 3;
	client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (client == INVALID_HANDLE_VALUE)
		return 4;
	if (GetFileType(client) != FILE_TYPE_PIPE)
		return 5;
	/* The client came before ConnectNamedPipe: the call reports a connection that exists. */
	event = CreateEventW(NULL, TRUE, FALSE, NULL);
	connect_io.hEvent = event;
	if (ConnectNamedPipe(server, &connect_io) || GetLastError() != ERROR_PIPE_CONNECTED)
		return 6;
	port = CreateIoCompletionPort(server, NULL, 1, 0);
	if (!port || !CreateIoCompletionPort(client, port, 2, 0))
		return 7;

	/* A read with nothing to read is pending until the client writes. */
	read_io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (ReadFile(server, buffer, sizeof(buffer), NULL, &read_io) || GetLastError() != ERROR_IO_PENDING)
		return 8;
	if (HasOverlappedIoCompleted(&read_io))
		return 9;
	if (GetOverlappedResult(server, &read_io, &count, FALSE) || GetLastError() != ERROR_IO_INCOMPLETE)
		return 10;
	write_io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!WriteFile(client, "hello", 5, NULL, &write_io) && GetLastError() != ERROR_IO_PENDING)
		return 11;
	/* The write and the read each report to the port. */
	{
		int seen_read = 0, seen_write = 0, round;
		for (round = 0; round < 2; ++round) {
			if (!GetQueuedCompletionStatus(port, &count, &key, &finished, 2000))
				return 12;
			if (key == 1 && finished == &read_io && count == 5)
				seen_read = 1;
			else if (key == 2 && finished == &write_io && count == 5)
				seen_write = 1;
			else
				return 13;
		}
		if (!seen_read || !seen_write)
			return 14;
	}
	if (!same(buffer, "hello", 5))
		return 15;
	if (!GetOverlappedResult(server, &read_io, &count, TRUE) || count != 5)
		return 16;
	if (WaitForSingleObject(read_io.hEvent, 0) != WAIT_OBJECT_0)
		return 17;

	/* The other direction, with the read started on the client. */
	if (ReadFile(client, buffer, sizeof(buffer), NULL, &other_io) || GetLastError() != ERROR_IO_PENDING)
		return 18;
	if (!WriteFile(server, "world!", 6, &count, &write_io) && GetLastError() != ERROR_IO_PENDING)
		return 19;
	{
		int round;
		for (round = 0; round < 2; ++round) {
			if (!GetQueuedCompletionStatus(port, &count, &key, &finished, 2000))
				return 20;
		}
	}
	if (!same(buffer, "world!", 6))
		return 21;

	/* A cancelled read completes as aborted. */
	if (ReadFile(server, buffer, sizeof(buffer), NULL, &read_io) || GetLastError() != ERROR_IO_PENDING)
		return 22;
	if (!CancelIoEx(server, &read_io))
		return 23;
	if (!GetQueuedCompletionStatus(port, &count, &key, &finished, 2000) && GetLastError() != ERROR_OPERATION_ABORTED)
		return 24;
	if (finished != &read_io || GetOverlappedResult(server, &read_io, &count, FALSE) || GetLastError() != ERROR_OPERATION_ABORTED)
		return 25;
	if (CancelIoEx(server, &read_io) || GetLastError() != ERROR_NOT_FOUND)
		return 26;

	/* Closing the client breaks the pipe for the server's pending read. */
	if (ReadFile(server, buffer, sizeof(buffer), NULL, &read_io) || GetLastError() != ERROR_IO_PENDING)
		return 27;
	CloseHandle(client);
	if (GetQueuedCompletionStatus(port, &count, &key, &finished, 2000) || GetLastError() != ERROR_BROKEN_PIPE || finished != &read_io)
		return 28;
	if (ReadFile(server, buffer, sizeof(buffer), NULL, &read_io) || GetLastError() != ERROR_BROKEN_PIPE)
		return 29;
	if (!DisconnectNamedPipe(server))
		return 30;
	if (ReadFile(server, buffer, sizeof(buffer), NULL, &read_io) || GetLastError() != ERROR_PIPE_LISTENING)
		return 31;
	CloseHandle(server);
	CloseHandle(port);

	/* A handle without overlapped I/O blocks only the thread that reads, and ConnectNamedPipe waits for a client. */
	blocked_server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
	if (blocked_server == INVALID_HANDLE_VALUE)
		return 32;
	client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (client == INVALID_HANDLE_VALUE)
		return 33;
	if (ConnectNamedPipe(blocked_server, NULL) || GetLastError() != ERROR_PIPE_CONNECTED)
		return 34;
	thread = CreateThread(NULL, 0, blocked_reader, NULL, 0, NULL);
	if (!thread)
		return 35;
	Sleep(50);
	if (blocked_result != 0)
		return 36;
	if (!WriteFile(client, "wire", 4, &count, NULL) || count != 4)
		return 37;
	if (WaitForSingleObject(thread, 3000) != WAIT_OBJECT_0 || blocked_result != 1)
		return 38;
	CloseHandle(thread);
	CloseHandle(client);
	CloseHandle(blocked_server);

	/* Message mode keeps the boundaries and reports a message that did not fit. */
	server = CreateNamedPipeW(message_name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
	if (server == INVALID_HANDLE_VALUE)
		return 39;
	client = CreateFileW(message_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (client == INVALID_HANDLE_VALUE)
		return 40;
	flags = PIPE_READMODE_MESSAGE;
	if (!SetNamedPipeHandleState(client, &flags, NULL, NULL))
		return 41;
	if (!WriteFile(client, "abc", 3, &count, NULL) || !WriteFile(client, "defghij", 7, &count, NULL))
		return 42;
	if (!ReadFile(server, buffer, 16, &count, NULL) || count != 3 || !same(buffer, "abc", 3))
		return 43;
	if (ReadFile(server, buffer, 4, &count, NULL) || GetLastError() != ERROR_MORE_DATA || count != 4 || !same(buffer, "defg", 4))
		return 44;
	if (!ReadFile(server, buffer, 16, &count, NULL) || count != 3 || !same(buffer, "hij", 3))
		return 45;
	CloseHandle(client);
	CloseHandle(server);

	/* An anonymous pipe carries bytes from the write end to the read end. */
	if (!CreatePipe(&read_end, &write_end, NULL, 0))
		return 46;
	if (GetFileType(read_end) != FILE_TYPE_PIPE)
		return 47;
	if (!WriteFile(write_end, "anon", 4, &count, NULL) || count != 4)
		return 48;
	if (!PeekNamedPipe(read_end, NULL, 0, NULL, &count, NULL) || count != 4)
		return 49;
	if (!ReadFile(read_end, buffer, 16, &count, NULL) || count != 4 || !same(buffer, "anon", 4))
		return 50;
	CloseHandle(write_end);
	if (ReadFile(read_end, buffer, 16, &count, NULL) || GetLastError() != ERROR_BROKEN_PIPE)
		return 51;
	CloseHandle(read_end);
	/* A completion port bound after the wait started still hears of the connection. */
	{
		OVERLAPPED wait = {0}, *done = NULL;
		ULONG_PTR key = 0;
		HANDLE late_port;
		server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 64, 64, 0, NULL);
		if (server == INVALID_HANDLE_VALUE)
			return 54;
		if (ConnectNamedPipe(server, &wait) || GetLastError() != ERROR_IO_PENDING)
			return 55;
		late_port = CreateIoCompletionPort(server, NULL, 7, 0);
		if (!late_port)
			return 56;
		client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (client == INVALID_HANDLE_VALUE)
			return 57;
		if (!GetQueuedCompletionStatus(late_port, &count, &key, &done, 5000) || key != 7 || done != &wait)
			return 58;
		CloseHandle(client);
		CloseHandle(server);
		CloseHandle(late_port);
	}
	/* A client of a message pipe reads bytes until it asks otherwise: a short read is no error, and the rest follows. */
	server = CreateNamedPipeW(message_name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
	if (server == INVALID_HANDLE_VALUE)
		return 59;
	client = CreateFileW(message_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (client == INVALID_HANDLE_VALUE)
		return 60;
	if (!WriteFile(server, "abcdef", 6, &count, NULL) || count != 6)
		return 61;
	SetLastError(0);
	if (!ReadFile(client, buffer, 4, &count, NULL) || count != 4 || !same(buffer, "abcd", 4))
		return 62;
	if (!ReadFile(client, buffer, 4, &count, NULL) || count != 2 || !same(buffer, "ef", 2))
		return 63;
	/* The server asked for messages when it made the pipe, so it hears that one did not fit. */
	if (!WriteFile(client, "ghijkl", 6, &count, NULL) || count != 6)
		return 64;
	if (ReadFile(server, buffer, 4, &count, NULL) || GetLastError() != ERROR_MORE_DATA || count != 4)
		return 65;
	if (!ReadFile(server, buffer, 4, &count, NULL) || count != 2 || !same(buffer, "kl", 2))
		return 66;
	CloseHandle(client);
	CloseHandle(server);
	/* A pipe nobody serves cannot be opened. */
	error = 0;
	if (CreateFileW(name, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) != INVALID_HANDLE_VALUE)
		return 52;
	error = GetLastError();
	if (error != ERROR_FILE_NOT_FOUND || WaitNamedPipeW(name, 10))
		return 53;
	return 0;
}
