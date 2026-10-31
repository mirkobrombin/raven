/* SPDX-License-Identifier: MIT */
#include "tom_private.h"

// The OLE face of an editor. It may outlive the editor; then every call reports that the control is gone.

static HRESULT STDMETHODCALLTYPE inner_query(IUnknown *iface, REFIID iid, void **out)
{
	rich_ole *self = CONTAINING_RECORD(iface, rich_ole, inner);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualIID(iid, &IID_IUnknown))
		*out = &self->inner;
	else if (IsEqualIID(iid, &IID_IRichEditOle))
		*out = &self->ole;
	else if (IsEqualIID(iid, &IID_ITextDocument) || IsEqualIID(iid, &IID_IDispatch) || IsEqualIID(iid, &IID_ITextDocument2Old_))
		*out = &self->document;
	if (!*out)
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE inner_add_ref(IUnknown *iface)
{
	return InterlockedIncrement(&CONTAINING_RECORD(iface, rich_ole, inner)->refs);
}

static void free_object(REOBJECT *object)
{
	if (object->poleobj)
		IOleObject_Release(object->poleobj);
	if (object->pstg)
		IStorage_Release(object->pstg);
	if (object->polesite)
		IOleClientSite_Release(object->polesite);
}

static ULONG STDMETHODCALLTYPE inner_release(IUnknown *iface)
{
	rich_ole *self = CONTAINING_RECORD(iface, rich_ole, inner);
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left) {
		tom_detach(self);
		for (INT i = 0; i < self->object_count; i++)
			free_object(&self->objects[i]);
		re_free(self->objects);
		re_free(self);
	}
	return left;
}

static const IUnknownVtbl inner_vtbl = {inner_query, inner_add_ref, inner_release};

#define SELF(iface) CONTAINING_RECORD(iface, rich_ole, ole)

static HRESULT STDMETHODCALLTYPE ole_query(IRichEditOle *iface, REFIID iid, void **out)
{
	return IUnknown_QueryInterface(SELF(iface)->controlling, iid, out);
}

static ULONG STDMETHODCALLTYPE ole_add_ref(IRichEditOle *iface)
{
	return IUnknown_AddRef(SELF(iface)->controlling);
}

static ULONG STDMETHODCALLTYPE ole_release(IRichEditOle *iface)
{
	return IUnknown_Release(SELF(iface)->controlling);
}

static HRESULT STDMETHODCALLTYPE ole_client_site(IRichEditOle *iface, IOleClientSite **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!SELF(iface)->editor)
		return CO_E_RELEASED;
	return tom_client_site_create(SELF(iface), out);
}

static LONG STDMETHODCALLTYPE ole_object_count(IRichEditOle *iface)
{
	return SELF(iface)->object_count;
}

static LONG STDMETHODCALLTYPE ole_link_count(IRichEditOle *iface)
{
	(void)iface;
	return 0;
}

// The objects keep the order of their marks in the text, so the nth mark is the nth object.
static INT object_position(const rich_ole *self, INT index)
{
	const re_doc *doc = &self->editor->doc;
	for (INT i = 0, seen = 0; i < doc->length; i++)
		if (doc->text[i] == RE_OBJECT && seen++ == index)
			return i;
	return -1;
}

static HRESULT STDMETHODCALLTYPE ole_get_object(IRichEditOle *iface, LONG index, REOBJECT *object, DWORD flags)
{
	rich_ole *self = SELF(iface);
	if (!object || object->cbStruct != sizeof(*object))
		return E_INVALIDARG;
	if (!self->editor)
		return CO_E_RELEASED;
	INT found = -1;
	if (index == REO_IOB_USE_CP) {
		INT seen = 0;
		for (INT i = 0; i < self->object_count && found < 0; i++, seen++)
			if (object_position(self, i) == object->cp)
				found = i;
	} else if (index == REO_IOB_SELECTION) {
		INT start = self->editor->sel_start < self->editor->sel_end ? self->editor->sel_start : self->editor->sel_end;
		INT end = self->editor->sel_start < self->editor->sel_end ? self->editor->sel_end : self->editor->sel_start;
		for (INT i = 0; i < self->object_count && found < 0; i++) {
			INT at = object_position(self, i);
			if (at >= start && at < end)
				found = i;
		}
	} else if (index >= 0 && index < self->object_count) {
		found = index;
	}
	if (found < 0)
		return E_INVALIDARG;
	const REOBJECT *stored = &self->objects[found];
	object->cp = object_position(self, found);
	object->clsid = stored->clsid;
	object->dvaspect = stored->dvaspect;
	object->dwFlags = stored->dwFlags;
	object->dwUser = stored->dwUser;
	object->sizel = stored->sizel;
	object->poleobj = NULL;
	object->pstg = NULL;
	object->polesite = NULL;
	if (flags & REO_GETOBJ_POLEOBJ && stored->poleobj) {
		object->poleobj = stored->poleobj;
		IOleObject_AddRef(object->poleobj);
	}
	if (flags & REO_GETOBJ_PSTG && stored->pstg) {
		object->pstg = stored->pstg;
		IStorage_AddRef(object->pstg);
	}
	if (flags & REO_GETOBJ_POLESITE && stored->polesite) {
		object->polesite = stored->polesite;
		IOleClientSite_AddRef(object->polesite);
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE ole_insert_object(IRichEditOle *iface, REOBJECT *object)
{
	rich_ole *self = SELF(iface);
	if (!object || object->cbStruct != sizeof(*object))
		return E_INVALIDARG;
	re_editor *editor = self->editor;
	if (!editor)
		return CO_E_RELEASED;
	INT start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
	INT end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
	if (object->cp != REO_CP_SELECTION)
		start = end = object->cp;
	if (editor->callback) {
		HRESULT asked = editor->callback->lpVtbl->QueryInsertObject(editor->callback, &object->clsid, object->pstg, REO_CP_SELECTION);
		if (asked != S_OK)
			return asked;
	}
	static const WCHAR mark = RE_OBJECT;
	CHARFORMAT2W format;
	re_doc_char_format(&editor->doc, start, &format);
	format.dwMask = CFM_ALL2;
	re_undo_begin_step(editor);
	if (!re_replace(editor, start, end - start, &mark, 1, &format, TRUE))
		return E_OUTOFMEMORY;
	re_undo_begin_step(editor);
	INT index = 0;
	for (INT i = 0; i < start; i++)
		if (editor->doc.text[i] == RE_OBJECT)
			index++;
	REOBJECT *grown = re_realloc(self->objects, (self->object_count + 1) * sizeof(*grown));
	if (!grown)
		return E_OUTOFMEMORY;
	self->objects = grown;
	memmove(grown + index + 1, grown + index, (self->object_count - index) * sizeof(*grown));
	grown[index] = *object;
	if (object->poleobj && !grown[index].sizel.cx && !grown[index].sizel.cy)
		IOleObject_GetExtent(object->poleobj, object->dvaspect, &grown[index].sizel);
	self->object_count++;
	if (object->poleobj)
		IOleObject_AddRef(object->poleobj);
	if (object->pstg)
		IStorage_AddRef(object->pstg);
	if (object->polesite)
		IOleClientSite_AddRef(object->polesite);
	editor->sel_start = editor->sel_end = start + 1;
	re_refresh(editor);
	re_notify_change(editor);
	return S_OK;
}

// Marks leave the text through every path that edits it; their objects go with them.
void re_ole_text_replaced(re_editor *editor, INT at, INT removed, INT inserted)
{
	if (!editor->ole)
		return;
	rich_ole *self = CONTAINING_RECORD(editor->ole, rich_ole, inner);
	const re_doc *doc = &editor->doc;
	INT index = 0;
	for (INT i = 0; i < at; i++)
		if (doc->text[i] == RE_OBJECT)
			index++;
	INT gone = 0;
	for (INT i = at; i < at + removed && i < doc->length; i++)
		if (doc->text[i] == RE_OBJECT)
			gone++;
	if (gone && index + gone <= self->object_count) {
		for (INT i = 0; i < gone; i++)
			free_object(&self->objects[index + i]);
		memmove(self->objects + index, self->objects + index + gone, (self->object_count - index - gone) * sizeof(REOBJECT));
		self->object_count -= gone;
	}
	tom_ranges_shift(self, at, removed, inserted);
}

void re_ole_text_reset(re_editor *editor)
{
	if (!editor->ole)
		return;
	rich_ole *self = CONTAINING_RECORD(editor->ole, rich_ole, inner);
	for (INT i = 0; i < self->object_count; i++)
		free_object(&self->objects[i]);
	self->object_count = 0;
	tom_ranges_reset(self);
}

static const REOBJECT *object_at(const re_editor *editor, INT at)
{
	if (!editor->ole)
		return NULL;
	const rich_ole *self = CONTAINING_RECORD(editor->ole, rich_ole, inner);
	INT index = 0;
	for (INT i = 0; i < at; i++)
		if (editor->doc.text[i] == RE_OBJECT)
			index++;
	return index < self->object_count ? &self->objects[index] : NULL;
}

// Objects measure themselves in hundredths of a millimetre.
BOOL re_ole_object_size(re_editor *editor, HDC dc, INT at, SIZE *out)
{
	const REOBJECT *object = object_at(editor, at);
	if (!object)
		return FALSE;
	out->cx = re_zoomed(editor, MulDiv(object->sizel.cx, GetDeviceCaps(dc, LOGPIXELSX), 2540));
	out->cy = re_zoomed(editor, MulDiv(object->sizel.cy, GetDeviceCaps(dc, LOGPIXELSY), 2540));
	return TRUE;
}

void re_ole_draw(re_editor *editor, HDC dc, INT at, INT left, INT bottom)
{
	const REOBJECT *object = object_at(editor, at);
	SIZE size;
	IViewObject *view;
	if (!object || !object->poleobj || !re_ole_object_size(editor, dc, at, &size))
		return;
	if (FAILED(IOleObject_QueryInterface(object->poleobj, &IID_IViewObject, (void **)&view)))
		return;
	RECTL bounds = {left, bottom - size.cy, left + size.cx, bottom};
	IViewObject_Draw(view, object->dvaspect, -1, NULL, NULL, NULL, dc, &bounds, NULL, NULL, 0);
	IViewObject_Release(view);
}

static HRESULT STDMETHODCALLTYPE ole_convert(IRichEditOle *iface, LONG index, REFCLSID clsid, LPCSTR name)
{
	(void)iface; (void)index; (void)clsid; (void)name;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ole_activate_as(IRichEditOle *iface, REFCLSID clsid, REFCLSID as)
{
	(void)iface; (void)clsid; (void)as;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ole_set_host_names(IRichEditOle *iface, LPCSTR app, LPCSTR object)
{
	(void)iface; (void)app; (void)object;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ole_set_link(IRichEditOle *iface, LONG index, BOOL available)
{
	(void)iface; (void)index; (void)available;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ole_set_aspect(IRichEditOle *iface, LONG index, DWORD aspect)
{
	(void)iface; (void)index; (void)aspect;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ole_hands_off(IRichEditOle *iface, LONG index)
{
	(void)iface; (void)index;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ole_saved(IRichEditOle *iface, LONG index, LPSTORAGE storage)
{
	(void)iface; (void)index; (void)storage;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ole_deactivate(IRichEditOle *iface)
{
	(void)iface;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ole_help(IRichEditOle *iface, BOOL enter)
{
	(void)iface; (void)enter;
	return E_NOTIMPL;
}

// A copy of a stretch of the text, as plain text.
typedef struct
{
	IDataObject iface;
	LONG refs;
	WCHAR *text;
	INT length;
} text_data;

#define DATA(object) CONTAINING_RECORD(object, text_data, iface)

static HRESULT STDMETHODCALLTYPE data_query(IDataObject *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IDataObject))
		return E_NOINTERFACE;
	*out = iface;
	IDataObject_AddRef(iface);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE data_add_ref(IDataObject *iface)
{
	return InterlockedIncrement(&DATA(iface)->refs);
}

static ULONG STDMETHODCALLTYPE data_release(IDataObject *iface)
{
	text_data *self = DATA(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left) {
		re_free(self->text);
		re_free(self);
	}
	return left;
}

static HRESULT STDMETHODCALLTYPE data_can_get(IDataObject *iface, FORMATETC *format)
{
	(void)iface;
	if (!format)
		return E_INVALIDARG;
	if (format->cfFormat != CF_UNICODETEXT)
		return DV_E_FORMATETC;
	return format->tymed & TYMED_HGLOBAL ? S_OK : DV_E_TYMED;
}

static HRESULT STDMETHODCALLTYPE data_get(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium)
{
	text_data *self = DATA(iface);
	HRESULT result = data_can_get(iface, format);
	if (FAILED(result))
		return result;
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (self->length + 1) * sizeof(WCHAR));
	if (!memory)
		return E_OUTOFMEMORY;
	WCHAR *copy = GlobalLock(memory);
	memcpy(copy, self->text, self->length * sizeof(WCHAR));
	copy[self->length] = 0;
	GlobalUnlock(memory);
	medium->tymed = TYMED_HGLOBAL;
	medium->hGlobal = memory;
	medium->pUnkForRelease = NULL;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE data_get_here(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium)
{
	(void)iface; (void)format; (void)medium;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE data_canonical(IDataObject *iface, FORMATETC *format, FORMATETC *out)
{
	(void)iface; (void)format;
	if (out)
		out->ptd = NULL;
	return DATA_S_SAMEFORMATETC;
}

static HRESULT STDMETHODCALLTYPE data_set(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium, BOOL release)
{
	(void)iface; (void)format; (void)medium; (void)release;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE data_enum(IDataObject *iface, DWORD direction, IEnumFORMATETC **out)
{
	(void)iface; (void)direction;
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE data_advise(IDataObject *iface, FORMATETC *format, DWORD flags, IAdviseSink *sink, DWORD *out)
{
	(void)iface; (void)format; (void)flags; (void)sink; (void)out;
	return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE data_unadvise(IDataObject *iface, DWORD connection)
{
	(void)iface; (void)connection;
	return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE data_enum_advise(IDataObject *iface, IEnumSTATDATA **out)
{
	(void)iface; (void)out;
	return OLE_E_ADVISENOTSUPPORTED;
}

static const IDataObjectVtbl data_vtbl = {
	data_query, data_add_ref, data_release, data_get, data_get_here, data_can_get, data_canonical, data_set, data_enum,
	data_advise, data_unadvise, data_enum_advise,
};

static HRESULT STDMETHODCALLTYPE ole_clipboard(IRichEditOle *iface, CHARRANGE *range, DWORD reco, LPDATAOBJECT *out)
{
	rich_ole *self = SELF(iface);
	(void)reco;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!self->editor)
		return CO_E_RELEASED;
	re_editor *editor = self->editor;
	INT text_length = re_text_length(editor);
	INT start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
	INT end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
	if (range) {
		start = range->cpMin < 0 ? 0 : range->cpMin;
		end = range->cpMax < 0 || range->cpMax > text_length ? text_length : range->cpMax;
	}
	start = start > end ? end : start;
	text_data *data = re_calloc(sizeof(*data));
	INT size = re_count_crlf(editor, start, end);
	WCHAR *text = re_alloc((size + 1) * sizeof(WCHAR));
	if (!data || !text) {
		re_free(data);
		re_free(text);
		return E_OUTOFMEMORY;
	}
	data->iface.lpVtbl = (IDataObjectVtbl *)&data_vtbl;
	data->refs = 1;
	data->text = text;
	data->length = re_get_text_crlf(editor, start, end, text, size, TRUE);
	for (INT i = 0; i < data->length; i++)
		if (text[i] == RE_OBJECT)
			text[i] = ' ';
	*out = &data->iface;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE ole_import(IRichEditOle *iface, LPDATAOBJECT data, CLIPFORMAT format, HGLOBAL picture)
{
	(void)iface; (void)data; (void)format; (void)picture;
	return E_NOTIMPL;
}

static const IRichEditOleVtbl ole_vtbl = {
	ole_query, ole_add_ref, ole_release, ole_client_site, ole_object_count, ole_link_count, ole_get_object, ole_insert_object,
	ole_convert, ole_activate_as, ole_set_host_names, ole_set_link, ole_set_aspect, ole_hands_off, ole_saved, ole_deactivate,
	ole_help, ole_clipboard, ole_import,
};

BOOL re_ole_frozen(const re_editor *editor)
{
	return editor->ole && CONTAINING_RECORD(editor->ole, rich_ole, inner)->frozen;
}

HRESULT re_ole_create(re_editor *editor, IUnknown *controlling, IUnknown **out)
{
	rich_ole *self = re_calloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->controlling = controlling ? controlling : &self->inner;
	self->inner.lpVtbl = (IUnknownVtbl *)&inner_vtbl;
	self->ole.lpVtbl = (IRichEditOleVtbl *)&ole_vtbl;
	self->document.lpVtbl = (ITextDocumentVtbl *)&tom_document_vtbl;
	self->refs = 1;
	self->editor = editor;
	*out = &self->inner;
	return S_OK;
}

void re_ole_detach(IUnknown *ole)
{
	rich_ole *self = CONTAINING_RECORD(ole, rich_ole, inner);
	tom_detach(self);
	self->editor = NULL;
	IUnknown_Release(ole);
}
