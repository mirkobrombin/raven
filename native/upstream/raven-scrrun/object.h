/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_SCRRUN_OBJECT_H
#define RAVEN_SCRRUN_OBJECT_H

#define COBJMACROS
#include <windows.h>
#include <oleauto.h>

/* Memory, from the process heap; sr_alloc zeroes what it hands out. */
void *sr_alloc(SIZE_T size);
void *sr_grow(void *block, SIZE_T size);
void sr_free(void *block);
WCHAR *sr_copy(const WCHAR *text);

/* The objects of the library are dispatch objects of one shape: a class gives the names of its members, the code that runs them and
 * what frees the rest of the object. Scripts reach them through IDispatch alone. */
typedef struct sr_object sr_object;

/* A member by name, with the most arguments it takes; a call with more is the language's to resolve (obj.Items(0) is the property's
 * value indexed), so it is refused with DISP_E_BADPARAMCOUNT. */
typedef struct {
	const WCHAR *name;
	DISPID id;
	BYTE takes;
} sr_member;

typedef struct {
	const sr_member *members;
	HRESULT (*invoke)(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result);
	void (*destroy)(sr_object *self);
} sr_class;

struct sr_object {
	IDispatch IDispatch_iface;
	LONG refs;
	const sr_class *cls;
};

/* A new object of the class, size bytes with the header first, holding one reference and one lock on the library. */
void *sr_object_new(const sr_class *cls, SIZE_T size);
static inline IDispatch *sr_dispatch(void *object) { return &((sr_object *)object)->IDispatch_iface; }
sr_object *sr_object_of(IDispatch *dispatch, const sr_class *cls);

/* Arguments in their natural order, without the value of a property put; NULL when the caller left it out. */
VARIANT *sr_arg(DISPPARAMS *params, WORD flags, UINT index);
/* The value a property is set to. */
VARIANT *sr_put_value(DISPPARAMS *params);
HRESULT sr_text(VARIANT *v, BSTR *out);
HRESULT sr_long(VARIANT *v, LONG *out);
HRESULT sr_bool(VARIANT *v, BOOL *out);
/* Optional arguments, with the value to take when one is left out. */
HRESULT sr_long_or(VARIANT *v, LONG otherwise, LONG *out);
HRESULT sr_bool_or(VARIANT *v, BOOL otherwise, BOOL *out);
BOOL sr_is_put(WORD flags);

HRESULT sr_return_text(VARIANT *result, const WCHAR *text);
HRESULT sr_return_bstr(VARIANT *result, BSTR text);
HRESULT sr_return_long(VARIANT *result, LONG value);
HRESULT sr_return_bool(VARIANT *result, BOOL value);
HRESULT sr_return_double(VARIANT *result, double value);
/* Hands the object to the result, or releases it when there is none. */
HRESULT sr_return_object(VARIANT *result, IDispatch *object);
HRESULT sr_return_enum(VARIANT *result, VARIANT *items, ULONG count);
/* A file time as a local date. */
HRESULT sr_return_date(VARIANT *result, const FILETIME *time);

/* Errors as the scripting languages number them, and the Win32 errors behind them. */
#define SR_E_ARGUMENT 5
#define SR_E_FILE_NOT_FOUND 53
#define SR_E_BAD_FILE_MODE 54
#define SR_E_FILE_EXISTS 58
#define SR_E_PAST_END 62
#define SR_E_PERMISSION 70
#define SR_E_DISK_NOT_READY 71
#define SR_E_PATH_NOT_FOUND 76
#define SR_E_KEY_EXISTS 457
#define SR_E_ELEMENT_NOT_FOUND 32811
HRESULT sr_error(LONG number);
HRESULT sr_last_error(void);

/* A class COM makes, by its CLSID; a table of them ends with an empty entry. Each object made holds a lock on the library. */
typedef struct {
	const CLSID *clsid;
	IDispatch *(*create)(void);
} sr_coclass;
HRESULT sr_class_object(const sr_coclass *classes, REFCLSID clsid, REFIID riid, void **out);
BOOL sr_can_unload(void);

#endif
