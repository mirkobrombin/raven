/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * Wire forms of BSTR, VARIANT and SAFEARRAY for generated proxies, per the OLE Automation
 * protocol. Sizing and marshalling run the same walk: without a buffer it only counts.
 */
#include "oa.h"
#include <rpcndr.h>

#define WIRE_USER_MARK 0x72657355

struct walker
{
	BYTE *at;
	BOOL write;
	ULONG *flags;
};

/* Sizing starts at an offset and marshalling at an address; alignment only looks at the low bits. */
static void walk_align(struct walker *walk, ULONG_PTR alignment)
{
	walk->at = (BYTE *)(((ULONG_PTR)walk->at + alignment - 1) & ~(alignment - 1));
}

static void walk_put(struct walker *walk, const void *data, SIZE_T size)
{
	if (walk->write && data)
		memcpy(walk->at, data, size);
	walk->at += size;
}

static void walk_dword(struct walker *walk, DWORD value)
{
	walk_align(walk, 4);
	walk_put(walk, &value, 4);
}

/* BSTR: character count, byte count (-1 for none), character count, characters. */
static void walk_bstr(struct walker *walk, BSTR value)
{
	DWORD bytes = value ? SysStringByteLen(value) : (DWORD)-1;
	DWORD chars = value ? (bytes + 1) / 2 : 0;
	walk_dword(walk, chars);
	walk_dword(walk, bytes);
	walk_dword(walk, chars);
	if (value)
		walk_put(walk, value, chars * 2);
}

static const BYTE *read_bstr(const BYTE *at, BSTR *out)
{
	const DWORD *head;
	DWORD bytes, chars;
	at = (const BYTE *)(((ULONG_PTR)at + 3) & ~(ULONG_PTR)3);
	head = (const DWORD *)at;
	bytes = head[1];
	chars = head[0];
	at += 12;
	if (bytes == (DWORD)-1)
	{
		SysFreeString(*out);
		*out = NULL;
		return at;
	}
	if (*out)
		SysReAllocStringLen(out, NULL, 0);
	if (*out && SysStringByteLen(*out) != bytes)
	{
		SysFreeString(*out);
		*out = NULL;
	}
	if (!*out)
		*out = SysAllocStringByteLen((const char *)at, bytes);
	else
		memcpy(*out, at, bytes);
	return at + chars * 2;
}

ULONG WINAPI BSTR_UserSize(ULONG *flags, ULONG start, BSTR *value)
{
	struct walker walk = {(BYTE *)(ULONG_PTR)start, FALSE, flags};
	walk_bstr(&walk, *value);
	return (ULONG)(ULONG_PTR)walk.at;
}

unsigned char *WINAPI BSTR_UserMarshal(ULONG *flags, unsigned char *buffer, BSTR *value)
{
	struct walker walk = {buffer, TRUE, flags};
	walk_bstr(&walk, *value);
	return walk.at;
}

unsigned char *WINAPI BSTR_UserUnmarshal(ULONG *flags, unsigned char *buffer, BSTR *value)
{
	return (unsigned char *)read_bstr(buffer, value);
}

void WINAPI BSTR_UserFree(ULONG *flags, BSTR *value)
{
	SysFreeString(*value);
	*value = NULL;
}

/* Interface pointers as marshalled objects: mark, length, length, object reference. */
static void walk_interface(struct walker *walk, REFIID iid, IUnknown *object)
{
	IStream *stream = NULL;
	HGLOBAL memory = NULL;
	DWORD size = 0;
	if (!object)
	{
		walk_dword(walk, 0);
		return;
	}
	walk_dword(walk, WIRE_USER_MARK);
	if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)))
	{
		walk_dword(walk, 0);
		walk_dword(walk, 0);
		return;
	}
	if (SUCCEEDED(CoMarshalInterface(stream, iid, object, LOWORD(*walk->flags), NULL, MSHLFLAGS_NORMAL)))
	{
		ULARGE_INTEGER end;
		LARGE_INTEGER zero;
		zero.QuadPart = 0;
		IStream_Seek(stream, zero, STREAM_SEEK_CUR, &end);
		size = end.LowPart;
		if (!walk->write)
			CoReleaseMarshalData(stream);
	}
	walk_dword(walk, size);
	walk_dword(walk, size);
	if (size && SUCCEEDED(GetHGlobalFromStream(stream, &memory)))
	{
		void *bytes = GlobalLock(memory);
		walk_put(walk, bytes, size);
		GlobalUnlock(memory);
	}
	IStream_Release(stream);
}

static const BYTE *read_interface(const BYTE *at, REFIID iid, IUnknown **out)
{
	DWORD size;
	IStream *stream = NULL;
	HGLOBAL memory;
	at = (const BYTE *)(((ULONG_PTR)at + 3) & ~(ULONG_PTR)3);
	if (!*(const DWORD *)at)
	{
		if (*out)
			IUnknown_Release(*out);
		*out = NULL;
		return at + 4;
	}
	size = *(const DWORD *)(at + 4);
	at += 12;
	memory = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
	if (memory)
	{
		memcpy(GlobalLock(memory), at, size);
		GlobalUnlock(memory);
		if (SUCCEEDED(CreateStreamOnHGlobal(memory, TRUE, &stream)))
		{
			IUnknown *object = NULL;
			if (SUCCEEDED(CoUnmarshalInterface(stream, iid, (void **)&object)))
			{
				if (*out)
					IUnknown_Release(*out);
				*out = object;
			}
			IStream_Release(stream);
		}
		else
			GlobalFree(memory);
	}
	return at + size;
}

static void walk_variant(struct walker *walk, VARIANT *value);
static const BYTE *read_variant(ULONG *flags, const BYTE *at, VARIANT *out);

/* Which arm of the wire union an array uses; the id doubles as an element class. */
static SF_TYPE array_class(const SAFEARRAY *array, VARTYPE type)
{
	if (array->fFeatures & FADF_HAVEIID)
		return SF_HAVEIID;
	switch (type)
	{
	case VT_I1:
	case VT_UI1:
		return SF_I1;
	case VT_BOOL:
	case VT_I2:
	case VT_UI2:
		return SF_I2;
	case VT_I4:
	case VT_UI4:
	case VT_INT:
	case VT_UINT:
	case VT_ERROR:
	case VT_R4:
		return SF_I4;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_DATE:
	case VT_CY:
		return SF_I8;
	case VT_BSTR:
		return SF_BSTR;
	case VT_VARIANT:
		return SF_VARIANT;
	case VT_DISPATCH:
		return SF_DISPATCH;
	case VT_UNKNOWN:
		return SF_UNKNOWN;
	case VT_RECORD:
		return SF_RECORD;
	default:
		return SF_ERROR;
	}
}

static ULONG array_cells(const SAFEARRAY *array)
{
	ULONG cells = 1;
	USHORT dim;
	for (dim = 0; dim < array->cDims; dim++)
		cells *= array->rgsabound[dim].cElements;
	return array->cDims ? cells : 0;
}

/* What each element is on the wire; on 64 bit this is not what it is in memory. */
static ULONG wire_element_size(const SAFEARRAY *array, SF_TYPE class)
{
	switch (class)
	{
	case SF_BSTR:
	case SF_DISPATCH:
	case SF_UNKNOWN:
	case SF_HAVEIID:
		return sizeof(DWORD);
	case SF_VARIANT:
		return 16;
	default:
		return array->cbElements;
	}
}

static void walk_array(struct walker *walk, SAFEARRAY *array)
{
	VARTYPE type = VT_EMPTY, guessed = VT_EMPTY;
	SF_TYPE class;
	ULONG cells, index;
	USHORT dim;
	GUID iid;
	if (!array)
	{
		walk_dword(walk, 0);
		return;
	}
	/* Hand-built descriptors may carry no type at all; their flags and element size decide. */
	if (FAILED(SafeArrayGetVartype(array, &type)))
		guessed = array->fFeatures & FADF_VARIANT ? VT_VARIANT : array->cbElements == 1 ? VT_I1 : array->cbElements == 2 ? VT_I2 : array->cbElements == 8 ? VT_I8 : VT_I4;
	class = array_class(array, guessed ? guessed : type);
	cells = array_cells(array);
	walk_dword(walk, WIRE_USER_MARK);
	walk_dword(walk, array->cDims);
	{
		WORD words[2] = {array->cDims, array->fFeatures};
		walk_put(walk, words, 4);
	}
	walk_dword(walk, wire_element_size(array, class));
	{
		WORD words[2] = {array->cLocks, array->fFeatures & FADF_HAVEIID ? 0 : type};
		walk_put(walk, words, 4);
	}
	walk_dword(walk, class);
	walk_dword(walk, cells);
	walk_dword(walk, array->pvData || !cells ? WIRE_USER_MARK : 0);
	if (class == SF_HAVEIID && SUCCEEDED(SafeArrayGetIID(array, &iid)))
		walk_put(walk, &iid, sizeof(iid));
	else if (class == SF_RECORD)
		walk_dword(walk, 0);
	/* Bounds go in natural order; the array stores them the other way round. */
	for (dim = array->cDims; dim-- > 0;)
		walk_put(walk, &array->rgsabound[dim], sizeof(SAFEARRAYBOUND));
	walk_dword(walk, cells);
	if (!array->pvData || !cells)
		return;
	switch (class)
	{
	case SF_BSTR:
	{
		BSTR *items = array->pvData;
		for (index = 0; index < cells; index++)
			walk_dword(walk, items[index] ? WIRE_USER_MARK : 0);
		for (index = 0; index < cells; index++)
			if (items[index])
				walk_bstr(walk, items[index]);
		break;
	}
	case SF_VARIANT:
	{
		VARIANT *items = array->pvData;
		for (index = 0; index < cells; index++)
			walk_dword(walk, WIRE_USER_MARK);
		for (index = 0; index < cells; index++)
			walk_variant(walk, &items[index]);
		walk_align(walk, 8);
		break;
	}
	case SF_DISPATCH:
	case SF_UNKNOWN:
	case SF_HAVEIID:
	{
		IUnknown **items = array->pvData;
		REFIID element = class == SF_DISPATCH ? &IID_IDispatch : class == SF_UNKNOWN ? &IID_IUnknown : &iid;
		for (index = 0; index < cells; index++)
			walk_dword(walk, items[index] ? WIRE_USER_MARK : 0);
		for (index = 0; index < cells; index++)
			if (items[index])
				walk_interface(walk, element, items[index]);
		break;
	}
	case SF_I8:
		walk_align(walk, 8);
		walk_put(walk, array->pvData, (SIZE_T)cells * array->cbElements);
		break;
	default:
		walk_align(walk, array->cbElements > 4 ? 4 : array->cbElements ? array->cbElements : 1);
		walk_put(walk, array->pvData, (SIZE_T)cells * array->cbElements);
		break;
	}
}

ULONG WINAPI LPSAFEARRAY_UserSize(ULONG *flags, ULONG start, LPSAFEARRAY *value)
{
	struct walker walk = {(BYTE *)(ULONG_PTR)start, FALSE, flags};
	walk_array(&walk, *value);
	return (ULONG)(ULONG_PTR)walk.at;
}

unsigned char *WINAPI LPSAFEARRAY_UserMarshal(ULONG *flags, unsigned char *buffer, LPSAFEARRAY *value)
{
	struct walker walk = {buffer, TRUE, flags};
	walk_array(&walk, *value);
	return walk.at;
}

static const BYTE *read_array(ULONG *flags, const BYTE *at, SAFEARRAY **out)
{
	DWORD mark, dims, cells, class_id;
	WORD features, type;
	ULONG element_size, index;
	USHORT dim;
	SAFEARRAY *array = NULL;
	SAFEARRAYBOUND *bounds;
	GUID iid;
	BOOL have_data;
	HRESULT status;
	at = (const BYTE *)(((ULONG_PTR)at + 3) & ~(ULONG_PTR)3);
	mark = *(const DWORD *)at;
	at += 4;
	if (!mark)
	{
		if (*out)
			SafeArrayDestroy(*out);
		*out = NULL;
		return at;
	}
	dims = *(const DWORD *)at;
	features = ((const WORD *)at)[3];
	element_size = *(const DWORD *)(at + 8);
	type = ((const WORD *)at)[7];
	class_id = *(const DWORD *)(at + 16);
	cells = *(const DWORD *)(at + 20);
	have_data = *(const DWORD *)(at + 24) != 0;
	at += 28;
	if (class_id == SF_HAVEIID)
	{
		memcpy(&iid, at, sizeof(iid));
		at += sizeof(iid);
	}
	else if (class_id == SF_RECORD)
		at += 4;
	bounds = (SAFEARRAYBOUND *)at;
	at += dims * sizeof(SAFEARRAYBOUND);
	at += 4;
	if (class_id == SF_HAVEIID)
		status = SafeArrayAllocDescriptorEx(VT_UNKNOWN, dims, &array);
	else
		status = SafeArrayAllocDescriptorEx(type, dims, &array);
	if (FAILED(status))
		return at;
	array->fFeatures |= features & ~(FADF_AUTO | FADF_STATIC | FADF_EMBEDDED | FADF_FIXEDSIZE);
	array->cbElements = class_id == SF_BSTR || class_id == SF_DISPATCH || class_id == SF_UNKNOWN || class_id == SF_HAVEIID ? sizeof(void *) : class_id == SF_VARIANT ? sizeof(VARIANT) : element_size;
	for (dim = 0; dim < dims; dim++)
		array->rgsabound[dims - 1 - dim] = bounds[dim];
	if (class_id == SF_HAVEIID)
		SafeArraySetIID(array, &iid);
	if (have_data)
	{
		status = SafeArrayAllocData(array);
		if (SUCCEEDED(status) && cells)
			switch (class_id)
			{
			case SF_BSTR:
			{
				BSTR *items = array->pvData;
				const DWORD *marks;
				at = (const BYTE *)(((ULONG_PTR)at + 3) & ~(ULONG_PTR)3);
				marks = (const DWORD *)at;
				at += cells * 4;
				for (index = 0; index < cells; index++)
					if (marks[index])
						at = read_bstr(at, &items[index]);
				break;
			}
			case SF_VARIANT:
			{
				VARIANT *items = array->pvData;
				at = (const BYTE *)(((ULONG_PTR)at + 3) & ~(ULONG_PTR)3);
				at += cells * 4;
				for (index = 0; index < cells; index++)
				{
					VariantInit(&items[index]);
					at = read_variant(flags, at, &items[index]);
				}
				at = (const BYTE *)(((ULONG_PTR)at + 7) & ~(ULONG_PTR)7);
				break;
			}
			case SF_DISPATCH:
			case SF_UNKNOWN:
			case SF_HAVEIID:
			{
				IUnknown **items = array->pvData;
				REFIID element = class_id == SF_DISPATCH ? &IID_IDispatch : class_id == SF_UNKNOWN ? &IID_IUnknown : &iid;
				const DWORD *marks;
				at = (const BYTE *)(((ULONG_PTR)at + 3) & ~(ULONG_PTR)3);
				marks = (const DWORD *)at;
				at += cells * 4;
				for (index = 0; index < cells; index++)
					if (marks[index])
						at = read_interface(at, element, &items[index]);
				break;
			}
			case SF_I8:
				at = (const BYTE *)(((ULONG_PTR)at + 7) & ~(ULONG_PTR)7);
				memcpy(array->pvData, at, (SIZE_T)cells * array->cbElements);
				at += (SIZE_T)cells * array->cbElements;
				break;
			default:
			{
				ULONG_PTR alignment = array->cbElements > 4 ? 4 : array->cbElements ? array->cbElements : 1;
				at = (const BYTE *)(((ULONG_PTR)at + alignment - 1) & ~(alignment - 1));
				memcpy(array->pvData, at, (SIZE_T)cells * array->cbElements);
				at += (SIZE_T)cells * array->cbElements;
				break;
			}
			}
	}
	if (*out)
		SafeArrayDestroy(*out);
	*out = array;
	return at;
}

unsigned char *WINAPI LPSAFEARRAY_UserUnmarshal(ULONG *flags, unsigned char *buffer, LPSAFEARRAY *value)
{
	return (unsigned char *)read_array(flags, buffer, value);
}

void WINAPI LPSAFEARRAY_UserFree(ULONG *flags, LPSAFEARRAY *value)
{
	SafeArrayDestroy(*value);
	*value = NULL;
}

/* The plain size of a value in the union; zero when the type carries something else. */
static ULONG plain_size(VARTYPE type)
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
	case VT_INT:
	case VT_UINT:
	case VT_R4:
	case VT_ERROR:
	case VT_HRESULT:
		return 4;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_DATE:
	case VT_CY:
		return 8;
	default:
		return 0;
	}
}

static DWORD byref_mark(VARTYPE type)
{
	ULONG size = plain_size(type & VT_TYPEMASK);
	if (type & VT_ARRAY)
		return 4;
	if (size)
		return size;
	switch (type & VT_TYPEMASK)
	{
	case VT_VARIANT:
		return sizeof(VARIANT);
	case VT_DECIMAL:
		return sizeof(DECIMAL);
	default:
		return 4;
	}
}

static void walk_variant(struct walker *walk, VARIANT *value)
{
	VARTYPE type = V_VT(value);
	BOOL by_ref = (type & VT_BYREF) != 0;
	VARTYPE base = type & ~VT_BYREF;
	void *field = by_ref ? V_BYREF(value) : (void *)&V_UI8(value);
	BYTE *start;
	DWORD head[2] = {0, 0};
	WORD words[4];
	DWORD switch_is = type & VT_ARRAY ? type & ~VT_TYPEMASK : type;
	walk_align(walk, 8);
	start = walk->at;
	walk_put(walk, head, sizeof(head));
	words[0] = type;
	words[1] = value->wReserved1;
	words[2] = value->wReserved2;
	words[3] = value->wReserved3;
	walk_put(walk, words, sizeof(words));
	walk_dword(walk, switch_is);
	if (by_ref)
		walk_dword(walk, byref_mark(base));
	if (base & VT_ARRAY)
	{
		walk_dword(walk, WIRE_USER_MARK);
		walk_array(walk, by_ref ? *(SAFEARRAY **)field : V_ARRAY(value));
	}
	else
		switch (base)
		{
		case VT_EMPTY:
		case VT_NULL:
			break;
		case VT_BSTR:
		{
			BSTR text = by_ref ? *(BSTR *)field : V_BSTR(value);
			walk_dword(walk, text ? WIRE_USER_MARK : 0);
			if (text)
				walk_bstr(walk, text);
			break;
		}
		case VT_VARIANT:
			/* Only reached by reference: a variant cannot hold a variant by value. */
			walk_dword(walk, WIRE_USER_MARK);
			walk_variant(walk, (VARIANT *)field);
			break;
		case VT_DECIMAL:
			walk_align(walk, 8);
			walk_put(walk, by_ref ? field : (void *)value, sizeof(DECIMAL));
			break;
		case VT_UNKNOWN:
			walk_interface(walk, &IID_IUnknown, by_ref ? *(IUnknown **)field : V_UNKNOWN(value));
			break;
		case VT_DISPATCH:
			walk_interface(walk, &IID_IDispatch, by_ref ? *(IUnknown **)field : (IUnknown *)V_DISPATCH(value));
			break;
		default:
		{
			ULONG size = plain_size(base);
			if (size)
			{
				walk_align(walk, size);
				walk_put(walk, field, size);
			}
			break;
		}
		}
	/* The total in eight byte units goes back into the head. */
	if (walk->write)
		*(DWORD *)start = (DWORD)(((ULONG_PTR)walk->at - (ULONG_PTR)start + 7) >> 3);
}

ULONG WINAPI VARIANT_UserSize(ULONG *flags, ULONG start, VARIANT *value)
{
	struct walker walk = {(BYTE *)(ULONG_PTR)start, FALSE, flags};
	walk_variant(&walk, value);
	return (ULONG)(ULONG_PTR)walk.at;
}

unsigned char *WINAPI VARIANT_UserMarshal(ULONG *flags, unsigned char *buffer, VARIANT *value)
{
	struct walker walk = {buffer, TRUE, flags};
	walk_variant(&walk, value);
	return walk.at;
}

static const BYTE *read_variant(ULONG *flags, const BYTE *at, VARIANT *out)
{
	const WORD *words;
	VARTYPE type, base, old_type = V_VT(out);
	BOOL by_ref;
	void *keep = old_type & VT_BYREF ? V_BYREF(out) : NULL;
	void *field;
	at = (const BYTE *)(((ULONG_PTR)at + 7) & ~(ULONG_PTR)7);
	words = (const WORD *)(at + 8);
	type = words[0];
	base = type & ~VT_BYREF;
	by_ref = (type & VT_BYREF) != 0;
	at += 20;
	/* A by-reference target with the right type is reused, as an in/out parameter must be. */
	if (old_type != type)
	{
		if (old_type & VT_BYREF)
		{
			CoTaskMemFree(keep);
			V_VT(out) = VT_EMPTY;
		}
		else
			VariantClear(out);
		keep = NULL;
	}
	else if (!by_ref)
		VariantClear(out);
	V_VT(out) = type;
	out->wReserved1 = words[1];
	out->wReserved2 = words[2];
	out->wReserved3 = words[3];
	if (by_ref)
	{
		SIZE_T room = base == VT_VARIANT ? sizeof(VARIANT) : base == VT_DECIMAL ? sizeof(DECIMAL) : base & VT_ARRAY || base == VT_BSTR || base == VT_UNKNOWN || base == VT_DISPATCH ? sizeof(void *) : plain_size(base);
		at = (const BYTE *)(((ULONG_PTR)at + 3) & ~(ULONG_PTR)3) + 4;
		if (!keep)
		{
			keep = CoTaskMemAlloc(room ? room : 8);
			if (keep)
				memset(keep, 0, room ? room : 8);
		}
		V_BYREF(out) = keep;
		field = keep;
	}
	else
		field = &V_UI8(out);
	if (base & VT_ARRAY)
	{
		at = (const BYTE *)(((ULONG_PTR)at + 3) & ~(ULONG_PTR)3) + 4;
		if (!by_ref)
			*(SAFEARRAY **)field = NULL;
		at = read_array(flags, at, (SAFEARRAY **)field);
		return at;
	}
	switch (base)
	{
	case VT_EMPTY:
	case VT_NULL:
		break;
	case VT_BSTR:
	{
		DWORD mark;
		at = (const BYTE *)(((ULONG_PTR)at + 3) & ~(ULONG_PTR)3);
		mark = *(const DWORD *)at;
		at += 4;
		if (!by_ref)
			*(BSTR *)field = NULL;
		if (mark)
			at = read_bstr(at, (BSTR *)field);
		else if (by_ref)
		{
			SysFreeString(*(BSTR *)field);
			*(BSTR *)field = NULL;
		}
		break;
	}
	case VT_VARIANT:
		at = (const BYTE *)(((ULONG_PTR)at + 3) & ~(ULONG_PTR)3) + 4;
		at = read_variant(flags, at, (VARIANT *)field);
		break;
	case VT_DECIMAL:
		at = (const BYTE *)(((ULONG_PTR)at + 7) & ~(ULONG_PTR)7);
		memcpy(by_ref ? field : (void *)out, at, sizeof(DECIMAL));
		if (!by_ref)
			V_VT(out) = type;
		at += sizeof(DECIMAL);
		break;
	case VT_UNKNOWN:
		if (!by_ref)
			*(IUnknown **)field = NULL;
		at = read_interface(at, &IID_IUnknown, (IUnknown **)field);
		break;
	case VT_DISPATCH:
		if (!by_ref)
			*(IUnknown **)field = NULL;
		at = read_interface(at, &IID_IDispatch, (IUnknown **)field);
		break;
	default:
	{
		ULONG size = plain_size(base);
		if (size)
		{
			at = (const BYTE *)(((ULONG_PTR)at + size - 1) & ~(ULONG_PTR)(size - 1));
			memcpy(field, at, size);
			at += size;
		}
		break;
	}
	}
	return at;
}

unsigned char *WINAPI VARIANT_UserUnmarshal(ULONG *flags, unsigned char *buffer, VARIANT *value)
{
	return (unsigned char *)read_variant(flags, buffer, value);
}

void WINAPI VARIANT_UserFree(ULONG *flags, VARIANT *value)
{
	VARTYPE type = V_VT(value);
	void *pointer = type & VT_BYREF ? V_BYREF(value) : NULL;
	VariantClear(value);
	if (!(type & VT_BYREF))
		return;
	/* The clear leaves by-reference targets alone; what unmarshalling allocated is released here. */
	switch (type & ~VT_BYREF)
	{
	case VT_BSTR:
		SysFreeString(*(BSTR *)pointer);
		break;
	case VT_UNKNOWN:
	case VT_DISPATCH:
		if (*(IUnknown **)pointer)
			IUnknown_Release(*(IUnknown **)pointer);
		break;
	case VT_VARIANT:
		VariantClear((VARIANT *)pointer);
		break;
	default:
		if (type & VT_ARRAY)
			SafeArrayDestroy(*(SAFEARRAY **)pointer);
		break;
	}
	CoTaskMemFree(pointer);
}
