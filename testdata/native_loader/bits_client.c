#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <bits.h>
#include <bitsmsg.h>

static SOCKET listener;
static char large[5000];

static int same_bytes(const char *left, const char *right, int length)
{
	for (int index = 0; index < length; ++index) if (left[index] != right[index]) return 0;
	return 1;
}

static int send_all(SOCKET peer, const char *data, int length)
{
	while (length > 0) {
		const int sent = send(peer, data, length, 0);
		if (sent <= 0) return 0;
		data += sent;
		length -= sent;
	}
	return 1;
}

/* Answers one request on each connection: /one is a short text, /two a larger body, anything else is missing. */
static DWORD WINAPI serve(void *unused)
{
	for (;;) {
		SOCKET peer = accept(listener, NULL, NULL);
		if (peer == INVALID_SOCKET) return 0;
		char request[2048];
		int size = 0, head = 0;
		while (!head && size < (int)sizeof(request)) {
			const int got = recv(peer, request + size, sizeof(request) - size, 0);
			if (got <= 0) break;
			size += got;
			for (int at = 0; at + 3 < size; ++at)
				if (same_bytes(request + at, "\r\n\r\n", 4)) { head = 1; break; }
		}
		if (size > 9 && same_bytes(request, "GET /one ", 9)) {
			static const char answer[] = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello world";
			send_all(peer, answer, sizeof(answer) - 1);
		} else if (size > 9 && same_bytes(request, "GET /two ", 9)) {
			static const char answer[] = "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\nConnection: close\r\n\r\n";
			send_all(peer, answer, sizeof(answer) - 1);
			send_all(peer, large, sizeof(large));
		} else {
			static const char answer[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			send_all(peer, answer, sizeof(answer) - 1);
		}
		closesocket(peer);
	}
}

/* A callback that tells the test when a job is transferred or fails. */
static HANDLE transferred_event, error_event;

static HRESULT WINAPI callback_query(IBackgroundCopyCallback *self, REFIID iid, void **out)
{
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IBackgroundCopyCallback)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI callback_reference(IBackgroundCopyCallback *self) { return 2; }
static HRESULT WINAPI callback_transferred(IBackgroundCopyCallback *self, IBackgroundCopyJob *job) { SetEvent(transferred_event); return S_OK; }
static HRESULT WINAPI callback_error(IBackgroundCopyCallback *self, IBackgroundCopyJob *job, IBackgroundCopyError *error) { SetEvent(error_event); return S_OK; }
static HRESULT WINAPI callback_modified(IBackgroundCopyCallback *self, IBackgroundCopyJob *job, DWORD reserved) { return S_OK; }
static IBackgroundCopyCallbackVtbl callback_table = {callback_query, callback_reference, callback_reference, callback_transferred, callback_error,
	callback_modified};
static IBackgroundCopyCallback callback = {&callback_table};

static void append(WCHAR *text, const WCHAR *tail)
{
	text += lstrlenW(text);
	while ((*text++ = *tail++));
}

static void append_number(WCHAR *text, unsigned value)
{
	WCHAR digits[12];
	int count = 0;
	do digits[count++] = (WCHAR)('0' + value % 10); while (value /= 10);
	text += lstrlenW(text);
	while (count) *text++ = digits[--count];
	*text = 0;
}

static int file_holds(const WCHAR *path, const char *data, DWORD length)
{
	static char read_back[8192];
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE) return 0;
	DWORD got = 0;
	const BOOL read = ReadFile(file, read_back, sizeof(read_back), &got, NULL);
	CloseHandle(file);
	return read && got == length && same_bytes(read_back, data, (int)length);
}

static BG_JOB_STATE state_of(IBackgroundCopyJob *job)
{
	BG_JOB_STATE state = (BG_JOB_STATE)99;
	IBackgroundCopyJob_GetState(job, &state);
	return state;
}

/* Two downloads from a local server through the manager COM serves for the local service: the files arrive and take their names when
 * the job completes, and a missing file puts its job in the error state, which tells the file and the server's status. */
int raven_entry(void)
{
	for (int index = 0; index < (int)sizeof(large); ++index) large[index] = (char)('a' + index % 26);
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in address;
	for (unsigned index = 0; index < sizeof(address); ++index) ((BYTE *)&address)[index] = 0;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int length = sizeof(address);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 8) ||
		getsockname(listener, (struct sockaddr *)&address, &length)) return 2;
	const unsigned port = ntohs(address.sin_port);
	CreateThread(NULL, 0, serve, NULL, 0, NULL);
	transferred_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	error_event = CreateEventW(NULL, FALSE, FALSE, NULL);

	WCHAR folder[MAX_PATH], one[MAX_PATH], two[MAX_PATH], missing[MAX_PATH], url_one[96] = L"http://127.0.0.1:", url_two[96], url_missing[96];
	if (!GetTempPathW(MAX_PATH, folder)) return 3;
	append(folder, L"raven-bits-");
	append_number(folder, GetCurrentProcessId());
	if (!CreateDirectoryW(folder, NULL)) return 4;
	append(folder, L"\\");
	lstrcpyW(one, folder);
	append(one, L"one.txt");
	lstrcpyW(two, folder);
	append(two, L"two.bin");
	lstrcpyW(missing, folder);
	append(missing, L"missing.bin");
	append_number(url_one, port);
	lstrcpyW(url_two, url_one);
	lstrcpyW(url_missing, url_one);
	append(url_one, L"/one");
	append(url_two, L"/two");
	append(url_missing, L"/missing");

	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 5;
	IBackgroundCopyManager *manager = NULL;
	if (CoCreateInstance(&CLSID_BackgroundCopyManager, NULL, CLSCTX_LOCAL_SERVER, &IID_IBackgroundCopyManager, (void **)&manager) != S_OK ||
		!manager) return 6;
	GUID id;
	IBackgroundCopyJob *job = NULL;
	if (IBackgroundCopyManager_CreateJob(manager, L"first", BG_JOB_TYPE_DOWNLOAD, &id, &job) != S_OK || !job) return 7;
	if (state_of(job) != BG_JOB_STATE_SUSPENDED) return 8;
	if (IBackgroundCopyJob_Resume(job) != BG_E_EMPTY) return 9;
	if (IBackgroundCopyJob_AddFile(job, url_one, L"one.txt") != E_INVALIDARG) return 10;
	if (IBackgroundCopyJob_AddFile(job, L"ftp://127.0.0.1/one", one) != BG_E_PROTOCOL_NOT_AVAILABLE) return 11;
	if (IBackgroundCopyJob_AddFile(job, url_one, one) != S_OK || IBackgroundCopyJob_AddFile(job, url_two, two) != S_OK) return 12;
	if (IBackgroundCopyJob_SetNotifyInterface(job, (IUnknown *)&callback) != S_OK) return 13;
	if (IBackgroundCopyJob_Resume(job) != S_OK) return 14;
	if (WaitForSingleObject(transferred_event, 15000) != WAIT_OBJECT_0) return 15;
	if (state_of(job) != BG_JOB_STATE_TRANSFERRED) return 16;
	BG_JOB_PROGRESS progress;
	if (IBackgroundCopyJob_GetProgress(job, &progress) != S_OK || progress.FilesTotal != 2 || progress.FilesTransferred != 2 ||
		progress.BytesTotal != 5011 || progress.BytesTransferred != 5011) return 17;
	if (GetFileAttributesW(one) != INVALID_FILE_ATTRIBUTES) return 18;
	IBackgroundCopyJob *found = NULL;
	if (IBackgroundCopyManager_GetJob(manager, &id, &found) != S_OK || found != job) return 19;
	IBackgroundCopyJob_Release(found);
	if (IBackgroundCopyJob_Complete(job) != S_OK || state_of(job) != BG_JOB_STATE_ACKNOWLEDGED) return 20;
	if (!file_holds(one, "hello world", 11) || !file_holds(two, large, sizeof(large))) return 21;
	if (IBackgroundCopyManager_GetJob(manager, &id, &found) != BG_E_NOT_FOUND || found) return 22;
	if (IBackgroundCopyJob_Complete(job) != BG_E_INVALID_STATE) return 23;
	IBackgroundCopyJob_Release(job);

	if (IBackgroundCopyManager_CreateJob(manager, L"second", BG_JOB_TYPE_DOWNLOAD, &id, &job) != S_OK) return 30;
	IBackgroundCopyError *error = (IBackgroundCopyError *)1;
	if (IBackgroundCopyJob_GetError(job, &error) != BG_E_ERROR_INFORMATION_UNAVAILABLE || error) return 31;
	if (IBackgroundCopyJob_AddFile(job, url_missing, missing) != S_OK || IBackgroundCopyJob_SetNotifyInterface(job, (IUnknown *)&callback) != S_OK ||
		IBackgroundCopyJob_Resume(job) != S_OK) return 32;
	if (WaitForSingleObject(error_event, 15000) != WAIT_OBJECT_0 || state_of(job) != BG_JOB_STATE_ERROR) return 33;
	BG_ERROR_CONTEXT context;
	HRESULT code = S_OK;
	if (IBackgroundCopyJob_GetError(job, &error) != S_OK || IBackgroundCopyError_GetError(error, &context, &code) != S_OK ||
		context != BG_ERROR_CONTEXT_REMOTE_FILE || code != BG_E_HTTP_ERROR_404) return 34;
	IBackgroundCopyFile *failed = NULL;
	LPWSTR text = NULL;
	if (IBackgroundCopyError_GetFile(error, &failed) != S_OK || IBackgroundCopyFile_GetRemoteName(failed, &text) != S_OK || lstrcmpW(text, url_missing))
		return 35;
	CoTaskMemFree(text);
	IBackgroundCopyFile_Release(failed);
	if (IBackgroundCopyError_GetErrorDescription(error, 0, &text) != S_OK || !text || !*text) return 36;
	CoTaskMemFree(text);
	IBackgroundCopyError_Release(error);
	IEnumBackgroundCopyJobs *jobs = NULL;
	ULONG count = 0;
	if (IBackgroundCopyManager_EnumJobs(manager, 0, &jobs) != S_OK || IEnumBackgroundCopyJobs_GetCount(jobs, &count) != S_OK || count != 1) return 37;
	IEnumBackgroundCopyJobs_Release(jobs);
	if (IBackgroundCopyJob_Cancel(job) != S_OK || state_of(job) != BG_JOB_STATE_CANCELLED || IBackgroundCopyJob_Cancel(job) != BG_E_INVALID_STATE)
		return 38;
	if (GetFileAttributesW(missing) != INVALID_FILE_ATTRIBUTES) return 39;
	IBackgroundCopyJob_Release(job);
	if (IBackgroundCopyManager_GetErrorDescription(manager, BG_E_HTTP_ERROR_404, 0, &text) != S_OK || !text) return 40;
	CoTaskMemFree(text);
	IBackgroundCopyManager_Release(manager);

	DeleteFileW(one);
	DeleteFileW(two);
	folder[lstrlenW(folder) - 1] = 0;
	if (!RemoveDirectoryW(folder)) return 41;
	closesocket(listener);
	return 0;
}
