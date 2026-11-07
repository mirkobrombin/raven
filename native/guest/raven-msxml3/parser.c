/* SPDX-License-Identifier: MIT */
#include "mx.h"

/* The node factory parser keeps what a caller configures and reports itself idle; it does not drive a factory yet, and
 * says so by refusing to run. The SDK headers on this toolchain do not carry the interface, so its layout is here. */

typedef struct parser parser;
typedef struct
{
	HRESULT (WINAPI *QueryInterface)(parser *, REFIID, void **);
	ULONG (WINAPI *AddRef)(parser *);
	ULONG (WINAPI *Release)(parser *);
	HRESULT (WINAPI *SetFactory)(parser *, IUnknown *);
	HRESULT (WINAPI *GetFactory)(parser *, IUnknown **);
	HRESULT (WINAPI *Abort)(parser *, BSTR);
	ULONG (WINAPI *GetLineNumber)(parser *);
	ULONG (WINAPI *GetLinePosition)(parser *);
	ULONG (WINAPI *GetAbsolutePosition)(parser *);
	HRESULT (WINAPI *GetLineBuffer)(parser *, const WCHAR **, ULONG *, ULONG *);
	HRESULT (WINAPI *GetLastError)(parser *);
	HRESULT (WINAPI *GetErrorInfo)(parser *, BSTR *);
	ULONG (WINAPI *GetFlags)(parser *);
	HRESULT (WINAPI *GetURL)(parser *, const WCHAR **);
	HRESULT (WINAPI *SetURL)(parser *, const WCHAR *, const WCHAR *, BOOL);
	HRESULT (WINAPI *Load)(parser *, BOOL, IMoniker *, IBindCtx *, DWORD);
	HRESULT (WINAPI *SetInput)(parser *, IUnknown *);
	HRESULT (WINAPI *PushData)(parser *, const char *, ULONG, BOOL);
	HRESULT (WINAPI *LoadDTD)(parser *, const WCHAR *, const WCHAR *);
	HRESULT (WINAPI *LoadEntity)(parser *, const WCHAR *, const WCHAR *, BOOL);
	HRESULT (WINAPI *ParseEntity)(parser *, const WCHAR *, ULONG, BOOL);
	HRESULT (WINAPI *ExpandEntity)(parser *, const WCHAR *, ULONG);
	HRESULT (WINAPI *SetRoot)(parser *, void *);
	HRESULT (WINAPI *GetRoot)(parser *, void **);
	HRESULT (WINAPI *Run)(parser *, LONG);
	HRESULT (WINAPI *GetParserState)(parser *);
	HRESULT (WINAPI *Suspend)(parser *);
	HRESULT (WINAPI *Reset)(parser *);
	HRESULT (WINAPI *SetFlags)(parser *, ULONG);
	HRESULT (WINAPI *SetSecureBaseURL)(parser *, const WCHAR *);
	HRESULT (WINAPI *GetSecureBaseURL)(parser *, const WCHAR **);
} parser_vtbl_type;

struct parser
{
	const parser_vtbl_type *vtbl;
	LONG refs;
	IUnknown *factory;
	IUnknown *input;
	void *root;
	ULONG flags;
	BSTR secure_url;
};

#define XMLPARSER_IDLE 0

static const GUID iid_node_source = {0xd242361d, 0x51a0, 0x11d2, {0x9c, 0xaf, 0x00, 0x60, 0xb0, 0xec, 0x3d, 0x39}};
static const GUID iid_parser = {0xd242361e, 0x51a0, 0x11d2, {0x9c, 0xaf, 0x00, 0x60, 0xb0, 0xec, 0x3d, 0x39}};

static HRESULT WINAPI parser_QueryInterface(parser *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &iid_node_source) || IsEqualGUID(riid, &iid_parser))
	{
		*out = self;
		InterlockedIncrement(&self->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI parser_AddRef(parser *self)
{
	return InterlockedIncrement(&self->refs);
}

static void replace_unknown(IUnknown **slot, IUnknown *value)
{
	if (value)
		IUnknown_AddRef(value);
	if (*slot)
		IUnknown_Release(*slot);
	*slot = value;
}

static ULONG WINAPI parser_Release(parser *self)
{
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		replace_unknown(&self->factory, NULL);
		replace_unknown(&self->input, NULL);
		SysFreeString(self->secure_url);
		mx_free(self);
	}
	return left;
}

static HRESULT WINAPI parser_SetFactory(parser *self, IUnknown *factory)
{
	replace_unknown(&self->factory, factory);
	return S_OK;
}

static HRESULT WINAPI parser_GetFactory(parser *self, IUnknown **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = self->factory;
	if (self->factory)
		IUnknown_AddRef(self->factory);
	return S_OK;
}

static HRESULT WINAPI parser_Abort(parser *self, BSTR reason)
{
	return E_NOTIMPL;
}

static ULONG WINAPI parser_GetZero(parser *self)
{
	return 0;
}

static HRESULT WINAPI parser_GetLineBuffer(parser *self, const WCHAR **buffer, ULONG *length, ULONG *start)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_GetLastError(parser *self)
{
	return S_OK;
}

static HRESULT WINAPI parser_GetErrorInfo(parser *self, BSTR *out)
{
	return E_NOTIMPL;
}

static ULONG WINAPI parser_GetFlags(parser *self)
{
	return self->flags;
}

static HRESULT WINAPI parser_GetURL(parser *self, const WCHAR **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_SetURL(parser *self, const WCHAR *base, const WCHAR *relative, BOOL async)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_Load(parser *self, BOOL available, IMoniker *name, IBindCtx *context, DWORD mode)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_SetInput(parser *self, IUnknown *input)
{
	if (!input)
		return E_INVALIDARG;
	replace_unknown(&self->input, input);
	return S_OK;
}

static HRESULT WINAPI parser_PushData(parser *self, const char *data, ULONG size, BOOL last)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_LoadDTD(parser *self, const WCHAR *base, const WCHAR *relative)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_LoadEntity(parser *self, const WCHAR *base, const WCHAR *relative, BOOL parameter)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_ParseEntity(parser *self, const WCHAR *text, ULONG length, BOOL parameter)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_ExpandEntity(parser *self, const WCHAR *text, ULONG length)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_SetRoot(parser *self, void *root)
{
	self->root = root;
	return S_OK;
}

static HRESULT WINAPI parser_GetRoot(parser *self, void **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = self->root;
	return S_OK;
}

static HRESULT WINAPI parser_Run(parser *self, LONG chars)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_GetParserState(parser *self)
{
	return XMLPARSER_IDLE;
}

static HRESULT WINAPI parser_Suspend(parser *self)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parser_Reset(parser *self)
{
	replace_unknown(&self->input, NULL);
	return S_OK;
}

static HRESULT WINAPI parser_SetFlags(parser *self, ULONG flags)
{
	self->flags = flags;
	return S_OK;
}

static HRESULT WINAPI parser_SetSecureBaseURL(parser *self, const WCHAR *url)
{
	SysFreeString(self->secure_url);
	self->secure_url = url ? SysAllocString(url) : NULL;
	return S_OK;
}

static HRESULT WINAPI parser_GetSecureBaseURL(parser *self, const WCHAR **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = self->secure_url;
	return S_OK;
}

static const parser_vtbl_type parser_vtbl = {
	parser_QueryInterface,
	parser_AddRef,
	parser_Release,
	parser_SetFactory,
	parser_GetFactory,
	parser_Abort,
	parser_GetZero,
	parser_GetZero,
	parser_GetZero,
	parser_GetLineBuffer,
	parser_GetLastError,
	parser_GetErrorInfo,
	parser_GetFlags,
	parser_GetURL,
	parser_SetURL,
	parser_Load,
	parser_SetInput,
	parser_PushData,
	parser_LoadDTD,
	parser_LoadEntity,
	parser_ParseEntity,
	parser_ExpandEntity,
	parser_SetRoot,
	parser_GetRoot,
	parser_Run,
	parser_GetParserState,
	parser_Suspend,
	parser_Reset,
	parser_SetFlags,
	parser_SetSecureBaseURL,
	parser_GetSecureBaseURL,
};

HRESULT mx_parser_create(REFIID riid, void **out)
{
	parser *self = mx_zalloc(sizeof(*self));
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &parser_vtbl;
	self->refs = 1;
	status = parser_QueryInterface(self, riid, out);
	parser_Release(self);
	return status;
}
