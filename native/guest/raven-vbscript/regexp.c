/* SPDX-License-Identifier: MIT */
#include "vbs.h"
#include "regex.h"

/* The RegExp object of VBScript 5.5 and the collections it hands out: Execute gives a Matches collection of Match objects, each
 * with its SubMatches; Test tells whether the pattern matches; Replace substitutes $1..$99, $&, $`, $' and $$ in the replacement. */

typedef enum { OBJECT_REGEXP, OBJECT_MATCH, OBJECT_MATCHES, OBJECT_SUBMATCHES } object_kind;

typedef struct rx_object rx_object;
struct rx_object {
	IDispatch IDispatch_iface;
	LONG refs;
	object_kind kind;
	/* RegExp */
	BSTR pattern;
	BOOL ignore_case, global, multiline;
	rx *compiled;
	/* Match: its text, where it starts, and the groups (VT_EMPTY for one that took no part). */
	BSTR value;
	LONG first_index;
	VARIANT *groups;
	UINT group_count;
	/* Matches */
	IDispatch **items;
	UINT item_count;
	/* SubMatches: the Match whose groups it lists. */
	rx_object *match;
};

enum {
	REGEXP_PATTERN = 1, REGEXP_IGNORECASE, REGEXP_GLOBAL, REGEXP_MULTILINE, REGEXP_EXECUTE, REGEXP_TEST, REGEXP_REPLACE,
	MATCH_FIRSTINDEX = 1, MATCH_LENGTH, MATCH_SUBMATCHES,
	COLLECTION_COUNT = 1, COLLECTION_ITEM,
};

typedef struct {
	const WCHAR *name;
	DISPID id;
} member;

static const member regexp_members[] = {
	{ L"Pattern", REGEXP_PATTERN }, { L"IgnoreCase", REGEXP_IGNORECASE }, { L"Global", REGEXP_GLOBAL }, { L"Multiline", REGEXP_MULTILINE },
	{ L"Execute", REGEXP_EXECUTE }, { L"Test", REGEXP_TEST }, { L"Replace", REGEXP_REPLACE }, { NULL },
};
static const member match_members[] = {
	{ L"Value", DISPID_VALUE }, { L"FirstIndex", MATCH_FIRSTINDEX }, { L"Length", MATCH_LENGTH }, { L"SubMatches", MATCH_SUBMATCHES }, { NULL },
};
static const member collection_members[] = {
	{ L"Count", COLLECTION_COUNT }, { L"Item", COLLECTION_ITEM }, { L"_NewEnum", DISPID_NEWENUM }, { NULL },
};

static IDispatchVtbl object_vtbl;

static rx_object *impl(IDispatch *iface) { return CONTAINING_RECORD(iface, rx_object, IDispatch_iface); }

static rx_object *new_object(object_kind kind)
{
	rx_object *object = vbs_alloc(sizeof(rx_object));
	object->IDispatch_iface.lpVtbl = &object_vtbl;
	object->refs = 1;
	object->kind = kind;
	return object;
}

IDispatch *vbs_regexp_new(void)
{
	rx_object *object = new_object(OBJECT_REGEXP);
	object->pattern = SysAllocString(L"");
	return &object->IDispatch_iface;
}

/* An enumerator over a copy of a collection's items, for For Each. */

typedef struct {
	IEnumVARIANT IEnumVARIANT_iface;
	LONG refs;
	VARIANT *items;
	ULONG count, at;
} enumerator;

static enumerator *enum_impl(IEnumVARIANT *iface) { return CONTAINING_RECORD(iface, enumerator, IEnumVARIANT_iface); }

static HRESULT WINAPI enum_QueryInterface(IEnumVARIANT *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumVARIANT)) {
		*out = iface;
		IEnumVARIANT_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI enum_AddRef(IEnumVARIANT *iface) { return InterlockedIncrement(&enum_impl(iface)->refs); }

static ULONG WINAPI enum_Release(IEnumVARIANT *iface)
{
	enumerator *e = enum_impl(iface);
	LONG refs = InterlockedDecrement(&e->refs);
	if (!refs) {
		for (ULONG i = 0; i < e->count; i++)
			VariantClear(&e->items[i]);
		vbs_free(e->items);
		vbs_free(e);
	}
	return refs;
}

static HRESULT WINAPI enum_Next(IEnumVARIANT *iface, ULONG wanted, VARIANT *out, ULONG *fetched)
{
	enumerator *e = enum_impl(iface);
	ULONG got = 0;
	for (; got < wanted && e->at < e->count; got++, e->at++) {
		VariantInit(&out[got]);
		VariantCopy(&out[got], &e->items[e->at]);
	}
	if (fetched)
		*fetched = got;
	return got == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Skip(IEnumVARIANT *iface, ULONG count)
{
	enumerator *e = enum_impl(iface);
	ULONG left = e->count - e->at;
	e->at += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Reset(IEnumVARIANT *iface)
{
	enum_impl(iface)->at = 0;
	return S_OK;
}

static IEnumVARIANT *make_enumerator(VARIANT *items, ULONG count);

static HRESULT WINAPI enum_Clone(IEnumVARIANT *iface, IEnumVARIANT **out)
{
	enumerator *e = enum_impl(iface);
	*out = make_enumerator(e->items, e->count);
	enum_impl(*out)->at = e->at;
	return S_OK;
}

static IEnumVARIANTVtbl enum_vtbl = { enum_QueryInterface, enum_AddRef, enum_Release, enum_Next, enum_Skip, enum_Reset, enum_Clone };

static IEnumVARIANT *make_enumerator(VARIANT *items, ULONG count)
{
	enumerator *e = vbs_alloc(sizeof(enumerator));
	e->IEnumVARIANT_iface.lpVtbl = &enum_vtbl;
	e->refs = 1;
	e->items = vbs_alloc(count * sizeof(VARIANT));
	e->count = count;
	for (ULONG i = 0; i < count; i++)
		VariantCopy(&e->items[i], &items[i]);
	return &e->IEnumVARIANT_iface;
}

/* IDispatch, shared by the four kinds. */

static HRESULT WINAPI object_QueryInterface(IDispatch *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch)) {
		*out = iface;
		IDispatch_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI object_AddRef(IDispatch *iface) { return InterlockedIncrement(&impl(iface)->refs); }

static ULONG WINAPI object_Release(IDispatch *iface)
{
	rx_object *object = impl(iface);
	LONG refs = InterlockedDecrement(&object->refs);
	if (refs)
		return refs;
	SysFreeString(object->pattern);
	rx_free(object->compiled);
	SysFreeString(object->value);
	for (UINT i = 0; i < object->group_count; i++)
		VariantClear(&object->groups[i]);
	vbs_free(object->groups);
	for (UINT i = 0; i < object->item_count; i++)
		IDispatch_Release(object->items[i]);
	vbs_free(object->items);
	if (object->match)
		IDispatch_Release(&object->match->IDispatch_iface);
	vbs_free(object);
	return 0;
}

static HRESULT WINAPI object_GetTypeInfoCount(IDispatch *iface, UINT *count)
{
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI object_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **info)
{
	*info = NULL;
	return DISP_E_BADINDEX;
}

static HRESULT WINAPI object_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	object_kind kind = impl(iface)->kind;
	const member *members = kind == OBJECT_REGEXP ? regexp_members : kind == OBJECT_MATCH ? match_members : collection_members;
	for (UINT i = 0; i < count; i++)
		ids[i] = DISPID_UNKNOWN;
	if (!count)
		return S_OK;
	for (const member *m = members; m->name; m++)
		if (vbs_same(m->name, names[0])) {
			ids[0] = m->id;
			return count == 1 ? S_OK : DISP_E_UNKNOWNNAME;
		}
	return DISP_E_UNKNOWNNAME;
}

static HRESULT raise(EXCEPINFO *info, LONG number)
{
	if (info) {
		memset(info, 0, sizeof(*info));
		info->scode = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_CONTROL, number);
		info->bstrSource = SysAllocString(L"Microsoft VBScript runtime error");
		info->bstrDescription = SysAllocString(number == RX_E_BRACKET ? L"Expected ']' in regular expression" :
			number == RX_E_PARENTHESIS ? L"Expected ')' in regular expression" : number == RX_E_SYNTAX ? L"Syntax error in regular expression" :
			vbs_error_text(number));
	}
	return DISP_E_EXCEPTION;
}

/* The pattern compiled with the current flags, compiled again after any of them changed. */
static HRESULT compiled(rx_object *re, EXCEPINFO *info)
{
	if (re->compiled)
		return S_OK;
	LONG error;
	re->compiled = rx_compile(re->pattern, SysStringLen(re->pattern), (re->ignore_case ? RX_IGNORE_CASE : 0) | (re->multiline ? RX_MULTILINE : 0),
		&error);
	return re->compiled ? S_OK : raise(info, error);
}

static VARIANT *argument(DISPPARAMS *params, UINT index)
{
	if (!params || index >= params->cArgs)
		return NULL;
	VARIANT *v = &params->rgvarg[params->cArgs - 1 - index];
	return V_VT(v) == (VT_BYREF | VT_VARIANT) ? V_VARIANTREF(v) : v;
}

static HRESULT text_argument(DISPPARAMS *params, UINT index, BSTR *out)
{
	VARIANT *v = argument(params, index);
	*out = NULL;
	if (!v)
		return DISP_E_PARAMNOTFOUND;
	return vbs_to_text(v, out);
}

static IDispatch *match_object(const WCHAR *text, const int *spans, UINT groups)
{
	rx_object *m = new_object(OBJECT_MATCH);
	m->first_index = spans[0];
	m->value = SysAllocStringLen(text + spans[0], (UINT)(spans[1] - spans[0]));
	m->group_count = groups;
	m->groups = vbs_alloc((groups ? groups : 1) * sizeof(VARIANT));
	for (UINT g = 0; g < groups; g++)
		if (spans[2 * (g + 1)] >= 0) {
			V_VT(&m->groups[g]) = VT_BSTR;
			V_BSTR(&m->groups[g]) = SysAllocStringLen(text + spans[2 * (g + 1)], (UINT)(spans[2 * (g + 1) + 1] - spans[2 * (g + 1)]));
		}
	return &m->IDispatch_iface;
}

/* Where the search goes on after a match: past it, or one further after an empty one. */
static UINT next_start(const int *spans) { return (UINT)spans[1] + (spans[1] == spans[0]); }

static HRESULT regexp_execute(rx_object *re, BSTR text, VARIANT *result, EXCEPINFO *info)
{
	HRESULT hr = compiled(re, info);
	if (FAILED(hr))
		return hr;
	UINT groups = rx_groups(re->compiled), length = SysStringLen(text), start = 0;
	int *spans = vbs_alloc(2 * (groups + 1) * sizeof(int));
	rx_object *matches = new_object(OBJECT_MATCHES);
	while (start <= length && rx_search(re->compiled, text, length, start, spans)) {
		matches->items = vbs_grow(matches->items, (matches->item_count + 1) * sizeof(IDispatch *));
		matches->items[matches->item_count++] = match_object(text, spans, groups);
		if (!re->global)
			break;
		start = next_start(spans);
	}
	vbs_free(spans);
	if (result) {
		V_VT(result) = VT_DISPATCH;
		V_DISPATCH(result) = &matches->IDispatch_iface;
	} else
		IDispatch_Release(&matches->IDispatch_iface);
	return S_OK;
}

typedef struct {
	WCHAR *text;
	UINT length, room;
} builder;

static void append(builder *b, const WCHAR *text, UINT length)
{
	if (b->length + length + 1 > b->room) {
		b->room = (b->length + length + 1) * 2;
		b->text = vbs_grow(b->text, b->room * sizeof(WCHAR));
	}
	memcpy(b->text + b->length, text, length * sizeof(WCHAR));
	b->length += length;
}

/* The replacement for one match, with its $ patterns expanded. */
static void substitute(builder *b, const WCHAR *with, UINT with_length, const WCHAR *text, UINT length, const int *spans, UINT groups)
{
	for (UINT i = 0; i < with_length; i++) {
		WCHAR c = with[i];
		if (c != L'$' || i + 1 >= with_length) {
			append(b, &c, 1);
			continue;
		}
		WCHAR next = with[i + 1];
		if (next == L'$') {
			append(b, L"$", 1);
			i++;
		} else if (next == L'&') {
			append(b, text + spans[0], (UINT)(spans[1] - spans[0]));
			i++;
		} else if (next == L'`') {
			append(b, text, (UINT)spans[0]);
			i++;
		} else if (next == L'\'') {
			append(b, text + spans[1], length - (UINT)spans[1]);
			i++;
		} else if (next >= L'0' && next <= L'9') {
			UINT number = next - L'0', used = 1;
			if (i + 2 < with_length && with[i + 2] >= L'0' && with[i + 2] <= L'9' && number * 10 + (with[i + 2] - L'0') <= groups) {
				number = number * 10 + (with[i + 2] - L'0');
				used = 2;
			}
			if (number >= 1 && number <= groups) {
				if (spans[2 * number] >= 0)
					append(b, text + spans[2 * number], (UINT)(spans[2 * number + 1] - spans[2 * number]));
				i += used;
			} else
				append(b, &c, 1);
		} else
			append(b, &c, 1);
	}
}

static HRESULT regexp_replace(rx_object *re, BSTR text, BSTR with, VARIANT *result, EXCEPINFO *info)
{
	HRESULT hr = compiled(re, info);
	if (FAILED(hr))
		return hr;
	UINT groups = rx_groups(re->compiled), length = SysStringLen(text), start = 0, copied = 0;
	int *spans = vbs_alloc(2 * (groups + 1) * sizeof(int));
	builder b = { 0 };
	append(&b, L"", 0);
	while (start <= length && rx_search(re->compiled, text, length, start, spans)) {
		append(&b, text + copied, (UINT)spans[0] - copied);
		substitute(&b, with, SysStringLen(with), text, length, spans, groups);
		copied = (UINT)spans[1];
		if (!re->global)
			break;
		start = next_start(spans);
	}
	append(&b, text + copied, length - copied);
	vbs_free(spans);
	if (result) {
		V_VT(result) = VT_BSTR;
		V_BSTR(result) = SysAllocStringLen(b.text, b.length);
	}
	vbs_free(b.text);
	return S_OK;
}

static HRESULT put_flag(rx_object *re, BOOL *flag, VARIANT *value)
{
	BOOL yes;
	HRESULT hr = vbs_to_bool(value, &yes);
	if (FAILED(hr))
		return hr;
	if (*flag != yes) {
		*flag = yes;
		rx_free(re->compiled);
		re->compiled = NULL;
	}
	return S_OK;
}

static HRESULT regexp_invoke(rx_object *re, DISPID id, BOOL put, DISPPARAMS *params, VARIANT *result, EXCEPINFO *info)
{
	VARIANT *value = put ? &params->rgvarg[0] : NULL;
	BOOL *flag = id == REGEXP_IGNORECASE ? &re->ignore_case : id == REGEXP_GLOBAL ? &re->global : &re->multiline;
	BSTR text = NULL, with = NULL;
	HRESULT hr = S_OK;
	switch (id) {
	case REGEXP_PATTERN:
		if (put) {
			BSTR pattern;
			if (SUCCEEDED(hr = vbs_to_text(value, &pattern))) {
				SysFreeString(re->pattern);
				re->pattern = pattern;
				rx_free(re->compiled);
				re->compiled = NULL;
			}
		} else if (result) {
			V_VT(result) = VT_BSTR;
			V_BSTR(result) = SysAllocString(re->pattern);
		}
		return hr;
	case REGEXP_IGNORECASE:
	case REGEXP_GLOBAL:
	case REGEXP_MULTILINE:
		if (put)
			return id == REGEXP_GLOBAL ? (vbs_to_bool(value, flag)) : put_flag(re, flag, value);
		if (result) {
			V_VT(result) = VT_BOOL;
			V_BOOL(result) = *flag ? VARIANT_TRUE : VARIANT_FALSE;
		}
		return S_OK;
	case REGEXP_EXECUTE:
	case REGEXP_TEST:
		if (put)
			return DISP_E_MEMBERNOTFOUND;
		if (FAILED(hr = text_argument(params, 0, &text)))
			return hr;
		if (id == REGEXP_EXECUTE)
			hr = regexp_execute(re, text, result, info);
		else if (SUCCEEDED(hr = compiled(re, info))) {
			UINT groups = rx_groups(re->compiled);
			int *spans = vbs_alloc(2 * (groups + 1) * sizeof(int));
			BOOL found = rx_search(re->compiled, text, SysStringLen(text), 0, spans);
			vbs_free(spans);
			if (result) {
				V_VT(result) = VT_BOOL;
				V_BOOL(result) = found ? VARIANT_TRUE : VARIANT_FALSE;
			}
		}
		SysFreeString(text);
		return hr;
	case REGEXP_REPLACE:
		if (put)
			return DISP_E_MEMBERNOTFOUND;
		if (SUCCEEDED(hr = text_argument(params, 0, &text)) && SUCCEEDED(hr = text_argument(params, 1, &with)))
			hr = regexp_replace(re, text, with, result, info);
		SysFreeString(text);
		SysFreeString(with);
		return hr;
	}
	return DISP_E_MEMBERNOTFOUND;
}

/* Count, Item(index) as the default member, and _NewEnum, over Matches or a Match's groups. */
static HRESULT collection_invoke(rx_object *object, DISPID id, BOOL put, DISPPARAMS *params, VARIANT *result, EXCEPINFO *info)
{
	BOOL matches = object->kind == OBJECT_MATCHES;
	UINT count = matches ? object->item_count : object->match->group_count;
	if (put)
		return DISP_E_MEMBERNOTFOUND;
	if (id == COLLECTION_COUNT) {
		if (result) {
			V_VT(result) = VT_I4;
			V_I4(result) = (LONG)count;
		}
		return S_OK;
	}
	if (id == DISPID_NEWENUM) {
		VARIANT *items = vbs_alloc((count ? count : 1) * sizeof(VARIANT));
		for (UINT i = 0; i < count; i++) {
			if (matches) {
				V_VT(&items[i]) = VT_DISPATCH;
				V_DISPATCH(&items[i]) = object->items[i];
			} else
				items[i] = object->match->groups[i];
		}
		IEnumVARIANT *e = make_enumerator(items, count);
		vbs_free(items);
		if (result) {
			V_VT(result) = VT_UNKNOWN;
			V_UNKNOWN(result) = (IUnknown *)e;
		} else
			IEnumVARIANT_Release(e);
		return S_OK;
	}
	if (id == COLLECTION_ITEM || id == DISPID_VALUE) {
		VARIANT *index = argument(params, 0);
		LONG at;
		if (!index)
			return DISP_E_BADPARAMCOUNT;
		if (FAILED(vbs_to_long(index, &at)))
			return DISP_E_TYPEMISMATCH;
		if (at < 0 || (UINT)at >= count)
			return raise(info, 5);
		if (!result)
			return S_OK;
		if (matches) {
			V_VT(result) = VT_DISPATCH;
			V_DISPATCH(result) = object->items[at];
			IDispatch_AddRef(object->items[at]);
			return S_OK;
		}
		return VariantCopy(result, &object->match->groups[at]);
	}
	return DISP_E_MEMBERNOTFOUND;
}

static HRESULT WINAPI object_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, UINT *bad)
{
	rx_object *object = impl(iface);
	BOOL put = (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) != 0;
	DISPPARAMS none = { 0 };
	if (!params)
		params = &none;
	if (put && !params->cArgs)
		return DISP_E_BADPARAMCOUNT;
	if (result)
		VariantInit(result);
	switch (object->kind) {
	case OBJECT_REGEXP:
		return regexp_invoke(object, id, put, params, result, info);
	case OBJECT_MATCH:
		if (put)
			return DISP_E_MEMBERNOTFOUND;
		/* Its members take no arguments; m.SubMatches(0) is the language's to resolve. */
		if (params->cArgs)
			return DISP_E_BADPARAMCOUNT;
		if (!result)
			return S_OK;
		switch (id) {
		case DISPID_VALUE:
			V_VT(result) = VT_BSTR;
			V_BSTR(result) = SysAllocString(object->value);
			return S_OK;
		case MATCH_FIRSTINDEX:
			V_VT(result) = VT_I4;
			V_I4(result) = object->first_index;
			return S_OK;
		case MATCH_LENGTH:
			V_VT(result) = VT_I4;
			V_I4(result) = (LONG)SysStringLen(object->value);
			return S_OK;
		case MATCH_SUBMATCHES: {
			rx_object *sub = new_object(OBJECT_SUBMATCHES);
			sub->match = object;
			IDispatch_AddRef(iface);
			V_VT(result) = VT_DISPATCH;
			V_DISPATCH(result) = &sub->IDispatch_iface;
			return S_OK;
		}
		}
		return DISP_E_MEMBERNOTFOUND;
	case OBJECT_MATCHES:
	case OBJECT_SUBMATCHES:
		return collection_invoke(object, id, put, params, result, info);
	}
	return DISP_E_MEMBERNOTFOUND;
}

static IDispatchVtbl object_vtbl = {
	object_QueryInterface, object_AddRef, object_Release, object_GetTypeInfoCount, object_GetTypeInfo, object_GetIDsOfNames, object_Invoke,
};
