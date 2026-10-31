/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_RE_HOST_H
#define RAVEN_RE_HOST_H

// The windowless interfaces in the documented method order. The toolkit's own header declares them for C++ only, and on
// 32-bit x86 Microsoft's text services use the member-function convention.
#ifdef __i386__
#define RE_THIS __attribute__((thiscall))
#else
#define RE_THIS
#endif

#define S_MSG_KEY_IGNORED MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_ITF, 0x201)

enum TXTBACKSTYLE { TXTBACK_TRANSPARENT = 0, TXTBACK_OPAQUE };
enum TXTHITRESULT { TXTHITRESULT_NOHIT = 0, TXTHITRESULT_TRANSPARENT, TXTHITRESULT_CLOSE, TXTHITRESULT_HIT };
enum TXTNATURALSIZE { TXTNS_FITTOCONTENT = 1, TXTNS_ROUNDTOLINE = 2 };
enum TXTVIEW { TXTVIEW_ACTIVE = 0, TXTVIEW_INACTIVE = 1 };

#define TXTBIT_RICHTEXT 1
#define TXTBIT_MULTILINE 2
#define TXTBIT_READONLY 4
#define TXTBIT_SHOWACCELERATOR 8
#define TXTBIT_USEPASSWORD 0x10
#define TXTBIT_HIDESELECTION 0x20
#define TXTBIT_SAVESELECTION 0x40
#define TXTBIT_AUTOWORDSEL 0x80
#define TXTBIT_VERTICAL 0x100
#define TXTBIT_SELBARCHANGE 0x200
#define TXTBIT_WORDWRAP 0x400
#define TXTBIT_ALLOWBEEP 0x800
#define TXTBIT_DISABLEDRAG 0x1000
#define TXTBIT_VIEWINSETCHANGE 0x2000
#define TXTBIT_BACKSTYLECHANGE 0x4000
#define TXTBIT_MAXLENGTHCHANGE 0x8000
#define TXTBIT_SCROLLBARCHANGE 0x10000
#define TXTBIT_CHARFORMATCHANGE 0x20000
#define TXTBIT_PARAFORMATCHANGE 0x40000
#define TXTBIT_EXTENTCHANGE 0x80000
#define TXTBIT_CLIENTRECTCHANGE 0x100000
#define TXTBIT_USECURRENTBKG 0x200000

typedef struct ITextHost ITextHost;
typedef struct ITextHost ITextHost2;
typedef struct ITextServices ITextServices;

typedef struct ITextHostVtbl
{
	HRESULT (STDMETHODCALLTYPE *QueryInterface)(ITextHost *, REFIID, void **);
	ULONG (STDMETHODCALLTYPE *AddRef)(ITextHost *);
	ULONG (STDMETHODCALLTYPE *Release)(ITextHost *);
	HDC (RE_THIS *TxGetDC)(ITextHost *);
	INT (RE_THIS *TxReleaseDC)(ITextHost *, HDC);
	BOOL (RE_THIS *TxShowScrollBar)(ITextHost *, INT, BOOL);
	BOOL (RE_THIS *TxEnableScrollBar)(ITextHost *, INT, INT);
	BOOL (RE_THIS *TxSetScrollRange)(ITextHost *, INT, LONG, INT, BOOL);
	BOOL (RE_THIS *TxSetScrollPos)(ITextHost *, INT, INT, BOOL);
	void (RE_THIS *TxInvalidateRect)(ITextHost *, LPCRECT, BOOL);
	void (RE_THIS *TxViewChange)(ITextHost *, BOOL);
	BOOL (RE_THIS *TxCreateCaret)(ITextHost *, HBITMAP, INT, INT);
	BOOL (RE_THIS *TxShowCaret)(ITextHost *, BOOL);
	BOOL (RE_THIS *TxSetCaretPos)(ITextHost *, INT, INT);
	BOOL (RE_THIS *TxSetTimer)(ITextHost *, UINT, UINT);
	void (RE_THIS *TxKillTimer)(ITextHost *, UINT);
	void (RE_THIS *TxScrollWindowEx)(ITextHost *, INT, INT, LPCRECT, LPCRECT, HRGN, LPRECT, UINT);
	void (RE_THIS *TxSetCapture)(ITextHost *, BOOL);
	void (RE_THIS *TxSetFocus)(ITextHost *);
	void (RE_THIS *TxSetCursor)(ITextHost *, HCURSOR, BOOL);
	BOOL (RE_THIS *TxScreenToClient)(ITextHost *, LPPOINT);
	BOOL (RE_THIS *TxClientToScreen)(ITextHost *, LPPOINT);
	HRESULT (RE_THIS *TxActivate)(ITextHost *, LONG *);
	HRESULT (RE_THIS *TxDeactivate)(ITextHost *, LONG);
	HRESULT (RE_THIS *TxGetClientRect)(ITextHost *, LPRECT);
	HRESULT (RE_THIS *TxGetViewInset)(ITextHost *, LPRECT);
	HRESULT (RE_THIS *TxGetCharFormat)(ITextHost *, const CHARFORMATW **);
	HRESULT (RE_THIS *TxGetParaFormat)(ITextHost *, const PARAFORMAT **);
	COLORREF (RE_THIS *TxGetSysColor)(ITextHost *, INT);
	HRESULT (RE_THIS *TxGetBackStyle)(ITextHost *, enum TXTBACKSTYLE *);
	HRESULT (RE_THIS *TxGetMaxLength)(ITextHost *, DWORD *);
	HRESULT (RE_THIS *TxGetScrollBars)(ITextHost *, DWORD *);
	HRESULT (RE_THIS *TxGetPasswordChar)(ITextHost *, WCHAR *);
	HRESULT (RE_THIS *TxGetAcceleratorPos)(ITextHost *, LONG *);
	HRESULT (RE_THIS *TxGetExtent)(ITextHost *, LPSIZEL);
	HRESULT (RE_THIS *OnTxCharFormatChange)(ITextHost *, const CHARFORMATW *);
	HRESULT (RE_THIS *OnTxParaFormatChange)(ITextHost *, const PARAFORMAT *);
	HRESULT (RE_THIS *TxGetPropertyBits)(ITextHost *, DWORD, DWORD *);
	HRESULT (RE_THIS *TxNotify)(ITextHost *, DWORD, void *);
	HIMC (RE_THIS *TxImmGetContext)(ITextHost *);
	void (RE_THIS *TxImmReleaseContext)(ITextHost *, HIMC);
	HRESULT (RE_THIS *TxGetSelectionBarWidth)(ITextHost *, LONG *);
} ITextHostVtbl;

struct ITextHost
{
	const ITextHostVtbl *lpVtbl;
};

#define ITextHost_Release(h) (h)->lpVtbl->Release(h)
#define ITextHost_AddRef(h) (h)->lpVtbl->AddRef(h)
#define ITextHost_TxGetDC(h) (h)->lpVtbl->TxGetDC(h)
#define ITextHost_TxReleaseDC(h, a) (h)->lpVtbl->TxReleaseDC(h, a)
#define ITextHost_TxSetScrollRange(h, a, b, c, d) (h)->lpVtbl->TxSetScrollRange(h, a, b, c, d)
#define ITextHost_TxSetScrollPos(h, a, b, c) (h)->lpVtbl->TxSetScrollPos(h, a, b, c)
#define ITextHost_TxInvalidateRect(h, a, b) (h)->lpVtbl->TxInvalidateRect(h, a, b)
#define ITextHost_TxCreateCaret(h, a, b, c) (h)->lpVtbl->TxCreateCaret(h, a, b, c)
#define ITextHost_TxShowCaret(h, a) (h)->lpVtbl->TxShowCaret(h, a)
#define ITextHost_TxSetCaretPos(h, a, b) (h)->lpVtbl->TxSetCaretPos(h, a, b)
#define ITextHost_TxGetClientRect(h, a) (h)->lpVtbl->TxGetClientRect(h, a)
#define ITextHost_TxNotify(h, a, b) (h)->lpVtbl->TxNotify(h, a, b)
#define ITextHost_TxGetPropertyBits(h, a, b) (h)->lpVtbl->TxGetPropertyBits(h, a, b)
#define ITextHost_TxGetCharFormat(h, a) (h)->lpVtbl->TxGetCharFormat(h, a)
#define ITextHost_TxGetParaFormat(h, a) (h)->lpVtbl->TxGetParaFormat(h, a)
#define ITextHost_TxGetMaxLength(h, a) (h)->lpVtbl->TxGetMaxLength(h, a)
#define ITextHost_TxGetScrollBars(h, a) (h)->lpVtbl->TxGetScrollBars(h, a)
#define ITextHost_TxGetPasswordChar(h, a) (h)->lpVtbl->TxGetPasswordChar(h, a)
#define ITextHost_TxGetBackStyle(h, a) (h)->lpVtbl->TxGetBackStyle(h, a)
#define ITextHost_TxGetViewInset(h, a) (h)->lpVtbl->TxGetViewInset(h, a)

STDAPI CreateTextServices(IUnknown *outer, ITextHost *host, IUnknown **out);

#endif
