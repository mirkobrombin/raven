/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: streams over files (SHCreateStreamOnFile, SHCreateStreamOnFileEx) and registry values (SHOpenRegStream).
 *
 * The stream holds the file handle it opened and reads, writes and seeks through it; its storage modes choose the access, the sharing
 * and whether the file is created, as the shell's file streams do. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>

typedef struct {
	const IStreamVtbl *vtbl;
	LONG refs;
	HANDLE file;
	DWORD mode;
	WCHAR name[MAX_PATH];
} FileStream;

static HRESULT last_error(void)
{
	return HRESULT_FROM_WIN32(GetLastError());
}

static HRESULT WINAPI stream_query(IStream *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (InlineIsEqualGUID(iid, &IID_IUnknown) || InlineIsEqualGUID(iid, &IID_ISequentialStream) || InlineIsEqualGUID(iid, &IID_IStream)) {
		*out = iface;
		IStream_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI stream_add(IStream *iface)
{
	return InterlockedIncrement(&((FileStream *)iface)->refs);
}

static ULONG WINAPI stream_release(IStream *iface)
{
	FileStream *stream = (FileStream *)iface;
	const ULONG refs = InterlockedDecrement(&stream->refs);
	if (!refs) {
		CloseHandle(stream->file);
		HeapFree(GetProcessHeap(), 0, stream);
	}
	return refs;
}

static HRESULT WINAPI stream_read(IStream *iface, void *buffer, ULONG size, ULONG *done)
{
	DWORD read = 0;
	if (!ReadFile(((FileStream *)iface)->file, buffer, size, &read, NULL))
		return last_error();
	if (done)
		*done = read;
	return read == size ? S_OK : S_FALSE;
}

static HRESULT WINAPI stream_write(IStream *iface, const void *buffer, ULONG size, ULONG *done)
{
	DWORD written = 0;
	if ((((FileStream *)iface)->mode & 3) == STGM_READ)
		return STG_E_ACCESSDENIED;
	if (!WriteFile(((FileStream *)iface)->file, buffer, size, &written, NULL))
		return last_error();
	if (done)
		*done = written;
	return S_OK;
}

static HRESULT WINAPI stream_seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
	LARGE_INTEGER moved;
	if (origin > STREAM_SEEK_END)
		return STG_E_INVALIDFUNCTION;
	if (!SetFilePointerEx(((FileStream *)iface)->file, move, &moved, origin))
		return last_error();
	if (position)
		position->QuadPart = moved.QuadPart;
	return S_OK;
}

static HRESULT WINAPI stream_size(IStream *iface, ULARGE_INTEGER size)
{
	FileStream *stream = (FileStream *)iface;
	LARGE_INTEGER zero = {0}, here, end;
	if ((stream->mode & 3) == STGM_READ)
		return STG_E_ACCESSDENIED;
	if (!SetFilePointerEx(stream->file, zero, &here, FILE_CURRENT))
		return last_error();
	end.QuadPart = (LONGLONG)size.QuadPart;
	if (!SetFilePointerEx(stream->file, end, NULL, FILE_BEGIN) || !SetEndOfFile(stream->file)) {
		const HRESULT result = last_error();
		SetFilePointerEx(stream->file, here, NULL, FILE_BEGIN);
		return result;
	}
	SetFilePointerEx(stream->file, here, NULL, FILE_BEGIN);
	return S_OK;
}

static HRESULT WINAPI stream_copy(IStream *iface, IStream *target, ULARGE_INTEGER size, ULARGE_INTEGER *read_total, ULARGE_INTEGER *written_total)
{
	BYTE buffer[4096];
	ULONGLONG read_sum = 0, written_sum = 0;
	HRESULT result = S_OK;
	if (!target)
		return STG_E_INVALIDPOINTER;
	while (read_sum < size.QuadPart) {
		const ULONG want = size.QuadPart - read_sum < sizeof buffer ? (ULONG)(size.QuadPart - read_sum) : sizeof buffer;
		ULONG read = 0, written = 0;
		result = stream_read(iface, buffer, want, &read);
		if (FAILED(result) || !read)
			break;
		read_sum += read;
		result = IStream_Write(target, buffer, read, &written);
		written_sum += written;
		if (FAILED(result) || written != read)
			break;
	}
	if (read_total)
		read_total->QuadPart = read_sum;
	if (written_total)
		written_total->QuadPart = written_sum;
	return FAILED(result) ? result : S_OK;
}

static HRESULT WINAPI stream_commit(IStream *iface, DWORD flags)
{
	return S_OK;
}

static HRESULT WINAPI stream_revert(IStream *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_lock(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_stat(IStream *iface, STATSTG *stat, DWORD flags)
{
	FileStream *stream = (FileStream *)iface;
	BY_HANDLE_FILE_INFORMATION information;
	if (!stat)
		return STG_E_INVALIDPOINTER;
	if (!GetFileInformationByHandle(stream->file, &information))
		return last_error();
	ZeroMemory(stat, sizeof *stat);
	if (!(flags & STATFLAG_NONAME)) {
		const int bytes = (lstrlenW(stream->name) + 1) * sizeof(WCHAR);
		if (!(stat->pwcsName = CoTaskMemAlloc(bytes)))
			return E_OUTOFMEMORY;
		CopyMemory(stat->pwcsName, stream->name, bytes);
	}
	stat->type = STGTY_STREAM;
	stat->cbSize.u.LowPart = information.nFileSizeLow;
	stat->cbSize.u.HighPart = information.nFileSizeHigh;
	stat->mtime = information.ftLastWriteTime;
	stat->ctime = information.ftCreationTime;
	stat->atime = information.ftLastAccessTime;
	stat->grfMode = stream->mode;
	return S_OK;
}

static HRESULT WINAPI stream_clone(IStream *iface, IStream **clone)
{
	if (clone)
		*clone = NULL;
	return E_NOTIMPL;
}

static const IStreamVtbl stream_vtbl = {stream_query, stream_add, stream_release, stream_read, stream_write, stream_seek, stream_size, stream_copy,
	stream_commit, stream_revert, stream_lock, stream_lock, stream_stat, stream_clone};

HRESULT WINAPI SHCreateStreamOnFileEx(LPCWSTR path, DWORD mode, DWORD attributes, BOOL create, IStream *pattern, IStream **out)
{
	DWORD access, share, disposition;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!path || (mode & (STGM_CONVERT | STGM_DELETEONRELEASE | STGM_TRANSACTED)))
		return E_INVALIDARG;
	switch (mode & 3) {
	case STGM_READ:
		access = GENERIC_READ;
		break;
	case STGM_WRITE:
	case STGM_READWRITE:
		access = GENERIC_READ | GENERIC_WRITE;
		break;
	default:
		return E_INVALIDARG;
	}
	switch (mode & 0x70) {
	case STGM_SHARE_EXCLUSIVE:
		share = 0;
		break;
	case STGM_SHARE_DENY_WRITE:
		share = FILE_SHARE_READ;
		break;
	case STGM_SHARE_DENY_READ:
		share = FILE_SHARE_WRITE;
		break;
	case 0:
	case STGM_SHARE_DENY_NONE:
		share = FILE_SHARE_READ | FILE_SHARE_WRITE;
		break;
	default:
		return E_INVALIDARG;
	}
	/* STGM_CREATE replaces the file; otherwise an existing file is opened, or a new one made when the caller asks for it. */
	disposition = mode & STGM_CREATE ? CREATE_ALWAYS : create ? CREATE_NEW : OPEN_EXISTING;
	HANDLE file = CreateFileW(path, access, share, NULL, disposition, attributes ? attributes : FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return last_error();
	FileStream *stream = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *stream);
	if (!stream) {
		CloseHandle(file);
		return E_OUTOFMEMORY;
	}
	stream->vtbl = &stream_vtbl;
	stream->refs = 1;
	stream->file = file;
	stream->mode = mode;
	lstrcpynW(stream->name, path, MAX_PATH);
	*out = (IStream *)stream;
	return S_OK;
}

HRESULT WINAPI SHCreateStreamOnFileW(LPCWSTR path, DWORD mode, IStream **out)
{
	return SHCreateStreamOnFileEx(path, mode, 0, FALSE, NULL, out);
}

HRESULT WINAPI SHCreateStreamOnFileA(LPCSTR path, DWORD mode, IStream **out)
{
	WCHAR wide[MAX_PATH];
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!path || !MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH))
		return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
	return SHCreateStreamOnFileEx(wide, mode, 0, FALSE, NULL, out);
}

/* Streams over a registry value (SHOpenRegStream2, SHOpenRegStream, OpenRegStream): the value's bytes are read into memory when the
 * stream opens, and a stream opened for writing stores what it then holds back into the value, as binary data, when it is released. */
typedef struct {
	const IStreamVtbl *vtbl;
	LONG refs;
	BYTE *data;
	DWORD size, capacity, position;
	HKEY key;
	WCHAR value[256];
	BOOL writable, changed;
} RegistryStream;

static ULONG WINAPI registry_release(IStream *iface)
{
	RegistryStream *stream = (RegistryStream *)iface;
	const ULONG refs = InterlockedDecrement(&stream->refs);
	if (!refs) {
		if (stream->key) {
			if (stream->writable && stream->changed)
				RegSetValueExW(stream->key, stream->value[0] ? stream->value : NULL, 0, REG_BINARY, stream->data, stream->size);
			RegCloseKey(stream->key);
		}
		HeapFree(GetProcessHeap(), 0, stream->data);
		HeapFree(GetProcessHeap(), 0, stream);
	}
	return refs;
}

static ULONG WINAPI registry_add(IStream *iface)
{
	return InterlockedIncrement(&((RegistryStream *)iface)->refs);
}

static HRESULT WINAPI registry_read(IStream *iface, void *buffer, ULONG size, ULONG *done)
{
	RegistryStream *stream = (RegistryStream *)iface;
	const DWORD left = stream->position < stream->size ? stream->size - stream->position : 0, count = size < left ? size : left;
	CopyMemory(buffer, stream->data + stream->position, count);
	stream->position += count;
	if (done)
		*done = count;
	return count == size ? S_OK : S_FALSE;
}

static BOOL registry_room(RegistryStream *stream, DWORD size)
{
	if (size <= stream->capacity)
		return TRUE;
	DWORD capacity = stream->capacity ? stream->capacity : 256;
	while (capacity < size)
		capacity *= 2;
	BYTE *grown = stream->data ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, stream->data, capacity) :
		HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity);
	if (!grown)
		return FALSE;
	stream->data = grown;
	stream->capacity = capacity;
	return TRUE;
}

static HRESULT WINAPI registry_write(IStream *iface, const void *buffer, ULONG size, ULONG *done)
{
	RegistryStream *stream = (RegistryStream *)iface;
	if (!stream->writable)
		return STG_E_ACCESSDENIED;
	if (!registry_room(stream, stream->position + size))
		return E_OUTOFMEMORY;
	CopyMemory(stream->data + stream->position, buffer, size);
	stream->position += size;
	if (stream->position > stream->size)
		stream->size = stream->position;
	stream->changed = TRUE;
	if (done)
		*done = size;
	return S_OK;
}

static HRESULT WINAPI registry_seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
	RegistryStream *stream = (RegistryStream *)iface;
	const LONGLONG base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? stream->position : origin == STREAM_SEEK_END ? stream->size : -1;
	if (base < 0)
		return STG_E_INVALIDFUNCTION;
	const LONGLONG target = base + move.QuadPart;
	if (target < 0 || target > 0x7fffffff)
		return STG_E_INVALIDFUNCTION;
	stream->position = (DWORD)target;
	if (position)
		position->QuadPart = stream->position;
	return S_OK;
}

static HRESULT WINAPI registry_size(IStream *iface, ULARGE_INTEGER size)
{
	RegistryStream *stream = (RegistryStream *)iface;
	if (!stream->writable)
		return STG_E_ACCESSDENIED;
	if (size.QuadPart > 0x7fffffff || !registry_room(stream, (DWORD)size.QuadPart))
		return E_OUTOFMEMORY;
	stream->size = (DWORD)size.QuadPart;
	stream->changed = TRUE;
	return S_OK;
}

static HRESULT WINAPI registry_copy(IStream *iface, IStream *target, ULARGE_INTEGER size, ULARGE_INTEGER *read_total, ULARGE_INTEGER *written_total)
{
	RegistryStream *stream = (RegistryStream *)iface;
	const DWORD left = stream->position < stream->size ? stream->size - stream->position : 0;
	const ULONG count = size.QuadPart < left ? (ULONG)size.QuadPart : left;
	ULONG written = 0;
	const HRESULT result = target ? IStream_Write(target, stream->data + stream->position, count, &written) : STG_E_INVALIDPOINTER;
	stream->position += count;
	if (read_total)
		read_total->QuadPart = count;
	if (written_total)
		written_total->QuadPart = written;
	return result;
}

static HRESULT WINAPI registry_stat(IStream *iface, STATSTG *stat, DWORD flags)
{
	if (!stat)
		return STG_E_INVALIDPOINTER;
	ZeroMemory(stat, sizeof *stat);
	stat->type = STGTY_STREAM;
	stat->cbSize.QuadPart = ((RegistryStream *)iface)->size;
	return S_OK;
}

static const IStreamVtbl registry_vtbl = {stream_query, registry_add, registry_release, registry_read, registry_write, registry_seek, registry_size,
	registry_copy, stream_commit, stream_revert, stream_lock, stream_lock, registry_stat, stream_clone};

/* The stream of a value; without the key, or with it unreadable, a stream that holds nothing when `fallback` asks for one, else NULL. */
static IStream *registry_stream(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD mode, BOOL fallback)
{
	RegistryStream *stream = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *stream);
	if (!stream)
		return NULL;
	stream->vtbl = &registry_vtbl;
	stream->refs = 1;
	stream->writable = (mode & 3) != STGM_READ;
	if (value)
		lstrcpynW(stream->value, value, 256);
	const LONG opened = stream->writable ? RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &stream->key, NULL) :
		RegOpenKeyExW(root, subkey, 0, KEY_READ, &stream->key);
	if (opened) {
		stream->key = NULL;
		if (!fallback) {
			HeapFree(GetProcessHeap(), 0, stream);
			return NULL;
		}
		stream->writable = FALSE;
		return (IStream *)stream;
	}
	DWORD size = 0;
	if (!RegQueryValueExW(stream->key, value, NULL, NULL, NULL, &size) && size && registry_room(stream, size) &&
		!RegQueryValueExW(stream->key, value, NULL, NULL, stream->data, &size))
		stream->size = size;
	return (IStream *)stream;
}

static LPWSTR widened(LPCSTR text)
{
	if (!text)
		return NULL;
	const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	LPWSTR wide = HeapAlloc(GetProcessHeap(), 0, length * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

IStream *WINAPI SHOpenRegStream2W(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD mode)
{
	return registry_stream(root, subkey, value, mode, FALSE);
}

IStream *WINAPI SHOpenRegStream2A(HKEY root, LPCSTR subkey, LPCSTR value, DWORD mode)
{
	LPWSTR wide_key = widened(subkey), wide_value = widened(value);
	IStream *stream = registry_stream(root, wide_key, wide_value, mode, FALSE);
	HeapFree(GetProcessHeap(), 0, wide_key);
	HeapFree(GetProcessHeap(), 0, wide_value);
	return stream;
}

/* SHOpenRegStream never fails for a missing key: it gives an empty stream instead. */
IStream *WINAPI SHOpenRegStreamW(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD mode)
{
	return registry_stream(root, subkey, value, mode, TRUE);
}

IStream *WINAPI SHOpenRegStreamA(HKEY root, LPCSTR subkey, LPCSTR value, DWORD mode)
{
	LPWSTR wide_key = widened(subkey), wide_value = widened(value);
	IStream *stream = registry_stream(root, wide_key, wide_value, mode, TRUE);
	HeapFree(GetProcessHeap(), 0, wide_key);
	HeapFree(GetProcessHeap(), 0, wide_value);
	return stream;
}

/* The shell's own name for the same stream. */
IStream *WINAPI OpenRegStream(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD mode)
{
	return registry_stream(root, subkey, value, mode, TRUE);
}
