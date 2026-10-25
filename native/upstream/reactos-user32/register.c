/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <user32.h>

LRESULT WINAPI RavenScrollBarProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

/* Registers the built-in window classes whose window procedures live in this component. */

BOOL WINAPI RavenInitControls(VOID)
{
	static const struct builtin_class_descr *const classes[] = {
		&DIALOG_builtin_class, &COMBO_builtin_class, &COMBOLBOX_builtin_class,
		&BUTTON_builtin_class, &LISTBOX_builtin_class, &EDIT_builtin_class, &STATIC_builtin_class,
	};
	WNDCLASSEXW definition;
	unsigned index;
	for (index = 0; index < sizeof classes / sizeof classes[0]; ++index) {
		ZeroMemory(&definition, sizeof definition);
		definition.cbSize = sizeof definition;
		definition.style = classes[index]->style | CS_GLOBALCLASS;
		definition.lpfnWndProc = classes[index]->procW;
		definition.cbWndExtra = classes[index]->extra;
		definition.hInstance = User32Instance;
		definition.hCursor = LoadCursorW(NULL, classes[index]->cursor);
		definition.hbrBackground = classes[index]->brush;
		/* The dialog class is named by its predefined atom upstream; its textual name is #32770. */
		definition.lpszClassName = (ULONG_PTR)classes[index]->name == 0x8002 ? L"#32770" : classes[index]->name;
		if (!RegisterClassExW(&definition))
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
