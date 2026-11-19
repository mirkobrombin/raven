/* A data object put on the OLE clipboard, shaped like the one WPF's DataGrid copies: text, then "Csv" and "HTML Format", which it
   registers only as it lists them. Every format it lists ends up on the clipboard, whether it hands its memory over or keeps it, and
   the objects that keep their memory are released. A null object empties the clipboard. The exit code names the first check that
   failed. */
#define COBJMACROS
#include <windows.h>
#include <ole2.h>

static const WCHAR text[] = L"name\tcount";
static const char csv[] = "name,count\r\nraven,3\r\n";
static const char html[] = "Version:0.9\r\n<table><tr><td>raven</td></tr></table>";
static UINT csv_format, html_format;
static LONG keeper_refs;

static BOOL same_iid(REFIID left, REFIID right)
{
	const BYTE *a = (const BYTE *)left, *b = (const BYTE *)right;
	for (unsigned index = 0; index < sizeof(GUID); ++index)
		if (a[index] != b[index])
			return FALSE;
	return TRUE;
}

static BOOL same_bytes(const void *left, const void *right, SIZE_T size)
{
	const BYTE *a = left, *b = right;
	for (SIZE_T index = 0; index < size; ++index)
		if (a[index] != b[index])
			return FALSE;
	return TRUE;
}

/* The object that owns memory it hands out: the clipboard releases it once it has its copy. */
static HRESULT STDMETHODCALLTYPE keeper_query(IUnknown *self, REFIID iid, void **out)
{
	*out = same_iid(iid, &IID_IUnknown) ? self : NULL;
	if (!*out)
		return E_NOINTERFACE;
	InterlockedIncrement(&keeper_refs);
	return S_OK;
}
static ULONG STDMETHODCALLTYPE keeper_add_ref(IUnknown *self) { return InterlockedIncrement(&keeper_refs); }
static ULONG STDMETHODCALLTYPE keeper_release(IUnknown *self) { return InterlockedDecrement(&keeper_refs); }
static IUnknownVtbl keeper_vtbl = {keeper_query, keeper_add_ref, keeper_release};
static IUnknown keeper = {&keeper_vtbl};
static HGLOBAL kept;

static HGLOBAL global_of(const void *data, SIZE_T size)
{
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
	void *to = memory ? GlobalLock(memory) : NULL;
	if (!to)
		return NULL;
	for (SIZE_T index = 0; index < size; ++index)
		((BYTE *)to)[index] = ((const BYTE *)data)[index];
	GlobalUnlock(memory);
	return memory;
}

/* The formats, listed one at a time. */
typedef struct {
	IEnumFORMATETC face;
	ULONG next;
} Listing;

static UINT listed_format(ULONG index)
{
	return index == 0 ? CF_UNICODETEXT : index == 1 ? (csv_format = RegisterClipboardFormatW(L"Csv")) :
		index == 2 ? (html_format = RegisterClipboardFormatW(L"HTML Format")) : 0;
}

static HRESULT STDMETHODCALLTYPE listing_query(IEnumFORMATETC *self, REFIID iid, void **out) { *out = NULL; return E_NOINTERFACE; }
static ULONG STDMETHODCALLTYPE listing_add_ref(IEnumFORMATETC *self) { return 2; }
static ULONG STDMETHODCALLTYPE listing_release(IEnumFORMATETC *self) { return 1; }
static HRESULT STDMETHODCALLTYPE listing_next(IEnumFORMATETC *self, ULONG wanted, FORMATETC *out, ULONG *fetched)
{
	Listing *listing = (Listing *)self;
	ULONG count = 0;
	UINT format;
	while (count < wanted && (format = listed_format(listing->next))) {
		FORMATETC entry = {(CLIPFORMAT)format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
		out[count++] = entry;
		++listing->next;
	}
	if (fetched)
		*fetched = count;
	return count == wanted ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE listing_skip(IEnumFORMATETC *self, ULONG count) { ((Listing *)self)->next += count; return S_OK; }
static HRESULT STDMETHODCALLTYPE listing_reset(IEnumFORMATETC *self) { ((Listing *)self)->next = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE listing_clone(IEnumFORMATETC *self, IEnumFORMATETC **out) { *out = NULL; return E_NOTIMPL; }
static IEnumFORMATETCVtbl listing_vtbl = {listing_query, listing_add_ref, listing_release, listing_next, listing_skip, listing_reset,
	listing_clone};
static Listing listing = {{&listing_vtbl}, 0};

static HRESULT STDMETHODCALLTYPE data_query(IDataObject *self, REFIID iid, void **out)
{
	*out = same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_IDataObject) ? self : NULL;
	return *out ? S_OK : E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE data_add_ref(IDataObject *self) { return 2; }
static ULONG STDMETHODCALLTYPE data_release(IDataObject *self) { return 1; }

/* Text and HTML are handed over; the CSV stays the object's, as memory it keeps. */
static HRESULT STDMETHODCALLTYPE data_get(IDataObject *self, FORMATETC *format, STGMEDIUM *medium)
{
	if (!(format->tymed & TYMED_HGLOBAL))
		return DV_E_TYMED;
	medium->tymed = TYMED_HGLOBAL;
	medium->pUnkForRelease = NULL;
	if (format->cfFormat == CF_UNICODETEXT)
		medium->hGlobal = global_of(text, sizeof text);
	else if (csv_format && format->cfFormat == csv_format) {
		if (!kept)
			kept = global_of(csv, sizeof csv);
		medium->hGlobal = kept;
		medium->pUnkForRelease = &keeper;
		InterlockedIncrement(&keeper_refs);
	} else if (html_format && format->cfFormat == html_format)
		medium->hGlobal = global_of(html, sizeof html);
	else
		return DV_E_FORMATETC;
	return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE data_get_here(IDataObject *self, FORMATETC *format, STGMEDIUM *medium) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE data_query_get(IDataObject *self, FORMATETC *format) { return S_OK; }
static HRESULT STDMETHODCALLTYPE data_canonical(IDataObject *self, FORMATETC *format, FORMATETC *out) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE data_set(IDataObject *self, FORMATETC *format, STGMEDIUM *medium, BOOL release) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE data_enum(IDataObject *self, DWORD direction, IEnumFORMATETC **out)
{
	listing.next = 0;
	*out = direction == DATADIR_GET ? &listing.face : NULL;
	return *out ? S_OK : E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE data_advise(IDataObject *self, FORMATETC *f, DWORD a, IAdviseSink *s, DWORD *c) { return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE data_unadvise(IDataObject *self, DWORD c) { return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE data_advisers(IDataObject *self, IEnumSTATDATA **out) { return OLE_E_ADVISENOTSUPPORTED; }
static IDataObjectVtbl data_vtbl = {data_query, data_add_ref, data_release, data_get, data_get_here, data_query_get, data_canonical,
	data_set, data_enum, data_advise, data_unadvise, data_advisers};
static IDataObject data = {&data_vtbl};

/* The clipboard holds `size` bytes equal to `expected` in `format`. */
static BOOL holds(UINT format, const void *expected, SIZE_T size)
{
	HANDLE memory = GetClipboardData(format);
	const void *bytes = memory ? GlobalLock(memory) : NULL;
	BOOL same = bytes && GlobalSize(memory) >= size && same_bytes(bytes, expected, size);
	if (bytes)
		GlobalUnlock(memory);
	return same;
}

unsigned int raven_entry(void)
{
	OleInitialize(NULL);
	if (OleSetClipboard(&data) != S_OK)
		return 1;
	if (!csv_format || !html_format)
		return 2;
	if (!OpenClipboard(NULL))
		return 3;
	if (!holds(CF_UNICODETEXT, text, sizeof text))
		return 4;
	if (!holds(csv_format, csv, sizeof csv))
		return 5;
	if (!holds(html_format, html, sizeof html))
		return 6;
	/* The clipboard took its own copy of the CSV the object keeps, and gave the object back its reference. */
	if (GetClipboardData(csv_format) == kept || keeper_refs != 0)
		return 7;
	CloseClipboard();
	/* The OLE clipboard reads the CSV back, as a program exporting the grid does. */
	IDataObject *current = NULL;
	FORMATETC wanted = {(CLIPFORMAT)csv_format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	STGMEDIUM medium;
	if (OleGetClipboard(&current) != S_OK || !current || IDataObject_GetData(current, &wanted, &medium) != S_OK ||
		medium.tymed != TYMED_HGLOBAL)
		return 8;
	const void *bytes = GlobalLock(medium.hGlobal);
	if (!bytes || !same_bytes(bytes, csv, sizeof csv))
		return 9;
	GlobalUnlock(medium.hGlobal);
	ReleaseStgMedium(&medium);
	IDataObject_Release(current);
	/* No object empties the clipboard. */
	if (OleSetClipboard(NULL) != S_OK || !OpenClipboard(NULL))
		return 10;
	if (CountClipboardFormats() != 0)
		return 11;
	CloseClipboard();
	GlobalFree(kept);
	return 0;
}
