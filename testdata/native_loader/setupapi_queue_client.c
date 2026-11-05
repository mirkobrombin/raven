#include <windows.h>
#include <setupapi.h>

static DWORD seen_flags;
static int copy_errors;

static UINT CALLBACK record(PVOID context, UINT notification, UINT_PTR first, UINT_PTR second)
{
	if (notification == SPFILENOTIFY_COPYERROR) {
		seen_flags = ((const FILEPATHS_W *)first)->Flags;
		++copy_errors;
		return FILEOP_SKIP;
	}
	return SetupDefaultQueueCallbackW(context, notification, first, second);
}

static BOOL commit_missing(const WCHAR *root, const WCHAR *target, DWORD style, PSP_FILE_CALLBACK_W callback, void *context)
{
	HSPFILEQ queue = SetupOpenFileQueue();
	BOOL committed;
	if (queue == INVALID_HANDLE_VALUE || !SetupQueueCopyW(queue, root, NULL, L"raven-missing.txt", NULL, NULL, target, NULL, style))
		return -1;
	committed = SetupCommitFileQueueW(NULL, queue, callback, context);
	SetupCloseFileQueue(queue);
	return committed;
}

/* A queued copy whose source is missing: the default callback skips it, unless its style says it may not be skipped, which fails the
 * commit with the copy's error. The callback sees that style in FILEPATHS.Flags, and SetupCopyError answers the same way. */
int raven_entry(void)
{
	WCHAR root[MAX_PATH], target[MAX_PATH + 16];
	DWORD length = GetTempPathW(MAX_PATH, root);
	lstrcpyW(target, root);
	lstrcpyW(target + length, L"raven-queue-out");
	CreateDirectoryW(target, NULL);
	void *context = SetupInitDefaultQueueCallback(NULL);
	if (!context) return 1;
	if (commit_missing(root, target, 0, SetupDefaultQueueCallbackW, context) != TRUE) return 2;
	SetLastError(0);
	if (commit_missing(root, target, SP_COPY_NOSKIP, SetupDefaultQueueCallbackW, context) != FALSE || GetLastError() != ERROR_FILE_NOT_FOUND) return 3;
	if (commit_missing(root, target, SP_COPY_NOSKIP | SP_COPY_NEWER, record, context) != TRUE || copy_errors != 1 ||
		(seen_flags & (SP_COPY_NOSKIP | SP_COPY_NEWER)) != SP_COPY_NOSKIP) return 4;
	SetupTermDefaultQueueCallback(context);
	WCHAR path[MAX_PATH];
	if (SetupCopyErrorW(NULL, NULL, NULL, root, L"raven-missing.txt", NULL, ERROR_FILE_NOT_FOUND, IDF_NOSKIP, path, MAX_PATH, NULL) != DPROMPT_CANCEL) return 5;
	if (SetupCopyErrorW(NULL, NULL, NULL, root, L"raven-missing.txt", NULL, ERROR_FILE_NOT_FOUND, 0, path, MAX_PATH, NULL) != DPROMPT_SKIPFILE) return 6;
	RemoveDirectoryW(target);
	return 0;
}
