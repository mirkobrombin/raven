/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * Arithmetic, logic and comparison over variants. Every operation does the same three things:
 * look at the two types and decide a result type, convert both sides to it, work there.
 */
#include "oa.h"
#include "number.h"

#define FLOAT_MAX 3.4028234663852886e38

enum support
{
	/* I1, UI2, UI4, UI8, INT and UINT are refused by the older operators. */
	CORE_TYPES,
	ALL_TYPES,
};

/* A side of an operation, with any by-reference or object value already fetched. */
struct side
{
	VARIANT value;
	BOOL owned;
};

static BOOL is_extended(VARTYPE type)
{
	return type == VT_I1 || type == VT_UI2 || type == VT_UI4 || type == VT_UI8 || type == VT_INT || type == VT_UINT;
}

static BOOL is_core(VARTYPE type)
{
	switch (type)
	{
	case VT_EMPTY:
	case VT_NULL:
	case VT_I2:
	case VT_I4:
	case VT_R4:
	case VT_R8:
	case VT_CY:
	case VT_DATE:
	case VT_BSTR:
	case VT_BOOL:
	case VT_DECIMAL:
	case VT_UI1:
	case VT_I8:
		return TRUE;
	}
	return FALSE;
}

static HRESULT check_type(const VARIANT *value, enum support support)
{
	VARTYPE full = V_VT(value), type = full & VT_TYPEMASK;
	if (full & VT_ARRAY)
		return DISP_E_TYPEMISMATCH;
	/* By reference is never followed here: the caller is expected to have done it. */
	if (full & (VT_VECTOR | VT_RESERVED | VT_BYREF))
		return DISP_E_BADVARTYPE;
	if (is_core(type))
		return S_OK;
	if (is_extended(type))
		return support == ALL_TYPES ? S_OK : DISP_E_BADVARTYPE;
	switch (type)
	{
	case VT_DISPATCH:
		return S_OK;
	case VT_UNKNOWN:
	case VT_ERROR:
	case VT_RECORD:
		return DISP_E_TYPEMISMATCH;
	}
	return DISP_E_BADVARTYPE;
}

static HRESULT object_value(IDispatch *object, VARIANT *out)
{
	DISPPARAMS none = {NULL, NULL, 0, 0};
	if (!object)
		return DISP_E_TYPEMISMATCH;
	V_VT(out) = VT_EMPTY;
	return IDispatch_Invoke(object, DISPID_VALUE, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &none, out, NULL, NULL);
}

static HRESULT side_load(struct side *side, VARIANT *from)
{
	HRESULT status;
	side->owned = FALSE;
	if (V_VT(from) & VT_BYREF)
	{
		V_VT(&side->value) = VT_EMPTY;
		status = VariantCopyInd(&side->value, from);
		side->owned = SUCCEEDED(status);
		return status;
	}
	if (V_VT(from) == VT_DISPATCH)
	{
		status = object_value(V_DISPATCH(from), &side->value);
		side->owned = SUCCEEDED(status);
		return status;
	}
	side->value = *from;
	return S_OK;
}

static void side_free(struct side *side)
{
	if (side->owned)
		VariantClear(&side->value);
	side->owned = FALSE;
}

static HRESULT sides_load(struct side *left, struct side *right, VARIANT *from_left, VARIANT *from_right)
{
	HRESULT status = side_load(left, from_left);
	if (FAILED(status))
		return status;
	status = side_load(right, from_right);
	if (FAILED(status))
		side_free(left);
	return status;
}

static HRESULT check_pair(VARIANT *left, VARIANT *right, enum support support)
{
	HRESULT status = check_type(left, support);
	return FAILED(status) ? status : check_type(right, support);
}

static BOOL either_null(const VARIANT *left, const VARIANT *right)
{
	return (V_VT(left) & VT_TYPEMASK) == VT_NULL || (V_VT(right) & VT_TYPEMASK) == VT_NULL;
}

static BOOL has(VARTYPE left, VARTYPE right, VARTYPE type)
{
	return left == type || right == type;
}

static HRESULT to_type(const VARIANT *from, VARTYPE type, VARIANT *out)
{
	V_VT(out) = VT_EMPTY;
	return VariantChangeTypeEx(out, (VARIANT *)from, LOCALE_USER_DEFAULT, 0, type);
}

/* A whole number result moves up to the next type that holds it. */
static HRESULT store_whole(LONG64 value, VARTYPE type, VARIANT *out)
{
	if (type == VT_UI1 && value >= 0 && value <= 255)
	{
		V_VT(out) = VT_UI1;
		V_UI1(out) = (BYTE)value;
		return S_OK;
	}
	if (type == VT_BOOL)
	{
		V_VT(out) = VT_BOOL;
		V_BOOL(out) = (VARIANT_BOOL)value;
		return S_OK;
	}
	if ((type == VT_UI1 || type == VT_I2) && value >= -32768 && value <= 32767)
	{
		V_VT(out) = VT_I2;
		V_I2(out) = (SHORT)value;
		return S_OK;
	}
	if (type == VT_I8)
	{
		V_VT(out) = VT_I8;
		V_I8(out) = value;
		return S_OK;
	}
	if (value >= -2147483647LL - 1 && value <= 2147483647LL)
	{
		V_VT(out) = VT_I4;
		V_I4(out) = (LONG)value;
		return S_OK;
	}
	V_VT(out) = VT_R8;
	V_R8(out) = (double)value;
	return S_OK;
}

enum arith
{
	OP_ADD,
	OP_SUB,
	OP_MUL,
};

static VARTYPE arith_type(VARTYPE left, VARTYPE right, enum arith op)
{
	if (has(left, right, VT_DECIMAL))
		return VT_DECIMAL;
	if (op != OP_MUL && has(left, right, VT_DATE))
		return left == VT_DATE && right == VT_DATE && op == OP_SUB ? VT_R8 : VT_DATE;
	if (op != OP_MUL && has(left, right, VT_CY))
		return VT_CY;
	if (has(left, right, VT_R8) || has(left, right, VT_DATE))
		return VT_R8;
	if (has(left, right, VT_BSTR))
	{
		if (op == OP_ADD && (left == VT_BSTR && right == VT_BSTR || has(left, right, VT_EMPTY)))
			return VT_BSTR;
		return VT_R8;
	}
	if (has(left, right, VT_R4))
		return has(left, right, VT_I4) || has(left, right, VT_I8) || has(left, right, VT_CY) ? VT_R8 : VT_R4;
	if (has(left, right, VT_CY))
		return VT_CY;
	if (has(left, right, VT_I8))
		return VT_I8;
	if (has(left, right, VT_I4))
		return VT_I4;
	if (has(left, right, VT_I2) || has(left, right, VT_BOOL) || left == VT_EMPTY && right == VT_EMPTY)
		return VT_I2;
	return VT_UI1;
}

static HRESULT arith_whole(LONG64 left, LONG64 right, enum arith op, VARTYPE type, VARIANT *out)
{
	LONG64 value;
	BOOL overflow;
	if (op == OP_ADD)
		overflow = __builtin_add_overflow(left, right, &value);
	else if (op == OP_SUB)
		overflow = __builtin_sub_overflow(left, right, &value);
	else
		overflow = __builtin_mul_overflow(left, right, &value);
	if (overflow)
		return DISP_E_OVERFLOW;
	return store_whole(value, type, out);
}

static double arith_real(double left, double right, enum arith op)
{
	if (op == OP_ADD)
		return left + right;
	return op == OP_SUB ? left - right : left * right;
}

static BOOL real_overflows(double value)
{
	return value - value != 0.0;
}

/* What the older operators refuse, and in which words. */
static HRESULT arith_check(const VARIANT *left, const VARIANT *right)
{
	VARTYPE l = V_VT(left), r = V_VT(right);
	HRESULT first = check_type(left, CORE_TYPES), second = check_type(right, CORE_TYPES);
	if ((l | r) & VT_ARRAY)
		return DISP_E_TYPEMISMATCH;
	if ((l | r) & ~VT_TYPEMASK)
		return DISP_E_BADVARTYPE;
	if (r == VT_RECORD)
		return DISP_E_TYPEMISMATCH;
	if (l == VT_RECORD)
		return r <= VT_UI1 || r == VT_I8 ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
	if (first == DISP_E_BADVARTYPE || second == DISP_E_BADVARTYPE || l == VT_UNKNOWN || r == VT_UNKNOWN)
		return DISP_E_BADVARTYPE;
	if (l == VT_ERROR || r == VT_ERROR)
		return DISP_E_TYPEMISMATCH;
	if ((l == VT_DISPATCH || r == VT_DISPATCH) && !(l == VT_NULL || r == VT_NULL))
		return DISP_E_TYPEMISMATCH;
	return S_OK;
}

static HRESULT arith_run(VARIANT *from_left, VARIANT *from_right, VARIANT *out, enum arith op)
{
	struct side left, right;
	VARIANT a, b, result;
	VARTYPE type;
	HRESULT status = arith_check(from_left, from_right);
	if (FAILED(status))
		return status;
	V_VT(&a) = V_VT(&b) = V_VT(&result) = VT_EMPTY;
	if (either_null(from_left, from_right))
	{
		V_VT(out) = VT_NULL;
		return S_OK;
	}
	status = sides_load(&left, &right, from_left, from_right);
	if (FAILED(status))
		return status;
	status = check_pair(&left.value, &right.value, CORE_TYPES);
	if (FAILED(status))
		goto done;
	if (either_null(&left.value, &right.value))
	{
		V_VT(&result) = VT_NULL;
		goto store;
	}
	type = arith_type(V_VT(&left.value), V_VT(&right.value), op);
	if (type == VT_BSTR)
	{
		status = VarCat(&left.value, &right.value, &result);
		goto store;
	}
	/* Whole numbers are worked in 64 bits and then narrowed; the rest in their own type. */
	{
		VARTYPE work = type;
		if (type == VT_I2 || type == VT_I4 || type == VT_UI1)
			work = VT_I8;
		else if (type == VT_DATE || type == VT_R4)
			work = VT_R8;
		status = to_type(&left.value, work, &a);
		if (SUCCEEDED(status))
			status = to_type(&right.value, work, &b);
		if (FAILED(status))
			goto store;
		switch (type)
		{
		case VT_UI1:
		case VT_I2:
		case VT_I4:
		case VT_I8:
			status = arith_whole(V_I8(&a), V_I8(&b), op, type, &result);
			break;
		case VT_R4:
		{
			double value = arith_real(V_R8(&a), V_R8(&b), op);
			if (real_overflows(value))
				status = DISP_E_OVERFLOW;
			else if (value > FLOAT_MAX || value < -FLOAT_MAX)
			{
				V_VT(&result) = VT_R8;
				V_R8(&result) = value;
			}
			else
			{
				V_VT(&result) = VT_R4;
				V_R4(&result) = (float)value;
			}
			break;
		}
		case VT_R8:
		case VT_DATE:
			V_R8(&result) = arith_real(V_R8(&a), V_R8(&b), op);
			V_VT(&result) = type;
			if (real_overflows(V_R8(&result)))
				status = DISP_E_OVERFLOW;
			break;
		case VT_CY:
			V_VT(&result) = VT_CY;
			if (op == OP_ADD)
				status = VarCyAdd(V_CY(&a), V_CY(&b), &V_CY(&result));
			else if (op == OP_SUB)
				status = VarCySub(V_CY(&a), V_CY(&b), &V_CY(&result));
			else
				status = VarCyMul(V_CY(&a), V_CY(&b), &V_CY(&result));
			break;
		case VT_DECIMAL:
		{
			DECIMAL value;
			if (op == OP_ADD)
				status = VarDecAdd(&V_DECIMAL(&a), &V_DECIMAL(&b), &value);
			else if (op == OP_SUB)
				status = VarDecSub(&V_DECIMAL(&a), &V_DECIMAL(&b), &value);
			else
				status = VarDecMul(&V_DECIMAL(&a), &V_DECIMAL(&b), &value);
			if (SUCCEEDED(status))
			{
				V_DECIMAL(&result) = value;
				V_VT(&result) = VT_DECIMAL;
			}
			break;
		}
		}
	}
store:
	if (SUCCEEDED(status))
		*out = result;
done:
	side_free(&left);
	side_free(&right);
	VariantClear(&a);
	VariantClear(&b);
	return status;
}

HRESULT WINAPI VarAdd(LPVARIANT left, LPVARIANT right, LPVARIANT out)
{
	return arith_run(left, right, out, OP_ADD);
}

HRESULT WINAPI VarSub(LPVARIANT left, LPVARIANT right, LPVARIANT out)
{
	return arith_run(left, right, out, OP_SUB);
}

HRESULT WINAPI VarMul(LPVARIANT left, LPVARIANT right, LPVARIANT out)
{
	return arith_run(left, right, out, OP_MUL);
}

HRESULT WINAPI VarDiv(LPVARIANT from_left, LPVARIANT from_right, LPVARIANT out)
{
	struct side left, right;
	VARIANT a, b, result;
	VARTYPE l, r, type;
	HRESULT status = check_pair(from_left, from_right, CORE_TYPES);
	if (FAILED(status))
		return status;
	if (either_null(from_left, from_right))
	{
		V_VT(out) = VT_NULL;
		return S_OK;
	}
	status = sides_load(&left, &right, from_left, from_right);
	if (FAILED(status))
		return status;
	V_VT(&a) = V_VT(&b) = V_VT(&result) = VT_EMPTY;
	l = V_VT(&left.value);
	r = V_VT(&right.value);
	if (l == VT_NULL || r == VT_NULL)
		V_VT(&result) = VT_NULL;
	else if (has(l, r, VT_DECIMAL))
	{
		status = to_type(&left.value, VT_DECIMAL, &a);
		if (SUCCEEDED(status))
			status = to_type(&right.value, VT_DECIMAL, &b);
		if (SUCCEEDED(status))
		{
			DECIMAL value;
			status = VarDecDiv(&V_DECIMAL(&a), &V_DECIMAL(&b), &value);
			if (SUCCEEDED(status))
			{
				V_DECIMAL(&result) = value;
				V_VT(&result) = VT_DECIMAL;
			}
		}
	}
	else
	{
		type = VT_R8;
		if (has(l, r, VT_R4))
		{
			VARTYPE other = l == VT_R4 ? r : l;
			if (other == VT_R4 || other == VT_UI1 || other == VT_BOOL || other == VT_I2 || other == VT_EMPTY)
				type = VT_R4;
		}
		status = to_type(&left.value, VT_R8, &a);
		if (SUCCEEDED(status))
			status = to_type(&right.value, VT_R8, &b);
		if (SUCCEEDED(status))
		{
			if (V_R8(&b) == 0.0)
				status = V_R8(&a) == 0.0 ? DISP_E_OVERFLOW : DISP_E_DIVBYZERO;
			else
			{
				double value = V_R8(&a) / V_R8(&b);
				if (real_overflows(value))
					status = DISP_E_OVERFLOW;
				else if (type == VT_R4 && value <= FLOAT_MAX && value >= -FLOAT_MAX)
				{
					V_VT(&result) = VT_R4;
					V_R4(&result) = (float)value;
				}
				else
				{
					V_VT(&result) = VT_R8;
					V_R8(&result) = value;
				}
			}
		}
	}
	if (SUCCEEDED(status))
		*out = result;
	else
		V_VT(out) = VT_EMPTY;
	side_free(&left);
	side_free(&right);
	VariantClear(&a);
	VariantClear(&b);
	return status;
}

static VARTYPE whole_type(VARTYPE left, VARTYPE right)
{
	if (has(left, right, VT_I8))
		return VT_I8;
	if (left == VT_UI1 && right == VT_UI1)
		return VT_UI1;
	if ((left == VT_UI1 || left == VT_I2 || left == VT_BOOL || left == VT_EMPTY) && (right == VT_UI1 || right == VT_I2 || right == VT_BOOL || right == VT_EMPTY))
		return VT_I2;
	return VT_I4;
}

static HRESULT division_check(const VARIANT *value)
{
	if (V_VT(value) == VT_VARIANT)
		return DISP_E_TYPEMISMATCH;
	if (V_VT(value) & VT_ARRAY && (V_VT(value) & VT_TYPEMASK) <= VT_NULL)
		return DISP_E_BADVARTYPE;
	return check_type(value, ALL_TYPES);
}

static HRESULT whole_division(VARIANT *from_left, VARIANT *from_right, VARIANT *out, BOOL remainder)
{
	struct side left, right;
	VARIANT a, b, result;
	VARTYPE l, r;
	HRESULT status;
	V_VT(out) = VT_EMPTY;
	status = division_check(from_left);
	if (SUCCEEDED(status))
		status = division_check(from_right);
	if (FAILED(status))
		return status;
	if (has(V_VT(from_left), V_VT(from_right), VT_I8) && has(V_VT(from_left), V_VT(from_right), VT_INT))
		return DISP_E_TYPEMISMATCH;
	if (either_null(from_left, from_right))
	{
		V_VT(out) = VT_NULL;
		return S_OK;
	}
	status = sides_load(&left, &right, from_left, from_right);
	if (FAILED(status))
		return status;
	V_VT(&a) = V_VT(&b) = V_VT(&result) = VT_EMPTY;
	l = V_VT(&left.value);
	r = V_VT(&right.value);
	status = to_type(&left.value, VT_I8, &a);
	if (SUCCEEDED(status))
		status = to_type(&right.value, VT_I8, &b);
	if (SUCCEEDED(status))
	{
		if (!V_I8(&b))
			status = DISP_E_DIVBYZERO;
		else if (V_I8(&b) == -1)
			status = store_whole(remainder ? 0 : -V_I8(&a), whole_type(l, r), &result);
		else
			status = store_whole(remainder ? V_I8(&a) % V_I8(&b) : V_I8(&a) / V_I8(&b), whole_type(l, r), &result);
	}
	if (SUCCEEDED(status))
		*out = result;
	side_free(&left);
	side_free(&right);
	return status;
}

HRESULT WINAPI VarIdiv(LPVARIANT left, LPVARIANT right, LPVARIANT out)
{
	return whole_division(left, right, out, FALSE);
}

HRESULT WINAPI VarMod(LPVARIANT left, LPVARIANT right, LPVARIANT out)
{
	return whole_division(left, right, out, TRUE);
}

HRESULT WINAPI VarPow(LPVARIANT from_left, LPVARIANT from_right, LPVARIANT out)
{
	struct side left, right;
	VARIANT a, b;
	HRESULT status = check_pair(from_left, from_right, ALL_TYPES);
	if (FAILED(status))
		return status;
	if (V_VT(from_left) & VT_BYREF || V_VT(from_right) & VT_BYREF)
		return DISP_E_BADVARTYPE;
	if (either_null(from_left, from_right))
	{
		V_VT(out) = VT_NULL;
		return S_OK;
	}
	status = sides_load(&left, &right, from_left, from_right);
	if (FAILED(status))
		return status;
	status = to_type(&left.value, VT_R8, &a);
	if (SUCCEEDED(status))
		status = to_type(&right.value, VT_R8, &b);
	if (SUCCEEDED(status))
		status = VarR8Pow(V_R8(&a), V_R8(&b), &V_R8(out));
	if (SUCCEEDED(status))
		V_VT(out) = VT_R8;
	side_free(&left);
	side_free(&right);
	return status;
}

/* Logic works on bits, and a null side only sometimes decides the answer. */
enum logic
{
	OP_AND,
	OP_OR,
	OP_XOR,
	OP_EQV,
	OP_IMP,
};

static VARTYPE logic_type(VARTYPE left, VARTYPE right)
{
	if (has(left, right, VT_I8))
		return VT_I8;
	if (left == VT_BOOL && right == VT_BOOL)
		return VT_BOOL;
	if (left == VT_UI1 && right == VT_UI1)
		return VT_UI1;
	if (left == VT_BSTR && right == VT_BSTR)
		return VT_BOOL;
	if ((left == VT_UI1 || left == VT_I2 || left == VT_BOOL || left == VT_EMPTY || left == VT_NULL) && (right == VT_UI1 || right == VT_I2 || right == VT_BOOL || right == VT_EMPTY || right == VT_NULL))
		return VT_I2;
	return VT_I4;
}

static LONG64 logic_bits(LONG64 left, LONG64 right, enum logic op)
{
	switch (op)
	{
	case OP_AND:
		return left & right;
	case OP_OR:
		return left | right;
	case OP_XOR:
		return left ^ right;
	case OP_EQV:
		return ~(left ^ right);
	default:
		return ~left | right;
	}
}

/* Logic results keep their bits whatever the sign says. */
static HRESULT store_bits(LONG64 value, VARTYPE type, VARIANT *out)
{
	V_VT(out) = type;
	switch (type)
	{
	case VT_UI1:
		V_UI1(out) = (BYTE)value;
		break;
	case VT_I2:
		V_I2(out) = (SHORT)value;
		break;
	case VT_BOOL:
		V_BOOL(out) = (VARIANT_BOOL)value;
		break;
	case VT_I8:
		V_I8(out) = value;
		break;
	default:
		V_VT(out) = VT_I4;
		V_I4(out) = (LONG)value;
		break;
	}
	return S_OK;
}

/* Text counts as a number when it reads as one, as a boolean otherwise. */
static HRESULT logic_side(const VARIANT *from, VARIANT *out, VARTYPE *type)
{
	HRESULT status = to_type(from, VT_I8, out);
	if (V_VT(from) != VT_BSTR)
		return status;
	*type = VT_I4;
	if (SUCCEEDED(status))
		return status;
	status = to_type(from, VT_BOOL, out);
	if (SUCCEEDED(status))
	{
		LONG64 value = V_BOOL(out);
		V_VT(out) = VT_I8;
		V_I8(out) = value;
		*type = VT_BOOL;
	}
	return status;
}

static HRESULT logic_run(VARIANT *from_left, VARIANT *from_right, VARIANT *out, enum logic op)
{
	struct side left, right;
	VARIANT a, b, result;
	VARTYPE l, r;
	HRESULT status = check_pair(from_left, from_right, ALL_TYPES);
	if (FAILED(status))
		return status;
	if (op == OP_IMP && (V_VT(from_left) & VT_TYPEMASK) == VT_I8 && (V_VT(from_right) & VT_TYPEMASK) == VT_INT)
		return DISP_E_TYPEMISMATCH;
	if (op != OP_IMP && has(V_VT(from_left), V_VT(from_right), VT_I8) && has(V_VT(from_left), V_VT(from_right), VT_INT))
		return DISP_E_TYPEMISMATCH;
	status = sides_load(&left, &right, from_left, from_right);
	if (FAILED(status))
		return status;
	V_VT(&a) = V_VT(&b) = V_VT(&result) = VT_EMPTY;
	l = V_VT(&left.value);
	r = V_VT(&right.value);
	if (l == VT_NULL && r == VT_NULL)
		V_VT(&result) = VT_NULL;
	else if (l == VT_NULL || r == VT_NULL)
	{
		const VARIANT *known = l == VT_NULL ? &right.value : &left.value;
		VARTYPE kind = l == VT_NULL ? r : l;
		VARTYPE keep = kind == VT_EMPTY ? VT_I2 : kind == VT_BSTR ? VT_BOOL : logic_type(kind, kind);
		LONG64 value = 0;
		/* Currency rounds here; the plain conversion would step past a negative value. */
		if (kind == VT_CY)
		{
			V_VT(&a) = VT_I8;
			V_I8(&a) = oa_cy_to_i8(V_CY(known).int64);
			status = S_OK;
		}
		else
			status = to_type(known, VT_I8, &a);
		if (FAILED(status) && kind == VT_BSTR)
		{
			status = to_type(known, VT_BOOL, &a);
			if (SUCCEEDED(status))
			{
				value = V_BOOL(&a);
				V_VT(&a) = VT_I8;
				V_I8(&a) = value;
			}
		}
		if (SUCCEEDED(status))
		{
			value = V_I8(&a);
			switch (op)
			{
			case OP_AND:
				if (value)
					V_VT(&result) = VT_NULL;
				else
					status = store_bits(0, keep, &result);
				break;
			case OP_OR:
				if (value)
					status = store_bits(value, keep, &result);
				else
					V_VT(&result) = VT_NULL;
				break;
			case OP_IMP:
				if (l == VT_NULL && value)
					status = store_bits(value, keep, &result);
				else if (r == VT_NULL && (keep == VT_UI1 ? ~value & 0xff : ~value))
					status = store_bits(~value, keep, &result);
				else
					V_VT(&result) = VT_NULL;
				break;
			default:
				V_VT(&result) = VT_NULL;
				break;
			}
		}
		VariantClear(&a);
	}
	else
	{
		status = logic_side(&left.value, &a, &l);
		if (SUCCEEDED(status))
			status = logic_side(&right.value, &b, &r);
		if (SUCCEEDED(status))
			status = store_bits(logic_bits(V_I8(&a), V_I8(&b), op), logic_type(l, r), &result);
	}
	if (SUCCEEDED(status))
		*out = result;
	side_free(&left);
	side_free(&right);
	return status;
}

HRESULT WINAPI VarAnd(LPVARIANT left, LPVARIANT right, LPVARIANT out)
{
	return logic_run(left, right, out, OP_AND);
}

HRESULT WINAPI VarOr(LPVARIANT left, LPVARIANT right, LPVARIANT out)
{
	return logic_run(left, right, out, OP_OR);
}

HRESULT WINAPI VarXor(LPVARIANT left, LPVARIANT right, LPVARIANT out)
{
	return logic_run(left, right, out, OP_XOR);
}

HRESULT WINAPI VarEqv(LPVARIANT left, LPVARIANT right, LPVARIANT out)
{
	return logic_run(left, right, out, OP_EQV);
}

HRESULT WINAPI VarImp(LPVARIANT left, LPVARIANT right, LPVARIANT out)
{
	return logic_run(left, right, out, OP_IMP);
}

HRESULT WINAPI VarNot(LPVARIANT from, LPVARIANT out)
{
	struct side side;
	VARIANT work;
	VARTYPE type;
	HRESULT status;
	/* An array has no value to invert, and neither has a reference to nothing. */
	if (V_VT(from) & VT_ARRAY || (V_VT(from) & VT_BYREF && !V_BYREF(from)))
	{
		VARTYPE base = V_VT(from) & VT_TYPEMASK;
		BOOL known = (base <= VT_LPWSTR || base == VT_RECORD) && base != 15 && !(base >= 24 && base <= 31) && base > VT_NULL;
		return known && !(V_VT(from) & (VT_VECTOR | VT_RESERVED)) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
	}
	status = division_check(from);
	if (FAILED(status))
		return status;
	status = side_load(&side, from);
	if (FAILED(status))
		return status;
	type = V_VT(&side.value);
	V_VT(&work) = VT_EMPTY;
	switch (type)
	{
	case VT_NULL:
		V_VT(out) = VT_NULL;
		break;
	case VT_EMPTY:
		V_VT(out) = VT_I2;
		V_I2(out) = -1;
		break;
	case VT_BOOL:
		V_VT(out) = VT_BOOL;
		V_BOOL(out) = ~V_BOOL(&side.value);
		break;
	case VT_UI1:
		V_VT(out) = VT_UI1;
		V_UI1(out) = ~V_UI1(&side.value);
		break;
	case VT_I2:
		V_VT(out) = VT_I2;
		V_I2(out) = ~V_I2(&side.value);
		break;
	case VT_I8:
		V_VT(out) = VT_I8;
		V_I8(out) = ~V_I8(&side.value);
		break;
	case VT_UI8:
		V_VT(out) = VT_I4;
		V_I4(out) = ~(LONG)V_UI8(&side.value);
		break;
	case VT_I4:
	case VT_INT:
		V_VT(out) = VT_I4;
		V_I4(out) = ~V_I4(&side.value);
		break;
	case VT_BSTR:
		status = to_type(&side.value, VT_I4, &work);
		if (SUCCEEDED(status))
		{
			V_VT(out) = VT_I4;
			V_I4(out) = ~V_I4(&work);
			break;
		}
		status = to_type(&side.value, VT_BOOL, &work);
		if (SUCCEEDED(status))
		{
			V_VT(out) = VT_BOOL;
			V_BOOL(out) = ~V_BOOL(&work);
		}
		break;
	default:
		status = to_type(&side.value, VT_I4, &work);
		if (SUCCEEDED(status))
		{
			V_VT(out) = VT_I4;
			V_I4(out) = ~V_I4(&work);
		}
		break;
	}
	side_free(&side);
	return status;
}

enum unary
{
	OP_ABS,
	OP_NEG,
	OP_FIX,
	OP_INT,
};

static double real_whole(double value, BOOL toward_zero)
{
	double whole;
	if (value >= 4503599627370496.0 || value <= -4503599627370496.0 || value != value)
		return value;
	whole = (double)(LONG64)value;
	if (!toward_zero && whole > value)
		whole -= 1.0;
	return whole;
}

static double unary_real(double value, enum unary op)
{
	switch (op)
	{
	case OP_ABS:
		return value < 0 ? -value : value;
	case OP_NEG:
		return -value;
	case OP_FIX:
		return real_whole(value, TRUE);
	default:
		return real_whole(value, FALSE);
	}
}

static HRESULT unary_run(VARIANT *from, VARIANT *out, enum unary op)
{
	struct side side;
	VARIANT work;
	VARTYPE type;
	HRESULT status = check_type(from, ALL_TYPES);
	if (FAILED(status))
		return status;
	status = side_load(&side, from);
	if (FAILED(status))
		return status;
	V_VT(&work) = VT_EMPTY;
	type = V_VT(&side.value);
	if (is_extended(type))
		status = DISP_E_BADVARTYPE;
	else
		switch (type)
		{
		case VT_NULL:
			V_VT(out) = VT_NULL;
			break;
		case VT_EMPTY:
			V_VT(out) = VT_I2;
			V_I2(out) = 0;
			break;
		case VT_BOOL:
		case VT_UI1:
		case VT_I2:
		case VT_I4:
		case VT_I8:
			status = to_type(&side.value, VT_I8, &work);
			if (SUCCEEDED(status))
			{
				LONG64 value = V_I8(&work);
				if (op == OP_NEG || op == OP_ABS && value < 0)
					value = -value;
				if (type == VT_I8 && value == (-0x7fffffffffffffffLL - 1))
					status = DISP_E_OVERFLOW;
				else if (type == VT_BOOL)
					status = store_whole(value, VT_I2, out);
				else
					status = store_whole(value, op == OP_NEG && type == VT_UI1 ? VT_I2 : type, out);
			}
			break;
		case VT_R4:
			V_VT(out) = VT_R4;
			V_R4(out) = (float)unary_real(V_R4(&side.value), op);
			break;
		case VT_R8:
			V_VT(out) = VT_R8;
			V_R8(out) = unary_real(V_R8(&side.value), op);
			break;
		case VT_DATE:
			V_VT(out) = VT_DATE;
			V_R8(out) = unary_real(V_DATE(&side.value), op);
			break;
		case VT_BSTR:
			status = to_type(&side.value, VT_R8, &work);
			if (SUCCEEDED(status))
			{
				V_VT(out) = VT_R8;
				V_R8(out) = unary_real(V_R8(&work), op);
			}
			break;
		case VT_CY:
		{
			CY value;
			if (op == OP_ABS)
				status = VarCyAbs(V_CY(&side.value), &value);
			else if (op == OP_NEG)
				status = VarCyNeg(V_CY(&side.value), &value);
			else if (op == OP_FIX)
				status = VarCyFix(V_CY(&side.value), &value);
			else
				status = VarCyInt(V_CY(&side.value), &value);
			if (SUCCEEDED(status))
			{
				V_VT(out) = VT_CY;
				V_CY(out) = value;
			}
			break;
		}
		case VT_DECIMAL:
		{
			DECIMAL value;
			if (op == OP_ABS)
				status = VarDecAbs(&V_DECIMAL(&side.value), &value);
			else if (op == OP_NEG)
				status = VarDecNeg(&V_DECIMAL(&side.value), &value);
			else if (op == OP_FIX)
				status = VarDecFix(&V_DECIMAL(&side.value), &value);
			else
				status = VarDecInt(&V_DECIMAL(&side.value), &value);
			if (SUCCEEDED(status))
			{
				V_DECIMAL(out) = value;
				V_VT(out) = VT_DECIMAL;
			}
			break;
		}
		default:
			status = DISP_E_TYPEMISMATCH;
			break;
		}
	side_free(&side);
	return status;
}

HRESULT WINAPI VarAbs(LPVARIANT in, LPVARIANT out)
{
	return unary_run(in, out, OP_ABS);
}

HRESULT WINAPI VarNeg(LPVARIANT in, LPVARIANT out)
{
	return unary_run(in, out, OP_NEG);
}

HRESULT WINAPI VarFix(LPVARIANT in, LPVARIANT out)
{
	return unary_run(in, out, OP_FIX);
}

HRESULT WINAPI VarInt(LPVARIANT in, LPVARIANT out)
{
	return unary_run(in, out, OP_INT);
}

HRESULT WINAPI VarRound(LPVARIANT from, int places, LPVARIANT out)
{
	struct side side;
	VARIANT work;
	HRESULT status;
	if (places < 0)
		return E_INVALIDARG;
	status = check_type(from, ALL_TYPES);
	if (FAILED(status))
		return status;
	status = side_load(&side, from);
	if (FAILED(status))
		return status;
	V_VT(&work) = VT_EMPTY;
	switch (V_VT(&side.value))
	{
	case VT_NULL:
		V_VT(out) = VT_NULL;
		break;
	case VT_EMPTY:
		V_VT(out) = VT_I2;
		V_I2(out) = 0;
		break;
	case VT_BOOL:
		V_VT(out) = VT_I2;
		V_I2(out) = V_BOOL(&side.value);
		break;
	case VT_UI1:
	case VT_I2:
	case VT_I4:
	case VT_I8:
		*out = side.value;
		break;
	case VT_R4:
	{
		double value;
		status = VarR8Round(V_R4(&side.value), places, &value);
		V_VT(out) = VT_R4;
		V_R4(out) = (float)value;
		break;
	}
	case VT_R8:
		V_VT(out) = VT_R8;
		status = VarR8Round(V_R8(&side.value), places, &V_R8(out));
		break;
	case VT_DATE:
		V_VT(out) = VT_DATE;
		status = VarR8Round(V_DATE(&side.value), places, &V_DATE(out));
		break;
	case VT_BSTR:
		status = to_type(&side.value, VT_R8, &work);
		if (SUCCEEDED(status))
		{
			V_VT(out) = VT_R8;
			status = VarR8Round(V_R8(&work), places, &V_R8(out));
		}
		break;
	case VT_CY:
		V_VT(out) = VT_CY;
		status = VarCyRound(V_CY(&side.value), places, &V_CY(out));
		break;
	case VT_DECIMAL:
	{
		DECIMAL value;
		status = VarDecRound(&V_DECIMAL(&side.value), places, &value);
		if (SUCCEEDED(status))
		{
			V_DECIMAL(out) = value;
			V_VT(out) = VT_DECIMAL;
		}
		break;
	}
	default:
		status = DISP_E_BADVARTYPE;
		break;
	}
	side_free(&side);
	return status;
}

/* Strings. */
HRESULT WINAPI VarBstrCat(BSTR left, BSTR right, BSTR *out)
{
	UINT left_size = left ? SysStringByteLen(left) : 0;
	UINT right_size = right ? SysStringByteLen(right) : 0;
	BSTR joined;
	if (!out)
		return E_INVALIDARG;
	if ((ULONG64)left_size + right_size > 0x7fffffe0u)
		return E_OUTOFMEMORY;
	joined = SysAllocStringByteLen(NULL, left_size + right_size);
	if (!joined)
		return E_OUTOFMEMORY;
	if (left_size)
		memcpy(joined, left, left_size);
	if (right_size)
		memcpy((BYTE *)joined + left_size, right, right_size);
	*out = joined;
	return S_OK;
}

static BOOL has_word_mark(const WCHAR *text, UINT size)
{
	UINT index;
	for (index = 0; index < size; index++)
		if (text[index] == L'-' || text[index] == L'\'')
			return TRUE;
	return FALSE;
}

static BSTR without_word_marks(const WCHAR *text, UINT size)
{
	BSTR out = SysAllocStringLen(NULL, size);
	UINT index, used = 0;
	if (!out)
		return NULL;
	for (index = 0; index < size; index++)
		if (text[index] != L'-' && text[index] != L'\'')
			out[used++] = text[index];
	out[used] = 0;
	*((DWORD *)out - 1) = used * sizeof(WCHAR);
	return out;
}

HRESULT WINAPI VarBstrCmp(BSTR left, BSTR right, LCID lcid, ULONG flags)
{
	UINT left_size = left ? SysStringLen(left) : 0, right_size = right ? SysStringLen(right) : 0;
	int order;
	if (!left_size || !right_size)
	{
		if (!left_size && !right_size)
			return VARCMP_EQ;
		return left_size ? VARCMP_GT : VARCMP_LT;
	}
	/* No locale means the raw bytes decide, however odd their number. */
	if (!lcid)
	{
		UINT left_bytes = SysStringByteLen(left), right_bytes = SysStringByteLen(right), index;
		for (index = 0; index < left_bytes && index < right_bytes; index++)
			if (((BYTE *)left)[index] != ((BYTE *)right)[index])
				return ((BYTE *)left)[index] < ((BYTE *)right)[index] ? VARCMP_LT : VARCMP_GT;
		return left_bytes == right_bytes ? VARCMP_EQ : left_bytes < right_bytes ? VARCMP_LT : VARCMP_GT;
	}
	while (left_size && !left[left_size - 1])
		left_size--;
	while (right_size && !right[right_size - 1])
		right_size--;
	/* Word sort sets hyphens and apostrophes aside; they only break a tie, and the longer text is then the greater. */
	if (!(flags & SORT_STRINGSORT) && (has_word_mark(left, left_size) || has_word_mark(right, right_size)))
	{
		BSTR bare_left = without_word_marks(left, left_size), bare_right = without_word_marks(right, right_size);
		order = bare_left && bare_right ? CompareStringW(lcid, flags, bare_left, SysStringLen(bare_left), bare_right, SysStringLen(bare_right)) : 0;
		SysFreeString(bare_left);
		SysFreeString(bare_right);
		if (order == CSTR_EQUAL && left_size != right_size)
			return left_size < right_size ? VARCMP_LT : VARCMP_GT;
	}
	else
		order = CSTR_EQUAL;
	if (order == CSTR_EQUAL)
		order = CompareStringW(lcid, flags, left, left_size, right, right_size);
	if (!order)
		return E_INVALIDARG;
	return order - CSTR_LESS_THAN + VARCMP_LT;
}

static HRESULT cat_text(const VARIANT *value, BSTR *out)
{
	VARIANT text;
	HRESULT status;
	if (V_VT(value) == VT_EMPTY || V_VT(value) == VT_NULL)
	{
		*out = NULL;
		return S_OK;
	}
	if (V_VT(value) == VT_BSTR)
	{
		*out = V_BSTR(value);
		return S_FALSE;
	}
	V_VT(&text) = VT_EMPTY;
	status = VariantChangeTypeEx(&text, (VARIANT *)value, LOCALE_USER_DEFAULT, V_VT(value) == VT_BOOL ? VARIANT_ALPHABOOL : 0, VT_BSTR);
	if (SUCCEEDED(status))
	{
		*out = V_BSTR(&text);
		status = S_OK;
	}
	return status;
}

static BOOL cat_accepts(VARTYPE type)
{
	return is_core(type) || is_extended(type);
}

HRESULT WINAPI VarCat(LPVARIANT from_left, LPVARIANT from_right, LPVARIANT out)
{
	struct side left, right;
	BSTR a = NULL, b = NULL, joined = NULL;
	HRESULT left_state, right_state, status;
	VARTYPE l = V_VT(from_left), r = V_VT(from_right);
	if (l == VT_NULL && r == VT_NULL)
	{
		V_VT(out) = VT_NULL;
		return S_OK;
	}
	if (!(l == VT_DISPATCH || cat_accepts(l & ~VT_BYREF)) || !(r == VT_DISPATCH || cat_accepts(r & ~VT_BYREF)))
	{
		if (l == VT_VARIANT && (r == VT_ERROR || r == VT_DATE || r == VT_DECIMAL))
			return DISP_E_TYPEMISMATCH;
		if (r == VT_ERROR && l < VT_VOID)
			return DISP_E_TYPEMISMATCH;
		if (l == VT_ERROR && (r == VT_DATE || r == VT_ERROR || r == VT_DECIMAL))
			return DISP_E_TYPEMISMATCH;
		if (r == VT_DATE || r == VT_ERROR || r == VT_DECIMAL)
			return DISP_E_BADVARTYPE;
		if (l == VT_ERROR || r == VT_ERROR || l == VT_VARIANT)
			return DISP_E_TYPEMISMATCH;
		if (r == VT_VARIANT && cat_accepts(l))
			return DISP_E_TYPEMISMATCH;
		return DISP_E_BADVARTYPE;
	}
	status = sides_load(&left, &right, from_left, from_right);
	if (FAILED(status))
		return status;
	left_state = cat_text(&left.value, &a);
	if (FAILED(left_state))
		status = left_state;
	else
	{
		right_state = cat_text(&right.value, &b);
		if (FAILED(right_state))
			status = right_state;
		else
		{
			status = VarBstrCat(a, b, &joined);
			if (SUCCEEDED(status))
			{
				V_VT(out) = VT_BSTR;
				V_BSTR(out) = joined;
			}
			if (right_state == S_OK)
				SysFreeString(b);
		}
		if (left_state == S_OK)
			SysFreeString(a);
	}
	side_free(&left);
	side_free(&right);
	return status;
}

/* Comparison. */
static HRESULT from_order(int order)
{
	return order < 0 ? VARCMP_LT : order > 0 ? VARCMP_GT : VARCMP_EQ;
}

static BOOL cmp_real(VARTYPE type)
{
	return type == VT_R4 || type == VT_R8 || type == VT_DATE;
}

static HRESULT cmp_numbers(const VARIANT *left, const VARIANT *right)
{
	VARIANT a, b;
	HRESULT status;
	VARTYPE l = V_VT(left), r = V_VT(right);
	if (has(l, r, VT_DECIMAL) || (has(l, r, VT_CY) && !cmp_real(l) && !cmp_real(r)))
	{
		/* Decimal is the sharper type, as long as the other side fits in it. */
		status = to_type(left, VT_DECIMAL, &a);
		if (SUCCEEDED(status))
			status = to_type(right, VT_DECIMAL, &b);
		if (SUCCEEDED(status))
			return VarDecCmp(&V_DECIMAL(&a), &V_DECIMAL(&b));
	}
	if (cmp_real(l) || cmp_real(r) || has(l, r, VT_DECIMAL))
	{
		status = to_type(left, VT_R8, &a);
		if (SUCCEEDED(status))
			status = to_type(right, VT_R8, &b);
		if (FAILED(status))
			return status;
		/* A float only ever knew its own few digits. */
		if (has(l, r, VT_R4) && (has(l, r, VT_R8) || has(l, r, VT_DATE)) && V_R8(&a) <= FLOAT_MAX && V_R8(&a) >= -FLOAT_MAX && V_R8(&b) <= FLOAT_MAX && V_R8(&b) >= -FLOAT_MAX)
		{
			V_R8(&a) = (float)V_R8(&a);
			V_R8(&b) = (float)V_R8(&b);
		}
		return from_order(V_R8(&a) < V_R8(&b) ? -1 : V_R8(&a) > V_R8(&b));
	}
	status = to_type(left, VT_I8, &a);
	if (SUCCEEDED(status))
		status = to_type(right, VT_I8, &b);
	if (FAILED(status))
		return status;
	return from_order(V_I8(&a) < V_I8(&b) ? -1 : V_I8(&a) > V_I8(&b));
}

HRESULT WINAPI VarCmp(LPVARIANT from_left, LPVARIANT from_right, LCID lcid, ULONG flags)
{
	struct side left, right;
	VARIANT plain_left = *from_left, plain_right = *from_right;
	BOOL left_marked = (V_VT(from_left) & VT_RESERVED) != 0, right_marked = (V_VT(from_right) & VT_RESERVED) != 0;
	VARTYPE l, r;
	HRESULT status;
	V_VT(&plain_left) &= ~VT_RESERVED;
	V_VT(&plain_right) &= ~VT_RESERVED;
	if (V_VT(&plain_left) == VT_INT)
		V_VT(&plain_left) = VT_I4;
	status = V_VT(&plain_left) == VT_ERROR && V_VT(&plain_right) == VT_ERROR ? S_OK : check_pair(&plain_left, &plain_right, CORE_TYPES);
	if (SUCCEEDED(status) && (V_VT(&plain_left) == VT_DISPATCH || V_VT(&plain_right) == VT_DISPATCH))
		status = DISP_E_TYPEMISMATCH;
	if (FAILED(status))
		return status;
	status = sides_load(&left, &right, &plain_left, &plain_right);
	if (FAILED(status))
		return status;
	l = V_VT(&left.value);
	r = V_VT(&right.value);
	if (l == VT_NULL || r == VT_NULL)
		status = VARCMP_NULL;
	else if (l == VT_EMPTY && r == VT_EMPTY)
		status = VARCMP_EQ;
	else if (l == VT_BSTR && r == VT_BSTR)
		status = VarBstrCmp(V_BSTR(&left.value), V_BSTR(&right.value), lcid, flags);
	else if (l == VT_BSTR && r == VT_EMPTY)
		status = VarBstrCmp(V_BSTR(&left.value), NULL, lcid, flags);
	else if (l == VT_EMPTY && r == VT_BSTR)
		status = VarBstrCmp(NULL, V_BSTR(&right.value), lcid, flags);
	else if (l == VT_BSTR || r == VT_BSTR)
	{
		/* Text is above any number. A mark on the text alone compares both as text; a mark on the
		 * number alone reads the text as a number if it can be one; marks on both leave only the
		 * sign of the text's number to decide. */
		BOOL swap = l != VT_BSTR;
		const VARIANT *text_side = swap ? &right.value : &left.value, *number_side = swap ? &left.value : &right.value;
		VARIANT other, wide;
		BOOL text_marked = swap ? right_marked : left_marked, number_marked = swap ? left_marked : right_marked;
		status = VARCMP_GT;
		if (text_marked && !number_marked)
		{
			status = to_type(number_side, VT_BSTR, &other);
			if (SUCCEEDED(status))
			{
				status = VarBstrCmp(V_BSTR(text_side), V_BSTR(&other), lcid, flags);
				VariantClear(&other);
			}
		}
		else if (number_marked && V_BSTR(text_side) && *V_BSTR(text_side) && SUCCEEDED(to_type(text_side, VT_R8, &other)))
		{
			if (text_marked)
				status = V_R8(&other) >= 0 ? VARCMP_GT : VARCMP_LT;
			else if (SUCCEEDED(to_type(number_side, VT_R8, &wide)))
				status = cmp_numbers(&other, &wide);
		}
		if (swap && status == VARCMP_GT)
			status = VARCMP_LT;
		else if (swap && status == VARCMP_LT)
			status = VARCMP_GT;
	}
	else if (l == VT_ERROR && r == VT_ERROR)
		status = VARCMP_EQ;
	else
		status = cmp_numbers(&left.value, &right.value);
	side_free(&left);
	side_free(&right);
	return status;
}
