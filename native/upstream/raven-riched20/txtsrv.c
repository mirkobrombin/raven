/* SPDX-License-Identifier: MIT */
#include <initguid.h>
#include "re.h"

extern const IID IID_ITextDocument2Old_;

// Exported data: the interface names callers read straight out of the library.
DEFINE_GUID(IID_ITextServices, 0x8d33f740, 0xcf58, 0x11ce, 0xa8, 0x9d, 0x00, 0xaa, 0x00, 0x6c, 0xad, 0xc5);
DEFINE_GUID(IID_ITextHost, 0x13e670f4, 0x1a5a, 0x11cf, 0xab, 0xeb, 0x00, 0xaa, 0x00, 0xb6, 0x5e, 0xa1);
DEFINE_GUID(IID_ITextDocument, 0x8cc497c0, 0xa1df, 0x11ce, 0x80, 0x98, 0x00, 0xaa, 0x00, 0x47, 0xbe, 0x5d);
DEFINE_GUID(IID_ITextDocument2Old_, 0x01c25500, 0x4268, 0x11d1, 0x88, 0x3a, 0x3c, 0x8b, 0x00, 0xc1, 0x00, 0x00);
DEFINE_GUID(IID_ITextRange, 0x8cc497c2, 0xa1df, 0x11ce, 0x80, 0x98, 0x00, 0xaa, 0x00, 0x47, 0xbe, 0x5d);
DEFINE_GUID(IID_ITextSelection, 0x8cc497c1, 0xa1df, 0x11ce, 0x80, 0x98, 0x00, 0xaa, 0x00, 0x47, 0xbe, 0x5d);
DEFINE_GUID(IID_ITextFont, 0x8cc497c3, 0xa1df, 0x11ce, 0x80, 0x98, 0x00, 0xaa, 0x00, 0x47, 0xbe, 0x5d);
DEFINE_GUID(IID_ITextPara, 0x8cc497c4, 0xa1df, 0x11ce, 0x80, 0x98, 0x00, 0xaa, 0x00, 0x47, 0xbe, 0x5d);
DEFINE_GUID(IID_ITextHost2, 0x13e670f5, 0x1a5a, 0x11cf, 0xab, 0xeb, 0x00, 0xaa, 0x00, 0xb6, 0x5e, 0xa1);

typedef struct text_services
{
	IUnknown inner;
	struct text_services_vtbl_holder *services_unused;
	const void *services_vtbl;
	IUnknown *outer;
	LONG refs;
	re_editor editor;
} text_services;

#define FROM_SERVICES(p) ((text_services *)((char *)(p) - offsetof(text_services, services_vtbl)))

typedef struct ITextServicesVtbl
{
	HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
	ULONG (STDMETHODCALLTYPE *AddRef)(void *);
	ULONG (STDMETHODCALLTYPE *Release)(void *);
	HRESULT (RE_THIS *TxSendMessage)(void *, UINT, WPARAM, LPARAM, LRESULT *);
	HRESULT (RE_THIS *TxDraw)(void *, DWORD, LONG, void *, DVTARGETDEVICE *, HDC, HDC, LPCRECTL, LPCRECTL, LPRECT, BOOL (CALLBACK *)(DWORD), DWORD, LONG);
	HRESULT (RE_THIS *TxGetHScroll)(void *, LONG *, LONG *, LONG *, LONG *, BOOL *);
	HRESULT (RE_THIS *TxGetVScroll)(void *, LONG *, LONG *, LONG *, LONG *, BOOL *);
	HRESULT (RE_THIS *OnTxSetCursor)(void *, DWORD, LONG, void *, DVTARGETDEVICE *, HDC, HDC, LPCRECT, INT, INT);
	HRESULT (RE_THIS *TxQueryHitPoint)(void *, DWORD, LONG, void *, DVTARGETDEVICE *, HDC, HDC, LPCRECT, INT, INT, DWORD *);
	HRESULT (RE_THIS *OnTxInPlaceActivate)(void *, LPCRECT);
	HRESULT (RE_THIS *OnTxInPlaceDeactivate)(void *);
	HRESULT (RE_THIS *OnTxUIActivate)(void *);
	HRESULT (RE_THIS *OnTxUIDeactivate)(void *);
	HRESULT (RE_THIS *TxGetText)(void *, BSTR *);
	HRESULT (RE_THIS *TxSetText)(void *, LPCWSTR);
	HRESULT (RE_THIS *TxGetCurTargetX)(void *, LONG *);
	HRESULT (RE_THIS *TxGetBaseLinePos)(void *, LONG *);
	HRESULT (RE_THIS *TxGetNaturalSize)(void *, DWORD, HDC, HDC, DVTARGETDEVICE *, DWORD, const SIZEL *, LONG *, LONG *);
	HRESULT (RE_THIS *TxGetDropTarget)(void *, IDropTarget **);
	HRESULT (RE_THIS *OnTxPropertyBitsChange)(void *, DWORD, DWORD);
	HRESULT (RE_THIS *TxGetCachedSize)(void *, DWORD *, DWORD *);
} ITextServicesVtbl;

static void read_host_settings(text_services *self, DWORD mask)
{
	re_editor *editor = &self->editor;
	ITextHost *host = editor->host;
	DWORD bits = 0;
	ITextHost_TxGetPropertyBits(host, mask, &bits);
	if (mask & TXTBIT_RICHTEXT)
		editor->text_mode = (editor->text_mode & ~(TM_RICHTEXT | TM_PLAINTEXT)) | (bits & TXTBIT_RICHTEXT ? TM_RICHTEXT : TM_PLAINTEXT);
	if (mask & TXTBIT_MULTILINE)
		editor->style = bits & TXTBIT_MULTILINE ? editor->style | ES_MULTILINE : editor->style & ~ES_MULTILINE;
	if (mask & TXTBIT_READONLY) {
		editor->read_only = (bits & TXTBIT_READONLY) != 0;
		editor->options = editor->read_only ? editor->options | ECO_READONLY : editor->options & ~ECO_READONLY;
	}
	if (mask & TXTBIT_HIDESELECTION)
		editor->hide_selection = (bits & TXTBIT_HIDESELECTION) != 0;
	if (mask & TXTBIT_WORDWRAP)
		editor->wrap = (bits & TXTBIT_WORDWRAP) != 0;
	if (mask & TXTBIT_USEPASSWORD) {
		WCHAR password = 0;
		if (bits & TXTBIT_USEPASSWORD)
			ITextHost_TxGetPasswordChar(host, &password);
		editor->password = password;
	}
	if (mask & TXTBIT_MAXLENGTHCHANGE) {
		DWORD length = 0;
		if (SUCCEEDED(ITextHost_TxGetMaxLength(host, &length)) && length)
			editor->limit = length > 0x7fffffff ? 0x7fffffff : (INT)length;
	}
	if (mask & TXTBIT_CHARFORMATCHANGE) {
		const CHARFORMATW *look = NULL;
		if (SUCCEEDED(ITextHost_TxGetCharFormat(host, &look)) && look) {
			CHARFORMAT2W format;
			if (re_format_from_user(&format, look, FALSE)) {
				re_doc_set_format(&editor->doc, 0, editor->doc.length, &format);
				re_format_merge(&editor->default_format, &format);
			}
		}
	}
	if (mask & TXTBIT_PARAFORMATCHANGE) {
		const PARAFORMAT *para = NULL;
		if (SUCCEEDED(ITextHost_TxGetParaFormat(host, &para)) && para && (para->cbSize == sizeof(PARAFORMAT) || para->cbSize == sizeof(PARAFORMAT2))) {
			PARAFORMAT2 format;
			memset(&format, 0, sizeof(format));
			memcpy(&format, para, para->cbSize);
			re_doc_set_para(&editor->doc, 0, editor->doc.length, &format);
		}
	}
}

static HRESULT query_inner(text_services *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualIID(iid, &IID_IUnknown))
		*out = &self->inner;
	else if (IsEqualIID(iid, &IID_ITextServices))
		*out = &self->services_vtbl;
	else if (IsEqualIID(iid, &IID_IRichEditOle) || IsEqualIID(iid, &IID_ITextDocument) || IsEqualIID(iid, &IID_ITextDocument2Old_)) {
		if (!self->editor.ole && FAILED(re_ole_create(&self->editor, self->outer, &self->editor.ole)))
			return E_OUTOFMEMORY;
		return IUnknown_QueryInterface(self->editor.ole, iid, out);
	}
	if (!*out)
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE inner_query(IUnknown *iface, REFIID iid, void **out)
{
	return query_inner(CONTAINING_RECORD(iface, text_services, inner), iid, out);
}

static ULONG STDMETHODCALLTYPE inner_add_ref(IUnknown *iface)
{
	return InterlockedIncrement(&CONTAINING_RECORD(iface, text_services, inner)->refs);
}

static ULONG STDMETHODCALLTYPE inner_release(IUnknown *iface)
{
	text_services *self = CONTAINING_RECORD(iface, text_services, inner);
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left) {
		re_editor_destroy(&self->editor);
		re_free(self);
	}
	return left;
}

static const IUnknownVtbl inner_vtbl = {inner_query, inner_add_ref, inner_release};

static HRESULT STDMETHODCALLTYPE services_query(void *iface, REFIID iid, void **out)
{
	text_services *self = FROM_SERVICES(iface);
	return IUnknown_QueryInterface(self->outer, iid, out);
}

static ULONG STDMETHODCALLTYPE services_add_ref(void *iface)
{
	text_services *self = FROM_SERVICES(iface);
	return IUnknown_AddRef(self->outer);
}

static ULONG STDMETHODCALLTYPE services_release(void *iface)
{
	text_services *self = FROM_SERVICES(iface);
	return IUnknown_Release(self->outer);
}

static HRESULT RE_THIS services_send(void *iface, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
	text_services *self = FROM_SERVICES(iface);
	re_editor *editor = &self->editor;
	BOOL handled = TRUE;
	LRESULT value = 0;
	if (message == WM_PAINT || message == WM_ERASEBKGND || message == WM_PRINTCLIENT) {
		handled = FALSE;
	} else {
		if (!editor->layout.count)
			re_layout_update(editor);
		value = re_message(editor, message, wparam, lparam, &handled);
	}
	if (result)
		*result = value;
	if (!handled)
		return S_FALSE;
	if (message == EM_SETTEXTMODE && FAILED((HRESULT)value))
		return S_OK;
	return S_OK;
}

static HRESULT RE_THIS services_draw(void *iface, DWORD aspect, LONG index, void *aspect_info, DVTARGETDEVICE *device, HDC draw, HDC target, LPCRECTL bounds, LPCRECTL meta, LPRECT update, BOOL (CALLBACK *cont)(DWORD), DWORD param, LONG view)
{
	(void)aspect; (void)index; (void)aspect_info; (void)device; (void)target; (void)meta; (void)cont; (void)param;
	text_services *self = FROM_SERVICES(iface);
	if (view == TXTVIEW_ACTIVE && re_ole_frozen(&self->editor))
		return E_UNEXPECTED;
	// Out of place the caller says where to draw; in place the host does.
	if (!self->editor.in_place && !bounds)
		return E_INVALIDARG;
	if (!self->editor.in_place && !draw)
		return E_FAIL;
	HDC host_dc = draw ? NULL : ITextHost_TxGetDC(self->editor.host);
	if (host_dc)
		draw = host_dc;
	if (!draw)
		return E_FAIL;
	if (bounds && !self->editor.in_place) {
		self->editor.client = *(const RECT *)bounds;
		self->editor.client_valid = TRUE;
	}
	if (!self->editor.layout.count)
		re_layout_update(&self->editor);
	ITextHost_TxNotify(self->editor.host, EN_UPDATE, NULL);
	RECT area;
	if (bounds)
		area = *(const RECT *)bounds;
	else if (update)
		area = *update;
	else
		re_view_rect(&self->editor, &area);
	re_paint(&self->editor, draw, &area);
	if (host_dc)
		ITextHost_TxReleaseDC(self->editor.host, host_dc);
	return S_OK;
}

static HRESULT RE_THIS services_h_scroll(void *iface, LONG *minimum, LONG *maximum, LONG *position, LONG *page, BOOL *enabled)
{
	re_editor *editor = &FROM_SERVICES(iface)->editor;
	RECT view;
	re_view_rect(editor, &view);
	if (minimum) *minimum = 0;
	if (maximum) *maximum = editor->layout.widest;
	if (position) *position = editor->scroll_x;
	if (page) *page = view.right - view.left;
	if (enabled) *enabled = editor->layout.widest > view.right - view.left;
	return S_OK;
}

static HRESULT RE_THIS services_v_scroll(void *iface, LONG *minimum, LONG *maximum, LONG *position, LONG *page, BOOL *enabled)
{
	re_editor *editor = &FROM_SERVICES(iface)->editor;
	RECT view;
	re_view_rect(editor, &view);
	if (minimum) *minimum = 0;
	if (maximum) *maximum = editor->layout.total_height > 0 ? editor->layout.total_height - 1 : 0;
	if (position) *position = editor->scroll_y;
	if (page) *page = view.bottom - view.top;
	if (enabled) *enabled = editor->layout.total_height > view.bottom - view.top;
	return S_OK;
}

static HRESULT RE_THIS services_set_cursor(void *iface, DWORD aspect, LONG index, void *info, DVTARGETDEVICE *device, HDC draw, HDC target, LPCRECT client, INT x, INT y)
{
	(void)iface; (void)aspect; (void)index; (void)info; (void)device; (void)draw; (void)target; (void)client; (void)x; (void)y;
	SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32513)));
	return S_OK;
}

static HRESULT RE_THIS services_hit(void *iface, DWORD aspect, LONG index, void *info, DVTARGETDEVICE *device, HDC draw, HDC target, LPCRECT client, INT x, INT y, DWORD *hit)
{
	(void)aspect; (void)index; (void)info; (void)device; (void)draw; (void)target;
	(void)iface;
	if (hit) {
		POINT point = {x, y};
		*hit = client && !PtInRect(client, point) ? TXTHITRESULT_NOHIT : TXTHITRESULT_HIT;
	}
	return S_OK;
}

static HRESULT RE_THIS services_deactivate(void *iface)
{
	FROM_SERVICES(iface)->editor.in_place = FALSE;
	return S_OK;
}

static HRESULT RE_THIS services_activate(void *iface, LPCRECT client)
{
	FROM_SERVICES(iface)->editor.in_place = TRUE;
	(void)client;
	re_editor *editor = &FROM_SERVICES(iface)->editor;
	re_layout_update(editor);
	re_update_scrollbars(editor);
	return S_OK;
}

static HRESULT RE_THIS services_no_op(void *iface)
{
	(void)iface;
	return S_OK;
}

static HRESULT RE_THIS services_get_text(void *iface, BSTR *out)
{
	re_editor *editor = &FROM_SERVICES(iface)->editor;
	INT length = re_text_length(editor);
	if (!out)
		return E_POINTER;
	if (!length) {
		*out = NULL;
		return S_OK;
	}
	INT size = re_count_crlf(editor, 0, length);
	*out = SysAllocStringLen(NULL, size);
	if (!*out)
		return E_OUTOFMEMORY;
	re_get_text_crlf(editor, 0, length, *out, size, TRUE);
	(*out)[size] = 0;
	return S_OK;
}

static HRESULT RE_THIS services_set_text(void *iface, LPCWSTR text)
{
	re_editor *editor = &FROM_SERVICES(iface)->editor;
	SETTEXTEX options = {ST_DEFAULT, 1200};
	BOOL handled;
	// A replaced document stays clean of selection and undo, whatever the mark looked like.
	editor->style |= ES_MULTILINE;
	re_message(editor, EM_SETTEXTEX, (WPARAM)&options, (LPARAM)(text ? text : L""), &handled);
	return S_OK;
}

static HRESULT RE_THIS services_target_x(void *iface, LONG *x)
{
	re_editor *editor = &FROM_SERVICES(iface)->editor;
	POINT at;
	if (!x)
		return E_INVALIDARG;
	re_position(editor, editor->sel_end, editor->caret_at_line_start, &at, NULL);
	*x = at.x;
	return S_OK;
}

static HRESULT RE_THIS services_baseline(void *iface, LONG *y)
{
	re_editor *editor = &FROM_SERVICES(iface)->editor;
	if (!y)
		return E_INVALIDARG;
	*y = editor->layout.count ? editor->layout.lines[0].ascent : 0;
	return S_OK;
}

static HRESULT RE_THIS services_natural_size(void *iface, DWORD aspect, HDC draw, HDC target, DVTARGETDEVICE *device, DWORD mode, const SIZEL *extent, LONG *width, LONG *height)
{
	(void)aspect; (void)target; (void)device; (void)mode; (void)extent;
	re_editor *editor = &FROM_SERVICES(iface)->editor;
	if (!width || !height)
		return E_INVALIDARG;
	re_layout layout;
	memset(&layout, 0, sizeof(layout));
	re_layout_document(editor, draw, &layout, *width, 1, 1);
	*height = layout.total_height;
	*width = layout.widest;
	re_layout_free(&layout);
	return S_OK;
}

static HRESULT RE_THIS services_drop_target(void *iface, IDropTarget **out)
{
	(void)iface;
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT RE_THIS services_bits_change(void *iface, DWORD mask, DWORD bits)
{
	(void)bits;
	text_services *self = FROM_SERVICES(iface);
	read_host_settings(self, mask);
	if (mask & (TXTBIT_WORDWRAP | TXTBIT_CHARFORMATCHANGE | TXTBIT_PARAFORMATCHANGE | TXTBIT_CLIENTRECTCHANGE | TXTBIT_USEPASSWORD))
		re_refresh(&self->editor);
	return S_OK;
}

static HRESULT RE_THIS services_cached_size(void *iface, DWORD *width, DWORD *height)
{
	(void)iface;
	if (width)
		*width = 0;
	if (height)
		*height = 0;
	return S_OK;
}

static const ITextServicesVtbl services_vtbl = {
	services_query, services_add_ref, services_release, services_send, services_draw, services_h_scroll, services_v_scroll,
	services_set_cursor, services_hit, services_activate, services_deactivate, services_no_op, services_no_op, services_get_text,
	services_set_text, services_target_x, services_baseline, services_natural_size, services_drop_target, services_bits_change,
	services_cached_size,
};

STDAPI CreateTextServices(IUnknown *outer, ITextHost *host, IUnknown **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!host)
		return E_POINTER;
	text_services *self = re_calloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->inner.lpVtbl = (IUnknownVtbl *)&inner_vtbl;
	self->services_vtbl = &services_vtbl;
	self->refs = 1;
	self->outer = outer ? outer : &self->inner;
	if (!re_editor_create(&self->editor)) {
		re_free(self);
		return E_OUTOFMEMORY;
	}
	self->editor.host = host;
	self->editor.options = 0;
	self->editor.limit = 32767;
	read_host_settings(self, TXTBIT_RICHTEXT | TXTBIT_MULTILINE | TXTBIT_READONLY | TXTBIT_USEPASSWORD | TXTBIT_HIDESELECTION | TXTBIT_WORDWRAP | TXTBIT_MAXLENGTHCHANGE | TXTBIT_CHARFORMATCHANGE | TXTBIT_PARAFORMATCHANGE);
	*out = &self->inner;
	return S_OK;
}
