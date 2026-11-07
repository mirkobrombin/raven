/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include "user32_private.h"

/* The static control, from Microsoft's documentation of its styles and messages. */

typedef struct {
	HFONT font;
	HANDLE image;
} static_state;

static static_state *state_of(HWND window)
{
	return (static_state *)GetWindowLongPtrW(window, 0);
}

static UINT kind_of(DWORD style)
{
	return style & SS_TYPEMASK;
}

static BOOL shows_text(UINT kind)
{
	switch (kind) {
	case SS_LEFT:
	case SS_CENTER:
	case SS_RIGHT:
	case SS_SIMPLE:
	case SS_LEFTNOWORDWRAP:
		return TRUE;
	}
	return FALSE;
}

static HBRUSH control_brush(HWND window, HDC dc)
{
	HWND parent = GetParent(window);
	HBRUSH brush = (HBRUSH)SendMessageW(parent ? parent : window, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)window);
	return brush ? brush : (HBRUSH)DefWindowProcW(window, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)window);
}

static void image_size(HANDLE image, UINT kind, SIZE *size)
{
	BITMAP bitmap;
	size->cx = size->cy = 0;
	if (kind == SS_BITMAP) {
		if (GetObjectW(image, sizeof(bitmap), &bitmap)) {
			size->cx = bitmap.bmWidth;
			size->cy = bitmap.bmHeight;
		}
		return;
	}
	if (kind == SS_ICON) {
		ICONINFO info;
		if (GetIconInfo((HICON)image, &info)) {
			if (GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap)) {
				size->cx = bitmap.bmWidth;
				size->cy = info.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
			}
			if (info.hbmColor)
				DeleteObject(info.hbmColor);
			if (info.hbmMask)
				DeleteObject(info.hbmMask);
		}
	}
}

/* An icon or bitmap control sizes itself to its image, unless it centers the image or keeps its size. */
static void fit_to_image(HWND window, DWORD style, HANDLE image)
{
	SIZE size;
	if (!image || (style & (SS_CENTERIMAGE | SS_REALSIZECONTROL)))
		return;
	image_size(image, kind_of(style), &size);
	if (!size.cx || !size.cy)
		return;
	UINT flags = SWP_NOACTIVATE | SWP_NOZORDER;
	int x = 0, y = 0;
	if (style & SS_RIGHTJUST) {
		/* The lower right corner stays where it is and the other two sides move. */
		RECT rect;
		POINT corner;
		GetWindowRect(window, &rect);
		corner.x = rect.right;
		corner.y = rect.bottom;
		ScreenToClient(GetParent(window), &corner);
		x = corner.x - size.cx;
		y = corner.y - size.cy;
	} else
		flags |= SWP_NOMOVE;
	SetWindowPos(window, NULL, x, y, size.cx, size.cy, flags);
}

static void paint_text(HWND window, HDC dc, const RECT *client, DWORD style, static_state *state)
{
	UINT kind = kind_of(style), format;
	switch (kind) {
	case SS_CENTER:
		format = DT_CENTER | DT_WORDBREAK | DT_EXPANDTABS;
		break;
	case SS_RIGHT:
		format = DT_RIGHT | DT_WORDBREAK | DT_EXPANDTABS;
		break;
	case SS_SIMPLE:
		format = DT_LEFT | DT_SINGLELINE;
		break;
	case SS_LEFTNOWORDWRAP:
		format = DT_LEFT | DT_EXPANDTABS;
		break;
	default:
		format = DT_LEFT | DT_WORDBREAK | DT_EXPANDTABS;
		break;
	}
	if (style & SS_NOPREFIX)
		format |= DT_NOPREFIX;
	if (style & SS_EDITCONTROL)
		format |= DT_EDITCONTROL;
	switch (style & SS_ELLIPSISMASK) {
	case SS_ENDELLIPSIS:
		format = (format & ~(UINT)DT_WORDBREAK) | DT_END_ELLIPSIS | DT_SINGLELINE;
		break;
	case SS_PATHELLIPSIS:
		format = (format & ~(UINT)DT_WORDBREAK) | DT_PATH_ELLIPSIS | DT_SINGLELINE;
		break;
	case SS_WORDELLIPSIS:
		format = (format & ~(UINT)DT_WORDBREAK) | DT_WORD_ELLIPSIS | DT_SINGLELINE;
		break;
	}
	if (style & SS_CENTERIMAGE)
		format = (format & ~(UINT)DT_WORDBREAK) | DT_VCENTER | DT_SINGLELINE;
	HGDIOBJ previous = state->font ? SelectObject(dc, state->font) : NULL;
	HBRUSH brush = control_brush(window, dc);
	if (brush)
		FillRect(dc, client, brush);
	int length = GetWindowTextLengthW(window);
	if (length) {
		WCHAR small_text[256], *text = length < 256 ? small_text : HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * (length + 1));
		if (text) {
			GetWindowTextW(window, text, length + 1);
			if (kind != SS_SIMPLE && !IsWindowEnabled(window))
				SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
			RECT area = *client;
			SetBkMode(dc, TRANSPARENT);
			DrawTextW(dc, text, -1, &area, format);
			if (text != small_text)
				HeapFree(GetProcessHeap(), 0, text);
		}
	}
	if (previous)
		SelectObject(dc, previous);
}

static void paint_rectangle(HDC dc, const RECT *client, UINT kind, HWND window)
{
	int color;
	switch (kind) {
	case SS_BLACKRECT:
	case SS_BLACKFRAME:
		color = COLOR_3DDKSHADOW;
		break;
	case SS_GRAYRECT:
	case SS_GRAYFRAME:
		color = COLOR_3DSHADOW;
		break;
	default:
		color = COLOR_3DHIGHLIGHT;
		break;
	}
	if (kind == SS_BLACKFRAME || kind == SS_GRAYFRAME || kind == SS_WHITEFRAME)
		FrameRect(dc, client, GetSysColorBrush(color));
	else
		FillRect(dc, client, GetSysColorBrush(color));
}

static void paint_image(HWND window, HDC dc, const RECT *client, DWORD style, static_state *state)
{
	HBRUSH brush = control_brush(window, dc);
	UINT kind = kind_of(style);
	if (brush)
		FillRect(dc, client, brush);
	if (!state->image)
		return;
	if (kind == SS_ENHMETAFILE) {
		PlayEnhMetaFile(dc, (HENHMETAFILE)state->image, client);
		return;
	}
	SIZE size;
	image_size(state->image, kind, &size);
	int x = 0, y = 0, width = size.cx, height = size.cy;
	if (style & SS_CENTERIMAGE) {
		x = (client->right - size.cx) / 2;
		y = (client->bottom - size.cy) / 2;
	} else if (style & SS_REALSIZECONTROL) {
		width = client->right;
		height = client->bottom;
	}
	if (kind == SS_ICON) {
		DrawIconEx(dc, x, y, (HICON)state->image, width, height, 0, NULL, DI_NORMAL);
		return;
	}
	HDC memory = CreateCompatibleDC(dc);
	if (memory) {
		HGDIOBJ previous = SelectObject(memory, state->image);
		StretchBlt(dc, x, y, width, height, memory, 0, 0, size.cx, size.cy, SRCCOPY);
		SelectObject(memory, previous);
		DeleteDC(memory);
	}
}

static void paint(HWND window, HDC dc, static_state *state)
{
	DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
	UINT kind = kind_of(style);
	RECT client;
	GetClientRect(window, &client);
	switch (kind) {
	case SS_LEFT:
	case SS_CENTER:
	case SS_RIGHT:
	case SS_SIMPLE:
	case SS_LEFTNOWORDWRAP:
		paint_text(window, dc, &client, style, state);
		break;
	case SS_ICON:
	case SS_BITMAP:
	case SS_ENHMETAFILE:
		paint_image(window, dc, &client, style, state);
		break;
	case SS_BLACKRECT:
	case SS_GRAYRECT:
	case SS_WHITERECT:
	case SS_BLACKFRAME:
	case SS_GRAYFRAME:
	case SS_WHITEFRAME:
		paint_rectangle(dc, &client, kind, window);
		break;
	case SS_ETCHEDHORZ:
		DrawEdge(dc, &client, EDGE_ETCHED, BF_TOP | BF_BOTTOM);
		break;
	case SS_ETCHEDVERT:
		DrawEdge(dc, &client, EDGE_ETCHED, BF_LEFT | BF_RIGHT);
		break;
	case SS_ETCHEDFRAME:
		DrawEdge(dc, &client, EDGE_ETCHED, BF_RECT);
		break;
	case SS_OWNERDRAW: {
		DRAWITEMSTRUCT item = {ODT_STATIC, (UINT)GetDlgCtrlID(window), 0, ODA_DRAWENTIRE, IsWindowEnabled(window) ? 0 : ODS_DISABLED, window, dc, client, 0};
		control_brush(window, dc);
		SendMessageW(GetParent(window), WM_DRAWITEM, item.CtlID, (LPARAM)&item);
		break;
	}
	}
}

/* The name of a resource: a string, or 0xffff and an ordinal. */
static LPCWSTR resource_name(const WCHAR *title)
{
	return title[0] == 0xffff ? (LPCWSTR)(ULONG_PTR)title[1] : title;
}

static HANDLE load_named(HWND window, UINT kind, const WCHAR *title)
{
	HINSTANCE instance = (HINSTANCE)GetWindowLongPtrW(window, GWLP_HINSTANCE);
	if (!title || !*title)
		return NULL;
	LPCWSTR name = resource_name(title);
	if (kind == SS_ICON) {
		if ((DWORD)GetWindowLongPtrW(window, GWL_STYLE) & SS_REALSIZEIMAGE)
			return LoadImageW(instance, name, IMAGE_ICON, 0, 0, LR_SHARED);
		HANDLE image = LoadIconW(instance, name);
		if (!image)
			image = LoadCursorW(instance, name);
		return image ? image : LoadIconW(NULL, name);
	}
	return LoadImageW(instance, name, IMAGE_BITMAP, 0, 0, LR_SHARED);
}

static HANDLE replace_image(HWND window, static_state *state, HANDLE image)
{
	DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
	HANDLE previous = state->image;
	state->image = image;
	fit_to_image(window, style, image);
	InvalidateRect(window, NULL, TRUE);
	return previous;
}

LRESULT WINAPI RavenStaticProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	static_state *state = state_of(window);
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
	UINT kind = kind_of(style);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		/* The etched lines and the sunken border are this extended style's edge. */
		if ((style & SS_SUNKEN) || kind == SS_ETCHEDHORZ || kind == SS_ETCHEDVERT)
			SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_STATICEDGE);
		if (kind == SS_ICON || kind == SS_BITMAP) {
			/* The title names a resource here, which the default handling would take for text. */
			state->image = load_named(window, kind, create->lpszName);
			return DefWindowProcW(window, message, wparam, (LPARAM)&(CREATESTRUCTW){.lpszName = L""});
		}
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_CREATE:
		if (state->image)
			fit_to_image(window, style, state->image);
		return 0;
	case WM_NCDESTROY:
		HeapFree(GetProcessHeap(), 0, state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = wparam ? (HDC)wparam : BeginPaint(window, &ps);
		paint(window, dc, state);
		if (!wparam)
			EndPaint(window, &ps);
		return 0;
	}
	case WM_PRINTCLIENT:
		paint(window, (HDC)wparam, state);
		return 0;
	case WM_ERASEBKGND:
		/* Painting covers the whole control, and two passes would flicker. */
		return 1;
	case WM_ENABLE:
		if (style & SS_NOTIFY)
			SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), wparam ? STN_ENABLE : STN_DISABLE), (LPARAM)window);
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_NCHITTEST:
		return style & SS_NOTIFY ? HTCLIENT : HTTRANSPARENT;
	case WM_GETDLGCODE:
		return DLGC_STATIC;
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		if (lparam)
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_SETTEXT: {
		if (kind == SS_ICON || kind == SS_BITMAP) {
			replace_image(window, state, load_named(window, kind, (const WCHAR *)lparam));
			return TRUE;
		}
		LRESULT result = DefWindowProcW(window, message, wparam, lparam);
		InvalidateRect(window, NULL, TRUE);
		return result;
	}
	case WM_LBUTTONDOWN:
	case WM_NCLBUTTONDOWN:
		if (style & SS_NOTIFY)
			SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), STN_CLICKED), (LPARAM)window);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_LBUTTONDBLCLK:
	case WM_NCLBUTTONDBLCLK:
		if (style & SS_NOTIFY)
			SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), STN_DBLCLK), (LPARAM)window);
		return DefWindowProcW(window, message, wparam, lparam);
	case STM_GETICON:
		return kind == SS_ICON ? (LRESULT)state->image : 0;
	case STM_SETICON:
		return kind == SS_ICON ? (LRESULT)replace_image(window, state, (HANDLE)wparam) : 0;
	case STM_GETIMAGE:
		if ((wparam == IMAGE_ICON || wparam == IMAGE_CURSOR) ? kind != SS_ICON : wparam == IMAGE_BITMAP ? kind != SS_BITMAP : kind != SS_ENHMETAFILE)
			return 0;
		return (LRESULT)state->image;
	case STM_SETIMAGE:
		if ((wparam == IMAGE_ICON && kind == SS_ICON) || (wparam == IMAGE_BITMAP && kind == SS_BITMAP) || (wparam == IMAGE_ENHMETAFILE && kind == SS_ENHMETAFILE) ||
		    (wparam == IMAGE_CURSOR && kind == SS_ICON))
			return (LRESULT)replace_image(window, state, (HANDLE)lparam);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}
