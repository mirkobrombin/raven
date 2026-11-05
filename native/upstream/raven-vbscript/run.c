/* SPDX-License-Identifier: MIT */
#include "vbs.h"

/* The interpreter walks the parsed statements. Values are variants: arrays are safe arrays of variants, objects IDispatch pointers, and
 * the arithmetic, comparisons and conversions are oleaut32's, as they are for VBScript on Windows. A ByRef parameter is a variable whose
 * value refers to the caller's variable. Classes made by the script are objects with IDispatch of their own, so the host and other
 * objects can hold and call them. */

typedef enum { F_NORMAL, F_EXIT_DO, F_EXIT_FOR, F_EXIT_PROCEDURE, F_ERROR } flow;

typedef struct instance instance;

struct frame {
	engine *en;
	procedure *proc;
	instance *self;
	variable *locals;
	/* A function's value, which an assignment to its name sets. */
	VARIANT result;
	BOOL resume_next, global;
	IDispatch *with[32];
	UINT with_depth;
	script_failure *failure;
};

struct instance {
	IDispatch IDispatch_iface;
	LONG refs;
	engine *en;
	class_def *cls;
	variable *fields;
	BOOL terminated;
};

static flow run_block(frame *f, block *b);
static BOOL evaluate(frame *f, expr *e, VARIANT *out);
static BOOL call_procedure(engine *en, procedure *proc, instance *self, VARIANT *args, UINT count, VARIANT *result, script_failure *failure);
static IDispatchVtbl instance_vtbl;

BOOL vbs_raise(script_failure *failure, LONG number, const WCHAR *description)
{
	if (!failure->number) {
		failure->number = number;
		SysFreeString(failure->description);
		failure->description = SysAllocString(description ? description : vbs_error_text(number));
		SysFreeString(failure->source);
		failure->source = SysAllocString(L"Microsoft VBScript runtime error");
	}
	return FALSE;
}

void vbs_failure_clear(script_failure *failure)
{
	SysFreeString(failure->description);
	SysFreeString(failure->source);
	*failure = (script_failure){ 0 };
}

/* An HRESULT a call returned, as the script error it raises. */
static BOOL raise_result(script_failure *failure, HRESULT hr)
{
	switch (hr) {
	case DISP_E_TYPEMISMATCH: return vbs_raise(failure, VBS_E_TYPE_MISMATCH, NULL);
	case DISP_E_OVERFLOW: return vbs_raise(failure, VBS_E_OVERFLOW, NULL);
	case DISP_E_DIVBYZERO: return vbs_raise(failure, VBS_E_DIVISION, NULL);
	case DISP_E_BADINDEX: return vbs_raise(failure, VBS_E_SUBSCRIPT, NULL);
	case DISP_E_UNKNOWNNAME:
	case DISP_E_MEMBERNOTFOUND: return vbs_raise(failure, VBS_E_NOT_SUPPORTED, NULL);
	case DISP_E_BADPARAMCOUNT:
	case DISP_E_PARAMNOTOPTIONAL: return vbs_raise(failure, VBS_E_WRONG_ARGUMENTS, NULL);
	case E_OUTOFMEMORY: return vbs_raise(failure, VBS_E_OUT_OF_MEMORY, NULL);
	}
	/* Errors of the control facility carry a script error number; others raise themselves. */
	if (HRESULT_FACILITY(hr) == FACILITY_CONTROL)
		return vbs_raise(failure, HRESULT_CODE(hr), NULL);
	return vbs_raise(failure, (LONG)hr, NULL);
}

static VARIANT *slot(variable *v)
{
	return V_VT(&v->value) == (VT_BYREF | VT_VARIANT) ? V_VARIANTREF(&v->value) : &v->value;
}

static variable *find_in(variable *list, const WCHAR *name)
{
	for (; list; list = list->next)
		if (vbs_same(list->name, name))
			return list;
	return NULL;
}

static variable *add_to(variable **list, const WCHAR *name)
{
	variable *v = vbs_alloc(sizeof(variable));
	v->name = vbs_copy(name);
	VariantInit(&v->value);
	v->next = *list;
	*list = v;
	return v;
}

static void free_variables(variable *list)
{
	while (list) {
		variable *next = list->next;
		if (V_VT(&list->value) != (VT_BYREF | VT_VARIANT))
			VariantClear(&list->value);
		vbs_free(list->name);
		vbs_free(list);
		list = next;
	}
}

variable *vbs_global_variable(engine *en, const WCHAR *name) { return find_in(en->globals, name); }

procedure *vbs_global_procedure(engine *en, const WCHAR *name)
{
	for (UINT i = en->procedure_count; i-- > 0;)
		if (vbs_same(en->procedures[i]->name, name))
			return en->procedures[i];
	return NULL;
}

static class_def *find_class(engine *en, const WCHAR *name)
{
	for (UINT i = en->class_count; i-- > 0;)
		if (vbs_same(en->classes[i]->name, name))
			return en->classes[i];
	return NULL;
}

/* A class's procedure of a name for the kind of access. */
static procedure *class_procedure(class_def *c, const WCHAR *name, WORD flags, BOOL outside)
{
	procedure *found = NULL;
	for (UINT i = 0; i < c->procedure_count; i++) {
		procedure *proc = c->procedures[i];
		if (!vbs_same(proc->name, name) || (outside && proc->is_private))
			continue;
		if (flags & DISPATCH_PROPERTYPUTREF ? proc->kind == P_SET : flags & DISPATCH_PROPERTYPUT ? proc->kind == P_LET :
			proc->kind != P_LET && proc->kind != P_SET)
			return proc;
		found = found ? found : proc;
	}
	/* A Property Let stands in for a missing Property Set, as Windows lets it. */
	if (found && (flags & DISPATCH_PROPERTYPUTREF) && found->kind == P_LET)
		return found;
	return NULL;
}

static procedure *default_procedure(class_def *c, WORD flags)
{
	for (UINT i = 0; i < c->procedure_count; i++)
		if (c->procedures[i]->is_default)
			return class_procedure(c, c->procedures[i]->name, flags, TRUE);
	return NULL;
}

/* Values. */

HRESULT vbs_value_of(const VARIANT *in, VARIANT *out)
{
	VariantInit(out);
	while (V_VT(in) == (VT_BYREF | VT_VARIANT))
		in = V_VARIANTREF(in);
	if (V_VT(in) == VT_DISPATCH) {
		if (!V_DISPATCH(in))
			return DISP_E_TYPEMISMATCH;
		/* An object used as a value is its default property. */
		DISPPARAMS none = { 0 };
		EXCEPINFO info = { 0 };
		HRESULT hr = IDispatch_Invoke(V_DISPATCH(in), DISPID_VALUE, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET | DISPATCH_METHOD,
			&none, out, &info, NULL);
		SysFreeString(info.bstrSource);
		SysFreeString(info.bstrDescription);
		SysFreeString(info.bstrHelpFile);
		if (FAILED(hr))
			return hr == DISP_E_UNKNOWNNAME ? DISP_E_MEMBERNOTFOUND : hr;
		if (V_VT(out) == VT_DISPATCH) {
			VARIANT inner;
			hr = vbs_value_of(out, &inner);
			VariantClear(out);
			*out = inner;
			return hr;
		}
		return S_OK;
	}
	if (V_VT(in) & VT_BYREF)
		return VariantCopyInd(out, (VARIANT *)in);
	return VariantCopy(out, (VARIANT *)in);
}

/* What an operation reads: the value, an error raised when there is none. */
static BOOL plain(frame *f, VARIANT *v)
{
	if (V_VT(v) != VT_DISPATCH && !(V_VT(v) & VT_BYREF))
		return TRUE;
	VARIANT value;
	HRESULT hr = vbs_value_of(v, &value);
	VariantClear(v);
	*v = value;
	return SUCCEEDED(hr) || raise_result(f->failure, hr);
}

HRESULT vbs_to_text(const VARIANT *in, BSTR *out)
{
	VARIANT value, text;
	HRESULT hr = vbs_value_of(in, &value);
	if (FAILED(hr))
		return hr;
	VariantInit(&text);
	if (V_VT(&value) == VT_BOOL) {
		*out = SysAllocString(V_BOOL(&value) ? L"True" : L"False");
		return S_OK;
	}
	if (V_VT(&value) == VT_NULL)
		return DISP_E_TYPEMISMATCH;
	hr = VariantChangeType(&text, &value, VARIANT_ALPHABOOL, VT_BSTR);
	VariantClear(&value);
	if (FAILED(hr))
		return hr;
	*out = V_BSTR(&text);
	return S_OK;
}

HRESULT vbs_to_number(const VARIANT *in, double *out)
{
	VARIANT value, number;
	HRESULT hr = vbs_value_of(in, &value);
	if (FAILED(hr))
		return hr;
	VariantInit(&number);
	hr = VariantChangeType(&number, &value, 0, VT_R8);
	VariantClear(&value);
	if (SUCCEEDED(hr))
		*out = V_R8(&number);
	return hr;
}

HRESULT vbs_to_long(const VARIANT *in, LONG *out)
{
	VARIANT value, number;
	HRESULT hr = vbs_value_of(in, &value);
	if (FAILED(hr))
		return hr;
	VariantInit(&number);
	hr = VariantChangeType(&number, &value, 0, VT_I4);
	VariantClear(&value);
	if (SUCCEEDED(hr))
		*out = V_I4(&number);
	return hr;
}

HRESULT vbs_to_bool(const VARIANT *in, BOOL *out)
{
	VARIANT value, flag;
	HRESULT hr = vbs_value_of(in, &value);
	if (FAILED(hr))
		return hr;
	if (V_VT(&value) == VT_NULL) {
		*out = FALSE;
		return S_OK;
	}
	VariantInit(&flag);
	hr = VariantChangeType(&flag, &value, VARIANT_ALPHABOOL, VT_BOOL);
	VariantClear(&value);
	if (SUCCEEDED(hr))
		*out = V_BOOL(&flag) != VARIANT_FALSE;
	return hr;
}

/* Calls on objects. */

HRESULT vbs_invoke(IDispatch *object, const WCHAR *name, WORD flags, VARIANT *args, UINT count, VARIANT *result, script_failure *failure)
{
	DISPID id = DISPID_VALUE;
	HRESULT hr;
	/* A member that returns nothing leaves the result as it found it. */
	if (result)
		VariantInit(result);
	if (name) {
		LPOLESTR names[] = { (LPOLESTR)name };
		hr = IDispatch_GetIDsOfNames(object, &IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
		if (FAILED(hr))
			return raise_result(failure, DISP_E_UNKNOWNNAME), hr;
	}
	/* DISPPARAMS lists the arguments last first. */
	VARIANT *reversed = count ? vbs_alloc(count * sizeof(VARIANT)) : NULL;
	for (UINT i = 0; i < count; i++)
		reversed[count - 1 - i] = args[i];
	DISPID put = DISPID_PROPERTYPUT;
	BOOL putting = (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) != 0;
	DISPPARAMS params = { reversed, putting ? &put : NULL, count, putting ? 1 : 0 };
	EXCEPINFO info = { 0 };
	VARIANT ignored;
	VariantInit(&ignored);
	hr = IDispatch_Invoke(object, id, &IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result ? result : putting ? NULL : &ignored, &info, NULL);
	VariantClear(&ignored);
	vbs_free(reversed);
	if (hr == DISP_E_EXCEPTION) {
		if (info.pfnDeferredFillIn)
			info.pfnDeferredFillIn(&info);
		LONG number = info.scode ? (HRESULT_FACILITY(info.scode) == FACILITY_CONTROL ? HRESULT_CODE(info.scode) : info.scode) : info.wCode;
		if (!failure->number) {
			failure->number = number ? number : VBS_E_UNDEFINED;
			failure->description = info.bstrDescription ? info.bstrDescription : SysAllocString(L"");
			failure->source = info.bstrSource ? info.bstrSource : SysAllocString(L"");
			info.bstrDescription = info.bstrSource = NULL;
		}
		SysFreeString(info.bstrDescription);
		SysFreeString(info.bstrSource);
		SysFreeString(info.bstrHelpFile);
		return hr;
	}
	SysFreeString(info.bstrDescription);
	SysFreeString(info.bstrSource);
	SysFreeString(info.bstrHelpFile);
	if (FAILED(hr))
		raise_result(failure, hr);
	return hr;
}

static instance *as_instance(IDispatch *object)
{
	return object && object->lpVtbl == &instance_vtbl ? CONTAINING_RECORD(object, instance, IDispatch_iface) : NULL;
}

/* Arrays. */

static SAFEARRAY *new_array(frame *f, LONG *upper, UINT dims)
{
	SAFEARRAYBOUND bounds[60];
	if (dims > ARRAYSIZE(bounds))
		return vbs_raise(f->failure, VBS_E_SUBSCRIPT, NULL), NULL;
	for (UINT i = 0; i < dims; i++) {
		if (upper[i] < -1)
			return vbs_raise(f->failure, VBS_E_SUBSCRIPT, NULL), NULL;
		/* SafeArrayCreate takes the bounds first dimension first, as the script writes them. */
		bounds[i].lLbound = 0;
		bounds[i].cElements = (ULONG)(upper[i] + 1);
	}
	SAFEARRAY *array = SafeArrayCreate(VT_VARIANT, dims, bounds);
	if (!array)
		vbs_raise(f->failure, VBS_E_OUT_OF_MEMORY, NULL);
	return array;
}

static BOOL bounds_of(frame *f, expr **bounds, UINT count, LONG *upper)
{
	for (UINT i = 0; i < count; i++) {
		VARIANT v;
		if (!evaluate(f, bounds[i], &v))
			return FALSE;
		HRESULT hr = vbs_to_long(&v, &upper[i]);
		VariantClear(&v);
		if (FAILED(hr))
			return raise_result(f->failure, hr);
	}
	return TRUE;
}

/* The element an index list names in an array. */
static VARIANT *element(frame *f, SAFEARRAY *array, VARIANT *args, UINT count)
{
	if (!array || count != SafeArrayGetDim(array))
		return vbs_raise(f->failure, VBS_E_SUBSCRIPT, NULL), NULL;
	LONG indices[60];
	for (UINT i = 0; i < count; i++) {
		HRESULT hr = vbs_to_long(&args[i], &indices[i]);
		if (FAILED(hr))
			return raise_result(f->failure, hr), NULL;
	}
	/* SafeArrayPtrOfIndex takes the indices first dimension first too. */
	VARIANT *at = NULL;
	if (FAILED(SafeArrayPtrOfIndex(array, indices, (void **)&at)))
		return vbs_raise(f->failure, VBS_E_SUBSCRIPT, NULL), NULL;
	return at;
}

static SAFEARRAY *array_of(VARIANT *v)
{
	while (V_VT(v) == (VT_BYREF | VT_VARIANT))
		v = V_VARIANTREF(v);
	if (V_VT(v) == (VT_ARRAY | VT_VARIANT))
		return V_ARRAY(v);
	if (V_VT(v) == (VT_BYREF | VT_ARRAY | VT_VARIANT))
		return *V_ARRAYREF(v);
	return NULL;
}

static BOOL is_array(const VARIANT *v)
{
	while (V_VT(v) == (VT_BYREF | VT_VARIANT))
		v = V_VARIANTREF(v);
	return (V_VT(v) & VT_ARRAY) != 0;
}

/* Names. */

static BOOL item_object(engine *en, const WCHAR *name, IDispatch **out)
{
	for (UINT i = 0; i < en->item_count; i++)
		if ((en->items[i].flags & SCRIPTITEM_ISVISIBLE) && vbs_same(en->items[i].name, name) && (*out = vbs_item_object(en, i)))
			return TRUE;
	return FALSE;
}

/* The object among the items whose members are global that knows a name. */
static IDispatch *global_member_owner(engine *en, const WCHAR *name)
{
	for (UINT i = 0; i < en->item_count; i++) {
		IDispatch *object = en->items[i].global_members ? vbs_item_object(en, i) : NULL;
		DISPID id;
		LPOLESTR names[] = { (LPOLESTR)name };
		if (object && SUCCEEDED(IDispatch_GetIDsOfNames(object, &IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id)))
			return object;
	}
	return NULL;
}

/* The variable a name stands for here: a local, the function's own value, a field of the object, a global. */
static VARIANT *find_variable(frame *f, const WCHAR *name)
{
	variable *v;
	if (f->proc && (f->proc->kind == P_FUNCTION || f->proc->kind == P_GET) && vbs_same(f->proc->name, name))
		return &f->result;
	if ((v = find_in(f->locals, name)))
		return slot(v);
	if (f->self && (v = find_in(f->self->fields, name)))
		return slot(v);
	if ((v = find_in(f->en->globals, name)))
		return slot(v);
	return NULL;
}

static VARIANT *declare_here(frame *f, const WCHAR *name)
{
	if (f->global)
		return &add_to(&f->en->globals, name)->value;
	return &add_to(&f->locals, name)->value;
}

/* Arguments, evaluated in order. A name of a variable and nothing more passes the variable itself, for a ByRef parameter. */
static BOOL arguments(frame *f, expr **args, UINT count, VARIANT *out, BOOL references)
{
	for (UINT i = 0; i < count; i++) {
		VariantInit(&out[i]);
		if (!args[i]) {
			V_VT(&out[i]) = VT_ERROR;
			V_ERROR(&out[i]) = DISP_E_PARAMNOTFOUND;
			continue;
		}
		if (references && args[i]->kind == E_NAME && !args[i]->parenthesized) {
			VARIANT *place = find_variable(f, args[i]->name);
			if (place && place != &f->result) {
				V_VT(&out[i]) = VT_BYREF | VT_VARIANT;
				V_VARIANTREF(&out[i]) = place;
				continue;
			}
		}
		if (!evaluate(f, args[i], &out[i])) {
			for (UINT j = 0; j < i; j++)
				if (V_VT(&out[j]) != (VT_BYREF | VT_VARIANT))
					VariantClear(&out[j]);
			return FALSE;
		}
	}
	return TRUE;
}

static void free_arguments(VARIANT *args, UINT count)
{
	for (UINT i = 0; i < count; i++)
		if (V_VT(&args[i]) != (VT_BYREF | VT_VARIANT))
			VariantClear(&args[i]);
}

/* An object's member: a class's own procedures and fields from inside, IDispatch from everywhere else. */
static BOOL member_call(frame *f, IDispatch *object, const WCHAR *name, WORD flags, VARIANT *args, UINT count, VARIANT *result)
{
	instance *self = as_instance(object);
	if (self && self == f->self && name) {
		/* Inside its class, private members are reachable through Me too. */
		variable *field = find_in(self->fields, name);
		procedure *proc = field ? NULL : class_procedure(self->cls, name,
			flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF) ? flags : DISPATCH_PROPERTYGET, FALSE);
		if (field && !(flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF))) {
			VARIANT *value = slot(field);
			if (count) {
				VARIANT *at = element(f, array_of(value), args, count);
				return at && SUCCEEDED(VariantCopy(result, at));
			}
			return SUCCEEDED(VariantCopy(result, value));
		}
		if (proc)
			return call_procedure(f->en, proc, self, args, count, result, f->failure);
	}
	HRESULT hr = vbs_invoke(object, name, flags, args, count, result, f->failure);
	/* obj.Items(0) where Items takes no arguments: the property's value, then its element or its default member with them. */
	if (hr == DISP_E_BADPARAMCOUNT && name && count && !(flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF))) {
		VARIANT holder;
		vbs_failure_clear(f->failure);
		hr = vbs_invoke(object, name, DISPATCH_PROPERTYGET, NULL, 0, &holder, f->failure);
		if (SUCCEEDED(hr)) {
			if (is_array(&holder)) {
				VARIANT *at = element(f, array_of(&holder), args, count);
				hr = !at ? DISP_E_EXCEPTION : result ? VariantCopy(result, at) : S_OK;
			} else if (V_VT(&holder) == VT_DISPATCH && V_DISPATCH(&holder)) {
				hr = vbs_invoke(V_DISPATCH(&holder), NULL, flags, args, count, result, f->failure);
			} else {
				vbs_raise(f->failure, VBS_E_TYPE_MISMATCH, NULL);
				hr = DISP_E_TYPEMISMATCH;
			}
			VariantClear(&holder);
		}
	}
	return SUCCEEDED(hr);
}

static BOOL call_builtin(frame *f, const WCHAR *name, VARIANT *args, UINT count, VARIANT *out, BOOL *found);

/* A name read, with arguments or without: a variable or its element, a procedure, a named object, a built-in function. */
static BOOL resolve(frame *f, const WCHAR *name, expr **arg_exprs, UINT arg_count, BOOL has_args, VARIANT *out, WORD flags)
{
	engine *en = f->en;
	VariantInit(out);
	VARIANT *place = find_variable(f, name);
	if (place && !(place == &f->result && has_args)) {
		VARIANT *args = arg_count ? vbs_alloc(arg_count * sizeof(VARIANT)) : NULL;
		BOOL fine = arguments(f, arg_exprs, arg_count, args, FALSE);
		if (fine) {
			if (is_array(place) && has_args) {
				VARIANT *at = element(f, array_of(place), args, arg_count);
				fine = at && SUCCEEDED(VariantCopy(out, at));
			} else if (has_args && arg_count && V_VT(place) == VT_DISPATCH && V_DISPATCH(place)) {
				fine = member_call(f, V_DISPATCH(place), NULL, flags, args, arg_count, out);
			} else if (has_args && arg_count) {
				fine = vbs_raise(f->failure, VBS_E_TYPE_MISMATCH, NULL);
			} else {
				fine = SUCCEEDED(VariantCopy(out, place)) || vbs_raise(f->failure, VBS_E_OUT_OF_MEMORY, NULL);
			}
		}
		free_arguments(args, arg_count);
		vbs_free(args);
		return fine;
	}
	procedure *proc = f->self ? class_procedure(f->self->cls, name, DISPATCH_PROPERTYGET, FALSE) : NULL;
	instance *self = proc ? f->self : NULL;
	if (!proc)
		proc = vbs_global_procedure(en, name);
	if (proc) {
		VARIANT *args = arg_count ? vbs_alloc(arg_count * sizeof(VARIANT)) : NULL;
		BOOL fine = arguments(f, arg_exprs, arg_count, args, TRUE) &&
			call_procedure(en, proc, self, args, arg_count, out, f->failure);
		free_arguments(args, arg_count);
		vbs_free(args);
		return fine;
	}
	IDispatch *object;
	if (item_object(en, name, &object)) {
		if (!arg_count) {
			V_VT(out) = VT_DISPATCH;
			V_DISPATCH(out) = object;
			IDispatch_AddRef(object);
			return TRUE;
		}
		VARIANT *args = vbs_alloc(arg_count * sizeof(VARIANT));
		BOOL fine = arguments(f, arg_exprs, arg_count, args, FALSE) && member_call(f, object, NULL, flags, args, arg_count, out);
		free_arguments(args, arg_count);
		vbs_free(args);
		return fine;
	}
	VARIANT *args = arg_count ? vbs_alloc(arg_count * sizeof(VARIANT)) : NULL;
	if (!arguments(f, arg_exprs, arg_count, args, FALSE)) {
		vbs_free(args);
		return FALSE;
	}
	BOOL found = FALSE, fine = call_builtin(f, name, args, arg_count, out, &found);
	if (!found) {
		IDispatch *owner = global_member_owner(en, name);
		if (owner) {
			fine = member_call(f, owner, name, flags, args, arg_count, out);
		} else if (has_args) {
			/* Calling a name nothing defines is a type mismatch whose text names it, as Windows reports it. */
			WCHAR text[200];
			wsprintfW(text, L"Type mismatch: '%.150s'", name);
			fine = vbs_raise(f->failure, VBS_E_TYPE_MISMATCH, text);
		} else {
			fine = !en->explicit_names || vbs_raise(f->failure, VBS_E_UNDEFINED, NULL);
		}
	}
	free_arguments(args, arg_count);
	vbs_free(args);
	return fine;
}

static BOOL object_of(frame *f, expr *e, IDispatch **out)
{
	VARIANT v;
	if (e->kind == E_WITH) {
		if (!f->with_depth)
			return vbs_raise(f->failure, VBS_E_OBJECT_REQUIRED, NULL);
		*out = f->with[f->with_depth - 1];
		IDispatch_AddRef(*out);
		return TRUE;
	}
	if (!evaluate(f, e, &v))
		return FALSE;
	VARIANT *at = &v;
	while (V_VT(at) == (VT_BYREF | VT_VARIANT))
		at = V_VARIANTREF(at);
	if (V_VT(at) == (VT_BYREF | VT_DISPATCH) && *V_DISPATCHREF(at)) {
		*out = *V_DISPATCHREF(at);
		IDispatch_AddRef(*out);
		VariantClear(&v);
		return TRUE;
	}
	if (V_VT(at) != VT_DISPATCH || !V_DISPATCH(at)) {
		VariantClear(&v);
		return vbs_raise(f->failure, VBS_E_OBJECT_REQUIRED, NULL);
	}
	*out = V_DISPATCH(at);
	return TRUE;
}

/* The members of an expression that names something callable: its object, member name and arguments. */
static BOOL call_value(frame *f, expr *target, expr **args, UINT count, BOOL has_args, VARIANT *out, WORD flags)
{
	VariantInit(out);
	if (target->kind == E_NAME)
		return resolve(f, target->name, args, count, has_args, out, flags);
	if (target->kind == E_MEMBER) {
		IDispatch *object;
		if (!object_of(f, target->target, &object))
			return FALSE;
		VARIANT *values = count ? vbs_alloc(count * sizeof(VARIANT)) : NULL;
		BOOL fine = arguments(f, args, count, values, FALSE) && member_call(f, object, target->name, flags, values, count, out);
		free_arguments(values, count);
		vbs_free(values);
		IDispatch_Release(object);
		return fine;
	}
	/* A value called with arguments: an array's element or an object's default member. */
	VARIANT base;
	if (!evaluate(f, target, &base))
		return FALSE;
	VARIANT *values = count ? vbs_alloc(count * sizeof(VARIANT)) : NULL;
	BOOL fine = arguments(f, args, count, values, FALSE);
	if (fine) {
		if (is_array(&base)) {
			VARIANT *at = element(f, array_of(&base), values, count);
			fine = at && SUCCEEDED(VariantCopy(out, at));
		} else if (V_VT(&base) == VT_DISPATCH && V_DISPATCH(&base)) {
			fine = member_call(f, V_DISPATCH(&base), NULL, flags, values, count, out);
		} else if (!count) {
			*out = base;
			VariantInit(&base);
		} else {
			fine = vbs_raise(f->failure, VBS_E_TYPE_MISMATCH, NULL);
		}
	}
	free_arguments(values, count);
	vbs_free(values);
	VariantClear(&base);
	return fine;
}

/* Operators. */

static BOOL is_numeric_kind(VARTYPE vt)
{
	switch (vt) {
	case VT_I1: case VT_I2: case VT_I4: case VT_I8: case VT_UI1: case VT_UI2: case VT_UI4: case VT_UI8: case VT_INT: case VT_UINT:
	case VT_R4: case VT_R8: case VT_CY: case VT_DECIMAL: case VT_DATE: case VT_BOOL:
		return TRUE;
	}
	return FALSE;
}

static BOOL compare(frame *f, VARIANT *a, VARIANT *b, const WCHAR *op, VARIANT *out)
{
	if (V_VT(a) == VT_NULL || V_VT(b) == VT_NULL) {
		V_VT(out) = VT_NULL;
		return TRUE;
	}
	HRESULT order;
	/* A number is less than any string, as VBScript orders variants of the two kinds. */
	if (is_numeric_kind(V_VT(a)) && V_VT(b) == VT_BSTR)
		order = VARCMP_LT;
	else if (V_VT(a) == VT_BSTR && is_numeric_kind(V_VT(b)))
		order = VARCMP_GT;
	else
		order = VarCmp(a, b, LOCALE_USER_DEFAULT, 0);
	if (FAILED(order))
		return raise_result(f->failure, order);
	BOOL truth;
	if (!lstrcmpW(op, L"="))
		truth = order == VARCMP_EQ;
	else if (!lstrcmpW(op, L"<>"))
		truth = order != VARCMP_EQ;
	else if (!lstrcmpW(op, L"<"))
		truth = order == VARCMP_LT;
	else if (!lstrcmpW(op, L">"))
		truth = order == VARCMP_GT;
	else if (!lstrcmpW(op, L"<="))
		truth = order != VARCMP_GT;
	else
		truth = order != VARCMP_LT;
	V_VT(out) = VT_BOOL;
	V_BOOL(out) = truth ? VARIANT_TRUE : VARIANT_FALSE;
	return TRUE;
}

static BOOL same_object(VARIANT *a, VARIANT *b, VARIANT *out, frame *f)
{
	IUnknown *left = NULL, *right = NULL;
	if (V_VT(a) != VT_DISPATCH || V_VT(b) != VT_DISPATCH)
		return vbs_raise(f->failure, VBS_E_OBJECT_REQUIRED, NULL);
	if (V_DISPATCH(a))
		IDispatch_QueryInterface(V_DISPATCH(a), &IID_IUnknown, (void **)&left);
	if (V_DISPATCH(b))
		IDispatch_QueryInterface(V_DISPATCH(b), &IID_IUnknown, (void **)&right);
	V_VT(out) = VT_BOOL;
	V_BOOL(out) = left == right ? VARIANT_TRUE : VARIANT_FALSE;
	if (left)
		IUnknown_Release(left);
	if (right)
		IUnknown_Release(right);
	return TRUE;
}

static BOOL binary(frame *f, expr *e, VARIANT *out)
{
	VARIANT a, b;
	if (!evaluate(f, e->left, &a))
		return FALSE;
	if (!evaluate(f, e->right, &b)) {
		VariantClear(&a);
		return FALSE;
	}
	BOOL fine;
	if (!lstrcmpW(e->op, L"is")) {
		fine = same_object(&a, &b, out, f);
	} else if (!plain(f, &a) || !plain(f, &b)) {
		fine = FALSE;
	} else if (e->op[0] == L'=' || e->op[0] == L'<' || e->op[0] == L'>') {
		fine = compare(f, &a, &b, e->op, out);
	} else {
		HRESULT hr;
		switch (e->op[0]) {
		case L'+': hr = VarAdd(&a, &b, out); break;
		case L'-': hr = VarSub(&a, &b, out); break;
		case L'*': hr = VarMul(&a, &b, out); break;
		case L'/': hr = VarDiv(&a, &b, out); break;
		case L'\\': hr = VarIdiv(&a, &b, out); break;
		case L'^': hr = VarPow(&a, &b, out); break;
		case L'&': hr = VarCat(&a, &b, out); break;
		default:
			if (vbs_same(e->op, L"mod"))
				hr = VarMod(&a, &b, out);
			else if (vbs_same(e->op, L"and"))
				hr = VarAnd(&a, &b, out);
			else if (vbs_same(e->op, L"or"))
				hr = VarOr(&a, &b, out);
			else if (vbs_same(e->op, L"xor"))
				hr = VarXor(&a, &b, out);
			else if (vbs_same(e->op, L"eqv"))
				hr = VarEqv(&a, &b, out);
			else
				hr = VarImp(&a, &b, out);
		}
		fine = SUCCEEDED(hr) || raise_result(f->failure, hr);
	}
	VariantClear(&a);
	VariantClear(&b);
	return fine;
}

static BOOL evaluate(frame *f, expr *e, VARIANT *out)
{
	VariantInit(out);
	switch (e->kind) {
	case E_CONSTANT:
		return SUCCEEDED(VariantCopy(out, &e->constant));
	case E_NOTHING:
		V_VT(out) = VT_DISPATCH;
		V_DISPATCH(out) = NULL;
		return TRUE;
	case E_ME:
		if (!f->self)
			return vbs_raise(f->failure, VBS_E_OBJECT_REQUIRED, NULL);
		V_VT(out) = VT_DISPATCH;
		V_DISPATCH(out) = &f->self->IDispatch_iface;
		IDispatch_AddRef(V_DISPATCH(out));
		return TRUE;
	case E_WITH:
		if (!f->with_depth)
			return vbs_raise(f->failure, VBS_E_OBJECT_REQUIRED, NULL);
		V_VT(out) = VT_DISPATCH;
		V_DISPATCH(out) = f->with[f->with_depth - 1];
		IDispatch_AddRef(V_DISPATCH(out));
		return TRUE;
	case E_NEW: {
		class_def *c = find_class(f->en, e->name);
		if (!c && vbs_same(e->name, L"RegExp")) {
			V_VT(out) = VT_DISPATCH;
			V_DISPATCH(out) = vbs_regexp_new();
			return TRUE;
		}
		if (!c)
			return vbs_raise(f->failure, 506, NULL);
		instance *object = vbs_alloc(sizeof(instance));
		object->IDispatch_iface.lpVtbl = &instance_vtbl;
		object->refs = 1;
		object->en = f->en;
		object->cls = c;
		for (UINT i = c->field_count; i-- > 0;) {
			variable *field = add_to(&object->fields, c->fields[i]);
			field->is_private = c->field_private[i];
			if (c->field_bound_counts[i]) {
				LONG upper[60];
				if (c->field_bound_counts[i] > 60 || !bounds_of(f, c->field_bounds[i], c->field_bound_counts[i], upper)) {
					IDispatch_Release(&object->IDispatch_iface);
					return FALSE;
				}
				SAFEARRAY *array = new_array(f, upper, c->field_bound_counts[i]);
				if (!array) {
					IDispatch_Release(&object->IDispatch_iface);
					return FALSE;
				}
				V_VT(&field->value) = VT_ARRAY | VT_VARIANT;
				V_ARRAY(&field->value) = array;
			}
		}
		V_VT(out) = VT_DISPATCH;
		V_DISPATCH(out) = &object->IDispatch_iface;
		procedure *init = class_procedure(c, L"Class_Initialize", DISPATCH_METHOD, FALSE);
		if (init) {
			VARIANT ignored;
			VariantInit(&ignored);
			if (!call_procedure(f->en, init, object, NULL, 0, &ignored, f->failure)) {
				VariantClear(out);
				return FALSE;
			}
			VariantClear(&ignored);
		}
		return TRUE;
	}
	case E_NAME:
		return resolve(f, e->name, NULL, 0, FALSE, out, DISPATCH_PROPERTYGET | DISPATCH_METHOD);
	case E_MEMBER:
		return call_value(f, e, NULL, 0, FALSE, out, DISPATCH_PROPERTYGET | DISPATCH_METHOD);
	case E_CALL:
		return call_value(f, e->target, e->args, e->arg_count, TRUE, out, DISPATCH_PROPERTYGET | DISPATCH_METHOD);
	case E_UNARY: {
		VARIANT v;
		if (!evaluate(f, e->left, &v))
			return FALSE;
		if (!plain(f, &v)) {
			VariantClear(&v);
			return FALSE;
		}
		HRESULT hr = e->op[0] == L'-' ? VarNeg(&v, out) : VarNot(&v, out);
		VariantClear(&v);
		return SUCCEEDED(hr) || raise_result(f->failure, hr);
	}
	case E_BINARY:
		return binary(f, e, out);
	}
	return vbs_raise(f->failure, VBS_E_UNDEFINED, NULL);
}

/* Assignment. */

/* Puts a value in a variable: an object is its default value unless Set put it there. */
static BOOL store(frame *f, VARIANT *place, VARIANT *value, BOOL set)
{
	VARIANT stored;
	VariantInit(&stored);
	if (set) {
		if (V_VT(value) != VT_DISPATCH && V_VT(value) != VT_UNKNOWN) {
			VariantClear(value);
			return vbs_raise(f->failure, VBS_E_OBJECT_REQUIRED, NULL);
		}
		stored = *value;
	} else {
		if (!plain(f, value))
			return FALSE;
		stored = *value;
	}
	VariantInit(value);
	VariantClear(place);
	*place = stored;
	return TRUE;
}

static BOOL assign(frame *f, expr *target, VARIANT *value, BOOL set)
{
	WORD put = set ? DISPATCH_PROPERTYPUTREF : DISPATCH_PROPERTYPUT;
	if (target->kind == E_NAME) {
		VARIANT *place = find_variable(f, target->name);
		if (!place && f->self) {
			procedure *proc = class_procedure(f->self->cls, target->name, put, FALSE);
			if (proc) {
				VARIANT ignored;
				VariantInit(&ignored);
				BOOL fine = call_procedure(f->en, proc, f->self, value, 1, &ignored, f->failure);
				VariantClear(&ignored);
				VariantClear(value);
				return fine;
			}
		}
		if (!place) {
			IDispatch *owner = global_member_owner(f->en, target->name);
			if (owner) {
				BOOL fine = member_call(f, owner, target->name, put, value, 1, NULL);
				VariantClear(value);
				return fine;
			}
			if (f->en->explicit_names) {
				VariantClear(value);
				return vbs_raise(f->failure, VBS_E_UNDEFINED, NULL);
			}
			place = declare_here(f, target->name);
		}
		variable *v = find_in(f->en->globals, target->name);
		if (v && v->constant && place == slot(v)) {
			VariantClear(value);
			return vbs_raise(f->failure, 501, L"Illegal assignment");
		}
		return store(f, place, value, set);
	}
	if (target->kind == E_MEMBER) {
		IDispatch *object;
		if (!object_of(f, target->target, &object)) {
			VariantClear(value);
			return FALSE;
		}
		instance *self = as_instance(object);
		BOOL fine;
		variable *field = self && self == f->self ? find_in(self->fields, target->name) : NULL;
		if (field) {
			fine = store(f, slot(field), value, set);
		} else {
			if (!set && !plain(f, value)) {
				IDispatch_Release(object);
				VariantClear(value);
				return FALSE;
			}
			fine = member_call(f, object, target->name, put, value, 1, NULL);
		}
		IDispatch_Release(object);
		VariantClear(value);
		return fine;
	}
	if (target->kind == E_CALL) {
		/* a(i) = v: an array's element, or an object's member or default member with the index. */
		expr *base = target->target;
		VARIANT *place = base->kind == E_NAME ? find_variable(f, base->name) : NULL;
		VARIANT *args = vbs_alloc((target->arg_count + 1) * sizeof(VARIANT));
		if (!arguments(f, target->args, target->arg_count, args, FALSE)) {
			vbs_free(args);
			VariantClear(value);
			return FALSE;
		}
		BOOL fine;
		if (place && is_array(place)) {
			VARIANT *at = element(f, array_of(place), args, target->arg_count);
			fine = at && store(f, at, value, set);
		} else {
			IDispatch *object = NULL;
			const WCHAR *name = NULL;
			if (base->kind == E_MEMBER) {
				fine = object_of(f, base->target, &object);
				name = base->name;
			} else if (place && V_VT(place) == VT_DISPATCH && V_DISPATCH(place)) {
				object = V_DISPATCH(place);
				IDispatch_AddRef(object);
				fine = TRUE;
			} else if (base->kind == E_NAME && !place && f->self && class_procedure(f->self->cls, base->name, put, FALSE)) {
				object = &f->self->IDispatch_iface;
				IDispatch_AddRef(object);
				name = base->name;
				fine = TRUE;
			} else {
				fine = vbs_raise(f->failure, VBS_E_TYPE_MISMATCH, NULL);
			}
			if (fine) {
				if (!set)
					fine = plain(f, value);
				if (fine) {
					args[target->arg_count] = *value;
					VariantInit(value);
					fine = member_call(f, object, name, put, args, target->arg_count + 1, NULL);
					VariantClear(&args[target->arg_count]);
				}
			}
			if (object)
				IDispatch_Release(object);
		}
		free_arguments(args, target->arg_count);
		vbs_free(args);
		VariantClear(value);
		return fine;
	}
	VariantClear(value);
	return vbs_raise(f->failure, VBS_E_TYPE_MISMATCH, L"Illegal assignment");
}

/* Statements. */

static BOOL condition(frame *f, expr *e, BOOL *out)
{
	VARIANT v;
	if (!evaluate(f, e, &v))
		return FALSE;
	HRESULT hr = vbs_to_bool(&v, out);
	VariantClear(&v);
	return SUCCEEDED(hr) || raise_result(f->failure, hr);
}

static BOOL declare(frame *f, stmt *s)
{
	for (UINT i = 0; i < s->name_count; i++) {
		VARIANT *place = NULL;
		if (f->global) {
			variable *existing = find_in(f->en->globals, s->names[i]);
			if (existing && !s->arrays[i])
				continue;
			place = existing ? slot(existing) : &add_to(&f->en->globals, s->names[i])->value;
		} else {
			if (find_in(f->locals, s->names[i]))
				return vbs_raise(f->failure, VBS_E_NAME_REDEFINED, NULL);
			place = &add_to(&f->locals, s->names[i])->value;
		}
		if (s->arrays[i]) {
			LONG upper[60];
			UINT dims = s->bound_counts[i];
			if (dims > 60 || !bounds_of(f, s->bounds[i], dims, upper))
				return FALSE;
			VariantClear(place);
			if (dims) {
				SAFEARRAY *array = new_array(f, upper, dims);
				if (!array)
					return FALSE;
				V_VT(place) = VT_ARRAY | VT_VARIANT;
				V_ARRAY(place) = array;
			} else {
				/* Dim a(): an array to size with ReDim, which is a dynamic one with no elements. */
				SAFEARRAYBOUND none = { 0, 0 };
				V_VT(place) = VT_ARRAY | VT_VARIANT;
				V_ARRAY(place) = SafeArrayCreate(VT_VARIANT, 1, &none);
			}
		}
	}
	return TRUE;
}

static BOOL redim(frame *f, stmt *s)
{
	for (UINT i = 0; i < s->name_count; i++) {
		LONG upper[60];
		UINT dims = s->bound_counts[i];
		if (dims > 60 || !bounds_of(f, s->bounds[i], dims, upper))
			return FALSE;
		VARIANT *place = find_variable(f, s->names[i]);
		if (!place) {
			if (f->en->explicit_names)
				return vbs_raise(f->failure, VBS_E_UNDEFINED, NULL);
			place = declare_here(f, s->names[i]);
		}
		SAFEARRAY *old = array_of(place);
		if (s->preserve && old && SafeArrayGetDim(old) == dims) {
			/* Preserve keeps the elements; only the last dimension may change, as on Windows. */
			for (UINT d = 0; d + 1 < dims; d++) {
				LONG low, high;
				SafeArrayGetUBound(old, d + 1, &high);
				SafeArrayGetLBound(old, d + 1, &low);
				if (high != upper[d])
					return vbs_raise(f->failure, VBS_E_SUBSCRIPT, NULL);
			}
			SAFEARRAYBOUND last = { (ULONG)(upper[dims - 1] + 1), 0 };
			HRESULT hr = SafeArrayRedim(old, &last);
			if (FAILED(hr))
				return raise_result(f->failure, hr);
			continue;
		}
		SAFEARRAY *array = new_array(f, upper, dims);
		if (!array)
			return FALSE;
		VariantClear(place);
		V_VT(place) = VT_ARRAY | VT_VARIANT;
		V_ARRAY(place) = array;
	}
	return TRUE;
}

static BOOL each_value(frame *f, stmt *s, VARIANT *item, flow *result)
{
	VARIANT *place = find_variable(f, s->name);
	if (!place) {
		if (f->en->explicit_names)
			return vbs_raise(f->failure, VBS_E_UNDEFINED, NULL);
		place = declare_here(f, s->name);
	}
	VariantClear(place);
	if (FAILED(VariantCopyInd(place, item)))
		return vbs_raise(f->failure, VBS_E_OUT_OF_MEMORY, NULL);
	*result = run_block(f, &s->blocks[0]);
	return TRUE;
}

static flow for_each(frame *f, stmt *s)
{
	VARIANT collection;
	if (!evaluate(f, s->value, &collection))
		return F_ERROR;
	flow result = F_NORMAL;
	SAFEARRAY *array = array_of(&collection);
	if (array) {
		/* Every element, in the order the array keeps them. */
		VARIANT *data;
		if (FAILED(SafeArrayAccessData(array, (void **)&data))) {
			VariantClear(&collection);
			return vbs_raise(f->failure, VBS_E_SUBSCRIPT, NULL), F_ERROR;
		}
		ULONG total = 1;
		for (UINT d = 0; d < SafeArrayGetDim(array); d++)
			total *= array->rgsabound[d].cElements;
		SafeArrayUnaccessData(array);
		for (ULONG i = 0; i < total && result != F_ERROR; i++) {
			VARIANT item;
			VariantInit(&item);
			SafeArrayAccessData(array, (void **)&data);
			VariantCopy(&item, &data[i]);
			SafeArrayUnaccessData(array);
			if (!each_value(f, s, &item, &result))
				result = F_ERROR;
			VariantClear(&item);
			if (result == F_EXIT_FOR) {
				result = F_NORMAL;
				break;
			}
			if (result != F_NORMAL)
				break;
		}
		VariantClear(&collection);
		return result;
	}
	if (V_VT(&collection) != VT_DISPATCH || !V_DISPATCH(&collection)) {
		VariantClear(&collection);
		return vbs_raise(f->failure, VBS_E_OBJECT_REQUIRED, NULL), F_ERROR;
	}
	/* A collection hands out an enumerator through DISPID_NEWENUM. */
	DISPPARAMS none = { 0 };
	VARIANT enumerator;
	VariantInit(&enumerator);
	HRESULT hr = IDispatch_Invoke(V_DISPATCH(&collection), DISPID_NEWENUM, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD | DISPATCH_PROPERTYGET,
		&none, &enumerator, NULL, NULL);
	VariantClear(&collection);
	IEnumVARIANT *items = NULL;
	if (SUCCEEDED(hr) && (V_VT(&enumerator) == VT_UNKNOWN || V_VT(&enumerator) == VT_DISPATCH) && V_UNKNOWN(&enumerator))
		IUnknown_QueryInterface(V_UNKNOWN(&enumerator), &IID_IEnumVARIANT, (void **)&items);
	VariantClear(&enumerator);
	if (!items)
		return vbs_raise(f->failure, 451, L"Object not a collection"), F_ERROR;
	for (;;) {
		VARIANT item;
		VariantInit(&item);
		ULONG fetched = 0;
		if (IEnumVARIANT_Next(items, 1, &item, &fetched) != S_OK || !fetched)
			break;
		BOOL fine = each_value(f, s, &item, &result);
		VariantClear(&item);
		if (!fine)
			result = F_ERROR;
		if (result == F_EXIT_FOR) {
			result = F_NORMAL;
			break;
		}
		if (result != F_NORMAL)
			break;
	}
	IEnumVARIANT_Release(items);
	return result;
}

static flow for_loop(frame *f, stmt *s)
{
	VARIANT start, end, step;
	VariantInit(&step);
	if (!evaluate(f, s->value, &start))
		return F_ERROR;
	if (!evaluate(f, s->to, &end) || !plain(f, &start) || !plain(f, &end)) {
		VariantClear(&start);
		return F_ERROR;
	}
	if (s->step) {
		if (!evaluate(f, s->step, &step) || !plain(f, &step)) {
			VariantClear(&start);
			VariantClear(&end);
			return F_ERROR;
		}
	} else {
		V_VT(&step) = VT_I2;
		V_I2(&step) = 1;
	}
	double direction = 0;
	if (FAILED(vbs_to_number(&step, &direction)) || FAILED(vbs_to_number(&end, &(double){ 0 }))) {
		VariantClear(&start);
		VariantClear(&end);
		VariantClear(&step);
		return vbs_raise(f->failure, VBS_E_TYPE_MISMATCH, NULL), F_ERROR;
	}
	VARIANT *place = find_variable(f, s->name);
	if (!place) {
		if (f->en->explicit_names) {
			VariantClear(&start);
			VariantClear(&end);
			VariantClear(&step);
			return vbs_raise(f->failure, VBS_E_UNDEFINED, NULL), F_ERROR;
		}
		place = declare_here(f, s->name);
	}
	VariantClear(place);
	*place = start;
	flow result = F_NORMAL;
	for (;;) {
		/* The counter is read again each time round: the body may have changed it. */
		place = find_variable(f, s->name);
		HRESULT order = VarCmp(place, &end, LOCALE_USER_DEFAULT, 0);
		if (FAILED(order)) {
			result = (raise_result(f->failure, order), F_ERROR);
			break;
		}
		if ((direction >= 0 && order == VARCMP_GT) || (direction < 0 && order == VARCMP_LT))
			break;
		result = run_block(f, &s->blocks[0]);
		if (result == F_EXIT_FOR) {
			result = F_NORMAL;
			break;
		}
		if (result != F_NORMAL)
			break;
		place = find_variable(f, s->name);
		VARIANT next;
		HRESULT hr = VarAdd(place, &step, &next);
		if (FAILED(hr)) {
			result = (raise_result(f->failure, hr), F_ERROR);
			break;
		}
		VariantClear(place);
		*place = next;
	}
	VariantClear(&end);
	VariantClear(&step);
	return result;
}

static flow select_case(frame *f, stmt *s)
{
	VARIANT tested;
	if (!evaluate(f, s->value, &tested) || !plain(f, &tested))
		return F_ERROR;
	for (UINT i = 0; i < s->branch_count; i++)
		for (UINT j = 0; j < s->case_counts[i]; j++) {
			VARIANT candidate, same;
			if (!evaluate(f, s->case_values[i][j], &candidate) || !plain(f, &candidate)) {
				VariantClear(&tested);
				return F_ERROR;
			}
			VariantInit(&same);
			BOOL fine = compare(f, &tested, &candidate, L"=", &same);
			VariantClear(&candidate);
			if (!fine) {
				VariantClear(&tested);
				return F_ERROR;
			}
			if (V_VT(&same) == VT_BOOL && V_BOOL(&same)) {
				VariantClear(&tested);
				return run_block(f, &s->blocks[i]);
			}
		}
	VariantClear(&tested);
	return s->has_otherwise ? run_block(f, &s->otherwise) : F_NORMAL;
}

static flow run_statement(frame *f, stmt *s)
{
	switch (s->kind) {
	case S_CALL: {
		VARIANT ignored;
		VariantInit(&ignored);
		expr *call = s->target;
		BOOL fine;
		if (call->target->kind == E_NAME || call->target->kind == E_MEMBER)
			fine = call_value(f, call->target, call->args, call->arg_count, call->arg_count != 0, &ignored, DISPATCH_METHOD);
		else
			fine = evaluate(f, call, &ignored);
		VariantClear(&ignored);
		return fine ? F_NORMAL : F_ERROR;
	}
	case S_ASSIGN:
	case S_SET: {
		VARIANT v;
		if (!evaluate(f, s->value, &v))
			return F_ERROR;
		return assign(f, s->target, &v, s->kind == S_SET) ? F_NORMAL : F_ERROR;
	}
	case S_IF:
		for (UINT i = 0; i < s->branch_count; i++) {
			BOOL truth;
			if (!condition(f, s->conditions[i], &truth))
				return F_ERROR;
			if (truth)
				return run_block(f, &s->blocks[i]);
		}
		return s->has_otherwise ? run_block(f, &s->otherwise) : F_NORMAL;
	case S_FOR:
		return for_loop(f, s);
	case S_FOR_EACH:
		return for_each(f, s);
	case S_DO:
		for (;;) {
			BOOL truth = TRUE;
			if (s->test_first && s->conditions[0]) {
				if (!condition(f, s->conditions[0], &truth))
					return F_ERROR;
				if (s->until ? truth : !truth)
					return F_NORMAL;
			}
			flow result = run_block(f, &s->blocks[0]);
			if (result == F_EXIT_DO)
				return F_NORMAL;
			if (result != F_NORMAL)
				return result;
			if (!s->test_first && s->conditions[0]) {
				if (!condition(f, s->conditions[0], &truth))
					return F_ERROR;
				if (s->until ? truth : !truth)
					return F_NORMAL;
			}
		}
	case S_WHILE:
		for (;;) {
			BOOL truth;
			if (!condition(f, s->conditions[0], &truth))
				return F_ERROR;
			if (!truth)
				return F_NORMAL;
			flow result = run_block(f, &s->blocks[0]);
			if (result != F_NORMAL)
				return result;
		}
	case S_EXIT:
		if (vbs_same(s->exit_kind, L"do"))
			return F_EXIT_DO;
		if (vbs_same(s->exit_kind, L"for"))
			return F_EXIT_FOR;
		return F_EXIT_PROCEDURE;
	case S_DIM:
		return declare(f, s) ? F_NORMAL : F_ERROR;
	case S_REDIM:
		return redim(f, s) ? F_NORMAL : F_ERROR;
	case S_ERASE:
		for (UINT i = 0; i < s->name_count; i++) {
			VARIANT *place = find_variable(f, s->names[i]);
			SAFEARRAY *array = place ? array_of(place) : NULL;
			if (!array)
				return vbs_raise(f->failure, VBS_E_TYPE_MISMATCH, NULL), F_ERROR;
			/* Erase empties a dynamic array; a fixed one keeps its size with its elements emptied. */
			VariantClear(place);
		}
		return F_NORMAL;
	case S_CONST:
		for (UINT i = 0; i < s->name_count; i++) {
			VARIANT v;
			if (!evaluate(f, s->conditions[i], &v) || !plain(f, &v))
				return F_ERROR;
			variable *made = f->global ? add_to(&f->en->globals, s->names[i]) : add_to(&f->locals, s->names[i]);
			made->constant = TRUE;
			made->value = v;
		}
		return F_NORMAL;
	case S_ON_ERROR:
		f->resume_next = s->resume_next;
		vbs_failure_clear(&f->en->err);
		return F_NORMAL;
	case S_SELECT:
		return select_case(f, s);
	case S_WITH: {
		IDispatch *object;
		if (!object_of(f, s->value, &object))
			return F_ERROR;
		if (f->with_depth == ARRAYSIZE(f->with)) {
			IDispatch_Release(object);
			return vbs_raise(f->failure, 28, L"Out of stack space"), F_ERROR;
		}
		f->with[f->with_depth++] = object;
		flow result = run_block(f, &s->blocks[0]);
		f->with_depth--;
		IDispatch_Release(f->with[f->with_depth]);
		return result;
	}
	case S_RANDOMIZE: {
		double seed = (double)GetTickCount();
		if (s->value) {
			VARIANT v;
			if (!evaluate(f, s->value, &v))
				return F_ERROR;
			HRESULT hr = vbs_to_number(&v, &seed);
			VariantClear(&v);
			if (FAILED(hr))
				return raise_result(f->failure, hr), F_ERROR;
		}
		f->en->random_seed = (ULONG)(LONGLONG)(seed * 1000) ^ 0x50000;
		return F_NORMAL;
	}
	case S_EXECUTE:
		break;
	}
	return F_NORMAL;
}

/* A statement's error goes on to the caller, or into Err when On Error Resume Next is in effect. */
static flow run_block(frame *f, block *b)
{
	for (UINT i = 0; i < b->count; i++) {
		script_failure *failure = f->failure;
		flow result = run_statement(f, b->items[i]);
		if (result == F_ERROR) {
			if (!failure->number)
				vbs_raise(failure, VBS_E_UNDEFINED, NULL);
			if (!failure->line)
				failure->line = b->items[i]->line;
			if (!f->resume_next)
				return F_ERROR;
			vbs_failure_clear(&f->en->err);
			f->en->err = *failure;
			*failure = (script_failure){ 0 };
			continue;
		}
		if (result != F_NORMAL)
			return result;
	}
	return F_NORMAL;
}

/* Procedures. */

static BOOL call_procedure(engine *en, procedure *proc, instance *self, VARIANT *args, UINT count, VARIANT *result, script_failure *failure)
{
	if (count > proc->param_count)
		return vbs_raise(failure, VBS_E_WRONG_ARGUMENTS, NULL);
	if (count < proc->param_count && !(proc->kind == P_LET || proc->kind == P_SET))
		return vbs_raise(failure, VBS_E_WRONG_ARGUMENTS, NULL);
	if (en->depth > 256)
		return vbs_raise(failure, 28, L"Out of stack space");
	frame f = { .en = en, .proc = proc, .self = self, .failure = failure };
	VariantInit(&f.result);
	/* Parameters: a reference to the caller's variable for ByRef, a copy of the value otherwise; for Let and Set the value comes last. */
	for (UINT i = 0; i < proc->param_count; i++) {
		variable *param = add_to(&f.locals, proc->params[i]);
		if (i >= count)
			continue;
		if (V_VT(&args[i]) == (VT_BYREF | VT_VARIANT) && !proc->by_value[i]) {
			param->value = args[i];
		} else if (V_VT(&args[i]) == VT_ERROR && V_ERROR(&args[i]) == DISP_E_PARAMNOTFOUND) {
			/* A left-out argument reads as a missing value. */
			param->value = args[i];
		} else {
			HRESULT hr = V_VT(&args[i]) == (VT_BYREF | VT_VARIANT) ? VariantCopyInd(&param->value, V_VARIANTREF(&args[i])) :
				VariantCopyInd(&param->value, &args[i]);
			if (FAILED(hr)) {
				free_variables(f.locals);
				return raise_result(failure, hr);
			}
		}
	}
	if (self)
		IDispatch_AddRef(&self->IDispatch_iface);
	en->depth++;
	flow done = run_block(&f, &proc->body);
	en->depth--;
	for (UINT i = 0; i < f.with_depth; i++)
		IDispatch_Release(f.with[i]);
	free_variables(f.locals);
	if (self)
		IDispatch_Release(&self->IDispatch_iface);
	if (done == F_ERROR) {
		VariantClear(&f.result);
		return FALSE;
	}
	if (result)
		*result = f.result;
	else
		VariantClear(&f.result);
	return TRUE;
}

static BOOL call_builtin(frame *f, const WCHAR *name, VARIANT *args, UINT count, VARIANT *out, BOOL *found)
{
	builtin_call call = { f->en, args, count, out, f->failure };
	return vbs_builtin(name, &call, found);
}

BOOL vbs_run(engine *en, script_code *code, script_failure *failure)
{
	frame f = { .en = en, .global = TRUE, .failure = failure };
	VariantInit(&f.result);
	flow done = run_block(&f, &code->main);
	for (UINT i = 0; i < f.with_depth; i++)
		IDispatch_Release(f.with[i]);
	return done != F_ERROR;
}

BOOL vbs_evaluate(engine *en, script_code *code, VARIANT *result, script_failure *failure)
{
	frame f = { .en = en, .global = TRUE, .failure = failure };
	return evaluate(&f, code->expression, result) && plain(&f, result);
}

BOOL vbs_call_global(engine *en, const WCHAR *name, VARIANT *args, UINT count, VARIANT *result, script_failure *failure)
{
	procedure *proc = vbs_global_procedure(en, name);
	if (!proc)
		return vbs_raise(failure, VBS_E_UNDEFINED, NULL);
	return call_procedure(en, proc, NULL, args, count, result, failure);
}

void vbs_runtime_free(engine *en)
{
	free_variables(en->globals);
	en->globals = NULL;
}

/* A class instance's IDispatch: its public fields and procedures by name, the default member as DISPID_VALUE. */

#define FIELD_IDS 0x1000
#define PROCEDURE_IDS 0x2000

static HRESULT WINAPI instance_QueryInterface(IDispatch *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch)) {
		*out = iface;
		IDispatch_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI instance_AddRef(IDispatch *iface)
{
	return InterlockedIncrement(&CONTAINING_RECORD(iface, instance, IDispatch_iface)->refs);
}

static ULONG WINAPI instance_Release(IDispatch *iface)
{
	instance *object = CONTAINING_RECORD(iface, instance, IDispatch_iface);
	LONG refs = InterlockedDecrement(&object->refs);
	if (refs)
		return refs;
	if (!object->terminated) {
		/* Class_Terminate runs once, with the object alive for its length. */
		object->terminated = TRUE;
		procedure *end = class_procedure(object->cls, L"Class_Terminate", DISPATCH_METHOD, FALSE);
		if (end) {
			object->refs = 1;
			script_failure failure = { 0 };
			call_procedure(object->en, end, object, NULL, 0, NULL, &failure);
			vbs_failure_clear(&failure);
			if (InterlockedDecrement(&object->refs))
				return object->refs;
		}
	}
	free_variables(object->fields);
	vbs_free(object);
	return 0;
}

static HRESULT WINAPI instance_GetTypeInfoCount(IDispatch *iface, UINT *count)
{
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI instance_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **info)
{
	*info = NULL;
	return DISP_E_BADINDEX;
}

static HRESULT WINAPI instance_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	instance *object = CONTAINING_RECORD(iface, instance, IDispatch_iface);
	class_def *c = object->cls;
	HRESULT hr = S_OK;
	for (UINT n = 0; n < count; n++) {
		ids[n] = DISPID_UNKNOWN;
		if (n)
			continue;
		for (UINT i = 0; i < c->field_count && ids[n] == DISPID_UNKNOWN; i++)
			if (!c->field_private[i] && vbs_same(c->fields[i], names[n]))
				ids[n] = FIELD_IDS + i;
		for (UINT i = 0; i < c->procedure_count && ids[n] == DISPID_UNKNOWN; i++)
			if (!c->procedures[i]->is_private && vbs_same(c->procedures[i]->name, names[n]))
				ids[n] = PROCEDURE_IDS + i;
		if (ids[n] == DISPID_UNKNOWN)
			hr = DISP_E_UNKNOWNNAME;
	}
	return count > 1 && hr == S_OK ? DISP_E_UNKNOWNNAME : hr;
}

static HRESULT WINAPI instance_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, UINT *bad)
{
	instance *object = CONTAINING_RECORD(iface, instance, IDispatch_iface);
	class_def *c = object->cls;
	UINT count = params ? params->cArgs : 0;
	VARIANT *args = count ? vbs_alloc(count * sizeof(VARIANT)) : NULL;
	/* Back to the order of the call; a put's value, which DISPPARAMS lists first, goes last. */
	for (UINT i = 0; i < count; i++)
		args[i] = params->rgvarg[count - 1 - i];
	script_failure failure = { 0 };
	HRESULT hr = S_OK;
	VARIANT value;
	VariantInit(&value);
	if (id >= FIELD_IDS && id < FIELD_IDS + (DISPID)c->field_count) {
		variable *field = find_in(object->fields, c->fields[id - FIELD_IDS]);
		VARIANT *place = slot(field);
		if (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) {
			if (!count) {
				hr = DISP_E_BADPARAMCOUNT;
			} else if (count == 1) {
				VariantClear(place);
				hr = VariantCopyInd(place, &args[0]);
			} else {
				frame f = { .en = object->en, .self = object, .failure = &failure };
				VARIANT *at = element(&f, array_of(place), args, count - 1);
				hr = at ? (VariantClear(at), VariantCopyInd(at, &args[count - 1])) : DISP_E_EXCEPTION;
			}
		} else if (count) {
			frame f = { .en = object->en, .self = object, .failure = &failure };
			VARIANT *at = element(&f, array_of(place), args, count);
			hr = at ? VariantCopy(&value, at) : DISP_E_EXCEPTION;
		} else {
			hr = VariantCopy(&value, place);
		}
	} else {
		procedure *proc = NULL;
		if (id == DISPID_VALUE)
			proc = default_procedure(c, flags);
		else if (id >= PROCEDURE_IDS && id < PROCEDURE_IDS + (DISPID)c->procedure_count)
			proc = class_procedure(c, c->procedures[id - PROCEDURE_IDS]->name, flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF) ? flags :
				DISPATCH_PROPERTYGET, TRUE);
		if (!proc)
			hr = DISP_E_MEMBERNOTFOUND;
		else if (!call_procedure(object->en, proc, object, args, count, &value, &failure))
			hr = DISP_E_EXCEPTION;
	}
	vbs_free(args);
	if (hr == DISP_E_EXCEPTION) {
		if (info) {
			memset(info, 0, sizeof(*info));
			info->scode = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_CONTROL, failure.number);
			info->bstrDescription = SysAllocString(failure.description);
			info->bstrSource = SysAllocString(failure.source);
		}
		vbs_failure_clear(&failure);
		VariantClear(&value);
		return hr;
	}
	vbs_failure_clear(&failure);
	if (result)
		*result = value;
	else
		VariantClear(&value);
	return hr;
}

BSTR vbs_class_name(IDispatch *object)
{
	instance *self = as_instance(object);
	return self ? SysAllocString(self->cls->name) : NULL;
}

static IDispatchVtbl instance_vtbl = {
	instance_QueryInterface, instance_AddRef, instance_Release, instance_GetTypeInfoCount, instance_GetTypeInfo, instance_GetIDsOfNames,
	instance_Invoke,
};
