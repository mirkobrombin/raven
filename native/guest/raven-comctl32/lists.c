/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <commctrl.h>

/* The dynamic structure and pointer arrays. Programs read the count and the pointer array straight from the handle
 * through the DSA_GetItemCount, DPA_GetPtrCount and DPA_GetPtrPtr macros, which fixes where those two fields sit. */

#ifndef DEFAULT_GROW
#define DEFAULT_GROW 8
#endif

typedef struct _DSA {
	INT count;
	BYTE *items;
	INT capacity;
	INT item_size;
	INT grow;
} structure_array;

typedef struct _DPA {
	INT count;
	void **items;
	HANDLE heap;
	INT capacity;
	INT grow;
} pointer_array;

typedef struct {
	DWORD size;
	DWORD version;
	DWORD count;
} stream_header;

/* ---- structure arrays ---- */

static BOOL reserve_structures(structure_array *array, INT wanted)
{
	if (wanted <= array->capacity)
		return TRUE;
	INT capacity = (wanted + array->grow - 1) / array->grow * array->grow;
	SIZE_T bytes = (SIZE_T)capacity * array->item_size;
	BYTE *items = array->items ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, array->items, bytes)
				   : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
	if (!items)
		return FALSE;
	array->items = items;
	array->capacity = capacity;
	return TRUE;
}

HDSA WINAPI DSA_Create(INT item_size, INT grow)
{
	if (item_size <= 0)
		return NULL;
	structure_array *array = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*array));
	if (!array)
		return NULL;
	array->item_size = item_size;
	array->grow = grow > 0 ? grow : 1;
	return (HDSA)array;
}

BOOL WINAPI DSA_DeleteAllItems(HDSA handle)
{
	structure_array *array = (structure_array *)handle;
	if (!array)
		return FALSE;
	if (array->items)
		HeapFree(GetProcessHeap(), 0, array->items);
	array->items = NULL;
	array->count = array->capacity = 0;
	return TRUE;
}

BOOL WINAPI DSA_Destroy(HDSA handle)
{
	if (!handle)
		return TRUE;
	DSA_DeleteAllItems(handle);
	return HeapFree(GetProcessHeap(), 0, handle);
}

BOOL WINAPI DSA_GetItem(HDSA handle, INT index, LPVOID out)
{
	structure_array *array = (structure_array *)handle;
	if (!array || !out || index < 0 || index >= array->count)
		return FALSE;
	CopyMemory(out, array->items + (SIZE_T)index * array->item_size, array->item_size);
	return TRUE;
}

LPVOID WINAPI DSA_GetItemPtr(HDSA handle, INT index)
{
	structure_array *array = (structure_array *)handle;
	if (!array || index < 0 || index >= array->count)
		return NULL;
	return array->items + (SIZE_T)index * array->item_size;
}

BOOL WINAPI DSA_SetItem(HDSA handle, INT index, const void *item)
{
	structure_array *array = (structure_array *)handle;
	if (!array || !item || index < 0)
		return FALSE;
	if (index >= array->count) {
		if (!reserve_structures(array, index + 1))
			return FALSE;
		array->count = index + 1;
	}
	CopyMemory(array->items + (SIZE_T)index * array->item_size, item, array->item_size);
	return TRUE;
}

INT WINAPI DSA_InsertItem(HDSA handle, INT index, const void *item)
{
	structure_array *array = (structure_array *)handle;
	if (!array || !item || index < 0)
		return DSA_ERR;
	if (index > array->count)
		index = array->count;
	if (!reserve_structures(array, array->count + 1))
		return DSA_ERR;
	BYTE *slot = array->items + (SIZE_T)index * array->item_size;
	MoveMemory(slot + array->item_size, slot, (SIZE_T)(array->count - index) * array->item_size);
	CopyMemory(slot, item, array->item_size);
	array->count++;
	return index;
}

BOOL WINAPI DSA_DeleteItem(HDSA handle, INT index)
{
	structure_array *array = (structure_array *)handle;
	if (!array || index < 0 || index >= array->count)
		return FALSE;
	BYTE *slot = array->items + (SIZE_T)index * array->item_size;
	array->count--;
	MoveMemory(slot, slot + array->item_size, (SIZE_T)(array->count - index) * array->item_size);
	return TRUE;
}

VOID WINAPI DSA_EnumCallback(HDSA handle, PFNDSAENUMCALLBACK callback, LPVOID data)
{
	structure_array *array = (structure_array *)handle;
	if (!array || !callback)
		return;
	for (INT index = 0; index < array->count; ++index)
		if (!callback(array->items + (SIZE_T)index * array->item_size, data))
			return;
}

void WINAPI DSA_DestroyCallback(HDSA handle, PFNDSAENUMCALLBACK callback, LPVOID data)
{
	DSA_EnumCallback(handle, callback, data);
	DSA_Destroy(handle);
}

HDSA WINAPI DSA_Clone(HDSA handle)
{
	structure_array *array = (structure_array *)handle;
	if (!array)
		return NULL;
	structure_array *copy = (structure_array *)DSA_Create(array->item_size, array->grow);
	if (!copy)
		return NULL;
	if (array->count) {
		if (!reserve_structures(copy, array->count)) {
			DSA_Destroy((HDSA)copy);
			return NULL;
		}
		CopyMemory(copy->items, array->items, (SIZE_T)array->count * array->item_size);
		copy->count = array->count;
	}
	return (HDSA)copy;
}

ULONGLONG WINAPI DSA_GetSize(HDSA handle)
{
	structure_array *array = (structure_array *)handle;
	return array ? sizeof(*array) + (ULONGLONG)array->capacity * array->item_size : 0;
}

/* ---- pointer arrays ---- */

static BOOL reserve_pointers(pointer_array *array, INT wanted)
{
	if (wanted <= array->capacity)
		return TRUE;
	INT capacity = (wanted + array->grow - 1) / array->grow * array->grow;
	SIZE_T bytes = (SIZE_T)capacity * sizeof(void *);
	void **items = array->items ? HeapReAlloc(array->heap, HEAP_ZERO_MEMORY, array->items, bytes)
				    : HeapAlloc(array->heap, HEAP_ZERO_MEMORY, bytes);
	if (!items) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	array->items = items;
	array->capacity = capacity;
	return TRUE;
}

HDPA WINAPI DPA_CreateEx(INT grow, HANDLE heap)
{
	if (!heap)
		heap = GetProcessHeap();
	pointer_array *array = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(*array));
	if (!array)
		return NULL;
	array->heap = heap;
	array->grow = grow > 0 ? grow : DEFAULT_GROW;
	return (HDPA)array;
}

HDPA WINAPI DPA_Create(INT grow)
{
	return DPA_CreateEx(grow, NULL);
}

BOOL WINAPI DPA_DeleteAllPtrs(HDPA handle)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array)
		return FALSE;
	if (array->items)
		HeapFree(array->heap, 0, array->items);
	array->items = NULL;
	array->count = array->capacity = 0;
	return TRUE;
}

BOOL WINAPI DPA_Destroy(HDPA handle)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array)
		return TRUE;
	DPA_DeleteAllPtrs(handle);
	return HeapFree(array->heap, 0, array);
}

BOOL WINAPI DPA_Grow(HDPA handle, INT wanted)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array)
		return FALSE;
	/* The increment takes the size asked for, so later growth comes in steps at least that big. */
	if (wanted > array->grow)
		array->grow = wanted;
	return reserve_pointers(array, wanted);
}

LPVOID WINAPI DPA_GetPtr(HDPA handle, INT_PTR index)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array || !array->items || index < 0 || index >= array->count)
		return NULL;
	return array->items[index];
}

INT WINAPI DPA_GetPtrIndex(HDPA handle, LPCVOID pointer)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array || !array->items)
		return DPA_ERR;
	for (INT index = 0; index < array->count; ++index)
		if (array->items[index] == pointer)
			return index;
	return DPA_ERR;
}

BOOL WINAPI DPA_SetPtr(HDPA handle, INT index, LPVOID pointer)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array || index < 0)
		return FALSE;
	if (index >= array->count) {
		if (!reserve_pointers(array, index + 1))
			return FALSE;
		array->count = index + 1;
	}
	array->items[index] = pointer;
	return TRUE;
}

INT WINAPI DPA_InsertPtr(HDPA handle, INT index, LPVOID pointer)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array || index < 0)
		return DPA_ERR;
	if (index > array->count)
		index = array->count;
	if (!reserve_pointers(array, array->count + 1))
		return DPA_ERR;
	MoveMemory(array->items + index + 1, array->items + index, (SIZE_T)(array->count - index) * sizeof(void *));
	array->items[index] = pointer;
	array->count++;
	return index;
}

LPVOID WINAPI DPA_DeletePtr(HDPA handle, INT index)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array || !array->items || index < 0 || index >= array->count)
		return NULL;
	void *pointer = array->items[index];
	array->count--;
	MoveMemory(array->items + index, array->items + index + 1, (SIZE_T)(array->count - index) * sizeof(void *));
	return pointer;
}

/* Replaces what `target` holds when there is one; otherwise the copy is a new array on the source's heap. */
HDPA WINAPI DPA_Clone(const HDPA handle, HDPA target)
{
	const pointer_array *array = (const pointer_array *)handle;
	if (!array)
		return NULL;
	pointer_array *copy = (pointer_array *)target;
	if (copy)
		DPA_DeleteAllPtrs(target);
	else
		copy = (pointer_array *)DPA_CreateEx(array->grow, array->heap);
	if (!copy)
		return NULL;
	if (array->count) {
		if (!reserve_pointers(copy, array->count)) {
			if (!target)
				DPA_Destroy((HDPA)copy);
			return NULL;
		}
		CopyMemory(copy->items, array->items, (SIZE_T)array->count * sizeof(void *));
		copy->count = array->count;
	}
	return (HDPA)copy;
}

VOID WINAPI DPA_EnumCallback(HDPA handle, PFNDPAENUMCALLBACK callback, LPVOID data)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array || !callback)
		return;
	for (INT index = 0; index < array->count; ++index)
		if (!callback(array->items[index], data))
			return;
}

void WINAPI DPA_DestroyCallback(HDPA handle, PFNDPAENUMCALLBACK callback, LPVOID data)
{
	DPA_EnumCallback(handle, callback, data);
	DPA_Destroy(handle);
}

ULONGLONG WINAPI DPA_GetSize(HDPA handle)
{
	pointer_array *array = (pointer_array *)handle;
	return array ? sizeof(*array) + (ULONGLONG)array->capacity * sizeof(void *) : 0;
}

/* A merge sort, since items that compare equal have to keep their order. */
BOOL WINAPI DPA_Sort(HDPA handle, PFNDPACOMPARE compare, LPARAM parameter)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array || !compare)
		return FALSE;
	if (array->count < 2)
		return TRUE;
	void **scratch = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)array->count * sizeof(void *));
	if (!scratch)
		return FALSE;
	void **from = array->items, **to = scratch;
	for (INT width = 1; width < array->count; width *= 2) {
		for (INT low = 0; low < array->count; low += width * 2) {
			INT middle = low + width < array->count ? low + width : array->count;
			INT high = low + width * 2 < array->count ? low + width * 2 : array->count;
			INT left = low, right = middle, out = low;
			while (left < middle && right < high)
				to[out++] = compare(from[left], from[right], parameter) <= 0 ? from[left++] : from[right++];
			while (left < middle)
				to[out++] = from[left++];
			while (right < high)
				to[out++] = from[right++];
		}
		void **swap = from;
		from = to;
		to = swap;
	}
	if (from != array->items)
		CopyMemory(array->items, from, (SIZE_T)array->count * sizeof(void *));
	HeapFree(GetProcessHeap(), 0, scratch);
	return TRUE;
}

INT WINAPI DPA_Search(HDPA handle, LPVOID wanted, INT start, PFNDPACOMPARE compare, LPARAM parameter, UINT options)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array || !compare || !array->items)
		return DPA_ERR;
	if (!(options & DPAS_SORTED)) {
		for (INT index = start > 0 ? start : 0; index < array->count; ++index)
			if (!compare(wanted, array->items[index], parameter))
				return index;
		return DPA_ERR;
	}
	/* A sorted search always covers the whole array, whatever the start. */
	INT low = 0, high = array->count - 1;
	while (low <= high) {
		INT middle = low + (high - low) / 2;
		int order = compare(wanted, array->items[middle], parameter);
		if (!order)
			return middle;
		if (order < 0)
			high = middle - 1;
		else
			low = middle + 1;
	}
	return options & (DPAS_INSERTBEFORE | DPAS_INSERTAFTER) ? low : DPA_ERR;
}

/* Walks both sorted arrays once, building the result apart so the callbacks can still read the destination. */
BOOL WINAPI DPA_Merge(HDPA destination, HDPA source, DWORD flags, PFNDPACOMPARE compare, PFNDPAMERGE merge, LPARAM parameter)
{
	pointer_array *to = (pointer_array *)destination, *from = (pointer_array *)source;
	if (!to || !from || !compare || !merge)
		return FALSE;
	if (!(flags & DPAM_SORTED)) {
		DPA_Sort(destination, compare, parameter);
		DPA_Sort(source, compare, parameter);
	}
	INT capacity = to->count + from->count;
	if (!capacity)
		return TRUE;
	void **result = HeapAlloc(to->heap, 0, (SIZE_T)capacity * sizeof(void *));
	if (!result)
		return FALSE;
	INT kept = 0, left = 0, right = 0;
	BOOL succeeded = TRUE;
	while (succeeded && (left < to->count || right < from->count)) {
		int order = left >= to->count ? 1 : right >= from->count ? -1 : compare(to->items[left], from->items[right], parameter);
		if (!order) {
			void *merged = merge(DPAMM_MERGE, to->items[left], from->items[right], parameter);
			if (merged)
				result[kept++] = merged;
			else
				succeeded = FALSE;
			++left;
			++right;
		} else if (order < 0) {
			if (flags & DPAM_INTERSECT)
				merge(DPAMM_DELETE, to->items[left], NULL, parameter);
			else
				result[kept++] = to->items[left];
			++left;
		} else {
			if (flags & DPAM_UNION) {
				void *inserted = merge(DPAMM_INSERT, from->items[right], NULL, parameter);
				if (inserted)
					result[kept++] = inserted;
				else
					succeeded = FALSE;
			}
			++right;
		}
	}
	if (!succeeded) {
		HeapFree(to->heap, 0, result);
		return FALSE;
	}
	if (to->items)
		HeapFree(to->heap, 0, to->items);
	to->items = result;
	to->capacity = capacity;
	to->count = kept;
	return TRUE;
}

/* ---- streams ---- */

static HRESULT stream_position(IStream *stream, ULARGE_INTEGER *position)
{
	LARGE_INTEGER zero = {0};
	return IStream_Seek(stream, zero, STREAM_SEEK_CUR, position);
}

static void stream_rewind(IStream *stream, ULARGE_INTEGER position)
{
	LARGE_INTEGER target;
	target.QuadPart = (LONGLONG)position.QuadPart;
	IStream_Seek(stream, target, STREAM_SEEK_SET, NULL);
}

HRESULT WINAPI DPA_LoadStream(HDPA *out, PFNDPASTREAM load, IStream *stream, LPVOID data)
{
	if (!out || !load || !stream)
		return E_INVALIDARG;
	*out = NULL;
	ULARGE_INTEGER start;
	HRESULT result = stream_position(stream, &start);
	if (FAILED(result))
		return result;
	stream_header header;
	ULONG got = 0;
	result = IStream_Read(stream, &header, sizeof(header), &got);
	if (FAILED(result) || got != sizeof(header) || header.size < sizeof(header) || header.version != 1) {
		stream_rewind(stream, start);
		return E_FAIL;
	}
	HDPA array = DPA_Create(header.count ? header.count : 1);
	if (!array)
		return E_OUTOFMEMORY;
	if (header.count && !DPA_Grow(array, header.count)) {
		DPA_Destroy(array);
		return E_OUTOFMEMORY;
	}
	result = S_OK;
	for (DWORD index = 0; index < header.count && result == S_OK; ++index) {
		DPASTREAMINFO info = {(INT)index, NULL};
		result = load(&info, stream, data);
		if (result == S_OK && DPA_InsertPtr(array, (INT)index, info.pvItem) == DPA_ERR)
			result = E_OUTOFMEMORY;
	}
	if (FAILED(result)) {
		DPA_Destroy(array);
		return result;
	}
	*out = array;
	return result;
}

/* The header goes first with the planned count and is written again if the callback stopped early. */
HRESULT WINAPI DPA_SaveStream(HDPA handle, PFNDPASTREAM save, IStream *stream, LPVOID data)
{
	pointer_array *array = (pointer_array *)handle;
	if (!array || !save || !stream)
		return E_INVALIDARG;
	ULARGE_INTEGER start, end;
	HRESULT result = stream_position(stream, &start);
	if (FAILED(result))
		return result;
	stream_header header = {sizeof(header), 1, (DWORD)array->count};
	result = IStream_Write(stream, &header, sizeof(header), NULL);
	if (FAILED(result))
		return result;
	DWORD saved = 0;
	HRESULT step = S_OK;
	for (INT index = 0; index < array->count; ++index) {
		DPASTREAMINFO info = {index, array->items[index]};
		step = save(&info, stream, data);
		if (step != S_OK)
			break;
		++saved;
	}
	if (FAILED(step)) {
		stream_rewind(stream, start);
		return step;
	}
	result = stream_position(stream, &end);
	if (FAILED(result))
		return result;
	header.size = (DWORD)(end.QuadPart - start.QuadPart);
	header.count = saved;
	stream_rewind(stream, start);
	result = IStream_Write(stream, &header, sizeof(header), NULL);
	stream_rewind(stream, end);
	return FAILED(result) ? result : step;
}
