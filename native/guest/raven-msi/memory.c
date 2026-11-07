/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

void *msi_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}

void *msi_grow(void *memory, SIZE_T size)
{
	if (!memory)
		return msi_alloc(size);
	return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, memory, size ? size : 1);
}

void msi_free(void *memory)
{
	if (memory)
		HeapFree(GetProcessHeap(), 0, memory);
}

WCHAR *msi_copy_n(const WCHAR *text, SIZE_T length)
{
	WCHAR *copy = msi_alloc((length + 1) * sizeof(WCHAR));
	if (copy && length)
		memcpy(copy, text, length * sizeof(WCHAR));
	return copy;
}

WCHAR *msi_copy(const WCHAR *text)
{
	return text ? msi_copy_n(text, lstrlenW(text)) : NULL;
}

WCHAR *msi_from_ansi(const char *text)
{
	if (!text)
		return NULL;
	INT length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	WCHAR *wide = msi_alloc(length * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

char *msi_to_ansi(const WCHAR *text)
{
	if (!text)
		return NULL;
	INT length = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
	char *narrow = msi_alloc(length);
	if (narrow)
		WideCharToMultiByte(CP_ACP, 0, text, -1, narrow, length, NULL, NULL);
	return narrow;
}

BOOL same_text(const WCHAR *a, const WCHAR *b)
{
	return !lstrcmpW(a ? a : L"", b ? b : L"");
}

UINT out_text_w(const WCHAR *text, SIZE_T length, WCHAR *out, DWORD *size)
{
	UINT status = ERROR_SUCCESS;
	if (out && size) {
		if (*size > length) {
			memcpy(out, text, length * sizeof(WCHAR));
			out[length] = 0;
		} else {
			if (*size) {
				memcpy(out, text, (*size - 1) * sizeof(WCHAR));
				out[*size - 1] = 0;
			}
			status = ERROR_MORE_DATA;
		}
	}
	if (size)
		*size = (DWORD)length;
	return status;
}

UINT out_text_a(const WCHAR *text, SIZE_T length, char *out, DWORD *size)
{
	INT needed = length ? WideCharToMultiByte(CP_ACP, 0, text, (INT)length, NULL, 0, NULL, NULL) : 0;
	UINT status = ERROR_SUCCESS;
	if (out && size) {
		if ((INT)*size > needed) {
			if (needed)
				WideCharToMultiByte(CP_ACP, 0, text, (INT)length, out, needed, NULL, NULL);
			out[needed] = 0;
		} else {
			if (*size) {
				char *whole = msi_alloc(needed + 1);
				if (whole) {
					WideCharToMultiByte(CP_ACP, 0, text, (INT)length, whole, needed, NULL, NULL);
					memcpy(out, whole, *size - 1);
					msi_free(whole);
				}
				out[*size - 1] = 0;
			}
			status = ERROR_MORE_DATA;
		}
	}
	if (size)
		*size = needed;
	return status;
}

msi_blob *blob_new(const void *data, SIZE_T size)
{
	msi_blob *blob = msi_alloc(sizeof(*blob));
	if (!blob)
		return NULL;
	blob->refs = 1;
	blob->size = size;
	blob->data = msi_alloc(size);
	if (data && size)
		memcpy(blob->data, data, size);
	return blob;
}

msi_blob *blob_ref(msi_blob *blob)
{
	if (blob)
		InterlockedIncrement(&blob->refs);
	return blob;
}

void blob_unref(msi_blob *blob)
{
	if (blob && !InterlockedDecrement(&blob->refs)) {
		msi_free(blob->data);
		msi_free(blob);
	}
}

msi_blob *blob_from_file(const WCHAR *path)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	DWORD size = GetFileSize(file, NULL), done = 0;
	msi_blob *blob = size == INVALID_FILE_SIZE ? NULL : blob_new(NULL, size);
	if (blob && !ReadFile(file, blob->data, size, &done, NULL)) {
		blob_unref(blob);
		blob = NULL;
	}
	CloseHandle(file);
	return blob;
}
