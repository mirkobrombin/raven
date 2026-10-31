/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_ATL_H
#define RAVEN_ATL_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <ole2.h>
#include <ocidl.h>
#include <olectl.h>
#include <exdisp.h>

/* The shapes programs built with ATL 10 hand to this library. */

typedef HRESULT (WINAPI atl_creator)(void *, REFIID, void **);
typedef HRESULT (WINAPI atl_creator_arg)(void *, REFIID, void **, DWORD_PTR);

typedef struct
{
	const CLSID *clsid;
	HRESULT (WINAPI *update_registry)(BOOL);
	atl_creator *get_class_object;
	atl_creator *create_instance;
	IUnknown *factory;
	DWORD registered;
	const WCHAR *(WINAPI *description)(void);
	const void *(WINAPI *categories)(void);
	void (WINAPI *object_main)(BOOL);
} atl_object_entry;

typedef struct
{
	UINT size;
	HINSTANCE type_library_module;
	atl_object_entry **first;
	atl_object_entry **last;
	CRITICAL_SECTION lock;
} atl_com_module;

typedef struct atl_create_data
{
	void *object;
	DWORD thread;
	struct atl_create_data *next;
} atl_create_data;

typedef struct
{
	UINT size;
	CRITICAL_SECTION lock;
	atl_create_data *list;
	ATOM *atoms;
	int atom_count;
	int atom_room;
} atl_win_module;

typedef struct atl_term_entry
{
	void (WINAPI *function)(DWORD_PTR);
	DWORD_PTR argument;
	struct atl_term_entry *next;
} atl_term_entry;

typedef struct
{
	UINT size;
	LONG locks;
	atl_term_entry *terms;
	CRITICAL_SECTION lock;
} atl_module;

typedef struct
{
	const IID *iid;
	DWORD_PTR data;
	atl_creator_arg *function;
} atl_interface_entry;

typedef struct
{
	int kind;
	const GUID *category;
} atl_category_entry;

typedef struct
{
	const WCHAR *key;
	const WCHAR *value;
} atl_replacement;

typedef struct atl_registrar atl_registrar;
typedef struct
{
	HRESULT (WINAPI *QueryInterface)(atl_registrar *, REFIID, void **);
	ULONG (WINAPI *AddRef)(atl_registrar *);
	ULONG (WINAPI *Release)(atl_registrar *);
	HRESULT (WINAPI *AddReplacement)(atl_registrar *, const WCHAR *, const WCHAR *);
	HRESULT (WINAPI *ClearReplacements)(atl_registrar *);
	HRESULT (WINAPI *ResourceRegisterSz)(atl_registrar *, const WCHAR *, const WCHAR *, const WCHAR *);
	HRESULT (WINAPI *ResourceUnregisterSz)(atl_registrar *, const WCHAR *, const WCHAR *, const WCHAR *);
	HRESULT (WINAPI *FileRegister)(atl_registrar *, const WCHAR *);
	HRESULT (WINAPI *FileUnregister)(atl_registrar *, const WCHAR *);
	HRESULT (WINAPI *StringRegister)(atl_registrar *, const WCHAR *);
	HRESULT (WINAPI *StringUnregister)(atl_registrar *, const WCHAR *);
	HRESULT (WINAPI *ResourceRegister)(atl_registrar *, const WCHAR *, UINT, const WCHAR *);
	HRESULT (WINAPI *ResourceUnregister)(atl_registrar *, const WCHAR *, UINT, const WCHAR *);
} atl_registrar_vtbl;

struct atl_registrar
{
	const atl_registrar_vtbl *vtbl;
	LONG refs;
	struct replacement *replacements;
};

extern BOOL atl_per_user;
extern const IID IID_IRegistrar;
extern const CLSID CLSID_Registrar;

HRESULT atl_registrar_create(REFIID riid, void **out);
HRESULT atl_delete_tree(HKEY parent, const WCHAR *name);
void *atl_alloc(SIZE_T size);
void atl_free(void *block);

#endif
