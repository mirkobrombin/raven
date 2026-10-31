/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * Automation helpers and the raw call behind every typed Invoke.
 */
#include "typelib.h"

#ifdef _WIN64
/* Integer and float registers are both loaded from the same slots, so the callee finds each
 * argument in whichever one its signature uses. Result registers are left as the callee set them. */
__asm__(
	".text\n"
	".globl raven_call_slots\n"
	".seh_proc raven_call_slots\n"
	"raven_call_slots:\n"
	"	push %rsi\n"
	"	.seh_pushreg %rsi\n"
	"	push %rdi\n"
	"	.seh_pushreg %rdi\n"
	"	push %rbp\n"
	"	.seh_pushreg %rbp\n"
	"	mov %rsp, %rbp\n"
	"	.seh_setframe %rbp, 0\n"
	"	.seh_endprologue\n"
	"	mov %rcx, %r11\n"
	"	mov %rdx, %rsi\n"
	"	mov %r8, %rcx\n"
	"	lea (,%r8,8), %rax\n"
	"	sub %rax, %rsp\n"
	"	and $-16, %rsp\n"
	"	mov %rsp, %rdi\n"
	"	rep movsq\n"
	"	mov 0(%rsp), %rcx\n"
	"	mov 8(%rsp), %rdx\n"
	"	mov 16(%rsp), %r8\n"
	"	mov 24(%rsp), %r9\n"
	"	movq %rcx, %xmm0\n"
	"	movq %rdx, %xmm1\n"
	"	movq %r8, %xmm2\n"
	"	movq %r9, %xmm3\n"
	"	call *%r11\n"
	"	mov %rbp, %rsp\n"
	"	pop %rbp\n"
	"	pop %rdi\n"
	"	pop %rsi\n"
	"	ret\n"
	".seh_endproc\n");

UINT64 raven_call_slots(void *function, const UINT64 *slots, SIZE_T count);
#define raven_call_float ((float (*)(void *, const UINT64 *, SIZE_T))raven_call_slots)
#define raven_call_double ((double (*)(void *, const UINT64 *, SIZE_T))raven_call_slots)

HRESULT WINAPI DispCallFunc(void *instance, ULONG_PTR offset, CALLCONV convention, VARTYPE return_type, UINT count, VARTYPE *types, VARIANTARG **values, VARIANT *result)
{
	UINT64 *slots;
	SIZE_T used = 0, room = (count + 2 > 4 ? count + 2 : 4) + 2;
	void *function;
	UINT index;
	VARIANT local;
	if (!result)
	{
		VariantInit(&local);
		result = &local;
	}
	if (count && (!types || !values))
		return E_INVALIDARG;
	if (return_type == VT_HRESULT)
		return E_INVALIDARG;
	slots = tl_alloc(room * sizeof(*slots));
	if (!slots)
		return E_OUTOFMEMORY;
	if (instance)
	{
		function = (*(void ***)instance)[offset / sizeof(void *)];
		slots[used++] = (UINT64)instance;
	}
	else
		function = (void *)offset;
	if (return_type == VT_VARIANT || return_type == VT_DECIMAL)
		slots[used++] = (UINT64)result;
	for (index = 0; index < count; index++)
	{
		VARIANTARG *value = values[index];
		switch (types[index])
		{
		case VT_VARIANT:
		case VT_DECIMAL:
			/* Anything wider than a register travels by address. */
			slots[used++] = (UINT64)value;
			break;
		case VT_BOOL:
			slots[used++] = (UINT64)(USHORT)V_BOOL(value);
			break;
		default:
			slots[used++] = V_UI8(value);
			break;
		}
	}
	if (used < 4)
		used = 4;
	switch (return_type)
	{
	case VT_R4:
		V_R4(result) = raven_call_float(function, slots, used);
		break;
	case VT_R8:
	case VT_DATE:
		V_R8(result) = raven_call_double(function, slots, used);
		break;
	case VT_VOID:
		raven_call_slots(function, slots, used);
		break;
	case VT_VARIANT:
	case VT_DECIMAL:
		raven_call_slots(function, slots, used);
		break;
	default:
		V_UI8(result) = raven_call_slots(function, slots, used);
		break;
	}
	tl_free(slots);
	if (return_type != VT_VARIANT)
		V_VT(result) = return_type == VT_VOID ? VT_EMPTY : return_type;
	return S_OK;
}
#else
/* The callee sees the same stack a compiler would have built; the frame pointer puts it right
 * whichever side cleans up. Result registers and the FPU top are left as the callee set them. */
__asm__(
	".text\n"
	".globl _raven_call_stack\n"
	"_raven_call_stack:\n"
	"	push %ebp\n"
	"	mov %esp, %ebp\n"
	"	push %esi\n"
	"	push %edi\n"
	"	mov 16(%ebp), %ecx\n"
	"	mov 12(%ebp), %esi\n"
	"	lea (,%ecx,4), %eax\n"
	"	sub %eax, %esp\n"
	"	and $-16, %esp\n"
	"	mov %esp, %edi\n"
	"	rep movsl\n"
	"	mov 8(%ebp), %eax\n"
	"	call *%eax\n"
	"	lea -8(%ebp), %esp\n"
	"	pop %edi\n"
	"	pop %esi\n"
	"	pop %ebp\n"
	"	ret\n");

UINT64 raven_call_stack(void *function, const DWORD *stack, UINT count);
#define raven_call_float ((float (*)(void *, const DWORD *, UINT))raven_call_stack)
#define raven_call_double ((double (*)(void *, const DWORD *, UINT))raven_call_stack)

HRESULT WINAPI DispCallFunc(void *instance, ULONG_PTR offset, CALLCONV convention, VARTYPE return_type, UINT count, VARTYPE *types, VARIANTARG **values, VARIANT *result)
{
	DWORD *stack;
	UINT used = 0, index, room = 2;
	void *function;
	VARIANT local;
	if (convention != CC_STDCALL && convention != CC_CDECL)
		return E_INVALIDARG;
	if (!result)
	{
		VariantInit(&local);
		result = &local;
	}
	if (count && (!types || !values))
		return E_INVALIDARG;
	if (return_type == VT_HRESULT)
		return E_INVALIDARG;
	for (index = 0; index < count; index++)
		room += types[index] == VT_VARIANT || types[index] == VT_DECIMAL ? 4 : 2;
	stack = tl_alloc(room * sizeof(*stack));
	if (!stack)
		return E_OUTOFMEMORY;
	if (instance)
	{
		function = (*(void ***)instance)[offset / sizeof(void *)];
		stack[used++] = (DWORD)instance;
	}
	else
		function = (void *)offset;
	if (return_type == VT_VARIANT || return_type == VT_DECIMAL)
		stack[used++] = (DWORD)result;
	for (index = 0; index < count; index++)
	{
		VARIANTARG *value = values[index];
		switch (types[index])
		{
		case VT_VARIANT:
		case VT_DECIMAL:
			memcpy(&stack[used], value, sizeof(VARIANT));
			used += 4;
			break;
		case VT_R8:
		case VT_DATE:
		case VT_I8:
		case VT_UI8:
		case VT_CY:
			memcpy(&stack[used], &V_UI8(value), 8);
			used += 2;
			break;
		case VT_BOOL:
			stack[used++] = (USHORT)V_BOOL(value);
			break;
		default:
			stack[used++] = V_UI4(value);
			break;
		}
	}
	switch (return_type)
	{
	case VT_R4:
		V_R4(result) = raven_call_float(function, stack, used);
		break;
	case VT_R8:
	case VT_DATE:
		V_R8(result) = raven_call_double(function, stack, used);
		break;
	case VT_VOID:
	case VT_VARIANT:
	case VT_DECIMAL:
		raven_call_stack(function, stack, used);
		break;
	default:
		V_UI8(result) = raven_call_stack(function, stack, used);
		break;
	}
	tl_free(stack);
	if (return_type != VT_VARIANT)
		V_VT(result) = return_type == VT_VOID ? VT_EMPTY : return_type;
	return S_OK;
}
#endif

HRESULT WINAPI DispGetParam(DISPPARAMS *params, UINT position, VARTYPE type, VARIANT *out, UINT *arg_err)
{
	UINT index, at = (UINT)-1;
	HRESULT status;
	if (!params)
		return E_INVALIDARG;
	for (index = 0; index < params->cNamedArgs; index++)
		if (params->rgdispidNamedArgs[index] == (DISPID)position)
		{
			at = index;
			break;
		}
	if (at == (UINT)-1)
	{
		if (position >= params->cArgs - params->cNamedArgs)
			return DISP_E_PARAMNOTFOUND;
		at = params->cArgs - position - 1;
	}
	if (!out)
	{
		if (arg_err)
			*arg_err = at;
		return E_INVALIDARG;
	}
	VariantInit(out);
	status = VariantChangeType(out, &params->rgvarg[at], 0, type);
	if (FAILED(status) && arg_err)
		*arg_err = at;
	return status;
}

HRESULT WINAPI DispGetIDsOfNames(ITypeInfo *info, OLECHAR **names, UINT count, DISPID *ids)
{
	return ITypeInfo_GetIDsOfNames(info, names, count, ids);
}

HRESULT WINAPI DispInvoke(void *instance, ITypeInfo *info, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return ITypeInfo_Invoke(info, instance, id, flags, params, result, excep, arg_err);
}

/* A dispatch object for any instance that has a description. */
struct std_dispatch
{
	IDispatch IDispatch_iface;
	IUnknown IUnknown_inner;
	IUnknown *outer;
	void *instance;
	ITypeInfo *info;
	LONG ref;
};

static inline struct std_dispatch *dispatch_of(IDispatch *iface)
{
	return CONTAINING_RECORD(iface, struct std_dispatch, IDispatch_iface);
}

static inline struct std_dispatch *dispatch_of_inner(IUnknown *iface)
{
	return CONTAINING_RECORD(iface, struct std_dispatch, IUnknown_inner);
}

static HRESULT WINAPI inner_QueryInterface(IUnknown *iface, REFIID riid, void **out)
{
	struct std_dispatch *object = dispatch_of_inner(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualIID(riid, &IID_IUnknown))
		*out = &object->IUnknown_inner;
	else if (IsEqualIID(riid, &IID_IDispatch))
		*out = &object->IDispatch_iface;
	else
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI inner_AddRef(IUnknown *iface)
{
	return InterlockedIncrement(&dispatch_of_inner(iface)->ref);
}

static ULONG WINAPI inner_Release(IUnknown *iface)
{
	struct std_dispatch *object = dispatch_of_inner(iface);
	ULONG count = InterlockedDecrement(&object->ref);
	if (!count)
	{
		ITypeInfo_Release(object->info);
		CoTaskMemFree(object);
	}
	return count;
}

static const IUnknownVtbl inner_vtbl = {
	inner_QueryInterface,
	inner_AddRef,
	inner_Release,
};

static HRESULT WINAPI std_QueryInterface(IDispatch *iface, REFIID riid, void **out)
{
	return IUnknown_QueryInterface(dispatch_of(iface)->outer, riid, out);
}

static ULONG WINAPI std_AddRef(IDispatch *iface)
{
	return IUnknown_AddRef(dispatch_of(iface)->outer);
}

static ULONG WINAPI std_Release(IDispatch *iface)
{
	return IUnknown_Release(dispatch_of(iface)->outer);
}

static HRESULT WINAPI std_GetTypeInfoCount(IDispatch *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI std_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	struct std_dispatch *object = dispatch_of(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (index)
		return DISP_E_BADINDEX;
	*out = object->info;
	ITypeInfo_AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI std_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	if (!IsEqualIID(riid, &IID_NULL))
		return DISP_E_UNKNOWNINTERFACE;
	return DispGetIDsOfNames(dispatch_of(iface)->info, names, count, ids);
}

static HRESULT WINAPI std_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	struct std_dispatch *object = dispatch_of(iface);
	if (!IsEqualIID(riid, &IID_NULL))
		return DISP_E_UNKNOWNINTERFACE;
	return DispInvoke(object->instance, object->info, id, flags, params, result, excep, arg_err);
}

static const IDispatchVtbl std_dispatch_vtbl = {
	std_QueryInterface,
	std_AddRef,
	std_Release,
	std_GetTypeInfoCount,
	std_GetTypeInfo,
	std_GetIDsOfNames,
	std_Invoke,
};

HRESULT WINAPI CreateStdDispatch(IUnknown *outer, void *instance, ITypeInfo *info, IUnknown **out)
{
	struct std_dispatch *object;
	if (!instance || !info || !out)
		return E_INVALIDARG;
	object = CoTaskMemAlloc(sizeof(*object));
	if (!object)
		return E_OUTOFMEMORY;
	object->IDispatch_iface.lpVtbl = &std_dispatch_vtbl;
	object->IUnknown_inner.lpVtbl = &inner_vtbl;
	object->outer = outer ? outer : &object->IUnknown_inner;
	object->instance = instance;
	object->info = info;
	object->ref = 1;
	ITypeInfo_AddRef(info);
	*out = &object->IUnknown_inner;
	return S_OK;
}

/* A description built from the plain tables old automation servers pass. */
HRESULT WINAPI CreateDispTypeInfo(INTERFACEDATA *data, LCID lcid, ITypeInfo **out)
{
	struct tl_lib *lib;
	struct tl_type *iface_type, *class_type;
	UINT index;
	if (!data || !out)
		return E_INVALIDARG;
	lib = tl_lib_create();
	if (!lib)
		return E_OUTOFMEMORY;
	lib->attr.lcid = lcid;
	iface_type = tl_type_create(lib, TKIND_INTERFACE);
	class_type = tl_type_create(lib, TKIND_COCLASS);
	if (!iface_type || !class_type)
		goto failed;
	if (data->cMembers)
	{
		iface_type->funcs = tl_keep(lib, data->cMembers * sizeof(*iface_type->funcs));
		if (!iface_type->funcs)
			goto failed;
	}
	for (index = 0; index < data->cMembers; index++)
	{
		const METHODDATA *method = &data->pmethdata[index];
		struct tl_func *func = &iface_type->funcs[index];
		UINT param;
		func->name = SysAllocString(method->szName);
		func->desc.memid = method->dispid;
		func->desc.funckind = FUNC_VIRTUAL;
		func->desc.invkind = method->wFlags;
		func->desc.callconv = method->cc;
		func->desc.cParams = method->cArgs;
		func->desc.oVft = method->iMeth * sizeof(void *);
		func->desc.elemdescFunc.tdesc.vt = method->vtReturn;
		if (method->cArgs)
		{
			func->desc.lprgelemdescParam = tl_keep(lib, method->cArgs * sizeof(ELEMDESC));
			func->params = tl_keep(lib, method->cArgs * sizeof(*func->params));
			if (!func->desc.lprgelemdescParam || !func->params)
				goto failed;
		}
		for (param = 0; param < method->cArgs; param++)
		{
			func->desc.lprgelemdescParam[param].tdesc.vt = method->ppdata[param].vt;
			func->params[param].name = SysAllocString(method->ppdata[param].szName);
		}
		iface_type->attr.cFuncs++;
	}
	class_type->impls = tl_keep(lib, sizeof(*class_type->impls));
	if (!class_type->impls)
		goto failed;
	class_type->impls[0].ref = 0;
	class_type->attr.cImplTypes = 1;
	*out = (ITypeInfo *)&class_type->ITypeInfo2_iface;
	ITypeInfo_AddRef(*out);
	ITypeLib2_Release(&lib->ITypeLib2_iface);
	return S_OK;
failed:
	ITypeLib2_Release(&lib->ITypeLib2_iface);
	return E_OUTOFMEMORY;
}

/* The running object table is the one place an application object can be found. */
static void guid_item_name(REFCLSID clsid, WCHAR *text)
{
	static const WCHAR hex[] = L"0123456789ABCDEF";
	const BYTE *bytes = (const BYTE *)clsid;
	UINT index;
	for (index = 0; index < sizeof(GUID); index++)
	{
		text[index * 2] = hex[bytes[index] >> 4];
		text[index * 2 + 1] = hex[bytes[index] & 15];
	}
	text[sizeof(GUID) * 2] = 0;
}

static HRESULT make_active_moniker(REFCLSID clsid, IMoniker **out)
{
	WCHAR text[sizeof(GUID) * 2 + 8];
	lstrcpyW(text, L"!");
	guid_item_name(clsid, text + 1);
	return CreateItemMoniker(L"!", text + 1, out);
}

HRESULT WINAPI RegisterActiveObject(IUnknown *object, REFCLSID clsid, DWORD flags, DWORD *cookie)
{
	IRunningObjectTable *table = NULL;
	IMoniker *moniker = NULL;
	HRESULT status = make_active_moniker(clsid, &moniker);
	if (FAILED(status))
		return status;
	status = GetRunningObjectTable(0, &table);
	if (SUCCEEDED(status))
	{
		status = IRunningObjectTable_Register(table, flags == ACTIVEOBJECT_WEAK ? 0 : ROTFLAGS_REGISTRATIONKEEPSALIVE, object, moniker, cookie);
		IRunningObjectTable_Release(table);
	}
	IMoniker_Release(moniker);
	return status;
}

HRESULT WINAPI RevokeActiveObject(DWORD cookie, void *reserved)
{
	IRunningObjectTable *table = NULL;
	HRESULT status = GetRunningObjectTable(0, &table);
	if (SUCCEEDED(status))
	{
		status = IRunningObjectTable_Revoke(table, cookie);
		IRunningObjectTable_Release(table);
	}
	return status;
}

HRESULT WINAPI GetActiveObject(REFCLSID clsid, void *reserved, IUnknown **out)
{
	IRunningObjectTable *table = NULL;
	IMoniker *moniker = NULL;
	HRESULT status = make_active_moniker(clsid, &moniker);
	if (FAILED(status))
		return status;
	status = GetRunningObjectTable(0, &table);
	if (SUCCEEDED(status))
	{
		status = IRunningObjectTable_GetObject(table, moniker, out);
		IRunningObjectTable_Release(table);
	}
	IMoniker_Release(moniker);
	return status;
}

HRESULT WINAPI OleTranslateColor(OLE_COLOR color, HPALETTE palette, COLORREF *out)
{
	COLORREF value;
	BYTE kind = (BYTE)(color >> 24);
	switch (kind)
	{
	case 0x00:
		value = palette ? PALETTERGB(GetRValue(color), GetGValue(color), GetBValue(color)) : color;
		break;
	case 0x01:
		if (palette)
		{
			PALETTEENTRY entry;
			if (!GetPaletteEntries(palette, LOWORD(color), 1, &entry))
				return E_INVALIDARG;
		}
		value = color;
		break;
	case 0x02:
		value = color;
		break;
	case 0x80:
		if (LOBYTE(color) > COLOR_MENUBAR)
			return E_INVALIDARG;
		value = GetSysColor(color & 0xff);
		break;
	default:
		return E_INVALIDARG;
	}
	if (out)
		*out = value;
	return S_OK;
}
