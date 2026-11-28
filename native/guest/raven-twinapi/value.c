/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Foundation.PropertyValue: the boxes that carry a value of the Windows Runtime as an object, as settings and property sets keep
 * them. A box answers IPropertyValue and the IReference<T> of its type, or IReferenceArray<T> for an array, and its class name is that
 * interface's name, as on Windows.
 *
 * A box is read as its own type, and a number also as any other numeric type that holds its value: integers between themselves, either
 * way within range (DISP_E_OVERFLOW beyond it), and integers and floating values as floating values; a floating value is not read as an
 * integer. Anything else is TYPE_E_TYPEMISMATCH. An array is read only as its own type; reading one hands out a copy that the caller
 * frees with CoTaskMemFree, with its strings and objects its own. CreateInspectable answers the object it is given, which is its own box,
 * and CreateEmpty answers no object at all. */
#include "object.h"
#include <objbase.h>

static const IID iid_property_value = {0x4bd682dd, 0x7554, 0x40e9, {0x9a, 0x9b, 0x82, 0x65, 0x4e, 0xde, 0x7e, 0x62}};
static const IID iid_property_value_statics = {0x629bdbc8, 0xd932, 0x4ff4, {0x96, 0xb9, 0x8d, 0x96, 0xc5, 0xc1, 0xe8, 0x58}};

#define OVERFLOW ((HRESULT)0x8002000a)

/* PropertyType: the scalar types from 1 to 19, an array of one of them 1024 above it. */
enum { type_empty, type_uint8, type_int16, type_uint16, type_int32, type_uint32, type_int64, type_uint64, type_single, type_double,
	type_char16, type_boolean, type_string, type_inspectable, type_date_time, type_time_span, type_guid, type_point, type_size, type_rect };
#define ARRAY 1024
#define SCALARS 19

struct point { FLOAT x, y; };
struct rect { FLOAT x, y, width, height; };

/* Each scalar type: its size, IReference<T> and IReferenceArray<T>, whose identifiers derive from pinterface({61c17706-...};T) and
 * pinterface({61c17707-...};T) with the signature T beside them, and the class names of its boxes. */
struct type_info {
	size_t size;
	IID reference, reference_array;
	const WCHAR *name, *array_name;
};

static const struct type_info types[SCALARS + 1] = {
	[type_uint8] = {1, {0xe5198cc8, 0x2873, 0x55f5, {0xb0, 0xa1, 0x84, 0xff, 0x9e, 0x4a, 0xad, 0x62}},
		{0x2af22683, 0x3734, 0x56d0, {0xa6, 0x0e, 0x68, 0x8c, 0xc8, 0x5d, 0x16, 0x19}}, /* u1 */
		L"Windows.Foundation.IReference`1<UInt8>", L"Windows.Foundation.IReferenceArray`1<UInt8>"},
	[type_int16] = {2, {0x6ec9e41b, 0x6709, 0x5647, {0x99, 0x18, 0xa1, 0x27, 0x01, 0x10, 0xfc, 0x4e}},
		{0x912f8fd7, 0xadc0, 0x5d60, {0xa8, 0x96, 0x7e, 0xd7, 0x60, 0x89, 0xcc, 0x5b}}, /* i2 */
		L"Windows.Foundation.IReference`1<Int16>", L"Windows.Foundation.IReferenceArray`1<Int16>"},
	[type_uint16] = {2, {0x5ab7d2c3, 0x6b62, 0x5e71, {0xa4, 0xb6, 0x2d, 0x49, 0xc4, 0xf2, 0x38, 0xfd}},
		{0x6624a2dd, 0x83f7, 0x519c, {0x9d, 0x55, 0xbb, 0x1f, 0x65, 0x60, 0x45, 0x6b}}, /* u2 */
		L"Windows.Foundation.IReference`1<UInt16>", L"Windows.Foundation.IReferenceArray`1<UInt16>"},
	[type_int32] = {4, {0x548cefbd, 0xbc8a, 0x5fa0, {0x8d, 0xf2, 0x95, 0x74, 0x40, 0xfc, 0x8b, 0xf4}},
		{0xa6d080a5, 0xb087, 0x5bc2, {0x9a, 0x9f, 0x5c, 0xd6, 0x87, 0xb4, 0xd1, 0xf7}}, /* i4 */
		L"Windows.Foundation.IReference`1<Int32>", L"Windows.Foundation.IReferenceArray`1<Int32>"},
	[type_uint32] = {4, {0x513ef3af, 0xe784, 0x5325, {0xa9, 0x1e, 0x97, 0xc2, 0xb8, 0x11, 0x1c, 0xf3}},
		{0x97374b68, 0xeb87, 0x56cc, {0xb1, 0x8e, 0x27, 0xef, 0x0f, 0x9c, 0xfc, 0x0c}}, /* u4 */
		L"Windows.Foundation.IReference`1<UInt32>", L"Windows.Foundation.IReferenceArray`1<UInt32>"},
	[type_int64] = {8, {0x4dda9e24, 0xe69f, 0x5c6a, {0xa0, 0xa6, 0x93, 0x42, 0x73, 0x65, 0xaf, 0x2a}},
		{0x6e333271, 0x2e2a, 0x5955, {0x87, 0x90, 0x83, 0x6c, 0x76, 0xee, 0x53, 0xb6}}, /* i8 */
		L"Windows.Foundation.IReference`1<Int64>", L"Windows.Foundation.IReferenceArray`1<Int64>"},
	[type_uint64] = {8, {0x6755e376, 0x53bb, 0x568b, {0xa1, 0x1d, 0x17, 0x23, 0x98, 0x68, 0x30, 0x9e}},
		{0x38b60434, 0xd67c, 0x523e, {0x9d, 0x0e, 0x24, 0xd6, 0x43, 0x41, 0x10, 0x73}}, /* u8 */
		L"Windows.Foundation.IReference`1<UInt64>", L"Windows.Foundation.IReferenceArray`1<UInt64>"},
	[type_single] = {4, {0x719cc2ba, 0x3e76, 0x5def, {0x9f, 0x1a, 0x38, 0xd8, 0x5a, 0x14, 0x5e, 0xa8}},
		{0x6ab1ea83, 0xcb41, 0x5f99, {0x92, 0xcc, 0x23, 0xbd, 0x43, 0x36, 0xa1, 0xfb}}, /* f4 */
		L"Windows.Foundation.IReference`1<Single>", L"Windows.Foundation.IReferenceArray`1<Single>"},
	[type_double] = {8, {0x2f2d6c29, 0x5473, 0x5f3e, {0x92, 0xe7, 0x96, 0x57, 0x2b, 0xb9, 0x90, 0xe2}},
		{0xd301f253, 0xe0a3, 0x5d2b, {0x9a, 0x41, 0xa4, 0xd6, 0x2b, 0xec, 0x46, 0x23}}, /* f8 */
		L"Windows.Foundation.IReference`1<Double>", L"Windows.Foundation.IReferenceArray`1<Double>"},
	[type_char16] = {2, {0xfb393ef3, 0xbbac, 0x5bd5, {0x91, 0x44, 0x84, 0xf2, 0x35, 0x76, 0xf4, 0x15}},
		{0xa4095aab, 0xeb7d, 0x5782, {0x8f, 0xad, 0x16, 0x09, 0xde, 0xa2, 0x49, 0xad}}, /* c2 */
		L"Windows.Foundation.IReference`1<Char16>", L"Windows.Foundation.IReferenceArray`1<Char16>"},
	[type_boolean] = {1, {0x3c00fd60, 0x2950, 0x5939, {0xa2, 0x1a, 0x2d, 0x12, 0xc5, 0xa0, 0x1b, 0x8a}},
		{0xe8e72666, 0x48cc, 0x593f, {0xba, 0x85, 0x26, 0x63, 0x49, 0x69, 0x56, 0xe3}}, /* b1 */
		L"Windows.Foundation.IReference`1<Boolean>", L"Windows.Foundation.IReferenceArray`1<Boolean>"},
	[type_string] = {sizeof(HSTRING), {0xfd416dfb, 0x2a07, 0x52eb, {0xaa, 0xe3, 0xdf, 0xce, 0x14, 0x11, 0x6c, 0x05}},
		{0x0385688e, 0xe3c7, 0x5c5e, {0xa3, 0x89, 0x55, 0x24, 0xed, 0xe3, 0x49, 0xf1}}, /* string */
		L"Windows.Foundation.IReference`1<String>", L"Windows.Foundation.IReferenceArray`1<String>"},
	[type_inspectable] = {sizeof(void *), {0x06dccc90, 0xa058, 0x5c88, {0x87, 0xb7, 0x6f, 0x33, 0x60, 0xa2, 0xfc, 0x16}},
		{0x9cd7a84f, 0x0c80, 0x59c5, {0xb4, 0x4e, 0x97, 0x78, 0x41, 0xbb, 0x43, 0xd9}}, /* cinterface(IInspectable) */
		L"Windows.Foundation.IReference`1<Object>", L"Windows.Foundation.IReferenceArray`1<Object>"},
	[type_date_time] = {8, {0x5541d8a7, 0x497c, 0x5aa4, {0x86, 0xfc, 0x77, 0x13, 0xad, 0xbf, 0x2a, 0x2c}},
		{0x1b8e9594, 0x588e, 0x5a07, {0x9e, 0x65, 0x07, 0x31, 0xa4, 0xc9, 0xa2, 0xdb}}, /* struct(Windows.Foundation.DateTime;i8) */
		L"Windows.Foundation.IReference`1<Windows.Foundation.DateTime>", L"Windows.Foundation.IReferenceArray`1<Windows.Foundation.DateTime>"},
	[type_time_span] = {8, {0x604d0c4c, 0x91de, 0x5c2a, {0x93, 0x5f, 0x36, 0x2f, 0x13, 0xea, 0xf8, 0x00}},
		{0xad73197d, 0x2cfa, 0x57a6, {0x89, 0x93, 0x9f, 0xac, 0x40, 0xfe, 0xb7, 0x91}}, /* struct(Windows.Foundation.TimeSpan;i8) */
		L"Windows.Foundation.IReference`1<Windows.Foundation.TimeSpan>", L"Windows.Foundation.IReferenceArray`1<Windows.Foundation.TimeSpan>"},
	[type_guid] = {16, {0x7d50f649, 0x632c, 0x51f9, {0x84, 0x9a, 0xee, 0x49, 0x42, 0x89, 0x33, 0xea}},
		{0xeecf9838, 0xc1c2, 0x5b4a, {0x97, 0x6f, 0xce, 0xc2, 0x61, 0xae, 0x1d, 0x55}}, /* g16 */
		L"Windows.Foundation.IReference`1<Guid>", L"Windows.Foundation.IReferenceArray`1<Guid>"},
	[type_point] = {8, {0x84f14c22, 0xa00a, 0x5272, {0x8d, 0x3d, 0x82, 0x11, 0x2e, 0x66, 0xdf, 0x00}},
		{0x39313214, 0x5c7d, 0x599d, {0xae, 0x5a, 0x17, 0xd9, 0xd6, 0x49, 0x22, 0x58}}, /* struct(Windows.Foundation.Point;f4;f4) */
		L"Windows.Foundation.IReference`1<Windows.Foundation.Point>", L"Windows.Foundation.IReferenceArray`1<Windows.Foundation.Point>"},
	[type_size] = {8, {0x61723086, 0x8e53, 0x5276, {0x9f, 0x36, 0x2a, 0x4b, 0xb9, 0x3e, 0x2b, 0x75}},
		{0x3b40e9d4, 0xe0c3, 0x56f6, {0xb8, 0x8b, 0xe5, 0x05, 0xeb, 0x73, 0x75, 0x7b}}, /* struct(Windows.Foundation.Size;f4;f4) */
		L"Windows.Foundation.IReference`1<Windows.Foundation.Size>", L"Windows.Foundation.IReferenceArray`1<Windows.Foundation.Size>"},
	[type_rect] = {16, {0x80423f11, 0x054f, 0x5eac, {0xaf, 0xd3, 0x63, 0xb6, 0xce, 0x15, 0xe7, 0x7b}},
		{0x8a444256, 0xd661, 0x5e9a, {0xa7, 0x2b, 0xd8, 0xf1, 0xd7, 0x96, 0x2d, 0x0c}}, /* struct(Windows.Foundation.Rect;f4;f4;f4;f4) */
		L"Windows.Foundation.IReference`1<Windows.Foundation.Rect>", L"Windows.Foundation.IReferenceArray`1<Windows.Foundation.Rect>"},
};

/* A box: its type, and the value of a scalar inline or the elements of an array, which it owns (strings and objects included). */
struct value {
	struct object base;
	INT32 type;
	union {
		BYTE bytes[16];
		HSTRING string;
	} scalar;
	BYTE *elements;
	UINT32 count;
};

static struct value *value_of(void *iface) { return (struct value *)owner(iface); }

static INT32 element_type(const struct value *self) { return self->type & ~ARRAY; }

/* Strings and objects held by an element block are released with it. */
static void release_elements(INT32 type, BYTE *elements, UINT32 count)
{
	for (UINT32 index = 0; elements && index < count; ++index) {
		void *element = ((void **)elements)[index];
		if (type == type_string)
			WindowsDeleteString(element);
		else if (type == type_inspectable && element)
			release(element);
	}
}

static void destroy_value(struct object *base)
{
	struct value *self = (struct value *)base;
	if (self->type == type_string)
		WindowsDeleteString(self->scalar.string);
	release_elements(element_type(self), self->elements, self->count);
	FREE(self->elements);
}

/* Copies count elements of a type, taking a reference to each string and object; into is zeroed on failure. */
static HRESULT copy_elements(INT32 type, BYTE *into, const BYTE *from, UINT32 count)
{
	if (type != type_string && type != type_inspectable) {
		memcpy(into, from, (size_t)count * types[type].size);
		return S_OK;
	}
	for (UINT32 index = 0; index < count; ++index) {
		void *element = ((void *const *)from)[index];
		if (type == type_inspectable) {
			if (element)
				reference(element);
			((void **)into)[index] = element;
		} else if (FAILED(WindowsDuplicateString(element, &((HSTRING *)into)[index]))) {
			release_elements(type, into, index);
			memset(into, 0, (size_t)count * sizeof(void *));
			return E_OUTOFMEMORY;
		}
	}
	return S_OK;
}

/* The number a numeric box holds: an integer with its sign, or a floating value. */
struct number {
	BOOL floating, negative;
	UINT64 magnitude;
	double real;
};

static BOOL number_of(const struct value *self, struct number *out)
{
	const BYTE *bytes = self->scalar.bytes;
	INT64 signed_value = 0;
	memset(out, 0, sizeof *out);
	switch (self->type) {
	case type_uint8: out->magnitude = bytes[0]; return TRUE;
	case type_uint16: out->magnitude = *(const UINT16 *)bytes; return TRUE;
	case type_uint32: out->magnitude = *(const UINT32 *)bytes; return TRUE;
	case type_uint64: out->magnitude = *(const UINT64 *)bytes; return TRUE;
	case type_int16: signed_value = *(const INT16 *)bytes; break;
	case type_int32: signed_value = *(const INT32 *)bytes; break;
	case type_int64: signed_value = *(const INT64 *)bytes; break;
	case type_single: out->floating = TRUE; out->real = *(const FLOAT *)bytes; return TRUE;
	case type_double: out->floating = TRUE; out->real = *(const double *)bytes; return TRUE;
	default: return FALSE;
	}
	out->negative = signed_value < 0;
	out->magnitude = out->negative ? (UINT64)0 - (UINT64)signed_value : (UINT64)signed_value;
	return TRUE;
}

/* Reads the box as an integer that spans lowest to highest (lowest as a magnitude below zero). */
static HRESULT integer(void *iface, UINT64 lowest_magnitude, UINT64 highest, BOOL *negative, UINT64 *magnitude)
{
	struct number number;
	if (!number_of(value_of(iface), &number) || number.floating)
		return TYPE_MISMATCH;
	if (number.negative ? number.magnitude > lowest_magnitude : number.magnitude > highest)
		return OVERFLOW;
	*negative = number.negative;
	*magnitude = number.magnitude;
	return S_OK;
}

#define UNSIGNED_GETTER(name, type, highest) \
	static HRESULT WINAPI name(void *iface, type *out) \
	{ \
		BOOL negative; \
		UINT64 magnitude; \
		if (!out) \
			return E_POINTER; \
		const HRESULT result = integer(iface, 0, highest, &negative, &magnitude); \
		if (SUCCEEDED(result)) \
			*out = (type)magnitude; \
		return result; \
	}
#define SIGNED_GETTER(name, type, highest) \
	static HRESULT WINAPI name(void *iface, type *out) \
	{ \
		BOOL negative; \
		UINT64 magnitude; \
		if (!out) \
			return E_POINTER; \
		const HRESULT result = integer(iface, (UINT64)(highest) + 1, highest, &negative, &magnitude); \
		if (SUCCEEDED(result)) \
			*out = negative ? (type)(0 - magnitude) : (type)magnitude; \
		return result; \
	}

UNSIGNED_GETTER(get_uint8, BYTE, 0xff)
UNSIGNED_GETTER(get_uint16, UINT16, 0xffff)
UNSIGNED_GETTER(get_uint32, UINT32, 0xffffffffu)
UNSIGNED_GETTER(get_uint64, UINT64, ~(UINT64)0)
SIGNED_GETTER(get_int16, INT16, 0x7fff)
SIGNED_GETTER(get_int32, INT32, 0x7fffffff)
SIGNED_GETTER(get_int64, INT64, 0x7fffffffffffffffull)

static HRESULT real(void *iface, double *out)
{
	struct number number;
	if (!number_of(value_of(iface), &number))
		return TYPE_MISMATCH;
	*out = number.floating ? number.real : number.negative ? -(double)number.magnitude : (double)number.magnitude;
	return S_OK;
}

static HRESULT WINAPI get_double(void *iface, double *out)
{
	if (!out)
		return E_POINTER;
	return real(iface, out);
}

static HRESULT WINAPI get_single(void *iface, FLOAT *out)
{
	double value;
	if (!out)
		return E_POINTER;
	const HRESULT result = real(iface, &value);
	if (FAILED(result))
		return result;
	/* A finite double beyond the range of a float overflows it; infinities and NaN carry over. */
	const BOOL finite = value - value == 0;
	if (finite && (value > 3.402823466e38 || value < -3.402823466e38))
		return OVERFLOW;
	*out = (FLOAT)value;
	return S_OK;
}

/* Copies the scalar of a box of exactly this type. */
static HRESULT exact(void *iface, INT32 type, void *out)
{
	const struct value *self = value_of(iface);
	if (!out)
		return E_POINTER;
	if (self->type != type)
		return TYPE_MISMATCH;
	memcpy(out, self->scalar.bytes, types[type].size);
	return S_OK;
}

static HRESULT WINAPI get_char16(void *iface, WCHAR *out) { return exact(iface, type_char16, out); }
static HRESULT WINAPI get_boolean(void *iface, boolean *out) { return exact(iface, type_boolean, out); }
static HRESULT WINAPI get_guid(void *iface, GUID *out) { return exact(iface, type_guid, out); }
static HRESULT WINAPI get_date_time(void *iface, INT64 *out) { return exact(iface, type_date_time, out); }
static HRESULT WINAPI get_time_span(void *iface, INT64 *out) { return exact(iface, type_time_span, out); }
static HRESULT WINAPI get_point(void *iface, struct point *out) { return exact(iface, type_point, out); }
static HRESULT WINAPI get_size(void *iface, struct point *out) { return exact(iface, type_size, out); }
static HRESULT WINAPI get_rect(void *iface, struct rect *out) { return exact(iface, type_rect, out); }

static HRESULT WINAPI get_string(void *iface, HSTRING *out)
{
	const struct value *self = value_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->type != type_string)
		return TYPE_MISMATCH;
	return WindowsDuplicateString(self->scalar.string, out);
}

static HRESULT WINAPI value_type(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = value_of(iface)->type;
	return S_OK;
}

static HRESULT WINAPI value_numeric(void *iface, boolean *out)
{
	const INT32 type = value_of(iface)->type;
	return answer_flag(out, type >= type_uint8 && type <= type_double);
}

/* A copy of an array box of exactly this type, allocated with CoTaskMemAlloc; none for an empty array. */
static HRESULT array_copy(void *iface, INT32 type, UINT32 *count, void **out)
{
	const struct value *self = value_of(iface);
	if (!count || !out)
		return E_POINTER;
	*count = 0;
	*out = NULL;
	if (self->type != (type | ARRAY))
		return TYPE_MISMATCH;
	if (!self->count)
		return S_OK;
	BYTE *copy = CoTaskMemAlloc((size_t)self->count * types[type].size);
	if (!copy)
		return E_OUTOFMEMORY;
	const HRESULT result = copy_elements(type, copy, self->elements, self->count);
	if (FAILED(result)) {
		CoTaskMemFree(copy);
		return result;
	}
	*count = self->count;
	*out = copy;
	return S_OK;
}

#define ARRAY_GETTER(name, type) \
	static HRESULT WINAPI name(void *iface, UINT32 *count, void **out) { return array_copy(iface, type, count, out); }

ARRAY_GETTER(get_uint8_array, type_uint8)
ARRAY_GETTER(get_int16_array, type_int16)
ARRAY_GETTER(get_uint16_array, type_uint16)
ARRAY_GETTER(get_int32_array, type_int32)
ARRAY_GETTER(get_uint32_array, type_uint32)
ARRAY_GETTER(get_int64_array, type_int64)
ARRAY_GETTER(get_uint64_array, type_uint64)
ARRAY_GETTER(get_single_array, type_single)
ARRAY_GETTER(get_double_array, type_double)
ARRAY_GETTER(get_char16_array, type_char16)
ARRAY_GETTER(get_boolean_array, type_boolean)
ARRAY_GETTER(get_string_array, type_string)
ARRAY_GETTER(get_inspectable_array, type_inspectable)
ARRAY_GETTER(get_guid_array, type_guid)
ARRAY_GETTER(get_date_time_array, type_date_time)
ARRAY_GETTER(get_time_span_array, type_time_span)
ARRAY_GETTER(get_point_array, type_point)
ARRAY_GETTER(get_size_array, type_size)
ARRAY_GETTER(get_rect_array, type_rect)

/* IReference<T>.Value: the scalar as its own type. */
static HRESULT WINAPI reference_value(void *iface, void *out)
{
	const struct value *self = value_of(iface);
	if (self->type == type_string)
		return get_string(iface, out);
	return exact(iface, self->type, out);
}

/* IReferenceArray<T>.Value: a copy of the elements. */
static HRESULT WINAPI reference_array_value(void *iface, UINT32 *count, void **out)
{
	return array_copy(iface, element_type(value_of(iface)), count, out);
}

TABLE(value_table, 0, INSPECTABLE, (const void *)value_type, (const void *)value_numeric, (const void *)get_uint8, (const void *)get_int16,
	(const void *)get_uint16, (const void *)get_int32, (const void *)get_uint32, (const void *)get_int64, (const void *)get_uint64,
	(const void *)get_single, (const void *)get_double, (const void *)get_char16, (const void *)get_boolean, (const void *)get_string,
	(const void *)get_guid, (const void *)get_date_time, (const void *)get_time_span, (const void *)get_point, (const void *)get_size,
	(const void *)get_rect, (const void *)get_uint8_array, (const void *)get_int16_array, (const void *)get_uint16_array,
	(const void *)get_int32_array, (const void *)get_uint32_array, (const void *)get_int64_array, (const void *)get_uint64_array,
	(const void *)get_single_array, (const void *)get_double_array, (const void *)get_char16_array, (const void *)get_boolean_array,
	(const void *)get_string_array, (const void *)get_inspectable_array, (const void *)get_guid_array, (const void *)get_date_time_array,
	(const void *)get_time_span_array, (const void *)get_point_array, (const void *)get_size_array, (const void *)get_rect_array);
TABLE(reference_table, 1, INSPECTABLE, (const void *)reference_value);
TABLE(reference_array_table, 1, INSPECTABLE, (const void *)reference_array_value);

/* The kinds of boxes, one for each scalar type and one for an array of each: IPropertyValue at place 0, the reference at place 1. */
static struct face_iid scalar_iids[SCALARS + 1][3], array_iids[SCALARS + 1][3];
static struct kind scalar_kinds[SCALARS + 1], array_kinds[SCALARS + 1];
static INIT_ONCE kinds_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK make_kinds(INIT_ONCE *once, void *parameter, void **context)
{
	for (INT32 type = type_uint8; type <= SCALARS; ++type) {
		scalar_iids[type][0] = (struct face_iid){&iid_property_value, 0};
		scalar_iids[type][1] = (struct face_iid){&types[type].reference, 1};
		array_iids[type][0] = (struct face_iid){&iid_property_value, 0};
		array_iids[type][1] = (struct face_iid){&types[type].reference_array, 1};
		scalar_kinds[type] = (struct kind){"PropertyValue", types[type].name, scalar_iids[type], destroy_value};
		array_kinds[type] = (struct kind){"PropertyValue", types[type].array_name, array_iids[type], destroy_value};
	}
	return TRUE;
}

static struct value *new_value(INT32 type, void **out)
{
	static const void *const *const scalar_tables[] = {value_table.slots, reference_table.slots};
	static const void *const *const array_tables[] = {value_table.slots, reference_array_table.slots};
	InitOnceExecuteOnce(&kinds_once, make_kinds, NULL, NULL);
	*out = NULL;
	const BOOL array = (type & ARRAY) != 0;
	struct value *self = make(sizeof *self, array ? &array_kinds[type & ~ARRAY] : &scalar_kinds[type], array ? array_tables : scalar_tables, 2);
	if (self)
		self->type = type;
	return self;
}

static HRESULT box(INT32 type, const void *value, void **out)
{
	if (!out)
		return E_POINTER;
	struct value *self = new_value(type, out);
	if (!self)
		return E_OUTOFMEMORY;
	if (type == type_string) {
		if (FAILED(WindowsDuplicateString(*(const HSTRING *)value, &self->scalar.string))) {
			release(face_of(&self->base, 0));
			return E_OUTOFMEMORY;
		}
	} else {
		memcpy(self->scalar.bytes, value, types[type].size);
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT box_array(INT32 type, UINT32 count, const void *values, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (count && !values)
		return E_POINTER;
	struct value *self = new_value(type | ARRAY, out);
	if (!self)
		return E_OUTOFMEMORY;
	if (count) {
		self->elements = ALLOC((size_t)count * types[type].size);
		if (!self->elements || FAILED(copy_elements(type, self->elements, values, count))) {
			release(face_of(&self->base, 0));
			return E_OUTOFMEMORY;
		}
		self->count = count;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI create_empty(void *iface, void **out) { return answer_null(out); }
static HRESULT WINAPI create_uint8(void *iface, BYTE value, void **out) { return box(type_uint8, &value, out); }
static HRESULT WINAPI create_int16(void *iface, INT16 value, void **out) { return box(type_int16, &value, out); }
static HRESULT WINAPI create_uint16(void *iface, UINT16 value, void **out) { return box(type_uint16, &value, out); }
static HRESULT WINAPI create_int32(void *iface, INT32 value, void **out) { return box(type_int32, &value, out); }
static HRESULT WINAPI create_uint32(void *iface, UINT32 value, void **out) { return box(type_uint32, &value, out); }
static HRESULT WINAPI create_int64(void *iface, INT64 value, void **out) { return box(type_int64, &value, out); }
static HRESULT WINAPI create_uint64(void *iface, UINT64 value, void **out) { return box(type_uint64, &value, out); }
static HRESULT WINAPI create_single(void *iface, FLOAT value, void **out) { return box(type_single, &value, out); }
static HRESULT WINAPI create_double(void *iface, double value, void **out) { return box(type_double, &value, out); }
static HRESULT WINAPI create_char16(void *iface, WCHAR value, void **out) { return box(type_char16, &value, out); }
static HRESULT WINAPI create_boolean(void *iface, boolean value, void **out) { return box(type_boolean, &value, out); }
static HRESULT WINAPI create_string(void *iface, HSTRING value, void **out) { return box(type_string, &value, out); }
static HRESULT WINAPI create_guid(void *iface, GUID value, void **out) { return box(type_guid, &value, out); }
static HRESULT WINAPI create_date_time(void *iface, INT64 value, void **out) { return box(type_date_time, &value, out); }
static HRESULT WINAPI create_time_span(void *iface, INT64 value, void **out) { return box(type_time_span, &value, out); }
static HRESULT WINAPI create_point(void *iface, struct point value, void **out) { return box(type_point, &value, out); }
static HRESULT WINAPI create_size(void *iface, struct point value, void **out) { return box(type_size, &value, out); }
static HRESULT WINAPI create_rect(void *iface, struct rect value, void **out) { return box(type_rect, &value, out); }

static HRESULT WINAPI create_inspectable(void *iface, IInspectable *value, void **out)
{
	if (!out)
		return E_POINTER;
	if (value)
		reference(value);
	*out = value;
	return S_OK;
}

#define ARRAY_CREATOR(name, type) \
	static HRESULT WINAPI name(void *iface, UINT32 count, const void *values, void **out) { return box_array(type, count, values, out); }

ARRAY_CREATOR(create_uint8_array, type_uint8)
ARRAY_CREATOR(create_int16_array, type_int16)
ARRAY_CREATOR(create_uint16_array, type_uint16)
ARRAY_CREATOR(create_int32_array, type_int32)
ARRAY_CREATOR(create_uint32_array, type_uint32)
ARRAY_CREATOR(create_int64_array, type_int64)
ARRAY_CREATOR(create_uint64_array, type_uint64)
ARRAY_CREATOR(create_single_array, type_single)
ARRAY_CREATOR(create_double_array, type_double)
ARRAY_CREATOR(create_char16_array, type_char16)
ARRAY_CREATOR(create_boolean_array, type_boolean)
ARRAY_CREATOR(create_string_array, type_string)
ARRAY_CREATOR(create_inspectable_array, type_inspectable)
ARRAY_CREATOR(create_guid_array, type_guid)
ARRAY_CREATOR(create_date_time_array, type_date_time)
ARRAY_CREATOR(create_time_span_array, type_time_span)
ARRAY_CREATOR(create_point_array, type_point)
ARRAY_CREATOR(create_size_array, type_size)
ARRAY_CREATOR(create_rect_array, type_rect)

TABLE(value_factory_table, 0, FACTORY);
TABLE(value_statics_table, 1, INSPECTABLE, (const void *)create_empty, (const void *)create_uint8, (const void *)create_int16,
	(const void *)create_uint16, (const void *)create_int32, (const void *)create_uint32, (const void *)create_int64, (const void *)create_uint64,
	(const void *)create_single, (const void *)create_double, (const void *)create_char16, (const void *)create_boolean,
	(const void *)create_string, (const void *)create_inspectable, (const void *)create_guid, (const void *)create_date_time,
	(const void *)create_time_span, (const void *)create_point, (const void *)create_size, (const void *)create_rect,
	(const void *)create_uint8_array, (const void *)create_int16_array, (const void *)create_uint16_array, (const void *)create_int32_array,
	(const void *)create_uint32_array, (const void *)create_int64_array, (const void *)create_uint64_array, (const void *)create_single_array,
	(const void *)create_double_array, (const void *)create_char16_array, (const void *)create_boolean_array, (const void *)create_string_array,
	(const void *)create_inspectable_array, (const void *)create_guid_array, (const void *)create_date_time_array,
	(const void *)create_time_span_array, (const void *)create_point_array, (const void *)create_size_array, (const void *)create_rect_array);
static const struct face_iid value_factory_iids[] = {{&iid_factory, 0}, {&iid_property_value_statics, 1}, {NULL, 0}};
static const struct kind value_factory_kind = {"PropertyValue", L"Windows.Foundation.PropertyValue", value_factory_iids, NULL};
struct object property_value_class = {{value_factory_table.slots, value_statics_table.slots}, &value_factory_kind, 1};

HRESULT property_value_make(INT32 type, const void *value, void **out)
{
	if (type < type_uint8 || type > SCALARS || type == type_inspectable)
		return E_INVALIDARG;
	return box(type, value, out);
}

HRESULT property_value_make_array(INT32 type, UINT32 count, const void *values, void **out)
{
	if (type < type_uint8 || type > SCALARS)
		return E_INVALIDARG;
	return box_array(type, count, values, out);
}

size_t property_type_size(INT32 type) { return type >= type_uint8 && type <= SCALARS ? types[type].size : 0; }

HRESULT property_value_string(IUnknown *value, HSTRING *out)
{
	IUnknown *box = NULL;
	*out = NULL;
	if (!value || FAILED(value->lpVtbl->QueryInterface(value, &iid_property_value, (void **)&box)))
		return TYPE_MISMATCH;
	const HRESULT result = ((HRESULT (WINAPI *)(void *, HSTRING *))method(box, 19))(box, out);
	release(box);
	return result;
}
