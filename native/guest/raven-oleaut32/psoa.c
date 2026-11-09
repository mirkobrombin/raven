/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * Interfaces a type library describes cross apartments and processes through that description: the class the registry names as
 * their ProxyStubClsid32 makes proxies that read each call's arguments the way the description lays them out and send them over the
 * channel, and stubs that rebuild them on the other side and call the object. The bytes between the two are this component's own:
 * both ends are always these proxies and stubs.
 *
 * The methods an interface inherits from IDispatch are answered by the proxy from the description itself, and its Invoke calls the
 * proxy's own methods, which travel like any other.
 */
#include "oa.h"

static const CLSID automation_marshaler = {0x00020424, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/* The slots of an interface: for each one the method the description gives it, or nothing for those of IUnknown and IDispatch. */
typedef struct {
	ITypeInfo *info;
	FUNCDESC *desc;
} method;

typedef struct {
	LONG refs;
	IID iid;
	ITypeInfo *info, *dispatch;
	UINT count;
	BOOL dispatchable;
	method *methods;
} described;

static void described_release(described *type)
{
	if (InterlockedDecrement(&type->refs))
		return;
	for (UINT slot = 0; slot < type->count; slot++)
		if (type->methods[slot].info) {
			ITypeInfo_ReleaseFuncDesc(type->methods[slot].info, type->methods[slot].desc);
			ITypeInfo_Release(type->methods[slot].info);
		}
	HeapFree(GetProcessHeap(), 0, type->methods);
	if (type->dispatch)
		ITypeInfo_Release(type->dispatch);
	ITypeInfo_Release(type->info);
	HeapFree(GetProcessHeap(), 0, type);
}

static HRESULT collect(described *type, ITypeInfo *info)
{
	TYPEATTR *attr;
	HRESULT result = ITypeInfo_GetTypeAttr(info, &attr);
	if (FAILED(result))
		return result;
	if (IsEqualIID(&attr->guid, &IID_IUnknown)) {
		ITypeInfo_ReleaseTypeAttr(info, attr);
		return S_OK;
	}
	if (IsEqualIID(&attr->guid, &IID_IDispatch)) {
		type->dispatchable = TRUE;
		ITypeInfo_ReleaseTypeAttr(info, attr);
		return S_OK;
	}
	const WORD functions = attr->cFuncs, bases = attr->cImplTypes;
	ITypeInfo_ReleaseTypeAttr(info, attr);
	if (bases) {
		HREFTYPE reference;
		ITypeInfo *base;
		result = ITypeInfo_GetRefTypeOfImplType(info, 0, &reference);
		if (SUCCEEDED(result))
			result = ITypeInfo_GetRefTypeInfo(info, reference, &base);
		if (FAILED(result))
			return result;
		result = collect(type, base);
		ITypeInfo_Release(base);
		if (FAILED(result))
			return result;
	}
	for (UINT index = 0; index < functions; index++) {
		FUNCDESC *desc;
		result = ITypeInfo_GetFuncDesc(info, index, &desc);
		if (FAILED(result))
			return result;
		const UINT slot = (USHORT)desc->oVft / sizeof(void *);
		if (slot >= type->count || type->methods[slot].info) {
			ITypeInfo_ReleaseFuncDesc(info, desc);
			continue;
		}
		ITypeInfo_AddRef(info);
		type->methods[slot].info = info;
		type->methods[slot].desc = desc;
	}
	return S_OK;
}

/* The description of an interface: HKEY_CLASSES_ROOT\Interface\{iid}\TypeLib names its library and version, the library the type. A
 * dual interface is described as dispatch and as interface; the slots come from the latter. */
static HRESULT describe(REFIID iid, described **out)
{
	WCHAR key[96], guid[40], text[64], version[16];
	LONG size = sizeof text;
	GUID library;
	ITypeLib *lib;
	ITypeInfo *info;
	TYPEATTR *attr;
	StringFromGUID2(iid, guid, 40);
	wsprintfW(key, L"Interface\\%s\\TypeLib", guid);
	if (RegQueryValueW(HKEY_CLASSES_ROOT, key, text, &size) || FAILED(CLSIDFromString(text, &library)))
		return REGDB_E_IIDNOTREG;
	HKEY handle;
	DWORD kind, length = sizeof version;
	lstrcpyW(version, L"1.0");
	if (!RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &handle)) {
		if (RegQueryValueExW(handle, L"Version", NULL, &kind, (BYTE *)version, &length) || kind != REG_SZ)
			lstrcpyW(version, L"1.0");
		RegCloseKey(handle);
	}
	WORD major = 0, minor = 0;
	const WCHAR *at = version;
	for (; *at && *at != L'.'; at++)
		major = (WORD)(major * 16 + (*at <= L'9' ? *at - L'0' : (*at | 0x20) - L'a' + 10));
	if (*at == L'.')
		for (at++; *at; at++)
			minor = (WORD)(minor * 16 + (*at <= L'9' ? *at - L'0' : (*at | 0x20) - L'a' + 10));
	HRESULT result = LoadRegTypeLib(&library, major, minor, 0, &lib);
	if (FAILED(result))
		return result;
	result = ITypeLib_GetTypeInfoOfGuid(lib, iid, &info);
	ITypeLib_Release(lib);
	if (FAILED(result))
		return result;
	described *type = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *type);
	if (!type) {
		ITypeInfo_Release(info);
		return E_OUTOFMEMORY;
	}
	type->refs = 1;
	type->iid = *iid;
	type->info = info;
	result = ITypeInfo_GetTypeAttr(info, &attr);
	if (SUCCEEDED(result) && attr->typekind == TKIND_DISPATCH) {
		HREFTYPE reference;
		ITypeInfo *face;
		ITypeInfo_ReleaseTypeAttr(info, attr);
		result = ITypeInfo_GetRefTypeOfImplType(info, -1, &reference);
		if (SUCCEEDED(result))
			result = ITypeInfo_GetRefTypeInfo(info, reference, &face);
		if (FAILED(result)) {
			ITypeInfo_Release(info);
			HeapFree(GetProcessHeap(), 0, type);
			return E_NOINTERFACE;
		}
		type->dispatch = info;
		type->info = face;
		result = ITypeInfo_GetTypeAttr(face, &attr);
	}
	if (FAILED(result)) {
		type->refs = 1;
		described_release(type);
		return result;
	}
	type->count = attr->cbSizeVft / sizeof(void *);
	ITypeInfo_ReleaseTypeAttr(type->info, attr);
	type->methods = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (type->count + 1) * sizeof *type->methods);
	result = type->methods ? collect(type, type->info) : E_OUTOFMEMORY;
	if (FAILED(result)) {
		described_release(type);
		return result;
	}
	if (type->dispatchable && !type->dispatch) {
		type->dispatch = type->info;
		ITypeInfo_AddRef(type->dispatch);
	}
	*out = type;
	return S_OK;
}

/* The bytes of a call. */
typedef struct {
	BYTE *data;
	DWORD size, room;
	HRESULT failed;
} writer;

typedef struct {
	const BYTE *data;
	DWORD size, at;
	HRESULT failed;
} reader;

static void put(writer *out, const void *data, DWORD size)
{
	if (out->failed)
		return;
	if (out->size + size > out->room) {
		DWORD room = out->room ? out->room * 2 : 256;
		while (room < out->size + size)
			room *= 2;
		BYTE *grown = out->data ? HeapReAlloc(GetProcessHeap(), 0, out->data, room) : HeapAlloc(GetProcessHeap(), 0, room);
		if (!grown) {
			out->failed = E_OUTOFMEMORY;
			return;
		}
		out->data = grown;
		out->room = room;
	}
	CopyMemory(out->data + out->size, data, size);
	out->size += size;
}

static void put_word(writer *out, DWORD value)
{
	put(out, &value, sizeof value);
}

static const void *get(reader *in, DWORD size)
{
	if (in->failed || in->size - in->at < size) {
		in->failed = RPC_E_INVALID_DATA;
		return NULL;
	}
	const void *data = in->data + in->at;
	in->at += size;
	return data;
}

static DWORD get_word(reader *in)
{
	const DWORD *value = get(in, sizeof(DWORD));
	return value ? *value : 0;
}

/* What a type is once aliases and enumerations are followed: a variant type, with the interface or record it names. */
typedef struct {
	VARTYPE vt;
	ITypeInfo *info;
	IID iid;
	ULONG size;
} resolved;

static void resolved_clear(resolved *type)
{
	if (type->info)
		ITypeInfo_Release(type->info);
	type->info = NULL;
}

static HRESULT resolve(ITypeInfo *info, const TYPEDESC *desc, resolved *out)
{
	ZeroMemory(out, sizeof *out);
	if (desc->vt != VT_USERDEFINED) {
		out->vt = desc->vt;
		return S_OK;
	}
	ITypeInfo *named;
	TYPEATTR *attr;
	HRESULT result = ITypeInfo_GetRefTypeInfo(info, desc->hreftype, &named);
	if (FAILED(result))
		return result;
	result = ITypeInfo_GetTypeAttr(named, &attr);
	if (FAILED(result)) {
		ITypeInfo_Release(named);
		return result;
	}
	switch (attr->typekind) {
	case TKIND_ALIAS: {
		TYPEDESC alias = attr->tdescAlias;
		result = resolve(named, &alias, out);
		break;
	}
	case TKIND_ENUM:
		out->vt = VT_I4;
		break;
	case TKIND_INTERFACE:
	case TKIND_DISPATCH:
	case TKIND_COCLASS:
		out->vt = VT_UNKNOWN;
		out->iid = attr->guid;
		if (attr->typekind == TKIND_COCLASS)
			out->iid = IID_IUnknown;
		break;
	case TKIND_RECORD:
		out->vt = VT_RECORD;
		out->size = attr->cbSizeInstance;
		out->info = named;
		ITypeInfo_AddRef(named);
		break;
	default:
		result = DISP_E_BADVARTYPE;
	}
	ITypeInfo_ReleaseTypeAttr(named, attr);
	ITypeInfo_Release(named);
	return result;
}

/* The size a value of the type takes in memory. */
static ULONG value_size(VARTYPE vt, ULONG record)
{
	switch (vt) {
	case VT_I1: case VT_UI1:
		return 1;
	case VT_I2: case VT_UI2: case VT_BOOL:
		return 2;
	case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_ERROR: case VT_HRESULT: case VT_R4:
		return 4;
	case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE:
		return 8;
	case VT_VARIANT:
		return sizeof(VARIANT);
	case VT_DECIMAL:
		return sizeof(DECIMAL);
	case VT_RECORD:
		return record;
	default:
		return sizeof(void *);
	}
}

static void put_interface(writer *out, IUnknown *object, REFIID iid)
{
	IStream *stream;
	HGLOBAL global;
	if (!object) {
		put_word(out, 0);
		return;
	}
	HRESULT result = CreateStreamOnHGlobal(NULL, TRUE, &stream);
	if (SUCCEEDED(result)) {
		result = CoMarshalInterface(stream, iid, object, MSHCTX_LOCAL, NULL, MSHLFLAGS_NORMAL);
		if (SUCCEEDED(result))
			result = GetHGlobalFromStream(stream, &global);
		if (SUCCEEDED(result)) {
			ULARGE_INTEGER end;
			LARGE_INTEGER none = {0};
			IStream_Seek(stream, none, STREAM_SEEK_CUR, &end);
			put_word(out, end.LowPart);
			put(out, GlobalLock(global), end.LowPart);
			GlobalUnlock(global);
		}
		IStream_Release(stream);
	}
	if (FAILED(result) && !out->failed)
		out->failed = result;
}

static IUnknown *get_interface(reader *in, REFIID iid)
{
	IStream *stream;
	IUnknown *object = NULL;
	const DWORD size = get_word(in);
	if (!size)
		return NULL;
	const void *data = get(in, size);
	if (!data)
		return NULL;
	HRESULT result = CreateStreamOnHGlobal(NULL, TRUE, &stream);
	if (SUCCEEDED(result)) {
		LARGE_INTEGER start = {0};
		IStream_Write(stream, data, size, NULL);
		IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
		result = CoUnmarshalInterface(stream, iid, (void **)&object);
		IStream_Release(stream);
	}
	if (FAILED(result) && !in->failed)
		in->failed = result;
	return object;
}

static void put_value(writer *out, ITypeInfo *info, const TYPEDESC *desc, const void *data);
static void get_value(reader *in, ITypeInfo *info, const TYPEDESC *desc, void *data);
static void put_variant(writer *out, const VARIANT *value);
static void get_variant(reader *in, VARIANT *value);

static void put_string(writer *out, BSTR text)
{
	if (!text) {
		put_word(out, ~0u);
		return;
	}
	const DWORD size = SysStringByteLen(text);
	put_word(out, size);
	put(out, text, size);
}

static BSTR get_string(reader *in)
{
	const DWORD size = get_word(in);
	if (size == ~0u)
		return NULL;
	const void *data = get(in, size);
	return data ? SysAllocStringByteLen(data, size) : NULL;
}

/* A safe array: its element type, its bounds, then each element in the order of its memory. */
static void put_array(writer *out, SAFEARRAY *array)
{
	VARTYPE vt;
	if (!array) {
		put_word(out, 0);
		return;
	}
	if (FAILED(SafeArrayGetVartype(array, &vt)) || array->cDims == 0) {
		out->failed = DISP_E_BADVARTYPE;
		return;
	}
	put_word(out, array->cDims);
	put_word(out, vt);
	ULONG total = 1;
	for (USHORT dimension = 0; dimension < array->cDims; dimension++) {
		put_word(out, array->rgsabound[dimension].cElements);
		put_word(out, (DWORD)array->rgsabound[dimension].lLbound);
		total *= array->rgsabound[dimension].cElements;
	}
	const BYTE *elements = array->pvData;
	for (ULONG index = 0; index < total && !out->failed; index++) {
		const void *element = elements + index * array->cbElements;
		if (vt == VT_BSTR)
			put_string(out, *(BSTR *)element);
		else if (vt == VT_VARIANT)
			put_variant(out, element);
		else if (vt == VT_UNKNOWN || vt == VT_DISPATCH)
			put_interface(out, *(IUnknown **)element, vt == VT_UNKNOWN ? &IID_IUnknown : &IID_IDispatch);
		else if (vt == VT_RECORD)
			out->failed = DISP_E_BADVARTYPE;
		else
			put(out, element, array->cbElements);
	}
}

static SAFEARRAY *get_array(reader *in)
{
	SAFEARRAYBOUND bounds[8];
	const DWORD dimensions = get_word(in);
	if (!dimensions)
		return NULL;
	const VARTYPE vt = (VARTYPE)get_word(in);
	if (dimensions > ARRAYSIZE(bounds)) {
		in->failed = RPC_E_INVALID_DATA;
		return NULL;
	}
	ULONG total = 1;
	for (DWORD dimension = 0; dimension < dimensions; dimension++) {
		bounds[dimension].cElements = get_word(in);
		bounds[dimension].lLbound = (LONG)get_word(in);
		total *= bounds[dimension].cElements;
	}
	if (in->failed)
		return NULL;
	SAFEARRAY *array = SafeArrayCreate(vt, dimensions, bounds);
	if (!array) {
		in->failed = E_OUTOFMEMORY;
		return NULL;
	}
	BYTE *elements = array->pvData;
	for (ULONG index = 0; index < total && !in->failed; index++) {
		void *element = elements + index * array->cbElements;
		if (vt == VT_BSTR)
			*(BSTR *)element = get_string(in);
		else if (vt == VT_VARIANT)
			get_variant(in, element);
		else if (vt == VT_UNKNOWN || vt == VT_DISPATCH)
			*(IUnknown **)element = get_interface(in, vt == VT_UNKNOWN ? &IID_IUnknown : &IID_IDispatch);
		else {
			const void *data = get(in, array->cbElements);
			if (data)
				CopyMemory(element, data, array->cbElements);
		}
	}
	return array;
}

/* A variant: its type, then its value. A variant that refers to storage of its caller cannot cross. */
static void put_variant(writer *out, const VARIANT *value)
{
	const VARTYPE vt = V_VT(value);
	put_word(out, vt);
	if (vt & VT_ARRAY) {
		put_array(out, V_ARRAY(value));
		return;
	}
	switch (vt) {
	case VT_EMPTY: case VT_NULL:
		return;
	case VT_BSTR:
		put_string(out, V_BSTR(value));
		return;
	case VT_UNKNOWN:
		put_interface(out, V_UNKNOWN(value), &IID_IUnknown);
		return;
	case VT_DISPATCH:
		put_interface(out, (IUnknown *)V_DISPATCH(value), &IID_IDispatch);
		return;
	case VT_DECIMAL:
		put(out, &V_DECIMAL(value), sizeof(DECIMAL));
		return;
	case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_BOOL: case VT_I4: case VT_UI4: case VT_INT: case VT_UINT:
	case VT_ERROR: case VT_R4: case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE:
		put(out, &V_UI8(value), value_size(vt, 0));
		return;
	}
	out->failed = DISP_E_BADVARTYPE;
}

static void get_variant(reader *in, VARIANT *value)
{
	VariantInit(value);
	const VARTYPE vt = (VARTYPE)get_word(in);
	if (in->failed)
		return;
	if (vt & VT_ARRAY) {
		V_ARRAY(value) = get_array(in);
		V_VT(value) = vt;
		return;
	}
	switch (vt) {
	case VT_EMPTY: case VT_NULL:
		break;
	case VT_BSTR:
		V_BSTR(value) = get_string(in);
		break;
	case VT_UNKNOWN:
		V_UNKNOWN(value) = get_interface(in, &IID_IUnknown);
		break;
	case VT_DISPATCH:
		V_DISPATCH(value) = (IDispatch *)get_interface(in, &IID_IDispatch);
		break;
	case VT_DECIMAL: {
		const void *data = get(in, sizeof(DECIMAL));
		if (data)
			CopyMemory(&V_DECIMAL(value), data, sizeof(DECIMAL));
		break;
	}
	default: {
		const ULONG size = value_size(vt, 0);
		const void *data = size <= 8 ? get(in, size) : NULL;
		if (!data) {
			in->failed = RPC_E_INVALID_DATA;
			return;
		}
		CopyMemory(&V_UI8(value), data, size);
	}
	}
	V_VT(value) = vt;
}

/* A record crosses as its bytes when none of its fields points anywhere. */
static BOOL plain_record(ITypeInfo *info)
{
	TYPEATTR *attr;
	if (FAILED(ITypeInfo_GetTypeAttr(info, &attr)))
		return FALSE;
	BOOL plain = TRUE;
	for (WORD index = 0; index < attr->cVars && plain; index++) {
		VARDESC *desc;
		resolved field;
		if (FAILED(ITypeInfo_GetVarDesc(info, index, &desc)))
			plain = FALSE;
		else {
			if (FAILED(resolve(info, &desc->elemdescVar.tdesc, &field)))
				plain = FALSE;
			else {
				if (field.vt == VT_RECORD)
					plain = plain_record(field.info);
				else if (field.vt != VT_CARRAY && value_size(field.vt, 0) == sizeof(void *) && field.vt != VT_I8 && field.vt != VT_UI8 &&
					field.vt != VT_R8 && field.vt != VT_CY && field.vt != VT_DATE && field.vt != VT_I4 && field.vt != VT_UI4 &&
					field.vt != VT_INT && field.vt != VT_UINT && field.vt != VT_ERROR && field.vt != VT_R4)
					plain = FALSE;
				resolved_clear(&field);
			}
			ITypeInfo_ReleaseVarDesc(info, desc);
		}
	}
	ITypeInfo_ReleaseTypeAttr(info, attr);
	return plain;
}

/* One value of a described type, read from where it lies in memory. A pointer crosses as what it points to. */
static void put_value(writer *out, ITypeInfo *info, const TYPEDESC *desc, const void *data)
{
	resolved type;
	if (out->failed)
		return;
	if (desc->vt == VT_PTR) {
		resolved target;
		HRESULT result = resolve(info, desc->lptdesc, &target);
		if (FAILED(result)) {
			out->failed = result;
			return;
		}
		/* A pointer to an interface is the interface itself. */
		if (target.vt == VT_UNKNOWN && desc->lptdesc->vt == VT_USERDEFINED)
			put_interface(out, *(IUnknown **)data, &target.iid);
		else {
			const void *pointed = *(void *const *)data;
			put_word(out, pointed != NULL);
			if (pointed)
				put_value(out, info, desc->lptdesc, pointed);
		}
		resolved_clear(&target);
		return;
	}
	HRESULT result = resolve(info, desc, &type);
	if (FAILED(result)) {
		out->failed = result;
		return;
	}
	switch (type.vt) {
	case VT_BSTR:
		put_string(out, *(BSTR *)data);
		break;
	case VT_LPWSTR: {
		const WCHAR *text = *(const WCHAR *const *)data;
		put_word(out, text ? (DWORD)lstrlenW(text) : ~0u);
		if (text)
			put(out, text, lstrlenW(text) * sizeof(WCHAR));
		break;
	}
	case VT_LPSTR: {
		const char *text = *(const char *const *)data;
		put_word(out, text ? (DWORD)lstrlenA(text) : ~0u);
		if (text)
			put(out, text, lstrlenA(text));
		break;
	}
	case VT_VARIANT:
		put_variant(out, data);
		break;
	case VT_SAFEARRAY:
		put_array(out, *(SAFEARRAY **)data);
		break;
	case VT_UNKNOWN:
		put_interface(out, *(IUnknown **)data, IsEqualIID(&type.iid, &GUID_NULL) ? &IID_IUnknown : &type.iid);
		break;
	case VT_DISPATCH:
		put_interface(out, *(IUnknown **)data, &IID_IDispatch);
		break;
	case VT_RECORD:
		if (plain_record(type.info))
			put(out, data, type.size);
		else
			out->failed = DISP_E_BADVARTYPE;
		break;
	case VT_VOID: case VT_CARRAY: case VT_USERDEFINED:
		out->failed = DISP_E_BADVARTYPE;
		break;
	default:
		put(out, data, value_size(type.vt, 0));
	}
	resolved_clear(&type);
}

/* The memory a pointer of the stub points to for a value read: zeroed, released with the call. */
typedef struct block {
	struct block *next;
} block;

static void *block_take(block **blocks, SIZE_T size)
{
	block *made = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(block) + (size < 16 ? 16 : size));
	if (!made)
		return NULL;
	made->next = *blocks;
	*blocks = made;
	return made + 1;
}

static block *call_blocks;

static void get_value(reader *in, ITypeInfo *info, const TYPEDESC *desc, void *data)
{
	resolved type;
	if (in->failed)
		return;
	if (desc->vt == VT_PTR) {
		resolved target;
		HRESULT result = resolve(info, desc->lptdesc, &target);
		if (FAILED(result)) {
			in->failed = result;
			return;
		}
		if (target.vt == VT_UNKNOWN && desc->lptdesc->vt == VT_USERDEFINED)
			*(IUnknown **)data = get_interface(in, &target.iid);
		else if (get_word(in)) {
			void **pointer = data;
			/* Storage the caller already had is written over; the stub makes its own. */
			if (!*pointer)
				*pointer = block_take(&call_blocks, value_size(target.vt, target.size));
			if (*pointer)
				get_value(in, info, desc->lptdesc, *pointer);
			else
				in->failed = E_OUTOFMEMORY;
		}
		resolved_clear(&target);
		return;
	}
	HRESULT result = resolve(info, desc, &type);
	if (FAILED(result)) {
		in->failed = result;
		return;
	}
	switch (type.vt) {
	case VT_BSTR:
		*(BSTR *)data = get_string(in);
		break;
	case VT_LPWSTR: {
		const DWORD length = get_word(in);
		WCHAR *text = NULL;
		if (length != ~0u) {
			const void *source = get(in, length * sizeof(WCHAR));
			text = source ? CoTaskMemAlloc((length + 1) * sizeof(WCHAR)) : NULL;
			if (text) {
				CopyMemory(text, source, length * sizeof(WCHAR));
				text[length] = 0;
			}
		}
		*(WCHAR **)data = text;
		break;
	}
	case VT_LPSTR: {
		const DWORD length = get_word(in);
		char *text = NULL;
		if (length != ~0u) {
			const void *source = get(in, length);
			text = source ? CoTaskMemAlloc(length + 1) : NULL;
			if (text) {
				CopyMemory(text, source, length);
				text[length] = 0;
			}
		}
		*(char **)data = text;
		break;
	}
	case VT_VARIANT:
		get_variant(in, data);
		break;
	case VT_SAFEARRAY:
		*(SAFEARRAY **)data = get_array(in);
		break;
	case VT_UNKNOWN:
		*(IUnknown **)data = get_interface(in, IsEqualIID(&type.iid, &GUID_NULL) ? &IID_IUnknown : &type.iid);
		break;
	case VT_DISPATCH:
		*(IUnknown **)data = get_interface(in, &IID_IDispatch);
		break;
	case VT_RECORD: {
		const void *source = get(in, type.size);
		if (source)
			CopyMemory(data, source, type.size);
		break;
	}
	default: {
		const ULONG size = value_size(type.vt, 0);
		const void *source = get(in, size);
		if (source)
			CopyMemory(data, source, size);
	}
	}
	resolved_clear(&type);
}

/* What a value owns goes with it: strings, arrays, variants and the interfaces it holds. */
static void free_value(ITypeInfo *info, const TYPEDESC *desc, void *data)
{
	resolved type;
	if (desc->vt == VT_PTR) {
		resolved target;
		if (FAILED(resolve(info, desc->lptdesc, &target)))
			return;
		if (target.vt == VT_UNKNOWN && desc->lptdesc->vt == VT_USERDEFINED) {
			IUnknown *object = *(IUnknown **)data;
			if (object)
				IUnknown_Release(object);
			*(IUnknown **)data = NULL;
		} else if (*(void **)data)
			free_value(info, desc->lptdesc, *(void **)data);
		resolved_clear(&target);
		return;
	}
	if (FAILED(resolve(info, desc, &type)))
		return;
	switch (type.vt) {
	case VT_BSTR:
		SysFreeString(*(BSTR *)data);
		*(BSTR *)data = NULL;
		break;
	case VT_LPWSTR: case VT_LPSTR:
		CoTaskMemFree(*(void **)data);
		*(void **)data = NULL;
		break;
	case VT_VARIANT:
		VariantClear(data);
		break;
	case VT_SAFEARRAY:
		if (*(SAFEARRAY **)data)
			SafeArrayDestroy(*(SAFEARRAY **)data);
		*(SAFEARRAY **)data = NULL;
		break;
	case VT_UNKNOWN: case VT_DISPATCH:
		if (*(IUnknown **)data)
			IUnknown_Release(*(IUnknown **)data);
		*(IUnknown **)data = NULL;
		break;
	}
	resolved_clear(&type);
}

/* The proxy. Its interface is a table of thunks, one per slot, that hand the arguments as they lie on the stack to proxy_call. */
typedef struct {
	IRpcProxyBuffer buffer;
	const void *const *face;
	LONG refs;
	IUnknown *outer;
	IRpcChannelBuffer *channel;
	described *type;
	void **table;
} proxy;

static inline proxy *proxy_of_face(void *face)
{
	return CONTAINING_RECORD(face, proxy, face);
}

static BOOL is_float(ITypeInfo *info, const TYPEDESC *desc)
{
	resolved type;
	if (FAILED(resolve(info, desc, &type)))
		return FALSE;
	resolved_clear(&type);
	return type.vt == VT_R4 || type.vt == VT_R8 || type.vt == VT_DATE;
}

/* Where each argument lies. 64-bit code passes the first four in registers, which the thunk stores next to the ones on the stack, and
 * passes anything wider than a register by its address; 32-bit code pushes every argument, the wide ones whole. */
static void *argument_at(ITypeInfo *info, const TYPEDESC *desc, ULONG_PTR *arguments, double *floats, UINT position, DWORD *offset)
{
#ifdef _WIN64
	(void)offset;
	if (desc->vt != VT_PTR) {
		resolved type;
		if (SUCCEEDED(resolve(info, desc, &type))) {
			const VARTYPE vt = type.vt;
			const ULONG size = value_size(vt, type.size);
			resolved_clear(&type);
			if (vt == VT_VARIANT || vt == VT_DECIMAL || (vt == VT_RECORD && size > 8))
				return (void *)arguments[position];
		}
		if (position >= 1 && position <= 3 && is_float(info, desc))
			return &floats[position - 1];
	}
	return &arguments[position];
#else
	(void)floats, (void)position;
	ULONG size = sizeof(void *);
	if (desc->vt != VT_PTR) {
		resolved type;
		if (SUCCEEDED(resolve(info, desc, &type))) {
			size = value_size(type.vt, type.size);
			resolved_clear(&type);
		}
	}
	void *at = (BYTE *)arguments + *offset;
	*offset += (size + 3) & ~3u;
	return at;
#endif
}

HRESULT proxy_call(ULONG_PTR *arguments, DWORD slot, double *floats, DWORD *popped)
{
	proxy *self = proxy_of_face((void *)arguments[0]);
	described *type = self->type;
	DWORD offset = sizeof(void *);
	*popped = sizeof(void *);
	if (slot >= type->count || !type->methods[slot].info)
		return E_NOTIMPL;
	ITypeInfo *info = type->methods[slot].info;
	const FUNCDESC *desc = type->methods[slot].desc;
	void *places[64];
	if (desc->cParams > ARRAYSIZE(places))
		return E_NOTIMPL;
	for (SHORT index = 0; index < desc->cParams; index++)
		places[index] = argument_at(info, &desc->lprgelemdescParam[index].tdesc, arguments, floats, (UINT)index + 1, &offset);
	*popped = offset;
	if (!self->channel)
		return CO_E_OBJNOTCONNECTED;
	writer request = {0};
	for (SHORT index = 0; index < desc->cParams; index++) {
		const ELEMDESC *element = &desc->lprgelemdescParam[index];
		const USHORT flags = element->paramdesc.wParamFlags;
		if ((flags & PARAMFLAG_FIN) || !(flags & PARAMFLAG_FOUT))
			put_value(&request, info, &element->tdesc, places[index]);
	}
	if (request.failed) {
		HeapFree(GetProcessHeap(), 0, request.data);
		return request.failed;
	}
	RPCOLEMESSAGE message = {0};
	message.cbBuffer = request.size;
	message.iMethod = slot;
	HRESULT result = IRpcChannelBuffer_GetBuffer(self->channel, &message, &type->iid);
	if (SUCCEEDED(result)) {
		CopyMemory(message.Buffer, request.data, request.size);
		ULONG status = 0;
		result = IRpcChannelBuffer_SendReceive(self->channel, &message, &status);
		if (SUCCEEDED(result)) {
			reader reply = {message.Buffer, message.cbBuffer, 0, S_OK};
			result = (HRESULT)get_word(&reply);
			for (SHORT index = 0; index < desc->cParams && !reply.failed; index++) {
				const ELEMDESC *element = &desc->lprgelemdescParam[index];
				if (!(element->paramdesc.wParamFlags & PARAMFLAG_FOUT) || element->tdesc.vt != VT_PTR)
					continue;
				void *storage = *(void **)places[index];
				if (!storage)
					continue;
				/* What the caller passed in is replaced by what comes back. */
				if (element->paramdesc.wParamFlags & PARAMFLAG_FIN)
					free_value(info, element->tdesc.lptdesc, storage);
				get_value(&reply, info, element->tdesc.lptdesc, storage);
			}
			if (reply.failed)
				result = reply.failed;
		} else if (status)
			result = (HRESULT)status;
		IRpcChannelBuffer_FreeBuffer(self->channel, &message);
	}
	HeapFree(GetProcessHeap(), 0, request.data);
	return result;
}

#ifdef _WIN64
__asm__(
	".text\n"
	".balign 16\n"
	"proxy_thunks:\n"
	".set slot, 0\n"
	".rept 1024\n"
	"	mov $slot, %eax\n"
	"	jmp proxy_common\n"
	"	.balign 16\n"
	"	.set slot, slot + 1\n"
	".endr\n"
	"proxy_common:\n"
	"	mov %rcx, 8(%rsp)\n"
	"	mov %rdx, 16(%rsp)\n"
	"	mov %r8, 24(%rsp)\n"
	"	mov %r9, 32(%rsp)\n"
	"	sub $72, %rsp\n"
	"	movsd %xmm1, 40(%rsp)\n"
	"	movsd %xmm2, 48(%rsp)\n"
	"	movsd %xmm3, 56(%rsp)\n"
	"	lea 80(%rsp), %rcx\n"
	"	mov %eax, %edx\n"
	"	lea 40(%rsp), %r8\n"
	"	lea 64(%rsp), %r9\n"
	"	call proxy_call\n"
	"	add $72, %rsp\n"
	"	ret\n");
#else
__asm__(
	".text\n"
	".balign 16\n"
	"_proxy_thunks:\n"
	".set slot, 0\n"
	".rept 1024\n"
	"	mov $slot, %eax\n"
	"	jmp proxy_common\n"
	"	.balign 16\n"
	"	.set slot, slot + 1\n"
	".endr\n"
	"proxy_common:\n"
	"	lea 4(%esp), %ecx\n"
	"	push $0\n"
	"	mov %esp, %edx\n"
	"	push %edx\n"
	"	push $0\n"
	"	push %eax\n"
	"	push %ecx\n"
	"	call _proxy_call\n"
	"	add $16, %esp\n"
	"	pop %ecx\n"
	"	pop %edx\n"
	"	add %ecx, %esp\n"
	"	jmp *%edx\n");
#endif

extern const BYTE proxy_thunks[];
#define PROXY_SLOTS 1024

static HRESULT WINAPI face_query(void *face, REFIID iid, void **out)
{
	return IUnknown_QueryInterface(proxy_of_face(face)->outer, iid, out);
}

static ULONG WINAPI face_addref(void *face)
{
	return IUnknown_AddRef(proxy_of_face(face)->outer);
}

static ULONG WINAPI face_release(void *face)
{
	return IUnknown_Release(proxy_of_face(face)->outer);
}

/* IDispatch, answered from the description: Invoke calls the proxy's own slots. */
static HRESULT WINAPI face_type_count(void *face, UINT *count)
{
	if (!count)
		return E_INVALIDARG;
	*count = 1;
	return S_OK;
}

static HRESULT WINAPI face_type(void *face, UINT index, LCID locale, ITypeInfo **out)
{
	(void)locale;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (index)
		return DISP_E_BADINDEX;
	*out = proxy_of_face(face)->type->dispatch;
	ITypeInfo_AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI face_names(void *face, REFIID iid, LPOLESTR *names, UINT count, LCID locale, DISPID *identifiers)
{
	(void)iid, (void)locale;
	return DispGetIDsOfNames(proxy_of_face(face)->type->dispatch, names, count, identifiers);
}

static HRESULT WINAPI face_invoke(void *face, DISPID member, REFIID iid, LCID locale, WORD flags, DISPPARAMS *parameters, VARIANT *result,
	EXCEPINFO *exception, UINT *argument)
{
	(void)iid, (void)locale;
	return ITypeInfo_Invoke(proxy_of_face(face)->type->dispatch, face, member, flags, parameters, result, exception, argument);
}

static HRESULT WINAPI proxy_query(IRpcProxyBuffer *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IRpcProxyBuffer)) {
		*out = iface;
		IRpcProxyBuffer_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI proxy_addref(IRpcProxyBuffer *iface)
{
	return InterlockedIncrement(&CONTAINING_RECORD(iface, proxy, buffer)->refs);
}

static ULONG WINAPI proxy_release(IRpcProxyBuffer *iface)
{
	proxy *self = CONTAINING_RECORD(iface, proxy, buffer);
	const LONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	if (self->channel)
		IRpcChannelBuffer_Release(self->channel);
	described_release(self->type);
	HeapFree(GetProcessHeap(), 0, self->table);
	HeapFree(GetProcessHeap(), 0, self);
	return 0;
}

static HRESULT WINAPI proxy_connect(IRpcProxyBuffer *iface, IRpcChannelBuffer *channel)
{
	proxy *self = CONTAINING_RECORD(iface, proxy, buffer);
	if (channel)
		IRpcChannelBuffer_AddRef(channel);
	if (self->channel)
		IRpcChannelBuffer_Release(self->channel);
	self->channel = channel;
	return S_OK;
}

static void WINAPI proxy_disconnect(IRpcProxyBuffer *iface)
{
	proxy_connect(iface, NULL);
}

static const IRpcProxyBufferVtbl proxy_table = {proxy_query, proxy_addref, proxy_release, proxy_connect, proxy_disconnect};

static HRESULT create_proxy(IUnknown *outer, REFIID iid, IRpcProxyBuffer **buffer, void **out)
{
	described *type;
	*buffer = NULL;
	*out = NULL;
	HRESULT result = describe(iid, &type);
	if (FAILED(result))
		return result;
	if (type->count > PROXY_SLOTS) {
		described_release(type);
		return E_NOTIMPL;
	}
	proxy *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *self);
	void **table = HeapAlloc(GetProcessHeap(), 0, (type->count < 7 ? 7 : type->count) * sizeof(void *));
	if (!self || !table) {
		HeapFree(GetProcessHeap(), 0, self);
		HeapFree(GetProcessHeap(), 0, table);
		described_release(type);
		return E_OUTOFMEMORY;
	}
	for (UINT slot = 0; slot < type->count; slot++)
		table[slot] = (void *)(proxy_thunks + slot * 16);
	table[0] = face_query;
	table[1] = face_addref;
	table[2] = face_release;
	if (type->dispatchable) {
		table[3] = face_type_count;
		table[4] = face_type;
		table[5] = face_names;
		table[6] = face_invoke;
	}
	self->buffer.lpVtbl = (IRpcProxyBufferVtbl *)&proxy_table;
	self->face = (const void *const *)table;
	self->table = table;
	self->refs = 1;
	self->outer = outer ? outer : (IUnknown *)&self->buffer;
	self->type = type;
	*buffer = &self->buffer;
	*out = &self->face;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

/* The stub: the object's slot is called with the arguments the request carries, and the reply carries the method's result and what it
 * wrote back. */
typedef struct {
	IRpcStubBuffer iface;
	LONG refs;
	IUnknown *object;
	described *type;
} stub;

static inline stub *stub_of(IRpcStubBuffer *iface)
{
	return CONTAINING_RECORD(iface, stub, iface);
}

static HRESULT WINAPI stub_query(IRpcStubBuffer *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IRpcStubBuffer)) {
		*out = iface;
		IRpcStubBuffer_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI stub_addref(IRpcStubBuffer *iface)
{
	return InterlockedIncrement(&stub_of(iface)->refs);
}

static ULONG WINAPI stub_release(IRpcStubBuffer *iface)
{
	stub *self = stub_of(iface);
	const LONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	if (self->object)
		IUnknown_Release(self->object);
	described_release(self->type);
	HeapFree(GetProcessHeap(), 0, self);
	return 0;
}

static HRESULT WINAPI stub_connect(IRpcStubBuffer *iface, IUnknown *server)
{
	stub *self = stub_of(iface);
	IUnknown *object = NULL;
	if (server) {
		const HRESULT result = IUnknown_QueryInterface(server, &self->type->iid, (void **)&object);
		if (FAILED(result))
			return result;
	}
	if (self->object)
		IUnknown_Release(self->object);
	self->object = object;
	return S_OK;
}

static void WINAPI stub_disconnect(IRpcStubBuffer *iface)
{
	stub_connect(iface, NULL);
}

/* How the raw call passes each argument. */
static VARTYPE passed_as(ITypeInfo *info, const TYPEDESC *desc)
{
	resolved type;
	if (desc->vt == VT_PTR || FAILED(resolve(info, desc, &type)))
		return sizeof(void *) == 8 ? VT_UI8 : VT_UI4;
	const VARTYPE vt = type.vt;
	const ULONG size = value_size(vt, type.size);
	resolved_clear(&type);
	switch (vt) {
	case VT_VARIANT: case VT_DECIMAL:
		return vt;
	case VT_R8: case VT_DATE: case VT_I8: case VT_UI8: case VT_CY:
		return vt;
	case VT_R4:
		return VT_R4;
	case VT_RECORD:
		return size > 4 ? VT_I8 : VT_UI4;
	}
	return sizeof(void *) == 8 ? VT_UI8 : VT_UI4;
}

static HRESULT WINAPI stub_invoke(IRpcStubBuffer *iface, RPCOLEMESSAGE *message, IRpcChannelBuffer *channel)
{
	stub *self = stub_of(iface);
	described *type = self->type;
	const DWORD slot = message->iMethod;
	if (slot >= type->count || !type->methods[slot].info)
		return RPC_E_INVALIDMETHOD;
	if (!self->object)
		return CO_E_OBJNOTCONNECTED;
	ITypeInfo *info = type->methods[slot].info;
	const FUNCDESC *desc = type->methods[slot].desc;
	const SHORT count = desc->cParams;
	if (count > 64)
		return RPC_E_INVALIDMETHOD;
	VARIANTARG values[64], *pointers[64];
	VARTYPE types[64];
	void *storage[64];
	block *saved = call_blocks;
	call_blocks = NULL;
	reader request = {message->Buffer, message->cbBuffer, 0, S_OK};
	for (SHORT index = 0; index < count; index++) {
		const ELEMDESC *element = &desc->lprgelemdescParam[index];
		const USHORT flags = element->paramdesc.wParamFlags;
		types[index] = passed_as(info, &element->tdesc);
		pointers[index] = &values[index];
		VariantInit(&values[index]);
		V_VT(&values[index]) = types[index];
		storage[index] = NULL;
		if (types[index] == VT_VARIANT || types[index] == VT_DECIMAL) {
			if ((flags & PARAMFLAG_FIN) || !(flags & PARAMFLAG_FOUT))
				get_value(&request, info, &element->tdesc, &values[index]);
			continue;
		}
		/* An out pointer points at storage of the stub's own when the request carries none. */
		void *place = &V_UI8(&values[index]);
		if ((flags & PARAMFLAG_FIN) || !(flags & PARAMFLAG_FOUT))
			get_value(&request, info, &element->tdesc, place);
		else if (element->tdesc.vt == VT_PTR) {
			resolved target;
			if (SUCCEEDED(resolve(info, element->tdesc.lptdesc, &target))) {
				*(void **)place = block_take(&call_blocks, value_size(target.vt, target.size) < 16 ? 16 : value_size(target.vt, target.size));
				resolved_clear(&target);
			}
		}
		storage[index] = place;
	}
	writer reply = {0};
	HRESULT result = request.failed;
	if (SUCCEEDED(result)) {
		VARIANT returned;
		VariantInit(&returned);
		result = DispCallFunc(self->object, slot * sizeof(void *), CC_STDCALL, VT_I4, count, types, pointers, &returned);
		if (SUCCEEDED(result)) {
			put_word(&reply, (DWORD)V_I4(&returned));
			for (SHORT index = 0; index < count; index++) {
				const ELEMDESC *element = &desc->lprgelemdescParam[index];
				if ((element->paramdesc.wParamFlags & PARAMFLAG_FOUT) && element->tdesc.vt == VT_PTR) {
					void *pointed = *(void **)&V_UI8(&values[index]);
					if (pointed)
						put_value(&reply, info, element->tdesc.lptdesc, pointed);
					else
						put_word(&reply, 0);
				}
			}
			result = reply.failed;
		}
	}
	/* Whatever the call left behind goes: arguments, and what came back after it was sent. */
	for (SHORT index = 0; index < count; index++) {
		const ELEMDESC *element = &desc->lprgelemdescParam[index];
		if (types[index] == VT_VARIANT || types[index] == VT_DECIMAL) {
			VariantClear(&values[index]);
			continue;
		}
		if (element->tdesc.vt == VT_PTR) {
			resolved target;
			BOOL whole = FALSE;
			if (SUCCEEDED(resolve(info, element->tdesc.lptdesc, &target))) {
				whole = target.vt == VT_UNKNOWN && element->tdesc.lptdesc->vt == VT_USERDEFINED;
				resolved_clear(&target);
			}
			if (whole)
				free_value(info, &element->tdesc, &V_UI8(&values[index]));
			else if (*(void **)&V_UI8(&values[index]))
				free_value(info, element->tdesc.lptdesc, *(void **)&V_UI8(&values[index]));
		} else
			free_value(info, &element->tdesc, &V_UI8(&values[index]));
	}
	while (call_blocks) {
		block *next = call_blocks->next;
		HeapFree(GetProcessHeap(), 0, call_blocks);
		call_blocks = next;
	}
	call_blocks = saved;
	if (FAILED(result)) {
		HeapFree(GetProcessHeap(), 0, reply.data);
		return result;
	}
	message->cbBuffer = reply.size;
	result = IRpcChannelBuffer_GetBuffer(channel, message, &type->iid);
	if (SUCCEEDED(result))
		CopyMemory(message->Buffer, reply.data, reply.size);
	HeapFree(GetProcessHeap(), 0, reply.data);
	return result;
}

static IRpcStubBuffer *WINAPI stub_supported(IRpcStubBuffer *iface, REFIID iid)
{
	stub *self = stub_of(iface);
	if (!IsEqualIID(iid, &self->type->iid))
		return NULL;
	IRpcStubBuffer_AddRef(iface);
	return iface;
}

static ULONG WINAPI stub_count(IRpcStubBuffer *iface)
{
	return stub_of(iface)->object ? 1 : 0;
}

static HRESULT WINAPI stub_debug_query(IRpcStubBuffer *iface, void **out)
{
	*out = stub_of(iface)->object;
	return *out ? S_OK : E_UNEXPECTED;
}

static void WINAPI stub_debug_release(IRpcStubBuffer *iface, void *object)
{
	(void)iface, (void)object;
}

static const IRpcStubBufferVtbl stub_table = {stub_query, stub_addref, stub_release, stub_connect, stub_disconnect, stub_invoke, stub_supported,
	stub_count, stub_debug_query, stub_debug_release};

static HRESULT create_stub(REFIID iid, IUnknown *server, IRpcStubBuffer **out)
{
	described *type;
	*out = NULL;
	HRESULT result = describe(iid, &type);
	if (FAILED(result))
		return result;
	stub *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *self);
	if (!self) {
		described_release(type);
		return E_OUTOFMEMORY;
	}
	self->iface.lpVtbl = (IRpcStubBufferVtbl *)&stub_table;
	self->refs = 1;
	self->type = type;
	result = stub_connect(&self->iface, server);
	if (FAILED(result)) {
		stub_release(&self->iface);
		return result;
	}
	*out = &self->iface;
	return S_OK;
}

static HRESULT WINAPI factory_query(IPSFactoryBuffer *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IPSFactoryBuffer)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_addref(IPSFactoryBuffer *iface)
{
	(void)iface;
	return 2;
}

static ULONG WINAPI factory_release(IPSFactoryBuffer *iface)
{
	(void)iface;
	return 1;
}

static HRESULT WINAPI factory_proxy(IPSFactoryBuffer *iface, IUnknown *outer, REFIID iid, IRpcProxyBuffer **buffer, void **out)
{
	(void)iface;
	return create_proxy(outer, iid, buffer, out);
}

static HRESULT WINAPI factory_stub(IPSFactoryBuffer *iface, REFIID iid, IUnknown *server, IRpcStubBuffer **out)
{
	(void)iface;
	return create_stub(iid, server, out);
}

static const IPSFactoryBufferVtbl factory_table = {factory_query, factory_addref, factory_release, factory_proxy, factory_stub};
static IPSFactoryBuffer factory = {(IPSFactoryBufferVtbl *)&factory_table};

/* The class object of the marshaler, for DllGetClassObject; nothing for any other class. */
HRESULT oa_marshaler_class(REFCLSID clsid, REFIID iid, void **out)
{
	if (!IsEqualCLSID(clsid, &automation_marshaler))
		return CLASS_E_CLASSNOTAVAILABLE;
	return factory_query(&factory, iid, out);
}
