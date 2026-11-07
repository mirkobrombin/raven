/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * ITypeInfo2 and ITypeComp over the model.
 */
#include "typelib.h"

static const GUID id_dispatch_iface = {0x00020400, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

/* Flat copies: everything a description points at sits in the one block handed out. */
static SIZE_T align_size(SIZE_T size)
{
	return (size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
}

SIZE_T tl_desc_extra(const TYPEDESC *desc)
{
	SIZE_T size = 0;
	if (desc->vt == VT_PTR || desc->vt == VT_SAFEARRAY)
	{
		size = sizeof(TYPEDESC);
		if (desc->lptdesc)
			size += tl_desc_extra(desc->lptdesc);
	}
	else if (desc->vt == VT_CARRAY && desc->lpadesc)
	{
		size = align_size(sizeof(ARRAYDESC) + desc->lpadesc->cDims * sizeof(SAFEARRAYBOUND));
		size += tl_desc_extra(&desc->lpadesc->tdescElem);
	}
	return size;
}

void tl_desc_copy(TYPEDESC *to, const TYPEDESC *from, BYTE **room)
{
	*to = *from;
	if ((from->vt == VT_PTR || from->vt == VT_SAFEARRAY) && from->lptdesc)
	{
		to->lptdesc = (TYPEDESC *)*room;
		*room += sizeof(TYPEDESC);
		tl_desc_copy(to->lptdesc, from->lptdesc, room);
	}
	else if (from->vt == VT_CARRAY && from->lpadesc)
	{
		SIZE_T size = sizeof(ARRAYDESC) + from->lpadesc->cDims * sizeof(SAFEARRAYBOUND);
		to->lpadesc = (ARRAYDESC *)*room;
		memcpy(to->lpadesc, from->lpadesc, size);
		*room += align_size(size);
		tl_desc_copy(&to->lpadesc->tdescElem, &from->lpadesc->tdescElem, room);
	}
}

SIZE_T tl_elem_extra(const ELEMDESC *desc, BOOL has_flags)
{
	SIZE_T size = tl_desc_extra(&desc->tdesc);
	if (has_flags && desc->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT && desc->paramdesc.pparamdescex)
		size += sizeof(PARAMDESCEX);
	return size;
}

void tl_elem_copy(ELEMDESC *to, const ELEMDESC *from, BOOL has_flags, BYTE **room)
{
	*to = *from;
	tl_desc_copy(&to->tdesc, &from->tdesc, room);
	if (!has_flags)
		return;
	if (from->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT && from->paramdesc.pparamdescex)
	{
		PARAMDESCEX *copy = (PARAMDESCEX *)*room;
		*room += sizeof(PARAMDESCEX);
		copy->cBytes = sizeof(PARAMDESCEX);
		VariantInit(&copy->varDefaultValue);
		VariantCopy(&copy->varDefaultValue, &from->paramdesc.pparamdescex->varDefaultValue);
		to->paramdesc.pparamdescex = copy;
	}
	else
		to->paramdesc.pparamdescex = NULL;
}

/* A dual type, or a dispinterface defined by naming an interface, shows that interface's chain. */
static BOOL is_dual_dispatch(const struct tl_type *type)
{
	if (type->attr.typekind != TKIND_DISPATCH)
		return FALSE;
	if (type->attr.wTypeFlags & TYPEFLAG_FDUAL)
		return TRUE;
	return type->attr.cImplTypes && (type->impls[0].ref & ~3u) != (type->lib->dispatch_ref & ~3u);
}

static struct tl_type *base_type(struct tl_type *type)
{
	ITypeInfo *info = NULL;
	struct tl_type *base;
	if (!type->attr.cImplTypes || type->attr.typekind == TKIND_COCLASS)
		return NULL;
	if (FAILED(tl_ref_info(type, type->impls[0].ref, &info)) || !info)
		return NULL;
	if (!tl_is_ours(info))
	{
		ITypeInfo_Release(info);
		return NULL;
	}
	base = tl_type_from_info((ITypeInfo2 *)info);
	/* The library holds its types; the caller only looks. */
	ITypeInfo_Release(info);
	return base;
}

/* A dual dispatch face lists everything in its chain, the most basic interface first. */
static UINT visible_funcs(struct tl_type *type)
{
	if (is_dual_dispatch(type))
	{
		struct tl_type *base = base_type(type);
		UINT count = type->attr.cFuncs;
		while (base)
		{
			count += base->attr.cFuncs;
			base = base_type(base);
		}
		return count;
	}
	return type->attr.cFuncs;
}

static struct tl_func *visible_func(struct tl_type *type, UINT index, struct tl_type **owner)
{
	struct tl_type *chain[32];
	UINT depth = 0, level;
	struct tl_type *walk = type;
	*owner = type;
	if (!is_dual_dispatch(type))
		return index < type->attr.cFuncs ? &type->funcs[index] : NULL;
	while (walk && depth < ARRAYSIZE(chain))
	{
		chain[depth++] = walk;
		walk = base_type(walk);
	}
	for (level = depth; level-- > 0;)
	{
		if (index < chain[level]->attr.cFuncs)
		{
			*owner = chain[level];
			return &chain[level]->funcs[index];
		}
		index -= chain[level]->attr.cFuncs;
	}
	return NULL;
}

static HRESULT WINAPI info_QueryInterface(ITypeInfo2 *iface, REFIID riid, void **out)
{
	struct tl_type *type = tl_type_from_info(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ITypeInfo) || IsEqualIID(riid, &IID_ITypeInfo2))
		*out = &type->ITypeInfo2_iface;
	else if (IsEqualIID(riid, &IID_ICreateTypeInfo) || IsEqualIID(riid, &IID_ICreateTypeInfo2))
		*out = &type->ICreateTypeInfo2_iface;
	else if (IsEqualIID(riid, &IID_ITypeComp))
		*out = &type->ITypeComp_iface;
	if (!*out)
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

/* A type's first reference holds its library, and that is all that keeps the type alive. */
static ULONG WINAPI info_AddRef(ITypeInfo2 *iface)
{
	struct tl_type *type = tl_type_from_info(iface);
	ULONG count = InterlockedIncrement(&type->ref);
	if (count == 1)
		ITypeLib2_AddRef(&type->lib->ITypeLib2_iface);
	return count;
}

static ULONG WINAPI info_Release(ITypeInfo2 *iface)
{
	struct tl_type *type = tl_type_from_info(iface);
	ULONG count = InterlockedDecrement(&type->ref);
	if (!count)
		ITypeLib2_Release(&type->lib->ITypeLib2_iface);
	return count;
}

static HRESULT WINAPI info_GetTypeAttr(ITypeInfo2 *iface, TYPEATTR **out)
{
	struct tl_type *type = tl_type_from_info(iface);
	SIZE_T extra;
	TYPEATTR *attr;
	BYTE *room;
	if (!out)
		return E_INVALIDARG;
	tl_settle(type);
	extra = type->attr.typekind == TKIND_ALIAS ? tl_desc_extra(&type->attr.tdescAlias) : 0;
	attr = CoTaskMemAlloc(sizeof(*attr) + extra);
	if (!attr)
		return E_OUTOFMEMORY;
	*attr = type->attr;
	room = (BYTE *)(attr + 1);
	if (type->attr.typekind == TKIND_ALIAS)
		tl_desc_copy(&attr->tdescAlias, &type->attr.tdescAlias, &room);
	if (type->attr.typekind == TKIND_DISPATCH)
	{
		attr->cFuncs = (WORD)visible_funcs(type);
		attr->cbSizeVft = 7 * sizeof(void *);
		attr->cImplTypes = 1;
	}
	*out = attr;
	return S_OK;
}

static HRESULT WINAPI info_GetTypeComp(ITypeInfo2 *iface, ITypeComp **out)
{
	struct tl_type *type = tl_type_from_info(iface);
	if (!out)
		return E_INVALIDARG;
	*out = &type->ITypeComp_iface;
	ITypeComp_AddRef(*out);
	return S_OK;
}

static BOOL is_hresult_call(const FUNCDESC *desc)
{
	return desc->elemdescFunc.tdesc.vt == VT_HRESULT;
}

/* Hands out a description. Seen through a dispatch face, an interface method has no HRESULT:
 * its last out value becomes the return. */
#define INHERITED_REF_UNIT 0x04000000

/* A reference copied out of a base interface means something in that interface: it says how far up. */
static void mark_inherited(TYPEDESC *desc, UINT level)
{
	while (desc->vt == VT_PTR || desc->vt == VT_SAFEARRAY)
		desc = desc->lptdesc;
	if (desc->vt == VT_USERDEFINED)
		desc->hreftype |= level * INHERITED_REF_UNIT;
}

static HRESULT copy_func_desc(const struct tl_func *func, BOOL as_dispatch, FUNCDESC **out)
{
	const FUNCDESC *from = &func->desc;
	SIZE_T size = sizeof(FUNCDESC) + from->cParams * sizeof(ELEMDESC) + tl_elem_extra(&from->elemdescFunc, TRUE);
	SHORT index;
	FUNCDESC *copy;
	BYTE *room;
	for (index = 0; index < from->cParams; index++)
		size += tl_elem_extra(&from->lprgelemdescParam[index], TRUE);
	copy = CoTaskMemAlloc(size);
	if (!copy)
		return E_OUTOFMEMORY;
	*copy = *from;
	copy->lprgscode = NULL;
	copy->lprgelemdescParam = from->cParams ? (ELEMDESC *)(copy + 1) : NULL;
	room = (BYTE *)(copy + 1) + from->cParams * sizeof(ELEMDESC);
	tl_elem_copy(&copy->elemdescFunc, &from->elemdescFunc, TRUE, &room);
	for (index = 0; index < from->cParams; index++)
		tl_elem_copy(&copy->lprgelemdescParam[index], &from->lprgelemdescParam[index], TRUE, &room);
	if (from->funckind == FUNC_DISPATCH)
		copy->oVft = 0;
	if (as_dispatch && from->funckind != FUNC_DISPATCH)
	{
		SHORT kept = 0;
		copy->funckind = FUNC_DISPATCH;
		if (is_hresult_call(from))
			copy->elemdescFunc.tdesc.vt = VT_VOID;
		for (index = 0; index < from->cParams; index++)
		{
			USHORT param_flags = from->lprgelemdescParam[index].paramdesc.wParamFlags;
			if (param_flags & PARAMFLAG_FLCID)
				continue;
			if (param_flags & PARAMFLAG_FRETVAL)
			{
				if (copy->lprgelemdescParam[index].tdesc.vt == VT_PTR)
					copy->elemdescFunc.tdesc = *copy->lprgelemdescParam[index].tdesc.lptdesc;
				continue;
			}
			copy->lprgelemdescParam[kept++] = copy->lprgelemdescParam[index];
		}
		copy->cParams = kept;
	}
	*out = copy;
	return S_OK;
}

static HRESULT WINAPI info_GetFuncDesc(ITypeInfo2 *iface, UINT index, FUNCDESC **out)
{
	tl_settle(tl_type_from_info(iface));
	struct tl_type *type = tl_type_from_info(iface), *owner;
	const struct tl_func *func;
	if (!out)
		return E_INVALIDARG;
	if (type->attr.typekind == TKIND_ALIAS)
		return TYPE_E_ELEMENTNOTFOUND;
	func = visible_func(type, index, &owner);
	if (!func)
		return TYPE_E_ELEMENTNOTFOUND;
	{
		HRESULT status = copy_func_desc(func, type->attr.typekind == TKIND_DISPATCH, out);
		UINT level = 0;
		struct tl_type *walk = type;
		while (SUCCEEDED(status) && walk != owner && walk)
		{
			walk = base_type(walk);
			level++;
		}
		if (SUCCEEDED(status) && walk && level)
		{
			SHORT param;
			mark_inherited(&(*out)->elemdescFunc.tdesc, level);
			for (param = 0; param < (*out)->cParams; param++)
				mark_inherited(&(*out)->lprgelemdescParam[param].tdesc, level);
		}
		return status;
	}
}

static HRESULT WINAPI info_GetVarDesc(ITypeInfo2 *iface, UINT index, VARDESC **out)
{
	struct tl_type *type = tl_type_from_info(iface);
	tl_settle(type);
	const VARDESC *from;
	VARDESC *copy;
	SIZE_T size;
	BYTE *room;
	if (!out)
		return E_INVALIDARG;
	if (index >= type->attr.cVars)
		return TYPE_E_ELEMENTNOTFOUND;
	from = &type->vars[index].desc;
	size = sizeof(VARDESC) + tl_elem_extra(&from->elemdescVar, FALSE);
	if (from->varkind == VAR_CONST)
		size += sizeof(VARIANT);
	copy = CoTaskMemAlloc(size);
	if (!copy)
		return E_OUTOFMEMORY;
	*copy = *from;
	room = (BYTE *)(copy + 1);
	tl_elem_copy(&copy->elemdescVar, &from->elemdescVar, FALSE, &room);
	copy->elemdescVar.paramdesc = from->elemdescVar.paramdesc;
	if (from->varkind == VAR_CONST)
	{
		copy->lpvarValue = (VARIANT *)room;
		VariantInit(copy->lpvarValue);
		if (from->lpvarValue)
			VariantCopy(copy->lpvarValue, from->lpvarValue);
	}
	*out = copy;
	return S_OK;
}

static HRESULT WINAPI info_GetNames(ITypeInfo2 *iface, MEMBERID id, BSTR *names, UINT limit, UINT *count)
{
	tl_settle(tl_type_from_info(iface));
	struct tl_type *type = tl_type_from_info(iface), *owner;
	UINT index, total = visible_funcs(type);
	if (!names || !count)
		return E_INVALIDARG;
	*count = 0;
	for (index = 0; index < total; index++)
	{
		const struct tl_func *func = visible_func(type, index, &owner);
		if (func && func->desc.memid == id)
		{
			UINT have = 0;
			SHORT param;
			BOOL face = type->attr.typekind == TKIND_DISPATCH && func->desc.funckind != FUNC_DISPATCH;
			if (limit)
				names[have++] = SysAllocString(func->name);
			for (param = 0; param < func->desc.cParams && have < limit; param++)
			{
				/* Seen through the dispatch face, filled-in parameters have no name to pass. */
				if (face && func->desc.lprgelemdescParam[param].paramdesc.wParamFlags & (PARAMFLAG_FRETVAL | PARAMFLAG_FLCID))
					continue;
				if (!func->params[param].name)
					break;
				names[have++] = SysAllocString(func->params[param].name);
			}
			*count = have;
			return S_OK;
		}
	}
	for (index = 0; index < type->attr.cVars; index++)
		if (type->vars[index].desc.memid == id)
		{
			if (limit)
			{
				names[0] = SysAllocString(type->vars[index].name);
				*count = 1;
			}
			return S_OK;
		}
	{
		struct tl_type *base = base_type(type);
		if (base && type->attr.typekind != TKIND_DISPATCH)
			return info_GetNames(&base->ITypeInfo2_iface, id, names, limit, count);
	}
	return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI info_GetRefTypeOfImplType(ITypeInfo2 *iface, UINT index, HREFTYPE *out)
{
	struct tl_type *type = tl_type_from_info(iface);
	if (!out)
		return E_INVALIDARG;
	if (index == (UINT)-1)
	{
		if (!(type->attr.wTypeFlags & TYPEFLAG_FDUAL))
			return TYPE_E_ELEMENTNOTFOUND;
		*out = (HREFTYPE)-2;
		return S_OK;
	}
	if (!index && type->attr.typekind == TKIND_DISPATCH)
	{
		*out = type->lib->dispatch_ref;
		return S_OK;
	}
	if (index >= type->attr.cImplTypes)
		return TYPE_E_ELEMENTNOTFOUND;
	*out = type->impls[index].ref;
	if (type->attr.typekind == TKIND_INTERFACE)
		*out |= 2;
	return S_OK;
}

static HRESULT WINAPI info_GetImplTypeFlags(ITypeInfo2 *iface, UINT index, INT *out)
{
	struct tl_type *type = tl_type_from_info(iface);
	if (!out)
		return E_INVALIDARG;
	if (type->attr.typekind != TKIND_COCLASS)
	{
		if (index >= type->attr.cImplTypes)
			return TYPE_E_ELEMENTNOTFOUND;
		*out = 0;
		return S_OK;
	}
	if (index >= type->attr.cImplTypes)
		return TYPE_E_ELEMENTNOTFOUND;
	*out = type->impls[index].flags;
	return S_OK;
}

static HRESULT WINAPI info_GetIDsOfNames(ITypeInfo2 *iface, LPOLESTR *names, UINT count, MEMBERID *ids)
{
	tl_settle(tl_type_from_info(iface));
	struct tl_type *type = tl_type_from_info(iface), *owner;
	UINT index, total = visible_funcs(type), rest;
	if (!names || !ids)
		return E_INVALIDARG;
	for (rest = 0; rest < count; rest++)
		ids[rest] = MEMBERID_NIL;
	if (!count)
		return S_OK;
	if (type->attr.typekind == TKIND_COCLASS)
	{
		for (index = 0; index < type->attr.cImplTypes; index++)
		{
			ITypeInfo *info = NULL;
			HRESULT status;
			if (FAILED(tl_ref_info(type, type->impls[index].ref, &info)))
				continue;
			status = ITypeInfo_GetIDsOfNames(info, names, count, ids);
			ITypeInfo_Release(info);
			if (status != DISP_E_UNKNOWNNAME)
				return status;
		}
		return DISP_E_UNKNOWNNAME;
	}
	for (index = 0; index < total; index++)
	{
		const struct tl_func *func = visible_func(type, index, &owner);
		if (func && func->name && !lstrcmpiW(func->name, names[0]))
		{
			HRESULT status = S_OK;
			ids[0] = func->desc.memid;
			for (rest = 1; rest < count; rest++)
			{
				SHORT param;
				for (param = 0; param < func->desc.cParams; param++)
					if (func->params[param].name && !lstrcmpiW(func->params[param].name, names[rest]))
					{
						ids[rest] = param;
						break;
					}
				if (param == func->desc.cParams)
					status = DISP_E_UNKNOWNNAME;
			}
			return status;
		}
	}
	for (index = 0; index < type->attr.cVars; index++)
		if (type->vars[index].name && !lstrcmpiW(type->vars[index].name, names[0]))
		{
			ids[0] = type->vars[index].desc.memid;
			return count > 1 ? DISP_E_UNKNOWNNAME : S_OK;
		}
	if (type->attr.typekind != TKIND_DISPATCH)
	{
		struct tl_type *base = base_type(type);
		if (base)
			return info_GetIDsOfNames(&base->ITypeInfo2_iface, names, count, ids);
	}
	return DISP_E_UNKNOWNNAME;
}

/* What a declared type becomes as a variant type. */
HRESULT tl_vartype_of(struct tl_type *type, const TYPEDESC *desc, VARTYPE *out)
{
	VARTYPE inner;
	HRESULT status;
	switch (desc->vt)
	{
	case VT_PTR:
		status = tl_vartype_of(type, desc->lptdesc, &inner);
		if (FAILED(status))
			return status;
		*out = inner == VT_UNKNOWN || inner == VT_DISPATCH ? inner : inner | VT_BYREF;
		if ((inner == VT_UNKNOWN || inner == VT_DISPATCH) && desc->lptdesc->vt == VT_PTR)
			*out = inner | VT_BYREF;
		return S_OK;
	case VT_SAFEARRAY:
		status = tl_vartype_of(type, desc->lptdesc, &inner);
		if (FAILED(status))
			return status;
		*out = inner | VT_ARRAY;
		return S_OK;
	case VT_USERDEFINED:
	{
		ITypeInfo *info = NULL;
		TYPEATTR *attr = NULL;
		status = tl_ref_info(type, desc->hreftype, &info);
		if (SUCCEEDED(status))
			status = ITypeInfo_GetTypeAttr(info, &attr);
		if (SUCCEEDED(status))
		{
			switch (attr->typekind)
			{
			case TKIND_ENUM:
				*out = VT_I4;
				break;
			case TKIND_ALIAS:
				status = tl_vartype_of(tl_type_from_info((ITypeInfo2 *)info), &attr->tdescAlias, out);
				break;
			case TKIND_INTERFACE:
				*out = IsEqualGUID(&attr->guid, &IID_IUnknown) ? VT_UNKNOWN : attr->wTypeFlags & TYPEFLAG_FDISPATCHABLE ? VT_DISPATCH : VT_UNKNOWN;
				break;
			case TKIND_DISPATCH:
				*out = VT_DISPATCH;
				break;
			case TKIND_COCLASS:
				*out = VT_UNKNOWN;
				break;
			case TKIND_RECORD:
				*out = VT_RECORD;
				break;
			default:
				status = DISP_E_BADVARTYPE;
				break;
			}
			ITypeInfo_ReleaseTypeAttr(info, attr);
		}
		if (info)
			ITypeInfo_Release(info);
		return status;
	}
	default:
		*out = desc->vt;
		return S_OK;
	}
}

/* Calls one method through its vtable slot, translating the dispatch arguments. */
static HRESULT invoke_method(struct tl_type *owner, const struct tl_func *func, void *instance, WORD flags, DISPPARAMS *params, UINT extra, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	SHORT count = func->desc.cParams, index;
	VARIANTARG *values, **pointers, *holders, retained;
	VARTYPE *types;
	UINT given = params->cArgs, named = params->cNamedArgs, used = 0;
	void **targets;
	VARIANT call_result;
	VARTYPE return_type;
	SHORT ret_param = -1;
	HRESULT status = S_OK, called;
	UINT positional = given - named, consumed = 0, put_slot = named, slot;
	SHORT last_input = -1;
	for (slot = 0; slot < named; slot++)
		if (params->rgdispidNamedArgs && params->rgdispidNamedArgs[slot] == DISPID_PROPERTYPUT)
			put_slot = slot;
	for (index = count - 1; index >= 0; index--)
		if (!(func->desc.lprgelemdescParam[index].paramdesc.wParamFlags & (PARAMFLAG_FRETVAL | PARAMFLAG_FLCID)))
		{
			last_input = index;
			break;
		}
	values = tl_alloc((count + 1) * (sizeof(VARIANTARG) * 2 + sizeof(void *) * 2 + sizeof(VARTYPE)));
	if (!values)
		return E_OUTOFMEMORY;
	holders = values + count + 1;
	pointers = (VARIANTARG **)(holders + count + 1);
	targets = (void **)(pointers + count + 1);
	types = (VARTYPE *)(targets + count + 1);
	VariantInit(&retained);
	for (index = 0; index < count && SUCCEEDED(status); index++)
	{
		const ELEMDESC *elem = &func->desc.lprgelemdescParam[index];
		USHORT param_flags = elem->paramdesc.wParamFlags;
		VARTYPE wanted;
		VARIANTARG *value = &values[index], *source;
		status = tl_vartype_of(owner, &elem->tdesc, &wanted);
		if (FAILED(status))
			break;
		VariantInit(value);
		VariantInit(&holders[index]);
		pointers[index] = value;
		types[index] = wanted;
		if (param_flags & PARAMFLAG_FLCID)
		{
			V_VT(value) = VT_I4;
			V_I4(value) = LOCALE_USER_DEFAULT;
			types[index] = VT_I4;
			continue;
		}
		if (param_flags & PARAMFLAG_FRETVAL)
		{
			/* The return comes back through a holder of the pointed-to type. */
			VARTYPE inner = wanted & ~VT_BYREF;
			ret_param = index;
			if (!(wanted & VT_BYREF))
			{
				inner = wanted;
				types[index] = wanted | VT_BYREF;
			}
			V_VT(&holders[index]) = inner;
			V_VT(value) = types[index];
			V_BYREF(value) = inner == VT_VARIANT ? (void *)&holders[index] : (void *)&V_I4(&holders[index]);
			continue;
		}
		source = NULL;
		for (slot = 0; slot < named && !source; slot++)
			if (params->rgdispidNamedArgs && params->rgdispidNamedArgs[slot] == index)
				source = &params->rgvarg[slot];
		if (!source && put_slot < named && index == last_input)
			source = &params->rgvarg[put_slot];
		if (!source && consumed < positional)
			source = &params->rgvarg[given - 1 - consumed++];
		if (source)
		{
			used++;
			if (V_VT(source) == VT_ERROR && V_ERROR(source) == DISP_E_PARAMNOTFOUND && param_flags & PARAMFLAG_FHASDEFAULT && elem->paramdesc.pparamdescex)
				source = &elem->paramdesc.pparamdescex->varDefaultValue;
			if (wanted == VT_VARIANT)
			{
				status = VariantCopy(value, source);
				if (SUCCEEDED(status))
				{
					holders[index] = *value;
					types[index] = VT_VARIANT;
					pointers[index] = &holders[index];
					V_VT(value) = VT_VARIANT;
					V_BYREF(value) = &holders[index];
				}
			}
			else if (wanted == (VT_VARIANT | VT_BYREF))
			{
				V_VT(value) = wanted;
				if (V_VT(source) == (VT_VARIANT | VT_BYREF))
					V_BYREF(value) = V_BYREF(source);
				else
					V_BYREF(value) = source;
			}
			else if (wanted & VT_BYREF && V_VT(source) == wanted)
				*value = *source;
			else if (wanted & VT_BYREF)
			{
				/* A by-value caller for a by-reference parameter: pass a local copy. */
				status = VariantChangeType(&holders[index], source, 0, wanted & ~VT_BYREF);
				if (SUCCEEDED(status))
				{
					V_VT(value) = wanted;
					V_BYREF(value) = &V_I4(&holders[index]);
				}
			}
			else
				status = VariantChangeType(value, source, 0, wanted);
			if (FAILED(status))
			{
				if (arg_err)
					*arg_err = given - used;
			}
		}
		else if (param_flags & PARAMFLAG_FHASDEFAULT && elem->paramdesc.pparamdescex)
			status = VariantChangeType(value, &elem->paramdesc.pparamdescex->varDefaultValue, 0, wanted);
		else if (param_flags & PARAMFLAG_FOPT)
		{
			V_VT(value) = VT_ERROR;
			V_ERROR(value) = DISP_E_PARAMNOTFOUND;
			types[index] = wanted == VT_VARIANT ? VT_VARIANT : VT_ERROR;
			if (wanted == VT_VARIANT)
			{
				holders[index] = *value;
				V_VT(value) = VT_VARIANT;
				V_BYREF(value) = &holders[index];
			}
		}
		else
			status = DISP_E_BADPARAMCOUNT;
	}
	if (SUCCEEDED(status) && used + extra < given)
		status = DISP_E_BADPARAMCOUNT;
	if (SUCCEEDED(status))
	{
		for (index = 0; index < count; index++)
		{
			pointers[index] = &values[index];
			if (types[index] == VT_VARIANT)
				pointers[index] = (VARIANTARG *)V_BYREF(&values[index]);
		}
		status = tl_vartype_of(owner, &func->desc.elemdescFunc.tdesc, &return_type);
		/* The raw call treats a failure code as plain data; only a real HRESULT return is looked at below. */
		if (return_type == VT_HRESULT)
			return_type = VT_ERROR;
	}
	if (SUCCEEDED(status))
	{
		VariantInit(&call_result);
		status = DispCallFunc(instance, func->desc.oVft, func->desc.callconv, return_type, count, types, pointers, &call_result);
	}
	if (SUCCEEDED(status))
	{
		called = func->desc.elemdescFunc.tdesc.vt == VT_HRESULT ? V_ERROR(&call_result) : S_OK;
		if (FAILED(called))
		{
			IErrorInfo *error = NULL;
			if (excep)
			{
				memset(excep, 0, sizeof(*excep));
				excep->scode = called;
				if (GetErrorInfo(0, &error) == S_OK && error)
				{
					IErrorInfo_GetSource(error, &excep->bstrSource);
					IErrorInfo_GetDescription(error, &excep->bstrDescription);
					IErrorInfo_GetHelpFile(error, &excep->bstrHelpFile);
					IErrorInfo_GetHelpContext(error, &excep->dwHelpContext);
					IErrorInfo_Release(error);
				}
			}
			status = DISP_E_EXCEPTION;
		}
		else if (result)
		{
			if (ret_param >= 0)
			{
				/* The callee filled the holder and now owns nothing: the caller gets it. */
				if (V_VT(&holders[ret_param]) == VT_VARIANT)
					status = VariantCopy(result, &holders[ret_param]);
				else
				{
					*result = holders[ret_param];
					VariantInit(&holders[ret_param]);
					if (V_VT(result) == VT_INT)
						V_VT(result) = VT_I4;
					else if (V_VT(result) == VT_UINT)
						V_VT(result) = VT_UI4;
				}
			}
			else if (func->desc.elemdescFunc.tdesc.vt != VT_HRESULT && return_type != VT_VOID)
			{
				*result = call_result;
				VariantInit(&call_result);
				/* Automation has no platform sized integers. */
				V_VT(result) = return_type == VT_INT ? VT_I4 : return_type == VT_UINT ? VT_UI4 : return_type;
			}
		}
	}
	for (index = 0; index < count; index++)
	{
		if (!(types[index] & VT_BYREF) && types[index] != VT_VARIANT && V_VT(&values[index]) != VT_EMPTY)
			VariantClear(&values[index]);
		if (index != ret_param && V_VT(&holders[index]) != VT_EMPTY)
			VariantClear(&holders[index]);
	}
	tl_free(values);
	return status;
}

/* Parameters a caller actually supplies: return values and locales are filled in. */
static SHORT input_params(const struct tl_func *func)
{
	SHORT index, count = 0;
	for (index = 0; index < func->desc.cParams; index++)
		if (!(func->desc.lprgelemdescParam[index].paramdesc.wParamFlags & (PARAMFLAG_FRETVAL | PARAMFLAG_FLCID)))
			count++;
	return count;
}

static BOOL returns_dispatch(const struct tl_type *owner, const struct tl_func *func)
{
	SHORT count = func->desc.cParams;
	const TYPEDESC *desc;
	VARTYPE type;
	if (!count || !(func->desc.lprgelemdescParam[count - 1].paramdesc.wParamFlags & PARAMFLAG_FRETVAL))
		return FALSE;
	desc = &func->desc.lprgelemdescParam[count - 1].tdesc;
	if (FAILED(tl_vartype_of((struct tl_type *)owner, desc, &type)))
		return FALSE;
	return (type & ~VT_BYREF) == VT_DISPATCH;
}

static HRESULT invoke_found(struct tl_type *owner, const struct tl_func *func, void *instance, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	UINT extra = 0, inputs = input_params(func);
	HRESULT status;
	IDispatch *object;
	DISPPARAMS rest;
	VARIANT value;
	/* Arguments past the parameters index whatever a property getter returns. */
	if (flags & DISPATCH_PROPERTYGET && !params->cNamedArgs && params->cArgs > inputs && returns_dispatch(owner, func))
		extra = params->cArgs - inputs;
	if (!extra)
		return invoke_method(owner, func, instance, flags, params, 0, result, excep, arg_err);
	/* Trailing arguments sit first in the array, and the call must look at the leading ones only. */
	rest = *params;
	rest.rgvarg += extra;
	rest.cArgs -= extra;
	VariantInit(&value);
	status = invoke_method(owner, func, instance, flags, &rest, 0, &value, excep, arg_err);
	if (FAILED(status))
		return status;
	if (V_VT(&value) != VT_DISPATCH || !V_DISPATCH(&value))
	{
		VariantClear(&value);
		return DISP_E_NOTACOLLECTION;
	}
	object = V_DISPATCH(&value);
	rest.rgvarg = params->rgvarg;
	rest.cArgs = extra;
	rest.cNamedArgs = 0;
	rest.rgdispidNamedArgs = NULL;
	status = IDispatch_Invoke(object, DISPID_VALUE, &IID_NULL, LOCALE_USER_DEFAULT, flags, &rest, result, excep, arg_err);
	IDispatch_Release(object);
	return status;
}

static HRESULT WINAPI info_Invoke(ITypeInfo2 *iface, void *instance, MEMBERID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	struct tl_type *type = tl_type_from_info(iface), *owner;
	UINT index, total = visible_funcs(type);
	const struct tl_func *getter = NULL;
	/* Restricted members are simply not there, whatever else is wrong with the call. */
	for (index = 0; index < total; index++)
	{
		const struct tl_func *func = visible_func(type, index, &owner);
		if (func && func->desc.memid == id && func->desc.wFuncFlags & FUNCFLAG_FRESTRICTED)
			return DISP_E_MEMBERNOTFOUND;
	}
	if (!params || params->cNamedArgs > params->cArgs)
		return E_INVALIDARG;
	if (type->attr.typekind == TKIND_COCLASS)
	{
		/* A class answers through the interfaces it lists. */
		for (index = 0; index < type->attr.cImplTypes; index++)
		{
			ITypeInfo *info = NULL;
			HRESULT status = tl_ref_info(type, type->impls[index].ref, &info);
			if (FAILED(status))
				continue;
			status = ITypeInfo_Invoke(info, instance, id, flags, params, result, excep, arg_err);
			ITypeInfo_Release(info);
			if (status != DISP_E_MEMBERNOTFOUND)
				return status;
		}
		return DISP_E_MEMBERNOTFOUND;
	}
	for (index = 0; index < total; index++)
	{
		const struct tl_func *func = visible_func(type, index, &owner);
		if (!func || func->desc.memid != id)
			continue;
		if (func->desc.invkind == INVOKE_PROPERTYGET)
			getter = func;
		if (!(func->desc.invkind & flags))
			continue;
		if ((params->cArgs && !params->rgvarg) || !instance)
			return E_INVALIDARG;
		if (result)
			VariantInit(result);
		if (func->desc.funckind == FUNC_DISPATCH)
		{
			IDispatch *dispatch = NULL;
			HRESULT status = IUnknown_QueryInterface((IUnknown *)instance, &id_dispatch_iface, (void **)&dispatch);
			if (SUCCEEDED(status))
			{
				status = IDispatch_Invoke(dispatch, id, &IID_NULL, LOCALE_USER_DEFAULT, flags, params, result, excep, arg_err);
				IDispatch_Release(dispatch);
			}
			return status;
		}
		if (func->desc.funckind != FUNC_PUREVIRTUAL && func->desc.funckind != FUNC_VIRTUAL)
			return DISP_E_MEMBERNOTFOUND;
		return invoke_found(owner, func, instance, flags, params, result, excep, arg_err);
	}
	/* Putting to a property that only reads is told apart by whether the count would have fit a put. */
	if (getter && flags & DISPATCH_PROPERTYPUT)
	{
		SHORT inputs = input_params(getter);
		if (!inputs || params->cArgs == (UINT)inputs + 1)
			return DISP_E_BADPARAMCOUNT;
		return DISP_E_MEMBERNOTFOUND;
	}
	if (type->attr.typekind != TKIND_DISPATCH)
	{
		struct tl_type *base = base_type(type);
		if (base)
			return info_Invoke(&base->ITypeInfo2_iface, instance, id, flags, params, result, excep, arg_err);
	}
	return DISP_E_MEMBERNOTFOUND;
}

static void hand_out(BSTR *out, const WCHAR *value)
{
	if (out)
		*out = value ? SysAllocString(value) : NULL;
}

static HRESULT WINAPI info_GetDocumentation(ITypeInfo2 *iface, MEMBERID id, BSTR *name, BSTR *doc, DWORD *context, BSTR *file)
{
	tl_settle(tl_type_from_info(iface));
	struct tl_type *type = tl_type_from_info(iface), *owner;
	UINT index, total = visible_funcs(type);
	if (id == MEMBERID_NIL)
	{
		hand_out(name, type->name);
		hand_out(doc, type->doc);
		if (context)
			*context = type->help_context;
		hand_out(file, type->lib->help_file);
		return S_OK;
	}
	for (index = 0; index < total; index++)
	{
		const struct tl_func *func = visible_func(type, index, &owner);
		if (func && func->desc.memid == id)
		{
			hand_out(name, func->name);
			hand_out(doc, func->doc);
			if (context)
				*context = func->help_context;
			hand_out(file, type->lib->help_file);
			return S_OK;
		}
	}
	for (index = 0; index < type->attr.cVars; index++)
		if (type->vars[index].desc.memid == id)
		{
			hand_out(name, type->vars[index].name);
			hand_out(doc, type->vars[index].doc);
			if (context)
				*context = type->vars[index].help_context;
			hand_out(file, type->lib->help_file);
			return S_OK;
		}
	{
		struct tl_type *base = base_type(type);
		if (base && type->attr.typekind != TKIND_DISPATCH)
			return info_GetDocumentation(&base->ITypeInfo2_iface, id, name, doc, context, file);
	}
	return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI info_GetDllEntry(ITypeInfo2 *iface, MEMBERID id, INVOKEKIND kind, BSTR *dll, BSTR *entry, WORD *ordinal)
{
	struct tl_type *type = tl_type_from_info(iface);
	tl_settle(type);
	UINT index;
	if (dll)
		*dll = NULL;
	if (entry)
		*entry = NULL;
	if (ordinal)
		*ordinal = 0;
	if (type->attr.typekind != TKIND_MODULE)
		return TYPE_E_BADMODULEKIND;
	for (index = 0; index < type->attr.cFuncs; index++)
	{
		const struct tl_func *func = &type->funcs[index];
		if (func->desc.memid == id && func->desc.invkind == kind)
		{
			hand_out(dll, type->dll);
			if (func->entry)
				hand_out(entry, func->entry);
			else if (ordinal)
				*ordinal = func->ordinal;
			return S_OK;
		}
	}
	return TYPE_E_ELEMENTNOTFOUND;
}

/* The other face of a dual interface shares all contents. */
static struct tl_type *other_face(struct tl_type *type)
{
	struct tl_type *face;
	if (type->other_face)
		return type->other_face;
	face = tl_alloc(sizeof(*face));
	if (!face)
		return NULL;
	*face = *type;
	face->ref = 0;
	face->other_face = type;
	face->is_other_face = TRUE;
	face->attr.typekind = type->attr.typekind == TKIND_DISPATCH ? TKIND_INTERFACE : TKIND_DISPATCH;
	/* Only the interface side of a dual type is itself an automation interface. */
	face->attr.wTypeFlags = type->attr.typekind == TKIND_DISPATCH ? type->attr.wTypeFlags | type->automation_flag : type->attr.wTypeFlags & ~TYPEFLAG_FOLEAUTOMATION;
	type->other_face = face;
	return face;
}

HRESULT tl_ref_info(struct tl_type *type, HREFTYPE ref, ITypeInfo **out)
{
	struct tl_lib *lib = type->lib;
	HRESULT status;
	*out = NULL;
	if ((INT)ref >= INHERITED_REF_UNIT)
	{
		UINT level = ref / INHERITED_REF_UNIT;
		struct tl_type *owner = type;
		while (level-- && owner)
			owner = base_type(owner);
		return owner ? tl_ref_info(owner, ref & (INHERITED_REF_UNIT - 1), out) : TYPE_E_ELEMENTNOTFOUND;
	}
	if ((INT)ref < 0)
	{
		struct tl_type *face;
		if (!(type->attr.wTypeFlags & TYPEFLAG_FDUAL))
			return TYPE_E_ELEMENTNOTFOUND;
		face = other_face(type);
		if (!face)
			return E_OUTOFMEMORY;
		*out = (ITypeInfo *)&face->ITypeInfo2_iface;
		ITypeInfo_AddRef(*out);
		return S_OK;
	}
	if (ref & 1)
	{
		struct tl_import *import;
		UINT index = (ref & ~3u) / 12;
		if (index >= lib->import_count)
			return TYPE_E_ELEMENTNOTFOUND;
		import = &lib->imports[index];
		if (!import->loaded)
		{
			status = tl_find_loaded(&import->library, import->major, import->minor, import->lcid, &import->loaded);
			if (FAILED(status))
				status = LoadRegTypeLib(&import->library, import->major, import->minor, import->lcid, &import->loaded);
			if (FAILED(status) && import->path)
				status = LoadTypeLibEx(import->path, REGKIND_NONE, &import->loaded);
			if (FAILED(status))
				return status;
		}
		return import->by_guid ? ITypeLib_GetTypeInfoOfGuid(import->loaded, &import->type, out) : ITypeLib_GetTypeInfo(import->loaded, import->index, out);
	}
	{
		/* Types in the library are named by their place, and a tag asks for the interface face. */
		UINT index = (ref & ~3u) / 0x64;
		struct tl_type *found;
		if (index >= lib->type_count)
			return TYPE_E_ELEMENTNOTFOUND;
		found = lib->types[index];
		if (ref & 2 && found->attr.wTypeFlags & TYPEFLAG_FDUAL && found->attr.typekind == TKIND_DISPATCH)
		{
			found = other_face(found);
			if (!found)
				return E_OUTOFMEMORY;
		}
		*out = (ITypeInfo *)&found->ITypeInfo2_iface;
		ITypeInfo_AddRef(*out);
		return S_OK;
	}
}

static HRESULT WINAPI info_GetRefTypeInfo(ITypeInfo2 *iface, HREFTYPE ref, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return tl_ref_info(tl_type_from_info(iface), ref, out);
}

static HRESULT WINAPI info_AddressOfMember(ITypeInfo2 *iface, MEMBERID id, INVOKEKIND kind, void **out)
{
	BSTR dll = NULL, entry = NULL;
	WORD ordinal = 0;
	HMODULE module;
	HRESULT status;
	char name[256];
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	status = info_GetDllEntry(iface, id, kind, &dll, &entry, &ordinal);
	if (FAILED(status))
		return status;
	module = LoadLibraryW(dll);
	if (!module)
		status = STG_E_FILENOTFOUND;
	else
	{
		if (entry)
			WideCharToMultiByte(CP_ACP, 0, entry, -1, name, sizeof(name), NULL, NULL);
		*out = (void *)GetProcAddress(module, entry ? name : (const char *)(ULONG_PTR)ordinal);
		if (!*out)
			status = TYPE_E_DLLFUNCTIONNOTFOUND;
	}
	SysFreeString(dll);
	SysFreeString(entry);
	return status;
}

static HRESULT WINAPI info_CreateInstance(ITypeInfo2 *iface, IUnknown *outer, REFIID riid, void **out)
{
	struct tl_type *type = tl_type_from_info(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	if (type->attr.typekind != TKIND_COCLASS)
		return TYPE_E_BADMODULEKIND;
	if (type->attr.wTypeFlags & TYPEFLAG_FAPPOBJECT)
	{
		IUnknown *active = NULL;
		HRESULT status = GetActiveObject(&type->attr.guid, NULL, &active);
		if (SUCCEEDED(status))
		{
			status = IUnknown_QueryInterface(active, riid, out);
			IUnknown_Release(active);
			return status;
		}
	}
	return CoCreateInstance(&type->attr.guid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, riid, out);
}

static HRESULT WINAPI info_GetMops(ITypeInfo2 *iface, MEMBERID id, BSTR *out)
{
	if (out)
		*out = NULL;
	return S_OK;
}

static HRESULT WINAPI info_GetContainingTypeLib(ITypeInfo2 *iface, ITypeLib **lib, UINT *index)
{
	struct tl_type *type = tl_type_from_info(iface);
	if (index)
		*index = type->index;
	if (lib)
	{
		*lib = (ITypeLib *)&type->lib->ITypeLib2_iface;
		ITypeLib_AddRef(*lib);
	}
	return S_OK;
}

static void WINAPI info_ReleaseTypeAttr(ITypeInfo2 *iface, TYPEATTR *attr)
{
	CoTaskMemFree(attr);
}

static void WINAPI info_ReleaseFuncDesc(ITypeInfo2 *iface, FUNCDESC *desc)
{
	SHORT index, count;
	if (!desc)
		return;
	count = desc->cParams;
	for (index = 0; index < count; index++)
		if (desc->lprgelemdescParam[index].paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT && desc->lprgelemdescParam[index].paramdesc.pparamdescex)
			VariantClear(&desc->lprgelemdescParam[index].paramdesc.pparamdescex->varDefaultValue);
	CoTaskMemFree(desc);
}

static void WINAPI info_ReleaseVarDesc(ITypeInfo2 *iface, VARDESC *desc)
{
	if (desc && desc->varkind == VAR_CONST && desc->lpvarValue)
		VariantClear(desc->lpvarValue);
	CoTaskMemFree(desc);
}

static HRESULT WINAPI info_GetTypeKind(ITypeInfo2 *iface, TYPEKIND *out)
{
	*out = tl_type_from_info(iface)->attr.typekind;
	return S_OK;
}

static HRESULT WINAPI info_GetTypeFlags(ITypeInfo2 *iface, ULONG *out)
{
	*out = tl_type_from_info(iface)->attr.wTypeFlags;
	return S_OK;
}

static HRESULT WINAPI info_GetFuncIndexOfMemId(ITypeInfo2 *iface, MEMBERID id, INVOKEKIND kind, UINT *out)
{
	tl_settle(tl_type_from_info(iface));
	struct tl_type *type = tl_type_from_info(iface), *owner;
	UINT index, total = visible_funcs(type);
	for (index = 0; index < total; index++)
	{
		const struct tl_func *func = visible_func(type, index, &owner);
		if (func && func->desc.memid == id && func->desc.invkind == kind)
		{
			*out = index;
			return S_OK;
		}
	}
	return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI info_GetVarIndexOfMemId(ITypeInfo2 *iface, MEMBERID id, UINT *out)
{
	struct tl_type *type = tl_type_from_info(iface);
	tl_settle(type);
	UINT index;
	for (index = 0; index < type->attr.cVars; index++)
		if (type->vars[index].desc.memid == id)
		{
			*out = index;
			return S_OK;
		}
	return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT custom_get(const struct tl_customs *list, REFGUID guid, VARIANT *out)
{
	UINT index;
	if (!guid || !out)
		return E_INVALIDARG;
	VariantInit(out);
	for (index = 0; index < list->count; index++)
		if (IsEqualGUID(&list->items[index].guid, guid))
			return VariantCopy(out, &list->items[index].value);
	return S_OK;
}

static HRESULT customs_get_all(const struct tl_customs *list, CUSTDATA *out)
{
	UINT index;
	out->cCustData = list->count;
	out->prgCustData = NULL;
	if (!list->count)
		return S_OK;
	out->prgCustData = CoTaskMemAlloc(list->count * sizeof(CUSTDATAITEM));
	if (!out->prgCustData)
		return E_OUTOFMEMORY;
	for (index = 0; index < list->count; index++)
	{
		out->prgCustData[index].guid = list->items[index].guid;
		VariantInit(&out->prgCustData[index].varValue);
		VariantCopy(&out->prgCustData[index].varValue, &list->items[index].value);
	}
	return S_OK;
}

HRESULT tl_customs_get(const struct tl_customs *list, REFGUID guid, VARIANT *out)
{
	return custom_get(list, guid, out);
}

HRESULT tl_customs_get_all(const struct tl_customs *list, CUSTDATA *out)
{
	return customs_get_all(list, out);
}

static HRESULT WINAPI info_GetCustData(ITypeInfo2 *iface, REFGUID guid, VARIANT *out)
{
	return custom_get(&tl_type_from_info(iface)->custom, guid, out);
}

static HRESULT WINAPI info_GetFuncCustData(ITypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *out)
{
	struct tl_type *type = tl_type_from_info(iface), *owner;
	const struct tl_func *func = visible_func(type, index, &owner);
	return func ? custom_get(&func->custom, guid, out) : TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI info_GetParamCustData(ITypeInfo2 *iface, UINT index, UINT param, REFGUID guid, VARIANT *out)
{
	struct tl_type *type = tl_type_from_info(iface), *owner;
	const struct tl_func *func = visible_func(type, index, &owner);
	if (!func || param >= (UINT)func->desc.cParams)
		return TYPE_E_ELEMENTNOTFOUND;
	return custom_get(&func->params[param].custom, guid, out);
}

static HRESULT WINAPI info_GetVarCustData(ITypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *out)
{
	struct tl_type *type = tl_type_from_info(iface);
	return index < type->attr.cVars ? custom_get(&type->vars[index].custom, guid, out) : TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI info_GetImplTypeCustData(ITypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *out)
{
	struct tl_type *type = tl_type_from_info(iface);
	return index < type->attr.cImplTypes ? custom_get(&type->impls[index].custom, guid, out) : TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI info_GetDocumentation2(ITypeInfo2 *iface, MEMBERID id, LCID lcid, BSTR *text, DWORD *context, BSTR *dll)
{
	tl_settle(tl_type_from_info(iface));
	struct tl_type *type = tl_type_from_info(iface), *owner;
	UINT index, total = visible_funcs(type);
	if (text)
		*text = NULL;
	if (context)
		*context = 0;
	if (dll)
		*dll = NULL;
	if (id == MEMBERID_NIL)
	{
		hand_out(text, type->doc);
		if (context)
			*context = type->string_context;
		hand_out(dll, type->lib->help_dll);
		return S_OK;
	}
	for (index = 0; index < total; index++)
	{
		const struct tl_func *func = visible_func(type, index, &owner);
		if (func && func->desc.memid == id)
		{
			hand_out(text, func->doc);
			if (context)
				*context = func->string_context;
			hand_out(dll, type->lib->help_dll);
			return S_OK;
		}
	}
	for (index = 0; index < type->attr.cVars; index++)
		if (type->vars[index].desc.memid == id)
		{
			hand_out(text, type->vars[index].doc);
			if (context)
				*context = type->vars[index].string_context;
			hand_out(dll, type->lib->help_dll);
			return S_OK;
		}
	return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI info_GetAllCustData(ITypeInfo2 *iface, CUSTDATA *out)
{
	return customs_get_all(&tl_type_from_info(iface)->custom, out);
}

static HRESULT WINAPI info_GetAllFuncCustData(ITypeInfo2 *iface, UINT index, CUSTDATA *out)
{
	struct tl_type *type = tl_type_from_info(iface), *owner;
	const struct tl_func *func = visible_func(type, index, &owner);
	return func ? customs_get_all(&func->custom, out) : TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI info_GetAllParamCustData(ITypeInfo2 *iface, UINT index, UINT param, CUSTDATA *out)
{
	struct tl_type *type = tl_type_from_info(iface), *owner;
	const struct tl_func *func = visible_func(type, index, &owner);
	if (!func || param >= (UINT)func->desc.cParams)
		return TYPE_E_ELEMENTNOTFOUND;
	return customs_get_all(&func->params[param].custom, out);
}

static HRESULT WINAPI info_GetAllVarCustData(ITypeInfo2 *iface, UINT index, CUSTDATA *out)
{
	struct tl_type *type = tl_type_from_info(iface);
	return index < type->attr.cVars ? customs_get_all(&type->vars[index].custom, out) : TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI info_GetAllImplTypeCustData(ITypeInfo2 *iface, UINT index, CUSTDATA *out)
{
	struct tl_type *type = tl_type_from_info(iface);
	return index < type->attr.cImplTypes ? customs_get_all(&type->impls[index].custom, out) : TYPE_E_ELEMENTNOTFOUND;
}

const ITypeInfo2Vtbl tl_info_vtbl = {
	info_QueryInterface,
	info_AddRef,
	info_Release,
	info_GetTypeAttr,
	info_GetTypeComp,
	info_GetFuncDesc,
	info_GetVarDesc,
	info_GetNames,
	info_GetRefTypeOfImplType,
	info_GetImplTypeFlags,
	info_GetIDsOfNames,
	info_Invoke,
	info_GetDocumentation,
	info_GetDllEntry,
	info_GetRefTypeInfo,
	info_AddressOfMember,
	info_CreateInstance,
	info_GetMops,
	info_GetContainingTypeLib,
	info_ReleaseTypeAttr,
	info_ReleaseFuncDesc,
	info_ReleaseVarDesc,
	info_GetTypeKind,
	info_GetTypeFlags,
	info_GetFuncIndexOfMemId,
	info_GetVarIndexOfMemId,
	info_GetCustData,
	info_GetFuncCustData,
	info_GetParamCustData,
	info_GetVarCustData,
	info_GetImplTypeCustData,
	info_GetDocumentation2,
	info_GetAllCustData,
	info_GetAllFuncCustData,
	info_GetAllParamCustData,
	info_GetAllVarCustData,
	info_GetAllImplTypeCustData,
};

BOOL tl_is_ours(ITypeInfo *info)
{
	return info && info->lpVtbl == (const ITypeInfoVtbl *)&tl_info_vtbl;
}

/* ITypeComp of a type: members of the type and of the interfaces it builds on. */
static inline struct tl_type *type_from_comp(ITypeComp *iface)
{
	return CONTAINING_RECORD(iface, struct tl_type, ITypeComp_iface);
}

static HRESULT WINAPI info_comp_QueryInterface(ITypeComp *iface, REFIID riid, void **out)
{
	return info_QueryInterface(&type_from_comp(iface)->ITypeInfo2_iface, riid, out);
}

static ULONG WINAPI info_comp_AddRef(ITypeComp *iface)
{
	return info_AddRef(&type_from_comp(iface)->ITypeInfo2_iface);
}

static ULONG WINAPI info_comp_Release(ITypeComp *iface)
{
	return info_Release(&type_from_comp(iface)->ITypeInfo2_iface);
}

static HRESULT WINAPI info_comp_Bind(ITypeComp *iface, LPOLESTR name, ULONG hash, WORD flags, ITypeInfo **info, DESCKIND *kind, BINDPTR *bind)
{
	struct tl_type *type = type_from_comp(iface), *owner;
	UINT index, total = visible_funcs(type);
	BOOL mismatch = FALSE;
	HRESULT status;
	if (!name || !info || !kind || !bind)
		return E_INVALIDARG;
	*info = NULL;
	*kind = DESCKIND_NONE;
	bind->lpfuncdesc = NULL;
	for (index = 0; index < total; index++)
	{
		const struct tl_func *func = visible_func(type, index, &owner);
		if (!func || !func->name || lstrcmpiW(func->name, name))
			continue;
		if (flags && !(func->desc.invkind & flags))
		{
			mismatch = TRUE;
			continue;
		}
		status = copy_func_desc(func, type->attr.typekind == TKIND_DISPATCH, &bind->lpfuncdesc);
		if (FAILED(status))
			return status;
		*kind = DESCKIND_FUNCDESC;
		*info = (ITypeInfo *)&type->ITypeInfo2_iface;
		ITypeInfo_AddRef(*info);
		return S_OK;
	}
	for (index = 0; index < type->attr.cVars; index++)
		if (type->vars[index].name && !lstrcmpiW(type->vars[index].name, name))
		{
			status = info_GetVarDesc(&type->ITypeInfo2_iface, index, &bind->lpvardesc);
			if (FAILED(status))
				return status;
			*kind = DESCKIND_VARDESC;
			*info = (ITypeInfo *)&type->ITypeInfo2_iface;
			ITypeInfo_AddRef(*info);
			return S_OK;
		}
	if (mismatch)
		return TYPE_E_TYPEMISMATCH;
	if (type->attr.typekind != TKIND_DISPATCH)
	{
		struct tl_type *base = base_type(type);
		if (base)
			return info_comp_Bind(&base->ITypeComp_iface, name, hash, flags, info, kind, bind);
	}
	return S_OK;
}

static HRESULT WINAPI info_comp_BindType(ITypeComp *iface, LPOLESTR name, ULONG hash, ITypeInfo **info, ITypeComp **comp)
{
	if (!name || !info || !comp)
		return E_INVALIDARG;
	*info = NULL;
	*comp = NULL;
	return S_OK;
}

const ITypeCompVtbl tl_info_comp_vtbl = {
	info_comp_QueryInterface,
	info_comp_AddRef,
	info_comp_Release,
	info_comp_Bind,
	info_comp_BindType,
};
