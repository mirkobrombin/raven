/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * IRecordInfo: user defined types driven by their description.
 */
#include "oa.h"

struct record_field
{
	VARTYPE type;
	ULONG offset;
	BSTR name;
	VARKIND kind;
};

struct record_info
{
	IRecordInfo IRecordInfo_iface;
	LONG ref;
	ULONG size;
	GUID guid;
	BSTR name;
	ITypeInfo *info;
	struct record_field *fields;
	WORD count;
};

static inline struct record_info *record_of(IRecordInfo *iface)
{
	return CONTAINING_RECORD(iface, struct record_info, IRecordInfo_iface);
}

/* Fields store raw values: a variant is laid over the field only for the time of a conversion. */
static void field_to_variant(VARTYPE type, void *field, VARIANT *out)
{
	V_VT(out) = type;
	switch (type)
	{
	case VT_I1:
	case VT_UI1:
		V_UI1(out) = *(BYTE *)field;
		break;
	case VT_I2:
	case VT_UI2:
	case VT_BOOL:
		V_UI2(out) = *(USHORT *)field;
		break;
	case VT_I4:
	case VT_UI4:
	case VT_INT:
	case VT_UINT:
	case VT_ERROR:
	case VT_HRESULT:
	case VT_R4:
		V_UI4(out) = *(ULONG *)field;
		break;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_DATE:
	case VT_CY:
		V_UI8(out) = *(ULONG64 *)field;
		break;
	case VT_BSTR:
	case VT_DISPATCH:
	case VT_UNKNOWN:
	case VT_LPSTR:
	case VT_LPWSTR:
	case VT_PTR:
		V_BYREF(out) = *(void **)field;
		break;
	case VT_DECIMAL:
		V_DECIMAL(out) = *(DECIMAL *)field;
		V_VT(out) = type;
		break;
	default:
		V_VT(out) = VT_EMPTY;
		break;
	}
}

static HRESULT variant_to_field(VARTYPE type, VARIANT *from, void *field)
{
	switch (type)
	{
	case VT_I1:
	case VT_UI1:
		*(BYTE *)field = V_UI1(from);
		break;
	case VT_I2:
	case VT_UI2:
	case VT_BOOL:
		*(USHORT *)field = V_UI2(from);
		break;
	case VT_I4:
	case VT_UI4:
	case VT_INT:
	case VT_UINT:
	case VT_ERROR:
	case VT_HRESULT:
	case VT_R4:
		*(ULONG *)field = V_UI4(from);
		break;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_DATE:
	case VT_CY:
		*(ULONG64 *)field = V_UI8(from);
		break;
	case VT_BSTR:
	case VT_DISPATCH:
	case VT_UNKNOWN:
	case VT_LPSTR:
	case VT_LPWSTR:
	case VT_PTR:
		*(void **)field = V_BYREF(from);
		break;
	case VT_DECIMAL:
		*(DECIMAL *)field = V_DECIMAL(from);
		break;
	default:
		return DISP_E_BADVARTYPE;
	}
	return S_OK;
}

static void *field_byref(VARTYPE type, void *field)
{
	/* Most by-reference variants point at the value itself. */
	return field;
}

static HRESULT WINAPI record_QueryInterface(IRecordInfo *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IRecordInfo))
		return E_NOINTERFACE;
	*out = iface;
	IRecordInfo_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI record_AddRef(IRecordInfo *iface)
{
	return InterlockedIncrement(&record_of(iface)->ref);
}

static ULONG WINAPI record_Release(IRecordInfo *iface)
{
	struct record_info *record = record_of(iface);
	ULONG count = InterlockedDecrement(&record->ref);
	if (!count)
	{
		WORD index;
		for (index = 0; index < record->count; index++)
			SysFreeString(record->fields[index].name);
		SysFreeString(record->name);
		ITypeInfo_Release(record->info);
		HeapFree(GetProcessHeap(), 0, record->fields);
		HeapFree(GetProcessHeap(), 0, record);
	}
	return count;
}

static HRESULT WINAPI record_RecordInit(IRecordInfo *iface, void *data)
{
	struct record_info *record = record_of(iface);
	if (!data)
		return E_INVALIDARG;
	memset(data, 0, record->size);
	return S_OK;
}

static HRESULT WINAPI record_RecordClear(IRecordInfo *iface, void *data)
{
	struct record_info *record = record_of(iface);
	WORD index;
	if (!data)
		return E_INVALIDARG;
	for (index = 0; index < record->count; index++)
	{
		const struct record_field *field = &record->fields[index];
		void *place = (BYTE *)data + field->offset;
		if (field->kind != VAR_PERINSTANCE)
			continue;
		switch (field->type)
		{
		case VT_BSTR:
			SysFreeString(*(BSTR *)place);
			*(BSTR *)place = NULL;
			break;
		case VT_DISPATCH:
		case VT_UNKNOWN:
			if (*(IUnknown **)place)
				IUnknown_Release(*(IUnknown **)place);
			*(IUnknown **)place = NULL;
			break;
		case VT_VARIANT:
			VariantClear((VARIANT *)place);
			break;
		case VT_SAFEARRAY:
			if (*(SAFEARRAY **)place)
				SafeArrayDestroy(*(SAFEARRAY **)place);
			*(SAFEARRAY **)place = NULL;
			break;
		default:
			break;
		}
	}
	return S_OK;
}

static HRESULT WINAPI record_RecordCopy(IRecordInfo *iface, void *from, void *to)
{
	struct record_info *record = record_of(iface);
	WORD index;
	HRESULT status = S_OK;
	if (!from || !to)
		return E_INVALIDARG;
	memcpy(to, from, record->size);
	for (index = 0; index < record->count && SUCCEEDED(status); index++)
	{
		const struct record_field *field = &record->fields[index];
		void *place = (BYTE *)to + field->offset, *source = (BYTE *)from + field->offset;
		if (field->kind != VAR_PERINSTANCE)
			continue;
		switch (field->type)
		{
		case VT_BSTR:
			if (*(BSTR *)source)
			{
				*(BSTR *)place = SysAllocStringByteLen((const char *)*(BSTR *)source, SysStringByteLen(*(BSTR *)source));
				if (!*(BSTR *)place)
					status = E_OUTOFMEMORY;
			}
			break;
		case VT_DISPATCH:
		case VT_UNKNOWN:
			if (*(IUnknown **)place)
				IUnknown_AddRef(*(IUnknown **)place);
			break;
		case VT_VARIANT:
			VariantInit((VARIANT *)place);
			status = VariantCopy((VARIANT *)place, (VARIANT *)source);
			break;
		case VT_SAFEARRAY:
			if (*(SAFEARRAY **)source)
				status = SafeArrayCopy(*(SAFEARRAY **)source, (SAFEARRAY **)place);
			break;
		default:
			break;
		}
	}
	return status;
}

static HRESULT WINAPI record_GetGuid(IRecordInfo *iface, GUID *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = record_of(iface)->guid;
	return S_OK;
}

static HRESULT WINAPI record_GetName(IRecordInfo *iface, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = SysAllocString(record_of(iface)->name);
	return S_OK;
}

static HRESULT WINAPI record_GetSize(IRecordInfo *iface, ULONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = record_of(iface)->size;
	return S_OK;
}

static HRESULT WINAPI record_GetTypeInfo(IRecordInfo *iface, ITypeInfo **out)
{
	struct record_info *record = record_of(iface);
	if (!out)
		return E_INVALIDARG;
	*out = record->info;
	ITypeInfo_AddRef(*out);
	return S_OK;
}

static const struct record_field *find_field(const struct record_info *record, LPCOLESTR name)
{
	WORD index;
	if (!name)
		return NULL;
	for (index = 0; index < record->count; index++)
		if (record->fields[index].name && !lstrcmpW(record->fields[index].name, name))
			return &record->fields[index];
	return NULL;
}

static HRESULT WINAPI record_GetField(IRecordInfo *iface, void *data, LPCOLESTR name, VARIANT *out)
{
	struct record_info *record = record_of(iface);
	const struct record_field *field;
	VARIANT local;
	if (!data || !out || !name)
		return E_INVALIDARG;
	field = find_field(record, name);
	if (!field)
		return TYPE_E_FIELDNOTFOUND;
	VariantClear(out);
	if (field->type == VT_VARIANT)
		return VariantCopy(out, (VARIANT *)((BYTE *)data + field->offset));
	field_to_variant(field->type, (BYTE *)data + field->offset, &local);
	return VariantCopy(out, &local);
}

static HRESULT WINAPI record_GetFieldNoCopy(IRecordInfo *iface, void *data, LPCOLESTR name, VARIANT *out, void **place)
{
	struct record_info *record = record_of(iface);
	const struct record_field *field;
	if (!data || !out || !name || !place)
		return E_INVALIDARG;
	field = find_field(record, name);
	if (!field)
		return TYPE_E_FIELDNOTFOUND;
	VariantClear(out);
	V_VT(out) = field->type | VT_BYREF;
	V_BYREF(out) = field_byref(field->type, (BYTE *)data + field->offset);
	*place = (BYTE *)data + field->offset;
	return S_OK;
}

static HRESULT WINAPI record_PutField(IRecordInfo *iface, ULONG flags, void *data, LPCOLESTR name, VARIANT *in)
{
	struct record_info *record = record_of(iface);
	const struct record_field *field;
	VARIANT converted;
	HRESULT status;
	if (!data || !in || !name)
		return E_INVALIDARG;
	if (flags != INVOKE_PROPERTYPUT && flags != INVOKE_PROPERTYPUTREF)
		return E_INVALIDARG;
	field = find_field(record, name);
	if (!field)
		return TYPE_E_FIELDNOTFOUND;
	if (field->type == VT_VARIANT)
	{
		VariantClear((VARIANT *)((BYTE *)data + field->offset));
		return VariantCopy((VARIANT *)((BYTE *)data + field->offset), in);
	}
	VariantInit(&converted);
	status = VariantChangeType(&converted, in, 0, field->type);
	if (FAILED(status))
		return status;
	{
		/* Releases the old value through the same rule a clear would use. */
		VARIANT old;
		field_to_variant(field->type, (BYTE *)data + field->offset, &old);
		if (field->type == VT_BSTR || field->type == VT_DISPATCH || field->type == VT_UNKNOWN)
			VariantClear(&old);
	}
	return variant_to_field(field->type, &converted, (BYTE *)data + field->offset);
}

static HRESULT WINAPI record_PutFieldNoCopy(IRecordInfo *iface, ULONG flags, void *data, LPCOLESTR name, VARIANT *in)
{
	struct record_info *record = record_of(iface);
	const struct record_field *field;
	if (!data || !in || !name)
		return E_INVALIDARG;
	if (flags != INVOKE_PROPERTYPUT && flags != INVOKE_PROPERTYPUTREF)
		return E_INVALIDARG;
	field = find_field(record, name);
	if (!field)
		return TYPE_E_FIELDNOTFOUND;
	if (field->type == VT_VARIANT)
	{
		*(VARIANT *)((BYTE *)data + field->offset) = *in;
		return S_OK;
	}
	if (V_VT(in) != field->type)
		return DISP_E_TYPEMISMATCH;
	return variant_to_field(field->type, in, (BYTE *)data + field->offset);
}

static HRESULT WINAPI record_GetFieldNames(IRecordInfo *iface, ULONG *count, BSTR *names)
{
	struct record_info *record = record_of(iface);
	ULONG index, limit;
	if (!count)
		return E_INVALIDARG;
	if (!names)
	{
		*count = record->count;
		return S_OK;
	}
	limit = *count < record->count ? *count : record->count;
	for (index = 0; index < limit; index++)
		names[index] = SysAllocString(record->fields[index].name);
	*count = limit;
	return S_OK;
}

static BOOL WINAPI record_IsMatchingType(IRecordInfo *iface, IRecordInfo *other)
{
	struct record_info *record = record_of(iface);
	GUID guid;
	return other && (iface == other || (SUCCEEDED(IRecordInfo_GetGuid(other, &guid)) && IsEqualGUID(&guid, &record->guid)));
}

static void *WINAPI record_RecordCreate(IRecordInfo *iface)
{
	struct record_info *record = record_of(iface);
	void *data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, record->size);
	if (data)
		IRecordInfo_RecordInit(iface, data);
	return data;
}

static HRESULT WINAPI record_RecordCreateCopy(IRecordInfo *iface, void *from, void **out)
{
	HRESULT status;
	if (!from || !out)
		return E_INVALIDARG;
	*out = record_RecordCreate(iface);
	if (!*out)
		return E_OUTOFMEMORY;
	status = IRecordInfo_RecordCopy(iface, from, *out);
	if (FAILED(status))
	{
		HeapFree(GetProcessHeap(), 0, *out);
		*out = NULL;
	}
	return status;
}

static HRESULT WINAPI record_RecordDestroy(IRecordInfo *iface, void *data)
{
	HRESULT status;
	if (!data)
		return E_INVALIDARG;
	status = IRecordInfo_RecordClear(iface, data);
	if (SUCCEEDED(status))
		HeapFree(GetProcessHeap(), 0, data);
	return status;
}

static const IRecordInfoVtbl record_vtbl = {
	record_QueryInterface,
	record_AddRef,
	record_Release,
	record_RecordInit,
	record_RecordClear,
	record_RecordCopy,
	record_GetGuid,
	record_GetName,
	record_GetSize,
	record_GetTypeInfo,
	record_GetField,
	record_GetFieldNoCopy,
	record_PutField,
	record_PutFieldNoCopy,
	record_GetFieldNames,
	record_IsMatchingType,
	record_RecordCreate,
	record_RecordCreateCopy,
	record_RecordDestroy,
};

HRESULT WINAPI GetRecordInfoFromTypeInfo(ITypeInfo *info, IRecordInfo **out)
{
	struct record_info *record;
	TYPEATTR *attr = NULL;
	ITypeInfo *resolved = info;
	HRESULT status;
	WORD index;
	if (!info || !out)
		return E_INVALIDARG;
	*out = NULL;
	status = ITypeInfo_GetTypeAttr(info, &attr);
	if (FAILED(status))
		return status;
	ITypeInfo_AddRef(resolved);
	/* Aliases of a record count as the record. */
	if (attr->typekind == TKIND_ALIAS && attr->tdescAlias.vt == VT_USERDEFINED)
	{
		ITypeInfo *target = NULL;
		status = ITypeInfo_GetRefTypeInfo(info, attr->tdescAlias.hreftype, &target);
		ITypeInfo_ReleaseTypeAttr(info, attr);
		attr = NULL;
		ITypeInfo_Release(resolved);
		if (FAILED(status))
			return status;
		resolved = target;
		status = ITypeInfo_GetTypeAttr(resolved, &attr);
		if (FAILED(status))
		{
			ITypeInfo_Release(resolved);
			return status;
		}
	}
	if (attr->typekind != TKIND_RECORD)
	{
		ITypeInfo_ReleaseTypeAttr(resolved, attr);
		ITypeInfo_Release(resolved);
		return E_INVALIDARG;
	}
	record = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*record));
	if (record)
		record->fields = attr->cVars ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, attr->cVars * sizeof(*record->fields)) : NULL;
	if (!record || (attr->cVars && !record->fields))
	{
		HeapFree(GetProcessHeap(), 0, record);
		ITypeInfo_ReleaseTypeAttr(resolved, attr);
		ITypeInfo_Release(resolved);
		return E_OUTOFMEMORY;
	}
	record->IRecordInfo_iface.lpVtbl = &record_vtbl;
	record->ref = 1;
	record->size = attr->cbSizeInstance;
	record->guid = attr->guid;
	record->info = resolved;
	record->count = attr->cVars;
	ITypeInfo_GetDocumentation(resolved, MEMBERID_NIL, &record->name, NULL, NULL, NULL);
	for (index = 0; index < attr->cVars; index++)
	{
		VARDESC *desc = NULL;
		struct record_field *field = &record->fields[index];
		if (FAILED(ITypeInfo_GetVarDesc(resolved, index, &desc)))
			continue;
		field->type = desc->elemdescVar.tdesc.vt;
		field->kind = desc->varkind;
		field->offset = desc->oInst;
		ITypeInfo_GetDocumentation(resolved, desc->memid, &field->name, NULL, NULL, NULL);
		if (field->type == VT_USERDEFINED)
			field->type = VT_I4;
		ITypeInfo_ReleaseVarDesc(resolved, desc);
	}
	ITypeInfo_ReleaseTypeAttr(resolved, attr);
	*out = &record->IRecordInfo_iface;
	return S_OK;
}

HRESULT WINAPI GetRecordInfoFromGuids(REFGUID library, ULONG major, ULONG minor, LCID lcid, REFGUID type, IRecordInfo **out)
{
	ITypeLib *lib = NULL;
	ITypeInfo *info = NULL;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	status = LoadRegTypeLib(library, (WORD)major, (WORD)minor, lcid, &lib);
	if (FAILED(status))
		return status;
	status = ITypeLib_GetTypeInfoOfGuid(lib, type, &info);
	ITypeLib_Release(lib);
	if (FAILED(status))
		return status;
	status = GetRecordInfoFromTypeInfo(info, out);
	ITypeInfo_Release(info);
	return status;
}
