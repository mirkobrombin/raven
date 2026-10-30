/* The older entry points on top of FDI: Extract, which lists or unpacks a cabinet for a caller-owned session, and the version queries. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <fdi.h>
#include <shlwapi.h>

/* The session's layout is the contract with the callers that allocate it, advpack among them. */
struct listed_file
{
	char *name;
	struct listed_file *next;
	BOOL pending;
};

struct session
{
	INT file_size;
	ERF error;
	struct listed_file *files;
	INT file_count;
	INT operation;
	char destination[MAX_PATH];
	char current[MAX_PATH];
	char reserved[MAX_PATH];
	struct listed_file *filter;
};

enum { operation_list = 1, operation_extract = 2 };

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(module);
	return TRUE;
}

LPCSTR WINAPI GetDllVersion(void)
{
	return "5.1.2600.5512";
}

HRESULT WINAPI DllGetVersion(DLLVERSIONINFO *info)
{
	if (info->cbSize != sizeof(*info))
		return E_INVALIDARG;
	info->dwMajorVersion = 5;
	info->dwMinorVersion = 1;
	info->dwBuildNumber = 1106;
	info->dwPlatformID = 1;
	return S_OK;
}

static void *CDECL heap_get(ULONG size)
{
	return HeapAlloc(GetProcessHeap(), 0, size);
}

static void CDECL heap_put(void *memory)
{
	HeapFree(GetProcessHeap(), 0, memory);
}

static INT_PTR CDECL file_open(char *name, int mode, int permissions)
{
	(void)mode;
	(void)permissions;
	return (INT_PTR)CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
}

static UINT CDECL file_read(INT_PTR file, void *data, UINT size)
{
	DWORD done;
	return ReadFile((HANDLE)file, data, size, &done, NULL) ? done : 0;
}

static UINT CDECL file_write(INT_PTR file, void *data, UINT size)
{
	DWORD done;
	return WriteFile((HANDLE)file, data, size, &done, NULL) ? done : 0;
}

static int CDECL file_close(INT_PTR file)
{
	return CloseHandle((HANDLE)file) ? 0 : -1;
}

static LONG CDECL file_seek(INT_PTR file, LONG distance, int origin)
{
	return (LONG)SetFilePointer((HANDLE)file, distance, NULL, (DWORD)origin);
}

static struct listed_file *find_listed(struct listed_file *list, const char *name)
{
	for (; list; list = list->next) {
		if (!lstrcmpiA(list->name, name))
			return list;
	}
	return NULL;
}

/* Makes every directory down to the file's own; the destination itself exists, Extract checked. */
static void make_parents(char *path, size_t root_length)
{
	for (char *cursor = path + root_length + 1; *cursor; cursor++) {
		if (*cursor != '\\')
			continue;
		*cursor = 0;
		CreateDirectoryA(path, NULL);
		*cursor = '\\';
	}
}

static INT_PTR open_listed_target(struct session *session, const char *name, LONG size)
{
	size_t root_length = (size_t)lstrlenA(session->destination);
	char *path = HeapAlloc(GetProcessHeap(), 0, root_length + 2 + (size_t)lstrlenA(name));
	if (!path)
		return -1;
	lstrcpyA(path, session->destination);
	lstrcatA(path, "\\");
	lstrcatA(path, name);
	session->file_size += size;

	if (session->operation & operation_list) {
		struct listed_file *entry = HeapAlloc(GetProcessHeap(), 0, sizeof(*entry));
		char *copy = HeapAlloc(GetProcessHeap(), 0, (size_t)lstrlenA(name) + 1);
		if (entry && copy) {
			lstrcpyA(copy, name);
			entry->name = copy;
			entry->pending = TRUE;
			entry->next = session->files;
			session->files = entry;
			lstrcpynA(session->current, path, MAX_PATH);
			session->file_count++;
		}
	}

	INT_PTR target = 0;
	if ((session->operation & operation_extract) || find_listed(session->filter, name)) {
		struct listed_file *entry = find_listed(session->files, name);
		if (!entry || entry->pending) {
			make_parents(path, root_length);
			HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			target = (INT_PTR)file;
			if (file != INVALID_HANDLE_VALUE && entry)
				entry->pending = FALSE;
		}
	}
	HeapFree(GetProcessHeap(), 0, path);
	return target;
}

static INT_PTR CDECL on_notification(FDINOTIFICATIONTYPE type, PFDINOTIFICATION note)
{
	if (type == fdintCOPY_FILE)
		return open_listed_target(note->pv, note->psz1, note->cb);
	if (type != fdintCLOSE_FILE_INFO)
		return 0;
	FILETIME local, utc;
	HANDLE file = (HANDLE)note->hf;
	if (!DosDateTimeToFileTime(note->date, note->time, &local) || !LocalFileTimeToFileTime(&local, &utc) || !SetFileTime(file, &utc, NULL, &utc))
		return FALSE;
	CloseHandle(file);
	return TRUE;
}

HRESULT WINAPI Extract(struct session *session, LPCSTR cabinet)
{
	HFDI context = FDICreate(heap_get, heap_put, file_open, file_read, file_write, file_close, file_seek, cpuUNKNOWN, &session->error);
	if (!context)
		return E_FAIL;
	HRESULT result = S_OK;
	char *copy = NULL;
	if (GetFileAttributesA(session->destination) != INVALID_FILE_ATTRIBUTES) {
		copy = HeapAlloc(GetProcessHeap(), 0, (size_t)lstrlenA(cabinet) + 2);
		if (!copy) {
			result = E_OUTOFMEMORY;
		} else {
			/* FDI wants the folder and the name apart. */
			const char *slash = NULL;
			for (const char *cursor = cabinet; *cursor; cursor++) {
				if (*cursor == '\\')
					slash = cursor;
			}
			const char *name = slash ? slash + 1 : cabinet;
			char *path = NULL, *base = copy;
			if (slash) {
				size_t folder = (size_t)(name - cabinet);
				CopyMemory(copy, cabinet, folder);
				copy[folder] = 0;
				path = copy;
				base = copy + folder + 1;
			}
			lstrcpyA(base, name);
			session->file_size = 0;
			if (!FDICopy(context, base, path, 0, on_notification, NULL, session))
				result = HRESULT_FROM_WIN32(GetLastError());
		}
	}
	HeapFree(GetProcessHeap(), 0, copy);
	FDIDestroy(context);
	return result;
}

/* Nothing documents what this removes, and guessing wrong would delete a caller's files. */
VOID WINAPI DeleteExtractedFiles(struct session *session)
{
	(void)session;
}
