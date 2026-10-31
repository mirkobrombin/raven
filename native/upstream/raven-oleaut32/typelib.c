/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * The library object, loading from files and modules, and the registry side.
 */
#include "typelib.h"

#define RT_TYPELIB_NAME L"TYPELIB"

static CRITICAL_SECTION cache_lock;
static struct tl_lib *cache_head;

void oa_typelib_startup(void)
{
	InitializeCriticalSection(&cache_lock);
}

static void lock_cache(void)
{
	EnterCriticalSection(&cache_lock);
}

void *tl_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void tl_free(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

void *tl_keep(struct tl_lib *lib, SIZE_T size)
{
	void **block = tl_alloc(sizeof(void *) * 2 + size);
	if (!block)
		return NULL;
	block[0] = lib->blocks;
	lib->blocks = block;
	return block + 2;
}

void tl_customs_free(struct tl_customs *list)
{
	UINT index;
	for (index = 0; index < list->count; index++)
		VariantClear(&list->items[index].value);
	tl_free(list->items);
	list->items = NULL;
	list->count = 0;
}

HRESULT tl_customs_add(struct tl_customs *list, const GUID *guid, const VARIANT *value)
{
	struct tl_custom *items;
	UINT index;
	for (index = 0; index < list->count; index++)
		if (IsEqualGUID(&list->items[index].guid, guid))
			break;
	if (index == list->count)
	{
		items = tl_alloc((list->count + 1) * sizeof(*items));
		if (!items)
			return E_OUTOFMEMORY;
		if (list->count)
			memcpy(items, list->items, list->count * sizeof(*items));
		tl_free(list->items);
		list->items = items;
		list->count++;
		list->items[index].guid = *guid;
		VariantInit(&list->items[index].value);
	}
	return VariantCopy(&list->items[index].value, (VARIANT *)value);
}

void tl_func_free(struct tl_func *func)
{
	SHORT index;
	SysFreeString(func->name);
	SysFreeString(func->doc);
	SysFreeString(func->entry);
	tl_customs_free(&func->custom);
	for (index = 0; index < func->desc.cParams && func->params; index++)
	{
		const ELEMDESC *elem = &func->desc.lprgelemdescParam[index];
		SysFreeString(func->params[index].name);
		tl_customs_free(&func->params[index].custom);
		if (elem->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT && elem->paramdesc.pparamdescex)
			VariantClear(&elem->paramdesc.pparamdescex->varDefaultValue);
	}
}

void tl_var_free(struct tl_var *var)
{
	SysFreeString(var->name);
	SysFreeString(var->doc);
	tl_customs_free(&var->custom);
	if (var->desc.varkind == VAR_CONST && var->desc.lpvarValue)
		VariantClear(var->desc.lpvarValue);
}

static void free_type(struct tl_type *type)
{
	UINT index;
	for (index = 0; index < type->attr.cFuncs && type->funcs; index++)
		tl_func_free(&type->funcs[index]);
	for (index = 0; index < type->attr.cVars && type->vars; index++)
		tl_var_free(&type->vars[index]);
	for (index = 0; index < type->attr.cImplTypes && type->impls; index++)
		tl_customs_free(&type->impls[index].custom);
	tl_customs_free(&type->custom);
	SysFreeString(type->name);
	SysFreeString(type->doc);
	SysFreeString(type->dll);
	tl_free(type->other_face);
	tl_free(type);
}

static void free_lib(struct tl_lib *lib)
{
	UINT index;
	void **block;
	for (index = 0; index < lib->type_count; index++)
		free_type(lib->types[index]);
	for (index = 0; index < lib->import_count; index++)
	{
		SysFreeString(lib->imports[index].path);
		if (lib->imports[index].loaded)
			ITypeLib_Release(lib->imports[index].loaded);
	}
	tl_customs_free(&lib->custom);
	SysFreeString(lib->name);
	SysFreeString(lib->doc);
	SysFreeString(lib->help_file);
	SysFreeString(lib->help_dll);
	tl_free(lib->types);
	tl_free(lib->path);
	block = lib->blocks;
	while (block)
	{
		void **next = block[0];
		tl_free(block);
		block = next;
	}
	tl_free(lib);
}

static inline struct tl_lib *lib_from_iface(ITypeLib2 *iface)
{
	return CONTAINING_RECORD(iface, struct tl_lib, ITypeLib2_iface);
}

static HRESULT WINAPI lib_QueryInterface(ITypeLib2 *iface, REFIID riid, void **out)
{
	struct tl_lib *lib = lib_from_iface(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ITypeLib) || IsEqualIID(riid, &IID_ITypeLib2))
		*out = &lib->ITypeLib2_iface;
	else if (IsEqualIID(riid, &IID_ICreateTypeLib) || IsEqualIID(riid, &IID_ICreateTypeLib2))
		*out = &lib->ICreateTypeLib2_iface;
	if (!*out)
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI lib_AddRef(ITypeLib2 *iface)
{
	return InterlockedIncrement(&lib_from_iface(iface)->ref);
}

static ULONG WINAPI lib_Release(ITypeLib2 *iface)
{
	struct tl_lib *lib = lib_from_iface(iface);
	ULONG count;
	/* A load must not revive a library between the last release and its unlinking. */
	if (lib->cached)
		lock_cache();
	count = InterlockedDecrement(&lib->ref);
	if (lib->cached)
	{
		if (!count)
		{
			struct tl_lib **link = &cache_head;
			while (*link && *link != lib)
				link = &(*link)->next_cached;
			if (*link)
				*link = lib->next_cached;
		}
		LeaveCriticalSection(&cache_lock);
	}
	if (!count)
		free_lib(lib);
	return count;
}

static UINT WINAPI lib_GetTypeInfoCount(ITypeLib2 *iface)
{
	return lib_from_iface(iface)->type_count;
}

static HRESULT WINAPI lib_GetTypeInfo(ITypeLib2 *iface, UINT index, ITypeInfo **out)
{
	struct tl_lib *lib = lib_from_iface(iface);
	if (!out)
		return E_INVALIDARG;
	if (index >= lib->type_count)
		return TYPE_E_ELEMENTNOTFOUND;
	*out = (ITypeInfo *)&lib->types[index]->ITypeInfo2_iface;
	ITypeInfo_AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI lib_GetTypeInfoType(ITypeLib2 *iface, UINT index, TYPEKIND *out)
{
	struct tl_lib *lib = lib_from_iface(iface);
	if (!out)
		return E_INVALIDARG;
	if (index >= lib->type_count)
		return TYPE_E_ELEMENTNOTFOUND;
	*out = lib->types[index]->attr.typekind;
	return S_OK;
}

static HRESULT WINAPI lib_GetTypeInfoOfGuid(ITypeLib2 *iface, REFGUID guid, ITypeInfo **out)
{
	struct tl_lib *lib = lib_from_iface(iface);
	UINT index;
	if (!guid || !out)
		return E_INVALIDARG;
	for (index = 0; index < lib->type_count; index++)
		if (IsEqualGUID(&lib->types[index]->attr.guid, guid))
			return lib_GetTypeInfo(iface, index, out);
	*out = NULL;
	return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI lib_GetLibAttr(ITypeLib2 *iface, TLIBATTR **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = CoTaskMemAlloc(sizeof(TLIBATTR));
	if (!*out)
		return E_OUTOFMEMORY;
	**out = lib_from_iface(iface)->attr;
	return S_OK;
}

static HRESULT WINAPI lib_GetTypeComp(ITypeLib2 *iface, ITypeComp **out)
{
	struct tl_lib *lib = lib_from_iface(iface);
	if (!out)
		return E_INVALIDARG;
	*out = &lib->ITypeComp_iface;
	ITypeComp_AddRef(*out);
	return S_OK;
}

static void hand_out(BSTR *out, const WCHAR *value)
{
	if (out)
		*out = value ? SysAllocString(value) : NULL;
}

static HRESULT WINAPI lib_GetDocumentation(ITypeLib2 *iface, INT index, BSTR *name, BSTR *doc, DWORD *context, BSTR *file)
{
	struct tl_lib *lib = lib_from_iface(iface);
	if (index == -1)
	{
		hand_out(name, lib->name);
		hand_out(doc, lib->doc);
		if (context)
			*context = lib->help_context;
		hand_out(file, lib->help_file);
		return S_OK;
	}
	if ((UINT)index >= lib->type_count)
		return TYPE_E_ELEMENTNOTFOUND;
	return ITypeInfo2_GetDocumentation(&lib->types[index]->ITypeInfo2_iface, MEMBERID_NIL, name, doc, context, file);
}

/* Calls found for every name in the library a lookup can see; stops when found says so. */
static BOOL name_matches(const WCHAR *candidate, const WCHAR *name, WCHAR *fix)
{
	if (!candidate || lstrcmpiW(candidate, name))
		return FALSE;
	if (fix)
		lstrcpyW(fix, candidate);
	return TRUE;
}

static HRESULT WINAPI lib_IsName(ITypeLib2 *iface, LPOLESTR name, ULONG hash, BOOL *found)
{
	struct tl_lib *lib = lib_from_iface(iface);
	UINT index, member;
	if (!name || !found)
		return E_INVALIDARG;
	*found = TRUE;
	for (index = 0; index < lib->type_count; index++)
	{
		struct tl_type *type = lib->types[index];
		if (name_matches(type->name, name, name))
			return S_OK;
		for (member = 0; member < type->attr.cFuncs; member++)
		{
			struct tl_func *func = &type->funcs[member];
			SHORT param;
			if (name_matches(func->name, name, name))
				return S_OK;
			for (param = 0; param < func->desc.cParams; param++)
				if (name_matches(func->params[param].name, name, name))
					return S_OK;
		}
		for (member = 0; member < type->attr.cVars; member++)
			if (name_matches(type->vars[member].name, name, name))
				return S_OK;
	}
	*found = FALSE;
	return S_OK;
}

static HRESULT WINAPI lib_FindName(ITypeLib2 *iface, LPOLESTR name, ULONG hash, ITypeInfo **infos, MEMBERID *ids, USHORT *count)
{
	struct tl_lib *lib = lib_from_iface(iface);
	UINT index, member;
	USHORT limit, found = 0;
	if (!name || !infos || !ids || !count || !*count)
		return E_INVALIDARG;
	limit = *count;
	for (index = 0; index < lib->type_count && found < limit; index++)
	{
		struct tl_type *type = lib->types[index];
		MEMBERID id = MEMBERID_NIL;
		BOOL match = name_matches(type->name, name, name);
		for (member = 0; !match && member < type->attr.cFuncs; member++)
			if (name_matches(type->funcs[member].name, name, name))
			{
				match = TRUE;
				id = type->funcs[member].desc.memid;
			}
		for (member = 0; !match && member < type->attr.cVars; member++)
			if (name_matches(type->vars[member].name, name, name))
			{
				match = TRUE;
				id = type->vars[member].desc.memid;
			}
		if (match)
		{
			infos[found] = (ITypeInfo *)&type->ITypeInfo2_iface;
			ITypeInfo_AddRef(infos[found]);
			ids[found] = id;
			found++;
		}
	}
	*count = found;
	return S_OK;
}

static void WINAPI lib_ReleaseTLibAttr(ITypeLib2 *iface, TLIBATTR *attr)
{
	CoTaskMemFree(attr);
}

static HRESULT WINAPI lib_GetCustData(ITypeLib2 *iface, REFGUID guid, VARIANT *out)
{
	return tl_customs_get(&lib_from_iface(iface)->custom, guid, out);
}

static HRESULT WINAPI lib_GetLibStatistics(ITypeLib2 *iface, ULONG *unique, ULONG *chars)
{
	struct tl_lib *lib = lib_from_iface(iface);
	UINT index, member;
	ULONG names = 0, length = 0;
	if (!unique)
		return E_INVALIDARG;
	for (index = 0; index < lib->type_count; index++)
	{
		struct tl_type *type = lib->types[index];
		names++;
		length += SysStringLen(type->name);
		for (member = 0; member < type->attr.cFuncs; member++)
		{
			names++;
			length += SysStringLen(type->funcs[member].name);
		}
		for (member = 0; member < type->attr.cVars; member++)
		{
			names++;
			length += SysStringLen(type->vars[member].name);
		}
	}
	*unique = names;
	if (chars)
		*chars = length;
	return S_OK;
}

static HRESULT WINAPI lib_GetDocumentation2(ITypeLib2 *iface, INT index, LCID lcid, BSTR *text, DWORD *context, BSTR *dll)
{
	struct tl_lib *lib = lib_from_iface(iface);
	if (index == -1)
	{
		hand_out(text, lib->doc);
		if (context)
			*context = lib->string_context;
		hand_out(dll, lib->help_dll);
		return S_OK;
	}
	if ((UINT)index >= lib->type_count)
		return TYPE_E_ELEMENTNOTFOUND;
	return ITypeInfo2_GetDocumentation2(&lib->types[index]->ITypeInfo2_iface, MEMBERID_NIL, lcid, text, context, dll);
}

static HRESULT WINAPI lib_GetAllCustData(ITypeLib2 *iface, CUSTDATA *out)
{
	return tl_customs_get_all(&lib_from_iface(iface)->custom, out);
}

static const ITypeLib2Vtbl lib_vtbl = {
	lib_QueryInterface,
	lib_AddRef,
	lib_Release,
	lib_GetTypeInfoCount,
	lib_GetTypeInfo,
	lib_GetTypeInfoType,
	lib_GetTypeInfoOfGuid,
	lib_GetLibAttr,
	lib_GetTypeComp,
	lib_GetDocumentation,
	lib_IsName,
	lib_FindName,
	lib_ReleaseTLibAttr,
	lib_GetCustData,
	lib_GetLibStatistics,
	lib_GetDocumentation2,
	lib_GetAllCustData,
};

/* Names a library binds: its modules and enums as scopes, and their members directly. */
static inline struct tl_lib *lib_from_comp(ITypeComp *iface)
{
	return CONTAINING_RECORD(iface, struct tl_lib, ITypeComp_iface);
}

static HRESULT WINAPI lib_comp_QueryInterface(ITypeComp *iface, REFIID riid, void **out)
{
	return lib_QueryInterface(&lib_from_comp(iface)->ITypeLib2_iface, riid, out);
}

static ULONG WINAPI lib_comp_AddRef(ITypeComp *iface)
{
	return lib_AddRef(&lib_from_comp(iface)->ITypeLib2_iface);
}

static ULONG WINAPI lib_comp_Release(ITypeComp *iface)
{
	return lib_Release(&lib_from_comp(iface)->ITypeLib2_iface);
}

static HRESULT WINAPI lib_comp_Bind(ITypeComp *iface, LPOLESTR name, ULONG hash, WORD flags, ITypeInfo **info, DESCKIND *kind, BINDPTR *bind)
{
	struct tl_lib *lib = lib_from_comp(iface);
	BOOL mismatch = FALSE;
	UINT index;
	if (!name || !info || !kind || !bind)
		return E_INVALIDARG;
	*info = NULL;
	*kind = DESCKIND_NONE;
	bind->lptcomp = NULL;
	for (index = 0; index < lib->type_count; index++)
	{
		struct tl_type *type = lib->types[index];
		TYPEKIND type_kind = type->attr.typekind;
		if ((type_kind == TKIND_ENUM || type_kind == TKIND_MODULE) && type->name && !lstrcmpiW(type->name, name))
		{
			*kind = DESCKIND_TYPECOMP;
			bind->lptcomp = &type->ITypeComp_iface;
			ITypeComp_AddRef(bind->lptcomp);
			return S_OK;
		}
		if (type_kind == TKIND_ENUM || type_kind == TKIND_MODULE)
		{
			HRESULT status = ITypeComp_Bind(&type->ITypeComp_iface, name, hash, flags, info, kind, bind);
			if (status == TYPE_E_TYPEMISMATCH)
				mismatch = TRUE;
			else if (FAILED(status) || *kind != DESCKIND_NONE)
				return status;
		}
		else if (type_kind == TKIND_COCLASS && type->attr.wTypeFlags & TYPEFLAG_FAPPOBJECT)
		{
			if (type->name && !lstrcmpiW(type->name, name))
			{
				*kind = DESCKIND_IMPLICITAPPOBJ;
				bind->lptcomp = &type->ITypeComp_iface;
				ITypeComp_AddRef(bind->lptcomp);
				*info = (ITypeInfo *)&type->ITypeInfo2_iface;
				ITypeInfo_AddRef(*info);
				return S_OK;
			}
		}
	}
	return mismatch ? TYPE_E_TYPEMISMATCH : S_OK;
}

static HRESULT WINAPI lib_comp_BindType(ITypeComp *iface, LPOLESTR name, ULONG hash, ITypeInfo **info, ITypeComp **comp)
{
	struct tl_lib *lib = lib_from_comp(iface);
	UINT index;
	if (!name || !info || !comp)
		return E_INVALIDARG;
	*info = NULL;
	*comp = NULL;
	for (index = 0; index < lib->type_count; index++)
	{
		struct tl_type *type = lib->types[index];
		if (type->name && !lstrcmpiW(type->name, name))
		{
			*info = (ITypeInfo *)&type->ITypeInfo2_iface;
			ITypeInfo_AddRef(*info);
			*comp = &lib->ITypeComp_iface;
			ITypeComp_AddRef(*comp);
			return S_OK;
		}
	}
	return S_OK;
}

static const ITypeCompVtbl lib_comp_vtbl = {
	lib_comp_QueryInterface,
	lib_comp_AddRef,
	lib_comp_Release,
	lib_comp_Bind,
	lib_comp_BindType,
};

/* A library somebody already holds answers for its identity, wherever its file went. */
HRESULT tl_find_loaded(const GUID *library, WORD major, WORD minor, LCID lcid, ITypeLib **out)
{
	struct tl_lib *lib;
	HRESULT status = TYPE_E_LIBNOTREGISTERED;
	lock_cache();
	for (lib = cache_head; lib; lib = lib->next_cached)
		if (IsEqualGUID(&lib->attr.guid, library) && lib->attr.wMajorVerNum == major && lib->attr.wMinorVerNum == minor && lib->attr.lcid == lcid)
		{
			*out = (ITypeLib *)&lib->ITypeLib2_iface;
			ITypeLib_AddRef(*out);
			status = S_OK;
			break;
		}
	LeaveCriticalSection(&cache_lock);
	return status;
}

/* Where another library came from, for the import entry that will find it again. */
const WCHAR *tl_lib_path(ITypeLib *lib)
{
	static const WCHAR standard_name[] = {'s', 't', 'd', 'o', 'l', 'e', '2', '.', 't', 'l', 'b', 0};
	if (lib->lpVtbl != (const ITypeLibVtbl *)&lib_vtbl)
		return standard_name;
	return lib_from_iface((ITypeLib2 *)lib)->path ? lib_from_iface((ITypeLib2 *)lib)->path : standard_name;
}

struct tl_lib *tl_lib_create(void)
{
	struct tl_lib *lib = tl_alloc(sizeof(*lib));
	if (!lib)
		return NULL;
	lib->ITypeLib2_iface.lpVtbl = &lib_vtbl;
	lib->ITypeComp_iface.lpVtbl = &lib_comp_vtbl;
	lib->ICreateTypeLib2_iface.lpVtbl = &tl_create_lib_vtbl;
	lib->ref = 1;
	lib->dispatch_ref = (HREFTYPE)-1;
	return lib;
}

struct tl_type *tl_type_create(struct tl_lib *lib, TYPEKIND kind)
{
	struct tl_type *type;
	if (lib->type_count == lib->type_room)
	{
		UINT room = lib->type_room ? lib->type_room * 2 : 8;
		struct tl_type **types = tl_alloc(room * sizeof(*types));
		if (!types)
			return NULL;
		if (lib->type_count)
			memcpy(types, lib->types, lib->type_count * sizeof(*types));
		tl_free(lib->types);
		lib->types = types;
		lib->type_room = room;
	}
	type = tl_alloc(sizeof(*type));
	if (!type)
		return NULL;
	type->ITypeInfo2_iface.lpVtbl = &tl_info_vtbl;
	type->ITypeComp_iface.lpVtbl = &tl_info_comp_vtbl;
	type->ICreateTypeInfo2_iface.lpVtbl = &tl_create_info_vtbl;
	type->lib = lib;
	type->index = lib->type_count;
	type->attr.typekind = kind;
	type->attr.lcid = lib->attr.lcid;
	type->attr.memidConstructor = MEMBERID_NIL;
	type->attr.memidDestructor = MEMBERID_NIL;
	lib->types[lib->type_count++] = type;
	return type;
}

/* Modules carry the library as a resource; walk the image directly so that loading one runs nothing. */
static const BYTE *rva_to_file(const BYTE *data, SIZE_T size, const IMAGE_SECTION_HEADER *sections, UINT count, DWORD rva, DWORD length)
{
	UINT index;
	for (index = 0; index < count; index++)
	{
		DWORD start = sections[index].VirtualAddress, room = sections[index].SizeOfRawData;
		if (rva >= start && rva - start < room)
		{
			SIZE_T offset = (SIZE_T)sections[index].PointerToRawData + (rva - start);
			if (offset > size || length > size - offset)
				return NULL;
			return data + offset;
		}
	}
	return NULL;
}

static const IMAGE_RESOURCE_DIRECTORY_ENTRY *find_entry(const BYTE *root, SIZE_T room, DWORD at, const WCHAR *name, UINT id, BOOL first)
{
	const IMAGE_RESOURCE_DIRECTORY *directory;
	const IMAGE_RESOURCE_DIRECTORY_ENTRY *entries;
	UINT index, total;
	if (at > room || room - at < sizeof(*directory))
		return NULL;
	directory = (const IMAGE_RESOURCE_DIRECTORY *)(root + at);
	total = directory->NumberOfNamedEntries + directory->NumberOfIdEntries;
	if (room - at - sizeof(*directory) < total * sizeof(*entries))
		return NULL;
	entries = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(directory + 1);
	if (first)
		return total ? entries : NULL;
	for (index = 0; index < total; index++)
	{
		if (name && entries[index].NameIsString)
		{
			DWORD text = entries[index].NameOffset;
			UINT length, walk;
			if (text > room - 2)
				continue;
			length = *(const WORD *)(root + text);
			if (length != (UINT)lstrlenW(name) || room - text - 2 < length * 2)
				continue;
			for (walk = 0; walk < length; walk++)
				if (((const WCHAR *)(root + text + 2))[walk] != name[walk])
					break;
			if (walk == length)
				return &entries[index];
		}
		else if (!name && !entries[index].NameIsString && entries[index].Id == id)
			return &entries[index];
	}
	return NULL;
}

static const BYTE *module_typelib(const BYTE *data, SIZE_T size, UINT id, SIZE_T *length)
{
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)data;
	const IMAGE_FILE_HEADER *file;
	const IMAGE_SECTION_HEADER *sections;
	const IMAGE_DATA_DIRECTORY *directory;
	const IMAGE_RESOURCE_DIRECTORY_ENTRY *entry;
	const IMAGE_RESOURCE_DATA_ENTRY *leaf;
	const BYTE *root, *optional;
	DWORD room;
	if (size < sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE || (DWORD)dos->e_lfanew > size - 4 - sizeof(*file))
		return NULL;
	if (*(const DWORD *)(data + dos->e_lfanew) != IMAGE_NT_SIGNATURE)
		return NULL;
	file = (const IMAGE_FILE_HEADER *)(data + dos->e_lfanew + 4);
	optional = (const BYTE *)(file + 1);
	if ((SIZE_T)(optional - data) + file->SizeOfOptionalHeader + file->NumberOfSections * sizeof(*sections) > size)
		return NULL;
	sections = (const IMAGE_SECTION_HEADER *)(optional + file->SizeOfOptionalHeader);
	if (*(const WORD *)optional == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		directory = &((const IMAGE_OPTIONAL_HEADER64 *)optional)->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
	else
		directory = &((const IMAGE_OPTIONAL_HEADER32 *)optional)->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
	room = directory->Size;
	if (!room)
		return NULL;
	root = rva_to_file(data, size, sections, file->NumberOfSections, directory->VirtualAddress, room);
	if (!root)
		return NULL;
	entry = find_entry(root, room, 0, RT_TYPELIB_NAME, 0, FALSE);
	if (!entry || !entry->DataIsDirectory)
		return NULL;
	entry = find_entry(root, room, entry->OffsetToDirectory, NULL, id, FALSE);
	if (!entry || !entry->DataIsDirectory)
		return NULL;
	entry = find_entry(root, room, entry->OffsetToDirectory, NULL, 0, TRUE);
	if (!entry || entry->DataIsDirectory || entry->OffsetToData > room - sizeof(*leaf))
		return NULL;
	leaf = (const IMAGE_RESOURCE_DATA_ENTRY *)(root + entry->OffsetToData);
	*length = leaf->Size;
	return rva_to_file(data, size, sections, file->NumberOfSections, leaf->OffsetToData, leaf->Size);
}

static HRESULT load_from_bytes(const WCHAR *path, const BYTE *data, SIZE_T size, UINT id, struct tl_lib **out)
{
	const BYTE *image = data;
	SIZE_T length = size;
	struct tl_lib *lib;
	HRESULT status;
	if (size >= 2 && data[0] == 'M' && data[1] == 'Z')
	{
		image = module_typelib(data, size, id, &length);
		if (!image)
			return TYPE_E_CANTLOADLIBRARY;
	}
	lib = tl_lib_create();
	if (!lib)
		return E_OUTOFMEMORY;
	status = tl_read_file(lib, image, length);
	if (FAILED(status))
	{
		lib->ref = 0;
		free_lib(lib);
		return status;
	}
	*out = lib;
	return S_OK;
}

/* A trailing \N names the resource inside a module. */
static UINT split_resource(WCHAR *path)
{
	WCHAR *slash = wcsrchr(path, L'\\');
	WCHAR *walk;
	UINT id = 0;
	if (!slash || !slash[1] || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
		return 1;
	for (walk = slash + 1; *walk; walk++)
	{
		if (*walk < L'0' || *walk > L'9')
			return 1;
		id = id * 10 + (*walk - L'0');
	}
	*slash = 0;
	return id;
}

static HRESULT read_whole_file(const WCHAR *path, BYTE **data, SIZE_T *size)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	LARGE_INTEGER length;
	BYTE *buffer;
	DWORD got = 0;
	if (file == INVALID_HANDLE_VALUE)
		return TYPE_E_CANTLOADLIBRARY;
	if (!GetFileSizeEx(file, &length) || length.QuadPart > 0x7fffffff || !length.QuadPart)
	{
		CloseHandle(file);
		return TYPE_E_CANTLOADLIBRARY;
	}
	buffer = tl_alloc(length.QuadPart);
	if (!buffer)
	{
		CloseHandle(file);
		return E_OUTOFMEMORY;
	}
	if (!ReadFile(file, buffer, (DWORD)length.QuadPart, &got, NULL) || got != (DWORD)length.QuadPart)
	{
		tl_free(buffer);
		CloseHandle(file);
		return TYPE_E_CANTLOADLIBRARY;
	}
	CloseHandle(file);
	*data = buffer;
	*size = got;
	return S_OK;
}

static BOOL resolve_path(const WCHAR *name, WCHAR *resolved, UINT room)
{
	if (SearchPathW(NULL, name, NULL, room, resolved, NULL))
		return TRUE;
	if (GetFileAttributesW(name) != INVALID_FILE_ATTRIBUTES && GetFullPathNameW(name, room, resolved, NULL))
		return TRUE;
	/* A bare name of a library or module that lives beside the system. */
	{
		UINT length = GetSystemDirectoryW(resolved, room);
		if (length && length + lstrlenW(name) + 2 < room)
		{
			resolved[length] = L'\\';
			lstrcpyW(resolved + length + 1, name);
			if (GetFileAttributesW(resolved) != INVALID_FILE_ATTRIBUTES)
				return TRUE;
		}
	}
	return FALSE;
}

static HRESULT load_path(const WCHAR *name, ITypeLib **out);

/* The standard OLE library ships inside this module, so it needs no file in the prefix. */
static BOOL is_standard_library_name(const WCHAR *name)
{
	const WCHAR *slash = wcsrchr(name, L'\\');
	return !lstrcmpiW(slash ? slash + 1 : name, L"stdole2.tlb");
}

static HRESULT load_standard_library(ITypeLib **out)
{
	static const WCHAR key[] = L"*stdole2";
	struct tl_lib *lib = NULL, *walk;
	HMODULE self = NULL;
	HRSRC found;
	HGLOBAL loaded;
	HRESULT status;
	lock_cache();
	for (walk = cache_head; walk; walk = walk->next_cached)
		if (walk->path && !lstrcmpW(walk->path, key))
		{
			InterlockedIncrement(&walk->ref);
			*out = (ITypeLib *)&walk->ITypeLib2_iface;
			LeaveCriticalSection(&cache_lock);
			return S_OK;
		}
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (const WCHAR *)load_standard_library, &self);
	found = self ? FindResourceW(self, MAKEINTRESOURCEW(1), RT_TYPELIB_NAME) : NULL;
	loaded = found ? LoadResource(self, found) : NULL;
	status = loaded ? load_from_bytes(NULL, LockResource(loaded), SizeofResource(self, found), 1, &lib) : TYPE_E_CANTLOADLIBRARY;
	if (SUCCEEDED(status))
	{
		lib->path = tl_alloc(sizeof(key));
		if (lib->path)
			lstrcpyW(lib->path, key);
		lib->cached = TRUE;
		lib->next_cached = cache_head;
		cache_head = lib;
		*out = (ITypeLib *)&lib->ITypeLib2_iface;
	}
	LeaveCriticalSection(&cache_lock);
	return status;
}

static HRESULT load_path(const WCHAR *name, ITypeLib **out)
{
	WCHAR request[MAX_PATH + 16], resolved[MAX_PATH + 16], key[MAX_PATH + 32];
	UINT id;
	BYTE *data = NULL;
	SIZE_T size = 0;
	struct tl_lib *lib = NULL, *walk;
	HRESULT status;
	if (lstrlenW(name) >= MAX_PATH)
		return TYPE_E_CANTLOADLIBRARY;
	lstrcpyW(request, name);
	id = 1;
	if (!resolve_path(request, resolved, ARRAYSIZE(resolved)))
	{
		id = split_resource(request);
		if (!resolve_path(request, resolved, ARRAYSIZE(resolved)))
		{
			if (is_standard_library_name(name))
				return load_standard_library(out);
			return TYPE_E_CANTLOADLIBRARY;
		}
	}
	wsprintfW(key, L"%s\\%u", resolved, id);
	lock_cache();
	for (walk = cache_head; walk; walk = walk->next_cached)
		if (walk->path && !lstrcmpiW(walk->path, key))
		{
			InterlockedIncrement(&walk->ref);
			*out = (ITypeLib *)&walk->ITypeLib2_iface;
			LeaveCriticalSection(&cache_lock);
			return S_OK;
		}
	LeaveCriticalSection(&cache_lock);
	status = read_whole_file(resolved, &data, &size);
	if (FAILED(status))
		return status;
	status = load_from_bytes(resolved, data, size, id, &lib);
	tl_free(data);
	if (FAILED(status))
		return status;
	lib->path = tl_alloc((lstrlenW(key) + 1) * sizeof(WCHAR));
	if (lib->path)
		lstrcpyW(lib->path, key);
	lock_cache();
	for (walk = cache_head; walk; walk = walk->next_cached)
		if (walk->path && lib->path && !lstrcmpiW(walk->path, key))
			break;
	if (walk)
	{
		/* Another thread got here first; both must see one object. */
		InterlockedIncrement(&walk->ref);
		*out = (ITypeLib *)&walk->ITypeLib2_iface;
		LeaveCriticalSection(&cache_lock);
		lib->ref = 0;
		free_lib(lib);
		return S_OK;
	}
	lib->cached = TRUE;
	lib->next_cached = cache_head;
	cache_head = lib;
	LeaveCriticalSection(&cache_lock);
	*out = (ITypeLib *)&lib->ITypeLib2_iface;
	return S_OK;
}

HRESULT WINAPI LoadTypeLibEx(LPCOLESTR name, REGKIND kind, ITypeLib **out)
{
	HRESULT status;
	WCHAR path[MAX_PATH + 16];
	if (!name || !out)
		return E_INVALIDARG;
	*out = NULL;
	status = load_path(name, out);
	if (FAILED(status))
		return status;
	if (kind == REGKIND_REGISTER)
	{
		if (!SearchPathW(NULL, name, NULL, ARRAYSIZE(path), path, NULL))
			lstrcpynW(path, name, ARRAYSIZE(path));
		status = RegisterTypeLib(*out, path, NULL);
		if (FAILED(status))
		{
			ITypeLib_Release(*out);
			*out = NULL;
		}
	}
	return status;
}

HRESULT WINAPI LoadTypeLib(const OLECHAR *name, ITypeLib **out)
{
	return LoadTypeLibEx(name, REGKIND_DEFAULT, out);
}

/* The registry side. */
static BOOL per_user_registration;

void WINAPI OaEnablePerUserTLibRegistration(void)
{
	per_user_registration = TRUE;
}

static void format_guid(const GUID *guid, WCHAR *text)
{
	wsprintfW(text, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid->Data1, guid->Data2, guid->Data3, guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
}

static const WCHAR *platform_name(SYSKIND kind)
{
	switch (kind)
	{
	case SYS_WIN16:
		return L"win16";
	case SYS_WIN32:
		return L"win32";
	case SYS_WIN64:
		return L"win64";
	default:
		return NULL;
	}
}

static void set_string(HKEY key, const WCHAR *name, const WCHAR *value)
{
	RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
}

static HKEY open_classes(BOOL for_user)
{
	HKEY key = HKEY_CLASSES_ROOT;
	if (for_user)
	{
		if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
			return NULL;
	}
	return key;
}

static void close_classes(HKEY key, BOOL for_user)
{
	if (for_user && key)
		RegCloseKey(key);
}

static void register_interface(HKEY root, struct tl_type *type, const WCHAR *lib_guid, const TLIBATTR *attr)
{
	WCHAR path[64], guid[40], version[16];
	HKEY key, sub;
	/* Dual and automation interfaces marshal through the description, plain dispatch through IDispatch. */
	const WCHAR *proxy = type->attr.typekind == TKIND_DISPATCH && !(type->attr.wTypeFlags & TYPEFLAG_FDUAL) ? L"{00020420-0000-0000-C000-000000000046}" : L"{00020424-0000-0000-C000-000000000046}";
	format_guid(&type->attr.guid, guid);
	wsprintfW(path, L"Interface\\%s", guid);
	if (RegCreateKeyExW(root, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
		return;
	if (type->name)
		set_string(key, NULL, type->name);
	if (!RegCreateKeyExW(key, L"ProxyStubClsid", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sub, NULL))
	{
		set_string(sub, NULL, proxy);
		RegCloseKey(sub);
	}
	if (!RegCreateKeyExW(key, L"ProxyStubClsid32", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sub, NULL))
	{
		set_string(sub, NULL, proxy);
		RegCloseKey(sub);
	}
	if (!RegCreateKeyExW(key, L"TypeLib", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sub, NULL))
	{
		set_string(sub, NULL, lib_guid);
		wsprintfW(version, L"%x.%x", attr->wMajorVerNum, attr->wMinorVerNum);
		set_string(sub, L"Version", version);
		RegCloseKey(sub);
	}
	RegCloseKey(key);
}

static HRESULT register_lib(ITypeLib *iface, const WCHAR *path, const WCHAR *help, BOOL for_user)
{
	TLIBATTR *attr = NULL;
	WCHAR guid[40], key_path[128], number[16];
	HKEY root, key = NULL, sub, platform;
	BSTR name = NULL, doc = NULL;
	HRESULT status;
	UINT index, count;
	const WCHAR *platform_text;
	if (!iface || !path)
		return E_INVALIDARG;
	status = ITypeLib_GetLibAttr(iface, &attr);
	if (FAILED(status))
		return status;
	platform_text = platform_name(attr->syskind);
	root = open_classes(for_user);
	if (!platform_text || !root)
	{
		ITypeLib_ReleaseTLibAttr(iface, attr);
		return TYPE_E_REGISTRYACCESS;
	}
	format_guid(&attr->guid, guid);
	wsprintfW(key_path, L"TypeLib\\%s\\%x.%x", guid, attr->wMajorVerNum, attr->wMinorVerNum);
	if (RegCreateKeyExW(root, key_path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
	{
		status = TYPE_E_REGISTRYACCESS;
		goto done;
	}
	ITypeLib_GetDocumentation(iface, -1, &name, &doc, NULL, NULL);
	if (doc || name)
		set_string(key, NULL, doc ? doc : name);
	wsprintfW(number, L"%x\\%s", attr->lcid, platform_text);
	if (RegCreateKeyExW(key, number, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &platform, NULL))
		status = TYPE_E_REGISTRYACCESS;
	else
	{
		set_string(platform, NULL, path);
		RegCloseKey(platform);
	}
	if (!RegCreateKeyExW(key, L"FLAGS", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sub, NULL))
	{
		wsprintfW(number, L"%u", attr->wLibFlags);
		set_string(sub, NULL, number);
		RegCloseKey(sub);
	}
	if (!RegCreateKeyExW(key, L"HELPDIR", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sub, NULL))
	{
		set_string(sub, NULL, help ? help : L"");
		RegCloseKey(sub);
	}
	RegCloseKey(key);
	count = ITypeLib_GetTypeInfoCount(iface);
	for (index = 0; index < count; index++)
	{
		TYPEKIND kind;
		ITypeInfo *info = NULL;
		if (FAILED(ITypeLib_GetTypeInfoType(iface, index, &kind)) || FAILED(ITypeLib_GetTypeInfo(iface, index, &info)))
			continue;
		if (tl_is_ours(info))
		{
			struct tl_type *type = tl_type_from_info((ITypeInfo2 *)info);
			if ((kind == TKIND_INTERFACE && type->attr.wTypeFlags & (TYPEFLAG_FOLEAUTOMATION | TYPEFLAG_FDUAL)) || kind == TKIND_DISPATCH)
				register_interface(root, type, guid, attr);
		}
		ITypeInfo_Release(info);
	}
done:
	SysFreeString(name);
	SysFreeString(doc);
	close_classes(root, for_user);
	ITypeLib_ReleaseTLibAttr(iface, attr);
	return status;
}

HRESULT WINAPI RegisterTypeLib(ITypeLib *lib, const WCHAR *path, const WCHAR *help)
{
	return register_lib(lib, path, help, per_user_registration);
}

HRESULT WINAPI RegisterTypeLibForUser(ITypeLib *lib, OLECHAR *path, OLECHAR *help)
{
	return register_lib(lib, path, help, TRUE);
}

static void delete_tree(HKEY parent, const WCHAR *name)
{
	HKEY key;
	WCHAR child[256];
	DWORD length;
	if (RegOpenKeyExW(parent, name, 0, KEY_ALL_ACCESS, &key))
		return;
	for (;;)
	{
		length = ARRAYSIZE(child);
		if (RegEnumKeyExW(key, 0, child, &length, NULL, NULL, NULL, NULL))
			break;
		delete_tree(key, child);
	}
	RegCloseKey(key);
	RegDeleteKeyW(parent, name);
}

static HRESULT unregister_lib(REFGUID library, WORD major, WORD minor, LCID lcid, SYSKIND kind, BOOL for_user)
{
	WCHAR guid[40], path[128];
	BSTR file = NULL;
	ITypeLib *lib = NULL;
	HKEY root, key;
	const WCHAR *platform = platform_name(kind);
	HRESULT status;
	if (!platform)
		return TYPE_E_REGISTRYACCESS;
	status = QueryPathOfRegTypeLib(library, major, minor, lcid, &file);
	if (FAILED(status))
		return status;
	root = open_classes(for_user);
	if (!root)
	{
		SysFreeString(file);
		return TYPE_E_REGISTRYACCESS;
	}
	/* The interfaces are found through the library, so remove them while it can still load. */
	if (SUCCEEDED(LoadTypeLibEx(file, REGKIND_NONE, &lib)))
	{
		UINT index, count = ITypeLib_GetTypeInfoCount(lib);
		for (index = 0; index < count; index++)
		{
			ITypeInfo *info = NULL;
			TYPEKIND type_kind;
			if (FAILED(ITypeLib_GetTypeInfoType(lib, index, &type_kind)) || FAILED(ITypeLib_GetTypeInfo(lib, index, &info)))
				continue;
			if (tl_is_ours(info))
			{
				struct tl_type *type = tl_type_from_info((ITypeInfo2 *)info);
				if ((type_kind == TKIND_INTERFACE && type->attr.wTypeFlags & (TYPEFLAG_FOLEAUTOMATION | TYPEFLAG_FDUAL)) || type_kind == TKIND_DISPATCH)
				{
					WCHAR iid[40], iface_path[64];
					format_guid(&type->attr.guid, iid);
					wsprintfW(iface_path, L"Interface\\%s", iid);
					delete_tree(root, iface_path);
				}
			}
			ITypeInfo_Release(info);
		}
		ITypeLib_Release(lib);
	}
	SysFreeString(file);
	format_guid(library, guid);
	wsprintfW(path, L"TypeLib\\%s\\%x.%x", guid, major, minor);
	delete_tree(root, path);
	/* The library key goes only with its last version. */
	wsprintfW(path, L"TypeLib\\%s", guid);
	if (!RegOpenKeyExW(root, path, 0, KEY_ALL_ACCESS, &key))
	{
		WCHAR child[32];
		DWORD length = ARRAYSIZE(child);
		BOOL empty = RegEnumKeyExW(key, 0, child, &length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS;
		RegCloseKey(key);
		if (empty)
			RegDeleteKeyW(root, path);
	}
	close_classes(root, for_user);
	return S_OK;
}

HRESULT WINAPI UnRegisterTypeLib(REFGUID library, WORD major, WORD minor, LCID lcid, SYSKIND kind)
{
	return unregister_lib(library, major, minor, lcid, kind, per_user_registration);
}

HRESULT WINAPI UnRegisterTypeLibForUser(REFGUID library, WORD major, WORD minor, LCID lcid, SYSKIND kind)
{
	return unregister_lib(library, major, minor, lcid, kind, TRUE);
}

static UINT hex_value(const WCHAR *text)
{
	UINT value = 0;
	for (;; text++)
	{
		if (*text >= L'0' && *text <= L'9')
			value = value * 16 + (*text - L'0');
		else if (*text >= L'a' && *text <= L'f')
			value = value * 16 + (*text - L'a' + 10);
		else if (*text >= L'A' && *text <= L'F')
			value = value * 16 + (*text - L'A' + 10);
		else
			return value;
	}
}

/* The newest registered minor of the major, or the exact one when it exists. */
static BOOL find_version(HKEY key, WORD *major, WORD *minor)
{
	WCHAR name[32];
	DWORD length, index = 0;
	BOOL found = FALSE;
	UINT best_major = 0, best_minor = 0;
	/* All ones in both asks for the newest; in one of the two it names no version at all. */
	BOOL newest = *major == 0xffff && *minor == 0xffff;
	if (!newest && *major == 0xffff)
		return FALSE;
	for (;;)
	{
		UINT have_major, have_minor;
		WCHAR *dot;
		length = ARRAYSIZE(name);
		if (RegEnumKeyExW(key, index++, name, &length, NULL, NULL, NULL, NULL))
			break;
		dot = wcschr(name, L'.');
		if (!dot)
			continue;
		have_major = hex_value(name);
		have_minor = hex_value(dot + 1);
		if (newest)
		{
			if (!found || have_major > best_major || (have_major == best_major && have_minor > best_minor))
			{
				best_major = have_major;
				best_minor = have_minor;
				found = TRUE;
			}
			continue;
		}
		if (have_major != *major || have_minor < *minor)
			continue;
		if (have_minor == *minor)
			return TRUE;
		if (!found || have_minor > best_minor)
		{
			best_major = have_major;
			best_minor = have_minor;
			found = TRUE;
		}
	}
	if (found)
	{
		*major = (WORD)best_major;
		*minor = (WORD)best_minor;
	}
	return found;
}

static const GUID id_stdole2 = {0x00020430, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

static HRESULT query_path(REFGUID library, WORD major, WORD minor, LCID lcid, BSTR *out, BOOL redirect)
{
	WCHAR guid[40], path[128], value[MAX_PATH + 16], sub[32];
	HKEY lib_key, version_key;
	LCID candidates[3];
	UINT index;
	HRESULT status = TYPE_E_LIBNOTREGISTERED;
	static const WCHAR *const platforms[] = {
#ifdef _WIN64
		L"win64", L"win32",
#else
		L"win32",
#endif
	};
	if (!library || !out)
		return E_INVALIDARG;
	*out = NULL;
	format_guid(library, guid);
	wsprintfW(path, L"TypeLib\\%s", guid);
	if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &lib_key))
	{
		/* Always present, whether or not anything registered it. */
		if (IsEqualGUID(library, &id_stdole2) && major == 2)
		{
			*out = SysAllocString(L"stdole2.tlb");
			return *out ? S_OK : E_OUTOFMEMORY;
		}
		return TYPE_E_LIBNOTREGISTERED;
	}
	if (!find_version(lib_key, &major, &minor))
	{
		RegCloseKey(lib_key);
		return TYPE_E_LIBNOTREGISTERED;
	}
	wsprintfW(sub, L"%x.%x", major, minor);
	if (RegOpenKeyExW(lib_key, sub, 0, KEY_READ, &version_key))
	{
		RegCloseKey(lib_key);
		return TYPE_E_LIBNOTREGISTERED;
	}
	RegCloseKey(lib_key);
	candidates[0] = lcid;
	candidates[1] = PRIMARYLANGID(lcid);
	candidates[2] = 0;
	for (index = 0; index < ARRAYSIZE(candidates) && status != S_OK; index++)
	{
		UINT platform;
		for (platform = 0; platform < ARRAYSIZE(platforms) && status != S_OK; platform++)
		{
			DWORD length = sizeof(value) - sizeof(WCHAR), kind;
			HKEY leaf;
			wsprintfW(sub, L"%x\\%s", candidates[index], platforms[platform]);
			if (RegOpenKeyExW(version_key, sub, 0, KEY_READ, &leaf))
				continue;
			memset(value, 0, sizeof(value));
			if (!RegQueryValueExW(leaf, NULL, NULL, &kind, (BYTE *)value, &length))
			{
				*out = SysAllocString(value);
				status = *out ? S_OK : E_OUTOFMEMORY;
			}
			RegCloseKey(leaf);
			if (status == E_OUTOFMEMORY)
				break;
		}
	}
	RegCloseKey(version_key);
	return status;
}

HRESULT WINAPI QueryPathOfRegTypeLib(REFGUID library, WORD major, WORD minor, LCID lcid, LPBSTR out)
{
	return query_path(library, major, minor, lcid, out, TRUE);
}

HRESULT WINAPI LoadRegTypeLib(REFGUID library, WORD major, WORD minor, LCID lcid, ITypeLib **out)
{
	BSTR path = NULL;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	status = QueryPathOfRegTypeLib(library, major, minor, lcid, &path);
	if (FAILED(status))
		return status;
	status = LoadTypeLib(path, out);
	SysFreeString(path);
	if (SUCCEEDED(status))
	{
		/* What loaded must be at least what the caller asked for. */
		TLIBATTR *attr = NULL;
		if (SUCCEEDED(ITypeLib_GetLibAttr(*out, &attr)))
		{
			if (attr->wMajorVerNum != major || attr->wMinorVerNum < minor)
			{
				ITypeLib_Release(*out);
				*out = NULL;
				status = TYPE_E_LIBNOTREGISTERED;
			}
			CoTaskMemFree(attr);
		}
	}
	return status;
}

/* Name hashes stored in libraries; the high word records language and platform. */
static UINT hash_language_offset(LCID lcid)
{
	switch (PRIMARYLANGID(LANGIDFROMLCID(lcid)))
	{
	case LANG_CZECH:
	case LANG_HUNGARIAN:
	case LANG_POLISH:
	case LANG_SLOVAK:
	case LANG_SPANISH:
		return 32;
	case LANG_HEBREW:
		return 48;
	case LANG_JAPANESE:
		return 64;
	case LANG_KOREAN:
		return 80;
	case LANG_CHINESE:
		return 112;
	case LANG_GREEK:
		return 128;
	case LANG_ICELANDIC:
		return 144;
	case LANG_TURKISH:
		return 160;
	case LANG_NORWEGIAN:
		return SUBLANGID(LANGIDFROMLCID(lcid)) == SUBLANG_NORWEGIAN_NYNORSK ? 176 : 16;
	case LANG_RUSSIAN:
		return 224;
	default:
		return 16;
	}
}

/* Case folds to a single class; the few merges below are part of the stored format. */
static BYTE hash_fold(BYTE value)
{
	if (value >= 'a' && value <= 'z')
		value -= 'a' - 'A';
	if (value == 'W')
		return 'V';
	if (value == 'Y')
		return 'U';
	if (value == '/')
		return 0;
	return value;
}

ULONG WINAPI LHashValOfNameSysA(SYSKIND kind, LCID lcid, LPCSTR text)
{
	ULONG high = (hash_language_offset(lcid) | (kind == SYS_MAC ? 1 : 0)) << 16;
	ULONG low = 0x0deadbee;
	if (!text)
		return 0;
	for (; *text; text++)
		low = 37 * low + hash_fold((BYTE)*text);
	return high | ((low % 65599) & 0xffff);
}

ULONG WINAPI LHashValOfNameSys(SYSKIND kind, LCID lcid, LPCOLESTR text)
{
	char buffer[1024];
	int length;
	if (!text)
		return 0;
	length = WideCharToMultiByte(CP_ACP, 0, text, -1, buffer, sizeof(buffer), NULL, NULL);
	if (!length)
		buffer[0] = 0;
	return LHashValOfNameSysA(kind, lcid, buffer);
}
