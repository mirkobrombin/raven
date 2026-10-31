/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "oa.h"

/* Sixteen bytes before a descriptor hold what its features promise: a GUID, the variant type in
 * the last four bytes, or the record information in the last pointer. */
#define HIDDEN_BYTES 16
#define FADF_DATADELETED 0x1000
#define FADF_CREATEVECTOR 0x2000
#define OWN_FEATURES (FADF_BSTR | FADF_UNKNOWN | FADF_DISPATCH | FADF_VARIANT | FADF_HAVEIID | FADF_HAVEVARTYPE | FADF_RECORD)

static inline BYTE *hidden_of(SAFEARRAY *array)
{
	return (BYTE *)array - HIDDEN_BYTES;
}

static inline DWORD *vartype_slot(SAFEARRAY *array)
{
	return (DWORD *)array - 1;
}

static inline IRecordInfo **record_slot(SAFEARRAY *array)
{
	return (IRecordInfo **)array - 1;
}

static inline GUID *iid_slot(SAFEARRAY *array)
{
	return (GUID *)hidden_of(array);
}

static SIZE_T descriptor_bytes(UINT dims)
{
	return sizeof(SAFEARRAY) + (dims ? dims - 1 : 0) * sizeof(SAFEARRAYBOUND);
}

ULONG oa_vartype_size(VARTYPE type)
{
	switch (type)
	{
	case VT_I1: case VT_UI1:
		return 1;
	case VT_BOOL: case VT_I2: case VT_UI2:
		return 2;
	case VT_I4: case VT_UI4: case VT_R4: case VT_ERROR: case VT_INT: case VT_UINT:
		return 4;
	case VT_BSTR: case VT_DISPATCH: case VT_UNKNOWN:
		return sizeof(void *);
	case VT_R8: case VT_CY: case VT_DATE: case VT_I8: case VT_UI8:
		return 8;
	case VT_INT_PTR: case VT_UINT_PTR:
		return sizeof(void *);
	case VT_RECORD:
		return 32;
	case VT_VARIANT:
		return sizeof(VARIANT);
	case VT_DECIMAL:
		return sizeof(DECIMAL);
	default:
		return 0;
	}
}

static ULONG cell_count(const SAFEARRAY *array)
{
	ULONGLONG cells = 1;
	USHORT dim;
	for (dim = 0; dim < array->cDims; dim++)
		cells *= array->rgsabound[dim].cElements;
	return cells > 0xffffffffu ? 0xffffffffu : (ULONG)cells;
}

HRESULT WINAPI SafeArrayAllocDescriptor(UINT dims, SAFEARRAY **out)
{
	SIZE_T bytes;
	BYTE *block;
	if (!out)
		return E_POINTER;
	if (!dims || dims >= 0x10000)
		return E_INVALIDARG;
	bytes = HIDDEN_BYTES + descriptor_bytes(dims);
	block = CoTaskMemAlloc(bytes);
	if (!block)
		return E_OUTOFMEMORY;
	memset(block, 0, bytes);
	*out = (SAFEARRAY *)(block + HIDDEN_BYTES);
	(*out)->cDims = (USHORT)dims;
	return S_OK;
}

static HRESULT mark_type(SAFEARRAY *array, VARTYPE type)
{
	switch (type)
	{
	case VT_UNKNOWN:
		array->fFeatures |= FADF_HAVEIID;
		*iid_slot(array) = IID_IUnknown;
		break;
	case VT_DISPATCH:
		array->fFeatures |= FADF_HAVEIID;
		*iid_slot(array) = IID_IDispatch;
		break;
	case VT_RECORD:
		array->fFeatures |= FADF_RECORD;
		break;
	default:
		array->fFeatures |= FADF_HAVEVARTYPE;
		*vartype_slot(array) = type;
		break;
	}
	return S_OK;
}

/* A bare descriptor only says its type; an array made whole also knows what its cells own. */
static void mark_contents(SAFEARRAY *array, VARTYPE type)
{
	switch (type)
	{
	case VT_BSTR:
		array->fFeatures |= FADF_BSTR;
		break;
	case VT_UNKNOWN:
		array->fFeatures |= FADF_UNKNOWN;
		break;
	case VT_DISPATCH:
		array->fFeatures |= FADF_DISPATCH;
		break;
	case VT_VARIANT:
		array->fFeatures |= FADF_VARIANT;
		break;
	default:
		break;
	}
}

HRESULT WINAPI SafeArrayAllocDescriptorEx(VARTYPE type, UINT dims, SAFEARRAY **out)
{
	HRESULT result = SafeArrayAllocDescriptor(dims, out);
	if (FAILED(result))
		return result;
	(*out)->cbElements = oa_vartype_size(type);
	return mark_type(*out, type);
}

HRESULT WINAPI SafeArrayAllocData(SAFEARRAY *array)
{
	ULONGLONG bytes;
	if (!array)
		return E_INVALIDARG;
	bytes = (ULONGLONG)cell_count(array) * array->cbElements;
	if (bytes >= 0x7fffffffu)
		return E_OUTOFMEMORY;
	array->pvData = CoTaskMemAlloc((SIZE_T)bytes ? (SIZE_T)bytes : 1);
	if (!array->pvData)
		return E_OUTOFMEMORY;
	memset(array->pvData, 0, (SIZE_T)bytes);
	return S_OK;
}

static SAFEARRAY *create_with(VARTYPE type, UINT dims, const SAFEARRAYBOUND *bounds, ULONG size, void *extra)
{
	SAFEARRAY *array;
	UINT dim;
	if (!bounds || !dims || (!size && !oa_vartype_size(type)))
		return NULL;
	if (FAILED(SafeArrayAllocDescriptorEx(type, dims, &array)))
		return NULL;
	if (size)
		array->cbElements = size;
	mark_contents(array, type);
	/* Bounds are stored last dimension first. */
	for (dim = 0; dim < dims; dim++)
		array->rgsabound[dim] = bounds[dims - dim - 1];
	if (type == VT_RECORD && extra)
	{
		if (FAILED(SafeArraySetRecordInfo(array, extra)))
		{
			SafeArrayDestroyDescriptor(array);
			return NULL;
		}
	}
	if (FAILED(SafeArrayAllocData(array)))
	{
		SafeArrayDestroyDescriptor(array);
		return NULL;
	}
	return array;
}

SAFEARRAY *WINAPI SafeArrayCreate(VARTYPE type, UINT dims, SAFEARRAYBOUND *bounds)
{
	if (type == VT_RECORD)
		return NULL;
	return create_with(type, dims, bounds, 0, NULL);
}

SAFEARRAY *WINAPI SafeArrayCreateEx(VARTYPE type, UINT dims, SAFEARRAYBOUND *bounds, void *extra)
{
	ULONG size = 0;
	SAFEARRAY *array;
	if (type == VT_RECORD)
	{
		if (!extra)
			return NULL;
		/* A record that cannot say its size still makes an array. */
		IRecordInfo_GetSize((IRecordInfo *)extra, &size);
		return create_with(type, dims, bounds, size, extra);
	}
	array = create_with(type, dims, bounds, 0, NULL);
	if (array && extra && array->fFeatures & FADF_HAVEIID)
		SafeArraySetIID(array, extra);
	return array;
}

/* A vector keeps descriptor and data in one block. */
static SAFEARRAY *create_vector(VARTYPE type, LONG lower, ULONG cells, ULONG size)
{
	SAFEARRAY *array;
	ULONGLONG data = (ULONGLONG)cells * size;
	BYTE *block;
	SIZE_T lead = HIDDEN_BYTES + sizeof(SAFEARRAY);
	if (data > 0x7fffff00u)
		return NULL;
	block = CoTaskMemAlloc(lead + (SIZE_T)data);
	if (!block)
		return NULL;
	memset(block, 0, lead + (SIZE_T)data);
	array = (SAFEARRAY *)(block + HIDDEN_BYTES);
	array->cDims = 1;
	array->cbElements = size;
	array->fFeatures = FADF_CREATEVECTOR;
	array->pvData = block + lead;
	array->rgsabound[0].cElements = cells;
	array->rgsabound[0].lLbound = lower;
	mark_type(array, type);
	mark_contents(array, type);
	return array;
}

SAFEARRAY *WINAPI SafeArrayCreateVector(VARTYPE type, LONG lower, ULONG cells)
{
	ULONG size = oa_vartype_size(type);
	if (!size || type == VT_RECORD)
		return NULL;
	return create_vector(type, lower, cells, size);
}

SAFEARRAY *WINAPI SafeArrayCreateVectorEx(VARTYPE type, LONG lower, ULONG cells, void *extra)
{
	ULONG size = oa_vartype_size(type);
	SAFEARRAY *array;
	if (type == VT_RECORD)
	{
		if (!extra || FAILED(IRecordInfo_GetSize((IRecordInfo *)extra, &size)))
			return NULL;
	}
	else if (!size)
		return NULL;
	array = create_vector(type, lower, cells, size);
	if (array && extra)
	{
		if (type == VT_RECORD)
			SafeArraySetRecordInfo(array, extra);
		else if (array->fFeatures & FADF_HAVEIID)
			SafeArraySetIID(array, extra);
	}
	return array;
}

HRESULT WINAPI SafeArrayDestroyDescriptor(SAFEARRAY *array)
{
	if (!array)
		return S_OK;
	if (array->cLocks)
		return DISP_E_ARRAYISLOCKED;
	if (array->fFeatures & FADF_RECORD && *record_slot(array))
		IRecordInfo_Release(*record_slot(array));
	CoTaskMemFree(hidden_of(array));
	return S_OK;
}

/* Empties the cells from the given one on, whatever kind of thing they own. */
static HRESULT clear_cells(SAFEARRAY *array, ULONG start, ULONG cells)
{
	BYTE *at;
	ULONG index;
	if (!array->pvData || !array->cbElements || !cells)
		return S_OK;
	at = (BYTE *)array->pvData + (SIZE_T)start * array->cbElements;
	for (index = 0; index < cells; index++, at += array->cbElements)
	{
		if (array->fFeatures & FADF_BSTR)
		{
			SysFreeString(*(BSTR *)at);
			*(BSTR *)at = NULL;
		}
		else if (array->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH))
		{
			if (*(IUnknown **)at)
				IUnknown_Release(*(IUnknown **)at);
			*(IUnknown **)at = NULL;
		}
		else if (array->fFeatures & FADF_VARIANT)
		{
			HRESULT result = VariantClear((VARIANT *)at);
			if (FAILED(result))
				return result;
		}
		else if (array->fFeatures & FADF_RECORD && *record_slot(array))
			IRecordInfo_RecordClear(*record_slot(array), at);
	}
	return S_OK;
}

HRESULT WINAPI SafeArrayDestroyData(SAFEARRAY *array)
{
	HRESULT result;
	if (!array)
		return E_INVALIDARG;
	if (array->cLocks)
		return DISP_E_ARRAYISLOCKED;
	result = clear_cells(array, 0, cell_count(array));
	if (FAILED(result))
		return result;
	if (array->pvData)
	{
		if (array->fFeatures & (FADF_STATIC | FADF_EMBEDDED | FADF_AUTO))
			memset(array->pvData, 0, (SIZE_T)cell_count(array) * array->cbElements);
		else if (array->fFeatures & FADF_CREATEVECTOR)
			array->fFeatures |= FADF_DATADELETED;
		else
		{
			CoTaskMemFree(array->pvData);
			array->pvData = NULL;
		}
	}
	return S_OK;
}

HRESULT WINAPI SafeArrayDestroy(SAFEARRAY *array)
{
	HRESULT result;
	if (!array)
		return S_OK;
	if (array->cLocks)
		return DISP_E_ARRAYISLOCKED;
	result = SafeArrayDestroyData(array);
	if (FAILED(result))
		return result;
	return SafeArrayDestroyDescriptor(array);
}

UINT WINAPI SafeArrayGetDim(SAFEARRAY *array)
{
	return array ? array->cDims : 0;
}

UINT WINAPI SafeArrayGetElemsize(SAFEARRAY *array)
{
	return array ? array->cbElements : 0;
}

HRESULT WINAPI SafeArrayGetLBound(SAFEARRAY *array, UINT dim, LONG *value)
{
	if (!array || !value)
		return E_INVALIDARG;
	if (!dim || dim > array->cDims)
		return DISP_E_BADINDEX;
	*value = array->rgsabound[array->cDims - dim].lLbound;
	return S_OK;
}

HRESULT WINAPI SafeArrayGetUBound(SAFEARRAY *array, UINT dim, LONG *value)
{
	const SAFEARRAYBOUND *bound;
	if (!array || !value)
		return E_INVALIDARG;
	if (!dim || dim > array->cDims)
		return DISP_E_BADINDEX;
	bound = &array->rgsabound[array->cDims - dim];
	*value = bound->lLbound + (LONG)bound->cElements - 1;
	return S_OK;
}

HRESULT WINAPI SafeArrayLock(SAFEARRAY *array)
{
	ULONG locks;
	if (!array)
		return E_INVALIDARG;
	locks = InterlockedIncrement((LONG *)&array->cLocks);
	if (locks > 0xffff)
	{
		InterlockedDecrement((LONG *)&array->cLocks);
		return E_UNEXPECTED;
	}
	return S_OK;
}

HRESULT WINAPI SafeArrayUnlock(SAFEARRAY *array)
{
	if (!array)
		return E_INVALIDARG;
	if (!array->cLocks)
		return E_UNEXPECTED;
	InterlockedDecrement((LONG *)&array->cLocks);
	return S_OK;
}

HRESULT WINAPI SafeArrayAccessData(SAFEARRAY *array, void **data)
{
	HRESULT result;
	if (!array || !data)
		return E_INVALIDARG;
	result = SafeArrayLock(array);
	*data = SUCCEEDED(result) ? array->pvData : NULL;
	return result;
}

HRESULT WINAPI SafeArrayUnaccessData(SAFEARRAY *array)
{
	return SafeArrayUnlock(array);
}

HRESULT WINAPI SafeArrayPtrOfIndex(SAFEARRAY *array, LONG *indices, void **out)
{
	ULONGLONG offset = 0, stride = 1;
	UINT dim;
	if (!array || !indices || !out)
		return E_INVALIDARG;
	if (!array->pvData)
		return E_INVALIDARG;
	/* The first index is the slowest moving one in the stored, reversed bounds. */
	for (dim = 0; dim < array->cDims; dim++)
	{
		const SAFEARRAYBOUND *bound = &array->rgsabound[array->cDims - dim - 1];
		LONG place = indices[dim] - bound->lLbound;
		if (indices[dim] < bound->lLbound || (ULONG)place >= bound->cElements)
			return DISP_E_BADINDEX;
		offset += stride * (ULONG)place;
		stride *= bound->cElements;
	}
	*out = (BYTE *)array->pvData + offset * array->cbElements;
	return S_OK;
}

HRESULT WINAPI SafeArrayGetElement(SAFEARRAY *array, LONG *indices, void *out)
{
	void *cell;
	HRESULT result;
	if (!array || !indices || !out)
		return E_INVALIDARG;
	result = SafeArrayLock(array);
	if (FAILED(result))
		return result;
	result = SafeArrayPtrOfIndex(array, indices, &cell);
	if (SUCCEEDED(result))
	{
		if (array->fFeatures & FADF_VARIANT)
		{
			VariantInit(out);
			result = VariantCopy(out, cell);
		}
		else if (array->fFeatures & FADF_BSTR)
		{
			BSTR text = *(BSTR *)cell;
			*(BSTR *)out = NULL;
			if (text)
			{
				*(BSTR *)out = SysAllocStringLen(text, SysStringLen(text));
				if (!*(BSTR *)out)
					result = E_OUTOFMEMORY;
			}
		}
		else if (array->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH))
		{
			IUnknown *object = *(IUnknown **)cell;
			if (object)
				IUnknown_AddRef(object);
			*(IUnknown **)out = object;
		}
		else if (array->fFeatures & FADF_RECORD)
			result = IRecordInfo_RecordCopy(*record_slot(array), cell, out);
		else
			memcpy(out, cell, array->cbElements);
	}
	SafeArrayUnlock(array);
	return result;
}

HRESULT WINAPI SafeArrayPutElement(SAFEARRAY *array, LONG *indices, void *value)
{
	void *cell;
	HRESULT result;
	if (!array || !indices)
		return E_INVALIDARG;
	if (!value && !(array->fFeatures & (FADF_BSTR | FADF_UNKNOWN | FADF_DISPATCH | FADF_RECORD)))
		return E_INVALIDARG;
	result = SafeArrayLock(array);
	if (FAILED(result))
		return result;
	result = SafeArrayPtrOfIndex(array, indices, &cell);
	if (SUCCEEDED(result))
	{
		if (array->fFeatures & FADF_VARIANT)
		{
			VARIANT *slot = cell;
			result = VariantCopy(slot, value);
		}
		else if (array->fFeatures & FADF_BSTR)
		{
			BSTR copy = NULL;
			if (value)
			{
				copy = SysAllocStringLen(value, SysStringLen(value));
				if (!copy)
					result = E_OUTOFMEMORY;
			}
			if (SUCCEEDED(result))
			{
				SysFreeString(*(BSTR *)cell);
				*(BSTR *)cell = copy;
			}
		}
		else if (array->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH))
		{
			if (value)
				IUnknown_AddRef((IUnknown *)value);
			if (*(IUnknown **)cell)
				IUnknown_Release(*(IUnknown **)cell);
			*(IUnknown **)cell = value;
		}
		else if (array->fFeatures & FADF_RECORD)
			result = value ? IRecordInfo_RecordCopy(*record_slot(array), value, cell) : IRecordInfo_RecordClear(*record_slot(array), cell);
		else
			memcpy(cell, value, array->cbElements);
	}
	SafeArrayUnlock(array);
	return result;
}

/* Fills an already sized target with copies of what the source holds. */
static HRESULT copy_cells(SAFEARRAY *from, SAFEARRAY *to)
{
	ULONG cells = cell_count(from), index;
	BYTE *source = from->pvData, *target = to->pvData;
	HRESULT result = S_OK;
	if (!cells)
		return S_OK;
	if (!source || !target)
		return E_INVALIDARG;
	if (from->fFeatures & FADF_VARIANT)
	{
		for (index = 0; index < cells && SUCCEEDED(result); index++)
			result = VariantCopy((VARIANT *)target + index, (VARIANT *)source + index);
	}
	else if (from->fFeatures & FADF_BSTR)
	{
		for (index = 0; index < cells; index++)
		{
			BSTR text = ((BSTR *)source)[index], copy = NULL;
			if (text && !(copy = SysAllocStringByteLen((const char *)text, SysStringByteLen(text))))
				return E_OUTOFMEMORY;
			SysFreeString(((BSTR *)target)[index]);
			((BSTR *)target)[index] = copy;
		}
	}
	else if (from->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH))
	{
		for (index = 0; index < cells; index++)
		{
			IUnknown *object = ((IUnknown **)source)[index], *old = ((IUnknown **)target)[index];
			if (object)
				IUnknown_AddRef(object);
			if (old)
				IUnknown_Release(old);
			((IUnknown **)target)[index] = object;
		}
	}
	else if (from->fFeatures & FADF_RECORD)
	{
		for (index = 0; index < cells && SUCCEEDED(result); index++)
			result = IRecordInfo_RecordCopy(*record_slot(from), source + (SIZE_T)index * from->cbElements, target + (SIZE_T)index * from->cbElements);
	}
	else
		memcpy(target, source, (SIZE_T)cells * from->cbElements);
	return result;
}

HRESULT WINAPI SafeArrayCopyData(SAFEARRAY *from, SAFEARRAY *to)
{
	UINT dim;
	if (!from || !to || from->cDims != to->cDims || from->cbElements != to->cbElements)
		return E_INVALIDARG;
	if (!from->pvData || !to->pvData)
		return E_INVALIDARG;
	for (dim = 0; dim < from->cDims; dim++)
		if (from->rgsabound[dim].cElements != to->rgsabound[dim].cElements)
			return E_INVALIDARG;
	if (cell_count(from) && (from->fFeatures & OWN_FEATURES & ~(FADF_HAVEIID | FADF_HAVEVARTYPE)) != (to->fFeatures & OWN_FEATURES & ~(FADF_HAVEIID | FADF_HAVEVARTYPE)))
		return E_INVALIDARG;
	if (!(to->fFeatures & FADF_RECORD))
		clear_cells(to, 0, cell_count(to));
	return copy_cells(from, to);
}

HRESULT WINAPI SafeArrayCopy(SAFEARRAY *from, SAFEARRAY **out)
{
	HRESULT result;
	VARTYPE type = 0;
	SAFEARRAY *copy;
	if (!from)
	{
		if (out)
			*out = NULL;
		return S_OK;
	}
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!from->cbElements)
		return E_INVALIDARG;
	if (SUCCEEDED(SafeArrayGetVartype(from, &type)))
		result = SafeArrayAllocDescriptorEx(type, from->cDims, &copy);
	else
		result = SafeArrayAllocDescriptor(from->cDims, &copy);
	if (FAILED(result))
		return result;
	/* The copy owns its memory whichever way the source was made. */
	copy->fFeatures = from->fFeatures & OWN_FEATURES;
	copy->cbElements = from->cbElements;
	memcpy(copy->rgsabound, from->rgsabound, from->cDims * sizeof(SAFEARRAYBOUND));
	memcpy(hidden_of(copy), hidden_of(from), HIDDEN_BYTES);
	if (copy->fFeatures & FADF_RECORD && *record_slot(copy))
		IRecordInfo_AddRef(*record_slot(copy));
	result = SafeArrayAllocData(copy);
	if (SUCCEEDED(result) && from->pvData)
		result = copy_cells(from, copy);
	if (FAILED(result))
	{
		SafeArrayDestroy(copy);
		return result;
	}
	*out = copy;
	return S_OK;
}

HRESULT WINAPI SafeArrayRedim(SAFEARRAY *array, SAFEARRAYBOUND *bound)
{
	SAFEARRAYBOUND *last;
	ULONGLONG slab, old_bytes, new_bytes;
	if (!array || !bound)
		return E_INVALIDARG;
	if (array->cLocks || array->fFeatures & (FADF_FIXEDSIZE | FADF_STATIC))
		return DISP_E_ARRAYISLOCKED;
	/* Only the slowest dimension, stored first, may change. */
	last = &array->rgsabound[0];
	last->lLbound = bound->lLbound;
	if (bound->cElements == last->cElements)
		return S_OK;
	slab = (ULONGLONG)array->cbElements;
	{
		UINT dim;
		for (dim = 1; dim < array->cDims; dim++)
			slab *= array->rgsabound[dim].cElements;
	}
	old_bytes = slab * last->cElements;
	new_bytes = slab * bound->cElements;
	if (new_bytes >= 0x7fffffffu)
		return E_OUTOFMEMORY;
	if (bound->cElements < last->cElements)
	{
		ULONG per = (ULONG)(slab / (array->cbElements ? array->cbElements : 1));
		HRESULT result = clear_cells(array, per * bound->cElements, per * (last->cElements - bound->cElements));
		if (FAILED(result))
			return result;
	}
	else
	{
		void *data = array->pvData ? CoTaskMemRealloc(array->pvData, (SIZE_T)new_bytes) : CoTaskMemAlloc((SIZE_T)new_bytes);
		if (!data)
			return E_OUTOFMEMORY;
		array->pvData = data;
		memset((BYTE *)data + old_bytes, 0, (SIZE_T)(new_bytes - old_bytes));
	}
	last->cElements = bound->cElements;
	return S_OK;
}

HRESULT WINAPI SafeArrayGetVartype(SAFEARRAY *array, VARTYPE *type)
{
	if (!array || !type)
		return E_INVALIDARG;
	if (array->fFeatures & FADF_HAVEVARTYPE)
		*type = (VARTYPE)*vartype_slot(array);
	else if (array->fFeatures & FADF_RECORD)
		*type = VT_RECORD;
	else if (array->fFeatures & FADF_HAVEIID)
		*type = array->fFeatures & FADF_DISPATCH ? VT_DISPATCH : VT_UNKNOWN;
	else
		return E_INVALIDARG;
	return S_OK;
}

HRESULT WINAPI SafeArraySetIID(SAFEARRAY *array, REFGUID iid)
{
	if (!array || !iid || !(array->fFeatures & FADF_HAVEIID))
		return E_INVALIDARG;
	*iid_slot(array) = *iid;
	return S_OK;
}

HRESULT WINAPI SafeArrayGetIID(SAFEARRAY *array, GUID *iid)
{
	if (!array || !iid || !(array->fFeatures & FADF_HAVEIID))
		return E_INVALIDARG;
	*iid = *iid_slot(array);
	return S_OK;
}

HRESULT WINAPI SafeArraySetRecordInfo(SAFEARRAY *array, IRecordInfo *info)
{
	if (!array || !(array->fFeatures & FADF_RECORD))
		return E_INVALIDARG;
	if (info)
		IRecordInfo_AddRef(info);
	if (*record_slot(array))
		IRecordInfo_Release(*record_slot(array));
	*record_slot(array) = info;
	return S_OK;
}

HRESULT WINAPI SafeArrayGetRecordInfo(SAFEARRAY *array, IRecordInfo **info)
{
	if (!array || !info || !(array->fFeatures & FADF_RECORD))
		return E_INVALIDARG;
	*info = *record_slot(array);
	if (*info)
		IRecordInfo_AddRef(*info);
	return S_OK;
}

HRESULT WINAPI BstrFromVector(SAFEARRAY *array, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!array)
		return E_INVALIDARG;
	if (array->cDims != 1)
		return E_INVALIDARG;
	*out = SysAllocStringByteLen(array->pvData, array->rgsabound[0].cElements * array->cbElements);
	return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI VectorFromBstr(BSTR text, SAFEARRAY **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = SafeArrayCreateVector(VT_UI1, 0, SysStringByteLen(text));
	if (!*out)
		return E_OUTOFMEMORY;
	memcpy((*out)->pvData, text, SysStringByteLen(text));
	return S_OK;
}
