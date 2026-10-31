/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * Writes the model out in the MSFT type library file format. Every segment grows in memory and the
 * file positions are only known once all of them are complete.
 */
#include "typelib.h"

#define MSFT_MAGIC 0x5446534d
#define MSFT_VERSION 0x00010002
#define MSFT_HEADER_SIZE 0x54
#define MSFT_INFO_SIZE 0x64
#define HELP_DLL_FLAG 0x100
#define SEGMENT_COUNT 15
#define HASH_BYTES 0x80
#define NAME_HASH_BYTES 0x200
#define NAME_ENTRY_MARK 0x3800

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
};

struct blob
{
	BYTE *data;
	UINT used, room;
};

struct writer
{
	struct tl_lib *lib;
	struct blob seg[SEGMENT_COUNT];
	struct blob *members;
	UINT name_count, name_chars;
	BOOL failed;
};

static UINT put_bytes(struct writer *w, struct blob *blob, const void *bytes, UINT size)
{
	UINT at = blob->used;
	if (w->failed)
		return 0;
	if (blob->used + size > blob->room)
	{
		UINT room = blob->room ? blob->room * 2 : 256;
		BYTE *data;
		while (room < blob->used + size)
			room *= 2;
		data = tl_alloc(room);
		if (!data)
		{
			w->failed = TRUE;
			return 0;
		}
		if (blob->used)
			memcpy(data, blob->data, blob->used);
		tl_free(blob->data);
		blob->data = data;
		blob->room = room;
	}
	if (bytes)
		memcpy(blob->data + at, bytes, size);
	else
		memset(blob->data + at, 0, size);
	blob->used += size;
	return at;
}

static UINT put_int(struct writer *w, struct blob *blob, INT value)
{
	return put_bytes(w, blob, &value, sizeof(value));
}

static void put_words(struct writer *w, struct blob *blob, WORD low, WORD high)
{
	put_int(w, blob, MAKELONG(low, high));
}

/* Padding is visible in files from other writers, and carries no meaning. */
static void pad_to_int(struct writer *w, struct blob *blob)
{
	static const BYTE filler[4] = {0x57, 0x57, 0x57, 0x57};
	if (blob->used & 3)
		put_bytes(w, blob, filler, 4 - (blob->used & 3));
}

static char *narrow(const WCHAR *text, UINT length, int *out)
{
	int size = length ? WideCharToMultiByte(CP_ACP, 0, text, length, NULL, 0, NULL, NULL) : 0;
	char *bytes = tl_alloc(size + 1);
	if (bytes && size)
		WideCharToMultiByte(CP_ACP, 0, text, length, bytes, size, NULL, NULL);
	*out = size;
	return bytes;
}

static INT add_guid(struct writer *w, const GUID *guid, INT owner)
{
	struct blob *table = &w->seg[SEG_GUIDS];
	const DWORD *parts = (const DWORD *)guid;
	UINT at, bucket = ((parts[0] ^ parts[1] ^ parts[2] ^ parts[3]) % (HASH_BYTES / 4)) * 4;
	INT next;
	for (at = 0; at + 24 <= table->used; at += 24)
		if (!memcmp(table->data + at, guid, sizeof(*guid)))
			return at;
	at = put_bytes(w, table, guid, sizeof(*guid));
	put_int(w, table, owner);
	if (w->failed)
		return -1;
	memcpy(&next, w->seg[SEG_GUID_HASH].data + bucket, sizeof(next));
	put_int(w, table, next);
	memcpy(w->seg[SEG_GUID_HASH].data + bucket, &at, sizeof(at));
	return at;
}

static INT add_guid_if_set(struct writer *w, const GUID *guid, INT owner)
{
	return IsEqualGUID(guid, &GUID_NULL) ? -1 : add_guid(w, guid, owner);
}

static INT add_name(struct writer *w, const WCHAR *name, INT owner)
{
	struct blob *table = &w->seg[SEG_NAMES];
	int size;
	char *bytes;
	UINT at, bucket;
	ULONG hash;
	INT next;
	if (!name)
		return -1;
	bytes = narrow(name, lstrlenW(name), &size);
	if (!bytes)
	{
		w->failed = TRUE;
		return -1;
	}
	if (size > 0xff)
		size = 0xff;
	for (at = 0; at + 12 <= table->used;)
	{
		INT header;
		int existing;
		memcpy(&header, table->data + at + 8, sizeof(header));
		existing = header & 0xff;
		if (existing == size && !memcmp(table->data + at + 12, bytes, size))
		{
			tl_free(bytes);
			return at;
		}
		at += 12 + ((existing + 3) & ~3);
	}
	hash = LHashValOfNameSys(w->lib->attr.syskind, w->lib->attr.lcid, name);
	bucket = (hash % (NAME_HASH_BYTES / 4)) * 4;
	at = put_int(w, table, owner);
	if (w->failed)
	{
		tl_free(bytes);
		return -1;
	}
	memcpy(&next, w->seg[SEG_NAME_HASH].data + bucket, sizeof(next));
	put_int(w, table, next);
	put_int(w, table, (size & 0xff) | NAME_ENTRY_MARK | (hash & 0xffff0000));
	put_bytes(w, table, bytes, size);
	pad_to_int(w, table);
	tl_free(bytes);
	if (w->failed)
		return -1;
	memcpy(w->seg[SEG_NAME_HASH].data + bucket, &at, sizeof(at));
	w->name_count++;
	w->name_chars += size;
	return at;
}

static INT add_string(struct writer *w, const WCHAR *text)
{
	struct blob *table = &w->seg[SEG_STRINGS];
	int size;
	char *bytes;
	UINT at;
	WORD length;
	if (!text)
		return -1;
	bytes = narrow(text, SysStringLen((BSTR)text), &size);
	if (!bytes)
	{
		w->failed = TRUE;
		return -1;
	}
	for (at = 0; at + 2 <= table->used;)
	{
		WORD existing;
		memcpy(&existing, table->data + at, sizeof(existing));
		if (existing == size && !memcmp(table->data + at + 2, bytes, size))
		{
			tl_free(bytes);
			return at;
		}
		at += (2 + existing + 3) & ~3;
	}
	length = (WORD)size;
	at = put_bytes(w, table, &length, sizeof(length));
	put_bytes(w, table, bytes, size);
	pad_to_int(w, table);
	tl_free(bytes);
	return w->failed ? -1 : (INT)at;
}

/* Small non-negative integers fit in the reference itself. */
static INT add_value(struct writer *w, const VARIANT *value)
{
	struct blob *table = &w->seg[SEG_CUSTOM_DATA];
	WORD vt = V_VT(value);
	INT packed = 0, at;
	BOOL small = FALSE;
	switch (vt)
	{
	case VT_I2:
	case VT_BOOL:
		packed = V_I2(value);
		small = TRUE;
		break;
	case VT_I1:
		packed = V_I1(value);
		small = TRUE;
		break;
	case VT_UI1:
		packed = V_UI1(value);
		small = TRUE;
		break;
	case VT_UI2:
		packed = V_UI2(value);
		small = TRUE;
		break;
	case VT_I4:
	case VT_UI4:
	case VT_INT:
	case VT_UINT:
	case VT_ERROR:
	case VT_HRESULT:
		packed = V_I4(value);
		small = TRUE;
		break;
	default:
		break;
	}
	if (small && packed >= 0 && packed <= 0x3ffffff)
		return (INT)(0x80000000u | ((UINT)vt << 26) | (UINT)packed);
	at = put_bytes(w, table, &vt, sizeof(vt));
	switch (vt)
	{
	case VT_R8:
	case VT_CY:
	case VT_DATE:
	case VT_I8:
	case VT_UI8:
	case VT_FILETIME:
		put_bytes(w, table, &V_I8(value), 8);
		break;
	case VT_DECIMAL:
		put_bytes(w, table, &V_DECIMAL(value), sizeof(DECIMAL));
		break;
	case VT_BSTR:
		if (!V_BSTR(value))
			put_int(w, table, -1);
		else
		{
			int size;
			char *bytes = narrow(V_BSTR(value), SysStringLen(V_BSTR(value)), &size);
			if (!bytes)
				w->failed = TRUE;
			else
			{
				put_int(w, table, size);
				put_bytes(w, table, bytes, size);
				tl_free(bytes);
			}
		}
		break;
	case VT_R4:
		put_int(w, table, V_I4(value));
		break;
	default:
		put_int(w, table, small ? packed : 0);
		break;
	}
	pad_to_int(w, table);
	return w->failed ? -1 : at;
}

static INT add_customs(struct writer *w, const struct tl_customs *list)
{
	INT first = -1;
	UINT index;
	/* Each entry names the next, so the list reads back in the order it was set. */
	for (index = list->count; index-- > 0;)
	{
		INT guid = add_guid(w, &list->items[index].guid, -1);
		INT value = add_value(w, &list->items[index].value);
		struct blob *table = &w->seg[SEG_CUSTOM_GUIDS];
		INT at = table->used;
		put_int(w, table, guid);
		put_int(w, table, value);
		put_int(w, table, first);
		first = at;
	}
	return first;
}

static INT add_desc_record(struct writer *w, WORD first, WORD second, WORD low, WORD high)
{
	struct blob *table = &w->seg[SEG_TYPE_DESCS];
	WORD record[4];
	UINT at;
	record[0] = first;
	record[1] = second;
	record[2] = low;
	record[3] = high;
	for (at = 0; at + 8 <= table->used; at += 8)
		if (!memcmp(table->data + at, record, sizeof(record)))
			return at;
	return put_bytes(w, table, record, sizeof(record));
}

static BOOL is_plain(VARTYPE vt)
{
	return vt != VT_PTR && vt != VT_SAFEARRAY && vt != VT_CARRAY && vt != VT_USERDEFINED;
}

static INT add_desc(struct writer *w, const TYPEDESC *desc);

static INT add_array(struct writer *w, const ARRAYDESC *array)
{
	struct blob *table = &w->seg[SEG_ARRAY_DESCS];
	BOOL plain = is_plain(array->tdescElem.vt);
	WORD element = plain ? array->tdescElem.vt : (WORD)add_desc(w, &array->tdescElem);
	UINT dim, total = 1, at;
	for (dim = 0; dim < array->cDims; dim++)
		total *= array->rgbounds[dim].cElements;
	at = table->used;
	put_words(w, table, element, plain ? 0 : 1);
	put_words(w, table, array->cDims, (WORD)(total * sizeof(DWORD)));
	for (dim = 0; dim < array->cDims; dim++)
	{
		put_int(w, table, array->rgbounds[dim].cElements);
		put_int(w, table, array->rgbounds[dim].lLbound);
	}
	return at;
}

/* A plain type needs no record: the reference, with the top bit set, is the type. */
static INT add_desc(struct writer *w, const TYPEDESC *desc)
{
	if (is_plain(desc->vt))
		return (INT)(0x80000000u | ((UINT)desc->vt << 16) | desc->vt);
	switch (desc->vt)
	{
	case VT_USERDEFINED:
		return add_desc_record(w, VT_USERDEFINED, 0x7fff, LOWORD(desc->hreftype), HIWORD(desc->hreftype));
	case VT_CARRAY:
	{
		INT at = add_array(w, desc->lpadesc);
		return add_desc_record(w, VT_CARRAY, 0x7ffe, LOWORD(at), HIWORD(at));
	}
	default:
		if (!desc->lptdesc)
			return add_desc_record(w, desc->vt, 0x7ffe, VT_EMPTY, 0x8000);
		if (is_plain(desc->lptdesc->vt))
			return add_desc_record(w, desc->vt, 0x7ffe, desc->lptdesc->vt, 0x8000);
		return add_desc_record(w, desc->vt, 0x7ffe, (WORD)add_desc(w, desc->lptdesc), 0);
	}
}

static void put_func(struct writer *w, struct blob *block, const struct tl_func *func, UINT index)
{
	SHORT params = func->desc.cParams, param;
	BOOL defaults = FALSE, customs = func->custom.count != 0;
	INT bits, entry = -1;
	UINT extras, length;
	for (param = 0; param < params; param++)
	{
		const ELEMDESC *elem = &func->desc.lprgelemdescParam[param];
		if (elem->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT && elem->paramdesc.pparamdescex)
			defaults = TRUE;
		if (func->params[param].custom.count)
			customs = TRUE;
	}
	bits = func->desc.funckind | (func->desc.invkind << 3) | (func->desc.callconv << 8);
	if (customs)
		bits |= 0x80;
	if (defaults)
		bits |= 0x1000;
	if (func->entry)
		entry = add_string(w, func->entry);
	else if (func->ordinal)
	{
		entry = func->ordinal;
		bits |= 0x2000;
	}
	extras = customs ? 7 + params : 6;
	length = 0x18 + extras * 4 + (defaults ? params * 4 : 0) + params * 12;
	put_int(w, block, length | (index << 16));
	put_int(w, block, add_desc(w, &func->desc.elemdescFunc.tdesc));
	put_int(w, block, func->desc.wFuncFlags);
	put_words(w, block, (WORD)func->desc.oVft, 0);
	put_int(w, block, bits);
	put_words(w, block, params, func->desc.cParamsOpt);
	put_int(w, block, func->help_context);
	put_int(w, block, add_string(w, func->doc));
	put_int(w, block, entry);
	put_int(w, block, -1);
	put_int(w, block, -1);
	put_int(w, block, func->string_context);
	if (customs)
	{
		put_int(w, block, add_customs(w, &func->custom));
		for (param = 0; param < params; param++)
			put_int(w, block, add_customs(w, &func->params[param].custom));
	}
	if (defaults)
		for (param = 0; param < params; param++)
		{
			const ELEMDESC *elem = &func->desc.lprgelemdescParam[param];
			if (elem->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT && elem->paramdesc.pparamdescex)
				put_int(w, block, add_value(w, &elem->paramdesc.pparamdescex->varDefaultValue));
			else
				put_int(w, block, -1);
		}
	for (param = 0; param < params; param++)
	{
		const ELEMDESC *elem = &func->desc.lprgelemdescParam[param];
		put_int(w, block, add_desc(w, &elem->tdesc));
		put_int(w, block, add_name(w, func->params[param].name, -1));
		put_int(w, block, elem->paramdesc.wParamFlags);
	}
}

static void put_var(struct writer *w, struct blob *block, const struct tl_var *var, UINT index)
{
	put_int(w, block, 0x28 | (index << 16));
	put_int(w, block, add_desc(w, &var->desc.elemdescVar.tdesc));
	put_int(w, block, var->desc.wVarFlags);
	put_words(w, block, (WORD)var->desc.varkind, 0);
	put_int(w, block, var->desc.varkind == VAR_CONST ? add_value(w, var->desc.lpvarValue) : (INT)var->desc.oInst);
	put_int(w, block, var->help_context);
	put_int(w, block, add_string(w, var->doc));
	put_int(w, block, -1);
	put_int(w, block, add_customs(w, &var->custom));
	put_int(w, block, var->string_context);
}

static void put_members(struct writer *w, struct blob *block, const struct tl_type *type)
{
	UINT funcs = type->attr.cFuncs, vars = type->attr.cVars, index;
	UINT *starts;
	INT size;
	put_int(w, block, 0);
	starts = tl_alloc((funcs + vars + 1) * sizeof(*starts));
	if (!starts)
	{
		w->failed = TRUE;
		return;
	}
	for (index = 0; index < funcs; index++)
	{
		starts[index] = block->used - 4;
		put_func(w, block, &type->funcs[index], index);
	}
	for (index = 0; index < vars; index++)
	{
		starts[funcs + index] = block->used - 4;
		put_var(w, block, &type->vars[index], funcs + index);
	}
	size = block->used - 4;
	for (index = 0; index < funcs; index++)
		put_int(w, block, type->funcs[index].desc.memid);
	for (index = 0; index < vars; index++)
		put_int(w, block, type->vars[index].desc.memid);
	for (index = 0; index < funcs; index++)
		put_int(w, block, add_name(w, type->funcs[index].name, -1));
	for (index = 0; index < vars; index++)
		put_int(w, block, add_name(w, type->vars[index].name, -1));
	for (index = 0; index < funcs + vars; index++)
		put_int(w, block, starts[index]);
	tl_free(starts);
	if (!w->failed)
		memcpy(block->data, &size, sizeof(size));
}

static INT add_impls(struct writer *w, const struct tl_type *type)
{
	struct blob *table = &w->seg[SEG_REFS];
	INT first = -1;
	UINT index;
	for (index = type->attr.cImplTypes; index-- > 0;)
	{
		INT customs = add_customs(w, &type->impls[index].custom);
		INT at = table->used;
		put_int(w, table, type->impls[index].ref);
		put_int(w, table, type->impls[index].flags);
		put_int(w, table, customs);
		put_int(w, table, first);
		first = at;
	}
	return first;
}

/* The position of the members goes in at the end; here it is only the block's number. */
static void put_type(struct writer *w, struct tl_type *type)
{
	struct blob *table = &w->seg[SEG_TYPE_INFOS];
	TYPEKIND kind = type->attr.typekind;
	INT owner = type->index * MSFT_INFO_SIZE, datatype = -1;
	WORD impls = type->attr.cImplTypes;
	if (kind == TKIND_ALIAS)
		datatype = add_desc(w, &type->attr.tdescAlias);
	else if (kind == TKIND_MODULE)
		datatype = add_string(w, type->dll);
	else if (kind == TKIND_COCLASS)
		datatype = add_impls(w, type);
	else if (impls)
		datatype = type->impls[0].ref;
	else if (kind == TKIND_DISPATCH)
	{
		datatype = w->lib->dispatch_ref;
		impls = 1;
	}
	put_members(w, &w->members[type->index], type);
	put_int(w, table, kind | (type->attr.cbAlignment << 11));
	put_int(w, table, 0);
	put_int(w, table, 0);
	put_int(w, table, -1);
	put_int(w, table, -1);
	put_int(w, table, 0);
	put_words(w, table, type->attr.cFuncs, type->attr.cVars);
	put_int(w, table, 0);
	put_int(w, table, 0);
	put_int(w, table, 0);
	put_int(w, table, 0);
	put_int(w, table, add_guid_if_set(w, &type->attr.guid, owner));
	put_int(w, table, type->attr.wTypeFlags);
	put_int(w, table, add_name(w, type->name, owner));
	put_words(w, table, type->attr.wMajorVerNum, type->attr.wMinorVerNum);
	put_int(w, table, add_string(w, type->doc));
	put_int(w, table, type->string_context);
	put_int(w, table, type->help_context);
	put_int(w, table, add_customs(w, &type->custom));
	put_words(w, table, impls, type->attr.cbSizeVft);
	put_int(w, table, type->attr.cbSizeInstance);
	put_int(w, table, datatype);
	put_int(w, table, -1);
	put_int(w, table, 0);
	put_int(w, table, -1);
}

static void put_imports(struct writer *w)
{
	struct tl_lib *lib = w->lib;
	UINT index;
	for (index = 0; index < lib->import_count; index++)
	{
		const struct tl_import *import = &lib->imports[index];
		struct blob *files = &w->seg[SEG_IMPORT_FILES];
		INT file = files->used, guid = add_guid(w, &import->library, -1), target;
		int size = 0;
		char *bytes = import->path ? narrow(import->path, SysStringLen(import->path), &size) : NULL;
		WORD length = (WORD)(size << 2);
		put_int(w, files, guid);
		put_int(w, files, import->lcid);
		put_words(w, files, import->major, import->minor);
		put_bytes(w, files, &length, sizeof(length));
		if (bytes)
			put_bytes(w, files, bytes, size);
		tl_free(bytes);
		pad_to_int(w, files);
		target = import->by_guid ? add_guid(w, &import->type, -1) : import->index;
		put_int(w, &w->seg[SEG_IMPORT_INFOS], import->by_guid ? 0x10000 : 0);
		put_int(w, &w->seg[SEG_IMPORT_INFOS], file);
		put_int(w, &w->seg[SEG_IMPORT_INFOS], target);
	}
}

static BOOL write_all(HANDLE file, const void *data, UINT size)
{
	DWORD done;
	return !size || (WriteFile(file, data, size, &done, NULL) && done == size);
}

static HRESULT emit(struct writer *w, const WCHAR *path, const INT *header)
{
	struct tl_lib *lib = w->lib;
	BOOL help_dll = lib->help_dll != NULL, written;
	UINT count = lib->type_count, index, at;
	INT directory[SEGMENT_COUNT * 4], help = help_dll ? add_string(w, lib->help_dll) : -1;
	HANDLE file;
	if (w->failed)
		return E_OUTOFMEMORY;
	at = MSFT_HEADER_SIZE + (help_dll ? 4 : 0) + count * 4 + sizeof(directory);
	for (index = 0; index < SEGMENT_COUNT; index++)
	{
		directory[index * 4] = at;
		directory[index * 4 + 1] = w->seg[index].used;
		directory[index * 4 + 2] = -1;
		directory[index * 4 + 3] = 0x0f;
		at += w->seg[index].used;
	}
	/* The members follow the segments, and only now does each type know where. */
	for (index = 0; index < count; index++)
	{
		INT position = at;
		memcpy(w->seg[SEG_TYPE_INFOS].data + index * MSFT_INFO_SIZE + 4, &position, sizeof(position));
		at += w->members[index].used;
	}
	file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return TYPE_E_IOERROR;
	written = write_all(file, header, MSFT_HEADER_SIZE);
	if (help_dll)
		written = written && write_all(file, &help, sizeof(help));
	for (index = 0; index < count && written; index++)
	{
		INT position = index * MSFT_INFO_SIZE;
		written = write_all(file, &position, sizeof(position));
	}
	written = written && write_all(file, directory, sizeof(directory));
	for (index = 0; index < SEGMENT_COUNT && written; index++)
		written = write_all(file, w->seg[index].data, w->seg[index].used);
	for (index = 0; index < count && written; index++)
		written = write_all(file, w->members[index].data, w->members[index].used);
	CloseHandle(file);
	return written ? S_OK : TYPE_E_IOERROR;
}

HRESULT tl_write_file(struct tl_lib *lib, const WCHAR *path)
{
	struct writer w;
	INT header[MSFT_HEADER_SIZE / 4];
	HRESULT status;
	UINT index;
	memset(&w, 0, sizeof(w));
	w.lib = lib;
	w.members = tl_alloc((lib->type_count + 1) * sizeof(*w.members));
	if (!w.members)
		return E_OUTOFMEMORY;
	put_bytes(&w, &w.seg[SEG_GUID_HASH], NULL, HASH_BYTES);
	put_bytes(&w, &w.seg[SEG_NAME_HASH], NULL, NAME_HASH_BYTES);
	if (!w.failed)
	{
		memset(w.seg[SEG_GUID_HASH].data, 0xff, HASH_BYTES);
		memset(w.seg[SEG_NAME_HASH].data, 0xff, NAME_HASH_BYTES);
	}
	for (index = 0; index < lib->type_count; index++)
	{
		tl_settle(lib->types[index]);
		put_type(&w, lib->types[index]);
	}
	put_imports(&w);
	header[0] = MSFT_MAGIC;
	header[1] = MSFT_VERSION;
	header[2] = add_guid_if_set(&w, &lib->attr.guid, -2);
	header[3] = lib->attr.lcid ? lib->attr.lcid : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
	header[4] = lib->attr.lcid;
	header[5] = lib->attr.syskind | 0x40 | (lib->help_dll ? HELP_DLL_FLAG : 0);
	header[6] = MAKELONG(lib->attr.wMajorVerNum, lib->attr.wMinorVerNum);
	header[7] = lib->attr.wLibFlags;
	header[8] = lib->type_count;
	header[9] = add_string(&w, lib->doc);
	header[10] = lib->string_context;
	header[11] = lib->help_context;
	header[14] = add_name(&w, lib->name, -2);
	header[15] = add_string(&w, lib->help_file);
	header[16] = add_customs(&w, &lib->custom);
	header[12] = w.name_count;
	header[13] = w.name_chars;
	header[17] = 0x20;
	header[18] = 0x80;
	header[19] = lib->dispatch_ref;
	header[20] = lib->import_count;
	status = emit(&w, path, header);
	for (index = 0; index < SEGMENT_COUNT; index++)
		tl_free(w.seg[index].data);
	for (index = 0; index < lib->type_count; index++)
		tl_free(w.members[index].data);
	tl_free(w.members);
	return status;
}
