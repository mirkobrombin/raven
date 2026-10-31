/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* Drag lists: the owner hears the drag through one registered message and answers with the cursor to show. */

#define SUBCLASS_ID 1
#define AUTOSCROLL_MS 100
#define COPY_CURSOR 104
#define MOVE_CURSOR 102
#define STOP_CURSOR 103
#define INSERT_ARROW 501
#define ARROW_SIZE 32

extern HMODULE COMCTL32_hModule;

typedef struct {
	BOOL dragging;
} drag_state;

UINT uDragListMessage;
static DWORD drag_time;

static LRESULT notify_owner(HWND list, UINT code, POINT cursor)
{
	DRAGLISTINFO info;
	info.uNotification = code;
	info.hWnd = list;
	info.ptCursor = cursor;
	return SendMessageW(GetParent(list), uDragListMessage, GetDlgCtrlID(list), (LPARAM)&info);
}

static void apply_cursor(LRESULT answer)
{
	WORD id = answer == DL_COPYCURSOR ? COPY_CURSOR : answer == DL_MOVECURSOR ? MOVE_CURSOR : STOP_CURSOR;
	if (answer)
		SetCursor(LoadCursorW(COMCTL32_hModule, MAKEINTRESOURCEW(id)));
	else
		SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_NO));
}

static void end_drag(HWND list, drag_state *state, UINT code, POINT cursor)
{
	state->dragging = FALSE;
	if (GetCapture() == list)
		ReleaseCapture();
	SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
	notify_owner(list, code, cursor);
}

static LRESULT CALLBACK list_procedure(HWND list, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
	drag_state *state = (drag_state *)data;
	POINT cursor;
	GetCursorPos(&cursor);
	switch (message) {
	case WM_LBUTTONDOWN:
		if (notify_owner(list, DL_BEGINDRAG, cursor)) {
			state->dragging = TRUE;
			SetCapture(list);
			SetFocus(list);
			return 0;
		}
		break;
	case WM_MOUSEMOVE:
		if (state->dragging) {
			apply_cursor(notify_owner(list, DL_DRAGGING, cursor));
			return 0;
		}
		break;
	case WM_LBUTTONUP:
		if (state->dragging) {
			end_drag(list, state, DL_DROPPED, cursor);
			return 0;
		}
		break;
	case WM_RBUTTONDOWN:
	case WM_CANCELMODE:
		if (state->dragging) {
			end_drag(list, state, DL_CANCELDRAG, cursor);
			return 0;
		}
		break;
	case WM_KEYDOWN:
		if (state->dragging && wparam == VK_ESCAPE) {
			end_drag(list, state, DL_CANCELDRAG, cursor);
			return 0;
		}
		break;
	case WM_NCDESTROY:
		RemoveWindowSubclass(list, list_procedure, id);
		HeapFree(GetProcessHeap(), 0, state);
		break;
	}
	return DefSubclassProc(list, message, wparam, lparam);
}

BOOL WINAPI MakeDragList(HWND list)
{
	drag_state *state;
	if (!uDragListMessage)
		uDragListMessage = RegisterWindowMessageW(DRAGLISTMSGSTRING);
	state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
	if (!state)
		return FALSE;
	return SetWindowSubclass(list, list_procedure, SUBCLASS_ID, (DWORD_PTR)state);
}

int WINAPI LBItemFromPt(HWND list, POINT point, BOOL autoscroll)
{
	RECT area;
	DWORD now;
	int top, count, index;
	ScreenToClient(list, &point);
	GetClientRect(list, &area);
	top = (int)SendMessageW(list, LB_GETTOPINDEX, 0, 0);
	count = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
	if (!PtInRect(&area, point)) {
		if (!autoscroll || point.x < area.left || point.x >= area.right)
			return -1;
		now = GetTickCount();
		if (now - drag_time < AUTOSCROLL_MS)
			return -1;
		drag_time = now;
		if (point.y < area.top && top > 0)
			SendMessageW(list, LB_SETTOPINDEX, top - 1, 0);
		else if (point.y >= area.bottom && top + 1 < count)
			SendMessageW(list, LB_SETTOPINDEX, top + 1, 0);
		return -1;
	}
	for (index = top; index < count; ++index) {
		RECT item;
		if (SendMessageW(list, LB_GETITEMRECT, index, (LPARAM)&item) == LB_ERR)
			break;
		if (PtInRect(&item, point))
			return index;
		if (item.top >= area.bottom)
			break;
	}
	return -1;
}

static BOOL arrow_rect(HWND parent, HWND list, int item, RECT *arrow)
{
	RECT place;
	if (SendMessageW(list, LB_GETITEMRECT, item, (LPARAM)&place) == LB_ERR)
		return FALSE;
	MapWindowPoints(list, parent, (POINT *)&place, 2);
	arrow->left = place.left - ARROW_SIZE;
	arrow->right = place.left;
	arrow->top = place.top - ARROW_SIZE / 2;
	arrow->bottom = arrow->top + ARROW_SIZE;
	return TRUE;
}

void WINAPI DrawInsert(HWND parent, HWND list, int item)
{
	static HICON icon;
	static HWND last_list;
	static int last_item = -1;
	RECT arrow;
	if (list == last_list && item == last_item)
		return;
	if (last_item >= 0 && last_list && IsWindow(last_list) && arrow_rect(parent, last_list, last_item, &arrow)) {
		InvalidateRect(parent, &arrow, TRUE);
		UpdateWindow(parent);
	}
	last_list = list;
	last_item = item;
	if (item < 0 || !arrow_rect(parent, list, item, &arrow))
		return;
	if (!icon)
		icon = LoadIconW(COMCTL32_hModule, MAKEINTRESOURCEW(INSERT_ARROW));
	HDC dc = GetDC(parent);
	if (dc) {
		DrawIcon(dc, arrow.left, arrow.top, icon);
		ReleaseDC(parent, dc);
	}
}
