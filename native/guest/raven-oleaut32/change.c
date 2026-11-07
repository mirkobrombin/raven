/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "oa.h"

static LCID conversion_setting;

HRESULT WINAPI SetVarConversionLocaleSetting(ULONG value)
{
	conversion_setting = value;
	return S_OK;
}

HRESULT WINAPI GetVarConversionLocaleSetting(ULONG *value)
{
	if (!value)
		return E_INVALIDARG;
	*value = conversion_setting;
	return S_OK;
}

/* A target of this family, from every source a variant can hold. */
#define SWITCH_SOURCE(Name, Out) \
	switch (V_VT(from)) \
	{ \
	case VT_I1: return Var##Name##FromI1(V_I1(from), Out); \
	case VT_UI1: return Var##Name##FromUI1(V_UI1(from), Out); \
	case VT_I2: return Var##Name##FromI2(V_I2(from), Out); \
	case VT_UI2: return Var##Name##FromUI2(V_UI2(from), Out); \
	case VT_I4: case VT_ERROR: case VT_INT: return Var##Name##FromI4(V_I4(from), Out); \
	case VT_UI4: case VT_UINT: return Var##Name##FromUI4(V_UI4(from), Out); \
	case VT_I8: return Var##Name##FromI8(V_I8(from), Out); \
	case VT_UI8: return Var##Name##FromUI8(V_UI8(from), Out); \
	case VT_R4: return Var##Name##FromR4(V_R4(from), Out); \
	case VT_R8: return Var##Name##FromR8(V_R8(from), Out); \
	case VT_CY: return Var##Name##FromCy(V_CY(from), Out); \
	case VT_DECIMAL: return Var##Name##FromDec(&V_DECIMAL(from), Out); \
	case VT_BOOL: return Var##Name##FromBool(V_BOOL(from), Out); \
	case VT_DATE: return Var##Name##FromDate(V_DATE(from), Out); \
	case VT_BSTR: return Var##Name##FromStr(V_BSTR(from), lcid, flags, Out); \
	case VT_DISPATCH: return Var##Name##FromDisp(V_DISPATCH(from), lcid, Out); \
	default: return DISP_E_TYPEMISMATCH; \
	}

/* Date has no source from the machine types below 32 bits in some paths, but all exist here. */
static HRESULT to_scalar(VARIANT *to, VARIANT *from, LCID lcid, ULONG flags, VARTYPE type)
{
	switch (type)
	{
	case VT_I1: SWITCH_SOURCE(I1, &V_I1(to))
	case VT_UI1: SWITCH_SOURCE(UI1, &V_UI1(to))
	case VT_I2: SWITCH_SOURCE(I2, &V_I2(to))
	case VT_UI2: SWITCH_SOURCE(UI2, &V_UI2(to))
	case VT_I4: case VT_INT: SWITCH_SOURCE(I4, &V_I4(to))
	case VT_UI4: case VT_UINT: SWITCH_SOURCE(UI4, &V_UI4(to))
	case VT_I8: SWITCH_SOURCE(I8, &V_I8(to))
	case VT_UI8: SWITCH_SOURCE(UI8, &V_UI8(to))
	case VT_R4: SWITCH_SOURCE(R4, &V_R4(to))
	case VT_R8: SWITCH_SOURCE(R8, &V_R8(to))
	case VT_CY: SWITCH_SOURCE(Cy, &V_CY(to))
	case VT_DECIMAL: SWITCH_SOURCE(Dec, &V_DECIMAL(to))
	case VT_BOOL: SWITCH_SOURCE(Bool, &V_BOOL(to))
	case VT_DATE: SWITCH_SOURCE(Date, &V_DATE(to))
	default:
		return DISP_E_TYPEMISMATCH;
	}
}

static HRESULT to_text(VARIANT *to, VARIANT *from, LCID lcid, ULONG flags)
{
	BSTR *out = &V_BSTR(to);
	flags &= ~(VARIANT_LOCALBOOL | VARIANT_ALPHABOOL) | VARIANT_LOCALBOOL | VARIANT_ALPHABOOL;
	switch (V_VT(from))
	{
	case VT_I1: return VarBstrFromI1(V_I1(from), lcid, 0, out);
	case VT_UI1: return VarBstrFromUI1(V_UI1(from), lcid, 0, out);
	case VT_I2: return VarBstrFromI2(V_I2(from), lcid, 0, out);
	case VT_UI2: return VarBstrFromUI2(V_UI2(from), lcid, 0, out);
	case VT_I4: case VT_INT: return VarBstrFromI4(V_I4(from), lcid, 0, out);
	case VT_UI4: case VT_UINT: return VarBstrFromUI4(V_UI4(from), lcid, 0, out);
	case VT_I8: return VarBstrFromI8(V_I8(from), lcid, 0, out);
	case VT_UI8: return VarBstrFromUI8(V_UI8(from), lcid, 0, out);
	case VT_R4: return VarBstrFromR4(V_R4(from), lcid, flags & LOCALE_NOUSEROVERRIDE, out);
	case VT_R8: return VarBstrFromR8(V_R8(from), lcid, flags & LOCALE_NOUSEROVERRIDE, out);
	case VT_CY: return VarBstrFromCy(V_CY(from), lcid, flags & LOCALE_NOUSEROVERRIDE, out);
	case VT_DECIMAL: return VarBstrFromDec(&V_DECIMAL(from), lcid, flags & LOCALE_NOUSEROVERRIDE, out);
	case VT_DATE: return VarBstrFromDate(V_DATE(from), lcid, flags & (VARIANT_USE_NLS | LOCALE_NOUSEROVERRIDE | VAR_TIMEVALUEONLY | VAR_DATEVALUEONLY | VAR_FOURDIGITYEARS), out);
	case VT_BOOL:
		/* Plain numbers unless the caller asked for words. */
		if (flags & (VARIANT_ALPHABOOL | VARIANT_LOCALBOOL))
			return VarBstrFromBool(V_BOOL(from), lcid, flags & VARIANT_LOCALBOOL ? VAR_LOCALBOOL : 0, out);
		return VarBstrFromI2(V_BOOL(from), lcid, 0, out);
	case VT_DISPATCH: return VarBstrFromDisp(V_DISPATCH(from), lcid, flags, out);
	default:
		return DISP_E_TYPEMISMATCH;
	}
}

static BOOL is_scalar_type(VARTYPE type)
{
	switch (type)
	{
	case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_I8: case VT_UI8:
	case VT_R4: case VT_R8: case VT_CY: case VT_DECIMAL: case VT_BOOL: case VT_DATE: case VT_BSTR: case VT_ERROR:
		return TRUE;
	default:
		return FALSE;
	}
}

/* An empty variant counts as zero or an empty text; only NULL itself may become NULL. */
static HRESULT from_empty(VARIANT *to, VARTYPE type)
{
	switch (type)
	{
	case VT_BSTR:
		V_BSTR(to) = SysAllocStringLen(NULL, 0);
		return V_BSTR(to) ? S_OK : E_OUTOFMEMORY;
	case VT_DECIMAL:
		memset(&V_DECIMAL(to), 0, sizeof(DECIMAL));
		return S_OK;
	case VT_CY:
		V_CY(to).int64 = 0;
		return S_OK;
	case VT_I8: case VT_UI8: case VT_R8: case VT_DATE:
		V_I8(to) = 0;
		return S_OK;
	case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4: case VT_BOOL:
		V_I4(to) = 0;
		return S_OK;
	default:
		return DISP_E_TYPEMISMATCH;
	}
}

/* Byte arrays and strings turn into each other; arrays otherwise only to their own kind. */
static HRESULT array_change(VARIANT *to, VARIANT *from, VARTYPE type)
{
	if (V_ISARRAY(from) && type == VT_BSTR)
	{
		VARTYPE have = 0;
		SafeArrayGetVartype(V_ARRAY(from), &have);
		if (have == VT_UI1)
			return BstrFromVector(V_ARRAY(from), &V_BSTR(to));
		return DISP_E_TYPEMISMATCH;
	}
	if (type & VT_ARRAY)
	{
		if (V_VT(from) == VT_BSTR && (type & VT_TYPEMASK) == VT_UI1)
			return VectorFromBstr(V_BSTR(from), &V_ARRAY(to));
		if (V_ISARRAY(from) && (V_VT(from) & VT_TYPEMASK) == (type & VT_TYPEMASK))
			return SafeArrayCopy(V_ARRAY(from), &V_ARRAY(to));
	}
	return DISP_E_TYPEMISMATCH;
}

static ULONG whole_width(VARTYPE type)
{
	switch (type)
	{
	case VT_I1:
	case VT_UI1:
		return 1;
	case VT_I2:
	case VT_UI2:
		return 2;
	case VT_I4:
	case VT_UI4:
	case VT_INT:
	case VT_UINT:
		return 4;
	}
	return 0;
}

HRESULT WINAPI VariantChangeTypeEx(VARIANTARG *to, VARIANTARG *from_in, LCID lcid, USHORT flags, VARTYPE type)
{
	VARIANT source, result;
	VARIANT *from;
	HRESULT status;
	VARTYPE base = type & VT_TYPEMASK;
	if (!to || !from_in)
		return E_INVALIDARG;
	if (!(type & VT_VECTOR) && V_ISBYREF(from_in) && !V_BYREF(from_in))
		return DISP_E_TYPEMISMATCH;
	if (type & VT_VECTOR || type & VT_BYREF || (base > VT_UINT && base != VT_RECORD && !((base == VT_INT_PTR || base == VT_UINT_PTR) && type & VT_ARRAY)))
		return DISP_E_BADVARTYPE;
	if ((base == 15 || base == VT_HRESULT || base == VT_VOID) && !(type & VT_ARRAY))
		return DISP_E_BADVARTYPE;
	if (V_VT(from_in) & VT_VECTOR)
		return DISP_E_BADVARTYPE;
	if (V_VT(from_in) == VT_ERROR && (type == VT_EMPTY || type == VT_NULL))
		return DISP_E_TYPEMISMATCH;
	if (V_VT(from_in) & VT_ARRAY && ((V_VT(from_in) & VT_TYPEMASK) == VT_INT_PTR || (V_VT(from_in) & VT_TYPEMASK) == VT_UINT_PTR))
		return DISP_E_BADVARTYPE;
	VariantInit(&source);
	from = from_in;
	/* References go away first, so the conversions see plain values. */
	if (V_ISBYREF(from_in))
	{
		status = VariantCopyInd(&source, from_in);
		if (FAILED(status))
			return status;
		from = &source;
	}
	VariantInit(&result);
	if (V_VT(from) == type)
		status = VariantCopy(&result, from);
	else if ((type & VT_ARRAY) || V_ISARRAY(from))
		status = array_change(&result, from, type);
	else if (V_VT(from) == VT_NULL)
		status = DISP_E_TYPEMISMATCH;
	else if (type == VT_NULL || type == VT_EMPTY)
		status = S_OK;
	else if (V_VT(from) == VT_EMPTY)
		status = from_empty(&result, type);
	else if (type == VT_UNKNOWN || type == VT_DISPATCH)
	{
		if (V_VT(from) == VT_UNKNOWN || V_VT(from) == VT_DISPATCH)
		{
			if (!V_UNKNOWN(from))
				V_UNKNOWN(&result) = NULL;
			else
				status = IUnknown_QueryInterface(V_UNKNOWN(from), type == VT_UNKNOWN ? &IID_IUnknown : &IID_IDispatch, (void **)&V_UNKNOWN(&result));
			if (status == E_NOINTERFACE)
				status = DISP_E_TYPEMISMATCH;
		}
		else
			status = DISP_E_TYPEMISMATCH;
	}
	else if (V_VT(from) == VT_UNKNOWN || type == VT_RECORD || V_VT(from) == VT_RECORD)
		status = DISP_E_TYPEMISMATCH;
	else if (V_VT(from) == VT_DISPATCH && flags & VARIANT_NOVALUEPROP)
		status = DISP_E_TYPEMISMATCH;
	else if (V_VT(from) == VT_ERROR)
		status = DISP_E_TYPEMISMATCH;
	else if (type == VT_ERROR)
		status = DISP_E_TYPEMISMATCH;
	else if (type == VT_VARIANT)
		status = DISP_E_TYPEMISMATCH;
	else if (whole_width(type) && whole_width(type) == whole_width(V_VT(from)))
		/* Changing only the sign at the same size keeps the bits and never overflows. */
		memcpy(&result.lVal, &from->lVal, whole_width(type));
	else if (type == VT_BSTR)
		status = to_text(&result, from, lcid, flags);
	else if (is_scalar_type(type))
		status = to_scalar(&result, from, lcid, flags, type);
	else
		status = DISP_E_TYPEMISMATCH;
	VariantClear(&source);
	if (FAILED(status))
		return status;
	{
		HRESULT clear = VariantClear(to);
		if (FAILED(clear))
		{
			VariantClear(&result);
			return clear;
		}
	}
	*to = result;
	V_VT(to) = type;
	return S_OK;
}

HRESULT WINAPI VariantChangeType(VARIANTARG *to, VARIANTARG *from, USHORT flags, VARTYPE type)
{
	return VariantChangeTypeEx(to, from, LOCALE_USER_DEFAULT, flags, type);
}
