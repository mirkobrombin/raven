/* SPDX-License-Identifier: MIT */
#include "tom_private.h"

// The text object model: a document, ranges of it and the selection. Positions count the final mark, so a story is one longer than its text.


#define DOC_SELF(iface) CONTAINING_RECORD(iface, rich_ole, document)
#define RANGE_SELF(object) CONTAINING_RECORD(object, text_range, iface)

typedef struct client_site
{
	struct client_site *next;
	IOleClientSite site;
	IOleInPlaceSite place;
	LONG refs;
	rich_ole *owner;
} client_site;

static LONG story_length(const rich_ole *owner)
{
	return owner->editor->doc.length;
}

void tom_range_ends(const text_range *range, LONG *start, LONG *end)
{
	if (!range->is_selection) {
		*start = range->start;
		*end = range->end;
		return;
	}
	const re_editor *editor = range->owner->editor;
	*start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
	*end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
}

static void range_store(text_range *range, LONG start, LONG end)
{
	LONG length = story_length(range->owner);
	if (start >= length && end >= length)
		start = end = length - 1;
	start = start < 0 ? 0 : start > length ? length : start;
	end = end < 0 ? 0 : end > length ? length : end;
	if (start > end) {
		LONG swap = start;
		start = end;
		end = swap;
	}
	if (range->is_selection) {
		re_set_selection(range->owner->editor, start, end);
		re_update_caret(range->owner->editor);
	} else {
		range->start = start;
		range->end = end;
	}
}

// A point caught by a replacement stays where the change began; past it, it moves along.
static LONG shift_point(LONG point, INT at, INT removed, INT inserted)
{
	if (point <= at)
		return point;
	if (point < at + removed)
		return at;
	return point + inserted - removed;
}

void tom_ranges_shift(rich_ole *self, INT at, INT removed, INT inserted)
{
	for (text_range *range = self->ranges; range; range = range->next) {
		range->start = shift_point(range->start, at, removed, inserted);
		range->end = shift_point(range->end, at, removed, inserted);
	}
}

void tom_ranges_reset(rich_ole *self)
{
	for (text_range *range = self->ranges; range; range = range->next)
		range->start = range->end = 0;
}

void tom_detach(rich_ole *self)
{
	for (text_range *range = self->ranges; range; range = range->next)
		range->owner = NULL;
	self->ranges = NULL;
	if (self->selection) {
		text_range *selection = self->selection;
		self->selection = NULL;
		selection->owner = NULL;
		ITextRange_Release(&selection->iface);
	}
	for (client_site *site = self->sites; site; site = site->next)
		site->owner = NULL;
	self->sites = NULL;
}

// ---- automation ----

static HMODULE module_of_tom(void)
{
	return re_instance();
}

static ITypeLib *type_library(void)
{
	static ITypeLib *library;
	if (!library) {
		WCHAR path[MAX_PATH];
		ITypeLib *loaded;
		GetModuleFileNameW(module_of_tom(), path, MAX_PATH);
		if (SUCCEEDED(LoadTypeLibEx(path, REGKIND_NONE, &loaded)) && InterlockedCompareExchangePointer((void **)&library, loaded, NULL))
			ITypeLib_Release(loaded);
	}
	return library;
}

HRESULT tom_type_info(REFIID iid, ITypeInfo **out)
{
	ITypeLib *library = type_library();
	if (!library)
		return TYPE_E_CANTLOADLIBRARY;
	return ITypeLib_GetTypeInfoOfGuid(library, iid, out);
}

HRESULT tom_get_ids(REFIID iid, LPOLESTR *names, UINT count, DISPID *ids)
{
	ITypeInfo *info;
	HRESULT result = tom_type_info(iid, &info);
	if (FAILED(result))
		return result;
	result = ITypeInfo_GetIDsOfNames(info, names, count, ids);
	ITypeInfo_Release(info);
	return result;
}

HRESULT tom_invoke(void *object, REFIID iid, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *exception, UINT *error)
{
	ITypeInfo *info;
	HRESULT status = tom_type_info(iid, &info);
	if (FAILED(status))
		return status;
	status = ITypeInfo_Invoke(info, object, id, flags, params, result, exception, error);
	ITypeInfo_Release(info);
	return status;
}

// ---- client site ----

#define SITE_OF(iface) CONTAINING_RECORD(iface, client_site, site)
#define PLACE_OF(iface) CONTAINING_RECORD(iface, client_site, place)

static HRESULT STDMETHODCALLTYPE site_query(IOleClientSite *iface, REFIID iid, void **out)
{
	client_site *self = SITE_OF(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IOleClientSite))
		*out = &self->site;
	else if (IsEqualIID(iid, &IID_IOleWindow) || IsEqualIID(iid, &IID_IOleInPlaceSite))
		*out = &self->place;
	if (!*out)
		return E_NOINTERFACE;
	IOleClientSite_AddRef(iface);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE site_add_ref(IOleClientSite *iface)
{
	return InterlockedIncrement(&SITE_OF(iface)->refs);
}

static ULONG STDMETHODCALLTYPE site_release(IOleClientSite *iface)
{
	client_site *self = SITE_OF(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left) {
		if (self->owner) {
			struct client_site **link = &self->owner->sites;
			while (*link && *link != self)
				link = &(*link)->next;
			if (*link)
				*link = self->next;
		}
		re_free(self);
	}
	return left;
}

static HRESULT STDMETHODCALLTYPE site_save(IOleClientSite *iface)
{
	return SITE_OF(iface)->owner ? E_NOTIMPL : CO_E_RELEASED;
}

static HRESULT STDMETHODCALLTYPE site_moniker(IOleClientSite *iface, DWORD assign, DWORD which, IMoniker **out)
{
	(void)iface; (void)assign; (void)which; (void)out;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE site_container(IOleClientSite *iface, IOleContainer **out)
{
	(void)iface;
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE site_show(IOleClientSite *iface)
{
	(void)iface;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE site_on_show(IOleClientSite *iface, BOOL show)
{
	(void)iface; (void)show;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE site_layout(IOleClientSite *iface)
{
	(void)iface;
	return E_NOTIMPL;
}

static const IOleClientSiteVtbl site_vtbl = {site_query, site_add_ref, site_release, site_save, site_moniker, site_container, site_show, site_on_show, site_layout};

static HRESULT STDMETHODCALLTYPE place_query(IOleInPlaceSite *iface, REFIID iid, void **out)
{
	return site_query(&PLACE_OF(iface)->site, iid, out);
}

static ULONG STDMETHODCALLTYPE place_add_ref(IOleInPlaceSite *iface)
{
	return site_add_ref(&PLACE_OF(iface)->site);
}

static ULONG STDMETHODCALLTYPE place_release(IOleInPlaceSite *iface)
{
	return site_release(&PLACE_OF(iface)->site);
}

static HRESULT STDMETHODCALLTYPE place_window(IOleInPlaceSite *iface, HWND *out)
{
	client_site *self = PLACE_OF(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!self->owner || !self->owner->editor)
		return CO_E_RELEASED;
	*out = self->owner->editor->hwnd;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE place_help(IOleInPlaceSite *iface, BOOL enter)
{
	(void)iface; (void)enter;
	return E_NOTIMPL;
}

#define PLACE_STUB(name) \
	static HRESULT STDMETHODCALLTYPE name(IOleInPlaceSite *iface) \
	{ \
		(void)iface; \
		return E_NOTIMPL; \
	}

PLACE_STUB(place_can_activate)
PLACE_STUB(place_on_activate)
PLACE_STUB(place_on_ui_activate)
PLACE_STUB(place_discard_undo)
PLACE_STUB(place_undo)
PLACE_STUB(place_deactivate)

static HRESULT STDMETHODCALLTYPE place_context(IOleInPlaceSite *iface, IOleInPlaceFrame **frame, IOleInPlaceUIWindow **doc, LPRECT position, LPRECT clip, LPOLEINPLACEFRAMEINFO info)
{
	(void)iface; (void)frame; (void)doc; (void)position; (void)clip; (void)info;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE place_scroll(IOleInPlaceSite *iface, SIZE amount)
{
	(void)iface; (void)amount;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE place_ui_deactivate(IOleInPlaceSite *iface, BOOL undoable)
{
	(void)iface; (void)undoable;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE place_extent(IOleInPlaceSite *iface, LPCRECT position)
{
	(void)iface; (void)position;
	return E_NOTIMPL;
}

static const IOleInPlaceSiteVtbl place_vtbl = {
	place_query, place_add_ref, place_release, place_window, place_help, place_can_activate, place_on_activate, place_on_ui_activate,
	place_context, place_scroll, place_ui_deactivate, place_deactivate, place_discard_undo, place_undo, place_extent,
};

HRESULT tom_client_site_create(rich_ole *owner, IOleClientSite **out)
{
	client_site *self = re_calloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->site.lpVtbl = (IOleClientSiteVtbl *)&site_vtbl;
	self->place.lpVtbl = (IOleInPlaceSiteVtbl *)&place_vtbl;
	self->refs = 1;
	self->owner = owner;
	self->next = owner->sites;
	owner->sites = self;
	*out = &self->site;
	return S_OK;
}

// ---- ranges ----

#define ALIVE(self) do { if (!(self)->owner || !(self)->owner->editor) return CO_E_RELEASED; } while (0)
#define OUT_ALIVE(self, pointer) do { ALIVE(self); if (!(pointer)) return E_INVALIDARG; } while (0)

static HRESULT STDMETHODCALLTYPE range_query(ITextRange *iface, REFIID iid, void **out)
{
	text_range *self = RANGE_SELF(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDispatch) || IsEqualIID(iid, &IID_ITextRange)
		|| (self->is_selection && IsEqualIID(iid, &IID_ITextSelection)))
		*out = iface;
	if (!*out)
		return E_NOINTERFACE;
	ITextRange_AddRef(iface);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE range_add_ref(ITextRange *iface)
{
	return InterlockedIncrement(&RANGE_SELF(iface)->refs);
}

static ULONG STDMETHODCALLTYPE range_release(ITextRange *iface)
{
	text_range *self = RANGE_SELF(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	if (self->owner) {
		if (self->is_selection) {
			self->owner->selection = NULL;
		} else {
			text_range **link = &self->owner->ranges;
			while (*link && *link != self)
				link = &(*link)->next;
			if (*link)
				*link = self->next;
		}
	}
	re_free(self);
	return 0;
}

static const IID *range_iid(const text_range *self)
{
	return self->is_selection ? &IID_ITextSelection : &IID_ITextRange;
}

static HRESULT STDMETHODCALLTYPE range_type_info_count(ITextRange *iface, UINT *out)
{
	(void)iface;
	*out = 1;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_type_info(ITextRange *iface, UINT index, LCID locale, ITypeInfo **out)
{
	(void)index; (void)locale;
	return tom_type_info(range_iid(RANGE_SELF(iface)), out);
}

static HRESULT STDMETHODCALLTYPE range_ids(ITextRange *iface, REFIID iid, LPOLESTR *names, UINT count, LCID locale, DISPID *ids)
{
	(void)iid; (void)locale;
	return tom_get_ids(range_iid(RANGE_SELF(iface)), names, count, ids);
}

static HRESULT STDMETHODCALLTYPE range_invoke(ITextRange *iface, DISPID id, REFIID iid, LCID locale, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *exception, UINT *error)
{
	(void)iid; (void)locale;
	return tom_invoke(iface, range_iid(RANGE_SELF(iface)), id, flags, params, result, exception, error);
}

static HRESULT STDMETHODCALLTYPE range_get_text(ITextRange *iface, BSTR *out)
{
	text_range *self = RANGE_SELF(iface);
	OUT_ALIVE(self, out);
	LONG start, end;
	tom_range_ends(self, &start, &end);
	*out = NULL;
	if (start == end)
		return S_OK;
	const re_doc *doc = &self->owner->editor->doc;
	// Past the last character the final mark is a line ending like the others.
	*out = SysAllocStringLen(doc->text + start, end - start);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static BOOL range_replace(text_range *self, LONG start, LONG end, const WCHAR *text, INT length)
{
	re_editor *editor = self->owner->editor;
	CHARFORMAT2W format;
	re_doc_char_format(&editor->doc, start, &format);
	format.dwMask = CFM_ALL2;
	LONG sel_start = editor->sel_start, sel_end = editor->sel_end;
	re_undo_begin_step(editor);
	if (!re_replace(editor, start, end - start, text, length, &format, TRUE))
		return FALSE;
	re_undo_begin_step(editor);
	INT limit = editor->doc.length;
	sel_start = shift_point(sel_start, start, end - start, length);
	sel_end = shift_point(sel_end, start, end - start, length);
	editor->sel_start = sel_start > limit ? limit : sel_start;
	editor->sel_end = sel_end > limit ? limit : sel_end;
	editor->modified = TRUE;
	if (!self->owner->frozen)
		re_refresh(editor);
	re_notify_change(editor);
	return TRUE;
}

static HRESULT STDMETHODCALLTYPE range_set_text(ITextRange *iface, BSTR text)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	LONG start, end, limit = story_length(self->owner) - 1;
	tom_range_ends(self, &start, &end);
	if (end > limit)
		end = limit;
	if (start > limit)
		start = limit;
	INT length = text ? SysStringLen(text) : 0;
	if (!range_replace(self, start, end, text, length))
		return E_OUTOFMEMORY;
	range_store(self, start, start + length);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_get_char(ITextRange *iface, LONG *out)
{
	text_range *self = RANGE_SELF(iface);
	OUT_ALIVE(self, out);
	LONG start, end;
	tom_range_ends(self, &start, &end);
	const re_doc *doc = &self->owner->editor->doc;
	*out = start < doc->length ? doc->text[start] : 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_set_char(ITextRange *iface, LONG value)
{
	(void)value;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE range_duplicate(ITextRange *iface, ITextRange **out)
{
	text_range *self = RANGE_SELF(iface);
	OUT_ALIVE(self, out);
	LONG start, end;
	tom_range_ends(self, &start, &end);
	return tom_range_create(self->owner, start, end, out);
}

static HRESULT STDMETHODCALLTYPE range_formatted(ITextRange *iface, ITextRange **out)
{
	(void)out;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE range_set_formatted(ITextRange *iface, ITextRange *other)
{
	(void)other;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE range_get_start(ITextRange *iface, LONG *out)
{
	text_range *self = RANGE_SELF(iface);
	OUT_ALIVE(self, out);
	LONG end;
	tom_range_ends(self, out, &end);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_get_end(ITextRange *iface, LONG *out)
{
	text_range *self = RANGE_SELF(iface);
	OUT_ALIVE(self, out);
	LONG start;
	tom_range_ends(self, &start, out);
	return S_OK;
}

// Moving one end past the other carries the other along.
static HRESULT STDMETHODCALLTYPE range_set_start(ITextRange *iface, LONG value)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	LONG start, end, length = story_length(self->owner);
	tom_range_ends(self, &start, &end);
	value = value < 0 ? 0 : value > length - 1 ? length - 1 : value;
	if (value == start)
		return S_FALSE;
	range_store(self, value, end < value ? value : end);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_set_end(ITextRange *iface, LONG value)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	LONG start, end, length = story_length(self->owner);
	tom_range_ends(self, &start, &end);
	value = value < 0 ? 0 : value > length ? length : value;
	if (value == end)
		return S_FALSE;
	range_store(self, start > value ? value : start, value);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_get_font(ITextRange *iface, ITextFont **out)
{
	text_range *self = RANGE_SELF(iface);
	OUT_ALIVE(self, out);
	return tom_font_create(self, out);
}

static HRESULT STDMETHODCALLTYPE range_set_font(ITextRange *iface, ITextFont *font)
{
	text_range *self = RANGE_SELF(iface);
	if (!font)
		return E_INVALIDARG;
	ALIVE(self);
	ITextFont *mine;
	HRESULT result = tom_font_create(self, &mine);
	if (FAILED(result))
		return result;
	result = ITextFont_SetDuplicate(mine, font);
	ITextFont_Release(mine);
	return result;
}

static HRESULT STDMETHODCALLTYPE range_get_para(ITextRange *iface, ITextPara **out)
{
	text_range *self = RANGE_SELF(iface);
	OUT_ALIVE(self, out);
	return tom_para_create(self, out);
}

static HRESULT STDMETHODCALLTYPE range_set_para(ITextRange *iface, ITextPara *para)
{
	(void)para;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE range_story_length(ITextRange *iface, LONG *out)
{
	text_range *self = RANGE_SELF(iface);
	OUT_ALIVE(self, out);
	*out = story_length(self->owner);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_story_type(ITextRange *iface, LONG *out)
{
	text_range *self = RANGE_SELF(iface);
	OUT_ALIVE(self, out);
	*out = tomUnknownStory;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_collapse(ITextRange *iface, LONG to_start)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	LONG start, end;
	tom_range_ends(self, &start, &end);
	if (start == end)
		return S_FALSE;
	if (to_start)
		end = start;
	else
		start = end;
	range_store(self, start, end);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_expand(ITextRange *iface, LONG unit, LONG *delta)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	if (delta)
		*delta = 0;
	if (unit != tomStory)
		return E_NOTIMPL;
	LONG start, end, length = story_length(self->owner);
	tom_range_ends(self, &start, &end);
	if (!start && end == length)
		return S_FALSE;
	if (delta)
		*delta = length - (end - start);
	range_store(self, 0, length);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_get_index(ITextRange *iface, LONG unit, LONG *out)
{
	(void)unit; (void)out;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE range_set_index(ITextRange *iface, LONG unit, LONG index, LONG extend)
{
	(void)unit; (void)index; (void)extend;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE range_set_range(ITextRange *iface, LONG active, LONG other)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	LONG start, end, length = story_length(self->owner);
	tom_range_ends(self, &start, &end);
	LONG low = active < other ? active : other, high = active < other ? other : active;
	if (low >= length && high >= length)
		low = high = length - 1;
	low = low < 0 ? 0 : low > length - 1 ? length - 1 : low;
	high = high < 0 ? 0 : high > length ? length : high;
	if (low == start && high == end)
		return S_FALSE;
	range_store(self, low, high);
	return S_OK;
}

static HRESULT compare_ranges(text_range *self, ITextRange *other, LONG *out, BOOL equal)
{
	if (out)
		*out = tomFalse;
	ALIVE(self);
	if (!other)
		return S_FALSE;
	if (self->is_selection) {
		ITextSelection *selection;
		if (FAILED(ITextRange_QueryInterface(other, &IID_ITextSelection, (void **)&selection)))
			return S_FALSE;
		ITextSelection_Release(selection);
	}
	LONG start, end, other_start, other_end;
	tom_range_ends(self, &start, &end);
	if (FAILED(ITextRange_GetStart(other, &other_start)) || FAILED(ITextRange_GetEnd(other, &other_end)))
		return S_FALSE;
	BOOL hit = equal ? start == other_start && end == other_end : start >= other_start && end <= other_end;
	if (out)
		*out = hit ? tomTrue : tomFalse;
	return hit ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE range_in_range(ITextRange *iface, ITextRange *other, LONG *out)
{
	return compare_ranges(RANGE_SELF(iface), other, out, FALSE);
}

static HRESULT STDMETHODCALLTYPE range_in_story(ITextRange *iface, ITextRange *other, LONG *out)
{
	(void)other; (void)out;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE range_is_equal(ITextRange *iface, ITextRange *other, LONG *out)
{
	return compare_ranges(RANGE_SELF(iface), other, out, TRUE);
}

static HRESULT STDMETHODCALLTYPE range_select(ITextRange *iface)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	LONG start, end;
	tom_range_ends(self, &start, &end);
	re_editor *editor = self->owner->editor;
	re_set_selection(editor, start, end);
	re_scroll_to_caret(editor);
	// Unlike scrolling to the caret, selecting shows all of the last line.
	POINT at;
	INT height;
	RECT view;
	re_position(editor, end, FALSE, &at, &height);
	re_view_rect(editor, &view);
	if (at.y + height > view.bottom)
		re_scroll_to(editor, editor->scroll_x, editor->scroll_y + at.y + height - view.bottom);
	re_update_caret(editor);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_start_of(ITextRange *iface, LONG unit, LONG extend, LONG *delta)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	if (delta)
		*delta = 0;
	LONG start, end;
	tom_range_ends(self, &start, &end);
	if (unit == tomCharacter) {
		if (start == end || extend)
			return S_FALSE;
		range_store(self, start, start);
		if (delta)
			*delta = -1;
		return S_OK;
	}
	if (unit != tomStory)
		return E_NOTIMPL;
	if (delta)
		*delta = -start;
	range_store(self, 0, extend ? end : 0);
	return start ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE range_end_of(ITextRange *iface, LONG unit, LONG extend, LONG *delta)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	if (delta)
		*delta = 0;
	LONG start, end, length = story_length(self->owner);
	tom_range_ends(self, &start, &end);
	if (unit == tomCharacter) {
		// A character's end cannot come before the first character.
		LONG target = end ? end : 1;
		if (extend && end)
			return S_FALSE;
		range_store(self, extend ? start : target, target);
		if (!extend && start == target)
			return S_FALSE;
		if (delta)
			*delta = 1;
		return S_OK;
	}
	if (unit != tomStory)
		return E_NOTIMPL;
	LONG target = length - 1;
	if (delta)
		*delta = target > end ? target - end : 0;
	range_store(self, extend ? start : target, target > end ? target : end);
	return end < target ? S_OK : S_FALSE;
}

static HRESULT finish_move(LONG moved, LONG *delta)
{
	if (delta)
		*delta = moved;
	return moved ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE range_move(ITextRange *iface, LONG unit, LONG count, LONG *delta)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	if (delta)
		*delta = 0;
	LONG start, end, length = story_length(self->owner);
	tom_range_ends(self, &start, &end);
	if (unit == tomStory) {
		LONG target = count > 0 ? length - 1 : 0;
		if (!count || (count > 0 && start == target && end == target) || (count < 0 && !start && !end))
			return S_FALSE;
		range_store(self, target, target);
		return finish_move(count > 0 ? 1 : -1, delta);
	}
	if (unit != tomCharacter)
		return E_NOTIMPL;
	if (!count)
		return S_FALSE;
	// A range that holds something collapses first, which takes one of the moves.
	LONG from = count > 0 ? end : start, moved = 0;
	if (start != end)
		moved += count > 0 ? 1 : -1;
	LONG left = count - moved;
	LONG target = from + left;
	if (start != end)
		target = from + left;
	target = target < 0 ? 0 : target > length - 1 ? length - 1 : target;
	moved += target - from;
	range_store(self, target, target);
	return finish_move(moved, delta);
}

static HRESULT STDMETHODCALLTYPE range_move_start(ITextRange *iface, LONG unit, LONG count, LONG *delta)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	if (delta)
		*delta = 0;
	LONG start, end, length = story_length(self->owner);
	tom_range_ends(self, &start, &end);
	if (unit == tomStory) {
		LONG target = count > 0 ? length - 1 : 0;
		if (!count || start == target)
			return S_FALSE;
		range_store(self, target, end < target ? target : end);
		return finish_move(count > 0 ? 1 : -1, delta);
	}
	if (unit != tomCharacter)
		return E_NOTIMPL;
	LONG target = start + count;
	target = target < 0 ? 0 : target > length - 1 ? length - 1 : target;
	range_store(self, target, end < target ? target : end);
	return finish_move(target - start, delta);
}

static HRESULT STDMETHODCALLTYPE range_move_end(ITextRange *iface, LONG unit, LONG count, LONG *delta)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	if (delta)
		*delta = 0;
	LONG start, end, length = story_length(self->owner);
	tom_range_ends(self, &start, &end);
	if (unit == tomStory) {
		if (!count)
			return S_FALSE;
		LONG low = count > 0 ? start : 0, high = count > 0 ? length : 0;
		if (count > 0 && end == length)
			return S_FALSE;
		if (count < 0 && !start && !end)
			return S_FALSE;
		range_store(self, low, high);
		return finish_move(count > 0 ? 1 : -1, delta);
	}
	if (unit != tomCharacter)
		return E_NOTIMPL;
	LONG target = end + count;
	target = target < 0 ? 0 : target > length ? length : target;
	range_store(self, start > target ? target : start, target);
	return finish_move(target - end, delta);
}

#define RANGE_VARIANT_STUB(name) \
	static HRESULT STDMETHODCALLTYPE name(ITextRange *iface, VARIANT *set, LONG count, LONG *delta) \
	{ \
		(void)set; (void)count; (void)delta; \
		ALIVE(RANGE_SELF(iface)); \
		return E_NOTIMPL; \
	}

RANGE_VARIANT_STUB(range_move_while)
RANGE_VARIANT_STUB(range_move_start_while)
RANGE_VARIANT_STUB(range_move_end_while)
RANGE_VARIANT_STUB(range_move_until)
RANGE_VARIANT_STUB(range_move_start_until)
RANGE_VARIANT_STUB(range_move_end_until)

#define RANGE_FIND_STUB(name) \
	static HRESULT STDMETHODCALLTYPE name(ITextRange *iface, BSTR text, LONG count, LONG flags, LONG *length) \
	{ \
		(void)text; (void)count; (void)flags; (void)length; \
		ALIVE(RANGE_SELF(iface)); \
		return E_NOTIMPL; \
	}

RANGE_FIND_STUB(range_find_text)
RANGE_FIND_STUB(range_find_text_start)
RANGE_FIND_STUB(range_find_text_end)

static HRESULT STDMETHODCALLTYPE range_delete(ITextRange *iface, LONG unit, LONG count, LONG *delta)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	if (delta)
		*delta = 0;
	if (unit != tomCharacter)
		return E_NOTIMPL;
	LONG start, end, limit = story_length(self->owner) - 1;
	tom_range_ends(self, &start, &end);
	end = end > limit ? limit : end;
	start = start > limit ? limit : start;
	LONG from = start, to = end;
	LONG done = 0;
	if (!count) {
		if (from == to)
			return S_FALSE;
		done = from == to ? 0 : 1;
	} else if (from != to) {
		done = count > 0 ? 1 : -1;
		count -= done;
		if (count > 0)
			to = end + count > limit ? limit : end + count;
		else if (count < 0)
			from = start + count < 0 ? 0 : start + count;
		done += count > 0 ? to - end : from - start;
	} else {
		if (count > 0)
			to = start + count > limit ? limit : start + count;
		else
			from = start + count < 0 ? 0 : start + count;
		done = count > 0 ? to - start : from - start;
	}
	if (from == to)
		return finish_move(0, delta);
	if (!range_replace(self, from, to, NULL, 0))
		return E_OUTOFMEMORY;
	range_store(self, from, from);
	return finish_move(done, delta);
}

// The clipboard messages work on the selection, so it stands in for the range during the call.
static HRESULT with_selection(text_range *self, UINT message)
{
	re_editor *editor = self->owner->editor;
	LONG start, end;
	tom_range_ends(self, &start, &end);
	editor->sel_start = start;
	editor->sel_end = end;
	re_message(editor, message, 0, 0, NULL);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_cut(ITextRange *iface, VARIANT *out)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	if (out)
		return E_NOTIMPL;
	LONG start, end;
	tom_range_ends(self, &start, &end);
	re_editor *editor = self->owner->editor;
	if (editor->read_only)
		return E_ACCESSDENIED;
	LONG limit = story_length(self->owner) - 1;
	end = end > limit ? limit : end;
	start = start > end ? end : start;
	INT sel_start = editor->sel_start, sel_end = editor->sel_end;
	editor->sel_start = start;
	editor->sel_end = end;
	re_copy_selected(editor);
	editor->sel_start = sel_start;
	editor->sel_end = sel_end;
	if (!range_replace(self, start, end, NULL, 0))
		return E_OUTOFMEMORY;
	range_store(self, start, start);
	return S_OK;
}

// With nothing in the range the whole story goes.
static HRESULT STDMETHODCALLTYPE range_copy(ITextRange *iface, VARIANT *out)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	if (out)
		return E_NOTIMPL;
	re_editor *editor = self->owner->editor;
	LONG start, end;
	tom_range_ends(self, &start, &end);
	INT sel_start = editor->sel_start, sel_end = editor->sel_end;
	editor->sel_start = start == end ? 0 : start;
	editor->sel_end = start == end ? re_text_length(editor) : end;
	re_copy_selected(editor);
	editor->sel_start = sel_start;
	editor->sel_end = sel_end;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_paste(ITextRange *iface, VARIANT *in, LONG format)
{
	text_range *self = RANGE_SELF(iface);
	(void)format;
	ALIVE(self);
	if (in)
		return E_NOTIMPL;
	return with_selection(self, WM_PASTE);
}

static HRESULT STDMETHODCALLTYPE range_can_paste(ITextRange *iface, VARIANT *in, LONG format, LONG *out)
{
	text_range *self = RANGE_SELF(iface);
	(void)format;
	ALIVE(self);
	if (in)
		return E_NOTIMPL;
	BOOL can = re_message(self->owner->editor, EM_CANPASTE, 0, 0, NULL) != 0;
	if (out)
		*out = can ? tomTrue : tomFalse;
	return can ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE range_can_edit(ITextRange *iface, LONG *out)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	BOOL can = !self->owner->editor->read_only;
	if (out)
		*out = can ? tomTrue : tomFalse;
	return can ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE range_change_case(ITextRange *iface, LONG type)
{
	(void)type;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE range_get_point(ITextRange *iface, LONG type, LONG *x, LONG *y)
{
	(void)type; (void)x; (void)y;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE range_set_point(ITextRange *iface, LONG x, LONG y, LONG type, LONG extend)
{
	(void)x; (void)y; (void)type; (void)extend;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE range_scroll(ITextRange *iface, LONG value)
{
	text_range *self = RANGE_SELF(iface);
	ALIVE(self);
	LONG start, end;
	tom_range_ends(self, &start, &end);
	re_editor *editor = self->owner->editor;
	POINT at;
	INT height;
	RECT view;
	switch (value) {
	case tomStart:
		re_position(editor, start, FALSE, &at, &height);
		break;
	case tomEnd:
		re_position(editor, end, FALSE, &at, &height);
		break;
	default:
		return E_NOTIMPL;
	}
	re_view_rect(editor, &view);
	re_scroll_to(editor, editor->scroll_x, editor->scroll_y + at.y - view.top);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_embedded(ITextRange *iface, IUnknown **out)
{
	(void)out;
	ALIVE(RANGE_SELF(iface));
	return E_NOTIMPL;
}

static const ITextRangeVtbl range_vtbl = {
	range_query, range_add_ref, range_release, range_type_info_count, range_type_info, range_ids, range_invoke,
	range_get_text, range_set_text, range_get_char, range_set_char, range_duplicate, range_formatted, range_set_formatted,
	range_get_start, range_set_start, range_get_end, range_set_end, range_get_font, range_set_font, range_get_para, range_set_para,
	range_story_length, range_story_type, range_collapse, range_expand, range_get_index, range_set_index, range_set_range,
	range_in_range, range_in_story, range_is_equal, range_select, range_start_of, range_end_of, range_move, range_move_start,
	range_move_end, range_move_while, range_move_start_while, range_move_end_while, range_move_until, range_move_start_until,
	range_move_end_until, range_find_text, range_find_text_start, range_find_text_end, range_delete, range_cut, range_copy,
	range_paste, range_can_paste, range_can_edit, range_change_case, range_get_point, range_set_point, range_scroll, range_embedded,
};

HRESULT tom_range_create(rich_ole *owner, LONG start, LONG end, ITextRange **out)
{
	text_range *self = re_calloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = (ITextRangeVtbl *)&range_vtbl;
	self->refs = 1;
	self->owner = owner;
	self->next = owner->ranges;
	owner->ranges = self;
	range_store(self, start, end);
	*out = &self->iface;
	return S_OK;
}

// ---- the selection ----

#define SEL_SELF(object) CONTAINING_RECORD((ITextRange *)(object), text_range, iface)

static HRESULT STDMETHODCALLTYPE sel_get_flags(ITextSelection *iface, LONG *out)
{
	text_range *self = SEL_SELF(iface);
	OUT_ALIVE(self, out);
	*out = self->owner->editor->sel_start <= self->owner->editor->sel_end ? 0 : tomSelStartActive;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE sel_set_flags(ITextSelection *iface, LONG flags)
{
	(void)flags;
	ALIVE(SEL_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE sel_get_type(ITextSelection *iface, LONG *out)
{
	text_range *self = SEL_SELF(iface);
	OUT_ALIVE(self, out);
	LONG start, end;
	tom_range_ends(self, &start, &end);
	*out = start == end ? tomSelectionIP : tomSelectionNormal;
	return S_OK;
}

#define SEL_MOVE_STUB(name) \
	static HRESULT STDMETHODCALLTYPE name(ITextSelection *iface, LONG unit, LONG count, LONG extend, LONG *delta) \
	{ \
		(void)unit; (void)count; (void)extend; (void)delta; \
		ALIVE(SEL_SELF(iface)); \
		return E_NOTIMPL; \
	}

SEL_MOVE_STUB(sel_move_left)
SEL_MOVE_STUB(sel_move_right)
SEL_MOVE_STUB(sel_move_up)
SEL_MOVE_STUB(sel_move_down)

static HRESULT STDMETHODCALLTYPE sel_home(ITextSelection *iface, LONG unit, LONG extend, LONG *delta)
{
	(void)unit; (void)extend; (void)delta;
	ALIVE(SEL_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE sel_end(ITextSelection *iface, LONG unit, LONG extend, LONG *delta)
{
	(void)unit; (void)extend; (void)delta;
	ALIVE(SEL_SELF(iface));
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE sel_type_text(ITextSelection *iface, BSTR text)
{
	text_range *self = SEL_SELF(iface);
	ALIVE(self);
	return range_set_text((ITextRange *)iface, text);
}

// A selection answers everything a range does, in the same places of its table.
static const struct
{
	ITextRangeVtbl range;
	HRESULT (STDMETHODCALLTYPE *get_flags)(ITextSelection *, LONG *);
	HRESULT (STDMETHODCALLTYPE *set_flags)(ITextSelection *, LONG);
	HRESULT (STDMETHODCALLTYPE *get_type)(ITextSelection *, LONG *);
	HRESULT (STDMETHODCALLTYPE *move_left)(ITextSelection *, LONG, LONG, LONG, LONG *);
	HRESULT (STDMETHODCALLTYPE *move_right)(ITextSelection *, LONG, LONG, LONG, LONG *);
	HRESULT (STDMETHODCALLTYPE *move_up)(ITextSelection *, LONG, LONG, LONG, LONG *);
	HRESULT (STDMETHODCALLTYPE *move_down)(ITextSelection *, LONG, LONG, LONG, LONG *);
	HRESULT (STDMETHODCALLTYPE *home)(ITextSelection *, LONG, LONG, LONG *);
	HRESULT (STDMETHODCALLTYPE *end)(ITextSelection *, LONG, LONG, LONG *);
	HRESULT (STDMETHODCALLTYPE *type_text)(ITextSelection *, BSTR);
} selection_vtbl = {
	{
		range_query, range_add_ref, range_release, range_type_info_count, range_type_info, range_ids, range_invoke,
		range_get_text, range_set_text, range_get_char, range_set_char, range_duplicate, range_formatted, range_set_formatted,
		range_get_start, range_set_start, range_get_end, range_set_end, range_get_font, range_set_font, range_get_para, range_set_para,
		range_story_length, range_story_type, range_collapse, range_expand, range_get_index, range_set_index, range_set_range,
		range_in_range, range_in_story, range_is_equal, range_select, range_start_of, range_end_of, range_move, range_move_start,
		range_move_end, range_move_while, range_move_start_while, range_move_end_while, range_move_until, range_move_start_until,
		range_move_end_until, range_find_text, range_find_text_start, range_find_text_end, range_delete, range_cut, range_copy,
		range_paste, range_can_paste, range_can_edit, range_change_case, range_get_point, range_set_point, range_scroll, range_embedded,
	},
	sel_get_flags, sel_set_flags, sel_get_type, sel_move_left, sel_move_right, sel_move_up, sel_move_down, sel_home, sel_end, sel_type_text,
};

HRESULT tom_selection_get(rich_ole *owner, ITextSelection **out)
{
	if (owner->selection) {
		ITextRange_AddRef(&owner->selection->iface);
		*out = (ITextSelection *)&owner->selection->iface;
		return S_OK;
	}
	text_range *self = re_calloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = (ITextRangeVtbl *)&selection_vtbl;
	self->refs = 2;
	self->owner = owner;
	self->is_selection = TRUE;
	owner->selection = self;
	*out = (ITextSelection *)&self->iface;
	return S_OK;
}

// ---- the document ----

static HRESULT STDMETHODCALLTYPE doc_query(ITextDocument *iface, REFIID iid, void **out)
{
	return IUnknown_QueryInterface(DOC_SELF(iface)->controlling, iid, out);
}

static ULONG STDMETHODCALLTYPE doc_add_ref(ITextDocument *iface)
{
	return IUnknown_AddRef(DOC_SELF(iface)->controlling);
}

static ULONG STDMETHODCALLTYPE doc_release(ITextDocument *iface)
{
	return IUnknown_Release(DOC_SELF(iface)->controlling);
}

static HRESULT STDMETHODCALLTYPE doc_type_info_count(ITextDocument *iface, UINT *out)
{
	(void)iface;
	*out = 1;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE doc_type_info(ITextDocument *iface, UINT index, LCID locale, ITypeInfo **out)
{
	(void)iface; (void)index; (void)locale;
	return tom_type_info(&IID_ITextDocument, out);
}

static HRESULT STDMETHODCALLTYPE doc_ids(ITextDocument *iface, REFIID iid, LPOLESTR *names, UINT count, LCID locale, DISPID *ids)
{
	(void)iface; (void)iid; (void)locale;
	return tom_get_ids(&IID_ITextDocument, names, count, ids);
}

static HRESULT STDMETHODCALLTYPE doc_invoke(ITextDocument *iface, DISPID id, REFIID iid, LCID locale, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *exception, UINT *error)
{
	(void)iid; (void)locale;
	return tom_invoke(iface, &IID_ITextDocument, id, flags, params, result, exception, error);
}

static HRESULT STDMETHODCALLTYPE doc_name(ITextDocument *iface, BSTR *out)
{
	(void)iface; (void)out;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE doc_selection(ITextDocument *iface, ITextSelection **out)
{
	rich_ole *self = DOC_SELF(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!self->editor)
		return CO_E_RELEASED;
	return tom_selection_get(self, out);
}

static HRESULT STDMETHODCALLTYPE doc_story_count(ITextDocument *iface, LONG *out)
{
	(void)iface;
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE doc_stories(ITextDocument *iface, ITextStoryRanges **out)
{
	(void)iface;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE doc_get_saved(ITextDocument *iface, LONG *out)
{
	rich_ole *self = DOC_SELF(iface);
	if (!out)
		return E_INVALIDARG;
	if (!self->editor)
		return CO_E_RELEASED;
	*out = self->editor->modified ? tomFalse : tomTrue;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE doc_set_saved(ITextDocument *iface, LONG value)
{
	rich_ole *self = DOC_SELF(iface);
	if (!self->editor)
		return CO_E_RELEASED;
	self->editor->modified = value == tomFalse;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE doc_get_tab(ITextDocument *iface, float *out)
{
	(void)iface; (void)out;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE doc_set_tab(ITextDocument *iface, float value)
{
	(void)iface; (void)value;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE doc_new(ITextDocument *iface)
{
	(void)iface;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE doc_open(ITextDocument *iface, VARIANT *file, LONG flags, LONG code_page)
{
	(void)iface; (void)file; (void)flags; (void)code_page;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE doc_save(ITextDocument *iface, VARIANT *file, LONG flags, LONG code_page)
{
	(void)iface; (void)file; (void)flags; (void)code_page;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE doc_freeze(ITextDocument *iface, LONG *out)
{
	rich_ole *self = DOC_SELF(iface);
	if (!self->editor)
		return CO_E_RELEASED;
	LONG count = ++self->frozen;
	if (out)
		*out = count;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE doc_unfreeze(ITextDocument *iface, LONG *out)
{
	rich_ole *self = DOC_SELF(iface);
	if (!self->editor)
		return CO_E_RELEASED;
	if (self->frozen)
		self->frozen--;
	if (!self->frozen)
		re_refresh(self->editor);
	if (out)
		*out = self->frozen;
	return self->frozen ? S_FALSE : S_OK;
}

static HRESULT STDMETHODCALLTYPE doc_begin_collection(ITextDocument *iface)
{
	(void)iface;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE doc_end_collection(ITextDocument *iface)
{
	(void)iface;
	return E_NOTIMPL;
}

// False throws the history away and stops keeping one; suspending only stops adding to it.
static BOOL undo_control(re_editor *editor, LONG command, HRESULT *result)
{
	*result = S_FALSE;
	switch (command) {
	case tomFalse:
		re_undo_clear(&editor->undo);
		re_undo_clear(&editor->redo);
		if (editor->undo_limit)
			editor->undo_limit_kept = editor->undo_limit;
		editor->undo_limit = 0;
		editor->undo_suspended = FALSE;
		*result = S_OK;
		return TRUE;
	case tomTrue:
		if (!editor->undo_limit)
			editor->undo_limit = editor->undo_limit_kept;
		return TRUE;
	case tomSuspend:
		editor->undo_suspended = editor->undo_limit != 0;
		return TRUE;
	case tomResume:
		editor->undo_suspended = FALSE;
		if (!editor->undo_limit)
			editor->undo_limit = editor->undo_limit_kept;
		return TRUE;
	}
	return FALSE;
}

static HRESULT do_steps(rich_ole *self, LONG count, LONG *out, BOOL (*step)(re_editor *))
{
	LONG done = 0;
	if (!self->editor)
		return CO_E_RELEASED;
	HRESULT result;
	if (step == re_undo && undo_control(self->editor, count, &result)) {
		if (out)
			*out = 0;
		return result;
	}
	for (; done < count && step(self->editor); done++)
		;
	if (out)
		*out = done;
	return done || !count ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE doc_undo(ITextDocument *iface, LONG count, LONG *out)
{
	return do_steps(DOC_SELF(iface), count, out, re_undo);
}

static HRESULT STDMETHODCALLTYPE doc_redo(ITextDocument *iface, LONG count, LONG *out)
{
	return do_steps(DOC_SELF(iface), count, out, re_redo);
}

static HRESULT STDMETHODCALLTYPE doc_range(ITextDocument *iface, LONG first, LONG last, ITextRange **out)
{
	rich_ole *self = DOC_SELF(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!self->editor)
		return CO_E_RELEASED;
	return tom_range_create(self, first, last, out);
}

static HRESULT STDMETHODCALLTYPE doc_range_point(ITextDocument *iface, LONG x, LONG y, ITextRange **out)
{
	(void)iface; (void)x; (void)y;
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

const ITextDocumentVtbl tom_document_vtbl = {
	doc_query, doc_add_ref, doc_release, doc_type_info_count, doc_type_info, doc_ids, doc_invoke,
	doc_name, doc_selection, doc_story_count, doc_stories, doc_get_saved, doc_set_saved, doc_get_tab, doc_set_tab,
	doc_new, doc_open, doc_save, doc_freeze, doc_unfreeze, doc_begin_collection, doc_end_collection, doc_undo, doc_redo,
	doc_range, doc_range_point,
};
