/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * Reads the MSFT type library file format into the model. Every offset comes from the file, so
 * every access goes through the bounds checks here.
 */
#include "typelib.h"

#define MSFT_MAGIC 0x5446534d
#define MSFT_HEADER_SIZE 0x54
#define MSFT_INFO_SIZE 0x64
#define HELP_DLL_FLAG 0x100

/* Segments, in file order. */
enum
{
	SEG_TYPE_INFOS,
	SEG_IMPORT_INFOS,
	SEG_IMPORT_FILES,
	SEG_REFS,
	SEG_GUID_HASH,
	SEG_GUIDS,
	SEG_NAME_HASH,
	SEG_NAMES,
	SEG_STRINGS,
	SEG_TYPE_DESCS,
	SEG_ARRAY_DESCS,
	SEG_CUSTOM_DATA,
	SEG_CUSTOM_GUIDS,
	SEG_COUNT = 15,
};

struct reader
{
	struct tl_lib *lib;
	const BYTE *data;
	SIZE_T size;
	struct
	{
		INT offset, length;
	} seg[SEG_COUNT];
	TYPEDESC *descs;
	UINT desc_count;
	UINT code_page;
};

static BOOL have(const struct reader *r, INT64 offset, SIZE_T size)
{
	return offset >= 0 && (UINT64)offset <= r->size && size <= r->size - (SIZE_T)offset;
}

static INT read_int(const struct reader *r, INT64 offset)
{
	INT value = 0;
	if (have(r, offset, 4))
		memcpy(&value, r->data + offset, 4);
	return value;
}

static WORD read_word(const struct reader *r, INT64 offset)
{
	WORD value = 0;
	if (have(r, offset, 2))
		memcpy(&value, r->data + offset, 2);
	return value;
}

static BSTR bstr_of_bytes(const struct reader *r, const BYTE *bytes, int length)
{
	int wide;
	BSTR text;
	if (length <= 0)
		return SysAllocStringLen(NULL, 0);
	wide = MultiByteToWideChar(r->code_page, 0, (const char *)bytes, length, NULL, 0);
	text = SysAllocStringLen(NULL, wide);
	if (text)
		MultiByteToWideChar(r->code_page, 0, (const char *)bytes, length, text, wide);
	return text;
}

static BSTR read_name(const struct reader *r, INT offset)
{
	INT64 at;
	int length;
	if (offset < 0)
		return NULL;
	at = (INT64)r->seg[SEG_NAMES].offset + offset;
	if (!have(r, at, 12))
		return NULL;
	length = read_int(r, at + 8) & 0xff;
	if (!have(r, at + 12, length))
		return NULL;
	return bstr_of_bytes(r, r->data + at + 12, length);
}

static BSTR read_string(const struct reader *r, INT offset)
{
	INT64 at;
	int length;
	if (offset < 0)
		return NULL;
	at = (INT64)r->seg[SEG_STRINGS].offset + offset;
	if (!have(r, at, 2))
		return NULL;
	length = read_word(r, at);
	if (length == 0xffff || !have(r, at + 2, length))
		return NULL;
	return bstr_of_bytes(r, r->data + at + 2, length);
}

static void read_guid(const struct reader *r, INT offset, GUID *out)
{
	INT64 at = (INT64)r->seg[SEG_GUIDS].offset + offset;
	if (offset >= 0 && have(r, at, sizeof(GUID)))
		memcpy(out, r->data + at, sizeof(GUID));
	else
		memset(out, 0, sizeof(*out));
}

/* A value is either packed inside the offset itself, or stored in the data segment. */
static void read_value(const struct reader *r, INT offset, VARIANT *out)
{
	INT64 at;
	VariantInit(out);
	if (offset < 0)
	{
		V_VT(out) = (offset & 0x7c000000) >> 26;
		V_I4(out) = offset & 0x3ffffff;
		return;
	}
	at = (INT64)r->seg[SEG_CUSTOM_DATA].offset + offset;
	V_VT(out) = read_word(r, at);
	at += 2;
	switch (V_VT(out))
	{
	case VT_R8:
	case VT_CY:
	case VT_DATE:
	case VT_I8:
	case VT_UI8:
	case VT_FILETIME:
		if (have(r, at, 8))
			memcpy(&V_I8(out), r->data + at, 8);
		break;
	case VT_DECIMAL:
		if (have(r, at, sizeof(DECIMAL)))
		{
			memcpy(&V_DECIMAL(out), r->data + at, sizeof(DECIMAL));
			V_VT(out) = VT_DECIMAL;
		}
		break;
	case VT_BSTR:
	{
		INT length = read_int(r, at);
		if (length == -1 || !have(r, at + 4, length > 0 ? length : 0))
			V_BSTR(out) = NULL;
		else
			V_BSTR(out) = bstr_of_bytes(r, r->data + at + 4, length);
		break;
	}
	case VT_EMPTY:
	case VT_NULL:
	case VT_I2:
	case VT_I4:
	case VT_R4:
	case VT_ERROR:
	case VT_BOOL:
	case VT_I1:
	case VT_UI1:
	case VT_UI2:
	case VT_UI4:
	case VT_INT:
	case VT_UINT:
	case VT_VOID:
	case VT_HRESULT:
		V_I4(out) = read_int(r, at);
		break;
	default:
		V_VT(out) = VT_EMPTY;
		break;
	}
}

static void read_customs(const struct reader *r, INT offset, struct tl_customs *list)
{
	UINT guard = 0;
	while (offset >= 0 && guard++ < 4096)
	{
		INT64 at = (INT64)r->seg[SEG_CUSTOM_GUIDS].offset + offset;
		GUID guid;
		VARIANT value;
		if (!have(r, at, 12))
			return;
		read_guid(r, read_int(r, at), &guid);
		read_value(r, read_int(r, at + 4), &value);
		tl_customs_add(list, &guid, &value);
		VariantClear(&value);
		offset = read_int(r, at + 8);
	}
}

/* The file's type descriptions refer to one another by position, so they load as one table. */
static BOOL load_descs(struct reader *r)
{
	UINT index, count = r->seg[SEG_TYPE_DESCS].length > 0 ? r->seg[SEG_TYPE_DESCS].length / 8 : 0;
	INT base = r->seg[SEG_TYPE_DESCS].offset;
	if (!count)
		return TRUE;
	r->descs = tl_keep(r->lib, count * sizeof(TYPEDESC));
	if (!r->descs)
		return FALSE;
	r->desc_count = count;
	for (index = 0; index < count; index++)
	{
		WORD first = read_word(r, base + index * 8), low = read_word(r, base + index * 8 + 4), high = read_word(r, base + index * 8 + 6);
		TYPEDESC *desc = &r->descs[index];
		desc->vt = first & VT_TYPEMASK;
		if (desc->vt == VT_PTR || desc->vt == VT_SAFEARRAY)
		{
			TYPEDESC *target = NULL;
			if (high & 0x8000)
			{
				target = tl_keep(r->lib, sizeof(*target));
				if (target)
					target->vt = low;
			}
			else if ((UINT)(low / 8) < count)
				target = &r->descs[low / 8];
			desc->lptdesc = target;
		}
		else if (desc->vt == VT_USERDEFINED)
			desc->hreftype = MAKELONG(low, high);
		else if (desc->vt == VT_CARRAY)
		{
			INT64 at = (INT64)r->seg[SEG_ARRAY_DESCS].offset + MAKELONG(low, high);
			WORD dims = read_word(r, at + 4);
			UINT dim;
			ARRAYDESC *array = tl_keep(r->lib, sizeof(ARRAYDESC) + dims * sizeof(SAFEARRAYBOUND));
			if (array)
			{
				WORD element = read_word(r, at), complex = read_word(r, at + 2);
				if (complex && (UINT)(element / 8) < count)
					array->tdescElem = r->descs[element / 8];
				else
					array->tdescElem.vt = element & VT_TYPEMASK;
				array->cDims = dims;
				for (dim = 0; dim < dims; dim++)
				{
					array->rgbounds[dim].cElements = read_int(r, at + 8 + dim * 8);
					array->rgbounds[dim].lLbound = read_int(r, at + 12 + dim * 8);
				}
			}
			desc->lpadesc = array;
		}
	}
	return TRUE;
}

static void read_desc(const struct reader *r, INT type, TYPEDESC *out)
{
	memset(out, 0, sizeof(*out));
	if (type < 0)
		out->vt = type & VT_TYPEMASK;
	else if ((UINT)(type / 8) < r->desc_count)
		*out = r->descs[type / 8];
	else
		out->vt = VT_EMPTY;
}

static void read_funcs(const struct reader *r, struct tl_type *type, UINT count, UINT vars, INT64 base)
{
	INT used = read_int(r, base);
	INT64 record = base + 4, tables = base + used;
	UINT index;
	if (!count)
		return;
	type->funcs = tl_keep(r->lib, count * sizeof(*type->funcs));
	if (!type->funcs)
		return;
	type->func_room = count;
	for (index = 0; index < count && have(r, record, 0x18); index++)
	{
		struct tl_func *func = &type->funcs[index];
		INT length = read_int(r, record) & 0xffff, flags = read_int(r, record + 8), bits = read_int(r, record + 16);
		WORD params = read_word(r, record + 20), optional = read_word(r, record + 22);
		INT64 extras = record + 0x18, param_table = record + length - params * 12;
		int extra_count = (int)((param_table - extras) / 4);
		UINT param;
		if (length < 0x18 || !have(r, record, length) || extra_count < 0)
			break;
		if (bits & 0x1000)
			extra_count -= params;
		func->desc.memid = read_int(r, tables + (index + 1) * 4);
		func->name = read_name(r, read_int(r, tables + (count + vars + index + 1) * 4));
		func->desc.funckind = bits & 7;
		func->desc.invkind = (bits >> 3) & 15;
		func->desc.callconv = (bits >> 8) & 15;
		func->desc.cParams = params;
		func->desc.cParamsOpt = optional;
		func->desc.oVft = read_word(r, record + 12) & ~1;
		func->desc.wFuncFlags = LOWORD(flags);
		read_desc(r, read_int(r, record + 4), &func->desc.elemdescFunc.tdesc);
		if (extra_count > 0)
			func->help_context = read_int(r, extras);
		if (extra_count > 1)
			func->doc = read_string(r, read_int(r, extras + 4));
		if (extra_count > 2)
		{
			INT entry = read_int(r, extras + 8);
			if (bits & 0x2000)
				func->ordinal = (WORD)entry;
			else if (entry != -1)
				func->entry = read_string(r, entry);
		}
		if (extra_count > 5)
			func->string_context = read_int(r, extras + 20);
		if (bits & 0x80 && extra_count > 6)
			read_customs(r, read_int(r, extras + 24), &func->custom);
		if (params)
		{
			func->desc.lprgelemdescParam = tl_keep(r->lib, params * sizeof(ELEMDESC));
			func->params = tl_keep(r->lib, params * sizeof(*func->params));
		}
		for (param = 0; param < params && func->desc.lprgelemdescParam && func->params; param++)
		{
			ELEMDESC *elem = &func->desc.lprgelemdescParam[param];
			INT64 entry = param_table + param * 12;
			INT name = read_int(r, entry + 4);
			read_desc(r, read_int(r, entry), &elem->tdesc);
			elem->paramdesc.wParamFlags = (USHORT)read_int(r, entry + 8);
			func->params[param].name = read_name(r, name);
			if (bits & 0x1000 && elem->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT)
			{
				PARAMDESCEX *defaults = tl_keep(r->lib, sizeof(*defaults));
				if (defaults)
				{
					defaults->cBytes = sizeof(*defaults);
					read_value(r, read_int(r, param_table - params * 4 + param * 4), &defaults->varDefaultValue);
					elem->paramdesc.pparamdescex = defaults;
				}
			}
			if (bits & 0x80 && extra_count > 7 + (int)param)
				read_customs(r, read_int(r, extras + 28 + param * 4), &func->params[param].custom);
		}
		record += length;
		type->attr.cFuncs++;
	}
}

static void read_vars(const struct reader *r, struct tl_type *type, UINT funcs, UINT count, INT64 base)
{
	INT used = read_int(r, base);
	INT64 record = base + 4, tables = base + used;
	UINT index;
	if (!count)
		return;
	type->vars = tl_keep(r->lib, count * sizeof(*type->vars));
	if (!type->vars)
		return;
	type->var_room = count;
	/* The records of the functions come first. */
	for (index = 0; index < funcs && have(r, record, 4); index++)
		record += read_int(r, record) & 0xffff;
	for (index = 0; index < count && have(r, record, 0x14); index++)
	{
		struct tl_var *var = &type->vars[index];
		INT length = read_int(r, record) & 0xffff;
		if (length < 0x14 || !have(r, record, length))
			break;
		var->desc.memid = read_int(r, tables + (funcs + index + 1) * 4);
		var->name = read_name(r, read_int(r, tables + (funcs * 2 + count + index + 1) * 4));
		read_desc(r, read_int(r, record + 4), &var->desc.elemdescVar.tdesc);
		var->desc.wVarFlags = LOWORD(read_int(r, record + 8));
		var->desc.varkind = read_word(r, record + 12);
		if (var->desc.varkind == VAR_CONST)
		{
			var->desc.lpvarValue = tl_keep(r->lib, sizeof(VARIANT));
			if (var->desc.lpvarValue)
				read_value(r, read_int(r, record + 16), var->desc.lpvarValue);
		}
		else
			var->desc.oInst = read_int(r, record + 16);
		if (length > 0x14)
			var->help_context = read_int(r, record + 0x14);
		if (length > 0x18)
			var->doc = read_string(r, read_int(r, record + 0x18));
		if (length > 0x20)
			read_customs(r, read_int(r, record + 0x20), &var->custom);
		if (length > 0x24)
			var->string_context = read_int(r, record + 0x24);
		record += length;
		type->attr.cVars++;
	}
}

static void read_impls(const struct reader *r, struct tl_type *type, INT datatype, UINT count)
{
	UINT index;
	if (!count)
		return;
	type->impls = tl_keep(r->lib, count * sizeof(*type->impls));
	if (!type->impls)
		return;
	type->impl_room = count;
	if (type->attr.typekind != TKIND_COCLASS)
	{
		/* Anything else inherits from one interface, kept right in the header. */
		type->impls[0].ref = datatype;
		type->attr.cImplTypes = 1;
		return;
	}
	for (index = 0; index < count && datatype >= 0; index++)
	{
		INT64 at = (INT64)r->seg[SEG_REFS].offset + datatype;
		if (!have(r, at, 16))
			break;
		type->impls[index].ref = read_int(r, at);
		type->impls[index].flags = read_int(r, at + 4);
		read_customs(r, read_int(r, at + 8), &type->impls[index].custom);
		datatype = read_int(r, at + 12);
		type->attr.cImplTypes++;
	}
}

static HRESULT read_type(const struct reader *r, struct tl_lib *lib, UINT index)
{
	INT64 at = (INT64)r->seg[SEG_TYPE_INFOS].offset + index * MSFT_INFO_SIZE;
	struct tl_type *type;
	INT kind, elements, datatype, version;
	if (!have(r, at, MSFT_INFO_SIZE))
		return TYPE_E_CANTLOADLIBRARY;
	kind = read_int(r, at);
	type = tl_type_create(lib, kind & 0xf);
	if (!type)
		return E_OUTOFMEMORY;
	type->attr.cbAlignment = (WORD)((kind >> 11) & 0x1f);
	elements = read_int(r, at + 0x18);
	read_guid(r, read_int(r, at + 0x2c), &type->attr.guid);
	type->attr.wTypeFlags = (WORD)read_int(r, at + 0x30);
	type->name = read_name(r, read_int(r, at + 0x34));
	version = read_int(r, at + 0x38);
	type->attr.wMajorVerNum = LOWORD(version);
	type->attr.wMinorVerNum = HIWORD(version);
	type->doc = read_string(r, read_int(r, at + 0x3c));
	type->string_context = read_int(r, at + 0x40);
	type->help_context = read_int(r, at + 0x44);
	read_customs(r, read_int(r, at + 0x48), &type->custom);
	type->attr.cbSizeVft = read_word(r, at + 0x4e);
	type->attr.cbSizeInstance = read_int(r, at + 0x50);
	datatype = read_int(r, at + 0x54);
	if (type->attr.typekind == TKIND_ALIAS)
		read_desc(r, datatype, &type->attr.tdescAlias);
	else if (type->attr.typekind == TKIND_MODULE && datatype != -1)
		type->dll = read_string(r, datatype);
	else if (read_word(r, at + 0x4c))
		read_impls(r, type, datatype, read_word(r, at + 0x4c));
	read_funcs(r, type, LOWORD(elements), HIWORD(elements), read_int(r, at + 4));
	read_vars(r, type, LOWORD(elements), HIWORD(elements), read_int(r, at + 4));
	/* Dual interfaces show their dispatch face first. */
	if (type->attr.wTypeFlags & TYPEFLAG_FDUAL)
	{
		type->attr.typekind = TKIND_DISPATCH;
		type->automation_flag = type->attr.wTypeFlags & TYPEFLAG_FOLEAUTOMATION;
		type->attr.wTypeFlags &= ~TYPEFLAG_FOLEAUTOMATION;
	}
	return S_OK;
}

static void read_imports(const struct reader *r, struct tl_lib *lib)
{
	UINT index, count = read_int(r, 0x50);
	if (!count || count > (UINT)r->seg[SEG_IMPORT_INFOS].length / 12)
		return;
	lib->imports = tl_keep(r->lib, count * sizeof(*lib->imports));
	if (!lib->imports)
		return;
	lib->import_count = count;
	for (index = 0; index < count; index++)
	{
		INT64 at = (INT64)r->seg[SEG_IMPORT_INFOS].offset + index * 12;
		INT flags = read_int(r, at), file = read_int(r, at + 4), target = read_int(r, at + 8);
		struct tl_import *import = &lib->imports[index];
		INT64 record = (INT64)r->seg[SEG_IMPORT_FILES].offset + file;
		int length;
		read_guid(r, read_int(r, record), &import->library);
		import->lcid = read_int(r, record + 4);
		import->major = read_word(r, record + 8);
		import->minor = read_word(r, record + 10);
		length = read_word(r, record + 12) >> 2;
		if (have(r, record + 14, length))
			import->path = bstr_of_bytes(r, r->data + record + 14, length);
		if (flags & 0x10000)
		{
			import->by_guid = TRUE;
			read_guid(r, target, &import->type);
		}
		else
			import->index = target;
	}
}

HRESULT tl_read_file(struct tl_lib *lib, const BYTE *data, SIZE_T size)
{
	struct reader r;
	INT flags, types, index, version, name;
	INT64 directory;
	memset(&r, 0, sizeof(r));
	r.lib = lib;
	r.data = data;
	r.size = size;
	r.code_page = CP_ACP;
	if (!have(&r, 0, MSFT_HEADER_SIZE) || read_int(&r, 0) != MSFT_MAGIC)
		return TYPE_E_CANTLOADLIBRARY;
	flags = read_int(&r, 0x14);
	types = read_int(&r, 0x20);
	if (types < 0 || types > 0x100000)
		return TYPE_E_CANTLOADLIBRARY;
	directory = MSFT_HEADER_SIZE + (flags & HELP_DLL_FLAG ? 4 : 0) + types * 4;
	if (!have(&r, directory, SEG_COUNT * 16))
		return TYPE_E_CANTLOADLIBRARY;
	for (index = 0; index < SEG_COUNT; index++)
	{
		r.seg[index].offset = read_int(&r, directory + index * 16);
		r.seg[index].length = read_int(&r, directory + index * 16 + 4);
	}
	if (!load_descs(&r))
		return E_OUTOFMEMORY;
	read_guid(&r, read_int(&r, 8), &lib->attr.guid);
	lib->attr.lcid = read_int(&r, 0x10);
	lib->attr.syskind = flags & 0xf;
	version = read_int(&r, 0x18);
	lib->attr.wMajorVerNum = LOWORD(version);
	lib->attr.wMinorVerNum = HIWORD(version);
	lib->attr.wLibFlags = (WORD)read_int(&r, 0x1c) | LIBFLAG_FHASDISKIMAGE;
	lib->doc = read_string(&r, read_int(&r, 0x24));
	lib->string_context = read_int(&r, 0x28);
	lib->help_context = read_int(&r, 0x2c);
	name = read_int(&r, 0x38);
	lib->name = read_name(&r, name);
	lib->help_file = read_string(&r, read_int(&r, 0x3c));
	read_customs(&r, read_int(&r, 0x40), &lib->custom);
	lib->dispatch_ref = read_int(&r, 0x4c);
	if (flags & HELP_DLL_FLAG)
		lib->help_dll = read_string(&r, read_int(&r, MSFT_HEADER_SIZE));
	read_imports(&r, lib);
	for (index = 0; index < types; index++)
	{
		HRESULT status = read_type(&r, lib, index);
		if (FAILED(status))
			return status;
	}
	tl_adopt_native_sizes(lib);
	return S_OK;
}
