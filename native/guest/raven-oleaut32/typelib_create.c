/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * ICreateTypeLib2 and ICreateTypeInfo2 edit the same model the readers look at. What depends on the
 * whole of a type, generated ids and sizes, settles only when somebody looks.
 */
#include "typelib.h"

#define GENERATED_FUNC_ID 0x60000000
#define GENERATED_VAR_ID 0x40000000
#define DISPATCH_METHOD_COUNT 7
#define DEFAULT_CREATED_ALIGNMENT 4

static const GUID id_dispatch = {0x00020400, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

static UINT pointer_size(const struct tl_lib *lib)
{
	return lib->attr.syskind == SYS_WIN64 ? 8 : 4;
}

static UINT min_align(UINT natural, UINT cap)
{
	return natural < cap ? natural : cap;
}

static void lay_out_type(struct tl_type *type, UINT ptr, UINT cap);

static void desc_layout(struct tl_type *type, const TYPEDESC *desc, UINT ptr, UINT cap, UINT *size, UINT *align)
{
	switch (desc->vt)
	{
	case VT_I1:
	case VT_UI1:
		*size = *align = 1;
		break;
	case VT_I2:
	case VT_UI2:
	case VT_BOOL:
		*size = *align = 2;
		break;
	case VT_I4:
	case VT_UI4:
	case VT_R4:
	case VT_ERROR:
	case VT_INT:
	case VT_UINT:
	case VT_HRESULT:
		*size = *align = 4;
		break;
	case VT_R8:
	case VT_CY:
	case VT_DATE:
	case VT_I8:
	case VT_UI8:
		*size = 8;
		*align = min_align(8, cap);
		break;
	case VT_BSTR:
	case VT_DISPATCH:
	case VT_UNKNOWN:
	case VT_PTR:
	case VT_SAFEARRAY:
	case VT_LPSTR:
	case VT_LPWSTR:
		*size = ptr;
		*align = min_align(ptr, cap);
		break;
	case VT_VARIANT:
		*size = ptr == 8 ? 24 : 16;
		*align = min_align(8, cap);
		break;
	case VT_DECIMAL:
		*size = 16;
		*align = min_align(8, cap);
		break;
	case VT_CARRAY:
	{
		UINT dim, elements = 1;
		if (!desc->lpadesc)
		{
			*size = 0;
			*align = 1;
			break;
		}
		desc_layout(type, &desc->lpadesc->tdescElem, ptr, cap, size, align);
		for (dim = 0; dim < desc->lpadesc->cDims; dim++)
			elements *= desc->lpadesc->rgbounds[dim].cElements;
		*size *= elements;
		break;
	}
	case VT_USERDEFINED:
	{
		ITypeInfo *info;
		TYPEATTR *attr;
		*size = *align = 0;
		if (FAILED(tl_ref_info(type, desc->hreftype, &info)))
			break;
		if (tl_is_ours(info))
			lay_out_type(tl_type_from_info((ITypeInfo2 *)info), ptr, cap);
		if (SUCCEEDED(ITypeInfo_GetTypeAttr(info, &attr)))
		{
			*size = attr->cbSizeInstance;
			*align = attr->cbAlignment;
			ITypeInfo_ReleaseTypeAttr(info, attr);
		}
		ITypeInfo_Release(info);
		break;
	}
	default:
		*size = *align = 0;
		break;
	}
}

/* Structures and unions place their fields; an alias simply has the size of what it names. */
static void lay_out_type(struct tl_type *type, UINT ptr, UINT cap)
{
	UINT size, align, index, end = 0, widest = 1;
	if (type->laid_out || type->ITypeInfo2_iface.lpVtbl != &tl_info_vtbl)
		return;
	if (type->attr.typekind == TKIND_ALIAS)
	{
		type->laid_out = TRUE;
		desc_layout(type, &type->attr.tdescAlias, ptr, cap, &size, &align);
		type->attr.cbSizeInstance = size;
		type->attr.cbAlignment = (WORD)align;
		return;
	}
	if (type->attr.typekind != TKIND_RECORD && type->attr.typekind != TKIND_UNION)
		return;
	type->laid_out = TRUE;
	if (type->align_request)
		cap = min_align(type->align_request, cap);
	for (index = 0; index < type->attr.cVars; index++)
	{
		struct tl_var *var = &type->vars[index];
		UINT at = 0;
		if (var->desc.varkind != VAR_PERINSTANCE)
			continue;
		desc_layout(type, &var->desc.elemdescVar.tdesc, ptr, cap, &size, &align);
		if (align > cap)
			align = cap;
		if (align > widest)
			widest = align;
		if (type->attr.typekind == TKIND_RECORD)
			at = align ? (end + align - 1) & ~(align - 1) : end;
		var->desc.oInst = at;
		if (at + size > end)
			end = at + size;
	}
	type->attr.cbSizeInstance = (end + widest - 1) & ~(widest - 1);
	type->attr.cbAlignment = (WORD)widest;
}

/* Interfaces deeper in a hierarchy keep their generated ids apart from those of the base. */
static UINT inheritance_depth(struct tl_type *type)
{
	ITypeInfo *info = (ITypeInfo *)&type->ITypeInfo2_iface;
	UINT depth = 0;
	ITypeInfo_AddRef(info);
	while (depth < 64)
	{
		HREFTYPE ref;
		ITypeInfo *base;
		TYPEATTR *attr;
		BOOL go_on = FALSE;
		if (SUCCEEDED(ITypeInfo_GetTypeAttr(info, &attr)))
		{
			go_on = attr->typekind == TKIND_INTERFACE && attr->cImplTypes;
			ITypeInfo_ReleaseTypeAttr(info, attr);
		}
		if (!go_on || FAILED(ITypeInfo_GetRefTypeOfImplType(info, 0, &ref)) || FAILED(ITypeInfo_GetRefTypeInfo(info, ref, &base)))
			break;
		ITypeInfo_Release(info);
		info = base;
		depth++;
	}
	ITypeInfo_Release(info);
	return depth;
}

static BOOL func_id_taken(const struct tl_type *type, MEMBERID id)
{
	UINT index;
	for (index = 0; index < type->attr.cFuncs; index++)
		if (type->funcs[index].desc.memid == id)
			return TRUE;
	return FALSE;
}

static BOOL var_id_taken(const struct tl_type *type, MEMBERID id)
{
	UINT index;
	for (index = 0; index < type->attr.cVars; index++)
		if (type->vars[index].desc.memid == id)
			return TRUE;
	return FALSE;
}

/* An id comes from the member's place; a taken place moves past the end of the list. */
static void assign_ids(struct tl_type *type, UINT depth)
{
	UINT index;
	for (index = 0; index < type->attr.cFuncs; index++)
		if (type->funcs[index].desc.memid == MEMBERID_NIL)
		{
			MEMBERID base = GENERATED_FUNC_ID + (depth << 16), id = base + index;
			if (func_id_taken(type, id))
				id = base + type->attr.cFuncs;
			while (func_id_taken(type, id))
				id++;
			type->funcs[index].desc.memid = id;
		}
	for (index = 0; index < type->attr.cVars; index++)
		if (type->vars[index].desc.memid == MEMBERID_NIL)
		{
			MEMBERID base = GENERATED_VAR_ID + (depth << 16), id = base + index;
			if (var_id_taken(type, id))
				id = base + type->attr.cVars;
			while (var_id_taken(type, id))
				id++;
			type->vars[index].desc.memid = id;
		}
}

static UINT base_table_size(struct tl_type *type, UINT ptr)
{
	ITypeInfo *info;
	TYPEATTR *attr;
	UINT size = 0;
	if (!type->attr.cImplTypes || FAILED(tl_ref_info(type, type->impls[0].ref, &info)))
		return 0;
	if (SUCCEEDED(ITypeInfo_GetTypeAttr(info, &attr)))
	{
		size = attr->cbSizeVft;
		/* A base from another library was measured for this process, not for the library being built. */
		if (!tl_is_ours(info) || tl_type_from_info((ITypeInfo2 *)info)->lib != type->lib)
			size = size / sizeof(void *) * ptr;
		ITypeInfo_ReleaseTypeAttr(info, attr);
	}
	ITypeInfo_Release(info);
	return size;
}

static void place_functions(struct tl_type *type, UINT ptr)
{
	UINT base = base_table_size(type, ptr), end = base, index;
	for (index = 0; index < type->attr.cFuncs; index++)
	{
		struct tl_func *func = &type->funcs[index];
		UINT slot = func->fixed_slot ? (USHORT)func->desc.oVft : base + index * ptr;
		func->desc.oVft = (SHORT)slot;
		if (slot + ptr > end)
			end = slot + ptr;
	}
	type->attr.cbSizeVft = (WORD)end;
}

void tl_settle(struct tl_type *type)
{
	UINT ptr = pointer_size(type->lib);
	if (!type->dirty)
		return;
	type->dirty = FALSE;
	assign_ids(type, inheritance_depth(type));
	if (type->attr.typekind == TKIND_INTERFACE)
		place_functions(type, ptr);
	else if (type->attr.typekind == TKIND_DISPATCH)
		type->attr.cbSizeVft = (WORD)(DISPATCH_METHOD_COUNT * ptr);
	type->laid_out = FALSE;
	lay_out_type(type, ptr, DEFAULT_CREATED_ALIGNMENT);
	if (type->other_face)
	{
		struct tl_type *face = type->other_face;
		face->funcs = type->funcs;
		face->vars = type->vars;
		face->impls = type->impls;
		face->attr.cFuncs = type->attr.cFuncs;
		face->attr.cVars = type->attr.cVars;
		face->attr.cImplTypes = type->attr.cImplTypes;
		face->attr.cbSizeVft = type->attr.cbSizeVft;
		face->attr.guid = type->attr.guid;
	}
}

/* A library made for a 32-bit process describes smaller types than this process has. */
void tl_adopt_native_sizes(struct tl_lib *lib)
{
	UINT index;
	if (sizeof(void *) != 8 || lib->attr.syskind != SYS_WIN32)
		return;
	for (index = 0; index < lib->type_count; index++)
	{
		struct tl_type *type = lib->types[index];
		UINT func;
		switch (type->attr.typekind)
		{
		case TKIND_INTERFACE:
		case TKIND_DISPATCH:
			type->attr.cbSizeVft = (WORD)(type->attr.cbSizeVft * 2);
			for (func = 0; func < type->attr.cFuncs; func++)
				type->funcs[func].desc.oVft = (SHORT)((USHORT)type->funcs[func].desc.oVft * 2);
			/* fall through */
		case TKIND_COCLASS:
			type->attr.cbSizeInstance = sizeof(void *);
			type->attr.cbAlignment = sizeof(void *);
			break;
		default:
			break;
		}
	}
	for (index = 0; index < lib->type_count; index++)
		lay_out_type(lib->types[index], sizeof(void *), sizeof(void *));
}

static void *grown(struct tl_lib *lib, void *items, UINT used, UINT *room, SIZE_T item)
{
	UINT wanted;
	void *larger;
	if (used < *room)
		return items;
	wanted = *room ? *room * 2 : 8;
	larger = tl_keep(lib, wanted * item);
	if (!larger)
		return NULL;
	if (used)
		memcpy(larger, items, used * item);
	*room = wanted;
	return larger;
}

static HRESULT replace_string(BSTR *target, const WCHAR *value)
{
	BSTR copy = value ? SysAllocString(value) : NULL;
	if (value && !copy)
		return E_OUTOFMEMORY;
	SysFreeString(*target);
	*target = copy;
	return S_OK;
}

static HRESULT set_custom(struct tl_customs *list, const GUID *guid, const VARIANT *value)
{
	if (!guid || !value)
		return E_INVALIDARG;
	switch (V_VT(value))
	{
	case VT_I2:
	case VT_I4:
	case VT_R4:
	case VT_R8:
	case VT_CY:
	case VT_DATE:
	case VT_BSTR:
	case VT_ERROR:
	case VT_BOOL:
	case VT_DECIMAL:
	case VT_I1:
	case VT_UI1:
	case VT_UI2:
	case VT_UI4:
	case VT_I8:
	case VT_UI8:
	case VT_INT:
	case VT_UINT:
	case VT_HRESULT:
		return tl_customs_add(list, guid, value);
	default:
		return DISP_E_BADVARTYPE;
	}
}

static inline struct tl_type *type_from_create(ICreateTypeInfo2 *iface)
{
	return CONTAINING_RECORD(iface, struct tl_type, ICreateTypeInfo2_iface);
}

static HRESULT WINAPI create_info_QueryInterface(ICreateTypeInfo2 *iface, REFIID riid, void **out)
{
	return ITypeInfo2_QueryInterface(&type_from_create(iface)->ITypeInfo2_iface, riid, out);
}

static ULONG WINAPI create_info_AddRef(ICreateTypeInfo2 *iface)
{
	return ITypeInfo2_AddRef(&type_from_create(iface)->ITypeInfo2_iface);
}

static ULONG WINAPI create_info_Release(ICreateTypeInfo2 *iface)
{
	return ITypeInfo2_Release(&type_from_create(iface)->ITypeInfo2_iface);
}

static HRESULT WINAPI create_info_SetGuid(ICreateTypeInfo2 *iface, REFGUID guid)
{
	if (!guid)
		return E_INVALIDARG;
	type_from_create(iface)->attr.guid = *guid;
	return S_OK;
}

static HRESULT WINAPI create_info_SetTypeFlags(ICreateTypeInfo2 *iface, UINT flags)
{
	struct tl_type *type = type_from_create(iface);
	if (flags & TYPEFLAG_FDUAL)
		flags |= TYPEFLAG_FDISPATCHABLE;
	type->attr.wTypeFlags = (WORD)(flags | (type->attr.wTypeFlags & TYPEFLAG_FDISPATCHABLE));
	return S_OK;
}

static HRESULT WINAPI create_info_SetDocString(ICreateTypeInfo2 *iface, LPOLESTR text)
{
	if (!text)
		return E_INVALIDARG;
	return replace_string(&type_from_create(iface)->doc, text);
}

static HRESULT WINAPI create_info_SetHelpContext(ICreateTypeInfo2 *iface, DWORD context)
{
	type_from_create(iface)->help_context = context;
	return S_OK;
}

static HRESULT WINAPI create_info_SetVersion(ICreateTypeInfo2 *iface, WORD major, WORD minor)
{
	struct tl_type *type = type_from_create(iface);
	type->attr.wMajorVerNum = major;
	type->attr.wMinorVerNum = minor;
	return S_OK;
}

/* Imports are shared by every type of the library, one entry per type referred to. */
static HRESULT import_type(struct tl_lib *lib, ITypeInfo *info, HREFTYPE *out)
{
	ITypeLib *other;
	TLIBATTR *other_attr;
	TYPEATTR *attr;
	UINT position = 0, index;
	struct tl_import *import;
	HRESULT status = ITypeInfo_GetContainingTypeLib(info, &other, &position);
	BOOL by_guid;
	if (FAILED(status))
		return status;
	status = ITypeLib_GetLibAttr(other, &other_attr);
	if (FAILED(status))
	{
		ITypeLib_Release(other);
		return status;
	}
	status = ITypeInfo_GetTypeAttr(info, &attr);
	if (FAILED(status))
		goto done;
	by_guid = !IsEqualGUID(&attr->guid, &GUID_NULL);
	for (index = 0; index < lib->import_count; index++)
	{
		const struct tl_import *existing = &lib->imports[index];
		if (IsEqualGUID(&existing->library, &other_attr->guid) && existing->by_guid == by_guid && (by_guid ? IsEqualGUID(&existing->type, &attr->guid) : existing->index == (INT)position))
			break;
	}
	if (index == lib->import_count)
	{
		UINT room = lib->import_room;
		struct tl_import *imports = grown(lib, lib->imports, lib->import_count, &room, sizeof(*imports));
		if (!imports)
		{
			status = E_OUTOFMEMORY;
			goto release_attr;
		}
		lib->imports = imports;
		lib->import_room = room;
		lib->import_count++;
		import = &imports[index];
		memset(import, 0, sizeof(*import));
		import->library = other_attr->guid;
		import->major = other_attr->wMajorVerNum;
		import->minor = other_attr->wMinorVerNum;
		import->lcid = other_attr->lcid;
		import->by_guid = by_guid;
		import->type = attr->guid;
		import->index = position;
		if (FAILED(QueryPathOfRegTypeLib(&other_attr->guid, other_attr->wMajorVerNum, other_attr->wMinorVerNum, other_attr->lcid, &import->path)))
			import->path = SysAllocString(tl_lib_path(other));
	}
	*out = index * 12 | 1;
release_attr:
	ITypeInfo_ReleaseTypeAttr(info, attr);
done:
	ITypeLib_ReleaseTLibAttr(other, other_attr);
	ITypeLib_Release(other);
	return status;
}

static HRESULT WINAPI create_info_AddRefTypeInfo(ICreateTypeInfo2 *iface, ITypeInfo *info, HREFTYPE *out)
{
	struct tl_type *type = type_from_create(iface);
	if (!info || !out)
		return E_INVALIDARG;
	if (tl_is_ours(info) && tl_type_from_info((ITypeInfo2 *)info)->lib == type->lib)
	{
		*out = tl_type_from_info((ITypeInfo2 *)info)->index * 0x64;
		return S_OK;
	}
	return import_type(type->lib, info, out);
}

static HRESULT WINAPI create_info_AddFuncDesc(ICreateTypeInfo2 *iface, UINT index, FUNCDESC *desc)
{
	struct tl_type *type = type_from_create(iface);
	struct tl_lib *lib = type->lib;
	UINT count = type->attr.cFuncs, room;
	struct tl_func *funcs, *func;
	SHORT param;
	if (!desc)
		return E_INVALIDARG;
	if (index > count)
		return TYPE_E_ELEMENTNOTFOUND;
	if ((desc->invkind == INVOKE_PROPERTYPUT || desc->invkind == INVOKE_PROPERTYPUTREF) && !desc->cParams)
		return TYPE_E_INCONSISTENTPROPFUNCS;
	if (desc->cParamsOpt > desc->cParams || (desc->cParams && !desc->lprgelemdescParam))
		return E_INVALIDARG;
	if (desc->oVft & (pointer_size(lib) - 1))
		return E_INVALIDARG;
	room = type->func_room;
	funcs = grown(lib, type->funcs, count, &room, sizeof(*funcs));
	if (!funcs)
		return E_OUTOFMEMORY;
	type->funcs = funcs;
	type->func_room = room;
	memmove(funcs + index + 1, funcs + index, (count - index) * sizeof(*funcs));
	func = &funcs[index];
	memset(func, 0, sizeof(*func));
	func->desc = *desc;
	func->desc.lprgscode = NULL;
	func->desc.cScodes = 0;
	func->desc.lprgelemdescParam = NULL;
	func->fixed_slot = desc->oVft != 0;
	{
		SIZE_T extra = tl_elem_extra(&desc->elemdescFunc, FALSE);
		BYTE *space;
		for (param = 0; param < desc->cParams; param++)
			extra += tl_elem_extra(&desc->lprgelemdescParam[param], TRUE);
		space = tl_keep(lib, extra + 1);
		if (!space)
		{
			memmove(funcs + index, funcs + index + 1, (count - index) * sizeof(*funcs));
			return E_OUTOFMEMORY;
		}
		tl_elem_copy(&func->desc.elemdescFunc, &desc->elemdescFunc, FALSE, &space);
		if (desc->cParams)
		{
			func->desc.lprgelemdescParam = tl_keep(lib, desc->cParams * sizeof(ELEMDESC));
			func->params = tl_keep(lib, desc->cParams * sizeof(*func->params));
			if (!func->desc.lprgelemdescParam || !func->params)
			{
				memmove(funcs + index, funcs + index + 1, (count - index) * sizeof(*funcs));
				return E_OUTOFMEMORY;
			}
		}
		for (param = 0; param < desc->cParams; param++)
		{
			ELEMDESC *elem = &func->desc.lprgelemdescParam[param];
			tl_elem_copy(elem, &desc->lprgelemdescParam[param], TRUE, &space);
			/* A default takes the parameter's type, unless that type can hold anything. */
			if (elem->paramdesc.pparamdescex && elem->tdesc.vt != VT_VARIANT && elem->tdesc.vt != VT_USERDEFINED)
			{
				VARIANT *value = &elem->paramdesc.pparamdescex->varDefaultValue;
				VARTYPE target = elem->tdesc.vt == VT_INT ? VT_I4 : elem->tdesc.vt == VT_UINT ? VT_UI4 : elem->tdesc.vt;
				VariantChangeType(value, value, 0, target);
			}
		}
	}
	type->attr.cFuncs++;
	type->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI create_info_AddImplType(ICreateTypeInfo2 *iface, UINT index, HREFTYPE ref)
{
	struct tl_type *type = type_from_create(iface);
	ITypeInfo *info;
	TYPEATTR *attr;
	UINT room;
	struct tl_impl *impls;
	HRESULT status;
	if (type->attr.typekind == TKIND_COCLASS)
	{
		if (index != type->attr.cImplTypes)
			return TYPE_E_ELEMENTNOTFOUND;
	}
	else if (type->attr.typekind == TKIND_INTERFACE || type->attr.typekind == TKIND_DISPATCH)
	{
		if (index)
			return TYPE_E_ELEMENTNOTFOUND;
		if (type->attr.cImplTypes)
			return TYPE_E_BADMODULEKIND;
	}
	else
		return TYPE_E_BADMODULEKIND;
	status = tl_ref_info(type, ref, &info);
	if (FAILED(status))
		return status;
	if (type->attr.typekind != TKIND_COCLASS && SUCCEEDED(ITypeInfo_GetTypeAttr(info, &attr)))
	{
		if (IsEqualGUID(&attr->guid, &id_dispatch) || attr->wTypeFlags & TYPEFLAG_FDISPATCHABLE)
			type->attr.wTypeFlags |= TYPEFLAG_FDISPATCHABLE;
		ITypeInfo_ReleaseTypeAttr(info, attr);
	}
	ITypeInfo_Release(info);
	room = type->impl_room;
	impls = grown(type->lib, type->impls, type->attr.cImplTypes, &room, sizeof(*impls));
	if (!impls)
		return E_OUTOFMEMORY;
	type->impls = impls;
	type->impl_room = room;
	memset(&impls[index], 0, sizeof(impls[index]));
	impls[index].ref = ref;
	type->attr.cImplTypes++;
	type->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI create_info_SetImplTypeFlags(ICreateTypeInfo2 *iface, UINT index, INT flags)
{
	struct tl_type *type = type_from_create(iface);
	if (type->attr.typekind != TKIND_COCLASS)
		return TYPE_E_BADMODULEKIND;
	if (index >= type->attr.cImplTypes)
		return TYPE_E_ELEMENTNOTFOUND;
	type->impls[index].flags = flags;
	return S_OK;
}

static HRESULT WINAPI create_info_SetAlignment(ICreateTypeInfo2 *iface, WORD alignment)
{
	struct tl_type *type = type_from_create(iface);
	type->align_request = alignment;
	type->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI create_info_SetSchema(ICreateTypeInfo2 *iface, LPOLESTR schema)
{
	return S_OK;
}

static HRESULT WINAPI create_info_AddVarDesc(ICreateTypeInfo2 *iface, UINT index, VARDESC *desc)
{
	struct tl_type *type = type_from_create(iface);
	struct tl_lib *lib = type->lib;
	UINT count = type->attr.cVars, room;
	struct tl_var *vars, *var;
	BYTE *space;
	if (!desc)
		return E_INVALIDARG;
	if (index > count)
		return TYPE_E_ELEMENTNOTFOUND;
	room = type->var_room;
	vars = grown(lib, type->vars, count, &room, sizeof(*vars));
	if (!vars)
		return E_OUTOFMEMORY;
	type->vars = vars;
	type->var_room = room;
	space = tl_keep(lib, tl_elem_extra(&desc->elemdescVar, FALSE) + sizeof(VARIANT));
	if (!space)
		return E_OUTOFMEMORY;
	memmove(vars + index + 1, vars + index, (count - index) * sizeof(*vars));
	var = &vars[index];
	memset(var, 0, sizeof(*var));
	var->desc = *desc;
	if (desc->varkind == VAR_CONST && desc->lpvarValue)
	{
		var->desc.lpvarValue = (VARIANT *)space;
		space += sizeof(VARIANT);
		VariantInit(var->desc.lpvarValue);
		VariantCopy(var->desc.lpvarValue, desc->lpvarValue);
	}
	tl_elem_copy(&var->desc.elemdescVar, &desc->elemdescVar, FALSE, &space);
	type->attr.cVars++;
	type->dirty = TRUE;
	return S_OK;
}

static BOOL is_property(INVOKEKIND kind)
{
	return kind & (INVOKE_PROPERTYGET | INVOKE_PROPERTYPUT | INVOKE_PROPERTYPUTREF);
}

static HRESULT WINAPI create_info_SetFuncAndParamNames(ICreateTypeInfo2 *iface, UINT index, LPOLESTR *names, UINT count)
{
	struct tl_type *type = type_from_create(iface);
	struct tl_func *func;
	UINT other, name;
	if (!names)
		return E_INVALIDARG;
	if (index >= type->attr.cFuncs || !count)
		return TYPE_E_ELEMENTNOTFOUND;
	func = &type->funcs[index];
	if (func->desc.invkind & (INVOKE_PROPERTYPUT | INVOKE_PROPERTYPUTREF))
	{
		if (count > (UINT)func->desc.cParams)
			return TYPE_E_ELEMENTNOTFOUND;
	}
	else if (count > (UINT)func->desc.cParams + 1)
		return TYPE_E_ELEMENTNOTFOUND;
	if (!names[0])
		return E_INVALIDARG;
	/* Accessors of one property share a name, each once. */
	for (other = 0; other < type->attr.cFuncs; other++)
	{
		const struct tl_func *candidate = &type->funcs[other];
		if (other == index || !candidate->name || lstrcmpiW(candidate->name, names[0]))
			continue;
		if (!is_property(func->desc.invkind) || !is_property(candidate->desc.invkind) || func->desc.invkind == candidate->desc.invkind)
			return TYPE_E_AMBIGUOUSNAME;
	}
	if (FAILED(replace_string(&func->name, names[0])))
		return E_OUTOFMEMORY;
	for (name = 1; name < count; name++)
		if (FAILED(replace_string(&func->params[name - 1].name, names[name])))
			return E_OUTOFMEMORY;
	return S_OK;
}

static HRESULT WINAPI create_info_SetVarName(ICreateTypeInfo2 *iface, UINT index, LPOLESTR name)
{
	struct tl_type *type = type_from_create(iface);
	if (!name)
		return E_INVALIDARG;
	if (index >= type->attr.cVars)
		return TYPE_E_ELEMENTNOTFOUND;
	return replace_string(&type->vars[index].name, name);
}

static HRESULT WINAPI create_info_SetTypeDescAlias(ICreateTypeInfo2 *iface, TYPEDESC *desc)
{
	struct tl_type *type = type_from_create(iface);
	BYTE *space;
	if (!desc)
		return E_INVALIDARG;
	if (type->attr.typekind != TKIND_ALIAS)
		return TYPE_E_BADMODULEKIND;
	space = tl_keep(type->lib, tl_desc_extra(desc) + 1);
	if (!space)
		return E_OUTOFMEMORY;
	tl_desc_copy(&type->attr.tdescAlias, desc, &space);
	type->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI create_info_DefineFuncAsDllEntry(ICreateTypeInfo2 *iface, UINT index, LPOLESTR dll, LPOLESTR entry)
{
	struct tl_type *type = type_from_create(iface);
	struct tl_func *func;
	if (type->attr.typekind != TKIND_MODULE)
		return TYPE_E_BADMODULEKIND;
	if (index >= type->attr.cFuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	if (!dll || !entry)
		return E_INVALIDARG;
	func = &type->funcs[index];
	if (FAILED(replace_string(&type->dll, dll)))
		return E_OUTOFMEMORY;
	if (IS_INTRESOURCE(entry))
	{
		func->ordinal = (WORD)(ULONG_PTR)entry;
		return replace_string(&func->entry, NULL);
	}
	func->ordinal = 0;
	return replace_string(&func->entry, entry);
}

static HRESULT WINAPI create_info_SetFuncDocString(ICreateTypeInfo2 *iface, UINT index, LPOLESTR text)
{
	struct tl_type *type = type_from_create(iface);
	if (!text)
		return E_INVALIDARG;
	if (index >= type->attr.cFuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	return replace_string(&type->funcs[index].doc, text);
}

static HRESULT WINAPI create_info_SetVarDocString(ICreateTypeInfo2 *iface, UINT index, LPOLESTR text)
{
	struct tl_type *type = type_from_create(iface);
	if (!text)
		return E_INVALIDARG;
	if (index >= type->attr.cVars)
		return TYPE_E_ELEMENTNOTFOUND;
	return replace_string(&type->vars[index].doc, text);
}

static HRESULT WINAPI create_info_SetFuncHelpContext(ICreateTypeInfo2 *iface, UINT index, DWORD context)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cFuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	type->funcs[index].help_context = context;
	return S_OK;
}

static HRESULT WINAPI create_info_SetVarHelpContext(ICreateTypeInfo2 *iface, UINT index, DWORD context)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cVars)
		return TYPE_E_ELEMENTNOTFOUND;
	type->vars[index].help_context = context;
	return S_OK;
}

static HRESULT WINAPI create_info_SetMops(ICreateTypeInfo2 *iface, UINT index, BSTR mops)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI create_info_SetTypeIdldesc(ICreateTypeInfo2 *iface, IDLDESC *desc)
{
	if (!desc)
		return E_INVALIDARG;
	type_from_create(iface)->attr.idldescType = *desc;
	return S_OK;
}

static HRESULT WINAPI create_info_LayOut(ICreateTypeInfo2 *iface)
{
	struct tl_type *type = type_from_create(iface);
	type->dirty = TRUE;
	tl_settle(type);
	return S_OK;
}

static void remove_func(struct tl_type *type, UINT index)
{
	tl_func_free(&type->funcs[index]);
	memmove(type->funcs + index, type->funcs + index + 1, (type->attr.cFuncs - index - 1) * sizeof(*type->funcs));
	type->attr.cFuncs--;
	type->dirty = TRUE;
}

static void remove_var(struct tl_type *type, UINT index)
{
	tl_var_free(&type->vars[index]);
	memmove(type->vars + index, type->vars + index + 1, (type->attr.cVars - index - 1) * sizeof(*type->vars));
	type->attr.cVars--;
	type->dirty = TRUE;
}

static HRESULT WINAPI create_info_DeleteFuncDesc(ICreateTypeInfo2 *iface, UINT index)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cFuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	remove_func(type, index);
	return S_OK;
}

static HRESULT WINAPI create_info_DeleteFuncDescByMemId(ICreateTypeInfo2 *iface, MEMBERID id, INVOKEKIND kind)
{
	struct tl_type *type = type_from_create(iface);
	UINT index;
	tl_settle(type);
	for (index = 0; index < type->attr.cFuncs; index++)
		if (type->funcs[index].desc.memid == id && type->funcs[index].desc.invkind == kind)
		{
			remove_func(type, index);
			return S_OK;
		}
	return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI create_info_DeleteVarDesc(ICreateTypeInfo2 *iface, UINT index)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cVars)
		return TYPE_E_ELEMENTNOTFOUND;
	remove_var(type, index);
	return S_OK;
}

static HRESULT WINAPI create_info_DeleteVarDescByMemId(ICreateTypeInfo2 *iface, MEMBERID id)
{
	struct tl_type *type = type_from_create(iface);
	UINT index;
	tl_settle(type);
	for (index = 0; index < type->attr.cVars; index++)
		if (type->vars[index].desc.memid == id)
		{
			remove_var(type, index);
			return S_OK;
		}
	return TYPE_E_ELEMENTNOTFOUND;
}

/* The flag it left on the type stays: a type once dispatchable remains so. */
static HRESULT WINAPI create_info_DeleteImplType(ICreateTypeInfo2 *iface, UINT index)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cImplTypes)
		return TYPE_E_ELEMENTNOTFOUND;
	tl_customs_free(&type->impls[index].custom);
	memmove(type->impls + index, type->impls + index + 1, (type->attr.cImplTypes - index - 1) * sizeof(*type->impls));
	type->attr.cImplTypes--;
	type->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI create_info_SetCustData(ICreateTypeInfo2 *iface, REFGUID guid, VARIANT *value)
{
	return set_custom(&type_from_create(iface)->custom, guid, value);
}

static HRESULT WINAPI create_info_SetFuncCustData(ICreateTypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *value)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cFuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	return set_custom(&type->funcs[index].custom, guid, value);
}

static HRESULT WINAPI create_info_SetParamCustData(ICreateTypeInfo2 *iface, UINT index, UINT param, REFGUID guid, VARIANT *value)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cFuncs || param >= (UINT)type->funcs[index].desc.cParams)
		return TYPE_E_ELEMENTNOTFOUND;
	return set_custom(&type->funcs[index].params[param].custom, guid, value);
}

static HRESULT WINAPI create_info_SetVarCustData(ICreateTypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *value)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cVars)
		return TYPE_E_ELEMENTNOTFOUND;
	return set_custom(&type->vars[index].custom, guid, value);
}

static HRESULT WINAPI create_info_SetImplTypeCustData(ICreateTypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *value)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cImplTypes)
		return TYPE_E_ELEMENTNOTFOUND;
	return set_custom(&type->impls[index].custom, guid, value);
}

static HRESULT WINAPI create_info_SetHelpStringContext(ICreateTypeInfo2 *iface, ULONG context)
{
	type_from_create(iface)->string_context = context;
	return S_OK;
}

static HRESULT WINAPI create_info_SetFuncHelpStringContext(ICreateTypeInfo2 *iface, UINT index, ULONG context)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cFuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	type->funcs[index].string_context = context;
	return S_OK;
}

static HRESULT WINAPI create_info_SetVarHelpStringContext(ICreateTypeInfo2 *iface, UINT index, ULONG context)
{
	struct tl_type *type = type_from_create(iface);
	if (index >= type->attr.cVars)
		return TYPE_E_ELEMENTNOTFOUND;
	type->vars[index].string_context = context;
	return S_OK;
}

static HRESULT WINAPI create_info_Invalidate(ICreateTypeInfo2 *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI create_info_SetName(ICreateTypeInfo2 *iface, LPOLESTR name)
{
	if (!name)
		return E_INVALIDARG;
	return replace_string(&type_from_create(iface)->name, name);
}

const ICreateTypeInfo2Vtbl tl_create_info_vtbl =
{
	create_info_QueryInterface,
	create_info_AddRef,
	create_info_Release,
	create_info_SetGuid,
	create_info_SetTypeFlags,
	create_info_SetDocString,
	create_info_SetHelpContext,
	create_info_SetVersion,
	create_info_AddRefTypeInfo,
	create_info_AddFuncDesc,
	create_info_AddImplType,
	create_info_SetImplTypeFlags,
	create_info_SetAlignment,
	create_info_SetSchema,
	create_info_AddVarDesc,
	create_info_SetFuncAndParamNames,
	create_info_SetVarName,
	create_info_SetTypeDescAlias,
	create_info_DefineFuncAsDllEntry,
	create_info_SetFuncDocString,
	create_info_SetVarDocString,
	create_info_SetFuncHelpContext,
	create_info_SetVarHelpContext,
	create_info_SetMops,
	create_info_SetTypeIdldesc,
	create_info_LayOut,
	create_info_DeleteFuncDesc,
	create_info_DeleteFuncDescByMemId,
	create_info_DeleteVarDesc,
	create_info_DeleteVarDescByMemId,
	create_info_DeleteImplType,
	create_info_SetCustData,
	create_info_SetFuncCustData,
	create_info_SetParamCustData,
	create_info_SetVarCustData,
	create_info_SetImplTypeCustData,
	create_info_SetHelpStringContext,
	create_info_SetFuncHelpStringContext,
	create_info_SetVarHelpStringContext,
	create_info_Invalidate,
	create_info_SetName,
};

static inline struct tl_lib *lib_from_create(ICreateTypeLib2 *iface)
{
	return CONTAINING_RECORD(iface, struct tl_lib, ICreateTypeLib2_iface);
}

static HRESULT WINAPI create_lib_QueryInterface(ICreateTypeLib2 *iface, REFIID riid, void **out)
{
	return ITypeLib2_QueryInterface(&lib_from_create(iface)->ITypeLib2_iface, riid, out);
}

static ULONG WINAPI create_lib_AddRef(ICreateTypeLib2 *iface)
{
	return ITypeLib2_AddRef(&lib_from_create(iface)->ITypeLib2_iface);
}

static ULONG WINAPI create_lib_Release(ICreateTypeLib2 *iface)
{
	return ITypeLib2_Release(&lib_from_create(iface)->ITypeLib2_iface);
}

/* A dispatch interface refers to IDispatch without anybody asking for it. */
static void import_dispatch(struct tl_lib *lib)
{
	ITypeLib *standard;
	ITypeInfo *info;
	static const WCHAR standard_name[] = {'s', 't', 'd', 'o', 'l', 'e', '2', '.', 't', 'l', 'b', 0};
	HREFTYPE ref;
	if (lib->dispatch_ref != (HREFTYPE)-1 || FAILED(LoadTypeLib(standard_name, &standard)))
		return;
	if (SUCCEEDED(ITypeLib_GetTypeInfoOfGuid(standard, &id_dispatch, &info)))
	{
		if (SUCCEEDED(import_type(lib, info, &ref)))
			lib->dispatch_ref = ref;
		ITypeInfo_Release(info);
	}
	ITypeLib_Release(standard);
}

static HRESULT WINAPI create_lib_CreateTypeInfo(ICreateTypeLib2 *iface, LPOLESTR name, TYPEKIND kind, ICreateTypeInfo **out)
{
	struct tl_lib *lib = lib_from_create(iface);
	struct tl_type *type;
	UINT index, ptr = pointer_size(lib);
	if (!name || !out)
		return E_INVALIDARG;
	if (kind >= TKIND_MAX)
		return TYPE_E_BADMODULEKIND;
	*out = NULL;
	for (index = 0; index < lib->type_count; index++)
		if (lib->types[index]->name && !lstrcmpiW(lib->types[index]->name, name))
			return TYPE_E_NAMECONFLICT;
	type = tl_type_create(lib, kind);
	if (!type)
		return E_OUTOFMEMORY;
	type->name = SysAllocString(name);
	type->attr.cbAlignment = DEFAULT_CREATED_ALIGNMENT;
	type->dirty = TRUE;
	switch (kind)
	{
	case TKIND_DISPATCH:
		type->attr.wTypeFlags = TYPEFLAG_FDISPATCHABLE;
		import_dispatch(lib);
		/* fall through */
	case TKIND_INTERFACE:
	case TKIND_COCLASS:
		type->attr.cbSizeInstance = ptr;
		break;
	case TKIND_ENUM:
		type->attr.cbSizeInstance = 4;
		break;
	case TKIND_MODULE:
		type->attr.cbSizeInstance = 2;
		type->attr.cbAlignment = 1;
		break;
	default:
		break;
	}
	*out = (ICreateTypeInfo *)&type->ICreateTypeInfo2_iface;
	ICreateTypeInfo_AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI create_lib_SetName(ICreateTypeLib2 *iface, LPOLESTR name)
{
	if (!name)
		return E_INVALIDARG;
	return replace_string(&lib_from_create(iface)->name, name);
}

static HRESULT WINAPI create_lib_SetVersion(ICreateTypeLib2 *iface, WORD major, WORD minor)
{
	struct tl_lib *lib = lib_from_create(iface);
	lib->attr.wMajorVerNum = major;
	lib->attr.wMinorVerNum = minor;
	return S_OK;
}

static HRESULT WINAPI create_lib_SetGuid(ICreateTypeLib2 *iface, REFGUID guid)
{
	if (!guid)
		return E_INVALIDARG;
	lib_from_create(iface)->attr.guid = *guid;
	return S_OK;
}

static HRESULT WINAPI create_lib_SetDocString(ICreateTypeLib2 *iface, LPOLESTR text)
{
	if (!text)
		return E_INVALIDARG;
	return replace_string(&lib_from_create(iface)->doc, text);
}

static HRESULT WINAPI create_lib_SetHelpFileName(ICreateTypeLib2 *iface, LPOLESTR name)
{
	if (!name)
		return E_INVALIDARG;
	return replace_string(&lib_from_create(iface)->help_file, name);
}

static HRESULT WINAPI create_lib_SetHelpContext(ICreateTypeLib2 *iface, DWORD context)
{
	lib_from_create(iface)->help_context = context;
	return S_OK;
}

static HRESULT WINAPI create_lib_SetLcid(ICreateTypeLib2 *iface, LCID lcid)
{
	lib_from_create(iface)->attr.lcid = lcid;
	return S_OK;
}

static HRESULT WINAPI create_lib_SetLibFlags(ICreateTypeLib2 *iface, UINT flags)
{
	if (flags & ~(LIBFLAG_FRESTRICTED | LIBFLAG_FCONTROL | LIBFLAG_FHIDDEN | LIBFLAG_FHASDISKIMAGE))
		return TYPE_E_BADMODULEKIND;
	lib_from_create(iface)->attr.wLibFlags = (WORD)flags;
	return S_OK;
}

static HRESULT WINAPI create_lib_SaveAllChanges(ICreateTypeLib2 *iface)
{
	struct tl_lib *lib = lib_from_create(iface);
	if (!lib->path)
		return TYPE_E_IOERROR;
	return tl_write_file(lib, lib->path);
}

/* Nothing refers to a type by its place once it is gone, so only the last type can go cleanly. */
static HRESULT WINAPI create_lib_DeleteTypeInfo(ICreateTypeLib2 *iface, LPOLESTR name)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI create_lib_SetCustData(ICreateTypeLib2 *iface, REFGUID guid, VARIANT *value)
{
	return set_custom(&lib_from_create(iface)->custom, guid, value);
}

static HRESULT WINAPI create_lib_SetHelpStringContext(ICreateTypeLib2 *iface, ULONG context)
{
	lib_from_create(iface)->string_context = context;
	return S_OK;
}

static HRESULT WINAPI create_lib_SetHelpStringDll(ICreateTypeLib2 *iface, LPOLESTR dll)
{
	if (!dll)
		return E_INVALIDARG;
	return replace_string(&lib_from_create(iface)->help_dll, dll);
}

const ICreateTypeLib2Vtbl tl_create_lib_vtbl =
{
	create_lib_QueryInterface,
	create_lib_AddRef,
	create_lib_Release,
	create_lib_CreateTypeInfo,
	create_lib_SetName,
	create_lib_SetVersion,
	create_lib_SetGuid,
	create_lib_SetDocString,
	create_lib_SetHelpFileName,
	create_lib_SetHelpContext,
	create_lib_SetLcid,
	create_lib_SetLibFlags,
	create_lib_SaveAllChanges,
	create_lib_DeleteTypeInfo,
	create_lib_SetCustData,
	create_lib_SetHelpStringContext,
	create_lib_SetHelpStringDll,
};

static HRESULT create_library(SYSKIND kind, const WCHAR *path, ICreateTypeLib2 **out)
{
	struct tl_lib *lib;
	SIZE_T size;
	if (!path || !out)
		return E_INVALIDARG;
	if (kind != SYS_WIN16 && kind != SYS_WIN32 && kind != SYS_MAC && kind != SYS_WIN64)
		return E_INVALIDARG;
	lib = tl_lib_create();
	if (!lib)
		return E_OUTOFMEMORY;
	size = (lstrlenW(path) + 1) * sizeof(WCHAR);
	lib->path = tl_alloc(size);
	if (!lib->path)
	{
		ITypeLib2_Release(&lib->ITypeLib2_iface);
		return E_OUTOFMEMORY;
	}
	memcpy(lib->path, path, size);
	lib->attr.syskind = kind;
	*out = &lib->ICreateTypeLib2_iface;
	return S_OK;
}

HRESULT WINAPI CreateTypeLib2(SYSKIND kind, LPCOLESTR path, ICreateTypeLib2 **out)
{
	return create_library(kind, path, out);
}

HRESULT WINAPI CreateTypeLib(SYSKIND kind, LPCOLESTR path, ICreateTypeLib **out)
{
	return create_library(kind, path, (ICreateTypeLib2 **)out);
}
