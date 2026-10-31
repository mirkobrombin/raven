/* SPDX-License-Identifier: MIT */
#include "ps.h"
#include <wchar.h>

/* Every scalar reads through one number: its magnitude and sign, or a double when the source is a real number. */
typedef struct
{
	ULONGLONG magnitude;
	double real;
	BOOL negative;
	BOOL is_real;
} number;

#define BAD_TEXT DISP_E_TYPEMISMATCH
#define OVERFLOW_STATUS HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW)
#define PSTF_LOCAL_TIME 1

WCHAR *ps_copy_text(const WCHAR *text)
{
	SIZE_T size = (wcslen(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = CoTaskMemAlloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

WCHAR *ps_widen(const char *text)
{
	int count = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	WCHAR *wide = CoTaskMemAlloc(count * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, count);
	return wide;
}

static HRESULT number_from_text(const WCHAR *text, number *out)
{
	const WCHAR *scan = text;
	ULONGLONG total = 0;
	UINT base = 10;
	if (!text)
		return BAD_TEXT;
	out->is_real = FALSE;
	out->negative = FALSE;
	if (*scan == '+' || *scan == '-')
		out->negative = *scan++ == '-';
	if (scan[0] == '0' && (scan[1] == 'x' || scan[1] == 'X'))
	{
		base = 16;
		scan += 2;
	}
	if (!*scan)
		return BAD_TEXT;
	for (; *scan; scan++)
	{
		UINT digit;
		if (*scan >= '0' && *scan <= '9')
			digit = *scan - '0';
		else if (base == 16 && *scan >= 'a' && *scan <= 'f')
			digit = *scan - 'a' + 10;
		else if (base == 16 && *scan >= 'A' && *scan <= 'F')
			digit = *scan - 'A' + 10;
		else
			return BAD_TEXT;
		if (total > (~0ull - digit) / base)
			return OVERFLOW_STATUS;
		total = total * base + digit;
	}
	out->magnitude = total;
	return S_OK;
}

static void number_signed(number *out, LONGLONG value)
{
	out->negative = value < 0;
	out->magnitude = value < 0 ? 0ull - (ULONGLONG)value : (ULONGLONG)value;
}

/* The real number's whole part, rounding halves to the even neighbour as the automation conversions do. */
static HRESULT number_settle(number *value)
{
	double whole, rest;
	ULONGLONG truncated;
	if (!value->is_real)
		return S_OK;
	whole = value->real < 0 ? -value->real : value->real;
	if (whole >= 18446744073709551616.0)
		return OVERFLOW_STATUS;
	truncated = (ULONGLONG)whole;
	rest = whole - (double)truncated;
	if (rest > 0.5 || (rest == 0.5 && (truncated & 1)))
		truncated++;
	value->negative = value->real < 0 && truncated;
	value->magnitude = truncated;
	return S_OK;
}

static HRESULT read_number(const PROPVARIANT *source, number *out)
{
	WCHAR *wide;
	HRESULT status;
	memset(out, 0, sizeof(*out));
	switch (source->vt)
	{
	case VT_EMPTY:
	case VT_NULL:
		return S_OK;
	case VT_I1:
		number_signed(out, source->cVal);
		return S_OK;
	case VT_I2:
		number_signed(out, source->iVal);
		return S_OK;
	case VT_I4:
	case VT_INT:
		number_signed(out, source->lVal);
		return S_OK;
	case VT_I8:
		number_signed(out, source->hVal.QuadPart);
		return S_OK;
	case VT_UI1:
		out->magnitude = source->bVal;
		return S_OK;
	case VT_UI2:
		out->magnitude = source->uiVal;
		return S_OK;
	case VT_UI4:
	case VT_UINT:
		out->magnitude = source->ulVal;
		return S_OK;
	case VT_UI8:
		out->magnitude = source->uhVal.QuadPart;
		return S_OK;
	case VT_BOOL:
		out->magnitude = source->boolVal != VARIANT_FALSE;
		return S_OK;
	case VT_R4:
		out->real = source->fltVal;
		out->is_real = TRUE;
		return S_OK;
	case VT_R8:
		out->real = source->dblVal;
		out->is_real = TRUE;
		return S_OK;
	case VT_LPWSTR:
		return number_from_text(source->pwszVal, out);
	case VT_BSTR:
		return number_from_text(source->bstrVal, out);
	case VT_LPSTR:
		if (!source->pszVal)
			return BAD_TEXT;
		if (!(wide = ps_widen(source->pszVal)))
			return E_OUTOFMEMORY;
		status = number_from_text(wide, out);
		CoTaskMemFree(wide);
		return status;
	}
	return E_INVALIDARG;
}

static HRESULT read_ranged(const PROPVARIANT *source, LONGLONG low, ULONGLONG high, number *out)
{
	HRESULT status = read_number(source, out);
	if (SUCCEEDED(status))
		status = number_settle(out);
	if (FAILED(status))
		return status;
	if (out->negative ? out->magnitude > 0ull - (ULONGLONG)low : out->magnitude > high)
		return OVERFLOW_STATUS;
	return S_OK;
}

#define SCALAR_FAMILY(name, type, low, high) \
	HRESULT WINAPI PropVariantTo##name(REFPROPVARIANT source, type *out) \
	{ \
		number value; \
		HRESULT status = read_ranged(source, low, high, &value); \
		if (SUCCEEDED(status)) \
			*out = value.negative ? (type)(0ull - value.magnitude) : (type)value.magnitude; \
		return status; \
	} \
	type WINAPI PropVariantTo##name##WithDefault(REFPROPVARIANT source, type fallback) \
	{ \
		type value; \
		if (source->vt == VT_EMPTY || source->vt == VT_NULL || FAILED(PropVariantTo##name(source, &value))) \
			return fallback; \
		return value; \
	}

SCALAR_FAMILY(Int16, SHORT, -32768, 32767)
SCALAR_FAMILY(UInt16, USHORT, 0, 65535)
SCALAR_FAMILY(Int32, LONG, -2147483647ll - 1, 2147483647)
SCALAR_FAMILY(UInt32, ULONG, 0, 4294967295ull)
SCALAR_FAMILY(Int64, LONGLONG, -9223372036854775807ll - 1, 9223372036854775807ull)
SCALAR_FAMILY(UInt64, ULONGLONG, 0, ~0ull)

HRESULT WINAPI PropVariantToDouble(REFPROPVARIANT source, double *out)
{
	number value;
	HRESULT status;
	const WCHAR *text = source->vt == VT_LPWSTR ? source->pwszVal : source->vt == VT_BSTR ? source->bstrVal : NULL;
	if (text)
		return VarR8FromStr((OLECHAR *)text, LOCALE_USER_DEFAULT, 0, out);
	status = read_number(source, &value);
	if (FAILED(status))
		return status;
	*out = value.is_real ? value.real : value.negative ? -(double)value.magnitude : (double)value.magnitude;
	return S_OK;
}

double WINAPI PropVariantToDoubleWithDefault(REFPROPVARIANT source, double fallback)
{
	double value;
	if (source->vt == VT_EMPTY || source->vt == VT_NULL || FAILED(PropVariantToDouble(source, &value)))
		return fallback;
	return value;
}

static BOOL ascii_same(const WCHAR *text, const char *word)
{
	for (; *word; text++, word++)
	{
		WCHAR letter = *text >= 'A' && *text <= 'Z' ? *text + 32 : *text;
		if (letter != (WCHAR)*word)
			return FALSE;
	}
	return !*text;
}

static HRESULT boolean_from_text(const WCHAR *text, BOOL *out)
{
	number value;
	HRESULT status;
	if (!text)
		return BAD_TEXT;
	if (ascii_same(text, "true") || !wcscmp(text, L"#TRUE#"))
	{
		*out = TRUE;
		return S_OK;
	}
	if (ascii_same(text, "false") || !wcscmp(text, L"#FALSE#"))
		return S_OK;
	status = number_from_text(text, &value);
	if (FAILED(status))
		return BAD_TEXT;
	*out = value.magnitude != 0;
	return S_OK;
}

HRESULT WINAPI PropVariantToBoolean(REFPROPVARIANT source, BOOL *out)
{
	number value;
	HRESULT status;
	WCHAR *wide;
	*out = FALSE;
	switch (source->vt)
	{
	case VT_BOOL:
		*out = source->boolVal == VARIANT_TRUE;
		return S_OK;
	case VT_LPWSTR:
		return boolean_from_text(source->pwszVal, out);
	case VT_BSTR:
		return boolean_from_text(source->bstrVal, out);
	case VT_LPSTR:
		if (!source->pszVal)
			return BAD_TEXT;
		if (!(wide = ps_widen(source->pszVal)))
			return E_OUTOFMEMORY;
		status = boolean_from_text(wide, out);
		CoTaskMemFree(wide);
		return status;
	}
	status = read_number(source, &value);
	if (FAILED(status))
		return status;
	*out = value.is_real ? value.real != 0 : value.magnitude != 0;
	return S_OK;
}

BOOL WINAPI PropVariantToBooleanWithDefault(REFPROPVARIANT source, BOOL fallback)
{
	BOOL value;
	if (source->vt == VT_EMPTY || source->vt == VT_NULL || FAILED(PropVariantToBoolean(source, &value)))
		return fallback;
	return value;
}

/* Text */

static WCHAR *format_unsigned(WCHAR *end, ULONGLONG value, BOOL negative)
{
	*--end = 0;
	do
	{
		*--end = '0' + (WCHAR)(value % 10);
		value /= 10;
	} while (value);
	if (negative)
		*--end = '-';
	return end;
}

static void write_digits(WCHAR *out, UINT value, UINT count)
{
	while (count--)
	{
		out[count] = '0' + value % 10;
		value /= 10;
	}
}

static HRESULT render_time(const SYSTEMTIME *time, WCHAR **out)
{
	WCHAR text[24] = L"0000/00/00:00:00:00.000";
	write_digits(text, time->wYear, 4);
	write_digits(text + 5, time->wMonth, 2);
	write_digits(text + 8, time->wDay, 2);
	write_digits(text + 11, time->wHour, 2);
	write_digits(text + 14, time->wMinute, 2);
	write_digits(text + 17, time->wSecond, 2);
	write_digits(text + 20, time->wMilliseconds, 3);
	return (*out = ps_copy_text(text)) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT render_bstr(HRESULT status, const BSTR *holder, WCHAR **out)
{
	BSTR text = *holder;
	if (FAILED(status))
		return status;
	*out = ps_copy_text(text);
	SysFreeString(text);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT render_scalar(VARTYPE type, const void *data, WCHAR **out)
{
	PROPVARIANT single;
	PropVariantInit(&single);
	single.vt = type;
	switch (type)
	{
	case VT_I1:
	case VT_UI1:
		single.bVal = *(const BYTE *)data;
		break;
	case VT_I2:
	case VT_UI2:
	case VT_BOOL:
		single.uiVal = *(const USHORT *)data;
		break;
	case VT_I4:
	case VT_UI4:
	case VT_R4:
		single.ulVal = *(const ULONG *)data;
		break;
	case VT_I8:
	case VT_UI8:
		single.uhVal.QuadPart = *(const ULONGLONG *)data;
		break;
	case VT_R8:
	case VT_DATE:
		single.dblVal = *(const double *)data;
		break;
	case VT_FILETIME:
		single.filetime = *(const FILETIME *)data;
		break;
	case VT_CLSID:
		single.puuid = (CLSID *)data;
		break;
	case VT_LPWSTR:
	case VT_BSTR:
		single.pwszVal = *(WCHAR *const *)data;
		break;
	case VT_LPSTR:
		single.pszVal = *(char *const *)data;
		break;
	default:
		return E_INVALIDARG;
	}
	return ps_render(&single, out);
}

static UINT element_size(VARTYPE type)
{
	switch (type)
	{
	case VT_I1:
	case VT_UI1:
		return 1;
	case VT_I2:
	case VT_UI2:
	case VT_BOOL:
		return 2;
	case VT_I4:
	case VT_UI4:
	case VT_R4:
		return 4;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_DATE:
	case VT_FILETIME:
		return 8;
	case VT_CLSID:
		return sizeof(CLSID);
	case VT_LPWSTR:
	case VT_LPSTR:
	case VT_BSTR:
		return sizeof(void *);
	}
	return 0;
}

/* A vector shows as its items joined by a semicolon and a space. */
static HRESULT render_vector(const PROPVARIANT *source, WCHAR **out)
{
	VARTYPE type = source->vt & VT_TYPEMASK;
	UINT size = element_size(type);
	const BYTE *data = source->caub.pElems;
	SIZE_T used = 0, room = 16;
	ULONG index;
	WCHAR *text;
	if (!size)
		return E_INVALIDARG;
	if (!(text = CoTaskMemAlloc(room * sizeof(WCHAR))))
		return E_OUTOFMEMORY;
	text[0] = 0;
	for (index = 0; index < source->caub.cElems; index++)
	{
		WCHAR *item, *grown;
		SIZE_T length;
		HRESULT status = render_scalar(type, data + index * size, &item);
		if (FAILED(status))
		{
			CoTaskMemFree(text);
			return status;
		}
		length = wcslen(item);
		if (used + length + 3 > room)
		{
			room = (used + length + 3) * 2;
			if (!(grown = CoTaskMemRealloc(text, room * sizeof(WCHAR))))
			{
				CoTaskMemFree(item);
				CoTaskMemFree(text);
				return E_OUTOFMEMORY;
			}
			text = grown;
		}
		if (index)
		{
			text[used++] = ';';
			text[used++] = ' ';
		}
		memcpy(text + used, item, (length + 1) * sizeof(WCHAR));
		used += length;
		CoTaskMemFree(item);
	}
	*out = text;
	return S_OK;
}

HRESULT ps_render(const PROPVARIANT *source, WCHAR **out)
{
	WCHAR digits[32], guid[40];
	BSTR text = NULL;
	SYSTEMTIME time;
	const LCID invariant_decimal = MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT);
	*out = NULL;
	if (source->vt & VT_VECTOR)
		return render_vector(source, out);
	switch (source->vt)
	{
	case VT_EMPTY:
	case VT_NULL:
		return (*out = ps_copy_text(L"")) ? S_OK : E_OUTOFMEMORY;
	case VT_BOOL:
		return (*out = ps_copy_text(source->boolVal ? L"1" : L"0")) ? S_OK : E_OUTOFMEMORY;
	case VT_R4:
		return render_bstr(VarBstrFromR4(source->fltVal, invariant_decimal, 0, &text), &text, out);
	case VT_R8:
		return render_bstr(VarBstrFromR8(source->dblVal, invariant_decimal, 0, &text), &text, out);
	case VT_DATE:
		if (!VariantTimeToSystemTime(source->date, &time))
			return E_INVALIDARG;
		return render_time(&time, out);
	case VT_FILETIME:
		if (!FileTimeToSystemTime(&source->filetime, &time))
			return E_INVALIDARG;
		return render_time(&time, out);
	case VT_CLSID:
		if (!source->puuid)
			return E_INVALIDARG;
		StringFromGUID2(source->puuid, guid, ARRAYSIZE(guid));
		return (*out = ps_copy_text(guid)) ? S_OK : E_OUTOFMEMORY;
	case VT_LPWSTR:
		return (*out = ps_copy_text(source->pwszVal ? source->pwszVal : L"")) ? S_OK : E_OUTOFMEMORY;
	case VT_BSTR:
		return (*out = ps_copy_text(source->bstrVal ? source->bstrVal : L"")) ? S_OK : E_OUTOFMEMORY;
	case VT_LPSTR:
		return (*out = source->pszVal ? ps_widen(source->pszVal) : ps_copy_text(L"")) ? S_OK : E_OUTOFMEMORY;
	default:
	{
		number value;
		HRESULT status = read_number(source, &value);
		if (FAILED(status))
			return status;
		return (*out = ps_copy_text(format_unsigned(digits + ARRAYSIZE(digits), value.magnitude, value.negative))) ? S_OK : E_OUTOFMEMORY;
	}
	}
}

HRESULT WINAPI PropVariantToStringAlloc(REFPROPVARIANT source, WCHAR **out)
{
	return ps_render(source, out);
}

static HRESULT copy_bounded(const WCHAR *text, WCHAR *out, UINT room)
{
	SIZE_T length = wcslen(text);
	if (length >= room)
	{
		memcpy(out, text, (room - 1) * sizeof(WCHAR));
		out[room - 1] = 0;
		return STRSAFE_E_INSUFFICIENT_BUFFER;
	}
	memcpy(out, text, (length + 1) * sizeof(WCHAR));
	return S_OK;
}

HRESULT WINAPI PropVariantToString(REFPROPVARIANT source, PWSTR out, UINT room)
{
	WCHAR *text;
	HRESULT status;
	if (!room)
	{
		out[0] = 0;
		return E_INVALIDARG;
	}
	out[0] = 0;
	status = ps_render(source, &text);
	if (FAILED(status))
		return status;
	status = copy_bounded(text, out, room);
	CoTaskMemFree(text);
	return status;
}

PCWSTR WINAPI PropVariantToStringWithDefault(REFPROPVARIANT source, LPCWSTR fallback)
{
	if (source->vt == VT_BSTR)
		return source->bstrVal ? source->bstrVal : L"";
	if (source->vt == VT_LPWSTR && source->pwszVal)
		return source->pwszVal;
	return fallback;
}

HRESULT WINAPI PropVariantToBSTR(REFPROPVARIANT source, BSTR *out)
{
	WCHAR *text;
	HRESULT status = ps_render(source, &text);
	if (FAILED(status))
		return status;
	*out = SysAllocString(text);
	CoTaskMemFree(text);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* Buffers and GUIDs */

HRESULT WINAPI PropVariantToBuffer(REFPROPVARIANT source, void *out, UINT size)
{
	void *data;
	HRESULT status;
	if (source->vt == (VT_VECTOR | VT_UI1))
	{
		if (size > source->caub.cElems)
			return E_FAIL;
		if (size)
			memcpy(out, source->caub.pElems, size);
		return S_OK;
	}
	if (source->vt != (VT_ARRAY | VT_UI1))
		return E_INVALIDARG;
	if (SafeArrayGetDim(source->parray) != 1 || size > source->parray->rgsabound[0].cElements * source->parray->cbElements)
		return E_FAIL;
	status = SafeArrayAccessData(source->parray, &data);
	if (FAILED(status))
		return status;
	if (size)
		memcpy(out, data, size);
	SafeArrayUnaccessData(source->parray);
	return S_OK;
}

static BOOL guid_from_text(const WCHAR *text, GUID *out)
{
	static const BYTE places[] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
	BYTE *bytes = (BYTE *)out;
	UINT index;
	if (!text || *text++ != '{')
		return FALSE;
	for (index = 0; index < 16; index++)
	{
		UINT high, low;
		if (index == 4 || index == 6 || index == 8 || index == 10)
		{
			if (*text++ != '-')
				return FALSE;
		}
		high = text[0];
		low = high ? text[1] : 0;
		high = high >= '0' && high <= '9' ? high - '0' : (high | 32) >= 'a' && (high | 32) <= 'f' ? (high | 32) - 'a' + 10 : 16;
		low = low >= '0' && low <= '9' ? low - '0' : (low | 32) >= 'a' && (low | 32) <= 'f' ? (low | 32) - 'a' + 10 : 16;
		if (high > 15 || low > 15)
			return FALSE;
		bytes[places[index]] = (BYTE)(high << 4 | low);
		text += 2;
	}
	return text[0] == '}' && !text[1];
}

static HRESULT guid_from_array(SAFEARRAY *array, GUID *out)
{
	void *data;
	HRESULT status;
	if (SafeArrayGetDim(array) != 1 || array->rgsabound[0].cElements * array->cbElements != sizeof(GUID))
		return E_INVALIDARG;
	status = SafeArrayAccessData(array, &data);
	if (FAILED(status))
		return status;
	memcpy(out, data, sizeof(GUID));
	SafeArrayUnaccessData(array);
	return S_OK;
}

HRESULT WINAPI PropVariantToGUID(const PROPVARIANT *source, GUID *out)
{
	switch (source->vt)
	{
	case VT_CLSID:
		if (!source->puuid)
			return E_INVALIDARG;
		*out = *source->puuid;
		return S_OK;
	case VT_LPWSTR:
		return guid_from_text(source->pwszVal, out) ? S_OK : E_INVALIDARG;
	case VT_BSTR:
		return guid_from_text(source->bstrVal, out) ? S_OK : E_INVALIDARG;
	case VT_ARRAY | VT_UI1:
		return guid_from_array(source->parray, out);
	case VT_VECTOR | VT_UI1:
		if (source->caub.cElems != sizeof(GUID))
			return E_INVALIDARG;
		memcpy(out, source->caub.pElems, sizeof(GUID));
		return S_OK;
	}
	return E_INVALIDARG;
}

HRESULT WINAPI VariantToGUID(const VARIANT *source, GUID *out)
{
	switch (V_VT(source))
	{
	case VT_BSTR:
		return guid_from_text(V_BSTR(source), out) ? S_OK : E_FAIL;
	case VT_ARRAY | VT_UI1:
		return guid_from_array(V_ARRAY(source), out);
	}
	return E_NOTIMPL;
}

/* Construction */

HRESULT WINAPI InitPropVariantFromGUIDAsString(REFGUID guid, PROPVARIANT *out)
{
	WCHAR text[40];
	PropVariantInit(out);
	if (!guid)
		return E_FAIL;
	StringFromGUID2(guid, text, ARRAYSIZE(text));
	if (!(out->pwszVal = ps_copy_text(text)))
		return E_OUTOFMEMORY;
	out->vt = VT_LPWSTR;
	return S_OK;
}

HRESULT WINAPI InitVariantFromGUIDAsString(REFGUID guid, VARIANT *out)
{
	WCHAR text[40];
	VariantInit(out);
	if (!guid)
		return E_FAIL;
	StringFromGUID2(guid, text, ARRAYSIZE(text));
	if (!(V_BSTR(out) = SysAllocString(text)))
		return E_OUTOFMEMORY;
	V_VT(out) = VT_BSTR;
	return S_OK;
}

HRESULT WINAPI InitPropVariantFromBuffer(const VOID *data, UINT size, PROPVARIANT *out)
{
	PropVariantInit(out);
	out->caub.pElems = CoTaskMemAlloc(size);
	if (!out->caub.pElems)
		return E_OUTOFMEMORY;
	out->vt = VT_VECTOR | VT_UI1;
	out->caub.cElems = size;
	if (size)
		memcpy(out->caub.pElems, data, size);
	return S_OK;
}

HRESULT WINAPI InitVariantFromBuffer(const VOID *data, UINT size, VARIANT *out)
{
	SAFEARRAY *array;
	void *target;
	HRESULT status;
	VariantInit(out);
	if (!(array = SafeArrayCreateVector(VT_UI1, 0, size)))
		return E_OUTOFMEMORY;
	status = SafeArrayAccessData(array, &target);
	if (FAILED(status))
	{
		SafeArrayDestroy(array);
		return status;
	}
	if (size)
		memcpy(target, data, size);
	SafeArrayUnaccessData(array);
	V_VT(out) = VT_ARRAY | VT_UI1;
	V_ARRAY(out) = array;
	return S_OK;
}

HRESULT WINAPI InitPropVariantFromCLSID(REFCLSID clsid, PROPVARIANT *out)
{
	PropVariantInit(out);
	if (!(out->puuid = CoTaskMemAlloc(sizeof(CLSID))))
		return E_OUTOFMEMORY;
	*out->puuid = *clsid;
	out->vt = VT_CLSID;
	return S_OK;
}

HRESULT WINAPI InitPropVariantFromFileTime(const FILETIME *time, PROPVARIANT *out)
{
	PropVariantInit(out);
	out->vt = VT_FILETIME;
	out->filetime = *time;
	return S_OK;
}

HRESULT WINAPI InitPropVariantFromStringVector(PCWSTR *strings, ULONG count, PROPVARIANT *out)
{
	ULONG index;
	PropVariantInit(out);
	/* An empty vector still owns a block, so callers can tell it from a variant that holds nothing. */
	if (!(out->calpwstr.pElems = CoTaskMemAlloc(count ? count * sizeof(WCHAR *) : 1)))
		return E_OUTOFMEMORY;
	out->vt = VT_VECTOR | VT_LPWSTR;
	for (index = 0; index < count; index++)
	{
		out->calpwstr.pElems[index] = ps_copy_text(strings[index]);
		if (!out->calpwstr.pElems[index])
		{
			out->calpwstr.cElems = index;
			PropVariantClear(out);
			return E_OUTOFMEMORY;
		}
	}
	out->calpwstr.cElems = count;
	return S_OK;
}

HRESULT WINAPI InitVariantFromFileTime(const FILETIME *time, VARIANT *out)
{
	SYSTEMTIME system;
	double date;
	VariantInit(out);
	V_DATE(out) = 0;
	if (!FileTimeToSystemTime(time, &system) || !SystemTimeToVariantTime(&system, &date))
		return E_INVALIDARG;
	V_VT(out) = VT_DATE;
	V_DATE(out) = date;
	return S_OK;
}

/* Lists */

#define LIST_FAMILY(name, member, type_code, type) \
	HRESULT WINAPI InitPropVariantFrom##name##Vector(const type *items, ULONG count, PROPVARIANT *out) \
	{ \
		PropVariantInit(out); \
		if (!(out->member.pElems = CoTaskMemAlloc(count ? count * sizeof(type) : 1))) \
			return E_OUTOFMEMORY; \
		if (count) \
			memcpy(out->member.pElems, items, count * sizeof(type)); \
		out->vt = VT_VECTOR | type_code; \
		out->member.cElems = count; \
		return S_OK; \
	} \
	HRESULT WINAPI PropVariantTo##name##Vector(REFPROPVARIANT source, type *items, ULONG room, ULONG *count) \
	{ \
		*count = 0; \
		if (source->vt != (VT_VECTOR | type_code)) \
			return E_INVALIDARG; \
		if (source->member.cElems > room) \
			return TYPE_E_BUFFERTOOSMALL; \
		memcpy(items, source->member.pElems, source->member.cElems * sizeof(type)); \
		*count = source->member.cElems; \
		return S_OK; \
	} \
	HRESULT WINAPI PropVariantTo##name##VectorAlloc(REFPROPVARIANT source, type **items, ULONG *count) \
	{ \
		*items = NULL; \
		*count = 0; \
		if (source->vt != (VT_VECTOR | type_code)) \
			return E_INVALIDARG; \
		if (!(*items = CoTaskMemAlloc(source->member.cElems ? source->member.cElems * sizeof(type) : 1))) \
			return E_OUTOFMEMORY; \
		memcpy(*items, source->member.pElems, source->member.cElems * sizeof(type)); \
		*count = source->member.cElems; \
		return S_OK; \
	} \
	HRESULT WINAPI PropVariantGet##name##Elem(REFPROPVARIANT source, ULONG index, type *out) \
	{ \
		if (source->vt != (VT_VECTOR | type_code) || index >= source->member.cElems) \
			return E_INVALIDARG; \
		memcpy(out, &source->member.pElems[index], sizeof(type)); \
		return S_OK; \
	}

LIST_FAMILY(Int16, cai, VT_I2, SHORT)
LIST_FAMILY(UInt16, caui, VT_UI2, USHORT)
LIST_FAMILY(Int32, cal, VT_I4, LONG)
LIST_FAMILY(UInt32, caul, VT_UI4, ULONG)
LIST_FAMILY(Int64, cah, VT_I8, LONGLONG)
LIST_FAMILY(UInt64, cauh, VT_UI8, ULONGLONG)
LIST_FAMILY(Double, cadbl, VT_R8, DOUBLE)

ULONG WINAPI PropVariantGetElementCount(REFPROPVARIANT source)
{
	if (source->vt == VT_EMPTY || source->vt == VT_NULL)
		return 0;
	if (source->vt & VT_VECTOR)
		return source->caub.cElems;
	if (source->vt & VT_ARRAY)
	{
		ULONG total = 1;
		UINT dim;
		for (dim = 0; dim < SafeArrayGetDim(source->parray); dim++)
			total *= source->parray->rgsabound[dim].cElements;
		return total;
	}
	return 1;
}

HRESULT WINAPI PropVariantGetStringElem(REFPROPVARIANT source, ULONG index, WCHAR **out)
{
	BSTR item = NULL;
	LONG place = index;
	HRESULT status;
	*out = NULL;
	switch (source->vt)
	{
	case VT_BSTR:
		if (index)
			return E_INVALIDARG;
		*out = ps_copy_text(source->bstrVal ? source->bstrVal : L"");
		break;
	case VT_LPWSTR:
		if (index)
			return E_INVALIDARG;
		*out = ps_copy_text(source->pwszVal ? source->pwszVal : L"");
		break;
	case VT_VECTOR | VT_LPWSTR:
		if (index >= source->calpwstr.cElems)
			return E_INVALIDARG;
		*out = ps_copy_text(source->calpwstr.pElems[index] ? source->calpwstr.pElems[index] : L"");
		break;
	case VT_VECTOR | VT_BSTR:
		if (index >= source->cabstr.cElems)
			return E_INVALIDARG;
		*out = ps_copy_text(source->cabstr.pElems[index] ? source->cabstr.pElems[index] : L"");
		break;
	case VT_ARRAY | VT_BSTR:
		/* The index is checked against the count but read as an array subscript, so a raised lower bound shifts both ways. */
		if (SafeArrayGetDim(source->parray) != 1 || index >= source->parray->rgsabound[0].cElements)
			return E_INVALIDARG;
		status = SafeArrayGetElement(source->parray, &place, &item);
		if (FAILED(status))
			return status;
		*out = ps_copy_text(item ? item : L"");
		SysFreeString(item);
		break;
	default:
		return E_INVALIDARG;
	}
	return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI PropVariantToFileTime(REFPROPVARIANT source, int flags, FILETIME *out)
{
	FILETIME time;
	SYSTEMTIME system;
	if (source->vt == VT_FILETIME)
		time = source->filetime;
	else if (source->vt == VT_DATE)
	{
		if (!VariantTimeToSystemTime(source->date, &system) || !SystemTimeToFileTime(&system, &time))
			return E_INVALIDARG;
	}
	else
		return E_INVALIDARG;
	if (flags == PSTF_LOCAL_TIME)
		return FileTimeToLocalFileTime(&time, out) ? S_OK : E_INVALIDARG;
	*out = time;
	return S_OK;
}

void WINAPI ClearPropVariantArray(PROPVARIANT *items, UINT count)
{
	while (count--)
		PropVariantClear(&items[count]);
}

void WINAPI ClearVariantArray(VARIANT *items, UINT count)
{
	while (count--)
		VariantClear(&items[count]);
}

/* Changing types */

HRESULT WINAPI PropVariantChangeType(PROPVARIANT *out, REFPROPVARIANT source, PROPVAR_CHANGE_FLAGS flags, VARTYPE type)
{
	HRESULT status;
	PropVariantInit(out);
	switch (type)
	{
	case VT_EMPTY:
		return S_OK;
	case VT_I2:
		status = PropVariantToInt16(source, &out->iVal);
		break;
	case VT_UI2:
		status = PropVariantToUInt16(source, &out->uiVal);
		break;
	case VT_I4:
		status = PropVariantToInt32(source, &out->lVal);
		break;
	case VT_UI4:
		status = PropVariantToUInt32(source, &out->ulVal);
		break;
	case VT_I8:
		status = PropVariantToInt64(source, &out->hVal.QuadPart);
		break;
	case VT_UI8:
		status = PropVariantToUInt64(source, &out->uhVal.QuadPart);
		break;
	case VT_R8:
		status = PropVariantToDouble(source, &out->dblVal);
		break;
	case VT_R4:
	{
		double value;
		status = PropVariantToDouble(source, &value);
		if (SUCCEEDED(status))
			out->fltVal = (float)value;
		break;
	}
	case VT_BOOL:
	{
		BOOL value;
		status = PropVariantToBoolean(source, &value);
		if (SUCCEEDED(status))
			out->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
		break;
	}
	case VT_LPWSTR:
		status = ps_render(source, &out->pwszVal);
		break;
	case VT_BSTR:
		status = PropVariantToBSTR(source, &out->bstrVal);
		break;
	case VT_CLSID:
		if (!(out->puuid = CoTaskMemAlloc(sizeof(CLSID))))
			return E_OUTOFMEMORY;
		status = PropVariantToGUID(source, out->puuid);
		if (FAILED(status))
		{
			CoTaskMemFree(out->puuid);
			out->puuid = NULL;
		}
		break;
	default:
		status = source->vt == type ? PropVariantCopy(out, source) : E_NOTIMPL;
		return status;
	}
	if (FAILED(status))
	{
		/* A negative number cannot be spoken as an unsigned one: callers read this as a mismatch, not an overflow. */
		if ((type == VT_UI2 || type == VT_UI4 || type == VT_UI8) && status == OVERFLOW_STATUS &&
			(source->vt == VT_LPWSTR || source->vt == VT_BSTR || source->vt == VT_LPSTR))
			status = TYPE_E_TYPEMISMATCH;
		return status;
	}
	out->vt = type;
	return S_OK;
}

/* Comparison */

static BOOL holds_nothing(const PROPVARIANT *value)
{
	if (value->vt == VT_EMPTY || value->vt == VT_NULL)
		return TRUE;
	if (value->vt == VT_CLSID)
		return !value->puuid;
	if (value->vt & VT_VECTOR)
		return !value->caub.cElems;
	if (value->vt & VT_ARRAY)
		return !value->parray || !value->parray->rgsabound[0].cElements;
	return FALSE;
}

static int sign_of(int value)
{
	return value < 0 ? -1 : value > 0;
}

static int compare_text(const WCHAR *first, const WCHAR *second, PROPVAR_COMPARE_FLAGS flags)
{
	first = first ? first : L"";
	second = second ? second : L"";
	if (flags & (PVCF_USESTRCMP | PVCF_USESTRCMPC))
		return sign_of(wcscmp(first, second));
	return sign_of(CompareStringW(LOCALE_USER_DEFAULT, flags & (PVCF_USESTRCMPI | PVCF_USESTRCMPIC) ? NORM_IGNORECASE : 0, first, -1, second, -1) - CSTR_EQUAL);
}

INT WINAPI PropVariantCompareEx(REFPROPVARIANT first, REFPROPVARIANT second, PROPVAR_COMPARE_UNIT unit, PROPVAR_COMPARE_FLAGS flags)
{
	const int nothing_rank = flags & PVCF_TREATEMPTYASGREATERTHAN ? 1 : -1;
	PROPVARIANT converted;
	const PROPVARIANT *other = second;
	BOOL first_empty = holds_nothing(first), second_empty = holds_nothing(second);
	int result = -1;
	if (first_empty || second_empty)
		return first_empty && second_empty ? 0 : first_empty ? nothing_rank : -nothing_rank;
	PropVariantInit(&converted);
	if (first->vt != second->vt)
	{
		if (FAILED(PropVariantChangeType(&converted, second, 0, first->vt)))
			return -1;
		other = &converted;
	}
	switch (first->vt)
	{
	case VT_I1:
		result = first->cVal < other->cVal ? -1 : first->cVal > other->cVal;
		break;
	case VT_UI1:
		result = first->bVal < other->bVal ? -1 : first->bVal > other->bVal;
		break;
	case VT_I2:
		result = first->iVal < other->iVal ? -1 : first->iVal > other->iVal;
		break;
	case VT_UI2:
		result = first->uiVal < other->uiVal ? -1 : first->uiVal > other->uiVal;
		break;
	case VT_I4:
		result = first->lVal < other->lVal ? -1 : first->lVal > other->lVal;
		break;
	case VT_UI4:
		result = first->ulVal < other->ulVal ? -1 : first->ulVal > other->ulVal;
		break;
	case VT_I8:
		result = first->hVal.QuadPart < other->hVal.QuadPart ? -1 : first->hVal.QuadPart > other->hVal.QuadPart;
		break;
	case VT_UI8:
		result = first->uhVal.QuadPart < other->uhVal.QuadPart ? -1 : first->uhVal.QuadPart > other->uhVal.QuadPart;
		break;
	case VT_R4:
		result = first->fltVal < other->fltVal ? -1 : first->fltVal > other->fltVal;
		break;
	case VT_R8:
	case VT_DATE:
		result = first->dblVal < other->dblVal ? -1 : first->dblVal > other->dblVal;
		break;
	case VT_BOOL:
		result = first->boolVal == other->boolVal ? 0 : first->boolVal ? -1 : 1;
		break;
	case VT_FILETIME:
		result = sign_of(CompareFileTime(&first->filetime, &other->filetime));
		break;
	case VT_CLSID:
		result = sign_of(memcmp(first->puuid, other->puuid, sizeof(CLSID)));
		break;
	case VT_LPWSTR:
		result = compare_text(first->pwszVal, other->pwszVal, flags);
		break;
	case VT_BSTR:
		result = compare_text(first->bstrVal, other->bstrVal, flags);
		break;
	case VT_VECTOR | VT_UI1:
	case VT_VECTOR | VT_I1:
	{
		ULONG shared = min(first->caub.cElems, other->caub.cElems);
		result = sign_of(memcmp(first->caub.pElems, other->caub.pElems, shared));
		if (!result)
			result = first->caub.cElems < other->caub.cElems ? -1 : first->caub.cElems > other->caub.cElems;
		break;
	}
	}
	if (other == &converted)
		PropVariantClear(&converted);
	return result;
}

/* Variants */

HRESULT WINAPI VariantToPropVariant(const VARIANT *source, PROPVARIANT *out)
{
	VARIANT plain;
	HRESULT status;
	if (!source || !out)
		return E_INVALIDARG;
	if (V_VT(source) == VT_ILLEGAL)
		return TYPE_E_TYPEMISMATCH;
	if (V_VT(source) == VT_CLSID || (V_VT(source) & VT_TYPEMASK) > VT_VERSIONED_STREAM)
		return DISP_E_BADVARTYPE;
	VariantInit(&plain);
	status = VariantCopyInd(&plain, source);
	if (FAILED(status))
		return status;
	/* The two share their layout for everything a variant can hold, so ownership moves across. */
	PropVariantInit(out);
	memcpy(out, &plain, sizeof(plain));
	return S_OK;
}

HRESULT WINAPI PropVariantToVariant(const PROPVARIANT *source, VARIANT *out)
{
	WCHAR *text;
	HRESULT status;
	if (!source || !out)
		return E_INVALIDARG;
	switch (source->vt)
	{
	case VT_CLSID:
	case VT_LPWSTR:
	case VT_LPSTR:
		status = ps_render(source, &text);
		if (FAILED(status))
			return status;
		V_BSTR(out) = SysAllocString(text);
		CoTaskMemFree(text);
		if (!V_BSTR(out))
			return E_OUTOFMEMORY;
		V_VT(out) = VT_BSTR;
		/* A class is reported by how long its text is, which still reads as success. */
		return source->vt == VT_CLSID ? 39 : S_OK;
	case VT_ILLEGAL:
		return E_OUTOFMEMORY;
	case VT_FILETIME:
		return InitVariantFromFileTime(&source->filetime, out);
	}
	if ((source->vt & VT_TYPEMASK) > VT_VERSIONED_STREAM)
		return E_OUTOFMEMORY;
	if (source->vt & VT_VECTOR)
		return E_INVALIDARG;
	VariantInit(out);
	return VariantCopy(out, (const VARIANT *)source);
}

/* Reads a variant through the conversions above: they only look at the shared part of the layout. */
static const PROPVARIANT *plain_variant(const VARIANT *source)
{
	while (V_VT(source) == (VT_BYREF | VT_VARIANT))
		source = V_VARIANTREF(source);
	return (const PROPVARIANT *)source;
}

PCWSTR WINAPI VariantToStringWithDefault(const VARIANT *source, LPCWSTR fallback)
{
	const VARIANT *plain = (const VARIANT *)plain_variant(source);
	if (V_VT(plain) == VT_BSTR)
		return V_BSTR(plain) ? V_BSTR(plain) : L"";
	if (V_VT(plain) == (VT_BYREF | VT_BSTR))
		return *V_BSTRREF(plain) ? *V_BSTRREF(plain) : L"";
	return fallback;
}

HRESULT WINAPI VariantToString(REFVARIANT source, PWSTR out, UINT room)
{
	const VARIANT *plain = (const VARIANT *)plain_variant(source);
	if (room)
		out[0] = 0;
	else
	{
		out[0] = 0;
		return E_INVALIDARG;
	}
	if (V_VT(plain) == (VT_BYREF | VT_BSTR))
		return copy_bounded(*V_BSTRREF(plain) ? *V_BSTRREF(plain) : L"", out, room);
	return PropVariantToString((const PROPVARIANT *)plain, out, room);
}

HRESULT WINAPI VariantToStringAlloc(REFVARIANT source, WCHAR **out)
{
	return ps_render(plain_variant(source), out);
}

#define VARIANT_SCALAR(name, type) \
	HRESULT WINAPI VariantTo##name(REFVARIANT source, type *out) \
	{ \
		return PropVariantTo##name(plain_variant(source), out); \
	} \
	type WINAPI VariantTo##name##WithDefault(REFVARIANT source, type fallback) \
	{ \
		return PropVariantTo##name##WithDefault(plain_variant(source), fallback); \
	}

VARIANT_SCALAR(Int16, SHORT)
VARIANT_SCALAR(UInt16, USHORT)
VARIANT_SCALAR(Int32, LONG)
VARIANT_SCALAR(UInt32, ULONG)
VARIANT_SCALAR(Int64, LONGLONG)
VARIANT_SCALAR(UInt64, ULONGLONG)
VARIANT_SCALAR(Double, DOUBLE)
VARIANT_SCALAR(Boolean, BOOL)

HRESULT WINAPI VariantToBuffer(REFVARIANT source, void *out, UINT size)
{
	return PropVariantToBuffer(plain_variant(source), out, size);
}

INT WINAPI VariantCompare(REFVARIANT first, REFVARIANT second)
{
	return PropVariantCompareEx(plain_variant(first), plain_variant(second), 0, 0);
}

ULONG WINAPI VariantGetElementCount(REFVARIANT source)
{
	return PropVariantGetElementCount(plain_variant(source));
}
