/* The patch application interface of patchapi.h over files, handles and buffers. A function that fails sets the last error to the reason;
   the ones that take file names set it on success too. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <patchapi.h>
#include <stdlib.h>
#include <string.h>

#include "patch.h"

struct progress_target
{
	PPATCH_PROGRESS_CALLBACK callback;
	void *context;
};

static int report(void *context, size_t done, size_t total)
{
	const struct progress_target *target = context;
	return target->callback(target->context, (ULONG)done, (ULONG)total) != FALSE;
}

static DWORD error_of(enum patch_status status)
{
	switch (status) {
	case patch_done: return ERROR_SUCCESS;
	case patch_corrupt: return ERROR_PATCH_CORRUPT;
	case patch_unsupported: return ERROR_PATCH_PACKAGE_UNSUPPORTED;
	case patch_wrong_file: return ERROR_PATCH_WRONG_FILE;
	case patch_not_necessary: return ERROR_PATCH_NOT_NECESSARY;
	case patch_not_available: return ERROR_PATCH_NOT_AVAILABLE;
	case patch_decode_failure: return ERROR_PATCH_DECODE_FAILURE;
	case patch_cancelled: return ERROR_CANCELLED;
	case patch_no_memory: return ERROR_OUTOFMEMORY;
	}
	return ERROR_PATCH_CORRUPT;
}

static DWORD apply(const BYTE *patch, ULONG patch_size, const BYTE *old, ULONG old_size, ULONG options, BOOL header_only,
	PPATCH_PROGRESS_CALLBACK callback, void *context, struct patch_result *result)
{
	struct progress_target target = { callback, context };
	return error_of(patch_apply(patch, patch_size, old, old_size, options, header_only, result, callback ? report : NULL, &target));
}

static void to_file_time(uint32_t seconds, FILETIME *time)
{
	ULONGLONG ticks = ((ULONGLONG)seconds + 11644473600ull) * 10000000ull;
	time->dwLowDateTime = (DWORD)ticks;
	time->dwHighDateTime = (DWORD)(ticks >> 32);
}

/* The whole file behind a handle, from its start; NULL with *size 0 for INVALID_HANDLE_VALUE. */
static DWORD read_whole(HANDLE file, BYTE **data, ULONG *size)
{
	*data = NULL;
	*size = 0;
	if (file == INVALID_HANDLE_VALUE)
		return ERROR_SUCCESS;
	LARGE_INTEGER length;
	if (!GetFileSizeEx(file, &length))
		return GetLastError();
	if (length.QuadPart > 0xffffffffll)
		return ERROR_FILE_TOO_LARGE;
	*data = malloc(length.QuadPart ? (size_t)length.QuadPart : 1);
	if (!*data)
		return ERROR_OUTOFMEMORY;
	LARGE_INTEGER start = { 0 };
	if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN))
		return GetLastError();
	ULONG done = 0;
	while (done < (ULONG)length.QuadPart) {
		DWORD got = 0;
		if (!ReadFile(file, *data + done, (ULONG)length.QuadPart - done, &got, NULL))
			return GetLastError();
		if (!got)
			return ERROR_HANDLE_EOF;
		done += got;
	}
	*size = done;
	return ERROR_SUCCESS;
}

static BOOL by_handles(HANDLE patch_file, HANDLE old_file, HANDLE new_file, ULONG options, BOOL header_only,
	PPATCH_PROGRESS_CALLBACK callback, void *context)
{
	if (patch_file == INVALID_HANDLE_VALUE || !patch_file) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	BOOL writes = !header_only && !(options & APPLY_OPTION_TEST_ONLY);
	BYTE *patch = NULL, *old = NULL;
	ULONG patch_size, old_size;
	struct patch_result result = { 0 };
	DWORD error = read_whole(patch_file, &patch, &patch_size);
	if (!error)
		error = read_whole(old_file ? old_file : INVALID_HANDLE_VALUE, &old, &old_size);
	if (!error)
		error = apply(patch, patch_size, old, old_size, options, header_only, callback, context, &result);
	if (!error && writes) {
		LARGE_INTEGER start = { 0 };
		DWORD written = 0;
		if (!SetFilePointerEx(new_file, start, NULL, FILE_BEGIN) || !SetEndOfFile(new_file) ||
		    !WriteFile(new_file, result.output, (DWORD)result.new_size, &written, NULL))
			error = GetLastError();
		else if (written != result.new_size)
			error = ERROR_WRITE_FAULT;
		else if (result.has_stamp) {
			FILETIME time;
			to_file_time(result.stamp, &time);
			SetFileTime(new_file, &time, NULL, &time);
		}
	}
	free(result.output);
	free(patch);
	free(old);
	SetLastError(error);
	return !error;
}

static BOOL by_names(const WCHAR *patch_name, const WCHAR *old_name, const WCHAR *new_name, ULONG options, BOOL header_only,
	PPATCH_PROGRESS_CALLBACK callback, void *context)
{
	BOOL writes = !header_only && !(options & APPLY_OPTION_TEST_ONLY);
	HANDLE patch_file = CreateFileW(patch_name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (patch_file == INVALID_HANDLE_VALUE)
		return FALSE;
	HANDLE old_file = INVALID_HANDLE_VALUE, new_file = INVALID_HANDLE_VALUE;
	BOOL done = FALSE;
	DWORD error;
	if (old_name && (old_file = CreateFileW(old_name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)) == INVALID_HANDLE_VALUE) {
		error = GetLastError();
		goto close;
	}
	if (writes && (new_file = CreateFileW(new_name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL)) == INVALID_HANDLE_VALUE) {
		error = GetLastError();
		goto close;
	}
	done = by_handles(patch_file, old_file, new_file, options, header_only, callback, context);
	error = GetLastError();
	if (new_file != INVALID_HANDLE_VALUE) {
		CloseHandle(new_file);
		if (!done)
			DeleteFileW(new_name);
	}
close:
	if (old_file != INVALID_HANDLE_VALUE)
		CloseHandle(old_file);
	CloseHandle(patch_file);
	SetLastError(error);
	return done;
}

static WCHAR *widen(const char *text)
{
	if (!text)
		return NULL;
	int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	WCHAR *wide = length > 0 ? malloc(length * sizeof(WCHAR)) : NULL;
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

static BOOL by_narrow_names(const char *patch_name, const char *old_name, const char *new_name, ULONG options, BOOL header_only,
	PPATCH_PROGRESS_CALLBACK callback, void *context)
{
	WCHAR *patch = widen(patch_name), *old = widen(old_name), *new = widen(new_name);
	BOOL done = FALSE;
	if ((patch_name && !patch) || (old_name && !old) || (new_name && !new))
		SetLastError(ERROR_OUTOFMEMORY);
	else
		done = by_names(patch, old, new, options, header_only, callback, context);
	free(patch);
	free(old);
	free(new);
	return done;
}

BOOL WINAPI ApplyPatchToFileExW(LPCWSTR patch_name, LPCWSTR old_name, LPCWSTR new_name, ULONG options, PPATCH_PROGRESS_CALLBACK callback,
	PVOID context)
{
	return by_names(patch_name, old_name, new_name, options, FALSE, callback, context);
}

BOOL WINAPI ApplyPatchToFileW(LPCWSTR patch_name, LPCWSTR old_name, LPCWSTR new_name, ULONG options)
{
	return by_names(patch_name, old_name, new_name, options, FALSE, NULL, NULL);
}

BOOL WINAPI ApplyPatchToFileExA(LPCSTR patch_name, LPCSTR old_name, LPCSTR new_name, ULONG options, PPATCH_PROGRESS_CALLBACK callback,
	PVOID context)
{
	return by_narrow_names(patch_name, old_name, new_name, options, FALSE, callback, context);
}

BOOL WINAPI ApplyPatchToFileA(LPCSTR patch_name, LPCSTR old_name, LPCSTR new_name, ULONG options)
{
	return by_narrow_names(patch_name, old_name, new_name, options, FALSE, NULL, NULL);
}

BOOL WINAPI ApplyPatchToFileByHandlesEx(HANDLE patch_file, HANDLE old_file, HANDLE new_file, ULONG options, PPATCH_PROGRESS_CALLBACK callback,
	PVOID context)
{
	return by_handles(patch_file, old_file, new_file, options, FALSE, callback, context);
}

BOOL WINAPI ApplyPatchToFileByHandles(HANDLE patch_file, HANDLE old_file, HANDLE new_file, ULONG options)
{
	return by_handles(patch_file, old_file, new_file, options, FALSE, NULL, NULL);
}

BOOL WINAPI TestApplyPatchToFileW(LPCWSTR patch_name, LPCWSTR old_name, ULONG options)
{
	return by_names(patch_name, old_name, NULL, options, TRUE, NULL, NULL);
}

BOOL WINAPI TestApplyPatchToFileA(LPCSTR patch_name, LPCSTR old_name, ULONG options)
{
	return by_narrow_names(patch_name, old_name, NULL, options, TRUE, NULL, NULL);
}

BOOL WINAPI TestApplyPatchToFileByHandles(HANDLE patch_file, HANDLE old_file, ULONG options)
{
	return by_handles(patch_file, old_file, INVALID_HANDLE_VALUE, options, TRUE, NULL, NULL);
}

/* The new file goes into the caller's buffer when it is large enough; given none, into one from VirtualAlloc that the caller releases. */
BOOL WINAPI ApplyPatchToFileByBuffers(PBYTE patch, ULONG patch_size, PBYTE old, ULONG old_size, PBYTE *new_buffer, ULONG new_buffer_size,
	ULONG *new_size, FILETIME *new_time, ULONG options, PPATCH_PROGRESS_CALLBACK callback, PVOID context)
{
	BOOL test_only = (options & APPLY_OPTION_TEST_ONLY) != 0;
	if (!new_buffer && !test_only) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	struct patch_result result = { 0 };
	DWORD error = apply(patch, patch_size, old, old_size, options, FALSE, callback, context, &result);
	if (new_size && (!error || result.new_size))
		*new_size = (ULONG)result.new_size;
	if (!error && !test_only) {
		BYTE *target = *new_buffer;
		if (target && new_buffer_size < result.new_size)
			error = ERROR_INSUFFICIENT_BUFFER;
		else if (!target && !(target = VirtualAlloc(NULL, result.new_size ? result.new_size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
			error = GetLastError();
		else {
			memcpy(target, result.output, result.new_size);
			*new_buffer = target;
		}
	}
	if (!error && new_time) {
		new_time->dwLowDateTime = new_time->dwHighDateTime = 0;
		if (result.has_stamp)
			to_file_time(result.stamp, new_time);
	}
	free(result.output);
	if (error)
		SetLastError(error);
	return !error;
}

BOOL WINAPI TestApplyPatchToFileByBuffers(PBYTE patch, ULONG patch_size, PBYTE old, ULONG old_size, ULONG *new_size, ULONG options)
{
	struct patch_result result = { 0 };
	DWORD error = apply(patch, patch_size, old, old_size, options, TRUE, NULL, NULL, &result);
	if (new_size && (!error || result.new_size))
		*new_size = (ULONG)result.new_size;
	if (error)
		SetLastError(error);
	return !error;
}

/* Signatures come from a file normalized the way the patch creator rewrites executables, which is not known here. */
BOOL WINAPI GetFilePatchSignatureW(LPCWSTR name, ULONG options, PVOID data, ULONG ignore_count, PPATCH_IGNORE_RANGE ignore, ULONG retain_count,
	PPATCH_RETAIN_RANGE retain, ULONG size, PVOID signature)
{
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

BOOL WINAPI GetFilePatchSignatureA(LPCSTR name, ULONG options, PVOID data, ULONG ignore_count, PPATCH_IGNORE_RANGE ignore, ULONG retain_count,
	PPATCH_RETAIN_RANGE retain, ULONG size, PVOID signature)
{
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

BOOL WINAPI GetFilePatchSignatureByHandle(HANDLE file, ULONG options, PVOID data, ULONG ignore_count, PPATCH_IGNORE_RANGE ignore,
	ULONG retain_count, PPATCH_RETAIN_RANGE retain, ULONG size, PVOID signature)
{
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

BOOL WINAPI GetFilePatchSignatureByBuffer(PBYTE buffer, ULONG buffer_size, ULONG options, PVOID data, ULONG ignore_count,
	PPATCH_IGNORE_RANGE ignore, ULONG retain_count, PPATCH_RETAIN_RANGE retain, ULONG size, LPSTR signature)
{
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

INT WINAPI NormalizeFileForPatchSignature(PVOID buffer, ULONG buffer_size, ULONG options, PATCH_OPTION_DATA *data, ULONG new_base,
	ULONG new_time, ULONG ignore_count, PPATCH_IGNORE_RANGE ignore, ULONG retain_count, PPATCH_RETAIN_RANGE retain)
{
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
