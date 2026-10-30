/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include "user32_private.h"

HINSTANCE User32Instance;

LRESULT WINAPI RavenScrollBarProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT WINAPI RavenButtonProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT WINAPI RavenStaticProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT WINAPI RavenListBoxProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT WINAPI RavenComboProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT WINAPI RavenEditProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

/* Registers the built-in window classes whose window procedures live in this component. */

BOOL WINAPI RavenInitControls(VOID);

BOOL WINAPI RavenInitControls(VOID)
{
	WNDCLASSEXW definition;
	unsigned index;
	ZeroMemory(&definition, sizeof definition);
	definition.cbSize = sizeof definition;
	definition.style = CS_DBLCLKS | CS_SAVEBITS | CS_GLOBALCLASS;
	definition.lpfnWndProc = DefDlgProcW;
	definition.cbWndExtra = DLGWINDOWEXTRA;
	definition.hInstance = User32Instance;
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = L"#32770";
	if (!RegisterClassExW(&definition))
		return FALSE;
	static const struct {
		const WCHAR *name;
		WNDPROC procedure;
		UINT style;
	} own_classes[] = {
		{L"Button", RavenButtonProcW, CS_DBLCLKS | CS_VREDRAW | CS_HREDRAW | CS_PARENTDC},
		{L"Static", RavenStaticProcW, CS_DBLCLKS | CS_PARENTDC},
		{L"ListBox", RavenListBoxProcW, CS_DBLCLKS},
		{L"ComboLBox", RavenListBoxProcW, CS_DBLCLKS | CS_SAVEBITS},
		{L"ComboBox", RavenComboProcW, CS_DBLCLKS | CS_VREDRAW | CS_HREDRAW | CS_PARENTDC},
		{L"Edit", RavenEditProcW, CS_DBLCLKS | CS_PARENTDC},
	};
	for (index = 0; index < sizeof own_classes / sizeof own_classes[0]; ++index) {
		ZeroMemory(&definition, sizeof definition);
		definition.cbSize = sizeof definition;
		definition.style = own_classes[index].style | CS_GLOBALCLASS;
		definition.lpfnWndProc = own_classes[index].procedure;
		definition.cbWndExtra = sizeof(void *);
		definition.hInstance = User32Instance;
		definition.hCursor = LoadCursorW(NULL, own_classes[index].procedure == RavenEditProcW ? (LPCWSTR)IDC_IBEAM : (LPCWSTR)IDC_ARROW);
		if (!(definition.lpszClassName = own_classes[index].name) || !RegisterClassExW(&definition))
			return FALSE;
	}
	/* The scroll bar control, which this component implements itself. */
	ZeroMemory(&definition, sizeof definition);
	definition.cbSize = sizeof definition;
	definition.style = CS_DBLCLKS | CS_VREDRAW | CS_HREDRAW | CS_PARENTDC | CS_GLOBALCLASS;
	definition.lpfnWndProc = RavenScrollBarProcW;
	definition.cbWndExtra = sizeof(void *);
	definition.hInstance = User32Instance;
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = L"ScrollBar";
	return RegisterClassExW(&definition) != 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		User32Instance = instance;
		DisableThreadLibraryCalls(instance);
		/* Registering here, ahead of every other library, lets later libraries subclass the built-in classes. */
		if (!RavenInitControls())
			return FALSE;
	}
	return TRUE;
}
