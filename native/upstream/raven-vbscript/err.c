/* SPDX-License-Identifier: MIT */
#include "vbs.h"

/* The Err object. Each reference reads and changes the one error state of its engine: Number is its default property, Raise raises a
 * new error and Clear empties it. */

typedef struct {
	IDispatch IDispatch_iface;
	LONG refs;
	engine *en;
} err_object;

enum { ERR_NUMBER = DISPID_VALUE, ERR_DESCRIPTION = 1, ERR_SOURCE, ERR_HELPFILE, ERR_HELPCONTEXT, ERR_RAISE, ERR_CLEAR };

static const struct {
	const WCHAR *name;
	DISPID id;
} members[] = {
	{ L"Number", ERR_NUMBER }, { L"Description", ERR_DESCRIPTION }, { L"Source", ERR_SOURCE }, { L"HelpFile", ERR_HELPFILE },
	{ L"HelpContext", ERR_HELPCONTEXT }, { L"Raise", ERR_RAISE }, { L"Clear", ERR_CLEAR },
};

static err_object *impl(IDispatch *iface) { return CONTAINING_RECORD(iface, err_object, IDispatch_iface); }

static HRESULT WINAPI err_QueryInterface(IDispatch *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch)) {
		*out = iface;
		IDispatch_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI err_AddRef(IDispatch *iface) { return InterlockedIncrement(&impl(iface)->refs); }

static ULONG WINAPI err_Release(IDispatch *iface)
{
	err_object *err = impl(iface);
	LONG refs = InterlockedDecrement(&err->refs);
	if (!refs)
		vbs_free(err);
	return refs;
}

static HRESULT WINAPI err_GetTypeInfoCount(IDispatch *iface, UINT *count)
{
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI err_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **info)
{
	*info = NULL;
	return DISP_E_BADINDEX;
}

static HRESULT WINAPI err_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	for (UINT n = 0; n < count; n++)
		ids[n] = DISPID_UNKNOWN;
	for (UINT i = 0; i < ARRAYSIZE(members); i++)
		if (count && vbs_same(members[i].name, names[0])) {
			ids[0] = members[i].id;
			return count == 1 ? S_OK : DISP_E_UNKNOWNNAME;
		}
	return DISP_E_UNKNOWNNAME;
}

static void put_text(BSTR *field, VARIANT *value)
{
	BSTR text;
	if (FAILED(vbs_to_text(value, &text)))
		return;
	SysFreeString(*field);
	*field = text;
}

static HRESULT WINAPI err_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, UINT *bad)
{
	script_failure *state = &impl(iface)->en->err;
	UINT count = params ? params->cArgs : 0;
	BOOL put = (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) != 0;
	VARIANT *value = put && count ? &params->rgvarg[0] : NULL;
	if (put && !value)
		return DISP_E_BADPARAMCOUNT;
	if (result)
		VariantInit(result);
	switch (id) {
	case ERR_NUMBER: {
		if (put) {
			LONG number;
			if (FAILED(vbs_to_long(value, &number)))
				return DISP_E_TYPEMISMATCH;
			state->number = number;
		} else if (result) {
			V_VT(result) = VT_I4;
			V_I4(result) = state->number;
		}
		return S_OK;
	}
	case ERR_DESCRIPTION:
	case ERR_SOURCE:
	case ERR_HELPFILE: {
		static BSTR help_file;
		BSTR *field = id == ERR_DESCRIPTION ? &state->description : id == ERR_SOURCE ? &state->source : &help_file;
		if (put) {
			put_text(field, value);
		} else if (result) {
			V_VT(result) = VT_BSTR;
			V_BSTR(result) = SysAllocString(*field ? *field : L"");
		}
		return S_OK;
	}
	case ERR_HELPCONTEXT:
		if (result && !put) {
			V_VT(result) = VT_I4;
			V_I4(result) = 0;
		}
		return S_OK;
	case ERR_CLEAR:
		vbs_failure_clear(state);
		return S_OK;
	case ERR_RAISE: {
		/* Raise(number[, source[, description]]); DISPPARAMS holds them last first. */
		if (!count || count > 5)
			return DISP_E_BADPARAMCOUNT;
		LONG number;
		if (FAILED(vbs_to_long(&params->rgvarg[count - 1], &number)))
			return DISP_E_TYPEMISMATCH;
		BSTR source = NULL, description = NULL;
		if (count >= 2 && V_VT(&params->rgvarg[count - 2]) != VT_ERROR)
			vbs_to_text(&params->rgvarg[count - 2], &source);
		if (count >= 3 && V_VT(&params->rgvarg[count - 3]) != VT_ERROR)
			vbs_to_text(&params->rgvarg[count - 3], &description);
		if (info) {
			memset(info, 0, sizeof(*info));
			/* Small numbers are VBScript's own errors; others, such as vbObjectError + n, are HRESULTs already. */
			info->scode = number > 0 && number <= 0xffff ? MAKE_HRESULT(SEVERITY_ERROR, FACILITY_CONTROL, number) : number;
			info->bstrSource = source ? source : SysAllocString(L"Microsoft VBScript runtime error");
			info->bstrDescription = description ? description : SysAllocString(number > 0 && number <= 0xffff ?
				vbs_error_text(number) : L"Unknown runtime error");
		} else {
			SysFreeString(source);
			SysFreeString(description);
		}
		return DISP_E_EXCEPTION;
	}
	}
	return DISP_E_MEMBERNOTFOUND;
}

static IDispatchVtbl err_vtbl = {
	err_QueryInterface, err_AddRef, err_Release, err_GetTypeInfoCount, err_GetTypeInfo, err_GetIDsOfNames, err_Invoke,
};

IDispatch *vbs_err_object(engine *en)
{
	err_object *err = vbs_alloc(sizeof(err_object));
	err->IDispatch_iface.lpVtbl = &err_vtbl;
	err->refs = 1;
	err->en = en;
	return &err->IDispatch_iface;
}
