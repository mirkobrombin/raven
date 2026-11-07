/* SPDX-License-Identifier: MIT */
#include "vf.h"
#include <commdlg.h>
#include <wchar.h>

/* The MCI window keeps its creation rules and its messages but plays nothing: there are no MCI devices behind it to play with,
 * and a command that needs one says so. The file dialogs of the preview family show no preview. */

static const WCHAR class_name[] = L"MCIWndClass";
static DWORD creating_wide;

typedef struct
{
	LRESULT error;
	BOOL opened;
} window_data;

static LRESULT open_named(HWND window, const WCHAR *name)
{
	window_data *data = (window_data *)GetWindowLongPtrW(window, 0);
	DWORD attributes = name ? GetFileAttributesW(name) : INVALID_FILE_ATTRIBUTES;
	data->error = attributes == INVALID_FILE_ATTRIBUTES ? MCIERR_FILE_NOT_FOUND : 0;
	data->opened = !data->error;
	return data->error;
}

static LRESULT error_text(window_data *data, WPARAM size, void *text, BOOL wide)
{
	if (size && text)
	{
		if (wide)
			*(WCHAR *)text = 0;
		else
			*(char *)text = 0;
	}
	return data->error;
}

static LRESULT CALLBACK mci_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	window_data *data = (window_data *)GetWindowLongPtrW(window, 0);
	switch (message)
	{
	case WM_CREATE:
	{
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		if (!(data = vf_alloc(sizeof(*data))))
			return -1;
		SetWindowLongPtrW(window, 0, (LONG_PTR)data);
		if (create->lpCreateParams)
			SendMessageW(window, creating_wide || (create->hwndParent && IsWindowUnicode(create->hwndParent)) ? MCIWNDM_OPENW : MCIWNDM_OPENA, 0, (LPARAM)create->lpCreateParams);
		return 0;
	}
	case WM_NCDESTROY:
		vf_free(data);
		SetWindowLongPtrW(window, 0, 0);
		break;
	case MCIWNDM_OPENW:
		return open_named(window, (const WCHAR *)lparam);
	case MCIWNDM_OPENA:
	{
		WCHAR name[MAX_PATH];
		if (!MultiByteToWideChar(CP_ACP, 0, (const char *)lparam, -1, name, ARRAYSIZE(name)))
			return MCIERR_FILE_NOT_FOUND;
		return open_named(window, name);
	}
	case MCIWNDM_GETERRORW:
		return error_text(data, wparam, (void *)lparam, TRUE);
	case MCIWNDM_GETERRORA:
		return error_text(data, wparam, (void *)lparam, FALSE);
	case MCI_PLAY:
	case MCI_RECORD:
		return data->error = MCIERR_DEVICE_NOT_INSTALLED;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

BOOL VFWAPIV MCIWndRegisterClass(void)
{
	WNDCLASSW info;
	memset(&info, 0, sizeof(info));
	info.style = CS_DBLCLKS | CS_GLOBALCLASS;
	info.lpfnWndProc = mci_window;
	info.cbWndExtra = sizeof(void *);
	info.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	info.lpszClassName = class_name;
	return RegisterClassW(&info) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND VFWAPIV MCIWndCreateW(HWND parent, HINSTANCE instance, DWORD style, LPCWSTR name)
{
	HWND window;
	DWORD window_style = style;
	if ((style & (WS_POPUP | WS_CHILD)) == (WS_POPUP | WS_CHILD))
	{
		SetLastError(ERROR_INVALID_MENU_HANDLE);
		return NULL;
	}
	if (!(style & (WS_POPUP | WS_CHILD)))
		window_style |= parent ? WS_VISIBLE | WS_CHILD | WS_BORDER : WS_VISIBLE | WS_OVERLAPPEDWINDOW;
	MCIWndRegisterClass();
	creating_wide++;
	window = CreateWindowExW(0, class_name, NULL, window_style | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 300, 0, parent,
		parent && (window_style & WS_CHILD) ? (HMENU)66 : NULL, instance, (void *)name);
	creating_wide--;
	return window;
}

HWND VFWAPIV MCIWndCreateA(HWND parent, HINSTANCE instance, DWORD style, LPCSTR name)
{
	WCHAR wide[MAX_PATH];
	if (name)
		MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide));
	return MCIWndCreateW(parent, instance, style, name ? wide : NULL);
}

BOOL WINAPI GetOpenFileNamePreviewA(OPENFILENAMEA *info)
{
	return GetOpenFileNameA(info);
}

BOOL WINAPI GetOpenFileNamePreviewW(OPENFILENAMEW *info)
{
	return GetOpenFileNameW(info);
}

BOOL WINAPI GetSaveFileNamePreviewA(OPENFILENAMEA *info)
{
	return GetSaveFileNameA(info);
}

BOOL WINAPI GetSaveFileNamePreviewW(OPENFILENAMEW *info)
{
	return GetSaveFileNameW(info);
}

void vf_unavailable(void)
{
	RaiseException(0xC0000002, EXCEPTION_NONCONTINUABLE, 0, NULL);
}
