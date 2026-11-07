/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "setup.h"
#include <fdi.h>
#include <string.h>

/* Compressed sources come in two forms: SZDD files, a small LZSS variant with a 14 byte header
 * ("SZDD", a second magic, the method, a hint letter, the original size), and cabinets, which go
 * through cabinet.dll. The FDI entry points are loaded on demand so that programs that never
 * touch a cabinet do not pull the library in. */

#define SZDD_HEADER 14
#define LZ_WINDOW 4096
#define LZ_FILL 0x20
#define LZ_START 4078

static const BYTE szdd_magic[8] = { 'S', 'Z', 'D', 'D', 0x88, 0xf0, 0x27, 0x33 };
static const BYTE cabinet_magic[4] = { 'M', 'S', 'C', 'F' };

static BYTE *read_whole(const WCHAR *path, DWORD *size)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	BYTE *data;
	DWORD got = 0;

	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	*size = GetFileSize(file, NULL);
	data = setup_alloc(*size ? *size : 1);
	if (!data || !ReadFile(file, data, *size, &got, NULL) || got != *size) {
		DWORD error = data ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
		setup_free(data);
		CloseHandle(file);
		SetLastError(error);
		return NULL;
	}
	CloseHandle(file);
	return data;
}

/* Expands SZDD text. Each flag byte holds eight items, low bit first: a set bit is a literal, a
 * clear one a two byte window reference, 12 bits of position and 4 of length minus three. */
static DWORD expand_szdd(const BYTE *data, DWORD size, BYTE *out, DWORD out_size)
{
	BYTE window[LZ_WINDOW];
	DWORD at = SZDD_HEADER, written = 0;
	unsigned position = LZ_START, bit;

	memset(window, LZ_FILL, sizeof(window));
	while (at < size && written < out_size) {
		BYTE flags = data[at++];
		for (bit = 0; bit < 8 && at < size && written < out_size; bit++) {
			if (flags & (1u << bit)) {
				BYTE unit = data[at++];
				out[written++] = unit;
				window[position] = unit;
				position = (position + 1) % LZ_WINDOW;
			} else {
				unsigned from, length, step;
				if (at + 1 >= size)
					return written;
				from = data[at] | ((data[at + 1] & 0xf0) << 4);
				length = (data[at + 1] & 0x0f) + 3;
				at += 2;
				for (step = 0; step < length && written < out_size; step++) {
					BYTE unit = window[(from + step) % LZ_WINDOW];
					out[written++] = unit;
					window[position] = unit;
					position = (position + 1) % LZ_WINDOW;
				}
			}
		}
	}
	return written;
}

static DWORD szdd_size(const BYTE *data)
{
	return data[10] | ((DWORD)data[11] << 8) | ((DWORD)data[12] << 16) | ((DWORD)data[13] << 24);
}

static UINT detect_type(const BYTE *data, DWORD size)
{
	if (size >= SZDD_HEADER && !memcmp(data, szdd_magic, sizeof(szdd_magic)))
		return FILE_COMPRESSION_WINLZA;
	if (size >= 4 && !memcmp(data, cabinet_magic, sizeof(cabinet_magic)))
		return FILE_COMPRESSION_MSZIP;
	return FILE_COMPRESSION_NONE;
}

/* The FDI glue. */

typedef HFDI (DIAMONDAPI *fdi_create_fn)(PFNALLOC, PFNFREE, PFNOPEN, PFNREAD, PFNWRITE, PFNCLOSE, PFNSEEK, int, PERF);
typedef BOOL (DIAMONDAPI *fdi_copy_fn)(HFDI, char *, char *, int, PFNFDINOTIFY, PFNFDIDECRYPT, void *);
typedef BOOL (DIAMONDAPI *fdi_destroy_fn)(HFDI);

static void *DIAMONDAPI fdi_alloc(ULONG size)
{
	return setup_alloc(size);
}

static void DIAMONDAPI fdi_free(void *memory)
{
	setup_free(memory);
}

static INT_PTR DIAMONDAPI fdi_open(char *name, int flags, int mode)
{
	/* Names come back as they went in: UTF-8 text, so wide paths survive. */
	WCHAR path[MAX_PATH * 2];
	HANDLE file;

	MultiByteToWideChar(CP_UTF8, 0, name, -1, path, ARRAYSIZE(path));
	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	return file == INVALID_HANDLE_VALUE ? -1 : (INT_PTR)file;
}

static UINT DIAMONDAPI fdi_read(INT_PTR file, void *buffer, UINT size)
{
	DWORD got = 0;

	return ReadFile((HANDLE)file, buffer, size, &got, NULL) ? got : (UINT)-1;
}

static UINT DIAMONDAPI fdi_write(INT_PTR file, void *buffer, UINT size)
{
	DWORD put = 0;

	return WriteFile((HANDLE)file, buffer, size, &put, NULL) ? put : (UINT)-1;
}

static int DIAMONDAPI fdi_close(INT_PTR file)
{
	return CloseHandle((HANDLE)file) ? 0 : -1;
}

static LONG DIAMONDAPI fdi_seek(INT_PTR file, LONG offset, int origin)
{
	return SetFilePointer((HANDLE)file, offset, NULL, origin);
}

struct cabinet_api {
	HMODULE module;
	fdi_create_fn create;
	fdi_copy_fn copy;
	fdi_destroy_fn destroy;
};

static BOOL load_cabinet(struct cabinet_api *api)
{
	api->module = LoadLibraryW(L"cabinet.dll");
	if (!api->module)
		return FALSE;
	api->create = (fdi_create_fn)GetProcAddress(api->module, "FDICreate");
	api->copy = (fdi_copy_fn)GetProcAddress(api->module, "FDICopy");
	api->destroy = (fdi_destroy_fn)GetProcAddress(api->module, "FDIDestroy");
	if (!api->create || !api->copy || !api->destroy) {
		FreeLibrary(api->module);
		return FALSE;
	}
	return TRUE;
}

/* Splits a path for FDICopy, which wants the name and its directory apart, both in UTF-8. */
static void split_cabinet_path(const WCHAR *path, char *name, char *directory)
{
	const WCHAR *slash = wcsrchr(path, '\\');
	const WCHAR *other = wcsrchr(path, '/');
	WCHAR folder[MAX_PATH * 2];
	SIZE_T length;

	if (other > slash)
		slash = other;
	length = slash ? (SIZE_T)(slash - path) + 1 : 0;
	if (length)
		memcpy(folder, path, length * sizeof(WCHAR));
	folder[length] = 0;
	WideCharToMultiByte(CP_UTF8, 0, folder, -1, directory, CB_MAX_CAB_PATH, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : path, -1, name, CB_MAX_CABINET_NAME, NULL, NULL);
}

/* Extraction of one file. */

struct extract_state {
	const WCHAR *name;
	const WCHAR *target;
	BOOL done;
	DWORD error;
};

static BOOL cabinet_name_matches(const char *in_cabinet, UINT attributes, const WCHAR *wanted)
{
	WCHAR name[MAX_PATH];

	const WCHAR *leaf;

	MultiByteToWideChar(attributes & _A_NAME_IS_UTF ? CP_UTF8 : CP_ACP, 0, in_cabinet, -1, name, MAX_PATH);
	leaf = wcsrchr(name, '\\');
	return !lstrcmpiW(leaf ? leaf + 1 : name, wanted);
}

static INT_PTR DIAMONDAPI extract_notify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION info)
{
	struct extract_state *state = info->pv;

	switch (type) {
	case fdintCOPY_FILE: {
		HANDLE file;

		if (state->done || (state->name && !cabinet_name_matches(info->psz1, info->attribs, state->name)))
			return 0;
		file = CreateFileW(state->target, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE) {
			state->error = GetLastError();
			return -1;
		}
		return (INT_PTR)file;
	}
	case fdintCLOSE_FILE_INFO:
		CloseHandle((HANDLE)info->hf);
		state->done = TRUE;
		return TRUE;
	default:
		return 0;
	}
}

DWORD setup_extract_from_cabinet(const WCHAR *cabinet, const WCHAR *name, const WCHAR *target)
{
	struct cabinet_api api;
	struct extract_state state = { name, target, FALSE, 0 };
	char cabinet_name[CB_MAX_CABINET_NAME], directory[CB_MAX_CAB_PATH];
	ERF error;
	HFDI context;
	BOOL copied;

	if (!load_cabinet(&api))
		return ERROR_DLL_NOT_FOUND;
	context = api.create(fdi_alloc, fdi_free, fdi_open, fdi_read, fdi_write, fdi_close, fdi_seek, cpuUNKNOWN, &error);
	if (!context) {
		FreeLibrary(api.module);
		return ERROR_NOT_ENOUGH_MEMORY;
	}
	split_cabinet_path(cabinet, cabinet_name, directory);
	copied = api.copy(context, cabinet_name, directory, 0, extract_notify, NULL, &state);
	api.destroy(context);
	FreeLibrary(api.module);
	if (state.error)
		return state.error;
	if (!copied && !state.done)
		return ERROR_INVALID_DATA;
	return state.done ? 0 : ERROR_FILE_NOT_FOUND;
}

/* Size of the first file, for the compression query. */

struct first_size_state {
	DWORD size;
	BOOL found;
};

static INT_PTR DIAMONDAPI first_size_notify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION info)
{
	struct first_size_state *state = info->pv;

	if (type == fdintCOPY_FILE && !state->found) {
		state->size = (DWORD)info->cb;
		state->found = TRUE;
	}
	return 0;
}

static BOOL cabinet_first_size(const WCHAR *path, DWORD *size)
{
	struct cabinet_api api;
	struct first_size_state state = { 0, FALSE };
	char cabinet_name[CB_MAX_CABINET_NAME], directory[CB_MAX_CAB_PATH];
	ERF error;
	HFDI context;

	if (!load_cabinet(&api))
		return FALSE;
	context = api.create(fdi_alloc, fdi_free, fdi_open, fdi_read, fdi_write, fdi_close, fdi_seek, cpuUNKNOWN, &error);
	if (context) {
		split_cabinet_path(path, cabinet_name, directory);
		api.copy(context, cabinet_name, directory, 0, first_size_notify, NULL, &state);
		api.destroy(context);
	}
	FreeLibrary(api.module);
	*size = state.size;
	return state.found;
}

/* Public compression entry points. */

DWORD setup_decompress(const WCHAR *source, const WCHAR *target, UINT type)
{
	DWORD size = 0;
	BYTE *data = read_whole(source, &size);
	HANDLE file;
	DWORD error = 0;

	if (!data)
		return GetLastError();
	if (type == FILE_COMPRESSION_NTCAB || type == FILE_COMPRESSION_MSZIP) {
		setup_free(data);
		return setup_extract_from_cabinet(source, NULL, target);
	}
	file = CreateFileW(target, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		error = GetLastError();
		setup_free(data);
		return error;
	}
	if (type == FILE_COMPRESSION_WINLZA) {
		DWORD expanded = szdd_size(data), written, put = 0;
		BYTE *out = setup_alloc(expanded ? expanded : 1);
		if (!out) {
			error = ERROR_NOT_ENOUGH_MEMORY;
		} else {
			written = expand_szdd(data, size, out, expanded);
			if (!WriteFile(file, out, written, &put, NULL))
				error = GetLastError();
			setup_free(out);
		}
	} else {
		DWORD put = 0;
		if (!WriteFile(file, data, size, &put, NULL))
			error = GetLastError();
	}
	CloseHandle(file);
	setup_free(data);
	return error;
}

static UINT file_type(const WCHAR *path, DWORD *error)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	BYTE head[SZDD_HEADER];
	DWORD got = 0;

	*error = 0;
	if (file == INVALID_HANDLE_VALUE) {
		*error = GetLastError();
		return FILE_COMPRESSION_NONE;
	}
	ReadFile(file, head, sizeof(head), &got, NULL);
	CloseHandle(file);
	return detect_type(head, got);
}

DWORD WINAPI SetupDecompressOrCopyFileW(PCWSTR source, PCWSTR target, PUINT type)
{
	UINT kind;
	DWORD error;

	if (!source || !target)
		return ERROR_INVALID_PARAMETER;
	if (type) {
		kind = *type;
		if (kind > FILE_COMPRESSION_MSZIP)
			return ERROR_INVALID_PARAMETER;
		/* Saying LZ of something that is not stays a plain copy. */
		if (kind == FILE_COMPRESSION_WINLZA && file_type(source, &error) != FILE_COMPRESSION_WINLZA)
			kind = FILE_COMPRESSION_NONE;
	} else {
		kind = file_type(source, &error);
		if (error)
			return error;
	}
	return setup_decompress(source, target, kind);
}

DWORD WINAPI SetupDecompressOrCopyFileA(PCSTR source, PCSTR target, PUINT type)
{
	WCHAR *wide_source = setup_to_wide(source), *wide_target = setup_to_wide(target);
	DWORD result = SetupDecompressOrCopyFileW(wide_source, wide_target, type);

	setup_free(wide_source);
	setup_free(wide_target);
	return result;
}

BOOL WINAPI SetupGetFileCompressionInfoExW(PCWSTR source, PWSTR buffer, DWORD size, PDWORD required, PDWORD source_size, PDWORD target_size, PUINT type)
{
	HANDLE file;
	BYTE head[SZDD_HEADER];
	DWORD got = 0, length, whole;
	UINT kind;

	if (!source) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	length = lstrlenW(source) + 1;
	if (required)
		*required = length;
	if (!buffer || size < length) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	lstrcpyW(buffer, source);
	file = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	whole = GetFileSize(file, NULL);
	ReadFile(file, head, sizeof(head), &got, NULL);
	CloseHandle(file);
	kind = detect_type(head, got);
	if (source_size)
		*source_size = whole;
	if (target_size) {
		*target_size = whole;
		if (kind == FILE_COMPRESSION_WINLZA)
			*target_size = szdd_size(head);
		else if (kind == FILE_COMPRESSION_MSZIP)
			cabinet_first_size(source, target_size);
	}
	if (type)
		*type = kind;
	return TRUE;
}

BOOL WINAPI SetupGetFileCompressionInfoExA(PCSTR source, PSTR buffer, DWORD size, PDWORD required, PDWORD source_size, PDWORD target_size, PUINT type)
{
	WCHAR *wide = setup_to_wide(source), name[MAX_PATH * 2];
	DWORD wide_required = 0;
	BOOL result;

	if (!source) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	result = SetupGetFileCompressionInfoExW(wide, name, ARRAYSIZE(name), &wide_required, source_size, target_size, type);
	setup_free(wide);
	if (required)
		*required = lstrlenA(source) + 1;
	if (!result)
		return FALSE;
	if (!buffer || size < (DWORD)lstrlenA(source) + 1) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	lstrcpyA(buffer, source);
	return TRUE;
}

DWORD WINAPI SetupGetFileCompressionInfoW(PCWSTR source, PWSTR *name, PDWORD source_size, PDWORD target_size, PUINT type)
{
	WCHAR *copy;
	DWORD chars;

	if (!source || !name || !source_size || !target_size || !type)
		return ERROR_INVALID_PARAMETER;
	chars = lstrlenW(source) + 1;
	copy = setup_alloc((SIZE_T)chars * sizeof(WCHAR));
	if (!copy)
		return ERROR_NOT_ENOUGH_MEMORY;
	if (!SetupGetFileCompressionInfoExW(source, copy, chars, NULL, source_size, target_size, type)) {
		DWORD error = GetLastError();
		setup_free(copy);
		return error;
	}
	*name = copy;
	return ERROR_SUCCESS;
}

DWORD WINAPI SetupGetFileCompressionInfoA(PCSTR source, PSTR *name, PDWORD source_size, PDWORD target_size, PUINT type)
{
	WCHAR *wide, *wide_name = NULL;
	DWORD result;

	if (!source || !name || !source_size || !target_size || !type)
		return ERROR_INVALID_PARAMETER;
	wide = setup_to_wide(source);
	result = SetupGetFileCompressionInfoW(wide, &wide_name, source_size, target_size, type);
	setup_free(wide);
	if (result)
		return result;
	*name = setup_to_ansi(wide_name);
	setup_free(wide_name);
	return *name ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
}

/* Iteration over a cabinet with the setup style callback. */

struct iterate_state {
	PSP_FILE_CALLBACK_W callback;
	PSP_FILE_CALLBACK_A callback_a;
	void *context;
	const WCHAR *cabinet;
	WCHAR target[MAX_PATH];
	DWORD error;
};

static WCHAR *state_wide_name(const char *text, UINT attributes)
{
	UINT code_page = attributes & _A_NAME_IS_UTF ? CP_UTF8 : CP_ACP;
	int length = MultiByteToWideChar(code_page, 0, text, -1, NULL, 0);
	WCHAR *wide = setup_alloc((SIZE_T)length * sizeof(WCHAR));

	if (wide)
		MultiByteToWideChar(code_page, 0, text, -1, wide, length);
	return wide;
}

static INT_PTR DIAMONDAPI iterate_notify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION info)
{
	struct iterate_state *state = info->pv;

	switch (type) {
	case fdintCABINET_INFO: {
		WCHAR *next_cab = setup_to_wide(info->psz1), *next_disk = setup_to_wide(info->psz2), *next_path = setup_to_wide(info->psz3);

		if (state->callback_a) {
			CABINET_INFO_A cab;
			memset(&cab, 0, sizeof(cab));
			cab.CabinetPath = info->psz3;
			cab.CabinetFile = info->psz1;
			cab.DiskName = info->psz2;
			cab.SetId = info->setID;
			cab.CabinetNumber = info->iCabinet;
			state->callback_a(state->context, SPFILENOTIFY_CABINETINFO, (UINT_PTR)&cab, 0);
		} else {
			CABINET_INFO_W cab;
			memset(&cab, 0, sizeof(cab));
			cab.CabinetPath = next_path;
			cab.CabinetFile = next_cab;
			cab.DiskName = next_disk;
			cab.SetId = info->setID;
			cab.CabinetNumber = info->iCabinet;
			state->callback(state->context, SPFILENOTIFY_CABINETINFO, (UINT_PTR)&cab, 0);
		}
		setup_free(next_cab);
		setup_free(next_disk);
		setup_free(next_path);
		return 0;
	}
	case fdintCOPY_FILE: {
		WCHAR target[MAX_PATH];
		UINT answer;
		HANDLE file;

		target[0] = 0;
		if (state->callback_a) {
			FILE_IN_CABINET_INFO_A in_cabinet;
			memset(&in_cabinet, 0, sizeof(in_cabinet));
			in_cabinet.NameInCabinet = info->psz1;
			in_cabinet.FileSize = info->cb;
			in_cabinet.DosDate = info->date;
			in_cabinet.DosTime = info->time;
			in_cabinet.DosAttribs = info->attribs;
			{
				char *ansi_cabinet = setup_to_ansi(state->cabinet);
				answer = state->callback_a(state->context, SPFILENOTIFY_FILEINCABINET, (UINT_PTR)&in_cabinet, (UINT_PTR)ansi_cabinet);
				setup_free(ansi_cabinet);
			}
			if (answer == FILEOP_DOIT)
				MultiByteToWideChar(CP_ACP, 0, in_cabinet.FullTargetName, -1, target, MAX_PATH);
		} else {
			FILE_IN_CABINET_INFO_W in_cabinet;
			WCHAR *name = state_wide_name(info->psz1, info->attribs);
			memset(&in_cabinet, 0, sizeof(in_cabinet));
			in_cabinet.NameInCabinet = name;
			in_cabinet.FileSize = info->cb;
			in_cabinet.DosDate = info->date;
			in_cabinet.DosTime = info->time;
			in_cabinet.DosAttribs = info->attribs;
			answer = state->callback(state->context, SPFILENOTIFY_FILEINCABINET, (UINT_PTR)&in_cabinet, (UINT_PTR)state->cabinet);
			if (answer == FILEOP_DOIT)
				lstrcpyW(target, in_cabinet.FullTargetName);
			setup_free(name);
		}
		if (answer == FILEOP_ABORT) {
			state->error = GetLastError();
			return -1;
		}
		if (answer != FILEOP_DOIT)
			return 0;
		file = CreateFileW(target, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE) {
			state->error = GetLastError();
			return -1;
		}
		lstrcpyW(state->target, target);
		return (INT_PTR)file;
	}
	case fdintCLOSE_FILE_INFO: {
		FILETIME local, utc;
		HANDLE file = (HANDLE)info->hf;
		char *ansi_name = setup_to_ansi(state->target), *ansi_cabinet = setup_to_ansi(state->cabinet);

		if (DosDateTimeToFileTime(info->date, info->time, &local) && LocalFileTimeToFileTime(&local, &utc))
			SetFileTime(file, NULL, NULL, &utc);
		CloseHandle(file);
		if (state->callback_a) {
			FILEPATHS_A paths;
			memset(&paths, 0, sizeof(paths));
			paths.Source = ansi_cabinet;
			paths.Target = ansi_name;
			state->callback_a(state->context, SPFILENOTIFY_FILEEXTRACTED, (UINT_PTR)&paths, 0);
		} else {
			FILEPATHS_W paths;
			memset(&paths, 0, sizeof(paths));
			paths.Source = state->cabinet;
			paths.Target = state->target;
			state->callback(state->context, SPFILENOTIFY_FILEEXTRACTED, (UINT_PTR)&paths, 0);
		}
		setup_free(ansi_name);
		setup_free(ansi_cabinet);
		return TRUE;
	}
	default:
		return 0;
	}
}

static BOOL iterate_cabinet(const WCHAR *cabinet, PSP_FILE_CALLBACK_W callback, PSP_FILE_CALLBACK_A callback_a, void *context)
{
	struct cabinet_api api;
	struct iterate_state state;
	char cabinet_name[CB_MAX_CABINET_NAME], directory[CB_MAX_CAB_PATH];
	ERF error;
	HFDI fdi;
	BOOL copied;
	WCHAR full[MAX_PATH * 2];
	DWORD length;

	if (!cabinet) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	length = GetFullPathNameW(cabinet, ARRAYSIZE(full), full, NULL);
	if (!length || length >= ARRAYSIZE(full) || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	if (!load_cabinet(&api)) {
		SetLastError(ERROR_DLL_NOT_FOUND);
		return FALSE;
	}
	fdi = api.create(fdi_alloc, fdi_free, fdi_open, fdi_read, fdi_write, fdi_close, fdi_seek, cpuUNKNOWN, &error);
	if (!fdi) {
		FreeLibrary(api.module);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	memset(&state, 0, sizeof(state));
	state.callback = callback;
	state.callback_a = callback_a;
	state.context = context;
	state.cabinet = cabinet;
	split_cabinet_path(full, cabinet_name, directory);
	copied = api.copy(fdi, cabinet_name, directory, 0, iterate_notify, NULL, &state);
	api.destroy(fdi);
	FreeLibrary(api.module);
	if (state.error) {
		SetLastError(state.error);
		return FALSE;
	}
	if (!copied) {
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI SetupIterateCabinetW(PCWSTR cabinet, DWORD reserved, PSP_FILE_CALLBACK_W callback, PVOID context)
{
	return iterate_cabinet(cabinet, callback, NULL, context);
}

BOOL WINAPI SetupIterateCabinetA(PCSTR cabinet, DWORD reserved, PSP_FILE_CALLBACK_A callback, PVOID context)
{
	WCHAR *wide = setup_to_wide(cabinet);
	BOOL result = iterate_cabinet(wide, NULL, callback, context);

	setup_free(wide);
	return result;
}

/* Copying with the SP_COPY_ styles. */

static BOOL version_of(const WCHAR *path, ULONGLONG *version)
{
	DWORD handle, size = GetFileVersionInfoSizeW(path, &handle);
	BYTE *data;
	VS_FIXEDFILEINFO *info;
	UINT length;
	BOOL result = FALSE;

	if (!size)
		return FALSE;
	data = setup_alloc(size);
	if (data && GetFileVersionInfoW(path, 0, size, data) && VerQueryValueW(data, L"\\", (void **)&info, &length) && length) {
		*version = ((ULONGLONG)info->dwFileVersionMS << 32) | info->dwFileVersionLS;
		result = TRUE;
	}
	setup_free(data);
	return result;
}

BOOL setup_copy_styled(const WCHAR *source, const WCHAR *target, DWORD style, BOOL *in_use)
{
	BOOL exists = GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES;
	DWORD error;
	UINT kind = FILE_COMPRESSION_NONE;

	if (GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES) {
		error = GetLastError();
		if (!error)
			error = ERROR_FILE_NOT_FOUND;
		SetLastError(error);
		return FALSE;
	}
	if ((style & SP_COPY_REPLACEONLY) && !exists) {
		SetLastError(ERROR_SUCCESS);
		return TRUE;
	}
	if ((style & (SP_COPY_NOOVERWRITE | SP_COPY_FORCE_NOOVERWRITE)) && exists) {
		SetLastError(ERROR_SUCCESS);
		return TRUE;
	}
	if ((style & (SP_COPY_NEWER_OR_SAME | SP_COPY_NEWER_ONLY | SP_COPY_FORCE_NEWER)) && exists) {
		ULONGLONG from = 0, to = 0;
		if (version_of(source, &from) && version_of(target, &to)) {
			if (from < to || ((style & SP_COPY_NEWER_ONLY) && from == to)) {
				SetLastError(ERROR_SUCCESS);
				return TRUE;
			}
		}
	}
	if (!(style & SP_COPY_NODECOMP))
		kind = file_type(source, &error);
	/* Taking a read only target over needs its attribute off first. */
	if (exists)
		SetFileAttributesW(target, FILE_ATTRIBUTE_NORMAL);
	if (kind != FILE_COMPRESSION_NONE) {
		error = setup_decompress(source, target, kind);
		if (error) {
			SetLastError(error);
			return FALSE;
		}
	} else if (!CopyFileW(source, target, FALSE)) {
		return FALSE;
	}
	if (style & SP_COPY_DELETESOURCE)
		DeleteFileW(source);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI SetupInstallFileExW(HINF hinf, PINFCONTEXT line, PCWSTR source, PCWSTR root, PCWSTR target, DWORD style, PSP_FILE_CALLBACK_W callback, PVOID context, PBOOL in_use)
{
	WCHAR *full_source = NULL, *full_target = NULL, *subdir = NULL;
	const WCHAR *target_name = target;
	BOOL result = FALSE, busy = FALSE;
	UINT id = 0;

	if (in_use)
		*in_use = FALSE;
	if (hinf && hinf != INVALID_HANDLE_VALUE) {
		WCHAR *directory;
		const WCHAR *line_target;
		INFCONTEXT found;
		WCHAR section[256];
		UINT length;

		if (!line && source) {
			if (!SetupFindFirstLineW(hinf, L"SourceDisksFiles", source, &found))
				return FALSE;
		}
		line_target = line ? pSetupGetField(line, 1) : NULL;
		if (!target_name)
			target_name = line_target;
		if (!target_name)
			target_name = source;
		section[0] = 0;
		if (line)
			SetupEnumInfSectionsW(line->CurrentInf, line->Section, section, ARRAYSIZE(section), &length);
		directory = setup_section_target(hinf, section[0] ? section : NULL);
		full_target = directory && target_name ? setup_join_path(directory, target_name) : NULL;
		setup_free(directory);
		setup_source_location(hinf, source, &id, &subdir);
		full_source = setup_join_path(root, source);
		setup_free(subdir);
	} else {
		if (!source || !target) {
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		full_source = setup_join_path(root, source);
		full_target = setup_copy_wide(target);
	}
	if (!full_source || !full_target) {
		setup_free(full_source);
		setup_free(full_target);
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	{
		BOOL existed = GetFileAttributesW(full_target) != INVALID_FILE_ATTRIBUTES;
		if ((style & SP_COPY_REPLACEONLY) && !existed) {
			SetLastError(ERROR_SUCCESS);
			result = FALSE;
		} else if ((style & (SP_COPY_NOOVERWRITE | SP_COPY_FORCE_NOOVERWRITE)) && existed) {
			SetLastError(ERROR_SUCCESS);
			result = FALSE;
		} else {
			result = setup_copy_styled(full_source, full_target, style, &busy);
		}
	}
	if (in_use)
		*in_use = busy;
	setup_free(full_source);
	setup_free(full_target);
	return result;
}

BOOL WINAPI SetupInstallFileExA(HINF hinf, PINFCONTEXT line, PCSTR source, PCSTR root, PCSTR target, DWORD style, PSP_FILE_CALLBACK_A callback, PVOID context, PBOOL in_use)
{
	WCHAR *a = setup_to_wide(source), *b = setup_to_wide(root), *c = setup_to_wide(target);
	BOOL result = SetupInstallFileExW(hinf, line, a, b, c, style, (PSP_FILE_CALLBACK_W)callback, context, in_use);
	DWORD error = GetLastError();

	setup_free(a);
	setup_free(b);
	setup_free(c);
	SetLastError(error);
	return result;
}

BOOL WINAPI SetupInstallFileW(HINF hinf, PINFCONTEXT line, PCWSTR source, PCWSTR root, PCWSTR target, DWORD style, PSP_FILE_CALLBACK_W callback, PVOID context)
{
	return SetupInstallFileExW(hinf, line, source, root, target, style, callback, context, NULL);
}

BOOL WINAPI SetupInstallFileA(HINF hinf, PINFCONTEXT line, PCSTR source, PCSTR root, PCSTR target, DWORD style, PSP_FILE_CALLBACK_A callback, PVOID context)
{
	return SetupInstallFileExA(hinf, line, source, root, target, style, callback, context, NULL);
}
