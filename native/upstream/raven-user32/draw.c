/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>

/* The drawing helpers of user32, from Microsoft's documentation of DrawEdge, DrawFrameControl and DrawState. Everything is painted with brushes and small
 * shapes, so nothing depends on a symbol font being installed. */

static void fill(HDC dc, int left, int top, int right, int bottom, int color)
{
	RECT area = {left, top, right, bottom};
	FillRect(dc, &area, GetSysColorBrush(color));
}

static void fill_solid(HDC dc, int left, int top, int right, int bottom, COLORREF color)
{
	HBRUSH brush = CreateSolidBrush(color);
	RECT area = {left, top, right, bottom};
	if (brush) {
		FillRect(dc, &area, brush);
		DeleteObject(brush);
	}
}

/* One ring of an edge: top and left in one color, bottom and right in another, on the sides asked for. */
static void edge_ring(HDC dc, RECT *area, UINT sides, int first, int second)
{
	if (sides & BF_TOP)
		fill(dc, area->left, area->top, area->right, area->top + 1, first);
	if (sides & BF_LEFT)
		fill(dc, area->left, area->top, area->left + 1, area->bottom, first);
	if (sides & BF_BOTTOM)
		fill(dc, area->left, area->bottom - 1, area->right, area->bottom, second);
	if (sides & BF_RIGHT)
		fill(dc, area->right - 1, area->top, area->right, area->bottom, second);
	if (sides & BF_TOP)
		area->top++;
	if (sides & BF_LEFT)
		area->left++;
	if (sides & BF_BOTTOM)
		area->bottom--;
	if (sides & BF_RIGHT)
		area->right--;
}

static void diagonal_edge(HDC dc, const RECT *area, UINT edge, UINT flags)
{
	int color = edge & (BDR_RAISEDOUTER | BDR_RAISEDINNER) ? COLOR_3DSHADOW : COLOR_3DHIGHLIGHT;
	HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(flags & BF_MONO ? COLOR_WINDOWFRAME : color));
	HGDIOBJ previous = SelectObject(dc, pen);
	POINT position;
	BOOL rising = (flags & (BF_DIAGONAL_ENDTOPRIGHT & ~BF_DIAGONAL)) == (BF_TOP | BF_RIGHT) || (flags & BF_BOTTOM) && (flags & BF_LEFT);
	MoveToEx(dc, area->left, rising ? area->bottom - 1 : area->top, &position);
	LineTo(dc, area->right, rising ? area->top - 1 : area->bottom);
	MoveToEx(dc, position.x, position.y, NULL);
	SelectObject(dc, previous);
	DeleteObject(pen);
}

BOOL WINAPI DrawEdge(HDC dc, LPRECT rect, UINT edge, UINT flags)
{
	RECT area = *rect;
	if (flags & BF_DIAGONAL) {
		diagonal_edge(dc, rect, edge, flags);
		return TRUE;
	}
	if (flags & (BF_MONO | BF_FLAT)) {
		int color = flags & BF_MONO ? COLOR_WINDOWFRAME : COLOR_3DSHADOW;
		if (edge & (BDR_RAISEDOUTER | BDR_SUNKENOUTER))
			edge_ring(dc, &area, flags, color, color);
		if (edge & (BDR_RAISEDINNER | BDR_SUNKENINNER))
			edge_ring(dc, &area, flags, flags & BF_MONO ? COLOR_WINDOW : COLOR_3DFACE, flags & BF_MONO ? COLOR_WINDOW : COLOR_3DFACE);
	} else {
		BOOL soft = flags & BF_SOFT;
		if (edge & BDR_RAISEDOUTER)
			edge_ring(dc, &area, flags, soft ? COLOR_3DHIGHLIGHT : COLOR_3DLIGHT, COLOR_3DDKSHADOW);
		else if (edge & BDR_SUNKENOUTER)
			edge_ring(dc, &area, flags, soft ? COLOR_3DDKSHADOW : COLOR_3DSHADOW, COLOR_3DHIGHLIGHT);
		if (edge & BDR_RAISEDINNER)
			edge_ring(dc, &area, flags, soft ? COLOR_3DLIGHT : COLOR_3DHIGHLIGHT, COLOR_3DSHADOW);
		else if (edge & BDR_SUNKENINNER)
			edge_ring(dc, &area, flags, soft ? COLOR_3DSHADOW : COLOR_3DDKSHADOW, COLOR_3DLIGHT);
	}
	if (flags & BF_MIDDLE)
		FillRect(dc, &area, GetSysColorBrush(flags & BF_MONO ? COLOR_WINDOW : COLOR_3DFACE));
	if (flags & BF_ADJUST)
		*rect = area;
	return TRUE;
}

BOOL WINAPI InvertRect(HDC dc, const RECT *rect)
{
	return PatBlt(dc, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, DSTINVERT);
}

/* A dotted rectangle in XOR, so that a second call removes it. */
BOOL WINAPI DrawFocusRect(HDC dc, const RECT *rect)
{
	static const WORD dots[8] = {0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55};
	HBITMAP pattern = CreateBitmap(8, 8, 1, 1, dots);
	HBRUSH brush = pattern ? CreatePatternBrush(pattern) : NULL;
	BOOL done = FALSE;
	if (brush) {
		COLORREF text = SetTextColor(dc, RGB(0, 0, 0)), back = SetBkColor(dc, RGB(255, 255, 255));
		HGDIOBJ previous = SelectObject(dc, brush);
		int width = rect->right - rect->left, height = rect->bottom - rect->top;
		PatBlt(dc, rect->left, rect->top, width, 1, PATINVERT);
		PatBlt(dc, rect->left, rect->bottom - 1, width, 1, PATINVERT);
		if (height > 2) {
			PatBlt(dc, rect->left, rect->top + 1, 1, height - 2, PATINVERT);
			PatBlt(dc, rect->right - 1, rect->top + 1, 1, height - 2, PATINVERT);
		}
		SelectObject(dc, previous);
		SetTextColor(dc, text);
		SetBkColor(dc, back);
		done = TRUE;
	}
	if (brush)
		DeleteObject(brush);
	if (pattern)
		DeleteObject(pattern);
	return done;
}

BOOL WINAPI FlashWindow(HWND window, BOOL invert)
{
	FLASHWINFO info = {sizeof(info), window, invert ? FLASHW_ALL : FLASHW_STOP, 1, 0};
	return FlashWindowEx(&info);
}

/* A filled triangle pointing one way, centered in an area, sized to it. */
static void triangle(HDC dc, const RECT *area, int direction, COLORREF color)
{
	int width = area->right - area->left, height = area->bottom - area->top;
	int size = (width < height ? width : height) / 3;
	if (size < 2)
		size = 2;
	int middle_x = area->left + width / 2, middle_y = area->top + height / 2;
	for (int row = 0; row < size; row++) {
		int span = row;
		switch (direction) {
		case DFCS_SCROLLUP:
			fill_solid(dc, middle_x - span, middle_y - size / 2 + row, middle_x + span + 1, middle_y - size / 2 + row + 1, color);
			break;
		case DFCS_SCROLLDOWN:
			fill_solid(dc, middle_x - span, middle_y + size / 2 - row, middle_x + span + 1, middle_y + size / 2 - row + 1, color);
			break;
		case DFCS_SCROLLLEFT:
			fill_solid(dc, middle_x - size / 2 + row, middle_y - span, middle_x - size / 2 + row + 1, middle_y + span + 1, color);
			break;
		default:
			fill_solid(dc, middle_x + size / 2 - row, middle_y - span, middle_x + size / 2 - row + 1, middle_y + span + 1, color);
			break;
		}
	}
}

static void check_mark(HDC dc, const RECT *area, COLORREF color)
{
	int width = area->right - area->left, height = area->bottom - area->top;
	int unit = (width < height ? width : height) / 7;
	if (unit < 1)
		unit = 1;
	int x = area->left + (width - 7 * unit) / 2, y = area->top + (height - 7 * unit) / 2;
	/* Two strokes: down to the corner, then up to the right. */
	for (int step = 0; step < 3; step++)
		fill_solid(dc, x + (step + 0) * unit, y + (step + 2) * unit, x + (step + 1) * unit, y + (step + 5) * unit, color);
	for (int step = 0; step < 4; step++)
		fill_solid(dc, x + (step + 3) * unit, y + (3 - step) * unit, x + (step + 4) * unit, y + (6 - step) * unit, color);
}

static void x_mark(HDC dc, const RECT *area, COLORREF color)
{
	int width = area->right - area->left, height = area->bottom - area->top;
	int size = (width < height ? width : height) * 4 / 7, thick = size / 6 + 1;
	int x = area->left + (width - size) / 2, y = area->top + (height - size) / 2;
	for (int step = 0; step < size; step++) {
		fill_solid(dc, x + step, y + step, x + step + thick, y + step + 1, color);
		fill_solid(dc, x + size - step - thick, y + step, x + size - step, y + step + 1, color);
	}
}

static void pushed_shift(RECT *area, UINT state)
{
	if (state & DFCS_PUSHED)
		OffsetRect(area, 1, 1);
}

static COLORREF glyph_color(UINT state)
{
	return GetSysColor(state & DFCS_INACTIVE ? COLOR_GRAYTEXT : COLOR_BTNTEXT);
}

/* The face of a push-style frame: flat, mono or raised, sunken when pushed. */
static void button_face(HDC dc, RECT *area, UINT state)
{
	UINT flags = BF_RECT | BF_MIDDLE | BF_ADJUST | (state & DFCS_FLAT ? BF_FLAT : 0) | (state & DFCS_MONO ? BF_MONO : 0);
	if (state & (DFCS_PUSHED | DFCS_CHECKED))
		DrawEdge(dc, area, state & DFCS_FLAT ? BDR_SUNKENOUTER : EDGE_SUNKEN, flags);
	else
		DrawEdge(dc, area, state & DFCS_FLAT ? BDR_RAISEDINNER : EDGE_RAISED, flags);
	if (state & DFCS_CHECKED && !(state & DFCS_PUSHED))
		FillRect(dc, area, GetSysColorBrush(COLOR_3DLIGHT));
}

static void circle(HDC dc, int left, int top, int right, int bottom, COLORREF color)
{
	HBRUSH brush = CreateSolidBrush(color);
	HPEN pen = CreatePen(PS_SOLID, 1, color);
	HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
	Ellipse(dc, left, top, right, bottom);
	SelectObject(dc, old_brush);
	SelectObject(dc, old_pen);
	DeleteObject(brush);
	DeleteObject(pen);
}

static void check_box(HDC dc, const RECT *rect, UINT state)
{
	int width = rect->right - rect->left, height = rect->bottom - rect->top;
	int size = width < height ? width : height;
	int x = rect->left + (width - size) / 2, y = rect->top + (height - size) / 2;
	RECT box = {x, y, x + size, y + size};
	BOOL gray_face = (state & (DFCS_PUSHED | DFCS_INACTIVE));
	DrawEdge(dc, &box, state & DFCS_FLAT ? BDR_SUNKENOUTER : EDGE_SUNKEN, BF_RECT | BF_ADJUST | (state & DFCS_FLAT ? BF_FLAT : 0) | (state & DFCS_MONO ? BF_MONO : 0));
	FillRect(dc, &box, GetSysColorBrush(gray_face ? COLOR_3DFACE : COLOR_WINDOW));
	if (state & DFCS_CHECKED)
		check_mark(dc, &box, GetSysColor((state & 0x8) == DFCS_BUTTON3STATE ? COLOR_3DSHADOW : (state & DFCS_INACTIVE ? COLOR_3DSHADOW : COLOR_WINDOWTEXT)));
}

static void radio(HDC dc, const RECT *rect, UINT state)
{
	int width = rect->right - rect->left, height = rect->bottom - rect->top;
	int size = (width < height ? width : height) & ~1;
	int x = rect->left + (width - size) / 2, y = rect->top + (height - size) / 2;
	UINT kind = state & 0xff;
	if (kind == DFCS_BUTTONRADIOMASK) {
		circle(dc, x, y, x + size, y + size, RGB(0, 0, 0));
		return;
	}
	circle(dc, x, y, x + size, y + size, GetSysColor(COLOR_3DSHADOW));
	circle(dc, x + 1, y + 1, x + size, y + size, GetSysColor(COLOR_3DHIGHLIGHT));
	circle(dc, x + 1, y + 1, x + size - 1, y + size - 1, GetSysColor(COLOR_3DDKSHADOW));
	circle(dc, x + 2, y + 2, x + size - 1, y + size - 1, GetSysColor(COLOR_3DLIGHT));
	circle(dc, x + 2, y + 2, x + size - 2, y + size - 2, GetSysColor(state & (DFCS_PUSHED | DFCS_INACTIVE) ? COLOR_3DFACE : COLOR_WINDOW));
	if (state & DFCS_CHECKED) {
		int inset = size * 5 / 13;
		circle(dc, x + inset, y + inset, x + size - inset, y + size - inset, GetSysColor(state & DFCS_INACTIVE ? COLOR_3DSHADOW : COLOR_WINDOWTEXT));
	}
}

static BOOL draw_button(HDC dc, RECT *rect, UINT state)
{
	switch (state & 0x1f) {
	case DFCS_BUTTONPUSH:
		button_face(dc, rect, state);
		return TRUE;
	case DFCS_BUTTONCHECK:
	case DFCS_BUTTON3STATE:
		check_box(dc, rect, state);
		return TRUE;
	case DFCS_BUTTONRADIO:
	case DFCS_BUTTONRADIOIMAGE:
	case DFCS_BUTTONRADIOMASK:
		radio(dc, rect, state);
		return TRUE;
	}
	return FALSE;
}

static BOOL draw_caption(HDC dc, RECT *rect, UINT state)
{
	RECT area = *rect;
	button_face(dc, &area, state);
	pushed_shift(&area, state);
	int width = area.right - area.left, height = area.bottom - area.top;
	int size = (width < height ? width : height) / 2, thick = size / 6 + 1;
	int x = area.left + (width - size) / 2, y = area.top + (height - size) / 2;
	COLORREF color = glyph_color(state);
	switch (state & 0xf) {
	case DFCS_CAPTIONCLOSE:
		x_mark(dc, &area, color);
		return TRUE;
	case DFCS_CAPTIONMIN:
		fill_solid(dc, x, y + size - thick, x + size * 2 / 3, y + size, color);
		return TRUE;
	case DFCS_CAPTIONMAX:
		fill_solid(dc, x, y, x + size, y + thick + 1, color);
		fill_solid(dc, x, y, x + 1, y + size, color);
		fill_solid(dc, x + size - 1, y, x + size, y + size, color);
		fill_solid(dc, x, y + size - 1, x + size, y + size, color);
		return TRUE;
	case DFCS_CAPTIONRESTORE: {
		int shift = size / 3;
		fill_solid(dc, x + shift, y, x + size, y + thick, color);
		fill_solid(dc, x + size - 1, y, x + size, y + size - shift, color);
		fill_solid(dc, x, y + shift, x + size - shift, y + shift + thick, color);
		fill_solid(dc, x, y + shift, x + 1, y + size, color);
		fill_solid(dc, x + size - shift - 1, y + shift, x + size - shift, y + size, color);
		fill_solid(dc, x, y + size - 1, x + size - shift, y + size, color);
		return TRUE;
	}
	case DFCS_CAPTIONHELP: {
		HFONT font = CreateFontW(-height * 3 / 4, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Tahoma");
		HGDIOBJ previous = SelectObject(dc, font);
		int mode = SetBkMode(dc, TRANSPARENT);
		COLORREF text = SetTextColor(dc, color);
		DrawTextW(dc, L"?", 1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		SetTextColor(dc, text);
		SetBkMode(dc, mode);
		SelectObject(dc, previous);
		DeleteObject(font);
		return TRUE;
	}
	}
	return FALSE;
}

static BOOL draw_menu(HDC dc, RECT *rect, UINT state)
{
	/* A menu glyph is black on white, for the caller to use as a mask. */
	FillRect(dc, rect, GetStockObject(WHITE_BRUSH));
	COLORREF black = RGB(0, 0, 0);
	switch (state & 0xf) {
	case DFCS_MENUARROW:
		triangle(dc, rect, DFCS_SCROLLRIGHT, black);
		return TRUE;
	case DFCS_MENUARROWRIGHT:
		triangle(dc, rect, DFCS_SCROLLLEFT, black);
		return TRUE;
	case DFCS_MENUCHECK:
		check_mark(dc, rect, black);
		return TRUE;
	case DFCS_MENUBULLET: {
		int width = rect->right - rect->left, height = rect->bottom - rect->top;
		int size = (width < height ? width : height) / 3 + 1;
		int x = rect->left + (width - size) / 2, y = rect->top + (height - size) / 2;
		circle(dc, x, y, x + size, y + size, black);
		return TRUE;
	}
	}
	return FALSE;
}

static BOOL draw_scroll(HDC dc, RECT *rect, UINT state)
{
	RECT area = *rect;
	UINT kind = state & 0x1f;
	if (kind == DFCS_SCROLLSIZEGRIP || kind == DFCS_SCROLLSIZEGRIPRIGHT) {
		FillRect(dc, rect, GetSysColorBrush(COLOR_3DFACE));
		int size = (rect->right - rect->left < rect->bottom - rect->top ? rect->right - rect->left : rect->bottom - rect->top) - 1;
		for (int line = 3; line < size; line += 4) {
			for (int step = 0; step < line; step++) {
				int x = kind == DFCS_SCROLLSIZEGRIP ? rect->right - 1 - line + step : rect->left + line - step;
				int y = rect->bottom - 1 - step;
				SetPixel(dc, x, y, GetSysColor(COLOR_3DSHADOW));
				SetPixel(dc, x, y - 1, GetSysColor(COLOR_3DHIGHLIGHT));
			}
		}
		return TRUE;
	}
	button_face(dc, &area, state);
	pushed_shift(&area, state);
	COLORREF color = glyph_color(state);
	switch (kind) {
	case DFCS_SCROLLUP:
	case DFCS_SCROLLDOWN:
	case DFCS_SCROLLLEFT:
	case DFCS_SCROLLRIGHT:
		triangle(dc, &area, kind, color);
		return TRUE;
	case DFCS_SCROLLCOMBOBOX:
		triangle(dc, &area, DFCS_SCROLLDOWN, color);
		return TRUE;
	}
	return FALSE;
}

BOOL WINAPI DrawFrameControl(HDC dc, LPRECT rect, UINT type, UINT state)
{
	switch (type) {
	case DFC_BUTTON:
		return draw_button(dc, rect, state);
	case DFC_CAPTION:
		return draw_caption(dc, rect, state);
	case DFC_MENU:
		return draw_menu(dc, rect, state);
	case DFC_SCROLL:
		return draw_scroll(dc, rect, state);
	}
	return FALSE;
}

/* Whatever DrawState is drawing, at a position, in the device context given. */
static BOOL state_content(HDC dc, DRAWSTATEPROC callback, LPARAM data, WPARAM extra, int x, int y, int width, int height, UINT kind, BOOL unicode, UINT flags)
{
	switch (kind) {
	case DST_COMPLEX: {
		/* The callback draws from its own origin, at the position given. */
		POINT origin;
		if (!callback || !OffsetViewportOrgEx(dc, x, y, &origin))
			return FALSE;
		BOOL done = callback(dc, data, extra, width, height);
		SetViewportOrgEx(dc, origin.x, origin.y, NULL);
		return done;
	}
	case DST_TEXT:
	case DST_PREFIXTEXT: {
		RECT area = {x, y, x + width, y + height};
		UINT format = DT_LEFT | DT_TOP | DT_NOCLIP | (kind == DST_TEXT ? DT_NOPREFIX : 0) | (flags & DSS_HIDEPREFIX ? DT_HIDEPREFIX : 0) | (flags & DSS_PREFIXONLY ? DT_PREFIXONLY : 0);
		return unicode ? DrawTextW(dc, (LPCWSTR)data, (int)extra ? (int)extra : -1, &area, format) : DrawTextA(dc, (LPCSTR)data, (int)extra ? (int)extra : -1, &area, format);
	}
	case DST_ICON:
		return DrawIconEx(dc, x, y, (HICON)data, width, height, 0, NULL, DI_NORMAL);
	case DST_BITMAP: {
		HDC memory = CreateCompatibleDC(dc);
		if (!memory)
			return FALSE;
		HGDIOBJ previous = SelectObject(memory, (HBITMAP)data);
		BOOL done = BitBlt(dc, x, y, width, height, memory, 0, 0, SRCCOPY);
		SelectObject(memory, previous);
		DeleteDC(memory);
		return done;
	}
	}
	return FALSE;
}

/* Paints with a brush through a mask whose set bits show the brush. */
static void paint_through(HDC dc, HDC mask, int x, int y, int width, int height, HBRUSH brush)
{
	HGDIOBJ previous = SelectObject(dc, brush);
	COLORREF text = SetTextColor(dc, RGB(0, 0, 0)), back = SetBkColor(dc, RGB(255, 255, 255));
	BitBlt(dc, x, y, width, height, mask, 0, 0, 0x00e20746);
	SetTextColor(dc, text);
	SetBkColor(dc, back);
	SelectObject(dc, previous);
}

static BOOL draw_state(HDC dc, HBRUSH brush, DRAWSTATEPROC callback, LPARAM data, WPARAM extra, int x, int y, int width, int height, UINT flags, BOOL unicode)
{
	UINT kind = flags & 0xf, effect = flags & 0xfff0 & ~(DSS_RIGHT | DSS_HIDEPREFIX | DSS_PREFIXONLY);
	if (kind == 0 && callback)
		kind = DST_COMPLEX;
	if (!width || !height) {
		SIZE size = {0, 0};
		if (kind == DST_TEXT || kind == DST_PREFIXTEXT) {
			RECT area = {0, 0, 0, 0};
			if (unicode)
				DrawTextW(dc, (LPCWSTR)data, (int)extra ? (int)extra : -1, &area, DT_CALCRECT | (kind == DST_TEXT ? DT_NOPREFIX : 0));
			else
				DrawTextA(dc, (LPCSTR)data, (int)extra ? (int)extra : -1, &area, DT_CALCRECT | (kind == DST_TEXT ? DT_NOPREFIX : 0));
			size.cx = area.right;
			size.cy = area.bottom;
		} else if (kind == DST_ICON) {
			ICONINFO info;
			if (GetIconInfo((HICON)data, &info)) {
				BITMAP bitmap;
				if (GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap)) {
					size.cx = bitmap.bmWidth;
					size.cy = info.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
				}
				if (info.hbmColor)
					DeleteObject(info.hbmColor);
				if (info.hbmMask)
					DeleteObject(info.hbmMask);
			}
		} else if (kind == DST_BITMAP) {
			BITMAP bitmap;
			if (GetObjectW((HBITMAP)data, sizeof(bitmap), &bitmap)) {
				size.cx = bitmap.bmWidth;
				size.cy = bitmap.bmHeight;
			}
		}
		if (!width)
			width = size.cx;
		if (!height)
			height = size.cy;
	}
	if (!effect)
		return state_content(dc, callback, data, extra, x, y, width, height, kind, unicode, flags);

	/* The effects work from a one-bit image of the content: drawn in black on white, then inverted so set bits are the shape. */
	HDC memory = CreateCompatibleDC(dc);
	HBITMAP color = memory ? CreateCompatibleBitmap(dc, width, height) : NULL;
	HDC mask = memory ? CreateCompatibleDC(dc) : NULL;
	HBITMAP mask_bitmap = mask ? CreateBitmap(width, height, 1, 1, NULL) : NULL;
	BOOL done = FALSE;
	if (color && mask_bitmap) {
		HGDIOBJ old_color = SelectObject(memory, color), old_mask = SelectObject(mask, mask_bitmap);
		HGDIOBJ old_font = SelectObject(memory, GetCurrentObject(dc, OBJ_FONT));
		RECT all = {0, 0, width, height};
		FillRect(memory, &all, GetStockObject(WHITE_BRUSH));
		SetTextColor(memory, RGB(0, 0, 0));
		SetBkMode(memory, TRANSPARENT);
		done = state_content(memory, callback, data, extra, 0, 0, width, height, kind, unicode, flags);
		SetBkColor(memory, RGB(255, 255, 255));
		BitBlt(mask, 0, 0, width, height, memory, 0, 0, NOTSRCCOPY);
		if (effect & DSS_DISABLED) {
			paint_through(dc, mask, x + 1, y + 1, width, height, GetSysColorBrush(COLOR_3DHIGHLIGHT));
			paint_through(dc, mask, x, y, width, height, GetSysColorBrush(COLOR_3DSHADOW));
		} else {
			if (effect & DSS_UNION) {
				static const WORD half_tone[8] = {0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55};
				HBITMAP pattern = CreateBitmap(8, 8, 1, 1, half_tone);
				HBRUSH dither = pattern ? CreatePatternBrush(pattern) : NULL;
				if (dither) {
					HGDIOBJ previous = SelectObject(mask, dither);
					PatBlt(mask, 0, 0, width, height, 0x00a000c9);
					SelectObject(mask, previous);
					DeleteObject(dither);
				}
				if (pattern)
					DeleteObject(pattern);
			}
			paint_through(dc, mask, x, y, width, height, brush && (effect & DSS_MONO) ? brush : GetSysColorBrush(COLOR_WINDOWTEXT));
		}
		SelectObject(memory, old_font);
		SelectObject(memory, old_color);
		SelectObject(mask, old_mask);
	}
	if (mask_bitmap)
		DeleteObject(mask_bitmap);
	if (mask)
		DeleteDC(mask);
	if (color)
		DeleteObject(color);
	if (memory)
		DeleteDC(memory);
	return done;
}

BOOL WINAPI DrawStateW(HDC dc, HBRUSH brush, DRAWSTATEPROC callback, LPARAM data, WPARAM extra, int x, int y, int width, int height, UINT flags)
{
	return draw_state(dc, brush, callback, data, extra, x, y, width, height, flags, TRUE);
}

BOOL WINAPI DrawStateA(HDC dc, HBRUSH brush, DRAWSTATEPROC callback, LPARAM data, WPARAM extra, int x, int y, int width, int height, UINT flags)
{
	return draw_state(dc, brush, callback, data, extra, x, y, width, height, flags, FALSE);
}
