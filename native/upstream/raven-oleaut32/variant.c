/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "oa.h"

void WINAPI VariantInit(VARIANTARG *value)
{
	memset(value, 0, sizeof(*value));
}

/* Which types a variant may hold at all, with or without the modifier bits. */
static HRESULT validate_type(VARTYPE type)
{
	VARTYPE base = type & VT_TYPEMASK, extra = type & ~VT_TYPEMASK;
	if (extra & ~(VT_BYREF | VT_ARRAY))
		return DISP_E_BADVARTYPE;
	if (base > VT_LPWSTR && base != VT_RECORD && base != VT_CLSID)
		return DISP_E_BADVARTYPE;
	if (base == 15 || (base >= 24 && base <= 31))
		return DISP_E_BADVARTYPE;
	if (extra && base <= VT_NULL)
		return DISP_E_BADVARTYPE;
	return S_OK;
}

HRESULT WINAPI VariantClear(VARIANTARG *value)
{
	HRESULT result = validate_type(V_VT(value));
	if (FAILED(result))
		return result;
	if (V_VT(value) & VT_ARRAY)
	{
		if (!(V_VT(value) & VT_BYREF))
			result = SafeArrayDestroy(V_ARRAY(value));
	}
	else if (!(V_VT(value) & VT_BYREF))
	{
		switch (V_VT(value))
		{
		case VT_BSTR:
			SysFreeString(V_BSTR(value));
			break;
		case VT_DISPATCH: case VT_UNKNOWN:
			if (V_UNKNOWN(value))
				IUnknown_Release(V_UNKNOWN(value));
			break;
		case VT_RECORD:
			if (V_RECORDINFO(value))
			{
				IRecordInfo_RecordClear(V_RECORDINFO(value), V_RECORD(value));
				CoTaskMemFree(V_RECORD(value));
				IRecordInfo_Release(V_RECORDINFO(value));
			}
			break;
		default:
			break;
		}
	}
	if (SUCCEEDED(result))
		V_VT(value) = VT_EMPTY;
	return result;
}

static HRESULT copy_record(VARIANT *to, const VARIANT *from)
{
	IRecordInfo *info = V_RECORDINFO(from);
	ULONG size = 0;
	HRESULT result;
	void *data;
	if (!info)
		return S_OK;
	result = IRecordInfo_GetSize(info, &size);
	if (FAILED(result))
		return result;
	data = CoTaskMemAlloc(size);
	if (!data)
		return E_OUTOFMEMORY;
	memset(data, 0, size);
	result = IRecordInfo_RecordCopy(info, V_RECORD(from), data);
	if (FAILED(result))
	{
		CoTaskMemFree(data);
		return result;
	}
	IRecordInfo_AddRef(info);
	V_RECORDINFO(to) = info;
	V_RECORD(to) = data;
	return S_OK;
}

HRESULT WINAPI VariantCopy(VARIANTARG *to, VARIANTARG *from)
{
	HRESULT result = validate_type(V_VT(from));
	if (FAILED(result))
		return result;
	if ((V_VT(from) & VT_TYPEMASK) == VT_CLSID)
		return DISP_E_BADVARTYPE;
	if (to == from)
		return S_OK;
	result = VariantClear(to);
	if (FAILED(result))
		return result;
	if (V_ISARRAY(from) && !V_ISBYREF(from))
	{
		result = SafeArrayCopy(V_ARRAY(from), &V_ARRAY(to));
		if (FAILED(result))
			return result;
		V_VT(to) = V_VT(from);
		return S_OK;
	}
	if (V_ISBYREF(from))
	{
		*to = *from;
		return S_OK;
	}
	switch (V_VT(from))
	{
	case VT_BSTR:
		/* Even a null string comes out as a real, empty one. */
		V_BSTR(to) = SysAllocStringByteLen((const char *)V_BSTR(from), V_BSTR(from) ? SysStringByteLen(V_BSTR(from)) : 0);
		if (!V_BSTR(to))
			return E_OUTOFMEMORY;
		V_VT(to) = VT_BSTR;
		return S_OK;
	case VT_RECORD:
		result = copy_record(to, from);
		if (SUCCEEDED(result))
			V_VT(to) = VT_RECORD;
		return result;
	case VT_UNKNOWN: case VT_DISPATCH:
		if (V_UNKNOWN(from))
			IUnknown_AddRef(V_UNKNOWN(from));
		/* fall through */
	default:
		*to = *from;
		return S_OK;
	}
}

HRESULT WINAPI VariantCopyInd(VARIANT *to, VARIANTARG *from)
{
	VARIANTARG *source = from, local;
	VARTYPE base;
	HRESULT result;
	if (!V_ISBYREF(from))
		return VariantCopy(to, from);
	result = validate_type(V_VT(from));
	if (FAILED(result))
		return E_INVALIDARG;
	base = V_VT(from) & ~VT_BYREF;
	/* A reference to a variant is followed once, and may not lead to another. */
	if (base == VT_VARIANT)
	{
		source = V_VARIANTREF(from);
		if (V_VT(source) == (VT_VARIANT | VT_BYREF))
			return E_INVALIDARG;
		if (!V_ISBYREF(source))
			return to == from ? (local = *source, VariantCopy(to, &local)) : VariantCopy(to, source);
		base = V_VT(source) & ~VT_BYREF;
		if (base == VT_VARIANT)
			return E_INVALIDARG;
	}
	VariantInit(&local);
	if (base & VT_ARRAY)
	{
		result = SafeArrayCopy(*V_ARRAYREF(source), &V_ARRAY(&local));
	}
	else
	{
		switch (base)
		{
		case VT_BSTR:
			V_BSTR(&local) = *V_BSTRREF(source) ? SysAllocStringByteLen((const char *)*V_BSTRREF(source), SysStringByteLen(*V_BSTRREF(source))) : NULL;
			result = !*V_BSTRREF(source) || V_BSTR(&local) ? S_OK : E_OUTOFMEMORY;
			break;
		case VT_UNKNOWN: case VT_DISPATCH:
			V_UNKNOWN(&local) = *V_UNKNOWNREF(source);
			if (V_UNKNOWN(&local))
				IUnknown_AddRef(V_UNKNOWN(&local));
			result = S_OK;
			break;
		case VT_RECORD:
			result = copy_record(&local, source);
			break;
		case VT_DECIMAL:
			{
				DECIMAL value = *V_DECIMALREF(source);
				V_DECIMAL(&local) = value;
				result = S_OK;
			}
			break;
		default:
			{
				ULONG size = oa_vartype_size(base);
				if (!size)
					return E_INVALIDARG;
				memcpy(&V_UI1(&local), V_BYREF(source), size);
				result = S_OK;
			}
			break;
		}
	}
	if (FAILED(result))
		return result;
	if (to != from || 1)
	{
		result = VariantClear(to);
		if (FAILED(result))
		{
			V_VT(&local) = base;
			VariantClear(&local);
			return result;
		}
	}
	V_VT(&local) = base;
	*to = local;
	return S_OK;
}

void WINAPI ClearCustData(CUSTDATA *data)
{
	DWORD index;
	if (!data)
		return;
	for (index = 0; index < data->cCustData; index++)
		VariantClear(&data->prgCustData[index].varValue);
	CoTaskMemFree(data->prgCustData);
	data->prgCustData = NULL;
	data->cCustData = 0;
}
