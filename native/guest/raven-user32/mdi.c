/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include "user32_private.h"

/* The multiple document interface: a client window that keeps the documents of a frame. */

typedef struct mdi_client {
	HWND active;
	HMENU window_menu;
	UINT first_child;
	UINT count;
	UINT created;
	BOOL maximized;
} mdi_client;

static mdi_client *client_of(HWND window)
{
	return (mdi_client *)GetWindowLongPtrW(window, 0);
}

static void fill_client(HWND client, HWND child)
{
	RECT area;
	GetClientRect(client, &area);
	MoveWindow(child, 0, 0, area.right, area.bottom, TRUE);
}

static void activate_child(HWND client, mdi_client *data, HWND child)
{
	HWND previous = data->active;
	if (previous == child)
		return;
	data->active = child;
	if (previous)
		SendMessageW(previous, WM_MDIACTIVATE, (WPARAM)previous, (LPARAM)child);
	if (!child)
		return;
	SetWindowPos(child, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (data->maximized)
		fill_client(client, child);
	SendMessageW(child, WM_MDIACTIVATE, (WPARAM)previous, (LPARAM)child);
	SetFocus(child);
}

/* The sibling that comes up when a document leaves or the frame walks the list. */
static HWND next_child(HWND client, HWND from, BOOL backwards)
{
	HWND walk = from ? GetWindow(from, backwards ? GW_HWNDPREV : GW_HWNDNEXT) : NULL;
	if (!walk)
		walk = GetWindow(GetWindow(client, GW_CHILD), backwards ? GW_HWNDLAST : GW_HWNDFIRST);
	return walk == from ? NULL : walk;
}

static HWND create_child(HWND client, mdi_client *data, MDICREATESTRUCTW *request, DWORD extended)
{
	RECT area;
	int step = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME);
	int x = request->x, y = request->y, width = request->cx, height = request->cy;
	HWND child;
	GetClientRect(client, &area);
	if (x == CW_USEDEFAULT) {
		x = y = (data->created % 8) * step;
	}
	if (width == CW_USEDEFAULT) {
		width = area.right * 3 / 4;
		height = area.bottom * 3 / 4;
	}
	child = CreateWindowExW(extended | WS_EX_MDICHILD, request->szClass, request->szTitle,
		request->style | WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE, x, y, width, height, client,
		(HMENU)(UINT_PTR)(data->first_child + data->count), request->hOwner, request);
	if (!child)
		return NULL;
	data->count++;
	data->created++;
	if (request->style & WS_MAXIMIZE)
		data->maximized = TRUE;
	activate_child(client, data, child);
	return child;
}

static void cascade_children(HWND client)
{
	RECT area;
	int step = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME), at = 0;
	HWND child;
	GetClientRect(client, &area);
	for (child = GetWindow(client, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT), at += step)
		MoveWindow(child, at, at, area.right * 3 / 4, area.bottom * 3 / 4, TRUE);
}

static void tile_children(HWND client, mdi_client *data)
{
	RECT area;
	int columns = 1, rows, at = 0;
	HWND child;
	if (!data->count)
		return;
	while ((UINT)(columns * columns) < data->count)
		columns++;
	rows = (data->count + columns - 1) / columns;
	GetClientRect(client, &area);
	for (child = GetWindow(client, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT), at++)
		MoveWindow(child, (at % columns) * area.right / columns, (at / columns) * area.bottom / rows, area.right / columns,
			area.bottom / rows, TRUE);
}

LRESULT WINAPI RavenMdiClientProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	mdi_client *data = client_of(window);
	switch (message) {
	case WM_CREATE: {
		const CLIENTCREATESTRUCT *given = ((CREATESTRUCTW *)lparam)->lpCreateParams;
		data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *data);
		if (!data)
			return -1;
		if (given) {
			data->window_menu = given->hWindowMenu;
			data->first_child = given->idFirstChild;
		}
		SetWindowLongPtrW(window, 0, (LONG_PTR)data);
		return 0;
	}
	case WM_NCDESTROY:
		if (data)
			HeapFree(GetProcessHeap(), 0, data);
		SetWindowLongPtrW(window, 0, 0);
		break;
	case WM_SIZE:
		if (data && data->maximized && data->active)
			fill_client(window, data->active);
		break;
	case WM_SETFOCUS:
		if (data && data->active)
			SetFocus(data->active);
		return 0;
	case WM_MDICREATE:
		return data ? (LRESULT)create_child(window, data, (MDICREATESTRUCTW *)lparam, 0) : 0;
	case WM_MDIDESTROY: {
		HWND child = (HWND)wparam;
		if (!data || !IsWindow(child))
			return 0;
		if (child == data->active)
			activate_child(window, data, next_child(window, child, FALSE));
		DestroyWindow(child);
		if (data->count)
			data->count--;
		return 0;
	}
	case WM_MDIACTIVATE:
		if (data)
			activate_child(window, data, (HWND)wparam);
		return 0;
	case WM_MDINEXT:
		if (data) {
			HWND from = wparam ? (HWND)wparam : data->active;
			activate_child(window, data, next_child(window, from, lparam != 0));
		}
		return 0;
	case WM_MDIMAXIMIZE:
		if (data && IsWindow((HWND)wparam)) {
			data->maximized = TRUE;
			fill_client(window, (HWND)wparam);
		}
		return 0;
	case WM_MDIRESTORE:
		if (data)
			data->maximized = FALSE;
		return 0;
	case WM_MDIGETACTIVE:
		if (lparam && data)
			*(BOOL *)lparam = data->maximized;
		return data ? (LRESULT)data->active : 0;
	case WM_MDICASCADE:
		cascade_children(window);
		return 1;
	case WM_MDITILE:
		if (data)
			tile_children(window, data);
		return 1;
	case WM_MDIICONARRANGE:
		return 0;
	case WM_MDISETMENU: {
		HMENU frame = wparam ? (HMENU)wparam : NULL;
		HMENU previous = GetMenu(GetParent(window));
		if (frame)
			SetMenu(GetParent(window), frame);
		if (lparam && data)
			data->window_menu = (HMENU)lparam;
		return (LRESULT)previous;
	}
	case WM_MDIREFRESHMENU:
		return (LRESULT)GetMenu(GetParent(window));
	default:
		break;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT WINAPI DefFrameProcW(HWND frame, HWND client, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (client) {
		switch (message) {
		case WM_SIZE: {
			RECT area;
			GetClientRect(frame, &area);
			MoveWindow(client, 0, 0, area.right, area.bottom, TRUE);
			return 0;
		}
		case WM_SETFOCUS:
			SetFocus(client);
			return 0;
		case WM_COMMAND: {
			mdi_client *data = client_of(client);
			UINT id = LOWORD(wparam);
			HWND child;
			if (data && id >= data->first_child && id < data->first_child + data->count)
				for (child = GetWindow(client, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
					if ((UINT)GetDlgCtrlID(child) == id) {
						activate_child(client, data, child);
						return 0;
					}
			break;
		}
		default:
			break;
		}
	}
	return DefWindowProcW(frame, message, wparam, lparam);
}

LRESULT WINAPI DefFrameProcA(HWND frame, HWND client, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefFrameProcW(frame, client, message, wparam, lparam);
}

LRESULT WINAPI DefMDIChildProcW(HWND child, UINT message, WPARAM wparam, LPARAM lparam)
{
	HWND client = GetParent(child);
	switch (message) {
	case WM_CHILDACTIVATE:
	case WM_SETFOCUS:
	case WM_MOUSEACTIVATE:
		if (client)
			SendMessageW(client, WM_MDIACTIVATE, (WPARAM)child, 0);
		break;
	case WM_CLOSE:
		if (client) {
			SendMessageW(client, WM_MDIDESTROY, (WPARAM)child, 0);
			return 0;
		}
		break;
	default:
		break;
	}
	return DefWindowProcW(child, message, wparam, lparam);
}

LRESULT WINAPI DefMDIChildProcA(HWND child, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefMDIChildProcW(child, message, wparam, lparam);
}

HWND WINAPI CreateMDIWindowW(LPCWSTR class_name, LPCWSTR title, DWORD style, int x, int y, int width, int height, HWND client,
	HINSTANCE instance, LPARAM parameter)
{
	MDICREATESTRUCTW request = {class_name, title, instance, x, y, width, height, style, parameter};
	return (HWND)SendMessageW(client, WM_MDICREATE, 0, (LPARAM)&request);
}

/* Control+F4 and Control+F6 belong to the frame's active document. */
BOOL WINAPI TranslateMDISysAccel(HWND client, MSG *message)
{
	mdi_client *data = client ? client_of(client) : NULL;
	if (!data || !data->active || !message || (message->message != WM_KEYDOWN && message->message != WM_SYSKEYDOWN) ||
		GetKeyState(VK_CONTROL) >= 0)
		return FALSE;
	if (message->wParam == VK_F4) {
		PostMessageW(data->active, WM_CLOSE, 0, 0);
		return TRUE;
	}
	if (message->wParam == VK_F6) {
		SendMessageW(client, WM_MDINEXT, (WPARAM)data->active, GetKeyState(VK_SHIFT) < 0);
		return TRUE;
	}
	return FALSE;
}
