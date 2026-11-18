/* The compound file reader and writer of ole32's storage (native/guest/raven-storage/container.c), which opens every MSI database, Office
   document and property set a program hands it, fed changed files on the host under the address and undefined behaviour sanitizers.
   The seeds are written by the same code: a file of 512-byte sectors and one of 4096-byte sectors, with streams in the mini stream and in
   regular sectors, nested storages and enough entries for several directory sectors. Each round changes a few bytes of one, most often
   in the header, the allocation tables and the directory, opens it, reads every stream of the tree it finds, and half the time grows and
   writes a stream and flushes. A changed file may be refused at any step; nothing may be read or written outside what was allocated.

   cc -fshort-wchar -fsanitize=address,undefined -fno-sanitize-recover=all -Inative/fuzz/storage -Inative/guest/raven-storage
      native/fuzz/storage_container.c native/guest/raven-storage/container.c
   ./a.out [rounds] [seed] */

#include <stdio.h>
#include <stdlib.h>

#include "docfile.h"

static uint64_t state = 0x9e3779b97f4a7c15ull;

static uint64_t next(void)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}

HANDLE GetProcessHeap(void)
{
	return (HANDLE)1;
}

void *HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
	(void)heap;
	return flags & HEAP_ZERO_MEMORY ? calloc(1, size) : malloc(size);
}

void *HeapReAlloc(HANDLE heap, DWORD flags, void *block, SIZE_T size)
{
	(void)heap;
	(void)flags;
	return realloc(block, size);
}

BOOL HeapFree(HANDLE heap, DWORD flags, void *block)
{
	(void)heap;
	(void)flags;
	free(block);
	return TRUE;
}

void InitializeCriticalSection(CRITICAL_SECTION *section)
{
	(void)section;
}

void DeleteCriticalSection(CRITICAL_SECTION *section)
{
	(void)section;
}

void EnterCriticalSection(CRITICAL_SECTION *section)
{
	(void)section;
}

void LeaveCriticalSection(CRITICAL_SECTION *section)
{
	(void)section;
}

LONG InterlockedIncrement(LONG volatile *value)
{
	return ++*value;
}

LONG InterlockedDecrement(LONG volatile *value)
{
	return --*value;
}

void GetSystemTimeAsFileTime(FILETIME *time)
{
	time->dwLowDateTime = 0x12345678u;
	time->dwHighDateTime = 0x01d00000u;
}

int lstrlenW(const WCHAR *text)
{
	int length = 0;

	while (text[length])
		++length;
	return length;
}

WCHAR *lstrcpyW(WCHAR *to, const WCHAR *from)
{
	int at = 0;

	do
		to[at] = from[at];
	while (from[at++]);
	return to;
}

WCHAR *lstrcpynW(WCHAR *to, const WCHAR *from, int count)
{
	int at = 0;

	if (count <= 0)
		return to;
	for (; at < count - 1 && from[at]; ++at)
		to[at] = from[at];
	to[at] = 0;
	return to;
}

/* Only units outside ASCII come here: those of Latin-1 have their capitals 0x20 below, as do Greek and Cyrillic ones of the ranges
   checked; the rest stay. */
WCHAR *CharUpperW(WCHAR *text)
{
	const ULONG_PTR unit = (ULONG_PTR)text;

	if ((unit >= 0xe0 && unit <= 0xfe && unit != 0xf7) || (unit >= 0x3b1 && unit <= 0x3c9 && unit != 0x3c2) || (unit >= 0x430 && unit <= 0x44f))
		return (WCHAR *)(unit - 0x20);
	return text;
}

/* The bytes of a file in memory. A write past the end grows it, as a file does; a size larger than the fuzzer allows is refused, as a
   full disk would be. */
struct ILockBytes {
	BYTE *data;
	SIZE_T size;
	LONG refs;
};

#define LARGEST (64u << 20)

HRESULT ILockBytes_ReadAt(ILockBytes *bytes, ULARGE_INTEGER offset, void *buffer, ULONG count, ULONG *done)
{
	ULONG got = 0;

	if (offset.QuadPart < bytes->size)
		got = (ULONG)(bytes->size - offset.QuadPart < count ? bytes->size - offset.QuadPart : count);
	if (got)
		memcpy(buffer, bytes->data + offset.QuadPart, got);
	if (done)
		*done = got;
	return S_OK;
}

HRESULT ILockBytes_SetSize(ILockBytes *bytes, ULARGE_INTEGER size)
{
	BYTE *grown;

	if (size.QuadPart > LARGEST)
		return STG_E_MEDIUMFULL;
	grown = realloc(bytes->data, size.QuadPart ? (SIZE_T)size.QuadPart : 1);
	if (!grown)
		return E_OUTOFMEMORY;
	if (size.QuadPart > bytes->size)
		memset(grown + bytes->size, 0, (SIZE_T)size.QuadPart - bytes->size);
	bytes->data = grown;
	bytes->size = (SIZE_T)size.QuadPart;
	return S_OK;
}

HRESULT ILockBytes_WriteAt(ILockBytes *bytes, ULARGE_INTEGER offset, const void *buffer, ULONG count, ULONG *done)
{
	ULARGE_INTEGER end;

	if (done)
		*done = 0;
	end.QuadPart = offset.QuadPart + count;
	if (end.QuadPart > bytes->size) {
		HRESULT status = ILockBytes_SetSize(bytes, end);
		if (FAILED(status))
			return status;
	}
	memcpy(bytes->data + offset.QuadPart, buffer, count);
	if (done)
		*done = count;
	return S_OK;
}

HRESULT ILockBytes_Flush(ILockBytes *bytes)
{
	(void)bytes;
	return S_OK;
}

ULONG ILockBytes_AddRef(ILockBytes *bytes)
{
	return (ULONG)++bytes->refs;
}

ULONG ILockBytes_Release(ILockBytes *bytes)
{
	return (ULONG)--bytes->refs;
}

static void fill(BYTE *data, SIZE_T size, unsigned seed)
{
	for (SIZE_T at = 0; at < size; ++at)
		data[at] = (BYTE)(at * 31 + seed);
}

static void name(WCHAR *text, const char *prefix, unsigned number)
{
	int at = 0;

	for (; prefix[at]; ++at)
		text[at] = (WCHAR)prefix[at];
	text[at++] = (WCHAR)('A' + number % 26);
	text[at++] = (WCHAR)('a' + number / 26 % 26);
	text[at] = 0;
}

static element *add(volume *vol, element *storage, const char *prefix, unsigned number, BYTE kind, SIZE_T size)
{
	WCHAR text[32];
	element *item;
	BYTE *data;
	ULONG done;

	name(text, prefix, number);
	if (!(item = element_new(text, kind)))
		abort();
	element_insert(storage, item);
	if (kind != KIND_STREAM || !size)
		return item;
	if (!(data = malloc(size)))
		abort();
	fill(data, size, number);
	if (FAILED(element_write(vol, item, 0, data, (ULONG)size, &done)) || done != size)
		abort();
	free(data);
	return item;
}

/* A file with small and large streams at the root and in nested storages, and thirty entries more for a directory of several sectors. */
static void seed(ILockBytes *bytes, DWORD sector_size)
{
	volume *vol;
	element *inner, *deeper;

	if (FAILED(volume_create(bytes, sector_size, &vol)))
		abort();
	add(vol, vol->root, "small", 1, KIND_STREAM, 100);
	add(vol, vol->root, "edge", 2, KIND_STREAM, 4095);
	add(vol, vol->root, "large", 3, KIND_STREAM, 20000);
	add(vol, vol->root, "empty", 4, KIND_STREAM, 0);
	inner = add(vol, vol->root, "storage", 5, KIND_STORAGE, 0);
	add(vol, inner, "inner", 6, KIND_STREAM, 300);
	add(vol, inner, "inner", 7, KIND_STREAM, 9000);
	deeper = add(vol, inner, "deeper", 8, KIND_STORAGE, 0);
	add(vol, deeper, "leaf", 9, KIND_STREAM, 70);
	for (unsigned number = 10; number < 40; ++number)
		add(vol, vol->root, "entry", number, KIND_STREAM, number * 13);
	if (FAILED(volume_flush(vol)))
		abort();
	volume_release(vol);
}

static ULONGLONG walk(volume *vol, element *storage, int depth)
{
	static BYTE buffer[3000];
	ULONGLONG total = 0;

	if (depth > 64)
		return 0;
	for (element *child = storage->first_child; child; child = child->next) {
		if (child->kind == KIND_STREAM) {
			for (ULONGLONG offset = 0; offset < child->size && offset < (1u << 22); offset += sizeof(buffer)) {
				ULONG done = 0;
				if (FAILED(element_read(vol, child, offset, buffer, sizeof(buffer), &done)))
					break;
				total += done;
			}
		} else
			total += walk(vol, child, depth + 1);
	}
	return total;
}

static element *first_stream(element *storage)
{
	for (element *child = storage->first_child; child; child = child->next)
		if (child->kind == KIND_STREAM)
			return child;
	return NULL;
}

/* Where a change does the most: the header, the start of each sector (allocation tables and directory entries are laid out from there),
   and anywhere else now and then. */
static SIZE_T place(SIZE_T size, DWORD sector)
{
	const uint64_t pick = next();

	if (pick % 4 == 0)
		return (SIZE_T)(next() % (size < 512 ? size : 512));
	if (pick % 4 == 1) {
		const SIZE_T sectors = size / sector;
		return sectors ? (SIZE_T)(next() % sectors * sector + next() % 128) % size : 0;
	}
	return (SIZE_T)(next() % size);
}

static void change(ILockBytes *bytes, DWORD sector)
{
	static const DWORD special[] = {0xffffffffu, 0xfffffffeu, 0xfffffffdu, 0xfffffffcu, 0xfffffff0u, 0, 1, 2, 0x7fffffffu, 0x10000u};
	const unsigned changes = 1 + (unsigned)(next() % 8);

	for (unsigned count = 0; count < changes && bytes->size >= 4; ++count) {
		SIZE_T at = place(bytes->size, sector);
		switch (next() % 5) {
		case 0:
			bytes->data[at] ^= (BYTE)(1u << (next() % 8));
			break;
		case 1:
			bytes->data[at] = (BYTE)next();
			break;
		case 2:
			at &= ~(SIZE_T)3;
			if (at + 4 <= bytes->size) {
				const DWORD value = next() % 2 ? special[next() % (sizeof(special) / sizeof(*special))] : (DWORD)(next() % 256);
				memcpy(bytes->data + at, &value, 4);
			}
			break;
		case 3:
			bytes->size -= (SIZE_T)(next() % (bytes->size / 4 + 1));
			break;
		default:
			if (bytes->size + sector <= LARGEST) {
				BYTE *grown = realloc(bytes->data, bytes->size + sector);
				if (!grown)
					abort();
				memset(grown + bytes->size, (int)(next() & 0xff), sector);
				bytes->data = grown;
				bytes->size += sector;
			}
		}
	}
}

int main(int argc, char **argv)
{
	const unsigned long rounds = argc > 1 ? strtoul(argv[1], NULL, 10) : 20000;
	ILockBytes seeds[2] = {{0}};
	const DWORD sectors[2] = {512, 4096};
	unsigned long opened = 0, flushed = 0;

	if (argc > 2)
		state ^= strtoull(argv[2], NULL, 10) * 0x2545f4914f6cdd1dull;
	for (int which = 0; which < 2; ++which) {
		volume *vol;
		seed(&seeds[which], sectors[which]);
		/* The seed itself reads back whole. */
		if (FAILED(volume_load(&seeds[which], FALSE, &vol)) || !walk(vol, vol->root, 0))
			return 1;
		volume_release(vol);
	}
	for (unsigned long round = 0; round < rounds; ++round) {
		const int which = (int)(next() % 2);
		ILockBytes file = {0};
		volume *vol;
		file.size = seeds[which].size;
		if (!(file.data = malloc(file.size)))
			abort();
		memcpy(file.data, seeds[which].data, file.size);
		change(&file, sectors[which]);
		const BOOL writable = next() % 2;
		if (SUCCEEDED(volume_load(&file, writable, &vol))) {
			++opened;
			walk(vol, vol->root, 0);
			element *stream = first_stream(vol->root);
			if (writable && stream) {
				static BYTE data[6000];
				ULONG done;
				fill(data, sizeof(data), (unsigned)round);
				if (SUCCEEDED(element_write(vol, stream, next() % 8192, data, (ULONG)(next() % sizeof(data)), &done)) &&
					SUCCEEDED(element_resize(vol, stream, next() % 12000)) && SUCCEEDED(volume_flush(vol)))
					++flushed;
			}
			volume_release(vol);
		}
		free(file.data);
	}
	printf("%lu rounds, %lu files opened, %lu changed and written back\n", rounds, opened, flushed);
	free(seeds[0].data);
	free(seeds[1].data);
	return 0;
}
