/* SPDX-License-Identifier: MIT */
#include "wic.h"

/* IWICStream forwards to a stream it was initialized with; memory, files and regions of another stream are each a small IStream. */

typedef struct
{
	IStream iface;
	LONG references;
	BYTE *memory;
	HANDLE file;
	IStream *parent;
	ULARGE_INTEGER offset, size, position;
} backing;

static backing *backing_of(IStream *iface)
{
	return WIC_CONTAINING(iface, backing, iface);
}

static HRESULT WINAPI backing_QueryInterface(IStream *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ISequentialStream) || IsEqualGUID(riid, &IID_IStream)) {
		*out = iface;
		IStream_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI backing_AddRef(IStream *iface)
{
	return InterlockedIncrement(&backing_of(iface)->references);
}

static ULONG WINAPI backing_Release(IStream *iface)
{
	backing *self = backing_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		if (self->file)
			CloseHandle(self->file);
		if (self->parent)
			IStream_Release(self->parent);
		wic_free(self);
	}
	return left;
}

/* A region reads and writes its parent without moving it. */
static HRESULT parent_transfer(backing *self, const void *source, void *target, ULONG size, ULONG *done)
{
	const LARGE_INTEGER none = {{0, 0}};
	ULARGE_INTEGER kept;
	LARGE_INTEGER place;
	HRESULT result;
	if (FAILED(result = IStream_Seek(self->parent, none, STREAM_SEEK_CUR, &kept)))
		return result;
	place.QuadPart = (LONGLONG)(self->offset.QuadPart + self->position.QuadPart);
	result = IStream_Seek(self->parent, place, STREAM_SEEK_SET, NULL);
	if (SUCCEEDED(result))
		result = source ? IStream_Write(self->parent, source, size, done) : IStream_Read(self->parent, target, size, done);
	place.QuadPart = (LONGLONG)kept.QuadPart;
	IStream_Seek(self->parent, place, STREAM_SEEK_SET, NULL);
	return result;
}

static HRESULT WINAPI backing_Read(IStream *iface, void *target, ULONG size, ULONG *done)
{
	backing *self = backing_of(iface);
	ULONG got = 0;
	HRESULT result = S_OK;
	if (!target)
		return E_INVALIDARG;
	if (self->file) {
		if (!ReadFile(self->file, target, size, &got, NULL))
			return HRESULT_FROM_WIN32(GetLastError());
	}
	else {
		const UINT64 left = self->position.QuadPart < self->size.QuadPart ? self->size.QuadPart - self->position.QuadPart : 0;
		const ULONG wanted = size < left ? size : (ULONG)left;
		if (self->memory) {
			memcpy(target, self->memory + self->position.QuadPart, wanted);
			got = wanted;
		}
		else if (FAILED(result = parent_transfer(self, NULL, target, wanted, &got)))
			return result;
		self->position.QuadPart += got;
	}
	if (done)
		*done = got;
	return S_OK;
}

static HRESULT WINAPI backing_Write(IStream *iface, const void *source, ULONG size, ULONG *done)
{
	backing *self = backing_of(iface);
	ULONG put = 0;
	HRESULT result;
	if (!source)
		return E_INVALIDARG;
	if (self->file) {
		if (!WriteFile(self->file, source, size, &put, NULL))
			return HRESULT_FROM_WIN32(GetLastError());
	}
	else {
		if (self->memory) {
			if (self->position.QuadPart + size > self->size.QuadPart)
				return STG_E_MEDIUMFULL;
			memcpy(self->memory + self->position.QuadPart, source, size);
			put = size;
		}
		else {
			const UINT64 room = self->position.QuadPart < self->size.QuadPart ? self->size.QuadPart - self->position.QuadPart : 0;
			if (FAILED(result = parent_transfer(self, source, NULL, size < room ? size : (ULONG)room, &put)))
				return result;
		}
		self->position.QuadPart += put;
	}
	if (done)
		*done = put;
	return S_OK;
}

static HRESULT WINAPI backing_Seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *now)
{
	backing *self = backing_of(iface);
	LONGLONG base;
	if (self->file) {
		LARGE_INTEGER place;
		if (!SetFilePointerEx(self->file, move, &place, origin))
			return HRESULT_FROM_WIN32(GetLastError());
		if (now)
			now->QuadPart = (ULONGLONG)place.QuadPart;
		return S_OK;
	}
	switch (origin) {
	case STREAM_SEEK_SET:
		base = 0;
		break;
	case STREAM_SEEK_CUR:
		base = (LONGLONG)self->position.QuadPart;
		break;
	case STREAM_SEEK_END:
		base = (LONGLONG)self->size.QuadPart;
		break;
	default:
		return E_INVALIDARG;
	}
	if (base + move.QuadPart < 0)
		return self->parent ? WINCODEC_ERR_VALUEOUTOFRANGE : HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
	if (!self->parent && (ULONGLONG)(base + move.QuadPart) >> 32)
		return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
	if ((ULONGLONG)(base + move.QuadPart) > self->size.QuadPart)
		return self->parent ? WINCODEC_ERR_VALUEOUTOFRANGE : E_INVALIDARG;
	self->position.QuadPart = (ULONGLONG)(base + move.QuadPart);
	if (now)
		*now = self->position;
	return S_OK;
}

static HRESULT WINAPI backing_SetSize(IStream *iface, ULARGE_INTEGER size)
{
	backing *self = backing_of(iface);
	LARGE_INTEGER place, keep = {{0, 0}};
	if (!self->file)
		return E_NOTIMPL;
	place.QuadPart = (LONGLONG)size.QuadPart;
	if (!SetFilePointerEx(self->file, keep, &keep, FILE_CURRENT) || !SetFilePointerEx(self->file, place, NULL, FILE_BEGIN) || !SetEndOfFile(self->file))
		return HRESULT_FROM_WIN32(GetLastError());
	SetFilePointerEx(self->file, keep, NULL, FILE_BEGIN);
	return S_OK;
}

static HRESULT WINAPI backing_CopyTo(IStream *iface, IStream *target, ULARGE_INTEGER size, ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI backing_Commit(IStream *iface, DWORD flags)
{
	return S_OK;
}

static HRESULT WINAPI backing_Revert(IStream *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI backing_LockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI backing_UnlockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI backing_Stat(IStream *iface, STATSTG *stat, DWORD flags)
{
	backing *self = backing_of(iface);
	if (!stat)
		return E_INVALIDARG;
	memset(stat, 0, sizeof(*stat));
	stat->type = STGTY_STREAM;
	if (self->file) {
		LARGE_INTEGER size;
		if (!GetFileSizeEx(self->file, &size))
			return HRESULT_FROM_WIN32(GetLastError());
		stat->cbSize.QuadPart = (ULONGLONG)size.QuadPart;
	}
	else
		stat->cbSize = self->size;
	return S_OK;
}

static HRESULT WINAPI backing_Clone(IStream *iface, IStream **out)
{
	return E_NOTIMPL;
}

static const IStreamVtbl backing_vtbl = {
	backing_QueryInterface, backing_AddRef, backing_Release, backing_Read, backing_Write, backing_Seek, backing_SetSize, backing_CopyTo,
	backing_Commit, backing_Revert, backing_LockRegion, backing_UnlockRegion, backing_Stat, backing_Clone,
};

static backing *backing_new(void)
{
	backing *self = wic_alloc(sizeof(*self));
	if (self) {
		self->iface.lpVtbl = &backing_vtbl;
		self->references = 1;
	}
	return self;
}

typedef struct
{
	IWICStream iface;
	LONG references;
	IStream *inner;
} wic_stream;

static wic_stream *stream_of(IWICStream *iface)
{
	return WIC_CONTAINING(iface, wic_stream, iface);
}

static HRESULT WINAPI stream_QueryInterface(IWICStream *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ISequentialStream) || IsEqualGUID(riid, &IID_IStream) || IsEqualGUID(riid, &IID_IWICStream)) {
		*out = iface;
		IWICStream_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI stream_AddRef(IWICStream *iface)
{
	return InterlockedIncrement(&stream_of(iface)->references);
}

static ULONG WINAPI stream_Release(IWICStream *iface)
{
	wic_stream *self = stream_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		if (self->inner)
			IStream_Release(self->inner);
		wic_free(self);
	}
	return left;
}

#define INNER(iface) (stream_of(iface)->inner)
#define NEEDS_INNER(iface) do { if (!INNER(iface)) return WINCODEC_ERR_NOTINITIALIZED; } while (0)

static HRESULT WINAPI stream_Read(IWICStream *iface, void *target, ULONG size, ULONG *done)
{
	NEEDS_INNER(iface);
	return IStream_Read(INNER(iface), target, size, done);
}

static HRESULT WINAPI stream_Write(IWICStream *iface, const void *source, ULONG size, ULONG *done)
{
	NEEDS_INNER(iface);
	return IStream_Write(INNER(iface), source, size, done);
}

static HRESULT WINAPI stream_Seek(IWICStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *now)
{
	NEEDS_INNER(iface);
	return IStream_Seek(INNER(iface), move, origin, now);
}

static HRESULT WINAPI stream_SetSize(IWICStream *iface, ULARGE_INTEGER size)
{
	NEEDS_INNER(iface);
	return IStream_SetSize(INNER(iface), size);
}

static HRESULT WINAPI stream_CopyTo(IWICStream *iface, IStream *target, ULARGE_INTEGER size, ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
	NEEDS_INNER(iface);
	return IStream_CopyTo(INNER(iface), target, size, read, written);
}

static HRESULT WINAPI stream_Commit(IWICStream *iface, DWORD flags)
{
	NEEDS_INNER(iface);
	return IStream_Commit(INNER(iface), flags);
}

static HRESULT WINAPI stream_Revert(IWICStream *iface)
{
	NEEDS_INNER(iface);
	return IStream_Revert(INNER(iface));
}

static HRESULT WINAPI stream_LockRegion(IWICStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
	NEEDS_INNER(iface);
	return IStream_LockRegion(INNER(iface), offset, size, type);
}

static HRESULT WINAPI stream_UnlockRegion(IWICStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
	NEEDS_INNER(iface);
	return IStream_UnlockRegion(INNER(iface), offset, size, type);
}

static HRESULT WINAPI stream_Stat(IWICStream *iface, STATSTG *stat, DWORD flags)
{
	NEEDS_INNER(iface);
	return IStream_Stat(INNER(iface), stat, flags);
}

static HRESULT WINAPI stream_Clone(IWICStream *iface, IStream **out)
{
	NEEDS_INNER(iface);
	return IStream_Clone(INNER(iface), out);
}

static HRESULT WINAPI stream_InitializeFromIStream(IWICStream *iface, IStream *source)
{
	if (!source)
		return E_INVALIDARG;
	if (INNER(iface))
		return WINCODEC_ERR_WRONGSTATE;
	IStream_AddRef(source);
	INNER(iface) = source;
	return S_OK;
}

static HRESULT WINAPI stream_InitializeFromFilename(IWICStream *iface, LPCWSTR name, DWORD access)
{
	backing *inner;
	DWORD wanted = 0, disposition = OPEN_EXISTING;
	HANDLE file;
	if (!name)
		return E_INVALIDARG;
	if (INNER(iface))
		return WINCODEC_ERR_WRONGSTATE;
	if (access & GENERIC_READ)
		wanted |= GENERIC_READ;
	if (access & GENERIC_WRITE) {
		wanted |= GENERIC_WRITE;
		disposition = CREATE_ALWAYS;
	}
	file = CreateFileW(name, wanted, FILE_SHARE_READ, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());
	if (!(inner = backing_new())) {
		CloseHandle(file);
		return E_OUTOFMEMORY;
	}
	inner->file = file;
	INNER(iface) = &inner->iface;
	return S_OK;
}

static HRESULT WINAPI stream_InitializeFromMemory(IWICStream *iface, BYTE *memory, DWORD size)
{
	backing *inner;
	if (!memory)
		return E_INVALIDARG;
	if (INNER(iface))
		return WINCODEC_ERR_WRONGSTATE;
	if (!(inner = backing_new()))
		return E_OUTOFMEMORY;
	inner->memory = memory;
	inner->size.QuadPart = size;
	INNER(iface) = &inner->iface;
	return S_OK;
}

static HRESULT WINAPI stream_InitializeFromIStreamRegion(IWICStream *iface, IStream *source, ULARGE_INTEGER offset, ULARGE_INTEGER size)
{
	backing *inner;
	if (!source)
		return E_INVALIDARG;
	if (INNER(iface))
		return WINCODEC_ERR_WRONGSTATE;
	if (!(inner = backing_new()))
		return E_OUTOFMEMORY;
	IStream_AddRef(source);
	inner->parent = source;
	inner->offset = offset;
	inner->size = size;
	{
		STATSTG stat;
		if (SUCCEEDED(IStream_Stat(source, &stat, STATFLAG_NONAME))) {
			const UINT64 left = offset.QuadPart < stat.cbSize.QuadPart ? stat.cbSize.QuadPart - offset.QuadPart : 0;
			if (inner->size.QuadPart > left)
				inner->size.QuadPart = left;
		}
	}
	INNER(iface) = &inner->iface;
	return S_OK;
}

static const IWICStreamVtbl stream_vtbl = {
	stream_QueryInterface, stream_AddRef, stream_Release, stream_Read, stream_Write, stream_Seek, stream_SetSize, stream_CopyTo, stream_Commit,
	stream_Revert, stream_LockRegion, stream_UnlockRegion, stream_Stat, stream_Clone, stream_InitializeFromIStream, stream_InitializeFromFilename,
	stream_InitializeFromMemory, stream_InitializeFromIStreamRegion,
};

HRESULT wic_stream_create(IWICStream **out)
{
	wic_stream *self = wic_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &stream_vtbl;
	self->references = 1;
	*out = &self->iface;
	return S_OK;
}
