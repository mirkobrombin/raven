/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * Every type library, read from a file or under construction, lives as this one model.
 */
#ifndef RAVEN_OLEAUT32_TYPELIB_H
#define RAVEN_OLEAUT32_TYPELIB_H

#include "oa.h"

struct tl_custom
{
	GUID guid;
	VARIANT value;
};

struct tl_customs
{
	struct tl_custom *items;
	UINT count;
};

struct tl_param
{
	BSTR name;
	struct tl_customs custom;
};

struct tl_func
{
	FUNCDESC desc;
	BSTR name, doc, entry;
	WORD ordinal;
	ULONG help_context, string_context;
	struct tl_param *params;
	/* The caller chose the slot in the method table. */
	BOOL fixed_slot;
	struct tl_customs custom;
};

struct tl_var
{
	VARDESC desc;
	BSTR name, doc;
	ULONG help_context, string_context;
	struct tl_customs custom;
};

struct tl_impl
{
	HREFTYPE ref;
	INT flags;
	struct tl_customs custom;
};

/* A type in another library, reached by library identity and either its GUID or its position. */
struct tl_import
{
	GUID library;
	WORD major, minor;
	LCID lcid;
	BSTR path;
	GUID type;
	BOOL by_guid;
	INT index;
	/* Held once used: anything we hand out or walk from it stays valid as long as this library. */
	ITypeLib *loaded;
};

struct tl_lib;

struct tl_type
{
	ITypeInfo2 ITypeInfo2_iface;
	ITypeComp ITypeComp_iface;
	ICreateTypeInfo2 ICreateTypeInfo2_iface;
	LONG ref;
	struct tl_lib *lib;
	UINT index;
	TYPEATTR attr;
	BSTR name, doc, dll;
	ULONG help_context, string_context;
	struct tl_func *funcs;
	struct tl_var *vars;
	struct tl_impl *impls;
	UINT func_room, var_room, impl_room;
	struct tl_customs custom;
	/* The other face of a dual interface, made when somebody asks. */
	struct tl_type *other_face;
	BOOL is_other_face;
	/* Under construction: generated ids and sizes are stale. */
	BOOL dirty;
	BOOL laid_out;
	WORD align_request;
	/* The dispatch face hides this; the interface face gets it back. */
	WORD automation_flag;
};

struct tl_lib
{
	ITypeLib2 ITypeLib2_iface;
	ITypeComp ITypeComp_iface;
	ICreateTypeLib2 ICreateTypeLib2_iface;
	LONG ref;
	TLIBATTR attr;
	BSTR name, doc, help_file, help_dll;
	ULONG help_context, string_context;
	struct tl_type **types;
	UINT type_count, type_room;
	struct tl_import *imports;
	UINT import_count, import_room;
	struct tl_customs custom;
	HREFTYPE dispatch_ref;
	void *blocks;
	WCHAR *path;
	BOOL cached;
	struct tl_lib *next_cached;
};

struct tl_lib *tl_lib_create(void);
struct tl_type *tl_type_create(struct tl_lib *lib, TYPEKIND kind);
void *tl_alloc(SIZE_T size);
/* Zeroed memory that lives until the library goes. */
void *tl_keep(struct tl_lib *lib, SIZE_T size);
void tl_free(void *block);
void tl_customs_free(struct tl_customs *list);
HRESULT tl_customs_get(const struct tl_customs *list, REFGUID guid, VARIANT *out);
HRESULT tl_customs_get_all(const struct tl_customs *list, CUSTDATA *out);
HRESULT tl_customs_add(struct tl_customs *list, const GUID *guid, const VARIANT *value);
HRESULT tl_read_file(struct tl_lib *lib, const BYTE *data, SIZE_T size);
HRESULT tl_write_file(struct tl_lib *lib, const WCHAR *path);
void tl_settle(struct tl_type *type);
void tl_adopt_native_sizes(struct tl_lib *lib);
void tl_func_free(struct tl_func *func);
void tl_var_free(struct tl_var *var);
const WCHAR *tl_lib_path(ITypeLib *lib);
HRESULT tl_find_loaded(const GUID *library, WORD major, WORD minor, LCID lcid, ITypeLib **out);
HRESULT tl_vartype_of(struct tl_type *type, const TYPEDESC *desc, VARTYPE *out);
HRESULT tl_ref_info(struct tl_type *type, HREFTYPE ref, ITypeInfo **out);
SIZE_T tl_desc_extra(const TYPEDESC *desc);
void tl_desc_copy(TYPEDESC *to, const TYPEDESC *from, BYTE **room);
SIZE_T tl_elem_extra(const ELEMDESC *desc, BOOL has_flags);
void tl_elem_copy(ELEMDESC *to, const ELEMDESC *from, BOOL has_flags, BYTE **room);
extern const ITypeInfo2Vtbl tl_info_vtbl;
extern const ITypeCompVtbl tl_info_comp_vtbl;
extern const ICreateTypeInfo2Vtbl tl_create_info_vtbl;
extern const ICreateTypeLib2Vtbl tl_create_lib_vtbl;

static inline struct tl_type *tl_type_from_info(ITypeInfo2 *iface)
{
	return CONTAINING_RECORD(iface, struct tl_type, ITypeInfo2_iface);
}

/* Marks an object of ours, so a foreign ITypeInfo is never treated as one. */
BOOL tl_is_ours(ITypeInfo *info);

#endif
